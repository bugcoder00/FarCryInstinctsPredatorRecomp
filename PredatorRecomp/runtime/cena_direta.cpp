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
//      - nenhum video abre (CenaDiretaVideo) e a imagem fica preta ate o comando da fase (CenaDiretaTelaPreta): quem
//        olha ve o inicio do jogo e depois ja a tela de carregamento (01_beach: carregamento aos 12 s, cena aos 20 s);
//      - aperta START sozinho (tela "aperte START") e, com o perfil carregado, manda ao console do jogo o mesmo comando que o menu usa para abrir uma fase
//        ("StartLevel level=<fase> mission=Mission0") - sem passar pelos menus;
//      - na fase, alguns segundos depois de o jogador aparecer, leva o jogador ao gatilho e o dispara por Lua
//        (liga o gatilho, zera "ja disparou" e chama Event_Enter com o jogador; outro evento = BroadcastEvent).
//      - se a fase ja comeca com uma cena: com o jogador nascido a menos de 20 m do gatilho e a propria cena pedida
//        (nao dispara de novo); senao espera essa abertura acabar (EndCutScene) e dispara depois.
//      Testado (24/11/2026): 01_beach:SpawnTrigger, 22_duneb:1-1-5_ProxTrigg, video Intro_FCI. Nao funciona quando o
//      jogador esta num veiculo guiado pela fase (21_dunea depois da abertura: o jipe da Valerie; 2_1_1_dummy).
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
bool CenaDoScriptTocando();                                   // pular_cena.cpp
std::string LuaProcurarResultado(const std::string& marca);  // npc_editor.cpp

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
    bool g_nasceuNoGatilho = false;   // o jogador apareceu perto do gatilho pedido (a cena dele comeca sozinha)
    bool g_esperandoAbertura = false; // outra cena comecou com a fase: espera ela acabar
    double g_aberturaAcabou = 0;
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
    return fmod(perf::Now() / 1000.0, 0.8) < 0.3;
}

// pular_cena.cpp (BrilhoTela): com FCRECOMP_CENA a imagem fica preta ate o comando que abre a fase - a tela "aperte
// START" e o menu, que passam sozinhos, nao aparecem; a proxima coisa vista e a tela de carregamento da fase
bool CenaDiretaTelaPreta()
{
    return !Cena().fase.empty() && g_etapa == Etapa::Nada;
}

