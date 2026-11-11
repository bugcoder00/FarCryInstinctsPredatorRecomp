// Instrucao PowerPC decodificada (Xenon: PowerPC 64 bits + VMX + VMX128)
#pragma once

#include <array>
#include <cstdint>

namespace predator::cpu
{
    // Forma dos operandos (como extrai-los do opcode e em que ordem aparecem na desmontagem)
    enum class Layout : std::uint8_t
    {
        None,
        // desvios
        Branch,             // b target
        BranchCond,         // bc BO,BI,target
        BranchCondReg,      // bclr/bcctr BO,BI
        // inteiros (forma D)
        D_rD_rA_simm,       // addi rD,rA,SIMM
        D_rA_rS_uimm,       // ori rA,rS,UIMM
        D_crf_rA_simm,      // cmpi crfD,L,rA,SIMM
        D_crf_rA_uimm,      // cmpli crfD,L,rA,UIMM
        D_TO_rA_simm,       // twi TO,rA,SIMM
        Mem_rD,             // lwz rD,d(rA)
        Mem_fD,             // lfs fD,d(rA)
        Mem_rD_DS,          // ld rD,ds(rA)
        // rotacoes
        M_rA_rS_SH_MB_ME,   // rlwinm
        M_rA_rS_rB_MB_ME,   // rlwnm
        MD_rA_rS_SH_MB,     // rldicl/rldic/rldimi (e rldicr, onde o campo e ME)
        MDS_rA_rS_rB_MB,    // rldcl/rldcr
        // forma X / XO
        X_rD_rA_rB,
        X_rA_rS_rB,
        X_rA_rS,
        X_rA_rS_SH,         // srawi
        XS_rA_rS_SH,        // sradi (SH de 6 bits)
        X_rD_rA,            // neg, addze...
        X_crf_rA_rB,        // cmp crfD,L,rA,rB
        X_TO_rA_rB,
        X_rA_rB,            // dcbf, icbi...
        X_rD,               // mfcr, mfmsr
        X_rS_L,             // mtmsr/mtmsrd rS,L
        X_fD_rA_rB,         // lfsx
        X_vD_rA_rB,         // lvx
        X_rD_rA_NB,         // lswi
        X_sync,             // sync L
        XFX_mfspr,          // mfspr rD,SPR (e mftb)
        XFX_mtspr,          // mtspr SPR,rS
        XFX_mtcrf,          // mtcrf CRM,rS
        // ponto flutuante
        A_fD_fA_fB,
        A_fD_fA_fC,
        A_fD_fA_fC_fB,
        A_fD_fB,
        X_crf_fA_fB,
        X_fD,
        X_crbD,
        XFL_FM_fB,
        X_crf_imm,          // mtfsfi crfD,IMM
        X_crf_crf,          // mcrf/mcrfs crfD,crfS
        XL_crbD_crbA_crbB,
        // VMX
        VA_vD_vA_vB_vC,     // vperm/vsel
        VA_vD_vA_vC_vB,     // vmaddfp/vnmsubfp (ordem do manual)
        VA_vD_vA_vB_SH,     // vsldoi
        VX_vD_vA_vB,
        VX_vD_vB,
        VX_vD_vB_uimm,
        VX_vD_simm,
        VX_vD,
        VX_vB,
        VXR_vD_vA_vB,       // vcmp* (Rc no bit 10)
        // VMX128 (registradores v0-v127)
        V128_Mem,           // lvx128 vD,rA,rB
        V128_vD_vA_vB,
        V128_vD_vA_vD_vB,   // vmaddcfp128: vD = vA * vD + vB
        V128_vD_vA_vB_vD,   // vsel128: vD como seletor
        V128_vD_vA_vB_vC,   // vperm128 (vC de 3 bits)
        V128_vD_vB,
        V128_vD_vB_uimm,
        V128_vD_simm,       // vspltisw128
        V128_vD_vB_imm_z,   // vrlimi128 vD,vB,IMM,z
        V128_vpkd3d,        // vpkd3d128 vD,vB,tipo,mascara,deslocamento
        V128_vD_vB_perm,    // vpermwi128
        V128_vD_vA_vB_SH,   // vsldoi128
        V128R_vD_vA_vB,     // vcmp*128 (Rc no bit 6)
    };

    // Ajustes do mnemonico
    enum OpFlags : std::uint8_t
    {
        kFlagRc = 1 << 0,   // bit 0 grava CR0 ("." no fim)
        kFlagOE = 1 << 1,   // bit 10 grava XER[OV] ("o")
        kFlagLK = 1 << 2,   // bit 0 grava LR ("l")
        kFlagAA = 1 << 3,   // bit 1 = endereco absoluto ("a")
        kFlagRA0 = 1 << 4,  // (so na tabela) rA = 0 significa o valor 0, nao o registrador r0 (enderecamento "rA|0")
    };

    enum class Opcode : std::uint16_t
    {
        Invalid = 0,
#define PREDATOR_OPCODE(id, name, mask, value, layout, flags) id,
#include "cpu/OpcodeTable.inc"
#undef PREDATOR_OPCODE
        Count,
    };

    enum class OperandKind : std::uint8_t
    {
        None,
        Gpr,
        GprOrZero,  // rA em enderecamento "rA|0": o numero 0 quando value == 0
        Fpr,
        Vr,
        Cr,         // campo do CR (cr0-cr7)
        CrBit,      // bit do CR (0-31)
        Spr,
        Signed,     // imediato com sinal
        Unsigned,   // imediato sem sinal
        Target,     // endereco de desvio (ja resolvido)
        Memory,     // deslocamento(registrador base): value = deslocamento, reg = base (base 0 = endereco absoluto)
    };

    struct Operand
    {
        OperandKind kind = OperandKind::None;
        std::uint8_t reg = 0;      // base no caso de Memory
        std::int64_t value = 0;
    };

    struct Instruction
    {
        std::uint32_t address = 0;
        std::uint32_t raw = 0;
        Opcode opcode = Opcode::Invalid;
        Layout layout = Layout::None;
        std::uint8_t flags = 0;    // OpFlags efetivamente presentes nesta instrucao
        std::uint8_t operandCount = 0;
        std::array<Operand, 6> operands{};

        [[nodiscard]] bool Valid() const noexcept { return opcode != Opcode::Invalid; }
        [[nodiscard]] bool Has(OpFlags f) const noexcept { return (flags & f) != 0; }
    };

    struct OpcodeInfo
    {
        const char* name;
        std::uint32_t mask;
        std::uint32_t value;
        Layout layout;
        std::uint8_t flags;   // OpFlags que a instrucao pode ter
    };

    [[nodiscard]] const OpcodeInfo& GetOpcodeInfo(Opcode op) noexcept;
}
