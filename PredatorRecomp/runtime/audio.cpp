// Audio do Xbox (XAudio render driver): o jogo mixa o som e entrega quadros de 256 amostras x 6 canais (5.1) a 48 kHz
// a cada ~5,33 ms em XAudioSubmitRenderDriverFrame. Aqui eles vao para a placa de som do PC pelo XAudio2 (Windows).
// O ritmo e o do proprio dispositivo: o jogo so e chamado para mixar outro quadro quando a fila de som tem espaco.
// FCRECOMP_SEMSOM=1: nao abre o dispositivo (so mantem o relogio do audio andando, como antes).
#include "thread.h"
#include "perf.h"
#include <windows.h>
#include <xaudio2.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <string>
#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "xaudio2.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    std::atomic<uint32_t> g_callback{ 0 }, g_callbackArg{ 0 };
    std::atomic<uint64_t> g_framesSubmitted{ 0 }, g_callbacks{ 0 }, g_submitsInCallback{ 0 };
    thread_local bool t_inCallback = false;

    constexpr uint32_t CHANNELS = 6, SAMPLES = 256;          // quadro do Xbox: 6 canais x 256 amostras
    constexpr uint32_t RING = 16;                            // quadros guardados para o XAudio2
    // ~32 ms de som na fila (latencia x seguranca). Com 4 (~21 ms) a fila secava por instantes no comeco da intro da
    // Ubisoft (o jogo carregando ao mesmo tempo): o Windows tocava ~10 ms de silencio em pedacos = estalos
    constexpr uint32_t TARGET_QUEUE = 6;

    ComPtr<IXAudio2> g_xaudio;
    IXAudio2MasteringVoice* g_master = nullptr;
    IXAudio2SourceVoice* g_voice = nullptr;
    std::vector<float> g_ring[RING];
    uint32_t g_ringNext = 0;
    std::mutex g_submitMutex;

    bool InitOutput()
    {
        if (getenv("FCRECOMP_SEMSOM")) return false;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(XAudio2Create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)))
        {
            Log("[audio] XAudio2 indisponivel: sem som");
            return false;
        }
        if (FAILED(g_xaudio->CreateMasteringVoice(&g_master)))
        {
            Log("[audio] nenhum dispositivo de som: sem som");
            g_xaudio.Reset();
            return false;
        }
        WAVEFORMATEXTENSIBLE wf{};
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = CHANNELS;
        wf.Format.nSamplesPerSec = 48000;
        wf.Format.wBitsPerSample = 32;
        wf.Format.nBlockAlign = CHANNELS * 4;
        wf.Format.nAvgBytesPerSec = 48000 * wf.Format.nBlockAlign;
        wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wf.Samples.wValidBitsPerSample = 32;
        wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                           SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;   // o XAudio2 converte para os alto-falantes do PC
        wf.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        if (FAILED(g_xaudio->CreateSourceVoice(&g_voice, &wf.Format)))
        {
            Log("[audio] falha ao criar a voz de saida: sem som");
            return false;
        }
        for (auto& f : g_ring) f.resize(CHANNELS * SAMPLES);
        {
            XAUDIO2_VOICE_DETAILS md{};
            g_master->GetVoiceDetails(&md);
            float m[6 * 8] = {};
            uint32_t saidas = std::min<uint32_t>(md.InputChannels, 8);
            g_voice->GetOutputMatrix(g_master, CHANNELS, saidas, m);
            std::string s;
            for (uint32_t o = 0; o < saidas; o++)
            {
                s += " [";
                for (uint32_t c = 0; c < CHANNELS; c++) { char b[16]; snprintf(b, sizeof(b), "%s%.3f", c ? " " : "", m[o * CHANNELS + c]); s += b; }
                s += "]";
            }
            Log("[audio] saida do PC com %u canais; mistura dos 6 do jogo (FE FD C LFE TE TD) por saida:%s", md.InputChannels, s.c_str());
        }
        g_voice->Start();
        // nome do dispositivo de som padrao (fone/caixa Bluetooth costuma ter 150-300 ms de atraso)
        std::string nome = "?";
        {
            ComPtr<IMMDeviceEnumerator> en;
            ComPtr<IMMDevice> dev;
            ComPtr<IPropertyStore> props;
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
                SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) &&
                SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
            {
                char b[256];
                WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, b, sizeof(b), nullptr, nullptr);
                nome = b;
            }
            PropVariantClear(&pv);
        }
        Log("[audio] saida de som aberta (XAudio2, 48 kHz, 5.1) em \"%s\"", nome.c_str());
        return true;
    }

    uint32_t QueuedFrames()
    {
        if (!g_voice) return 0;
        XAUDIO2_VOICE_STATE st;
        g_voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        return st.BuffersQueued;
    }

    // Abrir a saida (XAudio2 + dispositivo) leva ~0,4 s. Antes era feito quando o jogo registrava o driver de som, junto
    // com o inicio do video da Ubisoft: o video (Bink, nao espera o som) comecava e o som saia ~375 ms (11 quadros)
    // atrasado ate o fim. Agora AudioPrepare abre a saida no inicio do programa, durante a preparacao da placa.
    std::atomic<uint64_t> g_vazias{ 0 };   // vezes em que a fila do XAudio2 esvaziou antes do jogo mandar mais som
    std::thread g_prepareThread;
    bool g_prepared = false;

    // O Xbox toca quadros de 256 amostras a 48 kHz (~5,33 ms). A cada quadro consumido o driver chama o jogo.
    // Com som: chama o jogo enquanto a fila do XAudio2 tiver menos que TARGET_QUEUE quadros (o dispositivo dita o ritmo).
    void AudioThread()
    {
        GuestCpu* ctx = AttachGuestThread("audio");
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // prioridade de audio do Windows (MMCSS "Pro Audio") e alta: o carregamento do jogo nao atrasa a mistura do som
        static const bool semPrioridade = getenv("FCRECOMP_SOMPRIORIDADENORMAL") != nullptr;
        if (!semPrioridade)
        {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            if (HMODULE avrt = LoadLibraryA("avrt.dll"))
                if (auto f = (HANDLE(WINAPI*)(LPCSTR, LPDWORD))GetProcAddress(avrt, "AvSetMmThreadCharacteristicsA"))
                {
                    DWORD idx = 0;
                    if (!f("Pro Audio", &idx)) Log("[audio] MMCSS indisponivel (erro %lu); so prioridade alta", GetLastError());
                }
        }
        if (g_prepareThread.joinable()) g_prepareThread.join();   // AudioPrepare (normalmente ja terminou)
        bool output = g_prepared ? g_voice != nullptr : InitOutput();
        auto next = std::chrono::steady_clock::now();
        for (;;)
        {
            uint32_t cb = g_callback;
            if (output)
            {
                // espera a fila esvaziar ate o alvo (dorme ~1 ms por vez)
                if (!cb || QueuedFrames() >= TARGET_QUEUE) { Sleep(1); continue; }
            }
            else
            {
                next += std::chrono::microseconds(5333);
                std::this_thread::sleep_until(next);
                if (!cb) continue;
            }
            if (output && QueuedFrames() == 0 && g_callbacks) g_vazias++;   // a fila acabou: o PC tocou silencio (picote)
            SetGpr(*ctx, 3, g_callbackArg);
            g_callbacks++;
            t_inCallback = true;
            GuestCall(*ctx, cb);
            t_inCallback = false;
        }
    }
}

