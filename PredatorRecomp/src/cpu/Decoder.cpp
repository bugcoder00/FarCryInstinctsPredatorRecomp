#include "cpu/Decoder.h"

#include <array>
#include <vector>

namespace predator::cpu
{
    namespace
    {
        // ---------------------------------------------------------------- codificacao da tabela
        constexpr std::uint32_t OPC(std::uint32_t po) { return po << 26; }
        constexpr std::uint32_t XOP(std::uint32_t po, std::uint32_t xo) { return (po << 26) | (xo << 1); }
        constexpr std::uint32_t MDOP(std::uint32_t po, std::uint32_t xo) { return (po << 26) | (xo << 2); }
        constexpr std::uint32_t DSOP(std::uint32_t po, std::uint32_t xo) { return (po << 26) | xo; }
        constexpr std::uint32_t VOP(std::uint32_t po, std::uint32_t xo) { return (po << 26) | xo; }

        constexpr std::uint32_t M_OP = 0xFC000000u;
        constexpr std::uint32_t M_X = 0xFC0007FEu;    // XO de 10 bits (bits 1-10)
        constexpr std::uint32_t M_XO = 0xFC0003FEu;   // XO de 9 bits (bits 1-9), OE no bit 10 livre
        constexpr std::uint32_t M_A = 0xFC00003Eu;    // XO de 5 bits (bits 1-5)
        constexpr std::uint32_t M_MD = 0xFC00001Cu;   // XO de 3 bits (bits 2-4)
        constexpr std::uint32_t M_MDS = 0xFC00001Eu;  // XO de 4 bits (bits 1-4)
        constexpr std::uint32_t M_DS = 0xFC000003u;
        constexpr std::uint32_t M_VX = 0xFC0007FFu;
        constexpr std::uint32_t M_VA = 0xFC00003Fu;
        constexpr std::uint32_t M_VXR = 0xFC0003FFu;

        using enum Layout;

        constexpr OpcodeInfo kInvalidInfo{ "(invalido)", 0, 0, Layout::None, 0 };

        constexpr OpcodeInfo kTable[] = {
            kInvalidInfo,
#define PREDATOR_OPCODE(id, name, mask, value, layout, flags) OpcodeInfo{ name, mask, value, layout, static_cast<std::uint8_t>(flags) },
#include "cpu/OpcodeTable.inc"
#undef PREDATOR_OPCODE
        };
        static_assert(std::size(kTable) == static_cast<std::size_t>(Opcode::Count));

        // indice por opcode primario: lista das entradas da tabela, na ordem em que aparecem
        struct PrimaryIndex
        {
            std::array<std::vector<std::uint16_t>, 64> entries;
            PrimaryIndex()
            {
                for (std::uint16_t i = 1; i < std::size(kTable); ++i)
                    entries[kTable[i].value >> 26].push_back(i);
            }
        };
        const PrimaryIndex& Index()
        {
            static const PrimaryIndex index;
            return index;
        }

        // ---------------------------------------------------------------- campos (numeracao de bits: 0 = menos significativo)
        constexpr std::uint32_t Bits(std::uint32_t v, unsigned shift, unsigned count) { return (v >> shift) & ((1u << count) - 1u); }
        constexpr std::int64_t SignExtend(std::uint32_t v, unsigned bits)
        {
            const std::uint32_t m = 1u << (bits - 1);
            return static_cast<std::int64_t>(static_cast<std::int32_t>((v ^ m) - m));
        }

        constexpr std::uint8_t rD(std::uint32_t r) { return static_cast<std::uint8_t>(Bits(r, 21, 5)); }
        constexpr std::uint8_t rA(std::uint32_t r) { return static_cast<std::uint8_t>(Bits(r, 16, 5)); }
        constexpr std::uint8_t rB(std::uint32_t r) { return static_cast<std::uint8_t>(Bits(r, 11, 5)); }
        constexpr std::uint8_t rC(std::uint32_t r) { return static_cast<std::uint8_t>(Bits(r, 6, 5)); }
        constexpr std::uint8_t crfD(std::uint32_t r) { return static_cast<std::uint8_t>(Bits(r, 23, 3)); }
        constexpr std::uint8_t crfS(std::uint32_t r) { return static_cast<std::uint8_t>(Bits(r, 18, 3)); }

        // VMX128: registradores de 7 bits com pedacos espalhados pela palavra
        constexpr std::uint8_t VD128(std::uint32_t r) { return static_cast<std::uint8_t>(Bits(r, 21, 5) | (Bits(r, 2, 2) << 5)); }
        constexpr std::uint8_t VA128(std::uint32_t r)
        {
            return static_cast<std::uint8_t>(Bits(r, 16, 5) | (Bits(r, 5, 1) << 5) | (Bits(r, 10, 1) << 6));
        }
        constexpr std::uint8_t VB128(std::uint32_t r) { return static_cast<std::uint8_t>(Bits(r, 11, 5) | (Bits(r, 0, 2) << 5)); }

