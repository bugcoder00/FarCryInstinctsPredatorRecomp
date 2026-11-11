// Decodificador PowerPC (Xenon): palavra de 32 bits -> Instruction
#pragma once

#include "cpu/Instruction.h"

#include <cstdint>

namespace predator::cpu
{
    // Decodifica a instrucao 'raw' que esta no endereco 'address' (usado para resolver os alvos de desvio).
    // Palavras que nao correspondem a nenhuma instrucao conhecida devolvem opcode Invalid.
    [[nodiscard]] Instruction Decode(std::uint32_t raw, std::uint32_t address) noexcept;
}
