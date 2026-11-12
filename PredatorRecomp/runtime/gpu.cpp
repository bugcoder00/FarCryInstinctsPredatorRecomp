// GPU Xenos - fase 1: processador de comandos (PM4), fences, vsync e janela com o framebuffer
#include "idioma.h"
#include "diag.h"
#include <mutex>
void WatchUpdate();   // watch.cpp
#include "perf.h"
uint64_t AudioFramesSubmitted();   // audio.cpp
void InputInit(HWND hwnd);         // input.cpp: mouse e controles.ini
bool InputWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);
std::string InputStatus();
bool BugKey(unsigned vk);          // content.cpp: F12 = pacote do bug
std::string BugStatus();
void ProfilerRegisterThread(const char* name);   // profiler.cpp
void StartFreezeWatch();   // content.cpp: vigia de travamento
void InputMarkMission();           // input.cpp: a fase comecou (roteiros "F..." do FCRECOMP_AUTOINPUT)
std::atomic<bool> g_saveOpened{ false };
std::atomic<uint64_t> g_missionStartFrame{ 0 };   // quadro em que a fase comecou (FCRECOMP_MISSAO)   // content.cpp: um jogo salvo foi aberto (FCRECOMP_MISSAOAPOSSAVE)
#include <intrin.h>
#include "thread.h"
#include "render.h"
#include "opcoes.h"
#include "xenos_draw.h"
#include <windows.h>
#include <dwmapi.h>
#include <atomic>
#include <chrono>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <memory>

std::string ProfilerStatus();   // profiler.cpp: aviso da medicao na tecla F3 (titulo da janela)

extern uint32_t g_ringBufferBase, g_ringBufferSizeLog2, g_ringRptrWriteBack;
extern uint32_t g_gfxInterruptCallback, g_gfxInterruptContext;

// versao das constantes dos shaders: [0] = vertex shader (0x4000), [1] = pixel shader (0x4400); bools/loops contam
// nos dois. Lida por xenos_draw (thread do processador de comandos, a mesma que escreve).
uint32_t g_constVersion[2] = {};

// Faixas do "predicated tiling" (lidos por xenos_draw). O jogo executa o mesmo buffer de comandos uma vez por faixa da
// tela (esquerda 0-768, direita 640-1280). Como a nossa EDRAM nao tem limite de tamanho, desenhamos a tela inteira na
// primeira faixa e pulamos os desenhos das outras: a geometria deixa de ser processada duas vezes (menos GPU e calor).
// g_tileState: 0 = sem faixas, 1 = primeira faixa, 2 = outra faixa (desenhos pulados), 3 = de volta a primeira depois
// das outras (comportamento normal). FCRECOMP_FAIXAS=1 volta ao modo antigo (cada faixa desenhada).
int g_tileState = 0;
// largura da tela inteira (pitch dos resolves feitos durante as faixas). Comeca em 1280 (a tela do jogo): com 0, o 1o
// quadro com faixas de cada processo so desenhava a faixa da esquerda (0-768) e os 40% da direita saiam pretos (item 31).
uint32_t g_tileFullWidth = 1280;
int64_t RoteiroTiqueAtual();     // input.cpp (registros de depuracao por tique do roteiro)

namespace
{
    constexpr uint32_t REG_CP_RB_RPTR = 0x01C4;
    constexpr uint32_t REG_CP_RB_WPTR = 0x01C5;

    std::atomic<uint64_t> g_swapCount{ 0 };
    std::atomic<uint64_t> g_drawCount{ 0 };
    std::mutex g_interruptMutex;

    // framebuffer apresentado no ultimo VdSwap
    std::atomic<uint32_t> g_frontBuffer{ 0 }, g_frontWidth{ 0 }, g_frontHeight{ 0 }, g_frontFormat{ 0 };
    HWND g_window = nullptr;
    std::atomic<bool> g_renderReady{ false };
    std::atomic<bool> g_drawReady{ false };   // executor de desenhos (xenos_draw) pronto
    // Preparacao da placa (DirectX + aquecimento) terminou, com ou sem sucesso. O jogo so comeca depois (GpuWaitReady):
    // antes ele rodava junto e o video da Ubisoft e o som picotavam com a compilacao ocupando os nucleos.
    std::atomic<bool> g_prepDone{ false };

    // lido uma vez: getenv em codigo chamado a cada desenho/registro custava ~6% da thread da GPU
    const bool kGpuLogEnv = getenv("FCRECOMP_GPULOG") != nullptr;

    uint32_t PhysToGuest(uint32_t phys) { return 0xA0000000 | (phys & 0x1FFFFFFF); }

    // A GPU le memoria com um "modo de endian" nos 2 bits baixos do endereco
    uint32_t GpuSwap(uint32_t raw, uint32_t endian)
    {
        switch (endian & 3)
        {
        case 1: return ((raw & 0x00FF00FF) << 8) | ((raw & 0xFF00FF00) >> 8);   // 8in16
        case 2: return __builtin_bswap32(raw);                                  // 8in32
        case 3: return (raw << 16) | (raw >> 16);                               // 16in32
        default: return raw;
        }
    }

    uint32_t GpuRead(uint32_t addrWithEndian)
    {
        uint32_t raw = *(uint32_t*)G(PhysToGuest(addrWithEndian & ~3u));
        return GpuSwap(raw, addrWithEndian & 3);
    }

    // FCRECOMP_FENCELOG=endereco fisico (hex, ex. 5000): anota cada escrita da GPU nesse endereco (cercas do D3D)
    uint32_t g_fenceLogAddr = getenv("FCRECOMP_FENCELOG") ? (uint32_t)strtoul(getenv("FCRECOMP_FENCELOG"), nullptr, 16) : 0xFFFFFFFF;
    const char* g_fenceLogSource = "";
    size_t g_ibDepth = 0;   // profundidade de INDIRECT_BUFFER (para o registro das cercas)
    void GpuWrite(uint32_t addrWithEndian, uint32_t value)
    {
        *(uint32_t*)G(PhysToGuest(addrWithEndian & ~3u)) = GpuSwap(value, addrWithEndian & 3);
        if (((addrWithEndian & ~3u) & 0x1FFFFFFF) == g_fenceLogAddr)
            Log("[cerca] %.0f ms %s escreveu %08X (quadro %llu, desenhos %llu, ib %zu)", perf::Now(), g_fenceLogSource, value,
                (unsigned long long)g_swapCount.load(), (unsigned long long)g_drawCount.load(), g_ibDepth);
    }

    // ------------------------------------------------------------ interrupcoes

    void DispatchInterrupt(GuestCpu& ctx, uint32_t source, uint32_t cpu)
    {
        if (!g_gfxInterruptCallback) return;
        std::lock_guard lock(g_interruptMutex);
        // o callback do jogo usa o numero da CPU atual (PCR+0x10C) para saber qual pedido atender
        W8(Gpr32(ctx, 13) + 0x10C, (uint8_t)cpu);
        SetGpr(ctx, 3, source);
        SetGpr(ctx, 4, g_gfxInterruptContext);
        GuestCall(ctx, g_gfxInterruptCallback);
    }

    void VsyncThread()
    {
        GuestCpu* ctx = AttachGuestThread("vsync");
        auto next = std::chrono::steady_clock::now();
        // depuracao: FCRECOMP_VSYNCX=N gera os vblanks N vezes mais rapido (mede o quanto o jogo depende deles)
        static const int speed = std::max(1, getenv("FCRECOMP_VSYNCX") ? atoi(getenv("FCRECOMP_VSYNCX")) : 1);
        for (;;)
        {
            next += std::chrono::microseconds(16667 / speed);
            std::this_thread::sleep_until(next);
            DispatchInterrupt(*ctx, 0, 2);
        }
    }

    // ------------------------------------------------------------ processador de comandos

    // Consultas de oclusao: endereco (do Xbox) das estruturas de contagem ainda nao respondidas. O jogo pode gravar
    // a marca 0xFFFFFEED um pouco depois de o comando passar pela GPU (corrida entre as threads); antes, nesses
    // casos a consulta ficava sem resposta e o objeto sumia naquele quadro (maos do personagem piscando).
    // Reconferimos a lista a cada comando, a cada troca de quadro e quando o processador de comandos esta parado.
    struct PendingQuery { uint32_t guest; uint64_t frame; };
    std::vector<PendingQuery> g_pendingQueries;
    std::mutex g_queryMutex;

    // Amostras visiveis da consulta cujo resultado fica em "guest" (= bloco + 0x40): contagem real da GPU do PC
    // (do quadro anterior, xdraw::QueryResult). FCRECOMP_OCLUSAOFIXA=1 volta ao antigo "sempre 1000 (visivel)".
    // reg = endereco fisico do registro da consulta (o mesmo passado ao xdraw::QueryBegin)
    uint32_t QuerySamples(uint32_t reg)
    {
        static const bool fixed = getenv("FCRECOMP_OCLUSAOFIXA") != nullptr;
        if (fixed || !g_drawReady) return 1000;
        return xdraw::QueryResult(reg, 1000);
    }

    // depuracao (FCRECOMP_OCLUSAOLOG=t...): o numero que o GetData do jogo (sub_82741238) calcularia para a consulta cujo
    // ultimo registro (faixa 1 de 2) e "reg": soma, nos 2 registros, de fim.ZPass_A+B - inicio.ZPass_A+B
    void LogVistoPeloJogo(uint32_t reg, bool antiga)
    {
        static const char* qlog = getenv("FCRECOMP_OCLUSAOLOG");
        if (!qlog || qlog[0] != 't' || g_tileState != 2) return;
        int64_t ini = atoll(qlog + 1), fim = strchr(qlog, '-') ? atoll(strchr(qlog, '-') + 1) : ini, tq = RoteiroTiqueAtual();
        if (tq < ini || tq > fim) return;
        uint32_t soma = 0;
        for (uint32_t r : { reg - 0x40, reg })
        {
            const uint32_t* c = (const uint32_t*)G(PhysToGuest(r));
            soma += c[4] + c[5] - c[12] - c[13];
        }
        Log("[oclusao] tique %lld: consulta %08X+%08X, o jogo ve %d amostras (%s)", (long long)tq, reg - 0x40, reg, (int32_t)soma, antiga ? "antigo" : "novo");
    }

    bool AnswerQueryAt(uint32_t guest)
    {
        const uint32_t kFinished = 0xEDFEFFFFu;   // 0xFFFFFEED gravado em big-endian, lido como little-endian
        uint32_t* c = (uint32_t*)G(guest);
        if (c[4] == kFinished || c[2] == kFinished || c[0] == kFinished)
        {
            uint32_t n = QuerySamples((guest & 0x1FFFFFFF) - 0x40);
            // FCRECOMP_OCLUSAOLOG=t4370-4445: cada resposta nesses tiques do roteiro (bloco, amostras, troca de tela)
            static const char* qlog = getenv("FCRECOMP_OCLUSAOLOG");
            if (qlog && qlog[0] == 't')
            {
                int64_t ini = atoll(qlog + 1), fim = strchr(qlog, '-') ? atoll(strchr(qlog, '-') + 1) : ini;
                int64_t tq = RoteiroTiqueAtual();
                if (tq >= ini && tq <= fim)
                    Log("[oclusao] tique %lld troca %llu: bloco %08X respondido com %u%s", (long long)tq, (unsigned long long)g_swapCount.load(),
                        (guest & 0x1FFFFFFF) - 0x40, n, xdraw::QueryResult((guest & 0x1FFFFFFF) - 0x40, 0xFFFFFFFFu) == 0xFFFFFFFFu ? " (sem contagem: padrao)" : "");
            }
            memset(c, 0, 32);
            c[0] = n;   // Total_A
            c[4] = n;   // ZPass_A
            return true;
        }
        return false;
    }

    // Item 40: consulta NOVA (o bloco ainda nao tem contagem da GPU do PC: a caixa foi desenhada neste quadro e a
    // contagem so chega no proximo) ficava com o padrao "1000 amostras visiveis": um brilho de sol escondido atras
    // de um poste (contagem real 22) aparecia inteiro por 1 quadro (a luz branca piscando na cutscene da 22_DUNEB,
    // gravacao 2026-11-10_17-32, tique 770). Agora ela fica "ainda nao pronta" (a marca 0xFFFFFEED do ultimo
    // registro continua) ate a contagem real chegar, como num console com a GPU atrasada: o GetData do jogo pergunta
    // de novo no quadro seguinte. Desiste depois de 3 trocas de tela (responde 0). FCRECOMP_OCLUSAOPADRAO=1 volta ao
    // antigo (1000 na hora).
    struct QueryAdiada { uint32_t reg1, reg2; uint64_t troca; };   // reg2 = registro da faixa 2 (0 sem faixas)
    std::vector<QueryAdiada> g_queriesAdiadas;
    std::vector<uint32_t> g_semContagem;   // registros da faixa 1 fechados sem contagem neste quadro
    bool OclusaoAdiar() { static const bool on = getenv("FCRECOMP_OCLUSAOPADRAO") == nullptr; return on; }
    void ResponderAdiadas(uint64_t troca)
    {
        size_t keep = 0;
        for (auto& q : g_queriesAdiadas)
        {
            uint32_t n = xdraw::QueryResult(q.reg1, 0xFFFFFFFFu);
            if (n == 0xFFFFFFFFu && troca <= q.troca + 3) { g_queriesAdiadas[keep++] = q; continue; }
            if (n == 0xFFFFFFFFu) n = 0;
            uint32_t* c = (uint32_t*)G(PhysToGuest(q.reg1));
            memset(c, 0, 32);
            c[0] = n;   // Total_A
            c[4] = n;   // ZPass_A
            if (q.reg2) memset(G(PhysToGuest(q.reg2)), 0, 32);   // faixa 2: nada desenhado (0) e tira a marca
            static const char* qlog = getenv("FCRECOMP_OCLUSAOLOG");
            if (qlog && qlog[0] == 't')
            {
                int64_t ini = atoll(qlog + 1), fim = strchr(qlog, '-') ? atoll(strchr(qlog, '-') + 1) : ini, tq = RoteiroTiqueAtual();
                if (tq >= ini && tq <= fim)
                    Log("[oclusao] tique %lld troca %llu: consulta nova %08X respondida com %u (adiada desde a troca %llu)", (long long)tq,
                        (unsigned long long)troca, q.reg1, n, (unsigned long long)q.troca);
            }
        }
        g_queriesAdiadas.resize(keep);
    }

