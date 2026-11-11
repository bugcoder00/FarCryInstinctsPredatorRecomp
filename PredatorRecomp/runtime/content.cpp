// Usuario conectado, perfil e saves (XamUser*, XamContent*, XamEnumerate, seletor de dispositivo).
//
// Ha um unico usuario local ("Jogador", sem Xbox Live) e um "HD" (dispositivo 1). Cada conteudo salvo pelo jogo
// (XCONTENT_DATA: tipo + nome de arquivo) vira uma pasta build\save\conteudo\<tipo>\<nome de arquivo>\, com o nome
// de exibicao em _nome.txt. O XamContentCreate monta essa pasta na raiz que o jogo pede ("savegame:" etc., ver fs.cpp).
// Ajustes do perfil (XamUserWriteProfileSettings) ficam em build\save\perfil\<id>.bin.
// FCRECOMP_SAVELOG=1 mostra no log cada chamada com os argumentos.
#include "idioma.h"
#include "objects.h"
#include "diag.h"
#include "opcoes.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <map>
#include <functional>
#include <cstdarg>
#include <algorithm>

namespace fs = std::filesystem;
extern fs::path g_saveDir;   // fs.cpp / main.cpp: build\save
void MountContentRoot(const std::string& name, const fs::path& path);   // fs.cpp
void UnmountContentRoot(const std::string& name);
void SignalEvent(uint32_t evHandle);                                     // fs.cpp
std::string DescribeThreads();                                           // thread.cpp

uint64_t RedeXuidLocal();   // rede.cpp

namespace
{
    constexpr uint32_t X_ERROR_SUCCESS = 0, X_ERROR_FILE_NOT_FOUND = 2, X_ERROR_ACCESS_DENIED = 5,
        X_ERROR_NO_MORE_FILES = 0x12, X_ERROR_ALREADY_EXISTS = 0xB7, X_ERROR_INSUFFICIENT_BUFFER = 0x7A,
        X_ERROR_IO_PENDING = 0x3E5, X_ERROR_FUNCTION_FAILED = 0x65B, X_ERROR_DEVICE_NOT_CONNECTED = 0x48F,
        X_ERROR_NO_SUCH_USER = 0x525, X_ERROR_CANCELLED = 0x4C7;
    constexpr uint32_t DEVICE_HDD = 1;
    // XUID "offline" (comeca com E), diferente em cada PC (rede.cpp): na LAN cada jogador precisa do seu. Os saves
    // nao guardam o XUID (conferido), entao trocar o antigo fixo (E000000012345678) nao afeta nada.
    #define XUID RedeXuidLocal()
    constexpr uint32_t CONTENT_DATA_SIZE = 0x134;      // XCONTENT_DATA

    bool SaveLog()
    {
        static const bool on = getenv("FCRECOMP_SAVELOG") != nullptr;
        return on;
    }
    // registro das chamadas: sempre as primeiras de cada funcao, todas com FCRECOMP_SAVELOG=1
    void Trace(const char* fn, const char* fmt, ...)
    {
        static std::mutex m;
        static std::map<std::string, int> counts;
        {
            std::lock_guard lock(m);
            if (!SaveLog() && counts[fn]++ >= 3) return;
        }
        char buf[512];
        va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
        Log("[save] %s %s", fn, buf);
    }

    // XAM_OVERLAPPED { resultado; tamanho; contexto; evento; rotina; contexto da rotina; erro estendido }
    uint32_t Complete(uint32_t ov, uint32_t result, uint32_t length = 0)
    {
        if (!ov) return result;
        W32(ov + 0x00, result);
        W32(ov + 0x04, length);
        W32(ov + 0x18, result);
        if (R32(ov + 0x10)) Log("[save] AVISO: overlapped com rotina de conclusao %08X (nao chamada)", R32(ov + 0x10));
        if (uint32_t ev = R32(ov + 0x0C)) SignalEvent(ev);
        return X_ERROR_IO_PENDING;
    }

    std::string ReadWide(uint32_t a, size_t max)
    {
        std::string s;
        for (size_t i = 0; i < max; i++)
        {
            uint16_t c = R16(a + (uint32_t)i * 2);
            if (!c) break;
            s += c < 0x80 ? (char)c : '?';
        }
        return s;
    }
    std::string ReadFixed(uint32_t a, size_t max)
    {
        std::string s;
        for (size_t i = 0; i < max; i++) { char c = (char)R8(a + (uint32_t)i); if (!c) break; s += c; }
        return s;
    }

    // XCONTENT_DATA { u32 dispositivo; u32 tipo; wchar nome_exibicao[128]; char nome_arquivo[42]; }
    struct ContentData { uint32_t device = 0, type = 0; std::string display, file; };
    ContentData ReadContent(uint32_t a)
    {
        ContentData c;
        c.device = R32(a); c.type = R32(a + 4);
        c.display = ReadWide(a + 8, 128);
        c.file = ReadFixed(a + 8 + 256, 42);
        return c;
    }
    void WriteContent(uint32_t a, const ContentData& c)
    {
        memset(G(a), 0, CONTENT_DATA_SIZE);
        W32(a, c.device); W32(a + 4, c.type);
        for (size_t i = 0; i < c.display.size() && i < 127; i++) W16(a + 8 + (uint32_t)i * 2, (uint8_t)c.display[i]);
        for (size_t i = 0; i < c.file.size() && i < 41; i++) W8(a + 8 + 256 + (uint32_t)i, (uint8_t)c.file[i]);
    }

    fs::path ContentRoot() { return g_saveDir / "conteudo"; }

