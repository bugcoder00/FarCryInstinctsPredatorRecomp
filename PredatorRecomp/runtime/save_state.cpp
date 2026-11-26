// Save state: salvar e carregar o estado do jogo a qualquer momento por tecla (padrao F5 salva, F9 carrega o mais novo;
// aba SAVE STATE do launcher e botao SAVE STATE da janela F2, que tem a lista dos estados salvos).
//
// Por que pelo checkpoint do proprio jogo e nao uma "foto" da maquina inteira: neste recompilador as threads do jogo sao
// threads do PC (thread.cpp), com pilhas do PC cheias de enderecos de retorno do codigo traduzido, alem de objetos do
// Windows (eventos, arquivos abertos), recursos da placa de video (D3D12) e vozes de audio. Copiar a memoria do Xbox
// (como o F11 faz) nao basta para voltar a um ponto: as pilhas e os registradores de cada thread do PC e tudo que esta
// fora da memoria do jogo teriam de voltar junto, e isso nao da para restaurar com seguranca.
// O jogo, porem, tem o seu sistema de checkpoint pronto para "voltar no tempo" dentro da fase, exposto ao Lua dos roteiros
// (tabela Game, 0x82046A10): Game:SaveCheckpoint("nome") serializa o estado da fase (jogador, vida, inimigos, objetos) na
// memoria do checkpoint do jogo salvo em uso e Game:LoadCheckpoint("nome") volta a ele sem tela de carregamento (os
// comandos de console save_game/load_game fazem o mesmo). Medido (26/11, roteiro 2026-11-26_13-03, 01_beach):
//  - SaveCheckpoint() sem nome nao guarda nada (carregar voltava sempre ao checkpoint do jogo; o 1o teste enganou, o
//    jogador estava parado justamente no ponto do checkpoint);
//  - o nome e ignorado: ha UM checkpoint so (salvar "s1" em A, "s2" em B, carregar "s1" -> B). Nao grava no disco.
// A memoria do checkpoint fica no jogo salvo em uso (gerente R32(0x8293A79C) +48 -> +348): sub_8222A4C0 abre a copia 0 em
// +45 e a copia 1 em +0x2E7000+45, 0x2E7000 bytes cada. Para ter uma LISTA de estados, o save state guarda uma copia
// desses 45 + 2 x 0x2E7000 bytes (~6 MB) a cada salvar e a devolve antes do LoadCheckpoint (ate MAX_ESTADOS, os mais
// velhos saem). A lista e da fase (g_LevelName, ex. 01_beach) e fica no disco, um arquivo .fcss por estado (cabecalho +
// a copia; ~3 MB) na pasta savestates\<fase> da pasta do perfil. Ao entrar numa fase a lista dela e lida de novo (voltar ao menu, REINICIAR FASE
// ou fechar o jogo nao a apagam); o launcher mostra os estados do perfil na aba SAVE STATE (SaveStatesDoPerfil).
// Os pedidos sao feitos no comeco da atualizacao da mira do jogador (input.cpp GUEST_SUB_MIRA: so roda numa fase, na
// thread principal do jogo, um ponto seguro como o do FCRECOMP_CENA); com o jogador morto (tela "CARREGAR PONTO SALVO")
// o carregar vem do quadro do menu (cena_direta.cpp). A resposta de cada trecho Lua e procurada numa thread a parte
// (LuaPedir/ThreadBusca): procurada na thread do jogo ela travava 70 a 400 ms (queda de FPS ao salvar/carregar).
// O estado salvo tambem vira o "ultimo checkpoint" do jogo: morrer depois de salvar e escolher CARREGAR PONTO SALVO volta
// para ele. Sem estado na lista, carregar volta ao ultimo checkpoint do jogo. Recusado no multijogador, e salvar numa
// cena do roteiro ou morto. O save do jogo (pasta conteudo) nao muda.
// Testes: ao_vivo.txt SAVESTATE=salvar1 / carregar2 / carregar3:2 (o 2o mais novo) - cada valor novo e um pedido. No modo
// de teste (FCRECOMP_*) as teclas ficam com as funcoes de diagnostico (F5..F9 do diag.cpp), a nao ser com
// FCRECOMP_SAVESTATE=1.
#ifndef PREDATOR_EDITOR
#include "guest.h"
#include "idioma.h"
#include "opcoes.h"
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>

extern std::filesystem::path g_saveDir;                               // fs.cpp: pasta do perfil em uso
uint32_t LuaRodar(GuestCpu& ctx, const std::string& texto);           // npc_editor.cpp
std::string LuaProcurarResultadoRapido(const std::string& marca);     // npc_editor.cpp
bool LuaPronto();                                                     // npc_editor.cpp
bool CenaDoScriptTocando();                                           // pular_cena.cpp
std::string AoVivoValor(const char* chave);                           // input.cpp: build_game\ao_vivo.txt
bool InputTeclaEmAcao(int vk);                                        // input.cpp: tecla usada por uma acao do jogo

namespace
{
    enum Pedido { NADA = 0, SALVAR = 1, CARREGAR = 2 };
    std::atomic<int> g_pedido{ NADA };
    std::atomic<uint32_t> g_pedidoId{ 0 };   // estado a carregar (0 = o mais novo)
    std::atomic<ULONGLONG> g_ultimo{ 0 };    // ultimo salvar/carregar feito (evita repetir com a tecla segurada)
    uint32_t g_conta = 0;                     // numero do pedido no texto que o Lua deixa (resultado de um pedido antigo nao vale)

