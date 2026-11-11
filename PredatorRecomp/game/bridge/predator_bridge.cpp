// Ponte: implementa os ganchos que o codigo gerado pelo PredatorRecomp chama (predator_cpu.h) usando o runtime do jogo
#include "guest.h"
#include "diag.h"

#include <intrin.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------- tabela endereco -> funcao

static std::vector<GuestFunc*> g_slots(GUEST_CODE_SIZE / 4, nullptr);
static GuestFunc* g_outside = nullptr;   // destino fora da regiao de codigo (sempre vazio)

GuestFunc*& GuestFunctionSlot(uint32_t address)
{
    if (address < GUEST_CODE_BASE || address >= GUEST_CODE_BASE + GUEST_CODE_SIZE || (address & 3))
    {
        g_outside = nullptr;
        return g_outside;
    }
    return g_slots[(address - GUEST_CODE_BASE) / 4];
}

[[noreturn]] static void Fatal(GuestCpu& ctx, const char* what, uint32_t from, uint32_t target)
{
    Log("[ERRO] %s: %08X -> %08X (lr=%08X)", what, from, target, (uint32_t)Lr(ctx));
    char key[96], detail[64];
    snprintf(key, sizeof(key), "%s %08X", what, target);
    snprintf(detail, sizeof(detail), "de %08X", from);
    diag::Note(diag::Crash, key, detail);
    diag::WriteReport(what);
    ExitProcess(1);
}

static void CallGuest(GuestCpu& ctx, uint8_t* base, uint32_t from, uint32_t target, const char* what)
{
    GuestFunc* fn = GuestFunctionSlot(target);
    if (!fn) Fatal(ctx, what, from, target);
    // o runtime preenche as posicoes vazias com a funcao de erro dele, que usa o CTR para dizer o destino
    SetCtr(ctx, target);
    fn(ctx, base);
}

extern "C" void predator_call_indirect(PredatorCpuContext* cpu, uint8_t* base, uint32_t target)
{
    CallGuest(*cpu, base, (uint32_t)Lr(*cpu) - 4, target, "chamada indireta para funcao inexistente");
}

// bctr sem tabela ou desvio para fora da funcao: vira uma chamada seguida de retorno
extern "C" void predator_jump_indirect(PredatorCpuContext* cpu, uint8_t* base, uint32_t from, uint32_t target)
{
    CallGuest(*cpu, base, from, target, "desvio indireto para funcao inexistente");
}

// ---------------------------------------------------------------- importacoes (biblioteca + ordinal -> funcao do runtime)

namespace
{
    struct ImportName
    {
        const char* library;
        uint32_t ordinal;
        const char* name;
    };
    const ImportName kImportNames[] = {
#include "import_names.inc"
    };

    struct ImportKey
    {
        std::string library;
        uint32_t ordinal;
        bool operator<(const ImportKey& o) const { return ordinal != o.ordinal ? ordinal < o.ordinal : library < o.library; }
    };

    struct ImportEntry
    {
        GuestFunc* fn = nullptr;
        std::string name;
        std::atomic<bool> warned{ false };
    };

    std::mutex g_importMutex;
    std::map<ImportKey, ImportEntry> g_imports;

    ImportEntry& ResolveImport(const char* library, uint32_t ordinal)
    {
        std::lock_guard lock(g_importMutex);
        auto [it, inserted] = g_imports.try_emplace(ImportKey{ library, ordinal });
        if (inserted)
        {
            for (const auto& n : kImportNames)
                if (n.ordinal == ordinal && _stricmp(n.library, library) == 0) it->second.name = n.name;
            if (!it->second.name.empty()) it->second.fn = GuestExport::Find(it->second.name);
            if (it->second.name.empty())
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "%s#%u", library, ordinal);
                it->second.name = buf;
            }
        }
        return it->second;
    }

    // Caminho rapido: cada thread guarda o que ja resolveu, pela posicao do texto da biblioteca no codigo gerado + ordinal.
    // Sem ele toda chamada ao sistema (milhoes por segundo no carregamento) passava por uma trava unica para todas as
    // threads e criava uma std::string com o nome da biblioteca.
    ImportEntry& ResolveImportFast(const char* library, uint32_t ordinal)
    {
        thread_local std::unordered_map<uint64_t, ImportEntry*> cache;
        const uint64_t key = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(library)) << 16) ^ ordinal;
        auto it = cache.find(key);
        if (it != cache.end()) return *it->second;
        ImportEntry& e = ResolveImport(library, ordinal);
        cache.emplace(key, &e);
        return e;
    }
}