    // ultimo jogo salvo aberto/criado (vai junto no pacote do bug)
    std::mutex g_activeMutex;
    fs::path g_activeSave;
    std::string g_activeName;
    fs::path ContentDir(uint32_t type, const std::string& file)
    {
        char t[16]; snprintf(t, sizeof(t), "%08X", type);
        return ContentRoot() / t / file;
    }

    std::vector<ContentData> ListContent(uint32_t type)
    {
        std::vector<ContentData> out;
        std::error_code ec;
        for (auto& tdir : fs::directory_iterator(ContentRoot(), ec))
        {
            if (!tdir.is_directory()) continue;
            uint32_t t = strtoul(tdir.path().filename().string().c_str(), nullptr, 16);
            if (type && t != type) continue;
            for (auto& item : fs::directory_iterator(tdir.path(), ec))
            {
                if (!item.is_directory()) continue;
                ContentData c;
                c.device = DEVICE_HDD; c.type = t; c.file = item.path().filename().string();
                std::ifstream n(item.path() / "_nome.txt");
                std::getline(n, c.display);
                if (c.display.empty()) c.display = c.file;
                out.push_back(c);
            }
        }
        return out;
    }

    // Enumerador de conteudo (XamContentCreateEnumerator + XamEnumerate)
    struct KEnum : KObject
    {
        std::vector<ContentData> items;
        size_t next = 0;
        uint32_t perCall = 1;
    };

    // ------------------------------------------------------------ notificacoes (XNotifyGetNext)
    struct KListener : KEvent
    {
        std::deque<std::pair<uint32_t, uint32_t>> queue;   // (id, parametro)
        KListener() : KEvent(true, false) {}
    };
    std::mutex g_listenersMutex;
    std::vector<std::weak_ptr<KListener>> g_listeners;

    // Manda uma notificacao para todos os ouvintes (como o sistema do Xbox)
    void Broadcast(uint32_t id, uint32_t param)
    {
        std::lock_guard lock(g_listenersMutex);
        for (auto& w : g_listeners)
            if (auto l = w.lock())
            {
                std::lock_guard objLock(g_objMutex);
                l->queue.push_back({ id, param });
                l->signaled = true;
            }
        SignalAll();
    }
    // Telas do sistema: o jogo costuma esperar "tela aberta" e depois "tela fechada" (XN_SYS_UI = 9)
    void SystemUiShown()
    {
        Broadcast(0x00000009, 1);
        Broadcast(0x00000009, 0);
    }
}

// ---------------------------------------------------------------- usuario e perfil

bool InputUsuarioLigado(uint32_t usuario);   // input.cpp: jogador 1 sempre; 2 quando separado do 1; 3 e 4 com dispositivo
std::string RedeNomeLocal();                 // rede.cpp: nome do usuario do Windows

KFUNC(XamUserGetSigninState)
{
    Ret(ctx, InputUsuarioLigado(Gpr32(ctx, 3)) ? 1 : 0);   // 1 = conectado localmente (sem Xbox Live)
}

// XamUserGetXUID(usuario, tipo, xuid*)
KFUNC(XamUserGetXUID)
{
    Trace("XamUserGetXUID", "(%u, %08X, %08X)", Gpr32(ctx, 3), Gpr32(ctx, 4), Gpr32(ctx, 5));
    if (!InputUsuarioLigado(Gpr32(ctx, 3))) { Ret(ctx, X_ERROR_NO_SUCH_USER); return; }
    if (Gpr32(ctx, 5)) W64(Gpr32(ctx, 5), XUID + Gpr32(ctx, 3));   // jogadores 2-4: outro XUID (+1..+3)
    Ret(ctx, X_ERROR_SUCCESS);
}

// XamUserGetName(usuario, buffer, tamanho)
KFUNC(XamUserGetName)
{
    if (!InputUsuarioLigado(Gpr32(ctx, 3))) { Ret(ctx, X_ERROR_NO_SUCH_USER); return; }
    // nome do jogador: o do perfil escolhido no launcher (item 32); o principal = o nome do usuario do Windows
    static const std::string nome1 = !OpcoesAtuais().perfil.empty() ? OpcoesAtuais().perfil : RedeNomeLocal();
    static const std::string nomes[4] = { nome1, nome1.substr(0, 13) + " 2", nome1.substr(0, 13) + " 3", nome1.substr(0, 13) + " 4" };
    const char* name = nomes[Gpr32(ctx, 3) & 3].c_str();
    uint32_t buf = Gpr32(ctx, 4), len = Gpr32(ctx, 5);
    if (buf && len)
    {
        uint32_t n = std::min<uint32_t>(len - 1, (uint32_t)strlen(name));
        memcpy(G(buf), name, n);
        W8(buf + n, 0);
    }
    Ret(ctx, X_ERROR_SUCCESS);
}

// XamUserCheckPrivilege(usuario, privilegio, resultado*): sem Xbox Live, mas jogos locais liberados
KFUNC(XamUserCheckPrivilege)
{
    Trace("XamUserCheckPrivilege", "(%u, %u, %08X)", Gpr32(ctx, 3), Gpr32(ctx, 4), Gpr32(ctx, 5));
    if (Gpr32(ctx, 5)) W32(Gpr32(ctx, 5), 0);
    Ret(ctx, Gpr32(ctx, 3) == 0 ? X_ERROR_SUCCESS : X_ERROR_NO_SUCH_USER);
}

KFUNC(XamUserAreUsersFriends) { Ret(ctx, X_ERROR_NO_SUCH_USER); }
KFUNC(XamUserCreateStatsEnumerator) { Ret(ctx, X_ERROR_NO_SUCH_USER); }

