#include "thread.h"
#include <windows.h>
#include <chrono>

namespace
{
    thread_local std::shared_ptr<GuestThread> t_current;
    std::atomic<uint32_t> g_nextThreadId{ 1 };

    struct ThreadExit { uint32_t code; };

    // Area de cada thread, numa pagina de 4 KB apontada por r13 (levantado no codigo do jogo):
    //   0x000 "PCR" (dados do processador). O jogo le +0x100 (KTHREAD atual), +0x10C (numero do processador, byte) e
    //         +0x150 (com 0, o erro vai para o KTHREAD). Copias de r13 no resto do codigo so servem de "dono" de travas.
    //   0x400 bloco TLS apontado por PCR+0 (o jogo nao le: os valores de TLS ficam no runtime, KeTlsGetValue)
    //   0x800 KTHREAD. O jogo le +0x58, +0x14C (id da thread) e +0x160 (ultimo erro); o runtime grava +4 (terminou)
    //         e +0x140 (codigo de saida), que o GetExitCodeThread do jogo le.
    constexpr uint32_t THREAD_AREA_SIZE = 0x1000;
    constexpr uint32_t TLS_OFFSET = 0x400;
    constexpr uint32_t KTHREAD_OFFSET = 0x800;
}

std::shared_ptr<GuestThread> CurrentGuestThread() { return t_current; }

namespace
{
    std::mutex g_allMutex;
    std::vector<std::weak_ptr<GuestThread>> g_all;

    void Register(const std::shared_ptr<GuestThread>& t)
    {
        std::lock_guard lock(g_allMutex);
        g_all.push_back(t);
    }
}

std::string GuestBacktrace(GuestCpu& c)
{
    std::string chain;
    char buf[32];
    snprintf(buf, sizeof(buf), "lr=%08X", (uint32_t)Lr(c));
    chain += buf;
    uint32_t sp = Gpr32(c, 1);
    for (int i = 0; i < 16 && sp >= 0x10000000 && sp < 0x80000000; i++)
    {
        uint32_t next = R32(sp);
        if (next <= sp || next - sp > 0x100000) break;
        snprintf(buf, sizeof(buf), " %08X", R32(next - 8));
        chain += buf;
        sp = next;
    }
    return chain;
}

