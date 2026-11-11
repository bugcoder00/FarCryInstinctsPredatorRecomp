// Decodificador dos quadros de audio do XMA (formato WMA Pro, 512 amostras por canal por quadro).
//
// Conteudo de um quadro, na ordem dos bits:
//  1. tamanho do quadro (15 bits);
//  2. divisao do tempo: cada canal e dividido em blocos de 512, 256 ou 128 amostras (ReadLayout);
//  3. opcoes do quadro (mistura de canais, ganho de faixa dinamica, corte do inicio/fim) que nao mudam o som aqui;
//  4. os blocos em ordem de tempo (DecodeBlock): modo estereo, passo de quantizacao, fatores de escala por banda e os
//     coeficientes do espectro (codigos de Huffman, de 4 em 4 no comeco e depois em pares "zeros + nivel").
// Cada bloco de N coeficientes vira 2N amostras por uma MDCT inversa; a primeira metade cruza com a segunda metade do
// bloco anterior numa janela seno do tamanho do menor dos dois (Overlap). O quadro sai com 256 amostras de atraso.
#define _USE_MATH_DEFINES
#include "xma_audio.h"
#include "xma_audio_tables.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
    constexpr int kFrame = 512;        // amostras por canal num quadro
    constexpr int kMaxChannels = 2;
    constexpr int kMaxBlocks = 32;     // blocos por canal num quadro
    constexpr int kMaxBands = 30;
    constexpr int kSizes = 4;          // tamanhos de bloco possiveis: 512 >> 0..3
    constexpr int kDelay = kFrame / 2; // atraso da sobreposicao

    int Log2(uint32_t v) { int r = 0; while (v >>= 1) r++; return r; }

    // ------------------------------------------------------------ leitura de bits (o mais alto primeiro)
    class BitReader
    {
    public:
        void Start(const uint8_t* p, int bits) { p_ = p; end_ = bits; pos_ = 0; }
        uint32_t Bit()   // depois do fim le zeros (e continua contando)
        {
            uint32_t b = pos_ < end_ ? (p_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1 : 0;
            pos_++;
            return b;
        }
        uint32_t Bits(int n) { uint32_t v = 0; while (n-- > 0) v = (v << 1) | Bit(); return v; }
        int32_t Signed(int n) { uint32_t v = Bits(n); return (v & (1u << (n - 1))) ? (int32_t)(v | (~0u << n)) : (int32_t)v; }
        void Skip(int n) { pos_ += n; }
        int Pos() const { return pos_; }
        int End() const { return end_; }
    private:
        const uint8_t* p_ = nullptr;
        int end_ = 0, pos_ = 0;
    };

    // numero de tamanho variavel: 8, 16, 24 ou 31 bits, conforme os bits de extensao
    uint32_t LongValue(BitReader& br)
    {
        static const int widths[4] = { 8, 16, 24, 31 };
        int i = 0;
        while (i < 3 && br.Bit()) i++;
        return br.Bits(widths[i]);
    }

    // ------------------------------------------------------------ livro de codigos (arvore binaria)
    class Codebook
    {
    public:
        template <size_t N> void Build(const xma_tables::Code (&codes)[N])
        {
            entries_ = codes;
            nodes_.assign(1, { 0, 0 });
            for (size_t i = 0; i < N; i++)
            {
                int node = 0;
                for (int b = codes[i].bits - 1; b >= 0; b--)
                {
                    int dir = (codes[i].code >> b) & 1;
                    if (b == 0) { nodes_[node][dir] = -(int)i - 1; break; }
                    if (nodes_[node][dir] == 0) { nodes_[node][dir] = (int)nodes_.size(); nodes_.push_back({ 0, 0 }); }
                    node = nodes_[node][dir];
                }
                maxBits_ = std::max<int>(maxBits_, codes[i].bits);
            }
        }
        // nullptr = codigo que nao existe (consome bits ate o tamanho do maior codigo)
        const xma_tables::Code* Read(BitReader& br) const
        {
            int node = 0;
            for (int depth = 1; depth <= maxBits_; depth++)
            {
                int next = nodes_[node][br.Bit()];
                if (next < 0) return &entries_[-next - 1];
                if (next == 0) { br.Skip(maxBits_ - depth); return nullptr; }
                node = next;
            }
            return nullptr;
        }
    private:
        const xma_tables::Code* entries_ = nullptr;
        std::vector<std::array<int, 2>> nodes_;
        int maxBits_ = 0;
    };

    Codebook g_scaleDelta, g_scaleRunLevel, g_coefRunLevel[2], g_quad, g_pair, g_single;
    std::vector<float> g_rise[kSizes];   // meia janela seno subindo, para cruzamentos de 512 >> i amostras
    std::once_flag g_once;

    void BuildShared()
    {
        g_scaleDelta.Build(xma_tables::kScaleDelta);
        g_scaleRunLevel.Build(xma_tables::kScaleRunLevel);
        g_coefRunLevel[0].Build(xma_tables::kCoefRunLevel0);
        g_coefRunLevel[1].Build(xma_tables::kCoefRunLevel1);
        g_quad.Build(xma_tables::kQuad);
        g_pair.Build(xma_tables::kPair);
        g_single.Build(xma_tables::kSingle);
        for (int i = 0; i < kSizes; i++)
        {
            int n = kFrame >> i;
            g_rise[i].resize(n);
            for (int k = 0; k < n; k++) g_rise[i][k] = (float)std::sin((k + 0.5) * (M_PI / (2.0 * n)));
        }
    }

    // ------------------------------------------------------------ MDCT inversa
    // N coeficientes -> 2N amostras: y[j] = escala * soma_k X[k] cos(pi/N (j + 1/2 + N/2)(k + 1/2)).
    // O miolo y[N/2 .. 3N/2) e uma DCT-IV (com sinal trocado e invertida); a DCT-IV de N pontos sai de uma FFT
    // complexa de N/2 pontos (pares em cima, impares de tras para frente em baixo, giro antes e depois). As pontas
    // vem das simetrias da MDCT: y[j] = -y[N-1-j] na primeira metade e y[j] = y[3N-1-j] na segunda.
    class InverseMdct
    {
    public:
        void Prepare(int n)
        {
            n_ = n;
            const int m = n / 2;
            before_.resize(m); after_.resize(m); spin_.resize(m / 2); slot_.resize(m); work_.resize(m); dct_.resize(n);
            for (int k = 0; k < m; k++)
            {
                before_[k] = std::polar(1.0, -M_PI * (4 * k + 1) / (4.0 * n));
                after_[k] = std::polar(1.0, -M_PI * k / n);
            }
            for (int k = 0; k < m / 2; k++) spin_[k] = std::polar(1.0, -2.0 * M_PI * k / m);
            const int bits = Log2(m);
            for (int k = 0; k < m; k++)
            {
                int r = 0;
                for (int b = 0; b < bits; b++) r = (r << 1) | ((k >> b) & 1);
                slot_[k] = r;
            }
        }
        void Run(const float* in, float* out, double scale)
        {
            const int n = n_, m = n / 2;
            for (int k = 0; k < m; k++) work_[slot_[k]] = std::complex<double>(in[2 * k], in[n - 1 - 2 * k]) * before_[k];
            for (int span = 2; span <= m; span *= 2)
            {
                const int half = span / 2, stride = m / span;
                for (int base = 0; base < m; base += span)
                    for (int k = 0; k < half; k++)
                    {
                        std::complex<double>& a = work_[base + k];
                        std::complex<double>& b = work_[base + k + half];
                        const std::complex<double> t = b * spin_[k * stride];
                        b = a - t;
                        a += t;
                    }
            }
            for (int k = 0; k < m; k++)
            {
                const std::complex<double> c = work_[k] * after_[k];
                dct_[2 * k] = c.real();
                dct_[n - 1 - 2 * k] = -c.imag();
            }
            for (int j = n / 2; j < 3 * n / 2; j++) out[j] = (float)(-scale * dct_[3 * n / 2 - 1 - j]);
            for (int j = 0; j < n / 2; j++) out[j] = -out[n - 1 - j];
            for (int j = 3 * n / 2; j < 2 * n; j++) out[j] = out[3 * n - 1 - j];
        }
    private:
        int n_ = 0;
        std::vector<std::complex<double>> before_, after_, spin_, work_;
        std::vector<int> slot_;
        std::vector<double> dct_;
    };
}

