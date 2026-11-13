// Ponto de observacao de memoria (parte da ferramenta de diagnostico).
// FCRECOMP_WATCH=endereco[:tamanho][,endereco[:tamanho]...][@quadro] (enderecos do Xbox em hexadecimal;
// @quadro = so comeca a observar a partir desse quadro)
// Cada escrita do jogo nessas faixas vai para o log com: funcao recompilada que escreveu, quem a chamou (lr),
// thread, valor antigo e novo. Funciona protegendo a pagina contra escrita: a escrita gera uma excecao, o
// tratador anota, libera a pagina, executa so aquela instrucao (passo a passo) e protege de novo.
#include "guest.h"
#include "thread.h"
#include "diag.h"
#include <windows.h>
#include <mutex>
#include <string>
#include <vector>
#include <set>

uint32_t GuestFunctionAt(uintptr_t rip);   // profiler.cpp

namespace
{
    struct Range { uint32_t addr, size; };
    std::vector<Range> g_ranges;
    std::vector<uint8_t*> g_pages;       // paginas protegidas (todas as copias espelhadas do endereco)
    std::mutex g_mutex;
    int g_logged = 0;
    const int MAX_LOG = getenv("FCRECOMP_WATCHMAX") ? atoi(getenv("FCRECOMP_WATCHMAX")) : 400;   // limite de anotacoes

    struct Pending { uint8_t* page = nullptr; uint32_t guest = 0; bool hit = false; };
    // FCRECOMP_WATCHENTRADA=1: vigia LEITURAS do analogico direito do objeto de entrada do jogo (sub_823140D8 guarda
    // RX/RY como float em +32/+36); cada funcao que le aparece uma vez no log. Serve para achar o codigo da camera.
    bool g_readMode = false;
    std::set<uint32_t> g_readers;
    thread_local Pending t_pending;

    // a memoria fisica aparece em 3 lugares (0xA0.., 0xC0.., 0xE0..): protege todas as copias
    std::vector<uint32_t> Mirrors(uint32_t a)
    {
        if (a < 0xA0000000) return { a };
        uint32_t phys = a & 0x1FFFFFFF;
        return { 0xA0000000 | phys, 0xC0000000 | phys, 0xE0000000 | phys };
    }

    bool Watched(uint32_t guest, uint32_t& hitAddr)
    {
        for (auto& r : g_ranges)
            for (uint32_t m : Mirrors(r.addr))
                if (guest + 8 > m && guest < m + r.size) { hitAddr = m; return true; }
        return false;
    }

