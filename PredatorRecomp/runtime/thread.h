// Threads do jogo rodando como threads do PC
#pragma once
#include "objects.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <string>

struct GuestThread : KObject
{
    uint32_t id = 0;
    uint32_t entry = 0, param = 0, xapiStartup = 0;
    uint32_t stackSize = 0;
    uint32_t pcr = 0, stack = 0;
    std::atomic<int32_t> suspendCount{ 0 };
    bool finished = false;
    uint32_t exitCode = 0;
    std::thread host;
    GuestCpu* ctx = nullptr;
    const char* name = "jogo";
    // diagnostico (F12 -> threads.txt): em que a thread esta esperando agora
    std::atomic<uint64_t> waitSince{ 0 };   // GetTickCount64 do inicio da espera (0 = nao esta esperando)
    std::atomic<uint32_t> waitLr{ 0 };      // de onde o jogo chamou a espera
    std::mutex waitMutex;
    std::string waitWhat;

    bool IsSignaled(uint32_t) override { return finished; }
};

std::shared_ptr<GuestThread> CurrentGuestThread();

// Prepara PCR/pilha e roda a funcao do jogo nesta thread do PC
void RunGuestThread(std::shared_ptr<GuestThread> t);
GuestCpu* AttachGuestThread(const char* name);
std::string GuestBacktrace(GuestCpu& c);
void DumpAllThreads();
std::string DescribeThreads();   // texto com cada thread do jogo: espera atual (ha quanto tempo, em que) e pilha
std::shared_ptr<GuestThread> CreateGuestThread(uint32_t entry, uint32_t param, uint32_t xapiStartup, uint32_t stackSize, bool suspended);