    #define NOME_CHECKPOINT "fcrecomp_savestate"
    constexpr size_t MAX_ESTADOS = 10;
    constexpr uint32_t TAM_COPIA = 0x2E7000;
    constexpr uint32_t TAM_CHECKPOINT = 45 + 2 * TAM_COPIA;

    // posicao a repor depois do carregar (o carregar do jogo termina nos quadros seguintes e poe o jogador no ponto do
    // checkpoint; aqui ele volta ao ponto exato salvo, com z + 0,3 m para pousar no chao em vez de atravessar)
    struct Repor { bool ativo = false; int quadros = 0; float x, y, z, ax, ay, az; };
    Repor g_repor;

    // o SaveCheckpoint so marca o pedido: o jogo grava a memoria do checkpoint nos quadros seguintes (medido: logo depois
    // do Lua nenhuma das duas copias tinha mudado, e a copia tirada ali era a do salvar anterior). A copia do estado e
    // tirada quando a memoria muda (QUADROS_DEPOIS quadros depois da 1a mudanca, para a gravacao terminar).
    struct Captura { bool ativo = false; uint32_t id = 0, mem = 0; int quadros = 0, mudouEm = 0; std::vector<uint8_t> antes; };
    Captura g_captura;
    constexpr int QUADROS_DEPOIS = 2, QUADROS_MAX = 150;

    struct Estado
    {
        uint32_t id = 0;
        std::vector<uint8_t> memoria;   // copia da memoria do checkpoint (cabecalho de 45 bytes + uma copia)
        bool aguardando = false;        // recem-salvo: o jogo ainda grava a memoria (a do jogo ja e a dele)
        SYSTEMTIME hora{};
        float x = 0, y = 0, z = 0;      // posicao e angulos do jogador ao salvar (postos de novo depois do carregar)
        float ax = 0, ay = 0, az = 0;
        std::filesystem::path arquivo;  // no disco (savestates\<fase>\ na pasta do perfil)
    };
    std::mutex g_estadosMutex;
    std::deque<Estado> g_estados;        // o mais novo primeiro; so os da fase g_fase
    std::string g_fase;                  // fase da lista (g_LevelName do jogo, ex. 01_beach); com g_estadosMutex
    unsigned g_geracao = 0;              // muda a cada troca de fase (leitura do disco de uma fase antiga nao vale)
    uint32_t g_proximoId = 1;
    std::atomic<unsigned> g_versao{ 0 };  // muda a cada mudanca da lista (a janela redesenha)

    std::mutex g_avisoMutex;
    std::wstring g_aviso;
    ULONGLONG g_avisoDesde = 0;

    void Avisar(const std::wstring& t)
    {
        std::lock_guard lock(g_avisoMutex);
        g_aviso = t;
        g_avisoDesde = GetTickCount64();
    }

    bool TeclasLigadas()
    {
        const Opcoes& o = OpcoesAtuais();
        if (o.modoTeste) { static const bool env = [] { const char* e = getenv("FCRECOMP_SAVESTATE"); return e && atoi(e) != 0; }(); return env; }
        return o.saveState;
    }

    bool Multijogador()
    {
        const uint32_t g = R32(0x8291234C);
        return g && (*G(g + 858) != 0 || R32(g + 108) > 1);
    }

    // memoria do checkpoint do jogo salvo em uso (0 = nao ha)
    uint32_t MemoriaCheckpoint()
    {
        const uint32_t ger = R32(0x8293A79C);
        const uint32_t salvo = ger ? R32(ger + 48) : 0;
        return salvo ? R32(salvo + 348) : 0;
    }

    // ---------------------------------------------------------------- estados no disco
    // Cada estado e um arquivo em <pasta do perfil>\savestates\<fase>\ (fora de "conteudo": o jogo nao o ve como save):
    // "FCSS", versao, hora, posicao e angulos, tamanho e a memoria do checkpoint (~3 MB). Ao entrar numa fase os dela
    // sao lidos de novo (thread a parte); assim a lista continua depois de voltar ao menu, reiniciar ou fechar o jogo.
    constexpr uint32_t ARQ_MAGICA = 0x53534346, ARQ_VERSAO = 1;   // "FCSS"

    // nome de fase seguro para pasta (vem do jogo, mas so letras, numeros, _ e -)
    std::string FaseSegura(const std::string& f)
    {
        std::string s;
        for (char c : f) if (isalnum((unsigned char)c) || c == '_' || c == '-') s += c;
        return s.substr(0, 64);
    }

    std::filesystem::path PastaFase(const std::string& fase) { return g_saveDir / "savestates" / FaseSegura(fase); }

