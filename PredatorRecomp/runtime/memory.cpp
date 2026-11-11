// Memoria do "Xbox": 4 GB reservados, alocador simples para o jogo
#include "guest.h"
#include <windows.h>
#include <map>
#include <mutex>

uint8_t* g_base = nullptr;
uint8_t* g_gpuRegsHost = nullptr;     // registradores da GPU, vistos pelo runtime (sem protecao)
static HANDLE g_gpuRegsMapping = nullptr;
HANDLE g_physicalMapping = nullptr;   // memoria fisica (512 MB), tambem aberta pela placa de video

namespace
{
    struct Heap
    {
        uint32_t start, end;
        std::map<uint32_t, uint32_t> freeList;  // inicio -> tamanho
        std::map<uint32_t, uint32_t> used;      // inicio -> tamanho
        std::map<uint32_t, uint32_t> serial;    // inicio -> numero da reserva (muda quando o pedaco e reaproveitado)
    };

    uint32_t g_nextSerial = 1;

    std::mutex g_heapMutex;
    Heap g_system{ 0x10000000, 0x3FFF0000 };     // uso interno do runtime (pilhas, objetos, XamAlloc)
    Heap g_virtual{ 0x40000000, 0x7F000000 };    // NtAllocateVirtualMemory
    Heap g_physical{ 0xA0000000, 0xBFFF0000 };   // MmAllocatePhysicalMemoryEx

    Heap& HeapFor(MemKind kind)
    {
        return kind == MemKind::Virtual ? g_virtual : kind == MemKind::Physical ? g_physical : g_system;
    }

    void Commit(uint32_t addr, uint32_t size)
    {
        if (addr >= 0xA0000000) return;   // memoria fisica ja esta mapeada
        VirtualAlloc(g_base + addr, size, MEM_COMMIT, PAGE_READWRITE);
    }

    uint32_t HeapAlloc(Heap& h, uint32_t size, uint32_t align)
    {
        for (auto it = h.freeList.begin(); it != h.freeList.end(); ++it)
        {
            uint32_t blockStart = it->first, blockSize = it->second;
            uint32_t aligned = (blockStart + align - 1) & ~(align - 1);
            if (aligned + size > blockStart + blockSize)
                continue;

            h.freeList.erase(it);
            if (aligned > blockStart)
                h.freeList[blockStart] = aligned - blockStart;
            uint32_t tail = blockStart + blockSize - (aligned + size);
            if (tail)
                h.freeList[aligned + size] = tail;

            h.used[aligned] = size;
            h.serial[aligned] = g_nextSerial++;
            return aligned;
        }
        return 0;
    }

    bool HeapFree(Heap& h, uint32_t addr)
    {
        auto it = h.used.find(addr);
        if (it == h.used.end())
            return false;
        uint32_t size = it->second;
        h.used.erase(it);
        h.serial.erase(addr);

        auto [ins, ok] = h.freeList.emplace(addr, size);
        // junta com o bloco seguinte
        auto next = std::next(ins);
        if (next != h.freeList.end() && ins->first + ins->second == next->first)
        {
            ins->second += next->second;
            h.freeList.erase(next);
        }
        // junta com o bloco anterior
        if (ins != h.freeList.begin())
        {
            auto prev = std::prev(ins);
            if (prev->first + prev->second == ins->first)
            {
                prev->second += ins->second;
                h.freeList.erase(ins);
            }
        }
        return true;
    }
}

bool MemoryInit()
{
    // registradores da GPU: um mapeamento visto pelo jogo (protegido contra escrita) e outro so do runtime
    g_gpuRegsMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, GPU_MMIO_SIZE, nullptr);
    g_gpuRegsHost = (uint8_t*)MapViewOfFile(g_gpuRegsMapping, FILE_MAP_ALL_ACCESS, 0, 0, GPU_MMIO_SIZE);
    if (!g_gpuRegsHost)
        return false;

    // memoria fisica (512 MB) aparece em 3 espelhos: 0xA0000000, 0xC0000000 e 0xE0000000
    HANDLE physical = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 0x20000000, nullptr);
    g_physicalMapping = physical;
    if (!physical)
        return false;

    for (int attempt = 0; attempt < 16 && !g_base; attempt++)
    {
        // acha um bloco livre de 4 GB, libera e remonta em partes no mesmo lugar
        uint8_t* probe = (uint8_t*)VirtualAlloc(nullptr, GUEST_MEMORY_SIZE, MEM_RESERVE, PAGE_NOACCESS);
        if (!probe) return false;
        VirtualFree(probe, 0, MEM_RELEASE);

        // 0 .. GPU_MMIO | registradores da GPU (mapeamento proprio) | resto ate 0xA0000000
        bool ok = VirtualAlloc(probe, GPU_MMIO_BASE, MEM_RESERVE, PAGE_NOACCESS) != nullptr;
        ok = ok && MapViewOfFileEx(g_gpuRegsMapping, FILE_MAP_ALL_ACCESS, 0, 0, GPU_MMIO_SIZE, probe + GPU_MMIO_BASE) == probe + GPU_MMIO_BASE;
        ok = ok && VirtualAlloc(probe + GPU_MMIO_BASE + GPU_MMIO_SIZE, 0xA0000000 - (GPU_MMIO_BASE + GPU_MMIO_SIZE), MEM_RESERVE, PAGE_NOACCESS) != nullptr;
        for (uint64_t mirror : { 0xA0000000ull, 0xC0000000ull, 0xE0000000ull })
            ok = ok && MapViewOfFileEx(physical, FILE_MAP_ALL_ACCESS, 0, 0, 0x20000000, probe + mirror) == probe + mirror;
        if (ok)
            g_base = probe;
        else
            Log("[mem] tentativa %d de montar a memoria falhou, tentando de novo", attempt);
    }
    if (!g_base)
        return false;

    // imagem do jogo + tabela de funcoes
    Commit(GUEST_IMAGE_BASE, GUEST_IMAGE_SIZE + GUEST_CODE_SIZE * 2 + 0x10000);

    // outros registradores de hardware (audio/XMA 0x7FEA0000...): por enquanto memoria comum
    Commit(0x7F000000, GPU_MMIO_BASE - 0x7F000000);
    Commit(GPU_MMIO_BASE + GPU_MMIO_SIZE, 0x80000000 - (GPU_MMIO_BASE + GPU_MMIO_SIZE));

    for (Heap* h : { &g_system, &g_virtual, &g_physical })
        h->freeList[h->start] = h->end - h->start;
    return true;
}

