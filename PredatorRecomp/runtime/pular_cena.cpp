// "PULAR CENA" no menu de pausa durante uma cena do motor (cutscene feita no editor de filmes do CryEngine). (Antes,
// fora das cenas o item virava "OPCOES DE VIDEO"; agora ela fica em OPCOES, menu_video.cpp.)
//
// Menu de pausa da fase = pagina MAGMA P_StartMenu (InGameMenu.xml, pacote CryInGame): RETOMAR, VOLTAR AO SALVO,
// REINICIAR FASE, OPCOES, AMIGOS, TRAPACAS, SAIR e "CHEATS (DEBUG)" (B_CheatDebug), que o jogo esconde na montagem da
// pagina (sub_821AFA10, tabela de virtuais em 0x82059CA0: [2] sub_821AF388 = A num item, que chama sub_821AE910 para
// os itens desta pagina; [9] sub_8216E4D0 = pagina mostrada, generica; [23] sub_821AFA10 = montagem). Aqui esse item
// escondido vira "PULAR CENA" e so aparece quando o menu abre durante uma cena (bit 0x80 de elemento+24 = visivel,
// 0x40 de +76 = escolhivel, como no OPCOES DE VIDEO de menu_video.cpp).
//
// Cenas: sistema de filmes = R32(0x829122C4) (lis -32111 + 8900). Lista das sequencias tocando em +76 (sentinela;
// no: +0 proximo, +8 sequencia, +12 tempo). Sequencia: +56 inicio, +60 fim, +64 bandeiras (0x8 = cena, a mesma que
// Movie:StopAllCutScenes usa; 0x4 = repete; 0x2 = fica parada no fim). sub_82342CC8 = atualizacao (f1 = tempo do
// quadro): soma o tempo, anima (sub_82350E60) e para (sub_82342990) as que chegaram no fim; nao faz nada com o jogo
// pausado.
//
// Pular: escolhido o item, o menu fecha como RETOMAR e, na proxima atualizacao, a cena e adiantada em passos de 0,1 s
// ate acabar (chamando a atualizacao original varias vezes no mesmo quadro). Assim as chaves de evento da cena (fim
// da cena, HUD, controle de volta, objetos que mudam de lugar) acontecem na ordem, como se a cena tivesse sido vista.
// Uma cena que nao acaba sozinha (repete ou fica parada no fim) e parada depois de passar do fim.
//
// As cenas das fases sao trabalhos de ancoras de IA (veja sub_8226AAF0 e sub_82306800 abaixo). Pulo direto: entre o
// BeginCutScene e o EndCutScene, cada PlayAnimation termina na hora e os filmes que a cena solta vao ate o fim, com o
// relogio acelerado, o som calado e a tela preta (fica ~0,5 s). FCRECOMP_PULARCENA_DIRETO=0 so acelera.
// FCRECOMP_PULARCENA=0 desliga.
#ifndef PREDATOR_EDITOR
#include "guest.h"
#include "idioma.h"
#include "perf.h"
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>

void RelogioAcelerar(uint32_t fator);        // input.cpp
std::atomic<bool> g_acelerando{ false };     // cena do script sendo adiantada (relogio acelerado, som calado, FPS sem limite)
extern std::atomic<bool> g_somCalado;        // audio.cpp
void SonsDoPuloInicio();                     // audio.cpp: para os sons que a cena pulada soltar
void SonsDoPuloFim();
void SonsCenaComeca();                       // audio.cpp: guarda os sons de antes da cena
bool MenuVideoDisponivel(uint32_t pausa);    // menu_video.cpp: a pagina de video da pausa existe agora
bool MenuVideoAbrir(GuestCpu& ctx, uint32_t pausa);   // menu_video.cpp: abre OPCOES DE VIDEO (modo video da P_Video)
void MenuVideoFechar(GuestCpu& ctx, bool devolver);

