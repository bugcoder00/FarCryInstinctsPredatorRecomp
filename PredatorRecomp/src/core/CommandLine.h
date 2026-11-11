// Linha de comando do PredatorRecomp
//
//   predatorrecomp [comando] [opcoes]
//
// Comandos:
//   all      recompila CPU e shaders num fluxo so (padrao)
//   cpu      so o executavel (PowerPC/VMX128 -> C)
//   shaders  so os shaders (microcodigo -> HLSL)
//   info     mostra o que foi lido do executavel (cabecalhos, regioes, importacoes)
//   disasm   desmonta instrucoes do executavel
//
// O resultado e uma estrutura validada (Options) ou uma mensagem de erro; nada aqui toca no disco alem de
// conferir se as entradas existem (isso fica para ValidateOptions).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace predator
{
    enum class Command
    {
        All,
        Cpu,
        Shaders,
        Info,
        Disasm,
        Help,
        Version,
    };

    struct Options
    {
        Command command = Command::All;
        std::filesystem::path executable;           // --xex / posicional: executavel do jogo
        std::filesystem::path image;                // --image: imagem de memoria ja descriptografada (opcional)
        std::optional<std::uint32_t> start;         // --start: endereco inicial (disasm)
        std::optional<std::uint32_t> end;           // --end: endereco final, exclusivo (disasm)
        std::uint32_t count = 0;                    // --count: numero de instrucoes (disasm)
        std::vector<std::filesystem::path> shaders; // --shaders: arquivos ou pastas com microcodigo de shader
        std::filesystem::path outputDir = "out";    // --out: pasta de saida
        std::filesystem::path configFile;           // --config: arquivo de configuracao do projeto (opcional)
        std::uint32_t jobs = 0;                     // --jobs: 0 = numero de nucleos
        bool verbose = false;
        bool quiet = false;
        bool dryRun = false;                        // --dry-run: valida e mostra o plano, nao gera nada
        bool force = false;                         // --force: sobrescreve a pasta de saida
    };

    struct ParseResult
    {
        std::optional<Options> options;
        std::string error;   // preenchido quando options esta vazio

        [[nodiscard]] bool Ok() const noexcept { return options.has_value(); }
    };

    // Interpreta os argumentos (sem o nome do programa)
    [[nodiscard]] ParseResult ParseCommandLine(std::span<const std::string_view> args);

    // Confere se as entradas pedidas pelo comando existem; devolve as mensagens de erro (vazio = ok)
    [[nodiscard]] std::vector<std::string> ValidateOptions(const Options& options);

    [[nodiscard]] std::string UsageText(std::string_view programName);
    [[nodiscard]] std::string_view CommandName(Command command) noexcept;
}
