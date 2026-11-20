// Renderizador DirectX 12: dispositivo, swapchain, quadros, memoria de upload, descritores e DXC
#include "opcoes.h"
#include "perf.h"
#include "render.h"
#include "guest.h"
#include "diag.h"
#include "idioma.h"
#include <dxcapi.h>
std::string AoVivoValor(const char* chave);   // input.cpp: build_game\ao_vivo.txt (testes com o jogo aberto)
#include <mutex>
#include <cmath>
#include <algorithm>

namespace xdraw { void PedirNovaEscala(); }   // xenos_draw.cpp: resolucao interna (com FSR depende do tamanho da janela)

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

void (*g_onGpuHang)(uint32_t) = nullptr;   // xenos_draw mostra o desenho que travou
void AvisarPlacaTravou();   // gpu.cpp: mensagem na janela e fecha o jogo

// brilho da imagem final (1 = normal): pular_cena.cpp escurece a tela enquanto uma cena e pulada
#ifdef PREDATOR_EDITOR
static float BrilhoTela() { return 1.0f; }
#else
float BrilhoTela();
#endif

namespace render
{
    namespace
    {
        constexpr UINT FRAMES = 2;
        constexpr UINT64 UPLOAD_SIZE = 96ull << 20;   // por quadro
        // 65 descritores por desenho; a missao passa de 1700 desenhos por quadro (~110 mil). Com 60000 a contagem
        // dava a volta e sobrescrevia os descritores dos primeiros desenhos do quadro (a faixa esquerda do
        // predicated tiling): vertices e texturas errados -> metade esquerda rosa, estilhacada e piscando.
        // 2 x 480000 fica abaixo do limite de 1 milhao do heap visivel aos shaders (Tier 1).
        constexpr UINT SRV_PER_FRAME = 480000;
        constexpr UINT SAMPLER_HEAP_SIZE = 2048;      // limite do D3D12; alocacao permanente (tabelas reaproveitadas)

        ComPtr<ID3D12Device> g_device;
        ComPtr<ID3D12InfoQueue> g_infoQueue;   // FCRECOMP_D3DDEBUG

        // mensagens da camada de verificacao (avisos e erros) para o log
        void LogD3DMessages()
        {
            if (!g_infoQueue) return;
            static int total = 0;
            UINT64 n = g_infoQueue->GetNumStoredMessages();
            for (UINT64 i = 0; i < n && total < 400; i++)
            {
                SIZE_T len = 0;
                g_infoQueue->GetMessage(i, nullptr, &len);
                std::vector<uint8_t> buf(len);
                auto* m = (D3D12_MESSAGE*)buf.data();
                // avisos antigos e inofensivos (alinhamento das texturas, valor da limpeza) nao contam
                if (SUCCEEDED(g_infoQueue->GetMessage(i, m, &len)) && m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING &&
                    !strstr(m->pDescription, "Alignment cannot be 4KB") && !strstr(m->pDescription, "clear values do not match"))
                {
                    Log("[d3d] %s", m->pDescription);
                    total++;
                }
            }
            g_infoQueue->ClearStoredMessages();
        }
        ComPtr<ID3D12CommandQueue> g_queue;
        ComPtr<IDXGIAdapter3> g_adapter3;   // consulta de uso da memoria de video
        ComPtr<ID3D12QueryHeap> g_tsHeap;           // 2 timestamps por quadro (tempo de GPU de cada quadro)
        ComPtr<ID3D12Resource> g_tsReadback;
        UINT64 g_tsFreq = 0;
        bool g_tsUsed[2] = {};
        double g_lastGpuMs = 0;
        // Consultas de oclusao (contagem de amostras que passaram no teste de profundidade), lidas 2 quadros depois
        constexpr UINT OCC_PER_FRAME = 2048;
        ComPtr<ID3D12QueryHeap> g_occHeap;
        ComPtr<ID3D12Resource> g_occReadback;
        std::vector<uint64_t> g_occTags[2];   // etiqueta de cada consulta do quadro (quem pediu)
        UINT g_occResolved[2] = {};           // quantas foram copiadas para o buffer de leitura
        int g_occOpen = -1;                   // consulta aberta na lista atual
        uint64_t g_frameSerial = 0;   // muda a cada lista de quadro aberta (o que foi enviado antes deixa de valer)
        ComPtr<IDXGISwapChain3> g_swapChain;
        ComPtr<ID3D12DescriptorHeap> g_rtvHeap, g_srvHeap, g_samplerHeap;
        ComPtr<ID3D12Resource> g_backBuffers[FRAMES];
        ComPtr<ID3D12CommandAllocator> g_allocators[FRAMES];
        ComPtr<ID3D12GraphicsCommandList> g_list;
        ComPtr<ID3D12Fence> g_fence;
        UINT64 g_fenceValues[FRAMES] = {};
        UINT64 g_nextFence = 1;
        HANDLE g_fenceEvent = nullptr;
        UINT g_rtvSize = 0, g_srvSize = 0, g_samplerSize = 0;
        uint32_t g_width = 0, g_height = 0;
        HWND g_hwnd = nullptr;
        // opcao "16:9 com barras pretas" (formato = 2) ou FCRECOMP_BARRAS=1: a troca de quadros acompanha o tamanho da
        // janela e a imagem do jogo e posta no meio sem esticar (faixas pretas dos lados num ultrawide, em cima/embaixo
        // numa janela mais alta). Sem isso, a troca de quadros tem o tamanho da resolucao interna e o Windows estica.
        bool Barras()
        {
            static const bool b = getenv("FCRECOMP_BARRAS") != nullptr || OpcoesAtuais().formato == 2;
            return b;
        }

        // estado do quadro em gravacao
        bool g_frameOpen = false;
        UINT g_frame = 0;   // indice do buffer de upload/descritores (alterna 0/1)
        ComPtr<ID3D12Resource> g_upload[FRAMES];
        uint8_t* g_uploadCpu[FRAMES] = {};
        UINT64 g_uploadUsed = 0;
        UINT g_srvUsed = 0, g_samplerUsed = 0;

        // apresentacao
        ComPtr<ID3D12RootSignature> g_blitRoot;
        ComPtr<ID3D12RootSignature> g_brilhoRoot;   // copia com brilho (tela preta e clarear do pular cena)
        ComPtr<ID3D12PipelineState> g_brilhoPso;
        ComPtr<ID3D12PipelineState> g_blitPso, g_aaPso[3];   // antisserrilhado niveis 1-3 (saida RGBA16F: a cena do jogo)
        ComPtr<ID3D12RootSignature> g_aoRoot;                // oclusao de ambiente: 3 texturas + 12 constantes
        ComPtr<ID3D12PipelineState> g_aoPso[2];              // [0] calcula (R8), [1] desfoca e escurece a cena (RGBA16F)
        ComPtr<ID3D12RootSignature> g_fsrRoot;               // FSR: 1 textura + 8 constantes
        ComPtr<ID3D12PipelineState> g_easuPso, g_rcasPso;    // FSR: ampliar (na imagem da janela) e dar nitidez (antes, em RGB 10 bits)
        ComPtr<ID3D12Resource> g_fsrTex;                     // imagem com nitidez (resolucao interna), entre as duas passadas
        ComPtr<ID3D12DescriptorHeap> g_fsrRtvHeap;
        UINT g_fsrW = 0, g_fsrH = 0;
        // Auto HDR (opcao autoHdr): a troca de quadros passa a HDR10 (R10G10B10A2, PQ/Rec.2020) e a imagem final do jogo
        // (SDR) e desenhada antes em g_hdrTex e convertida nela (HDR_HLSL). Monitor sem HDR ligado no Windows: fica SDR
        ComPtr<ID3D12PipelineState> g_hdrPso;                // converte a imagem final SDR para HDR10 (assinatura do FSR)
        ComPtr<ID3D12Resource> g_hdrTex;                     // imagem final SDR do tamanho da troca de quadros
        ComPtr<ID3D12DescriptorHeap> g_hdrRtvHeap;
        UINT g_hdrW = 0, g_hdrH = 0;
        bool g_hdrSaida = false;                             // a troca de quadros esta em HDR10 agora
        float g_hdrBranco = 200, g_hdrPico = 1000;           // nits do branco SDR (o do Windows) e do pico do monitor
        // HDR falso (opcao hdrFalso): filtro em SDR na imagem final (HDRFALSO_HLSL). Usa a mesma imagem intermediaria
        // g_hdrTex; com o Auto HDR ligado o mesmo filtro roda dentro da conversao para HDR10 (uma passada so)
        ComPtr<ID3D12PipelineState> g_falsoPso;              // g_hdrTex -> troca de quadros SDR com o filtro (assinatura do FSR)
        ComPtr<ID3D12PipelineState> g_textoPso;              // textos na tela (contador de FPS, aviso das teclas): alfa pre-multiplicado
        ComPtr<ID3D12Resource> g_pixelsTexture;
        uint32_t g_pixelsW = 0, g_pixelsH = 0;

        // Compilador de shaders: uma instancia por thread (a DXC nao aceita duas compilacoes ao mesmo tempo na mesma
        // instancia), para o aquecimento poder compilar varios shaders em paralelo
        DxcCreateInstanceProc DxcCreate()
        {
            static DxcCreateInstanceProc create = [] {
                HMODULE dll = LoadLibraryW(L"dxcompiler.dll");
                return dll ? (DxcCreateInstanceProc)GetProcAddress(dll, "DxcCreateInstance") : nullptr;
            }();
            return create;
        }
        IDxcCompiler3* ThreadCompiler()
        {
            thread_local ComPtr<IDxcCompiler3> compiler;
            if (!compiler)
                if (auto create = DxcCreate()) create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
            return compiler.Get();
        }

        const char* BLIT_HLSL = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID)
{
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(VSOut i) : SV_Target { return float4(tex.Sample(samp, i.uv).rgb, 1); }
)";
        // a mesma copia multiplicando o brilho (constante b0): tela preta enquanto uma cena e pulada e o clarear depois
        const char* BRILHO_HLSL = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
cbuffer C : register(b0) { float brilho; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID)
{
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(VSOut i) : SV_Target { return float4(tex.Sample(samp, i.uv).rgb * brilho, 1); }
)";

        // Auto HDR: a imagem final (SDR, sRGB) vira HDR10. Escrito aqui a partir das definicoes publicas (sRGB, matriz
        // BT.709 -> BT.2020 da BT.2087, curva PQ da SMPTE ST 2084). Ate o "joelho" a luminancia fica igual, no branco SDR
        // do Windows (como a area de trabalho e o jogo sem HDR); acima dele os realces (ceu, sol, fogo, explosoes) crescem
        // numa curva suave (derivada 1 no joelho, sem degrau) ate o pico do monitor no branco total. So a luminancia muda:
        // a cor (proporcao RGB) e mantida. Constantes: [0] branco SDR (nits), [1] pico / branco, [2] joelho (linear)
        // HDR falso: so em SDR, sem mudar o preto (0 continua 0) nem o branco (1 continua 1).
        //  - contraste local: media da luminancia em volta (8 amostras bilineares em dois aneis, ~1% e ~2,5% da altura,
        //    cada uma ja e a media de 4 pixels) e o pixel afastado dela (limitado, para nao criar halo nas bordas fortes);
        //  - curva de tom: sombras mais claras (pico perto de 25%) e luzes mais fortes (perto de 75%);
        //  - saturacao um pouco maior, menor nas sombras e nas luzes quase brancas (pele e ceu sem estourar).
        // forca: 1 = normal. px = tamanho de 1 pixel em uv.
        const char* HDRFALSO_FUNC = R"(
float LumaF(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 HdrFalso(Texture2D t, SamplerState s, float2 uv, float2 px, float forca)
{
    float3 c = saturate(t.SampleLevel(s, uv, 0).rgb);
    float y = LumaF(c);
    float2 r1 = float2(px.x / px.y, 1) * 0.010, r2 = float2(px.x / px.y, 1) * 0.025;   // raio pela altura da imagem
    // media local "bilateral": amostras muito diferentes do pixel (mira, textos e icones claros sobre fundo escuro, sol
    // atras de folhas) quase nao contam. Com a media simples cada amostra deslocada "carimbava" uma copia escura da mira
    // em volta dela (anel de 8 sombras).
    const float2 OFS[8] = { r1 * float2(0.7, 0.7), r1 * float2(-0.7, 0.7), r1 * float2(0.7, -0.7), r1 * float2(-0.7, -0.7),
                            r2 * float2(1, 0), r2 * float2(-1, 0), r2 * float2(0, 1), r2 * float2(0, -1) };
    float m = 0, pesos = 0;
    [unroll] for (int k = 0; k < 8; k++)
    {
        float l = LumaF(t.SampleLevel(s, uv + OFS[k], 0).rgb);
        float w = saturate(1 - abs(l - y) * 3.0);
        w *= w;
        m += l * w; pesos += w;
    }
    m = pesos > 0.001 ? m / pesos : y;
    float d = clamp(y - m, -0.12, 0.12);
    float y1 = saturate(y + forca * 0.45 * d);
    float u = 1 - y1;
    float y2 = saturate(y1 + forca * (0.45 * y1 * u * u * u + 0.30 * y1 * y1 * y1 * u));
    float3 o = y > 0.0001 ? c * (y2 / y) : c;
    float ns = 1 + forca * 0.25 * saturate(y2 * 4) * saturate((1 - y2) * 3);
    o = lerp(LumaF(o).xxx, o, ns);
    return saturate(o);
}
)";
        const char* TEXTO_HLSL = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID)
{
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(VSOut i) : SV_Target { return tex.SampleLevel(samp, i.uv, 0); }
)";
        const char* HDRFALSO_HLSL = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
