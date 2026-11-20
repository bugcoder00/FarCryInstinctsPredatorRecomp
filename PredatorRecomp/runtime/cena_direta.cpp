// Ver uma cutscene sem jogar ate ela (ferramenta tools\cutscenes.ps1). Dois tipos de cena no jogo:
//
// 1) Videos Bink (pasta videos: Ubisoft, Intro_FCI, Attract_Mode, fundos do menu). Quem toca e o proprio codigo do
//    jogo (o Bink recompilado; gpu.cpp so mostra os planos Y/Cb/Cr). FCRECOMP_VIDEO=<nome>.bik: o primeiro video que o
//    jogo abre (o da Ubisoft, na abertura) vira o escolhido (fs.cpp chama CenaDiretaVideo). Depois o jogo segue.
//
// 2) Cenas feitas no motor (fases da historia). Estao no XML da missao de cada fase (pacote levels\<FASE>): uma ancora
//    de IA (AIAnchor, trabalho generico) faz a cena - SendSignal GUI_BeginCutScene ao MissionHandler (que chama
//    Movie:SendCommandCarver("BeginCutScene"), veja pular_cena.cpp), PlayAnimation do Carver, falas, filmes
//    (PlaySequence) e GUI_EndCutScene. Quem solta a ancora e um gatilho (ProximityTrigger: o jogador entra na area e o
//    gatilho manda SwitchToGenericJob <ancora> ao personagem) ou a fase ao carregar. Lista: tools\cutscenes.txt
//    (tools\gerar_cutscenes.py).
//    FCRECOMP_CENA=<fase>[:<gatilho>[:<evento>]] (ex. 21_dunea:ProximityTrigger44):
//      - aperta START sozinho (pula os videos) e, com o perfil carregado, manda ao console do jogo o mesmo comando que o menu usa para abrir uma fase
//        ("StartLevel level=<fase> mission=Mission0") - sem passar pelos menus;
//      - na fase, alguns segundos depois de o jogador aparecer, leva o jogador ao gatilho e o dispara por Lua
//        (liga o gatilho, zera "ja disparou" e chama Event_Enter com o jogador; outro evento = BroadcastEvent).
//      Sem gatilho: so carrega a fase (cenas que comecam com ela). FCRECOMP_CENA_LEVAR=0 nao leva o jogador.
//      FCRECOMP_CENA_ESPERA=ms: espera na fase antes de disparar (padrao 4000).
#ifndef PREDATOR_EDITOR
#include "guest.h"
#include "perf.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

uint32_t LuaRodar(GuestCpu& ctx, const std::string& texto);   // npc_editor.cpp

namespace
{
    struct Pedido { std::string fase, gatilho, evento; };

    const Pedido& Cena()
    {
        static const Pedido p = [] {
            Pedido r;
            const char* e = getenv("FCRECOMP_CENA");
            if (!e || !*e) return r;
            std::string s = e;
            auto corta = [&s]() { const size_t i = s.find(':'); std::string a = s.substr(0, i); s = i == std::string::npos ? "" : s.substr(i + 1); return a; };
            r.fase = corta();
            r.gatilho = corta();
            r.evento = corta();
            if (r.evento.empty()) r.evento = "Enter";
            return r;
        }();
        return p;
    }

    // so letras, numeros e - _ . (entra num texto Lua entre aspas)
    bool NomeSeguro(const std::string& s)
    {
        for (char c : s)
            if (!isalnum((unsigned char)c) && c != '_' && c != '-' && c != '.') return false;
        return true;
    }

    enum class Etapa { Nada, Carregando, NaFase, Feito };
    Etapa g_etapa = Etapa::Nada;
    double g_faseVista = 0;
}

// Perfil do jogador (o mesmo de trapacas.cpp): R32(R32(0x8293A7A4) + 20). So existe depois do START na tela inicial; o
// StartLevel sem ele travava (leitura de perfil+976 em sub_8220F1xx, as conquistas da fase).
double g_perfilVisto = 0;
bool PerfilPronto()
{
    const uint32_t gerente = R32(0x8293A7A4);
    const bool ok = gerente && R32(gerente + 20);
    if (ok && g_perfilVisto == 0) g_perfilVisto = perf::Now();
    return ok;
}

// input.cpp, a cada leitura do controle do jogador 1: com FCRECOMP_CENA, aperta START (0,3 s a cada 1,5 s) ate o jogo
// entrar com o perfil - pula os videos da abertura e a tela "aperte START", como quem joga faria
bool CenaDiretaApertarStart()
{
    if (Cena().fase.empty() || g_etapa != Etapa::Nada || PerfilPronto()) return false;
    return fmod(perf::Now() / 1000.0, 1.5) < 0.3;
}

// fs.cpp, a cada video Bink aberto: nome do video a abrir no lugar de 'nome' (vazio = o mesmo). So o primeiro video do
// jogo e trocado.
std::string CenaDiretaVideo(const std::string& nome)
{
    static bool trocado = false;
    const char* e = getenv("FCRECOMP_VIDEO");
    if (trocado || !e || !*e) return {};
    trocado = true;
    Log("[cena] video %s trocado por %s (FCRECOMP_VIDEO)", nome.c_str(), e);
    return e;
}

