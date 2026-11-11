// Desmontagem em texto (sintaxe usual do PowerPC, com os mnemonicos simplificados mais comuns:
// li, lis, mr, nop, not, cmpwi/cmplwi/cmpw/cmplw (e versoes de 64 bits), blr, bctr, beq/bne/..., bdnz, mflr, mtctr...)
#pragma once

#include "cpu/Instruction.h"

#include <string>

namespace predator::cpu
{
    [[nodiscard]] std::string Disassemble(const Instruction& insn);
}