struct XmaAudioDecoder::State
{
    struct Channel
    {
        // divisao do quadro atual
        int blocks = 0, blockLen[kMaxBlocks] = {};
        int next = 0, done = 0;   // proximo bloco e amostras ja decodificadas
        // bloco atual
        bool coded = false;
        int quant = 0, vecCount = 0;
        // fatores de escala: dois conjuntos (atual e anterior, que pode ser reaproveitado em outro tamanho de bloco)
        int scale[2][kMaxBands] = {};
        int scaleCur = 0, scaleTable = 0, scaleStep = 0, scaleMax = 0;
        int* scaleNow = nullptr;
        bool scaleHave = false;
        float spec[kFrame] = {};
        float time[2 * kFrame] = {};
        // som: amostras prontas (com o atraso de kDelay) e a segunda metade do ultimo bloco, que espera o proximo
        float pcm[kFrame + kDelay] = {};
        float tail[kFrame] = {};
        int tailLen = kFrame;
    };

    // configuracao
    int channels = 0, rate = 0;
    uint32_t flags = 0;
    bool lenPrefix = false, drc = false, lenEscape = false;
    int maxBlocks = 1, minBlock = kFrame, lenBits = 1, sizes = 1;
    int bandCount[kSizes] = {};
    int bandEdge[kSizes][kMaxBands + 1] = {};
    uint8_t bandMap[kSizes][kSizes][kMaxBands] = {};   // banda de um tamanho de bloco -> banda de outro tamanho
    InverseMdct mdct[kSizes];

