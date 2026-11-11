// Far Cry Instincts Predator - recomp: inicializacao
#include "diag.h"
#include "thread.h"
#include "opcoes.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <cstdarg>
#include <chrono>
#include <intrin.h>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

extern std::filesystem::path g_gameDir;
extern std::filesystem::path g_saveDir;
extern uint64_t g_tscFrequency;
bool MemoryInit();

uint32_t g_xexHeader = 0;

// ponto de entrada: GUEST_ENTRY_POINT (bridge/guest_cpu_backend.h, um por programa: jogo ou editor de mapas)

// ---------------------------------------------------------------- log

static FILE* g_logFile = nullptr;
static std::mutex g_logMutex;

void Log(const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    std::lock_guard lock(g_logMutex);
    printf("%s\n", buf);
    if (g_logFile) { fprintf(g_logFile, "%s\n", buf); fflush(g_logFile); }
}

bool g_trace = false;

// ---------------------------------------------------------------- tempo por funcao do sistema (FCRECOMP_KTEMPO)
bool g_kfuncTiming = getenv("FCRECOMP_KTEMPO") != nullptr;
namespace
{
    struct KTime { uint64_t calls = 0; int64_t ticks = 0; };
    std::mutex g_kTimeMutex;
    std::unordered_map<const char*, KTime> g_kTime;
    int64_t Ticks() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
    int64_t TicksPerSecond() { static const int64_t f = [] { LARGE_INTEGER t; QueryPerformanceFrequency(&t); return t.QuadPart; }(); return f; }
    const int64_t g_kStart = Ticks();
    // janela "inicio:fim" em segundos desde a abertura do programa
    double KWindow(int part)
    {
        static const double v[2] = { getenv("FCRECOMP_KTEMPO") ? atof(getenv("FCRECOMP_KTEMPO")) : 0.0,
                                     getenv("FCRECOMP_KTEMPO") && strchr(getenv("FCRECOMP_KTEMPO"), ':') ? atof(strchr(getenv("FCRECOMP_KTEMPO"), ':') + 1) : 1e9 };
        return v[part];
    }
}

KFuncTimer::KFuncTimer(const char* n) : name(n), start(Ticks()) {}

KFuncTimer::~KFuncTimer()
{
    const int64_t now = Ticks();
    const double sec = double(now - g_kStart) / double(TicksPerSecond());
    if (sec < KWindow(0)) return;
    std::lock_guard lock(g_kTimeMutex);
    static bool reported = false;
    if (reported) return;
    if (sec <= KWindow(1))
    {
        auto& k = g_kTime[name];
        k.calls++;
        k.ticks += now - start;
        return;
    }
    reported = true;
    std::vector<std::pair<const char*, KTime>> rows(g_kTime.begin(), g_kTime.end());
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second.ticks > b.second.ticks; });
    Log("[ktempo] funcoes do sistema entre %.1f e %.1f s (soma de todas as threads):", KWindow(0), KWindow(1));
    for (size_t i = 0; i < rows.size() && i < 25; i++)
        Log("[ktempo] %8.1f ms %8llu chamadas  %s", rows[i].second.ticks * 1000.0 / TicksPerSecond(),
            (unsigned long long)rows[i].second.calls, rows[i].first);
    void ReportCsStats();   // kernel.cpp
    ReportCsStats();
}

void TraceCall(const char* name, GuestCpu& ctx)
{
    // FCRECOMP_TRACE=Net,Xam: so as funcoes cujo nome contem um desses textos (=1: todas)
    static const std::vector<std::string> filtro = [] {
        std::vector<std::string> v;
        std::string t = getenv("FCRECOMP_TRACE") ? getenv("FCRECOMP_TRACE") : "";
        for (size_t i = 0; i <= t.size();) { size_t j = t.find(',', i); if (j == std::string::npos) j = t.size(); if (j > i && t.substr(i, j - i) != "1") v.push_back(t.substr(i, j - i)); i = j + 1; }
        return v;
    }();
    if (!filtro.empty() && std::none_of(filtro.begin(), filtro.end(), [&](const std::string& f) { return strstr(name, f.c_str()) != nullptr; })) return;
    Log("[trace] t%u %s(%08X, %08X, %08X, %08X) lr=%08X", CurrentThreadId(), name,
        Gpr32(ctx, 3), Gpr32(ctx, 4), Gpr32(ctx, 5), Gpr32(ctx, 6), (uint32_t)Lr(ctx));
}

