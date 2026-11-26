// Teste de NPC no editor de mapas (ige.xex): roda um texto Lua dentro do editor, para experimentar criar um
// mercenario (Merc_x) perto do jogador no TESTAR MAPA. So depuracao, ligado pelo ao_vivo.txt:
//   LUA=n   (n novo a cada vez) roda build_game\lua_teste.lua uma vez, na thread do jogo (giro da camera do jogador,
//           input.cpp). O script recebe a variavel RUN (texto "n").
// Resultado: o script junta o que quer mostrar num texto global que comeca com "NPCRES#n:"; depois de rodar, a memoria
// do "Xbox" e procurada por esse texto (o maior achado vai para o registro como "[lua]").
//
// Motor de script do editor: Lua 4.1 (alpha). Sistema de script = R32(0x828C785C) (sub_82320CC8); o lua_State e o
// primeiro campo (sub_82322EE0 le +0). sub_823D1CD0(L, texto) = lua_dostring (strlen, lua_loadbuffer sub_823DD340 e
// lua_call sub_823DD008, que roda protegido por sub_823DCA30: erro no script volta como codigo, nao derruba o editor).
#ifdef PREDATOR_EDITOR
#include "guest.h"
#include <windows.h>
#include <atomic>
#include <mutex>
#include <set>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

std::string AoVivoValor(const char* chave);   // input.cpp: build_game\ao_vivo.txt

namespace
{
    constexpr uint32_t SISTEMA_SCRIPT = 0x828C785C;
    constexpr uint32_t LUA_DOSTRING = 0x823D1CD0;