    // quadro / bloco atual
    BitReader br;
    bool primed = false;   // o primeiro quadro so prepara a sobreposicao
    int blockLen = 0, table = 0, escBits = 0;
    int members[kMaxChannels] = {}, memberCount = 0;
    bool explicitVec = false, mix = false;
    uint8_t mixBand[kMaxBands] = {};
    float scaled[kFrame] = {};
    Channel ch[kMaxChannels];

    uint64_t frameErrors = 0, lengthErrors = 0;
    int lastFail = 0, lastFailBit = 0, lastLenDiff = 0;

    bool Fail(int stage) { if (!lastFail) { lastFail = stage; lastFailBit = br.Pos(); } return false; }

    bool Init(int numChannels, int sampleRate, uint32_t decodeFlags)
    {
        std::call_once(g_once, BuildShared);
        channels = numChannels; rate = sampleRate; flags = decodeFlags;
        if (channels < 1 || channels > kMaxChannels) return false;
        lenPrefix = (flags & 0x40) != 0;
        drc = (flags & 0x80) != 0;
        const int lg = (flags >> 3) & 7;
        maxBlocks = 1 << lg;
        minBlock = kFrame / maxBlocks;
        if (maxBlocks > kMaxBlocks || lg >= kSizes) return false;
        lenEscape = maxBlocks == 4 || maxBlocks == 16;
        lenBits = Log2(lg) + 1;
        sizes = lg + 1;
        primed = false;
        for (Channel& c : ch) c = Channel{};

        // bandas de fator de escala: limites das bandas criticas convertidos para coeficientes, multiplos de 4
        const int nominal = rate > 44100 ? 48000 : rate > 32000 ? 44100 : rate > 24000 ? 32000 : 24000;
        for (int t = 0; t < sizes; t++)
        {
            const int n = kFrame >> t;
            int* edge = bandEdge[t];
            int count = 0;
            edge[0] = 0;
            for (int hz : xma_tables::kBandEdgesHz)
            {
                if (edge[count] >= n || count >= kMaxBands - 2) break;
                const int at = ((n * 2 * hz) / nominal + 2) & ~3;
                if (at > edge[count]) edge[++count] = at;
                if (at >= n) break;
            }
            edge[count] = n;
            bandCount[t] = count;
            if (count <= 0) return false;
        }
        // para reaproveitar fatores de escala entre tamanhos: a banda do outro tamanho que contem o meio desta
        for (int t = 0; t < sizes; t++)
            for (int b = 0; b < bandCount[t]; b++)
            {
                const int middle = ((bandEdge[t][b] + bandEdge[t][b + 1] - 1) << t) >> 1;
                for (int u = 0; u < sizes; u++)
                {
                    int v = 0;
                    while (v < kMaxBands - 2 && (bandEdge[u][v + 1] << u) < middle) v++;
                    bandMap[t][u][b] = (uint8_t)v;
                }
            }
        for (int t = 0; t < sizes; t++) mdct[t].Prepare(kFrame >> t);
        return true;
    }

