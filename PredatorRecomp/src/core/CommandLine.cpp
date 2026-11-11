#include "core/CommandLine.h"

#include <charconv>
#include <system_error>

namespace predator
{
    namespace
    {
        std::optional<Command> CommandFromName(std::string_view name)
        {
            if (name == "all") return Command::All;
            if (name == "cpu") return Command::Cpu;
            if (name == "shaders") return Command::Shaders;
            if (name == "info") return Command::Info;
            if (name == "disasm") return Command::Disasm;
            if (name == "help") return Command::Help;
            if (name == "version") return Command::Version;
            return std::nullopt;
        }

        // Aceita "--opcao valor" e "--opcao=valor". Devolve o valor e avanca o indice quando ele vem no proximo argumento.
        class Reader
        {
        public:
            explicit Reader(std::span<const std::string_view> args) : m_args(args) {}

            [[nodiscard]] bool Done() const noexcept { return m_index >= m_args.size(); }
            std::string_view Next() { return m_args[m_index++]; }

            // Le o valor de uma opcao (inline depois de '=' ou o proximo argumento)
            std::optional<std::string_view> Value(std::string_view inlineValue, bool hasInline)
            {
                if (hasInline) return inlineValue;
                if (Done()) return std::nullopt;
                std::string_view v = m_args[m_index];
                if (v.starts_with("--")) return std::nullopt;   // a proxima coisa e outra opcao: valor faltando
                ++m_index;
                return v;
            }

        private:
            std::span<const std::string_view> m_args;
            std::size_t m_index = 0;
        };

        std::optional<std::uint32_t> ParseUnsigned(std::string_view text)
        {
            std::uint32_t value = 0;
            auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
            return value;
        }

        // numero decimal ou hexadecimal (0x...)
        std::optional<std::uint32_t> ParseAddress(std::string_view text)
        {
            int base = 10;
            if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { text.remove_prefix(2); base = 16; }
            std::uint32_t value = 0;
            auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, base);
            if (text.empty() || ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
            return value;
        }

        ParseResult Fail(std::string message) { return ParseResult{ std::nullopt, std::move(message) }; }
    }

    std::string_view CommandName(Command command) noexcept
    {
        switch (command)
        {
        case Command::All: return "all";
        case Command::Cpu: return "cpu";
        case Command::Shaders: return "shaders";
        case Command::Info: return "info";
        case Command::Disasm: return "disasm";
        case Command::Help: return "help";
        case Command::Version: return "version";
        }
        return "?";
    }

    ParseResult ParseCommandLine(std::span<const std::string_view> args)
    {
        Options options;
        Reader reader(args);
        bool commandSeen = false;

        while (!reader.Done())
        {
            std::string_view arg = reader.Next();

            if (arg == "-h" || arg == "--help") { options.command = Command::Help; return ParseResult{ options, {} }; }
            if (arg == "--version") { options.command = Command::Version; return ParseResult{ options, {} }; }

            if (arg.starts_with("--"))
            {
                std::string_view name = arg.substr(2), inlineValue;
                bool hasInline = false;
                if (auto eq = name.find('='); eq != std::string_view::npos)
                {
                    inlineValue = name.substr(eq + 1);
                    name = name.substr(0, eq);
                    hasInline = true;
                }

                auto requireValue = [&](std::string_view option) -> std::optional<std::string_view> {
                    auto v = reader.Value(inlineValue, hasInline);
                    if (!v || v->empty()) return std::nullopt;
                    (void)option;
                    return v;
                };
                auto flag = [&](bool& target) -> bool {
                    if (hasInline) return false;   // opcoes de liga/desliga nao recebem valor
                    target = true;
                    return true;
                };

                if (name == "xex" || name == "exe")
                {
                    auto v = requireValue(name);
                    if (!v) return Fail("--" + std::string(name) + " precisa do caminho do executavel");
                    options.executable = std::filesystem::path(*v);
                }
                else if (name == "image")
                {
                    auto v = requireValue(name);
                    if (!v) return Fail("--image precisa do caminho da imagem");
                    options.image = std::filesystem::path(*v);
                }
                else if (name == "start" || name == "end" || name == "count")
                {
                    auto v = requireValue(name);
                    auto n = v ? ParseAddress(*v) : std::nullopt;
                    if (!n) return Fail("--" + std::string(name) + " precisa de um numero (decimal ou 0x...)");
                    if (name == "start") options.start = *n;
                    else if (name == "end") options.end = *n;
                    else options.count = *n;
                }
                else if (name == "shaders")
                {
                    auto v = requireValue(name);
                    if (!v) return Fail("--shaders precisa de um arquivo ou pasta");
                    options.shaders.emplace_back(*v);
                }
                else if (name == "out" || name == "output")
                {
                    auto v = requireValue(name);
                    if (!v) return Fail("--out precisa de uma pasta");
                    options.outputDir = std::filesystem::path(*v);
                }
                else if (name == "config")
                {
                    auto v = requireValue(name);
                    if (!v) return Fail("--config precisa de um arquivo");
                    options.configFile = std::filesystem::path(*v);
                }
                else if (name == "jobs")
                {
                    auto v = requireValue(name);
                    auto n = v ? ParseUnsigned(*v) : std::nullopt;
                    if (!n || *n > 1024) return Fail("--jobs precisa de um numero entre 0 e 1024");
                    options.jobs = *n;
                }
                else if (name == "verbose") { if (!flag(options.verbose)) return Fail("--verbose nao recebe valor"); }
                else if (name == "quiet") { if (!flag(options.quiet)) return Fail("--quiet nao recebe valor"); }
                else if (name == "dry-run") { if (!flag(options.dryRun)) return Fail("--dry-run nao recebe valor"); }
                else if (name == "force") { if (!flag(options.force)) return Fail("--force nao recebe valor"); }
                else
                    return Fail("opcao desconhecida: " + std::string(arg));
                continue;
            }

            if (arg.size() > 1 && arg[0] == '-')
                return Fail("opcao desconhecida: " + std::string(arg));

            // posicionais: o primeiro pode ser o comando; o resto e o executavel
            if (!commandSeen && options.executable.empty())
                if (auto cmd = CommandFromName(arg))
                {
                    options.command = *cmd;
                    commandSeen = true;
                    if (*cmd == Command::Help || *cmd == Command::Version) return ParseResult{ options, {} };
                    continue;
                }
            if (!options.executable.empty())
                return Fail("argumento a mais: " + std::string(arg) + " (o executavel ja foi informado)");
            options.executable = std::filesystem::path(arg);
            commandSeen = true;
        }

        if (options.verbose && options.quiet)
            return Fail("--verbose e --quiet nao podem ser usados juntos");
        return ParseResult{ options, {} };
    }