    // procura "NPCRES#n:" nas partes da memoria do Xbox que existem; devolve o maior texto achado
    std::string ProcurarResultado(const std::string& marca)
    {
        std::string melhor;
        uint8_t* p = g_base;
        uint8_t* fim = g_base + 0x100000000ull;
        MEMORY_BASIC_INFORMATION mi;
        while (p < fim && VirtualQuery(p, &mi, sizeof(mi)))
        {
            uint8_t* rb = (uint8_t*)mi.BaseAddress;
            uint8_t* re = rb + mi.RegionSize;
            const bool legivel = mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                (mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
            if (legivel && re - rb > (ptrdiff_t)marca.size())
            {
                uint8_t* q = rb;
                while (q < re - marca.size())
                {
                    q = (uint8_t*)memchr(q, marca[0], re - marca.size() - q);
                    if (!q) break;
                    if (memcmp(q, marca.data(), marca.size()) == 0)
                    {
                        size_t n = 0;
                        while (q + n < re && q[n] && n < 4000) n++;
                        if (n > melhor.size()) melhor.assign((const char*)q, n);
                    }
                    q++;
                }
            }
            p = re;
        }
        return melhor;
    }
}

// input.cpp (giro da camera do jogador, thread do jogo): olha o ao_vivo.txt e roda o lua_teste.lua quando LUA muda
void EditorLuaQuadro(GuestCpu& ctx)
{
    static std::string antes;
    const std::string v = AoVivoValor("LUA");
    if (v.empty() || v == antes) return;
    antes = v;

    std::ifstream f("lua_teste.lua", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string texto = "RUN=\"" + v + "\"\n" + ss.str();
    const uint32_t sistema = R32(SISTEMA_SCRIPT);
    const uint32_t L = sistema ? R32(sistema) : 0;
    Log("[lua] LUA=%s: sistema de script %08X, lua_State %08X, script com %u bytes", v.c_str(), sistema, L, (unsigned)ss.str().size());
    if (!L || ss.str().empty() || texto.size() > 0x6000) return;

    // texto na pilha do Xbox, abaixo da pilha atual; a chamada usa a pilha abaixo dele
    const uint32_t sp = Gpr32(ctx, 1);
    const uint32_t buf = (sp - 0x200 - (uint32_t)texto.size() - 1) & ~15u;
    memcpy(G(buf), texto.c_str(), texto.size() + 1);
    GuestCpu tmp = ctx;
    SetGpr(tmp, 1, (buf - 0x200) & ~15u);
    SetGpr(tmp, 3, L);
    SetGpr(tmp, 4, buf);
    GuestCall(tmp, LUA_DOSTRING);
    const uint32_t r = Gpr32(tmp, 3);
    const std::string res = ProcurarResultado("NPCRES#" + v + ":");
    Log("[lua] lua_dostring devolveu %u; resultado: %s", r, res.empty() ? "(nao achado)" : res.c_str());
}

// ---------------------------------------------------------------- depuracao das animacoes (FCRECOMP_NPCLOG=1)
// sub_8219AA80(personagem, slot, modelo, r6, r7, carregar, .xba): monta o modelo do slot (instancia em
// personagem+(slot+174)*4); sub_8219B938(personagem, slot, nome) = o slot tem essa animacao? (virtual [57] da instancia)
namespace
{
    bool NpcLog() { static const bool l = getenv("FCRECOMP_NPCLOG") != nullptr; return l; }
    std::string TextoGuest(uint32_t a) { return a && a < 0xF0000000u ? RStr(a) : std::string("(nulo)"); }
}
GUEST_DECLARE_ORIGINAL(sub_8219AA80);
GUEST_FUNC(sub_8219AA80)
{
    const uint32_t obj = Gpr32(ctx, 3), slot = Gpr32(ctx, 4);
    std::string modelo, xba;
    if (NpcLog()) { modelo = TextoGuest(Gpr32(ctx, 5)); xba = TextoGuest(Gpr32(ctx, 9)); }
    // experimento: FCRECOMP_NPCR7=n troca o r7 da montagem dos mercenarios (o jogador usa 8, o NPC vem com 6)
    if (static const char* r7e = getenv("FCRECOMP_NPCR7"); r7e && Gpr32(ctx, 5))
    {
        std::string m = TextoGuest(Gpr32(ctx, 5));
        for (auto& c : m) c = (char)tolower((unsigned char)c);
        if (m.find("mercenaire") != std::string::npos) SetGpr(ctx, 7, (uint32_t)atoi(r7e));
    }
    // experimento: FCRECOMP_NPCXBA=caminho da o .xba aos mercenarios montados sem .xba (o motor tiraria do modelo:
    // merc_kit_03.xba, que nao existe)
    if (static const char* xe = getenv("FCRECOMP_NPCXBA"); xe && Gpr32(ctx, 5) && !Gpr32(ctx, 9))
    {
        std::string m = TextoGuest(Gpr32(ctx, 5));
        for (auto& c : m) c = (char)tolower((unsigned char)c);
        static uint32_t texto = 0;
        if (!texto) { texto = GuestAlloc((uint32_t)strlen(xe) + 1); memcpy(G(texto), xe, strlen(xe) + 1); }
        if (m.find("mercenaire") != std::string::npos) { SetGpr(ctx, 9, texto); if (NpcLog()) xba = std::string(xe) + " (forcado)"; }
    }
    const uint32_t r6 = Gpr32(ctx, 6), r7 = Gpr32(ctx, 7), r8 = Gpr32(ctx, 8);
    GUEST_ORIGINAL(sub_8219AA80)(ctx, base);
    if (NpcLog())
        Log("[npclog] monta personagem %08X slot %u modelo [%s] xba [%s] r6=%u r7=%u carregar=%u -> %u, instancia %08X", obj, slot,
            modelo.c_str(), xba.c_str(), r6, r7, r8 & 0xFF, Gpr32(ctx, 3), slot < 8 ? R32(obj + (slot + 174) * 4) : 0);
}
GUEST_DECLARE_ORIGINAL(sub_8219B938);
GUEST_FUNC(sub_8219B938)
{
    const uint32_t obj = Gpr32(ctx, 3), slot = Gpr32(ctx, 4);
    std::string nome;
    if (NpcLog()) nome = TextoGuest(Gpr32(ctx, 5));
    GUEST_ORIGINAL(sub_8219B938)(ctx, base);
    static std::mutex m;
    static std::set<std::string> vistos;
    bool novo = false;
    if (NpcLog())
    {
        char k[64];
        snprintf(k, sizeof(k), "%08X/%u/", obj, slot);
        std::lock_guard g(m);
        novo = vistos.size() < 500 && vistos.insert(k + nome).second;
    }
    if (novo)
        Log("[npclog] tem animacao? personagem %08X slot %u [%s] instancia %08X -> %u", obj, slot, nome.c_str(),
            slot < 8 ? R32(obj + (slot + 174) * 4) : 0, Gpr32(ctx, 3));
}
#endif

// ---------------------------------------------------------------- jogo (default.xex): o mesmo LUA=n do ao_vivo.txt
// Para testes (ex.: levar o jogador a um ponto da fase com _localplayer:SetPos e medir ali). Motor de script do jogo
// (achado comparando com o editor): sub_823DF778(L, texto, tamanho, nome) = lua_loadbuffer (o sub_823CB380 usa assim) e,
// se deu 0, sub_823DF1C8(L, 0, -1) = lua_call (igual ao sub_823DD008 do editor). Cuidado: sub_823DF418(L, nome) e o
// dofile (texto vira nome de arquivo; o arquivo nao existe e o jogo reinicia pelo aviso de disco sujo).
// O lua_State e pego no proprio lua_call (o motor chama funcoes Lua a todo quadro numa fase).
#ifndef PREDATOR_EDITOR
#include "guest.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

std::string AoVivoValor(const char* chave);   // input.cpp: build_game\ao_vivo.txt

namespace
{
    constexpr uint32_t LUA_CARREGA = 0x823DF778, LUA_CALL = 0x823DF1C8;
    std::atomic<uint32_t> g_luaState{ 0 };

    bool RegiaoLegivel(const MEMORY_BASIC_INFORMATION& mi)
    {
        return mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
    }

    // onde a ultima busca achou a marca (o monte do Lua: o texto novo costuma nascer no mesmo lugar) e a regiao dela.
    // A regiao fica guardada: o VirtualQuery nesse bloco gigante da memoria do jogo leva ~15 ms (era a travada)
    std::atomic<uint8_t*> g_ultimaAchada{ nullptr }, g_regiaoIni{ nullptr }, g_regiaoFim{ nullptr };

    // procura a marca em [rb, re); guarda em melhor o texto mais longo achado; true = achou
    bool ProcurarNaRegiao(uint8_t* rb, uint8_t* re, const std::string& marca, std::string& melhor)
    {
        bool achou = false;
        for (uint8_t* q = rb; re - rb > (ptrdiff_t)marca.size() && q < re - marca.size(); q++)
        {
            q = (uint8_t*)memchr(q, marca[0], re - marca.size() - q);
            if (!q) break;
            if (memcmp(q, marca.data(), marca.size()) == 0)
            {
                size_t n = 0;
                while (q + n < re && q[n] && n < 4000) n++;
                if (n > melhor.size()) melhor.assign((const char*)q, n);
                achou = true;
                g_ultimaAchada = q;
                g_regiaoIni = rb;
                g_regiaoFim = re;
            }
        }
        return achou;
    }

    std::string ProcurarResultadoJogo(const std::string& marca)
    {
        std::string melhor;
        uint8_t* p = g_base;
        uint8_t* fim = g_base + 0x100000000ull;
        MEMORY_BASIC_INFORMATION mi;
        while (p < fim && VirtualQuery(p, &mi, sizeof(mi)))
        {
            uint8_t* rb = (uint8_t*)mi.BaseAddress;
            uint8_t* re = rb + mi.RegionSize;
            if (RegiaoLegivel(mi)) ProcurarNaRegiao(rb, re, marca, melhor);
            p = re;
        }
        return melhor;
    }

    // ProcurarNaRegiao protegida (a regiao guardada pode ter deixado de existir): false = nao achou ou nao deu para ler
    bool ProcurarNaRegiaoSeguro(uint8_t* a, uint8_t* b, const std::string* marca, std::string* melhor)
    {
        __try { return ProcurarNaRegiao(a, b, *marca, *melhor); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // so para marca unica (com um numero novo a cada vez): olha 256 KB e depois 8 MB para cada lado de onde achou a ultima
    // vez (centesimos de ms), sem VirtualQuery; so varre a memoria toda do jogo (70 a 110 ms: uma travada) se nao achar
    std::string ProcurarResultadoRapido(const std::string& marca)
    {
        uint8_t* u = g_ultimaAchada.load();
        uint8_t* rb = g_regiaoIni.load();
        uint8_t* re = g_regiaoFim.load();
        // 256 KB, 8 MB e por fim a regiao toda (sem VirtualQuery: o da varredura completa parece segurar as alocacoes do
        // jogo enquanto roda, uma travada mesmo com a busca fora da thread do jogo)
        if (u && rb && re && u >= rb && u < re)
            for (size_t vizinho : { (size_t)256 << 10, (size_t)8 << 20, (size_t)re - (size_t)rb })
            {
                uint8_t* a = u - rb > (ptrdiff_t)vizinho ? u - vizinho : rb;
                uint8_t* b = re - u > (ptrdiff_t)vizinho ? u + vizinho : re;
                std::string melhor;
                if (ProcurarNaRegiaoSeguro(a, b, &marca, &melhor) && !melhor.empty()) return melhor;
            }
        return ProcurarResultadoJogo(marca);
    }
}

GUEST_DECLARE_ORIGINAL(sub_823DF1C8);
GUEST_FUNC(sub_823DF1C8)
{
    g_luaState = Gpr32(ctx, 3);
    GUEST_ORIGINAL(sub_823DF1C8)(ctx, base);
}

uint32_t LuaRodar(GuestCpu& ctx, const std::string& texto);

// texto que o Lua deixou na memoria do jogo comecando com 'marca' (o mais longo; vazio = nao achado). Tambem
// usado por cena_direta.cpp. A marca nao pode aparecer inteira no texto do script (monte-a com ..).
std::string LuaProcurarResultado(const std::string& marca) { return ProcurarResultadoJogo(marca); }
// igual, para marca unica: procura primeiro perto de onde achou a ultima vez (save_state.cpp, numa thread a parte)
std::string LuaProcurarResultadoRapido(const std::string& marca) { return ProcurarResultadoRapido(marca); }
void EditorLuaQuadro(GuestCpu& ctx)
{
    static std::string antes;
    const std::string v = AoVivoValor("LUA");
    if (v.empty() || v == antes) return;
    antes = v;
    std::ifstream f("lua_teste.lua", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string texto = "RUN=\"" + v + "\"\n" + ss.str();
    Log("[lua] LUA=%s: lua_State %08X, script com %u bytes", v.c_str(), (uint32_t)g_luaState, (unsigned)ss.str().size());
    if (ss.str().empty()) return;
    const uint32_t r = LuaRodar(ctx, texto);
    const std::string res = ProcurarResultadoJogo("NPCRES#" + v + ":");
    Log("[lua] devolveu %u (0 = ok); resultado: %s", r, res.empty() ? "(nao achado)" : res.c_str());
}

// roda um trecho Lua no estado do jogo, abaixo da pilha atual de ctx (tambem usado por cena_direta.cpp, FCRECOMP_CENA);
// devolve 0 = ok, ~0u = ainda sem lua_State (ou texto grande demais), outro = erro do lua_loadbuffer/lua_call
uint32_t LuaRodar(GuestCpu& ctx, const std::string& texto)
{
    const uint32_t L = g_luaState;
    if (!L || texto.empty() || texto.size() > 0x6000) return ~0u;
    const uint32_t sp = Gpr32(ctx, 1);
    const uint32_t buf = (sp - 0x200 - (uint32_t)texto.size() - 1) & ~15u;
    memcpy(G(buf), texto.c_str(), texto.size() + 1);
    // roda no proprio contexto da thread (restaurado no fim): um travamento la dentro aparece no relatorio com a pilha
    // de dentro; o quadro novo aponta para o atual (elo da pilha), para a pilha poder ser seguida
    const GuestCpu salvo = ctx;
    const uint32_t novo = (buf - 0x200) & ~15u;
    W32(novo, sp);
    SetGpr(ctx, 1, novo);
    SetGpr(ctx, 3, L);
    SetGpr(ctx, 4, buf);
    SetGpr(ctx, 5, (uint32_t)texto.size());
    SetGpr(ctx, 6, buf + (uint32_t)texto.size());   // nome do trecho: texto vazio (o 0 do fim)
    GuestCall(ctx, LUA_CARREGA);
    uint32_t r = Gpr32(ctx, 3);
    if (r == 0)
    {
        ctx = salvo;
        SetGpr(ctx, 1, novo);
        SetGpr(ctx, 3, L); SetGpr(ctx, 4, 0); SetGpr(ctx, 5, (uint32_t)-1);
        GuestCall(ctx, LUA_CALL);
        r = Gpr32(ctx, 3);
    }
    ctx = salvo;
    return r;
}

bool LuaPronto() { return g_luaState != 0; }
#endif
