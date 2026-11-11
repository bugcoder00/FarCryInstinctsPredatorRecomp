// Execucao dos desenhos da GPU Xenos com DirectX 12.
// Modelo da EDRAM: cada endereco base de cor (RB_COLOR_INFO) e de profundidade (RB_DEPTH_INFO) vira uma
// "superficie" de 2048x2048 (cor em RGBA16F, profundidade em D24S8). Os desenhos usam coordenadas em pixels
// dentro da superficie. Cada "resolve" copia o retangulo desenhado para uma textura identificada pelo endereco
// de destino; texturas com esse endereco usam a copia (cor) ou uma conversao (profundidade).
#include "idioma.h"
#include "diag.h"
void WatchAddReadRange(uint32_t addr, uint32_t size);   // watch.cpp
#include "perf.h"
#include <cstdlib>
extern uint64_t g_translateHash;   // xenos_translate.cpp (depuracao)

extern uint32_t g_constVersion[2];  // gpu.cpp: muda quando o jogo escreve nas constantes dos shaders
extern int g_tileState;              // gpu.cpp: faixa do predicated tiling (1 = primeira, desenhada na tela inteira)
extern uint32_t g_tileFullWidth;     // gpu.cpp: largura da tela inteira durante as faixas
#include <vector>
bool FotoReinicioPixels(std::vector<uint8_t>& bgra, uint32_t& w, uint32_t& h);   // gpu.cpp: tela de carregamento (troca de programa)
static const bool g_statsOn = std::getenv("FCRECOMP_STATS") != nullptr;   // estatisticas so quando pedidas (custam tempo)
#include "xenos_draw.h"
#include "xenos_translate.h"
#include "opcoes.h"
#include "render.h"
#include "guest.h"
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <immintrin.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
extern std::filesystem::path g_gameDir;   // fs.cpp: pasta do jogo


int64_t RoteiroTiqueAtual();   // input.cpp
bool AoVivoLigado(const char* chave);   // input.cpp: teste ligado no ao_vivo.txt (CHAVE=1)
void RelogioPausar(bool parar);          // input.cpp: para/solta o relogio do jogo (item 33)
std::string AoVivoValor(const char* chave);   // input.cpp
bool AoVivoPular(uint64_t vsHash, uint64_t psHash);   // input.cpp: build_game\ao_vivo.txt

namespace xdraw
{
    void ColetarReceita(const char* arquivo, const char* linha);   // item 39 (perto do AplicarReceita)
    void ColetarShader(uint64_t hash, bool pixel, const std::vector<uint32_t>& code);
    namespace
    {
        constexpr uint32_t CANVAS = 2048;   // tamanho de cada superficie da "EDRAM"
        // Resolucao interna (opcao "escala" do launcher), em meios: 2 = 1x, 3 = 1,5x, 4 = 2x. A EDRAM, os resolves e a
        // imagem final ficam maiores, mas tudo o que o jogo ve continua no tamanho dele (registradores, tamanho das
        // texturas, coordenadas dos desenhos e dos resolves, posicao do pixel nos shaders): as coordenadas so viram
        // pixels de verdade (Px) na hora de mandar para a placa. Texturas do jogo e dados lidos pela CPU nao mudam.
        uint32_t g_meios = 2;
        uint32_t Px(uint32_t v) { return v * g_meios / 2; }
        // Mapa de sombras com escala propria (opcao "sombras": 2 = como a tela, 4 = 2x, 6 = 3x; nunca menos que a tela).
        // As superficies dele sao as unicas com pitch maior que a tela (1360 para o mapa de 1344x1344).
        uint32_t g_sombraMeios = 2;
        uint32_t Pm(uint32_t v, uint32_t meios) { return v * meios / 2; }
        LONG PmL(LONG v, uint32_t meios) { return v * (LONG)meios / 2; }
        uint32_t SurfaceMeios(uint32_t key)
        {
            uint32_t pitch = (key >> 12) & 0x3FFF;
            return pitch > 1280 && pitch <= 2560 ? std::max(g_meios, g_sombraMeios) : g_meios;
        }
        constexpr DXGI_FORMAT COLOR_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;
        constexpr DXGI_FORMAT DS_TYPELESS = DXGI_FORMAT_R24G8_TYPELESS;
        constexpr DXGI_FORMAT DS_FORMAT = DXGI_FORMAT_D24_UNORM_S8_UINT;
        constexpr DXGI_FORMAT DS_SRV_FORMAT = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        constexpr DXGI_FORMAT DEPTH_COPY_FORMAT = DXGI_FORMAT_R32_FLOAT;
        // vagas de RTV/DSV (uma por superficie; passar do limite reaproveita a vaga de outra superficie e a imagem some:
        // com 64 a lista de aquecimento passou de 66 superficies de cor depois da fase 01_beach e a tela ficou preta)
        constexpr UINT MAX_SURFACES = 1024;

        ID3D12Device* g_dev = nullptr;
        ComPtr<ID3D12RootSignature> g_root;
        ComPtr<ID3D12DescriptorHeap> g_rtvHeap, g_dsvHeap;
        UINT g_rtvSize = 0, g_dsvSize = 0, g_rtvUsed = 0, g_dsvUsed = 0;
        // FCRECOMP_EXPERIMENTO: modo atual (gpu.cpp alterna a cada ~10 s): 0 normal, 1 sem consultas de oclusao (caixas
        // puladas, resposta fixa), 2 sem planos de corte, 3 limpeza da superficie inteira
        bool g_texDumpDraw = false;
   // FCRECOMP_TEXDUMP=ps=HASH: o desenho atual usa esse pixel shader
        std::vector<uint8_t> g_rectGs;
        bool g_drewThisFrame = false;
        ID3D12Resource* g_lastResolve = nullptr;
        std::map<std::string, int> g_stats;   // FCRECOMP_STATS=N: contagens do quadro N
        uint64_t g_frameNumber = 0;
        uint32_t g_drawsSinceResolve = 0;
        uint32_t g_lastResolveDest = 0;   // destino do resolve anterior (para reconhecer a segunda faixa do mesmo resolve)

        // Recursos substituidos no meio de um quadro: a lista de comandos aberta ainda pode usa-los,
        // entao so sao liberados depois que a GPU terminar alguns quadros
        std::vector<std::pair<uint64_t, ComPtr<ID3D12Resource>>> g_garbage;
        void ReleaseLater(ComPtr<ID3D12Resource> r) { if (r) g_garbage.push_back({ g_frameNumber + 3, std::move(r) }); }
        void CollectGarbage()
        {
            g_garbage.erase(std::remove_if(g_garbage.begin(), g_garbage.end(),
                [](auto& g) { return g.first <= g_frameNumber; }), g_garbage.end());
        }
        bool g_samplerResetPending = false;

        // "Imagem persistida" das telas de carregamento: o jogo desenha uma vez a tela completa (ex.: ficha do Crowe) e
        // nos quadros seguintes desenha uma textura de tela inteira (02860000) + o circulo girando. No console essa
        // textura ja tem a ultima tela; aqui nada a preenche (memoria zerada, nenhum resolve) e a tela piscava preta.
        // Guardamos uma copia do ultimo quadro de poucos desenhos que NAO usou essa textura e a entregamos no lugar dela.
        // FCRECOMP_NOPERSIST=1 desliga.
        // Bug 13: no jogo reaberto pelo "QUIT TO MAIN MENU" essa textura caiu no endereco em que o menu copiava o video
        // de fundo (0297E000, resolve a cada quadro do menu) e o carregamento mostrava o video parado. Um resolve de tela
        // inteira mais velho que a imagem guardada, com a memoria zerada, tambem vira a imagem persistida, se essa imagem
        // nao usou nenhum resolve de tela inteira (no menu toda imagem guardada usa o fundo: la o resolve continua valendo).
        ComPtr<ID3D12Resource> g_persisted;   // 1280x720, estado PIXEL_SHADER_RESOURCE
        uint64_t g_persistedFrame = 0;        // quadro em que a imagem foi guardada
        bool g_persistedPropria = false;      // a imagem guardada nao leu resolve de tela inteira
        bool g_fullResolveThisFrame = false;  // o quadro atual leu um resolve de tela inteira
        bool g_usedPersistThisFrame = false;
        uint32_t g_drawsThisFrame = 0;

        // Antisserrilhado (opcao do launcher) so na cena 3D. Na imagem final ele borrava o texto do HUD, dos menus (o de
        // pausa desenha a cena inteira por baixo) e os videos. O jogo resolve a cena pronta no endereco da tela e depois
        // a le num desenho de tela inteira (tesoura >= 1280x720) para montar a imagem; o brilho final, o HUD e os menus
        // vem depois, por cima. Nessa primeira leitura do quadro a textura e trocada por uma copia antisserrilhada.
        // So em quadros com >= 200 desenhos (videos ~3 por quadro, menu ~50, carregamento ~5, fase 800+).
        uint32_t g_prevFrontBuffer[2] = {}; // enderecos das 2 ultimas telas apresentadas (o jogo alterna entre duas)
        uint64_t g_aaFrame = ~0ull;         // quadro em que a cena ja foi antisserrilhada
        ComPtr<ID3D12Resource> g_aaScene;   // copia antisserrilhada (RGBA16F, tamanho da textura da cena)
        D3D12_RESOURCE_STATES g_aaSceneState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        int g_aaLevel = -1;

        bool MemoryIsZero(const uint8_t* p, size_t n)
        {
            const uint64_t* q = (const uint64_t*)p;
            for (size_t i = 0; i < n / 8; i++) if (q[i]) return false;
            return true;
        }

        // Descricao de cada desenho dos ultimos quadros (para achar o desenho que travou a GPU e para o desenhos.txt da
        // captura). So numeros em todo desenho: o texto e montado quando alguem le. Antes cada desenho montava o texto
        // com snprintf + std::string (~2 mil alocacoes e formatacoes por quadro na vista da vila, bug 7).
        struct HistoryEntry
        {
            uint64_t vs = 0, ps = 0;
            uint32_t prim = 0, count = 0, vertices = 0, color = 0, depth = 0;
            bool indexed = false, isDraw = false;
            std::string text;   // entradas que nao sao desenhos (resolve, copia...) ou detalhes extras da captura
            std::string Describe() const
            {
                if (!isDraw) return text;
                char b[256];
                snprintf(b, sizeof(b), "vs=%016llX ps=%016llX prim=%u n=%u vertices=%u indices=%s cor=%03X prof=%03X",
                    (unsigned long long)vs, (unsigned long long)ps, prim, count, vertices, indexed ? "sim" : "nao", color, depth);
                return b + text;
            }
        };
        std::vector<HistoryEntry> g_drawHistory[4];
        std::vector<HistoryEntry>& CurrentHistory() { return g_drawHistory[g_frameNumber & 3]; }
        void HistoryText(std::string text) { HistoryEntry e; e.text = std::move(text); CurrentHistory().push_back(std::move(e)); }

        // Estado ja aplicado na lista do quadro: o desenho so reaplica o que mudou (antes: assinatura, heaps, pipeline,
        // topologia e fator de mistura em todos os ~2 mil desenhos da vista da vila, bug 7). Zera a cada lista nova
        // (FrameSerial) e quando outra parte do codigo troca esses estados (copias do resolve: ForgetBoundState).
        struct BoundState
        {
            uint64_t serial = ~0ull;
            ID3D12RootSignature* root = nullptr;
            ID3D12DescriptorHeap* heaps[2] = {};
            ID3D12PipelineState* pso = nullptr;
            D3D12_PRIMITIVE_TOPOLOGY topo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
            float blend[4] = { -1, -1, -1, -1 };
        } g_bound;
        void ForgetBoundState() { uint64_t s = g_bound.serial; g_bound = BoundState{}; g_bound.serial = s; }

        void OnGpuHang(uint32_t completedDraws)
        {
            for (uint64_t f = 0; f < 4; f++)
            {
                uint64_t frame = g_frameNumber >= f ? g_frameNumber - f : 0;
                auto& h = g_drawHistory[frame & 3];
                if (completedDraws < h.size())
                    Log("[draw] quadro %llu, desenho %u: %s", (unsigned long long)frame, completedDraws, h[completedDraws].Describe().c_str());
            }
        }

        uint32_t Reg(uint32_t i) { return __builtin_bswap32(((uint32_t*)g_gpuRegsHost)[i & 0xFFFF]); }
        float RegF(uint32_t i) { uint32_t v = Reg(i); float f; memcpy(&f, &v, 4); return f; }
        uint8_t* Phys(uint32_t addr) { return g_base + (0xA0000000u | (addr & 0x1FFFFFFF)); }

        // ------------------------------------------------------------ aquecimento (build\warmcache)
        // Tudo o que e criado pela primeira vez (microcode dos shaders, pipelines e superficies da EDRAM) e anotado em
        // disco; na proxima execucao o Init recria tudo antes da abertura. Sem isso o primeiro quadro da missao levava
        // ~0,2-0,4 s traduzindo/compilando shaders, criando pipelines e superficies. FCRECOMP_NOWARM=1 desliga.
        namespace warm
        {
            bool g_loading = false;   // durante o aquecimento nada e anotado de novo
            bool Enabled() { static const bool on = getenv("FCRECOMP_NOWARM") == nullptr; return on; }
            void AppendLine(const char* file, const char* line)
            {
                if (!Enabled() || g_loading) return;
                CreateDirectoryA("warmcache", nullptr);
                std::string path = std::string("warmcache/") + file;
                if (FILE* f = fopen(path.c_str(), "a")) { fprintf(f, "%s\n", line); fclose(f); }
                ColetarReceita(file, line);
            }
            void SaveShader(uint64_t hash, bool pixel, const std::vector<uint32_t>& code)
            {
                if (!Enabled() || g_loading) return;
                CreateDirectoryA("warmcache", nullptr);
                char path[64];
                snprintf(path, sizeof(path), "warmcache/%s_%016llX.bin", pixel ? "ps" : "vs", (unsigned long long)hash);
                if (FILE* f = fopen(path, "rb")) { fclose(f); return; }
                if (FILE* f = fopen(path, "wb")) { fwrite(code.data(), 4, code.size(), f); fclose(f); }
                ColetarShader(hash, pixel, code);
            }
        }

        // ------------------------------------------------------------ superficies da EDRAM
        struct Surface
        {
            ComPtr<ID3D12Resource> res;
            D3D12_RESOURCE_STATES state;
            D3D12_CPU_DESCRIPTOR_HANDLE view;
            bool pendingClear = false;
            uint32_t clearValue = 0;
            DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT;   // superficies de cor: RGBA8 para alvos de 8 bits
            D3D12_RECT clearRect{};   // so o retangulo do resolve que pediu a limpeza (uniao, se houver varios)
            uint32_t width = 0;   // largura (a pitch da superficie), no tamanho do jogo; a placa tem Pm(width, meios)
            uint32_t height = CANVAS;   // altura no tamanho do jogo (veja SurfaceHeight); a placa tem Pm(height, meios)
            uint32_t meios = 2;   // escala desta superficie (SurfaceMeios)
        };

        // Largura de cada superficie = a pitch dela (bits 12-25 da chave), arredondada para 64. Antes todas tinham
        // 2048x2048: 11 de cor (32 MB cada) + 6 de profundidade passavam de 450 MB e estouravam os 512 MB de memoria de
        // video da GPU integrada -> o Windows trocava memoria com a RAM e o jogo travava por segundos.
        uint32_t SurfaceWidth(uint32_t key)
        {
            static const bool full = getenv("FCRECOMP_SUPCHEIA") != nullptr;   // depuracao: todas com 2048 de largura
            uint32_t pitch = (key >> 12) & 0x3FFF;
            // escala maior que 1x: superficies estreitas (reducoes da imagem, pitch < 640) ficam com a propria largura.
            // Na 1x continuam com 1280 como sempre foi.
            if (SurfaceMeios(key) > 2 && pitch && pitch < 640 && !full) return (pitch + 63) & ~63u;
            // pelo menos 1280: a superficie de uma faixa (pitch 800) recebe a tela inteira quando as faixas sao juntadas
            return pitch && !full ? std::min<uint32_t>(CANVAS, std::max<uint32_t>(1280, (pitch + 63) & ~63u)) : CANVAS;
        }
        // Altura de cada superficie. Na 1x sempre CANVAS. Acima disso cada pixel custa 2,25x/4x a memoria (2048 linhas
        // de 2560 pixels passavam de 1 GB somando as superficies): so as linhas que cabem na EDRAM de 10 MB a partir da
        // base (2048 blocos de 80x16 pixels de 32 bits), no minimo 736 (a tela e as faixas do predicated tiling).
        uint32_t SurfaceHeight(uint32_t key)
        {
            static const bool full = getenv("FCRECOMP_SUPCHEIA") != nullptr;
            if (SurfaceMeios(key) == 2 || full) return CANVAS;
            uint32_t base = key & 0xFFF, pitch = (key >> 12) & 0x3FFF;
            uint32_t tilesPerRow = std::max(1u, (pitch + 79) / 80);
            uint32_t rows = base < 2048 ? (2048 - base) / tilesPerRow * 16 : 0;
            return std::clamp<uint32_t>(rows, 736, CANVAS);
        }
        uint64_t g_surfaceBytes = 0;   // memoria de video das superficies (para o registro)
        std::unordered_map<uint32_t, Surface> g_colorSurfaces, g_depthSurfaces;

        void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES to)
        {
            if (state == to) return;
            D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
            b.Transition = { res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, to };
            list->ResourceBarrier(1, &b);
            state = to;
        }

        // Cada superficie da EDRAM e identificada pela base, largura (RB_SURFACE_INFO) e formato: passagens com
        // outra largura (ex.: mapa de sombras 1344x1344) nao podem sujar a cena que usa a mesma base.
        uint32_t ColorKey() { return (Reg(0x2001) & 0xFFF) | ((Reg(0x2000) & 0x3FFF) << 12) | (((Reg(0x2001) >> 16) & 0xF) << 26); }
        uint32_t DepthKey() { return (Reg(0x2002) & 0xFFF) | ((Reg(0x2000) & 0x3FFF) << 12); }

        // Formato da superficie de cor para a chave (RB_COLOR_INFO.color_format nos bits 26-29): alvos de 8 bits do Xbox
        // (0 = 8_8_8_8, 1 = 8_8_8_8_GAMMA) viram RGBA8 em vez de RGBA16F: metade da memoria escrita em cada camada
        // desenhada (folhagem com varias camadas) e o mesmo arredondamento da EDRAM de 8 bits. FCRECOMP_COR16=1 volta
        // tudo para RGBA16F.
        DXGI_FORMAT SurfaceFormat(uint32_t key)
        {
            static const bool all16 = getenv("FCRECOMP_COR16") != nullptr;
            uint32_t f = (key >> 26) & 0xF;
            return (!all16 && (f == 0 || f == 1)) ? DXGI_FORMAT_R8G8B8A8_UNORM : COLOR_FORMAT;
        }

        Surface& ColorSurface(uint32_t base)
        {
            auto& s = g_colorSurfaces[base];
            if (!s.res)
            {
                s.format = SurfaceFormat(base);
                D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC td{};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                s.meios = SurfaceMeios(base);
                s.width = SurfaceWidth(base);
                s.height = SurfaceHeight(base);
                td.Width = Pm(s.width, s.meios); td.Height = Pm(s.height, s.meios); td.DepthOrArraySize = 1; td.MipLevels = 1; td.SampleDesc.Count = 1;
                td.Format = s.format;
                g_surfaceBytes += (uint64_t)td.Width * td.Height * (s.format == COLOR_FORMAT ? 8 : 4);
                td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                D3D12_CLEAR_VALUE cv{ s.format, { 0, 0, 0, 0 } };
                { double tRt = perf::Now(); g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&s.res)); perf::g_rtCreateMs += perf::Now() - tRt; }
                s.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
                s.view = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
                s.view.ptr += (SIZE_T)(g_rtvUsed++ % MAX_SURFACES) * g_rtvSize;
                if (g_rtvUsed > MAX_SURFACES) Log("[draw] AVISO: mais de %u superficies de cor (vagas reaproveitadas)", MAX_SURFACES);
                g_dev->CreateRenderTargetView(s.res.Get(), nullptr, s.view);
                Log("[draw] superficie de cor na EDRAM base %03X largura %u formato %u (%ux%u na placa; superficies: %llu MB)", base & 0xFFF, (base >> 12) & 0x3FFF, base >> 26,
                    (unsigned)td.Width, td.Height, (unsigned long long)(g_surfaceBytes >> 20));
                // largura impossivel (> 2560) = registradores de lixo (pacote cortado/comandos velhos): nao vai para o
                // aquecimento, senao toda abertura recriaria superficies gigantes e o jogo ficava a ~1 quadro por segundo
                if (((base >> 12) & 0x3FFF) <= 2560) { char l[32]; snprintf(l, sizeof(l), "c %08X", base); warm::AppendLine("superficies.txt", l); }
            }
            return s;
        }

        Surface& DepthSurface(uint32_t base)
        {
            auto& s = g_depthSurfaces[base];
            if (!s.res)
            {
                D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC td{};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                s.meios = SurfaceMeios(base);
                s.width = SurfaceWidth(base);
                s.height = SurfaceHeight(base);
                td.Width = Pm(s.width, s.meios); td.Height = Pm(s.height, s.meios); td.DepthOrArraySize = 1; td.MipLevels = 1; td.SampleDesc.Count = 1;
                td.Format = DS_TYPELESS;
                g_surfaceBytes += (uint64_t)td.Width * td.Height * 4;
                td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                D3D12_CLEAR_VALUE dv{ DS_FORMAT };
                dv.DepthStencil = { 1.0f, 0 };
                { double tRt = perf::Now(); g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dv, IID_PPV_ARGS(&s.res)); perf::g_rtCreateMs += perf::Now() - tRt; }
                s.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                s.view = g_dsvHeap->GetCPUDescriptorHandleForHeapStart();
                s.view.ptr += (SIZE_T)(g_dsvUsed++ % MAX_SURFACES) * g_dsvSize;
                if (g_dsvUsed > MAX_SURFACES) Log("[draw] AVISO: mais de %u superficies de profundidade (vagas reaproveitadas)", MAX_SURFACES);
                D3D12_DEPTH_STENCIL_VIEW_DESC dd{};
                dd.Format = DS_FORMAT;
                dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                g_dev->CreateDepthStencilView(s.res.Get(), &dd, s.view);
                Log("[draw] superficie de profundidade na EDRAM base %03X largura %u (%ux%u na placa; superficies: %llu MB)", base & 0xFFF, (base >> 12) & 0x3FFF,
                    (unsigned)td.Width, td.Height, (unsigned long long)(g_surfaceBytes >> 20));
                // largura impossivel (> 2560) = registradores de lixo (pacote cortado/comandos velhos): nao vai para o
                // aquecimento, senao toda abertura recriaria superficies gigantes e o jogo ficava a ~1 quadro por segundo
                if (((base >> 12) & 0x3FFF) <= 2560) { char l[32]; snprintf(l, sizeof(l), "d %08X", base); warm::AppendLine("superficies.txt", l); }
            }
            return s;
        }

        // ------------------------------------------------------------ compilacao em segundo plano
        // Shader ou pipeline novo no meio do jogo (a primeira vez que aparece) levava ate ~200 ms para compilar e a
        // imagem travava (gravacao 2026-11-04_14-45: 217 ms num quadro). Agora a compilacao vai para 2 threads; o
        // desenho espera no maximo ESPERA_QUADRO ms por quadro (somando todas as esperas) e, se nao ficou pronto,
        // aquele objeto fica de fora ate ficar (alguns quadros). O aquecimento do inicio continua compilando tudo
        // antes de comecar. FCRECOMP_SEMASSINC=1 volta ao antigo (espera sempre).
        // Passadas de tela (bug 28): copias da cena para a tela, pos-processo e HUD (poucos vertices, sem gravar
        // profundidade) nao podem ficar de fora: a tela nao e repintada e o que sobra do quadro anterior acumula (fase
        // 21_DUNEA com os caches vazios: ~7 s de tela preta com o HUD borrado na cutscene de abertura, porque o shader
        // da copia esperava na fila atras de ~200 outros). Para elas (g_urgente) o trabalho e tirado da fila e feito
        // na hora pela propria thread da GPU (ou espera terminar, se uma thread de fundo ja o pegou).
        namespace assinc
        {
            struct Tarefa { std::function<void()> f; std::atomic<bool> pega{ false }, feita{ false }; };
            std::vector<std::shared_ptr<Tarefa>> g_faltando;    // desenho atual: compilacoes que nao ficaram prontas
            std::vector<std::shared_ptr<Tarefa>> g_esperadas;   // quadro atual: as dos desenhos grandes pulados (item 34)
            bool g_incompleto = false;
            constexpr int LIMIAR = 24;   // fila a partir da qual o quadro nao e mostrado (item 33)
            std::mutex g_mx;
            std::condition_variable g_cv;
            std::deque<std::shared_ptr<Tarefa>> g_fila;
            std::vector<std::thread> g_threads;
            bool g_parar = false;
            double g_esperaQuadro = 0;   // ms esperados neste quadro (thread da GPU)
            constexpr double ESPERA_QUADRO = 8.0;
            bool g_urgente = false;      // o desenho atual e uma passada de tela (thread da GPU)
            uint32_t g_urgentesFeitos = 0;
            // medicao (bug 29): trabalhos na fila/rodando e o tempo ate a fila esvaziar
            int g_pendentes = 0;         // na fila + rodando (protegido por g_mx)
            double g_inicioLote = 0, g_msLote = 0;
            uint32_t g_nLote = 0;

            // quantas threads compilam em segundo plano: FCRECOMP_COMPILADORES=n (antes do bug 29 eram 2). Padrao: os
            // nucleos logicos menos 2 (o jogo usa ~1,2 nucleo), entre 2 e 6
            int Quantas()
            {
                if (const char* e = getenv("FCRECOMP_COMPILADORES")) return std::clamp(atoi(e), 1, 16);
                int n = (int)std::thread::hardware_concurrency() - 2;
                return std::clamp(n, 2, 6);
            }
            void Rodar(const std::shared_ptr<Tarefa>& t)
            {
                double t0 = perf::Now();
                t->f(); t->f = nullptr;
                t->feita = true;
                double ms = perf::Now() - t0;
                std::lock_guard lock(g_mx);
                g_msLote += ms; g_nLote++;
                if (--g_pendentes == 0 && g_nLote)
                {
                    Log("[draw] fila de compilacao vazia: %u shaders/pipelines em %.1f s (%.0f ms de compilacao somados, media %.0f ms, %d threads)",
                        g_nLote, (perf::Now() - g_inicioLote) / 1000.0, g_msLote, g_msLote / g_nLote, (int)g_threads.size());
                    g_nLote = 0; g_msLote = 0;
                }
            }

            bool Ligado(bool aquecendo)
            {
                static const bool on = getenv("FCRECOMP_SEMASSINC") == nullptr;
                return on && !aquecendo;
            }
            std::shared_ptr<Tarefa> Enviar(std::function<void()> f)
            {
                auto t = std::make_shared<Tarefa>();
                t->f = std::move(f);
                std::lock_guard lock(g_mx);
                if (g_threads.empty())
                    for (int i = 0, n = Quantas(); i < n; i++)
                        g_threads.emplace_back([] {
                            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                            for (;;)
                            {
                                std::shared_ptr<Tarefa> job;
                                {
                                    std::unique_lock l(g_mx);
                                    g_cv.wait(l, [] { return g_parar || !g_fila.empty(); });
                                    if (g_parar) return;
                                    job = std::move(g_fila.front());
                                    g_fila.pop_front();
                                }
                                if (!job->pega.exchange(true)) Rodar(job);
                            }
                        });
                if (g_pendentes++ == 0) g_inicioLote = perf::Now();
                g_fila.push_back(t);
                g_cv.notify_one();
                return t;
            }
            // espera o trabalho terminar, dentro do que ainda sobra do limite do quadro; numa passada de tela
            // (g_urgente) faz o trabalho aqui mesmo se ainda estiver na fila, ou espera quem ja o pegou
            bool Esperar(const std::atomic<bool>& done, const std::shared_ptr<Tarefa>& t)
            {
                if (done) return true;
                double t0 = perf::Now();
                if (g_urgente && t)
                {
                    // relogio do jogo parado enquanto espera (item 34): o jogo nao "pula" depois da pausa
                    RelogioPausar(true);
                    if (!t->pega.exchange(true)) { Rodar(t); g_urgentesFeitos++; }
                    while (!done) std::this_thread::yield();
                    RelogioPausar(false);
                    g_esperaQuadro += perf::Now() - t0;
                    return true;
                }
                while (!done && g_esperaQuadro + (perf::Now() - t0) < ESPERA_QUADRO) std::this_thread::yield();
                g_esperaQuadro += perf::Now() - t0;
                if (!done && t) g_faltando.push_back(t);
                return done;
            }
            // Item 34: um desenho grande que ficou de fora (o chao, em camadas) deixa o quadro errado (chao azulado).
            // Esse quadro nao e mostrado: PresentFrame espera as compilacoes dele (as threads de fundo continuam em
            // paralelo e a da GPU ajuda) com o relogio do jogo parado, e o proximo quadro sai completo.
            void FaltouGrande()
            {
                for (auto& t : g_faltando)
                    if (std::find(g_esperadas.begin(), g_esperadas.end(), t) == g_esperadas.end()) g_esperadas.push_back(t);
                g_incompleto = true;
            }
            void EsperarGrandes()
            {
                double t0 = perf::Now();
                RelogioPausar(true);
                for (auto& t : g_esperadas)
                    if (!t->pega.exchange(true)) Rodar(t);   // ainda na fila: faz aqui
                for (auto& t : g_esperadas)
                    while (!t->feita) { if (perf::Now() - t0 > 5000) break; Sleep(1); }
                RelogioPausar(false);
                static int linhas = 0;
                if (linhas++ < 40)
                    Log("[draw] quadro com desenho grande sem shader nao mostrado: %zu compilacoes, esperou %.0f ms (relogio parado)",
                        g_esperadas.size(), perf::Now() - t0);
                g_esperadas.clear();
                g_incompleto = false;
            }
            // Item 33: ao entrar numa fase com o cache vazio sao ~340 compilacoes (~7 s) e o cenario aparecia aos pedacos
            // (so o pier e o ceu, depois as arvores, a ilha...). Agora, quando a fila passa de LIMIAR, o quadro incompleto
            // nao e mostrado (a janela continua com a ultima imagem: a tela "Carregando" do jogo) e a thread da GPU espera
            // a fila esvaziar com o relogio do jogo parado; o jogo fica parado junto (espera a placa) e continua dali.
            // Poucos shaders novos no meio do jogo (abaixo do LIMIAR) continuam em segundo plano. FCRECOMP_SEMSEGURAR=1 desliga.
            bool DeveSegurar()
            {
                static const bool off = getenv("FCRECOMP_SEMSEGURAR") != nullptr;
                if (off || g_threads.empty()) return false;
                std::lock_guard lock(g_mx);
                return g_pendentes >= LIMIAR;
            }
            void EsperarFila()
            {
                double t0 = perf::Now();
                int antes;
                { std::lock_guard lock(g_mx); antes = g_pendentes; }
                RelogioPausar(true);
                for (;;)
                {
                    { std::lock_guard lock(g_mx); if (g_pendentes == 0) break; }
                    if (perf::Now() - t0 > 20000) { Log("[draw] espera da fila de compilacao: passou de 20 s, seguindo"); break; }
                    Sleep(5);
                }
                RelogioPausar(false);
                static int linhas = 0;
                if (linhas++ < 40)
                    Log("[draw] imagem parada enquanto compila: %d shaders/pipelines na fila, esperou %.1f s (jogo e relogio parados)",
                        antes, (perf::Now() - t0) / 1000.0);
            }
            void Parar()
            {
                { std::lock_guard lock(g_mx); g_parar = true; g_fila.clear(); g_pendentes = 0; }
                g_cv.notify_all();
                for (auto& th : g_threads) th.join();
                g_threads.clear();
                g_parar = false;
            }
        }

        // ------------------------------------------------------------ shaders
        struct ShaderJob { std::string hlsl; bool pixel = false; std::vector<uint8_t> dxil; std::string errors; std::atomic<bool> done{ false }; std::shared_ptr<assinc::Tarefa> tarefa; };
        struct CompiledShader
        {
            std::vector<uint8_t> dxil; std::vector<uint32_t> fetchSlots; uint32_t textureMask = 0, cubeMask = 0; bool ok = false;
            std::shared_ptr<ShaderJob> job; std::string warnings;   // compilacao em segundo plano em andamento
        };
        std::unordered_map<uint64_t, CompiledShader> g_shaderCache;

        uint64_t ShaderKey(uint64_t hash, bool pixel, bool alphaTest)
        {
            return hash ^ (pixel ? 0x5050505050505050ull : 0) ^ (alphaTest ? 0xA1A1A1A100000000ull : 0);
        }

        // Traducao para HLSL (sempre numa thread so: o tradutor usa g_translateHash nas opcoes de depuracao)
        std::string TranslateFor(const ShaderRef& s, bool pixel, bool alphaTest, CompiledShader& c, std::string& warnings)
        {
            g_translateHash = s.hash;
            std::string hlsl = TranslateXenosShader(*s.code, pixel, &warnings, &c.fetchSlots, &c.textureMask, &c.cubeMask);
            if (alphaTest) hlsl = "#define XE_ALPHA_TEST\n" + hlsl;
            return hlsl;
        }

        void FinishShader(const ShaderRef& s, bool pixel, CompiledShader& c, const std::string& warnings, const std::string& errors);

        // alphaTest: variante do pixel shader com o alpha test (a outra fica sem "discard" e mantem o early-Z)
        const CompiledShader& GetShader(const ShaderRef& s, bool pixel, bool alphaTest = false)
        {
            auto& c = g_shaderCache[ShaderKey(s.hash, pixel, alphaTest)];
            if (!c.dxil.empty() || c.ok || !s.code) return c;
            auto concluir = [&] {
                auto j = std::move(c.job);
                c.dxil = std::move(j->dxil);
                FinishShader(s, pixel, c, c.warnings, j->errors);
            };
            if (c.job)   // ja esta compilando em segundo plano
            {
                if (assinc::Esperar(c.job->done, c.job->tarefa)) concluir();
                return c;
            }
            std::string warnings, errors;
            std::string hlsl = TranslateFor(s, pixel, alphaTest, c, warnings);
            if (!assinc::Ligado(warm::g_loading))
            {
                c.dxil = render::CompileShader(hlsl, pixel ? L"ps_6_0" : L"vs_6_0", &errors);
                FinishShader(s, pixel, c, warnings, errors);
                return c;
            }
            c.warnings = warnings;
            c.job = std::make_shared<ShaderJob>();
            c.job->hlsl = std::move(hlsl);
            c.job->pixel = pixel;
            c.job->tarefa = assinc::Enviar([j = c.job] {
                j->dxil = render::CompileShader(j->hlsl, j->pixel ? L"ps_6_0" : L"vs_6_0", &j->errors);
                j->done = true;
            });
            if (assinc::Esperar(c.job->done, c.job->tarefa)) concluir();
            return c;
        }

        // Anota o resultado da compilacao (erros e avisos no relatorio) e guarda o microcode para o proximo aquecimento
        void FinishShader(const ShaderRef& s, bool pixel, CompiledShader& c, const std::string& warnings, const std::string& errors)
        {
            char key[96];
            if (c.dxil.empty())
            {
                Log("[draw] shader %016llX nao compilou: %s", (unsigned long long)s.hash, errors.c_str());
                snprintf(key, sizeof(key), "%s %016llX nao compilou (desenhos com ele sao pulados)", pixel ? "pixel shader" : "vertex shader", (unsigned long long)s.hash);
                diag::Note(diag::Shader, key, errors.substr(0, 300));
            }
            else if (!warnings.empty())
            {
                Log("[draw] shader %016llX avisos: %s", (unsigned long long)s.hash, warnings.c_str());
                snprintf(key, sizeof(key), "%s %016llX: ", pixel ? "pixel shader" : "vertex shader", (unsigned long long)s.hash);
                diag::Note(diag::Shader, key + warnings);
            }
            c.ok = true;   // nao tenta de novo
            if (!c.dxil.empty()) warm::SaveShader(s.hash, pixel, *s.code);
        }

        // Progresso do aquecimento para a tela de preparacao (gpu.cpp): shaders compilados e pipelines criados
        std::atomic<uint32_t> g_warmShadersDone{ 0 }, g_warmShadersTotal{ 0 }, g_warmPsoDone{ 0 }, g_warmPsoTotal{ 0 };

        // Compila de uma vez, em paralelo, os shaders que ainda nao estao prontos (usado pelo aquecimento).
        // A traducao e o registro do resultado ficam nesta thread; so a compilacao (a parte demorada) e dividida.
        struct ShaderRequest { ShaderRef ref; bool pixel, alphaTest; };
        void CompileShadersInParallel(const std::vector<ShaderRequest>& requests)
        {
            struct Job { ShaderRequest req; CompiledShader* c; std::string hlsl, warnings, errors; };
            std::vector<Job> jobs;
            std::unordered_set<uint64_t> queued;
            for (const auto& r : requests)
            {
                const uint64_t k = ShaderKey(r.ref.hash, r.pixel, r.alphaTest);
                auto& c = g_shaderCache[k];
                if (!r.ref.code || !c.dxil.empty() || c.ok || c.job || !queued.insert(k).second) continue;
                Job j{ r, &c };
                j.hlsl = TranslateFor(r.ref, r.pixel, r.alphaTest, c, j.warnings);
                jobs.push_back(std::move(j));
            }
            if (jobs.empty()) return;
            g_warmShadersTotal = (uint32_t)jobs.size();

            // deixa um nucleo livre para a janela e o som
            const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
            const unsigned workers = std::max(1u, std::min<unsigned>((unsigned)jobs.size(), cores - 1));
            std::atomic<size_t> next{ 0 };
            auto work = [&] {
                for (size_t i; (i = next++) < jobs.size();)
                {
                    jobs[i].c->dxil = render::CompileShader(jobs[i].hlsl, jobs[i].req.pixel ? L"ps_6_0" : L"vs_6_0", &jobs[i].errors);
                    g_warmShadersDone++;
                }
            };
            std::vector<std::thread> pool;
            for (unsigned t = 1; t < workers; t++) pool.emplace_back(work);
            work();
            for (auto& t : pool) t.join();

            for (auto& j : jobs) FinishShader(j.req.ref, j.req.pixel, *j.c, j.warnings, j.errors);
            Log("[draw] aquecimento: %zu shaders preparados em %u threads", jobs.size(), workers);
        }

        // GS para "rect list" do Xenos: 3 cantos -> retangulo
        const char* RECT_GS_HLSL = R"(