// ---------------------------------------------------------------- funcoes do sistema por nome

static std::unordered_map<std::string, GuestFunc*>& ExportTable()
{
    static std::unordered_map<std::string, GuestFunc*> table;   // preenchida pelos construtores estaticos do KFUNC
    return table;
}

GuestExport::GuestExport(const char* name, GuestFunc* fn) { ExportTable()[name] = fn; }

GuestFunc* GuestExport::Find(const std::string& name)
{
    auto it = ExportTable().find(name);
    return it != ExportTable().end() ? it->second : nullptr;
}

// ---------------------------------------------------------------- erros

static GuestFunc* g_lastHost = nullptr;

static void MissingFunction(GuestCpu& ctx, uint8_t*)
{
    Log("[ERRO] chamada indireta para funcao inexistente: ctr=%08X lr=%08llX", Ctr32(ctx), Lr(ctx));
    char key[64], det[64];
    snprintf(key, sizeof(key), "chamada para funcao inexistente %08X", Ctr32(ctx));
    snprintf(det, sizeof(det), "chamada de %08X", (uint32_t)Lr(ctx));
    diag::Note(diag::Crash, key, det);
    diag::WriteReport("funcao inexistente");
    ExitProcess(1);
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* e)
{
    auto* rec = e->ExceptionRecord;
    if (rec->ExceptionCode == 0xE06D7363)   // excecao C++ (ExTerminateThread)
        return EXCEPTION_CONTINUE_SEARCH;
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
    {
        uint64_t addr = rec->ExceptionInformation[1];
        bool guest = addr >= (uint64_t)g_base && addr < (uint64_t)g_base + GUEST_MEMORY_SIZE;
        Log("[CRASH] acesso invalido (%s) em %s%08llX, codigo PC rip=%p (RVA %llX), thread %u",
            rec->ExceptionInformation[0] ? "escrita" : "leitura",
            guest ? "memoria do jogo 0x" : "PC 0x", guest ? addr - (uint64_t)g_base : addr,
            rec->ExceptionAddress, (uint64_t)rec->ExceptionAddress - (uint64_t)GetModuleHandleW(nullptr), CurrentThreadId());
    }
    else
    {
        Log("[CRASH] excecao %08X em %p, thread %u", rec->ExceptionCode, rec->ExceptionAddress, CurrentThreadId());
    }
    if (auto t = CurrentGuestThread(); t && t->ctx)
    {
        auto& c = *t->ctx;
        Log("[CRASH] thread '%s' lr=%08X r1=%08X r3=%08X r4=%08X r5=%08X r30=%08X r31=%08X",
            t->name, (uint32_t)Lr(c), Gpr32(c, 1), Gpr32(c, 3), Gpr32(c, 4), Gpr32(c, 5), Gpr32(c, 30), Gpr32(c, 31));
        // pilha do jogo: procura enderecos de retorno (back chain do PowerPC)
        uint32_t sp = Gpr32(c, 1);
        std::string chain;
        for (int i = 0; i < 12 && sp >= 0x10000000 && sp < 0x80000000; i++)
        {
            uint32_t next = R32(sp);
            if (next <= sp) break;
            uint32_t ret = R32(next - 8);
            char buf[16];
            snprintf(buf, sizeof(buf), " %08X", ret);
            chain += buf;
            sp = next;
        }
        Log("[CRASH] retornos na pilha:%s", chain.c_str());

        // FCRECOMP_CRASHPEEK=endereco: mostra 24 ponteiros a partir de [endereco] (depuracao)
        if (const char* peek = getenv("FCRECOMP_CRASHPEEK"))
        {
            uint32_t table = R32((uint32_t)strtoul(peek, nullptr, 16));
            std::string s;
            for (int i = 0; i < 24; i++) { char b[16]; snprintf(b, sizeof(b), " %08X", R32(table + i * 4)); s += b; }
            Log("[CRASH] [%s] = %08X ->%s", peek, table, s.c_str());
        }
        char key[96];
        snprintf(key, sizeof(key), "excecao %08X na thread '%s' (lr=%08X)", (unsigned)rec->ExceptionCode, t->name, (uint32_t)Lr(c));
        diag::Note(diag::Crash, key, "retornos na pilha:" + chain);
    }
    else
    {
        char key[96];
        snprintf(key, sizeof(key), "excecao %08X em %p (fora das threads do jogo)", (unsigned)rec->ExceptionCode, rec->ExceptionAddress);
        diag::Note(diag::Crash, key);
    }
    diag::WriteReport("travamento");
    RedeEncerrar();
    ExitProcess(1);
}

