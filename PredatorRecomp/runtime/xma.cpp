// Decodificador XMA do Xbox 360 (o chip que decodifica o audio comprimido: musica, falas e a maior parte dos efeitos).
//
// Hardware imitado:
//  - Registradores em 0x7FEA0000 (indice = deslocamento / 4): 0x600 endereco fisico da lista de contextos, 0x650-0x659
//    "kick" (liga), 0x690-0x699 "lock" (desliga), 0x6A0-0x6A9 "clear"; em cada grupo, bit i do registrador r = contexto
//    (r - base) * 32 + i. 320 contextos de 64 bytes (XMA_CONTEXT_DATA, big-endian) numa lista em memoria fisica.
//  - Cada contexto tem 2 buffers de entrada (pacotes XMA de 2048 bytes) e um buffer de saida circular em blocos de
//    256 bytes, onde o chip escreve PCM de 16 bits big-endian (estereo intercalado).
//  - Pacote XMA: cabecalho de 32 bits (6 bits quadros, 15 bits posicao do 1o quadro, 3 bits metadados, 8 bits quantos
//    pacotes de outros fluxos pular) e quadros de bits: 15 bits de tamanho (contando ele mesmo), dados WMA Pro e 1 bit
//    final "tem mais quadros". Um quadro pode continuar no pacote seguinte do mesmo fluxo (logo apos o cabecalho).
//
// Decodificacao: o quadro XMA e um quadro WMA Pro (512 amostras por canal, opcoes 0x10D6: prefixo de tamanho, ganho de
// faixa dinamica, ate 4 blocos por canal), decodificado pelo nosso XmaAudioDecoder (xma_audio.cpp).
//
// Depuracao: FCRECOMP_XMALOG=1 (resumo no log), FCRECOMP_XMAWAV=1 (salva o som decodificado dos primeiros contextos em
// build/xma_N.wav), FCRECOMP_SEMXMA=1 (desliga: o jogo fica sem musica/falas como antes), FCRECOMP_XMAREF=arquivo
// (grava entrada e som de todos os contextos) + FCRECOMP_XMATEST=arquivo (decodifica de novo e compara).
#include "guest.h"
#include "thread.h"
#include "diag.h"
#include <windows.h>
#include "xma_audio.h"
#include "perf.h"
#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <deque>
#include <algorithm>
#include <cmath>


namespace
{
    constexpr uint32_t XMA_BASE = 0x7FEA0000;
    constexpr uint32_t CONTEXT_COUNT = 320;
    constexpr uint32_t PACKET_BYTES = 2048, PACKET_BITS = PACKET_BYTES * 8;

    bool g_log = false, g_wav = false;
    uint32_t g_arrayGuest = 0;   // endereco (do jogo) da lista de contextos
    std::atomic<bool> g_allocated[CONTEXT_COUNT];
    std::atomic<bool> g_enabled[CONTEXT_COUNT];
    std::atomic<bool> g_clearRequest[CONTEXT_COUNT];
    std::mutex g_ctxMutex[CONTEXT_COUNT];   // o jogo "trava" o contexto (0x690) e espera o chip largar dele
    HANDLE g_workEvent = nullptr;

    // os registradores do chip sao escritos/lidos pelo jogo com os bytes invertidos (little-endian)
    uint32_t Reg(uint32_t index) { return *(uint32_t*)G(XMA_BASE + index * 4); }
    void SetReg(uint32_t index, uint32_t v) { *(uint32_t*)G(XMA_BASE + index * 4) = v; }
    uint8_t* Phys(uint32_t addr) { return G(0xA0000000u | (addr & 0x1FFFFFFF)); }

    // ------------------------------------------------------------ contexto (64 bytes big-endian)
    struct Ctx
    {
        uint32_t d[16];
        uint32_t Get(int w, int bit, int n) const { return (d[w] >> bit) & ((n == 32) ? ~0u : ((1u << n) - 1)); }
        void Set(int w, int bit, int n, uint32_t v) { uint32_t m = ((n == 32) ? ~0u : ((1u << n) - 1)) << bit; d[w] = (d[w] & ~m) | ((v << bit) & m); }
        // dword 0
        uint32_t ib0Count() const { return Get(0, 0, 12); }
        uint32_t loopCount() const { return Get(0, 12, 8); }
        bool ib0Valid() const { return Get(0, 20, 1); }
        bool ib1Valid() const { return Get(0, 21, 1); }
        uint32_t outBlocks() const { return Get(0, 22, 5); }
        uint32_t outWrite() const { return Get(0, 27, 5); }
        // dword 1
        uint32_t ib1Count() const { return Get(1, 0, 12); }
        uint32_t loopSubframeEnd() const { return Get(1, 14, 3); }    // ultimo subquadro (128 amostras) do quadro final do laco
        uint32_t loopSubframeSkip() const { return Get(1, 17, 3); }   // subquadros pulados no 1o quadro depois da volta
        uint32_t sampleRate() const { return Get(1, 27, 2); }   // 0=24k 1=32k 2=44,1k 3=48k
        bool stereo() const { return Get(1, 29, 1); }
        bool outValid() const { return Get(1, 31, 1); }
        // dwords 2-9
        uint32_t readOffset() const { return Get(2, 0, 26); }
        uint32_t loopStart() const { return Get(3, 0, 26); }
        uint32_t loopEnd() const { return Get(4, 0, 26); }
        uint32_t currentBuffer() const { return Get(4, 31, 1); }
        uint32_t ib0() const { return d[5]; }
        uint32_t ib1() const { return d[6]; }
        uint32_t out() const { return d[7]; }
        uint32_t outRead() const { return Get(9, 0, 5); }
    };

    uint32_t CtxGuest(uint32_t id) { return g_arrayGuest + id * 64; }
    void Load(uint32_t id, Ctx& c) { for (int i = 0; i < 16; i++) c.d[i] = R32(CtxGuest(id) + i * 4); }

    // ------------------------------------------------------------ bits (MSB primeiro)
    uint32_t GetBit(const uint8_t* p, uint32_t bit) { return (p[bit >> 3] >> (7 - (bit & 7))) & 1; }
    uint32_t GetBits(const uint8_t* p, uint32_t bit, uint32_t n)
    {
        uint32_t v = 0;
        for (uint32_t i = 0; i < n; i++) v = (v << 1) | GetBit(p, bit + i);
        return v;
    }
    struct BitWriter
    {
        std::vector<uint8_t> buf;
        uint32_t pos = 0;
        void Put(uint32_t v, uint32_t n)
        {
            for (uint32_t i = 0; i < n; i++)
            {
                if ((pos >> 3) >= buf.size()) buf.push_back(0);
                if ((v >> (n - 1 - i)) & 1) buf[pos >> 3] |= 0x80 >> (pos & 7);
                pos++;
            }
        }
    };

    uint32_t FrameOffset(const uint8_t* packet) { return (((packet[0] & 3) << 13) | (packet[1] << 5) | (packet[2] >> 3)) + 32; }
    uint32_t SkipCount(const uint8_t* packet) { return packet[3]; }

