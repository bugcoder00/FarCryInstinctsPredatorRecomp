// Contexto da CPU do jogo para o runtime (PredatorRecomp/runtime), versao PredatorGame.
//
// O runtime nao conhece a disposicao do contexto: ele usa so as funcoes e macros abaixo (a lista completa esta em
// runtime/guest.h). Aqui o contexto e a propria PredatorCpuContext do codigo gerado pelo PredatorRecomp, entao
// um GuestCpu& do runtime e o PredatorCpuContext* das funcoes geradas apontam para o mesmo objeto.
#pragma once

#include <cstdint>

#include "predator_cpu.h"

using GuestCpu = PredatorCpuContext;

// Funcoes do jogo e do sistema. Referencia e ponteiro sao passados do mesmo jeito, entao a assinatura bate com a das
// funcoes C geradas (void sub_X(PredatorCpuContext* ctx, uint8_t* base)).
typedef void GuestFunc(GuestCpu& __restrict ctx, uint8_t* base);
#define GUEST_FUNC(name) extern "C" void name(GuestCpu& __restrict ctx, uint8_t* base)

// Funcao do jogo substituida pelo runtime: o gerador chama a versao traduzida de <nome>_original
// (dois niveis para aceitar os apelidos GUEST_SUB_* abaixo)
#define GUEST_ORIGINAL(name) GUEST_ORIGINAL_(name)
#define GUEST_ORIGINAL_(name) name##_original
#define GUEST_DECLARE_ORIGINAL(name) extern "C" void GUEST_ORIGINAL(name)(GuestCpu& __restrict ctx, uint8_t* base)

// Simbolo criado por KFUNC (as importacoes sao achadas pelo nome, entao o prefixo e so para nao colidir)
#define GUEST_EXPORT_SYMBOL(name) guest_export_##name

// Mapa da imagem do programa traduzido: o jogo (default.xex, PredatorGame.exe) ou o editor de mapas (ige.xex,
// PredatorEditor.exe, compilado com PREDATOR_EDITOR; o menu MAP EDITOR do jogo pede para abrir o ige.xex)
constexpr uint64_t GUEST_IMAGE_BASE = 0x82000000ull;
constexpr uint64_t GUEST_MEMORY_SIZE = 0x100000000ull;
#ifdef PREDATOR_EDITOR
constexpr uint64_t GUEST_IMAGE_SIZE = 0x9A0000ull;
constexpr uint64_t GUEST_CODE_BASE = 0x820C0000ull;
constexpr uint64_t GUEST_CODE_SIZE = 0x7C0000ull;
constexpr uint32_t GUEST_ENTRY_POINT = 0x82636B58;
#define GUEST_XEX_NAME "ige.xex"
#ifdef PREDATOR_INGLES
#define GUEST_TITULO "Far Cry Instincts Predator Recomp - Map editor"
#else
#define GUEST_TITULO "Far Cry Instincts Predator Recomp - Editor de mapas"
#endif
#define GUEST_IMAGE_FILE "image_editor.bin"
#define GUEST_IMAGE_SHA256 "3a704773204a67eb94a20c4deedfbb37d35861257457bd0bbebad7a671143c0e"
// funcoes do programa que o runtime substitui (as mesmas do jogo, achadas no editor pelo codigo igual)
#define GUEST_SUB_LONGJMP sub_82622450    // longjmp (setjmp.cpp)
#define GUEST_SUB_GPUWAIT sub_826582E8    // "a GPU travou?" na espera por espaco no anel (gpu.cpp)
#define GUEST_SUB_FLTOUT sub_82633110     // _fltout (PREDATOR_SELFTEST)
#define GUEST_SUB_MIRA sub_82257300       // giro da camera do jogador (input.cpp: camera direta e mira do mouse)
#else
constexpr uint64_t GUEST_IMAGE_SIZE = 0x980000ull;
constexpr uint64_t GUEST_CODE_BASE = 0x820B0000ull;
constexpr uint64_t GUEST_CODE_SIZE = 0x7D0000ull;
constexpr uint32_t GUEST_ENTRY_POINT = 0x826444F0;
#define GUEST_XEX_NAME "default.xex"
#define GUEST_TITULO "Far Cry Instincts Predator Recomp"
#define GUEST_IMAGE_FILE "image.bin"
#define GUEST_IMAGE_SHA256 "de81b4fae4f1ae285c0a16bad49cfc6a844fc3378f34ad8a6083a53fb899ef30"
#define GUEST_SUB_LONGJMP sub_82630280
#define GUEST_SUB_GPUWAIT sub_82743D88
#define GUEST_SUB_FLTOUT sub_82641C18
#define GUEST_SUB_MIRA sub_8228BC78
#endif

// Registradores. No modo 32 bits do console os enderecos e a maioria dos valores usam os 32 bits de baixo.
inline uint32_t Gpr32(const GuestCpu& c, int n) { return static_cast<uint32_t>(c.r[n]); }
inline int32_t GprS32(const GuestCpu& c, int n) { return static_cast<int32_t>(static_cast<uint32_t>(c.r[n])); }
inline uint64_t Gpr64(const GuestCpu& c, int n) { return c.r[n]; }
inline void SetGpr(GuestCpu& c, int n, uint64_t v) { c.r[n] = v; }
inline double Fpr(const GuestCpu& c, int n) { return c.f[n].f; }
inline void SetFpr(GuestCpu& c, int n, double v) { c.f[n].f = v; }
inline uint64_t Lr(const GuestCpu& c) { return c.lr; }
inline uint32_t Ctr32(const GuestCpu& c) { return static_cast<uint32_t>(c.ctr); }
inline void SetCtr(GuestCpu& c, uint64_t v) { c.ctr = v; }
inline void ResetFpscr(GuestCpu& c) { c.fpscr = 0; }   // o codigo gerado usa o arredondamento padrao do PC

// Tabela endereco -> funcao (chamadas indiretas), em predator_bridge.cpp
GuestFunc*& GuestFunctionSlot(uint32_t address);

// Lista de funcoes gerada (predator_table.c): { endereco, funcao }, terminada em { 0, 0 }
struct GuestFunctionEntry
{
    uint32_t address;
    GuestFunc* function;
};
extern "C" const GuestFunctionEntry predator_functions[];

template <typename F>
void ForEachGuestFunction(F&& f)
{
    for (const GuestFunctionEntry* e = predator_functions; e->function; ++e) f(e->address, e->function);
}
