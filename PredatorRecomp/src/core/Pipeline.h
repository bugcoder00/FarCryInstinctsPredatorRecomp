// Orquestracao: recebe as opcoes validadas e chama os tradutores de CPU e de shaders na ordem certa
#pragma once

#include "core/CommandLine.h"
#include "core/Diagnostics.h"

namespace predator
{
    enum class ExitCode : int
    {
        Success = 0,
        Failure = 1,        // erro durante a traducao
        BadArguments = 2,   // linha de comando invalida ou entradas inexistentes
        NotImplemented = 3, // etapa ainda nao implementada
    };

    class Pipeline
    {
    public:
        Pipeline(const Options& options, Diagnostics& diag) : m_options(options), m_diag(diag) {}

        [[nodiscard]] ExitCode Run();

    private:
        [[nodiscard]] bool PrepareOutput();
        [[nodiscard]] ExitCode RunCpu();
        [[nodiscard]] ExitCode RunShaders();

        const Options& m_options;
        Diagnostics& m_diag;
    };
}
