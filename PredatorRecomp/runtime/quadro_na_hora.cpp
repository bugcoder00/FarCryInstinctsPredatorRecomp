// Quadro na hora: tira a espera entre ler a entrada (mouse) e mostrar a imagem.
//
// Medido em 25/11 com marcas de tempo (FCRECOMP_CAMTARDIALOG=2), 30 FPS:
//  - editor (ige.xex): o passo do jogo comeca logo depois do Present anterior, le o mouse, a thread de desenho do jogo
//    pede a troca (VdSwap) ~6-9 ms depois e a placa emulada fica ~25 ms parada num WAIT_REG_MEM esperando o vblank que
//    libera a troca (o jogo pede 1 troca a cada 2 vblanks, gpu.cpp FPS_TROCA). Leitura -> Present: ~33 ms.
//  - jogo: a thread de desenho trabalha um passo atras da do jogo (copia o estado no comeco do quadro dela, antes da
//    mira do passo em andamento: e o desenho em paralelo do motor, nao da para tirar sem reescreve-lo); o VdSwap dela
//    fica parado ate o Present anterior e a placa ainda espera ~22 ms o vblank. Leitura -> Present: ~80 ms.
// O ritmo (30/60 FPS) vinha da espera de vblank DEPOIS do quadro pronto. Durante o jogo (o passo dos personagens rodou
// ha menos de 0,2 s) a troca agora sai na hora (como "sem limite") e o ritmo e feito aqui, ANTES de ler a entrada: o
// passo do jogo dorme ate a hora do proximo quadro (intervalo pedido pelo jogo x 1/60 s; 60 FPS: 1/60 s). Fora do jogo
// (menus, videos, carregamento) nada muda. Se o passo atrasa mais de 2 ms, o ritmo recomeca dali (nao corre atras).
// Resultado (leitura -> Present do quadro que a mostra, roteiros 2026-11-25_14-47 e _14-10, 30 FPS):
//  - editor: 33 -> 11 ms, trocas regulares (33,3 ms, maior ~37). LIGADO por padrao.
//  - jogo: 81 -> 46 ms, mas sem a espera do vblank a variacao da thread de desenho (o VdSwap dela so sai depois do
//    PROXIMO passo do jogo) aparece: trocas a cada 35,5 ms em media com picos de 70-87 ms (o original: 33,3, maior
//    35). Dormir menos com a troca no vblank (folga medida) tambem perdia vblanks a partir de ~10 ms. DESLIGADO por
//    padrao no jogo (FCRECOMP_QUADRONAHORA=1 liga para testar).
// FCRECOMP_QUADRONAHORA=0/1 desliga/liga. FCRECOMP_QUADROLOG=1 anota a cada ~2 s (ritmo, trocas, passos atrasados).
#include "guest.h"
#include "perf.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>

int ModoFps();   // gpu.cpp: 0 = 30, 1 = 60, 2 = sem limite
void CamSeqMarca(const char* o, uint64_t n);   // camera_tardia.cpp (FCRECOMP_CAMTARDIALOG=2: hora de cada etapa)

namespace
{
#ifdef PREDATOR_EDITOR
    constexpr bool PADRAO = true;
#else
    constexpr bool PADRAO = false;   // jogo: so com FCRECOMP_QUADRONAHORA=1 (ver acima)
#endif
    const bool g_ligado = getenv("FCRECOMP_QUADRONAHORA") ? atoi(getenv("FCRECOMP_QUADRONAHORA")) != 0 : PADRAO;
    const bool g_log = getenv("FCRECOMP_QUADROLOG") != nullptr;

    std::atomic<double> g_ultimoPasso{ 0 };   // hora do ultimo passo dos personagens (jogo rodando)
    std::atomic<int> g_intervalo{ 0 };        // vblanks por troca pedidos pelo jogo (0 = sem ritmo)
    double g_alvo = 0;                        // hora marcada para o proximo passo (thread do jogo)
    // registro
    std::mutex g_m;
    double g_ultimaTroca = 0, g_somaSono = 0, g_somaTrocas = 0, g_maiorTroca = 0; int g_nPassos = 0, g_nTrocas = 0, g_atrasos = 0;