struct Interp { float4 pos : SV_Position; float4 o[16] : TEXCOORD0; float4 clip0 : SV_ClipDistance0; float2 clip1 : SV_ClipDistance1; };
[maxvertexcount(4)]
void gs(triangle Interp v[3], inout TriangleStream<Interp> s)
{
    // o canto reto e o vertice oposto a maior aresta (a diagonal); o 4o canto e o reflexo dele
    float2 p0 = v[0].pos.xy / v[0].pos.w, p1 = v[1].pos.xy / v[1].pos.w, p2 = v[2].pos.xy / v[2].pos.w;
    float d01 = dot(p1 - p0, p1 - p0), d12 = dot(p2 - p1, p2 - p1), d20 = dot(p0 - p2, p0 - p2);
    uint c = (d12 >= d01 && d12 >= d20) ? 0 : ((d20 >= d01) ? 1 : 2);
    Interp vc = v[c], va = v[(c + 1) % 3], vb = v[(c + 2) % 3], v3;
    v3.pos = va.pos + vb.pos - vc.pos;
    [unroll] for (int i = 0; i < 16; i++) v3.o[i] = va.o[i] + vb.o[i] - vc.o[i];
    v3.clip0 = va.clip0 + vb.clip0 - vc.clip0; v3.clip1 = va.clip1 + vb.clip1 - vc.clip1;
    s.Append(vc); s.Append(va); s.Append(vb); s.Append(v3);
}
)";

        // copia de cor com conversao (superficie RGBA8 -> textura RGBA16F do resolve); usa o VS e a assinatura da copia de
        // profundidade
        const char* COLOR_COPY_HLSL = R"(
Texture2D<float4> colorTex : register(t0);
cbuffer Off : register(b0) { int2 offset; };
float4 ps(float4 pos : SV_Position) : SV_Target { return colorTex.Load(int3(int2(pos.xy) + offset, 0)); }
)";

        // copia de profundidade -> textura de cor (resolve de profundidade, ex.: mapa de sombras)
        const char* DEPTH_COPY_HLSL = R"(
