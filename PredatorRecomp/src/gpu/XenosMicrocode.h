// Leitura do microcodigo de shader da GPU do console (Xenos).
//
// Um shader e uma lista de dwords (ja na ordem do host). Ele comeca com as instrucoes de controle de fluxo
// (48 bits cada, duas a cada 3 dwords) e depois vem a area de instrucoes (96 bits = 3 dwords cada), que podem ser
// de ALU (vetor + escalar em paralelo) ou de leitura (vertice ou textura). Os blocos "exec" do controle de fluxo
// apontam para trechos dessa area.
//
// Este arquivo so separa os campos; o significado de cada um e usado pelo gerador de HLSL (XenosHlsl).
// Nao depende de nada do resto do projeto para poder ser compilado tambem dentro do runtime do jogo.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace predator::gpu
{
    // ---------------------------------------------------------------- controle de fluxo
    enum class CfOpcode : std::uint8_t
    {
        Nop = 0,
        Exec = 1,
        ExecEnd = 2,
        CondExec = 3,             // bloco condicionado a uma constante booleana
        CondExecEnd = 4,
        CondPredExec = 5,         // bloco condicionado ao predicado p0
        CondPredExecEnd = 6,
        LoopStart = 7,
        LoopEnd = 8,
        CondCall = 9,
        Return = 10,
        CondJump = 11,
        Alloc = 12,
        CondExecPredClean = 13,   // como CondExec (constante booleana)
        CondExecPredCleanEnd = 14,
        MarkVsFetchDone = 15,
    };

    struct CfInstruction
    {
        CfOpcode opcode = CfOpcode::Nop;
        std::uint32_t address = 0;    // exec: 1a instrucao do bloco; saltos/lacos: indice de controle de fluxo
        std::uint32_t count = 0;      // exec: numero de instrucoes do bloco
        std::uint32_t sequence = 0;   // exec: 2 bits por instrucao (bit 0 = leitura, bit 1 = serializar)
        std::uint32_t boolIndex = 0;  // constante booleana testada (CondExec*, CondJump, CondCall)
        bool condition = false;       // valor esperado da condicao
        bool unconditional = false;   // CondJump / CondCall sem condicao
        bool predicated = false;      // CondJump / CondCall testam p0; LoopEnd: "break" se p0 == condition
        std::uint32_t loopId = 0;     // LoopStart / LoopEnd: constante de laco usada

        [[nodiscard]] bool IsExec() const;
        [[nodiscard]] bool EndsProgram() const;   // ExecEnd, CondExecEnd, CondPredExecEnd, CondExecPredCleanEnd
    };

    // ---------------------------------------------------------------- leituras (vertice / textura)
    enum class FetchOpcode : std::uint8_t
    {
        Vertex = 0,
        Texture = 1,
        GetBorderColorFraction = 16,
        GetComputedLod = 17,
        GetGradients = 18,
        GetWeights = 19,
        SetLod = 24,
        SetGradientsHorizontal = 25,
        SetGradientsVertical = 26,
    };

    enum class TextureDimension : std::uint8_t
    {
        D1 = 0,
        D2 = 1,
        D3 = 2,
        Cube = 3,
    };

    struct FetchInstruction
    {
        FetchOpcode opcode = FetchOpcode::Vertex;
        std::uint32_t srcRegister = 0;
        bool srcRelative = false;
        std::uint32_t dstRegister = 0;
        bool dstRelative = false;
        std::uint32_t dstSwizzle = 0;   // 3 bits por componente: 0-3 = x/y/z/w lido, 4 = 0.0, 5 = 1.0, 7 = nao grava
        bool predicated = false;
        bool predicateCondition = false;

        // leitura de vertice
        std::uint32_t constIndex = 0;       // leitura de vertice: indice*3 + seletor = slot (0-95); textura: slot (0-31)
        std::uint32_t srcComponent = 0;     // componente de srcRegister com o indice do vertice
        bool isSigned = false;              // format_comp_all
        bool isInteger = false;             // num_format_all (0 = normalizado)
        bool roundIndex = false;            // is_index_rounded
        std::uint32_t format = 0;
        std::int32_t expAdjust = 0;
        bool miniFetch = false;             // reaproveita slot/stride/registrador da ultima leitura completa
        std::uint32_t stride = 0;           // em dwords
        std::uint32_t offset = 0;           // em dwords

        // leitura de textura
        std::uint32_t srcSwizzle = 0;       // 2 bits por componente (x, y, z), absolutos
        bool unnormalized = false;          // tx_coord_denorm: coordenadas em texels
        TextureDimension dimension = TextureDimension::D2;
        std::int32_t offsetX = 0, offsetY = 0, offsetZ = 0;   // em meios texels
    };

    // ---------------------------------------------------------------- ALU
    enum class VectorOpcode : std::uint8_t
    {
        Add = 0, Mul = 1, Max = 2, Min = 3,
        SetEq = 4, SetGt = 5, SetGe = 6, SetNe = 7,
        Frac = 8, Trunc = 9, Floor = 10,
        MulAdd = 11,
        CndEq = 12, CndGe = 13, CndGt = 14,
        Dot4 = 15, Dot3 = 16, Dot2Add = 17,
        Cube = 18, Max4 = 19,
        PredSetEqPush = 20, PredSetNePush = 21, PredSetGtPush = 22, PredSetGePush = 23,
        KillEq = 24, KillGt = 25, KillGe = 26, KillNe = 27,
        Dst = 28,
        MaxA = 29,   // max + grava o registrador de endereco a0
    };

    enum class ScalarOpcode : std::uint8_t
    {
        Add = 0, AddPrev = 1, Mul = 2, MulPrev = 3, MulPrev2 = 4,
        Max = 5, Min = 6,
        SetEq = 7, SetGt = 8, SetGe = 9, SetNe = 10,
        Frac = 11, Trunc = 12, Floor = 13,
        Exp = 14, LogClamp = 15, Log = 16,
        RcpClamp = 17, RcpFlush = 18, Rcp = 19,
        RsqClamp = 20, RsqFlush = 21, Rsq = 22,
        MaxA = 23, MaxAFloor = 24,
        Sub = 25, SubPrev = 26,
        PredSetEq = 27, PredSetNe = 28, PredSetGt = 29, PredSetGe = 30,
        PredSetInv = 31, PredSetPop = 32, PredSetClear = 33, PredSetRestore = 34,
        KillEq = 35, KillGt = 36, KillGe = 37, KillNe = 38, KillOne = 39,
        Sqrt = 40,
        MulConst0 = 42, MulConst1 = 43, AddConst0 = 44, AddConst1 = 45, SubConst0 = 46, SubConst1 = 47,
        Sin = 48, Cos = 49,
        RetainPrev = 50,
    };

    struct AluSource
    {
        bool isRegister = false;    // false = constante
        std::uint32_t index = 0;    // registrador: 0-63 (bit 6 = relativo a aL, bit 7 = valor absoluto); constante: 0-255
        std::uint32_t swizzle = 0;  // 8 bits, relativos (componente i le ((s >> 2i) + i) & 3)
        bool negate = false;
    };

    struct AluInstruction
    {
        VectorOpcode vectorOpcode = VectorOpcode::Add;
        ScalarOpcode scalarOpcode = ScalarOpcode::RetainPrev;
        std::uint32_t vectorDest = 0;
        bool vectorDestRelative = false;
        std::uint32_t scalarDest = 0;
        bool scalarDestRelative = false;
        std::uint32_t vectorWriteMask = 0;
        std::uint32_t scalarWriteMask = 0;
        bool vectorSaturate = false;
        bool scalarSaturate = false;
        bool exportData = false;          // os destinos sao saidas (posicao, interpoladores, cores, profundidade)
        bool absConstants = false;        // valor absoluto em todas as constantes lidas
        AluSource src[3];                 // src1, src2, src3
        bool predicated = false;
        bool predicateCondition = false;
        bool relativeUsesA0 = false;      // enderecamento relativo das constantes: a0 (senao aL)
        bool constRelative[2] = { false, false };   // 1a e 2a constante lidas pela instrucao
    };

    // ---------------------------------------------------------------- shader inteiro
    struct Microcode
    {
        std::vector<CfInstruction> controlFlow;   // ate a 1a instrucao de ALU/leitura
        std::span<const std::uint32_t> code;      // o shader inteiro (para ler os blocos exec)
    };

    [[nodiscard]] CfInstruction DecodeCf(std::uint64_t bits48);
    [[nodiscard]] AluInstruction DecodeAlu(const std::uint32_t* words);
    [[nodiscard]] FetchInstruction DecodeFetch(const std::uint32_t* words);
    [[nodiscard]] Microcode DecodeShader(std::span<const std::uint32_t> code);

    // numero de operandos que a operacao de vetor le (1 a 3)
    [[nodiscard]] int VectorOperandCount(VectorOpcode op);
    // a operacao escalar le o 2o operando (componente "b") de src3?
    [[nodiscard]] bool ScalarUsesSecondOperand(ScalarOpcode op);
    // operacoes escalares com uma constante e um registrador (MUL/ADD/SUB_CONST)
    [[nodiscard]] bool IsScalarConstOp(ScalarOpcode op);
}
