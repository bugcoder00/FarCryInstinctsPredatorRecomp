// PredatorRecomp - ponto de entrada da linha de comando
#include "core/CommandLine.h"
#include "core/Diagnostics.h"
#include "core/Pipeline.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string_view>
#include <vector>

namespace
{
    std::string ProgramName(const char* argv0)
    {
        if (!argv0 || !*argv0) return "predatorrecomp";
        return std::filesystem::path(argv0).filename().string();
    }

    int Main(int argc, char** argv)
    {
        using namespace predator;

        const std::string program = ProgramName(argc > 0 ? argv[0] : nullptr);
        std::vector<std::string_view> args;
        args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

        ParseResult parsed = ParseCommandLine(args);
        if (!parsed.Ok())
        {
            std::fprintf(stderr, "erro: %s\n\n%s", parsed.error.c_str(), UsageText(program).c_str());
            return static_cast<int>(ExitCode::BadArguments);
        }
        const Options& options = *parsed.options;

        if (options.command == Command::Help)
        {
            std::fputs(UsageText(program).c_str(), stdout);
            return static_cast<int>(ExitCode::Success);
        }
        if (options.command == Command::Version)
        {
            std::printf("PredatorRecomp %s\n", PREDATOR_VERSION_STRING);
            return static_cast<int>(ExitCode::Success);
        }

        Diagnostics diag;
        diag.SetVerbose(options.verbose);
        diag.SetQuiet(options.quiet);

        if (auto errors = ValidateOptions(options); !errors.empty())
        {
            for (const auto& e : errors) diag.Error(e);
            std::fprintf(stderr, "\nuse \"%s --help\" para ver as opcoes\n", program.c_str());
            return static_cast<int>(ExitCode::BadArguments);
        }

        Pipeline pipeline(options, diag);
        ExitCode code = pipeline.Run();
        if (diag.ErrorCount() || diag.WarningCount())
            diag.Info(std::to_string(diag.ErrorCount()) + " erro(s), " + std::to_string(diag.WarningCount()) + " aviso(s)");
        return static_cast<int>(code);
    }
}

int main(int argc, char** argv)
{
    try
    {
        return Main(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "erro interno: %s\n", e.what());
    }
    catch (...)
    {
        std::fprintf(stderr, "erro interno desconhecido\n");
    }
    return static_cast<int>(predator::ExitCode::Failure);
}
