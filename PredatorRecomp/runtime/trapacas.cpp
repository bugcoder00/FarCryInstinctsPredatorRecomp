// Trapacas ligadas no launcher (aba TRAPACAS, opcoes.ini "trapaca_*"), aplicadas sem digitar o codigo no menu do jogo.
//
// Codigos do jogo (sub_821AA568 = texto digitado na caixinha do MENU DE TRAPACAS do menu principal, compara com sub_821AA960):
//   UnleashHell  -> bits de G+1036 |= 2   (municao infinita)
//   Bloodlust    -> |= 1                  (adrenalina infinita)
//   FeralAttack  -> |= 8                  (todos os poderes ferais)
//   ImJackCarver -> vida do jogador = vida maxima, uma vez (sub_82240588: float +232 = (float)int +236)
//   GiveMeItAll  -> perfil +176 |= 1      (todas as fases)
//   GiveMeTheMaps-> perfil +180 |= 0x7FF (e 0x7F800 com o GiveMeItAll)   (todos os mapas)
// G = R32(0x8291234C) (lis -32111 + 9036). Perfil = R32(R32(0x8293A7A4) + 20) (lis -32108 - 22620).
// Os bits de G+1036 sao lidos a cada quadro na atualizacao do jogador (sub_8226A0D8, r3 = jogador, jogador+440 = G),
// que so os usa fora do multijogador (sub_82212C30: G+858 = partida em rede, G+108 > 1 = mais de um jogador).
// O jogo zera G+1036 em jogo novo e grava/soma os bits no save (sub_8220B308/B580); por isso os ligados aqui sao
// postos de novo a cada quadro. Desligar no launcher nao apaga o que ja foi gravado num save com o codigo.
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

// atualizacao do jogador (uma vez por quadro, so o jogador de quem joga): poe os bits antes de o jogo le-los e,
// com "invencivel", enche a vida depois
GUEST_DECLARE_ORIGINAL(sub_8226A0D8);
GUEST_FUNC(sub_8226A0D8)
{
    const Ligadas l = Atuais();
    const uint32_t jogador = Gpr32(ctx, 3);
    const uint32_t g = jogador ? R32(jogador + 440) : 0;
    const uint32_t bits = (l.adrenalina ? 1u : 0u) | (l.municao ? 2u : 0u) | (l.feral ? 8u : 0u);
    if (g && bits && (R32(g + 1036) & bits) != bits)
    {
        W32(g + 1036, R32(g + 1036) | bits);
        Log("[trapacas] bits do jogo agora %X", R32(g + 1036));
    }
    if (jogador && g) Invencivel(ctx, l.invencivel && !Multijogador(g));
    GUEST_ORIGINAL(sub_8226A0D8)(ctx, base);
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