// ---------------------------------------------------------------- variaveis do kernel importadas pelo jogo

// Onde fica o registro de cada importacao do xboxkrnl.exe no executavel (ordinal -> endereco), lido do cabecalho
// XEX: chave 0x103FF = {u32 tamanho, u32 tamanho dos nomes, u32 n. de bibliotecas, nomes, bibliotecas}; cada
// biblioteca = {u32 tamanho, ..., +0x26 u16 n. de registros, +0x28 enderecos}. O registro guarda (tipo << 24 |
// biblioteca << 16 | ordinal). Antes os enderecos eram fixos do default.xex; o editor de mapas (ige.xex) usa outros.
static std::unordered_map<uint32_t, uint32_t> KernelImportSlots()
{
    std::unordered_map<uint32_t, uint32_t> slots;
    const uint32_t h = g_xexHeader;
    uint32_t imp = 0;
    for (uint32_t i = 0, n = R32(h + 0x14); i < n && i < 64; i++)
        if (R32(h + 0x18 + i * 8) == 0x103FF) imp = h + R32(h + 0x18 + i * 8 + 4);
    if (!imp) return slots;
    const uint32_t nomes = R32(imp + 4), nLib = R32(imp + 8);
    std::vector<std::string> libs;
    for (uint32_t p = imp + 12; p < imp + 12 + nomes && libs.size() < nLib;)
    {
        std::string s = (const char*)G(p);
        p += (uint32_t)((s.size() + 1 + 3) & ~3u);
        libs.push_back(s);
    }
    uint32_t lib = imp + 12 + nomes;
    for (uint32_t l = 0; l < nLib; l++)
    {
        const uint32_t tam = R32(lib), n = R16(lib + 0x26);
        // o indice da biblioteca no registro aponta para esta lista de nomes
        for (uint32_t k = 0; k < n; k++)
        {
            const uint32_t rec = R32(lib + 0x28 + k * 4);
            uint32_t v = R32(rec);
            // no image.bin antigo do desenvolvimento (game\image.bin) os registros estao com os bytes invertidos
            if ((v >> 24) > 1) v = __builtin_bswap32(v);
            if (rec >= GUEST_CODE_BASE) continue;   // atalhos de funcao (no codigo); as variaveis ficam antes
            if ((v >> 24) == 0 && ((v >> 16) & 0xFF) < libs.size() && _stricmp(libs[(v >> 16) & 0xFF].c_str(), "xboxkrnl.exe") == 0)
                slots.emplace(v & 0xFFFF, rec);
        }
        lib += tam;
    }
    return slots;
}