namespace
{
    // Ajuste do perfil: id = tipo (bits 28-31) | tamanho maximo (bits 16-27) | numero. Tipos: 1 int32, 2 int64,
    // 3 double, 4 texto, 5 float, 6 binario, 7 data.
    fs::path SettingPath(uint32_t id) { char n[32]; snprintf(n, sizeof(n), "%08X.bin", id); return g_saveDir / "perfil" / n; }
    std::vector<uint8_t> LoadSetting(uint32_t id)
    {
        std::ifstream f(SettingPath(id), std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
    }
}

// XamUserReadProfileSettings(titulo, usuario, n_xuids, xuids*, n_ajustes, ids*, tamanho*, buffer, overlapped)
// Buffer: { u32 n; u32 ajustes* } + n x X_USER_PROFILE_SETTING (0x28 bytes: u32 origem; u32 -; u64 xuid/usuario;
// u32 id; u32 -; dado X_USER_DATA 0x10: u8 tipo, -, u32 tamanho/valor @+8, u32 ponteiro @+12) + dados binarios.
KFUNC(XamUserReadProfileSettings)
{
    uint32_t user = Gpr32(ctx, 4), xuidCount = Gpr32(ctx, 5), count = Gpr32(ctx, 7), ids = Gpr32(ctx, 8),
        sizePtr = Gpr32(ctx, 9), buf = Gpr32(ctx, 10), ov = R32(Gpr32(ctx, 1) + 0x54);
    Trace("XamUserReadProfileSettings", "(titulo %08X, usuario %u, %u xuids, %u ajustes, ids %08X, tamanho* %08X = %u, buffer %08X, ov %08X)",
        Gpr32(ctx, 3), user, xuidCount, count, ids, sizePtr, sizePtr ? R32(sizePtr) : 0, buf, ov);
    void InputMarkMenu();   // input.cpp: o menu principal apareceu (roteiros "M..." do FCRECOMP_AUTOINPUT)
    InputMarkMenu();
    // usuario 255 = por lista de XUIDs (na LAN, os jogadores da partida; a lista pode vir vazia): uma entrada por
    // jogador e ajuste; so o jogador deste PC tem valores, os outros voltam "nao existe"
    const uint32_t xuids = Gpr32(ctx, 6);
    const bool porLista = user == 0xFF;
    if (!porLista && !InputUsuarioLigado(user) && !xuidCount) { Ret(ctx, Complete(ov, X_ERROR_NO_SUCH_USER)); return; }
    if (!ids || !sizePtr || (!count && !porLista)) { Ret(ctx, Complete(ov, X_ERROR_FUNCTION_FAILED)); return; }
    const uint32_t jogadores = porLista ? xuidCount : 1;

    std::vector<std::vector<uint8_t>> values(count);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t id = R32(ids + i * 4);
        values[i] = LoadSetting(id);
        if (SaveLog()) Log("[save]   ajuste %08X (tipo %u): %zu bytes salvos", id, id >> 28, values[i].size());
    }
    auto deste = [&](uint32_t j) { return !porLista || (xuids && (R64(xuids + j * 8) & ~3ull) == XUID); };
    uint32_t need = 8 + jogadores * count * 0x28;
    for (uint32_t j = 0; j < jogadores; j++)
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t type = R32(ids + i * 4) >> 28;
            if (deste(j) && (type == 4 || type == 6)) need += (uint32_t)((values[i].size() + 3) & ~3u);
        }
    if (!buf || R32(sizePtr) < need)
    {
        W32(sizePtr, need);
        Ret(ctx, Complete(ov, X_ERROR_INSUFFICIENT_BUFFER));
        return;
    }
    memset(G(buf), 0, need);
    W32(buf, jogadores * count);
    W32(buf + 4, buf + 8);
    uint32_t data = buf + 8 + jogadores * count * 0x28;
    for (uint32_t j = 0; j < jogadores; j++)
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t s = buf + 8 + (j * count + i) * 0x28, id = R32(ids + i * 4), type = id >> 28;
            const std::vector<uint8_t> vazio;
            auto& v = deste(j) ? values[i] : vazio;
            W32(s + 0x00, v.empty() ? 0 : 2);   // origem: 0 = nao existe, 1 = padrao, 2 = do titulo
            W64(s + 0x08, porLista ? (xuids ? R64(xuids + j * 8) : 0) : xuidCount && xuids ? R64(xuids) : user);
            W32(s + 0x10, id);
            W8(s + 0x18, (uint8_t)(v.empty() ? 0 : type));
            if (v.empty()) continue;
            if (type == 4 || type == 6)
            {
                memcpy(G(data), v.data(), v.size());
                W32(s + 0x18 + 8, (uint32_t)v.size());
                W32(s + 0x18 + 12, data);
                data += (uint32_t)((v.size() + 3) & ~3u);
            }
            else
                memcpy(G(s + 0x18 + 8), v.data(), std::min<size_t>(v.size(), 8));
        }
    Ret(ctx, Complete(ov, X_ERROR_SUCCESS));
}

