// Objetos do kernel (eventos, semaforos, mutex, threads, arquivos) e handles
#pragma once
#include "guest.h"
#include <memory>
#include <mutex>
#include <condition_variable>
#include <vector>

struct KObject
{
    virtual ~KObject() = default;
    virtual bool IsSignaled(uint32_t tid) { return true; }
    virtual void Acquire(uint32_t tid) {}
    uint32_t guestObject = 0;   // copia do objeto na memoria do jogo (para ObReferenceObjectByHandle)
};

struct KEvent : KObject
{
    bool manual, signaled;
    KEvent(bool manual, bool signaled) : manual(manual), signaled(signaled) {}
    bool IsSignaled(uint32_t) override { return signaled; }
    void Acquire(uint32_t) override { if (!manual) signaled = false; }
};

struct KSemaphore : KObject
{
    int32_t count, max;
    KSemaphore(int32_t count, int32_t max) : count(count), max(max) {}
    bool IsSignaled(uint32_t) override { return count > 0; }
    void Acquire(uint32_t) override { count--; }
};

struct KMutant : KObject
{
    uint32_t owner = 0, recursion = 0;
    bool IsSignaled(uint32_t tid) override { return owner == 0 || owner == tid; }
    void Acquire(uint32_t tid) override { owner = tid; recursion++; }
};

// trava global dos objetos (simples; da para refinar depois)
extern std::mutex g_objMutex;
extern std::condition_variable g_objCv;

uint32_t HandleCreate(std::shared_ptr<KObject> obj);
std::shared_ptr<KObject> HandleGet(uint32_t handle);
void HandleClose(uint32_t handle);
std::shared_ptr<KObject> ObjectFromGuest(uint32_t addr);   // KEVENT etc. na memoria do jogo

// espera; timeoutMs < 0 = infinito. Retorna STATUS_SUCCESS+indice ou STATUS_TIMEOUT
uint32_t WaitObjects(const std::vector<std::shared_ptr<KObject>>& objs, bool waitAll, int64_t timeoutMs);
int64_t TimeoutFromGuest(uint32_t timeoutPtr);   // LARGE_INTEGER* em unidades de 100ns

uint32_t CurrentThreadId();
void SignalAll();