        class Builder
        {
        public:
            explicit Builder(Instruction& insn) : m_insn(insn) {}
            void Reg(OperandKind kind, std::uint32_t n) { Add({ kind, 0, static_cast<std::int64_t>(n) }); }
            void Gpr(std::uint32_t n) { Reg(OperandKind::Gpr, n); }
            void GprOrZero(std::uint32_t n) { Reg(OperandKind::GprOrZero, n); }
            void Fpr(std::uint32_t n) { Reg(OperandKind::Fpr, n); }
            void Vr(std::uint32_t n) { Reg(OperandKind::Vr, n); }
            void Cr(std::uint32_t n) { Reg(OperandKind::Cr, n); }
            void CrBit(std::uint32_t n) { Reg(OperandKind::CrBit, n); }
            void Spr(std::uint32_t n) { Reg(OperandKind::Spr, n); }
            void Signed(std::int64_t v) { Add({ OperandKind::Signed, 0, v }); }
            void Unsigned(std::uint32_t v) { Add({ OperandKind::Unsigned, 0, static_cast<std::int64_t>(v) }); }
            void Target(std::uint32_t v) { Add({ OperandKind::Target, 0, static_cast<std::int64_t>(v) }); }
            void Mem(std::int64_t disp, std::uint32_t base) { Add({ OperandKind::Memory, static_cast<std::uint8_t>(base), disp }); }

        private:
            void Add(const Operand& op)
            {
                if (m_insn.operandCount < m_insn.operands.size()) m_insn.operands[m_insn.operandCount++] = op;
            }
            Instruction& m_insn;
        };

