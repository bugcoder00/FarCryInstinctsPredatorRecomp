// Trapacas ligadas no launcher (aba TRAPACAS, opcoes.ini "trapaca_*"), aplicadas sem digitar o codigo no menu do jogo.
//
// Codigos do jogo (sub_821AA568 = texto digitado na caixinha do MENU DE TRAPACAS do menu principal, compara com sub_821AA960):
//   UnleashHell  -> bits de G+1036 |= 2   (municao infinita)
//   Bloodlust    -> |= 1                  (adrenalina infinita)
//   FeralAttack  -> |= 8                  (poderes ferais: o jogo so liga o 5 e o 6; aqui as sete, veja Habilidades)
//   ImJackCarver -> vida do jogador = vida maxima, uma vez (sub_82240588: float +232 = (float)int +236)
//   GiveMeItAll  -> perfil +176 |= 1      (todas as fases)
//   GiveMeTheMaps-> perfil +180 |= 0x7FF (e 0x7F800 com o GiveMeItAll)   (todos os mapas)
// G = R32(0x8291234C) (lis -32111 + 9036). Perfil = R32(R32(0x8293A7A4) + 20) (lis -32108 - 22620).
// Os bits de G+1036 sao lidos a cada quadro na atualizacao do jogador (sub_8226A0D8, r3 = jogador, jogador+440 = G),
// que so os usa fora do multijogador (sub_82212C30: G+858 = partida em rede, G+108 > 1 = mais de um jogador).
// O jogo zera G+1036 em jogo novo e gravava/somava os bits no save (sub_8220B260/B4A8): agora as trapacas ficam fora
// do jogo salvo (veja "trapacas fora do jogo salvo") e os bits ligados aqui sao postos de novo a cada quadro.
// "Invencivel" (novo, nao existe no menu do jogo): o dano passa pelas regras do jogo em Lua (GameRules:OnDamage, que
// calcula tiros, explosoes, quedas - "landed" - e chama GameRules:ApplyDamage, que tira a vida). Aqui as duas sao
// embrulhadas (FcInvOD / FcInvAD, postas de novo se o jogo recarregar as regras): com a trapaca ligada, um golpe no
// jogador local (_localplayer) volta sem fazer nada, entao a vida nao cai em nenhum quadro e a queda nao mata.
// Antes ligava a variavel GodMode do motor (objeto em 0x829122F4): ela e de depuracao - a queda e alguns tiros tiravam
// vida mesmo assim e o jogo escrevia "GOD" sobre o jogador (SetLabel: um quadrado branco na tela).
// Reserva: se algo tirar vida mesmo assim, ela volta ao maximo no mesmo quadro (como o ImJackCarver sempre).
// FCRECOMP_VIDALOG=1 anota no registro cada quadro em que a vida do jogador ficou abaixo do maximo.
// Testes: FCRECOMP_TRAPACAS="municao,adrenalina,feral,fases,mapas,invencivel" (modo teste ignora o opcoes.ini).
#ifndef PREDATOR_EDITOR
#include "guest.h"
#include "opcoes.h"
#include <bit>
#include <cstdlib>
#include <cstring>
#include <string>
#include <windows.h>

namespace
{
constexpr uint32_t PERFIS = 0x8293A7A4;      // ponteiro para o gerenciador de perfis

struct Ligadas { bool municao, adrenalina, feral, fases, mapas, invencivel; };

Ligadas Atuais()
{
    static const char* teste = getenv("FCRECOMP_TRAPACAS");
    if (teste)
        return { strstr(teste, "municao") != nullptr, strstr(teste, "adrenalina") != nullptr, strstr(teste, "feral") != nullptr,
                 strstr(teste, "fases") != nullptr, strstr(teste, "mapas") != nullptr, strstr(teste, "invencivel") != nullptr };
    const Opcoes& o = OpcoesAtuais();
    if (o.modoTeste) return {};
    return { o.trapacaMunicao, o.trapacaAdrenalina, o.trapacaFeral, o.trapacaFases, o.trapacaMapas, o.trapacaInvencivel };
}

bool Multijogador(uint32_t g) { return *G(g + 858) != 0 || R32(g + 108) > 1; }

// embrulho do dano em Lua (veja o comeco). Funcoes globais definidas uma vez por estado Lua; FcInvAtivo liga/desliga.
const char* LUA_INVENCIVEL = R"(
if not FcInvAD then
  function FcInvProtege(t)
    if FcInvAtivo ~= 1 or not _localplayer then return nil end
    if t == _localplayer then return 1 end
    if type(t) == "table" and t.target == _localplayer then return 1 end
    return nil
  end
  function FcInvAD(self, a, b, c, d, e, f)
    if FcInvProtege(a) or FcInvProtege(b) then return end
    if FcInvOrigAD then return FcInvOrigAD(self, a, b, c, d, e, f) end
  end
  function FcInvOD(self, a, b, c, d, e, f)
    if FcInvProtege(a) or FcInvProtege(b) then return end
    if FcInvOrigOD then return FcInvOrigOD(self, a, b, c, d, e, f) end
  end
end
if GameRules then
  if GameRules.ApplyDamage ~= FcInvAD then FcInvOrigAD = GameRules.ApplyDamage  GameRules.ApplyDamage = FcInvAD end
  if GameRules.OnDamage ~= FcInvOD then FcInvOrigOD = GameRules.OnDamage  GameRules.OnDamage = FcInvOD end
end
)";
}

