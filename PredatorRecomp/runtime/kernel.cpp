// Kernel do Xbox 360 (xboxkrnl.exe): objetos, threads, sincronizacao, memoria
#include "diag.h"
#include "objects.h"
#include "thread.h"
#include <windows.h>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <thread>
#include <intrin.h>
#include <algorithm>
#include <vector>
[[noreturn]] void SairDoProcesso(unsigned codigo);   // main.cpp

#pragma comment(lib, "Synchronization.lib")   // WaitOnAddress / WakeByAddressSingle

std::mutex g_objMutex;
std::condition_variable g_objCv;

namespace
{
    std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_handles;
    std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_guestObjects;
    uint32_t g_nextHandle = 0x10000;
}

void SignalAll() { g_objCv.notify_all(); }

uint32_t HandleCreate(std::shared_ptr<KObject> obj)
{
    std::lock_guard lock(g_objMutex);
    uint32_t h = g_nextHandle;
    g_nextHandle += 4;
    g_handles[h] = std::move(obj);
    return h;
}

std::shared_ptr<KObject> HandleGet(uint32_t handle)
{
    if (handle == 0xFFFFFFFE)
        return CurrentGuestThread();
    std::lock_guard lock(g_objMutex);
    auto it = g_handles.find(handle);
    return it != g_handles.end() ? it->second : nullptr;
}

void HandleClose(uint32_t handle)
{
    std::lock_guard lock(g_objMutex);
    g_handles.erase(handle);
}

// Cria (na primeira vez) o objeto do PC que corresponde a um KEVENT/KSEMAPHORE/... na memoria do jogo
std::shared_ptr<KObject> ObjectFromGuest(uint32_t addr)
{
    std::lock_guard lock(g_objMutex);
    auto it = g_guestObjects.find(addr);
    if (it != g_guestObjects.end())
        return it->second;

    uint8_t type = R8(addr);
    int32_t signal = (int32_t)R32(addr + 4);
    std::shared_ptr<KObject> obj;
    switch (type)
    {
    case 0: obj = std::make_shared<KEvent>(true, signal != 0); break;    // NotificationEvent
    case 1: obj = std::make_shared<KEvent>(false, signal != 0); break;   // SynchronizationEvent
    case 2: obj = std::make_shared<KMutant>(); break;
    case 5: obj = std::make_shared<KSemaphore>(signal, (int32_t)R32(addr + 0x10)); break;
    case 8: obj = std::make_shared<KEvent>(true, signal != 0); break;    // timers
    case 9: obj = std::make_shared<KEvent>(false, signal != 0); break;
    default:
        Log("[kernel] objeto do jogo com tipo desconhecido %u em %08X", type, addr);
        diag::Note(diag::Kernel, "objeto do jogo com tipo desconhecido " + std::to_string(type));
        obj = std::make_shared<KEvent>(false, signal != 0);
        break;
    }
    obj->guestObject = addr;
    g_guestObjects[addr] = obj;
    return obj;
}

// Hora do sistema que o jogo ve (FILETIME, UTC). A repeticao de um roteiro gravado (input.cpp) desloca o relogio para a
// hora da gravacao: nomes com data (mapas do editor, saves) e o que o jogo sorteia pela hora saem iguais.
std::atomic<int64_t> g_relogioDesvio{ 0 };
uint64_t HoraDoJogo()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) + (uint64_t)g_relogioDesvio.load();
}

int64_t TimeoutFromGuest(uint32_t timeoutPtr)
{
    if (!timeoutPtr)
        return -1;
    int64_t t = (int64_t)R64(timeoutPtr);
    if (t < 0)
        return (-t) / 10000;   // relativo, 100ns -> ms
    if (t == 0)
        return 0;
    // absoluto (FILETIME): converte para relativo
    int64_t now = (int64_t)HoraDoJogo();
    return t > now ? (t - now) / 10000 : 0;
}

