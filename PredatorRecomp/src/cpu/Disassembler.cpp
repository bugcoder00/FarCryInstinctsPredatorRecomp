#include "cpu/Disassembler.h"

#include <cstdio>
#include <initializer_list>
#include <string_view>

namespace predator::cpu
{
    namespace
    {
        // absoluteZero: instrucao com enderecamento "rA|0" (base 0 = o numero 0)
        std::string Format(const Operand& op, bool absoluteZero)
        {
            char buf[48];
            const auto v = static_cast<long long>(op.value);
            switch (op.kind)
            {
            case OperandKind::Gpr: std::snprintf(buf, sizeof(buf), "r%lld", v); break;
            case OperandKind::GprOrZero:
                if (v == 0) std::snprintf(buf, sizeof(buf), "0");
                else std::snprintf(buf, sizeof(buf), "r%lld", v);
                break;
            case OperandKind::Fpr: std::snprintf(buf, sizeof(buf), "f%lld", v); break;
            case OperandKind::Vr: std::snprintf(buf, sizeof(buf), "v%lld", v); break;
            case OperandKind::Cr: std::snprintf(buf, sizeof(buf), "cr%lld", v); break;
            case OperandKind::CrBit:
            case OperandKind::Spr:
            case OperandKind::Signed:
            case OperandKind::Unsigned: std::snprintf(buf, sizeof(buf), "%lld", v); break;
            case OperandKind::Target: std::snprintf(buf, sizeof(buf), "0x%08llx", static_cast<unsigned long long>(op.value)); break;
            case OperandKind::Memory:
                if (absoluteZero && op.reg == 0) std::snprintf(buf, sizeof(buf), "%lld(0)", v);
                else std::snprintf(buf, sizeof(buf), "%lld(r%u)", v, op.reg);
                break;
            case OperandKind::None: buf[0] = 0; break;
            }
            return buf;
        }

        std::string Join(std::string_view mnemonic, std::initializer_list<std::string> operands)
        {
            std::string s(mnemonic);
            bool first = true;
            for (const auto& o : operands)
            {
                if (o.empty()) continue;
                s += first ? " " : ",";
                s += o;
                first = false;
            }
            return s;
        }

        std::string Suffixes(const Instruction& insn, std::string name)
        {
            if (insn.Has(kFlagOE)) name += 'o';
            if (insn.Has(kFlagLK)) name += 'l';
            if (insn.Has(kFlagAA)) name += 'a';
            if (insn.Has(kFlagRc)) name += '.';
            return name;
        }

        std::string CrPrefix(std::int64_t cr) { return cr == 0 ? std::string() : "cr" + std::to_string(cr); }

        // Bit do CR por nome: cr0 -> "eq", outros -> "4*cr6+eq"
        std::string CrBitName(std::uint32_t bi)
        {
            static const char* const names[] = { "lt", "gt", "eq", "so" };
            if (bi < 4) return names[bi];
            return "4*cr" + std::to_string(bi >> 2) + "+" + names[bi & 3];
        }

        // Desvio condicional: BO/BI -> forma simplificada. 'cond' recebe o sufixo (eq, dnz, dnzf...), 'operand' o operando
        // de condicao (campo ou bit do CR, vazio se nao houver) e 'hint' a dica de previsao (+ ou -).
        // Devolve false quando nao ha forma simplificada.
        bool BranchCondition(std::uint32_t bo, std::uint32_t bi, std::string& cond, std::string& operand, std::string& hint)
        {
            static const char* const whenTrue[] = { "lt", "gt", "eq", "so" };
            static const char* const whenFalse[] = { "ge", "le", "ne", "ns" };
            hint.clear();
            operand.clear();
            if ((bo & 0x1C) == 0x0C || (bo & 0x1C) == 0x04)   // 011at / 001at: depende de um bit do CR
            {
                cond = ((bo & 0x1C) == 0x0C ? whenTrue : whenFalse)[bi & 3];
                operand = CrPrefix(bi >> 2);
                if ((bo & 3) == 2) hint = "-";
                if ((bo & 3) == 3) hint = "+";
                return true;
            }
            if ((bo & 0x14) == 0x14)   // 1z1zz: sempre
            {
                cond.clear();
                return true;
            }
            if ((bo & 0x14) == 0x10)   // 1a00t / 1a01t: so o contador
            {
                cond = (bo & 0x02) ? "dz" : "dnz";
                if ((bo & 0x09) == 0x08) hint = "-";
                if ((bo & 0x09) == 0x09) hint = "+";
                return true;
            }
            // 0000y bdnzf, 0001y bdzf, 0100y bdnzt, 0101y bdzt: contador e bit do CR
            static const char* const withCounter[] = { "dnzf", "dzf", "", "", "dnzt", "dzt" };
            const std::uint32_t k = bo >> 1;
            if (k < 6 && withCounter[k][0])
            {
                cond = withCounter[k];
                operand = CrBitName(bi);
                return true;
            }
            return false;
        }

        // Condicoes de armadilha (campo TO)
        const char* TrapCondition(std::uint32_t to)
        {
            switch (to)
            {
            case 1: return "lgt";
            case 2: return "llt";
            case 4: return "eq";
            case 5: return "lge";
            case 6: return "lle";
            case 8: return "gt";
            case 12: return "ge";
            case 16: return "lt";
            case 20: return "le";
            case 24: return "ne";
            default: return nullptr;
            }
        }

        std::string SprName(std::int64_t spr)
        {
            switch (spr)
            {
            case 1: return "xer";
            case 8: return "lr";
            case 9: return "ctr";
            default: return {};
            }
        }
    }

