#include "cpu/CpuTranslator.h"

#include "core/Diagnostics.h"
#include "cpu/Analysis.h"
#include "cpu/CodeGen.h"
#include "cpu/Decoder.h"
#include "cpu/Disassembler.h"
#include "loader/Executable.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace predator::cpu
{
    namespace
    {
        std::string Hex32(std::uint32_t v)
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%08X", v);
            return buf;
        }

        std::optional<loader::Executable> Load(const CpuTranslatorSettings& settings, Diagnostics& diag)
        {
            std::string error;
            auto exe = loader::Executable::Load(settings.executable, settings.image, error);
            if (!exe) diag.Error(error);
            return exe;
        }
    }

    CpuStatus PrintExecutableInfo(const CpuTranslatorSettings& settings, Diagnostics& diag)
    {
        auto exe = Load(settings, diag);
        if (!exe) return CpuStatus::Failed;
        const auto& info = exe->Info();

        std::printf("executavel:        %s\n", settings.executable.string().c_str());
        if (!info.originalName.empty()) std::printf("nome original:     %s\n", info.originalName.c_str());
        std::printf("endereco base:     0x%08X\n", info.baseAddress);
        std::printf("ponto de entrada:  0x%08X\n", info.entryPoint);
        std::printf("tamanho da imagem: 0x%08X (%u KB, paginas de %u KB)\n", info.imageSize, info.imageSize / 1024, info.pageSize / 1024);
        std::printf("criptografia:      %s\n", info.encryption ? "sim" : "nao");
        static const char* const compression[] = { "nenhuma", "basica", "normal (LZX)", "delta" };
        std::printf("compressao:        %s\n", info.compression < 4 ? compression[info.compression] : "desconhecida");
        if (info.defaultStackSize) std::printf("pilha padrao:      %u KB\n", info.defaultStackSize / 1024);

        std::printf("\nregioes (%zu):\n", info.regions.size());
        for (const auto& r : info.regions)
            std::printf("  0x%08X - 0x%08X  %-16s %u KB\n", r.start, r.start + r.size, loader::RegionKindName(r.kind), r.size / 1024);

        std::printf("\nbibliotecas importadas (%zu):\n", info.imports.size());
        for (const auto& lib : info.imports)
            std::printf("  %-24s versao %u.%u.%u.%u  %zu entradas\n", lib.name.c_str(), lib.version >> 28, (lib.version >> 24) & 0xF,
                        (lib.version >> 8) & 0xFFFF, lib.version & 0xFF, lib.records.size());

        std::printf("\ncabecalhos opcionais (%zu):\n", info.headers.size());
        for (const auto& h : info.headers)
            std::printf("  %08X = %08X  %s\n", h.key, h.value, loader::OptionalHeaderName(h.key));
        return CpuStatus::Ok;
    }

    CpuStatus DisassembleExecutable(const CpuTranslatorSettings& settings, const DisasmRange& range, Diagnostics& diag)
    {
        auto exe = Load(settings, diag);
        if (!exe) return CpuStatus::Failed;

        // faixas a desmontar: a pedida ou todas as regioes de codigo
        std::vector<std::pair<std::uint32_t, std::uint32_t>> spans;
        if (range.start)
        {
            std::uint64_t end = range.end ? *range.end : std::uint64_t{ exe->Info().baseAddress } + exe->Image().size();
            if (range.count) end = std::min<std::uint64_t>(end, std::uint64_t{ *range.start } + std::uint64_t{ range.count } * 4);
            if (!exe->Contains(*range.start, 4))
            {
                diag.Error("o endereco 0x" + Hex32(*range.start) + " esta fora da imagem");
                return CpuStatus::Failed;
            }
            spans.emplace_back(*range.start & ~3u, static_cast<std::uint32_t>(end));
        }
        else
        {
            for (const auto& r : exe->Info().regions)
                if (r.kind == loader::RegionKind::Code) spans.emplace_back(r.start, r.start + r.size);
        }

        std::uint32_t printed = 0;
        for (const auto& [from, to] : spans)
            for (std::uint32_t a = from; a < to; a += 4)
            {
                auto word = exe->Read32(a);
                if (!word) break;
                const Instruction insn = Decode(*word, a);
                std::printf("%08X  %08X  %s\n", a, *word, Disassemble(insn).c_str());
                if (!range.start && range.count && ++printed >= range.count) return CpuStatus::Ok;
            }
        return CpuStatus::Ok;
    }

    CpuStatus TranslateExecutable(const CpuTranslatorSettings& settings, Diagnostics& diag)
    {
        auto exe = Load(settings, diag);
        if (!exe) return CpuStatus::Failed;
        const auto& info = exe->Info();
        diag.Info("cpu: imagem de " + std::to_string(info.imageSize / 1024) + " KB em 0x" + Hex32(info.baseAddress) +
                  ", entrada em 0x" + Hex32(info.entryPoint));

        // decodifica todas as regioes de codigo e conta o que nao foi reconhecido
        std::uint64_t total = 0, invalid = 0, zero = 0;
        std::map<std::uint32_t, std::uint64_t> invalidByPrimary;
        std::map<Opcode, std::uint64_t> used;
        for (const auto& r : info.regions)
        {
            if (r.kind != loader::RegionKind::Code) continue;
            for (std::uint32_t a = r.start; a < r.start + r.size; a += 4)
            {
                const auto word = exe->Read32(a);
                if (!word) break;
                ++total;
                if (*word == 0) { ++zero; continue; }   // preenchimento entre funcoes
                const Instruction insn = Decode(*word, a);
                if (!insn.Valid())
                {
                    ++invalid;
                    ++invalidByPrimary[*word >> 26];
                    continue;
                }
                ++used[insn.opcode];
            }
        }
        diag.Info("cpu: " + std::to_string(total) + " palavras nas regioes de codigo: " + std::to_string(total - invalid - zero) +
                  " instrucoes reconhecidas (" + std::to_string(used.size()) + " tipos diferentes), " + std::to_string(zero) +
                  " zeros, " + std::to_string(invalid) + " nao reconhecidas");
        if (invalid)
        {
            std::string detail = "cpu: nao reconhecidas por opcode primario:";
            for (const auto& [po, n] : invalidByPrimary) detail += " " + std::to_string(po) + "=" + std::to_string(n);
            diag.Info(detail);
        }

        // funcoes e tabelas de desvio
        const ProgramAnalysis analysis = AnalyzeProgram(*exe, diag);

        // geracao de C
        CodeGenSettings cg;
        cg.outputDir = settings.outputDir;
        CodeGenStats stats;
        if (!GenerateCode(*exe, analysis, cg, stats, diag)) return CpuStatus::Failed;
        diag.Info("cpu: " + std::to_string(stats.instructions) + " instrucoes traduzidas em " + std::to_string(stats.files) +
                  " arquivos (" + std::to_string(stats.bytes / (1024 * 1024)) + " MB) em " + cg.outputDir.string());
        if (stats.dataWords)
            diag.Info("cpu: " + std::to_string(stats.dataWords) + " palavras de dados/preenchimento dentro de funcoes (nao sao instrucoes)");
        if (stats.unimplemented)
        {
            std::vector<std::pair<std::uint64_t, std::string>> top;
            for (const auto& [name, n] : stats.unimplementedByName) top.emplace_back(n, name);
            std::sort(top.rbegin(), top.rend());
            std::string detail = "cpu: " + std::to_string(stats.unimplemented) + " instrucoes ainda sem traducao (" +
                                 std::to_string(top.size()) + " tipos):";
            for (std::size_t i = 0; i < top.size() && i < 25; ++i) detail += " " + top[i].second + "=" + std::to_string(top[i].first);
            diag.Info(detail);
        }
        return CpuStatus::Ok;
    }
}