    int ReadBlockLength(int at)
    {
        if (at == kFrame - minBlock) return minBlock;   // so cabe o menor
        if (br.Pos() >= br.End()) return 0;
        int shift;
        if (lenEscape) shift = br.Bit() ? 1 + (int)br.Bits(lenBits - 1) : 0;
        else shift = (int)br.Bits(lenBits);
        const int len = kFrame >> shift;
        return len >= minBlock && len <= kFrame ? len : 0;
    }

    // divide o quadro em blocos: a cada passo, os canais no ponto mais atrasado recebem (ou nao) um bloco novo
    bool ReadLayout()
    {
        int pos[kMaxChannels] = {};
        for (int c = 0; c < channels; c++) ch[c].blocks = 0;
        const bool shared = maxBlocks == 1 || br.Bit();   // todos os canais com a mesma divisao
        for (;;)
        {
            int at = kFrame, waiting = 0;
            for (int c = 0; c < channels; c++) at = std::min(at, pos[c]);
            if (at >= kFrame) return true;
            for (int c = 0; c < channels; c++) waiting += pos[c] == at;
            bool gets[kMaxChannels] = {};
            for (int c = 0; c < channels; c++)
                gets[c] = pos[c] == at && (shared || waiting == 1 || at == kFrame - minBlock || br.Bit());
            const int len = ReadBlockLength(at);
            if (!len) return Fail(1);
            for (int c = 0; c < channels; c++)
            {
                if (!gets[c]) continue;
                if (ch[c].blocks >= kMaxBlocks) return Fail(2);
                ch[c].blockLen[ch[c].blocks++] = len;
                if ((pos[c] += len) > kFrame) return Fail(3);
            }
            if (br.Pos() > br.End() + 64) return Fail(4);   // lendo alem do fim sem fechar a divisao
        }
    }

    bool ReadStereo()
    {
        mix = false;
        if (channels < 2) return true;
        if (br.Bit()) return Fail(10);   // transformacao propria por canal: nao existe no XMA
        if (memberCount < 2) return true;
        if (br.Bit()) { if (br.Bit()) return Fail(11); }   // canais independentes
        else mix = true;                                     // meio/lado
        if (mix)
        {
            if (br.Bit()) memset(mixBand, 1, sizeof(mixBand));
            else for (int b = 0; b < bandCount[table]; b++) mixBand[b] = (uint8_t)br.Bit();
        }
        return true;
    }

    bool ReadQuant()
    {
        int q = 90;   // passo base para amostras de 16 bits
        explicitVec = br.Bit() != 0;
        for (int i = 0; i < memberCount; i++)
        {
            Channel& c = ch[members[i]];
            c.vecCount = blockLen;
            if (explicitVec)
            {
                c.vecCount = (int)br.Bits(Log2((blockLen + 3) / 4) + 1) << 2;
                if (c.vecCount > blockLen) return Fail(20);
            }
        }
        const int step = br.Signed(6);
        q += step;
        if (step == 31 || step == -32)   // valor extremo: continua em grupos de 5 bits
        {
            int extra = 0, v = step;
            while (br.Pos() + 5 < br.End() && (v = (int)br.Bits(5)) == 31) extra += 31;
            q += step == 31 ? extra + v : -(extra + v);
        }
        if (memberCount == 1) { ch[members[0]].quant = q; return true; }
        const int adjustBits = (int)br.Bits(3);
        for (int i = 0; i < memberCount; i++)
        {
            Channel& c = ch[members[i]];
            c.quant = q;
            if (br.Bit()) c.quant += adjustBits ? (int)br.Bits(adjustBits) + 1 : 1;
        }
        return true;
    }

