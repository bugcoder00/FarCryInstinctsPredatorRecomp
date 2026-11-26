// Chat de voz (headset do Xbox) com o microfone e os alto-falantes do PC.
//
// O jogo traz a biblioteca de voz do Xbox (XHV) compilada dentro dele: ela codifica a voz e o proprio jogo manda os
// pacotes de voz pela rede (rede.cpp). Do sistema ela so usa o "headset" pelo XAM:
//   XamVoiceCreate(usuario, 15, &voz)      abre o headset do jogador (voz = 0: sem headset)
//   XamVoiceHeadsetPresent(voz)            1 = headset ligado
//   XamVoiceSubmitPacket(voz, sentido, p)  entrega um pacote: sentido 1 = ler do microfone, 0 = tocar no fone
//   XamVoiceClose(voz)
// Pacote (no jogo): +0 estado (0x103 = pendente; 0 = pronto), +4 bytes transferidos, +8 buffer, +12 tamanho em bytes,
// +20 formato. O jogo fica olhando o +0 ate sair de 0x103 (nao ha evento). A voz dentro do XHV e PCM 16 bits a 16 kHz.
// - microfone (sentido 1): formato 2 (o jogo pede este) = 1 byte por amostra a 8 kHz, codificado pela tabela de 256
//   valores do proprio jogo (0x828A1B80, que o XHV usa para voltar a PCM duplicando cada amostra); formato 1 = G.721
//   (ADPCM de 4 bits a 16 kHz, nibble baixo primeiro). Aqui so o 2 (o 1 volta silencio, com aviso no registro).
// - fone (sentido 0): sempre G.721 a 8 kHz (o XHV codifica no lugar antes de mandar), 2 amostras por byte. A taxa foi
//   medida: tocado a 16 kHz, um bipe de 1000 Hz chegava com 2000 Hz e na metade do tempo.
//   O decodificador abaixo e o G.721 de referencia (o mesmo que o jogo usa: fmult/update iguais aos do codigo dele).
// Os pacotes andam no ritmo real: o do microfone fica pronto quando ha amostras suficientes; o do fone quando o som
// anterior ja tocou (fila curta no XAudio2).
//
// Fone, microfone e volume: aba AUDIO do launcher / janela de audio do F2 (audio_janela.cpp); trocar o dispositivo
// reabre os dois aqui (SomVersao), o volume vale no pacote seguinte.
// FCRECOMP_VOZ=0: sem headset (como antes). FCRECOMP_VOZLOG=1: registro dos pacotes ([voz]).
// FCRECOMP_VOZ_TESTE=hz: no lugar do microfone, um bipe de "hz" (0,4 s sim, 0,4 s nao): para testar duas copias.
// FCRECOMP_VOZ_WAV=arquivo.wav: grava a voz recebida (o que toca no fone) nesse arquivo, ao fechar o headset.
#include "guest.h"
#include "opcoes.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <xaudio2.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <deque>
#include <vector>
#include <string>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <chrono>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr uint32_t PENDENTE = 0x103, FALHOU = 0xC0000001;
    constexpr uint32_t TABELA_8BITS = 0x828A1B80;   // 256 x u16 (big-endian) do jogo
    constexpr uint32_t TAXA = 16000;        // microfone (a voz dentro do XHV)
    constexpr uint32_t TAXA_FONE = 8000;    // fone: G.721 a 8 kHz

    bool VozLigada() { static const bool on = [] { const char* v = getenv("FCRECOMP_VOZ"); return !(v && v[0] == '0'); }(); return on; }
    bool VozLog() { static const bool on = getenv("FCRECOMP_VOZLOG") != nullptr; return on; }
    bool GravaWav() { static const bool on = getenv("FCRECOMP_VOZ_WAV") != nullptr; return on; }
    double TomTeste() { static const double hz = [] { const char* v = getenv("FCRECOMP_VOZ_TESTE"); return v ? atof(v) : 0.0; }(); return hz; }

    // ---------------------------------------------------------------- G.721 (referencia de dominio publico)
    struct G72x
    {
        int32_t yl = 34816; int16_t yu = 544, dms = 0, dml = 0, ap = 0;
        int16_t a[2] = { 0, 0 }, b[6] = { 0 }, pk[2] = { 0, 0 }, dq[6] = { 32, 32, 32, 32, 32, 32 }, sr[2] = { 32, 32 };
        char td = 0;
    };
    const int16_t kPower2[15] = { 1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000 };
    int Quan(int val) { int i = 0; for (; i < 15; i++) if (val < kPower2[i]) break; return i; }
    int Fmult(int an, int srn)
    {
        int16_t anmag = (int16_t)((an > 0) ? an : ((-an) & 0x1FFF));
        int16_t anexp = (int16_t)(Quan(anmag) - 6);
        int16_t anmant = (int16_t)((anmag == 0) ? 32 : (anexp >= 0) ? anmag >> anexp : anmag << -anexp);
        int16_t wanexp = (int16_t)(anexp + ((srn >> 6) & 0xF) - 13);
        int16_t wanmant = (int16_t)((anmant * (srn & 077) + 0x30) >> 4);
        int16_t ret = (int16_t)((wanexp >= 0) ? ((wanmant << wanexp) & 0x7FFF) : (wanmant >> -wanexp));
        return ((an ^ srn) < 0) ? -ret : ret;
    }
    int PredictorZero(const G72x& s) { int r = 0; for (int i = 0; i < 6; i++) r += Fmult(s.b[i] >> 2, s.dq[i]); return r; }
    int PredictorPole(const G72x& s) { return Fmult(s.a[1] >> 2, s.sr[1]) + Fmult(s.a[0] >> 2, s.sr[0]); }
    int StepSize(const G72x& s)
    {
        if (s.ap >= 256) return s.yu;
        int y = s.yl >> 6, dif = s.yu - y, al = s.ap >> 2;
        if (dif > 0) y += (dif * al) >> 6;
        else if (dif < 0) y += (dif * al + 0x3F) >> 6;
        return y;
    }
    int Reconstruct(int sign, int dqln, int y)
    {
        int16_t dql = (int16_t)(dqln + (y >> 2));
        if (dql < 0) return sign ? -0x8000 : 0;
        int16_t dex = (int16_t)((dql >> 7) & 15), dqt = (int16_t)(128 + (dql & 127));
        int16_t dq = (int16_t)((dqt << 7) >> (14 - dex));
        return sign ? (dq - 0x8000) : dq;
    }
    void Update(int y, int wi, int fi, int dq, int sr, int dqsez, G72x& s)
    {
        int16_t pk0 = (dqsez < 0) ? 1 : 0, mag = (int16_t)(dq & 0x7FFF);
        int16_t ylint = (int16_t)(s.yl >> 15), ylfrac = (int16_t)((s.yl >> 10) & 0x1F);
        int16_t thr1 = (int16_t)((32 + ylfrac) << ylint), thr2 = (int16_t)((ylint > 9) ? 31 << 10 : thr1);
        int16_t dqthr = (int16_t)((thr2 + (thr2 >> 1)) >> 1);
        char tr = (s.td == 0 || mag <= dqthr) ? 0 : 1;

        s.yu = (int16_t)(y + ((wi - y) >> 5));
        if (s.yu < 544) s.yu = 544; else if (s.yu > 5120) s.yu = 5120;
        s.yl += s.yu + ((-s.yl) >> 6);

        int16_t a2p = 0;
        if (tr == 1)
        {
            s.a[0] = s.a[1] = 0;
            for (auto& v : s.b) v = 0;
        }
        else
        {
            int16_t pks1 = pk0 ^ s.pk[0];
            a2p = (int16_t)(s.a[1] - (s.a[1] >> 7));
            if (dqsez != 0)
            {
                int16_t fa1 = pks1 ? s.a[0] : (int16_t)-s.a[0];
                if (fa1 < -8191) a2p -= 0x100;
                else if (fa1 > 8191) a2p += 0xFF;
                else a2p += fa1 >> 5;
                if (pk0 ^ s.pk[1])
                {
                    if (a2p <= -12160) a2p = -12288; else if (a2p >= 12416) a2p = 12288; else a2p -= 0x80;
                }
                else if (a2p <= -12416) a2p = -12288;
                else if (a2p >= 12160) a2p = 12288;
                else a2p += 0x80;
            }
            s.a[1] = a2p;
            s.a[0] -= s.a[0] >> 8;
            if (dqsez != 0) s.a[0] += pks1 == 0 ? 192 : -192;
            int16_t a1ul = (int16_t)(15360 - a2p);
            if (s.a[0] < -a1ul) s.a[0] = (int16_t)-a1ul; else if (s.a[0] > a1ul) s.a[0] = a1ul;
            for (int i = 0; i < 6; i++)
            {
                s.b[i] -= s.b[i] >> 8;
                if (dq & 0x7FFF) s.b[i] += ((int16_t)(dq ^ s.dq[i]) >= 0) ? 128 : -128;
            }
        }
        for (int i = 5; i > 0; i--) s.dq[i] = s.dq[i - 1];
        if (mag == 0) s.dq[0] = (int16_t)((int16_t)dq >= 0 ? 0x20 : 0xFC20);
        else
        {
            int e = Quan(mag);
            s.dq[0] = (int16_t)((int16_t)dq >= 0 ? (e << 6) + ((mag << 6) >> e) : (e << 6) + ((mag << 6) >> e) - 0x400);
        }
        s.sr[1] = s.sr[0];
        if (sr == 0) s.sr[0] = 0x20;
        else if (sr > 0) { int e = Quan(sr); s.sr[0] = (int16_t)((e << 6) + ((sr << 6) >> e)); }
        else if (sr > -32768) { int m = -sr, e = Quan(m); s.sr[0] = (int16_t)((e << 6) + ((m << 6) >> e) - 0x400); }
        else s.sr[0] = (int16_t)0xFC20;
        s.pk[1] = s.pk[0];
        s.pk[0] = pk0;
        s.td = (tr == 1) ? 0 : (a2p < -11776) ? 1 : 0;
        s.dms += (int16_t)((fi - s.dms) >> 5);
        s.dml += (int16_t)((((fi << 2) - s.dml) >> 7));
        if (tr == 1) s.ap = 256;
        else if (y < 1536 || s.td == 1 || std::abs((s.dms << 2) - s.dml) >= (s.dml >> 3)) s.ap += (0x200 - s.ap) >> 4;
        else s.ap += (-s.ap) >> 4;
    }
    const int16_t kDqln[16] = { -2048, 4, 135, 213, 273, 323, 373, 425, 425, 373, 323, 273, 213, 135, 4, -2048 };
    const int16_t kWi[16] = { -12, 18, 41, 64, 112, 198, 355, 1122, 1122, 355, 198, 112, 64, 41, 18, -12 };
    const int16_t kFi[16] = { 0, 0, 0, 0x200, 0x200, 0x200, 0x600, 0xE00, 0xE00, 0x600, 0x200, 0x200, 0x200, 0, 0, 0 };
    // devolve a amostra de 14 bits (o jogo multiplica por 4)
    int G721Decode(int i, G72x& s)
    {
        i &= 0x0F;
        int sezi = PredictorZero(s);
        int16_t sez = (int16_t)(sezi >> 1);
        int sei = sezi + PredictorPole(s);
        int16_t se = (int16_t)(sei >> 1);
        int y = StepSize(s);
        int16_t dq = (int16_t)Reconstruct(i & 0x08, kDqln[i], y);
        int16_t sr = (int16_t)((dq < 0) ? (se - (dq & 0x3FFF)) : se + dq);
        int16_t dqsez = (int16_t)(sr - se + sez);
        Update(y, kWi[i] << 5, kFi[i], dq, sr, dqsez, s);
        return sr;
    }

    // ---------------------------------------------------------------- estado
    struct Pacote { uint32_t voz, end; };
    std::mutex g_mutex;
    std::vector<uint32_t> g_vozes;                 // headsets abertos
    std::deque<Pacote> g_captura, g_toque;
    std::thread g_thread;
    std::atomic<bool> g_rodando{ false };
    uint32_t g_proxVoz = 0x56300001;
    G72x g_g721;
    std::vector<int16_t> g_wav;                     // FCRECOMP_VOZ_WAV
    uint64_t g_nCap = 0, g_nToque = 0, g_capVoz = 0;

    // microfone (16 kHz mono 16 bits, o Windows converte)
    ComPtr<IAudioClient> g_mic;
    ComPtr<IAudioCaptureClient> g_micCap;
    std::deque<int16_t> g_micBuf;
    // fone
    ComPtr<IXAudio2> g_xa;
    IXAudio2MasteringVoice* g_master = nullptr;
    IXAudio2SourceVoice* g_src = nullptr;
    std::deque<std::vector<int16_t>> g_tocando;    // buffers entregues ao XAudio2 (vivos ate tocarem)
    int g_volume = -1;                              // volume do chat de voz aplicado (opcoes volumeVoz)

    bool AbrirMicrofone()
    {
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> dev;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return false;
        // o escolhido na aba AUDIO (vazio = o padrao de comunicacao do Windows); se sumiu, o padrao
        const std::wstring id = SomEscolhido(SOM_VOZ_MIC);
        if (!id.empty() && FAILED(en->GetDevice(id.c_str(), &dev))) Log("[voz] o microfone escolhido nao esta ligado: usando o padrao");
        if (!dev && FAILED(en->GetDefaultAudioEndpoint(eCapture, eCommunications, &dev)) &&
            FAILED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev))) { Log("[voz] nenhum microfone no PC"); return false; }
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)g_mic.GetAddressOf()))) return false;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 1; wf.nSamplesPerSec = TAXA; wf.wBitsPerSample = 16;
        wf.nBlockAlign = 2; wf.nAvgBytesPerSec = TAXA * 2;
        HRESULT hr = g_mic->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 2000000, 0, &wf, nullptr);
        if (FAILED(hr) || FAILED(g_mic->GetService(IID_PPV_ARGS(&g_micCap))) || FAILED(g_mic->Start()))
        {
            Log("[voz] o microfone nao abriu (%08X)", (unsigned)hr);
            g_micCap.Reset(); g_mic.Reset();
            return false;
        }
        Log("[voz] microfone aberto (16 kHz)");
        return true;
    }

    bool AbrirFone()
    {
        if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
        // fone escolhido na aba AUDIO (vazio = o mesmo do som do jogo; os dois vazios = o padrao do Windows)
        std::wstring id = SomEscolhido(SOM_VOZ_FONE);
        if (id.empty()) id = SomEscolhido(SOM_SAIDA);
        if ((id.empty() || FAILED(g_xa->CreateMasteringVoice(&g_master, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, id.c_str()))) &&
            FAILED(g_xa->CreateMasteringVoice(&g_master))) { g_xa.Reset(); return false; }
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 1; wf.nSamplesPerSec = TAXA_FONE; wf.wBitsPerSample = 16;
        wf.nBlockAlign = 2; wf.nAvgBytesPerSec = TAXA_FONE * 2;
        if (FAILED(g_xa->CreateSourceVoice(&g_src, &wf))) { g_master->DestroyVoice(); g_master = nullptr; g_xa.Reset(); return false; }
        g_volume = -1;   // aplicado no primeiro pacote
        g_src->Start();
        return true;
    }

    void LerMicrofone()
    {
        if (TomTeste() > 0)
        {
            // bipe no ritmo do relogio: 0,4 s de tom, 0,4 s de silencio
            static auto t0 = std::chrono::steady_clock::now();
            static uint64_t feitas = 0;
            uint64_t devidas = (uint64_t)(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * TAXA);
            for (; feitas < devidas; feitas++)
            {
                bool liga = (feitas / (TAXA * 4 / 10)) % 2 == 0;
                g_micBuf.push_back(liga ? (int16_t)(6000 * std::sin(2 * 3.14159265358979 * TomTeste() * feitas / TAXA)) : 0);
            }
        }
        else if (g_micCap)
        {
            UINT32 n = 0;
            while (SUCCEEDED(g_micCap->GetNextPacketSize(&n)) && n)
            {
                BYTE* d = nullptr; UINT32 quadros = 0; DWORD flags = 0;
                if (FAILED(g_micCap->GetBuffer(&d, &quadros, &flags, nullptr, nullptr))) break;
                const int16_t* s = (const int16_t*)d;
                for (UINT32 i = 0; i < quadros; i++) g_micBuf.push_back((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0 : s[i]);
                g_micCap->ReleaseBuffer(quadros);
            }
        }
        else
        {
            // sem microfone: silencio no ritmo real (o jogo continua andando)
            static auto t0 = std::chrono::steady_clock::now();
            static uint64_t feitas = 0;
            uint64_t devidas = (uint64_t)(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * TAXA);
            for (; feitas < devidas; feitas++) g_micBuf.push_back(0);
        }
        while (g_micBuf.size() > TAXA / 2) g_micBuf.pop_front();   // no maximo 0,5 s de atraso
    }

    // amostra de 16 bits -> byte da tabela de 8 bits do jogo (o valor mais proximo)
    uint8_t Codigo8(int v)
    {
        static std::vector<uint8_t> mapa;
        if (mapa.empty())
        {
            int16_t val[256];
            for (int i = 0; i < 256; i++) val[i] = (int16_t)R16(TABELA_8BITS + i * 2);
            mapa.resize(65536);
            for (int x = -32768; x < 32768; x++)
            {
                int melhor = 0, dist = INT32_MAX;
                for (int i = 0; i < 256; i++) { int d = std::abs(val[i] - x); if (d < dist) { dist = d; melhor = i; } }
                mapa[(uint16_t)x] = (uint8_t)melhor;
            }
        }
        return mapa[(uint16_t)(int16_t)std::clamp(v, -32768, 32767)];
    }

    void Completar(uint32_t p, uint32_t estado, uint32_t bytes)
    {
        W32(p + 4, bytes);
        std::atomic_thread_fence(std::memory_order_release);
        W32(p + 0, estado);
    }

    void GravarWav();

    // com g_mutex
    void Servir()
    {
        LerMicrofone();
        while (!g_captura.empty())
        {
            uint32_t p = g_captura.front().end, buf = R32(p + 8), tam = R32(p + 12), fmt = R32(p + 20);
            size_t precisa = (size_t)tam * 2;   // 2 amostras de 16 kHz por byte (nos dois formatos)
            if (g_micBuf.size() < precisa) break;
            int pico = 0;
            for (uint32_t i = 0; i < tam; i++)
            {
                int a = g_micBuf[i * 2], b = g_micBuf[i * 2 + 1];
                pico = std::max({ pico, std::abs(a), std::abs(b) });
                if (fmt == 2) W8(buf + i, Codigo8((a + b) / 2));
                else W8(buf + i, fmt == 1 ? 0x00 : 0);
            }
            g_micBuf.erase(g_micBuf.begin(), g_micBuf.begin() + precisa);
            if (fmt != 2) { static bool avisou = false; if (!avisou) { avisou = true; Log("[voz] microfone: formato %u nao suportado (silencio)", fmt); } }
            Completar(p, 0, tam);
            g_captura.pop_front();
            g_nCap++;
            if (pico > 3000) g_capVoz++;
            if (VozLog() && (g_nCap <= 5 || g_nCap % 250 == 0))
                Log("[voz] microfone: pacote %llu (%u bytes, formato %u, pico %d; %llu com voz)", (unsigned long long)g_nCap, tam, fmt, pico, (unsigned long long)g_capVoz);
        }
        // fone: no maximo ~3 pacotes (~50 ms) na fila do XAudio2
        while (!g_toque.empty())
        {
            if (g_src)
            {
                XAUDIO2_VOICE_STATE st;
                g_src->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                while (g_tocando.size() > st.BuffersQueued) g_tocando.pop_front();
                if (st.BuffersQueued >= 3) break;
            }
            uint32_t p = g_toque.front().end, buf = R32(p + 8), tam = R32(p + 12), fmt = R32(p + 20);
            std::vector<int16_t> pcm;
            if (fmt == 1)
            {
                pcm.resize((size_t)tam * 2);
                for (uint32_t i = 0; i < tam; i++)
                {
                    uint8_t b = R8(buf + i);
                    pcm[i * 2] = (int16_t)std::clamp(G721Decode(b & 0xF, g_g721) * 4, -32768, 32767);
                    pcm[i * 2 + 1] = (int16_t)std::clamp(G721Decode(b >> 4, g_g721) * 4, -32768, 32767);
                }
            }
            else
            {
                pcm.resize(tam / 2);
                for (uint32_t i = 0; i < tam / 2; i++) pcm[i] = (int16_t)R16(buf + i * 2);
            }
            int pico = 0;
            for (int16_t v : pcm) pico = std::max(pico, std::abs((int)v));
            if (GravaWav()) { g_wav.insert(g_wav.end(), pcm.begin(), pcm.end()); if (g_wav.size() > TAXA_FONE * 600) g_wav.resize(TAXA_FONE * 600); }
            if (g_src && OpcoesAtuais().volumeVoz != g_volume)   // janela de audio: vale na hora
            {
                g_volume = OpcoesAtuais().volumeVoz;
                g_src->SetVolume(std::clamp(g_volume, 0, VOLUME_VOZ_MAX) / 100.0f);
            }
            if (g_src && !pcm.empty())
            {
                g_tocando.push_back(std::move(pcm));
                XAUDIO2_BUFFER xb{};
                xb.AudioBytes = (UINT32)(g_tocando.back().size() * 2);
                xb.pAudioData = (const BYTE*)g_tocando.back().data();
                g_src->SubmitSourceBuffer(&xb);
            }
            if (GravaWav())
            {
                // o jogo fecha sem XamVoiceClose: o arquivo e regravado a cada 2 s
                static auto ultima = std::chrono::steady_clock::now();
                if (std::chrono::steady_clock::now() - ultima > std::chrono::seconds(2)) { ultima = std::chrono::steady_clock::now(); GravarWav(); }
            }
            Completar(p, 0, tam);
            g_toque.pop_front();
            g_nToque++;
            if (VozLog() && (g_nToque <= 5 || g_nToque % 250 == 0 || pico > 2000))
            {
                static uint64_t comVoz = 0;
                if (pico > 2000) comVoz++;
                if (g_nToque <= 5 || g_nToque % 250 == 0 || comVoz <= 20 || comVoz % 100 == 0)
                    Log("[voz] fone: pacote %llu (%u bytes, formato %u, pico %d; %llu com voz)", (unsigned long long)g_nToque, tam, fmt, pico, (unsigned long long)comVoz);
            }
        }
    }

    void AbrirDispositivos()
    {
        if (TomTeste() <= 0) AbrirMicrofone();
        else Log("[voz] teste: bipe de %.0f Hz no lugar do microfone", TomTeste());
        if (!AbrirFone()) Log("[voz] o fone (XAudio2) nao abriu: a voz recebida nao vai tocar");
    }

    void FecharDispositivos()
    {
        if (g_src) { g_src->Stop(); g_src->DestroyVoice(); g_src = nullptr; }
        if (g_master) { g_master->DestroyVoice(); g_master = nullptr; }
        g_xa.Reset();
        g_tocando.clear();
        if (g_mic) g_mic->Stop();
        g_micCap.Reset(); g_mic.Reset();
    }

    void Laco()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        unsigned versao = SomVersao();
        AbrirDispositivos();
        while (g_rodando)
        {
            {
                std::lock_guard lk(g_mutex);
                if (SomVersao() != versao)   // trocou o fone ou o microfone na janela de audio: reabre
                {
                    versao = SomVersao();
                    FecharDispositivos();
                    AbrirDispositivos();
                    Log("[voz] dispositivos do chat de voz reabertos (troca na janela de audio)");
                }
                Servir();
            }
            Sleep(5);
        }
        FecharDispositivos();
        g_micBuf.clear();
        CoUninitialize();
    }

    void GravarWav()
    {
        const char* nome = getenv("FCRECOMP_VOZ_WAV");
        if (!nome || g_wav.empty()) return;
        FILE* f = fopen(nome, "wb");
        if (!f) return;
        uint32_t dados = (uint32_t)(g_wav.size() * 2), v;
        uint16_t h;
        fwrite("RIFF", 1, 4, f); v = 36 + dados; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
        v = 16; fwrite(&v, 4, 1, f); h = 1; fwrite(&h, 2, 1, f); h = 1; fwrite(&h, 2, 1, f);
        v = TAXA_FONE; fwrite(&v, 4, 1, f); v = TAXA_FONE * 2; fwrite(&v, 4, 1, f); h = 2; fwrite(&h, 2, 1, f); h = 16; fwrite(&h, 2, 1, f);
        fwrite("data", 1, 4, f); fwrite(&dados, 4, 1, f); fwrite(g_wav.data(), 2, g_wav.size(), f);
        fclose(f);
        static bool avisou = false;
        if (!avisou) { avisou = true; Log("[voz] voz recebida sendo gravada em %s (regravado a cada 2 s)", nome); }
    }

    bool VozValida(uint32_t v) { return std::find(g_vozes.begin(), g_vozes.end(), v) != g_vozes.end(); }
}