cbuffer C : register(b0) { float2 px; float forca; float k3, k4, k5, k6, k7; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID)
{
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(VSOut i) : SV_Target { return float4(HdrFalso(tex, samp, i.uv, px, forca), 1); }
)";

        const char* HDR_HLSL = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
cbuffer C : register(b0) { float branco; float pico; float joelho; float forcaFalso; float2 pxFalso; float k6, k7; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID)
{
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float3 Linear(float3 c) { return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045)); }
float4 ps(VSOut i) : SV_Target
{
    float3 s = forcaFalso > 0 ? HdrFalso(tex, samp, i.uv, pxFalso, forcaFalso) : saturate(tex.Load(int3(i.pos.xy, 0)).rgb);
    float3 c = Linear(s);
    float y = dot(c, float3(0.2126, 0.7152, 0.0722));
    float t = saturate((y - joelho) / (1 - joelho));
    float y2 = y + (pico - 1) * t * t;                     // y = 1 (branco) -> pico
    c *= y > 0.0001 ? y2 / y : 1;
    c = mul(float3x3(0.6274, 0.3293, 0.0433,
                     0.0691, 0.9195, 0.0114,
                     0.0164, 0.0880, 0.8956), c);
    float3 p = pow(saturate(c * branco / 10000), 0.1593017578125);
    return float4(pow((0.8359375 + 18.8515625 * p) / (1 + 18.6875 * p), 78.84375), 1);
}
)";

        // Upscale FSR 1 (AMD FidelityFX Super Resolution 1), escrito aqui a partir da descricao publica do algoritmo
        // (opcao "fsr" do launcher). A imagem final (cena + HUD) e desenhada numa resolucao interna menor e ampliada
        // para o tamanho da janela em duas passadas. A nitidez (RCAS) vem primeiro, ainda na resolucao interna, e o
        // EASU amplia direto na imagem da janela: em placa de video integrada cada passada do tamanho da janela custa
        // caro (1080p na Radeon 610M: ~2,7 ms so para copiar), e com o RCAS depois do EASU eram duas (FSR mais pesado
        // que jogar em 1x sem ele). O EASU nao passa do minimo/maximo do 2x2, entao a nitidez antes nao cria halo.
        //  EASU (ampliar): para cada pixel da saida, 12 pixels da entrada em volta (o 2x2 mais perto e uma cruz em cada
        //   lado). Da luminancia do 2x2 sai a direcao da borda (gradiente) e o quanto ela e "borda de verdade" (o pixel
        //   do meio fica entre os vizinhos = degrau; fica acima/abaixo dos dois = ponto/linha fina). Os 12 pesos vem de
        //   uma aproximacao do Lanczos 2 girada para a direcao da borda: mais estreita atravessando a borda (nitida) e
        //   mais larga ao longo dela (sem serrilhado). O resultado fica entre o minimo e o maximo do 2x2 (sem halo).
        //  RCAS (nitidez): cruz de 5 pixels; o peso negativo dos 4 vizinhos e o maior que nao estoura 0..1 em nenhum
        //   canal (sem halo), multiplicado pela nitidez escolhida e reduzido onde o pixel destoa da cruz (ruido).
        // Constantes: tamanho da entrada, tamanho da saida, canto da saida na janela (barras pretas), nitidez, brilho.
        const char* FSR_HLSL = R"(
Texture2D<float4> tex : register(t0);
cbuffer C : register(b0) { float2 tamEntrada; float2 tamSaida; float2 origem; float nitidez; float brilho; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID)
{
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float3 Ler(int2 p) { return saturate(tex.Load(int3(clamp(p, int2(0, 0), int2(tamEntrada) - 1), 0)).rgb); }
float Luma(float3 c) { return c.g + 0.5 * (c.r + c.b); }
#if PASSO == 1
// um dos 4 pixels do 2x2: c = ele, e/d = esquerda/direita, a/b = acima/abaixo; w = peso bilinear dele
void Borda(inout float2 dir, inout float forca, float w, float c, float e, float d, float a, float b)
{
    float dx = d - e, dy = b - a;
    float fx = saturate(abs(dx) / max(max(abs(d - c), abs(c - e)), 1.0 / 65536.0));
    float fy = saturate(abs(dy) / max(max(abs(b - c), abs(c - a)), 1.0 / 65536.0));
    dir += float2(dx, dy) * w;
    forca += (fx * fx + fy * fy) * w;
}
void Somar(inout float3 soma, inout float pesos, float3 cor, float2 desl, float2 dir, float2 eixo, float lobo, float corte)
{
    float2 v = float2(dot(desl, dir), dot(desl, float2(-dir.y, dir.x))) * eixo;
    float d2 = min(dot(v, v), corte);
    float wb = 0.4 * d2 - 1.0, wa = lobo * d2 - 1.0;   // Lanczos 2 aproximado: (25/16 (2/5 x2 - 1)^2 - 9/16) (lobo x2 - 1)^2
    wb *= wb; wa *= wa;
    float w = (1.5625 * wb - 0.5625) * wa;
    soma += cor * w;
    pesos += w;
}
float4 ps(VSOut i) : SV_Target
{
    float2 p = (i.pos.xy - origem) * (tamEntrada / tamSaida) - 0.5;
    float2 fp = floor(p), f = p - fp;
    int2 q = int2(fp);
    //    b c
    //  e F G h
    //  i J K l
    //    n o
    float3 cf = Ler(q), cg = Ler(q + int2(1, 0)), cj = Ler(q + int2(0, 1)), ck = Ler(q + int2(1, 1));
    float lf = Luma(cf), lg = Luma(cg), lj = Luma(cj), lk = Luma(ck);
    // 2x2 quase liso (ceu, chao, paredes; a maior parte da tela): o filtro de borda daria o mesmo que o bilinear.
    // Sai aqui com 4 leituras em vez de 12
    // (limiar em "nitidez" no passo 1: 1/8; o modo desempenho usa sempre o bilinear. Medido em 1080p na Radeon 610M,
    // qualidade: 1/32 = 17,4 ms, 1/8 = 14,3 ms, so bilinear = 13,9 ms; 1x sem FSR = 12,4 ms)
    if (max(max(lf, lg), max(lj, lk)) - min(min(lf, lg), min(lj, lk)) < nitidez)
        return float4(lerp(lerp(cf, cg, f.x), lerp(cj, ck, f.x), f.y) * brilho, 1);
    float3 cb = Ler(q + int2(0, -1)), cc = Ler(q + int2(1, -1));
    float3 ce = Ler(q + int2(-1, 0)), ch = Ler(q + int2(2, 0));
    float3 ci = Ler(q + int2(-1, 1)), cl = Ler(q + int2(2, 1));
    float3 cn = Ler(q + int2(0, 2)), co = Ler(q + int2(1, 2));
    float lb = Luma(cb), lc = Luma(cc), le = Luma(ce), lh = Luma(ch);
    float li = Luma(ci), ll = Luma(cl), ln = Luma(cn), lo = Luma(co);
    float2 dir = 0; float forca = 0;
    Borda(dir, forca, (1 - f.x) * (1 - f.y), lf, le, lg, lb, lj);
    Borda(dir, forca, f.x * (1 - f.y), lg, lf, lh, lc, lk);
    Borda(dir, forca, (1 - f.x) * f.y, lj, li, lk, lf, ln);
    Borda(dir, forca, f.x * f.y, lk, lj, ll, lg, lo);
    float d2 = dot(dir, dir);
    dir = d2 < 1.0 / 32768.0 ? float2(1, 0) : dir * rsqrt(d2);
    forca *= 0.5; forca *= forca;
    float estica = 1.0 / max(abs(dir.x), abs(dir.y));            // diagonal: o quadrado do pixel e mais comprido
    float2 eixo = float2(1.0 + (estica - 1.0) * forca, 1.0 - 0.5 * forca);
    float lobo = 0.5 - 0.29 * forca;                             // 1/2 (macio) a 0,21 (borda forte)
    float corte = 1.0 / lobo;
    float3 soma = 0; float pesos = 0;
    Somar(soma, pesos, cb, float2(0, -1) - f, dir, eixo, lobo, corte);
    Somar(soma, pesos, cc, float2(1, -1) - f, dir, eixo, lobo, corte);
    Somar(soma, pesos, ce, float2(-1, 0) - f, dir, eixo, lobo, corte);
    Somar(soma, pesos, cf, float2(0, 0) - f, dir, eixo, lobo, corte);
    Somar(soma, pesos, cg, float2(1, 0) - f, dir, eixo, lobo, corte);
    Somar(soma, pesos, ch, float2(2, 0) - f, dir, eixo, lobo, corte);
    Somar(soma, pesos, ci, float2(-1, 1) - f, dir, eixo, lobo, corte);
    Somar(soma, pesos, cj, float2(0, 1) - f, dir, eixo, lobo, corte);
    Somar(soma, pesos, ck, float2(1, 1) - f, dir, eixo, lobo, corte);
    Somar(soma, pesos, cl, float2(2, 1) - f, dir, eixo, lobo, corte);
    Somar(soma, pesos, cn, float2(0, 2) - f, dir, eixo, lobo, corte);
    Somar(soma, pesos, co, float2(1, 2) - f, dir, eixo, lobo, corte);
    float3 mn = min(min(cf, cg), min(cj, ck)), mx = max(max(cf, cg), max(cj, ck));
    return float4(clamp(soma / max(pesos, 1.0 / 65536.0), mn, mx) * brilho, 1);
}
#else
float4 ps(VSOut i) : SV_Target
{
    int2 q = int2(i.pos.xy - origem);
    float3 b = Ler(q + int2(0, -1)), d = Ler(q + int2(-1, 0)), e = Ler(q), f = Ler(q + int2(1, 0)), h = Ler(q + int2(0, 1));
    float3 mn = min(min(b, d), min(f, h)), mx = max(max(b, d), max(f, h));
    // peso w (<= 0) dos vizinhos em (w (b + d + f + h) + e) / (4w + 1): o mais forte que nao passa de 0 nem de 1
    float3 ateZero = min(mn, e) / max(4.0 * mx, 1.0 / 65536.0);
    float3 ateUm = (1.0 - max(mx, e)) / min(4.0 * mn - 4.0, -1.0 / 65536.0);
    float3 w3 = max(-ateZero, ateUm);
    float w = clamp(max(w3.r, max(w3.g, w3.b)), -(0.25 - 1.0 / 16.0), 0.0) * nitidez;
    // menos nitidez onde o pixel destoa da cruz (ruido, granulado)
    float lb = Luma(b), ld = Luma(d), le = Luma(e), lf = Luma(f), lh = Luma(h);
    float faixa = max(max(max(lb, ld), max(lf, lh)), le) - min(min(min(lb, ld), min(lf, lh)), le);
    float ruido = saturate(abs(0.25 * (lb + ld + lf + lh) - le) / max(faixa, 1.0 / 65536.0));
    // reducao completa (antes era a metade): fogo, fumaca e particulas tem um padrao pixel a pixel que a nitidez alta
    // reforcava e, depois de ampliado, virava uma grade quadriculada; bordas de verdade quase nao tem "ruido" (o pixel
    // acompanha a media da cruz) e continuam com a nitidez inteira
    // ("ruido": borda de verdade ~0,25; listras de 1 pixel 0,5; xadrez 1): inteira ate 0,25, nada a partir de 0,5
    w *= saturate((0.5 - ruido) * 4.0);
    float3 cor = (w * (b + d + f + h) + e) / (4.0 * w + 1.0);
    return float4(saturate(cor) * brilho, 1);
}
#endif
)";

        // Antisserrilhado da cena 3D (opcao do launcher; xenos_draw aplica na imagem da cena antes do HUD e dos menus).
        // So olha a imagem pronta, entao precisa adivinhar o que e borda: onde a luminancia em volta do pixel varia mais que
        // o limiar. Para nao borrar textura (folhagem, areia), cada borda achada e seguida para os dois lados ate acabar;
        // o pixel e deslocado no maximo meio pixel ATRAVES dela, proporcional a quao perto da ponta ele esta (degrau de
        // serrilhado comprido = suavizado; pontinho de folha = deslocamento ~0, fica nitido). Bordas mais curtas que
        // COMPRIMENTO_MIN pixels sao ignoradas, menos as de contraste forte (silhuetas: arma, postes, folha contra o ceu),
        // que tambem recebem mais SUBPIXEL (deslocamento para pixels isolados/diagonais) que as fracas (textura).
        // NIVEL 1-3 (definido ao compilar): limiares, passos da busca, comprimento minimo e subpixel
        const char* AA_HLSL = R"(