// Para o som (troca de fase: o processo novo assume enquanto este espera a janela nova aparecer)
void AudioSilenciar()
{
    if (g_voice) g_voice->Stop();
}

// Abre a saida de som numa thread propria no inicio do programa (main.cpp, antes da preparacao da placa)
void AudioPrepare()
{
    g_prepareThread = std::thread([] {
        double t0 = GetTickCount64();
        InitOutput();
        g_prepared = true;
        Log("[audio] saida de som preparada em %.0f ms", GetTickCount64() - t0);
    });
}

// quadros de som (256 amostras a 48 kHz) que o jogo ja entregou (FCRECOMP_QUADROLOG compara com os quadros de imagem)
uint64_t AudioFramesSubmitted() { return g_framesSubmitted; }

// XAudioRegisterRenderDriverClient(callbackInfo*, driver*): callbackInfo = { callback, callbackArg }
KFUNC(XAudioRegisterRenderDriverClient)
{
    uint32_t info = Gpr32(ctx, 3);
    g_callbackArg = R32(info + 4);
    g_callback = R32(info);
    Log("[audio] driver registrado: callback=%08X arg=%08X", R32(info), R32(info + 4));
    W32(Gpr32(ctx, 4), 0x41550000);

    static bool started = false;
    if (!started)
    {
        started = true;
        std::thread(AudioThread).detach();
    }
    Ret(ctx, 0);
}

