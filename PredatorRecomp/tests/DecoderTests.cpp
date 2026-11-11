// Testes do decodificador: palavras montadas a mao a partir da especificacao -> texto esperado
#include "cpu/Decoder.h"
#include "cpu/Disassembler.h"

#include <cstdio>
#include <string>

namespace
{
    int g_failures = 0;

    void Expect(std::uint32_t raw, std::uint32_t address, const char* expected)
    {
        const auto insn = predator::cpu::Decode(raw, address);
        const std::string got = predator::cpu::Disassemble(insn);
        if (got != expected)
        {
            std::fprintf(stderr, "%08X @ %08X: esperado \"%s\", obtido \"%s\"\n", raw, address, expected, got.c_str());
            ++g_failures;
        }
    }
}

int main()
{
    constexpr std::uint32_t at = 0x82000000;

    // sistema e desvios
    Expect(0x7C0802A6, at, "mflr r0");
    Expect(0x7C0803A6, at, "mtlr r0");
    Expect(0x7D2903A6, at, "mtctr r9");
    Expect(0x4E800020, at, "blr");
    Expect(0x4E800420, at, "bctr");
    Expect(0x4E800421, at, "bctrl");
    Expect(0x4BFFFFFC, at + 0x10, "b 0x8200000c");
    Expect(0x4BFFFF01, at + 0x100, "bl 0x82000000");
    Expect(0x4182000C, at, "beq 0x8200000c");
    Expect(0x419A000C, at, "beq cr6,0x8200000c");
    Expect(0x409A000C, at, "bne cr6,0x8200000c");
    Expect(0x4200FFF8, at + 0x20, "bdnz 0x82000018");
    Expect(0x4D820020, at, "beqlr");

    // inteiros
    Expect(0x60000000, at, "nop");
    Expect(0x38600001, at, "li r3,1");
    Expect(0x3C60FFFF, at, "lis r3,-1");
    Expect(0x38630010, at, "addi r3,r3,16");
    Expect(0x7C641B78, at, "mr r4,r3");
    Expect(0x2C030000, at, "cmpwi r3,0");
    Expect(0x2F830000, at, "cmpwi cr7,r3,0");
    Expect(0x2B030004, at, "cmplwi cr6,r3,4");
    Expect(0x7C642A14, at, "add r3,r4,r5");
    Expect(0x7C642E15, at, "addo. r3,r4,r5");
    Expect(0x5463103A, at, "rlwinm r3,r3,2,0,29");
    Expect(0x5463063E, at, "clrlwi r3,r3,24");
    Expect(0x7C830E76, at, "sradi r3,r4,33");

    // memoria
    Expect(0x9421FF80, at, "stwu r1,-128(r1)");
    Expect(0x80610010, at, "lwz r3,16(r1)");
    Expect(0xFBE1FFF8, at, "std r31,-8(r1)");

    // ponto flutuante
    Expect(0xFC22182A, at, "fadd f1,f2,f3");

    // VMX e VMX128
    Expect(0x1022180A, at, "vaddfp v1,v2,v3");
    Expect(0x100320C3, at, "lvx128 v0,r3,r4");
    Expect(0x100320CF, at, "lvx128 v96,r3,r4");

    // palavra que nao e instrucao
    Expect(0x00000000, at, ".long 0x0");

    // rA = 0 em enderecamento "rA|0" e o numero 0, nao o registrador r0
    Expect(0x7C60202E, at, "lwzx r3,0,r4");
    Expect(0x806007D0, at, "lwz r3,2000(0)");
    Expect(0x7C63202E, at, "lwzx r3,r3,r4");

    // dicas de previsao e armadilhas
    Expect(0x41FA000C, at, "beq+ cr6,0x8200000c");
    Expect(0x419B000C, at, "bso cr6,0x8200000c");
    Expect(0x0CC30000, at, "twllei r3,0");

    if (g_failures) std::fprintf(stderr, "%d caso(s) falharam\n", g_failures);
    else std::puts("todos os testes do decodificador passaram");
    return g_failures ? 1 : 0;
}
