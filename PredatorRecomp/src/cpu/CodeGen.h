// Gerador de C: cada funcao do jogo vira uma funcao C "void sub_XXXXXXXX(PredatorCpuContext* ctx, uint8_t* base)"
//
// Saida (pasta outputDir):
//   predator_cpu.h        contexto e operacoes (copia do cabecalho do runtime)
//   predator_functions.h  prototipos de todas as funcoes
//   predator_table.c      tabela endereco -> funcao (ordenada, para chamadas indiretas)
//   predator_code_NNN.c   as funcoes, divididas em varios arquivos
#pragma once

#include "cpu/Analysis.h"
#include "loader/Executable.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace predator
{
    class Diagnostics;
}

namespace predator::cpu
{
    struct CodeGenStats
    {
        std::uint64_t instructions = 0;
        std::uint64_t unimplemented = 0;
        std::uint64_t dataWords = 0;   // palavras que nao sao instrucoes dentro de funcoes (dados, preenchimento)
        std::map<std::string, std::uint64_t> unimplementedByName;
        std::uint32_t files = 0;
        std::uint64_t bytes = 0;
    };

    struct CodeGenSettings
    {
        std::filesystem::path outputDir;
        std::uint32_t instructionsPerFile = 40000;
        bool comments = true;   // desmontagem de cada instrucao como comentario
    };

    [[nodiscard]] bool GenerateCode(const loader::Executable& exe, const ProgramAnalysis& analysis, const CodeGenSettings& settings,
                                    CodeGenStats& stats, Diagnostics& diag);

    // Gera o C de uma unica funcao (usado pelos testes)
    [[nodiscard]] std::string GenerateFunction(const loader::Executable& exe, const ProgramAnalysis& analysis, const Function& function,
                                               CodeGenStats& stats, bool comments = true);

    // Texto do cabecalho predator_cpu.h embutido no programa
    [[nodiscard]] std::string_view RuntimeHeader();
}