    void GravarNoDisco(const Estado& e)
    {
        if (e.arquivo.empty() || e.memoria.empty()) return;
        // copia so do que vai para o arquivo; a gravacao (~3 MB) fica numa thread a parte
        auto c = std::make_shared<Estado>();
        c->memoria = e.memoria; c->hora = e.hora; c->arquivo = e.arquivo;
        c->x = e.x; c->y = e.y; c->z = e.z; c->ax = e.ax; c->ay = e.ay; c->az = e.az;
        std::thread([c] {
            std::error_code ec;
            std::filesystem::create_directories(c->arquivo.parent_path(), ec);
            std::filesystem::path tmp = c->arquivo; tmp += L".tmp";
            {
                std::ofstream f(tmp, std::ios::binary);
                const uint32_t cab[2] = { ARQ_MAGICA, ARQ_VERSAO };
                const float pos[6] = { c->x, c->y, c->z, c->ax, c->ay, c->az };
                const uint32_t tam = (uint32_t)c->memoria.size();
                f.write((const char*)cab, sizeof(cab));
                f.write((const char*)&c->hora, sizeof(c->hora));
                f.write((const char*)pos, sizeof(pos));
                f.write((const char*)&tam, sizeof(tam));
                f.write((const char*)c->memoria.data(), tam);
                if (!f) { Log("[savestate] nao gravou %s", tmp.string().c_str()); return; }
            }
            std::filesystem::rename(tmp, c->arquivo, ec);
            Log("[savestate] gravado no disco: %s%s", c->arquivo.string().c_str(), ec ? " (erro ao renomear)" : "");
        }).detach();
    }

    void ApagarDoDisco(const std::filesystem::path& p)
    {
        if (p.empty()) return;
        std::error_code ec;
        std::filesystem::remove(p, ec);
    }

    bool LerDoDisco(const std::filesystem::path& p, Estado& e)
    {
        std::ifstream f(p, std::ios::binary);
        uint32_t cab[2] = {}, tam = 0;
        float pos[6] = {};
        f.read((char*)cab, sizeof(cab));
        f.read((char*)&e.hora, sizeof(e.hora));
        f.read((char*)pos, sizeof(pos));
        f.read((char*)&tam, sizeof(tam));
        if (!f || cab[0] != ARQ_MAGICA || cab[1] != ARQ_VERSAO || tam != 45 + TAM_COPIA) return false;
        e.memoria.resize(tam);
        f.read((char*)e.memoria.data(), tam);
        if (!f) return false;
        e.x = pos[0]; e.y = pos[1]; e.z = pos[2]; e.ax = pos[3]; e.ay = pos[4]; e.az = pos[5];
        e.arquivo = p;
        return true;
    }

    // a fase do jogo e outra (ou a 1a): a lista passa a ser a dos estados dessa fase no disco
    void TrocarFase(const std::string& fase)
    {
        unsigned geracao;
        {
            std::lock_guard lock(g_estadosMutex);
            if (fase.empty() || fase == "nil" || FaseSegura(fase).empty() || fase == g_fase) return;
            Log("[savestate] fase %s (antes %s): lista lida do disco", fase.c_str(), g_fase.empty() ? "nenhuma" : g_fase.c_str());
            g_fase = fase;
            g_estados.clear();
            geracao = ++g_geracao;
            g_versao++;
        }
        std::thread([fase, geracao] {
            std::vector<Estado> lidos;
            std::error_code ec;
            for (const auto& a : std::filesystem::directory_iterator(PastaFase(fase), ec))
            {
                if (a.path().extension() != L".fcss") continue;
                Estado e;
                if (LerDoDisco(a.path(), e)) lidos.push_back(std::move(e));
                else Log("[savestate] arquivo invalido: %s", a.path().string().c_str());
            }
            // o mais novo primeiro (pela hora no arquivo)
            std::sort(lidos.begin(), lidos.end(), [](const Estado& a, const Estado& b) {
                FILETIME fa, fb; SystemTimeToFileTime(&a.hora, &fa); SystemTimeToFileTime(&b.hora, &fb);
                return CompareFileTime(&fa, &fb) > 0; });
            std::lock_guard lock(g_estadosMutex);
            if (geracao != g_geracao) return;   // a fase mudou de novo enquanto lia
            // os salvos enquanto lia ficam na frente
            for (Estado& e : lidos)
            {
                if (std::any_of(g_estados.begin(), g_estados.end(), [&](const Estado& j) { return j.arquivo == e.arquivo; })) continue;
                if (g_estados.size() >= MAX_ESTADOS) { ApagarDoDisco(e.arquivo); continue; }
                e.id = g_proximoId++;
                g_estados.push_back(std::move(e));
            }
            g_versao++;
            Log("[savestate] %zu estados da fase %s lidos do disco", g_estados.size(), fase.c_str());
        }).detach();
    }

    // pedido pelo ao_vivo.txt (testes): SAVESTATE=salvar<n> / carregar<n> / carregar<n>:<posicao na lista, 1 = o mais novo>
    void PedidoAoVivo()
    {
        static std::string antes;
        const std::string v = AoVivoValor("SAVESTATE");
        if (v.empty() || v == antes) return;
        antes = v;
        if (v[0] == 's' || v[0] == 'S') g_pedido = SALVAR;
        else if (v[0] == 'c' || v[0] == 'C')
        {
            uint32_t id = 0;
            if (size_t dp = v.find(':'); dp != std::string::npos)
            {
                const size_t pos = (size_t)atoi(v.c_str() + dp + 1);
                std::lock_guard lock(g_estadosMutex);
                if (pos >= 1 && pos <= g_estados.size()) id = g_estados[pos - 1].id;
            }
            g_pedidoId = id;
            g_pedido = CARREGAR;
        }
    }

