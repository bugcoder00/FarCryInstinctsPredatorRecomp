// Testes da linha de comando (sem framework: cada falha imprime a linha e o teste devolve 1)
#include "core/CommandLine.h"

#include <cstdio>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace
{
    int g_failures = 0;

#define CHECK(cond)                                                                 \
    do                                                                              \
    {                                                                               \
        if (!(cond))                                                                \
        {                                                                           \
            std::fprintf(stderr, "%s:%d: falhou: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

    predator::ParseResult Parse(std::initializer_list<std::string_view> list)
    {
        std::vector<std::string_view> args(list);
        return predator::ParseCommandLine(args);
    }
}

int main()
{
    using namespace predator;

    {   // sem argumentos: comando "all", saida padrao
        auto r = Parse({});
        CHECK(r.Ok());
        CHECK(r.options->command == Command::All);
        CHECK(r.options->outputDir == "out");
    }
    {   // comando + executavel posicional + opcoes nas duas formas
        auto r = Parse({ "cpu", "default.xex", "--out=gerado", "--jobs", "8", "--verbose" });
        CHECK(r.Ok());
        CHECK(r.options->command == Command::Cpu);
        CHECK(r.options->executable == "default.xex");
        CHECK(r.options->outputDir == "gerado");
        CHECK(r.options->jobs == 8);
        CHECK(r.options->verbose);
    }
    {   // varios --shaders
        auto r = Parse({ "shaders", "--shaders", "a", "--shaders=b" });
        CHECK(r.Ok());
        CHECK(r.options->shaders.size() == 2);
    }
    {   // um nome de comando depois do executavel e tratado como argumento a mais
        auto r = Parse({ "jogo.xex", "cpu" });
        CHECK(!r.Ok());
    }
    {   // erros
        CHECK(!Parse({ "--xex" }).Ok());                     // valor faltando
        CHECK(!Parse({ "--xex", "--out", "x" }).Ok());       // valor faltando (proxima e outra opcao)
        CHECK(!Parse({ "--jobs", "abc" }).Ok());             // numero invalido
        CHECK(!Parse({ "--jobs", "5000" }).Ok());            // fora da faixa
        CHECK(!Parse({ "--naoexiste" }).Ok());               // opcao desconhecida
        CHECK(!Parse({ "-x" }).Ok());                        // opcao curta desconhecida
        CHECK(!Parse({ "--verbose=1" }).Ok());               // liga/desliga nao recebe valor
        CHECK(!Parse({ "--verbose", "--quiet" }).Ok());      // conflito
        CHECK(!Parse({ "a.xex", "b.xex" }).Ok());            // dois executaveis
    }
    {   // ajuda e versao
        CHECK(Parse({ "--help" }).options->command == Command::Help);
        CHECK(Parse({ "help" }).options->command == Command::Help);
        CHECK(Parse({ "--version" }).options->command == Command::Version);
    }
    {   // validacao: executavel inexistente
        Options o;
        o.command = Command::Cpu;
        o.executable = "nao_existe_12345.xex";
        CHECK(!ValidateOptions(o).empty());
    }

    if (g_failures) std::fprintf(stderr, "%d verificacao(oes) falharam\n", g_failures);
    else std::puts("todos os testes da linha de comando passaram");
    return g_failures ? 1 : 0;
}