static void SetupKernelVariables()
{
    struct Var { uint32_t ordinal; const char* name; uint32_t size; };
    const Var lista[] = {
        { 0x0AD, "KeTimeStampBundle", 0x20 },
        { 0x1AE, "ExLoadedCommandLine", 0x100 },
        { 0x01B, "ExThreadObjectType", 0x40 },
        { 0x059, "KeDebugMonitorData", 0x100 },
        { 0x193, "XexExecutableModuleHandle", 4 },
        { 0x1BF, "VdGlobalXamDevice", 4 },
        { 0x1BE, "VdGlobalDevice", 4 },
        { 0x266, "KeCertMonitorData", 0x100 },
        { 0x158, "XboxKrnlVersion", 8 },
        { 0x1C1, "VdHSIOCalibrationLock", 0x1C },
        { 0x1C0, "VdGpuClockInMHz", 4 },
    };
    const auto slots = KernelImportSlots();
    struct Slot { uint32_t slot; const char* name; uint32_t size; };
    std::vector<Slot> vars;
    for (auto& v : lista)
        if (auto it = slots.find(v.ordinal); it != slots.end()) vars.push_back({ it->second, v.name, v.size });
    Log("[kernel] %zu variaveis do sistema importadas", vars.size());

    for (auto& v : vars)
    {
        uint32_t addr = GuestAlloc(v.size);
        W32(v.slot, addr);
        std::string n = v.name;

        if (n == "ExLoadedCommandLine")
            strcpy((char*)G(addr), "\"" GUEST_XEX_NAME "\"");
        else if (n == "XboxKrnlVersion")
        {
            W16(addr, 2); W16(addr + 2, 0); W16(addr + 4, 17559); W16(addr + 6, 0);
        }
        else if (n == "VdGpuClockInMHz")
            W32(addr, 500);
        else if (n == "XexExecutableModuleHandle")
        {
            // LDR_DATA_TABLE_ENTRY falso: +0x58 aponta para o cabecalho XEX
            uint32_t ldr = GuestAlloc(0x100);
            W32(ldr + 0x58, g_xexHeader);
            W32(addr, ldr);
        }
        else if (n == "KeTimeStampBundle")
        {
            // atualizado a cada 1 ms
            std::thread([addr]()
            {
                auto start = std::chrono::steady_clock::now();
                for (;;)
                {
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                    uint64_t HoraDoJogo();   // kernel.cpp
                    W64(addr + 0x00, (uint64_t)ms * 10000);
                    W64(addr + 0x08, HoraDoJogo());
                    W32(addr + 0x10, (uint32_t)ms);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }).detach();
        }
    }
}

// ---------------------------------------------------------------- inicio

static std::vector<uint8_t> ReadAll(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

int main(int argc, char** argv)
{
    timeBeginPeriod(1);   // relogio do Windows em 1 ms (o padrao de ~15,6 ms deixava toda espera curta lenta)
    // O programa ajusta o tamanho das janelas a escala de tela do Windows (125%, 150%...). Sem avisar isso, o Windows
    // desenhava o launcher e a janela de controles em 100% e esticava a imagem: letras borradas. A janela do jogo passa
    // a ter o tamanho escolhido em pixels de verdade.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // "PredatorGame.exe --controles": so a janela de configuracao dos controles (Configurar controles.bat)
    if (argc > 1 && std::string(argv[1]) == "--controles")
    {
        void ControlesAbrirJanela(bool esperarFechar);
        g_logFile = fopen((std::filesystem::path(argv[0]).parent_path() / "controles_log.txt").string().c_str(), "w");
        Log("janela de controles");
        ControlesAbrirJanela(true);
        Log("janela fechada");
        return 0;
    }
    std::filesystem::path exeDir;
    {
        wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
        exeDir = std::filesystem::path(p).parent_path();
    }
    // "PredatorGame.exe --upnp-teste": confere o UPnP do roteador (IP de internet; abre as portas do jogo e fecha de
    // novo). Resultado em upnp_teste.txt ao lado do exe (e no cmd de quem chamou).
    if (argc > 1 && std::string(argv[1]) == "--upnp-teste")
    {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) { FILE* f; freopen_s(&f, "CONOUT$", "w", stdout); }
        g_logFile = _wfopen((exeDir / L"upnp_teste.txt").c_str(), L"w");
        void UpnpAbrirPortasEsperar();
        void UpnpFecharPortas();
        std::string ip, erro, aviso;
        if (UpnpIpExterno(ip, erro, &aviso)) Log("[upnp] IP de internet: %s%s%s", ip.c_str(), aviso.empty() ? "" : "\n[upnp] ATENCAO: ", aviso.c_str());
        else Log("[upnp] IP de internet: %s", erro.c_str());
        UpnpAbrirPortasEsperar();
        UpnpFecharPortas();
        if (g_logFile) fclose(g_logFile);
        return 0;
    }
    // "PredatorGame.exe --criar-imagem <pasta do jogo>": so cria o image.bin (teste do xex_imagem.cpp)
    if (argc > 2 && std::string(argv[1]) == "--criar-imagem")
    {
        // programa de janela: escreve no cmd de quem chamou, se houver
        if (AttachConsole(ATTACH_PARENT_PROCESS)) { FILE* f; freopen_s(&f, "CONOUT$", "w", stdout); }
        std::string erro;
        bool ok = XexCriarImagem(std::filesystem::path(argv[2]) / GUEST_XEX_NAME, exeDir / GUEST_IMAGE_FILE, erro);
        printf("%s\n", ok ? "image.bin criado" : erro.c_str());
        return ok ? 0 : 1;
    }
    // "--extrair-imagem <arquivo.xex> <saida>": imagem de qualquer executavel, sem conferir (depuracao; usado para
    // tirar a imagem do ige.xex, o editor de mapas, antes de traduzi-lo)
    if (argc > 3 && std::string(argv[1]) == "--extrair-imagem")
    {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) { FILE* f; freopen_s(&f, "CONOUT$", "w", stdout); }
        g_logFile = stdout;
        std::string erro;
        bool ok = XexCriarImagem(argv[2], argv[3], erro, false);
        printf("%s\n", ok ? "imagem criada" : erro.c_str());
        return ok ? 0 : 1;
    }
    // Launcher (launcher.cpp): aparece ao abrir o executavel com dois cliques. Nao aparece nas execucoes de teste
    // (pasta do jogo na linha de comando ou qualquer variavel FCRECOMP_*), nem com "abrir direto" (Shift mostra).
    OpcoesCarregar(exeDir);
    bool modoTeste = argc > 1;
    for (char** e = _environ; e && *e && !modoTeste; e++) modoTeste = strncmp(*e, "FCRECOMP_", 9) == 0;
    // troca de fase (xam.cpp, XamLoaderLaunchTitle): o jogo se reabre com PREDATOR_REINICIO; o launcher nao aparece
    const bool reinicio = getenv("PREDATOR_REINICIO") != nullptr;
    if (!modoTeste && !reinicio)
    {
        auto& op = OpcoesAtuais();
        std::error_code ec;
        bool pastaOk = std::filesystem::exists(op.pastaJogo / "default.xex", ec);
        bool imagemOk = std::filesystem::file_size(exeDir / GUEST_IMAGE_FILE, ec) == GUEST_IMAGE_SIZE;
        if (!op.pularLauncher || !pastaOk || !imagemOk || (GetAsyncKeyState(VK_SHIFT) & 0x8000))
            if (!LauncherMostrar(exeDir)) return 0;
    }
    // O executavel e um programa de janela (sem cmd). A janela de registro so abre com a opcao do launcher ou com
    // FCRECOMP_CONSOLE=1; o registro completo fica sempre em fcrecomp_log.txt.
    if ((!modoTeste && OpcoesAtuais().mostrarRegistro) || getenv("FCRECOMP_CONSOLE"))
    {
        AllocConsole();
        FILE* f; freopen_s(&f, "CONOUT$", "w", stdout);
        SetConsoleOutputCP(CP_UTF8);
    }
    if (modoTeste)
    {
        // testes: janela 1280x720, imagem sem filtro e resolucao interna 1x, como antes do launcher (prints e medicoes
        // comparaveis). FCRECOMP_AA=1-3 e FCRECOMP_ESCALA=1.5/2 testam. (Antes isto era o "senao" da janela de registro
        // e valia tambem para quem joga sem ela: o tamanho da janela e o antisserrilhado do launcher eram ignorados.)
        OpcoesAtuais().janela = 1; OpcoesAtuais().telaCheia = false; OpcoesAtuais().antisserrilhado = getenv("FCRECOMP_AA") ? atoi(getenv("FCRECOMP_AA")) : 0;
        OpcoesAtuais().escala = 0; OpcoesAtuais().sombras = 0;   // FCRECOMP_SOMBRAS=2|3 testa
        OpcoesAtuais().modoTeste = true;
    }
    // pasta do jogo: linha de comando ou a escolhida no launcher (opcoes.ini; os testes usam a mesma)
    g_gameDir = argc > 1 ? std::filesystem::path(argv[1]) : OpcoesAtuais().pastaJogo;
    g_saveDir = PastaDoPerfil(exeDir, OpcoesAtuais().perfil);   // perfil escolhido no launcher (item 32); o principal = save
    // FCRECOMP_SAVEDIR=pasta: saves em outra pasta (tools\carregar_bug.ps1 usa build\save_teste para nao mexer nos saves de quem joga)
    if (const char* sd = getenv("FCRECOMP_SAVEDIR")) g_saveDir = std::filesystem::path(sd);
    // teste de LAN com duas copias no mesmo PC (PREDATOR_LAN_IP, rede.cpp): cada uma com o seu registro
    std::string nomeLog = "fcrecomp_log.txt";
    if (const char* lan = getenv("PREDATOR_LAN_IP")) nomeLog = std::string("fcrecomp_log_lan_") + (strrchr(lan, '.') ? strrchr(lan, '.') + 1 : lan) + ".txt";
    g_logFile = fopen((exeDir / nomeLog).string().c_str(), "w");

    g_trace = getenv("FCRECOMP_TRACE") != nullptr;
    SetUnhandledExceptionFilter(CrashHandler);
    AddVectoredExceptionHandler(0, [](EXCEPTION_POINTERS* e) -> LONG
    {
        if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
            CrashHandler(e);
        return EXCEPTION_CONTINUE_SEARCH;
    });

    Log(GUEST_TITULO);

    if (const char* file = getenv("FCRECOMP_XMATEST"))
    {
        int RunXmaTest(const char* path);
        return RunXmaTest(file);
    }
    if (const char* dir = getenv("FCRECOMP_SHADERTEST"))
    {
        int RunShaderTest(const char* dir);
        return RunShaderTest(dir);
    }
    Log("pasta do jogo: %s", g_gameDir.string().c_str());
    Log("jogos salvos: %s (perfil %s)", g_saveDir.string().c_str(), OpcoesAtuais().perfil.empty() ? "principal" : OpcoesAtuais().perfil.c_str());

    if (!MemoryInit()) { Log("falha ao reservar 4 GB de memoria virtual"); return 1; }

    // imagem do jogo: criada a partir do default.xex da copia do jogo na primeira vez (xex_imagem.cpp)
    auto image = ReadAll(exeDir / GUEST_IMAGE_FILE);
    if (image.size() != GUEST_IMAGE_SIZE)
    {
        std::string erro;
        if (XexCriarImagem(g_gameDir / GUEST_XEX_NAME, exeDir / GUEST_IMAGE_FILE, erro)) image = ReadAll(exeDir / GUEST_IMAGE_FILE);
        else Log("nao deu para criar o image.bin: %s", erro.c_str());
    }
    if (image.size() != GUEST_IMAGE_SIZE) { Log("image.bin ausente ou com tamanho errado (%zu)", image.size()); return 1; }
    memcpy(G(GUEST_IMAGE_BASE), image.data(), image.size());

    // cabecalho XEX (para RtlImageXexHeaderField)
    auto xex = ReadAll(g_gameDir / GUEST_XEX_NAME);
    if (xex.size() < 0x18) { Log("%s nao encontrado na pasta do jogo", GUEST_XEX_NAME); return 1; }
    uint32_t hdrSize = __builtin_bswap32(*(uint32_t*)&xex[8]);
    g_xexHeader = GuestAlloc(hdrSize);
    memcpy(G(g_xexHeader), xex.data(), hdrSize);

    // tabela de funcoes para chamadas indiretas
    for (uint64_t a = GUEST_CODE_BASE; a < GUEST_CODE_BASE + GUEST_CODE_SIZE; a += 4)
        GuestFunctionSlot((uint32_t)a) = MissingFunction;
    size_t count = 0;
    ForEachGuestFunction([&](uint32_t address, GuestFunc* fn) { GuestFunctionSlot(address) = fn; count++; });
    Log("%zu funcoes registradas", count);

    // frequencia do relogio (mftb usa rdtsc)
    {
        auto t0 = std::chrono::steady_clock::now();
        uint64_t c0 = __rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        uint64_t c1 = __rdtsc();
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        g_tscFrequency = (uint64_t)((c1 - c0) / s);
        Log("frequencia do relogio: %llu Hz", g_tscFrequency);
    }

    void RoteiroIniciar();   // input.cpp: roteiro gravado (gravar desde a abertura / repetir; acerta a hora do jogo)
    RoteiroIniciar();
    SetupKernelVariables();

    void AudioPrepare();   // audio.cpp: abre a saida de som ja (leva ~0,4 s; antes atrasava o som do video de abertura)
    AudioPrepare();
    void GpuStart();
    GpuStart();
    void GpuWaitReady();   // gpu.cpp: o jogo so comeca depois da preparacao da placa (tela com barra de progresso)
    GpuWaitReady();

    // FCRECOMP_DUMP=N: a cada N segundos grava a pilha de todas as threads no log
    if (const char* dump = getenv("FCRECOMP_DUMP"))
    {
        int secs = std::max(1, atoi(dump));
        std::thread([secs]()
        {
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(secs));
                DumpAllThreads();
            }
        }).detach();
    }

    Log("iniciando o %s em %08X", GUEST_XEX_NAME, GUEST_ENTRY_POINT);
    auto mainThread = CreateGuestThread(GUEST_ENTRY_POINT, 0, 0, 0x100000, false);
    WaitObjects({ mainThread }, true, -1);
    Log("thread principal terminou");
    return 0;
}