    void AnswerPendingQueries(uint64_t frame)
    {
        std::lock_guard lock(g_queryMutex);
        size_t keep = 0;
        for (auto& q : g_pendingQueries)
        {
            if (AnswerQueryAt(q.guest)) continue;            // respondida
            if (frame > q.frame + 4) continue;               // velha demais: desiste
            g_pendingQueries[keep++] = q;
        }
        g_pendingQueries.resize(keep);
    }

    GuestCpu* g_cpCtx = nullptr;

    // Espera curta e precisa: no Windows, sleep_for(50 us) pode dormir 1-15 ms, e cada atraso desses
    // segurava a CPU do jogo (que espera a GPU andar) e a GPU (que espera o jogo). Gira um pouco e depois
    // usa um temporizador de alta resolucao.
    void ShortWait(int spin, int micros)
    {
        if (spin < 200) { _mm_pause(); return; }
        if (spin < 400) { SwitchToThread(); return; }
        thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)micros * 10;
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
        else std::this_thread::sleep_for(std::chrono::microseconds(micros));
    }

    // Espera do processador de comandos (registrador/memoria, fila vazia): como ShortWait, mas esperas longas dormem mais
    // por vez (50 us -> 200 us depois de ~1 ms -> 500 us depois de ~5 ms). Antes cada espera pelo sinal de video (~16 ms)
    // acordava a thread centenas de vezes. FCRECOMP_SPINWAIT=1 mantem 50 us sempre.
    void CpWait(int spin)
    {
        static const bool spinOnly = getenv("FCRECOMP_SPINWAIT") != nullptr;
        int micros = 50;
        if (!spinOnly && spin > 400 + 20) micros = 200;    // ~20 x 50 us = 1 ms
        if (!spinOnly && spin > 400 + 40) micros = 500;    // + 20 x 200 us = ~5 ms
        ShortWait(spin, micros);
    }

    bool Compare(uint32_t func, uint32_t value, uint32_t ref)
    {
        switch (func & 7)
        {
        case 0: return false;
        case 1: return value < ref;
        case 2: return value <= ref;
        case 3: return value == ref;
        case 4: return value != ref;
        case 5: return value >= ref;
        case 6: return value > ref;
        default: return true;
        }
    }

    // Registradores: CPU e GPU enxergam os mesmos valores. O runtime acessa pelo mapeamento sem protecao.
    uint32_t Reg(uint32_t index) { return __builtin_bswap32(((uint32_t*)g_gpuRegsHost)[index & 0xFFFF]); }
    void SetReg(uint32_t index, uint32_t value) { ((uint32_t*)g_gpuRegsHost)[index & 0xFFFF] = __builtin_bswap32(value); }

    constexpr uint32_t REG_SCRATCH_UMSK = 0x01DC;
    constexpr uint32_t REG_SCRATCH_ADDR = 0x01DD;
    constexpr uint32_t REG_SCRATCH_REG0 = 0x0578;

    // Efeitos colaterais de escrever num registrador (vale para escritas da GPU e da CPU)
    void OnRegisterWritten(uint32_t index, uint32_t value)
    {
        // registradores "scratch": se habilitados, o hardware copia o valor para a memoria
        if (index >= REG_SCRATCH_REG0 && index < REG_SCRATCH_REG0 + 8)
        {
            uint32_t n = index - REG_SCRATCH_REG0;
            if (Reg(REG_SCRATCH_UMSK) & (1u << n))
            {
                W32(PhysToGuest(Reg(REG_SCRATCH_ADDR) + n * 4), value);
                if (((Reg(REG_SCRATCH_ADDR) + n * 4) & 0x1FFFFFFF) == g_fenceLogAddr)
                    Log("[cerca] scratch%u escreveu %08X", n, value);
            }
            else if (((Reg(REG_SCRATCH_ADDR) + n * 4) & 0x1FFFFFFF) == g_fenceLogAddr)
                Log("[cerca] scratch%u = %08X NAO copiado (umsk %08X)", n, value, Reg(REG_SCRATCH_UMSK));
            if (kGpuLogEnv)
                Log("[gpu] scratch%u = %08X (umsk %08X)", n, value, Reg(REG_SCRATCH_UMSK));
        }
    }

    int g_logRegsAfterScratch = 0;

    uint32_t g_regSrc = 0;   // endereco do valor no buffer de comandos (pacotes tipo 0), para o FCRECOMP_NANLOG

    void WriteRegister(uint32_t index, uint32_t value)
    {
        index &= 0xFFFF;
        SetReg(index, value);
        // versao das constantes dos shaders (xenos_draw reaproveita o envio anterior se nada mudou)
        if (index >= 0x4000 && index < 0x4800) g_constVersion[index >= 0x4400]++;
        else if (index >= 0x4900 && index < 0x4928) { g_constVersion[0]++; g_constVersion[1]++; }
        // FCRECOMP_NANLOG=1: constantes de shader que chegam como NaN
        if (static const bool nanLog = getenv("FCRECOMP_NANLOG") != nullptr; nanLog && index >= 0x4080 && index < 0x40A0 &&
            (value & 0x7F800000u) == 0x7F800000u && (value & 0x7FFFFFu))
        {
            static int n = 0;
            if (n++ < 40)
                Log("[nan] quadro %llu: reg %04X (%s c%u.%c) = %08X (buffer %08X)", (unsigned long long)g_swapCount.load(), index,
                    index >= 0x4400 ? "PS" : "VS", ((index - 0x4000) & 1023) / 4, "xyzw"[index & 3], value, g_regSrc);
            // na primeira vez procura na memoria do jogo de onde a matriz veio (12+ palavras NaN seguidas)
            static bool procurou = false;
            if (!procurou)
            {
                procurou = true;
                int achados = 0;
                uint32_t a = 0;
                while (a < 0xC0000000u && achados < 40)
                {
                    MEMORY_BASIC_INFORMATION mi{};
                    if (!VirtualQuery(g_base + a, &mi, sizeof(mi))) break;
                    uint32_t fim = (uint32_t)std::min<uint64_t>(0xC0000000ull, (uint64_t)((uint8_t*)mi.BaseAddress - g_base) + mi.RegionSize);
                    if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                        for (uint32_t x = a; x + 48 <= fim && achados < 40; x += 4)
                        {
                            if (R32(x) != value) continue;
                            uint32_t k = 0;
                            while (k < 64 && x + k * 4 < fim && R32(x + k * 4) == value) k++;
                            if (k >= 12) { Log("[nan]   memoria %08X: %u palavras NaN seguidas", x, k); achados++; }
                            x += k * 4;
                        }
                    if (fim <= a) break;
                    a = fim;
                }
            }
        }
        OnRegisterWritten(index, value);
        if (g_logRegsAfterScratch > 0 && index < 0x2000)
        {
            g_logRegsAfterScratch--;
            Log("[gpu]   reg %04X = %08X", index, value);
        }
        if (index == REG_SCRATCH_REG0 && kGpuLogEnv)
            g_logRegsAfterScratch = 12;
    }

    // ------------------------------------------------------------ escritas da CPU nos registradores
    // A pagina vista pelo jogo e somente-leitura. Uma escrita gera excecao: liberamos a pagina,
    // executamos so aquela instrucao (passo a passo) e depois processamos o valor escrito.

    thread_local uint8_t* t_pendingMmio = nullptr;

    LONG CALLBACK MmioWriteHandler(EXCEPTION_POINTERS* e)
    {
        auto* rec = e->ExceptionRecord;
        uint8_t* mmio = g_base + GPU_MMIO_BASE;

        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->ExceptionInformation[0] == 1)
        {
            uint8_t* addr = (uint8_t*)rec->ExceptionInformation[1];
            if (addr < mmio || addr >= mmio + GPU_MMIO_SIZE)
                return EXCEPTION_CONTINUE_SEARCH;
            DWORD old;
            VirtualProtect((void*)((uintptr_t)addr & ~0xFFFull), 0x1000, PAGE_READWRITE, &old);
            t_pendingMmio = addr;
            e->ContextRecord->EFlags |= 0x100;   // passo a passo
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && t_pendingMmio)
        {
            uint8_t* addr = t_pendingMmio;
            t_pendingMmio = nullptr;
            DWORD old;
            VirtualProtect((void*)((uintptr_t)addr & ~0xFFFull), 0x1000, PAGE_READONLY, &old);
            uint32_t index = (uint32_t)((addr - mmio) / 4);
            static std::atomic<int> logged{ 0 };
            if (index != 0x1C5 && logged++ < 100)
                Log("[gpu] CPU escreveu registrador %04X = %08X", index, Reg(index));
            OnRegisterWritten(index, Reg(index));
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void ExecuteBuffer(uint32_t ptr, uint32_t count);
    void MaybeSaveFrame(uint64_t frame);
    std::string g_fotoDesteQuadro;   // roteiro gravado: foto pedida para o quadro que esta sendo apresentado
    void SalvarBmp(const std::string& nome, const uint32_t* px, uint32_t w, uint32_t h);
    std::vector<std::pair<uint32_t, uint32_t>> g_ibStack;   // pilha de INDIRECT_BUFFER (para depuracao)
    uint64_t g_binMask = ~0ull, g_binSelect = ~0ull;         // predicacao do "predicated tiling"
    uint64_t g_tileFirst = 0, g_tileUnion = 0, g_tileUnionLast = 0;   // primeira faixa da secao; uniao das faixas
    bool g_tileOthersSeen = false;
    bool MergeTiles() { static const bool off = getenv("FCRECOMP_FAIXAS") != nullptr; return !off; }

    // chamado depois de cada mudanca de SET_BIN_SELECT
    void OnBinSelect()
    {
        if (g_binSelect == ~0ull)
        {
            if (g_tileFirst) g_tileUnionLast = g_tileUnion;   // fim da secao: guarda a uniao para a proxima
            g_tileFirst = 0; g_tileUnion = 0; g_tileOthersSeen = false; g_tileState = 0;
            return;
        }
        if (!g_tileFirst) { g_tileFirst = g_binSelect; g_tileUnion = 0; g_tileOthersSeen = false; }
        g_tileUnion |= g_binSelect;
        if (g_binSelect != g_tileFirst) { g_tileOthersSeen = true; g_tileState = 2; }
        else g_tileState = g_tileOthersSeen ? 3 : 1;
    }
    bool g_seqLog = false;                                   // FCRECOMP_SEQLOG: ligado so nos quadros pedidos
    bool ReadFrontBuffer(std::vector<uint32_t>& pixels, uint32_t& w, uint32_t& h);

    // ------------------------------------------------------------ shaders
    // O jogo carrega o microcode dos shaders (formato Xenos) com IM_LOAD. Guardamos por hash.
    struct Shader
    {
        uint32_t type;                   // 0 = vertex, 1 = pixel
        std::vector<uint32_t> code;      // dwords (ja em ordem do host)
        uint64_t hash;
    };
    std::mutex g_shaderMutex;
    std::unordered_map<uint64_t, std::shared_ptr<Shader>> g_shaders;
    std::shared_ptr<Shader> g_currentShader[2];

    uint64_t HashDwords(const std::vector<uint32_t>& d)
    {
        uint64_t h = 0xCBF29CE484222325ull;   // FNV-1a
        for (uint32_t v : d)
            for (int i = 0; i < 4; i++) { h ^= (v >> (i * 8)) & 0xFF; h *= 0x100000001B3ull; }
        return h;
    }

    void LoadShader(uint32_t type, uint32_t guestAddr, uint32_t sizeDwords)
    {
        if (type > 1 || sizeDwords == 0 || sizeDwords > 0x10000) return;
        std::vector<uint32_t> code(sizeDwords);
        for (uint32_t i = 0; i < sizeDwords; i++)
            code[i] = R32(guestAddr + i * 4);
        uint64_t h = HashDwords(code);

        std::lock_guard lock(g_shaderMutex);
        auto& s = g_shaders[h];
        if (!s)
        {
            s = std::make_shared<Shader>(Shader{ type, std::move(code), h });
            // FCRECOMP_DUMPSHADERS=1: grava cada shader novo em build/shaders/
            if (getenv("FCRECOMP_DUMPSHADERS"))
            {
                CreateDirectoryA("shaders", nullptr);
                char name[64];
                snprintf(name, sizeof(name), "shaders\\%s_%016llX.bin", type ? "ps" : "vs", (unsigned long long)h);
                if (FILE* f = fopen(name, "wb"))
                {
                    for (uint32_t v : s->code) { uint32_t be = __builtin_bswap32(v); fwrite(&be, 4, 1, f); }
                    fclose(f);
                }
            }
            Log("[gpu] shader novo: %s %016llX (%u dwords) - total %zu", type ? "pixel" : "vertex",
                (unsigned long long)h, sizeDwords, g_shaders.size());
        }
        g_currentShader[type] = s;
    }

    // ------------------------------------------------------------ texturas usadas nos desenhos
    // Fetch constant de textura (6 dwords a partir do registrador 0x4800 + 6*n):
    //   d0: tipo (bits 0-1 = 2), pitch/32 (bits 22-30), tiled (bit 31)
    //   d1: formato (bits 0-5), base (bits 12-31)   d2: largura-1 (bits 0-12), altura-1 (bits 13-25)
    struct TexInfo { uint32_t base, width, height, format, pitch; bool tiled; };

    bool ReadTexFetch(uint32_t slot, TexInfo& t)
    {
        uint32_t d0 = Reg(0x4800 + slot * 6), d1 = Reg(0x4800 + slot * 6 + 1), d2 = Reg(0x4800 + slot * 6 + 2);
        if ((d0 & 3) != 2 || !(d1 & 0xFFFFF000)) return false;
        t.base = d1 & 0xFFFFF000;
        t.format = d1 & 0x3F;
        t.width = (d2 & 0x1FFF) + 1;
        t.height = ((d2 >> 13) & 0x1FFF) + 1;
        t.pitch = ((d0 >> 22) & 0x1FF) * 32;
        t.tiled = d0 >> 31;
        return true;
    }

    // texturas de 8 bits (formato k_8 = 2) do ultimo desenho: candidatas a planos Y/Cb/Cr do Bink
    std::mutex g_videoMutex;
    std::vector<TexInfo> g_lastPlanes, g_videoPlanes;

    void OnDraw()
    {
        std::vector<TexInfo> planes;
        for (uint32_t s = 0; s < 32; s++)
        {
            TexInfo t;
            if (ReadTexFetch(s, t) && t.format == 2)
                planes.push_back(t);
        }
        static int logged = 0;
        if (kGpuLogEnv && logged < 40)
        {
            for (uint32_t s = 0; s < 32; s++)
            {
                TexInfo t;
                if (ReadTexFetch(s, t))
                {
                    logged++;
                    Log("[gpu] desenho %llu: textura %u base=%08X %ux%u formato=%u pitch=%u tiled=%d",
                        (unsigned long long)g_drawCount.load(), s, t.base, t.width, t.height, t.format, t.pitch, t.tiled);
                }
            }
        }
        if (planes.size() >= 3)
        {
            std::lock_guard lock(g_videoMutex);
            g_lastPlanes = planes;
        }
    }

    // Executa um pacote tipo 3. Retorna quantos dwords consumiu (alem do cabecalho)
    bool g_gpuLog = kGpuLogEnv;

    void ExecuteType3(uint32_t opcode, uint32_t addr, uint32_t count)
    {
        auto D = [&](uint32_t i) { return R32(addr + i * 4); };
        if (g_gpuLog && opcode != 0x2D && opcode != 0x55 && opcode != 0x22 && opcode != 0x36)
            Log("[gpu] pacote %02X (%u dwords) %08X %08X %08X %08X", opcode, count,
                count > 0 ? D(0) : 0, count > 1 ? D(1) : 0, count > 2 ? D(2) : 0, count > 3 ? D(3) : 0);

        switch (opcode)
        {
        case 0x3F:   // INDIRECT_BUFFER
        case 0x37:   // INDIRECT_BUFFER_PFD
            g_ibStack.push_back({ addr - 4, D(1) & 0xFFFFF });
            g_ibDepth = g_ibStack.size();   // quem chamou (endereco do pacote) e tamanho
            ExecuteBuffer(PhysToGuest(D(0)), D(1) & 0xFFFFF);
            g_ibStack.pop_back();
            g_ibDepth = g_ibStack.size();
            break;

        case 0x3C:   // WAIT_REG_MEM
        {
            uint32_t info = D(0), poll = D(1), ref = D(2), mask = D(3);
            perf::Scope ps(perf::WaitReg);
            const double avStart = perf::AvLog() && g_swapCount < 6 ? perf::Now() : 0;
            for (int spin = 0;; spin++)
            {
                uint32_t value = (info & 0x10) ? GpuRead(poll) : Reg(poll & 0xFFFF);
                if (Compare(info, value & mask, ref))
                {
                    if (avStart && perf::Now() - avStart > 2)
                        Log("[av] %.1f ms: placa esperou %.1f ms por %s %08X (valor %08X & %08X, funcao %u, ref %08X)", perf::Now(),
                            perf::Now() - avStart, (info & 0x10) ? "memoria" : "registrador", poll, value, mask, info & 7, ref);
                    break;
                }
                if (spin == 20000)
                    Log("[gpu] WAIT_REG_MEM demorando: %s %08X (valor %08X & %08X, funcao %u, ref %08X)",
                        (info & 0x10) ? "memoria" : "registrador", poll, value, mask, info & 7, ref);
                CpWait(spin);
            }
            break;
        }

        case 0x21:   // REG_RMW
        {
            uint32_t info = D(0);
            uint32_t v = Reg(info & 0x1FFF);
            v &= (info & 0x80000000) ? Reg(D(1) & 0x1FFF) : D(1);
            v |= (info & 0x40000000) ? Reg(D(2) & 0x1FFF) : D(2);
            WriteRegister(info & 0x1FFF, v);
            break;
        }

        case 0x45:   // COND_WRITE
        {
            uint32_t info = D(0), poll = D(1), ref = D(2), mask = D(3), dst = D(4), data = D(5);
            uint32_t value = (info & 0x10) ? GpuRead(poll) : Reg(poll & 0xFFFF);
            if (Compare(info, value & mask, ref))
            {
                if (info & 0x100) { g_fenceLogSource = "COND_WRITE"; GpuWrite(dst, data); g_fenceLogSource = ""; }
                else WriteRegister(dst, data);
            }
            break;
        }

        case 0x58:   // EVENT_WRITE_SHD: escreve um valor de "fence" na memoria
        {
            uint32_t initiator = D(0), dst = D(1), value = D(2);
            if (initiator & 0x80000000) value = (uint32_t)g_swapCount;
            static std::unordered_map<uint32_t, uint32_t> lastFence;
            uint32_t& prev = lastFence[dst];
            if (value < prev && prev - value < 0x10000)
            {
                static int n = 0;
                if (n++ < 6)
                {
                    std::string chain;
                    for (auto& c : g_ibStack) { char b[40]; snprintf(b, sizeof(b), " %08X(+%X)", c.first, c.second); chain += b; }
                    Log("[gpu] fence voltou: %08X de %X para %X (pacote em %08X) chamadas:%s", dst, prev, value, addr, chain.c_str());
                }
            }
            prev = value;
            g_fenceLogSource = "EVENT_WRITE_SHD";
            GpuWrite(dst, value);
            g_fenceLogSource = "";
            break;
        }

        case 0x59:   // EVENT_WRITE_EXT: extents de tela
        {
            uint32_t dst = D(1);
            uint16_t ext[6] = { 0, 0x2000, 0, 0x2000, 0, 1 };
            uint32_t endian = dst & 3;
            uint32_t g = PhysToGuest(dst & ~3u);
            for (int i = 0; i < 6; i++)
                W16(g + i * 2, endian ? ext[i] : __builtin_bswap16(ext[i]));
            break;
        }

        case 0x3D:   // MEM_WRITE
        {
            uint32_t dst = D(0);
            g_fenceLogSource = "MEM_WRITE";
            for (uint32_t i = 1; i < count; i++)
                GpuWrite(dst + (i - 1) * 4, D(i));
            g_fenceLogSource = "";
            break;
        }

        case 0x5A:   // EVENT_WRITE_EXT: grava os limites da tela desenhada (em blocos de 8 pixels) no endereco dado
        {
            // tela inteira (0..2048 em x e y), z de 0 a 1; 6 valores de 16 bits
            const uint16_t extents[6] = { 0, 2048 >> 3, 0, 2048 >> 3, 0, 1 };
            uint32_t dst = PhysToGuest(D(1) & ~3u);
            for (int i = 0; i < 6; i++) W16(dst + i * 2, extents[i]);
            break;
        }

        case 0x5B:   // EVENT_WRITE_ZPD: consulta de oclusao ("quantos pixels passaram no teste de profundidade?")
        {
            // Estrutura de contagem (little-endian, 8 dwords): Total_A Total_B ZFail_A ZFail_B ZPass_A ZPass_B
            // StencilFail_A StencilFail_B. No fim da consulta o Direct3D marca 0xFFFFFEED nos campos e espera a GPU
            // trocar pelas contagens. Neste jogo cada consulta usa um bloco de 0x80 bytes: os comandos chegam com
            // RB_SAMPLE_COUNT_ADDR = bloco+0x20 e bloco+0x00, e a estrutura marcada fica em bloco+0x40, so nos campos A
            // (procurar ZPass_A e ZPass_B marcados no proprio endereco nunca batia). Entao procuramos a
            // marca nas estruturas de endereco ate endereco+0x40 e respondemos "1000 amostras visiveis".
            // Sem isso a resposta ficava zero e o jogo achava que objetos (ex.: a agua) estavam escondidos.
            uint32_t addr = Reg(0x2325);   // RB_SAMPLE_COUNT_ADDR (fisico)
            // Bugs 20/21 (luz atravessando modelos, sol piscando debaixo d'agua): o formato real, lido no proprio jogo
            // (sub_82741090 emite, sub_82740978 monta os pacotes, sub_82741238 = GetData): cada consulta tem UM
            // registro de 64 bytes POR FAIXA do tiling (aqui 2: ponteiros em consulta+24, quantidade em +144). Para
            // cada faixa um SET_CONSTANT predicado pela faixa poe RB_SAMPLE_COUNT_ADDR no registro dela: inicio em
            // registro+0x20, fim em registro+0x00. No inicio o jogo marca 0xFFFFFEED (campos 0, 8, 16 e 24) so no
            // ULTIMO registro, e o GetData espera essa marca sumir e soma, em todos os registros,
            // (fim.ZPass_A + fim.ZPass_B) - (inicio.ZPass_A + inicio.ZPass_B) (lidos little-endian).
            // Antes supunhamos blocos de 0x80 com a estrutura em +0x40: a consulta da faixa 1 (onde nao desenhamos
            // nada, ver g_tileState) ficava com uma contagem velha, o registro da faixa 0 nunca era escrito (lixo na
            // soma) e a resposta ia parar no registro da consulta vizinha. O brilho do sol variava de quadro a quadro
            // e halos apareciam atras de modelos. Agora cada ZPD grava o seu meio registro como a GPU: inicio = zeros,
            // fim = contagem (do quadro anterior) da caixa de teste desenhada entre os dois. FCRECOMP_OCLUSAOANTIGA=1
            // volta ao antigo (comparacao).
            static const bool antiga = getenv("FCRECOMP_OCLUSAOANTIGA") != nullptr;
            if (addr && !antiga)
            {
                const uint32_t reg = addr & 0x1FFFFFC0u, meio = addr & 0x3F;
                if (g_drawReady)
                {
                    if (meio == 0x20) xdraw::QueryBegin(reg);
                    else if (meio == 0x00) xdraw::QueryEnd(reg);
                }
                // item 40: consulta nova sem contagem ainda -> adiada (veja ResponderAdiadas)
                if (meio == 0x00 && g_drawReady && OclusaoAdiar())
                {
                    if (g_tileState == 2)
                    {
                        auto it = std::find(g_semContagem.begin(), g_semContagem.end(), reg - 0x40);
                        if (it != g_semContagem.end())
                        {
                            g_semContagem.erase(it);
                            if (g_queriesAdiadas.size() < 1024) g_queriesAdiadas.push_back({ reg - 0x40, reg, g_swapCount.load() });
                            if (meio == 0x00) LogVistoPeloJogo(reg, false);
                            break;   // nao grava: a marca de fim continua (GetData: "ainda nao")
                        }
                    }
                    else if (xdraw::QueryResult(reg, 0xFFFFFFFFu) == 0xFFFFFFFFu)
                    {
                        if (g_tileState == 0)
                        {
                            if (g_queriesAdiadas.size() < 1024) g_queriesAdiadas.push_back({ reg, 0, g_swapCount.load() });
                            LogVistoPeloJogo(reg, false);
                            break;
                        }
                        if (g_semContagem.size() > 256) g_semContagem.clear();
                        g_semContagem.push_back(reg);   // a resposta definitiva sai quando a faixa 2 fechar
                    }
                }
                if (meio == 0x00 || meio == 0x20)
                {
                    uint32_t* c = (uint32_t*)G(PhysToGuest(addr & ~3u));
                    uint32_t n = meio == 0x00 ? QuerySamples(reg) : 0;
                    memset(c, 0, 32);
                    c[0] = n;   // Total_A
                    c[4] = n;   // ZPass_A
                    static const char* qlog = getenv("FCRECOMP_OCLUSAOLOG");
                    if (qlog && qlog[0] == 't' && meio == 0x00)
                    {
                        int64_t ini = atoll(qlog + 1), fim = strchr(qlog, '-') ? atoll(strchr(qlog, '-') + 1) : ini, tq = RoteiroTiqueAtual();
                        if (tq >= ini && tq <= fim)
                            Log("[oclusao] tique %lld troca %llu: registro %08X (faixa %d) respondido com %u", (long long)tq,
                                (unsigned long long)g_swapCount.load(), reg, g_tileState, n);
                    }
                    diag::Note(diag::Other, "consulta de oclusao respondida (contagem da GPU do quadro anterior)");
                }
                else diag::Note(diag::Other, "consulta de oclusao: endereco fora do formato (registro + 0x00/0x20)");
                if (meio == 0x00) LogVistoPeloJogo(reg, false);
                break;
            }
            // ---- antigo (FCRECOMP_OCLUSAOANTIGA=1)
            // Ordem neste jogo: ZPD em bloco+0x20 abre a consulta, vem a caixa de teste (2 desenhos), ZPD em bloco+0x00
            // fecha. Os desenhos no meio sao contados pela GPU do PC (xdraw::QueryBegin/End).
            if (addr && g_drawReady)
            {
                if ((addr & 0x7F) == 0x20) xdraw::QueryBegin(addr & 0x1FFFFF80u);
                else if ((addr & 0x7F) == 0x00) xdraw::QueryEnd(addr & 0x1FFFFF80u);
            }
            if (addr)
            {
                bool answered = false;
                for (uint32_t off = 0; off <= 0x40; off += 0x20)
                    answered |= AnswerQueryAt(PhysToGuest((addr & ~3u) + off));
                // comando no inicio do bloco (bloco+0x00): a estrutura de fim (bloco+0x40) pode receber a marca depois
                if (!answered && (addr & 0x7F) == 0)
                {
                    std::lock_guard lock(g_queryMutex);
                    if (g_pendingQueries.size() < 4096) g_pendingQueries.push_back({ PhysToGuest((addr & ~3u) + 0x40), g_swapCount.load() });
                }
                AnswerPendingQueries(g_swapCount.load());
                if ((addr & 0x3F) == 0) LogVistoPeloJogo(addr & 0x1FFFFFC0u, true);
                diag::Note(diag::Other, answered ? "consulta de oclusao respondida (contagem da GPU do quadro anterior)" : "consulta de oclusao: comando sem marca de fim");
            }
            break;
        }

        case 0x23:   // VIZ_QUERY: inicio/fim de uma consulta de visibilidade; no fim dizemos "visivel"
        {
            uint32_t d0 = D(0), id = d0 & 0x3F;
            if (d0 & 0x100)
            {
                if (id < 32) SetReg(0x0C44, Reg(0x0C44) | (1u << id));          // PA_SC_VIZ_QUERY_STATUS_0
                else SetReg(0x0C45, Reg(0x0C45) | (1u << (id - 32)));           // PA_SC_VIZ_QUERY_STATUS_1
            }
            break;
        }

        case 0x46:   // EVENT_WRITE (eventos do cache/pipeline: nada a fazer aqui)
        case 0x3B:   // INVALIDATE_STATE
        case 0x48:   // ME_INIT
            break;

        case 0x54:   // INTERRUPT
        {
            uint32_t mask = D(0);
            for (uint32_t cpu = 0; cpu < 6; cpu++)
                if (mask & (1u << cpu))
                    DispatchInterrupt(*g_cpCtx, 1, cpu);
            break;
        }

        case 0x2D:   // SET_CONSTANT
        {
            uint32_t offsetType = D(0);
            uint32_t index = offsetType & 0x7FF;
            uint32_t type = (offsetType >> 16) & 0xFF;
            uint32_t baseIndex = type == 0 ? 0x4000 : type == 1 ? 0x4800 : type == 2 ? 0x4900 : type == 3 ? 0x4908 : 0x2000;
            for (uint32_t i = 1; i < count; i++)
                WriteRegister(baseIndex + index + i - 1, D(i));
            // FCRECOMP_NANLOG=1: anota constantes de shader que chegam como NaN (endereco no buffer de comandos)
            if (static const bool nanLog = getenv("FCRECOMP_NANLOG") != nullptr; nanLog && type == 0)
                for (uint32_t i = 1; i < count; i++)
                    if ((D(i) & 0x7F800000u) == 0x7F800000u && (D(i) & 0x7FFFFFu))
                    {
                        static int n = 0;
                        if (n++ < 20)
                            Log("[nan] quadro %llu: constante %s c%u.%c = %08X (buffer %08X)", (unsigned long long)g_swapCount.load(),
                                (index + i - 1) >= 1024 ? "PS" : "VS", ((index + i - 1) & 1023) / 4, "xyzw"[(index + i - 1) & 3], D(i), addr + i * 4);
                        break;
                    }
            break;
        }

        case 0x55:   // SET_CONSTANT2
        {
            uint32_t index = D(0) & 0xFFFF;
            for (uint32_t i = 1; i < count; i++)
                WriteRegister(index + i - 1, D(i));
            break;
        }

        case 0x27:   // IM_LOAD: microcode de shader na memoria
        {
            uint32_t addrType = D(0), startSize = D(1);
            LoadShader(addrType & 3, PhysToGuest(addrType & ~3u), startSize & 0xFFFF);
            break;
        }

        case 0x2B:   // IM_LOAD_IMMEDIATE: microcode inline no pacote
        {
            uint32_t type = D(0), startSize = D(1);
            LoadShader(type & 3, addr + 8, startSize & 0xFFFF);
            break;
        }

        case 0x22:   // DRAW_INDX
        case 0x36:   // DRAW_INDX_2
        {
            g_drawCount++;
            OnDraw();

            if (g_drawReady)
            {
                // VGT_DRAW_INITIATOR: tipo (bits 0-5), fonte dos indices (6-7: 0 memoria, 1 inline, 2 auto),
                // indices de 32 bits (bit 11), quantidade (16-31)
                uint32_t init = opcode == 0x22 ? D(1) : D(0);
                xdraw::DrawInfo di;
                di.primType = init & 0x3F;
                di.count = init >> 16;
                di.index32 = (init >> 11) & 1;
                uint32_t source = (init >> 6) & 3;
                if (opcode == 0x22 && source == 0)
                {
                    di.indexBase = D(2) & 0x1FFFFFFF;
                    di.indexEndian = D(3) >> 30;
                }
                else if (source == 1)
                {
                    // indices dentro do proprio pacote (o ring buffer esta na memoria fisica)
                    di.indexBase = (addr + (opcode == 0x22 ? 8 : 4)) & 0x1FFFFFFF;
                    di.indexEndian = 2;
                }
                if (g_currentShader[0]) di.vs = { g_currentShader[0]->hash, &g_currentShader[0]->code };
                if (g_currentShader[1]) di.ps = { g_currentShader[1]->hash, &g_currentShader[1]->code };

                if ((Reg(0x2208) & 7) == 6) { perf::Scope ps(perf::Resolve); xdraw::Resolve(); }   // RB_MODECONTROL = copia
                else { perf::Scope ps(perf::Draw); xdraw::Draw(di); }
            }
            // FCRECOMP_DRAWLOG=N: descreve todos os desenhos do quadro N
            static const char* drawLog = getenv("FCRECOMP_DRAWLOG");
            if ((drawLog && g_swapCount == (uint64_t)atoi(drawLog)) || diag::Capturing(g_swapCount))
            {
                uint32_t init = opcode == 0x22 ? D(1) : D(0);
                TexInfo t0{};
                bool hasTex = ReadTexFetch(0, t0);
                Log("[draw] %s prim=%u fonte=%u idx32=%u n=%u modo=%u mascara=%X cor=%08X surf=%08X prof=%08X blend=%08X depth=%08X vte=%08X vs=%016llX ps=%016llX tex0=%s%08X %ux%u f%u copia=%08X/%08X",
                    opcode == 0x22 ? "DRAW_INDX" : "DRAW_INDX_2", init & 0x3F, (init >> 6) & 3, (init >> 11) & 1, init >> 16,
                    Reg(0x2208) & 7, Reg(0x2104), Reg(0x2001), Reg(0x2000), Reg(0x2002), Reg(0x2201), Reg(0x2200), Reg(0x2206),
                    g_currentShader[0] ? (unsigned long long)g_currentShader[0]->hash : 0ull,
                    g_currentShader[1] ? (unsigned long long)g_currentShader[1]->hash : 0ull,
                    hasTex ? "" : "-", t0.base, t0.width, t0.height, t0.format, Reg(0x2318), Reg(0x2319));
            }
            break;
        }

        case 0x50:   // SET_BIN_MASK (64 bits)
            g_binMask = ((uint64_t)D(1) << 32) | D(0);
            if (g_seqLog) Log("[seq] SET_BIN_MASK %08X %08X", D(0), D(1));
            break;

        case 0x60:   // SET_BIN_MASK_LO
            g_binMask = (g_binMask & 0xFFFFFFFF00000000ull) | D(0);
            break;
        case 0x61:   // SET_BIN_MASK_HI
            g_binMask = (g_binMask & 0xFFFFFFFFull) | ((uint64_t)D(0) << 32);
            break;
        case 0x62:   // SET_BIN_SELECT_LO
            g_binSelect = (g_binSelect & 0xFFFFFFFF00000000ull) | D(0);
            OnBinSelect();
            if (g_seqLog) Log("[seq] SET_BIN_SELECT_LO %08X", D(0));
            break;
        case 0x63:   // SET_BIN_SELECT_HI
            g_binSelect = (g_binSelect & 0xFFFFFFFFull) | ((uint64_t)D(0) << 32);
            OnBinSelect();
            break;

        case 0x51:   // SET_BIN_SELECT (64 bits)
            g_binSelect = ((uint64_t)D(1) << 32) | D(0);
            OnBinSelect();
            if (g_seqLog) Log("[seq] SET_BIN_SELECT %08X %08X", D(0), D(1));
            break;

        case 0x64:   // XE_SWAP (pacote nosso, escrito pelo VdSwap)
        {
            g_frontBuffer = D(1);
            g_frontWidth = D(2);
            g_frontHeight = D(3);
            g_frontFormat = D(4);
            {
                // planos do video desenhados neste quadro (vazio se nao ha video)
                std::lock_guard lock(g_videoMutex);
                g_videoPlanes = g_lastPlanes;
                g_lastPlanes.clear();
            }
            g_swapCount++;
            if (perf::AvLog() && g_swapCount <= 6) Log("[av] %.1f ms: troca %llu processada (desenhos ate aqui %llu)", perf::Now(), (unsigned long long)g_swapCount.load(), (unsigned long long)g_drawCount.load());
            diag::SetFrame(g_swapCount);
            // teste do vigia de travamento: FCRECOMP_TRAVATESTE=quadro para a GPU emulada por 12 s nesse quadro
            if (static const char* tt = getenv("FCRECOMP_TRAVATESTE"); tt && g_swapCount == strtoull(tt, nullptr, 10))
                Sleep(12000);
            AnswerPendingQueries(g_swapCount.load());
            g_semContagem.clear();   // item 40
            if (!g_queriesAdiadas.empty()) ResponderAdiadas(g_swapCount.load());
            {
                static const char* seq = getenv("FCRECOMP_SEQLOG");
                g_seqLog = seq && g_swapCount >= (uint64_t)atoi(seq) && g_swapCount < (uint64_t)atoi(seq) + 2;
            }
            MaybeSaveFrame(g_swapCount);
            {
                // FCRECOMP_EXITFRAME=N: fecha o jogo sozinho ao chegar no quadro N (testes curtos)
                // FCRECOMP_MISSAO="10,40,70:100": captura os quadros 10, 40 e 70 depois do inicio da missao (primeiro
                // quadro com mais de 800 desenhos) e fecha o jogo no quadro 100 depois dele. O tempo de carregamento
                // varia, entao numeros absolutos de quadro caem em lugares diferentes a cada execucao.
                static const char* missEnv = getenv("FCRECOMP_MISSAO");
                static uint64_t missStart = 0, lastDraws = 0;
                uint64_t draws = g_drawCount.load();
                // FCRECOMP_MISSAOMIN=N muda o limite de desenhos (800); FCRECOMP_MISSAOAPOSSAVE=1 so procura depois que um
                // jogo salvo e aberto (tools\carregar_bug.ps1: o menu nao conta e checkpoints debaixo d'agua tem menos desenhos)
                static const uint64_t missMin = getenv("FCRECOMP_MISSAOMIN") ? strtoull(getenv("FCRECOMP_MISSAOMIN"), nullptr, 10) : 800;
                static const bool afterSave = getenv("FCRECOMP_MISSAOAPOSSAVE") != nullptr;
                if (missEnv && !missStart && (!afterSave || g_saveOpened) && draws - lastDraws > missMin)
                {
                    missStart = g_swapCount;
                    ::g_missionStartFrame = missStart;
                    Log("[gpu] missao comecou no quadro %llu", (unsigned long long)missStart);
                    ::InputMarkMission();   // input.cpp: itens "F" do FCRECOMP_AUTOINPUT
                }
                lastDraws = draws;
                if (missEnv && missStart)
                {
                    uint64_t rel = g_swapCount - missStart;
                    for (const char* e = missEnv; *e && *e != ':'; e = strchr(e, ',') ? strchr(e, ',') + 1 : ":")
                        if (rel + 1 == strtoull(e, nullptr, 10)) diag::CaptureAt(g_swapCount + 1);
                    const char* ex = strchr(missEnv, ':');
                    if (ex && rel == strtoull(ex + 1, nullptr, 10))
                        if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"FCRecomp_Sair")) { SetEvent(ev); CloseHandle(ev); }
                }
                static const char* exitEnv = getenv("FCRECOMP_EXITFRAME");
                if (exitEnv && g_swapCount == strtoull(exitEnv, nullptr, 10))
                    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"FCRecomp_Sair")) { SetEvent(ev); CloseHandle(ev); }
            }
            {
                // FCRECOMP_QUADROLOG=1: build/quadros.csv com cada troca de tela (hora real em ms e quadros de som ja
                // entregues pelo jogo). Mostra se a imagem anda no mesmo ritmo do som (quedas abaixo de 30 qps).
                static FILE* qlog = getenv("FCRECOMP_QUADROLOG") ? fopen("quadros.csv", "w") : nullptr;
                if (qlog)
                {
                    fprintf(qlog, "%llu;%.1f;%llu;%llu\n", (unsigned long long)g_swapCount.load(), perf::Now(),
                        (unsigned long long)AudioFramesSubmitted(), (unsigned long long)g_drawCount.load());
                    if ((g_swapCount & 31) == 0) fflush(qlog);
                }
            }
            // FCRECOMP_THREADSEM=40,42.5,...: escreve no log o que cada thread esta esperando nesses segundos de jogo
            // (o mesmo texto do threads.txt do F12; serve para ver o que segura um carregamento)
            if (static const char* thr = getenv("FCRECOMP_THREADSEM"); thr)
            {
                static const double start = perf::Now();
                static size_t next = 0;
                static std::vector<double> marks = [] {
                    std::vector<double> v;
                    for (const char* p = getenv("FCRECOMP_THREADSEM"); *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : "") v.push_back(atof(p) * 1000.0);
                    return v;
                }();
                if (next < marks.size() && perf::Now() - start >= marks[next])
                {
                    Log("[threads] %.1f s:\n%s", marks[next] / 1000.0, DescribeThreads().c_str());
                    next++;
                }
            }
            diag::MaybeDumpMemory(g_swapCount);
            WatchUpdate();   // FCRECOMP_WATCH (diagnostico)
            // FCRECOMP_TRAVADA=ms: anota cada quadro que passou desse tempo, com o que ele gastou (as medias do
            // FCRECOMP_PERF escondem os picos). Os contadores sao os mesmos do FCRECOMP_PERF (precisa dele ligado).
            static const char* hitchEnv = getenv("FCRECOMP_TRAVADA");
            if (hitchEnv)
            {
                using namespace perf;
                static double lastFrame = Now(), prevMs[Count] = {}, prevPso = 0, prevPsoMs = 0, prevTex = 0, prevUp = 0, prevCreate = 0, prevPrep = 0, prevRt = 0;
                double now = Now(), dt = now - lastFrame;
                // o FCRECOMP_PERF zera os contadores a cada janela: se o valor caiu, a conta recomeca do zero
                auto dlt = [](double cur, double prev) { return cur >= prev ? cur - prev : cur; };
                if (dt > atof(hitchEnv))
                    Log("[travada] quadro %llu: %.0f ms | GPU %.0f ms | espera-reg %.0f desenho %.0f (textura %.0f shader %.0f) resolve %.0f apresentar %.0f cerca %.0f | "
                        "PSOs novos %.0f (%.0f ms) texturas novas %.0f, %.1f MB enviados (criar %.0f ms, preparar %.0f ms) | superficies/resolves criados %.0f ms",
                        (unsigned long long)g_swapCount.load(), dt, render::LastGpuFrameMs(), dlt(g_ms[WaitReg], prevMs[WaitReg]), dlt(g_ms[Draw], prevMs[Draw]),
                        dlt(g_ms[Texture], prevMs[Texture]), dlt(g_ms[perf::Shader], prevMs[perf::Shader]), dlt(g_ms[Resolve], prevMs[Resolve]),
                        dlt(g_ms[Present], prevMs[Present]), dlt(g_ms[Fence], prevMs[Fence]), dlt(g_psoNew, prevPso), dlt(g_psoMs, prevPsoMs),
                        dlt(g_texNew, prevTex), dlt(g_uploadMB, prevUp), dlt(g_texCreateMs, prevCreate), dlt(g_texPrepMs, prevPrep), dlt(g_rtCreateMs, prevRt));
                lastFrame = now;
                for (int i = 0; i < Count; i++) prevMs[i] = g_ms[i];
                prevPso = g_psoNew; prevPsoMs = g_psoMs; prevTex = g_texNew; prevUp = g_uploadMB; prevCreate = g_texCreateMs; prevPrep = g_texPrepMs; prevRt = g_rtCreateMs;
            }
            // FCRECOMP_CAPTURAGPU=ms: captura (como o F9) o proximo quadro quando a GPU passar desse tempo; uma vez so
            // (picos que acontecem em quadros diferentes a cada execucao, como a explosao do barco no treino)
            if (static const char* capGpu = getenv("FCRECOMP_CAPTURAGPU"); capGpu)
            {
                static bool feito = false;
                if (!feito && render::LastGpuFrameMs() > atof(capGpu)) { feito = true; diag::RequestCapture(); }
            }
            // FCRECOMP_EXPERIMENTO=N: alterna entre N configuracoes (xdraw::g_expMode) a cada 300 quadros depois do inicio
            // da fase e anota o tempo medio de GPU de cada janela (compara custos numa execucao so)
            static const char* expEnv = getenv("FCRECOMP_EXPERIMENTO");
            if (expEnv)
            {
                static double sum = 0; static int n = 0;
                uint64_t start = ::g_missionStartFrame.load();
                if (start && g_swapCount > start + 150)
                {
                    uint64_t k = (g_swapCount - start - 150) / 300;
                    int mode = (int)(k % std::max(1, atoi(expEnv)));
                    uint64_t pos = (g_swapCount - start - 150) % 300;
                    if (pos >= 60) { sum += render::LastGpuFrameMs(); n++; }   // descarta o comeco (troca de modo)
                    if (pos == 299 && n) { Log("[exp] modo %d: GPU real %.2f ms por quadro (%d quadros)", mode, sum / n, n); sum = 0; n = 0; }
                    if (pos == 0) xdraw::g_expMode = mode;
                }
            }
            static const bool perfLog = getenv("FCRECOMP_PERF") != nullptr;
            if (perfLog)
            {
                static double last = perf::Now();
                static int frames = 0;
                static const int perfWindow = getenv("FCRECOMP_PERFJANELA") ? atoi(getenv("FCRECOMP_PERFJANELA")) : 120;   // quadros por medicao
                static double gpuSum = 0;
                gpuSum += render::LastGpuFrameMs();   // tempo real de GPU de cada quadro (timestamps, sem parar nada)
                if (++frames == perfWindow)
                {
                    double now = perf::Now(), total = now - last;
                    using namespace perf;
                    Log("[perf] %.1f qps | por quadro (ms): total %.1f ocioso %.1f espera-reg %.1f desenho %.1f (textura %.1f copia %.1f shader %.1f) resolve %.1f apresentar %.1f cerca %.1f",
                        frames * 1000.0 / total, total / frames, g_ms[Idle] / frames, g_ms[WaitReg] / frames, g_ms[Draw] / frames,
                        g_ms[Texture] / frames, g_ms[Copy] / frames, g_ms[perf::Shader] / frames, g_ms[Resolve] / frames, g_ms[Present] / frames, g_ms[Fence] / frames);
                    Log("[perf]   GPU real: %.1f ms por quadro", gpuSum / frames);
                    gpuSum = 0;
                    double vmUse, vmBudget, vmSys;
                    render::VideoMemory(vmUse, vmBudget, vmSys);
                    Log("[perf]   memoria de video: %.0f MB em uso de %.0f MB permitidos; %.0f MB na memoria do sistema", vmUse, vmBudget, vmSys);
                    Log("[perf]   texturas por quadro: %.1f MB conferidos (%.0f conferencias, %.1f ms), %.1f MB enviados (%.1f ms)",
                        g_hashMB / frames, g_hashCalls / frames, g_ms[TexHash] / frames, g_uploadMB / frames, g_ms[TexUpload] / frames);
                    Log("[perf]   pipelines novos: %.1f por quadro (%.1f ms)", g_psoNew / frames, g_psoMs / frames);
                    g_hashMB = g_uploadMB = g_hashCalls = 0;
                    g_psoNew = g_psoMs = g_texNew = g_texCreateMs = g_texPrepMs = g_rtCreateMs = 0;
                    for (auto& m : g_ms) m = 0;
                    frames = 0; last = now;
                }
            }
            double tPresent = perf::Now();
            {
                std::lock_guard lock(g_videoMutex);
                xdraw::SetVideoFrame(!g_videoPlanes.empty());   // videos ficam fora da mascara do escuro
            }
            if (g_drawReady && xdraw::PresentFrame(D(1)))
            {
                // quadro desenhado pela GPU emulada
            }
            else if (g_renderReady)
            {
                static std::vector<uint32_t> pixels;
                uint32_t w, h;
                if (ReadFrontBuffer(pixels, w, h))
                {
                    render::PresentPixels(pixels.data(), w, h);
                    if (!g_fotoDesteQuadro.empty()) SalvarBmp(g_fotoDesteQuadro, pixels.data(), w, h);   // video
                }
            }
            xdraw::PedirFoto(std::string());
            g_fotoDesteQuadro.clear();
            perf::g_ms[perf::Present] += perf::Now() - tPresent;
            break;
        }

        default:
            // ME_INIT, NOP, WAIT_FOR_IDLE, INVALIDATE_STATE, IM_LOAD, LOAD_ALU_CONSTANT, EVENT_WRITE...: ignorados por enquanto
            {
                char key[48];
                snprintf(key, sizeof(key), "pacote PM4 tipo 3 opcode 0x%02X", opcode);
                diag::Note(diag::GpuPacket, key);
            }
            break;
        }
    }

    void ExecuteBuffer(uint32_t ptr, uint32_t count)
    {
        uint32_t end = ptr + count * 4;
        while (ptr < end)
        {
            uint32_t header = R32(ptr);
            ptr += 4;
            switch (header >> 30)
            {
            case 0:   // escrita em registradores
            {
                uint32_t n = ((header >> 16) & 0x3FFF) + 1;
                uint32_t reg = header & 0x7FFF;
                bool single = header & 0x8000;
                for (uint32_t i = 0; i < n; i++)
                {
                    g_regSrc = ptr + i * 4;
                    WriteRegister(single ? reg : reg + i, R32(ptr + i * 4));
                }
                g_regSrc = 0;
                ptr += n * 4;
                break;
            }
            case 1:   // dois registradores
                WriteRegister(header & 0x7FF, R32(ptr));
                WriteRegister((header >> 11) & 0x7FF, R32(ptr + 4));
                ptr += 8;
                break;
            case 2:   // nop
                break;
            case 3:
            {
                uint32_t n = ((header >> 16) & 0x3FFF) + 1;
                uint32_t opcode = (header >> 8) & 0x7F;
                // pacote predicado (bit 0): so executa se a mascara de "bins" do tiling permitir
                static const char* predLog = getenv("FCRECOMP_PREDLOG");   // quadro: pacotes que so rodam em algumas faixas
                if ((header & 1) && predLog && g_swapCount >= strtoull(predLog, nullptr, 10) && g_swapCount <= strtoull(strchr(predLog, '-') ? strchr(predLog, '-') + 1 : predLog, nullptr, 10))
                    Log("[pred] q%llu pacote %02X n=%u mascara %016llX selecao %016llX -> %s d0=%08X", (unsigned long long)g_swapCount.load(), opcode, n,
                        (unsigned long long)g_binMask, (unsigned long long)g_binSelect, (g_binMask & g_binSelect) ? "executa" : "pula",
                        R32(ptr));
                bool exec = !(header & 1) || (g_binMask & g_binSelect) != 0;
                // desenhos (e copias/resolves, que tambem sao desenhos) com as faixas juntas: veja g_tileState
                if (MergeTiles() && (opcode == 0x22 || opcode == 0x36) && g_tileState != 0 && g_tileState != 3)
                {
                    bool copy = (Reg(0x2208) & 7) == 6;   // RB_MODECONTROL = copia (resolve)
                    if (g_tileState == 2) exec = false;
                    else if ((header & 1) && !copy) exec = (g_binMask & (g_tileUnion | g_tileUnionLast)) != 0;
                }
                if (!exec && g_fenceLogAddr != 0xFFFFFFFF && (opcode == 0x58 || opcode == 0x3D || opcode == 0x3C || opcode == 0x3F))
                    Log("[cerca] pacote %02X PULADO pela predicacao (d0=%08X d1=%08X d2=%08X, mascara %016llX selecao %016llX faixa %d)", opcode,
                        R32(ptr), R32(ptr + 4), R32(ptr + 8), (unsigned long long)g_binMask, (unsigned long long)g_binSelect, g_tileState);
                if (exec)
                {
                    double tPkt = perf::Now();
                    ExecuteType3(opcode, ptr, n);
                    // depuracao (FCRECOMP_FENCELOG): comando que segurou o processador por muito tempo
                    if (g_fenceLogAddr != 0xFFFFFFFF && perf::Now() - tPkt > 300)
                        Log("[cerca] pacote %02X levou %.0f ms (quadro %llu, desenhos %llu)", opcode, perf::Now() - tPkt,
                            (unsigned long long)g_swapCount.load(), (unsigned long long)g_drawCount.load());
                }
                else if (g_seqLog)
                    Log("[seq] pacote %02X pulado pela predicacao (mascara %016llX selecao %016llX)", opcode,
                        (unsigned long long)g_binMask, (unsigned long long)g_binSelect);
                ptr += n * 4;
                break;
            }
            }
        }
    }

    void CommandProcessorThread()
    {
        // (o ring pode ser recriado a qualquer momento: GpuRingInit zera as posicoes de leitura/escrita)
        g_cpCtx = AttachGuestThread("gpu-cp");
        uint32_t rptr = 0;
        uint64_t lastLog = 0;
        while (!g_ringBufferBase)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        uint32_t ringBase = 0, ringGuest = 0, ringDwords = 0;
        int idleSpin = 0;

        for (;;)
        {
            // o jogo pode recriar o ring buffer
            if (g_ringBufferBase != ringBase)
            {
                ringBase = g_ringBufferBase;
                ringGuest = PhysToGuest(ringBase);
                ringDwords = (1u << (g_ringBufferSizeLog2 + 3)) / 4;
                rptr = 0;
                Log("[gpu] ring buffer: %08X, %u dwords", ringGuest, ringDwords);
            }

            uint32_t wptr = Reg(REG_CP_RB_WPTR) % ringDwords;
            if (g_ringBufferBase != ringBase) continue;   // recriado entre as duas leituras: wptr ja e do ring novo
            static ULONGLONG idleSince = 0;
            if (wptr == rptr)
            {
                perf::Scope ps(perf::Idle);
                if (!idleSince) idleSince = GetTickCount64();
                if (!g_pendingQueries.empty()) AnswerPendingQueries(g_swapCount.load());
                if (!g_queriesAdiadas.empty()) ResponderAdiadas(g_swapCount.load());
                CpWait(idleSpin++);
                continue;
            }
            // depuracao (FCRECOMP_FENCELOG): o jogo ficou mais de 1 s sem mandar comandos novos para a GPU
            if (idleSince && g_fenceLogAddr != 0xFFFFFFFF && GetTickCount64() - idleSince > 1000)
                Log("[cerca] processador de comandos ficou %.1f s sem comandos novos (rptr=%u, novo wptr=%u, quadro %llu)",
                    (GetTickCount64() - idleSince) / 1000.0, rptr, wptr, (unsigned long long)g_swapCount.load());
            idleSince = 0;
            idleSpin = 0;

            static uint32_t maxWptr = 0;
            if (wptr > maxWptr) { maxWptr = wptr; if ((maxWptr & 0xFFF) == 0 || maxWptr > 30000) {} }
            if (wptr < rptr)
            {
                static int wraps = 0;
                if (wraps++ < 5) Log("[gpu] ring deu a volta: rptr=%u wptr=%u maior wptr visto=%u", rptr, wptr, maxWptr);
            }

            uint32_t newRptr = wptr;
            if (wptr > rptr)
                ExecuteBuffer(ringGuest + rptr * 4, wptr - rptr);
            else if (ringDwords - rptr > ringDwords / 4)
            {
                // o jogo recomecou o ring do inicio sem chegar perto do fim (acontece na abertura: rptr=31 wptr=25):
                // o resto do ring e lixo/zeros, so executa o que foi escrito no comeco
                static int resets = 0;
                if (resets++ < 5) Log("[gpu] ring recomecou do inicio (rptr=%u wptr=%u)", rptr, wptr);
                ExecuteBuffer(ringGuest, wptr);
            }
            else
            {
                // O ring e circular: um pacote pode comecar no fim e continuar no comeco. Executar as duas partes
                // separadas cortava esse pacote ao meio e o resto virava lixo (comandos velhos, "fence voltou" e por fim
                // um WAIT_REG_MEM falso que nunca termina: tela congelada com o audio tocando, a cada volta do ring
                // ~5 min de jogo). Junta as duas partes numa copia continua e executa de uma vez.
                static uint32_t joined = 0, joinedDwords = 0;
                if (joinedDwords < ringDwords) { joined = GuestAlloc(ringDwords * 4, 0x1000); joinedDwords = ringDwords; }
                uint32_t tail = ringDwords - rptr;
                memcpy(G(joined), G(ringGuest + rptr * 4), tail * 4);
                memcpy(G(joined + tail * 4), G(ringGuest), wptr * 4);
                // so executa pacotes completos: se o ultimo ainda nao foi todo escrito, ele fica para a proxima volta
                uint32_t total = tail + wptr, p = 0, complete = 0;
                bool crosses = false;
                while (p < total)
                {
                    uint32_t h = R32(joined + p * 4);
                    uint32_t t = h >> 30;
                    uint32_t next = p + 1 + (t == 0 || t == 3 ? ((h >> 16) & 0x3FFF) + 1 : t == 1 ? 2 : 0);
                    if (next > total) break;
                    if (p < tail && next > tail) crosses = true;
                    complete = p = next;
                }
                static int logged = 0;
                if (logged++ < 10)
                    Log("[gpu] volta do ring: %u dwords no fim + %u no comeco%s%s", tail, wptr,
                        crosses ? "; um pacote ATRAVESSA a emenda (antes era cortado)" : "",
                        complete < total ? "; ultimo pacote incompleto fica para depois" : "");
                ExecuteBuffer(joined, complete);
                newRptr = (rptr + complete) % ringDwords;
            }
            rptr = newRptr;

            SetReg(REG_CP_RB_RPTR, rptr);
            if (g_ringRptrWriteBack)
                W32(PhysToGuest(g_ringRptrWriteBack), rptr);

            if (g_swapCount != lastLog && (g_swapCount <= 5 || (g_swapCount % 60) == 1))
            {
                lastLog = g_swapCount;
                Log("[gpu] quadro %llu, %llu desenhos ate agora", (unsigned long long)g_swapCount.load(), (unsigned long long)g_drawCount.load());
            }
        }
    }

    // ------------------------------------------------------------ janela

    // Endereco de um pixel numa textura "tiled" do Xbox 360 (blocos de 32x32 da GPU)
    uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2bpp)
    {
        uint32_t macro = ((y >> 5) * (pitch >> 5)) << (log2bpp + 7);
        uint32_t micro = ((y & 6) << 2) << log2bpp;
        uint32_t rowOffset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2bpp)) + ((y & 1) << 4);

        macro = (x >> 5) << (log2bpp + 7);
        micro = (x & 7) << log2bpp;
        uint32_t offset = rowOffset + macro + ((micro & ~0xFu) << 1) + (micro & 0xF);
        return ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
    }

    // Quadro de video Bink: converte os planos Y (cheio) e Cr/Cb (metade) de YUV para BGRA
    bool ReadVideoFrame(std::vector<uint32_t>& pixels, uint32_t& w, uint32_t& h)
    {
        std::vector<TexInfo> planes;
        {
            std::lock_guard lock(g_videoMutex);
            planes = g_videoPlanes;
        }
        if (planes.size() < 3) return false;
        const TexInfo& Y = planes[0];
        const TexInfo& Cr = planes[1];
        const TexInfo& Cb = planes[2];
        if (Cr.width * 2 != Y.width) return false;

        w = Y.width;
        h = Y.height;
        pixels.resize(w * h);
        const uint8_t* py = G(PhysToGuest(Y.base));
        const uint8_t* pr = G(PhysToGuest(Cr.base));
        const uint8_t* pb = G(PhysToGuest(Cb.base));
        auto clamp = [](int v) { return v < 0 ? 0 : v > 255 ? 255 : v; };
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
            {
                int Yv = py[y * Y.pitch + x];
                int Rv = pr[(y / 2) * Cr.pitch + x / 2] - 128;
                int Bv = pb[(y / 2) * Cb.pitch + x / 2] - 128;
                int r = clamp(Yv + (1402 * Rv) / 1000);
                int g = clamp(Yv - (344 * Bv + 714 * Rv) / 1000);
                int b = clamp(Yv + (1772 * Bv) / 1000);
                pixels[y * w + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        return true;
    }

    // Le o framebuffer (8_8_8_8 tiled) para pixels lineares BGRA
    bool ReadFrontBuffer(std::vector<uint32_t>& pixels, uint32_t& w, uint32_t& h)
    {
        // enquanto a renderizacao nao existe, um video Bink e mostrado direto dos planos decodificados
        if (ReadVideoFrame(pixels, w, h))
            return true;

        uint32_t fb = g_frontBuffer;
        w = g_frontWidth;
        h = g_frontHeight;
        if (!fb || !w || !h || w > 4096 || h > 4096)
            return false;
        pixels.resize(w * h);
        uint32_t pitch = (w + 31) & ~31u;
        uint32_t base = PhysToGuest(fb);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                pixels[y * w + x] = R32(base + TiledOffset(x, y, pitch, 2));
        return true;
    }

    // FCRECOMP_SHOT=N: salva o quadro N (e a cada N quadros) em build/quadro_XXXX.bmp
    // fotos pedidas pelo roteiro gravado (input.cpp, FCRECOMP_ROTEIROFOTOS): troca -> arquivo
    std::mutex g_fotosMutex;
    std::vector<std::pair<uint64_t, std::string>> g_fotosPedidas;

    void MaybeSaveFrame(uint64_t frame)
    {
        std::string name;
        {
            std::lock_guard lock(g_fotosMutex);
            for (size_t i = 0; i < g_fotosPedidas.size(); i++)
                if (g_fotosPedidas[i].first == frame) { name = g_fotosPedidas[i].second; g_fotosPedidas.erase(g_fotosPedidas.begin() + i); break; }
        }
        if (!name.empty())   // a imagem da placa sai no PresentFrame; se o quadro nao for desenhado pela placa, abaixo
        {
            xdraw::PedirFoto(name);
            g_fotoDesteQuadro = name;
            return;
        }
        static const char* env = getenv("FCRECOMP_SHOT");
        if (!env) return;
        uint64_t every = std::max(1, atoi(env));
        if (frame % every) return;
        char b[64];
        snprintf(b, sizeof(b), "quadro_%05llu.bmp", (unsigned long long)frame);
        name = b;

        std::vector<uint32_t> px;
        uint32_t w, h;
        if (!ReadFrontBuffer(px, w, h)) return;
        SalvarBmp(name, px.data(), w, h);
    }

    void SalvarBmp(const std::string& nome, const uint32_t* px, uint32_t w, uint32_t h)
    {
        FILE* f = fopen(nome.c_str(), "wb");
        if (!f) return;
        uint32_t dataSize = w * h * 4;
        BITMAPFILEHEADER fh{ 0x4D42, (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + dataSize), 0, 0,
            sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) };
        BITMAPINFOHEADER ih{ sizeof(BITMAPINFOHEADER), (LONG)w, -(LONG)h, 1, 32 };
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
        fwrite(px, 4, (size_t)w * h, f);
        fclose(f);
        Log("[gpu] imagem salva em %s", nome.c_str());
    }

    // Tela cheia sem bordas (opcao do launcher; Alt+Enter troca): a janela vira um retangulo sem moldura do tamanho do
    // monitor; a imagem (resolucao interna) e ampliada pela troca de quadros (DXGI_SCALING_STRETCH)
    WINDOWPLACEMENT g_antesTelaCheia{ sizeof(WINDOWPLACEMENT) };   // lugar da janela antes da tela cheia
    std::atomic<bool> g_telaCheiaLigada{ false };
    void TelaCheia(HWND hwnd, bool ligar)
    {
        auto& antes = g_antesTelaCheia;
        if (ligar == g_telaCheiaLigada) return;
        g_telaCheiaLigada = ligar;
        if (ligar)
        {
            GetWindowPlacement(hwnd, &antes);
            if (antes.showCmd == SW_HIDE) antes.showCmd = SW_SHOWNORMAL;   // janela criada escondida (troca de fase)
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        else
        {
            SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowPlacement(hwnd, &antes);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        Log("[janela] %s", ligar ? "tela cheia" : "janela");
    }

    // Tela de preparacao (antes do 1o quadro do jogo): texto e barra de progresso desenhados pela propria janela (GDI),
    // sem usar a placa, que esta ocupada com o aquecimento. Depois do 1o quadro a imagem do DirectX cobre a janela.
    // Troca de programa (jogo <-> editor, troca de fase, item 30): tela de carregamento propria. Substituiu a "mascara
    // do escuro" (foto do ultimo quadro claro segurada; misturada com a tela nova dava "LOADINGANDO"). A janela antiga e
    // coberta por ela (camada por cima ate o processo fechar), a janela nova nasce com ela (GDI ate o 1o quadro) e a
    // placa a segura enquanto o jogo novo esta escuro (xenos_draw.cpp, FotoReinicioPixels). A barra corre pelo relogio
    // do Windows, entao as duas janelas mostram a mesma posicao e a troca nao aparece.
    const bool g_reinicio = getenv("PREDATOR_REINICIO") != nullptr;

    void DesenharTelaCarregando(HDC dc, int w, int h)
    {
        RECT r{ 0, 0, w, h };
        HBRUSH fundo = CreateSolidBrush(RGB(12, 12, 12));
        FillRect(dc, &r, fundo);
        DeleteObject(fundo);
        int esc = std::max(1, h / 360);   // tamanho das letras acompanha a janela
        int bw = w * 6 / 10, bh = std::max(8, h / 50), bx = (w - bw) / 2, by = h / 2;
        HFONT titulo = CreateFontW(-16 * esc, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ antes = SelectObject(dc, titulo);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(235, 235, 235));
        RECT tr{ 0, by - h / 6, w, by - bh * 2 };
        DrawTextW(dc, TR(L"Carregando", L"Loading"), -1, &tr, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
        SelectObject(dc, antes);
        DeleteObject(titulo);
        HBRUSH borda = CreateSolidBrush(RGB(70, 70, 70)), cheio = CreateSolidBrush(RGB(200, 120, 30));
        RECT b{ bx - 2, by - 2, bx + bw + 2, by + bh + 2 };
        FrameRect(dc, &b, borda);
        // um trecho de 1/4 da barra correndo da esquerda para a direita (1,4 s por volta)
        double f = (double)(GetTickCount64() % 1400) / 1400.0;
        int seg = bw / 4, x0 = bx - seg + (int)((bw + seg) * f);
        RECT s{ std::max(bx, x0), by, std::min(bx + bw, x0 + seg), by + bh };
        if (s.right > s.left) FillRect(dc, &s, cheio);
        DeleteObject(borda); DeleteObject(cheio);
    }

    // pinta a tela de carregamento fora da tela e copia de uma vez (sem piscar)
    void PintarTelaCarregando(HWND hwnd, HDC dc)
    {
        RECT c; GetClientRect(hwnd, &c);
        int w = std::max<int>(c.right, 1), h = std::max<int>(c.bottom, 1);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
        HGDIOBJ antes = SelectObject(mem, bmp);
        DesenharTelaCarregando(mem, w, h);
        BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, antes);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    // janela que so aparece depois de pintada: escondida pelo DWM (cloak) enquanto aparece e se pinta. Antes, entre
    // aparecer e pintar, podia sair 1 quadro preto (ou branco) no meio da troca.
    void Encobrir(HWND h, bool encobrir)
    {
        BOOL v = encobrir ? TRUE : FALSE;
        DwmSetWindowAttribute(h, DWMWA_CLOAK, &v, sizeof(v));
    }

    // Camada por cima da janela antiga com a tela de carregamento. Sem moldura, sem foco e fora da barra de tarefas;
    // some quando o processo fecha.
    constexpr UINT WM_COBRIR = WM_APP + 20;
    LRESULT CALLBACK ProcCobertura(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_PAINT)
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            PintarTelaCarregando(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        if (msg == WM_ERASEBKGND) return 1;
        if (msg == WM_TIMER) { InvalidateRect(h, nullptr, FALSE); return 0; }
        if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        return DefWindowProcW(h, msg, wp, lp);
    }

    void Cobrir(HWND janela)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = ProcCobertura;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PredatorCobertura";
        RegisterClassW(&wc);
        RECT c; GetClientRect(janela, &c);
        POINT p{ 0, 0 }; ClientToScreen(janela, &p);
        HWND h = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"PredatorCobertura", L"", WS_POPUP, p.x, p.y, c.right, c.bottom,
                                 janela, nullptr, wc.hInstance, nullptr);
        if (!h) return;
        BOOL sem = TRUE;
        DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &sem, sizeof(sem));
        Encobrir(h, true);
        ShowWindow(h, SW_SHOWNOACTIVATE);
        UpdateWindow(h);
        DwmFlush();
        Encobrir(h, false);
        SetTimer(h, 1, 50, nullptr);
    }

    void PaintPreparation(HWND hwnd)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_reinicio)   // troca de programa: a mesma tela de carregamento da janela antiga
        {
            PintarTelaCarregando(hwnd, dc);
            EndPaint(hwnd, &ps);
            return;
        }
        RECT r; GetClientRect(hwnd, &r);
        int w = r.right, h = r.bottom;
        HDC mem = CreateCompatibleDC(dc);   // desenha fora da tela e copia de uma vez (sem piscar)
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(w, 1), std::max(h, 1));
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        HBRUSH fundo = CreateSolidBrush(RGB(12, 12, 12));
        FillRect(mem, &r, fundo);
        DeleteObject(fundo);

        int esc = std::max(1, h / 360);   // tamanho das letras acompanha a janela
        HFONT titulo = CreateFontW(-16 * esc, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT texto = CreateFontW(-9 * esc, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SetBkMode(mem, TRANSPARENT);
        float p = g_prepDone ? 1.0f : std::clamp(xdraw::WarmUpProgress(), 0.0f, 1.0f);
        int bw = w * 6 / 10, bh = std::max(8, h / 50), bx = (w - bw) / 2, by = h / 2;

        HGDIOBJ oldFont = SelectObject(mem, titulo);
        SetTextColor(mem, RGB(235, 235, 235));
        RECT t{ 0, by - h / 6, w, by - bh * 2 };
        DrawTextW(mem, TR(L"Preparando os gráficos", L"Preparing the graphics"), -1, &t, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);

        HBRUSH borda = CreateSolidBrush(RGB(70, 70, 70)), cheio = CreateSolidBrush(RGB(200, 120, 30));
        RECT b{ bx - 2, by - 2, bx + bw + 2, by + bh + 2 };
        FrameRect(mem, &b, borda);
        RECT f{ bx, by, bx + (int)(bw * p), by + bh };
        FillRect(mem, &f, cheio);
        DeleteObject(borda); DeleteObject(cheio);

        SelectObject(mem, texto);
        SetTextColor(mem, RGB(160, 160, 160));
        wchar_t linha[160];
        swprintf(linha, 160, L"%d%%", (int)(p * 100 + 0.5f));
        RECT pc{ 0, by + bh * 2, w, by + bh * 2 + 20 * esc };
        DrawTextW(mem, linha, -1, &pc, DT_CENTER | DT_TOP | DT_SINGLELINE);
        RECT d{ 0, by + bh * 2 + 24 * esc, w, h };
        DrawTextW(mem, TR(L"Só demora na primeira vez (ou depois de uma atualização). O jogo começa em seguida.",
                           L"This only takes long the first time (or after an update). The game starts right after."), -1, &d,
                  DT_CENTER | DT_TOP | DT_SINGLELINE);

        BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldFont);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp); DeleteDC(mem);
        DeleteObject(titulo); DeleteObject(texto);
        EndPaint(hwnd, &ps);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (LRESULT r = 0; InputWindowMessage(hwnd, msg, wp, lp, r)) return r;   // mouse, F2, +/- (input.cpp)
        switch (msg)
        {
        // tela de preparacao ate o 1o quadro do jogo (com 100% entre o fim da preparacao e o video de abertura)
        case WM_COBRIR: Cobrir(hwnd); return 0;
        case WM_PAINT:
            if (g_swapCount == 0) { PaintPreparation(hwnd); return 0; }
            break;
        case WM_ERASEBKGND:
            if (g_swapCount == 0) return 1;   // o WM_PAINT pinta tudo
            break;
        case WM_SYSKEYDOWN:
            if (wp == VK_RETURN && (lp & (1 << 29)))   // Alt+Enter
            {
                LONG estilo = GetWindowLongW(hwnd, GWL_STYLE);
                TelaCheia(hwnd, (estilo & WS_CAPTION) != 0);
                return 0;
            }
            break;
        case WM_SYSCHAR:
            if (wp == VK_RETURN) return 0;   // sem o "bip" do Alt+Enter
            break;
        case WM_TIMER:
        {
            if (wp == 2)   // tela de preparacao: redesenha a barra ate o 1o quadro do jogo
            {
                if (g_swapCount > 0) KillTimer(hwnd, 2);
                else InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // FPS = quadros apresentados (trocas de tela do jogo) por segundo, medido a cada ~1 s
            static uint64_t lastSwaps = 0;
            static ULONGLONG lastTick = GetTickCount64();
            static double fps = 0;
            ULONGLONG now = GetTickCount64();
            if (now - lastTick >= 1000)
            {
                uint64_t swaps = g_swapCount.load();
                fps = (swaps - lastSwaps) * 1000.0 / (now - lastTick);
                lastSwaps = swaps;
                lastTick = now;
            }
            // quem joga ve so o nome e o FPS (mais os avisos que somem sozinhos: F12, mouse, sensibilidade, gravacao);
            // no modo teste continuam os contadores (quadros, desenhos, endereco da tela) para conferir os prints. O tamanho
            // "fb" e o que o jogo usa (sempre 1280x720); "interna" e o que a placa desenha de verdade (opcao escala)
            char title[256];
            if (OpcoesAtuais().modoTeste)
            {
                uint32_t meios = xdraw::EscalaMeios();
                snprintf(title, sizeof(title), GUEST_TITULO " - FPS: %.0f  |  quadros: %llu  desenhos: %llu  fb: %08X %ux%u (interna %ux%u)",
                    fps, (unsigned long long)g_swapCount.load(), (unsigned long long)g_drawCount.load(),
                    g_frontBuffer.load(), g_frontWidth.load(), g_frontHeight.load(),
                    g_frontWidth.load() * meios / 2, g_frontHeight.load() * meios / 2);
            }
            else
                snprintf(title, sizeof(title), GUEST_TITULO " - FPS: %.0f", fps);
            std::string full = std::string(title) + diag::Status() + InputStatus() + BugStatus() + ProfilerStatus();
            if (std::string warm = xdraw::WarmUpStatus(); !warm.empty())
                full = GUEST_TITULO " - " + warm + "...";
            else if (!g_renderReady)
                full = GUEST_TITULO TR(" - iniciando a placa de video...", " - starting the graphics card...");
            SetWindowTextA(hwnd, full.c_str());
            return 0;
        }
        case WM_KEYDOWN:
            if (BugKey((unsigned)wp) || diag::OnKey((unsigned)wp)) return 0;
            break;
        case WM_DESTROY:
            Log("[gpu] janela fechada, encerrando");
            diag::WriteReport("janela fechada");
            RedeEncerrar();   // fecha as portas abertas no roteador (rede.cpp)
            // espera a placa de video terminar antes de sair (evita recursos presos no driver)
            if (g_drawReady) { g_drawReady = false; Sleep(100); xdraw::Shutdown(); }
            else if (g_renderReady) render::WaitIdle();
            ExitProcess(0);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void WindowThread()
    {
        ProfilerRegisterThread("janela");   // o vigia de travamento tambem le a pilha desta thread
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FCRecomp";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);   // preto ate o 1o quadro (antes: lixo/branco)
        RegisterClassW(&wc);
        RECT r{ 0, 0, 1280, 720 };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        // centralizada na area de trabalho; se a tela for pequena, a janela diminui (a imagem e esticada)
        RECT work;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int w = r.right - r.left, h = r.bottom - r.top;
        int maxW = work.right - work.left, maxH = work.bottom - work.top;
        if (w > maxW || h > maxH)
        {
            float s = std::min((float)maxW / w, (float)maxH / h) * 0.95f;
            r = { 0, 0, (LONG)(1280 * s), (LONG)(720 * s) };
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            w = r.right - r.left;
            h = r.bottom - r.top;
        }
        // tamanho escolhido no launcher (limitado a area de trabalho)
        if (int t = OpcoesAtuais().janela; t > 0 && t <= 4)
        {
            static const int TAM[5][2] = { { 0, 0 }, { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 } };
            RECT rr{ 0, 0, TAM[t][0], TAM[t][1] };
            AdjustWindowRect(&rr, WS_OVERLAPPEDWINDOW, FALSE);
            w = std::min<int>(rr.right - rr.left, maxW);
            h = std::min<int>(rr.bottom - rr.top, maxH);
        }
        else if (OpcoesAtuais().janela == 0)
        {
            // automatico: o maior 16:9 que cabe (90% da area de trabalho)
            float s = std::min((float)maxW / 1280, (float)maxH / 720) * 0.9f;
            s = std::max(s, 0.5f);
            RECT rr{ 0, 0, (LONG)(1280 * s), (LONG)(720 * s) };
            AdjustWindowRect(&rr, WS_OVERLAPPEDWINDOW, FALSE);
            w = std::min<int>(rr.right - rr.left, maxW);
            h = std::min<int>(rr.bottom - rr.top, maxH);
        }
        int x = work.left + std::max(0, (int)(work.right - work.left - w) / 2);
        int y = work.top + std::max(0, (int)(work.bottom - work.top - h) / 2);

        // Janela de log: no Windows 11 o console abre no Windows Terminal, e a janela do jogo (centralizada) a cobria.
        // Se ela estiver visivel, fica a esquerda e o jogo a direita (diminuindo o jogo se faltar espaco).
        // GetConsoleWindow devolve uma janela falsa no Terminal; a janela real e a dona dela. FCRECOMP_LOGLADO=0 desliga.
        HWND con = GetConsoleWindow();
        HWND conTop = con ? GetAncestor(con, GA_ROOTOWNER) : nullptr;
        const char* logLado = getenv("FCRECOMP_LOGLADO");
        if (conTop && IsWindowVisible(conTop) && !(logLado && logLado[0] == '0'))
        {
            const int minLog = 420;
            if (maxW - w < minLog)
            {
                float s = std::min((float)(maxW - minLog) / w, 1.0f);
                RECT rr{ 0, 0, (LONG)(1280 * s), (LONG)(720 * s) };
                AdjustWindowRect(&rr, WS_OVERLAPPEDWINDOW, FALSE);
                w = rr.right - rr.left;
                h = rr.bottom - rr.top;
            }
            x = work.right - w;
            y = work.top;
            ShowWindow(conTop, SW_RESTORE);
            SetWindowPos(conTop, nullptr, work.left, work.top, x - work.left, maxH, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        // troca de fase (xam.cpp): a janela nasce no lugar e tamanho da anterior, inclusive tela cheia
        // (PREDATOR_REINICIO = "esquerda,topo,direita,baixo,tela cheia,maximizada")
        WINDOWPLACEMENT lugar{ sizeof(lugar) };
        int cheia = OpcoesAtuais().telaCheia, maximizada = 0;
        const char* reinicio = getenv("PREDATOR_REINICIO");
        bool usarLugar = reinicio && sscanf(reinicio, "%ld,%ld,%ld,%ld,%d,%d", &lugar.rcNormalPosition.left, &lugar.rcNormalPosition.top,
                                            &lugar.rcNormalPosition.right, &lugar.rcNormalPosition.bottom, &cheia, &maximizada) == 6;
        g_window = CreateWindowW(L"FCRecomp", L"" GUEST_TITULO, WS_OVERLAPPEDWINDOW | (usarLugar ? 0 : WS_VISIBLE),
            x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
        if (usarLugar)
        {
            // troca de programa: sem a animacao de abrir janela do Windows e escondida ate pintar a tela de carregamento
            BOOL sem = TRUE;
            DwmSetWindowAttribute(g_window, DWMWA_TRANSITIONS_FORCEDISABLED, &sem, sizeof(sem));
            Encobrir(g_window, true);   // so aparece depois de pintada com a tela de carregamento (abaixo)
        }
        AplicarIcone(g_window);
        SetTimer(g_window, 1, 500, nullptr);
        SetTimer(g_window, 2, 50, nullptr);   // barra da tela de preparacao
        if (usarLugar)
        {
            lugar.showCmd = cheia ? SW_HIDE : maximizada ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
            SetWindowPlacement(g_window, &lugar);
        }
        if (cheia) TelaCheia(g_window, true);
        if (usarLugar)
        {
            UpdateWindow(g_window);
            DwmFlush();
            Encobrir(g_window, false);
            Log("[janela] troca de programa: tela de carregamento");
        }
        if (const char* ev = getenv("PREDATOR_EVENTO"))
        {
            // a janela ja esta na tela com a tela de preparacao: a janela anterior pode fechar
            SetForegroundWindow(g_window);
            UpdateWindow(g_window);
            if (HANDLE h = OpenEventA(EVENT_MODIFY_STATE, FALSE, ev)) { SetEvent(h); CloseHandle(h); }
            Log("[janela] troca de fase: janela no lugar da anterior%s", cheia ? " (tela cheia)" : "");
        }
        InputInit(g_window);
        // Preparacao da placa de video (DirectX 12 + aquecimento: pipelines e superficies recriados) numa thread propria:
        // leva segundos (muito mais quando os shaders precisam ser recompilados) e, feita na thread da janela, a janela
        // aparecia travada/branca e sem o titulo. O titulo mostra o progresso (xdraw::WarmUpStatus). "Pronto" so no fim:
        // ninguem apresenta quadros pela metade da preparacao.
        SendMessageW(g_window, WM_TIMER, 1, 0);
        std::thread([] {
            // a troca de quadros tem o tamanho da resolucao interna (a imagem e ampliada/reduzida para a janela)
            uint32_t meios = xdraw::EscalaMeios();
            bool render = render::Init(g_window, 1280 * meios / 2, 720 * meios / 2);
            bool draw = render && !getenv("FCRECOMP_NODRAW") && xdraw::Init();
            g_drawReady = draw;
            g_renderReady = render;
            g_prepDone = true;
            PostMessageW(g_window, WM_TIMER, 1, 0);
        }).detach();
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

HWND GetGameWindow() { return g_window; }

// Fechamento pelo console (X do console, Ctrl+C, logoff): libera a GPU antes de sair
static BOOL WINAPI ConsoleCtrl(DWORD)
{
    diag::WriteReport("saida");
    RedeEncerrar();
    if (g_drawReady) { g_drawReady = false; Sleep(100); xdraw::Shutdown(); }
    ExitProcess(0);
}

void XmaInit();   // xma.cpp

HWND GameWindow() { return g_window; }

// Troca de fase (xam.cpp): foto da area da janela (o que esta na tela agora) gravada em "arquivo" (BMP) para a janela
// nova nascer com a mesma imagem; e sem a animacao de fechar do Windows nesta janela. false = nao deu.
// Foto da area da janela do jogo como esta na tela, em BMP (pacote do F12; funciona mesmo com a placa de video travada)
bool SalvarImagemDaJanela(const std::wstring& arquivo)
{
    if (!g_window) return false;
    RECT c; GetClientRect(g_window, &c);
    int w = c.right, h = c.bottom;
    if (w <= 0 || h <= 0) return false;
    HDC tela = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(tela);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(tela, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ antes = SelectObject(mem, bmp);
    POINT p{ 0, 0 }; ClientToScreen(g_window, &p);
    bool ok = BitBlt(mem, 0, 0, w, h, tela, p.x, p.y, SRCCOPY);
    SelectObject(mem, antes);
    DeleteDC(mem);
    ReleaseDC(nullptr, tela);
    if (ok)
        if (FILE* f = _wfopen(arquivo.c_str(), L"wb"))
        {
            uint32_t tam = (uint32_t)w * h * 4;
            BITMAPFILEHEADER fh{ 0x4D42, (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + tam), 0, 0,
                                 sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) };
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
            fwrite(bits, 1, tam, f);
            fclose(f);
        }
        else ok = false;
    DeleteObject(bmp);
    return ok;
}

// Troca de programa (xam.cpp): cobre esta janela com a tela de carregamento ate o processo fechar e tira a animacao de
// fechar do Windows. Nao grava mais foto (item 30): devolve false (sem PREDATOR_TELA).
bool CapturarJanelaParaReinicio(const std::wstring&)
{
    if (!g_window) return false;
    BOOL sem = TRUE;
    DwmSetWindowAttribute(g_window, DWMWA_TRANSITIONS_FORCEDISABLED, &sem, sizeof(sem));
    SendMessageW(g_window, WM_COBRIR, 0, 0);
    Log("[janela] troca de programa: coberta com a tela de carregamento");
    return false;
}

// Troca de programa: a tela de carregamento em BGRA (de cima para baixo), que a placa segura enquanto o jogo novo
// comeca escuro (xenos_draw.cpp). false = nao e uma troca.
bool FotoReinicioPixels(std::vector<uint8_t>& bgra, uint32_t& w, uint32_t& h)
{
    if (!g_reinicio) return false;
    w = 1280; h = 720;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC tela = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(tela);
    HBITMAP bmp = CreateDIBSection(tela, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, tela);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); DeleteDC(mem); return false; }
    HGDIOBJ antes = SelectObject(mem, bmp);
    DesenharTelaCarregando(mem, (int)w, (int)h);
    GdiFlush();
    bgra.assign((const uint8_t*)bits, (const uint8_t*)bits + (size_t)w * h * 4);
    for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;
    SelectObject(mem, antes);
    DeleteObject(bmp);
    DeleteDC(mem);
    return true;
}

// Lugar da janela para a troca de fase (xam.cpp): "esquerda,topo,direita,baixo,tela cheia,maximizada"; em tela cheia,
// o lugar da janela antes dela (para o Alt+Enter voltar ao mesmo ponto)
std::wstring JanelaParaReinicio()
{
    WINDOWPLACEMENT p{ sizeof(p) };
    bool cheia = g_telaCheiaLigada;
    if (cheia) p = g_antesTelaCheia;
    else if (!g_window || !GetWindowPlacement(g_window, &p)) return L"";
    wchar_t b[128];
    swprintf(b, 128, L"%ld,%ld,%ld,%ld,%d,%d", p.rcNormalPosition.left, p.rcNormalPosition.top, p.rcNormalPosition.right,
             p.rcNormalPosition.bottom, cheia ? 1 : 0, p.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
    return b;
}

// Espera a preparacao da placa (DirectX + aquecimento) antes de o jogo comecar: assim o video de abertura e o som nao
// disputam o processador com a compilacao dos shaders. A janela mostra a tela de preparacao enquanto isso.
void GpuWaitReady()
{
    double t0 = GetTickCount64();
    while (!g_prepDone) Sleep(10);
    Log("[gpu] preparacao da placa terminou em %.1f s; iniciando o jogo", (GetTickCount64() - t0) / 1000.0);
}

void GpuStart()
{
    XmaInit();   // chip de audio XMA (registradores em 0x7FEA0000)
    SetConsoleCtrlHandler(ConsoleCtrl, TRUE);

    // pedido de saida limpa vindo de fora (scripts de teste): evento nomeado "FCRecomp_Sair"
    std::thread([]()
    {
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, L"FCRecomp_Sair");
        ResetEvent(ev);   // pode ter ficado sinalizado de uma execucao anterior
        WaitForSingleObject(ev, INFINITE);
        Log("[gpu] pedido de saida recebido");
        ConsoleCtrl(0);
    }).detach();

    // escritas do jogo nos registradores passam pelo nosso tratador
    AddVectoredExceptionHandler(1, MmioWriteHandler);
    DWORD old;
    VirtualProtect(g_base + GPU_MMIO_BASE, GPU_MMIO_SIZE, PAGE_READONLY, &old);

    // registradores que a CPU le diretamente (valores fixos do hardware)
    SetReg(0x0F00, 0x08100748);            // RB_EDRAM_TIMING
    SetReg(0x1951, 1);                     // D1MODE_VBLANK_STATUS: vblank aconteceu
    SetReg(0x1961, (1280u << 16) | 720);   // AVIVO_D1MODE_VIEWPORT_SIZE

    std::thread(WindowThread).detach();
    std::thread(CommandProcessorThread).detach();
    StartFreezeWatch();
    std::thread(VsyncThread).detach();
}

// VdSwap(bufferPtr, fetchPtr, ...): o jogo pede para apresentar o quadro.
// Escrevemos um pacote XE_SWAP no buffer de comandos que o jogo passou.
// roteiro gravado (input.cpp): foto da imagem entregue pela troca n. "troca" (contada pela CPU, igual a g_swapCount)
void GpuPedirFoto(uint64_t troca, const std::string& arquivo)
{
    std::lock_guard lock(g_fotosMutex);
    g_fotosPedidas.push_back({ troca, arquivo });
}

KFUNC(VdSwap)
{
    uint32_t buffer = Gpr32(ctx, 3), fetch = Gpr32(ctx, 4);
    void InputOnSwap(); InputOnSwap();   // input.cpp
    if (static int n = 0; perf::AvLog() && n++ < 6) Log("[av] %.1f ms: jogo pediu a troca %d (VdSwap)", perf::Now(), n);
    static int logged = 0;
    if (logged++ < 3)
        Log("[gpu] VdSwap(%08X, %08X, %08X, %08X, %08X, %08X, %08X, %08X) [r5]=%08X [r8]=%08X", Gpr32(ctx, 3), Gpr32(ctx, 4), Gpr32(ctx, 5),
            Gpr32(ctx, 6), Gpr32(ctx, 7), Gpr32(ctx, 8), Gpr32(ctx, 9), Gpr32(ctx, 10), Gpr32(ctx, 5) ? R32(Gpr32(ctx, 5)) : 0, Gpr32(ctx, 8) ? R32(Gpr32(ctx, 8)) : 0);
    uint32_t f[6];
    for (int i = 0; i < 6; i++) f[i] = R32(fetch + i * 4);
    // o fetch do D3D tem o endereco virtual; a GPU usa o fisico
    uint32_t frontBuffer = (f[1] & 0xFFFFF000) & 0x1FFFFFFF;
    f[1] = (f[1] & 0xFFF) | frontBuffer;
    uint32_t width = (f[2] & 0x1FFF) + 1;
    uint32_t height = ((f[2] >> 13) & 0x1FFF) + 1;
    uint32_t format = f[1] & 0x3F;

    uint32_t o = 0;
    auto put = [&](uint32_t v) { W32(buffer + o * 4, v); o++; };
    put(((6u - 1) << 16) | 0x4800);                    // tipo 0: fetch constant 0 (6 dwords)
    for (int i = 0; i < 6; i++) put(f[i]);
    put(0xC0000000 | ((5u - 1) << 16) | (0x64u << 8)); // tipo 3 XE_SWAP, 5 dwords
    put('SWAP');
    put(frontBuffer);
    put(width);
    put(height);
    put(format);
    while (o < 64) put(0x80000000);                    // NOPs (tipo 2)
}

// VdInitializeRingBuffer: o ring novo comeca com leitura e escrita em 0, como no console. Antes a posicao de escrita do
// ring anterior ficava no registrador: o processador de comandos executava esse trecho do ring novo (lixo) e, se o
// jogo ja tivesse escrito mais que isso, comecava no meio de um pacote; as vezes virava um WAIT_REG_MEM falso que nunca
// terminava (editor de mapas preso no primeiro quadro, 1 em ~5 aberturas).
void GpuRingInit(uint32_t base, uint32_t sizeLog2)
{
    SetReg(REG_CP_RB_WPTR, 0);
    SetReg(REG_CP_RB_RPTR, 0);
    g_ringBufferSizeLog2 = sizeLog2;
    g_ringBufferBase = base;
}

// ------------------------------------------------------------ espera do jogo por espaco no anel de comandos
// sub_82737FC8 (D3D do Xbox) fica num laco chamando sub_82743D88 ("a GPU travou? passou de 2000 ms?") enquanto o anel
// de comandos esta cheio, esperando o processador de comandos ler. No Xbox cada thread tem seu nucleo; aqui isso deixava
// um nucleo inteiro a 100 % e, no notebook de 15 W, tirava energia (e clock) da GPU integrada. Agora, depois de algumas
// voltas seguidas, cada volta cede a vez/dorme ~0,1 ms (ShortWait). FCRECOMP_SPINWAIT=1 volta ao giro puro.
// (no editor de mapas e outro endereco: GUEST_SUB_GPUWAIT)
GUEST_DECLARE_ORIGINAL(GUEST_SUB_GPUWAIT);
GUEST_FUNC(GUEST_SUB_GPUWAIT)
{
    uint32_t waitState = Gpr32(ctx, 3);   // { device, hora do ultimo progresso, ultimo valor visto }
    GUEST_ORIGINAL(GUEST_SUB_GPUWAIT)(ctx, base);
    {
        // Diagnostico (bug 6, carregamento infinito): a cerca que o jogo espera (A0005000 neste jogo) nao avanca ha mais de
        // 3 s -> anota o alvo e o estado (e repete a cada 10 s enquanto continuar presa)
        thread_local uint32_t lastProgress = 0;
        thread_local ULONGLONG stuckSince = 0, lastReport = 0;
        ULONGLONG t = GetTickCount64();
        uint32_t dev = R32(waitState);
        uint32_t ptr = dev ? R32(dev + 10384) : 0;
        uint32_t progress = ptr ? R32(ptr) : 0;
        if (progress != lastProgress || !stuckSince) { lastProgress = progress; stuckSince = t; lastReport = 0; }
        else if (t - stuckSince > 3000 && t - lastReport > 10000)
        {
            lastReport = t;
            Log("[espera-gpu] thread %s: a cerca nao avanca ha %.1f s; espera %08X, GPU escreveu %08X, ultima pedida [10396]=%08X"
                " (chamada de %08X) | [10433]=%02X [13536]=%08X | CP rptr=%u wptr=%u | quadro %llu desenhos %llu",
                CurrentGuestThread() ? CurrentGuestThread()->name : "?", (t - stuckSince) / 1000.0, Gpr32(ctx, 30), progress,
                dev ? R32(dev + 10396) : 0, (uint32_t)Lr(ctx), dev ? R8(dev + 10433) : 0, dev ? R32(dev + 13536) : 0,
                Reg(REG_CP_RB_RPTR), Reg(REG_CP_RB_WPTR), (unsigned long long)g_swapCount.load(), (unsigned long long)g_drawCount.load());
        }
    }
    static const bool spinOnly = getenv("FCRECOMP_SPINWAIT") != nullptr;
    if (spinOnly || Gpr32(ctx, 3) != 1) return;   // 0 = tempo esgotado (o jogo trata a GPU travada)
    thread_local int spins = 0;
    thread_local ULONGLONG lastCall = 0;
    ULONGLONG now = GetTickCount64();
    if (now - lastCall > 2) spins = 0;   // outra espera: comeca girando de novo
    lastCall = now;
    ShortWait(spins++, 100);
}

// FPS (opcao do launcher "fps": 0 = 30, 1 = 60, 2 = sem limite; FCRECOMP_FPS=30/60/0 forca): sub_8273E6F8 e chamada pelo
// D3D do jogo a cada troca de imagem e arma a espera de vblanks antes de mostrar a proxima: 16 bits de cima de r3 =
// intervalo (o jogo pede 2 = 30 FPS), contado a partir do vblank da ultima troca (dispositivo+15620; o contador de vblanks
// e +15616 e a contagem regressiva +15624, que a interrupcao de vblank, sub_827373F8, desconta so enquanto > 0). Se o
// intervalo ja passou, a troca sai na hora quando a posicao do feixe (0..100 %) <= 16 bits de baixo (limite); senao espera
// 1 vblank. 60 FPS: intervalo 1. Sem limite: intervalo 0 e limite FFFF (a troca sai sempre na hora, sem esperar vblank).
// So muda o ritmo das trocas: o vblank continua a 60 Hz (videos e o resto seguem iguais) e o jogo mede o tempo de
// cada quadro pelo relogio (mftb), entao anda na mesma velocidade (medido em 12/11: 4,8 m/s a 30 e 4,2 a 60 FPS).
#ifndef PREDATOR_EDITOR
int ModoFps()
{
    static const int modo = [] {
        if (const char* e = getenv("FCRECOMP_FPS")) { int n = atoi(e); return n == 30 ? 0 : n == 60 ? 1 : 2; }
        if (const char* e = getenv("FCRECOMP_FPS60")) return atoi(e) != 0 ? 1 : 0;
        return OpcoesAtuais().fps;
    }();
    return modo;
}
GUEST_DECLARE_ORIGINAL(sub_8273E6F8);
GUEST_FUNC(sub_8273E6F8)
{
    const int modo = ModoFps();
    if (modo != 0)
    {
        uint32_t r3 = Gpr32(ctx, 3);
        uint32_t novo = modo == 1 ? (1u << 16) | (r3 & 0xFFFF) : 0xFFFFu;
        if ((r3 >> 16) > (novo >> 16))
        {
            static bool avisou = false;
            if (!avisou)
            {
                avisou = true;
                if (modo == 1) Log("[gpu] 60 FPS: troca de imagem a cada %u vblank(s) passou a 1", r3 >> 16);
                else Log("[gpu] FPS sem limite: troca de imagem a cada %u vblank(s) passou a sair na hora", r3 >> 16);
            }
            SetGpr(ctx, 3, novo);
        }
    }
    GUEST_ORIGINAL(sub_8273E6F8)(ctx, base);
}
// Sem limite: alem da espera de vblank, o laco principal do jogo (sub_82333318) tem um limitador proprio: mede o quadro e
// dorme (sub_826437C8 -> Sleep) o que falta para 1/60 s (constante float em 820822EC, talvez usada por outras partes,
// por isso nao e mudada). So nesse modo a chamada de sono vinda dali (retorno 82333414) e pulada. Achado em 12/11 com
// um registro temporario de KeDelayExecutionThread + pilha.
GUEST_DECLARE_ORIGINAL(sub_826437C8);
GUEST_FUNC(sub_826437C8)
{
    if (ModoFps() == 2 && (uint32_t)ctx.lr == 0x82333414u)
    {
        static uint32_t n = 0;
        if (n++ % 600 == 0) Log("[gpu] FPS sem limite: espera do limitador do jogo pulada (%u vezes, %d ms)", n, (int32_t)Gpr32(ctx, 3));
        return;
    }
    GUEST_ORIGINAL(sub_826437C8)(ctx, base);
}
#endif

// sub_823C32B8 = "tempo atual em ms" do jogo (secao critica + mftb + conversao). O agendador de tarefas da thread 16
// (sub_8239A488 -> sub_823C3A30 temporizadores / sub_823C3630 fila) fica num laco perguntando a hora ~1,5 milhao de vezes
// por segundo, quase sempre no mesmo milissegundo: um nucleo inteiro a 100 % so esperando (no Xbox havia nucleo de sobra;
// no notebook de 15 W isso esquenta e tira energia da GPU). Agora, se a mesma thread pergunta mais de 64 vezes no mesmo
// ms, ela dorme ~0,25 ms. FCRECOMP_SPINWAIT=1 desliga (giro puro, como antes).
// FCRECOMP_TEMPOLOG=1 (depuracao): conta quem chama e escreve no log a cada ~10 s os chamadores mais frequentes.
#ifndef PREDATOR_EDITOR   // so o jogo (no editor esta funcao nao foi achada; e so economia de processador)
GUEST_DECLARE_ORIGINAL(sub_823C32B8);
GUEST_FUNC(sub_823C32B8)
{
    static const bool logOn = getenv("FCRECOMP_TEMPOLOG") != nullptr;
    if (logOn)
    {
        static std::mutex m;
        static std::unordered_map<uint32_t, uint64_t> callers;
        static ULONGLONG last = GetTickCount64();
        std::lock_guard lock(m);
        callers[(uint32_t)Lr(ctx)]++;
        if (GetTickCount64() - last > 10000)
        {
            last = GetTickCount64();
            std::vector<std::pair<uint64_t, uint32_t>> v;
            for (auto& [a, n] : callers) v.push_back({ n, a });
            std::sort(v.rbegin(), v.rend());
            for (size_t i = 0; i < v.size() && i < 8; i++) Log("[tempo] chamado de %08X: %llu vezes", v[i].second, (unsigned long long)v[i].first);
            callers.clear();
        }
    }
    GUEST_ORIGINAL(sub_823C32B8)(ctx, base);
    if (logOn)   // compara o relogio do jogo com o relogio real
    {
        static std::atomic<uint64_t> wall0{ 0 }; static std::atomic<uint32_t> game0{ 0 }, lastLogGame{ 0 };
        uint64_t w = GetTickCount64();
        if (!wall0) { wall0 = w; game0 = Gpr32(ctx, 3); lastLogGame = Gpr32(ctx, 3); }
        else if (Gpr32(ctx, 3) - lastLogGame > 10000)
        {
            lastLogGame = Gpr32(ctx, 3);
            Log("[tempo] relogio do jogo andou %u ms, relogio real %llu ms", Gpr32(ctx, 3) - game0.load(), (unsigned long long)(w - wall0));
        }
    }
    static const bool spinOnly = getenv("FCRECOMP_SPINWAIT") != nullptr;
    if (spinOnly) return;
    thread_local uint32_t lastMs = 0, sameMs = 0;
    if (Gpr32(ctx, 3) == lastMs)
    {
        if (++sameMs > 64) { ShortWait(1000, 250); sameMs = 0; }
    }
    else { lastMs = Gpr32(ctx, 3); sameMs = 0; }
}
#endif
