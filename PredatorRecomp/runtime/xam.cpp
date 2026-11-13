// XAM (sistema do Xbox: usuario, controle, conteudo) e video (Vd*) - versao minima
#include "objects.h"
#include "diag.h"
#include <windows.h>
#include <vector>
#include <algorithm>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
[[noreturn]] void SairDoProcesso(unsigned codigo);   // main.cpp

constexpr uint32_t X_ERROR_DEVICE_NOT_CONNECTED = 0x48F;
constexpr uint32_t X_ERROR_NOT_FOUND = 0x490;
constexpr uint32_t X_ERROR_NO_SUCH_USER = 0x525;

// ---------------------------------------------------------------- sistema

KFUNC(XGetLanguage) { Ret(ctx, 1); }          // ingles
KFUNC(XamGetSystemVersion) { Ret(ctx, 0x20096B00); }
KFUNC(XamGetExecutionId)
{
    static uint32_t id = 0;
    if (!id) id = GuestAlloc(0x18);
    W32(Gpr32(ctx, 3), id);
    Ret(ctx, 0);
}

KFUNC(ExGetXConfigSetting)
{
    uint32_t cat = Gpr32(ctx, 3), setting = Gpr32(ctx, 4), buf = Gpr32(ctx, 5), size = Gpr32(ctx, 6), req = Gpr32(ctx, 7);
    if (buf) memset(G(buf), 0, size);
    Log("[xam] ExGetXConfigSetting %u/%u (%u bytes)", cat, setting, size);
    if (req) W16(req, (uint16_t)size);
    Ret(ctx, STATUS_SUCCESS);
}

// "Bilhete" de reinicio: ao trocar de fase o jogo guarda dados (qual fase carregar) com XamLoaderSetLaunchData e
// pede ao console para reiniciar o proprio jogo (XamLoaderLaunchTitle). Aqui o emulador se reabre (mesma pasta, mesmas
// variaveis) com o bilhete em hexadecimal na variavel PREDATOR_BILHETE (ate 0x1000 bytes = 8192 letras; antes era o
// arquivo bilhete_reinicio.bin na pasta do jogo, que ficava la quando o programa novo nao o lia, ex.: o editor), e na
// volta o jogo o le com XamLoaderGetLaunchData. Sem isso o jogo fechava no fim da fase (bug 6).
// A troca parece uma janela so: o processo novo recebe a posicao/tamanho/tela cheia da janela atual
// (PREDATOR_REINICIO) e avisa pelo evento PREDATOR_EVENTO quando a janela dele esta na tela; so entao este fecha.
// Os nomes PREDATOR_* nao contam como modo teste (antes FCRECOMP_BILHETE fazia o processo novo ignorar as opcoes do
// launcher: janela 1280x720, sem antisserrilhado, resolucao 1x...).
namespace
{
    std::vector<uint8_t> g_launchData;
    bool g_launchLoaded = false;

    void LoadLaunchData()
    {
        if (g_launchLoaded) return;
        g_launchLoaded = true;
        const char* hex = getenv("PREDATOR_BILHETE");
        if (!hex) return;
        auto valor = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
        for (const char* c = hex; c[0] && c[1] && g_launchData.size() < 0x1000; c += 2)
        {
            int hi = valor(c[0]), lo = valor(c[1]);
            if (hi < 0 || lo < 0) { g_launchData.clear(); break; }
            g_launchData.push_back((uint8_t)(hi * 16 + lo));
        }
        Log("[xam] bilhete de reinicio lido (%zu bytes)", g_launchData.size());
        // depuracao: FCRECOMP_LAUNCHZERO=inicio-fim (hex) zera esse trecho dos dados recebidos
        if (const char* z = getenv("FCRECOMP_LAUNCHZERO"))
        {
            size_t a = strtoul(z, nullptr, 16), e = strchr(z, '-') ? strtoul(strchr(z, '-') + 1, nullptr, 16) : a + 1;
            for (size_t i = a; i < e && i < g_launchData.size(); i++) g_launchData[i] = 0;
            Log("[xam] bilhete: %zX-%zX zerado (teste)", a, e);
        }
    }
}