// Diagnostico: anota na thread do jogo em que ela esta esperando (F12 -> threads.txt)
namespace
{
    std::string DescribeObject(const std::shared_ptr<KObject>& o)
    {
        char b[96];
        if (!o) return "(nulo)";
        if (auto e = std::dynamic_pointer_cast<KEvent>(o)) snprintf(b, sizeof(b), "evento%s %s", e->manual ? " manual" : "", e->signaled ? "sinalizado" : "nao sinalizado");
        else if (auto m = std::dynamic_pointer_cast<KSemaphore>(o)) snprintf(b, sizeof(b), "semaforo %d/%d", m->count, m->max);
        else if (auto x = std::dynamic_pointer_cast<KMutant>(o)) snprintf(b, sizeof(b), "mutex (dono t%u)", x->owner);
        else if (auto t = std::dynamic_pointer_cast<GuestThread>(o)) snprintf(b, sizeof(b), "fim da thread t%u %s", t->id, t->name);
        else snprintf(b, sizeof(b), "objeto");
        std::string r = b;
        if (o->guestObject) { snprintf(b, sizeof(b), " @%08X", o->guestObject); r += b; }
        return r;
    }
    struct WaitMark
    {
        std::shared_ptr<GuestThread> t;
        WaitMark(const std::vector<std::shared_ptr<KObject>>& objs, bool waitAll, int64_t timeoutMs) : t(CurrentGuestThread())
        {
            if (!t || timeoutMs == 0) { t = nullptr; return; }
            std::string w = objs.size() > 1 ? (waitAll ? "todos de: " : "qualquer de: ") : "";
            for (size_t i = 0; i < objs.size() && i < 8; i++) { if (i) w += ", "; w += DescribeObject(objs[i]); }
            if (timeoutMs > 0) w += " (limite " + std::to_string(timeoutMs) + " ms)";
            { std::lock_guard l(t->waitMutex); t->waitWhat = w; }
            t->waitLr = t->ctx ? (uint32_t)t->ctx->lr : 0;
            t->waitSince = GetTickCount64();
        }
        ~WaitMark() { if (t) t->waitSince = 0; }
    };
}

uint32_t WaitObjects(const std::vector<std::shared_ptr<KObject>>& objs, bool waitAll, int64_t timeoutMs)
{
    uint32_t tid = CurrentThreadId();
    WaitMark mark(objs, waitAll, timeoutMs);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);

    std::unique_lock lock(g_objMutex);
    for (;;)
    {
        if (waitAll)
        {
            bool all = true;
            for (auto& o : objs)
                all = all && o && o->IsSignaled(tid);
            if (all)
            {
                for (auto& o : objs) o->Acquire(tid);
                return STATUS_SUCCESS;
            }
        }
        else
        {
            for (size_t i = 0; i < objs.size(); i++)
            {
                if (objs[i] && objs[i]->IsSignaled(tid))
                {
                    objs[i]->Acquire(tid);
                    return STATUS_SUCCESS + (uint32_t)i;
                }
            }
        }

        if (timeoutMs == 0)
            return STATUS_TIMEOUT;
        if (timeoutMs < 0)
            g_objCv.wait_for(lock, std::chrono::milliseconds(50));
        else if (g_objCv.wait_until(lock, deadline) == std::cv_status::timeout && std::chrono::steady_clock::now() >= deadline)
        {
            // ultima checagem antes de desistir
            timeoutMs = 0;
        }
    }
}

// ---------------------------------------------------------------- sincronizacao

KFUNC(KeWaitForSingleObject)
{
    auto obj = ObjectFromGuest(Gpr32(ctx, 3));
    Ret(ctx, WaitObjects({ obj }, true, TimeoutFromGuest(Gpr32(ctx, 7))));
}

KFUNC(NtWaitForSingleObjectEx)
{
    auto obj = HandleGet(Gpr32(ctx, 3));
    if (!obj) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    Ret(ctx, WaitObjects({ obj }, true, TimeoutFromGuest(Gpr32(ctx, 6))));
}

KFUNC(NtWaitForMultipleObjectsEx)
{
    uint32_t count = Gpr32(ctx, 3);
    std::vector<std::shared_ptr<KObject>> objs;
    for (uint32_t i = 0; i < count; i++)
        objs.push_back(HandleGet(R32(Gpr32(ctx, 4) + i * 4)));
    Ret(ctx, WaitObjects(objs, Gpr32(ctx, 5) == 0, TimeoutFromGuest(Gpr32(ctx, 8))));
}