    // ------------------------------------------------------------ estado de cada contexto (lado do PC)
    struct State
    {
        XmaAudioDecoder dec;
        bool decReady = false;
        uint32_t decChannels = 0, decRate = 0;
        std::deque<int16_t> pending;      // PCM decodificado que ainda nao coube no buffer de saida
        std::vector<int16_t> scratch;
        FILE* wav = nullptr;
        uint32_t wavBytes = 0;
        uint64_t frames = 0, errors = 0, samples = 0;
        // saida cheia ate a posicao de leitura: leitura == escrita tambem e "vazia", entao lembramos que estava cheia
        bool outFull = false;
        ULONGLONG soundStart = 0;   // depuracao: duracao de cada som (amostras x tempo real)
        double soundStartMs = 0;
        uint32_t lastBuf = 0;
        // quadro que comecou no fim de um buffer de entrada e continua no proximo
        BitWriter carry;
        bool hasCarry = false;
        uint64_t carryJoined = 0, carryDropped = 0;
        uint64_t soundSamples = 0;
        uint32_t outFullPos = 0, outAddr = 0;
        uint32_t lastRefBuf = 0;  // ultimo buffer anotado na gravacao FCRECOMP_XMAREF
        uint32_t mudoKey = 0;     // FCRECOMP_XMAMUDO: buffer ja conferido e se ele sai em silencio
        bool mudo = false;
        // FCRECOMP_XMACONSUMO: quanto da saida o jogo leu (amostras) e quando, por som
        uint32_t consKey = 0, consSig = 0, consRead = 0, consRate = 0, consCh = 1;
        uint64_t consBytes = 0;
        double consFirst = 0, consLast = 0;
        uint32_t bufFrames = 0;   // depuracao: quadros decodificados do buffer de entrada atual (FCRECOMP_XMACONTA)
        std::vector<uint32_t> bufStarts;   // depuracao: onde cada um comecou (pacote * bits + bit)
        // subquadros do laco: o nosso decodificador entrega cada quadro um quadro depois (o 1o so "aquece"), entao o
        // som do quadro final do laco sai na 1a decodificacao depois da volta (corta o fim dele: lacoCorta) e o do
        // 1o quadro do laco sai na 2a (pula o comeco dele: lacoPula). lacoEtapa: 1 = proxima saida e a do quadro final
        uint32_t lacoCorta = 0, lacoPula = 0, lacoEtapa = 0;
    };
    State g_state[CONTEXT_COUNT];

    // FCRECOMP_XMAREF=arquivo: grava cada preparo do decodificador e cada quadro (entrada + som decodificado) de todos
    // os contextos, para FCRECOMP_XMATEST=arquivo repetir com o decodificador atual e comparar amostra a amostra.
    // Registros: 'I' contexto canais taxa | 'Q' contexto bits <bytes do quadro> amostras <PCM 16 bits>
    std::mutex g_refMutex;
    FILE* RefFile()
    {
        static FILE* f = getenv("FCRECOMP_XMAREF") ? fopen(getenv("FCRECOMP_XMAREF"), "wb") : nullptr;
        if (f && ftell(f) > 0x60000000) { fclose(f); f = nullptr; }   // limite de 1,5 GB
        return f;
    }
    void RefInit(uint32_t id, uint32_t ch, uint32_t sr)
    {
        std::lock_guard lock(g_refMutex);
        if (FILE* f = RefFile()) { uint32_t r[4] = { 'I', id, ch, sr }; fwrite(r, 4, 4, f); }
    }
    // 'B' contexto endereco pacotes ms: comecou a ler um buffer de entrada novo (para recortar cada som pela origem;
    // ms = relogio do log, o mesmo do "som acabou")
    void RefBuffer(uint32_t id, uint32_t addr, uint32_t packets)
    {
        std::lock_guard lock(g_refMutex);
        if (FILE* f = RefFile()) { uint32_t r[5] = { 'B', id, addr, packets, (uint32_t)perf::Now() }; fwrite(r, 4, 5, f); }
    }
    void RefFrame(uint32_t id, const uint8_t* data, uint32_t bits, const std::vector<int16_t>& pcm)
    {
        std::lock_guard lock(g_refMutex);
        if (FILE* f = RefFile())
        {
            uint32_t r[3] = { 'Q', id, bits }; fwrite(r, 4, 3, f);
            fwrite(data, 1, (bits + 7) / 8, f);
            uint32_t n = (uint32_t)pcm.size(); fwrite(&n, 4, 1, f);
            fwrite(pcm.data(), 2, n, f);
        }
    }

    void WavClose(State& s)
    {
        if (!s.wav) return;
        uint32_t riff = 36 + s.wavBytes;
        fseek(s.wav, 4, SEEK_SET); fwrite(&riff, 4, 1, s.wav);
        fseek(s.wav, 40, SEEK_SET); fwrite(&s.wavBytes, 4, 1, s.wav);
        fclose(s.wav);
        s.wav = nullptr;
    }

    void ResetState(uint32_t id)
    {
        State& s = g_state[id];
        WavClose(s);
        s.decReady = false;
        s.pending.clear();
        s.outFull = false;
        s.hasCarry = false;
        s.lacoCorta = s.lacoPula = s.lacoEtapa = 0;
    }

    // Le o proximo quadro do fluxo a partir de (pacote, bit) no buffer atual, seguindo os pacotes do mesmo fluxo
    // (pula SkipCount pacotes de outros fluxos). Retorna false se o buffer acabou antes do fim do quadro.
    struct Cursor { uint32_t packet, bit; };
    bool NextPacket(const uint8_t* buf, uint32_t packets, Cursor& c)
    {
        uint32_t next = c.packet + 1 + SkipCount(buf + c.packet * PACKET_BYTES);
        c.packet = next;
        c.bit = 32;
        return next < packets;
    }
    bool ReadBitsChain(const uint8_t* buf, uint32_t packets, Cursor& c, uint32_t n, BitWriter* out, uint32_t* value)
    {
        uint32_t v = 0;
        for (uint32_t i = 0; i < n; i++)
        {
            if (c.bit >= PACKET_BITS && !NextPacket(buf, packets, c)) return false;
            uint32_t b = GetBit(buf + c.packet * PACKET_BYTES, c.bit++);
            v = (v << 1) | b;
            if (out) out->Put(b, 1);
        }
        if (value) *value = v;
        return true;
    }

    // Depuracao (FCRECOMP_XMACONTA): conta os quadros de um buffer andando por eles um atras do outro (como ficam no
    // fluxo), sem as regras de pacote do Work. Comparado com os decodificados mostra quadros pulados (som "corrido").
    uint32_t CountFrames(const uint8_t* buf, uint32_t packets, std::vector<uint32_t>* starts = nullptr)
    {
        if (!packets) return 0;
        Cursor c{ 0, FrameOffset(buf) };
        uint32_t n = 0;
        for (int guard = 0; guard < 100000 && c.packet < packets; guard++)
        {
            if (c.bit >= PACKET_BITS) { if (!NextPacket(buf, packets, c)) break; c.bit = FrameOffset(buf + c.packet * PACKET_BYTES); continue; }
            Cursor start = c;
            uint32_t len = 0;
            if (!ReadBitsChain(buf, packets, c, 15, nullptr, &len)) break;
            if (len == 0x7FFF || len < 16)   // sem mais quadros neste pacote: primeiro quadro do proximo
            {
                c = start;
                if (!NextPacket(buf, packets, c)) break;
                c.bit = FrameOffset(buf + c.packet * PACKET_BYTES);
                continue;
            }
            n++;
            if (starts) starts->push_back(start.packet * PACKET_BITS + start.bit);
            if (!ReadBitsChain(buf, packets, c, len - 15, nullptr, nullptr)) break;   // continua no proximo buffer
        }
        return n;
    }

    // "impressao digital" de um som: FNV-1a do 1o pacote (o endereco muda de uma execucao para outra, o conteudo nao)
    uint32_t Assinatura(const uint8_t* buf)
    {
        uint32_t h = 2166136261u;
        for (int i = 0; i < 2048; i++) h = (h ^ buf[i]) * 16777619u;
        return h;
    }

