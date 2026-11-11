#include "core/Diagnostics.h"

#include <cstdio>

namespace predator
{
    void Diagnostics::Report(Severity severity, std::string_view message)
    {
        switch (severity)
        {
        case Severity::Debug:
            if (!m_verbose) return;
            std::fprintf(stdout, "[debug] %.*s\n", static_cast<int>(message.size()), message.data());
            break;
        case Severity::Info:
            if (m_quiet) return;
            std::fprintf(stdout, "%.*s\n", static_cast<int>(message.size()), message.data());
            break;
        case Severity::Warning:
            ++m_warnings;
            std::fflush(stdout);   // mantem a ordem das mensagens quando a saida normal esta redirecionada
            std::fprintf(stderr, "aviso: %.*s\n", static_cast<int>(message.size()), message.data());
            break;
        case Severity::Error:
            ++m_errors;
            std::fflush(stdout);
            std::fprintf(stderr, "erro: %.*s\n", static_cast<int>(message.size()), message.data());
            break;
        }
    }
}