KFUNC(XAudioUnregisterRenderDriverClient)
{
    g_callback = 0;
    Ret(ctx, 0);
}

// XAudioSubmitRenderDriverFrame(driver, samples*): 256 amostras x 6 canais em float big-endian, um canal depois do
// outro (FL FR C LFE BL BR). O XAudio2 quer os canais intercalados e little-endian.
KFUNC(XAudioSubmitRenderDriverFrame)
{
    g_framesSubmitted++;
    if (t_inCallback) g_submitsInCallback++;
    if (g_voice && Gpr32(ctx, 4))
    {
        std::lock_guard lock(g_submitMutex);
        auto& out = g_ring[g_ringNext];
        g_ringNext = (g_ringNext + 1) % RING;
        const uint32_t* in = (const uint32_t*)G(Gpr32(ctx, 4));
        for (uint32_t c = 0; c < CHANNELS; c++)
            for (uint32_t i = 0; i < SAMPLES; i++)
            {
                uint32_t v = __builtin_bswap32(in[c * SAMPLES + i]);
                memcpy(&out[i * CHANNELS + c], &v, 4);
            }
        if (perf::AvLog())   // inicio de cada som depois de silencio (sincronia com a imagem)
        {
            static bool sound = false;
            static int logged = 0;
            float m = 0;
            for (float v : out) m = std::max(m, std::fabs(v));
            if (!sound && m > 0.003f && logged++ < 6)
            {
                XAUDIO2_PERFORMANCE_DATA pd{};
                g_xaudio->GetPerformanceData(&pd);
                Log("[av] %.1f ms: som comecou (quadro de som %llu, fila %u; atraso do XAudio2/Windows %.1f ms)", perf::Now(),
                    (unsigned long long)g_framesSubmitted.load(), QueuedFrames(), pd.CurrentLatencyInSamples / 48.0);
            }
            sound = m > 0.003f;
        }
        // medicao: volume maximo a cada ~2 s (375 quadros) - mostra no log se o jogo esta mandando som de verdade
        static float peak = 0;
        static uint32_t count = 0;
        for (float v : out) peak = std::max(peak, std::fabs(v));
        if (++count == 375)
        {
            static ULONGLONG last = GetTickCount64();
            ULONGLONG now = GetTickCount64();
            static uint64_t lastCb = 0;
            XAUDIO2_PERFORMANCE_DATA pd{};
            g_xaudio->GetPerformanceData(&pd);
            Log("[audio] volume maximo nos ultimos 375 quadros: %.3f (fila %u, %llu ms; o certo e 2000; chamadas do driver %llu, entregas dentro da chamada %llu; atraso da saida %.0f ms; fila vazia %llu vezes, falhas do XAudio2 %u)",
                peak, QueuedFrames(), now - last, (unsigned long long)(g_callbacks - lastCb), (unsigned long long)g_submitsInCallback.load(),
                pd.CurrentLatencyInSamples / 48.0, (unsigned long long)g_vazias.exchange(0), pd.GlitchesSinceEngineStarted);
            lastCb = g_callbacks;
            last = now;
            peak = 0; count = 0;
        }
        // FCRECOMP_SOMWAV=1: grava a saida (frente esquerda/direita) em build/saida_som.wav, 48 kHz 16 bits;
        // FCRECOMP_SOMWAV=6: os 6 canais (FL FR C LFE BL BR)
        static const char* wavOpt = getenv("FCRECOMP_SOMWAV");
        static const uint32_t wavCh = wavOpt && atoi(wavOpt) == 6 ? 6 : 2;
        static FILE* wav = wavOpt ? fopen("saida_som.wav", "wb") : nullptr;
        static uint32_t wavBytes = 0;
        if (wav)
        {
            if (!wavBytes)
            {
                uint32_t hdr[11] = { 0x46464952, 0, 0x45564157, 0x20746D66, 16, 1u | (wavCh << 16), 48000, 48000 * 2 * wavCh,
                                     (2 * wavCh) | (16u << 16), 0x61746164, 0 };
                fwrite(hdr, 4, 11, wav);
            }
            int16_t pcm[SAMPLES * CHANNELS];
            for (uint32_t i = 0; i < SAMPLES * wavCh; i++)
                pcm[i] = (int16_t)std::clamp(out[(i / wavCh) * CHANNELS + (i % wavCh)] * 32767.0f, -32768.0f, 32767.0f);
            fwrite(pcm, 2, SAMPLES * wavCh, wav);
            wavBytes += SAMPLES * wavCh * 2;
            uint32_t riff = 36 + wavBytes;
            fseek(wav, 4, SEEK_SET); fwrite(&riff, 4, 1, wav);
            fseek(wav, 40, SEEK_SET); fwrite(&wavBytes, 4, 1, wav);
            fseek(wav, 0, SEEK_END);
        }
        XAUDIO2_BUFFER buf{};
        buf.AudioBytes = CHANNELS * SAMPLES * 4;
        buf.pAudioData = (const BYTE*)out.data();
        g_voice->SubmitSourceBuffer(&buf);
    }
    Ret(ctx, 0);
}