// XamUserWriteProfileSettings(titulo, usuario, n_ajustes, ajustes*, overlapped)
KFUNC(XamUserWriteProfileSettings)
{
    uint32_t count = Gpr32(ctx, 5), settings = Gpr32(ctx, 6), ov = Gpr32(ctx, 7);
    Trace("XamUserWriteProfileSettings", "(titulo %08X, usuario %u, %u ajustes, %08X, ov %08X)", Gpr32(ctx, 3), Gpr32(ctx, 4), count, settings, ov);
    std::error_code ec;
    fs::create_directories(g_saveDir / "perfil", ec);
    for (uint32_t i = 0; i < count && settings; i++)
    {
        // o jogo nao preenche o byte de tipo do dado (vem lixo): o tipo sai do proprio id (bits 28-31)
        uint32_t s = settings + i * 0x28, id = R32(s + 0x10), type = id >> 28;
        if (SaveLog())
        {
            std::string h;
            for (int k = 0; k < 0x28; k++) { char t[4]; snprintf(t, sizeof(t), "%02X", R8(s + k)); h += t; if (k % 8 == 7) h += ' '; }
            Log("[save]   ajuste %u cru: %s", i, h.c_str());
        }
        std::vector<uint8_t> v;
        if (type == 4 || type == 6)
        {
            uint32_t len = R32(s + 0x18 + 8), ptr = R32(s + 0x18 + 12);
            if (ptr && len < 0x10000) v.assign(G(ptr), G(ptr) + len);
        }
        else if (type)
            v.assign(G(s + 0x18 + 8), G(s + 0x18 + 16));
        std::ofstream(SettingPath(id), std::ios::binary).write((const char*)v.data(), v.size());
        if (SaveLog()) Log("[save]   ajuste %08X (tipo %u): %zu bytes gravados", id, type, v.size());
    }
    Ret(ctx, Complete(ov, X_ERROR_SUCCESS));
}

// ---------------------------------------------------------------- dispositivos

// XamContentGetDeviceState(dispositivo, overlapped)
KFUNC(XamContentGetDeviceState)
{
    Trace("XamContentGetDeviceState", "(%08X, ov %08X)", Gpr32(ctx, 3), Gpr32(ctx, 4));
    uint32_t r = Gpr32(ctx, 3) == DEVICE_HDD ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
    Ret(ctx, Complete(Gpr32(ctx, 4), r));
}

// XamContentGetDeviceName(dispositivo, nome (wchar), capacidade)
KFUNC(XamContentGetDeviceName)
{
    if (Gpr32(ctx, 3) != DEVICE_HDD) { Ret(ctx, X_ERROR_DEVICE_NOT_CONNECTED); return; }
    const char name[] = "Disco rigido";
    uint32_t buf = Gpr32(ctx, 4), cap = Gpr32(ctx, 5);
    for (uint32_t i = 0; buf && i < cap; i++) W16(buf + i * 2, i < sizeof(name) - 1 ? (uint8_t)name[i] : 0);
    Ret(ctx, X_ERROR_SUCCESS);
}

// XamShowDeviceSelectorUI(usuario, tipo de conteudo, opcoes, bytes pedidos (u64), dispositivo*, overlapped):
// escolhe o HD na hora (nao ha tela)
KFUNC(XamShowDeviceSelectorUI)
{
    Trace("XamShowDeviceSelectorUI", "(%u, tipo %08X, opcoes %08X, %llu bytes, disp* %08X, ov %08X)", Gpr32(ctx, 3), Gpr32(ctx, 4),
        Gpr32(ctx, 5), (unsigned long long)Gpr64(ctx, 6), Gpr32(ctx, 7), Gpr32(ctx, 8));
    if (Gpr32(ctx, 7)) W32(Gpr32(ctx, 7), DEVICE_HDD);
    SystemUiShown();
    Ret(ctx, Complete(Gpr32(ctx, 8), X_ERROR_SUCCESS));
}

// ---------------------------------------------------------------- conteudo

// XamContentCreate(usuario, raiz, XCONTENT_DATA*, opcoes, resultado*, licenca*, overlapped)
// opcoes (byte baixo): 1 criar novo, 2 criar sempre, 3 abrir existente, 4 abrir sempre, 5 truncar existente
KFUNC(XamContentCreate)
{
    std::string root = RStr(Gpr32(ctx, 4));
    ContentData c = ReadContent(Gpr32(ctx, 5));
    uint32_t flags = Gpr32(ctx, 6), disp = Gpr32(ctx, 7), lic = Gpr32(ctx, 8), ov = Gpr32(ctx, 9);
    fs::path dir = ContentDir(c.type, c.file);
    bool exists = fs::exists(dir);
    uint32_t mode = flags & 0xF, result = X_ERROR_SUCCESS, disposition = 0;
    switch (mode)
    {
    case 1: if (exists) result = X_ERROR_ALREADY_EXISTS; else disposition = 1; break;
    case 2: if (exists) fs::remove_all(dir); disposition = 1; break;
    case 3: if (!exists) result = X_ERROR_FILE_NOT_FOUND; else disposition = 2; break;
    case 4: disposition = exists ? 2 : 1; break;
    case 5: if (!exists) result = X_ERROR_FILE_NOT_FOUND; else { fs::remove_all(dir); disposition = 2; } break;
    default: disposition = exists ? 2 : 1; break;
    }
    if (result == X_ERROR_SUCCESS)
    {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (disposition == 1 || !fs::exists(dir / "_nome.txt")) std::ofstream(dir / "_nome.txt") << c.display;
        MountContentRoot(root, dir);
        if (c.type == 1 && disposition == 2)
        {
            extern std::atomic<bool> g_saveOpened;   // gpu.cpp: so procura o inicio da fase depois disto
            g_saveOpened = true;
        }
        if (c.type == 1)
        {
            std::lock_guard lock(g_activeMutex);
            g_activeSave = dir; g_activeName = c.display;
        }
    }
    if (disp) W32(disp, disposition);
    if (lic) W32(lic, 0);
    Log("[save] XamContentCreate raiz \"%s\" tipo %08X arquivo \"%s\" (\"%s\") opcoes %08X -> %s (%s)", root.c_str(), c.type,
        c.file.c_str(), c.display.c_str(), flags, result ? "erro" : disposition == 1 ? "criado" : "aberto", dir.string().c_str());
    Ret(ctx, Complete(ov, result));
}