    std::string Disassemble(const Instruction& insn)
    {
        if (!insn.Valid())
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), ".long 0x%x", insn.raw);
            return buf;
        }

        const OpcodeInfo& info = GetOpcodeInfo(insn.opcode);
        const auto& o = insn.operands;
        const bool absoluteZero = (info.flags & kFlagRA0) != 0;
        auto F = [&](int i) { return Format(o[static_cast<std::size_t>(i)], absoluteZero); };
        const std::string name = Suffixes(insn, info.name);
        const std::string rc = insn.Has(kFlagRc) ? "." : "";

        // ---------------------------------------------------------------- formas simplificadas
        switch (insn.opcode)
        {
        case Opcode::addi:
            if (o[1].value == 0) return Join("li", { F(0), F(2) });
            break;
        case Opcode::addis:
            if (o[1].value == 0) return Join("lis", { F(0), F(2) });
            break;
        case Opcode::ori:
            if (o[0].value == 0 && o[1].value == 0 && o[2].value == 0) return "nop";
            break;
        case Opcode::or_:
            if (o[0].value == 31 && o[1].value == 31 && o[2].value == 31 && rc.empty()) return "db16cyc";   // dica de espera do Xenon
            if (o[1].value == o[2].value) return Join("mr" + rc, { F(0), F(1) });
            break;
        case Opcode::nor:
            if (o[1].value == o[2].value) return Join("not" + rc, { F(0), F(1) });
            break;
        case Opcode::cmpi:
            return Join(o[1].value ? "cmpdi" : "cmpwi", { CrPrefix(o[0].value), F(2), F(3) });
        case Opcode::cmpli:
            return Join(o[1].value ? "cmpldi" : "cmplwi", { CrPrefix(o[0].value), F(2), F(3) });
        case Opcode::cmp:
            return Join(o[1].value ? "cmpd" : "cmpw", { CrPrefix(o[0].value), F(2), F(3) });
        case Opcode::cmpl:
            return Join(o[1].value ? "cmpld" : "cmplw", { CrPrefix(o[0].value), F(2), F(3) });
        case Opcode::rlwinm:
            if (o[3].value == 0 && o[4].value == 31) return Join("rotlwi" + rc, { F(0), F(1), F(2) });
            if (o[2].value == 0 && o[4].value == 31) return Join("clrlwi" + rc, { F(0), F(1), F(3) });
            break;
        case Opcode::rlwnm:
            if (o[3].value == 0 && o[4].value == 31) return Join("rotlw" + rc, { F(0), F(1), F(2) });
            break;
        case Opcode::rldicl:
            if (o[2].value == 0) return Join("clrldi" + rc, { F(0), F(1), F(3) });
            if (o[3].value == 0) return Join("rotldi" + rc, { F(0), F(1), F(2) });
            break;
        case Opcode::mfspr:
            if (auto s = SprName(o[1].value); !s.empty()) return Join("mf" + s, { F(0) });
            break;
        case Opcode::mtspr:
            if (auto s = SprName(o[0].value); !s.empty()) return Join("mt" + s, { F(1) });
            break;
        case Opcode::mftb:
            if (o[1].value == 268) return Join("mftb", { F(0) });
            if (o[1].value == 269) return Join("mftbu", { F(0) });
            break;
        case Opcode::mtcrf:
            if (o[0].value == 0xFF) return Join("mtcr", { F(1) });
            break;
        case Opcode::mfocrf:
            return Join("mfocrf", { F(0), std::to_string((insn.raw >> 12) & 0xFF) });
        case Opcode::sync:
            return (insn.raw >> 21) & 1 ? "lwsync" : "sync";
        case Opcode::bc:
        {
            std::string cond, operand, hint;
            if (BranchCondition(static_cast<std::uint32_t>(o[0].value), static_cast<std::uint32_t>(o[1].value), cond, operand, hint))
            {
                std::string m = "b" + cond;
                if (insn.Has(kFlagLK)) m += 'l';
                if (insn.Has(kFlagAA)) m += 'a';
                return Join(m + hint, { operand, F(2) });
            }
            break;
        }
        case Opcode::bclr:
        case Opcode::bcctr:
        {
            std::string cond, operand, hint;
            if (BranchCondition(static_cast<std::uint32_t>(o[0].value), static_cast<std::uint32_t>(o[1].value), cond, operand, hint))
            {
                std::string m = "b" + cond + (insn.opcode == Opcode::bclr ? "lr" : "ctr");
                if (insn.Has(kFlagLK)) m += 'l';
                return Join(m + hint, { operand });
            }
            break;
        }
        case Opcode::twi:
        case Opcode::tdi:
        case Opcode::tw:
        case Opcode::td:
        {
            const bool word = insn.opcode == Opcode::twi || insn.opcode == Opcode::tw;
            const bool imm = insn.opcode == Opcode::twi || insn.opcode == Opcode::tdi;
            const auto to = static_cast<std::uint32_t>(o[0].value);
            if (to == 31 && !imm && o[1].value == 0 && o[2].value == 0) return "trap";
            if (const char* c = TrapCondition(to)) return Join(std::string(word ? "tw" : "td") + c + (imm ? "i" : ""), { F(1), F(2) });
            break;
        }
        default:
            break;
        }

        // ---------------------------------------------------------------- forma geral
        switch (insn.layout)
        {
        case Layout::None:
        case Layout::X_sync:
            return name;
        case Layout::X_crf_rA_rB:
        case Layout::D_crf_rA_simm:
        case Layout::D_crf_rA_uimm:
            return Join(name, { F(0), F(1), F(2), F(3) });
        default:
            break;
        }
        std::string s = name;
        for (std::uint8_t i = 0; i < insn.operandCount; ++i)
        {
            s += i == 0 ? " " : ",";
            s += F(i);
        }
        return s;
    }
}