    // ---------------------------------------------------------------- trechos Lua com a resposta achada em segundo plano
    // O Lua deixa a resposta num texto da memoria do jogo; achar esse texto pode varrer a memoria toda (70 a 400 ms: era a
    // travada do salvar/carregar). A thread do jogo so roda o trecho (~0,1 ms) e uma thread a parte procura a resposta;
    // o salvar e o carregar continuam quando ela chega (alguns quadros depois). Cada pedido usa uma das 16 variaveis
    // FC_SAVESTATE1..16 (o texto fica vivo ate a busca o achar; o coletor do Lua nao o apaga antes).
    struct Busca { uint32_t n = 0; std::string marca, rotulo; };
    std::mutex g_buscaMutex;
    std::condition_variable g_buscaCv;
    std::deque<Busca> g_buscaFila;
    std::map<uint32_t, std::string> g_respostas;   // n -> resposta ("" = nao achada)

    void ThreadBusca()
    {
        for (;;)
        {
            Busca b;
            {
                std::unique_lock lock(g_buscaMutex);
                g_buscaCv.wait(lock, [] { return !g_buscaFila.empty(); });
                b = std::move(g_buscaFila.front());
                g_buscaFila.pop_front();
            }
            LARGE_INTEGER t0, t1, f;
            QueryPerformanceCounter(&t0);
            const std::string achado = LuaProcurarResultadoRapido(b.marca);
            QueryPerformanceCounter(&t1);
            QueryPerformanceFrequency(&f);
            const std::string res = achado.size() > b.marca.size() ?
                achado.substr(b.marca.size(), achado.find('#', b.marca.size()) - b.marca.size()) : "";
            const double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
            if (!b.rotulo.empty()) Log("[savestate] %s: %s (resposta achada em %.1f ms, fora do jogo)", b.rotulo.c_str(), res.empty() ? "(nao achado)" : res.c_str(), ms);
            else if (ms > 50) Log("[savestate] trecho Lua %u: resposta achada em %.1f ms (fora do jogo)", b.n, ms);
            std::lock_guard lock(g_buscaMutex);
            g_respostas[b.n] = res;
            while (g_respostas.size() > 32) g_respostas.erase(g_respostas.begin());
        }
    }

    // roda o trecho Lua (que poe a resposta em r) e pede a busca da resposta; devolve o numero do pedido (0 = o Lua falhou;
    // lua = codigo do LuaRodar). rotulo nao vazio: a thread da busca so anota a resposta no log
    uint32_t LuaPedir(GuestCpu& ctx, const std::string& corpo, uint32_t& lua, const std::string& rotulo = "")
    {
        static std::once_flag uma;
        std::call_once(uma, [] { std::thread(ThreadBusca).detach(); });
        const uint32_t n = ++g_conta;
        const std::string ns = std::to_string(n);
        // a marca e montada com .. (nao pode estar inteira no texto do script: a busca acharia o script)
        const std::string s = "local r = \"\"\n" + corpo + "FC_SAVESTATE" + std::to_string(n % 16 + 1) + " = \"FCSAVE\" .. \"STATE#" + ns + ":\" .. r .. \"#\"\n";
        lua = LuaRodar(ctx, s);
        if (lua != 0) return 0;
        {
            std::lock_guard lock(g_buscaMutex);
            g_buscaFila.push_back({ n, "FCSAVESTATE#" + ns + ":", rotulo });
        }
        g_buscaCv.notify_one();
        return n;
    }

    // resposta do pedido n, se a busca ja terminou (tira da lista)
    bool LuaResposta(uint32_t n, std::string& res)
    {
        std::lock_guard lock(g_buscaMutex);
        auto it = g_respostas.find(n);
        if (it == g_respostas.end()) return false;
        res = std::move(it->second);
        g_respostas.erase(it);
        return true;
    }

    // ---------------------------------------------------------------- salvar e carregar em etapas
    // Etapa em andamento: esperando a resposta do Lua do salvar (posicao do jogador) ou da conferencia do carregar
    enum Etapa { LIVRE = 0, SALVANDO = 1, CONFERINDO = 2 };
    struct Andamento { Etapa etapa = LIVRE; uint32_t pedido = 0; ULONGLONG desde = 0; uint32_t id = 0; bool soMorto = false; };
    Andamento g_andamento;
    constexpr ULONGLONG ESPERA_MAX = 5000;   // ms sem resposta: desiste
    std::vector<uint8_t> g_capturaPronta;     // copia tirada antes de a resposta do salvar chegar (do estado g_capturaProntaId)
    uint32_t g_capturaProntaId = 0;

    void Salvar(GuestCpu& ctx)
    {
        if (CenaDoScriptTocando())
        {
            Log("[savestate] salvar recusado: cena do roteiro tocando");
            Avisar(TR(L"Não dá para salvar durante uma cena", L"Cannot save during a cutscene"));
            return;
        }
        // o jogo alterna entre as duas copias a cada salvar (e marca a atual fora desta memoria): a que mudou e a nova
        const uint32_t memAntes = MemoriaCheckpoint();
        std::vector<uint8_t> antes;
        if (memAntes) antes.assign(G(memAntes), G(memAntes) + TAM_CHECKPOINT);
        // um trecho so: confere o jogador, salva e devolve a fase (g_LevelName), a posicao e os angulos
        uint32_t lua = 0;
        const uint32_t pedido = LuaPedir(ctx,
            "if not _localplayer then r = \"semjogador\"\n"
            "elseif _localplayer.cnt and _localplayer.cnt.health and _localplayer.cnt.health <= 0 then r = \"morto\"\n"
            "else local fase = tostring(g_LevelName)\n"
            "  Game:SaveCheckpoint(\"" NOME_CHECKPOINT "\")\n"
            "  local p = _localplayer:GetPos() local px, py, pz = p.x, p.y, p.z\n"
            "  local a = _localplayer:GetAngles()\n"
            "  r = format(\"ok %s %.3f %.3f %.3f %.3f %.3f %.3f\", fase, px, py, pz, a.x, a.y, a.z) end\n", lua);
        const uint32_t mem = MemoriaCheckpoint();
        Log("[savestate] salvar: Lua %u (0 = ok), memoria do checkpoint %08X", lua, mem);
        if (!pedido || !mem)
        {
            Avisar(TR(L"Não deu para salvar agora", L"Could not save right now"));
            return;
        }
        uint32_t id;
        {
            std::lock_guard lock(g_estadosMutex);
            id = g_proximoId++;
        }
        if (g_captura.ativo) Log("[savestate] o estado anterior ficou sem copia (salvo de novo antes de o jogo gravar)");
        g_captura.ativo = true;   // a memoria do checkpoint e copiada nos quadros seguintes (CapturaQuadro)
        g_captura.id = id;
        g_captura.mem = mem;
        g_captura.quadros = g_captura.mudouEm = 0;
        g_captura.antes = std::move(antes);
        g_andamento = { SALVANDO, pedido, GetTickCount64(), id, false };
    }