uint32_t LuaRodar(GuestCpu& ctx, const std::string& texto);   // npc_editor.cpp

// liga (a cada ~1 s: as regras podem ser recarregadas numa fase nova) ou desliga o embrulho do dano
static void Invencivel(GuestCpu& ctx, bool ligar)
{
    static int conta = 0;
    static bool ligado = false;
    if (!ligar && !ligado) return;
    if (ligar && ligado && ++conta % 60) return;
    const std::string s = ligar ? std::string("FcInvAtivo = 1 ") + LUA_INVENCIVEL : std::string("FcInvAtivo = 0");
    const uint32_t r = LuaRodar(ctx, s);
    if (r == ~0u) return;   // Lua ainda nao pronto
    if (ligar != ligado || r) Log("[trapacas] invencivel %s (Lua %u, 0 = ok)", ligar ? "ligado" : "desligado", r);
    ligado = ligar;
}

// ---------------------------------------------------------------- poderes ferais
// Habilidades do jogador ("evolucoes": jogador+2176, 7 entradas de 8 bytes a partir de +4: +0 tem, +1 em uso, +4 recarga;
// +60 lista das que se ligam com botao (segurar Y = visoes, atletismo), +88 quantas). Ligar poe as passivas "em uso" na
// hora e as de botao na lista; marcar estas "em uso" a forca faz o botao achar que ja estao ligadas e nao ligar nada. [84] da tabela do jogador (sub_82263890) = Ligar(i, sim/nao). Numeros (Lua
// EPlayerEvolution_*): 0 visao noturna, 1 visao de calor, 2 detector de movimento, 3 atletismo (corrida e pulo),
// 4 regeneracao, 5 adrenalina, 6 ExoFist (ataque). O FeralAttack do jogo (bit 8 de G+1036) so liga a 5 e a 6, por isso
// so o ataque funcionava: aqui a trapaca liga as sete. As que a trapaca ligou (g_nossas) nao vao para o save: a tabela
// inteira e gravada no jogo salvo (sub_82250928) e elas sao tiradas durante a gravacao.
namespace
{
uint32_t g_hab = 0;       // tabela das habilidades do jogador atual
uint32_t g_nossas = 0;    // bits (1 << i) das ligadas pela trapaca
bool g_ligando = false;   // a chamada de Ligar e nossa
}

static void Habilidades(GuestCpu& ctx, uint32_t jogador, bool ligar)
{
    const uint32_t h = jogador ? R32(jogador + 2176) : 0;
    if (h != g_hab) { g_hab = h; g_nossas = 0; }
    if (!h || !ligar) return;
    for (uint32_t i = 0; i < 7; i++)
    {
        if (*G(h + 4 + 8 * i)) continue;
        GuestCpu salvo = ctx;
        g_ligando = true;
        SetGpr(ctx, 3, jogador); SetGpr(ctx, 4, i); SetGpr(ctx, 5, 1);
        GuestCall(ctx, R32(R32(jogador) + 336));
        g_ligando = false;
        ctx = salvo;
        g_nossas |= 1u << i;
        Log("[trapacas] poder feral %u ligado", i);
    }
}

