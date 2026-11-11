#include "core/Pipeline.h"

#include "cpu/CpuTranslator.h"
#include "gpu/ShaderTranslator.h"

#include <system_error>
#include <thread>

namespace predator
{
    namespace
    {
        unsigned EffectiveJobs(std::uint32_t requested)
        {
            if (requested) return requested;
            unsigned n = std::thread::hardware_concurrency();
            return n ? n : 1;
        }

        // ordem de gravidade para combinar os resultados das etapas
        ExitCode Worst(ExitCode a, ExitCode b)
        {
            auto rank = [](ExitCode c) {
                switch (c)
                {
                case ExitCode::Success: return 0;
                case ExitCode::NotImplemented: return 1;
                case ExitCode::Failure: return 2;
                case ExitCode::BadArguments: return 3;
                }
                return 3;
            };
            return rank(a) >= rank(b) ? a : b;
        }
    }

    ExitCode Pipeline::Run()
    {
        // comandos de consulta: nao geram arquivos
        if (m_options.command == Command::Info || m_options.command == Command::Disasm)
        {
            cpu::CpuTranslatorSettings settings{ m_options.executable, m_options.image, m_options.outputDir / "cpu", EffectiveJobs(m_options.jobs) };
            cpu::CpuStatus st = m_options.command == Command::Info
                ? cpu::PrintExecutableInfo(settings, m_diag)
                : cpu::DisassembleExecutable(settings, cpu::DisasmRange{ m_options.start, m_options.end, m_options.count }, m_diag);
            return st == cpu::CpuStatus::Ok ? ExitCode::Success : ExitCode::Failure;
        }

        const bool cpu = m_options.command == Command::All || m_options.command == Command::Cpu;
        const bool shaders = (m_options.command == Command::All || m_options.command == Command::Shaders) && !m_options.shaders.empty();

        m_diag.Info("PredatorRecomp " PREDATOR_VERSION_STRING " - comando: " + std::string(CommandName(m_options.command)));
        if (cpu) m_diag.Info("  executavel: " + m_options.executable.string());
        if (cpu && !m_options.image.empty()) m_diag.Info("  imagem:     " + m_options.image.string());
        for (const auto& s : m_options.shaders) m_diag.Info("  shaders:    " + s.string());
        m_diag.Info("  saida:      " + m_options.outputDir.string());
        m_diag.Info("  threads:    " + std::to_string(EffectiveJobs(m_options.jobs)));

        if (m_options.dryRun)
        {
            m_diag.Info("(--dry-run: nada foi gerado)");
            return ExitCode::Success;
        }
        if (!PrepareOutput()) return ExitCode::Failure;

        ExitCode result = ExitCode::Success;
        if (cpu) result = Worst(result, RunCpu());
        if (shaders) result = Worst(result, RunShaders());
        return result;
    }

    bool Pipeline::PrepareOutput()
    {
        std::error_code ec;
        const auto& out = m_options.outputDir;
        if (std::filesystem::exists(out, ec) && !std::filesystem::is_empty(out, ec) && !m_options.force)
        {
            m_diag.Error("a pasta de saida ja tem arquivos: " + out.string() + " (use --force para sobrescrever)");
            return false;
        }
        std::filesystem::create_directories(out, ec);
        if (ec)
        {
            m_diag.Error("nao foi possivel criar a pasta de saida " + out.string() + ": " + ec.message());
            return false;
        }
        return true;
    }

    ExitCode Pipeline::RunCpu()
    {
        cpu::CpuTranslatorSettings settings{ m_options.executable, m_options.image, m_options.outputDir / "cpu", EffectiveJobs(m_options.jobs) };
        switch (cpu::TranslateExecutable(settings, m_diag))
        {
        case cpu::CpuStatus::Ok: return ExitCode::Success;
        case cpu::CpuStatus::NotImplemented: return ExitCode::NotImplemented;
        case cpu::CpuStatus::Failed: return ExitCode::Failure;
        }
        return ExitCode::Failure;
    }

    ExitCode Pipeline::RunShaders()
    {
        gpu::ShaderTranslatorSettings settings{ m_options.shaders, m_options.outputDir / "shaders", EffectiveJobs(m_options.jobs) };
        switch (gpu::TranslateShaders(settings, m_diag))
        {
        case gpu::ShaderStatus::Ok: return ExitCode::Success;
        case gpu::ShaderStatus::NotImplemented: return ExitCode::NotImplemented;
        case gpu::ShaderStatus::Failed: return ExitCode::Failure;
        }
        return ExitCode::Failure;
    }
}