    // a resposta do salvar chegou: o estado entra na lista
    void SalvarFim(const std::string& res)
    {
        const uint32_t id = g_andamento.id;
        g_andamento = {};
        Log("[savestate] salvar: resposta %s", res.empty() ? "(nao achada)" : res.c_str());
        if (res.compare(0, 3, "ok ") != 0)
        {
            if (g_captura.id == id) g_captura = {};
            g_capturaPronta.clear();
            g_capturaProntaId = 0;
            if (res == "morto") Avisar(TR(L"Não dá para salvar com o jogador morto", L"Cannot save while the player is dead"));
            else Avisar(TR(L"Não deu para salvar agora", L"Could not save right now"));
            return;
        }
        Estado e;
        e.id = id;
        GetLocalTime(&e.hora);
        char fase[80] = {};
        sscanf(res.c_str() + 3, "%79s %f %f %f %f %f %f", fase, &e.x, &e.y, &e.z, &e.ax, &e.ay, &e.az);
        TrocarFase(fase);   // a lista e a da fase em que salvou
        char nome[64];
        snprintf(nome, sizeof(nome), "estado_%04u%02u%02u_%02u%02u%02u_%03u.fcss", e.hora.wYear, e.hora.wMonth, e.hora.wDay,
                 e.hora.wHour, e.hora.wMinute, e.hora.wSecond, e.hora.wMilliseconds);
        e.arquivo = PastaFase(fase) / nome;
        if (g_capturaProntaId == id) e.memoria = std::move(g_capturaPronta);
        else e.aguardando = true;
        g_capturaPronta.clear();
        g_capturaProntaId = 0;
        GravarNoDisco(e);   // sem a memoria ainda nao grava: grava quando a CapturaQuadro a tirar
        size_t n;
        {
            std::lock_guard lock(g_estadosMutex);
            g_estados.push_front(std::move(e));
            while (g_estados.size() > MAX_ESTADOS) { ApagarDoDisco(g_estados.back().arquivo); g_estados.pop_back(); }
            n = g_estados.size();
            g_versao++;
        }
        Log("[savestate] estado guardado (%zu na lista)", n);
        wchar_t t[160];
        swprintf(t, 160, TR(L"Estado salvo (%zu na lista)  •  %ls para carregar", L"State saved (%zu in the list)  •  %ls to load"), n,
                 SaveStateNomeTecla(SaveStateIndice(OpcoesAtuais().saveStateCarregar)));
        Avisar(t);
    }

    // a cada quadro na fase: copia a memoria do checkpoint do ultimo salvar quando o jogo termina de grava-la
    void CapturaQuadro()
    {
        Captura& c = g_captura;
        if (!c.ativo) return;
        c.quadros++;
        const uint32_t mem = MemoriaCheckpoint();
        if (!mem || mem != c.mem || c.antes.size() != TAM_CHECKPOINT)
        {
            Log("[savestate] a memoria do checkpoint mudou de lugar: estado sem copia");
            c = {};
            return;
        }
        if (!c.mudouEm && memcmp(c.antes.data(), G(mem), TAM_CHECKPOINT) != 0) c.mudouEm = c.quadros;
        if (c.mudouEm && c.quadros >= c.mudouEm + QUADROS_DEPOIS)
        {
            const bool mudou0 = memcmp(c.antes.data() + 45, G(mem + 45), TAM_COPIA) != 0;
            const bool mudou1 = memcmp(c.antes.data() + 45 + TAM_COPIA, G(mem + 45 + TAM_COPIA), TAM_COPIA) != 0;
            const uint32_t copia = mudou1 && !mudou0 ? 1 : 0;
            std::vector<uint8_t> m(G(mem), G(mem) + 45);   // cabecalho (45 bytes) e a copia nova; ao carregar ela vai para as duas
            m.insert(m.end(), G(mem + 45 + copia * TAM_COPIA), G(mem + 45 + copia * TAM_COPIA) + TAM_COPIA);
            Log("[savestate] memoria do checkpoint gravada pelo jogo %d quadros depois do salvar (copias que mudaram: 0 = %d, 1 = %d): guardada a %u",
                c.mudouEm, mudou0, mudou1, copia);
            bool naLista = false;
            {
                std::lock_guard lock(g_estadosMutex);
                for (Estado& e : g_estados)
                    if (e.id == c.id) { e.memoria = std::move(m); e.aguardando = false; naLista = true; GravarNoDisco(e); }
            }
            if (!naLista) { g_capturaPronta = std::move(m); g_capturaProntaId = c.id; }   // a resposta do salvar ainda nao chegou
            c = {};
            return;
        }
        if (c.quadros >= QUADROS_MAX)
        {
            Log("[savestate] o jogo nao gravou a memoria do checkpoint em %d quadros: estado sem copia", QUADROS_MAX);
            c = {};
        }
    }