// Ligar(i, sim/nao) chamado pelo jogo: a habilidade passa a ser do jogo (vai para o save normalmente)
GUEST_DECLARE_ORIGINAL(sub_82263890);
GUEST_FUNC(sub_82263890)
{
    if (!g_ligando && R32(Gpr32(ctx, 3) + 2176) == g_hab && Gpr32(ctx, 4) < 7)
        g_nossas &= ~(1u << Gpr32(ctx, 4));
    GUEST_ORIGINAL(sub_82263890)(ctx, base);
}

// gravacao da tabela das habilidades no jogo salvo (r3 = tabela, r4 = destino)
GUEST_DECLARE_ORIGINAL(sub_82250928);
GUEST_FUNC(sub_82250928)
{
    const uint32_t h = Gpr32(ctx, 3);
    if (!h || h != g_hab || !g_nossas) { GUEST_ORIGINAL(sub_82250928)(ctx, base); return; }
    uint8_t copia[120];
    memcpy(copia, G(h), sizeof copia);
    for (uint32_t i = 0; i < 7; i++)
        if (g_nossas & (1u << i)) { *G(h + 4 + 8 * i) = 0; *G(h + 5 + 8 * i) = 0; }
    // tira as nossas da lista das de botao
    uint32_t n = R32(h + 88), k = 0;
    for (uint32_t j = 0; j < n && j < 7; j++)
    {
        const uint32_t v = R32(h + 60 + 4 * j);
        if (v < 7 && (g_nossas & (1u << v))) continue;
        W32(h + 60 + 4 * k++, v);
    }
    W32(h + 88, k);
    GUEST_ORIGINAL(sub_82250928)(ctx, base);
    memcpy(G(h), copia, sizeof copia);
    Log("[trapacas] jogo salvo sem os poderes ferais da trapaca (%X)", g_nossas);
}

// ---------------------------------------------------------------- trapacas fora do jogo salvo
// O jogo copia G+1036 (bits das trapacas) para o save (sub_8220B260: r3 = G, r4 = save, +936) e, ao carregar, soma
// o do save em G+1036 (sub_8220B4A8). Os bits 1, 2 e 8 (adrenalina, municao, ferais) nao vao mais para o save e os
// que estiverem num save antigo sao ignorados. Alem disso, a cada quadro esses bits ficam iguais aos do launcher
// (sub_8226A0D8): desligar no launcher desliga de verdade, venha o bit de onde vier.
constexpr uint32_t BITS_TRAPACA = 1u | 2u | 8u;

GUEST_DECLARE_ORIGINAL(sub_8220B260);
GUEST_FUNC(sub_8220B260)
{
    const uint32_t g = Gpr32(ctx, 3);
    const uint32_t antes = g ? R32(g + 1036) : 0;
    if (g) W32(g + 1036, antes & ~BITS_TRAPACA);
    GUEST_ORIGINAL(sub_8220B260)(ctx, base);
    if (g)
    {
        W32(g + 1036, antes);
        if (antes & BITS_TRAPACA) Log("[trapacas] jogo salvo sem as trapacas (bits %X)", antes & BITS_TRAPACA);
    }
}

GUEST_DECLARE_ORIGINAL(sub_8220B4A8);
GUEST_FUNC(sub_8220B4A8)
{
    const uint32_t s = Gpr32(ctx, 4);
    if (s && (R32(s + 936) & BITS_TRAPACA))
    {
        Log("[trapacas] save com trapacas gravadas (bits %X): ignoradas", R32(s + 936) & BITS_TRAPACA);
        W32(s + 936, R32(s + 936) & ~BITS_TRAPACA);
    }
    GUEST_ORIGINAL(sub_8220B4A8)(ctx, base);
}

