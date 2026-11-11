// Perfilador por amostragem das threads do Xbox (FCRECOMP_PROFILE=inicio:duracao, em segundos).
// A cada ~1 ms pausa cada thread registrada, le o RIP e conta em qual funcao recompilada ela esta
// (procurando na lista de funcoes do jogo). Depois do tempo pedido escreve no log as funcoes mais frequentes.
#include "guest.h"
#include <fstream>
#include <sstream>
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dbghelp.lib")

namespace
{
    std::string ExeSymbol(uint64_t rva);   // nome da funcao do exe pelo .map ("" sem o .map); definida mais abaixo
    std::string DllSymbol(uint64_t rip);   // "dll funcao" pela exportacao mais proxima ("" se nao achar)

    struct Sampled { HANDLE handle; std::string name; uint64_t samples = 0; std::unordered_map<uint64_t, uint64_t> hits; };
    std::mutex g_mutex;
    std::vector<Sampled*> g_threads;
    std::vector<std::pair<uintptr_t, uint32_t>> g_funcs;   // (endereco no PC, endereco no Xbox), ordenado
    bool g_enabled = false;

    // funcao recompilada que contem o endereco (0 = fora do codigo recompilado)
    uint32_t FindGuest(uintptr_t rip)
    {
        auto it = std::upper_bound(g_funcs.begin(), g_funcs.end(), std::make_pair(rip, UINT32_MAX));
        if (it == g_funcs.begin()) return 0;
        --it;
        if (rip - it->first > 0x40000) return 0;   // longe demais: runtime/sistema
        return it->second;
    }