    bool ReadScaleFactors()
    {
        const int bands = bandCount[table];
        for (int i = 0; i < memberCount; i++)
        {
            Channel& c = ch[members[i]];
            int* sf = c.scale[!c.scaleCur];
            c.scaleNow = sf;
            if (c.scaleHave)   // parte dos fatores anteriores (convertidos para o tamanho de bloco atual)
                for (int b = 0; b < bands; b++) sf[b] = c.scale[c.scaleCur][bandMap[table][c.scaleTable][b]];
            if (c.next == 0 || br.Bit())
            {
                if (!c.scaleHave)   // valores novos: diferencas em relacao a banda anterior
                {
                    c.scaleStep = (int)br.Bits(2) + 1;
                    int v = 45 / c.scaleStep;
                    for (int b = 0; b < bands; b++)
                    {
                        const xma_tables::Code* e = g_scaleDelta.Read(br);
                        sf[b] = v += e ? e->a : -1;
                    }
                }
                else   // ajustes esparsos: (bandas puladas, ajuste)
                {
                    for (int b = 0; b < bands; b++)
                    {
                        const xma_tables::Code* e = g_scaleRunLevel.Read(br);
                        if (!e) return Fail(30);
                        int skip, amount;
                        if (e->a == -2) break;
                        if (e->a == -1)
                        {
                            const uint32_t raw = br.Bits(14);
                            amount = (int)(raw >> 6);
                            skip = (int)((raw >> 1) & 31);
                            if (!(raw & 1)) amount = -amount;
                        }
                        else
                        {
                            skip = e->a;
                            amount = e->b;
                            if (!br.Bit()) amount = -amount;
                        }
                        if ((b += skip) >= bands) return Fail(31);
                        sf[b] += amount;
                    }
                }
                c.scaleCur = !c.scaleCur;
                c.scaleTable = table;
                c.scaleHave = true;
            }
            c.scaleMax = *std::max_element(sf, sf + bands);
        }
        return true;
    }

    // coeficientes em pares (zeros antes, nivel) a partir de 'from'; erro so encerra a leitura
    void ReadRunLevel(float* x, const Codebook& book, int from)
    {
        const int mask = blockLen - 1;
        for (int k = from; k < blockLen; k++)
        {
            const xma_tables::Code* e = book.Read(br);
            if (e && e->a >= 0)
            {
                k += e->a;
                x[k & mask] = br.Bit() ? (float)e->b : -(float)e->b;
                continue;
            }
            if (e && e->a == -2) return;   // fim do bloco
            const int level = (int)LongValue(br);
            if (br.Bit())
            {
                if (!br.Bit()) k += (int)br.Bits(2) + 1;
                else if (br.Bit()) return;
                else k += (int)br.Bits(escBits) + 4;
            }
            x[k & mask] = (float)(br.Bit() ? level : -level);
        }
    }

    // coeficientes do espectro: primeiro de 4 em 4 valores; depois de muitos zeros seguidos, em pares (zeros, nivel)
    void ReadSpectrum(Channel& c)
    {
        const Codebook& book = g_coefRunLevel[br.Bit()];
        float* x = c.spec;
        bool sparse = false;
        int i = 0, zeros = 0;
        while ((explicitVec || !sparse) && i + 3 < c.vecCount)
        {
            uint32_t v[4];
            const xma_tables::Code* e = g_quad.Read(br);
            if (e && e->a >= 0)
                for (int k = 0; k < 4; k++) v[k] = (e->a >> (12 - 4 * k)) & 15;
            else
                for (int k = 0; k < 4; k += 2)
                {
                    const xma_tables::Code* p = g_pair.Read(br);
                    if (p && p->a >= 0) { v[k] = p->a >> 4; v[k + 1] = p->a & 15; continue; }
                    for (int j = k; j < k + 2; j++)
                    {
                        const xma_tables::Code* s1 = g_single.Read(br);
                        v[j] = s1 ? (uint32_t)s1->a : ~0u;
                        if (v[j] == (uint32_t)xma_tables::kSingleEscape) v[j] += LongValue(br);
                    }
                }
            for (int k = 0; k < 4; k++, i++)
            {
                if (v[k]) { x[i] = br.Bit() ? (float)v[k] : -(float)v[k]; zeros = 0; }
                else { x[i] = 0; if (++zeros > (blockLen >> 8)) sparse = true; }
            }
        }
        if (i < blockLen)
        {
            std::fill(x + i, x + blockLen, 0.0f);
            ReadRunLevel(x, book, i);
        }
    }