    // 1a etapa do carregar: confere o jogador e a fase (soMorto = chamado do quadro do menu: so carrega com o jogador da
    // fase morto)
    void Carregar(GuestCpu& ctx, uint32_t id, bool soMorto)
    {
        uint32_t lua = 0;
        const uint32_t pedido = LuaPedir(ctx,
                              std::string("if not _localplayer then r = \"semjogador\"\n") +
                              (soMorto ? "elseif not (_localplayer.cnt and _localplayer.cnt.health and _localplayer.cnt.health <= 0) then r = \"vivo\"\n" : "") +
                              "else r = \"fase \" .. tostring(g_LevelName) end\n", lua);
        if (!pedido)
        {
            Log("[savestate] carregar recusado: Lua %u", lua);
            Avisar(TR(L"Não deu para carregar agora", L"Could not load right now"));
            return;
        }
        g_andamento = { CONFERINDO, pedido, GetTickCount64(), id, soMorto };
    }

    // 2a etapa: a conferencia respondeu; o estado volta para a memoria do checkpoint e o jogo carrega
    void CarregarFim(GuestCpu& ctx, const std::string& res)
    {
        const uint32_t id = g_andamento.id;
        g_andamento = {};
        if (res.compare(0, 5, "fase ") != 0)
        {
            Log("[savestate] carregar recusado: %s", res.empty() ? "(nao achado)" : res.c_str());
            Avisar(TR(L"Não deu para carregar agora", L"Could not load right now"));
            return;
        }
        TrocarFase(res.substr(5));   // so estados desta fase (a lista de outra fase e trocada pela desta)
        // o estado pedido (0 = o mais novo) volta para a memoria do checkpoint
        int posicao = 0;
        bool achou = false;
        float x = 0, y = 0, z = 0, ax = 0, ay = 0, az = 0;
        {
            std::lock_guard lock(g_estadosMutex);
            for (size_t i = 0; i < g_estados.size(); i++)
                if (id == 0 || g_estados[i].id == id)
                {
                    const uint32_t mem = MemoriaCheckpoint();
                    // o mais novo ainda sem copia (o jogo grava nos quadros seguintes): a memoria do jogo ja e a dele
                    if (i == 0 && g_estados[i].aguardando)
                    {
                        achou = true;
                        posicao = 1;
                        const Estado& e = g_estados[i];
                        x = e.x; y = e.y; z = e.z; ax = e.ax; ay = e.ay; az = e.az;
                    }
                    else if (mem && g_estados[i].memoria.size() == 45 + TAM_COPIA)
                    {
                        const uint8_t* m = g_estados[i].memoria.data();
                        memcpy(G(mem), m, 45);
                        memcpy(G(mem + 45), m + 45, TAM_COPIA);               // nas duas copias: vale qualquer
                        memcpy(G(mem + 45 + TAM_COPIA), m + 45, TAM_COPIA);   // que o jogo marque como a atual
                        achou = true;
                        posicao = (int)i + 1;
                        const Estado& e = g_estados[i];
                        x = e.x; y = e.y; z = e.z; ax = e.ax; ay = e.ay; az = e.az;
                    }
                    break;
                }
        }
        // depois do checkpoint, o jogador vai para a posicao e os angulos salvos (z + 0,3 m: pousa no chao em vez de
        // atravessar se a posicao ficar um pouco abaixo dele)
        uint32_t lua = 0;
        LuaPedir(ctx, achou ? "Game:LoadCheckpoint(\"" NOME_CHECKPOINT "\") local p = _localplayer:GetPos() r = format(\"%.2f %.2f %.2f\", p.x, p.y, p.z)\n"
                            : "Game:LoadLastCheckpoint() r = \"ultimo\"\n", lua,
                 achou ? "carregar estado " + std::to_string(posicao) + ": jogador logo depois do LoadCheckpoint" : "carregar ultimo checkpoint do jogo");
        Log("[savestate] carregar %s%s: Lua %u", achou ? "estado " : "ultimo checkpoint do jogo", achou ? std::to_string(posicao).c_str() : "", lua);
        if (lua != 0) { Avisar(TR(L"Não deu para carregar agora", L"Could not load right now")); return; }
        if (!achou) { Avisar(TR(L"Último checkpoint carregado (nenhum estado salvo nesta fase)", L"Last checkpoint loaded (no state saved in this level)")); return; }
        g_repor = { true, 0, x, y, z + 0.3f, ax, ay, az };
        wchar_t t[96];
        swprintf(t, 96, TR(L"Estado %d carregado", L"State %d loaded"), posicao);
        Avisar(t);
    }