// XamLoaderGetLaunchDataSize(tamanho*)
KFUNC(XamLoaderGetLaunchDataSize)
{
    LoadLaunchData();
    if (g_launchData.empty()) Ret(ctx, X_ERROR_NOT_FOUND);
    else { W32(Gpr32(ctx, 3), (uint32_t)g_launchData.size()); Ret(ctx, 0); }
}

// XamLoaderGetLaunchData(buffer, tamanho)
KFUNC(XamLoaderGetLaunchData)
{
    LoadLaunchData();
    if (g_launchData.empty()) { Ret(ctx, X_ERROR_NOT_FOUND); return; }
    uint32_t n = std::min<uint32_t>(Gpr32(ctx, 4), (uint32_t)g_launchData.size());
    memcpy(G(Gpr32(ctx, 3)), g_launchData.data(), n);
    Log("[xam] XamLoaderGetLaunchData: %u bytes entregues", n);
    Ret(ctx, 0);
}

// XamLoaderSetLaunchData(dados, tamanho)
KFUNC(XamLoaderSetLaunchData)
{
    uint32_t n = std::min<uint32_t>(Gpr32(ctx, 4), 0x1000);
    const uint8_t* d = (const uint8_t*)G(Gpr32(ctx, 3));
    g_launchData.assign(d, d + n);
    g_launchLoaded = true;
    Log("[xam] XamLoaderSetLaunchData: %u bytes", n);
    if (getenv("FCRECOMP_LAUNCHLOG"))   // depuracao: o conteudo (trechos nao zerados)
        for (uint32_t i = 0; i < n; i += 32)
        {
            bool zero = true;
            for (uint32_t k = i; k < i + 32 && k < n; k++) zero &= d[k] == 0;
            if (zero) continue;
            char hex[100] = {}, txt[40] = {};
            for (uint32_t k = i; k < i + 32 && k < n; k++)
            { snprintf(hex + (k - i) * 3, 4, "%02X ", d[k]); txt[k - i] = d[k] >= 32 && d[k] < 127 ? (char)d[k] : '.'; }
            Log("[xam]   +%03X %s %s", i, hex, txt);
        }
    Ret(ctx, 0);
}