#if NIVEL == 1
#define LIMIAR_ABS 0.0625
#define LIMIAR_REL 0.15
#define PASSOS 8
#define COMPRIMENTO_MIN 3.0
#define SUBPIXEL 0.0
#define SUBPIXEL_FORTE 0.5
#elif NIVEL == 2
#define LIMIAR_ABS 0.05
#define LIMIAR_REL 0.125
#define PASSOS 10
#define COMPRIMENTO_MIN 2.5
#define SUBPIXEL 0.1
#define SUBPIXEL_FORTE 0.65
#else
#define LIMIAR_ABS 0.0444
#define LIMIAR_REL 0.1
#define PASSOS 10
#define COMPRIMENTO_MIN 2.0
#define SUBPIXEL 0.25
#define SUBPIXEL_FORTE 0.75
#endif
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID)
{
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float Lum(float3 c) { return sqrt(dot(saturate(c), float3(0.299, 0.587, 0.114))); }   // perto do que o olho ve
float L(float2 uv) { return Lum(tex.SampleLevel(samp, uv, 0).rgb); }
static const float PASSO[10] = { 1, 1, 1, 1, 1.5, 2, 2, 2, 4, 8 };
float4 ps(VSOut i) : SV_Target
{
    float2 tam; tex.GetDimensions(tam.x, tam.y);
    float2 px = 1.0 / tam;
    float4 c4 = tex.SampleLevel(samp, i.uv, 0);   // o alfa da cena passa sem mudar
    float lC = Lum(c4.rgb);
    float lN = L(i.uv + float2(0, -px.y)), lS = L(i.uv + float2(0, px.y));
    float lW = L(i.uv + float2(-px.x, 0)), lE = L(i.uv + float2(px.x, 0));
    float lMin = min(lC, min(min(lN, lS), min(lW, lE)));
    float lMax = max(lC, max(max(lN, lS), max(lW, lE)));
    float contraste = lMax - lMin;
    if (contraste < max(LIMIAR_ABS, lMax * LIMIAR_REL)) return c4;

    float lNW = L(i.uv + float2(-px.x, -px.y)), lNE = L(i.uv + float2(px.x, -px.y));
    float lSW = L(i.uv + float2(-px.x, px.y)), lSE = L(i.uv + float2(px.x, px.y));
    // borda horizontal (a luminancia muda de cima para baixo) ou vertical
    float gH = abs(lN + lS - 2 * lC) * 2 + abs(lNW + lSW - 2 * lW) + abs(lNE + lSE - 2 * lE);
    float gV = abs(lW + lE - 2 * lC) * 2 + abs(lNW + lNE - 2 * lN) + abs(lSW + lSE - 2 * lS);
    bool horizontal = gH >= gV;
    float2 ao = horizontal ? float2(px.x, 0) : float2(0, px.y);         // ao longo da borda
    float2 atravessa = horizontal ? float2(0, px.y) : float2(px.x, 0);  // atraves dela
    float l1 = horizontal ? lN : lW, l2 = horizontal ? lS : lE;
    float g1 = abs(l1 - lC), g2 = abs(l2 - lC);
    bool lado1 = g1 >= g2;   // o vizinho de maior diferenca esta do lado 1 (cima/esquerda)
    float lLado = lado1 ? l1 : l2;
    float gradiente = 0.25 * max(g1, g2);
    float lMedia = 0.5 * (lLado + lC);
    if (lado1) atravessa = -atravessa;

    // segue a borda (na linha entre o pixel e o vizinho) para os dois lados ate a luminancia sair da media
    float2 uvBorda = i.uv + atravessa * 0.5;
    float2 uvA = uvBorda - ao * PASSO[0], uvB = uvBorda + ao * PASSO[0];
    float fimA = L(uvA) - lMedia, fimB = L(uvB) - lMedia;
    bool chegouA = abs(fimA) >= gradiente, chegouB = abs(fimB) >= gradiente;
    [unroll] for (int k = 1; k < PASSOS; k++)
    {
        if (chegouA && chegouB) break;
        if (!chegouA) { uvA -= ao * PASSO[k]; fimA = L(uvA) - lMedia; chegouA = abs(fimA) >= gradiente; }
        if (!chegouB) { uvB += ao * PASSO[k]; fimB = L(uvB) - lMedia; chegouB = abs(fimB) >= gradiente; }
    }
    float dA = horizontal ? (i.uv.x - uvA.x) * tam.x : (i.uv.y - uvA.y) * tam.y;
    float dB = horizontal ? (uvB.x - i.uv.x) * tam.x : (uvB.y - i.uv.y) * tam.y;
    float comprimento = dA + dB;
    bool maisPertoA = dA < dB;
    float fim = maisPertoA ? fimA : fimB;
    // so desloca se a ponta mais perto "vira" para o lado certo (o degrau do serrilhado)
    bool centroMenor = lC < lMedia;
    bool certo = (fim < 0) != centroMenor;
    // contraste forte = silhueta (arma, poste, folha contra o ceu): suaviza mesmo em diagonais curtas. Contraste
    // medio = quase sempre desenho dentro da textura (folhagem, areia): so degraus compridos
    float forte = saturate((contraste - 0.15) / 0.15);
    float desloc = (certo && (comprimento >= COMPRIMENTO_MIN || forte > 0.5)) ? 0.5 - min(dA, dB) / comprimento : 0;

    // pixel isolado ou diagonal (mais claro/escuro que a media em volta): desloca um pouco para o vizinho
    float media = (2 * (lN + lS + lW + lE) + lNW + lNE + lSW + lSE) / 12;
    float sub = saturate(abs(media - lC) / contraste);
    sub = smoothstep(0, 1, sub);
    desloc = max(desloc, sub * sub * lerp(SUBPIXEL, SUBPIXEL_FORTE, forte));

    if (desloc <= 0) return c4;
    return float4(tex.SampleLevel(samp, i.uv + atravessa * desloc, 0).rgb, c4.a);
}
)";

        // Oclusao de ambiente (AO) em espaco de tela, a partir da profundidade da cena 3D: cada pixel olha 12 pontos em
        // volta (6, 8 ou 12 conforme o nivel, num disco que encolhe com a distancia) e conta quantos estao "na frente" da sua superficie (acima do
        // plano dado pela normal, tirada da propria profundidade). Cantos, pes de paredes e baixo de objetos escurecem.
        // Passo 1: o termo de AO em meia resolucao (AO + distancia, 1/4 dos pixels; ruido varia o disco de pixel para pixel). Passo 2: os 4 vizinhos, respeitando
        // as bordas (so mistura pixels de profundidade parecida) e multiplica a cor da cena. A profundidade do Xbox nao
        // traz a distancia de verdade: o jogo usa profundidade invertida (1 perto, 0 longe), z da camera = perto / d, com
        // "perto" e o campo de visao como constantes medidas (veja xenos_draw.cpp, OcclusaoCena).
        const char* AO_HLSL = R"(