        void ExtractOperands(Instruction& insn, std::uint8_t tableFlags)
        {
            const std::uint32_t r = insn.raw;
            Builder b(insn);
            // rA de enderecamento: "rA|0" nas instrucoes marcadas com kFlagRA0
            const bool ra0 = (tableFlags & kFlagRA0) != 0;
            auto RA = [&](std::uint32_t n) { if (ra0) b.GprOrZero(n); else b.Gpr(n); };
            switch (insn.layout)
            {
            case Layout::None:
            case Layout::X_sync:
                break;

            case Branch:
            {
                const std::int64_t li = SignExtend(r & 0x03FFFFFCu, 26);
                const std::uint32_t base = insn.Has(kFlagAA) ? 0u : insn.address;
                b.Target(static_cast<std::uint32_t>(base + static_cast<std::uint32_t>(li)));
                break;
            }
            case BranchCond:
            {
                const std::int64_t bd = SignExtend(r & 0xFFFCu, 16);
                const std::uint32_t base = insn.Has(kFlagAA) ? 0u : insn.address;
                b.Unsigned(rD(r));   // BO
                b.Unsigned(rA(r));   // BI
                b.Target(static_cast<std::uint32_t>(base + static_cast<std::uint32_t>(bd)));
                break;
            }
            case BranchCondReg:
                b.Unsigned(rD(r));
                b.Unsigned(rA(r));
                break;

            case D_rD_rA_simm: b.Gpr(rD(r)); RA(rA(r)); b.Signed(SignExtend(r & 0xFFFF, 16)); break;
            case D_rA_rS_uimm: b.Gpr(rA(r)); b.Gpr(rD(r)); b.Unsigned(r & 0xFFFF); break;
            case D_crf_rA_simm: b.Cr(crfD(r)); b.Unsigned(Bits(r, 21, 1)); b.Gpr(rA(r)); b.Signed(SignExtend(r & 0xFFFF, 16)); break;
            case D_crf_rA_uimm: b.Cr(crfD(r)); b.Unsigned(Bits(r, 21, 1)); b.Gpr(rA(r)); b.Unsigned(r & 0xFFFF); break;
            case D_TO_rA_simm: b.Unsigned(rD(r)); b.Gpr(rA(r)); b.Signed(SignExtend(r & 0xFFFF, 16)); break;
            case Mem_rD: b.Gpr(rD(r)); b.Mem(SignExtend(r & 0xFFFF, 16), rA(r)); break;
            case Mem_fD: b.Fpr(rD(r)); b.Mem(SignExtend(r & 0xFFFF, 16), rA(r)); break;
            case Mem_rD_DS: b.Gpr(rD(r)); b.Mem(SignExtend(r & 0xFFFC, 16), rA(r)); break;

            case M_rA_rS_SH_MB_ME: b.Gpr(rA(r)); b.Gpr(rD(r)); b.Unsigned(rB(r)); b.Unsigned(rC(r)); b.Unsigned(Bits(r, 1, 5)); break;
            case M_rA_rS_rB_MB_ME: b.Gpr(rA(r)); b.Gpr(rD(r)); b.Gpr(rB(r)); b.Unsigned(rC(r)); b.Unsigned(Bits(r, 1, 5)); break;
            case MD_rA_rS_SH_MB:
                b.Gpr(rA(r));
                b.Gpr(rD(r));
                b.Unsigned(rB(r) | (Bits(r, 1, 1) << 5));      // SH: 5 bits + bit alto separado
                b.Unsigned(rC(r) | (Bits(r, 5, 1) << 5));      // MB/ME: 5 bits + bit alto separado
                break;
            case MDS_rA_rS_rB_MB:
                b.Gpr(rA(r));
                b.Gpr(rD(r));
                b.Gpr(rB(r));
                b.Unsigned(rC(r) | (Bits(r, 5, 1) << 5));
                break;

            case X_rD_rA_rB: b.Gpr(rD(r)); RA(rA(r)); b.Gpr(rB(r)); break;
            case X_rA_rS_rB: b.Gpr(rA(r)); b.Gpr(rD(r)); b.Gpr(rB(r)); break;
            case X_rA_rS: b.Gpr(rA(r)); b.Gpr(rD(r)); break;
            case X_rA_rS_SH: b.Gpr(rA(r)); b.Gpr(rD(r)); b.Unsigned(rB(r)); break;
            case XS_rA_rS_SH: b.Gpr(rA(r)); b.Gpr(rD(r)); b.Unsigned(rB(r) | (Bits(r, 1, 1) << 5)); break;
            case X_rD_rA: b.Gpr(rD(r)); b.Gpr(rA(r)); break;
            case X_crf_rA_rB: b.Cr(crfD(r)); b.Unsigned(Bits(r, 21, 1)); b.Gpr(rA(r)); b.Gpr(rB(r)); break;
            case X_TO_rA_rB: b.Unsigned(rD(r)); b.Gpr(rA(r)); b.Gpr(rB(r)); break;
            case X_rA_rB: RA(rA(r)); b.Gpr(rB(r)); break;
            case X_rD: b.Gpr(rD(r)); break;
            case X_rS_L: b.Gpr(rD(r)); b.Unsigned(Bits(r, 16, 1)); break;
            case X_fD_rA_rB: b.Fpr(rD(r)); RA(rA(r)); b.Gpr(rB(r)); break;
            case X_vD_rA_rB: b.Vr(rD(r)); RA(rA(r)); b.Gpr(rB(r)); break;
            case X_rD_rA_NB: b.Gpr(rD(r)); RA(rA(r)); b.Unsigned(rB(r)); break;
            case XFX_mfspr: b.Gpr(rD(r)); b.Spr(rA(r) | (static_cast<std::uint32_t>(rB(r)) << 5)); break;
            case XFX_mtspr: b.Spr(rA(r) | (static_cast<std::uint32_t>(rB(r)) << 5)); b.Gpr(rD(r)); break;
            case XFX_mtcrf: b.Unsigned(Bits(r, 12, 8)); b.Gpr(rD(r)); break;

            case A_fD_fA_fB: b.Fpr(rD(r)); b.Fpr(rA(r)); b.Fpr(rB(r)); break;
            case A_fD_fA_fC: b.Fpr(rD(r)); b.Fpr(rA(r)); b.Fpr(rC(r)); break;
            case A_fD_fA_fC_fB: b.Fpr(rD(r)); b.Fpr(rA(r)); b.Fpr(rC(r)); b.Fpr(rB(r)); break;
            case A_fD_fB: b.Fpr(rD(r)); b.Fpr(rB(r)); break;
            case X_crf_fA_fB: b.Cr(crfD(r)); b.Fpr(rA(r)); b.Fpr(rB(r)); break;
            case X_fD: b.Fpr(rD(r)); break;
            case X_crbD: b.CrBit(rD(r)); break;
            case XFL_FM_fB: b.Unsigned(Bits(r, 17, 8)); b.Fpr(rB(r)); break;
            case X_crf_imm: b.Cr(crfD(r)); b.Unsigned(Bits(r, 12, 4)); break;
            case X_crf_crf: b.Cr(crfD(r)); b.Cr(crfS(r)); break;
            case XL_crbD_crbA_crbB: b.CrBit(rD(r)); b.CrBit(rA(r)); b.CrBit(rB(r)); break;

            case VA_vD_vA_vB_vC: b.Vr(rD(r)); b.Vr(rA(r)); b.Vr(rB(r)); b.Vr(rC(r)); break;
            case VA_vD_vA_vC_vB: b.Vr(rD(r)); b.Vr(rA(r)); b.Vr(rC(r)); b.Vr(rB(r)); break;
            case VA_vD_vA_vB_SH: b.Vr(rD(r)); b.Vr(rA(r)); b.Vr(rB(r)); b.Unsigned(Bits(r, 6, 4)); break;
            case VX_vD_vA_vB: b.Vr(rD(r)); b.Vr(rA(r)); b.Vr(rB(r)); break;
            case VX_vD_vB: b.Vr(rD(r)); b.Vr(rB(r)); break;
            case VX_vD_vB_uimm: b.Vr(rD(r)); b.Vr(rB(r)); b.Unsigned(rA(r)); break;
            case VX_vD_simm: b.Vr(rD(r)); b.Signed(SignExtend(rA(r), 5)); break;
            case VX_vD: b.Vr(rD(r)); break;
            case VX_vB: b.Vr(rB(r)); break;
            case VXR_vD_vA_vB: b.Vr(rD(r)); b.Vr(rA(r)); b.Vr(rB(r)); break;

            case V128_Mem: b.Vr(VD128(r)); RA(rA(r)); b.Gpr(rB(r)); break;
            case V128_vD_vA_vB:
            case V128R_vD_vA_vB: b.Vr(VD128(r)); b.Vr(VA128(r)); b.Vr(VB128(r)); break;
            case V128_vD_vA_vB_vC: b.Vr(VD128(r)); b.Vr(VA128(r)); b.Vr(VB128(r)); b.Vr(Bits(r, 6, 3)); break;
            case V128_vD_vA_vD_vB: b.Vr(VD128(r)); b.Vr(VA128(r)); b.Vr(VD128(r)); b.Vr(VB128(r)); break;
            case V128_vD_vA_vB_vD: b.Vr(VD128(r)); b.Vr(VA128(r)); b.Vr(VB128(r)); b.Vr(VD128(r)); break;
            case V128_vpkd3d: b.Vr(VD128(r)); b.Vr(VB128(r)); b.Unsigned(Bits(r, 18, 3)); b.Unsigned(Bits(r, 16, 2)); b.Unsigned(Bits(r, 6, 2)); break;
            case V128_vD_vB: b.Vr(VD128(r)); b.Vr(VB128(r)); break;
            case V128_vD_vB_uimm: b.Vr(VD128(r)); b.Vr(VB128(r)); b.Unsigned(rA(r)); break;
            case V128_vD_simm: b.Vr(VD128(r)); b.Signed(SignExtend(rA(r), 5)); break;
            case V128_vD_vB_imm_z: b.Vr(VD128(r)); b.Vr(VB128(r)); b.Unsigned(rA(r)); b.Unsigned(Bits(r, 6, 2)); break;
            case V128_vD_vB_perm: b.Vr(VD128(r)); b.Vr(VB128(r)); b.Unsigned(rA(r) | (Bits(r, 6, 3) << 5)); break;
            case V128_vD_vA_vB_SH: b.Vr(VD128(r)); b.Vr(VA128(r)); b.Vr(VB128(r)); b.Unsigned(Bits(r, 6, 4)); break;
            }
        }