// leitura da tabela das habilidades do jogo salvo (r3 = tabela). Saves feitos antes desta correcao guardaram os poderes
// da trapaca: o FeralAttack do jogo dava exatamente o 5 e o 6 (adrenalina, ataque), que na historia so chegam depois das
// visoes e do atletismo. Um save com o 5 ou o 6 e nenhum de 0 a 4 so pode ter vindo da trapaca: esses dois saem.
GUEST_DECLARE_ORIGINAL(sub_82250AE8);
GUEST_FUNC(sub_82250AE8)
{
    const uint32_t h = Gpr32(ctx, 3);
    GUEST_ORIGINAL(sub_82250AE8)(ctx, base);
    if (!h) return;
    uint32_t t = 0;
    for (uint32_t i = 0; i < 7; i++) t |= (uint32_t)(*G(h + 4 + 8 * i) != 0) << i;
    if (!(t & 0x60) || (t & 0x1F)) return;
    for (uint32_t i = 5; i < 7; i++) { *G(h + 4 + 8 * i) = 0; *G(h + 5 + 8 * i) = 0; }
    uint32_t n = R32(h + 88), k = 0;
    for (uint32_t j = 0; j < n && j < 7; j++)
    {
        const uint32_t v = R32(h + 60 + 4 * j);
        if (v == 5 || v == 6) continue;
        W32(h + 60 + 4 * k++, v);
    }
    W32(h + 88, k);
    Log("[trapacas] save antigo com os poderes da trapaca (adrenalina e ataque, %X): tirados", t);
}

// atualizacao do jogador (uma vez por quadro, so o jogador de quem joga): poe os bits antes de o jogo le-los e,
// com "invencivel", enche a vida depois
GUEST_DECLARE_ORIGINAL(sub_8226A0D8);
GUEST_FUNC(sub_8226A0D8)
{
    const Ligadas l = Atuais();
    const uint32_t jogador = Gpr32(ctx, 3);
    const uint32_t g = jogador ? R32(jogador + 440) : 0;
    const uint32_t bits = (l.adrenalina ? 1u : 0u) | (l.municao ? 2u : 0u);   // o 8 (FeralAttack) e feito em Habilidades
    // exatamente os ligados no launcher: um bit que veio de outro lugar (save antigo, codigo digitado) e tirado
    if (g && (R32(g + 1036) & BITS_TRAPACA) != bits)
    {
        Log("[trapacas] bits do jogo %X -> %X", R32(g + 1036), (R32(g + 1036) & ~BITS_TRAPACA) | bits);
        W32(g + 1036, (R32(g + 1036) & ~BITS_TRAPACA) | bits);
    }
    if (jogador && g) Invencivel(ctx, l.invencivel && !Multijogador(g));
    GUEST_ORIGINAL(sub_8226A0D8)(ctx, base);
    Habilidades(ctx, jogador, l.feral && g && !Multijogador(g));
    static const bool vidaLog = getenv("FCRECOMP_VIDALOG") != nullptr;
    if (vidaLog && jogador)
    {
        const float maximo = (float)(int32_t)R32(jogador + 236), vida = std::bit_cast<float>(R32(jogador + 232));
        if (vida < maximo) Log("[trapacas] vida %.1f de %.0f", vida, maximo);
    }
    if (l.invencivel && jogador && g && !Multijogador(g))
    {
        const float maximo = (float)(int32_t)R32(jogador + 236);
        const float vida = std::bit_cast<float>(R32(jogador + 232));
        if (maximo > 0 && vida < maximo)
        {
            W32(jogador + 232, std::bit_cast<uint32_t>(maximo));
            static ULONGLONG ultimoLog = 0;   // no maximo um aviso a cada 5 s
            if (GetTickCount64() - ultimoLog > 5000) { ultimoLog = GetTickCount64(); Log("[trapacas] invencivel: vida %.0f -> %.0f", vida, maximo); }
        }
    }
}

// atualizacao do relogio do jogo (todo quadro, tambem nos menus): fases e mapas liberados no perfil carregado
GUEST_DECLARE_ORIGINAL(sub_823175F0);
GUEST_FUNC(sub_823175F0)
{
    GUEST_ORIGINAL(sub_823175F0)(ctx, base);
    const Ligadas l = Atuais();
    if (!l.fases && !l.mapas) return;
    const uint32_t gerente = R32(PERFIS);
    const uint32_t p = gerente ? R32(gerente + 20) : 0;
    if (!p || !*G(p + 1) || !R32(p + 144)) return;   // perfil ainda nao carregado (sub_821E25C0 confere o mesmo)
    if (l.fases && !(*G(p + 176) & 1))
    {
        *G(p + 176) |= 1;
        Log("[trapacas] todas as fases liberadas no perfil");
    }
    if (l.mapas)
    {
        const uint32_t m = 0x7FFu | ((*G(p + 176) & 1) ? 0x7F800u : 0u);
        if ((R32(p + 180) & m) != m)
        {
            W32(p + 180, R32(p + 180) | m);
            Log("[trapacas] todos os mapas liberados no perfil");
        }
    }
}
#endif
