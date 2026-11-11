#include "gpu/XenosMicrocode.h"

#include <algorithm>

namespace predator::gpu
{
    namespace
    {
        // n bits de v a partir do bit lo (0 = menos significativo)
        constexpr std::uint32_t Field(std::uint64_t v, unsigned lo, unsigned n)
        {
            return static_cast<std::uint32_t>((v >> lo) & ((std::uint64_t{ 1 } << n) - 1));
        }

        constexpr std::int32_t SignedField(std::uint64_t v, unsigned lo, unsigned n)
        {
            const std::uint32_t x = Field(v, lo, n);
            const std::uint32_t sign = 1u << (n - 1);
            return static_cast<std::int32_t>(x ^ sign) - static_cast<std::int32_t>(sign);
        }

        constexpr bool Flag(std::uint64_t v, unsigned bit) { return ((v >> bit) & 1) != 0; }
    }

    bool CfInstruction::IsExec() const
    {
        switch (opcode)
        {
        case CfOpcode::Exec: case CfOpcode::ExecEnd:
        case CfOpcode::CondExec: case CfOpcode::CondExecEnd:
        case CfOpcode::CondPredExec: case CfOpcode::CondPredExecEnd:
        case CfOpcode::CondExecPredClean: case CfOpcode::CondExecPredCleanEnd:
            return true;
        default:
            return false;
        }
    }

    bool CfInstruction::EndsProgram() const
    {
        return opcode == CfOpcode::ExecEnd || opcode == CfOpcode::CondExecEnd || opcode == CfOpcode::CondPredExecEnd ||
               opcode == CfOpcode::CondExecPredCleanEnd;
    }

    // Instrucao de controle de fluxo (48 bits). O opcode fica sempre nos bits 44-47; as condicoes por constante
    // booleana usam os bits 34-41 (indice) e 42 (valor esperado).
    CfInstruction DecodeCf(std::uint64_t v)
    {
        CfInstruction cf;
        cf.opcode = static_cast<CfOpcode>(Field(v, 44, 4));
        switch (cf.opcode)
        {
        case CfOpcode::Exec: case CfOpcode::ExecEnd:
        case CfOpcode::CondExec: case CfOpcode::CondExecEnd:
        case CfOpcode::CondPredExec: case CfOpcode::CondPredExecEnd:
        case CfOpcode::CondExecPredClean: case CfOpcode::CondExecPredCleanEnd:
            cf.address = Field(v, 0, 12);
            cf.count = Field(v, 12, 3);
            cf.sequence = Field(v, 16, 12);
            cf.boolIndex = Field(v, 34, 8);
            cf.condition = Flag(v, 42);
            break;
        case CfOpcode::LoopStart:
            cf.address = Field(v, 0, 13);
            cf.loopId = Field(v, 16, 5);
            break;
        case CfOpcode::LoopEnd:
            cf.address = Field(v, 0, 13);
            cf.loopId = Field(v, 16, 5);
            cf.predicated = Flag(v, 21);
            cf.condition = Flag(v, 42);
            break;
        case CfOpcode::CondCall:
        case CfOpcode::CondJump:
            cf.address = Field(v, 0, 13);
            cf.unconditional = Flag(v, 13);
            cf.predicated = Flag(v, 14);
            cf.boolIndex = Field(v, 34, 8);
            cf.condition = Flag(v, 42);
            break;
        default:
            break;
        }
        return cf;
    }

    AluInstruction DecodeAlu(const std::uint32_t* w)
    {
        AluInstruction a;
        const std::uint32_t w0 = w[0], w1 = w[1], w2 = w[2];
        a.vectorDest = Field(w0, 0, 6);
        a.vectorDestRelative = Flag(w0, 6);
        a.absConstants = Flag(w0, 7);
        a.scalarDest = Field(w0, 8, 6);
        a.scalarDestRelative = Flag(w0, 14);
        a.exportData = Flag(w0, 15);
        a.vectorWriteMask = Field(w0, 16, 4);
        a.scalarWriteMask = Field(w0, 20, 4);
        a.vectorSaturate = Flag(w0, 24);
        a.scalarSaturate = Flag(w0, 25);
        a.scalarOpcode = static_cast<ScalarOpcode>(Field(w0, 26, 6));

        // swizzles e negacoes: src3 nos bits baixos, src1 nos altos
        for (unsigned i = 0; i < 3; ++i)
        {
            a.src[i].swizzle = Field(w1, 16 - 8 * i, 8);
            a.src[i].negate = Flag(w1, 26 - i);
            a.src[i].index = Field(w2, 16 - 8 * i, 8);
            a.src[i].isRegister = Flag(w2, 31 - i);
        }
        a.predicateCondition = Flag(w1, 27);
        a.predicated = Flag(w1, 28);
        a.relativeUsesA0 = Flag(w1, 29);
        a.constRelative[1] = Flag(w1, 30);
        a.constRelative[0] = Flag(w1, 31);
        a.vectorOpcode = static_cast<VectorOpcode>(Field(w2, 24, 5));
        return a;
    }