    // depuracao (FCRECOMP_XMAMUDO=A1B2C3D4,...): sons com essas assinaturas (o "som XXXXXXXX" do FCRECOMP_XMALOG=2)
    // saem em silencio; o jogo continua lendo normalmente (para ouvir qual som atrapalha)
    std::vector<uint32_t> ListaAssinaturas(const char* p)
    {
        std::vector<uint32_t> v;
        for (; p && *p;)
        {
            char* fim; uint32_t a = strtoul(p, &fim, 16);
            if (fim == p) break;
            v.push_back(a);
            p = fim + (*fim == ',' || *fim == ';');
        }
        return v;
    }

    bool Mudo(uint32_t assinatura)
    {
        static const std::vector<uint32_t> lista = [] {
            std::vector<uint32_t> v;
            for (const char* p = getenv("FCRECOMP_XMAMUDO"); p && *p;)
            {
                char* fim; uint32_t a = strtoul(p, &fim, 16);
                if (fim == p) break;
                v.push_back(a);
                p = fim + (*fim == ',' || *fim == ';');
            }
            return v;
        }();
        for (uint32_t a : lista) if (a == assinatura) return true;
        return false;
    }

    // depuracao (FCRECOMP_XMACONSUMO=1): ao fim de cada som, quanto audio o jogo leu da saida e em quanto tempo real
    // (1,00x = velocidade normal; acima disso o jogo esta tocando o som mais rapido/agudo)
    const bool g_consumo = getenv("FCRECOMP_XMACONSUMO") != nullptr;
    void ConsumoFim(uint32_t id, State& s)
    {
        if (s.consBytes && s.consRate)
        {
            double audio = s.consBytes / 2.0 / s.consCh / s.consRate, real = (s.consLast - s.consFirst) / 1000.0;
            Log("[xmaconsumo] ctx %u som %08X %u Hz: %.2f s de audio lidos pelo jogo em %.2f s reais -> %.2fx (%.0f-%.0f ms)",
                id, s.consSig, s.consRate, audio, real, real > 0 ? audio / real : 0, s.consFirst, s.consLast);
        }
        s.consBytes = 0; s.consRead = 0; s.consFirst = s.consLast = 0;
    }