    // cruza a primeira metade do bloco novo com a segunda metade do anterior e guarda a segunda metade do novo
    void Overlap(Channel& c, int at)
    {
        const int n = blockLen, edge = at + kDelay;   // edge = fronteira entre o bloco anterior e este
        const int w = std::min(c.tailLen, n), h = w / 2;
        const float* rise = g_rise[Log2(kFrame / w)].data();
        const int tailStart = edge - c.tailLen / 2, newStart = edge - n / 2;
        for (int p = tailStart; p < edge - h; p++) c.pcm[p] = c.tail[p - tailStart];
        for (int k = 0; k < w; k++)
        {
            const int p = edge - h + k;
            c.pcm[p] = c.tail[p - tailStart] * rise[w - 1 - k] + c.time[p - newStart] * rise[k];
        }
        for (int p = edge + h; p < edge + n / 2; p++) c.pcm[p] = c.time[p - newStart];
        memcpy(c.tail, c.time + n, sizeof(float) * n);
        c.tailLen = n;
    }

    bool DecodeBlock()
    {
        // o bloco mais atrasado no tempo; vao juntos os canais com um bloco igual no mesmo ponto
        int at = kFrame;
        for (int c = 0; c < channels; c++)
            if (ch[c].done < at) { at = ch[c].done; blockLen = ch[c].blockLen[ch[c].next]; }
        memberCount = 0;
        for (int c = 0; c < channels; c++)
            if (ch[c].done == at && ch[c].blockLen[ch[c].next] == blockLen) { members[memberCount++] = c; ch[c].done += blockLen; }
        table = Log2(kFrame / blockLen);
        escBits = Log2(blockLen - 1) + 1;
        const int bands = bandCount[table];
        const int* edge = bandEdge[table];

        if (br.Bit())   // bits de preenchimento
        {
            int fill = (int)br.Bits(2);
            if (!fill) { const int w = (int)br.Bits(4); fill = (w ? (int)br.Bits(w) : 0) + 1; }
            if (br.Pos() + fill > br.End()) return Fail(40);
            br.Skip(fill);
        }
        if (br.Bit()) return Fail(41);   // reservado
        if (!ReadStereo()) return false;

        bool anyCoded = false;
        for (int i = 0; i < memberCount; i++) anyCoded |= (ch[members[i]].coded = br.Bit() != 0);
        if (anyCoded && (!ReadQuant() || !ReadScaleFactors())) return false;
        for (int i = 0; i < memberCount; i++)
        {
            Channel& c = ch[members[i]];
            if (c.coded && br.Pos() < br.End()) ReadSpectrum(c);
            else std::fill(c.spec, c.spec + blockLen, 0.0f);
        }

        if (anyCoded)
        {
            if (mix)   // meio/lado -> esquerdo/direito; bandas sem mistura so recebem o ganho que a mistura daria
            {
                float* a = ch[members[0]].spec;
                float* b = ch[members[1]].spec;
                for (int band = 0; band < bands; band++)
                    for (int k = edge[band]; k < std::min(edge[band + 1], blockLen); k++)
                    {
                        if (mixBand[band]) { const float m = a[k], s = b[k]; a[k] = m - s; b[k] = m + s; }
                        else { a[k] *= 181.0f / 128; b[k] *= 181.0f / 128; }
                    }
            }
            for (int i = 0; i < memberCount; i++)
            {
                Channel& c = ch[members[i]];
                for (int band = 0; band < bands; band++)
                {
                    const int db = c.quant - (c.scaleMax - c.scaleNow[band]) * c.scaleStep;
                    const float gain = (float)std::pow(10.0, db / 20.0);
                    for (int k = edge[band]; k < std::min(edge[band + 1], blockLen); k++) scaled[k] = c.spec[k] * gain;
                }
                mdct[table].Run(scaled, c.time, 2.0 / blockLen / 32768.0);
            }
        }
        else
            for (int i = 0; i < memberCount; i++) std::fill(ch[members[i]].time, ch[members[i]].time + 2 * blockLen, 0.0f);

        for (int i = 0; i < memberCount; i++)
        {
            Channel& c = ch[members[i]];
            Overlap(c, at);
            if (c.next >= c.blocks) return Fail(42);
            c.next++;
        }
        return true;
    }