// O nome pedido escolhe o programa: "game:\ige.xex" (menu MAP EDITOR) = PredatorEditor.exe (o editor de mapas,
// traduzido a parte); "game:\default.xex" (o editor voltando ao jogo) = PredatorGame.exe; sem nome = este mesmo.
KFUNC(XamLoaderLaunchTitle)
{
    const std::string nome = Gpr32(ctx, 3) ? RStr(Gpr32(ctx, 3)) : std::string();
    wchar_t atual[MAX_PATH];
    GetModuleFileNameW(nullptr, atual, MAX_PATH);
    std::wstring dir = atual;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    std::string minusculo = nome;
    for (char& c : minusculo) c = (char)tolower((unsigned char)c);
    std::wstring exe = atual;
    if (minusculo.find("ige.xex") != std::string::npos) exe = dir + L"PredatorEditor.exe";
    else if (minusculo.find("default.xex") != std::string::npos) exe = dir + L"PredatorGame.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        Log("[xam] XamLoaderLaunchTitle %s: o programa %ls nao existe; reabrindo este", nome.c_str(), exe.c_str());
        exe = atual;
    }
    else Log("[xam] XamLoaderLaunchTitle %s: abrindo %ls", nome.c_str(), exe.substr(dir.size()).c_str());

    // guarda o log desta execucao (o novo processo comeca outro)
    CopyFileW((dir + L"fcrecomp_log.txt").c_str(), (dir + L"fcrecomp_log_antes_reinicio.txt").c_str(), FALSE);

    DeleteFileW((dir + L"bilhete_reinicio.bin").c_str());   // sobra das versoes antigas (antes do item 31)
    if (!g_launchData.empty())
    {
        std::wstring hex;
        hex.reserve(g_launchData.size() * 2);
        for (uint8_t b : g_launchData) { hex += L"0123456789ABCDEF"[b >> 4]; hex += L"0123456789ABCDEF"[b & 15]; }
        SetEnvironmentVariableW(L"PREDATOR_BILHETE", hex.c_str());
    }
    else SetEnvironmentVariableW(L"PREDATOR_BILHETE", nullptr);

    std::wstring JanelaParaReinicio();                              // gpu.cpp
    bool CapturarJanelaParaReinicio(const std::wstring& arquivo);   // gpu.cpp
    void AudioSilenciar();                                          // audio.cpp
    SetEnvironmentVariableW(L"PREDATOR_REINICIO", JanelaParaReinicio().c_str());
    // portas abertas no roteador (rede.cpp/upnp.cpp): o processo novo fica com elas e as fecha quando sair
    SetEnvironmentVariableW(L"PREDATOR_UPNP_ABERTO", RedeReabrirComPortas() ? L"1" : nullptr);
    // n. da troca (roteiros de teste por etapa, input.cpp)
    SetEnvironmentVariableW(L"PREDATOR_ETAPA", std::to_wstring(getenv("PREDATOR_ETAPA") ? atoi(getenv("PREDATOR_ETAPA")) + 1 : 1).c_str());
    // cobre a janela com a tela de carregamento (gpu.cpp); a nova nasce com a mesma tela. Nao ha mais foto (item 30)
    std::wstring tela = dir + L"tela_reinicio.bmp";
    SetEnvironmentVariableW(L"PREDATOR_TELA", CapturarJanelaParaReinicio(tela) ? tela.c_str() : nullptr);
    wchar_t evento[64];
    swprintf(evento, 64, L"PredatorReinicio_%lu", GetCurrentProcessId());
    HANDLE pronto = CreateEventW(nullptr, TRUE, FALSE, evento);
    SetEnvironmentVariableW(L"PREDATOR_EVENTO", evento);

    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    // mesma linha de comando, trocando so o nome do programa (o 1o item)
    std::wstring antiga = GetCommandLineW();
    size_t fim = antiga[0] == L'"' ? antiga.find(L'"', 1) + 1 : antiga.find(L' ');
    std::wstring cmd = L"\"" + exe + L"\"" + (fim == std::wstring::npos || fim > antiga.size() ? L"" : antiga.substr(fim));
    if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        AllowSetForegroundWindow(pi.dwProcessId);   // a janela nova pode vir para a frente
        AudioSilenciar();
        // esta janela fica na tela ate a nova aparecer (no maximo 20 s ou ate o processo novo terminar)
        HANDLE espera[2] = { pronto, pi.hProcess };
        // (sem Log aqui: o processo novo ja abriu o fcrecomp_log.txt de novo)
        WaitForMultipleObjects(2, espera, FALSE, 20000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    else Log("[xam] falha ao reabrir o jogo (erro %lu)", GetLastError());
    SairDoProcesso(0);
}

// ---------------------------------------------------------------- mensagens para os "aplicativos" do sistema
// XMsg* conversa com servicos do console (0xFA tocador de musica, 0xFC Xbox Live, ...). Aqui nao ha Xbox Live:
// responder "sucesso" sem preencher nada fazia o jogo achar, por exemplo, que havia um convite para uma
// sessao ("DO YOU WISH TO JOIN THIS GAME SESSION?" no menu). Todas respondem falha.
constexpr uint32_t X_E_FAIL = 0x80004005;
void SignalEvent(uint32_t evHandle);   // fs.cpp

static void NoteXMsg(const char* fn, uint32_t app, uint32_t msg)
{
    char key[96];
    snprintf(key, sizeof(key), "%s app %02X mensagem %08X (respondida com falha)", fn, app, msg);
    diag::Note(diag::Kernel, key);
}