    void Report()
    {
        std::lock_guard lock(g_mutex);
        HMODULE exe = GetModuleHandleW(nullptr);
        for (auto* t : g_threads)
        {
            if (!t->samples) continue;
            std::unordered_map<uint32_t, uint64_t> byFunc;
            std::unordered_map<uint64_t, uint64_t> outside;
            std::unordered_map<std::string, uint64_t> byName;   // pelo .map: nome exato (jogo ou runtime)
            for (auto& [rip, n] : t->hits)
            {
                // com o .map o nome e exato; so pela tabela do jogo, o codigo do runtime que fica logo depois da
                // ultima funcao do jogo era contado como ela (o "sub_823140D8" de 25% era o xdraw::Draw)
                if (rip >= (uint64_t)exe && rip < (uint64_t)exe + 0x10000000)
                    if (std::string s = ExeSymbol(rip - (uint64_t)exe); !s.empty()) { byName[s] += n; continue; }
                uint32_t g = FindGuest((uintptr_t)rip);
                if (g) byFunc[g] += n;
                else
                {
                    // DLL do Windows: nome da funcao exportada mais proxima (ex.: ntdll RtlAllocateHeap), sem o deslocamento
                    if (std::string s = DllSymbol(rip); !s.empty()) { byName[s] += n; continue; }
                    // fora do codigo recompilado: agrupa por modulo (exe = runtime, outros = Windows/driver)
                    HMODULE mod = nullptr;
                    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)rip, &mod);
                    outside[mod == exe ? (rip & ~0xFFFull) : (uint64_t)mod] += n;
                }
            }
            std::vector<std::pair<uint64_t, std::string>> rows;
            for (auto& [s, n] : byName) rows.push_back({ n, s });
            for (auto& [g, n] : byFunc) { char b[64]; snprintf(b, sizeof(b), "sub_%08X", g); rows.push_back({ n, b }); }
            for (auto& [m, n] : outside)
            {
                char b[300] = "?";
                HMODULE mod = (HMODULE)m;
                wchar_t path[MAX_PATH];
                if (m & 0xFFF || !GetModuleFileNameW(mod, path, MAX_PATH)) snprintf(b, sizeof(b), "runtime+%llX", (unsigned long long)(m - (uint64_t)exe));
                else snprintf(b, sizeof(b), "%ls", wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path);
                rows.push_back({ n, b });
            }
            std::sort(rows.rbegin(), rows.rend());
            Log("[perfil] thread %s: %llu amostras", t->name.c_str(), (unsigned long long)t->samples);
            // enderecos do executavel mais amostrados (RVA em blocos de 64 bytes) - traduza com toolsddr2func.py
            {
                std::unordered_map<uint64_t, uint64_t> rva;
                for (auto& [rip, n] : t->hits)
                    if (rip >= (uint64_t)exe && rip < (uint64_t)exe + 0x10000000) rva[(rip - (uint64_t)exe) & ~63ull] += n;
                std::vector<std::pair<uint64_t, uint64_t>> top;
                for (auto& [r, n] : rva) top.push_back({ n, r });
                std::sort(top.rbegin(), top.rend());
                std::string line;
                for (size_t i = 0; i < top.size() && i < 8; i++) { char b[40]; snprintf(b, sizeof(b), " %llX(%.1f%%)", (unsigned long long)top[i].second, top[i].first * 100.0 / t->samples); line += b; }
                Log("[perfil]   RVAs do exe mais quentes:%s", line.c_str());
            }
            for (size_t i = 0; i < rows.size() && i < 12; i++)
                Log("[perfil]   %5.1f%%  %s", rows[i].first * 100.0 / t->samples, rows[i].second.c_str());
        }
    }

    std::atomic<ULONGLONG> g_sampleEnd{ 0 };   // fim da medicao em andamento (titulo da janela)
    std::atomic<int> g_measured{ 0 }, g_byKey{ 0 };

    void Sample(int seconds)
    {
        Log("[perfil] amostrando por %d s", seconds);
        ULONGLONG end = GetTickCount64() + seconds * 1000ull;
        g_sampleEnd = end;
        while (GetTickCount64() < end)
        {
            Sleep(1);
            std::lock_guard lock(g_mutex);
            for (auto* t : g_threads)
            {
                if (SuspendThread(t->handle) == (DWORD)-1) continue;
                CONTEXT c{};
                c.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(t->handle, &c)) { t->hits[c.Rip]++; t->samples++; }
                ResumeThread(t->handle);
            }
        }
        Report();
    }

    std::atomic<bool> g_keyRequest{ false };

    // delay < 0: espera a tecla F3 (quem joga aperta no lugar a medir); pode medir varias vezes
    void SamplerThread(int delay, int seconds)
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        timeBeginPeriod(1);
        if (delay >= 0) { Sleep(delay * 1000); Sample(seconds); return; }
        g_byKey = 1;
        for (int n = 1;; n++)
        {
            while (!g_keyRequest.exchange(false)) Sleep(50);
            Log("[perfil] medicao %d (F3)", n);
            Beep(880, 150);   // quem joga em tela cheia nao ve o titulo: um bipe ao comecar, dois ao terminar
            Sample(seconds);
            g_measured = n;
            Beep(1320, 120); Sleep(80); Beep(1320, 120);
            std::lock_guard lock(g_mutex);
            for (auto* t : g_threads) { t->hits.clear(); t->samples = 0; }
        }
    }
}

// tecla F3 (diag.cpp): pede uma medicao quando FCRECOMP_PROFILE=F3
bool ProfilerKeyRequest()
{
    if (!g_enabled) return false;
    g_keyRequest = true;
    return true;
}

// texto para o titulo da janela quando a medicao e pela tecla F3 (para quem joga ver que ela esta rodando)
std::string ProfilerStatus()
{
    if (!g_byKey) return "";
    ULONGLONG now = GetTickCount64(), end = g_sampleEnd;
    char b[80];
    if (end > now) snprintf(b, sizeof(b), "  |  MEDINDO... faltam %llu s", (end - now + 999) / 1000);
    else if (g_measured) snprintf(b, sizeof(b), "  |  %d medicao(oes) feita(s) - F3 mede de novo, F12 grava", g_measured.load());
    else snprintf(b, sizeof(b), "  |  F3 = medir aqui");
    return b;
}