    // Decodifica quadros enquanto houver entrada e espaco na saida
    void Work(uint32_t id)
    {
        State& s = g_state[id];
        std::lock_guard lock(g_ctxMutex[id]);
        if (!g_enabled[id]) return;
        Ctx c;
        Load(id, c);
        const Ctx before = c;
        static std::atomic<int> logged{ 0 };
        if (g_log && logged++ < 30)
            Log("[xma] contexto %u: %08X %08X %08X %08X %08X in0=%08X in1=%08X out=%08X work=%08X d9=%08X", id,
                c.d[0], c.d[1], c.d[2], c.d[3], c.d[4], c.d[5], c.d[6], c.d[7], c.d[8], c.d[9]);
        if (!c.outValid() || !c.out()) return;
        uint32_t ch = c.stereo() ? 2 : 1;
        static const uint32_t rates[4] = { 24000, 32000, 44100, 48000 };
        uint32_t sr = rates[c.sampleRate()];
        if (!s.decReady || s.decChannels != ch || s.decRate != sr)
        {
            s.decReady = s.dec.Init((int)ch, (int)sr);
            s.decChannels = ch; s.decRate = sr;
            RefInit(id, ch, sr);
            if (!s.decReady) { s.errors++; return; }
            if (g_wav && id < 8 && !s.wav)
            {
                char name[32]; snprintf(name, sizeof(name), "xma_%u.wav", id);
                if ((s.wav = fopen(name, "wb")))
                {
                    uint32_t hdr[11] = { 0x46464952, 0, 0x45564157, 0x20746D66, 16, 1u | (ch << 16), sr, sr * ch * 2, (ch * 2) | (16u << 16), 0x61746164, 0 };
                    fwrite(hdr, 4, 11, s.wav);
                    s.wavBytes = 0;
                }
            }
        }

        uint32_t capacity = c.outBlocks() * 256;
        if (!capacity) return;
        if (s.outAddr != c.out()) { s.outAddr = c.out(); s.outFull = false; s.consRead = c.outRead(); }   // buffer de saida novo
        if (g_consumo && c.outRead() != s.consRead)
        {
            // o que o jogo leu desde a ultima vez (o primeiro pedaco so marca o inicio: ele foi lido de uma vez)
            uint32_t blocks = c.outBlocks();
            uint32_t lidos = (c.outRead() + blocks - s.consRead % blocks) % blocks * 256;
            double agora = perf::Now();
            if (s.consFirst == 0) s.consFirst = agora; else s.consBytes += lidos;
            s.consLast = agora; s.consRead = c.outRead();
            s.consRate = sr; s.consCh = ch;
        }
        // continua cheia se o jogo nao leu nada desde que enchemos
        bool stillFull = s.outFull && c.outRead() == s.outFullPos && c.outWrite() == s.outFullPos;
        // depuracao (FCRECOMP_XMAFALTA): o jogo leu toda a saida de um som que ainda nao acabou (faltou som = corte)
        if (static const bool falta = getenv("FCRECOMP_XMAFALTA") != nullptr; falta && s.soundSamples && !stillFull &&
            c.outRead() == c.outWrite() && (c.ib0Valid() || c.ib1Valid()))
        {
            static std::atomic<int> n{ 0 };
            if (n++ < 60) Log("[xmafalta] ctx %u (%u Hz) saida vazia com som em andamento, t=%.0f ms, pendente %zu", id,
                rates[c.sampleRate()], perf::Now(), s.pending.size());
        }
        s.outFull = false;
        uint32_t frameBytes = 1024 * ch;   // 512 amostras x 2 bytes x canais
        bool wrote = false;
        for (int guard = 0; guard < 64; guard++)
        {
            // espaco livre no buffer circular de saida (leitura e escrita em blocos de 256 bytes)
            uint32_t rd = c.outRead() * 256, wr = c.outWrite() * 256;
            uint32_t used = wr >= rd ? wr - rd : capacity - rd + wr;
            if ((wrote || stillFull) && wr == rd) used = capacity;   // escrevemos ate encostar na leitura: cheio
            uint32_t freeBytes = capacity - used;

            // descarrega o que ja foi decodificado
            const size_t livre = s.pending.size();
            if (livre * 2 >= 256 && freeBytes >= 256)
            {
                uint32_t n = std::min<uint32_t>((uint32_t)livre * 2, freeBytes);
                n -= n % (256);                                      // blocos inteiros
                uint32_t samples = n / 2;
                uint8_t* outBuf = Phys(c.out());
                bool mudo = s.mudo;
                for (uint32_t i = 0; i < samples; i++)
                {
                    int16_t v = s.pending.front(); s.pending.pop_front();
                    if (s.wav) { fwrite(&v, 2, 1, s.wav); s.wavBytes += 2; }
                    if (mudo) v = 0;
                    uint32_t pos = (wr + i * 2) % capacity;
                    outBuf[pos] = (uint8_t)((uint16_t)v >> 8); outBuf[pos + 1] = (uint8_t)v;
                }
                wr = (wr + n) % capacity;
                c.Set(0, 27, 5, wr / 256);
                wrote = wrote || n > 0;
                continue;
            }
            if (freeBytes < frameBytes) break;
            if (livre * 2 >= frameBytes) break;   // ainda ha PCM esperando espaco

            // proximo quadro da entrada
            bool cur = c.currentBuffer();
            bool valid = cur ? c.ib1Valid() : c.ib0Valid();
            if (!valid)
            {
                bool other = cur ? c.ib0Valid() : c.ib1Valid();
                if (!other)   // sem entrada: saida "invalida" ate o jogo dar mais
                {
                    if (g_log && s.soundSamples)
                    {
                        double audio = s.soundSamples / (double)ch / sr, real = (GetTickCount64() - s.soundStart) / 1000.0;
                        Log("[xma] contexto %u: som acabou: %.2f s de audio (%u canais %u Hz) tocados em %.2f s reais -> %.2fx (%.0f-%.0f ms)",
                            id, audio, ch, sr, real, real > 0 ? audio / real : 0, s.soundStartMs, perf::Now());
                    }
                    s.soundSamples = 0;
                    c.Set(1, 31, 1, 0); break;
                }
                c.Set(4, 31, 1, cur ? 0 : 1);
                c.Set(2, 0, 26, 0);
                continue;
            }
            uint32_t packets = cur ? c.ib1Count() : c.ib0Count();
            const uint8_t* buf = Phys(cur ? c.ib1() : c.ib0());
            // FCRECOMP_XMALOG=2: inicio de cada buffer de entrada novo (para achar o som no dvdsound.dat)
            static const bool bufLog = getenv("FCRECOMP_XMALOG") && getenv("FCRECOMP_XMALOG")[0] == '2';
            uint32_t bufAddr = cur ? c.ib1() : c.ib0();
            uint32_t bufKey = bufAddr + packets * 7 + *(const uint32_t*)(buf + 4) + *(const uint32_t*)(buf + 12);
            if (c.readOffset() <= 32 && s.lastRefBuf != bufKey) { s.lastRefBuf = bufKey; RefBuffer(id, bufAddr, packets); }
            if (g_consumo && c.readOffset() <= 32 && s.consKey != bufKey)
            {
                ConsumoFim(id, s);
                s.consKey = bufKey; s.consSig = Assinatura(buf); s.consRead = c.outRead();
            }
            if (static const bool temMudo = getenv("FCRECOMP_XMAMUDO") != nullptr; temMudo && s.mudoKey != bufKey)
            {
                s.mudoKey = bufKey;
                s.mudo = Mudo(Assinatura(buf));
            }
            if (bufLog && s.lastBuf != bufKey)
            {
                s.lastBuf = bufKey;
                std::string h;
                for (int k = 0; k < 48; k++) { char t[4]; snprintf(t, sizeof(t), "%02X", buf[k]); h += t; }
                Log("[xmabuf] ctx %u end %08X som %08X%s leitura %u t=%.0f %u canais %u Hz, %u pacotes, laco %u (%u-%u) d1=%08X d3=%08X d4=%08X d9=%08X: %s", id, bufAddr,
                    Assinatura(buf), s.mudo ? " (MUDO)" : "", c.readOffset(), perf::Now(), ch, sr, packets, c.loopCount(), c.loopStart(), c.loopEnd(), c.d[1], c.d[3], c.d[4], c.d[9], h.c_str());
            }
            // FCRECOMP_XMADUMP=1: salva tambem os pacotes crus do primeiro buffer do contexto 0
            static bool rawDone = false;
            static const bool xmaDump = getenv("FCRECOMP_XMADUMP") != nullptr;
            if (!rawDone && id == 0 && xmaDump)
                if (FILE* rf = fopen("xma_pacotes.bin", "wb"))
                {
                    rawDone = true;
                    uint32_t n = std::min<uint32_t>(packets, 2000);
                    fwrite(&ch, 4, 1, rf); fwrite(&sr, 4, 1, rf); fwrite(&n, 4, 1, rf);
                    fwrite(buf, PACKET_BYTES, n, rf);
                    fclose(rf);
                }
            // laco (loop) do som: volta ao inicio do laco. O fim do laco pode cair logo no comeco do ultimo pacote, onde
            // nenhum quadro novo comeca; por isso ele vale em qualquer caminho que chegaria ao fim dos dados (antes, so
            // depois de um quadro: o som acabava em vez de repetir, como uma das camadas do helicoptero do treino)
            auto loopBack = [&] {
                if (!c.loopCount() || !c.loopEnd()) return false;
                // subquadros do laco (como o chip): do quadro final so tocam os subquadros 0..fim, e do primeiro quadro
                // depois da volta os primeiros "pula" ficam de fora. Os arquivos trazem ~10 ms de silencio no fim e no
                // comeco do laco; sem isso cada volta tinha um buraco (cachoeira da fase 05 "repetindo" a cada 2,4 s)
                s.lacoCorta = (3 - std::min<uint32_t>(c.loopSubframeEnd(), 3)) * 128 * ch;
                s.lacoPula = std::min<uint32_t>(c.loopSubframeSkip(), 3) * 128 * ch;
                s.lacoEtapa = (s.lacoCorta || s.lacoPula) ? 1 : 0;
                c.Set(2, 0, 26, c.loopStart());
                if (c.loopCount() < 255) c.Set(0, 12, 8, c.loopCount() - 1);
                s.hasCarry = false;
                return true;
            };
            auto swapBuffer = [&] {
                if (loopBack()) return;
                if (static const bool conta = getenv("FCRECOMP_XMACONTA") != nullptr; conta)
                {
                    std::vector<uint32_t> starts;
                    uint32_t total = CountFrames(buf, packets, &starts);
                    static std::atomic<int> nLog{ 0 };
                    if ((s.bufFrames + 1 < total || nLog < 15) && nLog++ < 80)
                    {
                        Log("[xmaconta] ctx %u %u Hz %u canais: %u pacotes, %u quadros no buffer, %u decodificados%s", id, sr, ch,
                            packets, total, s.bufFrames, s.bufFrames + 1 < total ? "  <-- FALTARAM" : "");
                        if (s.bufFrames + 1 < total)
                        {
                            std::string miss;
                            for (uint32_t p : starts)
                                if (std::find(s.bufStarts.begin(), s.bufStarts.end(), p) == s.bufStarts.end())
                                {
                                    char t[48]; snprintf(t, sizeof(t), " pac%u:bit%u", p / PACKET_BITS, p % PACKET_BITS); miss += t;
                                }
                            std::string pk;
                            for (uint32_t k = 0; k < packets && k < 16; k++)
                            {
                                char t[32]; snprintf(t, sizeof(t), " %u:fo%u/pula%u", k, FrameOffset(buf + k * PACKET_BYTES), SkipCount(buf + k * PACKET_BYTES)); pk += t;
                            }
                            Log("[xmaconta]   faltaram:%s | pacotes:%s | carry juntados %llu perdidos %llu", miss.c_str(), pk.c_str(),
                                (unsigned long long)s.carryJoined, (unsigned long long)s.carryDropped);
                        }
                    }
                    s.bufStarts.clear();
                }
                s.bufFrames = 0;
                static std::atomic<int> swLog{ 0 };
                if (g_log && swLog++ < 60)
                    Log("[xma] contexto %u: troca de buffer (este tinha %u pacotes, leitura em %u/%u)", id, packets,
                        c.readOffset(), packets * PACKET_BITS);
                if (g_log && swLog <= 60 && packets > 8)
                {
                    std::string h;
                    for (uint32_t k = 0; k < packets && k < 130; k++)
                    {
                        const uint8_t* pk = buf + k * PACKET_BYTES;
                        char t[24]; snprintf(t, sizeof(t), " %u:%02X%02X%02X%02X", k, pk[0], pk[1], pk[2], pk[3]); h += t;
                    }
                    Log("[xma]   cabecalhos:%s", h.c_str());
                }
                c.Set(0, cur ? 21 : 20, 1, 0);   // buffer atual consumido
                c.Set(4, 31, 1, cur ? 0 : 1);
                c.Set(2, 0, 26, 0);
            };
            uint32_t off = c.readOffset();
            Cursor cs{ off / PACKET_BITS, off % PACKET_BITS };
            if (cs.packet >= packets) { swapBuffer(); continue; }
            BitWriter fw;
            bool fromCarry = false;
            if (cs.bit < 32)
            {
                // inicio de pacote: vai para o primeiro quadro que comeca nele
                uint32_t fo = FrameOffset(buf + cs.packet * PACKET_BYTES);
                // quadro que comecou no fim do buffer anterior: o resto dele esta logo apos o cabecalho deste pacote
                // (o "frame offset" pula exatamente esse resto). So vale no 1o pacote de um buffer novo.
                if (s.hasCarry && cs.packet == 0 && fo > 32)
                {
                    fw = std::move(s.carry);
                    fromCarry = true;
                    cs.bit = 32;
                }
                else
                {
                    if (s.hasCarry) s.carryDropped++;
                    if (fo >= PACKET_BITS) { s.hasCarry = false; if (!NextPacket(buf, packets, cs)) { swapBuffer(); continue; } c.Set(2, 0, 26, cs.packet * PACKET_BITS); continue; }
                    cs.bit = fo;
                }
                s.hasCarry = false;
            }
            Cursor start = cs;
            if (!fromCarry && c.loopCount() > 0 && c.loopEnd() && start.packet * PACKET_BITS + start.bit >= c.loopEnd())
            {
                loopBack();   // quadro que ja fica depois do fim do laco
                continue;
            }
            // quadro cortado pelo fim do buffer: guarda os bits lidos e continua no proximo buffer (como o chip real)
            auto saveCarry = [&] {
                s.carry = std::move(fw);
                s.hasCarry = true;
                swapBuffer();
            };
            if (fw.pos < 15 && !ReadBitsChain(buf, packets, cs, 15 - fw.pos, &fw, nullptr)) { saveCarry(); continue; }
            uint32_t len = GetBits(fw.buf.data(), 0, 15);
            if (len == 0x7FFF || len < 16 || len <= fw.pos)
            {
                // sem mais quadros neste pacote: proximo pacote do fluxo
                if (fromCarry) s.carryDropped++;
                Cursor n = start;
                if (!NextPacket(buf, packets, n)) { swapBuffer(); continue; }
                c.Set(2, 0, 26, n.packet * PACKET_BITS);
                continue;
            }
            if (fromCarry)
            {
                // confere: o resto do quadro tem que terminar onde o pacote diz que o proximo quadro comeca
                uint32_t fo = FrameOffset(buf);
                if (32 + (len - fw.pos) != fo)
                {
                    s.carryDropped++;
                    c.Set(2, 0, 26, 0);   // volta ao inicio do pacote, sem o pedaco guardado
                    continue;
                }
            }
            if (!ReadBitsChain(buf, packets, cs, len - fw.pos, &fw, nullptr)) { saveCarry(); continue; }
            if (fromCarry) s.carryJoined++;
            bool more = GetBit(fw.buf.data(), len - 1);

            // FCRECOMP_XMADUMP=1: salva os quadros do contexto 0 (para FCRECOMP_XMATEST)
            static FILE* dump = getenv("FCRECOMP_XMADUMP") ? fopen("xma_quadros.bin", "wb") : nullptr;
            static uint32_t dumped = 0;
            if (dump && id == 0 && dumped < 4000)
            {
                if (!dumped++) { fwrite(&ch, 4, 1, dump); fwrite(&sr, 4, 1, dump); }
                fwrite(&len, 4, 1, dump); fwrite(fw.buf.data(), 1, (len + 7) / 8, dump); fflush(dump);
            }

            // depuracao (FCRECOMP_XMARITMO=endereco do buffer): cada quadro desse som com hora, posicao e saida
            if (static const uint32_t ritmo = getenv("FCRECOMP_XMARITMO") ? strtoul(getenv("FCRECOMP_XMARITMO"), nullptr, 16) : 0;
                ritmo && (bufAddr == ritmo || Assinatura(buf) == ritmo))   // endereco ou assinatura ("som XXXXXXXX")
            {
                static std::atomic<int> n{ 0 };
                if (n++ < 400)
                    Log("[xmaritmo] t=%.0f ctx %u quadro em %u (pac %u) %u bits, saida le=%u esc=%u de %u, d1=%08X canais %u",
                        perf::Now(), id, start.packet * PACKET_BITS + start.bit, start.packet, len, c.outRead(), c.outWrite(),
                        c.outBlocks(), c.d[1], ch);
            }
            // decodifica
            s.scratch.resize(512 * ch);
            int got = s.dec.DecodeFrame(fw.buf.data(), len, s.scratch.data());
            s.scratch.resize((size_t)got * ch);
            RefFrame(id, fw.buf.data(), len, s.scratch);
            if (s.lacoEtapa && !s.scratch.empty())   // volta do laco (ver lacoCorta): fim do quadro final, comeco do 1o
            {
                const uint32_t antes = (uint32_t)s.scratch.size();
                if (s.lacoEtapa == 1)
                {
                    uint32_t k = std::min<uint32_t>(s.lacoCorta, antes);
                    s.scratch.erase(s.scratch.end() - k, s.scratch.end());
                    s.lacoEtapa = 2;
                }
                else
                {
                    uint32_t k = std::min<uint32_t>(s.lacoPula, antes);
                    s.scratch.erase(s.scratch.begin(), s.scratch.begin() + k);
                    s.lacoEtapa = 0;
                }
                if (static const bool lg = getenv("FCRECOMP_XMALACO") != nullptr; lg)
                {
                    static std::atomic<int> n{ 0 };
                    if (n++ < 40) Log("[xmalaco] ctx %u: laco, saida de %u amostras ficou com %u", id, antes, (uint32_t)s.scratch.size());
                }
            }
            {
                s.frames++;
                s.bufFrames++;
                if (s.bufStarts.size() < 4096) s.bufStarts.push_back(start.packet * PACKET_BITS + start.bit);
                if (!s.soundSamples) { s.soundStart = GetTickCount64(); s.soundStartMs = perf::Now(); }
                s.soundSamples += s.scratch.size();
                for (int16_t v : s.scratch) s.pending.push_back(v);
                s.samples += s.scratch.size();
                if (s.wav)   // FCRECOMP_XMAWAV agora grava o que sai para o jogo (na descarga, ja com os cortes do laco); aqui so o cabecalho
                {
                    // cabecalho sempre atualizado (o programa pode sair sem fechar o arquivo)
                    long pos = ftell(s.wav);
                    uint32_t riff = 36 + s.wavBytes;
                    fseek(s.wav, 4, SEEK_SET); fwrite(&riff, 4, 1, s.wav);
                    fseek(s.wav, 40, SEEK_SET); fwrite(&s.wavBytes, 4, 1, s.wav);
                    fseek(s.wav, pos, SEEK_SET);
                    fflush(s.wav);
                }
            }

            // posicao do proximo quadro
            const uint32_t frameEnd = cs.packet * PACKET_BITS + cs.bit;
            if (more && cs.bit < PACKET_BITS)
                c.Set(2, 0, 26, cs.packet * PACKET_BITS + cs.bit);
            else
            {
                // ultimo quadro que comeca neste pacote (bit final 0): normalmente ele atravessa para o pacote seguinte,
                // e o proximo quadro comeca no pacote DEPOIS daquele em que este comecou (o "frame offset" dele pula
                // o pedaco que atravessou). Antes contavamos a partir de onde o quadro terminou e pulavamos um pacote
                // inteiro: metade de cada fala sumia (som "acelerado", falas acabando antes da hora).
                Cursor n = more ? cs : start;
                if (!NextPacket(buf, packets, n)) { swapBuffer(); continue; }
                c.Set(2, 0, 26, n.packet * PACKET_BITS);
            }
            // este quadro fecha o laco (terminou no fim dele) ou o proximo ja fica depois do fim
            if (c.loopCount() > 0 && c.loopEnd() && (frameEnd >= c.loopEnd() || c.readOffset() >= c.loopEnd())) loopBack();
        }
        // saida cheia: o jogo marca de novo quando consumir
        if ((wrote || stillFull) && c.outWrite() == c.outRead())
        {
            c.Set(1, 31, 1, 0);
            s.outFull = true; s.outFullPos = c.outWrite();
        }

        // grava de volta so os campos que o chip altera. Os bits "valido" o chip so apaga (o jogo e quem acende):
        // o jogo pode ter acendido um enquanto decodificavamos, e ele nao pode ser perdido
        uint32_t base = CtxGuest(id);
        Ctx now; Load(id, now);
        // depuracao (FCRECOMP_XMACMD): o jogo mexeu no laco ou nas entradas enquanto o chip decodificava?
        if (static const bool cmd = getenv("FCRECOMP_XMACMD") != nullptr; cmd &&
            (now.loopCount() != before.loopCount() || now.loopEnd() != before.loopEnd() || now.ib0Valid() != before.ib0Valid() ||
             now.ib1Valid() != before.ib1Valid() || now.ib0() != before.ib0() || now.ib1() != before.ib1()))
            Log("[xmacmd] t=%.0f ctx %u: JOGO MUDOU durante a decodificacao: laco %u->%u fim %u->%u ent %d%d->%d%d in0 %08X->%08X (chip ia gravar laco %u)",
                perf::Now(), id, before.loopCount(), now.loopCount(), before.loopEnd(), now.loopEnd(), (int)before.ib0Valid(), (int)before.ib1Valid(),
                (int)now.ib0Valid(), (int)now.ib1Valid(), before.ib0(), now.ib0(), c.loopCount());
        if (!c.ib0Valid() && before.ib0Valid()) now.Set(0, 20, 1, 0);
        if (!c.ib1Valid() && before.ib1Valid()) now.Set(0, 21, 1, 0);
        now.Set(0, 27, 5, c.outWrite()); now.Set(0, 12, 8, c.loopCount());
        if (!c.outValid() && before.outValid()) now.Set(1, 31, 1, 0);
        now.Set(2, 0, 26, c.readOffset());
        now.Set(4, 31, 1, c.currentBuffer());
        for (int i : { 0, 1, 2, 4 }) W32(base + i * 4, now.d[i]);
    }