    LONG WINAPI Handler(EXCEPTION_POINTERS* e)
    {
        auto* rec = e->ExceptionRecord;
        if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && t_pending.page)
        {
            // a escrita aconteceu: mostra o valor novo e protege a pagina de novo
            DWORD old;
            uint8_t* p = g_base + t_pending.guest;
            std::lock_guard lock(g_mutex);
            if (t_pending.hit && g_logged <= MAX_LOG)
                Log("[watch]   depois: %02X %02X %02X %02X %02X %02X %02X %02X", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
            VirtualProtect(t_pending.page, 0x1000, g_readMode ? PAGE_NOACCESS : PAGE_READONLY, &old);
            t_pending.page = nullptr;
            e->ContextRecord->EFlags &= ~0x100u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || (!g_readMode && rec->ExceptionInformation[0] != 1)) return EXCEPTION_CONTINUE_SEARCH;
        uint8_t* host = (uint8_t*)rec->ExceptionInformation[1];
        if (host < g_base || host >= g_base + 0x100000000ull) return EXCEPTION_CONTINUE_SEARCH;
        uint8_t* page = (uint8_t*)((uintptr_t)host & ~0xFFFull);
        bool ours = false;
        for (auto* pg : g_pages) if (pg == page) ours = true;
        if (!ours) return EXCEPTION_CONTINUE_SEARCH;

        uint32_t guest = (uint32_t)(host - g_base), hit = 0;
        bool inRange = Watched(guest, hit);
        if (inRange && g_readMode)
        {
            std::lock_guard lock(g_mutex);
            uintptr_t rip = (uintptr_t)e->ContextRecord->Rip;
            uint32_t fn = GuestFunctionAt(rip);
            // FCRECOMP_WATCHTODOS=1: anota todo acesso (ate FCRECOMP_WATCHMAX), nao so o 1o de cada funcao
            static const bool todos = getenv("FCRECOMP_WATCHTODOS") != nullptr;
            if (g_readers.insert(fn).second || (todos && g_logged++ < MAX_LOG))
            {
                auto t = CurrentGuestThread();
                Log("[watch] %s de %08X por sub_%08X (chamada de %08X) thread '%s'", rec->ExceptionInformation[0] == 1 ? "escrita" : "leitura",
                    guest, fn, t && t->ctx ? (uint32_t)t->ctx->lr : 0, t ? t->name : "?");
            }
        }
        else if (inRange)
        {
            std::lock_guard lock(g_mutex);
            if (g_logged++ < MAX_LOG)
            {
                auto t = CurrentGuestThread();
                uintptr_t rip = (uintptr_t)e->ContextRecord->Rip;
                Log("[watch] escrita em %08X por sub_%08X (chamada de %08X) thread '%s'  antes: %02X %02X %02X %02X %02X %02X %02X %02X",
                    guest, GuestFunctionAt(rip), t && t->ctx ? (uint32_t)t->ctx->lr : 0, t ? t->name : "?",
                    host[0], host[1], host[2], host[3], host[4], host[5], host[6], host[7]);
                if (t && t->ctx)
                {
                    auto& c = *t->ctx;
                    Log("[watch]   registradores: r3=%08X r4=%08X r5=%08X r6=%08X r29=%08X r30=%08X r31=%08X r1=%08X",
                        Gpr32(c, 3), Gpr32(c, 4), Gpr32(c, 5), Gpr32(c, 6), Gpr32(c, 29), Gpr32(c, 30), Gpr32(c, 31), Gpr32(c, 1));
                    // cadeia de retornos do PowerPC (back chain): quem chamou a funcao que escreveu
                    std::string chain;
                    uint32_t sp = Gpr32(c, 1);
                    for (int i = 0; i < 12 && sp >= 0x10000000 && sp < 0x80000000; i++)
                    {
                        uint32_t next = R32(sp);
                        if (next <= sp) break;
                        char b[16]; snprintf(b, sizeof(b), " %08X", R32(next - 8)); chain += b;
                        sp = next;
                    }
                    Log("[watch]   pilha:%s", chain.c_str());
                }
                char key[64];
                snprintf(key, sizeof(key), "escrita observada por sub_%08X", GuestFunctionAt(rip));
                diag::Note(diag::Other, key);
            }
        }
        // libera a pagina so para esta instrucao (passo a passo) e protege de novo no SINGLE_STEP
        DWORD old;
        VirtualProtect(page, 0x1000, PAGE_READWRITE, &old);
        t_pending.page = page;
        t_pending.guest = guest;
        t_pending.hit = inRange;
        e->ContextRecord->EFlags |= 0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
}

// Chamado a cada quadro pela thread da GPU: protege as paginas assim que a memoria existir
void WatchAddWriteRange(uint32_t addr, uint32_t size);
void WatchAddReadRange(uint32_t addr, uint32_t size);
void WatchUpdate()
{
    // FCRECOMP_WATCHARQ=1: a cada quadro procura diag\watch_agora.txt ("endereco:tamanho" por linha) e passa a vigiar
    // essas faixas (para vigiar algo achado com o jogo ja rodando, ex.: comparando copias da memoria)
    if (static const bool arq = getenv("FCRECOMP_WATCHARQ") != nullptr; arq)
        if (FILE* f = fopen("diag/watch_agora.txt", "r"))
        {
            // linha "R endereco:tamanho" = vigia leituras (e escritas) dessa faixa
            unsigned a, n; char kind[4];
            while (fscanf(f, " %3s", kind) == 1)
            {
                if (kind[0] == 'R') { if (fscanf(f, " %x:%u", &a, &n) == 2) WatchAddReadRange(a, n); }
                else if (sscanf(kind, "%x", &a) == 1) { fseek(f, -(long)strlen(kind), SEEK_CUR); if (fscanf(f, " %x:%u", &a, &n) == 2) WatchAddWriteRange(a, n); }
            }
            fclose(f);
            remove("diag/watch_agora.txt");
        }
    static const char* env = getenv("FCRECOMP_WATCH");
    if (!env) return;
    static uint64_t startFrame = strchr(env, '@') ? strtoull(strchr(env, '@') + 1, nullptr, 10) : 0;
    if (diag::CurrentFrame() < startFrame) return;
    static bool parsed = false;
    if (!parsed)
    {
        parsed = true;
        for (const char* p = env; *p;)
        {
            char* end;
            uint32_t a = (uint32_t)strtoul(p, &end, 16), size = 4;
            if (*end == ':') size = (uint32_t)strtoul(end + 1, &end, 0);
            g_ranges.push_back({ a, size });
            if (*end == '@') break;
            while (*end == ',' || *end == ' ') end++;
            p = end;
        }
        AddVectoredExceptionHandler(1, Handler);
        Log("[watch] observando %zu faixa(s) de memoria", g_ranges.size());
    }
    for (auto& r : g_ranges)
        for (uint32_t m : Mirrors(r.addr))
            for (uint32_t a = m & ~0xFFFu; a < m + r.size; a += 0x1000)
            {
                uint8_t* page = g_base + a;
                bool done = false;
                for (auto* pg : g_pages) if (pg == page) done = true;
                if (done) continue;
                MEMORY_BASIC_INFORMATION mi{};
                if (!VirtualQuery(page, &mi, sizeof(mi)) || mi.State != MEM_COMMIT) continue;
                DWORD old;
                if (VirtualProtect(page, 0x1000, PAGE_READONLY, &old))
                {
                    g_pages.push_back(page);
                    Log("[watch] pagina %08X protegida", a);
                }
            }
}

// ---------------------------------------------------------------- rastreador de texto
// FCRECOMP_STRWATCH=texto: toda copia feita pelo memcpy_s do jogo (sub_82628600) que contenha o texto
// (em ASCII ou Unicode big-endian) vai para o log com quem chamou e a cadeia de enderecos de retorno.
// Serve para achar a funcao que monta um texto errado (ex.: "OKx" no menu).
namespace
{
    std::string ChainFrom(uint32_t sp)
    {
        std::string chain;
        for (int i = 0; i < 10 && sp >= 0x10000000 && sp < 0x80000000; i++)
        {
            uint32_t next = R32(sp);
            if (next <= sp) break;
            char buf[16];
            snprintf(buf, sizeof(buf), " %08X", R32(next - 8));
            chain += buf;
            sp = next;
        }
        return chain;
    }