// Como abrir a fase sem menu: o menu manda ao console do jogo "StartLevel level=<fase> mission=<missao> checkpoint=..."
// (sub_820EA4F8: r3 = console R32(0x8291234C), r4 = texto). O laco principal (sub_8230B9A8) faz o mesmo com a linha de
// comando de desenvolvedor "-map <fase>", logo antes de rodar o quadro do menu (sub_8220D008) - mas na versao final o
// "-map" trava a montagem do jogo (sub_8220BFA0 le o objeto do menu, que fica 1) e manda mission=<fase>, que nao
// existe. Aqui: o mesmo comando, no mesmo lugar (comeco do sub_8220D008, thread principal), 1 s depois de o jogo
// entrar com o perfil (START apertado sozinho, CenaDiretaApertarStart), com mission=Mission0 (todas as fases da historia tem so essa missao, no XML da missao).
// (Game:LoadLevel por Lua travava: le objetos que so existem quando a fase e aberta pelo StartLevel.)
GUEST_DECLARE_ORIGINAL(sub_8220D008);
GUEST_FUNC(sub_8220D008)
{
    const Pedido& p = Cena();
    if (g_etapa == Etapa::Nada && !p.fase.empty() && PerfilPronto())
    if (g_etapa == Etapa::Nada && !p.fase.empty() && PerfilPronto() && perf::Now() - g_perfilVisto > 1000)
    {
        if (!NomeSeguro(p.fase) || !NomeSeguro(p.gatilho) || !NomeSeguro(p.evento))
        {
            Log("[cena] FCRECOMP_CENA com caracteres invalidos: ignorado");
            g_etapa = Etapa::Feito;
        }
        else if (const uint32_t console = R32(0x8291234C))
        {
            static uint32_t buf = 0;
            const std::string cmd = "StartLevel level=" + p.fase + " mission=Mission0";
            if (!buf) buf = GuestAlloc(256);
            if (buf && cmd.size() < 256)
            {
                memcpy(G(buf), cmd.c_str(), cmd.size() + 1);
                // jogo salvo em uso (gerente R32(0x8293A79C), +48): o menu o cria em NOVO JOGO / CARREGAR; o StartLevel
                // usa o +348 dele (sub_8222A4C0: memoria do checkpoint: duas copias de 0x2E7000 bytes, a partir de +45 e de +0x2E7000+45) e travava com
                // ele nulo. Para so ver a cena, um vazio com essa memoria (a ferramenta usa uma pasta de saves separada,
                // FCRECOMP_SAVEDIR).
                if (const uint32_t ger = R32(0x8293A79C); ger && !R32(ger + 48))
                {
                    const uint32_t vazio = GuestAlloc(0x400), mem = GuestAlloc(2 * 0x2E7000 + 0x1000);
                    if (vazio && mem)
                    {
                        memset(G(vazio), 0, 0x400);
                        memset(G(mem), 0, 2 * 0x2E7000 + 0x1000);
                        W32(vazio + 348, mem);
                        W32(ger + 48, vazio);
                        Log("[cena] jogo salvo vazio criado em %08X (memoria do checkpoint %08X)", vazio, mem);
                    }
                }
                g_etapa = Etapa::Carregando;
                Log("[cena] comando do jogo: %s", cmd.c_str());
                const GuestCpu salvo = ctx;
                SetGpr(ctx, 3, console);
                SetGpr(ctx, 4, buf);
                GuestCall(ctx, 0x820EA4F8);
                ctx = salvo;
            }
        }
    }
    GUEST_ORIGINAL(sub_8220D008)(ctx, base);
}

// input.cpp, atualizacao da mira do jogador (so numa fase, a cada quadro): dispara o gatilho da cena
void CenaDiretaFase(GuestCpu& ctx)
{
    const Pedido& p = Cena();
    if (g_etapa != Etapa::Carregando && g_etapa != Etapa::NaFase) return;
    if (g_etapa == Etapa::Carregando)
    {
        g_etapa = Etapa::NaFase;
        g_faseVista = perf::Now();
        Log("[cena] jogador na fase %s", p.fase.c_str());
        if (p.gatilho.empty()) { g_etapa = Etapa::Feito; Log("[cena] sem gatilho: a cena comeca com a fase"); }
        return;
    }
    static const double espera = [] { const char* e = getenv("FCRECOMP_CENA_ESPERA"); return e ? atof(e) : 4000.0; }();
    if (perf::Now() - g_faseVista < espera) return;
    static const bool levar = [] { const char* e = getenv("FCRECOMP_CENA_LEVAR"); return !e || atoi(e) != 0; }();
    // Lua 4 do motor (lua_call protegido: um erro so volta um codigo)
    std::string s;
    s += "local t = System:GetEntityByName(\"" + p.gatilho + "\")\n";
    s += "if t then\n";
    if (levar) s += "  local q = t:GetPos()\n  _localplayer:SetPos({x = q.x, y = q.y, z = q.z + 1})\n";
    s += "  if t.Properties then t.Properties.bEnabled = 1 end\n";
    s += "  t.bEnabled = 1  t.Entered = 0  t.bTriggered = 0  t.bLocked = 0\n";
    if (p.evento == "Enter") s += "  if t.Event_Enter then t:Event_Enter(_localplayer) else BroadcastEvent(t, \"Enter\") end\n";
    else s += "  BroadcastEvent(t, \"" + p.evento + "\")\n";
    s += "end\n";
    const uint32_t r = LuaRodar(ctx, s);
    Log("[cena] gatilho %s (%s) disparado -> %u (0 = ok)", p.gatilho.c_str(), p.evento.c_str(), r);
    if (r != ~0u) g_etapa = Etapa::Feito;
}
#endif