    void ClearContext(uint32_t id)
    {
        if (g_consumo) { ConsumoFim(id, g_state[id]); g_state[id].consKey = 0; }
        Ctx c; Load(id, c);
        c.Set(0, 20, 1, 0); c.Set(0, 21, 1, 0); c.Set(0, 27, 5, 0);
        c.Set(1, 31, 1, 0);
        c.Set(2, 0, 26, 0);
        c.Set(4, 31, 1, 0);
        c.Set(9, 0, 5, 0);
        for (int i : { 0, 1, 2, 4, 9 }) W32(CtxGuest(id) + i * 4, c.d[i]);
        g_state[id].pending.clear();
        g_state[id].decReady = false;
        g_state[id].lastRefBuf = 0;
        g_state[id].outFull = false;
        g_state[id].hasCarry = false;
    }

    void WorkerThread()
    {
        ULONGLONG lastLog = GetTickCount64();
        for (;;)
        {
            WaitForSingleObject(g_workEvent, 5);
            for (uint32_t id = 0; id < CONTEXT_COUNT; id++)
            {
                if (g_clearRequest[id].exchange(false)) { std::lock_guard lock(g_ctxMutex[id]); ClearContext(id); }
                // como no chip real: depois do "disparo" o contexto continua sendo atendido ate o jogo
                // travar (0x690) - o jogo consome a saida e o chip vai enchendo de novo
                if (g_allocated[id] && g_enabled[id]) Work(id);
            }
            if (g_log && GetTickCount64() - lastLog > 5000)
            {
                lastLog = GetTickCount64();
                uint64_t frames = 0, errors = 0, samples = 0; int active = 0;
                for (auto& s : g_state)
                {
                    uint64_t fe = 0, le = 0;
                    if (s.decReady) { s.dec.Errors(fe, le); active++; }
                    frames += s.frames; errors += s.errors + fe; samples += s.samples;
                }
                Log("[xma] %d contextos com decodificador, %llu quadros decodificados, %llu amostras, %llu erros", active,
                    (unsigned long long)frames, (unsigned long long)samples, (unsigned long long)errors);
                static uint64_t lastFrames[CONTEXT_COUNT];
                for (uint32_t id = 0; id < CONTEXT_COUNT; id++)
                {
                    State& s = g_state[id];
                    if (!s.decReady) continue;
                    Ctx c; Load(id, c);
                    Log("[xma]   contexto %u: %u canais %u Hz, %.1f quadros/s (o certo e %.1f), saida %u blocos le=%u esc=%u valida=%d, "
                        "quadros entre buffers: %llu juntados, %llu perdidos",
                        id, s.decChannels, s.decRate, (s.frames - lastFrames[id]) / 5.0, s.decRate / 512.0,
                        c.outBlocks(), c.outRead(), c.outWrite(), (int)c.outValid(),
                        (unsigned long long)s.carryJoined, (unsigned long long)s.carryDropped);
                    lastFrames[id] = s.frames;
                }
            }
        }
    }

