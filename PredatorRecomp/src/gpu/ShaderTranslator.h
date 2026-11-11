// Tradutor de shaders: microcodigo da GPU do console -> HLSL (DirectX 12)
//
// Etapas planejadas (cada uma em seu proprio arquivo dentro de src/gpu):
//   1. ler os binarios de shader (arquivo unico ou pasta)
//   2. decodificar o microcodigo (blocos de controle, ALU vetorial/escalar, leituras de vertice/textura)
//   3. mapear constantes, entradas/saidas e registradores de textura
//   4. gerar HLSL
#pragma once

#include <filesystem>
#include <vector>

namespace predator
{
    class Diagnostics;
}

namespace predator::gpu
{
    struct ShaderTranslatorSettings
    {
        std::vector<std::filesystem::path> inputs;   // arquivos ou pastas
        std::filesystem::path outputDir;             // os .hlsl gerados vao para outputDir/shaders
        unsigned jobs = 0;
    };

    enum class ShaderStatus
    {
        Ok,
        Failed,
        NotImplemented,
    };

    [[nodiscard]] ShaderStatus TranslateShaders(const ShaderTranslatorSettings& settings, Diagnostics& diag);
}