    void StrWatchCheck(GuestCpu& ctx, uint32_t src, uint32_t count, const char* who)
    {
        static const char* text = getenv("FCRECOMP_STRWATCH");
        if (!text || !src || count == 0 || count > 0x10000) return;
        size_t n = strlen(text);
        const uint8_t* p = g_base + src;
        bool found = false;
        for (uint32_t i = 0; !found && i + n <= count; i++)
            if (memcmp(p + i, text, n) == 0) found = true;   // ASCII
        for (uint32_t i = 0; !found && i + n * 2 <= count; i++)
        {
            bool ok = true;
            for (size_t k = 0; k < n && ok; k++) ok = p[i + k * 2] == 0 && p[i + k * 2 + 1] == (uint8_t)text[k];
            found = ok;
        }
        if (!found) return;
        static int logged = 0;
        if (logged++ > 200) return;
        std::string bytes;
        for (uint32_t i = 0; i < count && i < 48; i++) { char b[4]; snprintf(b, sizeof(b), "%02X", p[i]); bytes += b; }
        Log("[strwatch] %s copiou %u bytes de %08X para %08X, chamado de %08X; pilha:%s; bytes: %s",
            who, count, src, Gpr32(ctx, 3), (uint32_t)Lr(ctx), ChainFrom(Gpr32(ctx, 1)).c_str(), bytes.c_str());
        diag::Note(diag::Other, std::string("texto \"") + text + "\" montado (veja [strwatch] no log)");
    }
}

#ifndef PREDATOR_EDITOR   // gancho so do jogo: no editor de mapas (ige.xex) o endereco e de outra funcao
GUEST_DECLARE_ORIGINAL(sub_82628600);
// memcpy_s(destino r3, tamanho r4, origem r5, quantidade r6)
GUEST_FUNC(sub_82628600)
{
    StrWatchCheck(ctx, Gpr32(ctx, 5), Gpr32(ctx, 6), "memcpy_s");
    GUEST_ORIGINAL(sub_82628600)(ctx, base);
}
#endif

// Adiciona uma faixa vigiada em tempo de execucao (modo leitura)
void WatchAddReadRange(uint32_t addr, uint32_t size)
{
    std::lock_guard lock(g_mutex);
    g_readMode = true;
    g_ranges.push_back({ addr, size });
    static bool handler = false;
    if (!handler) { handler = true; AddVectoredExceptionHandler(1, Handler); }
    for (uint32_t m : Mirrors(addr))   // memoria fisica: o jogo pode ler por qualquer um dos 3 espelhos
    {
        uint8_t* page = g_base + (m & ~0xFFFu);
        DWORD old;
        if (VirtualProtect(page, 0x1000, PAGE_NOACCESS, &old)) { g_pages.push_back(page); Log("[watch] lendo %08X+%u (pagina %08X sem acesso)", m, size, m & ~0xFFFu); }
    }
}

#ifndef PREDATOR_EDITOR   // gancho so do jogo: no editor de mapas (ige.xex) o endereco e de outra funcao
GUEST_DECLARE_ORIGINAL(sub_823140D8);
// sub_823140D8(objeto de entrada): le o controle e guarda botoes/analogicos como float no objeto
GUEST_FUNC(sub_823140D8)
{
    static const bool want = getenv("FCRECOMP_WATCHENTRADA") != nullptr;
    static bool done = false;
    uint32_t obj = Gpr32(ctx, 3);
    GUEST_ORIGINAL(sub_823140D8)(ctx, base);
    if (want && !done && diag::CurrentFrame() > 1200)
    {
        done = true;
        WatchAddReadRange(obj + 32, 8);
    }
}
#endif

// Adiciona uma faixa vigiada (escritas) em tempo de execucao
void WatchAddWriteRange(uint32_t addr, uint32_t size)
{
    std::lock_guard lock(g_mutex);
    g_ranges.push_back({ addr, size });
    static bool handler = false;
    if (!handler) { handler = true; AddVectoredExceptionHandler(1, Handler); }
    for (uint32_t a = addr & ~0xFFFu; a < addr + size; a += 0x1000)
    {
        uint8_t* page = g_base + a;
        bool done = false;
        for (auto* pg : g_pages) if (pg == page) done = true;
        DWORD old;
        if (!done && VirtualProtect(page, 0x1000, PAGE_READONLY, &old)) { g_pages.push_back(page); Log("[watch] vigiando escritas em %08X+%u", addr, size); }
    }
}