    // ------------------------------------------------------------ escritas do jogo nos registradores
    thread_local uint8_t* t_pending = nullptr;
    void OnWrite(uint32_t index, uint32_t value)
    {
        static std::atomic<int> logged{ 0 };
        if (g_log && logged++ < 40) Log("[xma] jogo escreveu registrador %03X = %08X", index, value);
        auto bits = [&](uint32_t base, auto fn) {
            for (uint32_t i = 0; i < 32; i++)
                if (value & (1u << i)) { uint32_t id = (index - base) * 32 + i; if (id < CONTEXT_COUNT) fn(id); }
        };
        // depuracao (FCRECOMP_XMACMD): cada comando do jogo (liga/trava/limpa) com o contexto, a hora e os campos de laco
        if (static const bool cmd = getenv("FCRECOMP_XMACMD") != nullptr; cmd && index >= 0x650 && index <= 0x6A9)
            bits(index >= 0x6A0 ? 0x6A0 : index >= 0x690 ? 0x690 : 0x650, [&](uint32_t id) {
                Ctx c; Load(id, c);
                Log("[xmacmd] t=%.0f %s ctx %u: d0=%08X (laco %u, ent %d/%d) d1=%08X leitura %u laco %u-%u in0=%08X in1=%08X",
                    perf::Now(), index >= 0x6A0 ? "LIMPA" : index >= 0x690 ? "TRAVA" : "LIGA", id, c.d[0], c.loopCount(),
                    (int)c.ib0Valid(), (int)c.ib1Valid(), c.d[1], c.readOffset(), c.loopStart(), c.loopEnd(), c.ib0(), c.ib1());
            });
        // depuracao (FCRECOMP_XMAQUEM=endereco do buffer): quem no jogo limpa/liga o canal desse buffer (enderecos do .exe,
        // compare com o FCRecomp.map)
        if (static const char* quem = getenv("FCRECOMP_XMAQUEM"); quem && index >= 0x650 && index <= 0x6A9)
        {
            static const uint32_t alvo = (uint32_t)strtoul(quem, nullptr, 16);
            bits(index >= 0x6A0 ? 0x6A0 : index >= 0x690 ? 0x690 : 0x650, [&](uint32_t id) {
                Ctx c; Load(id, c);
                if (c.ib0() != alvo || index > 0x659 || c.readOffset() != 32) return;   // o canal (re)comecando do inicio
                void* pilha[40];
                USHORT n = RtlCaptureStackBackTrace(0, 40, pilha, nullptr);
                uintptr_t baseExe = (uintptr_t)GetModuleHandleW(nullptr);
                std::string s;
                for (USHORT i = 0; i < n; i++) { char b[24]; snprintf(b, sizeof(b), " %llX", (unsigned long long)((uintptr_t)pilha[i] - baseExe)); s += b; }
                Log("[xmaquem] t=%.0f LIGA do inicio ctx %u leitura %u:%s", perf::Now(), id, c.readOffset(), s.c_str());
            });
        }
        if (index >= 0x650 && index <= 0x659) { bits(0x650, [](uint32_t id) { g_enabled[id] = true; }); SetEvent(g_workEvent); }
        else if (index >= 0x690 && index <= 0x699)
            bits(0x690, [](uint32_t id) { std::lock_guard lock(g_ctxMutex[id]); g_enabled[id] = false; });   // espera o chip largar
        else if (index >= 0x6A0 && index <= 0x6A9) { bits(0x6A0, [](uint32_t id) { g_clearRequest[id] = true; }); SetEvent(g_workEvent); }
        // os registradores de acao nao guardam o valor (o jogo le zero de volta)
        if (index >= 0x650 && index <= 0x6A9) SetReg(index, 0);
    }