// Funcao recompilada (endereco no Xbox) que contem um endereco de codigo do PC; 0 = fora do jogo
uint32_t GuestFunctionAt(uintptr_t rip)
{
    static std::once_flag once;
    std::call_once(once, [] {
        if (g_funcs.empty())
        {
            ForEachGuestFunction([](uint32_t address, GuestFunc* fn) { g_funcs.push_back({ (uintptr_t)fn, address }); });
            std::sort(g_funcs.begin(), g_funcs.end());
        }
    });
    return FindGuest(rip);
}

// chamada por cada thread do Xbox ao comecar
void ProfilerRegisterThread(const char* name)
{
    // toda thread fica registrada (o vigia de travamento tambem le as pilhas); a amostragem so com FCRECOMP_PROFILE
    static const char* env = getenv("FCRECOMP_PROFILE");
    static std::once_flag once;
    if (env) std::call_once(once, [] {
        GuestFunctionAt(0);   // monta a tabela de funcoes
        g_enabled = true;
        const char* e = getenv("FCRECOMP_PROFILE");
        const char* colon = strchr(e, ':');
        if (e[0] == 'F' || e[0] == 'f')   // FCRECOMP_PROFILE=F3[:segundos]: mede quando apertar F3 (padrao 10 s)
            std::thread(SamplerThread, -1, colon ? atoi(colon + 1) : 10).detach();
        else
            std::thread(SamplerThread, colon ? atoi(e) : 0, colon ? atoi(colon + 1) : atoi(e)).detach();
    });
    auto* t = new Sampled{};
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    t->name = name ? name : "?";
    std::lock_guard lock(g_mutex);
    g_threads.push_back(t);
}

// ---------------------------------------------------------------- pilhas nativas (vigia de travamento)
// Pausa cada thread registrada, desenrola a pilha do PC (RtlVirtualUnwind) e traduz cada endereco: funcao do jogo
// (sub_XXXXXXXX), funcao do runtime (pelo FCRecomp.map ao lado do exe) ou DLL do Windows/driver (+ simbolo exportado).
namespace
{
    std::vector<std::pair<uint64_t, std::string>> g_mapSyms;   // RVA -> nome (do .map do linker)