        // Quais flags opcionais estao ligadas nesta instrucao
        std::uint8_t ActiveFlags(const OpcodeInfo& info, std::uint32_t r)
        {
            std::uint8_t f = 0;
            if ((info.flags & kFlagRc) && (r & 1)) f |= kFlagRc;
            if ((info.flags & kFlagOE) && (r & 0x400)) f |= kFlagOE;
            if ((info.flags & kFlagLK) && (r & 1)) f |= kFlagLK;
            if ((info.flags & kFlagAA) && (r & 2)) f |= kFlagAA;   // kFlagRA0 fica so na tabela
            // registro no bit 10 (vcmp*) ou no bit 6 (vcmp*128)
            if (info.layout == VXR_vD_vA_vB && (r & 0x400)) f |= kFlagRc;
            if (info.layout == V128R_vD_vA_vB && (r & 0x40)) f |= kFlagRc;
            return f;
        }
    }

    const OpcodeInfo& GetOpcodeInfo(Opcode op) noexcept
    {
        const auto i = static_cast<std::size_t>(op);
        return i < std::size(kTable) ? kTable[i] : kTable[0];
    }

    Instruction Decode(std::uint32_t raw, std::uint32_t address) noexcept
    {
        Instruction insn;
        insn.raw = raw;
        insn.address = address;
        for (std::uint16_t i : Index().entries[raw >> 26])
        {
            const OpcodeInfo& info = kTable[i];
            if ((raw & info.mask) != info.value) continue;
            insn.opcode = static_cast<Opcode>(i);
            insn.layout = info.layout;
            insn.flags = ActiveFlags(info, raw);
            ExtractOperands(insn, info.flags);
            break;
        }
        return insn;
    }
}
