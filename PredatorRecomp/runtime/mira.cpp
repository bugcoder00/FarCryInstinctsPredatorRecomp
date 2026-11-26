// Tamanho da mira: opcao "Tamanho da mira" do launcher e da janela F2 (opcoes.ini "mira" = porcentagem; 100 = a do jogo).
//
// A mira e o elemento "i_crosshair" do HUD (CryInGame_Hud; achado em sub_821F8xxx e guardado em hud+848; "ai_crosshair"
// em hud+844 e o quadro em volta dele). sub_821FA380 (r3 = hud, chamada pela atualizacao do HUD em 821FF160) escolhe a
// textura da arma (crosshair_primary, crosshair_secondary... da tabela da arma; crosshair_melee sem arma) e monta os
// retangulos das tres chaves da animacao do elemento (sub_82479778(elemento, 0/10/20) devolve a chave; retangulo em
// R32(chave+4)+16: x0, x1, y0, y1 em int16) com metade do tamanho da textura, centrado na tela. Depois dela os tres
// retangulos sao escalados em volta do proprio centro. A funcao refaz os retangulos do zero (x1 = x0 + largura), entao
// a escala nunca acumula. O resto do HUD nao muda.
// Teste: FCRECOMP_MIRA=porcentagem. FCRECOMP_MIRALOG=1 anota os retangulos (a cada 2 s).
#ifndef PREDATOR_EDITOR
#include "guest.h"
#include "opcoes.h"
#include "perf.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace
{
constexpr uint32_t CHAVE_ELEMENTO = 0x82479778u;   // (elemento, tempo) -> chave da animacao

uint32_t ChamarMira(GuestCpu& ctx, uint32_t f, uint32_t r3, uint32_t r4)
{
    GuestCpu salvo = ctx;
    SetGpr(ctx, 3, r3); SetGpr(ctx, 4, r4);
    GuestCall(ctx, f);
    const uint32_t r = Gpr32(ctx, 3);
    ctx = salvo;
    return r;
}

int16_t L16(uint32_t a) { return (int16_t)(uint16_t)((R8(a) << 8) | R8(a + 1)); }
void E16(uint32_t a, int v) { const uint16_t u = (uint16_t)(int16_t)std::clamp(v, -32768, 32767); W8(a, (uint8_t)(u >> 8)); W8(a + 1, (uint8_t)u); }
bool EnderecoOk(uint32_t a) { return (a >= 0x40000000 && a < 0x7FFF0000) || (a >= 0xA0000000 && a < 0xBFFF0000); }
}

GUEST_DECLARE_ORIGINAL(sub_821FA380);
GUEST_FUNC(sub_821FA380)
{
    const uint32_t hud = Gpr32(ctx, 3);
    GUEST_ORIGINAL(sub_821FA380)(ctx, base);
    static const bool diag = getenv("FCRECOMP_MIRALOG") != nullptr;
    const int pct = std::clamp(OpcoesAtuais().mira, MIRA_MIN, MIRA_MAX);
    if (pct == 100 && !diag) return;
    const uint32_t elem = EnderecoOk(hud) ? R32(hud + 848) : 0;
    if (!EnderecoOk(elem)) return;
    static double ultimo = 0;
    static int chamadas = 0;
    chamadas++;
    const bool anotar = diag && perf::Now() - ultimo > 2000;
    const float s = pct / 100.0f;
    for (uint32_t tempo : { 0u, 10u, 20u })
    {
        const uint32_t chave = ChamarMira(ctx, CHAVE_ELEMENTO, elem, tempo);
        if (!EnderecoOk(chave) || !EnderecoOk(R32(chave + 4))) continue;
        const uint32_t r = R32(chave + 4) + 16;
        const int x0 = L16(r), x1 = L16(r + 2), y0 = L16(r + 4), y1 = L16(r + 6);
        if (anotar)
            Log("[mira] hud %08X elemento %08X chave %u: x %d..%d y %d..%d (%d chamadas em 2 s), tamanho %d%%", hud, elem, tempo, x0, x1, y0, y1,
                chamadas, pct);
        if (pct == 100 || x1 <= x0 || y1 <= y0) continue;
        const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, mw = (x1 - x0) * 0.5f * s, mh = (y1 - y0) * 0.5f * s;
        E16(r, (int)std::lround(cx - mw)); E16(r + 2, (int)std::lround(cx + mw));
        E16(r + 4, (int)std::lround(cy - mh)); E16(r + 6, (int)std::lround(cy + mh));
    }
    if (anotar) { ultimo = perf::Now(); chamadas = 0; }
}
#endif