KFUNC(NtCreateEvent)
{
    auto ev = std::make_shared<KEvent>(Gpr32(ctx, 5) == 0, Gpr32(ctx, 6) != 0);
    W32(Gpr32(ctx, 3), HandleCreate(ev));
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(NtSetEvent)
{
    auto ev = std::dynamic_pointer_cast<KEvent>(HandleGet(Gpr32(ctx, 3)));
    if (!ev) { Log("[evento] NtSetEvent com handle invalido %08X (lr=%08X)", Gpr32(ctx, 3), (uint32_t)Lr(ctx)); Ret(ctx, STATUS_INVALID_HANDLE); return; }
    {
        std::lock_guard lock(g_objMutex);
        if (Gpr32(ctx, 4)) W32(Gpr32(ctx, 4), ev->signaled);
        ev->signaled = true;
    }
    SignalAll();
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(NtClearEvent)
{
    auto ev = std::dynamic_pointer_cast<KEvent>(HandleGet(Gpr32(ctx, 3)));
    if (!ev) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    std::lock_guard lock(g_objMutex);
    ev->signaled = false;
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(KeSetEvent)
{
    auto ev = std::dynamic_pointer_cast<KEvent>(ObjectFromGuest(Gpr32(ctx, 3)));
    uint32_t prev = 0;
    if (ev)
    {
        std::lock_guard lock(g_objMutex);
        prev = ev->signaled;
        ev->signaled = true;
        W32(Gpr32(ctx, 3) + 4, 1);
    }
    SignalAll();
    Ret(ctx, prev);
}

KFUNC(KeResetEvent)
{
    auto ev = std::dynamic_pointer_cast<KEvent>(ObjectFromGuest(Gpr32(ctx, 3)));
    uint32_t prev = 0;
    if (ev)
    {
        std::lock_guard lock(g_objMutex);
        prev = ev->signaled;
        ev->signaled = false;
        W32(Gpr32(ctx, 3) + 4, 0);
    }
    Ret(ctx, prev);
}

KFUNC(NtCreateSemaphore)
{
    W32(Gpr32(ctx, 3), HandleCreate(std::make_shared<KSemaphore>(GprS32(ctx, 5), GprS32(ctx, 6))));
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(NtReleaseSemaphore)
{
    auto s = std::dynamic_pointer_cast<KSemaphore>(HandleGet(Gpr32(ctx, 3)));
    if (!s) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    {
        std::lock_guard lock(g_objMutex);
        if (Gpr32(ctx, 5)) W32(Gpr32(ctx, 5), s->count);
        s->count += GprS32(ctx, 4);
        if (s->count > s->max) s->count = s->max;
    }
    SignalAll();
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(NtCreateMutant)
{
    auto m = std::make_shared<KMutant>();
    if (Gpr32(ctx, 5)) m->Acquire(CurrentThreadId());
    W32(Gpr32(ctx, 3), HandleCreate(m));
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(NtReleaseMutant)
{
    auto m = std::dynamic_pointer_cast<KMutant>(HandleGet(Gpr32(ctx, 3)));
    if (!m) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    {
        std::lock_guard lock(g_objMutex);
        if (Gpr32(ctx, 4)) W32(Gpr32(ctx, 4), m->recursion);
        if (m->recursion && --m->recursion == 0)
            m->owner = 0;
    }
    SignalAll();
    Ret(ctx, STATUS_SUCCESS);
}

// Timers: viram eventos que uma thread do PC sinaliza no tempo certo
struct KTimer : KEvent
{
    std::atomic<uint32_t> generation{ 0 };
    KTimer(bool manual) : KEvent(manual, false) {}
};

KFUNC(NtCreateTimer)
{
    W32(Gpr32(ctx, 3), HandleCreate(std::make_shared<KTimer>(Gpr32(ctx, 5) == 0)));
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(NtSetTimerEx)
{
    auto t = std::dynamic_pointer_cast<KTimer>(HandleGet(Gpr32(ctx, 3)));
    if (!t) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    int64_t due = TimeoutFromGuest(Gpr32(ctx, 4));
    uint32_t period = Gpr32(ctx, 9);
    if (Gpr32(ctx, 10)) W32(Gpr32(ctx, 10), t->signaled);
    if (Gpr32(ctx, 5)) Log("[kernel] NtSetTimerEx com APC (ignorado)");

    uint32_t gen = ++t->generation;
    {
        std::lock_guard lock(g_objMutex);
        t->signaled = false;
    }
    std::thread([t, gen, due, period]()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(due < 0 ? 0 : due));
        do
        {
            if (t->generation != gen) return;
            {
                std::lock_guard lock(g_objMutex);
                t->signaled = true;
            }
            SignalAll();
            if (period) std::this_thread::sleep_for(std::chrono::milliseconds(period));
        } while (period);
    }).detach();
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(NtCancelTimer)
{
    auto t = std::dynamic_pointer_cast<KTimer>(HandleGet(Gpr32(ctx, 3)));
    if (t)
    {
        if (Gpr32(ctx, 4)) W32(Gpr32(ctx, 4), t->signaled);
        t->generation++;
    }
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(NtClose)
{
    HandleClose(Gpr32(ctx, 3));
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(NtDuplicateObject)
{
    auto obj = HandleGet(Gpr32(ctx, 3));
    if (!obj) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    if (Gpr32(ctx, 4)) W32(Gpr32(ctx, 4), HandleCreate(obj));
    if (Gpr32(ctx, 5) & 1) HandleClose(Gpr32(ctx, 3));   // DUPLICATE_CLOSE_SOURCE
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(ObReferenceObjectByHandle)
{
    auto obj = HandleGet(Gpr32(ctx, 3));
    if (!obj) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    if (!obj->guestObject)
        obj->guestObject = GuestAlloc(0x200);
    if (Gpr32(ctx, 5)) W32(Gpr32(ctx, 5), obj->guestObject);
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(ObDereferenceObject) { Ret(ctx, 0); }

// ---------------------------------------------------------------- secoes criticas
// RTL_CRITICAL_SECTION: header(16) LockCount(+0x10) RecursionCount(+0x14) OwningThread(+0x18)

KFUNC(RtlInitializeCriticalSection)
{
    uint32_t cs = Gpr32(ctx, 3);
    memset(G(cs), 0, 0x1C);
    W8(cs, 1);
    W32(cs + 0x10, 0xFFFFFFFF);
}

static bool TryEnterCs(uint32_t cs, uint32_t tid)
{
    volatile long* owner = (volatile long*)G(cs + 0x18);
    if (R32(cs + 0x18) == tid)
    {
        W32(cs + 0x14, R32(cs + 0x14) + 1);
        return true;
    }
    if (_InterlockedCompareExchange(owner, (long)__builtin_bswap32(tid), 0) == 0)
    {
        W32(cs + 0x14, 1);
        return true;
    }
    return false;
}

// Ocupada: gira um pouco e depois dorme ate o dono soltar (WaitOnAddress; o Leave acorda quem espera). Antes ficava em
// "yield" sem parar: no carregamento as threads disputando a mesma secao gastavam processador e atrasavam quem a tinha.
// Estatistica por secao critica (so com FCRECOMP_KTEMPO, na mesma janela): entradas, quantas acharam ocupada e o chamador
namespace
{
    struct CsStat { uint64_t calls = 0, busy = 0; uint32_t lr = 0; };
    std::mutex g_csStatMutex;
    std::unordered_map<uint32_t, CsStat> g_csStats;
}
void ReportCsStats()   // chamada pelo relatorio do FCRECOMP_KTEMPO (main.cpp)
{
    std::lock_guard lock(g_csStatMutex);
    std::vector<std::pair<uint32_t, CsStat>> rows(g_csStats.begin(), g_csStats.end());
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second.calls > b.second.calls; });
    for (size_t i = 0; i < rows.size() && i < 8; i++)
        Log("[ktempo]   secao %08X: %llu entradas, %llu ocupada (chamada de %08X)", rows[i].first,
            (unsigned long long)rows[i].second.calls, (unsigned long long)rows[i].second.busy, rows[i].second.lr);
}

KFUNC(RtlEnterCriticalSection)
{
    const uint32_t cs = Gpr32(ctx, 3), tid = CurrentThreadId();
    volatile long* owner = (volatile long*)G(cs + 0x18);
    bool busy = false;
    for (uint32_t spin = 0; !TryEnterCs(cs, tid); spin++)
    {
        busy = true;
        if (spin < 128) { _mm_pause(); continue; }
        long seen = *owner;
        if (seen != 0) WaitOnAddress(owner, &seen, sizeof(seen), 2);   // 2 ms: seguranca se o jogo soltar sem o Leave
    }
    if (g_kfuncTiming)
    {
        std::lock_guard lock(g_csStatMutex);
        auto& s = g_csStats[cs];
        s.calls++;
        s.busy += busy;
        s.lr = (uint32_t)Lr(ctx);
    }
}

KFUNC(RtlTryEnterCriticalSection)
{
    Ret(ctx, TryEnterCs(Gpr32(ctx, 3), CurrentThreadId()) ? 1 : 0);
}

KFUNC(RtlLeaveCriticalSection)
{
    uint32_t cs = Gpr32(ctx, 3);
    uint32_t rec = R32(cs + 0x14);
    if (rec <= 1)
    {
        W32(cs + 0x14, 0);
        _InterlockedExchange((volatile long*)G(cs + 0x18), 0);
        WakeByAddressSingle((void*)G(cs + 0x18));
    }
    else
    {
        W32(cs + 0x14, rec - 1);
    }
}

// ---------------------------------------------------------------- spinlocks / IRQL

KFUNC(KfAcquireSpinLock)
{
    volatile long* lock = (volatile long*)G(Gpr32(ctx, 3));
    long tid = (long)__builtin_bswap32(CurrentThreadId());
    while (_InterlockedCompareExchange(lock, tid, 0) != 0)
        std::this_thread::yield();
    Ret(ctx, 0);
}

KFUNC(KfReleaseSpinLock) { _InterlockedExchange((volatile long*)G(Gpr32(ctx, 3)), 0); }
KFUNC(KeAcquireSpinLockAtRaisedIrql) { GUEST_EXPORT_SYMBOL(KfAcquireSpinLock)(ctx, base); }
KFUNC(KeReleaseSpinLockFromRaisedIrql) { GUEST_EXPORT_SYMBOL(KfReleaseSpinLock)(ctx, base); }
KFUNC(KeRaiseIrqlToDpcLevel) { Ret(ctx, 0); }
KFUNC(KfLowerIrql) {}
KFUNC(KeEnterCriticalRegion) {}
KFUNC(KeLeaveCriticalRegion) {}
KFUNC(KeLockL2) { Ret(ctx, 0); }
KFUNC(KeUnlockL2) {}
KFUNC(KeEnableFpuExceptions) {}

// ---------------------------------------------------------------- TLS

namespace
{
    std::atomic<uint64_t> g_tlsUsed{ 0 };
    thread_local uint32_t t_tls[64];
}

KFUNC(KeTlsAlloc)
{
    for (uint32_t i = 0; i < 64; i++)
    {
        uint64_t bit = 1ull << i;
        if (!(g_tlsUsed.fetch_or(bit) & bit))
        {
            Ret(ctx, i);
            return;
        }
    }
    Ret(ctx, 0xFFFFFFFF);
}

KFUNC(KeTlsFree)
{
    if (Gpr32(ctx, 3) < 64) g_tlsUsed &= ~(1ull << Gpr32(ctx, 3));
    Ret(ctx, 1);
}

KFUNC(KeTlsGetValue) { Ret(ctx, Gpr32(ctx, 3) < 64 ? t_tls[Gpr32(ctx, 3)] : 0); }

KFUNC(KeTlsSetValue)
{
    if (Gpr32(ctx, 3) < 64) t_tls[Gpr32(ctx, 3)] = Gpr32(ctx, 4);
    Ret(ctx, Gpr32(ctx, 3) < 64);
}

// ---------------------------------------------------------------- tempo

uint64_t g_tscFrequency = 0;

KFUNC(KeQueryPerformanceFrequency) { Ret(ctx, g_tscFrequency); }

KFUNC(KeQuerySystemTime)
{
    if (Gpr32(ctx, 3)) W64(Gpr32(ctx, 3), HoraDoJogo());
}

KFUNC(KeDelayExecutionThread)
{
    int64_t ms = TimeoutFromGuest(Gpr32(ctx, 5));
    if (ms <= 0) std::this_thread::yield();
    else std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(NtYieldExecution)
{
    std::this_thread::yield();
    Ret(ctx, STATUS_SUCCESS);
}

// TIME_FIELDS: Year Month Day Hour Minute Second Milliseconds Weekday (u16 cada)
KFUNC(RtlTimeToTimeFields)
{
    uint64_t t = R64(Gpr32(ctx, 3));
    FILETIME ft{ (DWORD)t, (DWORD)(t >> 32) };
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    uint32_t o = Gpr32(ctx, 4);
    W16(o + 0, st.wYear); W16(o + 2, st.wMonth); W16(o + 4, st.wDay); W16(o + 6, st.wHour);
    W16(o + 8, st.wMinute); W16(o + 10, st.wSecond); W16(o + 12, st.wMilliseconds); W16(o + 14, st.wDayOfWeek);
}

KFUNC(RtlTimeFieldsToTime)
{
    uint32_t i = Gpr32(ctx, 3);
    SYSTEMTIME st{ R16(i), R16(i + 2), R16(i + 14), R16(i + 4), R16(i + 6), R16(i + 8), R16(i + 10), R16(i + 12) };
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) { Ret(ctx, 0); return; }
    W64(Gpr32(ctx, 4), ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
    Ret(ctx, 1);
}

// ---------------------------------------------------------------- memoria

KFUNC(NtAllocateVirtualMemory)
{
    uint32_t basePtr = Gpr32(ctx, 3), sizePtr = Gpr32(ctx, 4), type = Gpr32(ctx, 5);
    // MEM_LARGE_PAGES no Xbox = paginas de 64 KB
    uint32_t page = (type & 0x20000000) ? 0x10000 : 0x1000;
    uint32_t want = R32(basePtr);
    uint32_t size = (R32(sizePtr) + page - 1) & ~(page - 1);

    if (want)
    {
        // endereco pedido: precisa estar dentro de uma regiao ja reservada
        uint32_t start, allocSize;
        want &= ~(page - 1);
        bool inside = GuestQuery(want, start, allocSize) && want + size <= start + allocSize;
        if (!inside && (type & 0x2000))   // MEM_RESERVE num endereco especifico
            inside = GuestAllocAt(want, size, MemKind::Virtual);
        if (g_trace) Log("[mem] NtAllocateVirtualMemory em %08X +%X tipo %X -> %s", want, size, type, inside ? "ok" : "falhou");
        if (!inside)
            Log("[mem] NtAllocateVirtualMemory em %08X (+%X, tipo %X) falhou", want, size, type);
        else
        {
            W32(basePtr, want);
            W32(sizePtr, size);
        }
        Ret(ctx, inside ? STATUS_SUCCESS : STATUS_NO_MEMORY);
        return;
    }

    uint32_t addr = GuestAlloc(size, (type & 0x20000000) ? 0x100000 : 0x10000, MemKind::Virtual);
    if (g_trace) Log("[mem] NtAllocateVirtualMemory %X bytes tipo %X -> %08X", size, type, addr);
    if (!addr) { Ret(ctx, STATUS_NO_MEMORY); return; }
    W32(basePtr, addr);
    W32(sizePtr, size);
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(NtFreeVirtualMemory)
{
    uint32_t addr = R32(Gpr32(ctx, 3));
    uint32_t size = Gpr32(ctx, 4) ? R32(Gpr32(ctx, 4)) : 0;
    if (Gpr32(ctx, 5) & 0x8000)   // MEM_RELEASE
        GuestFree(addr);
    else if ((Gpr32(ctx, 5) & 0x4000) && size)   // MEM_DECOMMIT: paginas voltam zeradas
    {
        uint32_t start = addr & ~0xFFFu;
        uint32_t end = (addr + size + 0xFFF) & ~0xFFFu;
        memset(G(start), 0, end - start);
    }
    if (g_trace) Log("[mem] NtFreeVirtualMemory %08X +%X tipo %X", addr, size, Gpr32(ctx, 5));
    Ret(ctx, STATUS_SUCCESS);
}

// MEMORY_BASIC_INFORMATION { BaseAddress, AllocationBase, AllocationProtect, RegionSize, State, Protect, Type }
KFUNC(NtQueryVirtualMemory)
{
    uint32_t addr = Gpr32(ctx, 3), info = Gpr32(ctx, 4);
    uint32_t page = addr & ~0xFFFu;
    uint32_t start, size;
    if (addr >= GUEST_IMAGE_BASE && addr < GUEST_IMAGE_BASE + GUEST_IMAGE_SIZE)
    {
        W32(info + 0x00, page);
        W32(info + 0x04, GUEST_IMAGE_BASE);
        W32(info + 0x08, 0x40);
        W32(info + 0x0C, GUEST_IMAGE_BASE + GUEST_IMAGE_SIZE - page);
        W32(info + 0x10, 0x1000);
        W32(info + 0x14, 0x40);
        W32(info + 0x18, 0x1000000);   // MEM_IMAGE
    }
    else if (GuestQuery(addr, start, size))
    {
        uint32_t end = (start + size + 0xFFF) & ~0xFFFu;
        W32(info + 0x00, page);
        W32(info + 0x04, start & ~0xFFFu);
        W32(info + 0x08, 4);
        W32(info + 0x0C, end - page);
        W32(info + 0x10, 0x1000);    // MEM_COMMIT
        W32(info + 0x14, 4);         // PAGE_READWRITE
        W32(info + 0x18, 0x20000);   // MEM_PRIVATE
    }
    else
    {
        uint32_t next = GuestNextAllocation(page);
        W32(info + 0x00, page);
        W32(info + 0x04, 0);
        W32(info + 0x08, 0);
        W32(info + 0x0C, (next ? next & ~0xFFFu : 0) - page);
        W32(info + 0x10, 0x10000);   // MEM_FREE
        W32(info + 0x14, 1);         // PAGE_NOACCESS
        W32(info + 0x18, 0);
    }
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(MmAllocatePhysicalMemoryEx)
{
    uint32_t size = Gpr32(ctx, 4), align = Gpr32(ctx, 8);
    if (align < 0x1000) align = 0x1000;
    Ret(ctx, GuestAlloc(size, align, MemKind::Physical));
}

KFUNC(MmFreePhysicalMemory) { GuestFree(Gpr32(ctx, 4)); }
KFUNC(MmGetPhysicalAddress) { Ret(ctx, Gpr32(ctx, 3) & 0x1FFFFFFF); }
KFUNC(MmQueryAddressProtect) { Ret(ctx, 4); }
KFUNC(MmQueryAllocationSize) { Ret(ctx, GuestAllocSize(Gpr32(ctx, 3))); }

KFUNC(MmQueryStatistics)
{
    uint32_t s = Gpr32(ctx, 3);
    uint32_t len = R32(s);
    memset(G(s + 4), 0, len > 4 ? len - 4 : 0);
    W32(s + 4, 0x20000);     // paginas totais (512 MB)
    W32(s + 8, 0x10000);     // paginas livres
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(XamAlloc)
{
    uint32_t addr = GuestAlloc(Gpr32(ctx, 4));
    W32(Gpr32(ctx, 5), addr);
    Ret(ctx, addr ? 0 : 0x8007000E);
}

KFUNC(XamFree)
{
    GuestFree(Gpr32(ctx, 3));
    Ret(ctx, 0);
}

// ---------------------------------------------------------------- memoria utilitarios

KFUNC(RtlCompareMemoryUlong)
{
    uint32_t src = Gpr32(ctx, 3), len = Gpr32(ctx, 4), pat = Gpr32(ctx, 5);
    uint32_t n = 0;
    while (n + 4 <= len && R32(src + n) == pat) n += 4;
    Ret(ctx, n);
}

KFUNC(RtlFillMemoryUlong)
{
    for (uint32_t i = 0; i + 4 <= Gpr32(ctx, 4); i += 4)
        W32(Gpr32(ctx, 3) + i, Gpr32(ctx, 5));
}

// ---------------------------------------------------------------- strings

KFUNC(RtlInitAnsiString)
{
    uint32_t dst = Gpr32(ctx, 3), src = Gpr32(ctx, 4);
    uint16_t len = src ? (uint16_t)strlen((char*)G(src)) : 0;
    W16(dst, len);
    W16(dst + 2, src ? len + 1 : 0);
    W32(dst + 4, src);
}

KFUNC(RtlInitUnicodeString)
{
    uint32_t dst = Gpr32(ctx, 3), src = Gpr32(ctx, 4);
    uint16_t len = 0;
    if (src) while (R16(src + len * 2)) len++;
    W16(dst, len * 2);
    W16(dst + 2, src ? len * 2 + 2 : 0);
    W32(dst + 4, src);
}

KFUNC(RtlFreeAnsiString)
{
    uint32_t buf = R32(Gpr32(ctx, 3) + 4);
    if (buf) GuestFree(buf);
    W32(Gpr32(ctx, 3) + 4, 0);
}

KFUNC(RtlUnicodeStringToAnsiString)
{
    uint32_t dst = Gpr32(ctx, 3), src = Gpr32(ctx, 4);
    uint16_t len = R16(src) / 2;
    uint32_t srcBuf = R32(src + 4);
    uint32_t buf = R32(dst + 4);
    if (Gpr32(ctx, 5))
    {
        buf = GuestAlloc(len + 1);
        W32(dst + 4, buf);
        W16(dst + 2, len + 1);
    }
    for (uint16_t i = 0; i < len; i++)
        W8(buf + i, (uint8_t)R16(srcBuf + i * 2));
    W8(buf + len, 0);
    W16(dst, len);
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(RtlMultiByteToUnicodeN)
{
    uint32_t dst = Gpr32(ctx, 3), maxBytes = Gpr32(ctx, 4), outPtr = Gpr32(ctx, 5), src = Gpr32(ctx, 6), srcBytes = Gpr32(ctx, 7);
    uint32_t n = std::min(maxBytes / 2, srcBytes);
    for (uint32_t i = 0; i < n; i++)
        W16(dst + i * 2, R8(src + i));
    if (outPtr) W32(outPtr, n * 2);
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(RtlUnicodeToMultiByteN)
{
    uint32_t dst = Gpr32(ctx, 3), maxBytes = Gpr32(ctx, 4), outPtr = Gpr32(ctx, 5), src = Gpr32(ctx, 6), srcBytes = Gpr32(ctx, 7);
    uint32_t n = std::min(maxBytes, srcBytes / 2);
    for (uint32_t i = 0; i < n; i++)
        W8(dst + i, (uint8_t)R16(src + i * 2));
    if (outPtr) W32(outPtr, n);
    Ret(ctx, STATUS_SUCCESS);
}

KFUNC(RtlNtStatusToDosError)
{
    uint32_t s = Gpr32(ctx, 3);
    if (s == 0) Ret(ctx, 0);
    else if (s == STATUS_OBJECT_NAME_NOT_FOUND) Ret(ctx, 2);
    else if (s == STATUS_OBJECT_PATH_NOT_FOUND) Ret(ctx, 3);
    else if (s == STATUS_END_OF_FILE) Ret(ctx, 38);
    else if (s == STATUS_NO_MORE_FILES) Ret(ctx, 18);
    else if ((s & 0xFFFF0000) == 0x80070000) Ret(ctx, s & 0xFFFF);
    else Ret(ctx, 317);
}

// ---------------------------------------------------------------- XEX

extern uint32_t g_xexHeader;

KFUNC(RtlImageXexHeaderField)
{
    uint32_t hdr = Gpr32(ctx, 3) ? Gpr32(ctx, 3) : g_xexHeader, key = Gpr32(ctx, 4);
    uint32_t count = R32(hdr + 0x14);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t k = R32(hdr + 0x18 + i * 8);
        uint32_t v = R32(hdr + 0x18 + i * 8 + 4);
        if (k == key)
        {
            Ret(ctx, (key & 0xFF) == 0 ? v : hdr + v);
            return;
        }
    }
    Ret(ctx, 0);
}

KFUNC(XexCheckExecutablePrivilege) { Ret(ctx, 0); }
KFUNC(KeGetCurrentProcessType) { Ret(ctx, 1); }

// ---------------------------------------------------------------- debug / fim

KFUNC(DbgPrint)
{
    Log("[DbgPrint] %s", RStr(Gpr32(ctx, 3)).c_str());
    Ret(ctx, 0);
}

KFUNC(DbgBreakPoint) { Log("[kernel] DbgBreakPoint"); }

KFUNC(KeBugCheck)
{
    Log("[kernel] KeBugCheck %08X", Gpr32(ctx, 3));
    SairDoProcesso(1);
}

KFUNC(KeBugCheckEx)
{
    Log("[kernel] KeBugCheckEx %08X %08X %08X %08X %08X", Gpr32(ctx, 3), Gpr32(ctx, 4), Gpr32(ctx, 5), Gpr32(ctx, 6), Gpr32(ctx, 7));
    SairDoProcesso(1);
}

KFUNC(HalReturnToFirmware)
{
    Log("[kernel] HalReturnToFirmware %u", Gpr32(ctx, 3));
    RedeEncerrar();
    SairDoProcesso(0);
}

KFUNC(XamLoaderTerminateTitle)
{
    Log("[xam] XamLoaderTerminateTitle");
    RedeEncerrar();
    SairDoProcesso(0);
}

KFUNC(ExRegisterTitleTerminateNotification) {}
KFUNC(KiApcNormalRoutineNop) {}