    LONG CALLBACK XmaWriteHandler(EXCEPTION_POINTERS* e)
    {
        auto* rec = e->ExceptionRecord;
        uint8_t* page = G(XMA_BASE + 0x1000);
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->ExceptionInformation[0] == 1)
        {
            uint8_t* addr = (uint8_t*)rec->ExceptionInformation[1];
            if (addr < page || addr >= page + 0x1000) return EXCEPTION_CONTINUE_SEARCH;
            DWORD old;
            VirtualProtect(page, 0x1000, PAGE_READWRITE, &old);
            t_pending = addr;
            e->ContextRecord->EFlags |= 0x100;   // executa so a instrucao que escreve
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && t_pending)
        {
            uint8_t* addr = t_pending;
            t_pending = nullptr;
            uint32_t index = (uint32_t)((addr - G(XMA_BASE)) / 4);
            OnWrite(index, Reg(index));
            DWORD old;
            VirtualProtect(page, 0x1000, PAGE_READONLY, &old);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

// Chamado na inicializacao (antes do jogo ler os registradores do audio)
void XmaInit()
{
    if (getenv("FCRECOMP_SEMXMA")) return;
    g_log = getenv("FCRECOMP_XMALOG") != nullptr;
    g_wav = getenv("FCRECOMP_XMAWAV") != nullptr;
    g_arrayGuest = GuestAlloc(CONTEXT_COUNT * 64, 256, MemKind::Physical);
    memset(G(g_arrayGuest), 0, CONTEXT_COUNT * 64);
    SetReg(0x600, g_arrayGuest & 0x1FFFFFFF);   // endereco fisico da lista de contextos
    g_workEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    AddVectoredExceptionHandler(1, XmaWriteHandler);
    DWORD old;
    VirtualProtect(G(XMA_BASE + 0x1000), 0x1000, PAGE_READONLY, &old);   // registradores 0x400-0x7FF
    std::thread(WorkerThread).detach();
    Log("[xma] chip XMA pronto: %u contextos em %08X", CONTEXT_COUNT, g_arrayGuest);
}

// XMACreateContext(contexto*): reserva um contexto livre e devolve o endereco dele
KFUNC(XMACreateContext)
{
    if (!g_arrayGuest) { W32(Gpr32(ctx, 3), GuestAlloc(0x40)); Ret(ctx, 0); return; }   // XMA desligado
    for (uint32_t id = 0; id < CONTEXT_COUNT; id++)
    {
        bool expected = false;
        if (g_allocated[id].compare_exchange_strong(expected, true))
        {
            memset(G(CtxGuest(id)), 0, 64);
            g_enabled[id] = false;
            // depuracao (FCRECOMP_XMAPILHA): quem no jogo pediu o canal (retornos da pilha, back chain do PowerPC)
            if (static const bool pilha = getenv("FCRECOMP_XMAPILHA") != nullptr; pilha)
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
                Log("[xmapilha] ctx %u criado (lr=%08X):%s", id, (uint32_t)Lr(ctx), chain.c_str());
            }
            g_clearRequest[id] = false;
            if (g_log) Log("[xma] contexto %u criado (%08X)", id, CtxGuest(id));
            W32(Gpr32(ctx, 3), CtxGuest(id));
            Ret(ctx, 0);
            return;
        }
    }
    Ret(ctx, 0xC0000017);   // STATUS_NO_MEMORY
}

// XMAReleaseContext(contexto)
KFUNC(XMAReleaseContext)
{
    uint32_t p = Gpr32(ctx, 3);
    if (g_arrayGuest && p >= g_arrayGuest && p < g_arrayGuest + CONTEXT_COUNT * 64)
    {
        uint32_t id = (p - g_arrayGuest) / 64;
        if (g_log) Log("[xma] contexto %u liberado", id);
        // limpa agora, antes de o contexto voltar a ficar livre: o jogo costuma pedir outro logo em seguida e recebe
        // este mesmo; uma limpeza deixada para o chip fazer depois poderia apagar o som novo antes de ele tocar
        {
            std::lock_guard lock(g_ctxMutex[id]);
            g_enabled[id] = false;
            ClearContext(id);
            g_clearRequest[id] = false;
        }
        g_allocated[id] = false;
    }
    Ret(ctx, 0);
}

// FCRECOMP_XMATEST=arquivo (salvo com FCRECOMP_XMADUMP=1): decodifica os quadros salvos com o decodificador WMA Pro do
// projeto, escreve xmateste.wav e mostra estatisticas (para conferir se e audio de verdade e nao ruido)
// Repete uma gravacao de FCRECOMP_XMAREF com o decodificador do projeto e compara amostra a amostra com o som gravado.
static int CompareWithReference(FILE* f)
{
    std::map<uint32_t, std::unique_ptr<XmaAudioDecoder>> decs;
    std::map<uint32_t, int> worst;   // maior diferenca por contexto
    std::vector<uint8_t> data;
    std::vector<int16_t> ref, got(1024);
    uint64_t frames = 0, countDiff = 0, samples = 0, differ = 0, off1 = 0, oldErrors = 0, oldLength = 0;
    int maxDiff = 0, shown = 0;
    double sq = 0;
    for (uint32_t kind; fread(&kind, 4, 1, f) == 1;)
    {
        uint32_t id = 0;
        fread(&id, 4, 1, f);
        if (kind == 'I')
        {
            uint32_t ch = 0, sr = 0;
            fread(&ch, 4, 1, f); fread(&sr, 4, 1, f);
            auto& d = decs[id];
            if (d) { uint64_t a = 0, b = 0; d->Errors(a, b); oldErrors += a; oldLength += b; }
            d = std::make_unique<XmaAudioDecoder>();
            if (!d->Init((int)ch, (int)sr)) Log("[xmaref] contexto %u: Init falhou (%u canais, %u Hz)", id, ch, sr);
            continue;
        }
        if (kind == 'B') { uint32_t skip[3]; fread(skip, 4, 3, f); continue; }   // inicio de um buffer de entrada
        if (kind != 'Q') { Log("[xmaref] arquivo corrompido"); break; }
        uint32_t bits = 0, n = 0;
        fread(&bits, 4, 1, f);
        data.assign((bits + 7) / 8 + 8, 0);
        fread(data.data(), 1, (bits + 7) / 8, f);
        fread(&n, 4, 1, f);
        ref.resize(n);
        if (fread(ref.data(), 2, n, f) != n) break;
        auto it = decs.find(id);
        if (it == decs.end()) continue;
        int k = it->second->DecodeFrame(data.data(), bits, got.data()) * it->second->Channels();
        frames++;
        if ((uint32_t)k != n) { countDiff++; continue; }
        int frameMax = 0;
        for (uint32_t i = 0; i < n; i++)
        {
            int dd = std::abs(got[i] - ref[i]);
            sq += (double)dd * dd;
            if (dd) { differ++; off1 += dd == 1; }
            frameMax = std::max(frameMax, dd);
        }
        samples += n;
        maxDiff = std::max(maxDiff, frameMax);
        worst[id] = std::max(worst[id], frameMax);
        if (frameMax > 2 && shown++ < 10) Log("[xmaref]   contexto %u, quadro %llu: diferenca maxima %d", id, (unsigned long long)frames, frameMax);
    }
    std::string per;
    for (auto& [id, w] : worst) { char t[24]; snprintf(t, sizeof(t), " %u:%d", id, w); per += t; }
    Log("[xmaref] %llu quadros, %llu com quantidade de amostras diferente; %llu amostras: %llu diferentes (%llu por 1), "
        "diferenca maxima %d, media quadratica %.4f", (unsigned long long)frames, (unsigned long long)countDiff,
        (unsigned long long)samples, (unsigned long long)differ, (unsigned long long)off1, maxDiff, samples ? std::sqrt(sq / samples) : 0.0);
    Log("[xmaref] maior diferenca por contexto:%s", per.c_str());
    uint64_t fe = oldErrors, le = oldLength;
    for (auto& [id, d] : decs) { uint64_t a = 0, b = 0; d->Errors(a, b); fe += a; le += b; }
    Log("[xmaref] quadros corrompidos: %llu, com tamanho que nao bate: %llu", (unsigned long long)fe, (unsigned long long)le);
    return countDiff || maxDiff > 2 ? 2 : 0;
}

int RunXmaTest(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) { Log("[xmateste] nao abriu %s", path); return 1; }
    uint32_t ch = 0, sr = 0;
    fread(&ch, 4, 1, f);
    if (ch == 'I') { fseek(f, 0, SEEK_SET); int r = CompareWithReference(f); fclose(f); return r; }
    fread(&sr, 4, 1, f);
    if (const char* c = getenv("FCRECOMP_XMACANAIS")) ch = (uint32_t)atoi(c);   // depuracao: forca mono/estereo
    if (const char* r = getenv("FCRECOMP_XMATAXA")) sr = (uint32_t)atoi(r);
    std::vector<std::vector<uint8_t>> all; std::vector<uint32_t> lens;
    for (uint32_t len; fread(&len, 4, 1, f) == 1;)
    {
        std::vector<uint8_t> b((len + 7) / 8 + 8, 0);
        if (fread(b.data(), 1, (len + 7) / 8, f) != (len + 7) / 8) break;
        all.push_back(std::move(b)); lens.push_back(len);
    }
    fclose(f);
    if (const char* fl = getenv("FCRECOMP_XMAFLAGS"))   // varre opcoes: "10D6,1056,..."
    {
        for (const char* e = fl; *e; e = strchr(e, ',') ? strchr(e, ',') + 1 : "")
        {
            uint32_t flags = strtoul(e, nullptr, 16);
            XmaAudioDecoder t;
            if (!t.Init((int)ch, (int)sr, flags)) { Log("[xmateste] opcoes %04X: Init falhou", flags); continue; }
            std::vector<int16_t> fr(512 * ch);
            std::string diffs;
            for (size_t k = 0; k < all.size(); k++)
            {
                t.DecodeFrame(all[k].data(), lens[k], fr.data());
                if (k < 20) { char db[16]; snprintf(db, sizeof(db), " %d", t.LastLenDiff()); diffs += db; }
            }
            Log("[xmateste]   opcoes %04X, sobras dos primeiros quadros:%s", flags, diffs.c_str());
            uint64_t fe = 0, le = 0; t.Errors(fe, le);
            Log("[xmateste] opcoes %04X: %llu quadros com erro, %llu com tamanho errado (de %zu)", flags, (unsigned long long)fe, (unsigned long long)le, all.size());
        }
    }
    XmaAudioDecoder dec;
    if (!dec.Init((int)ch, (int)sr)) { Log("[xmateste] Init falhou"); return 1; }
    std::vector<int16_t> pcm, frame(512 * ch);
    uint32_t frames = 0;
    for (size_t k = 0; k < all.size(); k++)
    {
        const std::vector<uint8_t>& b = all[k]; uint32_t len = lens[k];
        int got = dec.DecodeFrame(b.data(), len, frame.data());
        int failBit = 0, fail = dec.LastFail(failBit);
        if (fail && frames < 12) Log("[xmateste]   quadro %u (%u bits): erro no ponto %d, bit %d", frames, len, fail, failBit);
        pcm.insert(pcm.end(), frame.begin(), frame.begin() + got * ch);
        frames++;
    }
    uint64_t fe = 0, le = 0;
    dec.Errors(fe, le);
    double peak = 0, sum = 0, diff = 0;
    for (size_t i = 0; i < pcm.size(); i++)
    {
        double x = pcm[i] / 32768.0;
        peak = std::max(peak, std::abs(x)); sum += x * x;
        if (i >= ch) { double dd = x - pcm[i - ch] / 32768.0; diff += dd * dd; }
    }
    double rms = pcm.empty() ? 0 : std::sqrt(sum / pcm.size());
    Log("[xmateste] %u quadros (%u canais, %u Hz): %zu amostras, %llu quadros com erro, %llu com tamanho errado, pico %.3f, RMS %.4f, "
        "aspereza %.3f (perto de 0 = som liso; ~1,4 = ruido branco)", frames, ch, sr, pcm.size() / ch, (unsigned long long)fe,
        (unsigned long long)le, peak, rms, sum > 0 ? std::sqrt(diff / sum) : 0.0);
    if (FILE* w = fopen("xmateste.wav", "wb"))
    {
        uint32_t bytes = (uint32_t)pcm.size() * 2;
        uint32_t hdr[11] = { 0x46464952, 36 + bytes, 0x45564157, 0x20746D66, 16, 1u | (ch << 16), sr, sr * ch * 2, (ch * 2) | (16u << 16), 0x61746164, bytes };
        fwrite(hdr, 4, 11, w); fwrite(pcm.data(), 2, pcm.size(), w); fclose(w);
    }
    return 0;
}