// fs.cpp, a cada video Bink aberto: nome do video a abrir no lugar de 'nome' (vazio = o mesmo). So o primeiro video do
// jogo e trocado.
std::string CenaDiretaVideo(const std::string& nome)
{
    // FCRECOMP_CENA: nenhum video (abertura da Ubisoft, intro, fundos do menu) - o arquivo pedido nao existe e o jogo
    // segue para a proxima tela sem ele
    if (!Cena().fase.empty())
    {
        static bool avisado = false;
        if (!avisado) { avisado = true; Log("[cena] videos desligados (FCRECOMP_CENA): %s e os outros nao abrem", nome.c_str()); }
        return "sem_video_FCRECOMP_CENA.bik";
    }
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
void SaveStateQuadroMenu(GuestCpu& ctx);   // save_state.cpp: carregar o estado com o jogador morto
GUEST_DECLARE_ORIGINAL(sub_8220D008);
GUEST_FUNC(sub_8220D008)
{
    SaveStateQuadroMenu(ctx);
    const Pedido& p = Cena();
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
                    // +12: outro bloco; o byte +8 dele = 1 faz o jogo pular a parte que grava/le o save de verdade
                    // (sub_820F5088 e sub_821E1340 olham esse byte; com o bloco nulo ou 0 travavam)
                    // +0: tabela de metodos (o jogo chama o +12 dele ao abrir a fase, sub_8222B218): todos "nao faz nada,
                    // devolve 0" (sub_82600948 = li r3,0; blr)
                    const uint32_t vazio = GuestAlloc(0x400), info = GuestAlloc(0x400), mem = GuestAlloc(2 * 0x2E7000 + 0x1000),
                                   metodos = GuestAlloc(64 * 4);
                    if (vazio && info && mem && metodos)
                    {
                        for (int i = 0; i < 64; i++) W32(metodos + i * 4, 0x82600948);
                        memset(G(vazio), 0, 0x400);
                        memset(G(info), 0, 0x400);
                        memset(G(mem), 0, 2 * 0x2E7000 + 0x1000);
                        W32(vazio + 0, metodos);
                        W32(vazio + 12, info);
                        *(uint8_t*)G(info + 8) = 1;
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
        if (p.gatilho.empty()) { g_etapa = Etapa::Feito; Log("[cena] sem gatilho: a cena comeca com a fase"); return; }
        // onde o jogador apareceu: perto do gatilho (menos de 20 m) = a cena dele e a que comeca com a fase
        // (01_beach: SpawnTrigger; 21_dunea: ProximityTrigger44). O Lua deixa a distancia num texto
        // "CENADIST#<m>#" (montado com .., para a marca nao estar inteira no script) que e procurado na memoria.
        std::string s;
        s += "local t = System:GetEntityByName(\"" + p.gatilho + "\")\n";
        s += "local r = \"semgatilho\"\n";
        // (GetPos devolve sempre a mesma tabela: copiar os numeros antes de pedir a outra posicao)
        s += "if t then local a = t:GetPos() local ax, ay, az = a.x, a.y, a.z\n";
        s += "  local b = _localplayer:GetPos()\n";
        s += "  local x, y, z = ax - b.x, ay - b.y, az - b.z\n";
        s += "  r = format(\"%d\", sqrt(x * x + y * y + z * z))\n";
        s += "end\n";
        s += "CENA_DISTANCIA = \"CENA\" .. \"DIST#\" .. r .. \"#\"\n";
        const uint32_t r = LuaRodar(ctx, s);
        const std::string res = LuaProcurarResultado("CENADIST#");
        const std::string dist = res.size() > 9 ? res.substr(9, res.find('#', 9) - 9) : "";
        g_nasceuNoGatilho = !dist.empty() && isdigit((unsigned char)dist[0]) && atoi(dist.c_str()) < 20;
        Log("[cena] distancia do jogador ao gatilho %s: %s m (lua %u) -> %s", p.gatilho.c_str(), dist.empty() ? "?" : dist.c_str(), r,
            g_nasceuNoGatilho ? "a cena dele comeca com a fase" : "dispara depois");
        return;
    }
    static const double espera = [] { const char* e = getenv("FCRECOMP_CENA_ESPERA"); return e ? atof(e) : 4000.0; }();
    if (perf::Now() - g_faseVista < espera) return;
    // Uma cena ja tocando (comecou com a fase):
    //  - o jogador nasceu no gatilho pedido: e a propria cena pedida (01_beach) - disparar de novo a encerrava;
    //  - senao e a abertura da fase (21_dunea: Valerie no jipe): espera ela acabar e dispara depois.
    if (CenaDoScriptTocando())
    {
        if (g_nasceuNoGatilho)
        {
            Log("[cena] uma cena ja esta tocando (comecou sozinha com a fase): o gatilho %s nao e disparado", p.gatilho.c_str());
            g_etapa = Etapa::Feito;
            return;
        }
        if (!g_esperandoAbertura) Log("[cena] a fase comecou com outra cena: esperando ela acabar para disparar %s", p.gatilho.c_str());
        g_esperandoAbertura = true;
        g_aberturaAcabou = 0;
        return;
    }
    if (g_esperandoAbertura)   // 2 s depois do fim da abertura (o jogo devolve o controle ao jogador)
    {
        if (g_aberturaAcabou == 0) g_aberturaAcabou = perf::Now();
        if (perf::Now() - g_aberturaAcabou < 2000) return;
    }
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