// XamContentClose(raiz, overlapped)
KFUNC(XamContentClose)
{
    std::string root = RStr(Gpr32(ctx, 3));
    Trace("XamContentClose", "(\"%s\", ov %08X)", root.c_str(), Gpr32(ctx, 4));
    UnmountContentRoot(root);
    Ret(ctx, Complete(Gpr32(ctx, 4), X_ERROR_SUCCESS));
}

// XamContentDelete(usuario, XCONTENT_DATA*, overlapped)
KFUNC(XamContentDelete)
{
    ContentData c = ReadContent(Gpr32(ctx, 4));
    std::error_code ec;
    bool ok = fs::remove_all(ContentDir(c.type, c.file), ec) > 0;
    Log("[save] XamContentDelete tipo %08X arquivo \"%s\" -> %s", c.type, c.file.c_str(), ok ? "apagado" : "nao existia");
    Ret(ctx, Complete(Gpr32(ctx, 5), ok ? X_ERROR_SUCCESS : X_ERROR_FILE_NOT_FOUND));
}

// XamContentGetCreator(usuario, XCONTENT_DATA*, e_criador*, xuid*, overlapped)
KFUNC(XamContentGetCreator)
{
    if (Gpr32(ctx, 5)) W32(Gpr32(ctx, 5), 1);
    if (Gpr32(ctx, 6)) W64(Gpr32(ctx, 6), XUID);
    Ret(ctx, Complete(Gpr32(ctx, 7), X_ERROR_SUCCESS));
}

// XamContentSetThumbnail(usuario, XCONTENT_DATA*, imagem, tamanho, overlapped)
KFUNC(XamContentSetThumbnail)
{
    ContentData c = ReadContent(Gpr32(ctx, 4));
    fs::path dir = ContentDir(c.type, c.file);
    if (fs::exists(dir) && Gpr32(ctx, 5) && Gpr32(ctx, 6) < (16u << 20))
        std::ofstream(dir / "_miniatura.png", std::ios::binary).write((const char*)G(Gpr32(ctx, 5)), Gpr32(ctx, 6));
    Ret(ctx, Complete(Gpr32(ctx, 7), X_ERROR_SUCCESS));
}

// XamContentCreateEnumerator(usuario, dispositivo, tipo, opcoes, itens por chamada, tamanho do buffer*, handle*)
KFUNC(XamContentCreateEnumerator)
{
    uint32_t device = Gpr32(ctx, 4), type = Gpr32(ctx, 5), per = std::max(1u, Gpr32(ctx, 7));
    auto e = std::make_shared<KEnum>();
    if (device == 0 || device == DEVICE_HDD) e->items = ListContent(type);
    e->perCall = per;
    if (Gpr32(ctx, 8)) W32(Gpr32(ctx, 8), CONTENT_DATA_SIZE * per);
    uint32_t h = HandleCreate(e);
    if (Gpr32(ctx, 9)) W32(Gpr32(ctx, 9), h);
    Log("[save] XamContentCreateEnumerator dispositivo %08X tipo %08X: %zu itens salvos", device, type, e->items.size());
    Ret(ctx, X_ERROR_SUCCESS);
}

// XamEnumerate(handle, opcoes, buffer, tamanho, itens*, overlapped)
KFUNC(XamEnumerate)
{
    uint32_t buf = Gpr32(ctx, 5), size = Gpr32(ctx, 6), retPtr = Gpr32(ctx, 7), ov = Gpr32(ctx, 8);
    auto e = std::dynamic_pointer_cast<KEnum>(HandleGet(Gpr32(ctx, 3)));
    uint32_t n = 0;
    if (e)
    {
        uint32_t max = std::min<uint32_t>(e->perCall, size / CONTENT_DATA_SIZE);
        while (n < max && e->next < e->items.size())
            WriteContent(buf + n++ * CONTENT_DATA_SIZE, e->items[e->next++]);
    }
    uint32_t result = n ? X_ERROR_SUCCESS : X_ERROR_NO_MORE_FILES;
    Trace("XamEnumerate", "(%08X) -> %u itens", Gpr32(ctx, 3), n);
    if (ov) { Ret(ctx, Complete(ov, result, n)); return; }
    if (retPtr) W32(retPtr, n);
    Ret(ctx, result);
}

// ---------------------------------------------------------------- notificacoes: avisa que o usuario 0 esta conectado

KFUNC(XamNotifyCreateListener)
{
    auto l = std::make_shared<KListener>();
    l->queue.push_back({ 0x0000000A, 1 });   // XN_SYS_SIGNINCHANGED: usuario 0 conectado
    l->signaled = true;
    {
        std::lock_guard lock(g_listenersMutex);
        g_listeners.push_back(l);
    }
    Ret(ctx, HandleCreate(l));
}