// mensagens de sessao de partida (SYSTEM LINK): rede.cpp responde as que conhece
bool RedeXMsg(const char* fn, uint32_t app, uint32_t msg, uint32_t buf, uint32_t tam, uint32_t& resultado);

// XMsgInProcessCall(app, mensagem, parametro1, parametro2)
KFUNC(XMsgInProcessCall)
{
    uint32_t r;
    if (RedeXMsg("XMsgInProcessCall", Gpr32(ctx, 3), Gpr32(ctx, 4), Gpr32(ctx, 5), Gpr32(ctx, 6), r)) { Ret(ctx, r); return; }
    NoteXMsg("XMsgInProcessCall", Gpr32(ctx, 3), Gpr32(ctx, 4));
    Ret(ctx, X_E_FAIL);
}

// XMsgStartIORequest[Ex](app, mensagem, overlapped, buffer, tamanho[, extra])
static void FailIORequest(GuestCpu& ctx, const char* fn)
{
    uint32_t r = X_E_FAIL;
    bool tratada = RedeXMsg(fn, Gpr32(ctx, 3), Gpr32(ctx, 4), Gpr32(ctx, 6), Gpr32(ctx, 7), r);
    if (!tratada) NoteXMsg(fn, Gpr32(ctx, 3), Gpr32(ctx, 4));
    if (uint32_t ov = Gpr32(ctx, 5))
    {
        W32(ov + 0, r);          // InternalLow = resultado
        W32(ov + 4, 0);          // InternalHigh = bytes
        W32(ov + 0x18, r);       // erro estendido
        if (uint32_t ev = R32(ov + 12)) SignalEvent(ev);
        if (tratada && r == 0) { Ret(ctx, 0x3E5); return; }   // ERROR_IO_PENDING: o resultado esta no overlapped
    }
    Ret(ctx, r);
}
KFUNC(XMsgStartIORequest) { FailIORequest(ctx, "XMsgStartIORequest"); }
KFUNC(XMsgStartIORequestEx) { FailIORequest(ctx, "XMsgStartIORequestEx"); }
KFUNC(XMsgCancelIORequest) { Ret(ctx, 0); }


// ---------------------------------------------------------------- usuario, perfil, saves e notificacoes: content.cpp


// ---------------------------------------------------------------- controle (por enquanto: nenhum conectado alem do 0, parado)



// ---------------------------------------------------------------- telas do sistema: fingimos que o usuario cancelou

KFUNC(XamShowSigninUI) { Ret(ctx, 0x4C7); }

// ---------------------------------------------------------------- video

static void FillVideoMode(uint32_t p)
{
    memset(G(p), 0, 0x30);
    W32(p + 0x00, 1280);
    W32(p + 0x04, 720);
    W32(p + 0x08, 0);     // entrelacado
    W32(p + 0x0C, 1);     // widescreen
    W32(p + 0x10, 1);     // HD
    float hz = 60.0f;
    W32(p + 0x14, *(uint32_t*)&hz);
    W32(p + 0x18, 1);     // NTSC
    W32(p + 0x1C, 0x4A);
    W32(p + 0x20, 0x01);
}

KFUNC(XGetVideoMode) { FillVideoMode(Gpr32(ctx, 3)); }
KFUNC(VdQueryVideoMode) { FillVideoMode(Gpr32(ctx, 3)); }
KFUNC(VdQueryVideoFlags) { Ret(ctx, 7); }

KFUNC(VdGetCurrentDisplayInformation)
{
    uint32_t p = Gpr32(ctx, 3);
    memset(G(p), 0, 0x58);
    W16(p + 0x00, 1280);
    W16(p + 0x02, 720);
    W16(p + 0x08, 1280);
    W16(p + 0x0A, 720);
    W32(p + 0x40, 1280);
    W32(p + 0x44, 720);
}