// XamVoiceCreate(usuario, flags, voz*): so o jogador 1 (o do teclado/primeiro controle) ganha o headset do PC
KFUNC(XamVoiceCreate)
{
    const uint32_t usuario = Gpr32(ctx, 3), saida = Gpr32(ctx, 5);
    uint32_t v = 0;
    if (VozLigada() && usuario == 0)
    {
        std::lock_guard lk(g_mutex);
        v = g_proxVoz++;
        g_vozes.push_back(v);
        g_g721 = G72x{};
        if (!g_rodando.exchange(true)) g_thread = std::thread(Laco);
    }
    if (saida) W32(saida, v);
    Log("[voz] XamVoiceCreate(usuario %u, flags %X) = %08X", usuario, Gpr32(ctx, 4), v);
    Ret(ctx, 0);
}

KFUNC(XamVoiceHeadsetPresent)
{
    std::lock_guard lk(g_mutex);
    Ret(ctx, VozValida(Gpr32(ctx, 3)) ? 1 : 0);
}

// XamVoiceSubmitPacket(voz, sentido, pacote): 1 = microfone, 0 = fone
KFUNC(XamVoiceSubmitPacket)
{
    const uint32_t v = Gpr32(ctx, 3), sentido = Gpr32(ctx, 4), p = Gpr32(ctx, 5);
    std::lock_guard lk(g_mutex);
    if (!p || !VozValida(v)) { Ret(ctx, 0x80004005); return; }
    W32(p + 4, 0);
    W32(p + 0, PENDENTE);
    (sentido ? g_captura : g_toque).push_back({ v, p });
    Ret(ctx, 0);
}

KFUNC(XamVoiceClose)
{
    const uint32_t v = Gpr32(ctx, 3);
    std::thread parar;
    {
        std::lock_guard lk(g_mutex);
        if (!VozValida(v)) { Ret(ctx, 0); return; }
        for (auto* fila : { &g_captura, &g_toque })
            for (auto it = fila->begin(); it != fila->end();)
                if (it->voz == v) { Completar(it->end, FALHOU, 0); it = fila->erase(it); }
                else ++it;
        g_vozes.erase(std::find(g_vozes.begin(), g_vozes.end(), v));
        Log("[voz] XamVoiceClose(%08X): %llu pacotes do microfone (%llu com voz), %llu do fone", v,
            (unsigned long long)g_nCap, (unsigned long long)g_capVoz, (unsigned long long)g_nToque);
        if (g_vozes.empty() && g_rodando.exchange(false)) parar = std::move(g_thread);
    }
    if (parar.joinable()) parar.join();
    {
        std::lock_guard lk(g_mutex);
        if (g_vozes.empty()) { GravarWav(); g_wav.clear(); }
    }
    Ret(ctx, 0);
}