uint32_t GuestAlloc(uint32_t size, uint32_t align, MemKind kind)
{
    if (size == 0) size = 1;
    if (align < 16) align = 16;
    // memoria do jogo e sempre em paginas inteiras
    uint32_t granularity = kind == MemKind::System ? 16 : 0x1000;
    size = (size + granularity - 1) & ~(granularity - 1);

    std::lock_guard lock(g_heapMutex);
    uint32_t addr = HeapAlloc(HeapFor(kind), size, align);
    if (!addr)
    {
        Log("[mem] SEM MEMORIA: %u bytes (tipo %d)", size, (int)kind);
        return 0;
    }
    Commit(addr, size);
    memset(G(addr), 0, size);
    return addr;
}

// Aloca exatamente em addr, se essa faixa estiver livre
bool GuestAllocAt(uint32_t addr, uint32_t size, MemKind kind)
{
    size = (size + 0xFFF) & ~0xFFFu;
    std::lock_guard lock(g_heapMutex);
    Heap& h = HeapFor(kind);
    auto it = h.freeList.upper_bound(addr);
    if (it == h.freeList.begin()) return false;
    --it;
    uint32_t blockStart = it->first, blockSize = it->second;
    if (addr < blockStart || (uint64_t)addr + size > (uint64_t)blockStart + blockSize)
        return false;

    h.freeList.erase(it);
    if (addr > blockStart)
        h.freeList[blockStart] = addr - blockStart;
    uint32_t tail = blockStart + blockSize - (addr + size);
    if (tail)
        h.freeList[addr + size] = tail;
    h.used[addr] = size;
    h.serial[addr] = g_nextSerial++;
    Commit(addr, size);
    memset(G(addr), 0, size);
    return true;
}

void GuestFree(uint32_t addr)
{
    std::lock_guard lock(g_heapMutex);
    for (Heap* h : { &g_system, &g_virtual, &g_physical })
        if (HeapFree(*h, addr))
            return;
}

bool GuestQuery(uint32_t addr, uint32_t& start, uint32_t& size)
{
    std::lock_guard lock(g_heapMutex);
    for (Heap* h : { &g_system, &g_virtual, &g_physical })
    {
        auto it = h->used.upper_bound(addr);
        if (it == h->used.begin()) continue;
        --it;
        if (addr >= it->first && addr < it->first + it->second)
        {
            start = it->first;
            size = it->second;
            return true;
        }
    }
    return false;
}

// Inicio da proxima regiao ocupada a partir de addr (0 = nenhuma)
uint32_t GuestNextAllocation(uint32_t addr)
{
    std::lock_guard lock(g_heapMutex);
    uint64_t best = 0x100000000ull;
    if (addr < GUEST_IMAGE_BASE) best = GUEST_IMAGE_BASE;
    for (Heap* h : { &g_system, &g_virtual, &g_physical })
    {
        auto it = h->used.lower_bound(addr);
        if (it != h->used.end() && it->first < best)
            best = it->first;
    }
    return best == 0x100000000ull ? 0 : (uint32_t)best;
}

uint32_t GuestAllocSize(uint32_t addr)
{
    uint32_t start, size;
    return GuestQuery(addr, start, size) ? size - (addr - start) : 0;
}

// Numero da reserva que contem addr (0 = memoria livre). Muda quando o pedaco e liberado e reservado de novo: quem
// guarda um endereco para usar depois (ex.: retorno atrasado dos resolves, xenos_draw.cpp) confere se ainda e o mesmo dono.
uint32_t GuestAllocSerial(uint32_t addr)
{
    std::lock_guard lock(g_heapMutex);
    for (Heap* h : { &g_system, &g_virtual, &g_physical })
    {
        auto it = h->used.upper_bound(addr);
        if (it == h->used.begin()) continue;
        --it;
        if (addr >= it->first && addr < it->first + it->second)
            return h->serial[it->first];
    }
    return 0;
}
