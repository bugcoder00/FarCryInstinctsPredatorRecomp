// Diagnosticos: mensagens para o terminal com nivel e contagem de erros/avisos
#pragma once

#include <cstddef>
#include <string_view>

namespace predator
{
    enum class Severity
    {
        Debug,
        Info,
        Warning,
        Error,
    };

    class Diagnostics
    {
    public:
        void SetVerbose(bool verbose) noexcept { m_verbose = verbose; }
        void SetQuiet(bool quiet) noexcept { m_quiet = quiet; }

        void Report(Severity severity, std::string_view message);
        void Debug(std::string_view message) { Report(Severity::Debug, message); }
        void Info(std::string_view message) { Report(Severity::Info, message); }
        void Warning(std::string_view message) { Report(Severity::Warning, message); }
        void Error(std::string_view message) { Report(Severity::Error, message); }

        [[nodiscard]] std::size_t ErrorCount() const noexcept { return m_errors; }
        [[nodiscard]] std::size_t WarningCount() const noexcept { return m_warnings; }

    private:
        bool m_verbose = false;
        bool m_quiet = false;
        std::size_t m_errors = 0;
        std::size_t m_warnings = 0;
    };
}
