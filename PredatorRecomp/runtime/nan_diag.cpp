// Luz do sol sem projecao valida (casas pretas na vila da 21_DUNEA, gravacao 2026-11-04_14-45).
// sub_82369920(r3 = cena, r4 = descricao da luz) poe a luz na lista do quadro e monta a matriz de sombra dela
// (entrada da lista + 144, 4 linhas de 4 floats, vira as constantes c35-c38 do shader de vertices dos objetos que
// recebem sombra). A luz do sol da fase (tipo 3, raio 1e8, sem largura/altura) ja vem com a matriz de projecao
// cheia de infinitos, e o produto vira NaN. Enquanto o jogo usa a sombra dinamica do jogador a matriz certa e outra;
// quando passa a usar a do sol (e para de desenhar o mapa de sombra), os objetos recebiam coordenadas NaN e o teste
// de sombra do shader dava "na sombra" em tudo: casas e caixas quase pretas (so a luz ambiente, bem escura nessa fase).
// Correcao: se a matriz montada nao for finita, troca por uma que manda todo ponto para fora da sombra.
// FCRECOMP_SOMBRANAN=0 desliga; =valor muda a profundidade usada (teste). FCRECOMP_NANLOG=1 anota.
#include "guest.h"
#include <cmath>
#include <cstring>
#include <cstdlib>

#ifndef PREDATOR_EDITOR
namespace
{
    bool Finito(uint32_t a) { return (R32(a) & 0x7F800000u) != 0x7F800000u; }
    void WF(uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); W32(a, v); }
}

GUEST_DECLARE_ORIGINAL(sub_82369920);
GUEST_FUNC(sub_82369920)
{
    const uint32_t cena = Gpr32(ctx, 3), luz = Gpr32(ctx, 4);
    GUEST_ORIGINAL(sub_82369920)(ctx, base);

    static const char* env = getenv("FCRECOMP_SOMBRANAN");
    static const bool ligado = !env || atof(env) != 0.0;
    static const float prof = env && atof(env) != 0.0 ? (float)atof(env) : 2.0f;
    if (!ligado) return;
    // a entrada que acabou de ser preenchida: lista em R32(cena+12) + 9768, 248 bytes cada, contador em +17704
    uint32_t lista = R32(cena + 12);
    if (!lista) return;
    uint32_t n = R32(lista + 17704);
    if (n == 0 || n > 32) return;
    uint32_t m = lista + 9768 + (n - 1) * 248 + 144;
    bool ok = true;
    for (uint32_t i = 0; i < 16; i++) ok = ok && Finito(m + i * 4);
    if (ok) return;
    static int avisos = 0;
    static const bool logOn = getenv("FCRECOMP_NANLOG") != nullptr;
    if (logOn && avisos++ < 5)
        Log("[sombra] luz %08X (tipo %d) com matriz de sombra invalida: trocada por 'sem sombra' (profundidade %g)", luz, (int)R32(luz), prof);
    const float linhas[4][4] = { { 0, 0, 0, 0.5f }, { 0, 0, 0, 0.5f }, { 0, 0, 0, prof }, { 0, 0, 0, 1 } };
    for (uint32_t r = 0; r < 4; r++)
        for (uint32_t c = 0; c < 4; c++) WF(m + (r * 4 + c) * 4, linhas[r][c]);
}
#endif

// Depuracao das consultas de oclusao (bugs 20/21): FCRECOMP_OCLUSAOLOG=t<ini>-<fim> tambem anota cada GetData do jogo
// (sub_82741238: r3 = consulta, r4 = saida; volta 0 = pronto, 1 = ainda nao) com o numero que o jogo recebeu.
#ifndef PREDATOR_EDITOR
int64_t RoteiroTiqueAtual();
GUEST_DECLARE_ORIGINAL(sub_82741238);
GUEST_FUNC(sub_82741238)
{
    const uint32_t q = Gpr32(ctx, 3), out = Gpr32(ctx, 4);
    GUEST_ORIGINAL(sub_82741238)(ctx, base);
    static const char* qlog = getenv("FCRECOMP_OCLUSAOLOG");
    if (!qlog || qlog[0] != 't') return;
    int64_t ini = atoll(qlog + 1), fim = strchr(qlog, '-') ? atoll(strchr(qlog, '-') + 1) : ini, tq = RoteiroTiqueAtual();
    if (tq < ini || tq > fim) return;
    uint32_t n = R32(q + 144);
    Log("[getdata] tique %lld: consulta %08X (%u registros: %08X %08X) -> %s, %d amostras", (long long)tq, q, n, R32(q + 24),
        n > 1 ? R32(q + 28) : 0, Gpr32(ctx, 3) == 0 ? "pronto" : "ainda nao", (int32_t)R32(out));
}
#endif