namespace
{
constexpr uint32_t BUSCA_ELEMENTO = 0x82469EC8;   // (r3 = conjunto de elementos da pagina, r4 = id) -> elemento
constexpr uint32_t WSTRING_COPIA = 0x82158218;    // (r3 = wstring, r4 = texto, r5 = tamanho)
constexpr uint32_t PARAR_SEQUENCIA = 0x82342990;  // (r3 = sistema de filmes, r4 = sequencia)
constexpr uint32_t SISTEMA_FILMES = 0x82910000 + 8900;

constexpr uint32_t ID_RETOMAR = 229177234;        // B_Resume
constexpr uint32_t ID_PULAR = 2428657690;         // B_CheatDebug (escondido pelo jogo)

constexpr float PASSO = 0.1f;                     // segundos por passo ao adiantar a cena
constexpr int MAX_PASSOS = 20 * 60 * 10;          // 20 minutos de cena

bool Ligado()
{
    static const bool l = [] { const char* e = getenv("FCRECOMP_PULARCENA"); return !e || atoi(e) != 0; }();
    return l;
}

uint32_t g_pagina = 0;                            // P_StartMenu montada
uint32_t g_elemento = 0;                          // B_CheatDebug dela
uint8_t g_vis24 = 0, g_vis76 = 0, g_esc24 = 0, g_esc76 = 0;   // bytes do item visivel / escondido
double g_cenaInicio = 0;                          // hora do BeginCutScene (teste automatico)
bool g_pular = false;                             // pedido de pular a cena do sistema de filmes (proxima atualizacao)
bool g_modoVideo = false;                         // o item escondido esta como OPCOES DE VIDEO (fora das cenas)
const char* TextoItem() { return g_modoVideo ? TR("OPÇÕES DE VÍDEO", "VIDEO OPTIONS") : TR("PULAR CENA", "SKIP SCENE"); }
std::atomic<bool> g_cenaScript{ false };
double g_ultimoFimCena = -1e9;                    // hora do ultimo EndCutScene
int g_fimQuadros = 0;
// trabalhos com uma animacao de cena (DisableCollisions) tocando durante a cena: 22_duneb 1-1-1 tem cinco atores (Carver,
// ajudante, oficial, magistrado, guerrilheiro), e o EndCutScene vem da animacao do Carver; as outras tambem tem de
// terminar, senao o ator fica parado no lugar da cena
std::map<uint32_t, uint64_t> g_animCena;   // trabalho -> ultimo quadro em que o jogo o atualizou
uint64_t g_quadro = 0;
std::set<uint32_t> g_trabCena;                      // trabalhos que mandaram BeginCutScene (a cena em si)
uint32_t g_trabalhoAtual = 0;                      // trabalho cuja acao esta rodando agora
uint64_t g_quadroCarregando = 0;                   // ultimo quadro com uma animacao de cena ainda carregando (+60 = 1)
bool AtoresAndando()   // algum ator pendente atualizado nos ultimos 5 quadros (o jogo pausado nao os chama mais)
{
    for (auto& [t, q] : g_animCena) if (g_quadro - q <= 5) return true;
    return false;
}
int g_esperaAtores = 0;                             // quadros ate soltar o relogio depois do EndCutScene (0 = nada)          // entre SendCommandCarver("BeginCutScene") e ("EndCutScene")
          // cena do script sendo adiantada (relogio acelerado, som calado)
double g_acelInicio = 0;
std::atomic<double> g_clarearInicio{ 0 };         // fim do pulo: a tela clareia em CLAREAR ms
constexpr double CLAREAR = 1000;

// O relogio do jogo (objeto em 0x828BBE50, sub_823175F0 = atualizacao do quadro) limita o tempo de cada quadro a
// 0,1 s (double em 0x82051258, so usado ali): com o relogio 10x a cena so andava ~3x (15 s para 45 s de cena). Durante o
// pulo o limite sobe para LIMITE_PULO e o relogio corre FATOR vezes; os quadros saem sem limite de FPS (gpu.cpp ModoFps).
constexpr uint32_t LIMITE_QUADRO = 0x82051258;
constexpr double LIMITE_ORIGINAL = 0.1;
bool Filmes()   // FCRECOMP_PULARCENA_FILMES=0: no pulo direto, nao adianta os filmes que a cena solta
{
    static const bool l = [] { const char* e = getenv("FCRECOMP_PULARCENA_FILMES"); return !e || atoi(e) != 0; }();
    return l;
}
bool PuloDireto()   // FCRECOMP_PULARCENA_DIRETO=0 volta a so acelerar (as animacoes das cenas tocam inteiras)
{
    static const bool l = [] { const char* e = getenv("FCRECOMP_PULARCENA_DIRETO"); return !e || atoi(e) != 0; }();
    return l;
}
double LimitePulo()   // 2,0 travou a cena da fuga e 0,2 a intro do treino (a animacao do Carver nunca acaba); 0,15 passa nas duas
                      // (fuga 6 s, intro do treino 15 s; com 0,1: 9 s e 24 s). FCRECOMP_PULARCENA_LIMITE muda (testes)
{
    static const double l = [] { const char* e = getenv("FCRECOMP_PULARCENA_LIMITE"); return e ? atof(e) : 0.15; }();
    return l;
}

uint32_t Fator()
{
    static const uint32_t f = [] { const char* e = getenv("FCRECOMP_PULARCENA_FATOR"); return e ? (uint32_t)atoi(e) : 60u; }();
    return f;
}

void Limite(double v) { W64(LIMITE_QUADRO, std::bit_cast<uint64_t>(v)); }

uint32_t Chamar(GuestCpu& ctx, uint32_t f, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0)
{
    GuestCpu salvo = ctx;
    SetGpr(ctx, 3, r3); SetGpr(ctx, 4, r4); SetGpr(ctx, 5, r5);
    GuestCall(ctx, f);
    const uint32_t r = Gpr32(ctx, 3);
    ctx = salvo;
    return r;
}

float LerF(uint32_t a) { return std::bit_cast<float>(R32(a)); }

// sequencia que conta como cena do sistema de filmes: so a bandeira de cena (0x8). As cenas das fases nao usam essa
// bandeira (a fuga do Evolution, "4_1_1", tem bandeiras 0): elas sao do script (veja SendCommandCarver abaixo)
bool g_pularTodos = false;   // fim do pulo direto: adianta tambem os filmes sem 0x8 que nao repetem (0x4) nem ficam parados
                             // no fim (0x2), e so os que ainda nao chegaram no fim. Inclui os de objeto (0x1): em 22_duneb
                             // o se_1-1-1_PlayOnLoad (64 s) solta a explosao do aviao, as falas e os inimigos da cena
bool EhCena(uint32_t seq)
{
    const uint32_t f = R32(seq + 64);
    return (f & 0x8) != 0 || (g_pularTodos && (f & 0x6) == 0);
}

// nome da sequencia (std::string em +32: texto ou ponteiro, tamanho +48, capacidade +52)
std::string NomeSeq(uint32_t seq)
{
    const uint32_t tam = R32(seq + 48), cap = R32(seq + 52);
    if (tam > 200) return "?";
    const uint32_t p = cap >= 16 ? R32(seq + 32) : seq + 32;
    return p ? std::string((const char*)G(p), tam) : "?";
}

// sequencias de cena tocando; com 'tempo', soma dos tempos (para ver se a atualizacao andou)
int CenasTocando(float* tempo = nullptr, bool* todasNoFim = nullptr)
{
    const uint32_t ms = R32(SISTEMA_FILMES);
    if (!ms) return 0;
    const uint32_t sentinela = R32(ms + 76);
    if (!sentinela) return 0;
    int n = 0;
    float soma = 0;
    bool fim = true;
    uint32_t no = R32(sentinela);
    for (int i = 0; no && no != sentinela && i < 256; i++, no = R32(no))
    {
        const uint32_t seq = R32(no + 8);
        if (!seq || !EhCena(seq)) continue;
        const float t = LerF(no + 12);
        if (g_pularTodos && !(R32(seq + 64) & 0x8) && t >= LerF(seq + 60)) continue;
        n++;
        soma += t;
        if (t < LerF(seq + 60)) fim = false;
    }
    if (tempo) *tempo = soma;
    if (todasNoFim) *todasNoFim = fim;
    return n;
}

void PararCenas(GuestCpu& ctx)
{
    const uint32_t ms = R32(SISTEMA_FILMES);
    for (int volta = 0; volta < 64; volta++)
    {
        const uint32_t sentinela = R32(ms + 76);
        uint32_t achou = 0;
        for (uint32_t no = R32(sentinela); no && no != sentinela; no = R32(no))
            if (const uint32_t seq = R32(no + 8); seq && EhCena(seq)) { achou = seq; break; }
        if (!achou) return;
        Chamar(ctx, PARAR_SEQUENCIA, ms, achou);
    }
}

// texto UTF-8 -> UTF-16 big-endian na memoria do jogo
uint32_t TextoNoJogo(const std::string& s, uint32_t& tamanho)
{
    static std::map<std::string, std::pair<uint32_t, uint32_t>> textos;
    auto it = textos.find(s);
    if (it == textos.end())
    {
        std::u16string u;
        for (size_t i = 0; i < s.size();)
        {
            const unsigned char c = (unsigned char)s[i];
            if (c < 0x80) { u += c; i += 1; }
            else if ((c >> 5) == 6 && i + 1 < s.size()) { u += (char16_t)(((c & 0x1F) << 6) | (s[i + 1] & 0x3F)); i += 2; }
            else if (i + 2 < s.size()) { u += (char16_t)(((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F)); i += 3; }
            else break;
        }
        const uint32_t buf = GuestAlloc((uint32_t)(u.size() + 1) * 2);
        if (!buf) { tamanho = 0; return 0; }
        for (size_t i = 0; i <= u.size(); i++) W16(buf + (uint32_t)i * 2, i < u.size() ? u[i] : 0);
        it = textos.emplace(s, std::make_pair(buf, (uint32_t)u.size())).first;
    }
    tamanho = it->second.second;
    return it->second.first;
}

// Filmes que a cena soltou ainda por adiantar quando a caixa de instrucao pausou o jogo (intro do treino: a 2a cena
// solta o INTRO, 110 s, com a explosao do barco e as falas; logo depois do EndCutScene a caixa pausa o jogo e a
// atualizacao dos filmes nao anda). A tela e o som voltam (o jogador ve a caixa), mas o pedido fica: assim que o jogo
// anda de novo os filmes vao ate o fim (e os scripts do fim deles rodam). Ate la os sons novos continuam sendo parados.
bool g_filmesPendentes = false;

void PararAceleracao(const char* motivo, bool filmesDepois = false)
{
    if (!filmesDepois) g_pularTodos = false;
    if (!g_acelerando.exchange(false)) return;
    RelogioAcelerar(1);
    Limite(LIMITE_ORIGINAL);
    if (filmesDepois) { g_filmesPendentes = true; g_somCalado = false; }   // os sons novos continuam sendo parados
    else SonsDoPuloFim();   // e o som volta (audio.cpp)
    g_clarearInicio = perf::Now();
    Log("[pularcena] cena do script adiantada: %s depois de %.1f s (relogio %ux)", motivo, (perf::Now() - g_acelInicio) / 1000.0, Fator());
}

void IniciarPulo()
{
    if (g_acelerando.exchange(true)) return;
    g_acelInicio = perf::Now();
    g_somCalado = true;
    SonsDoPuloInicio();
    if (std::bit_cast<double>(R64(LIMITE_QUADRO)) == LIMITE_ORIGINAL) Limite(LimitePulo());
    RelogioAcelerar(Fator());
    Log("[pularcena] pulo da cena do script: relogio %ux, ate %.2f s por quadro", Fator(), LimitePulo());
}

void MostrarItem(bool mostrar)
{
    if (!g_elemento) return;
    W8(g_elemento + 24, mostrar ? g_vis24 : g_esc24);
    W8(g_elemento + 76, mostrar ? g_vis76 : g_esc76);
}
}

// montagem do menu de pausa: guarda o item escondido (visivel e escondido) e troca o nome dele
GUEST_DECLARE_ORIGINAL(sub_821AFA10);
GUEST_FUNC(sub_821AFA10)
{
    if (!Ligado()) { GUEST_ORIGINAL(sub_821AFA10)(ctx, base); return; }
    const uint32_t pagina = Gpr32(ctx, 3);
    const uint32_t conjunto = R32(R32(pagina + 80));
    const uint32_t e = conjunto ? Chamar(ctx, BUSCA_ELEMENTO, conjunto, ID_PULAR) : 0;
    const uint8_t b24 = e ? R8(e + 24) : 0, b76 = e ? R8(e + 76) : 0;
    // sem o bit 0x40 o jogo nao desliga o item nem redesenha o widget como desligado (ficava apagado)
    if (e) W8(e + 76, b76 & ~0x40);
    GUEST_ORIGINAL(sub_821AFA10)(ctx, base);
    MenuVideoFechar(ctx, false);   // fase nova: os rotulos trocados eram da fase anterior
    if (!e) return;
    g_pagina = pagina;
    g_elemento = e;
    g_fimQuadros = 0;
    g_animCena.clear(); g_trabCena.clear();
    g_cenaScript = false;   // fase nova (carregada ou reiniciada no meio de uma cena)
    PararAceleracao("fase nova");
    if (g_filmesPendentes) { g_filmesPendentes = false; SonsDoPuloFim(); }
    g_pular = false; g_pularTodos = false;
    g_vis24 = b24 | 0x80; g_vis76 = b76;
    g_esc24 = R8(e + 24) & 0x7F; g_esc76 = R8(e + 76) & ~0x40;
    MostrarItem(false);
    uint32_t n = 0;
    if (const uint32_t w = R32(e + 8))
        if (const uint32_t t = TextoNoJogo(TextoItem(), n)) Chamar(ctx, WSTRING_COPIA, w + 60, t, n);
    Log("[pularcena] menu de pausa montado: pagina %08X, item %08X (%02X/%02X)", pagina, e, b24, b76);
}

// pagina mostrada (generica): no menu de pausa, o item so aparece durante uma cena
GUEST_DECLARE_ORIGINAL(sub_8216E4D0);
GUEST_FUNC(sub_8216E4D0)
{
    const uint32_t pagina = Gpr32(ctx, 3);
    if (g_pagina && pagina == g_pagina)
    {
        // o item so aparece durante uma cena (PULAR CENA); OPCOES DE VIDEO fica em OPCOES (menu_video.cpp)
        MenuVideoFechar(ctx, true);   // a pagina de video ficou no modo video (menu fechado com START em cima dela)
        const int cenas = CenasTocando();
        const bool cena = !g_acelerando && (g_cenaScript || cenas > 0);
        g_modoVideo = false;
        MostrarItem(cena);
        Log("[pularcena] menu de pausa aberto: cena do script %s, %d cena(s) de filme, item %s", g_cenaScript ? "sim" : "nao", cenas,
            g_modoVideo ? "OPCOES DE VIDEO" : "PULAR CENA");
    }
    GUEST_ORIGINAL(sub_8216E4D0)(ctx, base);
    // o texto do item volta ao da tabela ("CHEATS (DEBUG)") depois da montagem: escreve de novo com a pagina na tela
    if (g_pagina && pagina == g_pagina && g_elemento)
        if (const uint32_t w = R32(g_elemento + 8))
        {
            uint32_t n = 0;
            if (const uint32_t t = TextoNoJogo(TextoItem(), n)) Chamar(ctx, WSTRING_COPIA, w + 60, t, n);
        }
}

// item focado no menu de pausa e em OPCOES (texto de ajuda no widget pagina+268): OPCOES DE VIDEO ganha a sua ajuda
GUEST_DECLARE_ORIGINAL(sub_821AFD28);
GUEST_FUNC(sub_821AFD28)
{
    const uint32_t pagina = Gpr32(ctx, 3), ev = Gpr32(ctx, 4);
    GUEST_ORIGINAL(sub_821AFD28)(ctx, base);
    // OPCOES DE VIDEO: o item copiado em pausa > OPCOES (id do B_MultiplayerSetup, menu_video.cpp)
    const uint32_t id = ev ? R32(ev + 4) : 0;
    if (id != 1606581513u && (!g_modoVideo || pagina != g_pagina || id != ID_PULAR)) return;
    if (const uint32_t w = R32(pagina + 268))
    {
        uint32_t n = 0;
        if (const uint32_t t = TextoNoJogo(TR("Resolução interna, sombras, suavização, quadros por segundo e sincronia vertical.",
                                              "Internal resolution, shadows, anti-aliasing, frames per second and vertical sync."), n))
            Chamar(ctx, 0x8216A6E8, w, t);   // muda o texto do widget
    }
}

// A num item do menu de pausa: PULAR CENA fecha o menu como RETOMAR e pede o pulo
GUEST_DECLARE_ORIGINAL(sub_821AE910);
GUEST_FUNC(sub_821AE910)
{
    const uint32_t ev = Gpr32(ctx, 4);
    if (!g_elemento || !ev || R32(ev + 4) != ID_PULAR) { GUEST_ORIGINAL(sub_821AE910)(ctx, base); return; }
    if (g_modoVideo) { MenuVideoAbrir(ctx, Gpr32(ctx, 3)); return; }   // OPCOES DE VIDEO: a pagina de video por cima do menu de pausa
    g_pular = CenasTocando() > 0;
    if (g_cenaScript) IniciarPulo();
    Log("[pularcena] PULAR CENA escolhido (cena do script %s, filme %s)", g_cenaScript ? "sim" : "nao", g_pular ? "sim" : "nao");
    MostrarItem(false);
    W32(ev + 4, ID_RETOMAR);
    GUEST_ORIGINAL(sub_821AE910)(ctx, base);
    W32(ev + 4, ID_PULAR);
}

// atualizacao do sistema de filmes: com o pedido de pular, adianta as cenas ate acabarem
GUEST_DECLARE_ORIGINAL(sub_82342CC8);
GUEST_FUNC(sub_82342CC8)
{
    GUEST_ORIGINAL(sub_82342CC8)(ctx, base);
    if (static const bool diag = getenv("FCRECOMP_PULARCENA_LOG") != nullptr; diag)
    {
        static std::map<uint32_t, int> vistas;
        const uint32_t ms = R32(SISTEMA_FILMES);
        if (const uint32_t s = ms ? R32(ms + 76) : 0)
            for (uint32_t no = R32(s); no && no != s; no = R32(no))
                if (const uint32_t seq = R32(no + 8); seq && !vistas.count(seq))
                {
                    vistas[seq] = 1;
                    std::string dump;
                    for (int k = 0; k < 72; k += 4) { char b[12]; snprintf(b, sizeof(b), " %08X", R32(seq + k)); dump += b; }
                    Log("[pularcena] sequencia %08X \"%s\" flags %X inicio %.2f fim %.2f t %.2f |%s", seq, NomeSeq(seq).c_str(), R32(seq + 64),
                        LerF(seq + 56), LerF(seq + 60), LerF(no + 12), dump.c_str());
                }
    }
    {
        static int antes = 0;
        float t = 0;
        const int agora = CenasTocando(&t);
        if (agora != antes) Log("[pularcena] cenas tocando: %d -> %d (tempo %.1f s)", antes, agora, t);
        antes = agora;
        // teste: FCRECOMP_PULARCENA_TESTE=s pede o pulo s segundos depois que uma cena comeca (sem o menu)
        static const float teste = [] { const char* e = getenv("FCRECOMP_PULARCENA_TESTE"); return e ? (float)atof(e) : 0.f; }();
        if (teste > 0 && agora && t >= teste && !g_pular) { static int vezes = 0; if (vezes++ < 1) { g_pular = true; Log("[pularcena] teste: pulo pedido"); } }
    }
    // teste: FCRECOMP_PULARCENA_AUTO=s pula a cena do script s segundos depois do BeginCutScene (sem o menu)
    if (static const double autoS = getenv("FCRECOMP_PULARCENA_AUTO") ? atof(getenv("FCRECOMP_PULARCENA_AUTO")) : 0; autoS > 0)
        if (g_cenaScript && !g_acelerando && perf::Now() - g_cenaInicio > autoS * 1000) { Log("[pularcena] teste: pulo automatico"); IniciarPulo(); }
    if (g_acelerando)   // registro a cada segundo: quanto o tempo do jogo andou (relogio em 0x828BBE50: +40 total, +104 quadro)
    {
        static double ultimo = 0; static uint64_t quadros = 0; quadros++;
        if (perf::Now() - ultimo > 1000)
        {
            ultimo = perf::Now();
            Log("[pularcena] acelerando: %llu quadros, tempo do jogo %.1f s, ultimo quadro %.3f s", (unsigned long long)quadros,
                std::bit_cast<double>(R64(0x828BBE50 + 40)), std::bit_cast<double>(R64(0x828BBE50 + 104)));
        }
    }
    g_quadro++;
    if (g_fimQuadros == 1 && !g_cenaScript && AtoresAndando() && g_esperaAtores < 120)
        g_esperaAtores++;   // ainda ha ator numa animacao de cena: espera mais um quadro
    else if (g_fimQuadros > 0 && --g_fimQuadros == 0 && !g_cenaScript)
    {
        if (!g_animCena.empty()) Log("[pularcena] %zu ator(es) nao terminaram a animacao da cena", g_animCena.size());
        g_esperaAtores = 0;
        // pulo direto: os filmes que a cena deixou tocando (a intro do treino: INTRO, 110 s, que solta a explosao do
        // barco) sao adiantados ate o fim ainda com a tela preta e o som calado
        g_pularTodos = PuloDireto() && Filmes();
        if (g_pularTodos && CenasTocando()) g_pular = true;
        else { g_pularTodos = false; PararAceleracao("fim da cena"); }
    }
    if (static const bool diag = getenv("FCRECOMP_PULARCENA_LOG") != nullptr; diag)   // todos os filmes a cada 2 s
    {
        static double ultimoF = 0;
        if (perf::Now() - ultimoF > 2000 && R32(SISTEMA_FILMES) && R32(R32(SISTEMA_FILMES) + 76))
        {
            ultimoF = perf::Now();
            const uint32_t sentinela = R32(R32(SISTEMA_FILMES) + 76);
            uint32_t no = R32(sentinela);
            std::string l;
            for (int i = 0; no && no != sentinela && i < 64; i++, no = R32(no))
                if (const uint32_t seq = R32(no + 8))
                {
                    char b[96];
                    snprintf(b, sizeof b, " %s(%X) %.1f/%.1f", NomeSeq(seq).c_str(), R32(seq + 64), LerF(no + 12), LerF(seq + 60));
                    l += b;
                }
            Log("[pularcena] filmes:%s", l.c_str());
        }
    }
    // seguranca: a cena do script nao acabou em 60 s de relogio acelerado
    if (g_acelerando && perf::Now() - g_acelInicio > 60000) PararAceleracao("o fim nao veio (desistiu)");
    // pulo direto: os filmes que a cena solta (a intro do treino: INTRO, 110 s, que solta a explosao do barco) vao
    // ate o fim ainda durante a cena: logo depois do EndCutScene a caixa de instrucao pausa o jogo e eles param
    // 24_shantyday (se-3-1-1, Carver no barco): adiantar o filme do barco antes de a animacao do Carver carregar deixava
    // a animacao carregando para sempre; espera todas as animacoes de cena terem carregado
    if (g_acelerando && g_cenaScript && !g_pular && PuloDireto() && Filmes() && g_quadro - g_quadroCarregando > 2)
    {
        g_pularTodos = true;
        if (CenasTocando()) g_pular = true; else g_pularTodos = false;
    }
    if (!g_pular) return;
    float t0 = 0;
    auto terminou = [] {
        g_pular = false;
        if (g_filmesPendentes)
        {
            g_filmesPendentes = false;
            SonsDoPuloFim();
            Log("[pularcena] filmes da cena adiantados depois da pausa do jogo");
        }
        if (!g_pularTodos) return;
        g_pularTodos = false;
        if (!g_cenaScript) PararAceleracao("fim da cena e dos filmes");
    };
    if (!CenasTocando(&t0)) { terminou(); return; }

    const GuestCpu salvo = ctx;
    int passos = 0, sobra = 0;
    for (; passos < MAX_PASSOS; passos++)
    {
        ctx = salvo;
        SetFpr(ctx, 1, (double)PASSO);
        GUEST_ORIGINAL(sub_82342CC8)(ctx, base);
        float t = 0;
        bool noFim = false;
        if (!CenasTocando(&t, &noFim)) break;
        if (passos == 0 && t == t0)   // jogo pausado: tenta no proximo quadro
        {
            ctx = salvo;
            if (g_pularTodos && g_acelerando && !g_cenaScript)   // caixa de instrucao: os filmes andam quando o jogo voltar
            {
                PararAceleracao("jogo pausado (caixa de instrucao); os filmes da cena vao ate o fim quando ele voltar", true);
                return;
            }
            if (g_pularTodos && !g_filmesPendentes) terminou();
            return;
        }
        if (noFim && ++sobra > 20) break;                       // repete ou fica parada no fim: passou 2 s do fim
    }
    ctx = salvo;
    const int restam = CenasTocando();
    if (restam)   // registra as que nao acabaram sozinhas
    {
        const uint32_t sentinela = R32(R32(SISTEMA_FILMES) + 76);
        uint32_t no = R32(sentinela);
        for (int i = 0; no && no != sentinela && i < 256; i++, no = R32(no))
            if (const uint32_t seq = R32(no + 8); seq && EhCena(seq))
                Log("[pularcena] filme \"%s\" flags %X em %.1f de %.1f s", NomeSeq(seq).c_str(), R32(seq + 64), LerF(no + 12), LerF(seq + 60));
        PararCenas(ctx);
    }
    Log("[pularcena] cena adiantada %.1f s (%d passos)%s", passos * PASSO, passos, restam ? ", parada no fim" : "");
    terminou();
}

// Acoes das ancoras de IA (trabalho generico): sub_82306800(r3 = trabalho, r4, r5 = acao, r6 = indice) escolhe pelo
// tipo (acao+4; nomes na tabela 0x828BB178: 11 = PlayAnimation, 12 = PlaySequence, 25 = Wait) e devolve o estado.
// Estado da acao em r3 + (indice+3)*76. Diagnostico (FCRECOMP_PULARCENA_LOG): registra quando o retorno muda.
GUEST_DECLARE_ORIGINAL(sub_82306800);
GUEST_FUNC(sub_82306800)
{
    static const bool diag = getenv("FCRECOMP_PULARCENA_LOG") != nullptr;
    const uint32_t trabalho = Gpr32(ctx, 3), acao = Gpr32(ctx, 5), indice = Gpr32(ctx, 6);
    const uint32_t tipo = R32(acao + 4);
    // pulo direto: a PlayAnimation fica na fase 1 (+56) ate o tempo do trabalho (trabalho+164) passar de +24 (inicio)
    // + +28 (duracao); +60 = 2 tocando, 4 = acabou (aviso da animacao); +40 = repeticoes que faltam (com 0 devolve 7 =
    // feita, senao recomeca a animacao). No pulo: acabou agora, sem repeticao.
    // Acoes filhas de uma PlayAnimation (acao+12/+16 = lista; rodam com indice 1 enquanto a animacao toca; ex. 21_dunea
    // 2_1_1_carver_a: Wait 8 s, GoPath do caminhao da Kade): a animacao so termina na hora depois que todas acabaram.
    static std::map<uint32_t, std::pair<uint32_t, uint32_t>> filhos;   // trabalho -> {acao mae, filhas feitas}
    if (indice == 0)
    {
        auto& f = filhos[trabalho];
        if (f.first != acao) f = { acao, 0 };
    }
    const uint32_t nFilhas = tipo == 11 && R32(acao + 12) ? (R32(acao + 16) - R32(acao + 12)) / 4 : 0;
    // So as animacoes de cena (DisableCollisions="1", acao+72: SE_..., T4_...); as dos outros personagens que rodam ao
    // mesmo tempo (guardas de 22_duneb) seguem normais, senao eles ficam parados depois do pulo.
    const bool deCena = tipo == 11 && (R32(acao + 72) >> 24) != 0;
    if (deCena && indice == 0 && (g_cenaScript || g_animCena.count(trabalho))) g_animCena[trabalho] = g_quadro;
    if (g_acelerando && deCena && PuloDireto() && (g_cenaScript || g_animCena.count(trabalho)) &&
        (indice != 0 || filhos[trabalho].second >= nFilhas))
    {
        const uint32_t est = trabalho + (indice + 3) * 76;
        if ((R32(est + 56) == 1 || R32(est + 56) == 2) && (R32(est + 60) == 2 || R32(est + 60) == 4))   // 2 = ChildrenDuration/Infinite
        {
            W32(est + 60, 4); W32(est + 24, 0); W32(est + 28, 0); W32(est + 40, 0);   // +40 = repeticoes que faltam
        }
    }
    // Wait por tempo (sub_8230B338: modo em acao+36 = 0 sinal, 1 Infinite, 2 MinDuration, 3 Character ReadyFor; o fim
    // fica em trabalho+176 e e comparado com o tempo do trabalho, +164) num trabalho da cena: acaba na hora. Vale para
    // as filhas das animacoes de cena (24_shantyday se-3-1-1: Wait 18 s antes do Unbind do Carver do barco; 21_dunea:
    // Wait 8 s do caminhao) e, entre o BeginCutScene e o EndCutScene, para as do trabalho da cena e dos atores
    // (02_native Crowe_Capture1, 13_black_forest elevador: Wait 9 s dentro de LookAt, 30_mountain 7_2_3...)
    if (g_acelerando && PuloDireto() && tipo == 25 && R32(acao + 36) == 2 && R32(trabalho + (indice + 3) * 76 + 56) != 0 &&
        ((indice != 0 && g_animCena.count(trabalho)) || (g_cenaScript && (g_trabCena.count(trabalho) || g_animCena.count(trabalho)))))
        W32(trabalho + 176, R32(trabalho + 164));
    const uint32_t trabalhoAntes = g_trabalhoAtual;
    g_trabalhoAtual = trabalho;
    GUEST_ORIGINAL(sub_82306800)(ctx, base);
    g_trabalhoAtual = trabalhoAntes;
    if (deCena && indice == 0 && Gpr32(ctx, 3) == 2 && (g_cenaScript || g_animCena.count(trabalho))) g_quadroCarregando = g_quadro;
    if (indice != 0 && Gpr32(ctx, 3) == 11) filhos[trabalho].second++;
    if (deCena && indice == 0 && Gpr32(ctx, 3) == 11) g_animCena.erase(trabalho);
    if (filhos.size() > 512) filhos.clear();
    if (!diag) return;
    const uint32_t volta = Gpr32(ctx, 3), estado = trabalho + (indice + 3) * 76;
    static std::map<uint64_t, uint32_t> ultimo;
    const uint64_t chave = (uint64_t)trabalho << 32 | indice;
    auto it = ultimo.find(chave);
    if (it != ultimo.end() && it->second == volta) return;
    ultimo[chave] = volta;
    const uint32_t nome = R32(0x828BB178 + 4 * tipo);
    Log("[pularcena] acao %08X #%u tipo %u (%s) volta %u (estado +56 %u, +60 %u)%s", trabalho, indice, tipo,
        tipo <= 25 ? RStr(nome).c_str() : "?", volta, R32(estado + 56), R32(estado + 60), g_acelerando ? " [acelerando]" : "");
}

// Movie:SendCommandCarver(comando, ...) do script (sub_8233EA30 -> sub_8226AAF0(r3, r4 = comando, f1, r6)): as cenas
// das fases (MissionHandler BeginCutScene/EndCutScene, chamados pelas acoes das ancoras de IA: animacao longa do
// Carver com a fala) mandam "BeginCutScene" e "EndCutScene"
GUEST_DECLARE_ORIGINAL(sub_8226AAF0);
GUEST_FUNC(sub_8226AAF0)
{
    const std::string cmd = RStr(Gpr32(ctx, 4));
    if (cmd == "BeginCutScene")
    {
        // a lista de sons e guardada no comeco da cena (nao numa cena emendada logo depois de outra)
        if (!g_acelerando && perf::Now() - g_ultimoFimCena > 1000) { SonsCenaComeca(); g_animCena.clear(); g_trabCena.clear(); }
        g_cenaScript = true; g_cenaInicio = perf::Now();
        if (g_trabalhoAtual) g_trabCena.insert(g_trabalhoAtual);
        if (g_fimQuadros) { g_fimQuadros = 0; g_acelInicio = perf::Now(); Log("[pularcena] outra cena emendada: continua pulando"); }
    }
    else if (cmd == "EndCutScene")
    {
        // a intro do treino sao duas cenas seguidas (EndCutScene, Stand, BeginCutScene no mesmo quadro): espera
        // alguns quadros antes de soltar o relogio para ver se outra cena comeca
        g_cenaScript = false;
        g_ultimoFimCena = perf::Now();
        if (g_acelerando) g_fimQuadros = 10;
    }
    Log("[pularcena] SendCommandCarver(\"%s\") trabalho %08X", cmd.c_str(), g_trabalhoAtual);
    GUEST_ORIGINAL(sub_8226AAF0)(ctx, base);
}

// render.cpp: brilho da imagem final - preta enquanto a cena e adiantada, clareando depois
float BrilhoTela()
{
    if (g_acelerando.load(std::memory_order_relaxed)) return 0.0f;
    const double ini = g_clarearInicio.load(std::memory_order_relaxed);
    if (ini <= 0) return 1.0f;
    const double t = (perf::Now() - ini) / CLAREAR;
    if (t >= 1) { g_clarearInicio = 0; return 1.0f; }
    return (float)(t * t);   // comeca devagar
}
#endif
