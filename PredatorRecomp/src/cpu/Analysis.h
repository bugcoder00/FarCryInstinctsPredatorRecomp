// Analise do codigo: onde comecam e terminam as funcoes, e para onde vao os desvios por tabela (switch)
#pragma once

#include "loader/Executable.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace predator
{
    class Diagnostics;
}

namespace predator::cpu
{
    enum class FunctionSource : std::uint8_t
    {
        Pdata,       // tabela de excecoes do executavel (inicio e tamanho exatos)
        Call,        // alvo de "bl" fora de qualquer funcao conhecida
        SecondEntry, // alvo de "bl" no meio de outra funcao (ex.: rotinas de salvar registradores com varias entradas)
        Entry,       // ponto de entrada do executavel
        Import,      // "thunk" de funcao importada do sistema
        Gap,         // codigo fora de qualquer funcao conhecida (funcoes so chamadas por ponteiro)
        Pointer,     // endereco de codigo citado nos dados ou montado com lis/addi
        BranchEntry, // alvo de um desvio "b" vindo de outra funcao (entra no meio desta)
    };

    struct Function
    {
        std::uint32_t start = 0;
        std::uint32_t end = 0;   // exclusivo
        std::uint32_t bodyStart = 0;   // de onde o corpo e traduzido; diferente de start numa entrada secundaria
                                       // (o corpo inteiro da funcao que a contem, comecando com um salto para start)
        FunctionSource source = FunctionSource::Pdata;
        std::string importLibrary;   // so para Import
        std::uint32_t importOrdinal = 0;
    };

    struct JumpTable
    {
        std::uint32_t address = 0;              // o bctr
        std::vector<std::uint32_t> targets;
    };

    struct ProgramAnalysis
    {
        std::vector<Function> functions;               // ordenadas pelo inicio
        std::map<std::uint32_t, JumpTable> jumpTables; // por endereco do bctr
        std::uint32_t pdataAddress = 0;
        std::uint32_t pdataCount = 0;
        std::uint64_t uncoveredWords = 0;              // palavras de codigo fora de qualquer funcao (sem contar zeros)
        std::uint32_t unresolvedJumps = 0;             // bctr sem tabela reconhecida

        [[nodiscard]] const Function* FunctionAt(std::uint32_t start) const noexcept;
    };

    [[nodiscard]] ProgramAnalysis AnalyzeProgram(const loader::Executable& exe, Diagnostics& diag);
}
