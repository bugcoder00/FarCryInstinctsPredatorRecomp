// Audio do Xbox (XAudio render driver): o jogo mixa o som e entrega quadros de 256 amostras x 6 canais (5.1) a 48 kHz
// a cada ~5,33 ms em XAudioSubmitRenderDriverFrame. Aqui eles vao para a placa de som do PC pelo XAudio2 (Windows).
// O ritmo e o do proprio dispositivo: o jogo so e chamado para mixar outro quadro quando a fila de som tem espaco.
// FCRECOMP_SEMSOM=1: nao abre o dispositivo (so mantem o relogio do audio andando, como antes).
#include "thread.h"
#include "perf.h"
#include "opcoes.h"
std::string AoVivoValor(const char* chave);   // input.cpp: ao_vivo.txt da pasta do jogo
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
#include <set>
#include <algorithm>
#include <cmath>
#include <thread>
void WatchAddWriteRange(uint32_t addr, uint32_t size);   // watch.cpp (diagnostico FCRECOMP_VOZESVIGIA)

#pragma comment(lib, "xaudio2.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    std::atomic<uint32_t> g_callback{ 0 }, g_callbackArg{ 0 };
    std::atomic<uint64_t> g_framesSubmitted{ 0 }, g_callbacks{ 0 }, g_submitsInCallback{ 0 };
    thread_local bool t_inCallback = false;
    // a thread de som esta dentro do callback do jogo (XAudioUnregisterRenderDriverClient espera ele terminar)
    std::atomic<bool> g_emCallback{ false };

    constexpr uint32_t CHANNELS = 6, SAMPLES = 256;          // quadro do Xbox: 6 canais x 256 amostras
    // saida em estereo (padrao, como o console na TV) ou 5.1 de verdade (FCRECOMP_SOM51=1)
    bool SomEstereo() { static const bool e = getenv("FCRECOMP_SOM51") == nullptr; return e; }
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
    unsigned g_somVersaoUsada = 0;   // SomVersao() quando a saida foi aberta (aba AUDIO / janela de audio do F2)
    int g_volumeUsado = 100;         // volume do jogo aplicado (opcoes volumeJogo)
    bool CriarSaida();

    bool InitOutput()
    {
        if (getenv("FCRECOMP_SEMSOM")) return false;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(XAudio2Create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)))
        {
            Log("[audio] XAudio2 indisponivel: sem som");
            return false;
        }
        if (!CriarSaida())
        {
            g_xaudio.Reset();
            return false;
        }
        return true;
    }

    // a saida escolhida nas opcoes (aba AUDIO; vazio = o padrao do Windows). Se ela sumiu (fone desligado), o padrao.
    bool CriarMaster()
    {
        g_somVersaoUsada = SomVersao();
        const std::wstring id = SomEscolhido(SOM_SAIDA);
        if (!id.empty() && SUCCEEDED(g_xaudio->CreateMasteringVoice(&g_master, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, id.c_str())))
            return true;
        if (!id.empty()) Log("[audio] a saida de som escolhida nao abriu (desligada?): usando a padrao do Windows");
        if (FAILED(g_xaudio->CreateMasteringVoice(&g_master)))
        {
            Log("[audio] nenhum dispositivo de som: sem som");
            return false;
        }
        return true;
    }

    // master + voz de saida (5.1 do jogo) no dispositivo escolhido; tambem ao trocar o dispositivo com o jogo rodando
    bool CriarSaida()
    {
        if (!CriarMaster()) return false;
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
            g_master->DestroyVoice(); g_master = nullptr;
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
        XAUDIO2_VOICE_DETAILS md{};
        g_master->GetVoiceDetails(&md);
        if (SomEstereo() && md.InputChannels >= 2)
        {
            // estereo (como o console ligado na TV): o jogo sempre mistura em 5.1 e o Xbox junta os 6 canais em 2. Sem isto,
            // o Windows manda os canais traseiros para as caixas de tras ou, num fone com 7.1 virtual, para "atras da cabeca"
            // (os passos do personagem, que ficam um pouco atras da camera, soavam em 3D)
            const uint32_t saidas = std::min<uint32_t>(md.InputChannels, 8);
            float m[6 * 8] = {};
            const float k = 0.7071f;
            m[0 * CHANNELS + 0] = 1; m[0 * CHANNELS + 2] = k; m[0 * CHANNELS + 4] = k;   // esquerda = FE + C + TE
            m[1 * CHANNELS + 1] = 1; m[1 * CHANNELS + 2] = k; m[1 * CHANNELS + 5] = k;   // direita  = FD + C + TD
            if (SUCCEEDED(g_voice->SetOutputMatrix(g_master, CHANNELS, saidas, m)))
                Log("[audio] som em estereo: os 6 canais do jogo juntados na esquerda/direita da frente");
        }
        g_voice->SetVolume(std::clamp(OpcoesAtuais().volumeJogo, 0, 100) / 100.0f);
        g_volumeUsado = OpcoesAtuais().volumeJogo;
        g_voice->Start();
        // nome do dispositivo de som (fone/caixa Bluetooth costuma ter 150-300 ms de atraso)
        std::string nome = "?";
        {
            ComPtr<IMMDeviceEnumerator> en;
            ComPtr<IMMDevice> dev;
            ComPtr<IPropertyStore> props;
            PROPVARIANT pv; PropVariantInit(&pv);
            const std::wstring id = SomEscolhido(SOM_SAIDA);
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
                ((!id.empty() && SUCCEEDED(en->GetDevice(id.c_str(), &dev))) || SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) &&
                SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) &&
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
            // depuracao (ao_vivo.txt): MICTESTE=1/0 abre/fecha o microfone como o TESTAR da barra de nivel da janela de
            // audio (num fone Bluetooth o "atraso da saida" do registro mostra o modo de chamada: ~36 ms em vez de ~290)
            if (static ULONGLONG olhada = 0; GetTickCount64() - olhada >= 500)
            {
                olhada = GetTickCount64();
                static std::string mic;
                const std::string m = AoVivoValor("MICTESTE");
                if (m != mic)
                {
                    mic = m;
                    Log("[audio] MICTESTE=%s", m.c_str());
                    std::thread([m] { if (m == "1") MicMedirIniciar(SomEscolhido(SOM_VOZ_MIC)); else MicMedirParar(); }).detach();
                }
            }
            if (output && SomVersao() != g_somVersaoUsada)
            {
                // trocou a saida na janela de audio: refaz o master e a voz no dispositivo novo (o quadro em curso se perde)
                std::lock_guard lock(g_submitMutex);
                g_voice->DestroyVoice(); g_voice = nullptr;
                g_master->DestroyVoice(); g_master = nullptr;
                output = CriarSaida();
                if (!output) { g_voice = nullptr; Log("[audio] a troca de saida falhou: sem som ate reabrir o jogo"); }
            }
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
            // marca "dentro do callback" e confere de novo: se o jogo desregistrou no meio, nao chama (o motor de som
            // dele pode ja estar sendo desmontado; editor de mapas voltando ao jogo lia um ponteiro zerado)
            g_emCallback = true;
            if (g_callback != cb) { g_emCallback = false; continue; }
            SetGpr(*ctx, 3, g_callbackArg);
            g_callbacks++;
            t_inCallback = true;
            GuestCall(*ctx, cb);
            t_inCallback = false;
            g_emCallback = false;
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

// Como no Xbox: depois de desregistrar, o callback nao roda mais; se estiver rodando agora, espera terminar (o jogo
// desmonta o motor de som logo em seguida). Chamado de dentro do proprio callback, nao espera (travaria).
KFUNC(XAudioUnregisterRenderDriverClient)
{
    g_callback = 0;
    if (!t_inCallback)
    {
        const ULONGLONG t0 = GetTickCount64();
        while (g_emCallback && GetTickCount64() - t0 < 2000) Sleep(1);
        if (g_emCallback) Log("[audio] callback do jogo nao terminou em 2 s ao desregistrar o driver");
    }
    Log("[audio] driver desregistrado");
    Ret(ctx, 0);
}

// pular_cena.cpp: saida em silencio enquanto uma cena e adiantada
std::atomic<bool> g_somCalado{ false };
// pular cena (pular_cena.cpp): sons que a cena pulada solta (vento, tiros, explosao do barco) nao podem continuar no
// jogo (o som fica calado ate o fim da parada). SonsDoPuloInicio guarda os sons que ja existiam; ate SonsDoPuloFim + 500 ms, todo som novo da lista do gerente
// de vozes (sub_824C37B8) recebe o estado 3 (parado; o gerente o tira da lista).
std::atomic<int> g_sonsPulo{ 0 };            // 0 nada, 1 pedido (guardar a lista), 2 parando os novos
std::atomic<bool> g_sonsCenaPedido{ false };  // BeginCutScene: guardar a lista na proxima rodada (o vento da intro do
                                              // treino comeca com a cena, antes de o jogador abrir o menu)
std::mutex g_sonsCenaMutex;
std::vector<uint32_t> g_sonsCena;             // sons que existiam quando a cena comecou
bool g_sonsCenaOk = false;
// Falas presas na animacao da cena (roteiro 2026-11-26_09-30, 02_native): as falas do Carver sao chaves de som da
// animacao de um personagem (evento postado pela atualizacao das entidades, retorno 82617A78 em sub_824B8020; grupo
// 0x03A2, itens 0-6). O pulo acaba a acao, mas a animacao pode continuar no ritmo normal (as vezes sim, as vezes nao) e
// soltava as falas que faltavam no meio do jogo (+7, +18, +20 e +25 s depois do pulo). Agora os grupos de som (ev[0] >> 16)
// postados por esse caminho durante a cena, e que nao apareceram fora de cena, ficam calados depois do pulo ate a proxima
// cena (no maximo 3 min). FCRECOMP_PULARCENA_SONSANIM=0 desliga.
#ifndef PREDATOR_EDITOR
bool CenaDoScriptTocando();                   // pular_cena.cpp
uint32_t AtorDaCenaNaPilha(uint32_t sp);     // pular_cena.cpp
extern std::atomic<bool> g_acelerando;       // pular_cena.cpp
namespace
{
    std::mutex g_animMutex;
    std::set<uint32_t> g_gruposCena, g_gruposFora;   // grupos do caminho das animacoes: na cena / fora de cena
    double g_animBloqueioIni = 0, g_animBloqueioFim = 0;   // perf::Now(); fim 0 = enquanto durar o pulo
    constexpr uint32_t RET_SOM_ANIMACAO = 0x82617A78u;
}
void AnimCenaComeca() { std::lock_guard l(g_animMutex); g_gruposCena.clear(); g_animBloqueioIni = 0; }
void AnimPuloComeca() { std::lock_guard l(g_animMutex); g_animBloqueioIni = perf::Now(); g_animBloqueioFim = 0; }
#else
void AnimCenaComeca() {}
void AnimPuloComeca() {}
#endif
void SonsCenaComeca() { g_sonsCenaPedido = true; AnimCenaComeca(); }
std::atomic<double> g_sonsPuloFim{ 0 };      // perf::Now() ate quando continua parando (0 = enquanto durar o pulo)
void SonsDoPuloInicio()
{
    g_sonsPuloFim = 0; g_sonsPulo = 1;
    AnimPuloComeca();
}
#ifndef PREDATOR_EDITOR
// true = o evento de som (r4 objeto, *r6 evento) postado do caminho das animacoes deve ficar calado
bool SomAnimacaoCalar(uint32_t lr, uint32_t ev, uint32_t sp, uint32_t obj)
{
    static const bool ligado = [] { const char* e = getenv("FCRECOMP_PULARCENA_SONSANIM"); return !e || atoi(e) != 0; }();
    static const bool porAtor = [] { const char* e = getenv("FCRECOMP_PULARCENA_SONSATOR"); return !e || atoi(e) != 0; }();
    if (!ligado || lr != RET_SOM_ANIMACAO || !ev) return false;
    const uint32_t grupo = R32(ev) >> 16;
    std::lock_guard l(g_animMutex);
    const double agora = perf::Now();
    if (g_animBloqueioIni > 0 && g_animBloqueioFim == 0 && !g_acelerando.load() && g_sonsPulo.load() == 0)
        g_animBloqueioFim = agora + 180000;   // pulo acabou: conta os 3 min
    const bool bloqueando = g_animBloqueioIni > 0 && (g_animBloqueioFim == 0 || agora < g_animBloqueioFim);
    if (bloqueando)
    {
        static int n = 0;
        if (g_gruposCena.count(grupo))
        {
            if (n++ < 50) Log("[pularcena] fala da animacao da cena calada depois do pulo (grupo %04X, item %04X)", grupo, R32(ev) & 0xFFFF);
            return true;
        }
        // animacao da cena que so comecou depois do pulo (ainda carregava quando o pulo veio): o personagem e um ator
        // da cena (pular_cena.cpp) e o grupo nunca apareceu fora de cena (passos e sons comuns aparecem logo)
        if (porAtor && !g_gruposFora.count(grupo))
            if (const uint32_t ator = AtorDaCenaNaPilha(sp))
            {
                if (n++ < 50) Log("[pularcena] som da animacao de um ator da cena calado depois do pulo (ator %08X, objeto %08X, grupo %04X, item %04X)",
                                  ator, obj, grupo, R32(ev) & 0xFFFF);
                return true;
            }
        if (!CenaDoScriptTocando() && g_gruposFora.size() < 256) g_gruposFora.insert(grupo);
        return false;
    }
    if (CenaDoScriptTocando()) { if (!g_gruposFora.count(grupo) && g_gruposCena.size() < 64) g_gruposCena.insert(grupo); }
    else if (g_gruposFora.size() < 256) g_gruposFora.insert(grupo);
    return false;
}
#endif
void SonsDoPuloFim()   // o som volta (g_somCalado) quando a parada acaba
{
    if (g_sonsPulo) g_sonsPuloFim = perf::Now() + 500; else g_somCalado = false;
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
        if (g_somCalado.load(std::memory_order_relaxed)) std::fill(std::begin(out), std::end(out), 0.0f);   // pular_cena.cpp
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
        if (const int vol = OpcoesAtuais().volumeJogo; vol != g_volumeUsado)   // janela de audio: vale na hora
        {
            g_volumeUsado = vol;
            g_voice->SetVolume(std::clamp(vol, 0, 100) / 100.0f);
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
// depuracao (FCRECOMP_SOMSWITCH=1, cachoeira repetindo): evento "mudar interruptor" do motor de som (sub_824B6070,
// chamado pelo despachante sub_824B6E38 com r3/r4 = grupo/id, *r5 = valor, r6 = ligado, r7). Anota cada mudanca.
// depuracao (FCRECOMP_SOMEVENTO=objeto): evento de som postado para esse objeto (sub_824B8020: r4 objeto, r5 id,
// r6 -> evento): quem postou (cadeia de retornos do PowerPC) e os campos do evento
GUEST_DECLARE_ORIGINAL(sub_824B8020);
GUEST_FUNC(sub_824B8020)
{
    // FCRECOMP_SOMEVENTO=todos: todos os eventos (para achar quem pede um som)
    if (static const uint32_t alvo = getenv("FCRECOMP_SOMEVENTO") ? (!strcmp(getenv("FCRECOMP_SOMEVENTO"), "todos") ? 0xFFFFFFFFu
                                                                      : strtoul(getenv("FCRECOMP_SOMEVENTO"), nullptr, 16)) : 0;
        alvo && (alvo == 0xFFFFFFFFu || Gpr32(ctx, 4) == alvo))
    {
        std::string chain;
        uint32_t sp = Gpr32(ctx, 1);
        for (int i = 0; i < 14 && sp >= 0x10000000 && sp < 0x80000000; i++)
        {
            uint32_t next = R32(sp);
            if (next <= sp) break;
            char b[16]; snprintf(b, sizeof(b), " %08X", R32(next - 8)); chain += b;
            sp = next;
        }
        uint32_t ev = Gpr32(ctx, 6) ? R32(Gpr32(ctx, 6)) : 0;
        std::string campos;
        for (uint32_t o = 0; ev && o < 96; o += 4) { char b[16]; snprintf(b, sizeof(b), " %08X", R32(ev + o)); campos += b; }
        // texto (nome do som, se houver) em ev+8 ou no que ele aponta
        auto Txt = [](uint32_t a) {
            std::string t;
            if (a < 0x40000000 || a >= 0xC0000000) return t;
            for (int i = 0; i < 64; i++) { uint8_t c = *(uint8_t*)G(a + i); if (!c) break; if (c < 32 || c > 126) return std::string(); t += (char)c; }
            return t;
        };
        if (ev) { const uint32_t p = R32(ev + 8); std::string t = Txt(p); if (t.empty() && p >= 0x40000000 && p < 0xC0000000) t = Txt(R32(p)); if (!t.empty()) campos += " '" + t + "'"; }
        Log("[somevento] t=%.0f obj %08X id %08X lr %08X ev %08X:%s | pilha:%s", perf::Now(), Gpr32(ctx, 4), Gpr32(ctx, 5),
            (uint32_t)Lr(ctx), ev, campos.c_str(), chain.c_str());
    }
    // estalo do gatilho dos fuzis (TriggerSoundEvent 4521985 = 0x450001, lua.dat BasicWeapon: tocado no 1o tiro de cada
    // aperto): com toques rapidos ele soava como "arma sem balas" por cima do tiro (pedido do usuario, 17/11). O som de
    // arma vazia de verdade (EmptyChamberSoundEvent 0x450008) e o das pistolas (0x450000) continuam.
    // FCRECOMP_CLIQUEGATILHO=1 devolve o estalo.
    // So quando sai tiro: o script de tiro (lua.dat linha 276) chama BasicWeapon.PlayTriggerSound, entao ha duas camadas
    // da maquina Lua (retorno 823E5AAC) antes da chamada vinda do C (823DF15C). Com a arma vazia o estalo e o "clique sem
    // balas" do fuzil (fuzis nao tem EmptyChamberSoundEvent) e continua tocando.
    if (SomAnimacaoCalar((uint32_t)Lr(ctx), Gpr32(ctx, 6) ? R32(Gpr32(ctx, 6)) : 0, Gpr32(ctx, 1), Gpr32(ctx, 4)))
    {
        Ret(ctx, (uint64_t)(int64_t)-1);   // como o caminho "nao tocou" da funcao original
        return;
    }
    if (static const bool manter = getenv("FCRECOMP_CLIQUEGATILHO") != nullptr; !manter)
        if (uint32_t pev = Gpr32(ctx, 6); pev)
            if (uint32_t ev = R32(pev); ev && R32(ev) == 0x00450001u)
            {
                int camadasLua = 0;
                std::string chain;
                uint32_t sp = Gpr32(ctx, 1);
                for (int i = 0; i < 24 && sp >= 0x10000000 && sp < 0x80000000; i++)
                {
                    uint32_t next = R32(sp);
                    if (next <= sp) break;
                    const uint32_t ret = R32(next - 8);
                    char b[16]; snprintf(b, sizeof(b), " %08X", ret); chain += b;
                    if (ret == 0x823DF15Cu) break;
                    if (ret == 0x823E5AACu) camadasLua++;
                    sp = next;
                }
                const bool tiro = camadasLua >= 2;
                Log("[som] estalo do gatilho (%u camadas Lua): %s |%s", camadasLua, tiro ? "tiro, silenciado" : "sem tiro, toca",
                    chain.c_str());
                if (tiro)
                {
                    Ret(ctx, (uint64_t)(int64_t)-1);   // como o caminho "nao tocou" da funcao original
                    return;
                }
            }
    GUEST_ORIGINAL(sub_824B8020)(ctx, base);
}

// depuracao (FCRECOMP_SOMESTADO=1): estado de som do personagem (sub_82279108 devolve 0-6, -1; sub_82278220 para o som
// do estado antigo e liga o do novo). Anota cada mudanca com os campos que a funcao usa.
GUEST_DECLARE_ORIGINAL(sub_82279108);
GUEST_FUNC(sub_82279108)
{
    uint32_t obj = Gpr32(ctx, 3);
    GUEST_ORIGINAL(sub_82279108)(ctx, base);
    if (static const bool est = getenv("FCRECOMP_SOMESTADO") != nullptr; est)
    {
        static std::mutex m;
        static std::vector<std::pair<uint32_t, int32_t>> ult;
        std::lock_guard lock(m);
        int32_t v = (int32_t)Gpr32(ctx, 3);
        auto it = std::find_if(ult.begin(), ult.end(), [&](auto& p) { return p.first == obj; });
        if (it == ult.end() || it->second != v)
        {
            auto F = [](uint32_t a) { uint32_t x = R32(a); float f; memcpy(&f, &x, 4); return f; };
            Log("[somestado] t=%.0f obj %08X estado %d -> %d (+1352 %u +1064 %u +2436 %u +2950 %u +354 %u +2396 %.2f +2400 %.2f +2404 %.2f)",
                perf::Now(), obj, it == ult.end() ? -99 : it->second, v, R32(obj + 1352), R32(obj + 1064), R32(obj + 2436),
                *(uint8_t*)G(obj + 2950), *(uint8_t*)G(obj + 354), F(obj + 2396), F(obj + 2400), F(obj + 2404));
            if (it == ult.end()) ult.push_back({ obj, v }); else it->second = v;
        }
    }
}

GUEST_DECLARE_ORIGINAL(sub_824B6070);
GUEST_FUNC(sub_824B6070)
{
    if (static const bool sw = getenv("FCRECOMP_SOMSWITCH") != nullptr; sw)
    {
        static std::mutex m;
        static std::vector<std::pair<uint64_t, uint64_t>> ult;   // (r3,r4) -> (valor,r6)
        std::lock_guard lock(m);
        uint64_t k = ((uint64_t)Gpr32(ctx, 3) << 32) | Gpr32(ctx, 4);
        uint32_t r5 = Gpr32(ctx, 5);
        uint64_t v = ((uint64_t)(r5 ? R32(r5) : 0) << 32) | Gpr32(ctx, 6);
        auto it = std::find_if(ult.begin(), ult.end(), [&](auto& p) { return p.first == k; });
        static const bool todos = getenv("FCRECOMP_SOMSWITCH")[0] == '2';
        if (todos || it == ult.end() || it->second != v)
        {
            static int n = 0;
            // texto (se houver) no valor e no que ele aponta
            auto Txt = [](uint32_t a) {
                std::string t;
                if (a < 0x40000000 || a >= 0xC0000000) return t;
                for (int i = 0; i < 48; i++) { uint8_t c = *(uint8_t*)G(a + i); if (!c) break; if (c < 32 || c > 126) return std::string(); t += (char)c; }
                return t;
            };
            uint32_t val = r5 ? R32(r5) : 0;
            if (n++ < 3000)
                Log("[somswitch] t=%.0f grupo %08X id %08X valor %08X (+4 %08X) ligado %u r7 %08X%s txt '%s' '%s' '%s'", perf::Now(), Gpr32(ctx, 3), Gpr32(ctx, 4),
                    val, r5 ? R32(r5 + 4) : 0, Gpr32(ctx, 6), Gpr32(ctx, 7), it == ult.end() ? " (novo)" : "", Txt(val).c_str(),
                    val >= 0x40000000 && val < 0xC0000000 ? Txt(R32(val)).c_str() : "", Txt(Gpr32(ctx, 3)).c_str());
            if (it == ult.end()) ult.push_back({ k, v }); else it->second = v;
        }
    }
    GUEST_ORIGINAL(sub_824B6070)(ctx, base);
}

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
    if (root && g_sonsCenaPedido.exchange(false))
    {
        std::lock_guard l(g_sonsCenaMutex);
        g_sonsCena.clear();
        int n = 0;
        for (uint32_t no = R32(root + 4); no && n < 512; no = R32(no), n++) if (uint32_t som = R32(no + 8)) g_sonsCena.push_back(som);
        g_sonsCenaOk = true;
    }
    // O gerente reaproveita os enderecos dos sons: um som de antes da cena que acaba deixa o endereco livre, e a fala
    // do Carver (ou outro som da cena) que nasce nele passava por "de antes" e continuava tocando depois do PULAR CENA
    // (as vezes sim, as vezes nao). Quem sai da lista deixa de ser "de antes". FCRECOMP_SONSCENA_ENDERECO=1 volta ao antigo.
    static const bool soEndereco = getenv("FCRECOMP_SONSCENA_ENDERECO") != nullptr;
    auto podar = [&](std::vector<uint32_t>& v)
    {
        if (soEndereco || v.empty()) return;
        uint32_t agora[512]; int k = 0;
        for (uint32_t no = R32(root + 4); no && k < 512; no = R32(no)) if (uint32_t som = R32(no + 8)) agora[k++] = som;
        v.erase(std::remove_if(v.begin(), v.end(), [&](uint32_t s) { return std::find(agora, agora + k, s) == agora + k; }), v.end());
    };
    if (root && g_sonsCenaOk && !g_sonsPulo.load())
    {
        std::lock_guard l(g_sonsCenaMutex);
        podar(g_sonsCena);
    }
    if (const int pulo = g_sonsPulo.load(); pulo && root)
    {
        static std::vector<uint32_t> antes;
        static int parados = 0;
        int n = 0;
        if (pulo == 1)
        {
            antes.clear(); parados = 0;
            std::lock_guard l(g_sonsCenaMutex);
            if (g_sonsCenaOk) antes = g_sonsCena;   // os de antes da cena: o que a cena soltou tambem para
            else for (uint32_t no = R32(root + 4); no && n < 512; no = R32(no), n++) if (uint32_t som = R32(no + 8)) antes.push_back(som);
            g_sonsPulo = 2;
        }
        else
        {
            podar(antes);
            for (uint32_t no = R32(root + 4); no && n < 512; no = R32(no), n++)
            {
                const uint32_t som = R32(no + 8);
                if (!som || R32(som + 40) == 3 || std::find(antes.begin(), antes.end(), som) != antes.end()) continue;
                // parar como o proprio gerente faz quando tira a voz de um som (824C38A0): funcoes da tabela do motor
                // (root+24): +28 (def +16, som+28) desliga a voz de verdade; +4 (dono +4, def, acabou) avisa o dono.
                // So escrever o estado 3 deixava a voz tocando: a musica do menu de pausa (em laco) nunca parava.
                if (const uint32_t tab = R32(root + 24))
                {
                    GuestCpu salvo = ctx;
                    const uint32_t est = R32(som + 40);
                    SetGpr(ctx, 3, R32(som + 16)); SetGpr(ctx, 4, som + 28);
                    GuestCall(ctx, R32(tab + 28));
                    if (est != 2)
                    {
                        SetGpr(ctx, 3, R32(som + 4)); SetGpr(ctx, 4, R32(som + 16)); SetGpr(ctx, 5, 1);
                        GuestCall(ctx, R32(tab + 4));
                    }
                    ctx = salvo;
                }
                W32(som + 40, 3);
                parados++;
            }
            if (const double fim = g_sonsPuloFim.load(); fim > 0 && perf::Now() > fim)
            {
                Log("[pularcena] %d sons da cena parados", parados);
                g_sonsPulo = 0;
                g_somCalado = false;
            }
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
                // FCRECOMP_VOZESVIGIA=1: sons de ambiente (1a camada com marcas 3, na posicao do ouvinte) tocando: vigia
                // quem escreve o estado (+40) e o no da lista, para achar quem os para (cachoeira repetindo)
                if (static const bool vigia = getenv("FCRECOMP_VOZESVIGIA") != nullptr; vigia && estado == 1 && cam && R32(cam + 36) == 3)
                {
                    Log("[vozes]    vigiando o som %08X (+8=%08X)", som, R32(som + 8));
                    ::WatchAddWriteRange(som + 40, 4);
                }
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