KFUNC(XAudioGetVoiceCategoryVolumeChangeMask)
{
    if (Gpr32(ctx, 4)) W32(Gpr32(ctx, 4), 0);
    Ret(ctx, 0);
}

KFUNC(XAudioGetVoiceCategoryVolume)
{
    float one = 1.0f;
    if (Gpr32(ctx, 4)) W32(Gpr32(ctx, 4), *(uint32_t*)&one);
    Ret(ctx, 0);
}

// depuracao (FCRECOMP_VOZESLOG=1, bug 9): gerente de vozes do motor de som do jogo. A cada rodada ele percorre a lista de
// sons ativos (+4 da raiz) e, por som: +12 custo, +36 grupo, +40 estado (1 tocando, 2 virtual, 3 parado), +88 volume,
// +32 e +44 marcas. Os que passam do limite do grupo (tabela +28: {maximo, em uso} por grupo, +32 grupos) ou do total
// (+20), ou ficam baixos demais, sao parados/virtualizados. Anota cada som novo e cada troca de estado.
#ifndef PREDATOR_EDITOR   // gancho so do jogo: no editor de mapas (ige.xex) o endereco e de outra funcao
GUEST_DECLARE_ORIGINAL(sub_824C37B8);
GUEST_FUNC(sub_824C37B8)
{
    static const bool vozesLog = getenv("FCRECOMP_VOZESLOG") != nullptr;
    // experimento (FCRECOMP_VOZESNOVAS=1): som que ainda nao comecou (estado 0) com o volume inicial -96 dB (o jogo ainda
    // nao calculou o volume dele) nao e descartado por estar baixo nesta rodada
    static const bool protegeNovas = getenv("FCRECOMP_VOZESNOVAS") != nullptr;
    uint32_t root = Gpr32(ctx, 3);
    // depuracao (FCRECOMP_VOZES3D=1): a cada ~250 ms, o ouvinte (posicao e os floats seguintes) e cada som tocando com o
    // volume (+88) e a posicao da 1a camada: mostra se volume e posicao acompanham o jogador
    if (static const bool v3d = getenv("FCRECOMP_VOZES3D") != nullptr; v3d && root)
    {
        static ULONGLONG ultimo = 0;
        ULONGLONG agora = GetTickCount64();
        if (agora - ultimo >= 250)
        {
            ultimo = agora;
            auto F = [](uint32_t a) { uint32_t v = R32(a); float f; memcpy(&f, &v, 4); return f; };
            std::string s;
            uint32_t ouv = R32(0x828D9C68);
            float lx = 0, ly = 0, lz = 0;
            if (ouv)
            {
                lx = F(ouv); ly = F(ouv + 4); lz = F(ouv + 8);
                char b[64];
                for (uint32_t o = 0; o < 56; o += 4) { snprintf(b, sizeof(b), " %.3f", F(ouv + o)); s += b; }
            }
            Log("[3d] t=%llu som=%llu ouvinte%s", (unsigned long long)agora, (unsigned long long)g_framesSubmitted.load(), s.c_str());
            int n = 0;
            for (uint32_t no = R32(root + 4); no && n < 512; no = R32(no), n++)
            {
                uint32_t som = R32(no + 8);
                if (!som || R32(som + 40) != 1 || !R32(som + 8)) continue;
                uint32_t cam = R32(som + 8);
                float x = F(cam + 12), y = F(cam + 16), z = F(cam + 20);
                float d = std::sqrt((x - lx) * (x - lx) + (y - ly) * (y - ly) + (z - lz) * (z - lz));
                Log("[3d]   som %08X vol %.1f dB pos %.2f %.2f %.2f (dist %.2f) camadas %u", som, (int32_t)R32(som + 88) / 65536.0, x, y, z, d, R32(som));            }
        }
    }
    uint32_t protegidos[64]; int nProt = 0;
    if (protegeNovas && root)
    {
        // protege por ate 100 ms a partir da primeira vez que o som aparece com -96 dB; anota quando o volume chega
        static std::mutex pm;
        static std::vector<std::pair<uint32_t, ULONGLONG>> vistos;
        std::lock_guard plock(pm);
        ULONGLONG agora = GetTickCount64();
        int n = 0;
        for (uint32_t no = R32(root + 4); no && n < 512 && nProt < 64; no = R32(no), n++)
        {
            uint32_t som = R32(no + 8);
            if (!som) continue;
            auto it = std::find_if(vistos.begin(), vistos.end(), [&](auto& p) { return p.first == som; });
            uint32_t est = R32(som + 40);
            if (R32(som + 88) == 0xFFA00000u)
            {
                if (est == 0 && it == vistos.end()) { vistos.push_back({ som, agora }); it = vistos.end() - 1; }
                if (it != vistos.end() && agora - it->second < 100 && est <= 1 && R32(som + 32))
                {
                    W32(som + 32, 0); protegidos[nProt++] = som;
                }
            }
            else if (it != vistos.end())
            {
                if (vozesLog) Log("[vozes] som %08X: volume chegou depois de %llu ms: %.1f dB (estado %u)", som,
                    agora - it->second, (int32_t)R32(som + 88) / 65536.0, est);
                vistos.erase(it);
            }
        }
        std::erase_if(vistos, [&](auto& p) { return agora - p.second > 2000; });
    }
    GUEST_ORIGINAL(sub_824C37B8)(ctx, base);
    for (int i = 0; i < nProt; i++) W32(protegidos[i] + 32, 1);
    if (!vozesLog || !root) return;
    static std::mutex m;
    std::lock_guard lock(m);
    static std::vector<std::pair<uint32_t, uint32_t>> conhecidos;   // som -> ultimo estado
    static uint32_t ultimaTabela = 0;
    static int linhas = 0;
    if (linhas > 4000) return;
    double t = (double)GetTickCount64();
    uint32_t grupos = R32(root + 32), tabela = R32(root + 28);
    uint32_t hashTabela = R32(root + 20);
    for (uint32_t g = 0; g < grupos && g < 64; g++) hashTabela = hashTabela * 31 + R32(tabela + g * 8);
    if (hashTabela != ultimaTabela)
    {
        ultimaTabela = hashTabela;
        std::string s;
        for (uint32_t g = 0; g < grupos && g < 64; g++) { char b[32]; snprintf(b, sizeof(b), " %u:%u/%u", g, R32(tabela + g * 8 + 4), R32(tabela + g * 8)); s += b; }
        Log("[vozes] t=%.0f raiz %08X total max %u, %u grupos (em uso/max):%s", t, root, R32(root + 20), grupos, s.c_str());
        linhas++;
    }
    std::vector<std::pair<uint32_t, uint32_t>> agora;
    int n = 0;
    for (uint32_t no = R32(root + 4); no && n < 512; no = R32(no), n++)
    {
        uint32_t som = R32(no + 8);
        if (!som) continue;
        uint32_t estado = R32(som + 40);
        agora.push_back({ som, estado });
        {
            // volume de cada som muda depois de criado? (so os que ja tocam, para nao lotar o log)
            static std::vector<std::pair<uint32_t, uint32_t>> volumes;
            uint32_t vol = R32(som + 88);
            auto v = std::find_if(volumes.begin(), volumes.end(), [&](auto& p) { return p.first == som; });
            if (v == volumes.end()) volumes.push_back({ som, vol });
            else if (v->second != vol)
            {
                if (estado == 1) { Log("[vozes] t=%.0f som %08X volume %.1f -> %.1f dB", t, som, (int32_t)v->second / 65536.0, (int32_t)vol / 65536.0); linhas++; }
                v->second = vol;
            }
            if (volumes.size() > 256) volumes.erase(volumes.begin(), volumes.begin() + 128);
        }
        auto it = std::find_if(conhecidos.begin(), conhecidos.end(), [&](auto& p) { return p.first == som; });
        if (it == conhecidos.end() || it->second != estado)
        {
            Log("[vozes] t=%.0f som %08X %s estado %u->%u grupo %u custo %u vol %d (%.1f dB?) +4=%08X +8=%08X +16=%08X +24=%08X +32=%08X +44=%08X", t, som,
                it == conhecidos.end() ? "NOVO" : "    ", it == conhecidos.end() ? 0 : it->second, estado, R32(som + 36), R32(som + 12),
                (int32_t)R32(som + 88), (int32_t)R32(som + 88) / 65536.0, R32(som + 4), R32(som + 8), R32(som + 16), R32(som + 24),
                R32(som + 32), R32(som + 44));
            linhas++;
            if (it == conhecidos.end())
            {
                // camadas do som (+0 quantas, +8 lista de 1832 bytes cada): marcas +36, posicao +12, ouvintes +40 e pares
                // {ouvinte, ligado} em +148 (sub_824DA1F0: ouvinte fora da tabela ou desligado = -96 dB)
                auto F = [](uint32_t a) { uint32_t v = R32(a); float f; memcpy(&f, &v, 4); return f; };
                uint32_t nCam = R32(som), cam = R32(som + 8);
                std::string s;
                for (uint32_t k = 0; k < nCam && k < 3 && cam; k++)
                {
                    uint32_t e = cam + k * 1832, nOuv = R32(e + 40);
                    char b[160];
                    snprintf(b, sizeof(b), " [camada %u marcas %08X pos %.1f %.1f %.1f ouvintes %u:", k, R32(e + 36), F(e + 12), F(e + 16), F(e + 20), nOuv);
                    s += b;
                    for (uint32_t j = 0; j < nOuv && j < 4; j++) { snprintf(b, sizeof(b), " %08X=%08X", R32(e + 148 + j * 8), R32(e + 152 + j * 8)); s += b; }
                    s += "]";
                }
                std::string o;
                int k = 0;
                for (uint32_t ouv = R32(0x828D9C68); ouv && k < 4; ouv = R32(ouv + 56), k++)
                {
                    char b[96]; snprintf(b, sizeof(b), " %08X(+48=%u pos %.1f %.1f %.1f)", ouv, R32(ouv + 48), F(ouv), F(ouv + 4), F(ouv + 8)); o += b;
                }
                Log("[vozes]    camadas %u:%s | ouvintes:%s", nCam, s.c_str(), o.c_str());
            }
        }
    }
    for (auto& p : conhecidos)
        if (std::none_of(agora.begin(), agora.end(), [&](auto& q) { return q.first == p.first; }))
        {
            Log("[vozes] t=%.0f som %08X SAIU da lista (estado %u)", t, p.first, p.second);
            linhas++;
        }
    conhecidos = std::move(agora);
}
#endif