// XNotifyGetNext(handle, id procurado, id*, parametro*) -> 1 se havia notificacao
KFUNC(XNotifyGetNext)
{
    auto l = std::dynamic_pointer_cast<KListener>(HandleGet(Gpr32(ctx, 3)));
    if (!l) { Ret(ctx, 0); return; }
    std::lock_guard lock(g_objMutex);
    for (auto it = l->queue.begin(); it != l->queue.end(); ++it)
    {
        if (Gpr32(ctx, 4) && it->first != Gpr32(ctx, 4)) continue;
        if (Gpr32(ctx, 5)) W32(Gpr32(ctx, 5), it->first);
        if (Gpr32(ctx, 6)) W32(Gpr32(ctx, 6), it->second);
        Trace("XNotifyGetNext", "-> %08X (%u)", it->first, it->second);
        l->queue.erase(it);
        l->signaled = !l->queue.empty();
        Ret(ctx, 1);
        return;
    }
    Ret(ctx, 0);
}

// ---------------------------------------------------------------- teclado virtual e caixas de mensagem

// XamShowKeyboardUI(usuario, opcoes, texto padrao, titulo, descricao, buffer, tamanho, overlapped)
// - codigo de trapaca (MENU DE TRAPACAS): abre a caixa de digitacao (teclado.cpp) e responde quando o jogador confirma
//   ou cancela; o jogo espera pelo overlapped (fica "pendente" ate la). FCRECOMP_TECLADO=texto responde sozinho (testes).
// - nome do jogo salvo: "digita" o texto padrao (ou "Jogador") com data e hora e confirma
void TecladoAbrir(std::wstring titulo, std::wstring descricao, std::wstring padrao, uint32_t maximo,
                  std::function<void(bool ok, const std::wstring& texto)> fim);
bool RoteiroAtivo();   // input.cpp

namespace
{
    std::wstring ReadWideW(uint32_t a, size_t max)
    {
        std::wstring s;
        for (size_t i = 0; a && i < max; i++) { uint16_t c = R16(a + (uint32_t)i * 2); if (!c) break; s += (wchar_t)c; }
        return s;
    }
    // resposta do teclado: texto no buffer do jogo (UTF-16, com o zero no fim) e o overlapped concluido
    void TecladoResponder(uint32_t buf, uint32_t len, uint32_t ov, bool ok, const std::wstring& texto)
    {
        for (uint32_t i = 0; buf && i < len; i++) W16(buf + i * 2, ok && i < texto.size() && i + 1 < len ? (uint16_t)texto[i] : 0);
        Broadcast(0x00000009, 0);   // tela do sistema fechada
        Complete(ov, ok ? X_ERROR_SUCCESS : X_ERROR_CANCELLED);
    }
}

KFUNC(XamShowKeyboardUI)
{
    uint32_t def = Gpr32(ctx, 5), buf = Gpr32(ctx, 8), len = Gpr32(ctx, 9), ov = Gpr32(ctx, 10);
    {
        std::wstring titulo = ReadWideW(Gpr32(ctx, 6), 128), desc = ReadWideW(Gpr32(ctx, 7), 256);
        std::string t = ReadWide(Gpr32(ctx, 6), 128) + " " + ReadWide(Gpr32(ctx, 7), 256);
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (t.find("trapa") != std::string::npos || t.find("cheat") != std::string::npos)
        {
            Log("[save] XamShowKeyboardUI: codigo de trapaca (\"%s\")", t.c_str());
            if (const char* auto_ = getenv("FCRECOMP_TECLADO"))
            {
                std::string s = auto_;
                Log("[teclado] resposta automatica \"%s\"", s.c_str());
                SystemUiShown();
                TecladoResponder(buf, len, ov, true, std::wstring(s.begin(), s.end()));
                Ret(ctx, X_ERROR_IO_PENDING);
                return;
            }
            if (RoteiroAtivo())   // gravacao/repeticao de roteiro: ninguem para digitar
            {
                SystemUiShown();
                TecladoResponder(buf, len, ov, false, L"");
                Ret(ctx, X_ERROR_IO_PENDING);
                return;
            }
            if (ov) { W32(ov + 0x00, X_ERROR_IO_PENDING); W32(ov + 0x18, X_ERROR_IO_PENDING); }
            Broadcast(0x00000009, 1);   // tela do sistema aberta
            TecladoAbrir(titulo, desc, L"", len, [buf, len, ov](bool ok, const std::wstring& texto) {
                std::string s(texto.begin(), texto.end());
                Log("[teclado] %s \"%s\"", ok ? "confirmado" : "cancelado", ok ? s.c_str() : "");
                TecladoResponder(buf, len, ov, ok, texto);
            });
            Ret(ctx, X_ERROR_IO_PENDING);
            return;
        }
    }
    std::string text = def ? ReadWide(def, 256) : "";
    if (text.empty()) text = "Jogador";
    {
        // nome com data e hora: varios saves ficam distinguiveis na lista do jogo (e no pacote do bug)
        uint64_t HoraDoJogo();   // kernel.cpp (a repeticao de um roteiro gravado usa a hora da gravacao)
        uint64_t h = HoraDoJogo();
        FILETIME ft{ (DWORD)h, (DWORD)(h >> 32) }, local;
        SYSTEMTIME t; FileTimeToLocalFileTime(&ft, &local); FileTimeToSystemTime(&local, &t);
        char when[32]; snprintf(when, sizeof(when), " %02u%02u %02uh%02u", t.wDay, t.wMonth, t.wHour, t.wMinute);
        size_t room = len > strlen(when) + 1 ? len - 1 - strlen(when) : 0;
        text = text.substr(0, std::min(text.size(), room)) + when;
    }
    Log("[save] XamShowKeyboardUI (opcoes %08X, ate %u letras) titulo \"%s\" descricao \"%s\" -> \"%s\"", Gpr32(ctx, 4), len, Gpr32(ctx, 6) ? ReadWide(Gpr32(ctx, 6), 128).c_str() : "",
        Gpr32(ctx, 7) ? ReadWide(Gpr32(ctx, 7), 256).c_str() : "", text.c_str());
    for (uint32_t i = 0; buf && i < len; i++) W16(buf + i * 2, i < text.size() && i + 1 < len ? (uint8_t)text[i] : 0);
    SystemUiShown();
    Ret(ctx, Complete(ov, X_ERROR_SUCCESS));
}