    void LoadMap()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring path = exe;
            path = path.substr(0, path.find_last_of(L'\\') + 1) + L"FCRecomp.map";
            std::ifstream f(path);
            uint64_t base = 0x140000000;
            for (std::string line; std::getline(f, line);)
            {
                if (line.find("Preferred load address is") != std::string::npos)
                    base = strtoull(line.substr(line.rfind(' ') + 1).c_str(), nullptr, 16);
                // " 0001:00012345       ?nome@@...      0000000140013345 f   obj"
                std::istringstream in(line);
                std::string sec, name, addr;
                if (!(in >> sec >> name >> addr) || sec.size() != 13 || sec[4] != ':' || addr.size() != 16) continue;
                uint64_t va = strtoull(addr.c_str(), nullptr, 16);
                if (va <= base) continue;
                char und[512];
                if (name[0] == '?' && UnDecorateSymbolName(name.c_str(), und, sizeof(und), UNDNAME_NAME_ONLY)) name = und;
                g_mapSyms.push_back({ va - base, name });
            }
            std::sort(g_mapSyms.begin(), g_mapSyms.end());
        });
    }

    std::string ExeSymbol(uint64_t rva)
    {
        LoadMap();
        if (g_mapSyms.empty()) return "";
        auto it = std::upper_bound(g_mapSyms.begin(), g_mapSyms.end(), std::make_pair(rva, std::string(1, (char)0xFF)));
        if (it == g_mapSyms.begin()) return "";
        --it;
        return it->second.find("sub_") != std::string::npos ? it->second : "runtime " + it->second;
    }

    std::string Describe(uint64_t rip)
    {
        char b[600];
        if (g_mapSyms.empty())   // sem o .map: estimativa pela tabela de funcoes do jogo
            if (uint32_t g = GuestFunctionAt((uintptr_t)rip)) { snprintf(b, sizeof(b), "jogo sub_%08X", g); return b; }
        HMODULE exe = GetModuleHandleW(nullptr), mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)rip, &mod);
        if (mod == exe)
        {
            uint64_t rva = rip - (uint64_t)exe;
            auto it = std::upper_bound(g_mapSyms.begin(), g_mapSyms.end(), std::make_pair(rva, std::string(1, (char)0xFF)));
            if (it != g_mapSyms.begin()) { --it; snprintf(b, sizeof(b), "%s %s+%llX", it->second.find("sub_") != std::string::npos ? "jogo" : "runtime", it->second.c_str(), (unsigned long long)(rva - it->first)); }
            else snprintf(b, sizeof(b), "runtime +%llX", (unsigned long long)rva);
            return b;
        }
        wchar_t path[MAX_PATH] = L"?";
        if (mod) GetModuleFileNameW(mod, path, MAX_PATH);
        const wchar_t* file = wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path;
        alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256]{};
        auto* sym = (SYMBOL_INFO*)symBuf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(), rip, &disp, sym)) snprintf(b, sizeof(b), "%ls!%s+%llX", file, sym->Name, (unsigned long long)disp);
        else snprintf(b, sizeof(b), "%ls+%llX", file, (unsigned long long)(rip - (uint64_t)mod));
        return b;
    }

    void InitSymbols()
    {
        static std::once_flag symOnce;
        std::call_once(symOnce, [] { SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); SymInitialize(GetCurrentProcess(), nullptr, TRUE); });
    }

    std::string DllSymbol(uint64_t rip)
    {
        HMODULE exe = GetModuleHandleW(nullptr), mod = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)rip, &mod) || mod == exe)
            return "";
        InitSymbols();
        wchar_t path[MAX_PATH];
        if (!GetModuleFileNameW(mod, path, MAX_PATH)) return "";
        const wchar_t* file = wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path;
        alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256]{};
        auto* sym = (SYMBOL_INFO*)symBuf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (!SymFromAddr(GetCurrentProcess(), rip, &disp, sym)) return "";
        char b[400];
        snprintf(b, sizeof(b), "%ls %s", file, sym->Name);
        return b;
    }
}

std::string NativeStacks()
{
    LoadMap();
    GuestFunctionAt(0);
    InitSymbols();

    struct Stack { std::string name; std::vector<uint64_t> frames; };
    std::vector<Stack> stacks;
    {
        std::lock_guard lock(g_mutex);
        for (auto* t : g_threads)
        {
            if (GetThreadId(t->handle) == GetCurrentThreadId()) continue;
            DWORD code = 0;
            if (GetExitCodeThread(t->handle, &code) && code != STILL_ACTIVE) continue;
            Stack st{ t->name };
            uint64_t frames[40]; int n = 0;
            if (SuspendThread(t->handle) == (DWORD)-1) continue;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(t->handle, &c))
            {
                // so desenrola enquanto a thread esta pausada (sem alocar memoria aqui)
                for (; n < 40 && c.Rip; n++)
                {
                    frames[n] = c.Rip;
                    DWORD64 imageBase = 0;
                    auto* fn = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
                    if (!fn) { if (n) break; c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8; continue; }   // funcao folha (so no topo)
                    void* handlerData; DWORD64 establisher;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &establisher, nullptr);
                }
            }
            ResumeThread(t->handle);
            st.frames.assign(frames, frames + n);
            stacks.push_back(std::move(st));
        }
    }
    std::string out;
    for (auto& st : stacks)
    {
        out += "== " + st.name + "\n";
        for (uint64_t f : st.frames) out += "    " + Describe(f) + "\n";
    }
    return out;
}