void DumpAllThreads()
{
    std::lock_guard lock(g_allMutex);
    Log("[dump] ===== estado das threads =====");
    for (auto& w : g_all)
        if (auto t = w.lock(); t && t->ctx && !t->finished)
            Log("[dump] t%u %-8s %s", t->id, t->name, GuestBacktrace(*t->ctx).c_str());

    // FCRECOMP_PEEK=endereco[,endereco...]: mostra valores da memoria do jogo (com "*" = segue o ponteiro)
    if (const char* peek = getenv("FCRECOMP_PEEK"))
    {
        std::string s = peek;
        size_t pos = 0;
        while (pos < s.size())
        {
            size_t comma = s.find(',', pos);
            std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            bool deref = !item.empty() && item[0] == '*';
            uint32_t a = (uint32_t)strtoul(item.c_str() + (deref ? 1 : 0), nullptr, 16);
            uint32_t v = R32(a);
            if (deref) Log("[dump] *[%08X] = [%08X] = %08X", a, v, R32(v));
            else Log("[dump] [%08X] = %08X", a, v);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
}

std::string DescribeThreads()
{
    std::string out;
    std::lock_guard lock(g_allMutex);
    uint64_t now = GetTickCount64();
    for (auto& w : g_all)
        if (auto t = w.lock(); t && t->ctx && !t->finished)
        {
            char head[160];
            uint64_t since = t->waitSince;
            if (since)
            {
                std::lock_guard wl(t->waitMutex);
                snprintf(head, sizeof(head), "t%u %s: ESPERANDO ha %.1f s (chamada de %08X) em ", t->id, t->name, (now - since) / 1000.0, t->waitLr.load());
                out += head; out += t->waitWhat;
            }
            else
            {
                snprintf(head, sizeof(head), "t%u %s: rodando", t->id, t->name);
                out += head;
            }
            out += "\n    pilha: " + GuestBacktrace(*t->ctx) + "\n";
        }
    return out;
}

uint32_t CurrentThreadId()
{
    return t_current ? t_current->id : 0xFFFF;
}

static GuestCpu* SetupGuestContext(std::shared_ptr<GuestThread> t);
void ProfilerRegisterThread(const char* name);

// Transforma a thread atual do PC numa thread "do Xbox" (para callbacks do runtime, ex. interrupcoes da GPU)
GuestCpu* AttachGuestThread(const char* name)
{
    auto t = std::make_shared<GuestThread>();
    t->id = g_nextThreadId++;
    t->name = name;
    t->stackSize = 0x40000;
    t_current = t;
    return SetupGuestContext(t);
}

static GuestCpu* SetupGuestContext(std::shared_ptr<GuestThread> t)
{
    ProfilerRegisterThread(t->name);   // perfilador (FCRECOMP_PROFILE)
    Log("[thread] %s = id %lu", t->name ? t->name : "?", GetCurrentThreadId());   // cruza com o uso de CPU por thread do Windows
    uint32_t stackSize = std::max<uint32_t>(t->stackSize, 0x40000);
    t->stack = GuestAlloc(stackSize, 0x1000);
    t->pcr = GuestAlloc(THREAD_AREA_SIZE, 0x1000);
    uint32_t tls = t->pcr + TLS_OFFSET;
    uint32_t kthread = t->pcr + KTHREAD_OFFSET;
    W32(t->pcr + 0x000, tls);
    W32(t->pcr + 0x100, kthread);
    W8(t->pcr + 0x10C, (uint8_t)(t->id % 6));    // numero do processador
    W32(kthread + 0x14C, t->id);
    t->guestObject = kthread;

    auto* ctx = new GuestCpu{};
    t->ctx = ctx;
    Register(t);
    SetGpr(*ctx, 1, t->stack + stackSize - 0x100);
    SetGpr(*ctx, 13, t->pcr);
    ResetFpscr(*ctx);
    return ctx;
}

void RunGuestThread(std::shared_ptr<GuestThread> t)
{
    t_current = t;

    // espera se foi criada suspensa
    while (t->suspendCount > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    GuestCpu* ctx = SetupGuestContext(t);

    try
    {
        if (t->xapiStartup)
        {
            SetGpr(*ctx, 3, t->entry);
            SetGpr(*ctx, 4, t->param);
            GuestCall(*ctx, t->xapiStartup);
        }
        else
        {
            SetGpr(*ctx, 3, t->param);
            GuestCall(*ctx, t->entry);
        }
        t->exitCode = Gpr32(*ctx, 3);
    }
    catch (const ThreadExit& e)
    {
        t->exitCode = e.code;
    }

    Log("[thread] thread %u terminou (codigo %u)", t->id, t->exitCode);
    {
        std::lock_guard lock(g_objMutex);
        // GetExitCodeThread do jogo le direto o KTHREAD: +4 = sinalizado (terminou), +0x140 = codigo de saida.
        // Sem isso ele ve 259 (ainda rodando) para sempre (bug 6: carregamento infinito no fim da fase)
        if (t->guestObject && !getenv("FCRECOMP_SEMFIMTHREAD"))
        {
            W32(t->guestObject + 0x140, t->exitCode);
            W32(t->guestObject + 4, 1);
        }
        t->finished = true;
    }
    SignalAll();
    delete ctx;
}

std::shared_ptr<GuestThread> CreateGuestThread(uint32_t entry, uint32_t param, uint32_t xapiStartup, uint32_t stackSize, bool suspended)
{
    auto t = std::make_shared<GuestThread>();
    t->id = g_nextThreadId++;
    t->entry = entry;
    t->param = param;
    t->xapiStartup = xapiStartup;
    t->stackSize = stackSize;
    t->suspendCount = suspended ? 1 : 0;
    {
        // nome unico (perfilador e log de threads): "jogo N (entrada)"
        char name[48];
        snprintf(name, sizeof(name), "jogo %u (%08X)", t->id, entry);
        t->name = _strdup(name);
    }
    t->host = std::thread(RunGuestThread, t);
    t->host.detach();
    return t;
}

// ExCreateThread(handle*, stackSize, threadId*, xapiStartup, startAddress, startContext, flags)
KFUNC(ExCreateThread)
{
    bool suspended = Gpr32(ctx, 9) & 1;
    auto t = CreateGuestThread(Gpr32(ctx, 7), Gpr32(ctx, 8), Gpr32(ctx, 6), Gpr32(ctx, 4), suspended);
    Log("[thread] ExCreateThread id=%u entrada=%08X xapi=%08X param=%08X suspensa=%d", t->id, Gpr32(ctx, 7), Gpr32(ctx, 6), Gpr32(ctx, 8), suspended);
    if (Gpr32(ctx, 3)) W32(Gpr32(ctx, 3), HandleCreate(t));
    if (Gpr32(ctx, 5)) W32(Gpr32(ctx, 5), t->id);
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(ExTerminateThread)
{
    throw ThreadExit{ Gpr32(ctx, 3) };
}

KFUNC(NtResumeThread)
{
    auto t = std::dynamic_pointer_cast<GuestThread>(HandleGet(Gpr32(ctx, 3)));
    if (!t) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    int32_t prev = t->suspendCount.load();
    if (prev > 0) t->suspendCount--;
    Log("[thread] NtResumeThread id=%u (contador %d)", t->id, prev);
    if (Gpr32(ctx, 4)) W32(Gpr32(ctx, 4), prev);
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(NtSuspendThread)
{
    auto t = std::dynamic_pointer_cast<GuestThread>(HandleGet(Gpr32(ctx, 3)));
    if (!t) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    Log("[thread] NtSuspendThread em thread ja rodando (ignorado)");
    if (Gpr32(ctx, 4)) W32(Gpr32(ctx, 4), t->suspendCount.load());
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(KeSetAffinityThread)
{
    if (Gpr32(ctx, 5)) W32(Gpr32(ctx, 5), 0x3F);
    Ret(ctx, 0x3F);
}

KFUNC(KeSetBasePriorityThread) { Ret(ctx, 0); }

// Diagnostico (bug 6, carregamento infinito no fim da fase): grupos de threads de trabalho do jogo.
// sub_82400BB8 cria o grupo (eventos "tem trabalho" em +68 e "pode sair" em +72, N threads em +76/+80);
// sub_82400D68 desliga: liga "pode sair" e espera cada thread terminar.
#include "guest.h"
#ifndef PREDATOR_EDITOR   // gancho so do jogo: no editor de mapas (ige.xex) o endereco e de outra funcao
GUEST_DECLARE_ORIGINAL(sub_82400BB8);
GUEST_FUNC(sub_82400BB8)
{
    GUEST_ORIGINAL(sub_82400BB8)(ctx, base);
    uint32_t p = Gpr32(ctx, 3);
    Log("[grupo] criado %08X: trabalho=%08X sair=%08X threads=%u (thread %u)", p, R32(p + 68), R32(p + 72), R32(p + 76), CurrentThreadId());
}

GUEST_DECLARE_ORIGINAL(sub_82400D68);
GUEST_FUNC(sub_82400D68)
{
    uint32_t p = Gpr32(ctx, 3);
    std::string hs;
    uint32_t n = R32(p + 76), list = R32(p + 80);
    for (uint32_t i = 0; i < n && i < 16 && list; i++) { char b[16]; snprintf(b, sizeof(b), " %08X", R32(list + i * 4)); hs += b; }
    Log("[grupo] desligando %08X: trabalho=%08X sair=%08X threads=%u:%s (thread %u)", p, R32(p + 68), R32(p + 72), n, hs.c_str(), CurrentThreadId());
    GUEST_ORIGINAL(sub_82400D68)(ctx, base);
    Log("[grupo] desligado %08X", p);
}

GUEST_DECLARE_ORIGINAL(sub_824011A8);
GUEST_FUNC(sub_824011A8)
{
    uint32_t p = Gpr32(ctx, 3);
    Log("[grupo] thread %u comecou no grupo %08X (trabalho=%08X sair=%08X)", CurrentThreadId(), p, R32(p + 68), R32(p + 72));
    GUEST_ORIGINAL(sub_824011A8)(ctx, base);
    Log("[grupo] thread %u saiu do grupo %08X", CurrentThreadId(), p);
}
#endif