// XamShowMessageBoxUIEx(usuario, titulo, texto, n_botoes, botoes*, botao ativo, opcoes, resultado*, overlapped):
// escolhe o botao ativo (o que ja vem selecionado)
KFUNC(XamShowMessageBoxUIEx)
{
    uint32_t count = Gpr32(ctx, 6), buttons = Gpr32(ctx, 7), active = Gpr32(ctx, 8), result = Gpr32(ctx, 10), ov = R32(Gpr32(ctx, 1) + 0x54);
    std::string opts;
    for (uint32_t i = 0; buttons && i < count && i < 4; i++) opts += " [" + ReadWide(R32(buttons + i * 4), 64) + "]";
    Log("[save] XamShowMessageBoxUIEx \"%s\": \"%s\" botoes%s -> escolhe %u", Gpr32(ctx, 4) ? ReadWide(Gpr32(ctx, 4), 128).c_str() : "",
        Gpr32(ctx, 5) ? ReadWide(Gpr32(ctx, 5), 512).c_str() : "", opts.c_str(), active);
    if (result) W32(result, active);
    SystemUiShown();
    Ret(ctx, Complete(ov, X_ERROR_SUCCESS));
}

bool SalvarImagemDaJanela(const std::wstring& arquivo);   // gpu.cpp: foto da janela do jogo

// ---------------------------------------------------------------- F12: pacote do bug
// Grava build\bugs\bug_N\ com: info.txt (hora, quadro, save ativo, fase), save\ (copia do jogo salvo ativo e do
// perfil, para carregar com tools\carregar_bug.ps1 -Bug N), tela.bmp (foto da janela), quadro\ (captura do quadro como o
// F9: lista de desenhos e imagens), diagnostico.txt e fcrecomp_log.txt. O essencial (registro, diagnostico, threads, foto)
// e gravado primeiro: com o jogo travado ou a placa de video derrubada a captura do quadro nunca vem, e antes o pacote
// esperava 20 s por ela e ficava so com o save (bugs 11 e 12).
namespace
{
    std::atomic<bool> g_bugBusy{ false };
    std::atomic<ULONGLONG> g_bugShownUntil{ 0 };
    std::mutex g_bugMsgMutex;
    std::string g_bugMsg;

    void CopyTree(const fs::path& from, const fs::path& to)
    {
        std::error_code ec;
        fs::create_directories(to, ec);
        fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) Log("[bug] erro ao copiar %s: %s", from.string().c_str(), ec.message().c_str());
    }

    void MakeBugPackage(std::string freezeReport)
    {
        std::error_code ec;
        fs::create_directories("bugs", ec);
        int n = 1;
        while (fs::exists("bugs\\bug_" + std::to_string(n))) n++;
        fs::path dir = "bugs\\bug_" + std::to_string(n);
        fs::create_directories(dir, ec);
        uint64_t frame = diag::CurrentFrame();
        diag::CaptureAt(frame + 2);
        Log("[bug] %s: pacote do bug %d em %s (quadro %llu)", freezeReport.empty() ? "F12" : "vigia", n, fs::absolute(dir).string().c_str(), (unsigned long long)frame);
        if (!freezeReport.empty()) std::ofstream(dir / "travamento.txt") << freezeReport;

        fs::path save; std::string name;
        {
            std::lock_guard lock(g_activeMutex);
            save = g_activeSave; name = g_activeName;
        }
        if (!save.empty() && fs::exists(save))
            CopyTree(save, dir / "save" / "conteudo" / save.parent_path().filename() / save.filename());
        if (fs::exists(g_saveDir / "perfil")) CopyTree(g_saveDir / "perfil", dir / "save" / "perfil");

        // primeiro o essencial: foto da janela (funciona com a placa de video travada), estado de cada thread do jogo (em
        // que esta esperando e ha quanto tempo), diagnostico e registro
        SalvarImagemDaJanela((dir / "tela.bmp").wstring());
        auto gravarEstado = [&] {
            std::ofstream(dir / "threads.txt") << ::DescribeThreads();   // thread.cpp
            diag::WriteReport(freezeReport.empty() ? "F12 (pacote do bug)" : "vigia (tela parada)");
            fs::copy_file("diagnostico.txt", dir / "diagnostico.txt", fs::copy_options::overwrite_existing, ec);
            fs::copy_file("fcrecomp_log.txt", dir / "fcrecomp_log.txt", fs::copy_options::overwrite_existing, ec);
        };
        gravarEstado();

        // captura do quadro (desenhos e resolves): so se o jogo ainda esta desenhando. Espera os arquivos pararem de
        // aparecer; desiste se nenhum quadro novo chegar em 1,5 s (travado) ou depois de 20 s.
        fs::path cap = "diag\\quadro_" + std::to_string(frame + 2);
        size_t last = 0; int still = 0, parado = 0;
        uint64_t quadroAntes = diag::CurrentFrame();
        for (int i = 0; i < 100 && still < 8 && parado < 8; i++)
        {
            Sleep(200);
            size_t count = 0;
            for (auto& e : fs::directory_iterator(cap, ec)) { (void)e; count++; }
            if (count && count == last) still++; else still = 0;
            last = count;
            uint64_t q = diag::CurrentFrame();
            parado = (q == quadroAntes && !count) ? parado + 1 : 0;
            quadroAntes = q;
        }
        if (fs::exists(cap)) CopyTree(cap, dir / "quadro");
        else Log("[bug] sem captura do quadro (o jogo nao desenhou quadros novos): o pacote tem a foto da janela (tela.bmp)");
        gravarEstado();   // de novo, com o que aconteceu durante a espera

        // fase atual: o ultimo arquivo de fase aberto (linhas "[fs] abriu: ...\levels\...")
        std::string level;
        {
            std::ifstream lf("fcrecomp_log.txt");
            for (std::string line; std::getline(lf, line);)
                if (line.rfind("[fs] abriu", 0) == 0)
                {
                    std::string low = line;
                    std::transform(low.begin(), low.end(), low.begin(), ::tolower);
                    if (size_t p = low.find("\\levels\\"); p != std::string::npos)
                        level = line.substr(p + 8, line.find('\\', p + 8) - (p + 8));
                }
        }
        SYSTEMTIME t; GetLocalTime(&t);
        std::ofstream info(dir / "info.txt");
        char when[64]; snprintf(when, sizeof(when), "%02u/%02u/%04u %02u:%02u:%02u", t.wDay, t.wMonth, t.wYear, t.wHour, t.wMinute, t.wSecond);
        info << "Pacote do bug " << n << "\n"
             << "Data: " << when << "\n"
             << "Quadro: " << frame << "\n"
             << "Fase: " << (level.empty() ? "(menu ou desconhecida)" : level) << "\n"
             << "Jogo salvo ativo: " << (save.empty() ? std::string("(nenhum - jogo sem salvar)") : name + "  (" + save.string() + ")") << "\n"
             << "Para voltar ao ponto: tools\\carregar_bug.ps1 -Bug " << n << " (carrega o ultimo checkpoint desse save)\n";
        info.close();
        {
            std::lock_guard lock(g_bugMsgMutex);
            g_bugMsg = "  [bug " + std::to_string(n) + " salvo em build\\bugs\\bug_" + std::to_string(n) +
                (save.empty() ? " - SEM jogo salvo]" : "]");
        }
        g_bugShownUntil = GetTickCount64() + 8000;
        Log("[bug] pacote do bug %d pronto%s", n, save.empty() ? " (sem jogo salvo ativo: use CREATE NEW GAME)" : "");
        g_bugBusy = false;
    }
}