cbuffer K : register(b0)
{
    float2 tam;          // tamanho da cena (pixels da placa)
    float perto;         // plano perto da camera (metros)
    float raio;          // raio do AO (metros)
    float forca;         // quanto escurece
    float tanV;          // tangente da metade do campo de visao vertical
    float aspecto;       // largura / altura
    float invertido;     // 1 = profundidade invertida (1 perto, 0 longe)
    float verAo;         // 1 = mostra so o AO (teste)
    float escProf;       // pixels da profundidade por pixel da cena
    float distFim;       // a partir daqui o AO some (nevoa, longe)
    float amostras;      // pontos olhados em volta (6, 8 ou 12 conforme o nivel)
    float div;           // pixels da cena por pixel do AO (2 = meia resolucao; 1 abaixo de 1x, veja OcclusaoCena)
};
// abaixo de 1x (FSR) as distancias em pixels encolhem com a escala (calibradas em 1x: 720 linhas)
float EscalaPx() { return min(1.0, tam.y / 720.0); }
Texture2D cena : register(t0);
Texture2D<float> prof : register(t1);
Texture2D<float2> aoTex : register(t2);   // x = AO, y = distancia (para a media respeitar as bordas)
SamplerState amostra : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID)
{
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float Z(float2 px)
{
    float d = prof.Load(int3(int2(px * escProf), 0));
    d = invertido > 0.5 ? d : 1 - d;
    return d <= 1e-7 ? 1e6 : perto / d;
}
float3 Pos(float2 px, float z)
{
    float2 ndc = float2(px.x / tam.x * 2 - 1, 1 - px.y / tam.y * 2);
    return float3(ndc.x * tanV * aspecto * z, ndc.y * tanV * z, z);
}
#if PASSO == 1
// em meia resolucao: cada pixel do AO vale 2x2 da cena (px = centro na cena)
float2 ps(VSOut i) : SV_Target
{
    float2 px = (floor(i.pos.xy) + 0.5) * div;
    float z = Z(px);
    if (z > distFim) return float2(1, min(z, 60000));
    float3 P = Pos(px, z);
    const float e = EscalaPx(), st = max(1.0, round(2 * e));   // vizinhos para a normal: 2 pixels em 1x
    float zl = Z(px - float2(st, 0)), zr = Z(px + float2(st, 0)), zu = Z(px - float2(0, st)), zd = Z(px + float2(0, st));
    float3 dx = abs(zr - z) < abs(z - zl) ? Pos(px + float2(st, 0), zr) - P : P - Pos(px - float2(st, 0), zl);
    float3 dy = abs(zd - z) < abs(z - zu) ? Pos(px + float2(0, st), zd) - P : P - Pos(px - float2(0, st), zu);
    float3 N = normalize(cross(dx, dy));
    if (N.z > 0) N = -N;   // virada para a camera
    float rpx = clamp(raio * tam.y / (2 * tanV) / z, 3 * e, 90 * e);
    // giro do disco em 4 posicoes que se repetem a cada 2x2 pixels do AO: a mistura dos 4 vizinhos no passo 2 junta
    // os 4 giros (4x mais pontos sem custo) e nao sobra pontilhado
    int2 g = int2(floor(i.pos.xy)) & 1;
    float giro = (g.x * 2 + g.y) * 1.5707963 + (g.x ^ g.y) * 0.39;
    float occ = 0;
    const int n = (int)amostras;
    [loop] for (int k = 0; k < n; k++)
    {
        float t = (k + 0.5) / n;
        float ang = k * 2.39996323 + giro;
        float2 sp = px + float2(cos(ang), sin(ang)) * sqrt(t) * rpx;
        if (any(sp < 0) || any(sp >= tam)) continue;
        float3 v = Pos(sp, Z(sp)) - P;
        float vv = dot(v, v);
        occ += saturate(dot(v * rsqrt(vv + 1e-6), N) - 0.12) * saturate(1 - vv / (raio * raio));
    }
    // forca (1,2 / 2 / 3 nos niveis) e quanto do escuro entra, nao um multiplicador do escuro: antes (1 - forca*occ*2,5)
    // um canto comum (occ ~0,3) ja ficava preto no nivel forte e objetos perto (balde, pedra) sumiam no escuro.
    // Agora o escuro cresce ate occ/n = 0,25 e para; o nivel forte escurece no maximo 60% (leve 24%, medio 40%).
    float bruto = saturate(occ / n * 4);
    float ao = 1 - bruto * saturate(forca * 0.2);
    return float2(lerp(ao, 1, saturate((z - distFim * 0.6) / (distFim * 0.4))), z);
}
#else
float4 ps(VSOut i) : SV_Target
{
    float2 px = floor(i.pos.xy) + 0.5;
    float4 c = cena.SampleLevel(amostra, i.uv, 0);
    float z = Z(px);
    // os 4 pixels do AO (meia resolucao) mais proximos, misturados como numa ampliacao suave, mas so os de
    // distancia parecida com a deste pixel (nao vaza escuro de um objeto para o fundo)
    // div 1: os 2x2 vizinhos com o mesmo peso (juntam os 4 giros do disco)
    float2 t = px / div - (div < 1.5 ? 1.0 : 0.5);
    float2 base = floor(t), f = t - base;
    float soma = 0, peso = 0;
    [unroll] for (int k = 0; k < 4; k++)
    {
        float2 o = float2(k & 1, k >> 1);
        float2 a = aoTex.Load(int3(int2(base + o), 0));
        // os 4 pontos tem os 4 giros do disco (veja o passo 1): com o peso da ampliacao (0,56/0,19/0,19/0,06) um giro
        // dominava cada pixel e sobrava um pontilhado em xadrez nas bordas dos objetos; com pesos iguais os giros se somam
        float b = 0.25;
        float w = b * saturate(1 - abs(a.y - z) / (0.06 * z + 0.02));
        soma += a.x * w;
        peso += w;
    }
    float ao = peso > 0.0005 ? soma / peso : 1;
    if (verAo > 1.5)   // teste: profundidade crua (vermelho), detalhe (verde) e distancia/50 m (azul)
    {
        float d = prof.Load(int3(int2(px * escProf), 0));
        return float4(frac(z), saturate(z / 100), saturate(d * 4), c.a);
    }
    if (verAo > 0.5) return float4(ao, ao, ao, c.a);
    return float4(c.rgb * ao, c.a);
}
#endif
)";

        void WaitFence(UINT64 value)
        {
            if (g_fence->GetCompletedValue() < value)
            {
                perf::Scope ps(perf::Fence);
                g_fence->SetEventOnCompletion(value, g_fenceEvent);
                WaitForSingleObject(g_fenceEvent, INFINITE);
            }
        }

        bool CreateBlitPipeline()
        {
            D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
            D3D12_ROOT_PARAMETER param{};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            param.DescriptorTable = { 1, &range };
            param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_STATIC_SAMPLER_DESC samp{};
            samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            samp.MaxLOD = D3D12_FLOAT32_MAX;
            D3D12_ROOT_SIGNATURE_DESC rs{ 1, &param, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE };
            ComPtr<ID3DBlob> blob, err;
            if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
                return false;
            g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_blitRoot));

            auto vs = CompileShader(BLIT_HLSL, L"vs_6_0");
            auto ps = CompileShader(BLIT_HLSL, L"ps_6_0");
            if (vs.empty() || ps.empty())
                return false;

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = g_blitRoot.Get();
            pso.VS = { vs.data(), vs.size() };
            pso.PS = { ps.data(), ps.size() };
            pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pso.SampleMask = UINT_MAX;
            pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso.NumRenderTargets = 1;
            pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            pso.SampleDesc.Count = 1;
            for (int n = 0; n < 3; n++)
            {
                auto psAa = CompileShader("#define NIVEL " + std::to_string(n + 1) + "\n" + AA_HLSL, L"ps_6_0");
                D3D12_GRAPHICS_PIPELINE_STATE_DESC aa = pso;
                aa.PS = { psAa.data(), psAa.size() };
                aa.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;   // formato das imagens da EDRAM
                if (psAa.empty() || FAILED(g_device->CreateGraphicsPipelineState(&aa, IID_PPV_ARGS(&g_aaPso[n]))))
                    Log("[render] antisserrilhado nivel %d indisponivel", n + 1);
            }
            // AO: tabela com 3 texturas (cena, profundidade, AO), 12 constantes, amostrador linear
            {
                D3D12_DESCRIPTOR_RANGE r3{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 };
                D3D12_ROOT_PARAMETER ps2[2]{};
                ps2[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                ps2[0].DescriptorTable = { 1, &r3 };
                ps2[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                ps2[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                ps2[1].Constants = { 0, 0, 13 };
                ps2[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                D3D12_ROOT_SIGNATURE_DESC rs2{ 2, ps2, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE };
                ComPtr<ID3DBlob> b2, e2;
                if (SUCCEEDED(D3D12SerializeRootSignature(&rs2, D3D_ROOT_SIGNATURE_VERSION_1, &b2, &e2)))
                    g_device->CreateRootSignature(0, b2->GetBufferPointer(), b2->GetBufferSize(), IID_PPV_ARGS(&g_aoRoot));
                auto vsAo = CompileShader("#define PASSO 1\n" + std::string(AO_HLSL), L"vs_6_0");
                for (int n = 0; n < 2 && g_aoRoot && !vsAo.empty(); n++)
                {
                    auto psAo = CompileShader("#define PASSO " + std::to_string(n + 1) + "\n" + AO_HLSL, L"ps_6_0");
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = pso;
                    d.pRootSignature = g_aoRoot.Get();
                    d.VS = { vsAo.data(), vsAo.size() };
                    d.PS = { psAo.data(), psAo.size() };
                    d.RTVFormats[0] = n == 0 ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
                    if (psAo.empty() || FAILED(g_device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&g_aoPso[n]))))
                        Log("[render] oclusao de ambiente (passo %d) indisponivel", n + 1);
                }
            }
            // copia com brilho: tabela da textura + 1 constante
            {
                D3D12_ROOT_PARAMETER pb[2]{};
                pb[0] = param;
                pb[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                pb[1].Constants = { 0, 0, 1 };
                pb[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                D3D12_ROOT_SIGNATURE_DESC rsb{ 2, pb, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE };
                ComPtr<ID3DBlob> bb, eb;
                if (SUCCEEDED(D3D12SerializeRootSignature(&rsb, D3D_ROOT_SIGNATURE_VERSION_1, &bb, &eb)))
                    g_device->CreateRootSignature(0, bb->GetBufferPointer(), bb->GetBufferSize(), IID_PPV_ARGS(&g_brilhoRoot));
                auto vsb = CompileShader(BRILHO_HLSL, L"vs_6_0");
                auto psb = CompileShader(BRILHO_HLSL, L"ps_6_0");
                D3D12_GRAPHICS_PIPELINE_STATE_DESC d = pso;
                d.pRootSignature = g_brilhoRoot.Get();
                d.VS = { vsb.data(), vsb.size() };
                d.PS = { psb.data(), psb.size() };
                if (!g_brilhoRoot || vsb.empty() || psb.empty() || FAILED(g_device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&g_brilhoPso))))
                    Log("[render] copia com brilho indisponivel (o pular cena mostra a cena acelerada)");
            }
            // FSR: tabela da textura + 8 constantes; RCAS sai em RGB 10 bits (imagem intermediaria, na resolucao interna), EASU na imagem da janela
            {
                D3D12_ROOT_PARAMETER pf[2]{};
                pf[0] = param;
                pf[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                pf[1].Constants = { 0, 0, 8 };
                pf[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                D3D12_ROOT_SIGNATURE_DESC rsf{ 2, pf, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE };
                ComPtr<ID3DBlob> bf, ef;
                if (SUCCEEDED(D3D12SerializeRootSignature(&rsf, D3D_ROOT_SIGNATURE_VERSION_1, &bf, &ef)))
                    g_device->CreateRootSignature(0, bf->GetBufferPointer(), bf->GetBufferSize(), IID_PPV_ARGS(&g_fsrRoot));
                auto vsf = CompileShader("#define PASSO 1\n" + std::string(FSR_HLSL), L"vs_6_0");
                for (int n = 0; n < 2 && g_fsrRoot && !vsf.empty(); n++)
                {
                    std::string erros;
                    auto psf = CompileShader("#define PASSO " + std::to_string(n + 1) + "\n" + FSR_HLSL, L"ps_6_0", &erros);
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = pso;
                    d.pRootSignature = g_fsrRoot.Get();
                    d.VS = { vsf.data(), vsf.size() };
                    d.PS = { psf.data(), psf.size() };
                    d.RTVFormats[0] = n == 0 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R10G10B10A2_UNORM;
                    ID3D12PipelineState** alvo = n == 0 ? g_easuPso.ReleaseAndGetAddressOf() : g_rcasPso.ReleaseAndGetAddressOf();
                    if (psf.empty() || FAILED(g_device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(alvo))))
                        Log("[render] FSR (%s) indisponivel %s", n == 0 ? "EASU" : "RCAS", erros.c_str());
                }
            }
            // Auto HDR: mesma assinatura do FSR (1 textura + 8 constantes), saida HDR10
            if (g_fsrRoot)
            {
                const std::string fonte = std::string(HDRFALSO_FUNC) + HDR_HLSL;
                auto vsh = CompileShader(fonte, L"vs_6_0");
                auto psh = CompileShader(fonte, L"ps_6_0");
                D3D12_GRAPHICS_PIPELINE_STATE_DESC d = pso;
                d.pRootSignature = g_fsrRoot.Get();
                d.VS = { vsh.data(), vsh.size() };
                d.PS = { psh.data(), psh.size() };
                d.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
                if (vsh.empty() || psh.empty() || FAILED(g_device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&g_hdrPso))))
                    Log("[render] Auto HDR indisponivel (shader)");
                const std::string fonteF = std::string(HDRFALSO_FUNC) + HDRFALSO_HLSL;
                auto vsF = CompileShader(fonteF, L"vs_6_0");
                std::string errosF;
                auto psF = CompileShader(fonteF, L"ps_6_0", &errosF);
                D3D12_GRAPHICS_PIPELINE_STATE_DESC dF = pso;
                dF.pRootSignature = g_fsrRoot.Get();
                dF.VS = { vsF.data(), vsF.size() };
                dF.PS = { psF.data(), psF.size() };
                dF.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                if (vsF.empty() || psF.empty() || FAILED(g_device->CreateGraphicsPipelineState(&dF, IID_PPV_ARGS(&g_falsoPso))))
                    Log("[render] HDR falso indisponivel (shader) %s", errosF.c_str());
            }
            // textos na tela: assinatura da copia (1 textura), mistura com alfa pre-multiplicado
            {
                auto vsT = CompileShader(TEXTO_HLSL, L"vs_6_0");
                auto psT = CompileShader(TEXTO_HLSL, L"ps_6_0");
                D3D12_GRAPHICS_PIPELINE_STATE_DESC dT = pso;
                dT.VS = { vsT.data(), vsT.size() };
                dT.PS = { psT.data(), psT.size() };
                auto& rt = dT.BlendState.RenderTarget[0];
                rt.BlendEnable = TRUE;
                rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                if (vsT.empty() || psT.empty() || FAILED(g_device->CreateGraphicsPipelineState(&dT, IID_PPV_ARGS(&g_textoPso))))
                    Log("[render] textos na tela indisponiveis (shader)");
            }
            return SUCCEEDED(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_blitPso)));
        }
    }

    ID3D12Device* Device() { return g_device.Get(); }
    ID3D12DescriptorHeap* SrvHeap() { return g_srvHeap.Get(); }
    ID3D12DescriptorHeap* SamplerHeap() { return g_samplerHeap.Get(); }

    std::vector<uint8_t> CompileShader(const std::string& source, const wchar_t* target, std::string* errors)
    {
        // Pode ser chamada de varias threads ao mesmo tempo (aquecimento em paralelo).
        // cache em disco (build\shadercache): o mesmo texto HLSL nao e compilado de novo nas proximas execucoes.
        // Sem ele o inicio da missao ficava ~20 s entre 7 e 20 quadros/s compilando centenas de shaders.
        // FCRECOMP_NOSHADERCACHE=1 desliga. O nome do arquivo e o hash do texto + alvo (mudar o tradutor muda o texto).
        static const bool noCache = getenv("FCRECOMP_NOSHADERCACHE") != nullptr;
        uint64_t h = 1469598103934665603ull;
        for (char ch : source) { h ^= (uint8_t)ch; h *= 1099511628211ull; }
        for (const wchar_t* t = target; *t; t++) { h ^= (uint8_t)*t; h *= 1099511628211ull; }
        // Sem -Gis (contas IEEE estritas): o compilador pode juntar multiplicacao e soma e simplificar. Na vista da vila
        // (bug 7, Radeon 610M) a GPU caiu de ~36 para ~30 ms por quadro, sem diferenca visivel. FCRECOMP_COMGIS=1 volta
        // ao -Gis (outro arquivo no cache).
        static const bool noGis = getenv("FCRECOMP_COMGIS") == nullptr;
        if (noGis) { h ^= 0x5A; h *= 1099511628211ull; }
        char cachePath[64];
        snprintf(cachePath, sizeof(cachePath), "shadercache/%016llX.dxil", (unsigned long long)h);
        if (!noCache)
            if (FILE* f = fopen(cachePath, "rb"))
            {
                std::vector<uint8_t> data;
                fseek(f, 0, SEEK_END);
                long size = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (size > 0) { data.resize(size); if (fread(data.data(), 1, size, f) != (size_t)size) data.clear(); }
                fclose(f);
                if (!data.empty()) return data;
            }
        IDxcCompiler3* dxc = ThreadCompiler();
        if (!dxc)
        {
            Log("[render] dxcompiler.dll nao encontrado");
            return {};
        }

        const wchar_t* entry = target[0] == L'v' ? L"vs" : target[0] == L'g' ? L"gs" : L"ps";
        const wchar_t* args[] = { L"-E", entry, L"-T", target, L"-O3", L"-Qstrip_debug", L"-Gis" };
        DxcBuffer buf{ source.data(), source.size(), DXC_CP_UTF8 };
        ComPtr<IDxcResult> result;
        dxc->Compile(&buf, args, _countof(args) - (noGis ? 1 : 0), nullptr, IID_PPV_ARGS(&result));

        ComPtr<IDxcBlobUtf8> err;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&err), nullptr);
        HRESULT status;
        result->GetStatus(&status);
        if (FAILED(status))
        {
            std::string msg = err && err->GetStringLength() ? err->GetStringPointer() : "erro desconhecido";
            if (errors) *errors = msg;
            else Log("[render] erro ao compilar shader: %s", msg.c_str());
            return {};
        }
        ComPtr<IDxcBlob> obj;
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr);
        auto* p = (const uint8_t*)obj->GetBufferPointer();
        std::vector<uint8_t> dxil(p, p + obj->GetBufferSize());
        if (!noCache)
        {
            // grava com nome temporario e renomeia: duas threads com o mesmo shader nunca deixam um arquivo pela metade
            CreateDirectoryA("shadercache", nullptr);
            char tmpPath[96];
            snprintf(tmpPath, sizeof(tmpPath), "%s.%lu.tmp", cachePath, GetCurrentThreadId());
            if (FILE* f = fopen(tmpPath, "wb"))
            {
                bool ok = fwrite(dxil.data(), 1, dxil.size(), f) == dxil.size();
                ok = fclose(f) == 0 && ok;
                if (!ok || !MoveFileExA(tmpPath, cachePath, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmpPath);
            }
        }
        return dxil;
    }

    bool Init(HWND window, uint32_t width, uint32_t height)
    {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
            return false;
        // DRED ("migalhas"): se a placa cair, o registro diz ate qual comando ela chegou. Ligado sempre desde o item 42
        // (antes so com FCRECOMP_DRED=1); FCRECOMP_SEMDRED=1 desliga.
        if (!getenv("FCRECOMP_SEMDRED"))
        {
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred))))
            {
                dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                Log("[render] DRED ligado");
            }
        }
        // FCRECOMP_D3DDEBUG=1: camada de verificacao do DirectX (lenta); os erros vao para o log a cada quadro ([d3d])
        static const bool d3dDebug = getenv("FCRECOMP_D3DDEBUG") != nullptr;
        if (d3dDebug)
        {
            ComPtr<ID3D12Debug> dbg;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); Log("[render] verificacao do DirectX ligada"); }
        }
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device))))
        {
            Log("[render] DirectX 12 indisponivel nesta placa de video");
            return false;
        }
        if (d3dDebug)
        {
            g_device.As(&g_infoQueue);
            // aviso na hora do erro, com os enderecos de quem chamou (RVA: traduzir com o FCRecomp.map)
            ComPtr<ID3D12InfoQueue1> iq1;
            DWORD cookie = 0;
            if (SUCCEEDED(g_device.As(&iq1)))
                iq1->RegisterMessageCallback([](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID, LPCSTR desc, void*) {
                    if (sev > D3D12_MESSAGE_SEVERITY_ERROR) return;
                    void* pilha[12];
                    USHORT n = CaptureStackBackTrace(1, 12, pilha, nullptr);
                    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
                    char b[400]; int o = 0;
                    for (USHORT i = 0; i < n && o < 380; i++) o += snprintf(b + o, sizeof(b) - o, " %llX", (unsigned long long)((uintptr_t)pilha[i] - base));
                    Log("[d3d] ERRO NA HORA: %s | pilha (RVA):%s", desc, b);
                }, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
        }
        {
            DXGI_ADAPTER_DESC1 desc;
            ComPtr<IDXGIAdapter1> adapter;
            if (SUCCEEDED(factory->EnumAdapterByLuid(g_device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) && SUCCEEDED(adapter->GetDesc1(&desc)))
            {
                char name[128];
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
                Log("[render] DirectX 12 em: %s", name);
            }
            adapter.As(&g_adapter3);
        }

        D3D12_COMMAND_QUEUE_DESC qd{ D3D12_COMMAND_LIST_TYPE_DIRECT };
        g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue));

        g_width = width;
        g_height = height;
        g_hwnd = window;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = width;
        sd.Height = height;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = FRAMES;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Scaling = DXGI_SCALING_STRETCH;
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(factory->CreateSwapChainForHwnd(g_queue.Get(), window, &sd, nullptr, nullptr, &sc1)))
            return false;
        sc1.As(&g_swapChain);
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

        D3D12_DESCRIPTOR_HEAP_DESC rh{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, FRAMES };
        g_device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtvHeap));
        g_rtvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT i = 0; i < FRAMES; i++)
        {
            g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i]));
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += i * g_rtvSize;
            g_device->CreateRenderTargetView(g_backBuffers[i].Get(), nullptr, h);
            g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocators[i]));

            D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD };
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = UPLOAD_SIZE;
            bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            g_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_upload[i]));
            g_upload[i]->Map(0, nullptr, (void**)&g_uploadCpu[i]);
        }

        D3D12_DESCRIPTOR_HEAP_DESC sh{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SRV_PER_FRAME * FRAMES, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
        g_device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_srvHeap));
        g_srvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_DESCRIPTOR_HEAP_DESC smh{ D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SAMPLER_HEAP_SIZE, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
        g_device->CreateDescriptorHeap(&smh, IID_PPV_ARGS(&g_samplerHeap));
        g_samplerSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        // item 41: o heap inteiro comeca com um sampler valido (linear, repete). Uma tabela le sempre 32 entradas a
        // partir do inicio dela; antes as que nunca tinham sido escritas ficavam com lixo. As ultimas 32 sao a tabela
        // reserva (SamplerReserva), usada quando o heap enche no meio de um quadro.
        {
            D3D12_SAMPLER_DESC sd{};
            sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sd.MaxAnisotropy = 1;
            sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            D3D12_CPU_DESCRIPTOR_HANDLE c = g_samplerHeap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < SAMPLER_HEAP_SIZE; i++, c.ptr += g_samplerSize) g_device->CreateSampler(&sd, c);
        }

        g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0].Get(), nullptr, IID_PPV_ARGS(&g_list));
        g_list->Close();
        g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
        g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        if (!CreateBlitPipeline())
        {
            Log("[render] falha ao criar o pipeline de apresentacao");
            return false;
        }
        Log("[render] DirectX 12 pronto (%ux%u)", width, height);
        return true;
    }

    ID3D12GraphicsCommandList5* ListaVrs()
    {
        static ID3D12GraphicsCommandList5* l5 = [] {
            ID3D12GraphicsCommandList5* l = nullptr;
            D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
            bool tem = SUCCEEDED(g_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6))) &&
                       o6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_1;
            if (getenv("FCRECOMP_SEMVRS")) tem = false;
            // a referencia fica guardada ate o fim (a lista do quadro e sempre a mesma, so e reaberta)
            if (tem && FAILED(g_list->QueryInterface(IID_PPV_ARGS(&l)))) l = nullptr;
            Log("[render] sombreamento de taxa variavel (particulas 2x2): %s", l ? "ligado" : "a placa nao tem (ou FCRECOMP_SEMVRS)");
            return l;
        }();
        // a lista pode ter sido recriada depois de um quadro descartado (bug 40): pega a interface da lista nova
        static ID3D12GraphicsCommandList* de = g_list.Get();
        static ComPtr<ID3D12GraphicsCommandList5> nova;
        if (l5 && de != g_list.Get())
        {
            de = g_list.Get();
            nova.Reset();
            g_list.As(&nova);
        }
        return nova ? nova.Get() : (de == g_list.Get() && !nova ? l5 : nullptr);
    }

    ID3D12GraphicsCommandList* FrameList()
    {
        if (!g_frameOpen)
        {
            WaitFence(g_fenceValues[g_frame]);   // os recursos deste indice estao livres
            g_allocators[g_frame]->Reset();
            if (FAILED(g_list->Reset(g_allocators[g_frame].Get(), nullptr)))
            {
                // lista que o Close() recusou (bug 40) nao aceita Reset: cria outra
                g_list.Reset();
                g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[g_frame].Get(), nullptr, IID_PPV_ARGS(&g_list));
                Log("[render] lista de comandos recriada depois de um quadro descartado");
            }
            g_uploadUsed = 0;
            g_srvUsed = 0;
            g_frameOpen = true;
            g_frameSerial++;
            // tempo de GPU de cada quadro (inicio -> fim da lista), sem parar nada: o deste indice (2 quadros atras)
            // ja terminou, entao le o resultado dele e marca o inicio do novo
            if (!g_tsHeap)
            {
                D3D12_QUERY_HEAP_DESC qd{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, FRAMES * 2, 0 };
                g_device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_tsHeap));
                D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
                D3D12_RESOURCE_DESC bd{ D3D12_RESOURCE_DIMENSION_BUFFER, 0, FRAMES * 16, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
                g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_tsReadback));
                g_queue->GetTimestampFrequency(&g_tsFreq);
            }
            if (g_tsUsed[g_frame])
            {
                uint64_t* ts = nullptr;
                D3D12_RANGE r{ g_frame * 16, g_frame * 16 + 16 };
                if (SUCCEEDED(g_tsReadback->Map(0, &r, (void**)&ts)))
                {
                    uint64_t t0 = ts[g_frame * 2], t1 = ts[g_frame * 2 + 1];
                    if (t1 > t0 && g_tsFreq) g_lastGpuMs = (t1 - t0) * 1000.0 / g_tsFreq;
                    D3D12_RANGE none{ 0, 0 };
                    g_tsReadback->Unmap(0, &none);
                }
            }
            g_list->EndQuery(g_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g_frame * 2);
            // resultados das consultas de oclusao deste indice (o quadro de 2 atras ja terminou)
            if (g_occResolved[g_frame])
            {
                uint64_t* v = nullptr;
                D3D12_RANGE r{ g_frame * OCC_PER_FRAME * 8, (g_frame * OCC_PER_FRAME + g_occResolved[g_frame]) * 8 };
                if (SUCCEEDED(g_occReadback->Map(0, &r, (void**)&v)))
                {
                    for (UINT i = 0; i < g_occResolved[g_frame]; i++)
                        if (g_onOcclusion) g_onOcclusion(g_occTags[g_frame][i], v[g_frame * OCC_PER_FRAME + i]);
                    D3D12_RANGE none{ 0, 0 };
                    g_occReadback->Unmap(0, &none);
                }
            }
            g_occTags[g_frame].clear();
            g_occResolved[g_frame] = 0;
            g_occOpen = -1;
        }
        return g_list.Get();
    }

    Upload UploadAlloc(size_t size, size_t align)
    {
        FrameList();
        UINT64 offset = (g_uploadUsed + align - 1) & ~(UINT64)(align - 1);
        if (offset + size > UPLOAD_SIZE)
        {
            Log("[render] memoria de upload do quadro esgotada (%zu bytes)", size);
            offset = 0;   // sobrescreve (imagem errada, mas nao trava)
        }
        g_uploadUsed = offset + size;
        return { g_uploadCpu[g_frame] + offset, g_upload[g_frame]->GetGPUVirtualAddress() + offset, g_upload[g_frame].Get(), offset };
    }

    void VideoMemory(double& usageMB, double& budgetMB, double& sysUsageMB)
    {
        usageMB = budgetMB = sysUsageMB = 0;
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, sys{};
        if (g_adapter3 && SUCCEEDED(g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
        {
            usageMB = local.CurrentUsage / 1048576.0;
            budgetMB = local.Budget / 1048576.0;
        }
        if (g_adapter3 && SUCCEEDED(g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &sys)))
            sysUsageMB = sys.CurrentUsage / 1048576.0;
    }

    double LastGpuFrameMs() { return g_lastGpuMs; }
    uint64_t FrameSerial() { FrameList(); return g_frameSerial; }

    UINT64 TimestampFrequency() { UINT64 f = 0; g_queue->GetTimestampFrequency(&f); return f; }

    ID3D12Resource* UploadBuffer() { FrameList(); return g_upload[g_frame].Get(); }
    UINT64 UploadCapacity() { return UPLOAD_SIZE; }

    Descriptors AllocSrv(UINT count)
    {
        FrameList();
        if (g_srvUsed + count > SRV_PER_FRAME)
        {
            static int warned = 0;
            if (warned++ < 3) Log("[render] descritores do quadro esgotados (%u): a imagem pode sair errada", SRV_PER_FRAME);
            diag::Note(diag::GpuFeature, "descritores do quadro esgotados (desenhos do inicio do quadro saem errados)");
            g_srvUsed = 0;
        }
        UINT index = g_frame * SRV_PER_FRAME + g_srvUsed;
        g_srvUsed += count;
        Descriptors d;
        d.cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
        d.cpu.ptr += (SIZE_T)index * g_srvSize;
        d.gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
        d.gpu.ptr += (UINT64)index * g_srvSize;
        d.increment = g_srvSize;
        return d;
    }

    // Samplers: alocacao permanente (nao volta a cada quadro). Retorna cpu.ptr = 0 se o heap encheu.
    Descriptors AllocSampler(UINT count)
    {
        Descriptors d{};
        // a tabela do root signature tem 32 entradas: o inicio precisa deixar 32 dentro do heap
        if (g_samplerUsed + std::max<UINT>(count, 32) > SAMPLER_HEAP_SIZE - 32) return d;   // as ultimas 32: reserva
        UINT index = g_samplerUsed;
        g_samplerUsed += count;
        d.cpu = g_samplerHeap->GetCPUDescriptorHandleForHeapStart();
        d.cpu.ptr += (SIZE_T)index * g_samplerSize;
        d.gpu = g_samplerHeap->GetGPUDescriptorHandleForHeapStart();
        d.gpu.ptr += (UINT64)index * g_samplerSize;
        d.increment = g_samplerSize;
        return d;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE SamplerReserva()
    {
        D3D12_GPU_DESCRIPTOR_HANDLE h = g_samplerHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += (UINT64)(SAMPLER_HEAP_SIZE - 32) * g_samplerSize;
        return h;
    }

    void ResetSamplers()
    {
        WaitIdle();
        g_samplerUsed = 0;
    }

    namespace
    {
        bool FsrLigado() { return OpcoesAtuais().fsr > 0 && g_easuPso && g_rcasPso && g_fsrRoot; }

        // forca do RCAS: 2^-paradas (opcao fsrNitidez 1-4: baixa, media, alta, maxima); 0 = sem nitidez
        float FsrNitidez()
        {
            static const float paradas[] = { 0.0f, 1.5f, 0.8f, 0.3f, 0.0f };
            const int n = std::clamp(OpcoesAtuais().fsrNitidez, 0, 4);
            return n == 0 ? 0.0f : std::exp2(-paradas[n]);
        }

        // FSR da imagem final (srvEntrada = texture) para o retangulo (vx, vy, vw, vh) da imagem da janela (rtvJanela, ja
        // como alvo): RCAS da imagem do jogo para g_fsrTex (mesmo tamanho) e EASU de g_fsrTex para a janela (so uma passada no
        // tamanho da janela). false = nao deu (vai a copia comum)
        bool FsrAmpliar(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, D3D12_GPU_DESCRIPTOR_HANDLE srvEntrada,
                        D3D12_CPU_DESCRIPTOR_HANDLE rtvJanela, float vx, float vy, float vw, float vh, float brilho)
        {
            const UINT w = (UINT)vw, h = (UINT)vh;
            if (w < 16 || h < 16) return false;
            if (!g_fsrRtvHeap)
            {
                D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
                if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_fsrRtvHeap)))) return false;
            }
            const D3D12_RESOURCE_DESC td = texture->GetDesc();
            const UINT ew = (UINT)td.Width, eh = td.Height;
            static UINT64 entW = 0; static UINT entH = 0; static UINT saiW = 0, saiH = 0;
            const float nitidez = FsrNitidez();
            ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get(), g_samplerHeap.Get() };
            list->SetDescriptorHeaps(2, heaps);
            list->SetGraphicsRootSignature(g_fsrRoot.Get());
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            D3D12_GPU_DESCRIPTOR_HANDLE srvAmpliar = srvEntrada;

            // 1) RCAS (nitidez) na resolucao interna: imagem do jogo -> g_fsrTex (do tamanho dela). Nitidez desligada: pula
            if (nitidez > 0)
            {
                if (!g_fsrTex || g_fsrW != ew || g_fsrH != eh)
                {
                    WaitIdle();   // o quadro anterior pode estar lendo a imagem antiga
                    g_fsrTex.Reset();
                    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
                    D3D12_RESOURCE_DESC d{};
                    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = ew; d.Height = eh; d.DepthOrArraySize = 1; d.MipLevels = 1;
                    d.Format = DXGI_FORMAT_R10G10B10A2_UNORM; d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                                 IID_PPV_ARGS(&g_fsrTex))))
                    {
                        g_fsrW = g_fsrH = 0;
                        return false;
                    }
                    g_device->CreateRenderTargetView(g_fsrTex.Get(), nullptr, g_fsrRtvHeap->GetCPUDescriptorHandleForHeapStart());
                    g_fsrW = ew; g_fsrH = eh;
                }
                D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
                b.Transition = { g_fsrTex.Get(), 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET };
                list->ResourceBarrier(1, &b);
                D3D12_CPU_DESCRIPTOR_HANDLE rtvMeio = g_fsrRtvHeap->GetCPUDescriptorHandleForHeapStart();
                list->OMSetRenderTargets(1, &rtvMeio, FALSE, nullptr);
                D3D12_VIEWPORT vp{ 0, 0, (float)ew, (float)eh, 0, 1 };
                D3D12_RECT sc{ 0, 0, (LONG)ew, (LONG)eh };
                list->RSSetViewports(1, &vp);
                list->RSSetScissorRects(1, &sc);
                list->SetPipelineState(g_rcasPso.Get());
                list->SetGraphicsRootDescriptorTable(0, srvEntrada);
                const float k1[8] = { (float)ew, (float)eh, (float)ew, (float)eh, 0, 0, nitidez, 1 };
                list->SetGraphicsRoot32BitConstants(1, 8, k1, 0);
                list->DrawInstanced(3, 1, 0, 0);
                b.Transition = { g_fsrTex.Get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
                list->ResourceBarrier(1, &b);
                auto srvMeio = AllocSrv(1);
                g_device->CreateShaderResourceView(g_fsrTex.Get(), nullptr, srvMeio.cpu);
                srvAmpliar = srvMeio.gpu;
            }
            if (entW != ew || entH != eh || saiW != w || saiH != h)
            {
                entW = ew; entH = eh; saiW = w; saiH = h;
                Log("[render] FSR %d: %ux%u ampliada para %ux%u (%.2fx), nitidez %d", OpcoesAtuais().fsr, ew, eh, w, h,
                    (double)h / std::max(1u, eh), OpcoesAtuais().fsrNitidez);
            }

            // 2) EASU (ampliar): -> imagem da janela (no retangulo da imagem do jogo), com o brilho
            list->OMSetRenderTargets(1, &rtvJanela, FALSE, nullptr);
            D3D12_VIEWPORT vj{ vx, vy, (float)w, (float)h, 0, 1 };
            D3D12_RECT sj{ (LONG)vx, (LONG)vy, (LONG)vx + (LONG)w, (LONG)vy + (LONG)h };
            list->RSSetViewports(1, &vj);
            list->RSSetScissorRects(1, &sj);
            list->SetPipelineState(g_easuPso.Get());
            list->SetGraphicsRootDescriptorTable(0, srvAmpliar);
            static const float limiarTeste = getenv("FCRECOMP_FSRLIMIAR") ? (float)atof(getenv("FCRECOMP_FSRLIMIAR")) : -1.0f;
            const float limiar = limiarTeste >= 0 ? limiarTeste : OpcoesAtuais().fsr == 3 ? 4.0f : 1.0f / 8.0f;   // desempenho: so bilinear (luma vai ate 2)
            const float k2[8] = { (float)ew, (float)eh, (float)w, (float)h, vx, vy, limiar, brilho };
            list->SetGraphicsRoot32BitConstants(1, 8, k2, 0);
            list->DrawInstanced(3, 1, 0, 0);
            return true;
        }
    }

    namespace
    {
        // branco SDR do Windows ("brilho do conteudo SDR" nas configuracoes de HDR) do monitor com esse nome GDI, em nits
        float BrancoSdrWindows(const wchar_t* gdi)
        {
            UINT32 np = 0, nm = 0;
            if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return 200;
            std::vector<DISPLAYCONFIG_PATH_INFO> p(np);
            std::vector<DISPLAYCONFIG_MODE_INFO> m(nm);
            if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, p.data(), &nm, m.data(), nullptr) != ERROR_SUCCESS) return 200;
            for (UINT32 i = 0; i < np; i++)
            {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME s{};
                s.header = { DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(s), p[i].sourceInfo.adapterId, p[i].sourceInfo.id };
                if (DisplayConfigGetDeviceInfo(&s.header) != ERROR_SUCCESS || wcscmp(s.viewGdiDeviceName, gdi) != 0) continue;
                DISPLAYCONFIG_SDR_WHITE_LEVEL w{};
                w.header = { DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(w), p[i].targetInfo.adapterId, p[i].targetInfo.id };
                if (DisplayConfigGetDeviceInfo(&w.header) == ERROR_SUCCESS && w.SDRWhiteLevel >= 1000)
                    return std::clamp(w.SDRWhiteLevel / 1000.0f * 80.0f, 80.0f, 480.0f);
            }
            return 200;
        }

        // o monitor onde esta a janela esta em HDR (HDR ligado no Windows)? Le de novo so quando o Windows muda os
        // monitores (fabrica DXGI velha) ou a janela vai para outro monitor; o resto do tempo e a resposta guardada
        bool MonitorHdr()
        {
            static ComPtr<IDXGIFactory1> fab;
            static HMONITOR monAntes = nullptr;
            static bool hdr = false, primeira = true;
            const HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
            if (fab && fab->IsCurrent() && mon == monAntes) return hdr;
            fab.Reset();
            monAntes = mon;
            const bool antes = hdr;
            hdr = false;
            float maxNits = 0;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fab))))
            {
                ComPtr<IDXGIAdapter1> a;
                for (UINT i = 0; fab->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++)
                {
                    ComPtr<IDXGIOutput> o;
                    for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; j++)
                    {
                        ComPtr<IDXGIOutput6> o6;
                        DXGI_OUTPUT_DESC1 d{};
                        if (FAILED(o.As(&o6)) || FAILED(o6->GetDesc1(&d)) || d.Monitor != mon) continue;
                        hdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                        maxNits = d.MaxLuminance;
                        if (hdr) g_hdrBranco = BrancoSdrWindows(d.DeviceName);
                    }
                }
            }
            // pico: o que o monitor informa (entre 400 e 1500 nits); sem informacao, 1000 (como a maioria das TVs HDR)
            if (hdr) g_hdrPico = maxNits >= 400 ? std::min(maxNits, 1500.0f) : 1000.0f;
            if (primeira || hdr != antes)
                Log("[render] monitor %s (pico informado %.0f nits)", hdr ? "com HDR ligado no Windows" : "sem HDR (ou HDR desligado no Windows)", maxNits);
            primeira = false;
            return hdr;
        }

        // liga/desliga a saida HDR10 conforme a opcao e o monitor; prepara g_hdrTex do tamanho da troca de quadros
        bool HdrPreparar()
        {
            static bool falhou = false;   // o driver recusou HDR10 nesta janela: nao tenta de novo a cada quadro
            const bool quer = OpcoesAtuais().autoHdr && g_hdrPso && g_fsrRoot && g_hwnd && !falhou && MonitorHdr();
            if (quer != g_hdrSaida)
            {
                WaitIdle();
                auto trocar = [](bool hdr) {
                    for (auto& bb : g_backBuffers) bb.Reset();
                    HRESULT hr = g_swapChain->ResizeBuffers(0, g_width, g_height, hdr ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM, 0);
                    for (UINT i = 0; i < FRAMES; i++)
                    {
                        g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i]));
                        D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
                        h.ptr += i * g_rtvSize;
                        g_device->CreateRenderTargetView(g_backBuffers[i].Get(), nullptr, h);
                    }
                    if (FAILED(hr)) return false;
                    const DXGI_COLOR_SPACE_TYPE cs = hdr ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
                    UINT sup = 0;
                    if (FAILED(g_swapChain->CheckColorSpaceSupport(cs, &sup)) || !(sup & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) return !hdr;
                    return SUCCEEDED(g_swapChain->SetColorSpace1(cs)) || !hdr;
                };
                bool ok = trocar(quer);
                if (quer && !ok) { falhou = true; trocar(false); }
                g_hdrSaida = quer && ok;
                if (quer && !ok) Log("[render] Auto HDR: a placa recusou a saida HDR10; continua em SDR");
                else Log("[render] Auto HDR %s (branco SDR %.0f nits, pico %.0f nits)", g_hdrSaida ? "ligado" : "desligado", g_hdrBranco, g_hdrPico);
            }
            return g_hdrSaida;
        }

        // forca do HDR falso (0 = desligado). Testes: FCRECOMP_HDRFALSO_FORCA=x muda a forca; HDRFALSO=0/1 no ao_vivo.txt
        // desliga/liga com o jogo aberto (comparar e medir no mesmo lugar)
        float HdrFalsoForca()
        {
            bool ligado = OpcoesAtuais().hdrFalso;
            if (const std::string v = AoVivoValor("HDRFALSO"); !v.empty()) ligado = v != "0";
            if (!ligado || !g_falsoPso || !g_fsrRoot) return 0.0f;
            static const float f = getenv("FCRECOMP_HDRFALSO_FORCA") ? (float)atof(getenv("FCRECOMP_HDRFALSO_FORCA")) : 1.0f;
            return std::clamp(f, 0.0f, 3.0f);
        }

        // imagem final SDR intermediaria (g_hdrTex) do tamanho da troca de quadros, para o Auto HDR e o HDR falso
        bool ImagemSdrPreparar()
        {
            if (!g_hdrTex || g_hdrW != g_width || g_hdrH != g_height)
            {
                WaitIdle();
                g_hdrTex.Reset();
                if (!g_hdrRtvHeap)
                {
                    D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
                    if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_hdrRtvHeap)))) return false;
                }
                D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC d{};
                d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = g_width; d.Height = g_height; d.DepthOrArraySize = 1; d.MipLevels = 1;
                d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                             IID_PPV_ARGS(&g_hdrTex))))
                {
                    g_hdrW = g_hdrH = 0;
                    return false;
                }
                g_device->CreateRenderTargetView(g_hdrTex.Get(), nullptr, g_hdrRtvHeap->GetCPUDescriptorHandleForHeapStart());
                g_hdrW = g_width; g_hdrH = g_height;
            }
            return true;
        }

        // g_hdrTex (imagem final SDR, ja como alvo) -> imagem da troca de quadros (rtvTroca): HDR10 (Auto HDR, com o HDR
        // falso junto se ligado) ou SDR so com o HDR falso
        void HdrConverter(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtvTroca, bool hdr10, float falso)
        {
            D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
            b.Transition = { g_hdrTex.Get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
            list->ResourceBarrier(1, &b);
            auto srv = AllocSrv(1);
            g_device->CreateShaderResourceView(g_hdrTex.Get(), nullptr, srv.cpu);
            list->OMSetRenderTargets(1, &rtvTroca, FALSE, nullptr);
            D3D12_VIEWPORT vp{ 0, 0, (float)g_width, (float)g_height, 0, 1 };
            D3D12_RECT sc{ 0, 0, (LONG)g_width, (LONG)g_height };
            list->RSSetViewports(1, &vp);
            list->RSSetScissorRects(1, &sc);
            ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get(), g_samplerHeap.Get() };
            list->SetDescriptorHeaps(2, heaps);
            list->SetPipelineState(hdr10 ? g_hdrPso.Get() : g_falsoPso.Get());
            list->SetGraphicsRootSignature(g_fsrRoot.Get());
            list->SetGraphicsRootDescriptorTable(0, srv.gpu);
            const float pxX = 1.0f / std::max(1u, g_width), pxY = 1.0f / std::max(1u, g_height);
            if (!hdr10)
            {
                const float kf[8] = { pxX, pxY, falso, 0, 0, 0, 0, 0 };
                list->SetGraphicsRoot32BitConstants(1, 8, kf, 0);
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                list->DrawInstanced(3, 1, 0, 0);
                return;
            }
            // joelho 0,6 linear (~80% no valor da imagem): pele, sombras e meios-tons ficam como no SDR
            static const float joelhoTeste = getenv("FCRECOMP_HDRJOELHO") ? (float)atof(getenv("FCRECOMP_HDRJOELHO")) : 0.6f;
            const float k[8] = { g_hdrBranco, std::max(1.0f, g_hdrPico / g_hdrBranco), std::clamp(joelhoTeste, 0.1f, 0.95f), falso, pxX, pxY, 0, 0 };
            list->SetGraphicsRoot32BitConstants(1, 8, k, 0);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, 1, 0, 0);
        }

        // ---- textos na tela (opcoes mostrarFps e avisoTeclas): o texto e desenhado pelo GDI numa imagem pequena so quando
        // muda (2 vezes por segundo no contador), enviado para a placa e misturado por cima da imagem final.
        struct Camada { ComPtr<ID3D12Resource> tex; UINT w = 0, h = 0; std::wstring texto; int alfa = -1, px = 0; };
        Camada g_contador, g_aviso;

        // prepara a imagem da camada (molde = texto mais largo possivel, para o tamanho nao mudar a cada troca do texto
        // e nao recriar a textura); alfa 0..1 (o aviso some aos poucos); fundo escuro de cantos arredondados
        bool CamadaPreparar(ID3D12GraphicsCommandList* list, Camada& c, const std::wstring& texto, const std::wstring& molde, int px,
                            float alfa, COLORREF cor)
        {
            const int a = (int)(std::clamp(alfa, 0.0f, 1.0f) * 32 + 0.5f);   // 33 niveis: no maximo um envio por quadro no sumico
            if (c.tex && texto == c.texto && a == c.alfa && px == c.px) return true;
            HDC dc = CreateCompatibleDC(nullptr);
            HFONT fonte = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
            HGDIOBJ fonteAntes = SelectObject(dc, fonte);
            RECT m{ 0, 0, 0, 0 };
            DrawTextW(dc, molde.c_str(), -1, &m, DT_CALCRECT | DT_NOPREFIX);
            const int padX = std::max(6, px * 3 / 4), padY = std::max(3, px / 3);
            const UINT w = (UINT)(m.right + 2 * padX), h = (UINT)(m.bottom + 2 * padY);
            BITMAPINFO bi{};
            bi.bmiHeader = { sizeof(BITMAPINFOHEADER), (LONG)w, -(LONG)h, 1, 32, BI_RGB };
            void* bits = nullptr;
            HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!bmp || !bits) { SelectObject(dc, fonteAntes); DeleteObject(fonte); DeleteDC(dc); return false; }
            HGDIOBJ bmpAntes = SelectObject(dc, bmp);
            memset(bits, 0, (size_t)w * h * 4);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            RECT r{ padX, padY, (LONG)w - padX, (LONG)h - padY };
            DrawTextW(dc, texto.c_str(), -1, &r, DT_CENTER | DT_NOPREFIX);
            GdiFlush();
            // cobertura do texto (branco no preto) -> cor do texto sobre o fundo escuro, alfa pre-multiplicado
            uint32_t* p = (uint32_t*)bits;
            const float k = a / 32.0f, fundo = 0.6f, raio = (float)padY + 2;
            const float cr = GetRValue(cor) / 255.0f, cg = GetGValue(cor) / 255.0f, cb = GetBValue(cor) / 255.0f;
            for (UINT y = 0; y < h; y++)
                for (UINT x = 0; x < w; x++)
                {
                    const float dx = std::max({ raio - x - 0.5f, x + 0.5f - (w - raio), 0.0f }), dy = std::max({ raio - y - 0.5f, y + 0.5f - (h - raio), 0.0f });
                    const float dentro = std::clamp(raio + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);   // canto arredondado
                    const float t = (p[y * w + x] >> 8 & 0xFF) / 255.0f;
                    const float al = (t + fundo * dentro * (1 - t)) * k;
                    auto q = [&](float v) { return (uint32_t)(std::clamp(v, 0.0f, 1.0f) * 255 + 0.5f); };
                    p[y * w + x] = q(cb * t * k) | q(cg * t * k) << 8 | q(cr * t * k) << 16 | q(al) << 24;
                }
            // envio para a placa
            if (!c.tex || c.w != w || c.h != h)
            {
                if (c.tex) WaitIdle();
                c.tex.Reset();
                D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC td{};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
                td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
                if (FAILED(g_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&c.tex))))
                    c.tex.Reset();
                c.w = w; c.h = h;
            }
            else
            {
                D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
                b.Transition = { c.tex.Get(), 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST };
                list->ResourceBarrier(1, &b);
            }
            if (c.tex)
            {
                const uint32_t rowPitch = (w * 4 + 255) & ~255u;
                auto up = UploadAlloc((size_t)rowPitch * h, 512);
                for (UINT y = 0; y < h; y++) memcpy((uint8_t*)up.cpu + (size_t)y * rowPitch, p + (size_t)y * w, w * 4);
                D3D12_TEXTURE_COPY_LOCATION dst{ c.tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                dst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION src{ up.resource, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                src.PlacedFootprint.Offset = up.offset;
                src.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 1, rowPitch };
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
                b.Transition = { c.tex.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
                list->ResourceBarrier(1, &b);
            }
            SelectObject(dc, bmpAntes); DeleteObject(bmp);
            SelectObject(dc, fonteAntes); DeleteObject(fonte);
            DeleteDC(dc);
            c.texto = texto; c.alfa = a; c.px = px;
            return c.tex != nullptr;
        }

        void CamadaDesenhar(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const Camada& c, float x, float y)
        {
            auto srv = AllocSrv(1);
            g_device->CreateShaderResourceView(c.tex.Get(), nullptr, srv.cpu);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            x = std::floor(x); y = std::floor(y);
            D3D12_VIEWPORT vp{ x, y, (float)c.w, (float)c.h, 0, 1 };
            D3D12_RECT sc{ (LONG)x, (LONG)y, (LONG)x + (LONG)c.w, (LONG)y + (LONG)c.h };
            list->RSSetViewports(1, &vp);
            list->RSSetScissorRects(1, &sc);
            ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get(), g_samplerHeap.Get() };
            list->SetDescriptorHeaps(2, heaps);
            list->SetPipelineState(g_textoPso.Get());
            list->SetGraphicsRootSignature(g_blitRoot.Get());
            list->SetGraphicsRootDescriptorTable(0, srv.gpu);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, 1, 0, 0);
        }

        // contador de FPS e ms (canto de cima, a esquerda) e aviso das teclas F1/F2/F3 (12 s no comeco do jogo, em cima,
        // no meio) dentro do retangulo da imagem (vx, vy, vw, vh: com barras pretas fica dentro da imagem). jogo = imagem
        // do jogo (false na tela de "preparando os graficos", que vem do PresentPixels)
        void TextosDesenhar(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, float vx, float vy, float vw, float vh, bool jogo)
        {
            if (!g_textoPso || vh < 100) return;
            const Opcoes& o = OpcoesAtuais();
            const ULONGLONG agora = GetTickCount64();
            const float esc = vh / 720.0f;
            // FPS: imagens entregues ao Windows a cada meio segundo; ms = tempo medio de cada uma
            static ULONGLONG t0 = agora;
            static int imagens = 0;
            static wchar_t contador[64] = L"";
            imagens++;
            if (agora - t0 >= 500)
            {
                const double fps = imagens * 1000.0 / (double)(agora - t0);
                swprintf(contador, 64, L"%.0f FPS  |  %.1f ms", fps, (double)(agora - t0) / imagens);
                imagens = 0; t0 = agora;
            }
            if (o.mostrarFps && contador[0])
                if (CamadaPreparar(list, g_contador, contador, L"000 FPS  |  000.0 ms", std::max(11, (int)(15 * esc + 0.5f)), 1.0f, RGB(236, 196, 92)))
                    CamadaDesenhar(list, rtv, g_contador, vx + 10 * esc, vy + 10 * esc);
            // aviso das teclas: comeca na primeira imagem do jogo; aparece em 0,3 s e some aos poucos no ultimo segundo
            static ULONGLONG inicio = 0;
            if (jogo && !inicio) inicio = agora;
            if (o.avisoTeclas && inicio && agora - inicio < 12000)
            {
                const float t = (float)(agora - inicio);
                const float alfa = t < 300 ? t / 300 : t > 11000 ? (12000 - t) / 1000 : 1.0f;
                static const std::wstring texto = std::wstring(TR(L"F1  Controles      •      F2  Opções de vídeo      •      F3  Soltar/prender o mouse",
                                                                  L"F1  Controls      •      F2  Video options      •      F3  Release/capture the mouse"))
                    + L"\n" + TR(L"(este aviso pode ser desligado no launcher, em OUTROS)", L"(this hint can be turned off in the launcher, under OTHER)");
                if (CamadaPreparar(list, g_aviso, texto, texto, std::max(12, (int)(18 * esc + 0.5f)), alfa, RGB(250, 246, 236)))
                    CamadaDesenhar(list, rtv, g_aviso, vx + (vw - g_aviso.w) / 2, vy + 48 * esc);
            }
        }
    }

    void PresentTexture(ID3D12Resource* texture)
    {
        auto* list = FrameList();
        const bool fsr = FsrLigado();
        if (texture && (Barras() || fsr) && g_hwnd)
        {
            // troca de quadros do tamanho da janela (minimizada: tamanho 0, fica como esta)
            RECT cr{};
            GetClientRect(g_hwnd, &cr);
            const uint32_t cw = (uint32_t)(cr.right - cr.left), ch = (uint32_t)(cr.bottom - cr.top);
            if (cw >= 16 && ch >= 16 && (cw != g_width || ch != g_height))
            {
                WaitIdle();   // nenhum quadro anterior pode estar usando os buffers da troca
                for (auto& bb : g_backBuffers) bb.Reset();
                HRESULT hr = g_swapChain->ResizeBuffers(0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
                for (UINT i = 0; i < FRAMES; i++)
                {
                    g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i]));
                    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
                    h.ptr += i * g_rtvSize;
                    g_device->CreateRenderTargetView(g_backBuffers[i].Get(), nullptr, h);
                }
                if (SUCCEEDED(hr)) { g_width = cw; g_height = ch; }
                Log("[render] %s: imagem da janela %ux%u (%s)", fsr ? "FSR" : "barras pretas", cw, ch, SUCCEEDED(hr) ? "ok" : "falhou");
                // FSR: a resolucao interna sai do tamanho da janela (xenos_draw EscalaMeios); troca no fim do quadro se mudou
                if (fsr) xdraw::PedirNovaEscala();
            }
        }
        const bool hdrSaida = HdrPreparar();
        const float falso = HdrFalsoForca();
        // Auto HDR e/ou HDR falso: tudo abaixo desenha na imagem SDR g_hdrTex; HdrConverter passa para a troca de quadros
        const bool hdr = (hdrSaida || falso > 0) && ImagemSdrPreparar();
        if (static float antes = -1; falso != antes) { Log("[render] HDR falso %s (forca %.2f)", falso > 0 ? "ligado" : "desligado", falso); antes = falso; }
        UINT back = g_swapChain->GetCurrentBackBufferIndex();
        if (texture) {
        auto srv = AllocSrv(1);
        g_device->CreateShaderResourceView(texture, nullptr, srv.cpu);

        D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        b.Transition = { g_backBuffers[back].Get(), 0, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
        list->ResourceBarrier(1, &b);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += back * g_rtvSize;
        const D3D12_CPU_DESCRIPTOR_HANDLE rtvTroca = rtv;
        if (hdr)
        {
            D3D12_RESOURCE_BARRIER bh{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
            bh.Transition = { g_hdrTex.Get(), 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET };
            list->ResourceBarrier(1, &bh);
            rtv = g_hdrRtvHeap->GetCPUDescriptorHandleForHeapStart();
        }
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        // barras pretas: a imagem do jogo no maior retangulo com a proporcao dela que cabe na janela, centralizado
        float vx = 0, vy = 0, vw = (float)g_width, vh = (float)g_height;
        if (Barras())
        {
            const D3D12_RESOURCE_DESC td = texture->GetDesc();
            const double tw = (double)td.Width, th = (double)td.Height;
            if (tw > 0 && th > 0 && g_width > 0 && g_height > 0)
            {
                if ((double)g_width * th > (double)g_height * tw)   // janela mais larga que a imagem: faixas dos lados
                {
                    vw = (float)std::floor(g_height * tw / th + 0.5);
                    vx = (float)std::floor((g_width - vw) / 2);
                }
                else                                                 // janela mais alta: faixas em cima e embaixo
                {
                    vh = (float)std::floor(g_width * th / tw + 0.5);
                    vy = (float)std::floor((g_height - vh) / 2);
                }
                const float preto[4] = { 0, 0, 0, 1 };
                list->ClearRenderTargetView(rtv, preto, 0, nullptr);
            }
        }
        D3D12_VIEWPORT vp{ vx, vy, vw, vh, 0, 1 };
        D3D12_RECT sc{ (LONG)vx, (LONG)vy, (LONG)(vx + vw), (LONG)(vy + vh) };
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);
        // sem antisserrilhado aqui: na imagem final ele achava "bordas" no texto do HUD, dos menus (inclusive o de pausa)
        // e nos videos e borrava tudo; agora vale so na cena 3D (Antisserrilhar, chamado por xenos_draw)
        const float brilho = BrilhoTela();
        if (fsr && FsrAmpliar(list, texture, srv.gpu, rtv, vx, vy, vw, vh, brilho)) {}
        else {
        const bool comBrilho = brilho < 1.0f && g_brilhoPso;
        list->SetPipelineState(comBrilho ? g_brilhoPso.Get() : g_blitPso.Get());
        list->SetGraphicsRootSignature(comBrilho ? g_brilhoRoot.Get() : g_blitRoot.Get());
        ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get(), g_samplerHeap.Get() };
        list->SetDescriptorHeaps(2, heaps);
        list->SetGraphicsRootDescriptorTable(0, srv.gpu);
        if (comBrilho) list->SetGraphicsRoot32BitConstants(1, 1, &brilho, 0);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        }   // sem FSR
        // textos na tela: por cima de tudo (no HDR10 antes da conversao, na imagem SDR; sem ele, depois do HDR falso)
        const bool jogo = texture != g_pixelsTexture.Get();
        if (hdr && hdrSaida) { TextosDesenhar(list, rtv, vx, vy, vw, vh, jogo); HdrConverter(list, rtvTroca, hdrSaida, falso); }
        else { if (hdr) HdrConverter(list, rtvTroca, hdrSaida, falso); TextosDesenhar(list, rtvTroca, vx, vy, vw, vh, jogo); }

        b.Transition = { g_backBuffers[back].Get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT };
        list->ResourceBarrier(1, &b);
        }   // texture
        list->EndQuery(g_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g_frame * 2 + 1);
        list->ResolveQueryData(g_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g_frame * 2, 2, g_tsReadback.Get(), g_frame * 16);
        if (g_occOpen >= 0) OcclusionEnd(g_occOpen);   // consulta que o jogo nao fechou neste quadro
        if (UINT n = (UINT)g_occTags[g_frame].size())
        {
            list->ResolveQueryData(g_occHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, g_frame * OCC_PER_FRAME, n, g_occReadback.Get(),
                (UINT64)g_frame * OCC_PER_FRAME * 8);
            g_occResolved[g_frame] = n;
        }
        g_tsUsed[g_frame] = true;
        // bug 40: uma lista com uma chamada invalida (ex.: barreira de um recurso que a placa nao conseguiu criar) faz
        // o Close() falhar; enviar assim mesmo derrubava a placa (motivo 887A0001) e o jogo fechava. Agora o quadro
        // e descartado (uma imagem repetida) e o problema vai para o diagnostico.
        // teste: FCRECOMP_TESTE_LISTA=N grava uma barreira invalida (recurso nulo) na N-esima imagem
        if (static const uint64_t testeLista = strtoull(getenv("FCRECOMP_TESTE_LISTA") ? getenv("FCRECOMP_TESTE_LISTA") : "0", nullptr, 10); testeLista)
            if (static uint64_t imagensL = 0; ++imagensL == testeLista)
            {
                Log("[render] teste: barreira invalida na lista");
                D3D12_RESOURCE_BARRIER nb{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
                nb.Transition = { nullptr, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
                list->ResourceBarrier(1, &nb);
            }
        if (HRESULT chr = list->Close(); FAILED(chr))
        {
            static int avisos = 0;
            if (avisos++ < 5) Log("[render] lista de comandos recusada pelo DirectX (Close=%08X): quadro descartado; FCRECOMP_D3DDEBUG=1 mostra qual chamada", (unsigned)chr);
            diag::Note(diag::Crash, "lista de comandos recusada (quadro descartado em vez de derrubar a placa)", "FCRECOMP_D3DDEBUG=1 mostra qual chamada");
        }
        else
        {
            ID3D12CommandList* lists[] = { list };
            g_queue->ExecuteCommandLists(1, lists);
        }
        // Sem vsync do PC: o jogo ja se ritma pela interrupcao de vsync emulada (60 Hz). Esperar tambem o monitor
        // fazia os quadros perderem a vez e cairem para ~17 qps. Em janela (modelo flip) o Windows compoe a
        // imagem, entao nao ha "rasgos". FCRECOMP_VSYNC=1 volta a esperar o monitor.
        static const bool vsyncTeste = getenv("FCRECOMP_VSYNC") != nullptr;
        const bool vsync = vsyncTeste || OpcoesAtuais().vsync;   // opcao do launcher (muda na hora em OPCOES DE VIDEO)
        LogD3DMessages();
        // teste do aviso de placa parada: FCRECOMP_TESTE_PLACA=N derruba a placa na N-esima imagem
        if (static const uint64_t teste = strtoull(getenv("FCRECOMP_TESTE_PLACA") ? getenv("FCRECOMP_TESTE_PLACA") : "0", nullptr, 10);
            teste)
            if (static uint64_t imagens = 0; ++imagens == teste)
                if (ComPtr<ID3D12Device5> d5; SUCCEEDED(g_device.As(&d5))) { Log("[render] teste: derrubando a placa"); d5->RemoveDevice(); }
        HRESULT phr = texture ? g_swapChain->Present(vsync ? 1 : 0, 0) : S_OK;   // sem textura: a janela fica como esta
        if (static int n = 0; perf::AvLog() && n++ < 6) Log("[av] %.1f ms: imagem %d entregue ao Windows (Present)", perf::Now(), n);
        if (FAILED(phr))
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                const HRESULT motivo = g_device->GetDeviceRemovedReason();
                Log("[render] a placa de video foi removida/reiniciada: Present=%08X motivo=%08X",
                    (unsigned)phr, (unsigned)motivo);
                std::string pista;   // o que o DRED achou (vai para o diagnostico.txt)
                ComPtr<ID3D12DeviceRemovedExtendedData> dred;
                D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
                if (SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&dred))) && SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs)))
                {
                    for (auto* n = crumbs.pHeadAutoBreadcrumbNode; n; n = n->pNext)
                    {
                        uint32_t done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
                        if (done >= n->BreadcrumbCount) continue;   // esta lista terminou
                        uint32_t draws = 0;
                        for (uint32_t i = 0; i < done; i++)
                            if (n->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED) draws++;
                        Log("[render] DRED: lista parou na operacao %u de %u (op=%d), depois de %u desenhos completos",
                            done, n->BreadcrumbCount, (int)n->pCommandHistory[done], draws);
                        char b[160];
                        snprintf(b, sizeof(b), "lista parou na operacao %u de %u (op=%d), depois de %u desenhos; ", done,
                                 n->BreadcrumbCount, (int)n->pCommandHistory[done], draws);
                        pista += b;
                        if (g_onGpuHang) g_onGpuHang(draws);
                    }
                }
                D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
                if (dred && SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)) && pf.PageFaultVA)
                {
                    Log("[render] DRED: falha de pagina no endereco da GPU %llX", (unsigned long long)pf.PageFaultVA);
                    char b[96]; snprintf(b, sizeof(b), "falha de pagina no endereco da GPU %llX; ", (unsigned long long)pf.PageFaultVA);
                    pista += b;
                }
                // Antes so o log contava (linha perdida no meio de milhares; bug relatado como "crash sem nada no log"):
                // agora vai para o topo do diagnostico.txt e a janela avisa e fecha (a imagem ficava congelada com o
                // jogo rodando por baixo).
                char chave[200];
                snprintf(chave, sizeof(chave), "a placa de video parou (Present=%08X motivo=%08X%s)",
                         (unsigned)phr, (unsigned)motivo, motivo == (HRESULT)0x887A0001 ? " = chamada invalida do recomp; FCRECOMP_D3DDEBUG=1 mostra qual" :
                         motivo == (HRESULT)0x887A0006 ? " = travou" :
                         motivo == (HRESULT)0x887A0007 ? " = driver reiniciado" : motivo == (HRESULT)0x887A0020 ? " = erro do driver" : "");
                diag::Note(diag::Crash, chave, pista.empty() ? "DRED: nenhum comando pela metade (sem pista de qual desenho)" : "DRED: " + pista);
                diag::WriteReport("placa de video travou");
                AvisarPlacaTravou();
            }
        }
        g_fenceValues[g_frame] = g_nextFence;
        g_queue->Signal(g_fence.Get(), g_nextFence++);
        g_frameOpen = false;
        g_frame ^= 1;
    }

    bool Antisserrilhar(ID3D12GraphicsCommandList* list, ID3D12Resource* src, D3D12_CPU_DESCRIPTOR_HANDLE rtv, UINT w, UINT h, int nivel)
    {
        if (nivel < 1 || nivel > 3 || !g_aaPso[nivel - 1]) return false;
        auto srv = AllocSrv(1);
        g_device->CreateShaderResourceView(src, nullptr, srv.cpu);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0, 1 };
        D3D12_RECT sc{ 0, 0, (LONG)w, (LONG)h };
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);
        list->SetPipelineState(g_aaPso[nivel - 1].Get());
        list->SetGraphicsRootSignature(g_blitRoot.Get());
        ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get(), g_samplerHeap.Get() };
        list->SetDescriptorHeaps(2, heaps);
        list->SetGraphicsRootDescriptorTable(0, srv.gpu);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        return true;
    }

    bool OclusaoAmbiente(ID3D12GraphicsCommandList* list, const AoEntrada& e)
    {
        if (!g_aoPso[0] || !g_aoPso[1]) return false;
        auto srv = AllocSrv(3);
        D3D12_CPU_DESCRIPTOR_HANDLE h = srv.cpu;
        g_device->CreateShaderResourceView(e.cena, nullptr, h);
        h.ptr += srv.increment;
        D3D12_SHADER_RESOURCE_VIEW_DESC pd{};
        pd.Format = e.formatoProf;
        pd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        pd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        pd.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(e.prof, &pd, h);
        h.ptr += srv.increment;
        g_device->CreateShaderResourceView(e.ao, nullptr, h);
        D3D12_VIEWPORT vp{ 0, 0, (float)e.w, (float)e.h, 0, 1 };
        D3D12_RECT sc{ 0, 0, (LONG)e.w, (LONG)e.h };
        const UINT div = e.k[12] < 1.5f ? 1 : 2;   // resolucao do AO (OcclusaoCena)
        D3D12_VIEWPORT vpMeia{ 0, 0, (float)((e.w + div - 1) / div), (float)((e.h + div - 1) / div), 0, 1 };
        D3D12_RECT scMeia{ 0, 0, (LONG)((e.w + div - 1) / div), (LONG)((e.h + div - 1) / div) };
        list->RSSetViewports(1, &vpMeia);
        list->RSSetScissorRects(1, &scMeia);
        list->SetGraphicsRootSignature(g_aoRoot.Get());
        ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get(), g_samplerHeap.Get() };
        list->SetDescriptorHeaps(2, heaps);
        list->SetGraphicsRootDescriptorTable(0, srv.gpu);
        list->SetGraphicsRoot32BitConstants(1, 13, e.k, 0);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        // passo 1: AO -> e.ao (chega como alvo e sai como leitura)
        list->OMSetRenderTargets(1, &e.aoRtv, FALSE, nullptr);
        list->SetPipelineState(g_aoPso[0].Get());
        list->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = { e.ao, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        list->ResourceBarrier(1, &b);
        list->OMSetRenderTargets(1, &e.saidaRtv, FALSE, nullptr);
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);
        list->SetPipelineState(g_aoPso[1].Get());
        list->DrawInstanced(3, 1, 0, 0);
        return true;
    }

    void (*g_onOcclusion)(uint64_t tag, uint64_t samples) = nullptr;

    int OcclusionBegin(uint64_t tag)
    {
        auto* list = FrameList();
        if (!g_occHeap)
        {
            D3D12_QUERY_HEAP_DESC qd{ D3D12_QUERY_HEAP_TYPE_OCCLUSION, FRAMES * OCC_PER_FRAME, 0 };
            g_device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_occHeap));
            D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC bd{ D3D12_RESOURCE_DIMENSION_BUFFER, 0, FRAMES * OCC_PER_FRAME * 8, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
            g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_occReadback));
        }
        if (!g_occHeap || !g_occReadback) return -1;
        if (g_occOpen >= 0) OcclusionEnd(g_occOpen);   // uma por vez
        auto& tags = g_occTags[g_frame];
        if (tags.size() >= OCC_PER_FRAME) return -1;
        int index = (int)(g_frame * OCC_PER_FRAME + tags.size());
        tags.push_back(tag);
        list->BeginQuery(g_occHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, index);
        g_occOpen = index;
        return index;
    }

    // Resultados do quadro anterior (ja enviado) assim que a placa termina, sem esperar o indice dele ser reusado no
    // FrameList (2 quadros depois). Com a contagem 1 quadro mais nova o jogo para de esconder o mar por alguns quadros
    // ao virar para ele (ceu abaixo do horizonte) e de mostrar luzes atras de modelos ao andar. esperar = para ate a
    // placa terminar (ao_vivo OCLUSAOESPERA=1, teste: igual ao console, mas tira a sobreposicao CPU/GPU).
    bool ColherOclusao(bool esperar)
    {
        if (!g_fence || !g_occReadback) return false;
        const int s = g_frame ^ 1;   // quadro aberto em g_frame (ou ja trocado no Present): o anterior esta no outro
        if (!g_occResolved[s]) return false;
        if (g_fence->GetCompletedValue() < g_fenceValues[s])
        {
            if (!esperar) return false;
            WaitFence(g_fenceValues[s]);
        }
        uint64_t* v = nullptr;
        D3D12_RANGE r{ s * OCC_PER_FRAME * 8, (s * OCC_PER_FRAME + g_occResolved[s]) * 8 };
        if (SUCCEEDED(g_occReadback->Map(0, &r, (void**)&v)))
        {
            for (UINT i = 0; i < g_occResolved[s]; i++)
                if (g_onOcclusion) g_onOcclusion(g_occTags[s][i], v[s * OCC_PER_FRAME + i]);
            D3D12_RANGE none{ 0, 0 };
            g_occReadback->Unmap(0, &none);
        }
        g_occResolved[s] = 0;   // o FrameList nao entrega de novo
        return true;
    }

    void OcclusionEnd(int index)
    {
        if (index < 0 || index != g_occOpen || !g_frameOpen) return;
        g_list->EndQuery(g_occHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, index);
        g_occOpen = -1;
    }

    void WaitIdle()
    {
        WaitFence(g_nextFence - 1);
    }

    void PresentPixels(const uint32_t* pixels, uint32_t width, uint32_t height)
    {
        if (!g_device) return;
        auto* list = FrameList();

        if (g_pixelsW != width || g_pixelsH != height)
        {
            // espera a GPU antes de trocar a textura
            WaitFence(g_nextFence - 1);
            g_pixelsW = width;
            g_pixelsH = height;
            D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = width;
            td.Height = height;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            g_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_pixelsTexture));
        }
        else
        {
            D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
            b.Transition = { g_pixelsTexture.Get(), 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST };
            list->ResourceBarrier(1, &b);
        }

        uint32_t rowPitch = (width * 4 + 255) & ~255u;
        auto up = UploadAlloc((size_t)rowPitch * height, 512);
        for (uint32_t y = 0; y < height; y++)
            memcpy((uint8_t*)up.cpu + (size_t)y * rowPitch, pixels + (size_t)y * width, width * 4);

        D3D12_TEXTURE_COPY_LOCATION dst{ g_pixelsTexture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{ up.resource, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        src.PlacedFootprint.Offset = up.offset;
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, width, height, 1, rowPitch };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        b.Transition = { g_pixelsTexture.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        list->ResourceBarrier(1, &b);
        PresentTexture(g_pixelsTexture.Get());
    }
}