Texture2D<float> depthTex : register(t0);
cbuffer Off : register(b0) { int2 offset; };
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps(float4 pos : SV_Position) : SV_Target { return depthTex.Load(int3(int2(pos.xy) + offset, 0)); }
)";
        ComPtr<ID3D12RootSignature> g_depthCopyRoot;
        ComPtr<ID3D12PipelineState> g_depthCopyPso;
        ComPtr<ID3D12PipelineState> g_colorCopyPso;   // resolve de superficie RGBA8 para textura RGBA16F (um desenho)
        ComPtr<ID3D12RootSignature> g_misturaRoot;    // mistura de duas imagens (mascara do escuro, perto do PresentFrame)
        ComPtr<ID3D12PipelineState> g_misturaPso;

        // ------------------------------------------------------------ pipelines
        struct PsoKey
        {
            uint64_t vs, ps;
            uint32_t topology, blend, depth, mask, rect, hasDepth;
            uint32_t rtFormat = COLOR_FORMAT;   // DXGI_FORMAT da superficie de cor
            uint32_t biasScale = 0, biasOffset = 0;   // deslocamento de poligono (bits do float de PA_SU_POLY_OFFSET_*)
            uint32_t cull = 0;   // PA_SU_SC_MODE_CNTL bits 0-2: corta frente (1) / tras (2); bit 2 (4) = horario e a frente
            bool operator==(const PsoKey& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
        };
        struct PsoKeyHash
        {
            size_t operator()(const PsoKey& k) const
            {
                size_t h = 1469598103934665603ull;
                const uint8_t* p = (const uint8_t*)&k;
                for (size_t i = 0; i < sizeof(k); i++) { h ^= p[i]; h *= 1099511628211ull; }
                return h;
            }
        };
        std::unordered_map<PsoKey, ComPtr<ID3D12PipelineState>, PsoKeyHash> g_psoCache;

        D3D12_BLEND BlendFactor(uint32_t f)
        {
            switch (f)
            {
            case 0: return D3D12_BLEND_ZERO;
            case 1: return D3D12_BLEND_ONE;
            case 4: return D3D12_BLEND_SRC_COLOR;
            case 5: return D3D12_BLEND_INV_SRC_COLOR;
            case 6: return D3D12_BLEND_SRC_ALPHA;
            case 7: return D3D12_BLEND_INV_SRC_ALPHA;
            case 8: return D3D12_BLEND_DEST_COLOR;
            case 9: return D3D12_BLEND_INV_DEST_COLOR;
            case 10: return D3D12_BLEND_DEST_ALPHA;
            case 11: return D3D12_BLEND_INV_DEST_ALPHA;
            case 12: case 14: return D3D12_BLEND_BLEND_FACTOR;
            case 13: case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
            case 16: return D3D12_BLEND_SRC_ALPHA_SAT;
            default: return D3D12_BLEND_ONE;
            }
        }

        D3D12_BLEND AlphaFactor(uint32_t f)
        {
            D3D12_BLEND b = BlendFactor(f);
            if (b == D3D12_BLEND_SRC_COLOR) return D3D12_BLEND_SRC_ALPHA;
            if (b == D3D12_BLEND_INV_SRC_COLOR) return D3D12_BLEND_INV_SRC_ALPHA;
            if (b == D3D12_BLEND_DEST_COLOR) return D3D12_BLEND_DEST_ALPHA;
            if (b == D3D12_BLEND_INV_DEST_COLOR) return D3D12_BLEND_INV_DEST_ALPHA;
            return b;
        }

        D3D12_BLEND_OP BlendOp(uint32_t o)
        {
            switch (o)
            {
            case 1: return D3D12_BLEND_OP_SUBTRACT;
            case 2: return D3D12_BLEND_OP_MIN;
            case 3: return D3D12_BLEND_OP_MAX;
            case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
            default: return D3D12_BLEND_OP_ADD;
            }
        }

        // pipelines sendo criados em segundo plano (veja namespace assinc) e os que falharam (nao tenta de novo)
        struct PsoJob { D3D12_GRAPHICS_PIPELINE_STATE_DESC d{}; std::vector<uint8_t> vs, ps; ComPtr<ID3D12PipelineState> pso; HRESULT hr = S_OK; double ms = 0; std::atomic<bool> done{ false }; std::shared_ptr<assinc::Tarefa> tarefa; };
        std::unordered_map<PsoKey, std::shared_ptr<PsoJob>, PsoKeyHash> g_psoPendente;
        std::unordered_set<PsoKey, PsoKeyHash> g_psoFalhou;

        // novo = o pipeline acabou de ficar pronto (para anotar no aquecimento)
        ID3D12PipelineState* GetPso(const PsoKey& key, const CompiledShader& vs, const CompiledShader& ps, bool* novo = nullptr)
        {
            if (novo) *novo = false;
            if (auto it = g_psoCache.find(key); it != g_psoCache.end() && it->second) return it->second.Get();
            if (g_psoFalhou.count(key)) return nullptr;
            auto concluir = [&](PsoJob& j) -> ID3D12PipelineState* {
                perf::g_psoMs += j.ms;
                perf::g_psoNew++;
                if (FAILED(j.hr) || !j.pso)
                {
                    Log("[draw] falha ao criar PSO (%08X) vs=%016llX ps=%016llX", (unsigned)j.hr, (unsigned long long)key.vs, (unsigned long long)key.ps);
                    g_psoFalhou.insert(key);
                    return nullptr;
                }
                if (novo) *novo = true;
                return (g_psoCache[key] = j.pso).Get();
            };
            if (auto it = g_psoPendente.find(key); it != g_psoPendente.end())
            {
                if (!assinc::Esperar(it->second->done, it->second->tarefa)) return nullptr;
                auto j = it->second;
                g_psoPendente.erase(it);
                return concluir(*j);
            }

            D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
            d.pRootSignature = g_root.Get();
            d.VS = { vs.dxil.data(), vs.dxil.size() };
            d.PS = { ps.dxil.data(), ps.dxil.size() };
            if (key.rect) d.GS = { g_rectGs.data(), g_rectGs.size() };

            // RB_BLENDCONTROL0: cor src(0-4) op(5-7) dst(8-12), alfa src(16-20) op(21-23) dst(24-28)
            auto& rt = d.BlendState.RenderTarget[0];
            uint32_t b = key.blend;
            rt.SrcBlend = BlendFactor(b & 0x1F);
            rt.BlendOp = BlendOp((b >> 5) & 7);
            rt.DestBlend = BlendFactor((b >> 8) & 0x1F);
            rt.SrcBlendAlpha = AlphaFactor((b >> 16) & 0x1F);
            rt.BlendOpAlpha = BlendOp((b >> 21) & 7);
            rt.DestBlendAlpha = AlphaFactor((b >> 24) & 0x1F);
            rt.BlendEnable = !(rt.SrcBlend == D3D12_BLEND_ONE && rt.DestBlend == D3D12_BLEND_ZERO && rt.BlendOp == D3D12_BLEND_OP_ADD &&
                               rt.SrcBlendAlpha == D3D12_BLEND_ONE && rt.DestBlendAlpha == D3D12_BLEND_ZERO && rt.BlendOpAlpha == D3D12_BLEND_OP_ADD);
            rt.RenderTargetWriteMask = key.mask & 0xF;

            d.SampleMask = UINT_MAX;
            d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            // Bug 23: o Xenos descarta as faces de costas (PA_SU_SC_MODE_CNTL); antes desenhavamos as duas faces de tudo.
            // As paredes de ripas da cabana tem os dois lados, com as frestas desencontradas: no mapa de sombra do sol as
            // duas camadas juntas tampavam quase todas as frestas (a mesa ficava escura; no Xbox ela tem faixas de luz).
            // A tela do Xenos tem y para baixo, como a da placa depois do viewport: a ordem dos vertices nao muda.
            d.RasterizerState.CullMode = (key.cull & 3) == 1 ? D3D12_CULL_MODE_FRONT : (key.cull & 3) == 2 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
            d.RasterizerState.FrontCounterClockwise = (key.cull & 4) ? FALSE : TRUE;
            d.RasterizerState.DepthClipEnable = getenv("FCRECOMP_NOZCLIP") ? FALSE : TRUE;   // depuracao
            // Deslocamento de poligono (bug 19: mapas de sombra das luzes internas). O Xenos soma a profundidade
            // escala * inclinacao + deslocamento (deslocamento em unidades de profundidade; na D24 do PC 1 unidade = 2^-24).
            if (key.biasScale || key.biasOffset)
            {
                float sc, of;
                memcpy(&sc, &key.biasScale, 4); memcpy(&of, &key.biasOffset, 4);
                static const float div = getenv("FCRECOMP_POLYDIV") ? (float)atof(getenv("FCRECOMP_POLYDIV")) : 1.0f;
                d.RasterizerState.SlopeScaledDepthBias = sc / div;
                d.RasterizerState.DepthBias = (INT)std::lround(std::clamp(of * 16777216.0f, -1e9f, 1e9f));
            }

            // RB_DEPTHCONTROL: bit1 z ligado, bit2 escrita, bits 4-6 funcao
            uint32_t dc = key.depth;
            d.DepthStencilState.DepthEnable = key.hasDepth && (dc & 2) != 0;
            d.DepthStencilState.DepthWriteMask = (dc & 4) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
            d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC(D3D12_COMPARISON_FUNC_NEVER + ((dc >> 4) & 7));
            d.DSVFormat = key.hasDepth ? DS_FORMAT : DXGI_FORMAT_UNKNOWN;

            d.PrimitiveTopologyType = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)key.topology;
            d.NumRenderTargets = 1;
            d.RTVFormats[0] = (DXGI_FORMAT)key.rtFormat;
            d.SampleDesc.Count = 1;

            auto j = std::make_shared<PsoJob>();
            j->d = d;
            if (!assinc::Ligado(warm::g_loading))
            {
                double tPso = perf::Now();
                j->hr = g_dev->CreateGraphicsPipelineState(&j->d, IID_PPV_ARGS(&j->pso));
                j->ms = perf::Now() - tPso;
                return concluir(*j);
            }
            // os shaders vao junto (copias): o cache pode ser limpo (F5) enquanto o pipeline e criado
            j->vs = vs.dxil; j->ps = ps.dxil;
            j->d.VS = { j->vs.data(), j->vs.size() };
            j->d.PS = { j->ps.data(), j->ps.size() };
            j->tarefa = assinc::Enviar([j] {
                double tPso = perf::Now();
                j->hr = g_dev->CreateGraphicsPipelineState(&j->d, IID_PPV_ARGS(&j->pso));
                j->ms = perf::Now() - tPso;
                j->done = true;
            });
            if (!assinc::Esperar(j->done, j->tarefa)) { g_psoPendente[key] = j; return nullptr; }
            return concluir(*j);
        }

        // ------------------------------------------------------------ texturas
        struct FormatInfo { DXGI_FORMAT dxgi; uint32_t blockW, bytesPerBlock; };

        bool GetFormat(uint32_t f, FormatInfo& fi)
        {
            switch (f)
            {
            case 2:  fi = { DXGI_FORMAT_R8_UNORM, 1, 1 }; return true;
            case 3:  fi = { DXGI_FORMAT_B5G5R5A1_UNORM, 1, 2 }; return true;
            case 4:  fi = { DXGI_FORMAT_B5G6R5_UNORM, 1, 2 }; return true;
            case 6: case 50: fi = { DXGI_FORMAT_B8G8R8A8_UNORM, 1, 4 }; return true;
            case 7: case 54: fi = { DXGI_FORMAT_R10G10B10A2_UNORM, 1, 4 }; return true;
            case 10: fi = { DXGI_FORMAT_R8G8_UNORM, 1, 2 }; return true;
            case 15: fi = { DXGI_FORMAT_B4G4R4A4_UNORM, 1, 2 }; return true;
            case 18: case 51: fi = { DXGI_FORMAT_BC1_UNORM, 4, 8 }; return true;
            case 19: case 52: fi = { DXGI_FORMAT_BC2_UNORM, 4, 16 }; return true;
            case 20: case 53: fi = { DXGI_FORMAT_BC3_UNORM, 4, 16 }; return true;
            case 24: fi = { DXGI_FORMAT_R16_UNORM, 1, 2 }; return true;
            case 25: fi = { DXGI_FORMAT_R16G16_UNORM, 1, 4 }; return true;
            case 26: fi = { DXGI_FORMAT_R16G16B16A16_UNORM, 1, 8 }; return true;
            case 30: fi = { DXGI_FORMAT_R16_FLOAT, 1, 2 }; return true;
            case 31: fi = { DXGI_FORMAT_R16G16_FLOAT, 1, 4 }; return true;
            case 32: fi = { DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 8 }; return true;
            case 36: fi = { DXGI_FORMAT_R32_FLOAT, 1, 4 }; return true;
            case 37: fi = { DXGI_FORMAT_R32G32_FLOAT, 1, 8 }; return true;
            case 38: fi = { DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16 }; return true;
            case 49: fi = { DXGI_FORMAT_BC5_UNORM, 4, 16 }; return true;   // DXN
            default: return false;
            }
        }

        uint32_t Log2(uint32_t v) { uint32_t r = 0; while ((1u << r) < v) r++; return r; }

        uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2bpp)
        {
            uint32_t macro = ((y >> 5) * (pitch >> 5)) << (log2bpp + 7);
            uint32_t micro = ((y & 6) << 2) << log2bpp;
            uint32_t row = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2bpp)) + ((y & 1) << 4);
            macro = (x >> 5) << (log2bpp + 7);
            micro = (x & 7) << log2bpp;
            uint32_t off = row + macro + ((micro & ~0xFu) << 1) + (micro & 0xF);
            return ((off & ~0x1FFu) << 3) + ((off & 0x1C0) << 2) + (off & 0x3F) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
        }

        struct CachedTexture
        {
            ComPtr<ID3D12Resource> res;
            uint64_t hash = 0;
            DXGI_FORMAT format;
            uint32_t levels = 1;
            uint32_t stable = 0;        // quantas conferencias seguidas sem mudanca
            uint64_t lastCheck = 0;     // quadro da ultima conferencia
        };
        std::unordered_map<uint64_t, CachedTexture> g_textures;

        // Conferir o hash de toda textura em todo quadro custava ~20 ms com a agua. Texturas que nao mudam ha
        // 30 conferencias passam a ser conferidas a cada 8 quadros; as que mudam continuam todo quadro.
        bool SkipTextureCheck(CachedTexture& tex)
        {
            static const bool always = getenv("FCRECOMP_SEMPULO") != nullptr;   // teste: confere toda textura todo quadro
            return !always && tex.res && tex.stable >= 30 && g_frameNumber - tex.lastCheck < 8;
        }
        void NoteTextureCheck(CachedTexture& tex, bool changed)
        {
            tex.stable = changed ? 0 : tex.stable + 1;
            tex.lastCheck = g_frameNumber;
        }
        std::unordered_map<uint64_t, uint64_t> g_uploadedThisFrame;

        // texturas produzidas por resolves (endereco destino -> textura)
        struct ResolvedTexture
        {
            ComPtr<ID3D12Resource> res;
            DXGI_FORMAT format;
            uint32_t width, height;
            uint64_t seq = 0;   // ordem do resolve (o mais novo cobre os antigos na montagem do quadro)
            uint32_t meios = 2; // escala da superficie de origem (a textura tem Pm(width, meios) x Pm(height, meios))
            D3D12_RESOURCE_STATES state;
            uint64_t frame = 0; // quadro do ultimo resolve neste endereco
            // memoria do jogo no destino no momento do resolve (bug 15): se o jogo escreve outra coisa ali depois, a
            // copia da placa ja nao vale (ver ResolveVencido)
            uint32_t memBytes = 0;
            uint64_t memHash = 0, memChecked = ~0ull;
        };
        std::unordered_map<uint32_t, ResolvedTexture> g_resolved;
        uint64_t g_resolveSeq = 0;
        // texturas de resolve guardadas por (endereco, tamanho, formato): o jogo alterna tamanhos no mesmo endereco
        // (ex.: 768x720 e 1280x720) e recriar a textura a cada quadro custava caro
        struct PooledTexture { ComPtr<ID3D12Resource> res; D3D12_RESOURCE_STATES state; };
        std::map<std::tuple<uint32_t, uint32_t, uint32_t, DXGI_FORMAT>, PooledTexture> g_resolvePool;

        // Hash de TODOS os bytes (4 acumuladores em paralelo). Antes so 8 de cada 64 bytes eram lidos, e mudancas
        // pequenas (blocos do video Bink, letras novas na textura da fonte) passavam despercebidas.
        uint64_t FastHash(const uint8_t* p, size_t n)
        {
            perf::Scope hashScope(perf::TexHash);
            perf::g_hashMB += n / 1048576.0;
            perf::g_hashCalls++;
            const uint64_t K = 0x9E3779B97F4A7C15ull;
            uint64_t a = K ^ n, b = 0xC2B2AE3D27D4EB4Full, c = 0x165667B19E3779F9ull, d = 0x27D4EB2F165667C5ull;
            size_t i = 0;
            for (; i + 32 <= n; i += 32)
            {
                uint64_t v[4];
                memcpy(v, p + i, 32);
                a = (a ^ v[0]) * K; b = (b ^ v[1]) * K; c = (c ^ v[2]) * K; d = (d ^ v[3]) * K;
                a ^= a >> 31; b ^= b >> 31; c ^= c >> 31; d ^= d >> 31;
            }
            for (; i < n; i++) a = (a ^ p[i]) * K;
            uint64_t h = a ^ (b * 3) ^ (c * 7) ^ (d * 13);
            h ^= h >> 29; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 32;
            return h;
        }

        // Bug 15 (fundo do menu roxo/azul na ESCOLHA DE MAPA do multiplayer): o video Bink do menu grava um plano de cor
        // (640x360 em 02884000) num endereco onde antes houve um resolve de tela inteira. As copias de resolve ficam so na
        // placa do PC e sempre ganhavam da memoria: o video lia o resolve velho como cor. Agora guardamos uma "impressao"
        // da memoria do destino no resolve (amostras de 32 bytes a cada 1 KB, so a area que o resolve ocupa) e, quando o
        // jogo le a textura, se a memoria mudou o resolve e descartado e a textura vem da memoria, como no console (la o
        // resolve grava a memoria e quem escreve depois por cima ganha). Uma conferencia por endereco e por quadro.
        // FCRECOMP_RESOLVEFIXO=1 volta ao antigo.
        uint64_t AmostraMemoria(uint32_t addr, uint32_t bytes)
        {
            uint64_t h = 0x9E3779B97F4A7C15ull ^ bytes;
            for (uint32_t off = 0; off + 32 <= bytes; off += 1024)
            {
                uint64_t v[4];
                memcpy(v, Phys(addr + off), 32);
                for (uint64_t x : v) { h = (h ^ x) * 0xFF51AFD7ED558CCDull; h ^= h >> 31; }
            }
            return h;
        }
        void MarcarMemoriaDoResolve(ResolvedTexture& tex, uint32_t dest, uint32_t destInfo)
        {
            if (tex.format != COLOR_FORMAT) { tex.memBytes = 0; return; }   // copias de profundidade: a CPU nao escreve
            uint32_t bpp = ((destInfo >> 7) & 0x3F) == 6 ? 4 : 8;
            uint64_t bytes = (uint64_t)((tex.width + 31) & ~31u) * tex.height * bpp;
            bytes = std::min<uint64_t>(bytes, 0x20000000u - (dest & 0x1FFFFFFF));
            tex.memBytes = (uint32_t)bytes;
            tex.memHash = AmostraMemoria(dest, tex.memBytes);
            tex.memChecked = g_frameNumber;
        }
        bool ResolveVencido(uint32_t dest, ResolvedTexture& tex)
        {
            static const bool fixo = getenv("FCRECOMP_RESOLVEFIXO") != nullptr;
            if (fixo || !tex.memBytes || tex.memChecked == g_frameNumber) return false;
            tex.memChecked = g_frameNumber;
            if (AmostraMemoria(dest, tex.memBytes) == tex.memHash) return false;
            static int avisos = 0;
            if (avisos++ < 30)
                Log("[draw] resolve %ux%u em %08X (quadro %llu) descartado: o jogo escreveu outra coisa nessa memoria",
                    tex.width, tex.height, dest, (unsigned long long)tex.frame);
            return true;
        }

        // Texturas do jogo dentro de blocos grandes de memoria de video ("placed resources"). Criar cada textura como
        // recurso proprio (CreateCommittedResource) custava ~0,85 ms no driver: 173 texturas = 146 ms no primeiro quadro
        // da missao. Alocacao sequencial; o espaco de uma textura recriada (mudou de tamanho/formato) nao e reaproveitado,
        // o que e raro. FCRECOMP_TEXCOMMITTED=1 volta ao jeito antigo.
        HRESULT CreateTextureResource(const D3D12_RESOURCE_DESC& desc, ComPtr<ID3D12Resource>& out)
        {
            D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
            static const bool committed = getenv("FCRECOMP_TEXCOMMITTED") != nullptr;
            if (committed)
                return g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
            constexpr UINT64 HEAP_SIZE = 64ull << 20;
            static std::vector<ComPtr<ID3D12Heap>> heaps;
            static UINT64 used = HEAP_SIZE;   // forca criar o primeiro bloco
            D3D12_RESOURCE_DESC d = desc;
            d.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;   // 4 KB para texturas pequenas, se o driver aceitar
            D3D12_RESOURCE_ALLOCATION_INFO info = g_dev->GetResourceAllocationInfo(0, 1, &d);
            if (info.Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT)
            {
                d.Alignment = 0;
                info = g_dev->GetResourceAllocationInfo(0, 1, &d);
            }
            if (info.SizeInBytes > HEAP_SIZE / 2)   // muito grande: recurso proprio
                return g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
            UINT64 offset = (used + info.Alignment - 1) & ~(info.Alignment - 1);
            // Criar um bloco de 64 MB leva ~70 ms (o driver zera a memoria): o proximo e criado numa thread separada
            // quando o atual passa da metade, para nao travar o jogo quando o atual enche.
            static std::mutex spareMutex;
            static ComPtr<ID3D12Heap> spare;
            static bool spareRequested = false;
            D3D12_HEAP_DESC hd{ HEAP_SIZE, hp, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES };
            if (!heaps.empty() && used > HEAP_SIZE / 2 && !spareRequested)
            {
                spareRequested = true;
                std::thread([hd] {
                    ComPtr<ID3D12Heap> h;
                    g_dev->CreateHeap(&hd, IID_PPV_ARGS(&h));
                    std::lock_guard lock(spareMutex);
                    spare = h;
                }).detach();
            }
            if (offset + info.SizeInBytes > HEAP_SIZE)
            {
                ComPtr<ID3D12Heap> heap;
                {
                    std::lock_guard lock(spareMutex);
                    if (spare) { heap = std::move(spare); spareRequested = false; }
                }
                if (!heap && FAILED(g_dev->CreateHeap(&hd, IID_PPV_ARGS(&heap))))
                    return g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
                heaps.push_back(heap);
                Log("[draw] bloco de texturas %zu criado (%llu MB)", heaps.size(), (unsigned long long)(HEAP_SIZE >> 20));
                offset = 0;
            }
            HRESULT hr = g_dev->CreatePlacedResource(heaps.back().Get(), offset, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
            if (SUCCEEDED(hr)) used = offset + info.SizeInBytes;
            else hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
            return hr;
        }

        // Cria/atualiza a textura de uma fetch constant. Retorna nullptr se o formato nao e suportado.
        // Textura montada a partir de resolves dentro dela, quando a textura lida e maior que o resolve (ex.: a refracao
        // da agua le 1280x720, mas cada faixa do predicated tiling resolve so 768 ou 640 pixels a partir de x=0). A posicao de cada pedaco vem
        // do endereco (offset "tiled" com passo = largura), como na montagem da tela. Recria so quando um pedaco muda.
        struct ComposedTexture { ComPtr<ID3D12Resource> res; D3D12_RESOURCE_STATES state; uint64_t signature = 0; };
        std::unordered_map<uint64_t, ComposedTexture> g_composed;

        // Monta uma linha da textura num buffer comum (desfaz o tiling e troca o endian) e so depois copia para a
        // memoria de envio. A memoria de envio (heap UPLOAD) e "write-combined": ler dela e lentissimo, e trocar o
        // endian no proprio lugar lia cada byte de volta (17 ms por quadro com as texturas da agua).
        void PrepareRow(uint8_t* dst, const uint8_t* src, bool tiled, uint32_t cols, uint32_t bpb, uint32_t log2bpb,
            uint32_t x0, uint32_t y, uint32_t pitchBlocks, uint32_t pitchAligned, uint32_t endian)
        {
            struct T { double t = perf::Now(); ~T() { perf::g_texPrepMs += perf::Now() - t; } } timer;
            thread_local std::vector<uint8_t> row;
            uint32_t rowBytes = cols * bpb;
            if (row.size() < rowBytes) row.resize(rowBytes);
            uint8_t* r = row.data();
            if (!tiled)
                memcpy(r, src + ((size_t)y * pitchBlocks + x0) * bpb, rowBytes);
            else
                for (uint32_t x = 0; x < cols; x++)
                {
                    uint32_t blockIndex = TiledOffset(x + x0, y, pitchAligned, log2bpb) >> log2bpb;
                    memcpy(r + x * bpb, src + (size_t)blockIndex * bpb, bpb);
                }
            // endian: 1 = 8in16, 2 = 8in32, 3 = 16in32
            if (endian == 1)
                for (uint32_t i = 0; i + 1 < rowBytes; i += 2) std::swap(r[i], r[i + 1]);
            else if (endian == 2)
                for (uint32_t i = 0; i + 3 < rowBytes; i += 4) { uint32_t v; memcpy(&v, r + i, 4); v = __builtin_bswap32(v); memcpy(r + i, &v, 4); }
            else if (endian == 3)
                for (uint32_t i = 0; i + 3 < rowBytes; i += 4) { uint32_t v; memcpy(&v, r + i, 4); v = (v << 16) | (v >> 16); memcpy(r + i, &v, 4); }
            memcpy(dst, r, rowBytes);   // uma unica escrita sequencial na memoria de envio
        }

        ID3D12Resource* ComposeResolved(uint32_t base, uint32_t width, uint32_t height, ID3D12GraphicsCommandList* list)
        {
            uint32_t pitch = (width + 31) & ~31u;
            std::vector<std::pair<uint32_t, ResolvedTexture*>> pieces;
            uint64_t signature = 0;
            for (auto& [addr, tex] : g_resolved)
                if (addr >= base && addr < base + pitch * height * 4 && tex.format == COLOR_FORMAT && tex.res && tex.meios == g_meios)
                {
                    pieces.push_back({ addr, &tex });
                    signature = signature * 31 + tex.seq + addr;
                }
            if (pieces.empty()) return nullptr;
            // um pedaco so: apenas faixas de tela (mesma altura, ao menos metade da largura); outros casos usam o proprio
            // resolve (montar em tamanho cheio deixava o resto com lixo -> metade da tela rosa)
            if (pieces.size() == 1 && (pieces[0].second->height != height || pieces[0].second->width * 2 < width)) return nullptr;
            // um pedaco so tambem conta: o resolve de uma faixa (ex.: 640x720) lido como textura de 1280x720 fica na
            // posicao dele dentro de uma textura do tamanho pedido, em vez de ser esticado
            std::sort(pieces.begin(), pieces.end(), [](auto& a, auto& b) { return a.second->seq < b.second->seq; });

            auto& c = g_composed[((uint64_t)base << 32) ^ ((uint64_t)width << 16) ^ height];
            if (!c.res)
            {
                D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC td{};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                td.Width = Px(width); td.Height = Px(height); td.DepthOrArraySize = 1; td.MipLevels = 1; td.SampleDesc.Count = 1;
                td.Format = COLOR_FORMAT;
                td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;   // para poder zerar ao criar
                D3D12_CLEAR_VALUE cv{ COLOR_FORMAT, { 0, 0, 0, 0 } };
                { double tRt = perf::Now(); g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&c.res)); perf::g_rtCreateMs += perf::Now() - tRt; }
                {
                    // conteudo inicial de um recurso novo e indefinido: zera
                    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
                    rtv.ptr += (SIZE_T)MAX_SURFACES * g_rtvSize;   // slot temporario
                    g_dev->CreateRenderTargetView(c.res.Get(), nullptr, rtv);
                    float zero[4] = { 0, 0, 0, 0 };
                    list->ClearRenderTargetView(rtv, zero, 0, nullptr);
                }
                c.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
                c.signature = ~0ull;
            }
            if (c.signature != signature)
            {
                Transition(list, c.res.Get(), c.state, D3D12_RESOURCE_STATE_COPY_DEST);
                for (auto& [addr, tex] : pieces)
                {
                    uint32_t delta = addr - base, px = 0, py = 0;
                    bool found = false;
                    for (uint32_t y = 0; y < height && !found; y += 32)
                        for (uint32_t x = 0; x < pitch && !found; x += 32)
                            if (TiledOffset(x, y, pitch, 2) == delta) { px = x; py = y; found = true; }
                    if (!found || px >= width || py >= height) continue;
                    uint32_t w = std::min(Px(tex->width), Px(width) - Px(px)), h = std::min(Px(tex->height), Px(height) - Px(py));
                    Transition(list, tex->res.Get(), tex->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    D3D12_TEXTURE_COPY_LOCATION dst{ c.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                    D3D12_TEXTURE_COPY_LOCATION src{ tex->res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                    D3D12_BOX box{ 0, 0, 0, w, h, 1 };
                    list->CopyTextureRegion(&dst, Px(px), Px(py), 0, &src, &box);
                    Transition(list, tex->res.Get(), tex->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                }
                c.signature = signature;
            }
            Transition(list, c.res.Get(), c.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            return c.res.Get();
        }

        // Bug 16 (luneta da sniper/bazuca ficando branca no multiplayer): o resolve copia retangulos arredondados para
        // multiplos de 8 (ex.: 20x12 vira 24x16, 80x46 vira 80x48, 320x180 vira 320x184), mas o jogo le so o pedaco
        // util (textura de 20x12 no mesmo endereco). Entregar o resolve inteiro esticava as coordenadas: cada reducao
        // da medicao de brilho (1280x720 -> 320x180 -> 80x46 -> 20x12 -> 5x3) pegava uma area deslocada com bordas
        // vazias, a medicao saia escura demais (na luneta, ~0 na direita e embaixo) e a exposicao subia sem parar. Agora
        // a textura menor que o resolve recebe so o recorte do canto superior esquerdo (como a memoria no console).
        // FCRECOMP_SEMRECORTE=1 volta ao antigo.
        struct CroppedTexture { ComPtr<ID3D12Resource> res; D3D12_RESOURCE_STATES state; uint64_t seq = ~0ull; };
        std::unordered_map<uint64_t, CroppedTexture> g_cropped;

        ID3D12Resource* CropResolved(uint32_t addr, ResolvedTexture& r, uint32_t width, uint32_t height, ID3D12GraphicsCommandList* list)
        {
            static const bool off = getenv("FCRECOMP_SEMRECORTE") != nullptr;
            if (off) return nullptr;
            uint32_t pw = Pm(width, r.meios), ph = Pm(height, r.meios);
            auto& c = g_cropped[((uint64_t)addr << 32) ^ ((uint64_t)width << 16) ^ height];
            if (c.res)
            {
                D3D12_RESOURCE_DESC d = c.res->GetDesc();
                if (d.Width != pw || d.Height != ph || d.Format != r.format) c.res.Reset();   // escala/formato mudou
            }
            if (!c.res)
            {
                D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC td{};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                td.Width = pw; td.Height = ph; td.DepthOrArraySize = 1; td.MipLevels = 1; td.SampleDesc.Count = 1;
                td.Format = r.format;
                if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&c.res))))
                    return nullptr;
                c.state = D3D12_RESOURCE_STATE_COPY_DEST;
                c.seq = ~0ull;
            }
            if (c.seq != r.seq)
            {
                Transition(list, c.res.Get(), c.state, D3D12_RESOURCE_STATE_COPY_DEST);
                Transition(list, r.res.Get(), r.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst{ c.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                D3D12_TEXTURE_COPY_LOCATION src{ r.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                D3D12_BOX box{ 0, 0, 0, pw, ph, 1 };
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                Transition(list, r.res.Get(), r.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                c.seq = r.seq;
            }
            Transition(list, c.res.Get(), c.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            return c.res.Get();
        }

        // scaled: a textura devolvida e uma imagem da EDRAM (resolve, montagem ou imagem persistida), com Px() do tamanho
        // do jogo; os shaders usam o tamanho do jogo nas contas com o tamanho da textura
        ID3D12Resource* GetTexture(const uint32_t f[6], ID3D12GraphicsCommandList* list, DXGI_FORMAT& outFormat, bool& scaled)
        {
            scaled = false;
            if ((f[0] & 3) != 2) return nullptr;
            uint32_t base = f[1] & 0xFFFFF000;
            uint32_t format = f[1] & 0x3F;
            uint32_t endian = (f[1] >> 6) & 3;
            uint32_t width = (f[2] & 0x1FFF) + 1;
            uint32_t height = ((f[2] >> 13) & 0x1FFF) + 1;
            uint32_t pitch = ((f[0] >> 22) & 0x1FF) * 32;
            bool tiled = f[0] >> 31;
            if (!base) return nullptr;

            // textura de tela inteira que ninguem preencheu: usa a imagem persistida (veja g_persisted)
            static const bool noPersist = getenv("FCRECOMP_NOPERSIST") != nullptr;
            auto persistida = [&]() -> ID3D12Resource* {
                if (noPersist || !g_persisted || width != 1280 || height != 720 || format != 6 ||
                    !MemoryIsZero(Phys(base), (size_t)std::max(pitch, width) * height * 4)) return nullptr;
                g_usedPersistThisFrame = true;
                outFormat = COLOR_FORMAT;
                scaled = true;
                return g_persisted.Get();
            };
            // resolve velho no mesmo endereco (bug 13): a imagem guardada depois dele vale mais
            if (auto it = g_resolved.find(base & 0x1FFFFFFF); it != g_resolved.end() && it->second.width == 1280 &&
                it->second.height == 720 && g_persistedPropria && g_persistedFrame > it->second.frame)
                if (ID3D12Resource* p = persistida())
                {
                    static std::set<uint32_t> avisados;
                    if (avisados.insert(base & 0x1FFFFFFF).second)
                        Log("[draw] %08X: resolve do quadro %llu mais velho que a imagem persistida (quadro %llu); usando a imagem",
                            base & 0x1FFFFFFF, (unsigned long long)it->second.frame, (unsigned long long)g_persistedFrame);
                    return p;
                }

            // resolve velho por baixo de dados novos do jogo (bug 15): volta para o estoque e a textura vem da memoria
            if (auto it = g_resolved.find(base & 0x1FFFFFFF); it != g_resolved.end() && ResolveVencido(it->first, it->second))
            {
                auto& r = it->second;
                auto& vaga = g_resolvePool[{ it->first, r.width | (r.meios << 16), r.height, r.format }];
                // a placa ainda pode estar usando a textura: nunca soltar na hora (a da vaga ocupada espera 4 quadros)
                static std::vector<std::pair<uint64_t, ComPtr<ID3D12Resource>>> soltar;
                std::erase_if(soltar, [](auto& e) { return e.first + 4 < g_frameNumber; });
                if (vaga.res) soltar.push_back({ g_frameNumber, std::move(r.res) });
                else if (r.res) vaga = { std::move(r.res), r.state };
                g_resolved.erase(it);
            }

            // copia de um resolve?
            if (auto it = g_resolved.find(base & 0x1FFFFFFF); it != g_resolved.end())
            {
                auto& r = it->second;
                // a textura e maior que o resolve neste endereco: e uma imagem montada por faixas
                if (r.format == COLOR_FORMAT && (r.width < width || r.height < height))
                    if (ID3D12Resource* full = ComposeResolved(base & 0x1FFFFFFF, width, height, list))
                    {
                        outFormat = COLOR_FORMAT;
                        scaled = true;
                        return full;
                    }
                // a textura e menor que o resolve (arredondado para multiplos de 8): so o canto superior esquerdo
                if (r.format == COLOR_FORMAT && r.width >= width && r.height >= height && (r.width > width || r.height > height))
                    if (ID3D12Resource* crop = CropResolved(base & 0x1FFFFFFF, r, width, height, list))
                    {
                        outFormat = COLOR_FORMAT;
                        scaled = r.meios == g_meios;
                        return crop;
                    }
                Transition(list, r.res.Get(), r.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                outFormat = r.format;
                // copias de profundidade (mapa de sombras) ficam com o tamanho real para os shaders: o filtro do jogo (4
                // leituras a +-0,5 ponto e getWeights) passa a valer no mapa maior, com bordas mais finas
                scaled = r.format != DEPTH_COPY_FORMAT && r.meios == g_meios;
                if (g_statsOn) g_stats["textura de resolve"]++;
                if (r.width == 1280 && r.height == 720) g_fullResolveThisFrame = true;
                return r.res.Get();
            }
            if (ID3D12Resource* p = persistida()) return p;

            FormatInfo fi;
            if (!GetFormat(format, fi)) { diag::Note(diag::GpuFeature, "formato de textura " + std::to_string(format) + " (textura vazia)", std::to_string(width) + "x" + std::to_string(height)); if (g_statsOn) g_stats[std::string("textura formato ") + std::to_string(format)]++; return nullptr; }
            if (g_statsOn) g_stats[std::string("textura ok formato ") + std::to_string(format) + (tiled ? " tiled" : " linear")]++;
            if (!pitch) pitch = width;

            uint32_t wBlocks = (width + fi.blockW - 1) / fi.blockW;
            uint32_t hBlocks = (height + fi.blockW - 1) / fi.blockW;
            uint32_t pitchBlocks = std::max(pitch / fi.blockW, wBlocks);
            uint32_t pitchAligned = (pitchBlocks + 31) & ~31u;
            uint32_t hAligned = (hBlocks + 31) & ~31u;
            size_t srcSize = tiled ? (size_t)pitchAligned * hAligned * fi.bytesPerBlock : (size_t)pitchBlocks * hBlocks * fi.bytesPerBlock;
            if ((base & 0x1FFFFFFF) + srcSize > 0x20000000) return nullptr;
            // mipmaps (dword 4: mip_max_level bits 6-9; dword 5: packed_mips bit 11, mip_address bits 12-31)
            uint32_t mipAddr = f[5] & 0xFFFFF000;
            bool packed = (f[5] >> 11) & 1;
            auto log2Ceil = [](uint32_t v) { uint32_t l = 0; while ((1u << l) < v) l++; return l; };
            auto nextPow2 = [&](uint32_t v) { return 1u << log2Ceil(v); };
            uint32_t log2Max = 0;
            while ((2u << log2Max) <= std::max(width, height)) log2Max++;
            static const bool noMipsEnv = getenv("FCRECOMP_NOMIPS") != nullptr;   // depuracao (ou F6)
            bool noMips = noMipsEnv || diag::g_noMips;
            uint32_t maxLevel = (mipAddr && !noMips) ? std::min((f[4] >> 6) & 15, log2Max) : 0;
            uint32_t levels = maxLevel + 1;
            uint32_t log2Min = log2Ceil(std::min(width, height));
            uint32_t packedLevel = packed ? (log2Min > 4 ? log2Min - 4 : 0) : 99;
            // posicao (em blocos) de um nivel dentro da "cauda" de mips pequenos empacotados
            auto packedOffset = [&](uint32_t mip, uint32_t& xb, uint32_t& yb)
            {
                xb = yb = 0;
                uint32_t lw = log2Ceil(width), lh = log2Ceil(height);
                if (log2Min > 4 + mip) return;
                uint32_t pbase = log2Min > 4 ? log2Min - 4 : 0, pm = mip - pbase;
                if (pm < 3) { if (lw > lh) yb = 16 >> pm; else xb = 16 >> pm; }
                else if (lw > lh) xb = (1u << (lw - pbase)) >> (pm - 2);
                else yb = (1u << (lh - pbase)) >> (pm - 2);
                xb /= fi.blockW; yb /= fi.blockW;
            };

            // onde cada nivel esta na memoria do Xbox: endereco, passo (blocos), linhas e deslocamento na cauda
            struct Level { uint32_t addr, pitchBlocks, rows, ox, oy, wB, hB; };
            std::vector<Level> lv(levels);
            uint32_t mipOffset = 0;
            for (uint32_t L = 0; L < levels; L++)
            {
                Level& l = lv[L];
                l.wB = (std::max(width >> L, 1u) + fi.blockW - 1) / fi.blockW;
                l.hB = (std::max(height >> L, 1u) + fi.blockW - 1) / fi.blockW;
                l.ox = l.oy = 0;
                if (L >= packedLevel) packedOffset(L, l.ox, l.oy);
                if (L == 0)
                {
                    l.addr = base;
                    l.pitchBlocks = pitchBlocks;
                    l.rows = hBlocks;
                    continue;
                }
                // mips: passo de uma textura de lado potencia de 2, alinhado a 32x32 blocos;
                // os niveis guardados ficam em fatias seguidas (alinhadas a 4 KB) a partir de mip_address,
                // e os niveis menores dividem a fatia do nivel empacotado
                uint32_t s = std::min(L, packedLevel);
                uint32_t pw = (std::max(nextPow2(width) >> s, 1u) + fi.blockW - 1) / fi.blockW;
                uint32_t ph = (std::max(nextPow2(height) >> s, 1u) + fi.blockW - 1) / fi.blockW;
                l.pitchBlocks = (pw + 31) & ~31u;
                l.rows = (ph + 31) & ~31u;
                if (!tiled) l.pitchBlocks = (((l.pitchBlocks * fi.bytesPerBlock) + 255) & ~255u) / fi.bytesPerBlock;
                if (L <= packedLevel || L == 1)
                {
                    l.addr = mipAddr + mipOffset;
                    mipOffset += ((l.pitchBlocks * fi.bytesPerBlock * l.rows) + 4095) & ~4095u;
                }
                else
                    l.addr = lv[L - 1].addr;   // mesma cauda
            }
            if (packedLevel == 0) packedOffset(0, lv[0].ox, lv[0].oy);
            for (uint32_t L = 1; L < levels; L++)
            {
                size_t end = (size_t)(lv[L].addr & 0x1FFFFFFF) + (size_t)lv[L].pitchBlocks * lv[L].rows * fi.bytesPerBlock;
                if (end > 0x20000000) { levels = L; break; }
            }
            lv.resize(levels);
            const uint8_t* src = Phys(base);

            uint64_t key = ((uint64_t)base << 32) ^ ((uint64_t)format << 26) ^ ((uint64_t)width << 13) ^ height ^ ((uint64_t)levels << 58);
            auto& tex = g_textures[key];
            perf::Scope ps(perf::Texture);
            outFormat = fi.dxgi;
            // conferida ou enviada neste quadro: nao calcula o hash de novo
            if (tex.res && g_uploadedThisFrame.count(key)) return tex.res.Get();
            if (SkipTextureCheck(tex)) return tex.res.Get();
            uint64_t hash = FastHash(src, srcSize);
            if (levels > 1) hash ^= FastHash(Phys(mipAddr), mipOffset) * 31;
            g_uploadedThisFrame[key] = hash;
            NoteTextureCheck(tex, !tex.res || tex.hash != hash);
            if (tex.res && tex.hash == hash) return tex.res.Get();
            if (tex.res)
            {
                char k[96];
                snprintf(k, sizeof(k), "textura reenviada (mudou na memoria) %08X %ux%u formato %u %s", base, width, height, format, tiled ? "tiled" : "linear");
                diag::Note(diag::Other, k, std::to_string(srcSize) + " bytes");
            }

            bool created = false;
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = wBlocks * fi.blockW;
            td.Height = hBlocks * fi.blockW;
            td.DepthOrArraySize = 1;
            td.MipLevels = (UINT16)levels;
            td.Format = fi.dxgi;
            td.SampleDesc.Count = 1;
            if (!tex.res)
            {
                D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
                double tCreate = perf::Now();
                HRESULT hrCreate = CreateTextureResource(td, tex.res);
                perf::g_texCreateMs += perf::Now() - tCreate;
                if (FAILED(hrCreate))
                    return nullptr;
                created = true;
                perf::g_texNew++;
                static int logged = 0;
                if (logged++ < 40)
                    Log("[draw] textura %08X %ux%u formato %u: %u niveis (mip_address %08X, max %u, empacotados %u)",
                        base, width, height, format, levels, mipAddr, (f[4] >> 6) & 15, (unsigned)packed);
            }
            tex.hash = hash;
            tex.format = fi.dxgi;
            tex.levels = levels;

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16];
            UINT rowsFp[16];
            UINT64 rowSizes[16], total = 0;
            g_dev->GetCopyableFootprints(&td, 0, levels, 0, fp, rowsFp, rowSizes, &total);
            auto up = render::UploadAlloc((size_t)total, 512);
            perf::g_uploadMB += total / 1048576.0;
            perf::Scope uploadScope(perf::TexUpload);
            uint32_t log2bpb = Log2(fi.bytesPerBlock);
            if (!created)
            {
                D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
                b.Transition = { tex.res.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST };
                list->ResourceBarrier(1, &b);
            }
            for (uint32_t L = 0; L < levels; L++)
            {
                // desfaz o tiling e troca o endian para a memoria de upload
                const Level& l = lv[L];
                const uint8_t* lsrc = Phys(l.addr);
                uint32_t pitchAlignedL = (l.pitchBlocks + 31) & ~31u;
                uint32_t rowsCopy = std::min<uint32_t>(rowsFp[L], l.hB);
                uint32_t colsCopy = std::min<uint32_t>((uint32_t)(rowSizes[L] / fi.bytesPerBlock), l.wB);
                uint32_t rowBytes = colsCopy * fi.bytesPerBlock;
                (void)rowBytes;
                for (uint32_t y = 0; y < rowsCopy; y++)
                {
                    uint8_t* dst = (uint8_t*)up.cpu + fp[L].Offset + (size_t)y * fp[L].Footprint.RowPitch;
                    PrepareRow(dst, lsrc, tiled, colsCopy, fi.bytesPerBlock, log2bpb, l.ox, y + l.oy, l.pitchBlocks, pitchAlignedL, endian);
                }
                // depuracao: FCRECOMP_TEXDUMP=endereco (hex), ou ps=HASH (texturas enviadas nos desenhos desse pixel shader), salva cada nivel como .dds (ja decodificado
                // do tiling/endian) em diag\textura_ENDERECO_nN.dds, e o endereco de cada nivel no log
                static const char* texDump = getenv("FCRECOMP_TEXDUMP");
                bool wantDump = texDump && (strncmp(texDump, "ps=", 3) == 0 ? g_texDumpDraw : strtoul(texDump, nullptr, 16) == (base & 0x1FFFFFFF));
                if (wantDump && (fi.dxgi == DXGI_FORMAT_BC1_UNORM ||
                    fi.dxgi == DXGI_FORMAT_BC2_UNORM || fi.dxgi == DXGI_FORMAT_BC3_UNORM || fi.bytesPerBlock == 4))
                {
                    Log("[texdump] %08X nivel %u: endereco %08X passo %u blocos, %u linhas, deslocamento na cauda %u,%u, %ux%u blocos",
                        base, L, l.addr, l.pitchBlocks, l.rows, l.ox, l.oy, l.wB, l.hB);
                    char name[96];
                    snprintf(name, sizeof(name), "diag\\textura_%08X_n%u.dds", base & 0x1FFFFFFF, L);
                    CreateDirectoryA("diag", nullptr);
                    if (FILE* df = fopen(name, "wb"))
                    {
                        uint32_t w = colsCopy * fi.blockW, h = rowsCopy * fi.blockW, hdr[32] = {};
                        bool bc = fi.blockW == 4;
                        hdr[0] = 0x20534444; hdr[1] = 124; hdr[2] = 0x1007 | (bc ? 0x80000 : 0x8); hdr[3] = h; hdr[4] = w;
                        hdr[5] = bc ? colsCopy * rowsCopy * fi.bytesPerBlock : w * 4;
                        hdr[19] = 32;
                        if (bc) { hdr[20] = 4; hdr[21] = fi.dxgi == DXGI_FORMAT_BC1_UNORM ? 0x31545844 : fi.dxgi == DXGI_FORMAT_BC2_UNORM ? 0x33545844 : 0x35545844; }
                        else { hdr[20] = 0x41; hdr[22] = 32; hdr[23] = 0x00FF0000; hdr[24] = 0x0000FF00; hdr[25] = 0x000000FF; hdr[26] = 0xFF000000; }
                        hdr[27] = 0x1000;
                        fwrite(hdr, 4, 32, df);
                        for (uint32_t y = 0; y < rowsCopy; y++)
                            fwrite((uint8_t*)up.cpu + fp[L].Offset + (size_t)y * fp[L].Footprint.RowPitch, 1, (size_t)colsCopy * fi.bytesPerBlock, df);
                        fclose(df);
                    }
                }
                D3D12_TEXTURE_COPY_LOCATION dst{ tex.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                dst.SubresourceIndex = L;
                D3D12_TEXTURE_COPY_LOCATION srcLoc{ up.resource, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                srcLoc.PlacedFootprint = fp[L];
                srcLoc.PlacedFootprint.Offset += up.offset;
                list->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
            }
            D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
            b.Transition = { tex.res.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
            list->ResourceBarrier(1, &b);
            return tex.res.Get();
        }

        // Textura cubica: 6 faces (+X -X +Y -Y +Z -Z) em fatias seguidas na memoria do Xbox. Cada fatia ocupa a
        // face alinhada a 32x32 blocos e a 4 KB. So o nivel base por enquanto.
        ID3D12Resource* GetTextureCube(const uint32_t f[6], ID3D12GraphicsCommandList* list, DXGI_FORMAT& outFormat)
        {
            static const bool noCube = getenv("FCRECOMP_NOCUBE") != nullptr;   // depuracao
            if (noCube) return nullptr;
            if ((f[0] & 3) != 2) return nullptr;
            uint32_t base = f[1] & 0xFFFFF000;
            uint32_t format = f[1] & 0x3F;
            uint32_t endian = (f[1] >> 6) & 3;
            uint32_t width = (f[2] & 0x1FFF) + 1;
            uint32_t height = ((f[2] >> 13) & 0x1FFF) + 1;
            uint32_t pitch = ((f[0] >> 22) & 0x1FF) * 32;
            bool tiled = f[0] >> 31;
            if (!base) return nullptr;
            FormatInfo fi;
            if (!GetFormat(format, fi))
            {
                diag::Note(diag::GpuFeature, "formato de textura cubica " + std::to_string(format) + " (cubo vazio)");
                return nullptr;
            }
            if (!pitch) pitch = width;
            uint32_t wBlocks = (width + fi.blockW - 1) / fi.blockW;
            uint32_t hBlocks = (height + fi.blockW - 1) / fi.blockW;
            uint32_t pitchBlocks = std::max(pitch / fi.blockW, wBlocks);
            uint32_t pitchAligned = (pitchBlocks + 31) & ~31u;
            uint32_t rowsAligned = (hBlocks + 31) & ~31u;
            uint32_t rowPitchGuest = (tiled ? pitchAligned : pitchBlocks) * fi.bytesPerBlock;
            uint32_t faceStride = (rowPitchGuest * rowsAligned + 4095) & ~4095u;
            size_t total = (size_t)faceStride * 6;
            if ((base & 0x1FFFFFFF) + total > 0x20000000) return nullptr;
            const uint8_t* src = Phys(base);

            uint64_t key = ((uint64_t)base << 32) ^ ((uint64_t)format << 26) ^ ((uint64_t)width << 13) ^ height ^ (7ull << 58);   // 7 = cubo
            auto& tex = g_textures[key];
            perf::Scope ps(perf::Texture);
            outFormat = fi.dxgi;
            if (tex.res && g_uploadedThisFrame.count(key)) return tex.res.Get();
            if (SkipTextureCheck(tex)) return tex.res.Get();
            uint64_t hash = FastHash(src, total);
            g_uploadedThisFrame[key] = hash;
            NoteTextureCheck(tex, !tex.res || tex.hash != hash);
            if (tex.res && tex.hash == hash) return tex.res.Get();

            bool created = false;
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = wBlocks * fi.blockW;
            td.Height = hBlocks * fi.blockW;
            td.DepthOrArraySize = 6;
            td.MipLevels = 1;
            td.Format = fi.dxgi;
            td.SampleDesc.Count = 1;
            if (!tex.res)
            {
                D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
                double tCreate = perf::Now();
                HRESULT hrCreate = CreateTextureResource(td, tex.res);
                perf::g_texCreateMs += perf::Now() - tCreate;
                if (FAILED(hrCreate))
                    return nullptr;
                created = true;
                perf::g_texNew++;
                static int logged = 0;
                if (logged++ < 20)
                    Log("[draw] textura cubica %08X %ux%u formato %u (%s)", base, width, height, format, tiled ? "tiled" : "linear");
            }
            tex.hash = hash;
            tex.format = fi.dxgi;
            tex.levels = 1;

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[6];
            UINT rowsFp[6];
            UINT64 rowSizes[6], upTotal = 0;
            g_dev->GetCopyableFootprints(&td, 0, 6, 0, fp, rowsFp, rowSizes, &upTotal);
            auto up = render::UploadAlloc((size_t)upTotal, 512);
            uint32_t log2bpb = Log2(fi.bytesPerBlock);
            if (!created)
            {
                D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
                b.Transition = { tex.res.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST };
                list->ResourceBarrier(1, &b);
            }
            for (uint32_t face = 0; face < 6; face++)
            {
                const uint8_t* fsrc = src + (size_t)face * faceStride;
                uint32_t rows = std::min<uint32_t>(rowsFp[face], hBlocks);
                uint32_t cols = std::min<uint32_t>((uint32_t)(rowSizes[face] / fi.bytesPerBlock), wBlocks);
                uint32_t rowBytes = cols * fi.bytesPerBlock;
                (void)rowBytes;
                for (uint32_t y = 0; y < rows; y++)
                {
                    uint8_t* dst = (uint8_t*)up.cpu + fp[face].Offset + (size_t)y * fp[face].Footprint.RowPitch;
                    PrepareRow(dst, fsrc, tiled, cols, fi.bytesPerBlock, log2bpb, 0, y, rowPitchGuest / fi.bytesPerBlock, pitchAligned, endian);
                }
                D3D12_TEXTURE_COPY_LOCATION dst{ tex.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                dst.SubresourceIndex = face;   // 1 nivel: subrecurso = face
                D3D12_TEXTURE_COPY_LOCATION srcLoc{ up.resource, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                srcLoc.PlacedFootprint = fp[face];
                srcLoc.PlacedFootprint.Offset += up.offset;
                list->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
            }
            D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
            b.Transition = { tex.res.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
            list->ResourceBarrier(1, &b);
            return tex.res.Get();
        }

        D3D12_TEXTURE_ADDRESS_MODE AddressMode(uint32_t c)
        {
            switch (c)
            {
            case 0: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            case 1: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
            case 2: case 3: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            case 5: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
            default: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            }
        }

        // ---- captura de depuracao: FCRECOMP_GPUSHOT=N salva cada resolve de cor do quadro N em build/resolve_N_i_ENDERECO.bmp
        struct PendingShot { ComPtr<ID3D12Resource> readback; uint32_t dest, index, w, h, rowPitch; std::string dir; bool rgba8 = false; std::string nome; bool prof = false; };
        std::vector<PendingShot> g_pendingShots;

        // copia uma regiao de um recurso de cor (RGBA16F) para leitura na CPU; salvo no fim do quadro
        void CaptureRegion(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES& state,
                           uint32_t w, uint32_t h, uint32_t tag)
        {
            bool rgba8 = res->GetDesc().Format == DXGI_FORMAT_R8G8B8A8_UNORM;
            PendingShot s{ nullptr, tag, (uint32_t)g_pendingShots.size(), w, h, (w * (rgba8 ? 4 : 8) + 255) & ~255u };
            s.rgba8 = rgba8;
            D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = (UINT64)s.rowPitch * s.h;
            bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.readback));
            D3D12_RESOURCE_STATES old = state;
            Transition(list, res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{ s.readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            dst.PlacedFootprint.Footprint = { rgba8 ? DXGI_FORMAT_R8G8B8A8_UNORM : COLOR_FORMAT, s.w, s.h, 1, s.rowPitch };
            D3D12_TEXTURE_COPY_LOCATION src{ res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            D3D12_BOX box{ 0, 0, 0, std::min<UINT>(w, (UINT)res->GetDesc().Width), std::min<UINT>(h, res->GetDesc().Height), 1 };
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
            Transition(list, res, state, old);
            g_pendingShots.push_back(s);
        }

        void CaptureForDebug(ID3D12GraphicsCommandList* list, ResolvedTexture& tex, uint32_t dest)
        {
            static const char* env = getenv("FCRECOMP_GPUSHOT");
            bool diagShot = diag::Capturing(diag::CurrentFrame());   // F9 do diagnostico
            const bool prof = tex.format == DEPTH_COPY_FORMAT;   // mapas de sombra: salvos em cinza (so na captura F9)
            if (tex.format != COLOR_FORMAT && !(prof && diagShot)) return;
            if (!diagShot)
            {
                if (!env) return;
                // "0:N" = N-esimo quadro depois do primeiro com mais de 800 desenhos (dentro da missao)
                static uint64_t autoFrame = 0;
                uint64_t want = strtoull(env, nullptr, 10);
                if (want == 0)
                {
                    if (!autoFrame && g_drawHistory[(g_frameNumber + 3) & 3].size() > 800) autoFrame = g_frameNumber;
                    const char* c = strchr(env, ':');
                    want = autoFrame ? autoFrame + (c ? atoi(c + 1) : 0) : ~0ull;
                }
                if (g_frameNumber != want) return;
            }
            PendingShot s{ nullptr, dest, (uint32_t)g_pendingShots.size(), Pm(tex.width, tex.meios), Pm(tex.height, tex.meios), (Pm(tex.width, tex.meios) * 8 + 255) & ~255u };
            if (diagShot) s.dir = diag::CaptureDir(diag::CurrentFrame());
            if (prof) { s.prof = true; s.rowPitch = (s.w * 4 + 255) & ~255u; }
            D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = (UINT64)s.rowPitch * s.h;
            bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.readback));
            Transition(list, tex.res.Get(), tex.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{ s.readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            dst.PlacedFootprint.Footprint = { prof ? DEPTH_COPY_FORMAT : COLOR_FORMAT, s.w, s.h, 1, s.rowPitch };
            D3D12_TEXTURE_COPY_LOCATION src{ tex.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Transition(list, tex.res.Get(), tex.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            g_pendingShots.push_back(s);
        }

        float HalfToFloat(uint16_t h)
        {
            uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
            if (exp == 0) return (sign ? -1.f : 1.f) * man / 16777216.0f;
            if (exp == 31) return sign ? -65504.f : 65504.f;
            uint32_t bits = (sign << 31) | ((exp + 112) << 23) | (man << 13);
            float f; memcpy(&f, &bits, 4); return f;
        }

        // ---- resolves pequenos de volta para a memoria do Xbox
        // O jogo mede o brilho da cena reduzindo a imagem ate 8x8 (0254D000) e LE esse resultado na CPU para ajustar a
        // exposicao (adaptacao do olho). Os resolves ficavam so na placa de video do PC: a CPU lia lixo/zeros, achava a
        // cena escura e abria a exposicao -> imagem estourada (bug 12). Resolves de cor de ate 64x64 pixels sao copiados
        // para a memoria do jogo (tiled, no formato/ordem de bytes do RB_COPY_DEST_INFO) ~2 quadros depois.
        // FCRECOMP_SEMRETORNO=1 desliga.
        // w, h: tamanho do jogo (o que vai para a memoria); a copia lida tem Px(w) x Px(h) e cada pixel do jogo pega o
        // pixel Px(x), Px(y) dela
        // Crashes do multiplayer (LAN, 30/10-01/11): la o jogo faz varios resolves 8x8 em texturas temporarias e libera
        // a memoria logo depois de ler; a biblioteca de som reservava o mesmo pedaco para uma estrutura sua e o nosso
        // retorno atrasado gravava os pixels por cima dela (estrutura com lixo -> leitura invalida na thread de som).
        // Agora o retorno so grava se o pedaco ainda e da mesma reserva (GuestAllocSerial) e se ninguem mais escreveu
        // nesses bytes desde o resolve (copia "antes"); senao e descartado (o jogo ja nao usa aquela textura).
        struct PendingWriteback
        {
            ComPtr<ID3D12Resource> readback; uint32_t dest, w, h, rowPitch, destInfo, pitch; uint64_t serial;
            uint32_t dono = 0;              // numero da reserva da memoria no momento do resolve
            std::vector<uint8_t> antes;     // bytes do destino no momento do resolve (ou do ultimo retorno nosso)
        };
        std::vector<PendingWriteback> g_pendingWritebacks;
        std::vector<ComPtr<ID3D12Resource>> g_writebackPool;   // buffers de leitura de 128 KB reaproveitados

        float HalfToFloat(uint16_t h);

        // os bytes do destino que o retorno escreve (4 por pixel do jogo, na ordem x, y)
        void WritebackBytes(const PendingWriteback& wb, std::vector<uint8_t>& out)
        {
            out.resize((size_t)wb.w * wb.h * 4);
            for (uint32_t y = 0, i = 0; y < wb.h; y++)
                for (uint32_t x = 0; x < wb.w; x++, i += 4)
                    memcpy(&out[i], Phys(wb.dest + TiledOffset(x, y, wb.pitch, 2)), 4);
        }

        void QueueWriteback(ID3D12GraphicsCommandList* list, ResolvedTexture& tex, uint32_t dest, uint32_t destInfo, uint32_t destPitch)
        {
            static const bool off = getenv("FCRECOMP_SEMRETORNO") != nullptr;
            if (off || tex.format != COLOR_FORMAT || tex.width * tex.height > 64 * 64 || tex.meios != g_meios) return;
            PendingWriteback wb{ nullptr, dest, tex.width, tex.height, (Px(tex.width) * 8 + 255) & ~255u, destInfo, destPitch & 0x3FFF, render::FrameSerial() };
            wb.pitch = std::max<uint32_t>(32, (wb.pitch + 31) & ~31u);
            wb.dono = GuestAllocSerial(0xA0000000u | (dest & 0x1FFFFFFF));
            WritebackBytes(wb, wb.antes);
            if (!g_writebackPool.empty()) { wb.readback = std::move(g_writebackPool.back()); g_writebackPool.pop_back(); }
            else
            {
                D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
                D3D12_RESOURCE_DESC bd{};
                bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bd.Width = 128 * 1024;   // ate 128x128 RGBA16F (64x64 do jogo na escala 2x): 128 linhas de 1 KB
                bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
                bd.SampleDesc.Count = 1;
                bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&wb.readback));
                if (!wb.readback) return;
            }
            Transition(list, tex.res.Get(), tex.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{ wb.readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            dst.PlacedFootprint.Footprint = { COLOR_FORMAT, Px(wb.w), Px(wb.h), 1, wb.rowPitch };
            D3D12_TEXTURE_COPY_LOCATION src{ tex.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Transition(list, tex.res.Get(), tex.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            g_pendingWritebacks.push_back(std::move(wb));
        }

        // grava na memoria do jogo os resolves cuja lista de comandos ja terminou na placa de video
        void ProcessWritebacks()
        {
            uint64_t now = render::FrameSerial();
            for (size_t i = 0; i < g_pendingWritebacks.size();)
            {
                auto& wb = g_pendingWritebacks[i];
                if (wb.serial + 2 > now) { i++; continue; }
                std::vector<uint8_t> agora;
                WritebackBytes(wb, agora);
                if (wb.dono != GuestAllocSerial(0xA0000000u | (wb.dest & 0x1FFFFFFF)) || agora != wb.antes)
                {
                    static int avisos = 0;
                    if (avisos++ < 20)
                        Log("[draw] retorno do resolve %ux%u para %08X descartado: a memoria %s depois do resolve", wb.w, wb.h, wb.dest,
                            wb.dono != GuestAllocSerial(0xA0000000u | (wb.dest & 0x1FFFFFFF)) ? "foi liberada/reaproveitada" : "foi escrita por outro");
                    diag::Note(diag::Other, "retorno de resolve pequeno descartado (memoria reaproveitada pelo jogo)");
                    if (static const char* vig = getenv("FCRECOMP_VIGIARETORNO"); vig && avisos % 30 == 0)
                        Log("[retorno] %d descartes ate agora (ultimo %08X, quadro %llu)", avisos, wb.dest, (unsigned long long)g_frameNumber);
                    g_writebackPool.push_back(std::move(wb.readback));
                    g_pendingWritebacks.erase(g_pendingWritebacks.begin() + i);
                    continue;
                }
                uint32_t format = (wb.destInfo >> 7) & 0x3F, endian = wb.destInfo & 7;
                bool swapRB = (wb.destInfo >> 24) & 1;
                int32_t expBias = ((int32_t)(wb.destInfo << 10)) >> 26;   // bits 16-21 com sinal
                float scale = std::ldexp(1.0f, expBias);
                uint8_t* p = nullptr;
                D3D12_RANGE r{ 0, (SIZE_T)wb.rowPitch * Px(wb.h) };
                if (format == 6 && SUCCEEDED(wb.readback->Map(0, &r, (void**)&p)))   // 8_8_8_8 (o unico usado por enquanto)
                {
                    uint32_t pitch = wb.pitch;
                    for (uint32_t y = 0; y < wb.h; y++)
                        for (uint32_t x = 0; x < wb.w; x++)
                        {
                            const uint16_t* px = (const uint16_t*)(p + (size_t)Px(y) * wb.rowPitch + Px(x) * 8);
                            uint8_t c[4];
                            for (int k = 0; k < 4; k++)
                                c[k] = (uint8_t)std::lround(std::clamp(HalfToFloat(px[k]) * scale, 0.0f, 1.0f) * 255.0f);
                            if (swapRB) std::swap(c[0], c[2]);
                            // bytes como a GPU grava (dword little-endian R,G,B,A) e a troca de bytes pedida
                            static const int order[4][4] = { { 0, 1, 2, 3 }, { 1, 0, 3, 2 }, { 3, 2, 1, 0 }, { 2, 3, 0, 1 } };
                            uint8_t* out = Phys(wb.dest + TiledOffset(x, y, pitch, 2));
                            for (int k = 0; k < 4; k++) out[k] = c[order[endian & 3][k]];
                        }
                    D3D12_RANGE none{ 0, 0 };
                    wb.readback->Unmap(0, &none);
                    // FCRECOMP_VIGIARETORNO=quadro (bug 16): a partir desse quadro vigia quem le os retornos 8x8 (watch.cpp) e
                    // anota a cada 30 quadros os 5x3 primeiros pixels gravados (bytes na memoria, R G B A por pixel)
                    if (static const char* vig = getenv("FCRECOMP_VIGIARETORNO"); vig && wb.w == 8 && wb.h == 8 && g_frameNumber >= strtoull(vig, nullptr, 10))
                    {
                        static std::set<uint32_t> vigiados;
                        static const bool soLog = strchr(vig, 'L') != nullptr;   // "1L": so anota, sem vigiar
                        if (!soLog && vigiados.size() < 4 && vigiados.insert(wb.dest).second) WatchAddReadRange(0xA0000000u | wb.dest, 0x1000);
                        static std::map<uint32_t, uint32_t> vezes;
                        if (vezes[wb.dest]++ % 30 == 0)
                        {
                            std::string t;
                            for (uint32_t y = 0; y < 3; y++)
                            {
                                for (uint32_t x = 0; x < 5; x++)
                                {
                                    const uint8_t* q = Phys(wb.dest + TiledOffset(x, y, pitch, 2));
                                    char b[24]; snprintf(b, sizeof(b), " %02X%02X%02X%02X", q[0], q[1], q[2], q[3]); t += b;
                                }
                                t += " |";
                            }
                            Log("[retorno] quadro %llu %08X:%s", (unsigned long long)g_frameNumber, wb.dest, t.c_str());
                        }
                    }
                    // os proximos retornos para o mesmo lugar (resolve a cada quadro) esperam encontrar o que gravamos
                    for (auto& outro : g_pendingWritebacks)
                        if (&outro != &wb && outro.dest == wb.dest)
                            WritebackBytes(outro, outro.antes);
                    // a escrita foi nossa: o resolve continua valendo (bug 15)
                    if (auto it = g_resolved.find(wb.dest); it != g_resolved.end() && it->second.memBytes)
                        it->second.memHash = AmostraMemoria(wb.dest, it->second.memBytes);
                }
                else if (format != 6)
                {
                    static std::set<uint32_t> warned;
                    if (warned.insert(format).second) Log("[draw] resolve pequeno para %08X em formato %u: retorno para a memoria nao suportado", wb.dest, format);
                }
                g_writebackPool.push_back(std::move(wb.readback));
                g_pendingWritebacks.erase(g_pendingWritebacks.begin() + i);
            }
        }

        void SavePendingShots()
        {
            if (g_pendingShots.empty()) return;
            render::WaitIdle();
            for (auto& s : g_pendingShots)
            {
                uint8_t* p;
                s.readback->Map(0, nullptr, (void**)&p);
                char name[96];
                snprintf(name, sizeof(name), "resolve_%llu_%02u_%08X_%ux%u.bmp", (unsigned long long)g_frameNumber, s.index, s.dest, s.w, s.h);
                std::string path = !s.nome.empty() ? s.nome : s.dir.empty() ? std::string(name) : s.dir + "\\" + name;
                if (FILE* f = fopen(path.c_str(), "wb"))
                {
                    uint32_t size = s.w * s.h * 4;
                    BITMAPFILEHEADER fh{ 0x4D42, (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + size), 0, 0,
                        sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) };
                    BITMAPINFOHEADER ih{ sizeof(BITMAPINFOHEADER), (LONG)s.w, -(LONG)s.h, 1, 32 };
                    fwrite(&fh, sizeof(fh), 1, f);
                    fwrite(&ih, sizeof(ih), 1, f);
                    std::vector<uint8_t> row(s.w * 4);
                    // profundidade: cinza esticado entre o menor e o maior valor abaixo de 1 (1 = vazio, fica branco)
                    float pmin = 1, pmax = 0;
                    if (s.prof)
                        for (uint32_t y = 0; y < s.h; y++)
                            for (uint32_t x = 0; x < s.w; x++)
                            {
                                float v = ((const float*)(p + (size_t)y * s.rowPitch))[x];
                                if (v < 1.0f) { pmin = std::min(pmin, v); pmax = std::max(pmax, v); }
                            }
                    for (uint32_t y = 0; y < s.h; y++)
                    {
                        if (s.prof)
                        {
                            const float* srcf = (const float*)(p + (size_t)y * s.rowPitch);
                            for (uint32_t x = 0; x < s.w; x++)
                            {
                                float v = srcf[x] >= 1.0f ? 1.0f : pmax > pmin ? (srcf[x] - pmin) / (pmax - pmin) * 0.9f : 0.0f;
                                uint8_t g = (uint8_t)(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
                                row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = g; row[x * 4 + 3] = 255;
                            }
                            fwrite(row.data(), 1, row.size(), f);
                            continue;
                        }
                        if (s.rgba8)
                        {
                            const uint8_t* src8 = p + (size_t)y * s.rowPitch;
                            for (uint32_t x = 0; x < s.w; x++)
                                for (int c = 0; c < 4; c++) row[x * 4 + (c == 0 ? 2 : c == 2 ? 0 : c)] = src8[x * 4 + c];
                            fwrite(row.data(), 1, row.size(), f);
                            continue;
                        }
                        const uint16_t* src = (const uint16_t*)(p + (size_t)y * s.rowPitch);
                        for (uint32_t x = 0; x < s.w; x++)
                            for (int c = 0; c < 4; c++)
                            {
                                float v = std::clamp(HalfToFloat(src[x * 4 + c]), 0.0f, 1.0f);
                                row[x * 4 + (c == 0 ? 2 : c == 2 ? 0 : c)] = (uint8_t)(v * 255.0f + 0.5f);
                            }
                        fwrite(row.data(), 1, row.size(), f);
                    }
                    fclose(f);
                    Log("[draw] captura salva: %s", path.c_str());
                    if (s.prof) Log("[draw]   profundidade: de %g a %g (abaixo de 1)", pmin, pmax);
                    if (s.prof)   // valores crus (float32, linha a linha) para analisar fora do jogo
                        if (FILE* fr = fopen((path + ".f32").c_str(), "wb"))
                        {
                            for (uint32_t y = 0; y < s.h; y++) fwrite(p + (size_t)y * s.rowPitch, 4, s.w, fr);
                            fclose(fr);
                        }
                }
                if (!s.rgba8 && !s.prof && s.nome.empty())
                {
                    // valores crus do pixel central (para ver INF/NaN)
                    const uint16_t* c = (const uint16_t*)(p + (size_t)(s.h / 2) * s.rowPitch) + (s.w / 2) * 4;
                    Log("[draw]   pixel central de %s: %g %g %g %g (bits %04X %04X %04X %04X)", name,
                        HalfToFloat(c[0]), HalfToFloat(c[1]), HalfToFloat(c[2]), HalfToFloat(c[3]), c[0], c[1], c[2], c[3]);
                }
                s.readback->Unmap(0, nullptr);
            }
            g_pendingShots.clear();
        }

        // Retangulo do resolve: vertices do proprio desenho de copia (rect list, posicao 32_32_FLOAT no slot 0)
        bool ResolveRect(uint32_t& x0, uint32_t& y0, uint32_t& x1, uint32_t& y1)
        {
            uint32_t f0 = Reg(0x4800), f1 = Reg(0x4801);
            uint32_t addr = f0 & 0x1FFFFFFC;
            uint32_t endian = f1 & 3;
            if (!addr) return false;
            // os vertices estao em coordenadas de janela; na EDRAM somamos PA_SC_WINDOW_OFFSET (como nos desenhos)
            float offX = 0, offY = 0;
            if (Reg(0x2205) & (1u << 16))
            {
                uint32_t wo = Reg(0x2080);
                offX = (float)(int32_t(wo << 17) >> 17);
                offY = (float)(int32_t(wo << 1) >> 17);
            }
            float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
            for (int v = 0; v < 3; v++)
            {
                uint32_t raw[2];
                memcpy(raw, Phys(addr + v * 8), 8);
                float xy[2];
                for (int i = 0; i < 2; i++)
                {
                    uint32_t w = raw[i];
                    if (endian == 2) w = __builtin_bswap32(w);
                    else if (endian == 1) w = ((w & 0x00FF00FF) << 8) | ((w & 0xFF00FF00) >> 8);
                    else if (endian == 3) w = (w << 16) | (w >> 16);
                    memcpy(&xy[i], &w, 4);
                }
                xy[0] += offX;
                xy[1] += offY;
                minX = std::min(minX, xy[0]); maxX = std::max(maxX, xy[0]);
                minY = std::min(minY, xy[1]); maxY = std::max(maxY, xy[1]);
            }
            if (!(maxX > minX && maxY > minY) || minX < -4 || minY < -4 || maxX > CANVAS + 4 || maxY > CANVAS + 4)
                return false;
            x0 = (uint32_t)std::max(0.0f, minX); y0 = (uint32_t)std::max(0.0f, minY);
            x1 = std::min<uint32_t>(CANVAS, (uint32_t)(maxX + 0.5f)); y1 = std::min<uint32_t>(CANVAS, (uint32_t)(maxY + 0.5f));
            return x1 > x0 && y1 > y0;
        }
    }

    namespace
    {
        // Copia [addr, addr+bytes) da memoria fisica para o upload do quadro (uma vez por quadro por buffer).
        // Retorna a posicao da copia dentro do buffer de upload (o "mem" dos shaders).
        std::unordered_map<uint64_t, uint32_t> g_copiedThisFrame;

        uint64_t g_curVsHash = 0;   // shader de vertices do desenho atual (diagnostico)
        uint32_t CopyForGpu(uint32_t addr, uint32_t bytes)
        {
            perf::Scope ps(perf::Copy);
            addr &= 0x1FFFFFFC;
            bytes = std::min<uint32_t>(std::max(bytes, 16u), 32u << 20);
            if ((uint64_t)addr + bytes > 0x20000000) bytes = 0x20000000 - addr;
            // diagnostico: o desenho le vertices/indices de uma regiao que e destino de um resolve? (os resolves
            // ficam so na GPU e nao sao gravados na memoria do Xbox -> esses dados estariam velhos/zerados)
            for (auto& [dest, r] : g_resolved)
            {
                uint32_t size = r.width * r.height * 4;   // tamanho no Xbox (32 bits por pixel na maioria)
                if (addr < dest + size && dest < addr + bytes)
                {
                    char key2[96], det[96];
                    snprintf(key2, sizeof(key2), "vertices/indices lidos de destino de resolve %08X (%ux%u)", dest, r.width, r.height);
                    snprintf(det, sizeof(det), "leitura em %08X (%u bytes), vs=%016llX", addr, bytes, (unsigned long long)g_curVsHash);
                    diag::Note(diag::GpuFeature, key2, det);
                }
            }
            uint64_t key = ((uint64_t)addr << 32) | bytes;
            static const bool noCache = getenv("FCRECOMP_NOVBCACHE") != nullptr;   // teste: copia sempre
            // diagnostico (FCRECOMP_VBCHECK): a memoria mudou entre a copia deste quadro e um desenho seguinte?
            static const bool vbCheck = getenv("FCRECOMP_VBCHECK") != nullptr;
            static std::unordered_map<uint64_t, uint32_t> vbSums;
            auto sum = [&] { uint32_t h = 2166136261u; const uint8_t* m = Phys(addr); for (uint32_t i = 0; i < bytes; i++) h = (h ^ m[i]) * 16777619u; return h; };
            if (auto it = g_copiedThisFrame.find(key); !noCache && it != g_copiedThisFrame.end())
            {
                if (vbCheck)
                {
                    uint32_t h = sum();
                    if (vbSums[key] != h)
                    {
                        static int n = 0;
                        if (n++ < 40) Log("[vbcheck] quadro %llu: %08X+%X mudou depois da copia (desenho %zu)", (unsigned long long)g_frameNumber, addr, bytes, CurrentHistory().size());
                        vbSums[key] = h;
                    }
                }
                return it->second;
            }
            auto up = render::UploadAlloc(bytes, 16);
            memcpy(up.cpu, Phys(addr), bytes);
            if (vbCheck) vbSums[key] = sum();
            g_copiedThisFrame[key] = (uint32_t)up.offset;
            if (g_statsOn) g_stats["bytes de vertices/indices copiados"] += bytes;
            return (uint32_t)up.offset;
        }
    }

    namespace
    {
        // Tabelas de samplers reaproveitadas: o heap de samplers do D3D12 tem so 2048 entradas. Cada tabela guarda so
        // os slots ate o ultimo que o desenho usa (count). Antes cada tabela tinha os 32: so cabiam 64 tabelas, a selva
        // passava disso e a cada quadro o heap era zerado com uma espera completa pela GPU (render::ResetSamplers) ->
        // quadros de 66 ms (15 qps) no inicio da missao, e o resto daquele quadro usava samplers de outra tabela.
        // (A tabela do root signature declara 32: os slots alem de count nunca sao lidos pelo shader.)
        std::unordered_map<uint64_t, D3D12_GPU_DESCRIPTOR_HANDLE> g_samplerTables;

        D3D12_GPU_DESCRIPTOR_HANDLE GetSamplerTable(const D3D12_SAMPLER_DESC (&samplers)[32], uint32_t count)
        {
            count = std::max(1u, std::min(32u, count));
            uint64_t h = 1469598103934665603ull ^ count;
            const uint8_t* p = (const uint8_t*)samplers;
            for (size_t i = 0; i < count * sizeof(D3D12_SAMPLER_DESC); i++) { h ^= p[i]; h *= 1099511628211ull; }
            if (auto it = g_samplerTables.find(h); it != g_samplerTables.end()) return it->second;

            auto d = render::AllocSampler(count);
            if (!d.cpu.ptr)
            {
                // heap cheio: usa a tabela reserva (32 samplers validos) ate o fim do quadro e recomeca no proximo.
                // Item 41: antes usava "qualquer tabela existente", que podia ter menos entradas do que o desenho le.
                if (!g_samplerResetPending)
                    Log("[draw] heap de samplers cheio (%zu tabelas): recomeca no proximo quadro", g_samplerTables.size());
                diag::Note(diag::Other, "heap de samplers cheio (o quadro seguinte espera a GPU inteira)");
                g_samplerResetPending = true;
                return render::SamplerReserva();
            }
            for (uint32_t i = 0; i < count; i++)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE c = d.cpu;
                c.ptr += (SIZE_T)i * d.increment;
                g_dev->CreateSampler(&samplers[i], c);
            }
            g_samplerTables[h] = d.gpu;
            return d.gpu;
        }
    }

    // FCRECOMP_GPUPROF=1: perfilador da GPU. A cada 30 quadros mede com timestamps o tempo de GPU de cada desenho e
    // resolve, soma por par de shaders (vs/ps) e escreve no log a cada 8 medicoes as etapas mais caras. O quadro medido
    // espera a GPU terminar (o FPS cai um pouco enquanto o perfilador esta ligado).
    namespace gpuprof
    {
        constexpr UINT MAX_Q = 16384;
        ComPtr<ID3D12QueryHeap> g_heap;
        ComPtr<ID3D12Resource> g_readback;
        std::vector<std::string> g_labels;   // um rotulo por par de timestamps
        bool g_active = false;
        int g_samples = 0;
        double g_frameMs = 0;
        std::unordered_map<std::string, std::pair<double, uint32_t>> g_sum;   // rotulo -> (ms, desenhos)

        bool Enabled() { static const bool on = getenv("FCRECOMP_GPUPROF") != nullptr; return on; }

        void BeginFrame(uint64_t frame)
        {
            // FCRECOMP_GPUPROF=passo[:inicio-fim]: mede 1 a cada "passo" quadros (1 = todos), so entre esses quadros
            static const char* env = getenv("FCRECOMP_GPUPROF");
            static const uint64_t passo = env && atoi(env) > 0 ? atoi(env) : 30;
            static const uint64_t ini = env && strchr(env, ':') ? strtoull(strchr(env, ':') + 1, nullptr, 10) : 0;
            static const uint64_t fim = env && strchr(env, '-') ? strtoull(strchr(env, '-') + 1, nullptr, 10) : ~0ull;
            // GPUPICO=ms no ao_vivo.txt: mede todos os quadros e anota os desenhos dos que passarem desse tempo
            g_active = (Enabled() && frame % passo == 0 && frame >= ini && frame <= fim) || diag::Capturing(diag::CurrentFrame()) ||
                !AoVivoValor("GPUPICO").empty();
            g_labels.clear();
            if (!g_active || g_heap) return;
            D3D12_QUERY_HEAP_DESC qd{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, MAX_Q, 0 };
            render::Device()->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_heap));
            D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC bd{ D3D12_RESOURCE_DIMENSION_BUFFER, 0, MAX_Q * 8, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
            render::Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readback));
        }

        // marca antes (begin=true) e depois de uma etapa
        void Mark(ID3D12GraphicsCommandList* list, const std::string& label, bool begin)
        {
            if (!g_active || !g_heap) return;
            UINT idx = begin ? (UINT)g_labels.size() * 2 : (UINT)g_labels.size() * 2 - 1;   // fim: par do ultimo inicio
            if (idx >= MAX_Q || (!begin && g_labels.empty())) return;
            if (begin) g_labels.push_back(label);
            list->EndQuery(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, idx);
        }

        void BeforePresent(ID3D12GraphicsCommandList* list)
        {
            if (!g_active || !g_heap || g_labels.empty()) return;
            list->ResolveQueryData(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, (UINT)g_labels.size() * 2, g_readback.Get(), 0);
        }

        void AfterPresent()
        {
            if (!g_active || !g_heap || g_labels.empty()) return;
            render::WaitIdle();
            uint64_t* ts = nullptr;
            D3D12_RANGE r{ 0, g_labels.size() * 16 };
            if (FAILED(g_readback->Map(0, &r, (void**)&ts))) return;
            double toMs = 1000.0 / (double)render::TimestampFrequency();
            for (size_t i = 0; i < g_labels.size(); i++)
            {
                auto& e = g_sum[g_labels[i]];
                e.first += (ts[i * 2 + 1] - ts[i * 2]) * toMs;
                e.second++;
            }
            g_frameMs += (ts[g_labels.size() * 2 - 1] - ts[0]) * toMs;
            if (uint64_t shown = diag::CurrentFrame() - 1; diag::Capturing(shown))
                if (FILE* f = fopen((diag::CaptureDir(shown) + "\\tempos_gpu.txt").c_str(), "w"))
                {
                    // tempo de GPU de cada desenho, na ordem (e o intervalo desde o fim do desenho anterior)
                    for (size_t i = 0; i < g_labels.size(); i++)
                        fprintf(f, "%5zu %8.3f ms  (antes %7.3f ms)  %s\n", i, (ts[i * 2 + 1] - ts[i * 2]) * toMs,
                            i ? (ts[i * 2] - ts[i * 2 - 1]) * toMs : 0.0, g_labels[i].c_str());
                    fclose(f);
                }
            if (std::string pico = AoVivoValor("GPUPICO"); !pico.empty())
            {
                // quadro lento (GPU acima de GPUPICO ms): os 40 desenhos mais caros e a soma por pixel shader em
                // build_game\picos_gpu.txt (anexa); quadros normais so contam
                static uint64_t normais = 0, lentos = 0;
                const double tot = (ts[g_labels.size() * 2 - 1] - ts[0]) * toMs;
                if (tot < atof(pico.c_str())) normais++;
                else if (FILE* f = fopen("picos_gpu.txt", "a"))
                {
                    lentos++;
                    std::vector<std::pair<double, size_t>> v;
                    std::unordered_map<std::string, std::pair<double, int>> porPs;
                    double gaps = 0;
                    for (size_t i = 0; i < g_labels.size(); i++)
                    {
                        double ms = (ts[i * 2 + 1] - ts[i * 2]) * toMs;
                        if (i) gaps += (ts[i * 2] - ts[i * 2 - 1]) * toMs;
                        v.push_back({ ms, i });
                        const std::string& l = g_labels[i];
                        size_t a = l.find("ps="); std::string k = a == std::string::npos ? l.substr(0, 40) : l.substr(a, 19);
                        porPs[k].first += ms; porPs[k].second++;
                    }
                    std::sort(v.begin(), v.end(), [](auto& x, auto& y) { return x.first > y.first; });
                    fprintf(f, "=== quadro %llu: GPU %.1f ms do 1o ao ultimo desenho (%zu desenhos, %.1f ms entre desenhos); %llu quadros normais antes\n",
                        (unsigned long long)diag::CurrentFrame(), tot, g_labels.size(), gaps, (unsigned long long)normais);
                    for (size_t i = 0; i < v.size() && i < 40; i++)
                        fprintf(f, "  #%-5zu %7.3f ms  %s\n", v[i].second, v[i].first, g_labels[v[i].second].c_str());
                    std::vector<std::pair<std::string, std::pair<double, int>>> w(porPs.begin(), porPs.end());
                    std::sort(w.begin(), w.end(), [](auto& x, auto& y) { return x.second.first > y.second.first; });
                    for (size_t i = 0; i < w.size() && i < 12; i++)
                        fprintf(f, "  soma %7.3f ms  %4d desenhos  %s\n", w[i].second.first, w[i].second.second, w[i].first.c_str());
                    fclose(f);
                    normais = 0;
                }
                (void)lentos;
            }
            D3D12_RANGE none{ 0, 0 };
            g_readback->Unmap(0, &none);
            g_active = false;
            if (++g_samples < 8) return;
            std::vector<std::pair<std::string, std::pair<double, uint32_t>>> v(g_sum.begin(), g_sum.end());
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.first > b.second.first; });
            double total = 0;
            for (auto& e : v) total += e.second.first;
            Log("[gpuprof] GPU por quadro: %.1f ms do primeiro ao ultimo desenho, %.1f ms somando as etapas", g_frameMs / g_samples, total / g_samples);
            for (size_t i = 0; i < v.size() && i < 25; i++)
                Log("[gpuprof] %6.2f ms %5.1f%%  %5u desenhos  %s", v[i].second.first / g_samples, 100.0 * v[i].second.first / total,
                    v[i].second.second / g_samples, v[i].first.c_str());
            g_sum.clear(); g_samples = 0; g_frameMs = 0;
        }
    }

    // Recria o que as execucoes anteriores anotaram em build\warmcache (veja namespace warm)
    namespace
    {
        std::mutex g_warmStatusMutex;
        std::string g_warmStatus;
        void SetWarmStatus(const std::string& st) { std::lock_guard lock(g_warmStatusMutex); g_warmStatus = st; }
    }
    std::string WarmUpStatus() { std::lock_guard lock(g_warmStatusMutex); return g_warmStatus; }

    // 0 a 1. Um shader compilado conta como 3 pipelines (com o cache de shaders vazio e a parte demorada).
    float WarmUpProgress()
    {
        double st = g_warmShadersTotal, pt = g_warmPsoTotal;
        if (st + pt <= 0) return 0;
        return (float)std::min(1.0, (g_warmShadersDone * 3.0 + g_warmPsoDone) / (st * 3.0 + pt));
    }

    // Item 35: na primeira abertura (e depois de cada versao nova do exe) traduz e compila todos os pixel shaders do
    // shaders.dat da pasta do jogo (582 diferentes; 655 no arquivo) nas duas variantes (com e sem alpha test) e guarda
    // no shadercache. Assim, no meio do jogo, quase todo pixel shader novo sai do disco em poucos ms em vez de ~100 ms
    // compilando (as paradas e o chao azulado do item 34). shaders.fat: "DAT2", quantidade, e 16 bytes por arquivo
    // (posicao, tamanho, tamanho, hash do nome; nada comprimido). Cada arquivo e um shader do Xbox: palavra 0x102A0E00
    // (pixel; 0x102A0E01 = vertex), posicao e tamanho do microcode (big-endian). Os vertex shaders nao: o D3D do jogo
    // ajusta o microcode deles na hora de usar (nenhum dos usados bate com o arquivo). Nada do jogo vai no pacote: e
    // feito no computador de quem joga, com os arquivos dele. FCRECOMP_SEMPREPARO=1 desliga.
    // Chama fn(pixel, hash, microcode) para cada shader diferente do shaders.dat da pasta do jogo (formato acima)
    bool LerShadersDoJogo(const std::function<void(bool, uint64_t, std::vector<uint32_t>&)>& fn)
    {
        auto ler = [](const std::filesystem::path& p) {
            std::vector<uint8_t> v;
            if (FILE* f = _wfopen(p.c_str(), L"rb"))
            {
                fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
                if (n > 0) { v.resize(n); if (fread(v.data(), 1, n, f) != (size_t)n) v.clear(); }
                fclose(f);
            }
            return v;
        };
        auto be32 = [](const uint8_t* b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; };
        auto fat = ler(g_gameDir / "shaders.fat");
        auto dat = ler(g_gameDir / "shaders.dat");
        if (fat.size() < 8 || memcmp(fat.data(), "DAT2", 4) != 0 || dat.empty()) { Log("[preparo] shaders.fat/.dat em formato inesperado: pulando"); return false; }
        uint32_t n;
        memcpy(&n, fat.data() + 4, 4);
        if (fat.size() < 8 + (size_t)n * 16) { Log("[preparo] shaders.fat curto: pulando"); return false; }
        std::unordered_set<uint64_t> vistos;
        for (uint32_t i = 0; i < n; i++)
        {
            uint32_t off, tam;
            memcpy(&off, fat.data() + 8 + i * 16, 4);
            memcpy(&tam, fat.data() + 12 + i * 16, 4);
            if (tam < 12 || (uint64_t)off + tam > dat.size()) continue;
            const uint8_t* b = dat.data() + off;
            const uint32_t magia = be32(b);
            if (magia != 0x102A0E00 && magia != 0x102A0E01) continue;
            const bool pixel = magia == 0x102A0E00;
            uint32_t co = be32(b + 4), cs = be32(b + 8);
            if (!cs || cs % 4 || (uint64_t)co + cs > tam || cs > 0x40000) continue;
            std::vector<uint32_t> code(cs / 4);
            uint64_t h = 0xCBF29CE484222325ull;
            for (uint32_t k = 0; k < code.size(); k++)
            {
                code[k] = be32(b + co + k * 4);
                for (int j = 0; j < 4; j++) { h ^= (code[k] >> (j * 8)) & 0xFF; h *= 0x100000001B3ull; }
            }
            if (vistos.insert(h ^ (pixel ? 0 : 1)).second) fn(pixel, h, code);
        }
        return true;
    }

    void PrepararShadersDoJogo()
    {
        if (getenv("FCRECOMP_SEMPREPARO") || getenv("FCRECOMP_NOSHADERCACHE")) return;
        std::error_code ec;
        const auto tamDat = std::filesystem::file_size(g_gameDir / "shaders.dat", ec);
        if (ec) { Log("[preparo] shaders.dat nao encontrado na pasta do jogo: pulando"); return; }

        // marcador: tamanho do shaders.dat + data do exe (exe novo pode traduzir diferente: confere de novo; o que nao
        // mudou sai do disco)
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        auto dataExe = std::filesystem::last_write_time(exe, ec).time_since_epoch().count();
        char marca[96];
        snprintf(marca, sizeof(marca), "%llu %lld", (unsigned long long)tamDat, (long long)dataExe);
        const char* MARCA = "shadercache/jogo_preparado.txt";
        if (FILE* f = fopen(MARCA, "r"))
        {
            char l[96] = {};
            bool igual = fgets(l, sizeof(l), f) && strncmp(l, marca, strlen(marca)) == 0;
            fclose(f);
            if (igual) return;
        }

        double t0 = perf::Now();
        SetWarmStatus(TR("preparando os shaders do jogo (so na primeira vez)", "preparing the game's shaders (first time only)"));
        // 1) microcode de cada pixel shader (hash igual ao do gpu.cpp: FNV-1a dos bytes de cada palavra)
        std::vector<std::pair<uint64_t, std::vector<uint32_t>>> itens;
        if (!LerShadersDoJogo([&](bool pixel, uint64_t h, std::vector<uint32_t>& code) { if (pixel) itens.emplace_back(h, std::move(code)); }))
            return;

        // 2) traducao (numa thread so) e 3) compilacao em paralelo (render::CompileShader grava no shadercache)
        std::vector<std::string> hlsls;
        for (auto& [h, code] : itens)
            for (bool alpha : { false, true })
            {
                CompiledShader c; std::string avisos;
                hlsls.push_back(TranslateFor(ShaderRef{ h, &code }, true, alpha, c, avisos));
            }
        const double msTraducao = perf::Now() - t0;
        g_warmShadersDone = 0;
        g_warmShadersTotal = (uint32_t)hlsls.size();
        const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
        const unsigned workers = std::max(1u, cores - 1);   // um nucleo livre para a janela
        std::atomic<size_t> prox{ 0 };
        std::atomic<uint32_t> falhas{ 0 };
        auto work = [&] {
            for (size_t i; (i = prox++) < hlsls.size();)
            {
                std::string erros;
                if (render::CompileShader(hlsls[i], L"ps_6_0", &erros).empty()) falhas++;
                g_warmShadersDone++;
            }
        };
        std::vector<std::thread> pool;
        for (unsigned k = 1; k < workers; k++) pool.emplace_back(work);
        work();
        for (auto& th : pool) th.join();
        g_warmShadersDone = 0;
        g_warmShadersTotal = 0;

        if (!falhas)
        {
            CreateDirectoryA("shadercache", nullptr);
            if (FILE* f = fopen(MARCA, "w")) { fprintf(f, "%s\n", marca); fclose(f); }
        }
        Log("[preparo] %zu pixel shaders do shaders.dat (%zu variantes) prontos no shadercache em %.1f s (traducao %.1f s, %u threads, %u falharam)",
            itens.size(), hlsls.size(), (perf::Now() - t0) / 1000.0, msTraducao / 1000.0, workers, falhas.load());
    }

    // Item 38: receita dos shaders (receita_shaders.txt ao lado do exe, gerada por tools\gerar_receita.py a partir do
    // warmcache de roteiros jogados). Cada shader usado e "molde do shaders.dat + palavras que o D3D do jogo
    // preenche" (so posicao e valor: nada do jogo vai no pacote); mais os pipelines e superficies (so numeros). Aqui o
    // microcode e remontado com o shaders.dat de quem joga e tudo entra no warmcache, que o WarmUp logo abaixo compila
    // e cria antes da abertura: os shaders dos trechos da receita ficam prontos ja na primeira vez (sem as travadas de
    // compilacao). Refeito quando a receita, o shaders.dat ou o warmcache mudam. FCRECOMP_SEMRECEITA=1 desliga.
    void AplicarReceita()
    {
        if (getenv("FCRECOMP_SEMRECEITA") || !warm::Enabled()) return;
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const auto caminho = std::filesystem::path(exe).parent_path() / TR("receita_shaders.txt", "shader_recipe.txt");
        std::error_code ec;
        const auto tamReceita = std::filesystem::file_size(caminho, ec);
        if (ec) return;
        const auto tamDat = std::filesystem::file_size(g_gameDir / "shaders.dat", ec);
        if (ec) return;
        char marca[96];
        snprintf(marca, sizeof(marca), "%llu %llu", (unsigned long long)tamReceita, (unsigned long long)tamDat);
        const char* MARCA = "warmcache/receita_aplicada.txt";
        if (FILE* f = fopen(MARCA, "r"))
        {
            char l[96] = {};
            bool igual = fgets(l, sizeof(l), f) && strncmp(l, marca, strlen(marca)) == 0;
            fclose(f);
            if (igual) return;
        }

        double t0 = perf::Now();
        std::unordered_map<uint64_t, std::vector<uint32_t>> moldes[2];   // [pixel]
        if (!LerShadersDoJogo([&](bool pixel, uint64_t h, std::vector<uint32_t>& code) { moldes[pixel][h] = std::move(code); }))
            return;

        CreateDirectoryA("warmcache", nullptr);
        auto lerLinhas = [](const char* arq) {
            std::unordered_set<std::string> s;
            if (FILE* f = fopen(arq, "r"))
            {
                for (char l[256]; fgets(l, sizeof(l), f);) { std::string x = l; while (!x.empty() && (x.back() == '\n' || x.back() == '\r' || x.back() == ' ')) x.pop_back(); s.insert(x); }
                fclose(f);
            }
            return s;
        };
        auto pipes = lerLinhas("warmcache/pipelines.txt");
        auto sups = lerLinhas("warmcache/superficies.txt");
        FILE* fp = fopen("warmcache/pipelines.txt", "a");
        FILE* fs = fopen("warmcache/superficies.txt", "a");
        uint32_t nShaders = 0, jaTinha = 0, falhou = 0, nPipes = 0, nSups = 0;
        std::unordered_set<std::string> faltando;   // shaders que nao deu para montar: os pipelines deles ficam de fora
        std::ifstream in(caminho);
        for (std::string l; std::getline(in, l);)
        {
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
            if (l.size() > 2 && l[0] == 'S' && l[1] == ' ')
            {
                char tipo = 0; unsigned long long h = 0, molde = 0; unsigned n = 0; int lido = 0;
                if (sscanf(l.c_str(), "S %c %llX %llX %u%n", &tipo, &h, &molde, &n, &lido) < 4) continue;
                const bool pixel = tipo == 'p';
                char path[64];
                snprintf(path, sizeof(path), "warmcache/%s_%016llX.bin", pixel ? "ps" : "vs", h);
                if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) { jaTinha++; continue; }
                auto it = moldes[pixel].find(molde);
                char chave[24];
                snprintf(chave, sizeof(chave), "%c%016llX", tipo, h);
                // molde 0 (item 44): shader que nao existe em nenhum arquivo do jogo, vem completo na receita
                if (molde && (it == moldes[pixel].end() || it->second.size() != n)) { falhou++; faltando.insert(chave); continue; }
                if (!molde && (n == 0 || n > 0x10000)) { falhou++; faltando.insert(chave); continue; }
                std::vector<uint32_t> code = molde ? it->second : std::vector<uint32_t>(n, 0);
                bool ok = true;
                for (const char* q = l.c_str() + lido; *q;)
                {
                    unsigned k, v; int m = 0;
                    if (sscanf(q, " %X=%X%n", &k, &v, &m) < 2 || !m) break;
                    if (k >= code.size()) { ok = false; break; }
                    code[k] = v;
                    q += m;
                }
                uint64_t hh = 0xCBF29CE484222325ull;
                for (uint32_t w : code) for (int j = 0; j < 4; j++) { hh ^= (w >> (j * 8)) & 0xFF; hh *= 0x100000001B3ull; }
                if (!ok || hh != h) { falhou++; faltando.insert(chave); continue; }
                if (FILE* f = fopen(path, "wb")) { fwrite(code.data(), 4, code.size(), f); fclose(f); nShaders++; }
            }
            else if (l.size() > 2 && l[0] == 'P' && l[1] == ' ' && fp)
            {
                std::string x = l.substr(2);
                char v[17] = {}, ps[17] = {};
                if (sscanf(x.c_str(), "%16s %16s", v, ps) == 2 && (faltando.count(std::string("v") + v) || faltando.count(std::string("p") + ps))) continue;
                if (pipes.insert(x).second) { fprintf(fp, "%s\n", x.c_str()); nPipes++; }
            }
            else if (l.size() > 2 && l[0] == 'F' && l[1] == ' ' && fs)
            {
                std::string x = l.substr(2);
                if (sups.insert(x).second) { fprintf(fs, "%s\n", x.c_str()); nSups++; }
            }
        }
        if (fp) fclose(fp);
        if (fs) fclose(fs);
        if (FILE* f = fopen(MARCA, "w")) { fprintf(f, "%s\n", marca); fclose(f); }
        Log("[receita] %u shaders montados do shaders.dat (%u ja estavam no warmcache, %u falharam), %u pipelines e %u superficies acrescentados em %.0f ms",
            nShaders, jaTinha, falhou, nPipes, nSups, perf::Now() - t0);
    }

    // Item 39: coleta da receita enquanto se joga (coletar_receita.bat liga FCRECOMP_COLETARRECEITA=1). Grava
    // receitas\receita_<data>.txt (ao lado do warmcache) no formato do receita_shaders.txt: primeiro tudo o que o
    // warmcache ja tem e depois cada shader/pipeline/superficie novo assim que aparece. Os shaders vao como "molde do
    // shaders.dat + palavras preenchidas" (nada do jogo). tools\gerar_receita.py juntar <pasta> junta as receitas.
    namespace
    {
        std::mutex g_recMutex;
        FILE* g_recArq = nullptr;
        std::unordered_map<uint64_t, std::vector<std::pair<uint64_t, std::vector<uint32_t>>>> g_recMoldes;   // (pixel, tamanho)
        std::unordered_set<std::string> g_recVistos;
        uint32_t g_recShaders = 0, g_recSemMolde = 0, g_recOutros = 0;
        bool RecLigada() { static const bool on = getenv("FCRECOMP_COLETARRECEITA") != nullptr; return on; }
        void RecEscrever(const std::string& l)
        {
            if (g_recArq && g_recVistos.insert(l).second) { fprintf(g_recArq, "%s\n", l.c_str()); fflush(g_recArq); }
        }
        void RecShader(uint64_t h, bool pixel, const std::vector<uint32_t>& code)
        {
            auto it = g_recMoldes.find((pixel ? 1ull << 32 : 0) | code.size());
            const std::pair<uint64_t, std::vector<uint32_t>>* melhor = nullptr;
            size_t menos = SIZE_MAX;
            if (it != g_recMoldes.end())
                for (auto& m : it->second)
                {
                    size_t d = 0;
                    for (size_t k = 0; k < code.size() && d < menos; k++) d += code[k] != m.second[k];
                    if (d < menos) { menos = d; melhor = &m; if (!d) break; }
                }
            // sem molde parecido: fica de fora (ir completo seria codigo do jogo; decisao do usuario, item 44)
            if (!melhor || menos > std::max<size_t>(24, code.size() / 4)) { g_recSemMolde++; return; }
            char b[64];
            snprintf(b, sizeof(b), "S %c %016llX %016llX %zu", pixel ? 'p' : 'v', (unsigned long long)h, (unsigned long long)melhor->first, code.size());
            std::string l = b;
            for (size_t k = 0; k < code.size(); k++)
                if (code[k] != melhor->second[k]) { snprintf(b, sizeof(b), " %zX=%08X", k, code[k]); l += b; }
            if (g_recVistos.count(l)) return;
            RecEscrever(l);
            g_recShaders++;
        }
    }
    void ColetarShader(uint64_t hash, bool pixel, const std::vector<uint32_t>& code)
    {
        if (!RecLigada()) return;
        std::lock_guard lock(g_recMutex);
        if (g_recArq) RecShader(hash, pixel, code);
    }
    void ColetarReceita(const char* arquivo, const char* linha)
    {
        if (!RecLigada()) return;
        std::lock_guard lock(g_recMutex);
        if (!g_recArq) return;
        const size_t antes = g_recVistos.size();
        if (!strcmp(arquivo, "pipelines.txt")) RecEscrever(std::string("P ") + linha);
        else if (!strcmp(arquivo, "superficies.txt")) RecEscrever(std::string("F ") + linha);
        if (g_recVistos.size() != antes) g_recOutros++;
    }
    void IniciarColetaReceita()
    {
        if (!RecLigada()) return;
        std::lock_guard lock(g_recMutex);
        if (g_recArq) return;
        double t0 = perf::Now();
        if (!LerShadersDoJogo([&](bool pixel, uint64_t h, std::vector<uint32_t>& code) {
                g_recMoldes[(pixel ? 1ull << 32 : 0) | code.size()].emplace_back(h, std::move(code)); }))
            return;
        // versao em ingles: pasta "recipes" e arquivos recipe_<data>.txt (mesmo formato)
        CreateDirectoryA(TR("receitas", "recipes"), nullptr);
        SYSTEMTIME st;
        GetLocalTime(&st);
        char nome[96];
        snprintf(nome, sizeof(nome), TR("receitas/receita_", "recipes/recipe_") "%04u-%02u-%02u_%02u-%02u-%02u.txt", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        g_recArq = fopen(nome, "w");
        if (!g_recArq) { Log("[receita] nao consegui criar %s", nome); return; }
        fprintf(g_recArq, "%s\n%s\n", TR("RECEITA 1", "RECIPE 1"), TR("# coletada jogando (coletar_receita.bat); junte com: python tools\\gerar_receita.py juntar <pasta receitas>",
                                                "# recorded while playing (collect_recipe.bat); numbers only, no game data. Send the \"recipes\" folder to the developer."));
        // o que o warmcache ja tem (de execucoes anteriores)
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator("warmcache", ec))
        {
            const std::string n = e.path().filename().string();
            if (n.size() != 23 || (n[0] != 'p' && n[0] != 'v') || n.compare(1, 2, "s_") || n.compare(19, 4, ".bin")) continue;
            uint64_t h = strtoull(n.substr(3, 16).c_str(), nullptr, 16);
            std::vector<uint32_t> code;
            if (FILE* f = _wfopen(e.path().c_str(), L"rb"))
            {
                fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
                code.resize(sz / 4);
                if (fread(code.data(), 4, code.size(), f) != code.size()) code.clear();
                fclose(f);
            }
            if (!code.empty()) RecShader(h, n[0] == 'p', code);
        }
        for (auto [arq, prefixo] : { std::pair{ "warmcache/pipelines.txt", "P " }, std::pair{ "warmcache/superficies.txt", "F " } })
            if (FILE* f = fopen(arq, "r"))
            {
                for (char l[256]; fgets(l, sizeof(l), f);)
                {
                    std::string x = l;
                    while (!x.empty() && (x.back() == '\n' || x.back() == '\r' || x.back() == ' ')) x.pop_back();
                    if (!x.empty() && g_recVistos.insert(prefixo + x).second) { fprintf(g_recArq, "%s%s\n", prefixo, x.c_str()); g_recOutros++; }
                }
                fclose(f);
            }
        fflush(g_recArq);
        Log("[receita] coletando em %s: %u shaders e %u pipelines/superficies do warmcache (%u shaders sem molde no shaders.dat ficam de fora) em %.0f ms",
            nome, g_recShaders, g_recOutros, g_recSemMolde, perf::Now() - t0);
    }

    void WarmUp()
    {
        PrepararShadersDoJogo();
        AplicarReceita();
        if (!warm::Enabled()) return;
        SetWarmStatus(TR("preparando a placa de video", "preparing the graphics card"));
        double t0 = perf::Now();
        warm::g_loading = true;
        std::set<std::string> seen;   // linhas repetidas (varias execucoes anotando ao mesmo tempo)
        uint32_t surfaces = 0, pipelines = 0, failed = 0;
        if (FILE* f = fopen("warmcache/superficies.txt", "r"))
        {
            char type; unsigned key;
            while (fscanf(f, " %c %X", &type, &key) == 2)
            {
                if (((key >> 12) & 0x3FFF) > 2560) continue;   // lixo anotado por versoes antigas
                if (type == 'c') ColorSurface(key); else DepthSurface(key);
                surfaces++;
            }
            fclose(f);
        }
        static std::unordered_map<uint64_t, std::vector<uint32_t>> codes;   // o microcode precisa continuar vivo
        auto load = [&](uint64_t hash, bool pixel) -> const std::vector<uint32_t>* {
            uint64_t k = hash ^ (pixel ? 0x5050505050505050ull : 0);
            if (auto it = codes.find(k); it != codes.end()) return it->second.empty() ? nullptr : &it->second;
            auto& v = codes[k];
            char path[64];
            snprintf(path, sizeof(path), "warmcache/%s_%016llX.bin", pixel ? "ps" : "vs", (unsigned long long)hash);
            if (FILE* f = fopen(path, "rb"))
            {
                fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
                v.resize(n / 4);
                if (fread(v.data(), 4, v.size(), f) != v.size()) v.clear();
                fclose(f);
            }
            return v.empty() ? nullptr : &v;
        };
        if (FILE* f = fopen("warmcache/pipelines.txt", "r"))
        {
            std::vector<std::string> lines;
            for (char l[200]; fgets(l, sizeof(l), f);)
                if (seen.insert(l).second) lines.push_back(l);
            fclose(f);

            // 1) shaders: todos de uma vez, em paralelo (com o cache em disco vazio e a parte demorada)
            {
                std::vector<ShaderRequest> requests;
                for (const auto& line : lines)
                {
                    unsigned long long vsh, psh; unsigned alpha;
                    if (sscanf(line.c_str(), "%llX %llX %u", &vsh, &psh, &alpha) < 3) continue;
                    requests.push_back({ ShaderRef{ vsh, load(vsh, false) }, false, false });
                    requests.push_back({ ShaderRef{ psh, load(psh, true) }, true, alpha != 0 });
                }
                SetWarmStatus(TR("preparando a placa de video (shaders)", "preparing the graphics card (shaders)"));
                CompileShadersInParallel(requests);
            }

            // 2) pipelines
            uint32_t done = 0, total = (uint32_t)lines.size();
            g_warmPsoTotal = total;
            double lastPump = perf::Now();
            for (const auto& lineText : lines)
            {
                const char* line = lineText.c_str();
                done++;
                g_warmPsoDone = done;
                if (perf::Now() - lastPump > 30)
                {
                    lastPump = perf::Now();
                    SetWarmStatus(TR("preparando a placa de video ", "preparing the graphics card ") + std::to_string(done) + "/" + std::to_string(total));
                }
                unsigned long long vsh, psh; unsigned alpha, topo, blend, depth, mask, rect, hasDepth, rtFormat = COLOR_FORMAT, bs = 0, bo = 0;
                unsigned cull = 0;
                if (sscanf(line, "%llX %llX %u %u %X %X %X %u %u %u %X %X %X", &vsh, &psh, &alpha, &topo, &blend, &depth, &mask, &rect, &hasDepth, &rtFormat, &bs, &bo, &cull) < 9) continue;
                ShaderRef vr{ vsh, load(vsh, false) }, pr{ psh, load(psh, true) };
                if (!vr.code || !pr.code) { failed++; continue; }
                const auto& vs = GetShader(vr, false);
                const auto& ps = GetShader(pr, true, alpha != 0);
                if (vs.dxil.empty() || ps.dxil.empty()) { failed++; continue; }
                PsoKey key{ vsh, psh ^ (alpha ? 0xA1A1A1A100000000ull : 0), topo, blend, depth, mask, rect, hasDepth, rtFormat, bs, bo, cull };
                if (GetPso(key, vs, ps)) pipelines++; else failed++;
            }
        }
        warm::g_loading = false;
        IniciarColetaReceita();
        SetWarmStatus("");
        if (surfaces || pipelines)
            Log("[draw] aquecimento: %u superficies e %u pipelines recriados em %.0f ms (%u falharam)", surfaces, pipelines, perf::Now() - t0, failed);
    }

    uint32_t EscalaMeios()
    {
        static const uint32_t meios = [] {
            if (const char* e = getenv("FCRECOMP_ESCALA"))   // testes: 1, 1.5 ou 2
                return (uint32_t)std::clamp<long>(std::lround(atof(e) * 2), 2, 4);
            return (uint32_t)std::clamp(OpcoesAtuais().escala, 0, 2) + 2;
        }();
        return meios;
    }

    // Mapa de sombras em meios: opcao "sombras" (0 = como a tela, 1 = 2x, 2 = 3x) ou FCRECOMP_SOMBRAS=1|2|3 (vezes)
    uint32_t SombraMeios()
    {
        static const uint32_t meios = [] {
            if (const char* e = getenv("FCRECOMP_SOMBRAS"))
                return (uint32_t)std::clamp<long>(std::lround(atof(e) * 2), 2, 6);
            int s = std::clamp(OpcoesAtuais().sombras, 0, 2);
            return s ? (uint32_t)(s + 1) * 2 : 2u;
        }();
        return std::max(meios, EscalaMeios());
    }

    bool Init()
    {
        g_dev = render::Device();
        if (!g_dev) return false;
        g_meios = EscalaMeios();
        g_sombraMeios = SombraMeios();
        Log("[draw] resolucao interna %.1fx (%ux%u), mapa de sombras %.1fx", g_meios / 2.0, Px(1280), Px(720), g_sombraMeios / 2.0);

        // assinatura: b0 VS, b0 PS, b1 desenho, t0-t32, s0-s31
        D3D12_DESCRIPTOR_RANGE srvRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 65, 0, 0, 0 };   // memoria + 32 texturas 2D + 32 cubos
        D3D12_DESCRIPTOR_RANGE sampRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 32, 0, 0, 0 };
        D3D12_ROOT_PARAMETER p[5]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[0].Descriptor = { 0, 0 }; p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[1].Descriptor = { 0, 0 }; p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[2].Descriptor = { 1, 0 }; p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[3].DescriptorTable = { 1, &srvRange }; p[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[4].DescriptorTable = { 1, &sampRange }; p[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;   // vertex shaders tambem leem texturas
        D3D12_ROOT_SIGNATURE_DESC rs{ 5, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
        {
            Log("[draw] erro na assinatura: %s", err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
        g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_root));

        D3D12_DESCRIPTOR_HEAP_DESC rh{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, MAX_SURFACES + 1 }, dh{ D3D12_DESCRIPTOR_HEAP_TYPE_DSV, MAX_SURFACES };
        g_dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtvHeap));
        g_dev->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&g_dsvHeap));
        g_rtvSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        g_dsvSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        std::string errors;
        g_rectGs = render::CompileShader(RECT_GS_HLSL, L"gs_6_0", &errors);
        if (g_rectGs.empty())
            Log("[draw] GS de retangulo nao compilou: %s", errors.c_str());

        // pipeline da copia de profundidade
        {
            D3D12_DESCRIPTOR_RANGE r{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
            D3D12_ROOT_PARAMETER dp[2]{};
            dp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; dp[0].DescriptorTable = { 1, &r }; dp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            dp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; dp[1].Constants = { 0, 0, 2 }; dp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC drs{ 2, dp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
            ComPtr<ID3DBlob> b2, e2;
            D3D12SerializeRootSignature(&drs, D3D_ROOT_SIGNATURE_VERSION_1, &b2, &e2);
            g_dev->CreateRootSignature(0, b2->GetBufferPointer(), b2->GetBufferSize(), IID_PPV_ARGS(&g_depthCopyRoot));
            auto vs = render::CompileShader(DEPTH_COPY_HLSL, L"vs_6_0");
            auto ps = render::CompileShader(DEPTH_COPY_HLSL, L"ps_6_0");
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
            d.pRootSignature = g_depthCopyRoot.Get();
            d.VS = { vs.data(), vs.size() };
            d.PS = { ps.data(), ps.size() };
            d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            d.SampleMask = UINT_MAX;
            d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            d.NumRenderTargets = 1;
            d.RTVFormats[0] = DEPTH_COPY_FORMAT;
            d.SampleDesc.Count = 1;
            if (FAILED(g_dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&g_depthCopyPso))))
                Log("[draw] falha ao criar pipeline de copia de profundidade");
            // mesma ideia para cor: le a superficie (RGBA8) e grava na textura do resolve (RGBA16F)
            auto cps = render::CompileShader(COLOR_COPY_HLSL, L"ps_6_0");
            d.PS = { cps.data(), cps.size() };
            d.RTVFormats[0] = COLOR_FORMAT;
            if (cps.empty() || FAILED(g_dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&g_colorCopyPso))))
                Log("[draw] falha ao criar pipeline de copia de cor");
        }
        // pipeline da mistura da mascara do escuro (duas imagens e a fracao)
        {
            D3D12_DESCRIPTOR_RANGE r{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 };
            D3D12_ROOT_PARAMETER mp[2]{};
            mp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; mp[0].DescriptorTable = { 1, &r }; mp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            mp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; mp[1].Constants = { 0, 0, 1 }; mp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_STATIC_SAMPLER_DESC amostra{};
            amostra.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            amostra.AddressU = amostra.AddressV = amostra.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            amostra.MaxLOD = D3D12_FLOAT32_MAX;
            amostra.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC mrs{ 2, mp, 1, &amostra, D3D12_ROOT_SIGNATURE_FLAG_NONE };
            ComPtr<ID3DBlob> b3, e3;
            if (SUCCEEDED(D3D12SerializeRootSignature(&mrs, D3D_ROOT_SIGNATURE_VERSION_1, &b3, &e3)))
                g_dev->CreateRootSignature(0, b3->GetBufferPointer(), b3->GetBufferSize(), IID_PPV_ARGS(&g_misturaRoot));
            const char* MISTURA_HLSL = R"(
Texture2D a : register(t0);
Texture2D b : register(t1);
SamplerState s : register(s0);
cbuffer C : register(b0) { float t; };
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) { V o; o.uv = float2((id << 1) & 2, id & 2); o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }
float4 ps(V i) : SV_Target { return float4(lerp(a.Sample(s, i.uv).rgb, b.Sample(s, i.uv).rgb, t), 1); }
)";
            auto vs = render::CompileShader(MISTURA_HLSL, L"vs_6_0");
            auto ps = render::CompileShader(MISTURA_HLSL, L"ps_6_0");
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
            d.pRootSignature = g_misturaRoot.Get();
            d.VS = { vs.data(), vs.size() };
            d.PS = { ps.data(), ps.size() };
            d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            d.SampleMask = UINT_MAX;
            d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            d.NumRenderTargets = 1;
            d.RTVFormats[0] = COLOR_FORMAT;
            d.SampleDesc.Count = 1;
            if (!g_misturaRoot || vs.empty() || ps.empty() || FAILED(g_dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&g_misturaPso))))
                Log("[draw] falha ao criar pipeline da mistura (mascara do escuro)");
        }

        g_onGpuHang = OnGpuHang;
        Log("[draw] renderizacao Xenos pronta (EDRAM com superficies de %ux%u)", CANVAS, CANVAS);
        WarmUp();
        return true;
    }

    // Solta tudo que aponta para a memoria do Xbox. O driver AMD nao libera sozinho o heap criado sobre
    // o file mapping quando o processo termina (vazava ~0,7 GB de memoria do sistema por execucao).
    void Shutdown()
    {
        Log("[draw] liberando recursos da GPU...");
        render::WaitIdle();
        assinc::Parar();
        g_psoPendente.clear();
        g_root.Reset();
        g_textures.clear();
        g_resolved.clear();
        g_resolvePool.clear();
        g_composed.clear();
        g_colorSurfaces.clear();
        g_depthSurfaces.clear();
        g_psoCache.clear();
        Log("[draw] recursos da GPU liberados");
    }

    // ------------------------------------------------------------ consultas de oclusao
    // Antes respondiamos sempre "1000 amostras visiveis": os halos das lampadas (que o jogo so mostra se a caixa de
    // teste aparece) atravessavam paredes e modelos. Agora a GPU do PC conta de verdade; como o resultado so chega
    // ~2 quadros depois, o jogo recebe o do quadro anterior do mesmo bloco (cada lampada usa sempre o mesmo bloco).
    namespace
    {
        uint32_t g_queryBlock = 0;   // bloco da consulta aberta (0 = nenhuma)
        int g_queryIndex = -1;       // consulta do D3D12 aberta para ele (aberta no 1o desenho, so se houver desenho)
        uint32_t g_querySerial = 0;  // numero da consulta aberta (cresce a cada QueryBegin)
        bool g_queryPulado = false;  // um desenho da consulta aberta ficou de fora (shader compilando; item 34)
        std::mutex g_queryResultMutex;
        struct QueryCount { uint32_t samples, serial; };
        std::unordered_map<uint32_t, QueryCount> g_queryResults;
        bool g_queryInit = false;
        bool NovaConsulta() { static const bool antiga = getenv("FCRECOMP_OCLUSAOANTIGA") != nullptr; return !antiga; }
        // guarda a contagem se for de uma consulta mais nova que a guardada (as da GPU chegam ~2 quadros depois;
        // as vazias, na hora)
        void GuardarContagem(uint32_t block, uint32_t samples, uint32_t serial)
        {
            std::lock_guard lock(g_queryResultMutex);
            auto [it, novo] = g_queryResults.try_emplace(block, QueryCount{ samples, serial });
            if (!novo && (int32_t)(serial - it->second.serial) >= 0) it->second = { samples, serial };
        }
    }

    // FCRECOMP_OCLUSAOLOG=t4370-4445: registros das consultas so nesses tiques do roteiro
    bool OclusaoLogAgora()
    {
        static const char* q = getenv("FCRECOMP_OCLUSAOLOG");
        if (!q || q[0] != 't') return false;
        int64_t ini = atoll(q + 1), fim = strchr(q, '-') ? atoll(strchr(q, '-') + 1) : ini, tq = RoteiroTiqueAtual();
        return tq >= ini && tq <= fim;
    }

    int g_expMode = 0;
    void QueryBegin(uint32_t block)
    {
        if (g_expMode == 1) return;
        if (!g_queryInit)
        {
            g_queryInit = true;
            render::g_onOcclusion = [](uint64_t tag, uint64_t samples) {
                samples = (samples * 4 + g_meios * g_meios - 1) / (g_meios * g_meios);   // pixels da placa -> do jogo (resolucao interna; 1 visivel nao vira 0)
                GuardarContagem((uint32_t)tag, (uint32_t)std::min<uint64_t>(samples, 0x7FFFFFFF), (uint32_t)(tag >> 32));
                static const char* qlogEnv = getenv("FCRECOMP_OCLUSAOLOG");   // depuracao: cada contagem
                static int logged = 0;
                if (qlogEnv && (qlogEnv[0] == 't' ? OclusaoLogAgora() : true) && logged++ < 3000)
                    Log("[oclusao] bloco %08X: %llu amostras visiveis", (uint32_t)tag, (unsigned long long)samples);
            };
        }
        if (g_queryIndex >= 0) render::OcclusionEnd(g_queryIndex);
        g_queryBlock = block;
        g_queryIndex = -1;
        g_querySerial++;
        g_queryPulado = false;
    }

    void QueryEnd(uint32_t block)
    {
        if (g_queryIndex < 0 && g_queryBlock)
        {
            if (OclusaoLogAgora())
                Log("[oclusao] bloco %08X fechado sem nenhum desenho contado (%s)", g_queryBlock, NovaConsulta() ? "resposta 0" : "resultado anterior fica");
            // nada desenhado entre o inicio e o fim (ex.: a faixa 1 do tiling, cujos desenhos pulamos porque a faixa 0
            // ja desenhou a tela inteira; ou o jogo nao mandou a caixa): na GPU do console a contagem seria 0
            // (item 34: se o desenho ficou de fora por falta de shader, fica a contagem anterior)
            if (NovaConsulta() && !g_queryPulado) GuardarContagem(g_queryBlock, 0, g_querySerial);
        }
        if (g_queryIndex >= 0) render::OcclusionEnd(g_queryIndex);
        g_queryBlock = 0;
        g_queryIndex = -1;
    }

    uint32_t QueryResult(uint32_t block, uint32_t fallback)
    {
        std::lock_guard lock(g_queryResultMutex);
        auto it = g_queryResults.find(block);
        return it == g_queryResults.end() ? fallback : it->second.samples;
    }

    namespace
    {
        // limita o retangulo da limpeza ao tamanho da superficie (right == 0: limpa tudo) e passa para pixels da placa
        // (a limpeza e feita logo em seguida; o retangulo pendente seguinte comeca de novo no tamanho do jogo)
        template <class S> void ClearRect(S& sf)
        {
            if (!sf.clearRect.right) return;
            sf.clearRect.right = std::min<LONG>(sf.clearRect.right, (LONG)(sf.width ? sf.width : CANVAS));
            sf.clearRect.bottom = std::min<LONG>(sf.clearRect.bottom, (LONG)sf.height);
            if (sf.clearRect.right <= sf.clearRect.left || sf.clearRect.bottom <= sf.clearRect.top) { sf.clearRect = {}; return; }
            sf.clearRect = { PmL(sf.clearRect.left, sf.meios), PmL(sf.clearRect.top, sf.meios), PmL(sf.clearRect.right, sf.meios), PmL(sf.clearRect.bottom, sf.meios) };
        }
    }

    namespace
    {
        ID3D12Resource* AntialiasedScene(ID3D12GraphicsCommandList* list, ID3D12Resource* src)
        {
            if (g_aaLevel < 0)
                g_aaLevel = std::clamp(OpcoesAtuais().antisserrilhado, 0, 3);
            if (!g_aaLevel) return src;
            D3D12_RESOURCE_DESC sd = src->GetDesc();
            if (sd.Format != COLOR_FORMAT) return src;
            if (!g_aaScene || g_aaScene->GetDesc().Width != sd.Width || g_aaScene->GetDesc().Height != sd.Height)
            {
                ReleaseLater(std::move(g_aaScene));
                D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC td = sd;
                td.MipLevels = 1;
                td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&g_aaScene))))
                    return src;
                g_aaSceneState = D3D12_RESOURCE_STATE_RENDER_TARGET;
                Log("[draw] antisserrilhado nivel %d na cena 3D (%llux%u)", g_aaLevel, (unsigned long long)sd.Width, sd.Height);
            }
            Transition(list, g_aaScene.Get(), g_aaSceneState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += (SIZE_T)MAX_SURFACES * g_rtvSize;   // ultimo slot: alvo temporario
            g_dev->CreateRenderTargetView(g_aaScene.Get(), nullptr, rtv);
            bool ok = render::Antisserrilhar(list, src, rtv, (UINT)sd.Width, sd.Height, g_aaLevel);
            ForgetBoundState();   // o desenho do jogo reaplica tudo
            Transition(list, g_aaScene.Get(), g_aaSceneState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (diag::Capturing(diag::CurrentFrame())) HistoryText("ANTISSERRILHADO DA CENA");
            return ok ? g_aaScene.Get() : src;
        }
    }

    void Draw(const DrawInfo& info)
    {
        if (!g_root || !info.vs.code || !info.ps.code || info.count == 0) return;
        {
            static const char* texDump = getenv("FCRECOMP_TEXDUMP");
            g_texDumpDraw = texDump && strncmp(texDump, "ps=", 3) == 0 && strtoull(texDump + 3, nullptr, 16) == info.ps.hash;
            // depuracao: FCRECOMP_CONSTLOG=HASH do VS -> constantes nao nulas desse shader no log (3 desenhos)
            static const char* constLog = getenv("FCRECOMP_CONSTLOG");
            static int constLogged = 0;
            if (constLog && strtoull(constLog, nullptr, 16) == info.vs.hash && constLogged++ < 3)
                for (uint32_t i = 0; i < 256; i++)
                {
                    float v[4] = { RegF(0x4000 + i * 4), RegF(0x4001 + i * 4), RegF(0x4002 + i * 4), RegF(0x4003 + i * 4) };
                    if (v[0] || v[1] || v[2] || v[3]) Log("[const] vs c[%u] = %g %g %g %g", i, v[0], v[1], v[2], v[3]);
                }
        }
        // depuracao: FCRECOMP_SKIPPS=HASH[,HASH] pula os desenhos desses pixel shaders (para ver o que cada um desenha)
        // ao_vivo.txt CONSTVS=HASH: constantes do shader de vertices nos proximos 4 desenhos dele (c[0..255] nao nulas)
        if (std::string cv = AoVivoValor("CONSTVS"); !cv.empty())
        {
            static std::string ultimo; static int n = 0;
            if (cv != ultimo) { ultimo = cv; n = 0; }
            char h[20]; snprintf(h, sizeof(h), "%016llX", (unsigned long long)info.vs.hash);
            const char* nf = strstr(cv.c_str(), "N=");   // "HASH N=145": so os desenhos com essa contagem
            if (cv.find(h) != std::string::npos && (!nf || (uint32_t)atoi(nf + 2) == info.count) && n++ < 4)
                for (uint32_t i = 0; i < 256; i++)
                {
                    float v[4] = { RegF(0x4000 + i * 4), RegF(0x4001 + i * 4), RegF(0x4002 + i * 4), RegF(0x4003 + i * 4) };
                    if (v[0] || v[1] || v[2] || v[3]) Log("[aovivo] vs %s desenho %d c[%u] = %g %g %g %g", h, n, i, v[0], v[1], v[2], v[3]);
                }
        }
        if (AoVivoPular(info.vs.hash, info.ps.hash)) return;   // ao_vivo.txt: investigar com o jogo aberto
        static const char* skipPs = getenv("FCRECOMP_SKIPPS");
        if (skipPs)
        {
            char h[20];
            snprintf(h, sizeof(h), "%016llX", (unsigned long long)info.ps.hash);
            if (strstr(skipPs, h)) return;
        }
        g_curVsHash = info.vs.hash;
        // passada de tela (bug 28): ate 6 vertices e sem gravar profundidade (copias da cena, pos-processo, HUD).
        // O shader/pipeline dela nao pode ficar para depois (veja namespace assinc). FCRECOMP_SEMURGENTE=1 desliga.
        static const bool semUrgente = getenv("FCRECOMP_SEMURGENTE") != nullptr;
        // Item 34 (chao azulado na 21_DUNEA com os caches vazios): (1) desenho de dentro de uma consulta de oclusao que
        // fica de fora (caixa de teste sem shader) nao conta como 0 (o jogo escondia o objeto): vale a contagem anterior
        // (QueryEnd, g_queryPulado). Fazer essas na hora deixava a entrada da fase ~2 s mais longa; (2) os blocos do terreno
        // (faixa de triangulos com >= 8000 indices: 8446, grade 64x64, desenhados em varias camadas; sem uma delas
        // aparecia o fundo azulado) que ficam de fora fazem o quadro nao ser mostrado (assinc::FaltouGrande). Antes era
        // qualquer desenho com >= 2000: travadas de 0,1-0,4 s a cada objeto novo.
        static const bool semUrgente34 = getenv("FCRECOMP_SEMURGENTE34") != nullptr;
        assinc::g_urgente = !semUrgente && info.count <= 6 && (Reg(0x2200) & 4) == 0;
        const bool grande = !semUrgente34 && info.primType == 6 && info.count >= 8000;
        assinc::g_faltando.clear();
        struct FimUrgente { ~FimUrgente() { assinc::g_urgente = false; } } fimUrgente;
        double tShader = perf::Now();
        const auto& vs = GetShader(info.vs, false);
        const bool alphaTest = (Reg(0x2202) & 8) != 0;   // RB_COLORCONTROL: alpha test ligado
        const auto& ps = GetShader(info.ps, true, alphaTest);
        perf::g_ms[perf::Shader] += perf::Now() - tShader;
        static const bool puladosLog = getenv("FCRECOMP_PULADOSLOG") != nullptr;   // depuracao (item 34): desenhos pulados
        auto logPulado = [&](const char* porque) {
            if (!puladosLog) return;
            Log("[pulado] q%llu %s vs=%016llX ps=%016llX n=%u prim=%u prof=%08X blend=%08X mask=%X", (unsigned long long)g_frameNumber, porque,
                (unsigned long long)info.vs.hash, (unsigned long long)info.ps.hash, info.count, info.primType, Reg(0x2200), Reg(0x2201), Reg(0x2104) & 0xF);
        };
        if (vs.dxil.empty() || ps.dxil.empty()) { logPulado("shader"); if (grande) assinc::FaltouGrande(); if (!semUrgente34 && g_queryBlock) g_queryPulado = true; return; }

        // primitiva
        D3D12_PRIMITIVE_TOPOLOGY topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        D3D12_PRIMITIVE_TOPOLOGY_TYPE type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        uint32_t vertexCount = info.count, primMode = 0;
        bool rect = false;
        switch (info.primType)
        {
        case 1: topo = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; break;
        case 2: topo = D3D_PRIMITIVE_TOPOLOGY_LINELIST; type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
        case 3: topo = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
        case 4: break;
        case 5: primMode = 2; vertexCount = info.count >= 3 ? (info.count - 2) * 3 : 0; break;
        case 6: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
        case 8: rect = true; vertexCount = info.count / 3 * 3; break;
        case 13: primMode = 1; vertexCount = info.count / 4 * 6; break;
        default:
        {
            static int warned = 0;
            if (warned++ < 5) Log("[draw] primitiva %u nao suportada", info.primType);
            diag::Note(diag::GpuFeature, "tipo de primitiva " + std::to_string(info.primType) + " (desenho pulado)");
            return;
        }
        }
        if (!vertexCount) return;
        if (rect && g_rectGs.empty()) return;

        uint32_t modeControl = Reg(0x2208) & 7;
        uint32_t colorMask = modeControl == 5 ? 0 : (Reg(0x2104) & 0xF);
        uint32_t depthControl = Reg(0x2200);
        bool hasDepth = (depthControl & 6) != 0;   // teste ou escrita de profundidade
        static const bool noDepth = getenv("FCRECOMP_NODEPTH") != nullptr;   // depuracao: desliga a profundidade
        if (noDepth) { hasDepth = false; depthControl &= ~6u; }
        if (g_statsOn)
        {
            char k[128];
            snprintf(k, sizeof(k), "desenho modo %u corfmt %u msaa %u prof %u vte %X escala %.0f,%.0f desloc %.0f,%.0f janela %08X",
                modeControl, (Reg(0x2001) >> 16) & 0xF, (Reg(0x2000) >> 16) & 3, (depthControl >> 1) & 1, Reg(0x2206),
                RegF(0x210F), RegF(0x2111), RegF(0x2110), RegF(0x2112), Reg(0x2080));
            if (g_statsOn) g_stats[k]++;
            snprintf(k, sizeof(k), "profundidade: controle %08X info %08X modo %u", depthControl, Reg(0x2002), modeControl);
            if (g_statsOn) g_stats[k]++;
        }

        PsoKey key{ info.vs.hash, info.ps.hash ^ (alphaTest ? 0xA1A1A1A100000000ull : 0), (uint32_t)type, Reg(0x2201) | 0, depthControl & 0x77, colorMask, rect ? 1u : 0u, hasDepth ? 1u : 0u,
            (uint32_t)SurfaceFormat(ColorKey()) };
        // PA_SU_SC_MODE_CNTL bit 11/12: deslocamento de poligono na frente/atras (0x2380/81 frente, 0x2382/83 atras).
        // Sem corte por face aqui: vale o da frente (ou o de tras, se so ele estiver ligado). FCRECOMP_SEMPOLYOFFSET=1 desliga.
        if (static const bool semPoly = getenv("FCRECOMP_SEMPOLYOFFSET") != nullptr; !semPoly && hasDepth && (Reg(0x2205) & 0x1800))
        {
            uint32_t base = (Reg(0x2205) & 0x800) ? 0x2380 : 0x2382;
            key.biasScale = Reg(base); key.biasOffset = Reg(base + 1);
        }
        // corte por face (bug 23). So triangulos (pontos, linhas e retangulos nao tem face). FCRECOMP_SEMCORTE=1 desliga.
        if (static const bool semCorte = getenv("FCRECOMP_SEMCORTE") != nullptr; !semCorte && !rect && !AoVivoLigado("SEMCORTE") &&
            key.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE)
        {
            uint32_t m = Reg(0x2205) & 7;
            if ((m & 3) == 3) return;   // corta as duas faces: nada aparece
            if (m & 3) key.cull = m;
        }
        bool newPso = false;
        ID3D12PipelineState* pso = GetPso(key, vs, ps, &newPso);
        if (!pso) { logPulado("pipeline"); if (grande) assinc::FaltouGrande(); if (!semUrgente34 && g_queryBlock) g_queryPulado = true; return; }
        if (newPso)
        {
            char l[160];
            snprintf(l, sizeof(l), "%016llX %016llX %u %u %08X %X %X %u %u %u %X %X %X", (unsigned long long)info.vs.hash, (unsigned long long)info.ps.hash,
                alphaTest ? 1u : 0u, key.topology, key.blend, key.depth, key.mask, key.rect, key.hasDepth, key.rtFormat, key.biasScale, key.biasOffset, key.cull);
            warm::AppendLine("pipelines.txt", l);
        }

        auto* list = render::FrameList();
        g_drewThisFrame = true;
        g_drawsSinceResolve++;
        g_drawsThisFrame++;

        // Desenho sem efeito na imagem: nao grava cor, nem profundidade (RB_DEPTHCONTROL bit 1 teste + bit 2 escrita),
        // nem usa stencil (bit 0). No jogo sao as caixas das consultas de oclusao, que nos respondemos sempre
        // "visivel" sem contar pixels. Algumas cobriam a superficie inteira de 2048x2048 (~2-6 ms de GPU por quadro).
        // FCRECOMP_NOSKIPINVISIBLE=1 desenha mesmo assim.
        static const bool keepInvisible = getenv("FCRECOMP_NOSKIPINVISIBLE") != nullptr;
        // dentro de uma consulta de oclusao a caixa de teste precisa ser desenhada (a GPU conta os pixels dela)
        if (g_queryBlock && g_queryIndex < 0) g_queryIndex = render::OcclusionBegin(((uint64_t)g_querySerial << 32) | g_queryBlock);
        if (!keepInvisible && g_queryIndex < 0 && colorMask == 0 && (depthControl & 6) != 6 && !(depthControl & 1))
        {
            if (diag::Capturing(diag::CurrentFrame()))
                HistoryText("PULADO (sem efeito na imagem: sem cor, profundidade nem stencil)");
            return;
        }

        // superficies da EDRAM: RB_COLOR_INFO / RB_DEPTH_INFO, base nos bits 0-11
        Surface& color = ColorSurface(ColorKey());
        Transition(list, color.res.Get(), color.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (color.pendingClear)
        {
            uint32_t c = color.clearValue;
            float v[4] = { ((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, (c >> 24) / 255.0f };
            ClearRect(color);
            list->ClearRenderTargetView(color.view, v, color.clearRect.right ? 1 : 0, color.clearRect.right ? &color.clearRect : nullptr);
            color.pendingClear = false;
        }
        Surface* depth = nullptr;
        if (hasDepth)
        {
            depth = &DepthSurface(DepthKey());
            Transition(list, depth->res.Get(), depth->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            if (depth->pendingClear)
            {
                // profundidade 24 bits << 8 | stencil
                ClearRect(*depth);
                list->ClearDepthStencilView(depth->view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                    (depth->clearValue >> 8) / 16777215.0f, (UINT8)(depth->clearValue & 0xFF),
                    depth->clearRect.right ? 1 : 0, depth->clearRect.right ? &depth->clearRect : nullptr);
                depth->pendingClear = false;
            }
        }

        // texturas e samplers (32 slots, fetch constants em 0x4800 + 6*n) - antes das constantes, pois podem gravar comandos
        auto srv = render::AllocSrv(65);
        uint32_t usedMask = vs.textureMask | ps.textureMask, cubeMask = vs.cubeMask | ps.cubeMask;   // slots lidos pelos dois shaders
        uint32_t scaledMask = 0;   // slots com imagens da EDRAM (tamanho Px do jogo; veja GetTexture)
        D3D12_SAMPLER_DESC samplers[32]{};
        {
            // Modelo pronto (heap so da CPU) com as 65 entradas: memoria do quadro + 32 vistas 2D vazias + 32 cubos vazios.
            // Uma copia por desenho em vez de 65 CreateShaderResourceView (eram ~100 mil chamadas por quadro na selva);
            // depois so os slots usados recebem a textura de verdade. Um modelo por buffer de upload (alternam por quadro).
            struct Template { ComPtr<ID3D12DescriptorHeap> heap; };
            static std::unordered_map<ID3D12Resource*, Template> templates;
            ID3D12Resource* upload = render::UploadBuffer();
            auto& t = templates[upload];
            if (!t.heap)
            {
                D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65, D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
                g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&t.heap));
                D3D12_CPU_DESCRIPTOR_HANDLE base = t.heap->GetCPUDescriptorHandleForHeapStart();
                D3D12_SHADER_RESOURCE_VIEW_DESC memDesc{};
                memDesc.Format = DXGI_FORMAT_R32_TYPELESS;
                memDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                memDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                memDesc.Buffer.NumElements = (UINT)(render::UploadCapacity() / 4);
                memDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                g_dev->CreateShaderResourceView(upload, &memDesc, base);
                D3D12_SHADER_RESOURCE_VIEW_DESC nd{};
                nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                for (uint32_t i = 0; i < 64; i++)
                {
                    D3D12_CPU_DESCRIPTOR_HANDLE h = base;
                    h.ptr += (SIZE_T)(1 + i) * srv.increment;
                    nd.ViewDimension = i < 32 ? D3D12_SRV_DIMENSION_TEXTURE2D : D3D12_SRV_DIMENSION_TEXTURECUBE;
                    if (i < 32) nd.Texture2D.MipLevels = 1; else nd.TextureCube.MipLevels = 1;
                    g_dev->CreateShaderResourceView(nullptr, &nd, h);
                }
            }
            g_dev->CopyDescriptorsSimple(65, srv.cpu, t.heap->GetCPUDescriptorHandleForHeapStart(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        for (uint32_t s = 0; s < 32; s++)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE hc = srv.cpu;   // slot do cubo
            hc.ptr += (SIZE_T)(33 + s) * srv.increment;
            bool isCube = (cubeMask >> s) & 1;
            if (!(usedMask & (1u << s)))
            {
                // slot que nenhum dos shaders usa: sampler padrao (menos combinacoes de samplers)
                samplers[s].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                samplers[s].AddressU = samplers[s].AddressV = samplers[s].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                samplers[s].MaxLOD = D3D12_FLOAT32_MAX;
                continue;
            }
            uint32_t f[6];
            for (int i = 0; i < 6; i++) f[i] = Reg(0x4800 + s * 6 + i);
            // depuracao: FCRECOMP_CAPTURATEX=endereco captura (como o F9) o quadro seguinte ao primeiro desenho que usa essa
            // textura (efeitos rapidos, como o clarao do tiro, que caem em quadros diferentes a cada execucao)
            if (static const char* ct = getenv("FCRECOMP_CAPTURATEX"); ct)
            {
                static bool done = false;
                if (!done && ((f[1] & 0x1FFFF000) == (strtoul(ct, nullptr, 16) & 0x1FFFF000)))
                {
                    done = true;
                    const char* plus = strchr(ct, '+');   // "endereco+N": N quadros depois (padrao 1)
                    diag::CaptureAt(diag::CurrentFrame() + (plus ? strtoull(plus + 1, nullptr, 10) : 1));
                    Log("[draw] textura %s apareceu no quadro %llu: capturando o seguinte", ct, (unsigned long long)diag::CurrentFrame());
                }
            }
            // ao_vivo.txt TEXLOG=HASH (vs ou ps): constante de leitura de cada textura dos proximos 6 desenhos desse shader
            if (std::string tl = AoVivoValor("TEXLOG"); !tl.empty())
            {
                static std::string ultimo; static int n = 0; static uint64_t ultimoDesenho = ~0ull;
                if (tl != ultimo) { ultimo = tl; n = 0; }
                char hv[20], hp[20];
                snprintf(hv, sizeof(hv), "%016llX", (unsigned long long)info.vs.hash);
                snprintf(hp, sizeof(hp), "%016llX", (unsigned long long)info.ps.hash);
                if ((tl.find(hv) != std::string::npos || tl.find(hp) != std::string::npos) && n < 6)
                {
                    if (ultimoDesenho != g_drawsThisFrame) { ultimoDesenho = g_drawsThisFrame; n++; }
                    uint32_t swz = (f[3] >> 1) & 0xFFF;
                    Log("[aovivo] vs %s ps %s slot %u: %08X %08X %08X %08X %08X %08X ordem %c%c%c%c cor=%X", hv, hp, s, f[0], f[1], f[2], f[3], f[4], f[5],
                        "xyzw01??"[swz & 7], "xyzw01??"[(swz >> 3) & 7], "xyzw01??"[(swz >> 6) & 7], "xyzw01??"[(swz >> 9) & 7], colorMask);
                }
            }
            // diagnostico (FCRECOMP_GAMALOG): combinacoes de sinal das componentes (0 sem sinal, 1 com sinal, 2 com
            // vies, 3 GAMMA) por formato de textura e formato da superficie de cor
            if (static const bool gl = getenv("FCRECOMP_GAMALOG") != nullptr; gl)
            {
                static std::set<uint64_t> seen;
                uint32_t signs = (f[0] >> 2) & 0xFF, tfmt = f[1] & 0x3F, rt = (Reg(0x2001) >> 16) & 0xF;
                int32_t expAdj = ((int32_t)(f[3] << 13)) >> 26;   // bits 13-18 com sinal
                uint32_t swz = (f[3] >> 1) & 0xFFF;
                uint64_t k = ((uint64_t)swz << 40) | ((uint64_t)(uint8_t)expAdj << 32) | ((uint64_t)signs << 16) | (tfmt << 8) | rt;
                if (seen.insert(k).second)
                    Log("[gama] textura formato %u sinais x%u y%u z%u w%u expoente %d ordem %c%c%c%c (%03X) -> superficie formato %u (ex.: %08X)", tfmt, signs & 3, (signs >> 2) & 3, (signs >> 4) & 3, (signs >> 6) & 3, expAdj,
                        "xyzw01??"[swz & 7], "xyzw01??"[(swz >> 3) & 7], "xyzw01??"[(swz >> 6) & 7], "xyzw01??"[(swz >> 9) & 7], swz, rt, f[1] & 0x1FFFF000);
            }
            D3D12_CPU_DESCRIPTOR_HANDLE h = srv.cpu;
            h.ptr += (SIZE_T)(1 + s) * srv.increment;
            DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (isCube)
            {
                ID3D12Resource* tex = GetTextureCube(f, list, fmt);
                sd.Format = fmt;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                sd.TextureCube.MipLevels = 1;
                g_dev->CreateShaderResourceView(tex, &sd, hc);   // tex nulo = cubo vazio
            }
            else
            {
                bool scaled = false;
                ID3D12Resource* tex = GetTexture(f, list, fmt, scaled);
                if (scaled) scaledMask |= 1u << s;
                // a cena pronta lida para montar a tela: antisserrilhado (veja g_aaScene)
                if (tex && scaled && g_aaFrame != g_frameNumber && g_drawsThisFrame >= 200 && ((f[1] & 0x1FFFF000) == g_prevFrontBuffer[0] || (f[1] & 0x1FFFF000) == g_prevFrontBuffer[1]) &&
                    (f[2] & 0x1FFF) + 1 >= 1280 && ((f[2] >> 13) & 0x1FFF) + 1 >= 720)
                {
                    uint32_t tl = Reg(0x2081), br = Reg(0x2082);
                    if ((br & 0x7FFF) - std::min(br & 0x7FFF, tl & 0x7FFF) >= 1280 && ((br >> 16) & 0x7FFF) - std::min((br >> 16) & 0x7FFF, (tl >> 16) & 0x7FFF) >= 720)
                    {
                        g_aaFrame = g_frameNumber;
                        tex = AntialiasedScene(list, tex);
                    }
                }
                sd.Format = fmt;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = tex ? UINT(-1) : 1;   // todos os niveis
                g_dev->CreateShaderResourceView(tex, &sd, h);   // tex nulo = textura vazia
            }

            D3D12_SAMPLER_DESC& sdesc = samplers[s];
            uint32_t mag = (f[3] >> 19) & 3, min = (f[3] >> 21) & 3;
            // mip_filter (bits 23-24): 0 = ponto, 1 = linear, 2 = so o nivel base
            uint32_t mipFilter = (f[3] >> 23) & 3;
            bool linear = !(mag == 0 && min == 0);
            if (mipFilter == 1) sdesc.Filter = linear ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
            else sdesc.Filter = linear ? D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT;
            sdesc.MinLOD = (float)((f[4] >> 2) & 15);   // mip_min_level
            sdesc.AddressU = AddressMode((f[0] >> 10) & 7);
            sdesc.AddressV = AddressMode((f[0] >> 13) & 7);
            sdesc.AddressW = AddressMode((f[0] >> 16) & 7);
            sdesc.MaxLOD = mipFilter == 2 ? 0.0f : (float)((f[4] >> 6) & 15);   // mip_max_level
            if (sdesc.MinLOD > sdesc.MaxLOD) sdesc.MinLOD = sdesc.MaxLOD;
            if (colorMask == 0 && AoVivoLigado("SOMBRAMIP0")) sdesc.MaxLOD = sdesc.MinLOD;   // teste: sombras so com o nivel 0
            sdesc.MaxAnisotropy = 1;
            // cor da borda (dword 5, bits 0-1): 0 preto, 1 branco, 2/3 preto em YCbCr. Bug 19: os mapas de sombra das luzes
            // internas pedem borda BRANCA (fora do mapa = profundidade maxima = sem luz); com a borda sempre preta o chao
            // fora do facho da luminaria ficava aceso. FCRECOMP_BORDAPRETA=1 volta ao antigo.
            static const bool bordaPreta = getenv("FCRECOMP_BORDAPRETA") != nullptr;
            if ((f[5] & 3) == 1 && !bordaPreta)
                sdesc.BorderColor[0] = sdesc.BorderColor[1] = sdesc.BorderColor[2] = sdesc.BorderColor[3] = 1.0f;
            // Filtragem anisotropica (dword 3 bits 25-27: 0 desligada, 1..5 = ate 1:1, 2:1, 4:1, 8:1, 16:1) e ajuste do
            // nivel de mip (dword 4 bits 12-21, com sinal, em 1/32). Sem elas a agua vista quase rasante pegava um mip
            // pequeno demais: as ondas so apareciam bem perto. FCRECOMP_SEMANISO=1 volta ao antigo.
            static const bool noAniso = getenv("FCRECOMP_SEMANISO") != nullptr;
            if (!noAniso)
            {
                uint32_t aniso = (f[3] >> 25) & 7;
                if (aniso >= 2 && aniso <= 5 && mipFilter != 2)
                {
                    sdesc.Filter = D3D12_FILTER_ANISOTROPIC;
                    sdesc.MaxAnisotropy = 1u << (aniso - 1);
                }
                int32_t bias = int32_t(f[4] << 10) >> 22;
                sdesc.MipLODBias = std::clamp(bias / 32.0f, -16.0f, 15.99f);
                static uint64_t seen[4] = {};   // registra cada combinacao nova (anisotropia, ajuste) uma vez
                uint32_t combo = (aniso << 10) | ((uint32_t)bias & 0x3FF);
                if (!(seen[combo >> 11 & 3] & (1ull << (combo & 63))) && combo < 8192)
                {
                    seen[combo >> 11 & 3] |= 1ull << (combo & 63);
                    Log("[draw] sampler: anisotropia %u (%ux), ajuste de mip %+.2f (formato %u, %ux%u)", aniso, sdesc.MaxAnisotropy,
                        bias / 32.0f, f[1] & 63, (f[2] & 0x1FFF) + 1, ((f[2] >> 13) & 0x1FFF) + 1);
                }
            }
        }
        if (static const bool mipLog = getenv("FCRECOMP_MIPLOG") != nullptr; mipLog)   // depuracao: niveis de detalhe
            for (uint32_t s = 0; s < 32; s++)
                if (usedMask & (1u << s))
                {
                    static std::set<uint32_t> seen;
                    uint32_t f1 = Reg(0x4801 + s * 6), f2 = Reg(0x4802 + s * 6), f4 = Reg(0x4804 + s * 6), f5 = Reg(0x4805 + s * 6);
                    if (seen.size() < 5000 && seen.insert(f1 & 0x1FFFF000).second)
                        Log("[mip] textura %08X %ux%u formato %u: mip_max %u mip_min %u enderecoMips %08X | amostrador min %.0f max %.0f filtro %X ajuste %.2f | slot %u enderecamento %u/%u/%u borda %u",
                            f1 & 0x1FFFF000, (f2 & 0x1FFF) + 1, ((f2 >> 13) & 0x1FFF) + 1, f1 & 63, (f4 >> 6) & 15, (f4 >> 2) & 15, f5 & 0xFFFFF000,
                            samplers[s].MinLOD, samplers[s].MaxLOD, (unsigned)samplers[s].Filter, samplers[s].MipLODBias,
                            s, (Reg(0x4800 + s * 6) >> 10) & 7, (Reg(0x4800 + s * 6) >> 13) & 7, (Reg(0x4800 + s * 6) >> 16) & 7, f5 & 3);
                }
        uint32_t samplerCount = 0;   // ate o ultimo slot usado pelos shaders
        for (uint32_t s = 0; s < 32; s++) if (usedMask & (1u << s)) samplerCount = s + 1;
        D3D12_GPU_DESCRIPTOR_HANDLE samplerTable = GetSamplerTable(samplers, samplerCount);

        // constantes: VS em 0x4000 (256 float4), PS em 0x4400; booleanos 0x4900; loops 0x4908
        // Constantes: se o jogo nao escreveu nelas desde o ultimo envio (g_constVersion) e ainda e o mesmo quadro,
        // reaproveita o envio anterior. Senao copia trocando o endian 16 bytes por vez (antes: 2 x 1064 registradores
        // um a um em todo desenho, ~13 MB por quadro na selva).
        auto stageConsts = [&](uint32_t baseReg) -> D3D12_GPU_VIRTUAL_ADDRESS
        {
            static D3D12_GPU_VIRTUAL_ADDRESS lastAddr[2];
            static uint32_t lastVersion[2] = { ~0u, ~0u };
            static uint64_t lastSerial[2] = { ~0ull, ~0ull };
            int st = baseReg == 0x4400;
            uint64_t serial = render::FrameSerial();
            if (lastVersion[st] == g_constVersion[st] && lastSerial[st] == serial) return lastAddr[st];
            auto up = render::UploadAlloc(4096 + 32 + 128);
            const __m128i swap = _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
            auto copy = [&](uint32_t* dst, uint32_t reg, uint32_t n) {
                const uint32_t* src = (const uint32_t*)g_gpuRegsHost + reg;
                for (uint32_t i = 0; i < n; i += 4)
                    _mm_storeu_si128((__m128i*)(dst + i), _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(src + i)), swap));
            };
            uint32_t* d = (uint32_t*)up.cpu;
            copy(d, baseReg, 1024);
            copy(d + 1024, 0x4900, 8);
            copy(d + 1032, 0x4908, 32);
            lastAddr[st] = up.gpu; lastVersion[st] = g_constVersion[st]; lastSerial[st] = serial;
            return up.gpu;
        };
        D3D12_GPU_VIRTUAL_ADDRESS vsConsts = stageConsts(0x4000);
        D3D12_GPU_VIRTUAL_ADDRESS psConsts = stageConsts(0x4400);

        // dados do desenho (layout do cbuffer Draw em xenos_common.hlsl.h)
        auto up = render::UploadAlloc(1024);
        uint32_t* d = (uint32_t*)up.cpu;
        for (uint32_t i = 0; i < 192; i++) d[i] = Reg(0x4800 + i);   // fetch constants de vertice
        // os buffers de vertices usados pelo shader sao copiados para a memoria de upload do quadro e a
        // fetch constant passa a apontar para a copia (d0: endereco | tipo; d1: endian | tamanho em dwords)
        for (uint32_t slot : vs.fetchSlots)
        {
            uint32_t f0 = d[slot * 2], f1 = d[slot * 2 + 1];
            uint32_t addr = f0 & 0x1FFFFFFC, bytes = ((f1 >> 2) & 0xFFFFFF) * 4;
            d[slot * 2] = CopyForGpu(addr, bytes) | (f0 & 3);
        }
        // FCRECOMP_PARTLOG=hash do PS: no quadro capturado (F9/CAPTURATEX) anota, para cada desenho com esse PS, os 4
        // primeiros vertices crus do fetch 95 (6 dwords cada, como estao na memoria) e as constantes usadas pelas particulas
        if (static const char* pl = getenv("FCRECOMP_PARTLOG"); pl && info.ps.hash == strtoull(pl, nullptr, 16) && diag::Capturing(diag::CurrentFrame()))
        {
            uint32_t f0 = Reg(0x4800 + 95 * 2), f1 = Reg(0x4801 + 95 * 2);
            uint32_t a = f0 & 0x1FFFFFFC;
            Log("[part] desenho %zu: VGT_INDX_OFFSET %u MIN %u MAX %u | fetch95 %08X endian %u tamanho %u | VS c130 %g %g %g %g c15 %g %g %g %g c16 %g %g %g %g c17 %g %g | PS c2 %g %g %g c3 %g %g %g",
                CurrentHistory().size(), Reg(0x2102), Reg(0x2101), Reg(0x2100), a, f1 & 3, ((f1 >> 2) & 0xFFFFFF) * 4,
                RegF(0x4000 + 130 * 4), RegF(0x4001 + 130 * 4), RegF(0x4002 + 130 * 4), RegF(0x4003 + 130 * 4),
                RegF(0x4000 + 15 * 4), RegF(0x4001 + 15 * 4), RegF(0x4002 + 15 * 4), RegF(0x4003 + 15 * 4),
                RegF(0x4000 + 16 * 4), RegF(0x4001 + 16 * 4), RegF(0x4002 + 16 * 4), RegF(0x4003 + 16 * 4),
                RegF(0x4000 + 17 * 4), RegF(0x4001 + 17 * 4),
                RegF(0x4400 + 2 * 4), RegF(0x4401 + 2 * 4), RegF(0x4402 + 2 * 4), RegF(0x4400 + 3 * 4), RegF(0x4401 + 3 * 4), RegF(0x4402 + 3 * 4));
            for (uint32_t v = 0; v < 4; v++)
            {
                const uint8_t* m = Phys(a + v * 24);
                char line[160]; int o = 0;
                for (int k = 0; k < 24; k++) o += snprintf(line + o, sizeof(line) - o, "%02X%s", m[k], (k & 3) == 3 ? " " : "");
                Log("[part]   v%u: %s", v, line);
            }
        }
        // FCRECOMP_PSLOG=hash do PS: mostra as bases de constantes e c[0..7] do pixel shader (uma vez)
        static const char* psLog = getenv("FCRECOMP_PSLOG");
        if (psLog && info.ps.hash == strtoull(psLog, nullptr, 16))
        {
            static int n = 0;
            if (n++ % (getenv("FCRECOMP_PSDUMP") ? 25 : 500) == 0)
            {
                Log("[draw] PS %016llX: SQ_VS_CONST=%08X SQ_PS_CONST=%08X SQ_PROGRAM_CNTL=%08X", (unsigned long long)info.ps.hash,
                    Reg(0x2307), Reg(0x2308), Reg(0x2180));
                for (uint32_t t = 0; t < 4; t++)
                {
                    uint32_t f0 = Reg(0x4800 + t * 6), f1 = Reg(0x4801 + t * 6), f2 = Reg(0x4802 + t * 6), f3 = Reg(0x4803 + t * 6);
                    uint32_t b = f1 & 0x1FFFF000;
                    Log("[draw]   textura %u: tipo %u base %08X formato %u endian %u %ux%u tiled %u swizzle %03X %s", t, f0 & 3, b, f1 & 0x3F,
                        (f1 >> 6) & 3, (f2 & 0x1FFF) + 1, ((f2 >> 13) & 0x1FFF) + 1, f0 >> 31, (f3 >> 1) & 0xFFF,
                        g_resolved.count(b) ? "(RESOLVE)" : "");
                }
                // FCRECOMP_PSDUMP: salva os bytes das texturas lineares (ex.: planos do video) como .raw
                if (getenv("FCRECOMP_PSDUMP"))
                    for (uint32_t t = 0; t < 3; t++)
                    {
                        uint32_t f0 = Reg(0x4800 + t * 6), f1 = Reg(0x4801 + t * 6), f2 = Reg(0x4802 + t * 6);
                        uint32_t w = (f2 & 0x1FFF) + 1, h = ((f2 >> 13) & 0x1FFF) + 1, pitch = ((f0 >> 22) & 0x1FF) * 32;
                        if (pitch < w) pitch = w;
                        char name[64];
                        snprintf(name, sizeof(name), "plano_%u_%u_%ux%u_p%u.raw", n, t, w, h, pitch);
                        if (FILE* fp = fopen(name, "wb")) { fwrite(Phys(f1 & 0x1FFFF000), 1, (size_t)pitch * h, fp); fclose(fp); }
                    }
                for (uint32_t i : { 140u, 255u })
                    Log("[draw]   c[%u] do PS: %g %g %g %g", i, RegF(0x4400 + i * 4), RegF(0x4401 + i * 4), RegF(0x4402 + i * 4), RegF(0x4403 + i * 4));
                for (uint32_t i = 0; i < 8; i++)
                    Log("[draw]   c[%u] em 0x4400: %.3f %.3f %.3f %.3f | em 0x4000: %.3f %.3f %.3f %.3f", i,
                        RegF(0x4400 + i * 4), RegF(0x4401 + i * 4), RegF(0x4402 + i * 4), RegF(0x4403 + i * 4),
                        RegF(0x4000 + i * 4), RegF(0x4001 + i * 4), RegF(0x4002 + i * 4), RegF(0x4003 + i * 4));
            }
        }
        // FCRECOMP_VBLOG=hash do VS: mostra a fetch constant e os primeiros dwords do slot 0 (uma vez)
        // "c" na frente (ex.: c14F582C04D82EBA2): todos os desenhos desse VS no quadro da captura (FCRECOMP_CAPTURE/F9)
        static const char* vbLog = getenv("FCRECOMP_VBLOG");
        if (vbLog && info.vs.hash == strtoull(vbLog + (vbLog[0] == 'c'), nullptr, 16))
        {
            static int n = 0;
            if (vbLog[0] == 'c' ? diag::Capturing(diag::CurrentFrame()) : n++ < 3)
            {
                uint32_t slot = vs.fetchSlots.empty() ? 0 : vs.fetchSlots[0];
                uint32_t f0 = Reg(0x4800 + slot * 2), f1 = Reg(0x4801 + slot * 2), a = f0 & 0x1FFFFFFC;
                std::string vals;
                for (int i = 0; i < 24; i++) { char b[16]; snprintf(b, sizeof(b), " %08X", __builtin_bswap32(*(uint32_t*)Phys(a + i * 4))); vals += b; }
                Log("[draw] VB slot %u: fetch %08X %08X (tamanho %u dwords) dados:%s", slot, f0, f1, (f1 >> 2) & 0xFFFFFF, vals.c_str());
            }
        }
        d[192] = 0;
        d[193] = (info.index32 ? 1 : 0) | (info.indexEndian << 1);
        if (info.indexBase)
        {
            d[192] = CopyForGpu(info.indexBase, info.count * (info.index32 ? 4 : 2) + 4);
            d[193] |= 8;
        }
        // VGT_INDX_OFFSET: somado a cada indice (gerado ou lido do buffer). As particulas guardam todas num buffer so e
        // cada desenho comeca num ponto dele por esse registrador; com 0 todos liam os primeiros vertices (posicao, cor e
        // textura de outra particula: manchas rosa/azuis no clarao do tiro e na bola de fogo, bugs 13/17/22)
        static const bool noIndexOffset = getenv("FCRECOMP_SEMINDXOFFSET") != nullptr;
        d[194] = noIndexOffset ? 0 : Reg(0x2102);
        d[195] = primMode;
        float* fl = (float*)(d + 196);
        fl[0] = RegF(0x210F); fl[1] = RegF(0x2111); fl[2] = RegF(0x2113); fl[3] = 0;   // escala x, y, z
        fl[4] = RegF(0x2110); fl[5] = RegF(0x2112); fl[6] = RegF(0x2114); fl[7] = 0;   // offset x, y, z
        fl[8] = (float)CANVAS; fl[9] = (float)CANVAS; fl[10] = 0; fl[11] = 0;
        // PA_SC_WINDOW_OFFSET (x bits 0-14, y bits 16-30, com sinal), se PA_SU_SC_MODE_CNTL.vtx_window_offset_enable (bit 16)
        if (Reg(0x2205) & (1u << 16))
        {
            uint32_t wo = Reg(0x2080);
            fl[10] = (float)(int32_t(wo << 17) >> 17);
            fl[11] = (float)(int32_t(wo << 1) >> 17);
        }
        d[208] = Reg(0x2206);   // PA_CL_VTE_CNTL
        // SQ_PROGRAM_CNTL bit 18 = param_gen; registrador em SQ_CONTEXT_MISC (0x2181) bits 8-15
        // RB_COLORCONTROL (alpha test); bit 16: alvo de ponto fixo (8_8_8_8 / 2_10_10_10) -> saida limitada a 0-1
        d[210] = (Reg(0x2202) & 0xFFFF) | (((Reg(0x2001) >> 16) & 0xF) <= 2 && !diag::g_noSaturate ? 0x10000u : 0u);
        d[211] = Reg(0x210E);   // RB_ALPHA_REF
        // PA_SU_VTX_CNTL.pix_center (bit 0): 0 = centro do pixel no numero inteiro (D3D9, o padrao do Xbox); o D3D12 usa
        // centro em +0,5. Sem o meio pixel, os quadrados de tela cheia do jogo nao cobriam a ultima coluna (x=1279) nem a
        // ultima linha (y=719): linhas finas na borda direita e de baixo. FCRECOMP_NOPIXSHIFT=1 desliga.
        {
            static const bool noShift = getenv("FCRECOMP_NOPIXSHIFT") != nullptr;
            float shift = (!noShift && !(Reg(0x2302) & 1)) ? 0.5f : 0.0f;
            memcpy(&d[212], &shift, 4);
        }
        d[209] = (Reg(0x2180) & (1u << 18)) ? (0x80000000u | ((Reg(0x2181) >> 8) & 0xFF)) : 0;
        // planos de corte do usuario: PA_CL_CLIP_CNTL (0x2204) e PA_CL_UCP_0..5 (0x2388-0x239F). Em 0x2380-0x2383 fica o
        // deslocamento de poligono (bug 19): despejando 0x2380.. no reflexo da praia, 0x2380-83 = -0 e os planos que o jogo
        // escreve (P, -P, P) comecam em 0x2388; antes liamos a partir de 0x2380 (dois planos fora do lugar; so dava certo
        // porque o jogo repete o mesmo plano). FCRECOMP_SEMUCP=1 ignora.
        {
            static const bool noUcp = getenv("FCRECOMP_SEMUCP") != nullptr;
            d[213] = (noUcp || g_expMode == 2) ? 0 : Reg(0x2204);
            float resScale = color.meios / 2.0f;   // escala do alvo deste desenho (pixels da placa por pixel do jogo)
            memcpy(&d[214], &resScale, 4);
            d[215] = scaledMask;
            float texScale = g_meios / 2.0f;   // escala das texturas marcadas em scaledMask (imagens da tela)
            memcpy(&d[240], &texScale, 4);
            for (uint32_t i = 0; i < 24; i++) d[216 + i] = Reg(0x2388 + i);
            static bool logged = false;
            if ((d[213] & 63) && !logged)
            {
                logged = true;
                Log("[draw] planos de corte do usuario em uso: PA_CL_CLIP_CNTL=%08X", d[213]);
            }
        }

        list->OMSetRenderTargets(1, &color.view, FALSE, depth ? &depth->view : nullptr);
        // o shader de vertices converte a posicao para -1..1 com o tamanho do jogo (rtSize = CANVAS); o viewport leva isso
        // para os pixels da placa. A tesoura e calculada no tamanho do jogo e convertida no fim.
        const uint32_t m = color.meios;   // escala do alvo (resolucao interna; mapa de sombras pode ter outra)
        D3D12_VIEWPORT vp{ 0, 0, (float)Pm(CANVAS, m), (float)Pm(CANVAS, m), 0, 1 };
        // PA_SC_WINDOW_SCISSOR_TL/BR (x bits 0-14, y bits 16-30; TL bit 31 = sem deslocamento da janela).
        // No "predicated tiling" cada faixa so pinta a sua parte: sem isto a cena inteira era pintada em cada faixa.
        uint32_t tl = Reg(0x2081), br = Reg(0x2082);
        LONG sx0 = tl & 0x7FFF, sy0 = (tl >> 16) & 0x7FFF, sx1 = br & 0x7FFF, sy1 = (br >> 16) & 0x7FFF;
        if (!(tl >> 31) && (Reg(0x2205) & (1u << 16)))
        {
            uint32_t wo = Reg(0x2080);
            LONG ox = int32_t(wo << 17) >> 17, oy = int32_t(wo << 1) >> 17;
            sx0 += ox; sx1 += ox; sy0 += oy; sy1 += oy;
        }
        D3D12_RECT sc{ std::max(sx0, 0L), std::max(sy0, 0L), std::min(sx1, (LONG)CANVAS), std::min(sy1, (LONG)CANVAS) };
        if (sc.right <= sc.left || sc.bottom <= sc.top) sc = { 0, 0, (LONG)CANVAS, (LONG)CANVAS };
        // faixas juntas: a primeira faixa desenha a largura toda (a tesoura dela vai so ate 768)
        if (g_tileState == 1 && g_tileFullWidth && sc.left == 0 && sc.right < (LONG)g_tileFullWidth)
            sc.right = std::min<LONG>((LONG)g_tileFullWidth, (LONG)CANVAS);
        // Caixa de teste de uma consulta de oclusao: so conta pixels dentro do viewport (a tela). A tesoura dessas caixas
        // vai ate 2048 e a superficie tem 2048 linhas: a parte fora da tela (profundidade limpa, nada na frente)
        // contava como "visivel" e o halo de uma lampada atras da parede continuava aparecendo.
        if (g_queryIndex >= 0)
        {
            float xs = std::fabs(RegF(0x210F)), xo = RegF(0x2110), ys = std::fabs(RegF(0x2111)), yo = RegF(0x2112);
            if (xs > 0 && ys > 0)
            {
                LONG ox = (LONG)fl[10], oy = (LONG)fl[11];   // deslocamento da janela (se o desenho usa)
                sc.left = std::max(sc.left, (LONG)std::floor(xo - xs) + ox);
                sc.right = std::min(sc.right, (LONG)std::ceil(xo + xs) + ox);
                sc.top = std::max(sc.top, (LONG)std::floor(yo - ys) + oy);
                sc.bottom = std::min(sc.bottom, (LONG)std::ceil(yo + ys) + oy);
                if (sc.right <= sc.left || sc.bottom <= sc.top) sc = { 0, 0, 0, 0 };
            }
        }
        // depuracao: FCRECOMP_NORASTER=hash do VS -> tesoura vazia (mede so o custo dos vertices desse shader)
        static const char* noRaster = getenv("FCRECOMP_NORASTER");
        if (noRaster && strtoull(noRaster, nullptr, 16) == info.vs.hash) sc = { 0, 0, 0, 0 };
        {
            D3D12_RECT pc{ PmL(sc.left, m), PmL(sc.top, m), PmL(sc.right, m), PmL(sc.bottom, m) };
            if (m > 2)   // superficies menores que CANVAS (veja SurfaceWidth/SurfaceHeight)
            {
                pc.right = std::min<LONG>(pc.right, (LONG)Pm(color.width, m));
                pc.bottom = std::min<LONG>(pc.bottom, (LONG)Pm(color.height, m));
                if (depth) { pc.right = std::min<LONG>(pc.right, (LONG)Pm(depth->width, depth->meios)); pc.bottom = std::min<LONG>(pc.bottom, (LONG)Pm(depth->height, depth->meios)); }
                if (pc.right <= pc.left || pc.bottom <= pc.top) pc = { 0, 0, 0, 0 };
            }
            list->RSSetScissorRects(1, &pc);
        }
        list->RSSetViewports(1, &vp);
        if (uint64_t serial = render::FrameSerial(); g_bound.serial != serial) { g_bound = BoundState{}; g_bound.serial = serial; }
        float blendFactor[4] = { RegF(0x2105), RegF(0x2106), RegF(0x2107), RegF(0x2108) };   // RB_BLEND_RED..ALPHA
        if (memcmp(blendFactor, g_bound.blend, sizeof(blendFactor)))
        {
            list->OMSetBlendFactor(blendFactor);
            memcpy(g_bound.blend, blendFactor, sizeof(blendFactor));
        }
        if (g_bound.root != g_root.Get()) { list->SetGraphicsRootSignature(g_root.Get()); g_bound.root = g_root.Get(); }
        ID3D12DescriptorHeap* heaps[] = { render::SrvHeap(), render::SamplerHeap() };
        if (heaps[0] != g_bound.heaps[0] || heaps[1] != g_bound.heaps[1])
        {
            list->SetDescriptorHeaps(2, heaps);
            g_bound.heaps[0] = heaps[0]; g_bound.heaps[1] = heaps[1];
        }
        list->SetGraphicsRootConstantBufferView(0, vsConsts);
        list->SetGraphicsRootConstantBufferView(1, psConsts);
        list->SetGraphicsRootConstantBufferView(2, up.gpu);
        list->SetGraphicsRootDescriptorTable(3, srv.gpu);
        list->SetGraphicsRootDescriptorTable(4, samplerTable);
        if (g_bound.pso != pso) { list->SetPipelineState(pso); g_bound.pso = pso; }
        if (g_bound.topo != topo) { list->IASetPrimitiveTopology(topo); g_bound.topo = topo; }
        {
            HistoryEntry entry;
            entry.isDraw = true;
            entry.vs = info.vs.hash; entry.ps = info.ps.hash; entry.prim = info.primType; entry.count = info.count;
            entry.vertices = vertexCount; entry.indexed = info.indexBase != 0;
            entry.color = Reg(0x2001) & 0xFFF; entry.depth = Reg(0x2002) & 0xFFF;
            std::string& full = entry.text;   // detalhes extras: so no quadro capturado
            if (diag::Capturing(diag::CurrentFrame()))
            {
                // captura F9: todas as texturas do desenho (R = imagem gerada pelo jogo, C = cubo, V = lida no vertex shader)
                uint32_t used = vs.textureMask | ps.textureMask;
                for (uint32_t t = 0; t < 32; t++)
                    if (used & (1u << t))
                    {
                        uint32_t f1 = Reg(0x4801 + t * 6), f2 = Reg(0x4802 + t * 6), b = f1 & 0x1FFFF000;
                        char tb[96];
                        snprintf(tb, sizeof(tb), " t%u=%08X %ux%u f%u%s%s%s", t, b, (f2 & 0x1FFF) + 1, ((f2 >> 13) & 0x1FFF) + 1, f1 & 0x3F,
                            g_resolved.count(b) ? " R" : "", ((vs.cubeMask | ps.cubeMask) >> t) & 1 ? " C" : "", (vs.textureMask >> t) & 1 ? " V" : "");
                        full += tb;
                    }
                // deslocamento da janela, tesoura (ja na EDRAM) e centro do viewport: mostra em que faixa o desenho caiu
                char wb[128];
                snprintf(wb, sizeof(wb), " jan=%.0f,%.0f tes=%ld,%ld-%ld,%ld vpx=%.0f modo=%08X tela=%08X/%08X sup=%08X", fl[10], fl[11], sc.left, sc.top, sc.right, sc.bottom,
                    RegF(0x2110), Reg(0x2205), Reg(0x200E), Reg(0x200F), Reg(0x2000));
                full += wb;
                // impressoes digitais: constantes do VS e cada buffer de vertices (compara o mesmo objeto entre faixas)
                auto fnv = [](const uint8_t* p, size_t n) { uint32_t h = 2166136261u; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; } return h; };
                uint32_t vc[1024];
                for (uint32_t i = 0; i < 1024; i++) vc[i] = Reg(0x4000 + i);
                snprintf(wb, sizeof(wb), " cvs=%08X cps=%08X at=%08X ref=%g blend=%08X", fnv((const uint8_t*)vc, sizeof(vc)),
                    [&] { for (uint32_t i = 0; i < 1024; i++) vc[i] = Reg(0x4400 + i); return fnv((const uint8_t*)vc, sizeof(vc)); }(),
                    Reg(0x2202), RegF(0x210E), Reg(0x2201));
                full += wb;
                if (Reg(0x2204) & 63)
                {
                    snprintf(wb, sizeof(wb), " corte=%08X", Reg(0x2204));
                    full += wb;
                    for (uint32_t k = 0; k < 6; k++)
                        if (Reg(0x2204) & (1u << k))
                        {
                            snprintf(wb, sizeof(wb), " plano%u=(%g,%g,%g,%g)", k, RegF(0x2388 + k * 4), RegF(0x2389 + k * 4), RegF(0x238A + k * 4), RegF(0x238B + k * 4));
                            full += wb;
                        }
                }
                for (uint32_t slot : vs.fetchSlots)
                {
                    uint32_t f0 = Reg(0x4800 + slot * 2), f1 = Reg(0x4801 + slot * 2);
                    uint32_t addr = f0 & 0x1FFFFFFC, bytes = std::min<uint32_t>(((f1 >> 2) & 0xFFFFFF) * 4, 32u << 20);
                    snprintf(wb, sizeof(wb), " vb%u=%08X+%X:%08X", slot, addr, bytes, fnv(Phys(addr), bytes));
                    full += wb;
                }
                if (info.indexBase)
                {
                    uint32_t ib = info.indexBase & 0x1FFFFFFC, n = info.count * (info.index32 ? 4 : 2);
                    snprintf(wb, sizeof(wb), " ib=%08X:%08X", ib, fnv(Phys(ib), n));
                    full += wb;
                }
            }
            CurrentHistory().push_back(std::move(entry));
        }
        std::string profLabel;
        if (gpuprof::g_active)
        {
            char pl[96];
            snprintf(pl, sizeof(pl), "vs=%016llX ps=%016llX prim=%u", (unsigned long long)info.vs.hash, (unsigned long long)info.ps.hash, info.primType);
            profLabel = pl;
            gpuprof::Mark(list, profLabel, true);
        }
        // Particulas (fogo, fumaca, faiscas: quadrados misturados que nao gravam profundidade) com o shader calculado
        // uma vez a cada 2x2 pixels (sombreamento de taxa variavel, D3D12 VRS nivel 1); a mistura continua por pixel.
        // Bug 14: na explosao do barco do treino ~350 particulas cobrindo a tela levavam a GPU integrada a 45-60 ms.
        // Volta a 1x1 logo depois (as copias dos resolves e o resto nao podem herdar). FCRECOMP_SEMVRS=1 desliga.
        const uint32_t bc = Reg(0x2201);
        const bool particula = info.primType == 13 && !(depthControl & 4) && colorMask &&
                               (bc & 0x1FFF1FFF) != 0x00010001;   // mistura ligada (nao e "fonte x1 + destino x0")
        ID3D12GraphicsCommandList5* l5 = particula ? render::ListaVrs() : nullptr;
        if (l5) l5->RSSetShadingRate(D3D12_SHADING_RATE_2X2, nullptr);
        list->DrawInstanced(vertexCount, 1, 0, 0);
        if (l5) l5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
        if (gpuprof::g_active) gpuprof::Mark(list, profLabel, false);

        static const char* drawShot = getenv("FCRECOMP_DRAWSHOT");
        if (drawShot)
        {
            uint64_t frame = strtoull(drawShot, nullptr, 10);
            uint32_t idx = (uint32_t)CurrentHistory().size() - 1;
            // quadro 0 = automatico: o primeiro quadro depois de um quadro com mais de 800 desenhos (dentro da missao)
            static uint64_t autoFrame = 0;
            if (frame == 0 && !autoFrame && g_drawHistory[(g_frameNumber + 3) & 3].size() > 800) autoFrame = g_frameNumber;
            if (frame == 0) frame = autoFrame ? autoFrame : ~0ull;
            // "c:i,j" = no quadro da captura (FCRECOMP_CAPTURE/F9)
            if (drawShot[0] == 'c' ? diag::Capturing(diag::CurrentFrame()) : frame == g_frameNumber)
                for (const char* p = strchr(drawShot, ':'); p; p = strchr(p + 1, ','))
                    // numero do desenho, ou "ps=HASH" (os primeiros desenhos com esse shader de pixel e mais de 1000 vertices)
                    if (strncmp(p + 1, "ps=", 3) == 0 ? CurrentHistory().back().Describe().find(std::string(p + 1, 19)) != std::string::npos &&
                            CurrentHistory().back().Describe().find(" n=") != std::string::npos &&
                            atoi(CurrentHistory().back().Describe().c_str() + CurrentHistory().back().Describe().find(" n=") + 3) > 1000
                        : (uint32_t)atoi(p + 1) == idx)
                    {
                        Log("[draw] captura apos o desenho %u: %s", idx, CurrentHistory().back().Describe().c_str());
                        Log("[draw]   booleanos %08X %08X %08X %08X %08X %08X %08X %08X", Reg(0x4900), Reg(0x4901), Reg(0x4902),
                            Reg(0x4903), Reg(0x4904), Reg(0x4905), Reg(0x4906), Reg(0x4907));
                        // FCRECOMP_DRAWSHOTC="0-3,42-45": constantes do shader de vertices nesse desenho
                        if (static const char* cl = getenv("FCRECOMP_DRAWSHOTC"); cl)
                            for (const char* q = cl; q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : nullptr)
                            {
                                int i0 = atoi(q), i1 = strchr(q, '-') && (!strchr(q, ',') || strchr(q, '-') < strchr(q, ',')) ? atoi(strchr(q, '-') + 1) : i0;
                                for (int i = i0; i <= i1 && i < 512; i++)
                                    Log("[draw]   c%d = %g %g %g %g", i, RegF(0x4000 + i * 4), RegF(0x4001 + i * 4), RegF(0x4002 + i * 4), RegF(0x4003 + i * 4));
                            }
                        Log("[draw]   2380..2387: %08X %08X %08X %08X %08X %08X %08X %08X | 237E %08X 237F %08X | modo %08X", Reg(0x2380), Reg(0x2381),
                            Reg(0x2382), Reg(0x2383), Reg(0x2384), Reg(0x2385), Reg(0x2386), Reg(0x2387), Reg(0x237E), Reg(0x237F), Reg(0x2205));
                        for (uint32_t t = 0; t < 16; t++)   // constantes de busca das texturas (6 dwords cada)
                            if (Reg(0x4800 + t * 6 + 1) & 0x1FFFF000)
                                Log("[draw]   busca t%u: %08X %08X %08X %08X %08X %08X", t, Reg(0x4800 + t * 6), Reg(0x4801 + t * 6),
                                    Reg(0x4802 + t * 6), Reg(0x4803 + t * 6), Reg(0x4804 + t * 6), Reg(0x4805 + t * 6));
                        for (int i : { 13, 14, 15, 16, 17, 254, 255 })
                            Log("[draw]   ps c%d = %g %g %g %g", i, RegF(0x4400 + i * 4), RegF(0x4401 + i * 4), RegF(0x4402 + i * 4), RegF(0x4403 + i * 4));
                        Log("[draw]   viewport escala %g %g %g deslocamento %g %g %g vte %08X", RegF(0x210F), RegF(0x2111), RegF(0x2113),
                            RegF(0x2110), RegF(0x2112), RegF(0x2114), Reg(0x2206));
                        CaptureRegion(list, color.res.Get(), color.state, Px(1280), Px(720), idx);
                    }
        }
    }

    void Resolve()
    {
        if (!g_root) return;
        uint32_t copyControl = Reg(0x2318);
        uint32_t dest = Reg(0x2319) & 0x1FFFF000;
        uint32_t destPitch = Reg(0x231A);   // largura (bits 0-13) e altura (bits 16-29) do destino
        auto* list = render::FrameList();
        uint32_t drawsBefore = g_drawsSinceResolve;
        g_drawsSinceResolve = 0;
        if (g_statsOn)
        {
            char k[400];
            snprintf(k, sizeof(k), "resolve destino %08X controle %08X destinfo %08X pitch %08X | 231C=%08X 231D=%08X 231E=%08X 231F=%08X 2320=%08X 2321=%08X", dest, copyControl, Reg(0x231B), destPitch, Reg(0x231C), Reg(0x231D), Reg(0x231E), Reg(0x231F), Reg(0x2320), Reg(0x2321));
            if (g_statsOn) g_stats[k]++;
        }
        static const char* seqLog = getenv("FCRECOMP_SEQLOG");
        if (seqLog && g_frameNumber >= (uint64_t)atoi(seqLog) && g_frameNumber < (uint64_t)atoi(seqLog) + 4)
            Log("[seq] quadro %llu: resolve -> %08X controle %08X (desenhos antes: %u) bruto: base %08X pitch %08X info %08X", (unsigned long long)g_frameNumber, dest, copyControl, drawsBefore, Reg(0x2319), Reg(0x231A), Reg(0x231B));

        uint32_t srcSelect = copyControl & 7;   // 0-3 = cor, 4 = profundidade
        bool isDepth = srcSelect == 4;
        Surface& color = ColorSurface(ColorKey());
        Surface* depth = isDepth || (copyControl & 0x200) ? &DepthSurface(DepthKey()) : nullptr;

        // retangulo da origem: vertices do desenho de copia; senao, o tamanho do destino
        uint32_t x0 = 0, y0 = 0, x1, y1;
        if (!ResolveRect(x0, y0, x1, y1))
        {
            x1 = std::min<uint32_t>(CANVAS, std::max(1u, destPitch & 0x3FFF));
            y1 = std::min<uint32_t>(CANVAS, std::max(1u, (destPitch >> 16) & 0x3FFF));
        }
        // faixas: a largura do destino (pitch) e a tela inteira; na primeira faixa (desenhada inteira) a copia vai ate ela
        if (g_tileState != 0 && !isDepth)
        {
            uint32_t destW = destPitch & 0x3FFF;
            if (destW > g_tileFullWidth) g_tileFullWidth = std::min<uint32_t>(destW, CANVAS);
            if (g_tileState == 1 && x0 == 0 && destW > x1) x1 = std::min<uint32_t>(destW, CANVAS);
        }
        uint32_t w = x1 - x0, h = y1 - y0;
        if (diag::Capturing(diag::CurrentFrame()))
        {
            char desc[200];
            snprintf(desc, sizeof(desc), "RESOLVE %u,%u-%u,%u para %08X  jan=%08X modo=%08X tesoura=%08X/%08X", x0, y0, x1, y1, dest,
                Reg(0x2080), Reg(0x2205), Reg(0x2081), Reg(0x2082));
            HistoryText(desc);
        }
        if (seqLog && g_frameNumber >= (uint64_t)atoi(seqLog) && g_frameNumber < (uint64_t)atoi(seqLog) + 2)
            Log("[seq]   retangulo %u,%u-%u,%u  WINDOW_OFFSET=%08X SC_MODE_CNTL=%08X VPORT_XOFF=%f YOFF=%f  WINDOW_SCISSOR=%08X/%08X",
                x0, y0, x1, y1, Reg(0x2080), Reg(0x2205), RegF(0x2110), RegF(0x2112), Reg(0x2081), Reg(0x2082));

        // FCRECOMP_RESOLVELOG=endereco[,endereco]: anota cada resolve para esses destinos (quadro, retangulo, registradores)
        static const char* resolveLog = getenv("FCRECOMP_RESOLVELOG");
        if (resolveLog)
            for (const char* e = resolveLog; e && *e; e = strchr(e, ',') ? strchr(e, ',') + 1 : "")
                if (strtoul(e, nullptr, 16) == dest || (*e == '*' && g_frameNumber >= strtoull(e + 1, nullptr, 10) &&
                    g_frameNumber <= strtoull(strchr(e, '-') ? strchr(e, '-') + 1 : e + 1, nullptr, 10)))   // "*960-1110": todos nesses quadros
                    Log("[resolve] quadro %llu (troca %llu): %08X <- %u,%u-%u,%u controle %08X pitch %08X info %08X base bruta %08X desenhos antes %u",
                        (unsigned long long)g_frameNumber, (unsigned long long)diag::CurrentFrame(), dest, x0, y0, x1, y1, copyControl, destPitch,
                        Reg(0x231B), Reg(0x2319), drawsBefore);
        // "predicated tiling": o resolve da segunda faixa sem desenhos novos nao muda a copia
        // So vale para o MESMO destino do resolve anterior: o jogo tambem faz dois resolves seguidos da mesma tela para
        // destinos diferentes (bug 16: o miolo da mira 65x65 e logo depois a tela inteira 006DA000); pular o segundo
        // deixava na textura a imagem final do quadro anterior e a medicao de luz clareava a tela sem parar.
        static const bool velho = getenv("FCRECOMP_RESOLVEREPETIDO") != nullptr;   // =1 volta ao comportamento antigo
        bool sameAsBefore = drawsBefore == 0 && g_resolved.count(dest) && !isDepth && (velho || dest == g_lastResolveDest);
        g_lastResolveDest = dest;

        if (dest && ((copyControl >> 20) & 3) != 3 && !sameAsBefore)
        {
            DXGI_FORMAT fmt = isDepth ? DEPTH_COPY_FORMAT : COLOR_FORMAT;
            const uint32_t m = isDepth ? depth->meios : color.meios;   // escala da superficie de origem
            auto& tex = g_resolved[dest];
            if (!tex.res || tex.width != w || tex.height != h || tex.format != fmt || tex.meios != m)
            {
                // devolve a textura atual ao estoque e pega (ou cria) a do tamanho pedido (escala no bit 16 da largura)
                if (tex.res) g_resolvePool[{ dest, tex.width | (tex.meios << 16), tex.height, tex.format }] = { std::move(tex.res), tex.state };
                auto& pooled = g_resolvePool[{ dest, w | (m << 16), h, fmt }];
                if (!pooled.res)
                {
                    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
                    D3D12_RESOURCE_DESC td{};
                    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                    td.Width = Pm(w, m); td.Height = Pm(h, m); td.DepthOrArraySize = 1; td.MipLevels = 1; td.SampleDesc.Count = 1;
                    td.Format = fmt;
                    // alvo de desenho: copia de profundidade e conversao de superficie RGBA8 (desenhos que gravam nela)
                    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                    { double tRt = perf::Now(); g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pooled.res)); perf::g_rtCreateMs += perf::Now() - tRt; }
                    pooled.state = D3D12_RESOURCE_STATE_COPY_DEST;
                    Log("[draw] destino de resolve %08X: %ux%u %s (origem %u,%u-%u,%u; COPY_CONTROL %08X DEST_PITCH %08X DEST_INFO %08X)",
                        dest, w, h, isDepth ? "(profundidade)" : "", x0, y0, x1, y1, copyControl, destPitch, Reg(0x231B));
                }
                tex.res = std::move(pooled.res);
                tex.state = pooled.state;
                g_resolvePool.erase({ dest, w | (m << 16), h, fmt });
                tex.width = w; tex.height = h; tex.format = fmt; tex.meios = m;
            }

            if (!isDepth && color.format != COLOR_FORMAT && g_colorCopyPso)
            {
                // superficie de 8 bits: o resolve vira um desenho que converte para a textura RGBA16F (CopyTextureRegion so
                // copia entre formatos iguais); o resto do emulador continua vendo os resolves em RGBA16F
                Transition(list, color.res.Get(), color.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                Transition(list, tex.res.Get(), tex.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
                auto srvc = render::AllocSrv(1);
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = color.format;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Texture2D.MipLevels = 1;
                g_dev->CreateShaderResourceView(color.res.Get(), &sd, srvc.cpu);
                D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
                rtv.ptr += (SIZE_T)MAX_SURFACES * g_rtvSize;   // ultimo slot: alvo temporario
                g_dev->CreateRenderTargetView(tex.res.Get(), nullptr, rtv);
                list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                // em pixels da placa; a superficie pode ser mais estreita/baixa que o retangulo pedido
                uint32_t cw = Pm(std::min(x1, color.width), m) > Pm(x0, m) ? Pm(std::min(x1, color.width), m) - Pm(x0, m) : 0;
                uint32_t ch = Pm(std::min(y1, color.height), m) > Pm(y0, m) ? Pm(std::min(y1, color.height), m) - Pm(y0, m) : 0;
                D3D12_VIEWPORT vp{ 0, 0, (float)Pm(w, m), (float)Pm(h, m), 0, 1 };
                D3D12_RECT sc{ 0, 0, (LONG)std::min(cw, Pm(w, m)), (LONG)std::min(ch, Pm(h, m)) };
                list->RSSetViewports(1, &vp);
                list->RSSetScissorRects(1, &sc);
                list->SetGraphicsRootSignature(g_depthCopyRoot.Get());
                ID3D12DescriptorHeap* heaps[] = { render::SrvHeap(), render::SamplerHeap() };
                list->SetDescriptorHeaps(2, heaps);
                list->SetGraphicsRootDescriptorTable(0, srvc.gpu);
                int32_t off[2] = { (int32_t)Pm(x0, m), (int32_t)Pm(y0, m) };
                list->SetGraphicsRoot32BitConstants(1, 2, off, 0);
                list->SetPipelineState(g_colorCopyPso.Get());
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ForgetBoundState();   // o proximo desenho do jogo reaplica tudo
                if (sc.right > 0 && sc.bottom > 0) list->DrawInstanced(3, 1, 0, 0);
            }
            else if (!isDepth)
            {
                Transition(list, color.res.Get(), color.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Transition(list, tex.res.Get(), tex.state, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION dst{ tex.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                D3D12_TEXTURE_COPY_LOCATION src{ color.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                // a superficie pode ser mais estreita (ou baixa) que o retangulo pedido: copia so a parte que existe
                // (em pixels da placa; a textura tem Pm(w) x Pm(h))
                D3D12_BOX box{ Pm(x0, m), Pm(y0, m), 0, std::min(Pm(x0, m) + Pm(w, m), Pm(color.width, m)), std::min(Pm(y0, m) + Pm(h, m), Pm(color.height, m)), 1 };
                if (box.right > box.left && box.bottom > box.top) list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
            }
            else if (g_depthCopyPso)
            {
                // profundidade -> R32_FLOAT com um desenho de tela cheia
                Transition(list, depth->res.Get(), depth->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                Transition(list, tex.res.Get(), tex.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
                auto srvd = render::AllocSrv(1);
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = DS_SRV_FORMAT;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Texture2D.MipLevels = 1;
                g_dev->CreateShaderResourceView(depth->res.Get(), &sd, srvd.cpu);
                D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
                rtv.ptr += (SIZE_T)MAX_SURFACES * g_rtvSize;   // ultimo slot: alvo temporario
                g_dev->CreateRenderTargetView(tex.res.Get(), nullptr, rtv);
                list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                D3D12_VIEWPORT vp{ 0, 0, (float)Pm(w, m), (float)Pm(h, m), 0, 1 };
                D3D12_RECT sc{ 0, 0, (LONG)Pm(w, m), (LONG)Pm(h, m) };
                list->RSSetViewports(1, &vp);
                list->RSSetScissorRects(1, &sc);
                list->SetGraphicsRootSignature(g_depthCopyRoot.Get());
                ID3D12DescriptorHeap* heaps[] = { render::SrvHeap(), render::SamplerHeap() };
                list->SetDescriptorHeaps(2, heaps);
                list->SetGraphicsRootDescriptorTable(0, srvd.gpu);
                int32_t off[2] = { (int32_t)Pm(x0, m), (int32_t)Pm(y0, m) };
                list->SetGraphicsRoot32BitConstants(1, 2, off, 0);
                list->SetPipelineState(g_depthCopyPso.Get());
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ForgetBoundState();   // o proximo desenho do jogo reaplica tudo
                {
                    char desc[128];
                    snprintf(desc, sizeof(desc), "COPIA DE PROFUNDIDADE %ux%u de (%u,%u) para %08X", w, h, x0, y0, dest);
                    HistoryText(desc);
                }
                list->DrawInstanced(3, 1, 0, 0);
            }
            Transition(list, tex.res.Get(), tex.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            tex.seq = ++g_resolveSeq;
            tex.frame = g_frameNumber;
            if (!isDepth) g_lastResolve = tex.res.Get();
            CaptureForDebug(list, tex, dest);
            if (!isDepth) QueueWriteback(list, tex, dest, Reg(0x231B), destPitch);
            MarcarMemoriaDoResolve(tex, dest, Reg(0x231B));
        }

        // limpezas pedidas junto com o resolve: feitas antes do proximo desenho na superficie, SO no retangulo copiado
        // (como a EDRAM do Xbox). Antes limpavamos a superficie inteira (ate 2048 linhas de RGBA16F/D24S8 por limpeza):
        // muita banda de memoria desperdicada numa GPU integrada. FCRECOMP_LIMPATUDO=1 volta ao antigo.
        static const bool clearAll = getenv("FCRECOMP_LIMPATUDO") != nullptr;
        auto markClear = [&](Surface& sf, uint32_t value) {
            D3D12_RECT r{ (LONG)x0, (LONG)y0, (LONG)std::min(x1, sf.width ? sf.width : x1), (LONG)std::min(y1, sf.height) };
            if (clearAll || g_expMode == 3 || r.right <= r.left || r.bottom <= r.top) r = {};
            else if (sf.pendingClear && sf.clearRect.right)   // ja havia limpeza pendente: junta os retangulos
                r = { std::min(r.left, sf.clearRect.left), std::min(r.top, sf.clearRect.top),
                      std::max(r.right, sf.clearRect.right), std::max(r.bottom, sf.clearRect.bottom) };
            else if (sf.pendingClear) r = {};                 // a pendente era da superficie inteira
            sf.pendingClear = true; sf.clearValue = value; sf.clearRect = r;
        };
        if (copyControl & 0x100) markClear(color, Reg(0x231E));                 // RB_COLOR_CLEAR (ARGB)
        if ((copyControl & 0x200) && depth) markClear(*depth, Reg(0x231D));     // RB_DEPTH_CLEAR
    }

    namespace
    {
        // ---------------------------------------------------------------- mascara do escuro
        // O jogo escurece a tela de carregamento ate o preto antes de trocar de programa (jogo <-> editor, troca de fase)
        // e o jogo novo comeca escuro. Item 30: a janela antiga e coberta pela tela de carregamento propria (gpu.cpp) e o
        // processo novo segura essa mesma tela (FotoReinicioPixels) enquanto o jogo novo esta escuro; o brilho de cada
        // quadro e medido (copia lida pela CPU ~2 quadros depois) e ela sai (corte direto, sem mistura) quando a imagem
        // nova esta clara e estavel. (Antes era a foto do ultimo quadro claro do jogo antigo: misturada com a tela nova
        // dava "LOADINGANDO".) Fora da troca nada e mexido. FCRECOMP_SEMMASCARA=1 desliga.
        struct Medida
        {
            ComPtr<ID3D12Resource> gpu, leitura;   // copia do quadro (para virar o "claro") e copia para a CPU
            D3D12_RESOURCE_STATES estado = D3D12_RESOURCE_STATE_COPY_DEST;
            uint64_t serial = 0;
            bool pendente = false;
            uint32_t w = 0, h = 0, passo = 0;
        };
        Medida g_med[3];
        uint32_t g_medPos = 0;
        ComPtr<ID3D12Resource> g_foto;             // tela de carregamento da troca de programa
        ID3D12Resource* g_segura = nullptr;        // imagem mostrada enquanto a tela esta escura
        bool g_segurando = false;
        double g_seguraDesde = 0, g_seguraMax = 0;
        float g_ultimaLum = -1;
        ID3D12Resource* g_misturaDe = nullptr;     // mistura ao soltar: de g_misturaDe para o quadro do jogo
        int g_misturaQuadro = 0;
        constexpr int MISTURA_QUADROS = 12;
        ComPtr<ID3D12Resource> g_mistura;          // resultado da mistura (alvo de desenho)
        D3D12_RESOURCE_STATES g_misturaEstado = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bool g_fotoTentada = false;
        bool g_videoFrame = false;                 // gpu.cpp: o quadro e de um video Bink (sem mascara)

        void Segurar(ID3D12Resource* img, double maxMs, const char* motivo)
        {
            g_segura = img; g_segurando = true; g_seguraDesde = perf::Now(); g_seguraMax = maxMs; g_ultimaLum = -1;
            g_misturaQuadro = 0;
            Log("[mascara] segurando a imagem (%s)", motivo);
        }

        void Soltar(bool misturar, const char* motivo)
        {
            if (!g_segurando) return;
            g_segurando = false;
            g_misturaDe = misturar ? g_segura : nullptr;
            g_misturaQuadro = misturar ? 1 : 0;
            Log("[mascara] soltando depois de %.0f ms (%s)", perf::Now() - g_seguraDesde, motivo);
        }

        // brilho medio (0-1) de uma copia RGBA16F mapeada: grade de 32x18 pontos
        float Brilho(const uint8_t* p, uint32_t w, uint32_t h, uint32_t passo)
        {
            double soma = 0;
            for (uint32_t gy = 0; gy < 18; gy++)
                for (uint32_t gx = 0; gx < 32; gx++)
                {
                    const uint16_t* px = (const uint16_t*)(p + (size_t)((gy * 2 + 1) * h / 36) * passo) + ((gx * 2 + 1) * w / 64) * 4;
                    soma += 0.299 * std::clamp(HalfToFloat(px[0]), 0.0f, 1.0f) + 0.587 * std::clamp(HalfToFloat(px[1]), 0.0f, 1.0f) +
                            0.114 * std::clamp(HalfToFloat(px[2]), 0.0f, 1.0f);
                }
            return (float)(soma / (32 * 18));
        }

        void ProcessarMedidas(ID3D12GraphicsCommandList* list)
        {
            uint64_t agora = render::FrameSerial();
            for (Medida& m : g_med)
            {
                if (!m.pendente || m.serial + 2 > agora) continue;
                m.pendente = false;
                uint8_t* p = nullptr;
                D3D12_RANGE r{ 0, (SIZE_T)m.passo * m.h };
                if (FAILED(m.leitura->Map(0, &r, (void**)&p))) continue;
                float lum = Brilho(p, m.w, m.h, m.passo);
                if (g_segurando)
                {
                    // solta quando a imagem esta clara e parou de clarear (a tela seguinte ja acendeu)
                    if (lum >= 0.06f && g_ultimaLum >= 0 && lum <= g_ultimaLum * 1.03f) Soltar(false, "imagem clara de novo");   // corte direto: misturar sobrepunha "Carregando" e o "LOADING" do jogo
                    g_ultimaLum = lum;
                }
                D3D12_RANGE nada{ 0, 0 };
                m.leitura->Unmap(0, &nada);
            }
        }

        void Medir(ID3D12GraphicsCommandList* list, ID3D12Resource* src)
        {
            D3D12_RESOURCE_DESC d = src->GetDesc();
            if (d.Format != COLOR_FORMAT) return;
            Medida& m = g_med[g_medPos];
            if (m.pendente) return;   // ainda nao lida (nao deve acontecer com 3 vagas)
            if (!m.gpu || m.w != (uint32_t)d.Width || m.h != d.Height)
            {
                ReleaseLater(std::move(m.gpu)); ReleaseLater(std::move(m.leitura));
                m.w = (uint32_t)d.Width; m.h = d.Height; m.passo = (m.w * 8 + 255) & ~255u;
                D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC td = d;
                td.MipLevels = 1; td.Flags = D3D12_RESOURCE_FLAG_NONE;
                g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m.gpu));
                m.estado = D3D12_RESOURCE_STATE_COPY_DEST;
                D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
                D3D12_RESOURCE_DESC bd{};
                bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bd.Width = (UINT64)m.passo * m.h;
                bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
                bd.SampleDesc.Count = 1;
                bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                g_dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m.leitura));
                if (!m.gpu || !m.leitura) return;
            }
            D3D12_RESOURCE_STATES srcEstado = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            Transition(list, src, srcEstado, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(list, m.gpu.Get(), m.estado, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(m.gpu.Get(), src);
            D3D12_TEXTURE_COPY_LOCATION dst{ m.leitura.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            dst.PlacedFootprint.Footprint = { COLOR_FORMAT, m.w, m.h, 1, m.passo };
            D3D12_TEXTURE_COPY_LOCATION s{ src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            list->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
            Transition(list, src, srcEstado, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m.serial = render::FrameSerial();
            m.pendente = true;
            g_medPos = (g_medPos + 1) % 3;
        }

        // troca de programa: a tela de carregamento vira a imagem segurada ate o jogo novo acender
        void CarregarFotoInicial(ID3D12GraphicsCommandList* list)
        {
            g_fotoTentada = true;
            std::vector<uint8_t> px;
            uint32_t w = 0, h = 0;
            if (!FotoReinicioPixels(px, w, h) || !w || !h) return;
            D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1; td.SampleDesc.Count = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_foto))))
                return;
            uint32_t passo = (w * 4 + 255) & ~255u;
            auto up = render::UploadAlloc((size_t)passo * h, 512);
            for (uint32_t y = 0; y < h; y++) memcpy((uint8_t*)up.cpu + (size_t)y * passo, px.data() + (size_t)y * w * 4, (size_t)w * 4);
            D3D12_TEXTURE_COPY_LOCATION dst{ g_foto.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            D3D12_TEXTURE_COPY_LOCATION src{ up.resource, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            src.PlacedFootprint.Offset = up.offset;
            src.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 1, passo };
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
            Transition(list, g_foto.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Segurar(g_foto.Get(), 15000, "tela de carregamento (troca de programa)");
        }

        // mistura (1 - t) * de + t * para num alvo do tamanho de "para"
        ID3D12Resource* Misturar(ID3D12GraphicsCommandList* list, ID3D12Resource* de, ID3D12Resource* para, float t)
        {
            if (!de || !para) return para ? para : de;
            if (!g_misturaPso) return para;
            D3D12_RESOURCE_DESC d = para->GetDesc();
            if (!g_mistura || g_mistura->GetDesc().Width != d.Width || g_mistura->GetDesc().Height != d.Height)
            {
                ReleaseLater(std::move(g_mistura));
                D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC td = d;
                td.MipLevels = 1; td.Format = COLOR_FORMAT; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&g_mistura))))
                    return para;
                g_misturaEstado = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            Transition(list, g_mistura.Get(), g_misturaEstado, D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto srv = render::AllocSrv(2);
            g_dev->CreateShaderResourceView(de, nullptr, srv.cpu);
            D3D12_CPU_DESCRIPTOR_HANDLE h2 = srv.cpu; h2.ptr += srv.increment;
            g_dev->CreateShaderResourceView(para, nullptr, h2);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += (SIZE_T)MAX_SURFACES * g_rtvSize;   // ultimo slot: alvo temporario
            g_dev->CreateRenderTargetView(g_mistura.Get(), nullptr, rtv);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            D3D12_VIEWPORT vp{ 0, 0, (float)d.Width, (float)d.Height, 0, 1 };
            D3D12_RECT sc{ 0, 0, (LONG)d.Width, (LONG)d.Height };
            list->RSSetViewports(1, &vp);
            list->RSSetScissorRects(1, &sc);
            list->SetGraphicsRootSignature(g_misturaRoot.Get());
            ID3D12DescriptorHeap* heaps[] = { render::SrvHeap(), render::SamplerHeap() };
            list->SetDescriptorHeaps(2, heaps);
            list->SetGraphicsRootDescriptorTable(0, srv.gpu);
            list->SetGraphicsRoot32BitConstants(1, 1, &t, 0);
            list->SetPipelineState(g_misturaPso.Get());
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, 1, 0, 0);
            ForgetBoundState();
            Transition(list, g_mistura.Get(), g_misturaEstado, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            return g_mistura.Get();
        }

        // imagem que vai para a tela neste quadro (veja "mascara do escuro")
        ID3D12Resource* MascararEscuro(ID3D12GraphicsCommandList* list, ID3D12Resource* src)
        {
            static const bool off = getenv("FCRECOMP_SEMMASCARA") != nullptr;
            if (off || !src) return src;
            if (!g_fotoTentada) CarregarFotoInicial(list);
            ProcessarMedidas(list);
            if (g_videoFrame) { Soltar(false, "video"); g_misturaQuadro = 0; return src; }
            if (g_segurando) Medir(list, src);
            if (g_segurando && perf::Now() - g_seguraDesde > g_seguraMax) Soltar(false, "tempo maximo");
            if (g_segurando && g_segura) return g_segura;
            if (g_misturaQuadro > 0 && g_misturaDe)
            {
                float t = (float)g_misturaQuadro / MISTURA_QUADROS;
                ID3D12Resource* de = g_misturaDe;   // (antes era zerada antes de usar: imagem vazia -> o DirectX derrubava a placa)
                if (++g_misturaQuadro > MISTURA_QUADROS) { g_misturaQuadro = 0; g_misturaDe = nullptr; }
                return Misturar(list, de, src, t);
            }
            return src;
        }
    }

    void SetVideoFrame(bool video) { g_videoFrame = video; }

    std::string g_fotoPedida;   // roteiro gravado (gpu.cpp): arquivo da foto da proxima imagem apresentada
    void PedirFoto(const std::string& arquivo) { g_fotoPedida = arquivo; }

    bool PresentFrame(uint32_t frontBuffer)
    {
        static const char* seqLog = getenv("FCRECOMP_SEQLOG");
        if (seqLog && g_frameNumber >= (uint64_t)atoi(seqLog) && g_frameNumber < (uint64_t)atoi(seqLog) + 4)
            Log("[seq] quadro %llu: SWAP frontbuffer %08X", (unsigned long long)g_frameNumber, frontBuffer);
        g_uploadedThisFrame.clear();
        ProcessWritebacks();
        g_copiedThisFrame.clear();
        static const char* statsEnv = getenv("FCRECOMP_STATS");
        if (statsEnv && g_frameNumber == (uint64_t)atoi(statsEnv))
            for (auto& [k, v] : g_stats) Log("[stats] %5d  %s", v, k.c_str());
        g_stats.clear();

        bool presented = false;
        bool segurado = false;   // quadro segurado de proposito (itens 33/34): conta como apresentado
        static const char* presentLog0 = getenv("FCRECOMP_PRESENTLOG");
        if (presentLog0 && !(g_drewThisFrame && g_lastResolve) && g_frameNumber >= strtoull(presentLog0, nullptr, 10) &&
            g_frameNumber <= strtoull(strchr(presentLog0, '-') ? strchr(presentLog0, '-') + 1 : presentLog0, nullptr, 10))
            Log("[present] quadro %llu fb %08X: NAO apresentado pela GPU (desenhou=%d, ultimo resolve=%p)", (unsigned long long)g_frameNumber,
                frontBuffer, (int)g_drewThisFrame, (void*)g_lastResolve);
        // Quadro sem desenhos (o jogo so repete a tela, ex.: fim do fade do "presents"): no Xbox o buffer de tela
        // continua com a imagem anterior. Remonta com os pedacos ja resolvidos para esse endereco, em vez de cair
        // no caminho antigo que le a memoria do Xbox (onde os resolves nao sao escritos -> tela preta).
        bool havePieces = false;
        {
            uint32_t fb = frontBuffer & 0x1FFFF000;
            for (auto& [addr, tex] : g_resolved)
                if (addr >= fb && addr < fb + 1280 * 720 * 4 && tex.format == COLOR_FORMAT && tex.res) { havePieces = true; break; }
        }
        if ((g_drewThisFrame && g_lastResolve) || havePieces)
        {
            // Monta o framebuffer (1280x720, tiled) com os pedacos resolvidos dentro dele. A posicao de cada
            // pedaco vem do endereco: procuramos (x, y) multiplos de 32 cujo deslocamento tiled bate.
            // Na placa a imagem tem Px(1280) x Px(720) (resolucao interna).
            auto* list = render::FrameList();
            static ComPtr<ID3D12Resource> fbTex;
            static D3D12_RESOURCE_STATES fbState;
            if (!fbTex)
            {
                D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC td{};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                td.Width = Px(1280); td.Height = Px(720); td.DepthOrArraySize = 1; td.MipLevels = 1; td.SampleDesc.Count = 1;
                td.Format = COLOR_FORMAT;
                g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&fbTex));
                fbState = D3D12_RESOURCE_STATE_COPY_DEST;
            }
            if (!fbTex) { SavePendingShots(); return false; }   // placa de video removida/sem memoria
            Transition(list, fbTex.Get(), fbState, D3D12_RESOURCE_STATE_COPY_DEST);

            uint32_t fb = frontBuffer & 0x1FFFF000;
            int pieces = 0;
            std::vector<std::pair<uint32_t, ResolvedTexture*>> order;
            for (auto& [addr, tex] : g_resolved) order.push_back({ addr, &tex });
            std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.second->seq < b.second->seq; });
            for (auto& [addr, texp] : order)
            {
                auto& tex = *texp;
                if (addr < fb || addr >= fb + 1280 * 720 * 4 || tex.format != COLOR_FORMAT || !tex.res) continue;
                uint32_t delta = addr - fb, px = 0, py = 0;
                bool found = false;
                for (uint32_t y = 0; y < 720 && !found; y += 32)
                    for (uint32_t x = 0; x < 1280 && !found; x += 32)
                        if (TiledOffset(x, y, 1280, 2) == delta) { px = x; py = y; found = true; }
                if (!found) continue;
                uint32_t w = std::min(Px(tex.width), Px(1280) - Px(px)), h = std::min(Px(tex.height), Px(720) - Px(py));
                Transition(list, tex.res.Get(), tex.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst{ fbTex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                D3D12_TEXTURE_COPY_LOCATION src{ tex.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                D3D12_BOX box{ 0, 0, 0, w, h, 1 };
                list->CopyTextureRegion(&dst, Px(px), Px(py), 0, &src, &box);
                Transition(list, tex.res.Get(), tex.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                pieces++;
            }
            Transition(list, fbTex.Get(), fbState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            static const char* presentLog = getenv("FCRECOMP_PRESENTLOG");   // "inicio-fim": como cada quadro foi montado
            if (presentLog && g_frameNumber >= strtoull(presentLog, nullptr, 10) && g_frameNumber <= strtoull(strchr(presentLog, '-') ? strchr(presentLog, '-') + 1 : presentLog, nullptr, 10))
            {
                std::string list;
                for (auto& [addr, texp] : order)
                {
                    char b[64];
                    snprintf(b, sizeof(b), " %08X(%ux%u seq%llu)", addr, texp->width, texp->height, (unsigned long long)texp->seq);
                    if (addr >= fb && addr < fb + 1280 * 720 * 4) list += b;
                }
                Log("[present] quadro %llu fb %08X: %d pedacos;%s", (unsigned long long)g_frameNumber, fb, pieces, list.c_str());
            }
            static const bool noPersist = getenv("FCRECOMP_NOPERSIST") != nullptr;
            if (!noPersist && pieces && !g_usedPersistThisFrame && g_drawsThisFrame > 0 && g_drawsThisFrame < 200)
            {
                if (!g_persisted)
                {
                    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
                    D3D12_RESOURCE_DESC td = fbTex->GetDesc();
                    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_persisted));
                }
                else
                {
                    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    Transition(list, g_persisted.Get(), st, D3D12_RESOURCE_STATE_COPY_DEST);
                }
                D3D12_RESOURCE_STATES st = fbState;
                Transition(list, fbTex.Get(), st, D3D12_RESOURCE_STATE_COPY_SOURCE);
                list->CopyResource(g_persisted.Get(), fbTex.Get());
                g_persistedFrame = g_frameNumber;
                g_persistedPropria = !g_fullResolveThisFrame;
                Transition(list, fbTex.Get(), st, fbState);
                D3D12_RESOURCE_STATES ps = D3D12_RESOURCE_STATE_COPY_DEST;
                Transition(list, g_persisted.Get(), ps, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            if ((pieces || g_lastResolve) && assinc::DeveSegurar())
            {
                // quadro incompleto (muitos shaders na fila): envia a lista sem mostrar e espera a fila (item 33)
                render::PresentTexture(nullptr);
                segurado = true;   // item 37: a janela fica com a imagem anterior (sem isso o gpu.cpp mostrava a memoria de tela do Xbox: quadro preto)
                assinc::EsperarFila();
                assinc::g_esperadas.clear(); assinc::g_incompleto = false;
            }
            else if ((pieces || g_lastResolve) && assinc::g_incompleto)
            {
                // faltou um desenho grande (item 34): nao mostra e espera as compilacoes dele
                render::PresentTexture(nullptr);
                segurado = true;
                assinc::EsperarGrandes();
            }
            else if (pieces || g_lastResolve)
            {
                gpuprof::BeforePresent(render::FrameList());
                ID3D12Resource* imagem = MascararEscuro(list, pieces ? fbTex.Get() : g_lastResolve);
                if (!g_fotoPedida.empty())   // a imagem final (lida da placa depois do quadro, em SavePendingShots)
                {
                    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    CaptureRegion(list, imagem, st, (uint32_t)imagem->GetDesc().Width, imagem->GetDesc().Height, 0);
                    g_pendingShots.back().nome = g_fotoPedida;
                    g_fotoPedida.clear();
                }
                render::PresentTexture(imagem);
                gpuprof::AfterPresent();
                presented = true;
            }
        }
        SavePendingShots();
        // o contador do diagnostico ja avancou no SWAP: o quadro que esta sendo apresentado e o anterior
        if (uint64_t shown = diag::CurrentFrame() - 1; diag::Capturing(shown))
        {
            // F9: lista dos desenhos deste quadro (shaders, primitivas, superficies) junto das imagens
            std::string path = diag::CaptureDir(shown) + "\\desenhos.txt";
            if (FILE* f = fopen(path.c_str(), "w"))
            {
                auto& h = CurrentHistory();
                for (size_t i = 0; i < h.size(); i++) fprintf(f, "%4zu  %s\n", i, h[i].Describe().c_str());
                fclose(f);
            }
            Log("[diag] captura do quadro salva em %s", diag::CaptureDir(shown).c_str());
        }
        if (diag::g_flushShaders || diag::g_flushTextures)
        {
            // F5/F6/F7: a lista deste quadro ja foi enviada; espera a GPU e descarta o que foi feito com o modo antigo
            render::WaitIdle();
            if (diag::g_flushShaders) { g_shaderCache.clear(); g_psoCache.clear(); g_psoPendente.clear(); g_psoFalhou.clear(); diag::g_flushShaders = false; }
            if (diag::g_flushTextures) { g_textures.clear(); diag::g_flushTextures = false; }
        }
        g_prevFrontBuffer[g_frameNumber & 1] = frontBuffer & 0x1FFFF000;
        g_drewThisFrame = false;
        if (assinc::g_urgentesFeitos)
        {
            static uint32_t total = 0, linhas = 0;
            total += assinc::g_urgentesFeitos;
            if (linhas++ < 40)
                Log("[draw] passada de tela: %u shader(s)/pipeline(s) feitos na hora (fora da fila), espera %.0f ms, total %u",
                    assinc::g_urgentesFeitos, assinc::g_esperaQuadro, total);
            assinc::g_urgentesFeitos = 0;
        }
        assinc::g_esperaQuadro = 0;
        g_lastResolve = nullptr;
        g_usedPersistThisFrame = false;
        g_fullResolveThisFrame = false;
        g_drawsThisFrame = 0;
        g_frameNumber++;
        gpuprof::BeginFrame(g_frameNumber);
        CurrentHistory().clear();
        CollectGarbage();
        if (g_samplerResetPending)
        {
            render::ResetSamplers();   // espera a GPU: nenhuma lista usa mais os samplers antigos
            g_samplerTables.clear();
            g_samplerResetPending = false;
        }
        return presented || segurado;
    }
}
