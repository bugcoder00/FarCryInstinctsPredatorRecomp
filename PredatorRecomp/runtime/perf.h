// Medicao simples de tempo por categoria (thread da GPU). FCRECOMP_PERF=1 escreve um resumo no log.
#pragma once
#include <windows.h>
#include <cstdint>
#include <intrin.h>

namespace perf
{
    enum Cat { Idle, WaitReg, Draw, Resolve, Present, Fence, Texture, Copy, Shader, TexHash, TexUpload, Pacotes, Count };
    inline double g_regs = 0, g_pacotes = 0;
    inline double g_oclEsperaMs = 0, g_oclEsperas = 0;   // thread do jogo esperando a contagem de uma consulta de oclusao   // registradores escritos e pacotes lidos do buffer de comandos (FCRECOMP_PERF)
    inline double g_ms[Count];
    inline double g_hashMB = 0, g_uploadMB = 0, g_hashCalls = 0;   // texturas: MB conferidos/enviados e conferencias
    inline double g_psoNew = 0, g_psoMs = 0, g_texNew = 0, g_texCreateMs = 0, g_texPrepMs = 0, g_rtCreateMs = 0;   // rtCreate: superficies/resolves criados   // pipelines (PSO) e texturas criados; tempo criando PSOs (ms)
    inline double Now() { static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }(); LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }
    // Scope roda milhares de vezes por quadro (cada desenho, textura e copia de vertices), sempre: com o QueryPerformanceCounter
    // eram ~3,9% da linha de desenho (roteiro 2026-11-25_20-39). Agora conta pelo contador do processador (rdtsc), com a
    // escala medida uma vez (2 ms) contra o relogio do Windows.
    inline double MsPorTique()
    {
        static const double k = [] {
            LARGE_INTEGER f, a, b;
            QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
            const uint64_t t0 = __rdtsc();
            do QueryPerformanceCounter(&b); while ((b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart < 2.0);
            const uint64_t t1 = __rdtsc();
            return ((b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart) / (double)(t1 - t0);
        }();
        return k;
    }
    struct Scope { Cat c; uint64_t t; Scope(Cat c) : c(c), t(__rdtsc()) {} ~Scope() { g_ms[c] += (double)(__rdtsc() - t) * MsPorTique(); } };
    // FCRECOMP_AVLOG=1: horario (Now, ms) das etapas do inicio dos videos no log, linhas "[av]" (sincronia som x imagem)
    inline bool AvLog() { static const bool on = getenv("FCRECOMP_AVLOG") != nullptr; return on; }
}
