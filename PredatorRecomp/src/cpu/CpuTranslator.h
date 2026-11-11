// Tradutor de CPU: executavel PowerPC 64 bits (com VMX128) -> codigo C
//
// Etapas (cada uma em seu proprio arquivo):
//   1. carregar o executavel e mapear as regioes de codigo/dados       (loader/Executable)
//   2. decodificar as instrucoes (PowerPC + VMX + VMX128)              (cpu/Decoder, cpu/OpcodeTable.inc)
//   3. descobrir funcoes e blocos basicos                              (proxima etapa)
//   4. gerar C usando o contexto PredatorCpuContext                    (proxima etapa)
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace predator
{
    class Diagnostics;
}

namespace predator::cpu
{
    struct CpuTranslatorSettings
    {
        std::filesystem::path executable;
        std::filesystem::path image;       // imagem ja descriptografada (vazio = tirar do proprio executavel)
        std::filesystem::path outputDir;   // os .c/.h gerados vao para outputDir/cpu
        unsigned jobs = 0;
    };

    enum class CpuStatus
    {
        Ok,
        Failed,
        NotImplemented,
    };

    // Carrega o executavel, decodifica todo o codigo e (proxima etapa) gera o C
    [[nodiscard]] CpuStatus TranslateExecutable(const CpuTranslatorSettings& settings, Diagnostics& diag);

    // Mostra cabecalhos, regioes e importacoes
    [[nodiscard]] CpuStatus PrintExecutableInfo(const CpuTranslatorSettings& settings, Diagnostics& diag);

    struct DisasmRange
    {
        std::optional<std::uint32_t> start;   // vazio = todas as regioes de codigo
        std::optional<std::uint32_t> end;     // exclusivo
        std::uint32_t count = 0;              // 0 = sem limite (ou 1 regiao inteira)
    };

    // Desmonta para a saida padrao: "endereco  palavra  instrucao"
    [[nodiscard]] CpuStatus DisassembleExecutable(const CpuTranslatorSettings& settings, const DisasmRange& range, Diagnostics& diag);
}