    FetchInstruction DecodeFetch(const std::uint32_t* w)
    {
        FetchInstruction f;
        const std::uint32_t w0 = w[0], w1 = w[1], w2 = w[2];
        f.opcode = static_cast<FetchOpcode>(Field(w0, 0, 5));
        f.srcRegister = Field(w0, 5, 6);
        f.srcRelative = Flag(w0, 11);
        f.dstRegister = Field(w0, 12, 6);
        f.dstRelative = Flag(w0, 18);
        f.dstSwizzle = Field(w1, 0, 12);
        f.predicated = Flag(w1, 31);
        f.predicateCondition = Flag(w2, 31);
        if (f.opcode == FetchOpcode::Vertex)
        {
            f.constIndex = Field(w0, 20, 5) * 3 + Field(w0, 25, 2);
            f.srcComponent = Field(w0, 30, 2);
            f.isSigned = Flag(w1, 12);
            f.isInteger = Flag(w1, 13);
            f.roundIndex = Flag(w1, 15);
            f.format = Field(w1, 16, 6);
            f.expAdjust = SignedField(w1, 24, 6);
            f.miniFetch = Flag(w1, 30);
            f.stride = Field(w2, 0, 8);
            f.offset = Field(w2, 8, 23);
        }
        else
        {
            f.constIndex = Field(w0, 20, 5);
            f.unnormalized = Flag(w0, 25);
            f.srcSwizzle = Field(w0, 26, 6);
            f.dimension = static_cast<TextureDimension>(Field(w2, 14, 2));
            f.offsetX = SignedField(w2, 16, 5);
            f.offsetY = SignedField(w2, 21, 5);
            f.offsetZ = SignedField(w2, 26, 5);
        }
        return f;
    }

    Microcode DecodeShader(std::span<const std::uint32_t> code)
    {
        Microcode m;
        m.code = code;
        // o controle de fluxo termina onde comeca o 1o bloco exec (enderecos em instrucoes de 3 dwords)
        std::size_t limit = code.size();
        for (std::size_t off = 0; off + 3 <= limit; off += 3)
        {
            const std::uint64_t a = code[off] | (std::uint64_t{ code[off + 1] & 0xFFFFu } << 32);
            const std::uint64_t b = (code[off + 1] >> 16) | (std::uint64_t{ code[off + 2] } << 16);
            for (const std::uint64_t bits : { a, b })
            {
                const CfInstruction cf = DecodeCf(bits);
                if (cf.IsExec() && cf.count) limit = std::min<std::size_t>(limit, std::size_t{ cf.address } * 3);
                m.controlFlow.push_back(cf);
            }
        }
        return m;
    }

    int VectorOperandCount(VectorOpcode op)
    {
        switch (op)
        {
        case VectorOpcode::Frac: case VectorOpcode::Trunc: case VectorOpcode::Floor: case VectorOpcode::Max4:
            return 1;
        case VectorOpcode::MulAdd: case VectorOpcode::CndEq: case VectorOpcode::CndGe: case VectorOpcode::CndGt:
        case VectorOpcode::Dot2Add:
            return 3;
        default:
            return 2;
        }
    }

    bool ScalarUsesSecondOperand(ScalarOpcode op)
    {
        switch (op)
        {
        case ScalarOpcode::Add: case ScalarOpcode::Mul: case ScalarOpcode::MulPrev2: case ScalarOpcode::Max:
        case ScalarOpcode::Min: case ScalarOpcode::MaxA: case ScalarOpcode::MaxAFloor: case ScalarOpcode::Sub:
            return true;
        default:
            return false;
        }
    }

    bool IsScalarConstOp(ScalarOpcode op)
    {
        return op >= ScalarOpcode::MulConst0 && op <= ScalarOpcode::SubConst1;
    }
}