    std::vector<std::string> ValidateOptions(const Options& options)
    {
        std::vector<std::string> errors;
        std::error_code ec;
        bool needsCpu = options.command == Command::All || options.command == Command::Cpu ||
                        options.command == Command::Info || options.command == Command::Disasm;
        bool needsShaders = options.command == Command::All || options.command == Command::Shaders;

        if (needsCpu)
        {
            if (options.executable.empty())
                errors.emplace_back("informe o executavel do jogo (--xex <arquivo> ou como argumento)");
            else if (!std::filesystem::is_regular_file(options.executable, ec))
                errors.emplace_back("executavel nao encontrado: " + options.executable.string());
        }
        if (needsShaders)
        {
            if (options.shaders.empty() && options.command == Command::Shaders)
                errors.emplace_back("informe os shaders (--shaders <arquivo ou pasta>)");
            for (const auto& s : options.shaders)
                if (!std::filesystem::exists(s, ec))
                    errors.emplace_back("shaders nao encontrados: " + s.string());
        }
        if (!options.image.empty() && !std::filesystem::is_regular_file(options.image, ec))
            errors.emplace_back("imagem nao encontrada: " + options.image.string());
        if (options.start && options.end && *options.end <= *options.start)
            errors.emplace_back("--end precisa ser maior que --start");
        if (!options.configFile.empty() && !std::filesystem::is_regular_file(options.configFile, ec))
            errors.emplace_back("arquivo de configuracao nao encontrado: " + options.configFile.string());
        if (std::filesystem::exists(options.outputDir, ec) && !std::filesystem::is_directory(options.outputDir, ec))
            errors.emplace_back("a saida existe e nao e uma pasta: " + options.outputDir.string());
        return errors;
    }

    std::string UsageText(std::string_view programName)
    {
        std::string p(programName);
        return "PredatorRecomp - recompilador estatico (CPU PowerPC/VMX128 -> C, shaders -> HLSL)\n"
               "\n"
               "Uso:\n"
               "  " + p + " [comando] [executavel] [opcoes]\n"
               "\n"
               "Comandos:\n"
               "  all        recompila CPU e shaders num fluxo so (padrao)\n"
               "  cpu        so o executavel\n"
               "  shaders    so os shaders\n"
               "  info       mostra o que foi lido do executavel\n"
               "  disasm     desmonta instrucoes (--start/--end/--count; sem --start: todo o codigo)\n"
               "  help       mostra esta ajuda\n"
               "  version    mostra a versao\n"
               "\n"
               "Opcoes:\n"
               "  --xex <arquivo>       executavel do jogo (tambem aceito como argumento sem opcao)\n"
               "  --image <arquivo>     imagem de memoria ja descriptografada (executavel criptografado)\n"
               "  --start <endereco>    endereco inicial (disasm); aceita 0x...\n"
               "  --end <endereco>      endereco final, exclusivo (disasm)\n"
               "  --count <n>           numero de instrucoes (disasm)\n"
               "  --shaders <caminho>   arquivo ou pasta com microcodigo de shader (pode repetir)\n"
               "  --out <pasta>         pasta de saida (padrao: out)\n"
               "  --config <arquivo>    configuracao do projeto\n"
               "  --jobs <n>            threads de trabalho (0 = numero de nucleos)\n"
               "  --force               sobrescreve uma pasta de saida que ja tenha conteudo\n"
               "  --dry-run             so valida e mostra o que seria feito\n"
               "  --verbose / --quiet   mais / menos mensagens\n"
               "  -h, --help            ajuda\n"
               "  --version             versao\n"
               "\n"
               "Exemplo:\n"
               "  " + p + " all --xex default.xex --shaders shaders --out gerado\n"
               "  " + p + " disasm default.xex --image image.bin --start 0x826444F0 --count 32\n";
    }
}
