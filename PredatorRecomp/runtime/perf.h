// Medicao simples de tempo por categoria (thread da GPU). FCRECOMP_PERF=1 escreve um resumo no log.
#pragma once
#include <windows.h>
#include <cstdint>

namespace perf
{
    enum Cat { Idle, WaitReg, Draw, Resolve, Present, Fence, Texture, Copy, Shader, TexHash, TexUpload, Count };
    inline double g_ms[Count];
    inline double g_hashMB = 0, g_uploadMB = 0, g_hashCalls = 0;   // texturas: MB conferidos/enviados e conferencias
    inline double g_psoNew = 0, g_psoMs = 0, g_texNew = 0, g_texCreateMs = 0, g_texPrepMs = 0, g_rtCreateMs = 0;   // rtCreate: superficies/resolves criados   // pipelines (PSO) e texturas criados; tempo criando PSOs (ms)
    inline double Now() { static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }(); LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }
    struct Scope { Cat c; double t; Scope(Cat c) : c(c), t(Now()) {} ~Scope() { g_ms[c] += Now() - t; } };
    // FCRECOMP_AVLOG=1: horario (Now, ms) das etapas do inicio dos videos no log, linhas "[av]" (sincronia som x imagem)
    inline bool AvLog() { static const bool on = getenv("FCRECOMP_AVLOG") != nullptr; return on; }
}