    // etapa em andamento: continua quando a resposta chega (menu = chamado do quadro do menu, jogador morto)
    void Continuar(GuestCpu& ctx, bool menu)
    {
        Andamento& a = g_andamento;
        if (a.etapa == LIVRE) return;
        // o LoadCheckpoint roda no mesmo ponto do jogo da conferencia (mira com o jogador vivo, menu com ele morto)
        const bool aquiNao = a.etapa == CONFERINDO && a.soMorto != menu;
        std::string res;
        if (aquiNao || !LuaResposta(a.pedido, res))
        {
            if (GetTickCount64() - a.desde < ESPERA_MAX) return;
            Log("[savestate] sem resposta do Lua em %llu ms: %s desistido", ESPERA_MAX, a.etapa == SALVANDO ? "salvar" : "carregar");
            if (a.etapa == SALVANDO && g_captura.id == a.id) g_captura = {};
            Avisar(a.etapa == SALVANDO ? TR(L"Não deu para salvar agora", L"Could not save right now") : TR(L"Não deu para carregar agora", L"Could not load right now"));
            a = {};
            return;
        }
        if (a.etapa == SALVANDO) SalvarFim(res);
        else CarregarFim(ctx, res);
    }
}

void SaveStatePedir(bool carregar) { g_pedidoId = 0; g_pedido = carregar ? CARREGAR : SALVAR; }
void SaveStateCarregarId(unsigned id) { g_pedidoId = id; g_pedido = CARREGAR; }

void SaveStateApagarId(unsigned id)
{
    std::lock_guard lock(g_estadosMutex);
    for (auto it = g_estados.begin(); it != g_estados.end(); ++it)
        if (it->id == id)
        {
            ApagarDoDisco(it->arquivo);
            g_estados.erase(it);
            g_versao++;
            Log("[savestate] estado apagado pela janela (e do disco)");
            return;
        }
}

unsigned SaveStateVersao() { return g_versao.load(); }

// texto de cada estado da lista (o mais novo primeiro): "1.  14:32:05   (574, 197, 162)"
std::vector<std::pair<unsigned, std::wstring>> SaveStateLista()
{
    std::vector<std::pair<unsigned, std::wstring>> v;
    std::lock_guard lock(g_estadosMutex);
    for (size_t i = 0; i < g_estados.size(); i++)
    {
        const Estado& e = g_estados[i];
        wchar_t t[128];
        swprintf(t, 128, TR(L"%zu.   %02u:%02u:%02u   •   posição %.0f, %.0f, %.0f%ls", L"%zu.   %02u:%02u:%02u   •   position %.0f, %.0f, %.0f%ls"), i + 1,
                 e.hora.wHour, e.hora.wMinute, e.hora.wSecond, e.x, e.y, e.z, i == 0 ? TR(L"   (o mais novo)", L"   (newest)") : L"");
        v.push_back({ e.id, t });
    }
    return v;
}

// gpu.cpp, tecla na janela do jogo (antes das teclas de diagnostico): true = a tecla era do save state
bool SaveStateTecla(unsigned vk)
{
    if (!TeclasLigadas() || InputTeclaEmAcao((int)vk)) return false;
    const Opcoes& o = OpcoesAtuais();
    const bool salvar = (int)vk == o.saveStateSalvar, carregar = (int)vk == o.saveStateCarregar;
    if (!salvar && !carregar) return false;
    if (GetTickCount64() - g_ultimo.load() < 800) return true;   // tecla segurada (repeticao do Windows)
    SaveStatePedir(carregar);
    return true;
}

// input.cpp GUEST_SUB_MIRA: a cada quadro numa fase, na thread principal do jogo
static std::atomic<ULONGLONG> g_miraVista{ 0 };
void SaveStateQuadro(GuestCpu& ctx)
{
    const ULONGLONG agora = GetTickCount64();
    // a fase voltou a rodar (comeco da fase, saida do menu): pergunta a fase (a lista passa a ser a dela, lida do disco);
    // de quebra a busca acha onde o Lua deixa os textos agora (a seguinte comeca perto e leva centesimos de ms)
    static uint32_t pedidoFase = 0;
    if (TeclasLigadas() && LuaPronto() && agora - g_miraVista.load() > 1000)
    {
        uint32_t lua = 0;
        pedidoFase = LuaPedir(ctx, "r = tostring(g_LevelName)\n", lua);
    }
    std::string fase;
    if (pedidoFase && LuaResposta(pedidoFase, fase)) { pedidoFase = 0; TrocarFase(fase); }
    g_miraVista = agora;
    CapturaQuadro();
    if (g_repor.ativo && LuaPronto())
    {
        // quadros depois do carregar: no QUADRO_REPOR poe o jogador no ponto salvo; 3 quadros depois anota onde ficou
        static const int QUADRO_REPOR = [] { const char* e = getenv("FCRECOMP_SAVESTATE_REPOR"); return e ? atoi(e) : 3; }();
        const int q = ++g_repor.quadros;
        if (q == QUADRO_REPOR || q >= QUADRO_REPOR + 3)
        {
            char b[400];
            if (q == QUADRO_REPOR)
                snprintf(b, sizeof(b), "if not _localplayer.theVehicle then\n"   // num veiculo quem manda na posicao e o veiculo
                         "  _localplayer:SetPos({x = %.3f, y = %.3f, z = %.3f})\n  _localplayer:SetAngles({x = %.3f, y = %.3f, z = %.3f})\nend\n"
                         "local p = _localplayer:GetPos() r = format(\"reposto %%.2f %%.2f %%.2f\", p.x, p.y, p.z)\n",
                         g_repor.x, g_repor.y, g_repor.z, g_repor.ax, g_repor.ay, g_repor.az);
            else
                snprintf(b, sizeof(b), "local p = _localplayer:GetPos() r = format(\"%%.2f %%.2f %%.2f\", p.x, p.y, p.z)\n");
            uint32_t lua = 0;
            LuaPedir(ctx, b, lua, "quadro " + std::to_string(q) + " depois do carregar: jogador em");
            if (q >= QUADRO_REPOR + 3 || lua != 0) g_repor.ativo = false;
        }
    }
    Continuar(ctx, false);
    PedidoAoVivo();
    if (g_andamento.etapa != LIVRE) return;   // o pedido novo espera a etapa em andamento
    // carregar enquanto o jogo ainda grava o ultimo salvar: espera ele terminar (a copia seria tirada errada)
    if (g_captura.ativo && g_pedido.load() == CARREGAR) return;
    const int p = g_pedido.exchange(NADA);
    if (p == NADA || !LuaPronto()) return;
    g_ultimo = GetTickCount64();
    if (Multijogador())
    {
        Log("[savestate] recusado: multijogador");
        Avisar(TR(L"Save state não funciona no multijogador", L"Save state does not work in multiplayer"));
        return;
    }
    if (p == SALVAR) Salvar(ctx);
    else Carregar(ctx, g_pedidoId.load(), false);
}