// Tecla na janela do jogo (gpu.cpp). F12 = pacote do bug.
bool BugKey(unsigned vk)
{
    if (vk != VK_F12) return false;
    if (g_bugBusy.exchange(true)) return true;
    {
        std::lock_guard lock(g_bugMsgMutex);
        g_bugMsg = "  [gravando o pacote do bug...]";
    }
    g_bugShownUntil = GetTickCount64() + 60000;
    std::thread(MakeBugPackage, std::string()).detach();
    return true;
}

// Vigia de travamento: se o quadro do jogo (contador de trocas da GPU emulada) nao muda por FCRECOMP_VIGIA segundos
// (padrao 6; 0 desliga), grava sozinho um pacote do bug com travamento.txt: estado das threads do jogo, pilhas nativas
// de todas as threads (o que o emulador estava fazendo) e se a janela responde. Um pacote por travamento (rearma
// quando os quadros voltam), no maximo 3 por execucao.
std::string NativeStacks();   // profiler.cpp
HWND GameWindow();             // gpu.cpp
void StartFreezeWatch()
{
    int limit = getenv("FCRECOMP_VIGIA") ? atoi(getenv("FCRECOMP_VIGIA")) : 6;
    if (limit <= 0) return;
    std::thread([limit] {
        uint64_t last = 0; ULONGLONG since = GetTickCount64();
        bool fired = false; int count = 0;
        for (;;)
        {
            Sleep(500);
            uint64_t f = diag::CurrentFrame();
            ULONGLONG now = GetTickCount64();
            if (f != last) { last = f; since = now; fired = false; continue; }
            if (fired || f < 60 || count >= 3 || now - since < (ULONGLONG)limit * 1000) continue;
            fired = true; count++;
            DWORD_PTR res = 0;
            HWND wnd = GameWindow();
            bool responds = wnd && SendMessageTimeoutW(wnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 2000, &res);
            char head[256];
            snprintf(head, sizeof(head), "Tela parada ha %.1f s no quadro %llu. Janela %s.\n\n",
                (now - since) / 1000.0, (unsigned long long)f, responds ? "respondendo" : "NAO responde (congelada)");
            std::string report = head;
            report += "===== threads do jogo =====\n" + ::DescribeThreads();
            report += "\n===== pilhas nativas (o que cada thread do PC estava fazendo) =====\n" + NativeStacks();
            Log("[vigia] tela parada ha %.1f s no quadro %llu (janela %s): gravando pacote do bug",
                (now - since) / 1000.0, (unsigned long long)f, responds ? "respondendo" : "congelada");
            if (g_bugBusy.exchange(true)) continue;
            {
                std::lock_guard lock(g_bugMsgMutex);
                g_bugMsg = "  [vigia: tela parada, gravando pacote do bug...]";
            }
            g_bugShownUntil = GetTickCount64() + 60000;
            MakeBugPackage(report);
        }
    }).detach();
}

std::string BugStatus()
{
    if (GetTickCount64() >= g_bugShownUntil) return "";
    std::lock_guard lock(g_bugMsgMutex);
    return g_bugMsg;
}