KFUNC(VdGetCurrentDisplayGamma)
{
    W32(Gpr32(ctx, 3), 2);
    float g = 2.22222233f;
    W32(Gpr32(ctx, 4), *(uint32_t*)&g);
}

KFUNC(VdIsHSIOTrainingSucceeded) { Ret(ctx, 1); }
KFUNC(VdGetGraphicsAsicID) { Ret(ctx, 0x10); }
KFUNC(VdRetrainEDRAM) { Ret(ctx, 0); }
KFUNC(VdRetrainEDRAMWorker) { Ret(ctx, 0); }
KFUNC(VdInitializeEDRAM) { Ret(ctx, 0); }
KFUNC(VdInitializeEngines) { Ret(ctx, 1); }
KFUNC(VdShutdownEngines) {}
// VdPersistDisplay: o jogo depois libera esse ponteiro com MmFreePhysicalMemory
KFUNC(VdPersistDisplay)
{
    Log("[vd] VdPersistDisplay(%08X, %08X) chamado de %08X", Gpr32(ctx, 3), Gpr32(ctx, 4), (uint32_t)Lr(ctx));
    if (Gpr32(ctx, 4)) W32(Gpr32(ctx, 4), GuestAlloc(64, 32, MemKind::Physical));
    Ret(ctx, 1);
}

// VdInitializeScalerCommandBuffer(... 12 argumentos): preenche dest (arg 11) com NOPs e retorna o tamanho (arg 12)
KFUNC(VdInitializeScalerCommandBuffer)
{
    uint32_t dest = R32(Gpr32(ctx, 1) + 0x54 + 8 * 2);
    uint32_t count = R32(Gpr32(ctx, 1) + 0x54 + 8 * 3);
    for (uint32_t i = 0; i < count; i++)
        W32(dest + i * 4, 0x80000000);
    Ret(ctx, count);
}

KFUNC(VdSetSystemCommandBufferGpuIdentifierAddress) {}
KFUNC(VdCallGraphicsNotificationRoutines) { Ret(ctx, 0); }
KFUNC(VdEnableDisableClockGating) { Ret(ctx, 0); }
KFUNC(VdSetDisplayMode) { Ret(ctx, 0); }

KFUNC(VdGetSystemCommandBuffer)
{
    memset(G(Gpr32(ctx, 3)), 0, 0x94);
    W32(Gpr32(ctx, 3), 0xBEEF0000);
    W32(Gpr32(ctx, 4), 0xBEEF0001);
}

// Ring buffer da GPU: guardamos os enderecos para a fase da GPU
uint32_t g_ringBufferBase = 0, g_ringBufferSizeLog2 = 0, g_ringRptrWriteBack = 0;
uint32_t g_gfxInterruptCallback = 0, g_gfxInterruptContext = 0;

KFUNC(VdInitializeRingBuffer)
{
    void GpuRingInit(uint32_t base, uint32_t sizeLog2);   // gpu.cpp
    GpuRingInit(Gpr32(ctx, 3), Gpr32(ctx, 4));
    Log("[gpu] VdInitializeRingBuffer base=%08X tamanho=2^%u", Gpr32(ctx, 3), Gpr32(ctx, 4));
}

KFUNC(VdEnableRingBufferRPtrWriteBack)
{
    g_ringRptrWriteBack = Gpr32(ctx, 3);
    Log("[gpu] VdEnableRingBufferRPtrWriteBack %08X", Gpr32(ctx, 3));
}

KFUNC(VdSetGraphicsInterruptCallback)
{
    g_gfxInterruptCallback = Gpr32(ctx, 3);
    g_gfxInterruptContext = Gpr32(ctx, 4);
    Log("[gpu] VdSetGraphicsInterruptCallback %08X ctx=%08X", Gpr32(ctx, 3), Gpr32(ctx, 4));
}


// rede (NetDll_*, XNet*): rede.cpp

// ---------------------------------------------------------------- audio: aceita e ignora por enquanto

// XMACreateContext / XMAReleaseContext: xma.cpp