// cena_direta.cpp, comeco do quadro do menu (sub_8220D008, todo quadro do laco principal): com o jogador morto a mira nao
// e atualizada (tela "CARREGAR PONTO SALVO / REINICIAR FASE"); o carregar e feito daqui, so com o jogador da fase morto
// (no menu principal nao ha _localplayer). Salvar fica so para a mira (jogador vivo).
void SaveStateQuadroMenu(GuestCpu& ctx)
{
    if (!g_miraVista.load() || GetTickCount64() - g_miraVista.load() < 500) return;
    Continuar(ctx, true);
    PedidoAoVivo();
    if (g_andamento.etapa != LIVRE || g_pedido.load() == NADA || !LuaPronto()) return;
    g_ultimo = GetTickCount64();
    if (g_pedido.exchange(NADA) == SALVAR)
    {
        Avisar(TR(L"Não dá para salvar agora", L"Cannot save right now"));
        Log("[savestate] salvar recusado: fora da fase ou jogador morto");
        return;
    }
    if (Multijogador()) return;
    Carregar(ctx, g_pedidoId.load(), true);
}

// render.cpp: texto do aviso na tela (vazio = nenhum); alfa para aparecer e sumir aos poucos
std::wstring SaveStateAviso(ULONGLONG agora, float& alfa)
{
    std::lock_guard lock(g_avisoMutex);
    if (g_aviso.empty()) return {};
    const ULONGLONG t = agora - g_avisoDesde;
    if (t >= 2500) { g_aviso.clear(); return {}; }
    alfa = t < 150 ? t / 150.0f : t > 2000 ? (2500 - t) / 500.0f : 1.0f;
    return g_aviso;
}
#else
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
void SaveStatePedir(bool) {}
void SaveStateCarregarId(unsigned) {}
void SaveStateApagarId(unsigned) {}
unsigned SaveStateVersao() { return 0; }
std::vector<std::pair<unsigned, std::wstring>> SaveStateLista() { return {}; }
bool SaveStateTecla(unsigned) { return false; }
std::wstring SaveStateAviso(ULONGLONG, float&) { return {}; }
#endif

// ---------------------------------------------------------------- estados no disco de um perfil (launcher.cpp)
// Mesmo arquivo que o GravarNoDisco escreve: "FCSS", versao 1, SYSTEMTIME, 6 floats (posicao e angulos), tamanho, memoria
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <tuple>
#include "opcoes.h"
std::vector<SaveStateArquivo> SaveStatesDoPerfil(const std::filesystem::path& pastaPerfil)
{
    std::vector<SaveStateArquivo> v;
    std::error_code ec;
    for (const auto& pf : std::filesystem::directory_iterator(pastaPerfil / "savestates", ec))
    {
        if (!pf.is_directory(ec)) continue;
        std::error_code ec2;
        for (const auto& a : std::filesystem::directory_iterator(pf.path(), ec2))
        {
            if (a.path().extension() != L".fcss") continue;
            std::ifstream f(a.path(), std::ios::binary);
            uint32_t cab[2] = {};
            SYSTEMTIME h{};
            float pos[6] = {};
            f.read((char*)cab, sizeof(cab));
            f.read((char*)&h, sizeof(h));
            f.read((char*)pos, sizeof(pos));
            if (!f || cab[0] != 0x53534346 || cab[1] != 1) continue;
            SaveStateArquivo s;
            s.fase = pf.path().filename().string();
            s.arquivo = a.path();
            s.ano = h.wYear; s.mes = h.wMonth; s.dia = h.wDay; s.hora = h.wHour; s.minuto = h.wMinute; s.segundo = h.wSecond;
            s.x = pos[0]; s.y = pos[1]; s.z = pos[2];
            v.push_back(std::move(s));
        }
    }
    auto chave = [](const SaveStateArquivo& s) { return std::make_tuple(s.ano, s.mes, s.dia, s.hora, s.minuto, s.segundo); };
    std::sort(v.begin(), v.end(), [&](const SaveStateArquivo& a, const SaveStateArquivo& b) { return chave(a) > chave(b); });
    return v;
}