// PREDATOR_SELFTEST=1: na primeira importacao, roda funcoes do jogo com entradas conhecidas e mostra o resultado
// (compara a traducao com o valor esperado sem precisar chegar ate o ponto do jogo que as usa)
extern "C" void GUEST_SUB_FLTOUT(PredatorCpuContext* ctx, uint8_t* base);   // double -> digitos decimais (_fltout)
static void SelfTest(GuestCpu& live, uint8_t* base)
{
    const double values[] = { 1.5, 100.0, 0.001, 123456.789, -2.0, 0.0, 1e300, 1e-300, 4.9e-324, 1e-310, INFINITY, NAN, 3.4e38, 1.17e-38 };
    for (double v : values)
    {
        GuestCpu ctx = live;
        SetGpr(ctx, 1, Gpr32(live, 1) - 0x2000);
        const uint32_t out = Gpr32(ctx, 1) - 0x400, buf = out + 0x40;
        memset(base + out, 0, 0x100);
        uint64_t bits;
        memcpy(&bits, &v, 8);
        SetGpr(ctx, 3, bits);
        SetGpr(ctx, 4, out);
        SetGpr(ctx, 5, buf);
        SetGpr(ctx, 6, 0x40);
        GUEST_SUB_FLTOUT(&ctx, base);
        Log("[selftest] _fltout(%g): sinal=%d expoente=%d flag=%08X digitos='%s'", v, (int32_t)R32(out), (int32_t)R32(out + 4), R32(out + 8),
            (const char*)(base + buf));
    }
    ExitProcess(0);
}

extern "C" void predator_import(PredatorCpuContext* cpu, uint8_t* base, const char* library, uint32_t ordinal)
{
    GuestCpu& ctx = *cpu;
    // getenv e lento (percorre todas as variaveis): lido uma vez so. Antes, a cada importacao, ele sozinho comia ate
    // 38% do tempo de algumas threads do jogo.
    static const bool selfTest = getenv("PREDATOR_SELFTEST") != nullptr;
    if (static std::atomic<bool> tested{ false }; selfTest && !tested.exchange(true)) SelfTest(ctx, base);
    ImportEntry& entry = ResolveImportFast(library, ordinal);
    if (entry.fn)
    {
        entry.fn(ctx, base);
        return;
    }
    if (!entry.warned.exchange(true))
    {
        Log("[importacao] %s nao existe no runtime (chamada de %08X); devolvendo 0", entry.name.c_str(), (uint32_t)Lr(ctx));
        diag::Note(diag::Stub, entry.name.c_str(), "importacao sem implementacao");
    }
    SetGpr(ctx, 3, 0);
}

// ---------------------------------------------------------------- instrucoes especiais

extern "C" void predator_syscall(PredatorCpuContext* cpu, uint8_t*, uint32_t address)
{
    static std::atomic<bool> warned{ false };
    if (!warned.exchange(true)) Log("[cpu] instrucao sc em %08X (r0=%08X) ignorada", address, (uint32_t)cpu->r[0]);
}

extern "C" void predator_trap(PredatorCpuContext* cpu, uint8_t*, uint32_t address)
{
    // o jogo usa armadilhas para asserts de depuracao: registra uma vez por endereco e continua
    static std::mutex m;
    static std::set<uint32_t> seen;
    std::lock_guard lock(m);
    if (seen.insert(address).second) Log("[cpu] armadilha (tw/twi) em %08X, lr=%08X; ignorada", address, (uint32_t)cpu->lr);
}

extern "C" void predator_unimplemented(PredatorCpuContext* cpu, uint8_t*, uint32_t address, const char* name)
{
    static std::mutex m;
    static std::set<uint32_t> seen;
    std::lock_guard lock(m);
    if (seen.insert(address).second)
    {
        Log("[cpu] %s em %08X sem traducao (lr=%08X)", name, address, (uint32_t)cpu->lr);
        char key[96];
        snprintf(key, sizeof(key), "instrucao sem traducao %s em %08X", name, address);
        diag::Note(diag::Stub, key, "PredatorRecomp");
    }
}

uint64_t RoteiroTimebase(uint64_t real);   // input.cpp: na repeticao de um roteiro, a duracao gravada de cada tique

extern "C" uint64_t predator_timebase(void)
{
    return RoteiroTimebase(__rdtsc());   // KeQueryPerformanceFrequency devolve a frequencia deste mesmo relogio
}
