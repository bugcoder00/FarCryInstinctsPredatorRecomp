// Utilitarios para acessar a memoria do Xbox 360 (big-endian) a partir do PC
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>

// Contexto da CPU do jogo, definido em guest_cpu_backend.h (PredatorRecomp/game/bridge) com:
//   GuestCpu, GuestFunc                       tipo do contexto e das funcoes do jogo/sistema
//   Gpr32/GprS32/Gpr64/SetGpr(ctx, n)         registradores inteiros (32 bits de baixo, com sinal, 64 bits, gravar)
//   Fpr/SetFpr(ctx, n), Lr(ctx), Ctr32/SetCtr(ctx)   ponto flutuante, retorno, contador
//   ResetFpscr(ctx)                           estado inicial do ponto flutuante de uma thread nova
//   GUEST_FUNC(nome)                          define uma funcao do jogo (ganchos) com parametros ctx e base
//   GUEST_ORIGINAL(nome), GUEST_DECLARE_ORIGINAL(nome)   versao traduzida de uma funcao substituida
//   GUEST_EXPORT_SYMBOL(nome)                 simbolo criado por KFUNC
//   GUEST_IMAGE_BASE/SIZE, GUEST_CODE_BASE/SIZE, GUEST_MEMORY_SIZE
//   GuestFunctionSlot(endereco), ForEachGuestFunction(f)   tabela de chamadas indiretas e lista de funcoes
#include "guest_cpu_backend.h"

extern uint8_t* g_base;   // inicio dos 4 GB de memoria do "Xbox"

// Registradores da GPU (0x7FC80000, 64K registradores de 4 bytes)
constexpr uint32_t GPU_MMIO_BASE = 0x7FC80000;
constexpr uint32_t GPU_MMIO_SIZE = 0x40000;
extern uint8_t* g_gpuRegsHost;

inline uint8_t* G(uint32_t addr) { return g_base + addr; }

inline uint32_t R32(uint32_t a) { return __builtin_bswap32(*(uint32_t*)G(a)); }
inline uint16_t R16(uint32_t a) { return __builtin_bswap16(*(uint16_t*)G(a)); }
inline uint64_t R64(uint32_t a) { return __builtin_bswap64(*(uint64_t*)G(a)); }
inline uint8_t  R8(uint32_t a)  { return *G(a); }
inline void W32(uint32_t a, uint32_t v) { *(uint32_t*)G(a) = __builtin_bswap32(v); }
inline void W16(uint32_t a, uint16_t v) { *(uint16_t*)G(a) = __builtin_bswap16(v); }
inline void W64(uint32_t a, uint64_t v) { *(uint64_t*)G(a) = __builtin_bswap64(v); }
inline void W8(uint32_t a, uint8_t v)   { *G(a) = v; }

inline std::string RStr(uint32_t a)
{
    return a ? std::string((const char*)G(a)) : std::string();
}

// ANSI_STRING { u16 Length; u16 MaxLength; u32 Buffer; }
inline std::string RAnsiString(uint32_t a)
{
    if (!a) return {};
    uint16_t len = R16(a);
    uint32_t buf = R32(a + 4);
    return buf ? std::string((const char*)G(buf), len) : std::string();
}

// Chama uma funcao do jogo pelo endereco
inline void GuestCall(GuestCpu& ctx, uint32_t addr)
{
    GuestFunctionSlot(addr)(ctx, g_base);
}

// Retorno de funcoes do kernel
inline void Ret(GuestCpu& ctx, uint64_t v) { SetGpr(ctx, 3, v); }

// Memoria do jogo
enum class MemKind { System, Virtual, Physical };
uint32_t GuestAlloc(uint32_t size, uint32_t align = 16, MemKind kind = MemKind::System);
bool GuestAllocAt(uint32_t addr, uint32_t size, MemKind kind);
void GuestFree(uint32_t addr);
uint32_t GuestAllocSize(uint32_t addr);
bool GuestQuery(uint32_t addr, uint32_t& start, uint32_t& size);
uint32_t GuestNextAllocation(uint32_t addr);
uint32_t GuestAllocSerial(uint32_t addr);

// Log
void Log(const char* fmt, ...);
void RedeEncerrar();            // rede.cpp: ao sair, fecha as portas abertas no roteador (UPnP)
bool RedeReabrirComPortas();    // rede.cpp: ha portas abertas (a troca de fase passa ao processo novo)

// Rastreamento: com FCRECOMP_TRACE=1 toda chamada ao sistema vai para o log
extern bool g_trace;
void TraceCall(const char* name, GuestCpu& ctx);

// Tempo gasto em cada funcao do sistema (FCRECOMP_KTEMPO=inicio:fim, em segundos): ranking no log no fim da janela
extern bool g_kfuncTiming;
struct KFuncTimer
{
    explicit KFuncTimer(const char* name);
    ~KFuncTimer();
    const char* name;
    int64_t start;
};

// Registro das funcoes do sistema por nome (o tradutor PredatorRecomp chama as importacoes por biblioteca + ordinal,
// e a ponte acha a implementacao pelo nome)
struct GuestExport
{
    GuestExport(const char* name, GuestFunc* fn);
    static GuestFunc* Find(const std::string& name);
};

#define KFUNC(name) \
    static void name##_impl(GuestCpu& __restrict ctx, uint8_t* base); \
    GUEST_FUNC(GUEST_EXPORT_SYMBOL(name)) \
    { \
        if (g_trace) TraceCall(#name, ctx); \
        if (g_kfuncTiming) { KFuncTimer t_(#name); name##_impl(ctx, base); } else name##_impl(ctx, base); \
    } \
    static const GuestExport name##_export(#name, GUEST_EXPORT_SYMBOL(name)); \
    static void name##_impl(GuestCpu& __restrict ctx, uint8_t* base)

// o windows.h define algumas destas como macros
#undef STATUS_TIMEOUT
#undef STATUS_PENDING
#undef STATUS_INVALID_HANDLE
#undef STATUS_INVALID_PARAMETER
#undef STATUS_NO_MEMORY
#undef STATUS_UNSUCCESSFUL
#undef STATUS_NOT_IMPLEMENTED
#undef STATUS_END_OF_FILE
#undef STATUS_OBJECT_NAME_NOT_FOUND
#undef STATUS_OBJECT_PATH_NOT_FOUND
#undef STATUS_OBJECT_NAME_COLLISION
#undef STATUS_NO_MORE_FILES

constexpr uint32_t STATUS_SUCCESS = 0;
constexpr uint32_t STATUS_TIMEOUT = 0x102;
constexpr uint32_t STATUS_PENDING = 0x103;
constexpr uint32_t STATUS_NO_MORE_FILES = 0x80000006;
constexpr uint32_t STATUS_UNSUCCESSFUL = 0xC0000001;
constexpr uint32_t STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr uint32_t STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t STATUS_END_OF_FILE = 0xC0000011;
constexpr uint32_t STATUS_NO_MEMORY = 0xC0000017;
constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr uint32_t STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