    int Lost(int16_t* out)   // quadro corrompido: silencio e a sobreposicao recomeca do zero
    {
        frameErrors++;
        for (int c = 0; c < channels; c++)
        {
            std::fill(std::begin(ch[c].pcm), std::end(ch[c].pcm), 0.0f);
            std::fill(std::begin(ch[c].tail), std::end(ch[c].tail), 0.0f);
            ch[c].tailLen = kFrame;
        }
        memset(out, 0, sizeof(int16_t) * kFrame * channels);
        return kFrame;
    }

    int DecodeFrame(const uint8_t* data, uint32_t bits, int16_t* out)
    {
        br.Start(data, (int)bits);
        const int declared = lenPrefix ? (int)br.Bits(15) : 0;
        if (!ReadLayout()) return Lost(out);
        if (channels > 1 && br.Bit() && br.Bit()) br.Skip(4 * channels * channels);   // matriz de mistura (nao usada)
        if (drc) br.Skip(8);                                                           // ganho de faixa dinamica
        if (br.Bit())                                                                  // corte do inicio/fim
        {
            if (br.Bit()) br.Skip(Log2(2 * kFrame));
            if (br.Bit()) br.Skip(Log2(2 * kFrame));
        }
        for (int c = 0; c < channels; c++) { ch[c].done = 0; ch[c].next = 0; ch[c].scaleHave = false; }
        for (;;)
        {
            bool left = false;
            for (int c = 0; c < channels; c++) left |= ch[c].done < kFrame;
            if (!left) break;
            if (!DecodeBlock()) return Lost(out);
        }

        int produced = 0;
        if (primed)
        {
            for (int s = 0; s < kFrame; s++)
                for (int c = 0; c < channels; c++)
                    out[s * channels + c] = (int16_t)std::lrintf(std::clamp(ch[c].pcm[s], -1.0f, 1.0f) * 32767.0f);
            produced = kFrame;
        }
        primed = true;
        for (int c = 0; c < channels; c++) memmove(ch[c].pcm, ch[c].pcm + kFrame, sizeof(float) * kDelay);

        lastLenDiff = lenPrefix ? declared - br.Pos() - 2 : 0;
        if (lastLenDiff) lengthErrors++;
        return produced;
    }
};

XmaAudioDecoder::XmaAudioDecoder() : s(std::make_unique<State>()) {}
XmaAudioDecoder::~XmaAudioDecoder() = default;
bool XmaAudioDecoder::Init(int channels, int sampleRate, uint32_t flags) { return s->Init(channels, sampleRate, flags); }
int XmaAudioDecoder::DecodeFrame(const uint8_t* data, uint32_t bits, int16_t* out) { return s->DecodeFrame(data, bits, out); }
int XmaAudioDecoder::Channels() const { return s->channels; }
void XmaAudioDecoder::Reset() { s->Init(s->channels, s->rate, s->flags); }
void XmaAudioDecoder::Errors(uint64_t& frames, uint64_t& lengths) const { frames = s->frameErrors; lengths = s->lengthErrors; }
int XmaAudioDecoder::LastFail(int& bit) const { bit = s->lastFailBit; int f = s->lastFail; s->lastFail = 0; return f; }
int XmaAudioDecoder::LastLenDiff() const { return s->lastLenDiff; }