    void DormirAte(double alvo)   // perf::Now() em ms
    {
        static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        const double falta = alvo - perf::Now();
        if (falta > 1.5 && timer)
        {
            LARGE_INTEGER t; t.QuadPart = -(LONGLONG)((falta - 1.0) * 10000.0);   // unidades de 100 ns, relativo
            if (SetWaitableTimer(timer, &t, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
        }
        while (perf::Now() < alvo) YieldProcessor();
    }
    bool Jogando() { return perf::Now() - g_ultimoPasso.load() < 200.0; }
}

// gpu.cpp FPS_TROCA (cada troca): intervalo = vblanks por troca que valeriam. true = a troca deve sair na hora
// (o ritmo fica com QuadroNaHoraEsperar)
bool QuadroTrocaImediata(uint32_t intervalo)
{
    g_intervalo = (int)intervalo;
    return g_ligado && intervalo != 0 && Jogando();
}

// gpu.cpp: troca de imagem processada pela placa (XE_SWAP): so registro
void QuadroTroca()
{
    CamSeqMarca("troca", 0);
    if (!g_log) return;
    const double agora = perf::Now();
    std::lock_guard l(g_m);
    if (g_ultimaTroca > 0 && Jogando())
    {
        const double d = agora - g_ultimaTroca;
        g_somaTrocas += d; g_nTrocas++; g_maiorTroca = std::max(g_maiorTroca, d);
    }
    g_ultimaTroca = agora;
}

// camera_tardia.cpp: comeco do passo (atualizacao dos personagens), thread do jogo, antes de ler a entrada
void QuadroNaHoraEsperar()
{
    const double agora = perf::Now();
    const bool jogava = Jogando();
    g_ultimoPasso = agora;
    const int iv = g_intervalo.load();
    const double periodo = iv * (1000.0 / 60.0);
    if (g_ligado)
    {
        if (!jogava || periodo <= 0 || ModoFps() == 2) { g_alvo = agora; return; }
        g_alvo += periodo;
        if (agora > g_alvo + 2.0) { g_alvo = agora; g_atrasos++; }   // atrasou: o ritmo recomeca daqui (recuperar o atraso
                                                                     // no passo seguinte deu mais atraso: 74 ms no jogo)
        else if (g_alvo > agora) DormirAte(g_alvo);
    }
    if (g_log)
    {
        std::lock_guard l(g_m);
        g_somaSono += perf::Now() - agora;
        if (++g_nPassos % 60 == 0)
        {
            Log("[quadro] na hora: dormiu %.1f ms por passo (ritmo %.1f ms), trocas a cada %.1f ms (maior %.1f), %d passos atrasados",
                g_somaSono / 60, periodo, g_nTrocas ? g_somaTrocas / g_nTrocas : 0.0, g_maiorTroca, g_atrasos);
            g_somaSono = g_somaTrocas = g_maiorTroca = 0; g_nTrocas = g_atrasos = 0;
        }
    }
}

// ---------------------------------------------------------------- desenho do passo atual (so o jogo)
// No jogo a thread de desenho comeca o quadro dela (copias da camera, sub_825C3748 de 825C585C) junto com o passo do
// jogo, antes da mira dele: desenhava o passo anterior (e a camera do desenho, copiada mais tarde de 825CE79C, as vezes
// ja era a nova: o original alternava 1 e 2 quadros de atraso). Agora, na primeira copia do quadro, ela espera o passo
// em andamento montar a camera (passo 4 de camera_tardia.cpp; so se o passo anterior tambem montou). Depois disso ela
// ainda ficava ~20 ms parada esperando o Present anterior, entao a espera cabe nessa folga.
// Limite da espera 12 ms (FCRECOMP_DESENHOLIMITE=ms): com 30 ms a espera chegava a ~20 ms e perdia um vblank a cada ~2 s
// (trocas a cada 33,9 ms, picos de 50); com 12 ms o ritmo fica como o original (33,3 ms) e ~10% dos quadros passam do
// limite e desenham o passo anterior (como antes). Roteiro 2026-11-25_14-10, leitura -> Present: ~53 -> 31-38 ms.
// FCRECOMP_DESENHONOVO=0 desliga.
namespace
{
    const bool g_desenhoNovo = !getenv("FCRECOMP_DESENHONOVO") || atoi(getenv("FCRECOMP_DESENHONOVO")) != 0;
    std::mutex g_dm;
    std::condition_variable g_dcv;
    uint64_t g_passos = 0, g_passos4 = 0;   // passos comecados / que montaram a camera
    bool g_esperar = false;                  // o passo em andamento deve montar a camera
    double g_somaEsp = 0, g_maiorEsp = 0; int g_nEsp = 0, g_estouros = 0;
}

// camera_tardia.cpp: comeco do passo (thread do jogo)
void QuadroPassoComecou()
{
    std::lock_guard l(g_dm);
    g_esperar = g_passos4 == g_passos;
    g_passos++;
}

// camera_tardia.cpp: o passo montou a camera do desenho (thread do jogo)
void QuadroPasso4()
{
    {
        std::lock_guard l(g_dm);
        g_passos4 = g_passos;
    }
    g_dcv.notify_all();
}

// camera_tardia.cpp: thread de desenho vai copiar a camera no comeco do quadro dela
void QuadroDesenhoComeco()
{
    if (!g_desenhoNovo) return;
    const double ini = perf::Now();
    std::unique_lock l(g_dm);
    if (!g_esperar || g_passos4 >= g_passos) return;
    static const int limite = getenv("FCRECOMP_DESENHOLIMITE") ? atoi(getenv("FCRECOMP_DESENHOLIMITE")) : 12;
    if (!g_dcv.wait_for(l, std::chrono::milliseconds(limite), [] { return g_passos4 >= g_passos; }))
    {
        g_estouros++;
        g_esperar = false;
    }
    const double esp = perf::Now() - ini;
    g_somaEsp += esp; g_maiorEsp = std::max(g_maiorEsp, esp);
    if (g_log && ++g_nEsp % 60 == 0)
    {
        Log("[quadro] desenho do passo atual: esperou %.1f ms por quadro (maior %.1f), %d esperas passaram do limite", g_somaEsp / 60, g_maiorEsp, g_estouros);
        g_somaEsp = g_maiorEsp = 0; g_estouros = 0;
    }
}
