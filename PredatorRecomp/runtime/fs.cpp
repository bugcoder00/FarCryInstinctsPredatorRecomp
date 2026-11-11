// Arquivos: traduz caminhos do Xbox (game:\, d:\, \Device\Cdrom0\) para a pasta do jogo no PC
#include <map>
#include "diag.h"
#include "perf.h"
#include <mutex>
#include "objects.h"
#include <windows.h>
#include <filesystem>
#include <algorithm>

std::filesystem::path g_gameDir;
std::filesystem::path g_saveDir;

namespace fs = std::filesystem;

struct KFile : KObject
{
    HANDLE h = INVALID_HANDLE_VALUE;
    fs::path path;
    // posicao propria (com trava): varias threads leem o mesmo arquivo (ex.: o Bink le o video em segundo plano)
    // e "move o ponteiro e depois le" em dois passos deixava uma thread ler da posicao da outra
    std::mutex lock;
    uint64_t pos = 0;
    bool directory = false;
    HANDLE find = INVALID_HANDLE_VALUE;
    ~KFile() override
    {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (find != INVALID_HANDLE_VALUE) FindClose(find);
    }
};

// Raizes criadas pelo XamContentCreate ("savegame:" -> build\save\conteudo\...). content.cpp
static std::mutex g_rootMutex;
static std::map<std::string, fs::path> g_contentRoots;
void MountContentRoot(const std::string& name, const fs::path& path)
{
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    std::lock_guard lock(g_rootMutex);
    g_contentRoots[n] = path;
}
void UnmountContentRoot(const std::string& name)
{
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    std::lock_guard lock(g_rootMutex);
    g_contentRoots.erase(n);
}

static fs::path MapPath(std::string p)
{
    std::replace(p.begin(), p.end(), '/', '\\');
    std::string lower = p;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    fs::path root = g_gameDir;
    std::string rest;
    if (lower.rfind("\\device\\cdrom0", 0) == 0)
        rest = p.substr(14);
    else if (auto colon = p.find(':'); colon != std::string::npos)
    {
        std::string dev = lower.substr(0, colon);
        bool mounted = false;
        {
            std::lock_guard lock(g_rootMutex);
            if (auto it = g_contentRoots.find(dev); it != g_contentRoots.end()) { root = it->second; mounted = true; }
        }
        if (!mounted && (dev == "cache" || dev == "save" || dev == "hdd" || dev == "e" || dev == "z" || dev == "update"))
            root = g_saveDir / dev;
        rest = p.substr(colon + 1);
    }
    else
        rest = p;

    while (!rest.empty() && rest[0] == '\\') rest.erase(0, 1);
    return rest.empty() ? root : root / rest;
}

// OBJECT_ATTRIBUTES { u32 RootDirectory; u32 ObjectName (ANSI_STRING*); u32 Attributes; }
static fs::path PathFromAttributes(uint32_t attr)
{
    std::string name = RAnsiString(R32(attr + 4));
    uint32_t rootHandle = R32(attr);
    if (rootHandle)
    {
        if (auto dir = std::dynamic_pointer_cast<KFile>(HandleGet(rootHandle)))
            return dir->path / name;
    }
    return MapPath(name);
}

static void WriteIoStatus(uint32_t io, uint32_t status, uint32_t info)
{
    if (!io) return;
    W32(io, status);
    W32(io + 4, info);
}

static uint64_t ToFileTime(const FILETIME& ft) { return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }

static uint32_t OpenFile(uint32_t handlePtr, uint32_t access, uint32_t attr, uint32_t io, uint32_t disposition, uint32_t options)
{
    fs::path path = PathFromAttributes(attr);
    auto f = std::make_shared<KFile>();
    f->path = path;

    std::error_code ec;
    bool exists = fs::exists(path, ec);
    bool isDir = exists && fs::is_directory(path, ec);

    if (isDir || (options & 1))   // FILE_DIRECTORY_FILE
    {
        if (!exists)
        {
            if (disposition == 2 || disposition == 3) fs::create_directories(path, ec);
            else { diag::Note(diag::File, path.string(), "pasta nao existe"); WriteIoStatus(io, STATUS_OBJECT_NAME_NOT_FOUND, 0); return STATUS_OBJECT_NAME_NOT_FOUND; }
        }
        f->directory = true;
    }
    else
    {
        // 0 SUPERSEDE, 1 OPEN, 2 CREATE, 3 OPEN_IF, 4 OVERWRITE, 5 OVERWRITE_IF
        DWORD win;
        switch (disposition)
        {
        case 1: win = OPEN_EXISTING; break;
        case 2: win = CREATE_NEW; break;
        case 3: win = OPEN_ALWAYS; break;
        case 4: win = TRUNCATE_EXISTING; break;
        default: win = CREATE_ALWAYS; break;
        }
        bool write = access & (0x40000000 | 0x2 | 0x4 | 0x10000000);   // GENERIC_WRITE / FILE_WRITE_DATA / APPEND / GENERIC_ALL
        if (write || win != OPEN_EXISTING)
            fs::create_directories(path.parent_path(), ec);

        f->h = CreateFileW(path.c_str(), GENERIC_READ | (write ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, win, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f->h == INVALID_HANDLE_VALUE)
        {
            uint32_t st = fs::exists(path.parent_path(), ec) ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
            if (GetLastError() == ERROR_FILE_EXISTS) st = STATUS_OBJECT_NAME_COLLISION;
            Log("[fs] nao abriu: %s", path.string().c_str());
            diag::Note(diag::File, path.string(), "nao abriu (erro do Windows " + std::to_string(GetLastError()) + ")");
            WriteIoStatus(io, st, 0);
            return st;
        }
    }

    Log("[fs] abriu: %s", path.string().c_str());
    if (perf::AvLog() && path.extension() == ".bik") Log("[av] %.1f ms: video aberto %s", perf::Now(), path.filename().string().c_str());
    W32(handlePtr, HandleCreate(f));
    WriteIoStatus(io, STATUS_SUCCESS, exists ? 1 : 2);   // FILE_OPENED / FILE_CREATED
    return STATUS_SUCCESS;
}

// NtCreateFile(handle*, access, attr, io, allocSize, fileAttrs, share, disposition, [options na pilha])
KFUNC(NtCreateFile)
{
    uint32_t options = R32(Gpr32(ctx, 1) + 0x54);
    Ret(ctx, OpenFile(Gpr32(ctx, 3), Gpr32(ctx, 4), Gpr32(ctx, 5), Gpr32(ctx, 6), Gpr32(ctx, 10), options));
}

// NtOpenFile(handle*, access, attr, io, share, options)
KFUNC(NtOpenFile)
{
    Ret(ctx, OpenFile(Gpr32(ctx, 3), Gpr32(ctx, 4), Gpr32(ctx, 5), Gpr32(ctx, 6), 1, Gpr32(ctx, 8)));
}

void SignalEvent(uint32_t evHandle)   // usada tambem por xam.cpp
{
    if (!evHandle) return;
    if (auto ev = std::dynamic_pointer_cast<KEvent>(HandleGet(evHandle)))
    {
        std::lock_guard lock(g_objMutex);
        ev->signaled = true;
    }
    SignalAll();
}

// Ajustes nos dados do jogo aplicados na leitura (o arquivo no disco nao muda). Cada um so vale se os bytes originais
// conferirem (outra versao do jogo nao e alterada).
bool InputMouseCamera();   // input.cpp: o mouse move a camera
extern float g_cameraSpeedBoost;   // input.cpp
namespace
{
    struct DataPatch { const wchar_t* file; uint64_t offset; uint8_t orig[4], repl[4]; bool (*active)(); const char* what; };
    const DataPatch g_patches[] = {
        // lua.dat, BasicWeapon: AccelerationThresholdX = 0.93 -> 2.0. Acima de 93% do analogico a camera acelera
        // (gira cada vez mais rapido enquanto segura): com o mouse virando analogico isso dava atraso e depois um pulo.
        // Com 2.0 o analogico (maximo 1.0) nunca chega la. So quando o mouse controla a camera.
        { L"lua.dat", 0x19AFF9, { 0x7B, 0x14, 0x6E, 0x3F }, { 0x00, 0x00, 0x00, 0x40 }, InputMouseCamera,
          "aceleracao do analogico da camera desligada (mouse)" },
        // lua.dat, BasicWeapon: ControllerSpeeds = 144.5 -> 578 (x4). Sem a aceleracao o giro maximo do analogico ficava
        // ~80 graus/s, lento para o mouse. input.cpp divide o analogico do mouse/teclado/controle por 4 (mesma
        // velocidade de antes no uso normal, com folga para giros rapidos do mouse).
        { L"lua.dat", 0x19AE72, { 0x00, 0x80, 0x10, 0x43 }, { 0x00, 0x80, 0x10, 0x44 }, InputMouseCamera,
          "velocidade maxima do giro da camera x4 (mouse)" },
    };

    void ApplyPatches(const KFile& f, uint64_t at, uint8_t* buf, uint32_t read)
    {
        for (auto& p : g_patches)
        {
            if (p.offset + 4 <= at || p.offset >= at + read) continue;
            if (_wcsicmp(f.path.filename().c_str(), p.file) != 0 || !p.active()) continue;
            if (p.offset < at || p.offset + 4 > at + read) continue;   // patch cortado entre duas leituras: ignora
            uint8_t* d = buf + (p.offset - at);
            if (memcmp(d, p.orig, 4) != 0) continue;
            memcpy(d, p.repl, 4);
            if (p.offset == 0x19AE72) g_cameraSpeedBoost = 4.0f;
            static int logged = 0;
            if (logged++ < 4) Log("[fs] ajuste em %ls: %s", p.file, p.what);
        }
    }
}

// NtReadFile(handle, event, apc, apcCtx, io, buffer, length, byteOffset*)
KFUNC(NtReadFile)
{
    auto f = std::dynamic_pointer_cast<KFile>(HandleGet(Gpr32(ctx, 3)));
    if (!f || f->h == INVALID_HANDLE_VALUE) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    DWORD read = 0;
    {
        std::lock_guard guard(f->lock);
        int64_t off = Gpr32(ctx, 10) ? (int64_t)R64(Gpr32(ctx, 10)) : -1;
        uint64_t at = off >= 0 ? (uint64_t)off : f->pos;
        OVERLAPPED ov{};   // leitura na posicao pedida num passo so
        ov.Offset = (DWORD)at;
        ov.OffsetHigh = (DWORD)(at >> 32);
        ReadFile(f->h, G(Gpr32(ctx, 8)), Gpr32(ctx, 9), &read, &ov);
        f->pos = at + read;
        ApplyPatches(*f, at, (uint8_t*)G(Gpr32(ctx, 8)), read);
    }
    uint32_t st = (read == 0 && Gpr32(ctx, 9) != 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    WriteIoStatus(Gpr32(ctx, 7), st, read);
    SignalEvent(Gpr32(ctx, 4));
    Ret(ctx, st);
}

KFUNC(NtWriteFile)
{
    auto f = std::dynamic_pointer_cast<KFile>(HandleGet(Gpr32(ctx, 3)));
    if (!f || f->h == INVALID_HANDLE_VALUE) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    DWORD written = 0;
    {
        std::lock_guard guard(f->lock);
        int64_t off = Gpr32(ctx, 10) ? (int64_t)R64(Gpr32(ctx, 10)) : -1;
        uint64_t at = off >= 0 ? (uint64_t)off : f->pos;
        OVERLAPPED ov{};
        ov.Offset = (DWORD)at;
        ov.OffsetHigh = (DWORD)(at >> 32);
        WriteFile(f->h, G(Gpr32(ctx, 8)), Gpr32(ctx, 9), &written, &ov);
        f->pos = at + written;
    }
    WriteIoStatus(Gpr32(ctx, 7), STATUS_SUCCESS, written);
    SignalEvent(Gpr32(ctx, 4));
    Ret(ctx, STATUS_SUCCESS);
}

static void FillNetworkOpenInfo(uint32_t out, const WIN32_FILE_ATTRIBUTE_DATA& d)
{
    uint64_t size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    W64(out + 0x00, ToFileTime(d.ftCreationTime));
    W64(out + 0x08, ToFileTime(d.ftLastAccessTime));
    W64(out + 0x10, ToFileTime(d.ftLastWriteTime));
    W64(out + 0x18, ToFileTime(d.ftLastWriteTime));
    W64(out + 0x20, (size + 0xFFF) & ~0xFFFull);
    W64(out + 0x28, size);
    W32(out + 0x30, (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x10 : 0x80);
}

// NtQueryInformationFile(handle, io, info, length, class)
KFUNC(NtQueryInformationFile)
{
    auto f = std::dynamic_pointer_cast<KFile>(HandleGet(Gpr32(ctx, 3)));
    if (!f) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    uint32_t info = Gpr32(ctx, 5), cls = Gpr32(ctx, 7);
    WIN32_FILE_ATTRIBUTE_DATA d{};
    GetFileAttributesExW(f->path.c_str(), GetFileExInfoStandard, &d);
    uint64_t size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;

    switch (cls)
    {
    case 5:   // FileStandardInformation
        W64(info + 0, (size + 0xFFF) & ~0xFFFull);
        W64(info + 8, size);
        W32(info + 16, 1);
        W8(info + 20, 0);
        W8(info + 21, f->directory);
        WriteIoStatus(Gpr32(ctx, 4), 0, 24);
        break;
    case 14:  // FilePositionInformation
    {
        std::lock_guard guard(f->lock);
        W64(info, f->pos);
        WriteIoStatus(Gpr32(ctx, 4), 0, 8);
        break;
    }
    case 34:  // FileNetworkOpenInformation
        FillNetworkOpenInfo(info, d);
        WriteIoStatus(Gpr32(ctx, 4), 0, 56);
        break;
    default:
        Log("[fs] NtQueryInformationFile classe %u nao implementada", cls);
        diag::Note(diag::Kernel, "NtQueryInformationFile classe " + std::to_string(cls));
        memset(G(info), 0, Gpr32(ctx, 6));
        break;
    }
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(NtSetInformationFile)
{
    auto f = std::dynamic_pointer_cast<KFile>(HandleGet(Gpr32(ctx, 3)));
    if (!f || f->h == INVALID_HANDLE_VALUE) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    uint32_t info = Gpr32(ctx, 5), cls = Gpr32(ctx, 7);
    LARGE_INTEGER li;
    switch (cls)
    {
    case 14:  // posicao
    {
        std::lock_guard guard(f->lock);
        f->pos = R64(info);
        break;
    }
    case 20:  // fim do arquivo
        li.QuadPart = R64(info);
        SetFilePointerEx(f->h, li, nullptr, FILE_BEGIN);
        SetEndOfFile(f->h);
        break;
    default:
        Log("[fs] NtSetInformationFile classe %u nao implementada", cls);
        diag::Note(diag::Kernel, "NtSetInformationFile classe " + std::to_string(cls));
        break;
    }
    WriteIoStatus(Gpr32(ctx, 4), 0, 0);
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(NtQueryFullAttributesFile)
{
    fs::path path = PathFromAttributes(Gpr32(ctx, 3));
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d))
    {
        Ret(ctx, STATUS_OBJECT_NAME_NOT_FOUND);
        return;
    }
    FillNetworkOpenInfo(Gpr32(ctx, 4), d);
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(NtQueryVolumeInformationFile)
{
    uint32_t info = Gpr32(ctx, 5), cls = Gpr32(ctx, 7);
    memset(G(info), 0, Gpr32(ctx, 6));
    if (cls == 3)   // FileFsSizeInformation
    {
        W64(info + 0, 0x100000);   // unidades totais
        W64(info + 8, 0x80000);    // livres
        W32(info + 16, 1);         // setores por unidade
        W32(info + 20, 0x1000);    // bytes por setor
    }
    else if (cls == 1)   // FileFsVolumeInformation
    {
        W32(info + 8, 0x12345678);
    }
    WriteIoStatus(Gpr32(ctx, 4), 0, Gpr32(ctx, 6));
    Ret(ctx, STATUS_SUCCESS);
}

// NtQueryDirectoryFile(handle, event, apc, apcCtx, io, info, length, mask (ANSI_STRING*), [restart na pilha])
KFUNC(NtQueryDirectoryFile)
{
    auto f = std::dynamic_pointer_cast<KFile>(HandleGet(Gpr32(ctx, 3)));
    if (!f || !f->directory) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    uint32_t info = Gpr32(ctx, 8), length = Gpr32(ctx, 9);
    std::string mask = Gpr32(ctx, 10) ? RAnsiString(Gpr32(ctx, 10)) : "*";
    if (mask.empty()) mask = "*";

    WIN32_FIND_DATAA fd;
    for (;;)
    {
        bool ok;
        if (f->find == INVALID_HANDLE_VALUE)
        {
            f->find = FindFirstFileA((f->path / mask).string().c_str(), &fd);
            ok = f->find != INVALID_HANDLE_VALUE;
        }
        else
            ok = FindNextFileA(f->find, &fd);

        if (!ok)
        {
            WriteIoStatus(Gpr32(ctx, 7), STATUS_NO_MORE_FILES, 0);
            Ret(ctx, STATUS_NO_MORE_FILES);
            return;
        }
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
            break;
    }

    Log("[fs] diretorio %s: %s (mascara %s)", f->path.string().c_str(), fd.cFileName, mask.c_str());
    uint32_t nameLen = (uint32_t)strlen(fd.cFileName);
    memset(G(info), 0, std::min<uint32_t>(length, 0x40 + nameLen));
    W32(info + 0x00, 0);
    W32(info + 0x04, 0);
    W64(info + 0x08, ToFileTime(fd.ftCreationTime));
    W64(info + 0x10, ToFileTime(fd.ftLastAccessTime));
    W64(info + 0x18, ToFileTime(fd.ftLastWriteTime));
    W64(info + 0x20, ToFileTime(fd.ftLastWriteTime));
    uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    W64(info + 0x28, size);
    W64(info + 0x30, (size + 0xFFF) & ~0xFFFull);
    W32(info + 0x38, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x10 : 0x80);
    W32(info + 0x3C, nameLen);
    memcpy(G(info + 0x40), fd.cFileName, std::min(nameLen, length > 0x40 ? length - 0x40 : 0));
    WriteIoStatus(Gpr32(ctx, 7), STATUS_SUCCESS, 0x40 + nameLen);
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(NtFlushBuffersFile)
{
    if (auto f = std::dynamic_pointer_cast<KFile>(HandleGet(Gpr32(ctx, 3))); f && f->h != INVALID_HANDLE_VALUE)
        FlushFileBuffers(f->h);
    Ret(ctx, STATUS_SUCCESS);
}
