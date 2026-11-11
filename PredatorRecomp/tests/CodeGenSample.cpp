// Gera C a partir de funcoes PowerPC montadas a mao; o resultado e compilado e executado por tests/CodeGenRun.c
// Uso: predator_codegen_sample <pasta de saida>
#include "cpu/Analysis.h"
#include "cpu/CodeGen.h"
#include "loader/Executable.h"

#include <cstdio>
#include <fstream>
#include <vector>

using namespace predator;

namespace
{
    constexpr std::uint32_t kBase = 0x10000;

    struct Sample
    {
        std::uint32_t address;
        std::vector<std::uint32_t> words;
    };

    // as mesmas funcoes sao descritas em CodeGenRun.c (o que cada uma deve devolver)
    const std::vector<Sample> kSamples = {
        // 0x10000: soma 1..r3 com o CTR
        { 0x10000, { 0x7C6903A6,     // mtctr r3
                     0x38800000,     // li r4,0
                     0x7C841A14,     // add r4,r4,r3
                     0x3863FFFF,     // addi r3,r3,-1
                     0x4200FFF8,     // bdnz -8
                     0x7C832378,     // mr r3,r4
                     0x4E800020 } }, // blr
        // 0x10040: chama a soma, desloca 4 bits e compara com 240
        { 0x10040, { 0x7D8802A6,     // mflr r12
                     0x9181FFF8,     // stw r12,-8(r1)
                     0x9421FFE0,     // stwu r1,-32(r1)
                     0x4BFFFFB5,     // bl 0x10000
                     0x38210020,     // addi r1,r1,32
                     0x8181FFF8,     // lwz r12,-8(r1)
                     0x7D8803A6,     // mtlr r12
                     0x54632036,     // rlwinm r3,r3,4,0,27
                     0x2C0300F0,     // cmpwi r3,240
                     0x4082000C,     // bne +12
                     0x38600001,     // li r3,1
                     0x4E800020,     // blr
                     0x38600000,     // li r3,0
                     0x4E800020 } }, // blr
        // 0x10100: vetores (3 + 4 em cada palavra), grava na memoria e le a palavra 1
        { 0x10100, { 0x1023038C,     // vspltisw v1,3
                     0x1044038C,     // vspltisw v2,4
                     0x10611080,     // vadduwm v3,v1,v2
                     0x7C6021CE,     // stvx v3,0,r4
                     0x80640004,     // lwz r3,4(r4)
                     0x4E800020 } }, // blr
        // 0x10140: ponto flutuante: [r4+4] = [r4] + [r4]
        { 0x10140, { 0xC0240000,     // lfs f1,0(r4)
                     0xEC21082A,     // fadds f1,f1,f1
                     0xD0240004,     // stfs f1,4(r4)
                     0x4E800020 } }, // blr
        // 0x10180: modo 32 bits: addic. compara so os 32 bits de baixo (r3 = 0xFFFFFFFE vindo de um lwz)
        { 0x10180, { 0x38630006,     // addi r3,r3,6      -> 0x1_00000004
                     0x3463FFFB,     // addic. r3,r3,-5   -> 0xFFFFFFFF: negativo em 32 bits
                     0x4180000C,     // blt +12
                     0x38600000,     // li r3,0
                     0x4E800020,     // blr
                     0x38600001,     // li r3,1
                     0x4E800020 } }, // blr
        // 0x101C0: le r4 (o chamador precisa ter gravado a copia local no contexto) e muda r5
        { 0x101C0, { 0x1C640002,     // mulli r3,r4,2
                     0x38A00014,     // li r5,20
                     0x4E800020 } }, // blr
        // 0x10200: r4 = 7, r5 = 10, chama 0x101C0 e soma r3 + r5 (o r5 tem de ser relido depois da chamada) -> 34
        { 0x10200, { 0x7D8802A6,     // mflr r12
                     0x38800007,     // li r4,7
                     0x38A0000A,     // li r5,10
                     0x4BFFFFB5,     // bl 0x101C0
                     0x7D8803A6,     // mtlr r12
                     0x7C632A14,     // add r3,r3,r5
                     0x4E800020 } }, // blr
    };
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "uso: predator_codegen_sample <pasta>\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];

    std::vector<std::uint8_t> image(0x300, 0);
    cpu::ProgramAnalysis analysis;
    for (const auto& s : kSamples)
    {
        for (std::size_t i = 0; i < s.words.size(); ++i)
        {
            const std::size_t at = s.address - kBase + i * 4;
            const std::uint32_t w = s.words[i];
            image[at] = static_cast<std::uint8_t>(w >> 24);
            image[at + 1] = static_cast<std::uint8_t>(w >> 16);
            image[at + 2] = static_cast<std::uint8_t>(w >> 8);
            image[at + 3] = static_cast<std::uint8_t>(w);
        }
        cpu::Function f;
        f.start = f.bodyStart = s.address;
        f.end = s.address + static_cast<std::uint32_t>(s.words.size() * 4);
        analysis.functions.push_back(f);
    }
    const auto exe = loader::Executable::FromImage(kBase, std::move(image));

    std::string text = "/* gerado por predator_codegen_sample */\n#include \"predator_cpu.h\"\n\n";
    for (const auto& f : analysis.functions)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "void sub_%08X(PredatorCpuContext* ctx, uint8_t* base);\n", f.start);   // definida via PREDATOR_FUNCTION
        text += buf;
    }
    text += "\n";
    cpu::CodeGenStats stats;
    for (const auto& f : analysis.functions) text += cpu::GenerateFunction(exe, analysis, f, stats);

    std::filesystem::create_directories(dir);
    std::ofstream(dir / "predator_cpu.h", std::ios::binary) << cpu::RuntimeHeader();
    std::ofstream(dir / "codegen_sample.c", std::ios::binary) << text;
    if (stats.unimplemented)
    {
        std::fprintf(stderr, "%llu instrucoes sem traducao\n", static_cast<unsigned long long>(stats.unimplemented));
        return 1;
    }
    return 0;
}
