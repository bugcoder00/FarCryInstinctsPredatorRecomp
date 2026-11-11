#include "cpu/CodeGen.h"

#include "core/Diagnostics.h"
#include "cpu/Decoder.h"
#include "cpu/Disassembler.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <set>
#include <system_error>

namespace predator::cpu
{
    // cabecalho do runtime embutido no executavel (gerado pelo CMake a partir de src/cpu/runtime/predator_cpu.h)
    extern const unsigned char kPredatorCpuHeader[];
    extern const std::size_t kPredatorCpuHeaderSize;

    std::string_view RuntimeHeader()
    {
        return { reinterpret_cast<const char*>(kPredatorCpuHeader), kPredatorCpuHeaderSize };
    }

    namespace
    {
        std::string FuncName(std::uint32_t a) { return std::format("sub_{:08X}", a); }
        std::string Label(std::uint32_t a) { return std::format("loc_{:08X}", a); }

        // ---------------------------------------------------------------- traducao de uma funcao
        class FunctionEmitter
        {
        public:
            FunctionEmitter(const loader::Executable& exe, const ProgramAnalysis& analysis, const Function& f, CodeGenStats& stats, bool comments)
                : m_exe(exe), m_analysis(analysis), m_f(f), m_stats(stats), m_comments(comments)
            {
            }

            std::string Emit()
            {
                if (m_f.source == FunctionSource::Import)
                {
                    m_out += std::format("PREDATOR_FUNCTION({})\n{{\n", FuncName(m_f.start));
                    m_out += std::format("    predator_import(ctx, base, \"{}\", {});\n}}\n\n", m_f.importLibrary, m_f.importOrdinal);
                    return std::move(m_out);
                }

                CollectLabels();
                CollectRegisters();
                if (m_local)
                {
                    // registradores em variaveis locais: o compilador do PC os mantem nos registradores dele. O contexto
                    // so e atualizado antes de chamar outra funcao ou sair (SAVE) e relido depois da chamada (LOAD).
                    std::string save, load;
                    for (int n : m_gprs) { save += std::format(" ctx->r[{0}] = r{0};", n); load += std::format(" r{0} = ctx->r[{0}];", n); }
                    for (int n : m_fprs) { save += std::format(" ctx->f[{0}] = f{0};", n); load += std::format(" f{0} = ctx->f[{0}];", n); }
                    if (m_ctr) { save += " ctx->ctr = ctr_;"; load += " ctr_ = ctx->ctr;"; }
                    m_out += std::format("#define PREDATOR_SAVE() do {{{} }} while (0)\n#define PREDATOR_LOAD() do {{{} }} while (0)\n", save, load);
                }
                m_out += std::format("PREDATOR_FUNCTION({})\n{{\n", FuncName(m_f.start));
                if (m_local)
                {
                    for (int n : m_gprs) m_out += std::format("    uint64_t r{0} = ctx->r[{0}];\n", n);
                    for (int n : m_fprs) m_out += std::format("    PredatorFpr f{0} = ctx->f[{0}];\n", n);
                    if (m_ctr) m_out += "    uint64_t ctr_ = ctx->ctr;\n";
                }
                if (m_f.bodyStart != m_f.start)
                {
                    // entrada secundaria: o corpo e o da funcao que a contem
                    m_labels.insert(m_f.start);
                    m_out += std::format("    goto {};\n", Label(m_f.start));
                }
                bool fallsThrough = true;
                for (std::uint32_t a = m_f.bodyStart; a < m_f.end; a += 4)
                {
                    if (m_labels.count(a)) m_out += Label(a) + ":\n";
                    auto w = m_exe.Read32(a);
                    if (!w) break;
                    const Instruction insn = Decode(*w, a);
                    ++m_stats.instructions;
                    if (m_comments) m_out += std::format("    /* {:08X}: {} */\n", a, Disassemble(insn));
                    m_code.clear();
                    fallsThrough = Translate(insn);
                    m_out += m_code;
                }
                // a ultima instrucao continua na proxima: se ali comeca outra funcao, e uma chamada final para ela
                if (fallsThrough)
                {
                    m_out += "    " + Save() + "\n";
                    if (m_analysis.FunctionAt(m_f.end)) m_out += std::format("    {}(ctx, base);\n", FuncName(m_f.end));
                    m_out += "    return;\n";
                }
                m_out += "}\n";
                if (m_local) m_out += "#undef PREDATOR_SAVE\n#undef PREDATOR_LOAD\n";
                m_out += "\n";
                return std::move(m_out);
            }

        private:
            // ------------------------------------------------------------ rotulos (alvos de desvio dentro da funcao)
            void CollectLabels()
            {
                for (std::uint32_t a = m_f.bodyStart; a < m_f.end; a += 4)
                {
                    auto w = m_exe.Read32(a);
                    if (!w) break;
                    const Instruction insn = Decode(*w, a);
                    if ((insn.opcode == Opcode::b && !insn.Has(kFlagLK)) || (insn.opcode == Opcode::bc && !insn.Has(kFlagLK)))
                    {
                        const auto t = static_cast<std::uint32_t>(insn.operands[insn.opcode == Opcode::b ? 0 : 2].value);
                        if (Inside(t)) m_labels.insert(t);
                    }
                }
                for (const auto& [at, table] : m_analysis.jumpTables)
                    if (Inside(at))
                        for (std::uint32_t t : table.targets) m_labels.insert(t);
            }

            [[nodiscard]] bool Inside(std::uint32_t a) const { return a >= m_f.bodyStart && a < m_f.end; }

            // ------------------------------------------------------------ registradores usados (para as copias locais)
            void CollectRegisters()
            {
                m_local = true;
                for (std::uint32_t a = m_f.bodyStart; a < m_f.end; a += 4)
                {
                    auto w = m_exe.Read32(a);
                    if (!w) break;
                    const Instruction insn = Decode(*w, a);
                    // lmw/stmw acessam os registradores por indice variavel: essas funcoes ficam no contexto
                    if (insn.opcode == Opcode::lmw || insn.opcode == Opcode::stmw) m_local = false;
                    for (std::size_t i = 0; i < insn.operandCount; ++i)
                    {
                        const Operand& o = insn.operands[i];
                        if (o.kind == OperandKind::Gpr || o.kind == OperandKind::GprOrZero) m_gprs.insert(static_cast<int>(o.value));
                        else if (o.kind == OperandKind::Memory) m_gprs.insert(o.reg);
                        else if (o.kind == OperandKind::Fpr) m_fprs.insert(static_cast<int>(o.value));
                    }
                    const bool usesCtr = (insn.opcode == Opcode::mtspr && insn.operands[0].value == 9) ||
                                         (insn.opcode == Opcode::mfspr && insn.operands[1].value == 9) || insn.opcode == Opcode::bcctr ||
                                         ((insn.opcode == Opcode::bc || insn.opcode == Opcode::bclr) && !(insn.operands[0].value & 4));
                    if (usesCtr) m_ctr = true;
                }
                if (!m_local) { m_gprs.clear(); m_fprs.clear(); m_ctr = false; }
            }

            // antes de chamar outra funcao ou sair: o contexto recebe as copias; depois da chamada: copias relidas
            std::string Save() const { return m_local ? "PREDATOR_SAVE(); " : ""; }
            std::string Load() const { return m_local ? " PREDATOR_LOAD();" : ""; }
            std::string Return() const { return m_local ? "{ PREDATOR_SAVE(); return; }" : "return;"; }
            std::string Ctr() const { return m_ctr ? "ctr_" : "ctx->ctr"; }

            void Line(const std::string& s) { m_code += "    " + s + "\n"; }

            // ------------------------------------------------------------ operandos
            std::string R(std::int64_t n) const { return m_local ? std::format("r{}", n) : std::format("ctx->r[{}]", n); }
            std::string R(int i) const { return R(m_insn->operands[static_cast<std::size_t>(i)].value); }
            std::string RZ(int i) const
            {
                const auto& o = m_insn->operands[static_cast<std::size_t>(i)];
                return (o.kind == OperandKind::GprOrZero && o.value == 0) ? std::string("0") : R(o.value);
            }
            std::string FprName(int i) const
            {
                const auto n = m_insn->operands[static_cast<std::size_t>(i)].value;
                return m_local ? std::format("f{}", n) : std::format("ctx->f[{}]", n);
            }
            std::string F(int i) const { return FprName(i) + ".f"; }
            std::string FU(int i) const { return FprName(i) + ".u"; }
            std::string V(int i) const { return std::format("&ctx->v[{}]", m_insn->operands[static_cast<std::size_t>(i)].value); }
            std::int64_t Val(int i) const { return m_insn->operands[static_cast<std::size_t>(i)].value; }
            static std::string S64(std::int64_t v) { return std::format("(uint64_t)(int64_t){}LL", v); }

            // endereco efetivo de um operando de memoria d(rA)
            std::string EA(int i) const
            {
                const auto& o = m_insn->operands[static_cast<std::size_t>(i)];
                const bool ra0 = (GetOpcodeInfo(m_insn->opcode).flags & kFlagRA0) != 0;
                if (ra0 && o.reg == 0) return std::format("(uint32_t){}", o.value);
                return std::format("(uint32_t)({} + {})", R(static_cast<std::int64_t>(o.reg)), S64(o.value));
            }
            std::string BaseReg(int i) const { return R(static_cast<std::int64_t>(m_insn->operands[static_cast<std::size_t>(i)].reg)); }
            // endereco efetivo indexado (rA|0) + rB
            std::string EAX(int a, int b) const
            {
                const std::string ra = RZ(a);
                return ra == "0" ? std::format("(uint32_t){}", R(b)) : std::format("(uint32_t)({} + {})", ra, R(b));
            }

            void Rc(const std::string& dest)
            {
                if (m_insn->Has(kFlagRc)) Line(std::format("predator_set_cr0(ctx, {});", dest));
            }

            void Unimplemented()
            {
                const std::string name = GetOpcodeInfo(m_insn->opcode).name;
                ++m_stats.unimplemented;
                ++m_stats.unimplementedByName[name];
                Line(std::format("predator_unimplemented(ctx, base, 0x{:08X}u, \"{}\");", m_insn->address, name));
            }

            // ------------------------------------------------------------ desvios
            std::string JumpTo(std::uint32_t target) const
            {
                if (m_labels.count(target)) return std::format("goto {};", Label(target));
                if (m_analysis.FunctionAt(target)) return std::format("{{ {}{}(ctx, base); return; }}", Save(), FuncName(target));
                return std::format("{{ {}predator_jump_indirect(ctx, base, 0x{:08X}u, 0x{:08X}u); return; }}", Save(), m_insn->address, target);
            }
            // setjmp do jogo: grava f14 no inicio do buffer (stfd f14,0(r3)), a pilha em 144 (std r1,144(r3)) e o lr em
            // 308 (stw r0,308(r3)) logo no comeco da funcao
            bool IsSetJmp(std::uint32_t target) const
            {
                bool f14 = false, r1 = false, lr = false;
                for (std::uint32_t a = target; a < target + 64 * 4; a += 4)
                {
                    auto w = m_exe.Read32(a);
                    if (!w) break;
                    f14 |= *w == 0xD9C30000u; r1 |= *w == 0xF8230090u; lr |= *w == 0x90030134u;
                    if (*w == 0x4E800020u) break;   // blr
                }
                return f14 && r1 && lr;
            }
            std::string CallTo(std::uint32_t target) const
            {
                std::string s = std::format("{{ {}ctx->lr = 0x{:08X}u; ", Save(), m_insn->address + 4);
                // o setjmp do PC precisa ficar nesta funcao (o quadro dela continua vivo ate o longjmp); na volta pelo
                // longjmp os registradores do jogo ja foram restaurados no contexto e so sao relidos (LOAD)
                if (m_analysis.FunctionAt(target) && IsSetJmp(target))
                    return s + std::format("if (setjmp(*predator_setjmp_buffer(ctx, base)) == 0) {}(ctx, base);{} }}", FuncName(target), Load());
                if (m_analysis.FunctionAt(target)) return s + std::format("{}(ctx, base);{} }}", FuncName(target), Load());
                return s + std::format("predator_call_indirect(ctx, base, 0x{:08X}u);{} }}", target, Load());
            }

            // condicao de um desvio condicional a partir de BO/BI (decrementa o CTR quando o BO pede)
            std::string BranchCondition(std::uint32_t bo, std::uint32_t bi) const
            {
                std::string ctr, cr;
                if (!(bo & 4)) ctr = "((uint32_t)--" + Ctr() + ((bo & 2) ? " == 0)" : " != 0)");
                if (!(bo & 16))
                {
                    static const char* const names[] = { "lt", "gt", "eq", "so" };
                    const std::string bit = std::format("ctx->cr[{}].{}", bi >> 2, names[bi & 3]);
                    cr = (bo & 8) ? bit : "!" + bit;
                }
                if (ctr.empty() && cr.empty()) return "1";
                if (ctr.empty()) return cr;
                if (cr.empty()) return ctr;
                return ctr + " && " + cr;
            }

            // ------------------------------------------------------------ traducao; devolve se a execucao continua na proxima
            bool Translate(const Instruction& insn)
            {
                m_insn = &insn;
                const std::uint32_t next = insn.address + 4;
                const auto& o = insn.operands;
                switch (insn.opcode)
                {
                // ---------------------------------------------------- desvios
                case Opcode::b:
                {
                    const auto t = static_cast<std::uint32_t>(o[0].value);
                    if (insn.Has(kFlagLK)) { Line(CallTo(t)); return true; }
                    Line(JumpTo(t));
                    return false;
                }
                case Opcode::bc:
                {
                    const auto t = static_cast<std::uint32_t>(o[2].value);
                    const std::string cond = BranchCondition(static_cast<std::uint32_t>(o[0].value), static_cast<std::uint32_t>(o[1].value));
                    if (insn.Has(kFlagLK)) Line(std::format("if ({}) {{ {} }}", cond, CallTo(t)));
                    else Line(std::format("if ({}) {}", cond, JumpTo(t)));
                    return cond != "1" || insn.Has(kFlagLK);
                }
                case Opcode::bclr:
                {
                    const std::string cond = BranchCondition(static_cast<std::uint32_t>(o[0].value), static_cast<std::uint32_t>(o[1].value));
                    if (insn.Has(kFlagLK))
                        Line(std::format("if ({}) {{ uint32_t t_ = (uint32_t)ctx->lr; {}ctx->lr = 0x{:08X}u; predator_call_indirect(ctx, base, t_);{} }}", cond, Save(), next, Load()));
                    else
                        Line(cond == "1" ? Return() : std::format("if ({}) {}", cond, Return()));
                    return cond != "1" || insn.Has(kFlagLK);
                }
                case Opcode::bcctr:
                {
                    const std::string cond = BranchCondition(static_cast<std::uint32_t>(o[0].value), static_cast<std::uint32_t>(o[1].value));
                    if (insn.Has(kFlagLK))
                    {
                        Line(std::format("if ({}) {{ {}ctx->lr = 0x{:08X}u; predator_call_indirect(ctx, base, (uint32_t){});{} }}", cond, Save(), next, Ctr(), Load()));
                        return true;
                    }
                    std::string body;
                    if (auto it = m_analysis.jumpTables.find(insn.address); it != m_analysis.jumpTables.end())
                    {
                        body = std::format("switch ((uint32_t){}) {{", Ctr());
                        std::set<std::uint32_t> seen;
                        for (std::uint32_t t : it->second.targets)
                            if (seen.insert(t).second) body += std::format(" case 0x{:08X}u: goto {};", t, Label(t));
                        body += std::format(" default: {}predator_jump_indirect(ctx, base, 0x{:08X}u, (uint32_t){}); return; }}", Save(), insn.address, Ctr());
                    }
                    else
                        body = std::format("{{ {}predator_jump_indirect(ctx, base, 0x{:08X}u, (uint32_t){}); return; }}", Save(), insn.address, Ctr());
                    Line(cond == "1" ? body : std::format("if ({}) {}", cond, body));
                    return cond != "1";
                }
                case Opcode::sc:
                    Line(std::format("{{ {}predator_syscall(ctx, base, 0x{:08X}u);{} }}", Save(), insn.address, Load()));
                    return true;

                // ---------------------------------------------------- inteiros com imediato
                case Opcode::addi: AddImmediate(Val(2)); return true;
                case Opcode::addis: AddImmediate(Val(2) * 65536); return true;
                case Opcode::addic:
                case Opcode::addic_rc:
                    Line(std::format("{{ uint64_t a_ = {}, r_ = a_ + {}; ctx->xer_ca = (uint32_t)r_ < (uint32_t)a_; {} = r_; }}", R(1), S64(Val(2)), R(0)));
                    if (insn.opcode == Opcode::addic_rc) Line(std::format("predator_set_cr0(ctx, {});", R(0)));
                    return true;
                case Opcode::subfic:
                    Line(std::format("{{ uint64_t a_ = {}, i_ = {}; ctx->xer_ca = (uint32_t)i_ >= (uint32_t)a_; {} = i_ - a_; }}", R(1), S64(Val(2)), R(0)));
                    return true;
                case Opcode::mulli: Line(std::format("{} = (uint64_t)((int64_t){} * {}LL);", R(0), R(1), Val(2))); return true;
                case Opcode::cmpi:
                    if (Val(1)) Line(std::format("predator_cmp_s64(&ctx->cr[{}], (int64_t){}, {}LL, ctx->xer_so);", Val(0), R(2), Val(3)));
                    else Line(std::format("predator_cmp_s64(&ctx->cr[{}], (int32_t){}, {}LL, ctx->xer_so);", Val(0), R(2), Val(3)));
                    return true;
                case Opcode::cmpli:
                    if (Val(1)) Line(std::format("predator_cmp_u64(&ctx->cr[{}], {}, {}ULL, ctx->xer_so);", Val(0), R(2), Val(3)));
                    else Line(std::format("predator_cmp_u64(&ctx->cr[{}], (uint32_t){}, {}ULL, ctx->xer_so);", Val(0), R(2), Val(3)));
                    return true;
                case Opcode::twi:
                case Opcode::tdi:
                    Trap(static_cast<std::uint32_t>(Val(0)), R(1), std::format("{}LL", Val(2)), insn.opcode == Opcode::twi);
                    return true;
                case Opcode::tw:
                case Opcode::td:
                    Trap(static_cast<std::uint32_t>(Val(0)), R(1), R(2), insn.opcode == Opcode::tw);
                    return true;
                case Opcode::ori: Line(std::format("{} = {} | {}ULL;", R(0), R(1), Val(2))); return true;
                case Opcode::oris: Line(std::format("{} = {} | {}ULL;", R(0), R(1), Val(2) << 16)); return true;
                case Opcode::xori: Line(std::format("{} = {} ^ {}ULL;", R(0), R(1), Val(2))); return true;
                case Opcode::xoris: Line(std::format("{} = {} ^ {}ULL;", R(0), R(1), Val(2) << 16)); return true;
                case Opcode::andi_rc: Line(std::format("{} = {} & {}ULL;", R(0), R(1), Val(2))); Line(std::format("predator_set_cr0(ctx, {});", R(0))); return true;
                case Opcode::andis_rc: Line(std::format("{} = {} & {}ULL;", R(0), R(1), Val(2) << 16)); Line(std::format("predator_set_cr0(ctx, {});", R(0))); return true;

                // ---------------------------------------------------- rotacoes
                case Opcode::rlwinm:
                    Line(std::format("{} = predator_rotl32x2({}, {}) & 0x{:016X}ULL;", R(0), R(1), Val(2), Mask(Val(3) + 32, Val(4) + 32)));
                    Rc(R(0)); return true;
                case Opcode::rlwnm:
                    Line(std::format("{} = predator_rotl32x2({}, (uint32_t){} & 31) & 0x{:016X}ULL;", R(0), R(1), R(2), Mask(Val(3) + 32, Val(4) + 32)));
                    Rc(R(0)); return true;
                case Opcode::rlwimi:
                {
                    const std::uint64_t m = Mask(Val(3) + 32, Val(4) + 32);
                    Line(std::format("{0} = (predator_rotl32x2({1}, {2}) & 0x{3:016X}ULL) | ({0} & 0x{4:016X}ULL);", R(0), R(1), Val(2), m, ~m));
                    Rc(R(0)); return true;
                }
                case Opcode::rldicl:
                    Line(std::format("{} = predator_rotl64({}, {}) & 0x{:016X}ULL;", R(0), R(1), Val(2), Mask(Val(3), 63)));
                    Rc(R(0)); return true;
                case Opcode::rldicr:
                    Line(std::format("{} = predator_rotl64({}, {}) & 0x{:016X}ULL;", R(0), R(1), Val(2), Mask(0, Val(3))));
                    Rc(R(0)); return true;
                case Opcode::rldic:
                    Line(std::format("{} = predator_rotl64({}, {}) & 0x{:016X}ULL;", R(0), R(1), Val(2), Mask(Val(3), 63 - Val(2))));
                    Rc(R(0)); return true;
                case Opcode::rldimi:
                {
                    const std::uint64_t m = Mask(Val(3), 63 - Val(2));
                    Line(std::format("{0} = (predator_rotl64({1}, {2}) & 0x{3:016X}ULL) | ({0} & 0x{4:016X}ULL);", R(0), R(1), Val(2), m, ~m));
                    Rc(R(0)); return true;
                }
                case Opcode::rldcl:
                    Line(std::format("{} = predator_rotl64({}, (uint32_t){} & 63) & 0x{:016X}ULL;", R(0), R(1), R(2), Mask(Val(3), 63)));
                    Rc(R(0)); return true;
                case Opcode::rldcr:
                    Line(std::format("{} = predator_rotl64({}, (uint32_t){} & 63) & 0x{:016X}ULL;", R(0), R(1), R(2), Mask(0, Val(3))));
                    Rc(R(0)); return true;

                // ---------------------------------------------------- aritmetica (forma XO)
                case Opcode::add: Arith(std::format("{} + {}", R(1), R(2)), R(1), R(2), false); return true;
                case Opcode::subf: Arith(std::format("{} - {}", R(2), R(1)), std::format("~{}", R(1)), R(2), true); return true;
                case Opcode::neg: Arith(std::format("(uint64_t)0 - {}", R(1)), std::format("~{}", R(1)), "0", true); return true;
                case Opcode::addc:
                    Line(std::format("{{ uint64_t a_ = {}, r_ = a_ + {}; ctx->xer_ca = (uint32_t)r_ < (uint32_t)a_; {} = r_; }}", R(1), R(2), R(0)));
                    Rc(R(0)); return true;
                case Opcode::subfc:
                    Line(std::format("{{ uint64_t a_ = {}, b_ = {}; ctx->xer_ca = (uint32_t)b_ >= (uint32_t)a_; {} = b_ - a_; }}", R(1), R(2), R(0)));
                    Rc(R(0)); return true;
                case Opcode::adde: ExtendedAdd(R(1), R(2)); return true;
                case Opcode::subfe: ExtendedAdd("~" + R(1), R(2)); return true;
                case Opcode::addze: ExtendedAdd(R(1), "0"); return true;
                case Opcode::subfze: ExtendedAdd("~" + R(1), "0"); return true;
                case Opcode::addme: ExtendedAdd(R(1), "~0ULL"); return true;
                case Opcode::subfme: ExtendedAdd("~" + R(1), "~0ULL"); return true;
                case Opcode::mullw: Line(std::format("{} = (uint64_t)((int64_t)(int32_t){} * (int64_t)(int32_t){});", R(0), R(1), R(2))); Rc(R(0)); return true;
                case Opcode::mulld: Line(std::format("{} = (uint64_t)((int64_t){} * (int64_t){});", R(0), R(1), R(2))); Rc(R(0)); return true;
                case Opcode::mulhw: Line(std::format("{} = (uint64_t)(((int64_t)(int32_t){} * (int64_t)(int32_t){}) >> 32);", R(0), R(1), R(2))); Rc(R(0)); return true;
                case Opcode::mulhwu: Line(std::format("{} = ((uint64_t)(uint32_t){} * (uint64_t)(uint32_t){}) >> 32;", R(0), R(1), R(2))); Rc(R(0)); return true;
                case Opcode::mulhd: Line(std::format("{} = (uint64_t)predator_mulhs64((int64_t){}, (int64_t){});", R(0), R(1), R(2))); Rc(R(0)); return true;
                case Opcode::mulhdu: Line(std::format("{} = predator_mulhu64({}, {});", R(0), R(1), R(2))); Rc(R(0)); return true;
                case Opcode::divw: Line(std::format("{} = (uint32_t)predator_divw((int32_t){}, (int32_t){});", R(0), R(1), R(2))); Rc(R(0)); return true;
                case Opcode::divwu: Line(std::format("{} = predator_divwu((uint32_t){}, (uint32_t){});", R(0), R(1), R(2))); Rc(R(0)); return true;
                case Opcode::divd: Line(std::format("{} = (uint64_t)predator_divd((int64_t){}, (int64_t){});", R(0), R(1), R(2))); Rc(R(0)); return true;
                case Opcode::divdu: Line(std::format("{} = predator_divdu({}, {});", R(0), R(1), R(2))); Rc(R(0)); return true;

                // ---------------------------------------------------- logica e deslocamentos (o0 = rA, o1 = rS)
                case Opcode::and_: Line(std::format("{} = {} & {};", R(0), R(1), R(2))); Rc(R(0)); return true;
                case Opcode::andc: Line(std::format("{} = {} & ~{};", R(0), R(1), R(2))); Rc(R(0)); return true;
                case Opcode::or_: Line(Val(1) == Val(2) ? std::format("{} = {};", R(0), R(1)) : std::format("{} = {} | {};", R(0), R(1), R(2))); Rc(R(0)); return true;
                case Opcode::orc: Line(std::format("{} = {} | ~{};", R(0), R(1), R(2))); Rc(R(0)); return true;
                case Opcode::xor_: Line(std::format("{} = {} ^ {};", R(0), R(1), R(2))); Rc(R(0)); return true;
                case Opcode::nand: Line(std::format("{} = ~({} & {});", R(0), R(1), R(2))); Rc(R(0)); return true;
                case Opcode::nor: Line(std::format("{} = ~({} | {});", R(0), R(1), R(2))); Rc(R(0)); return true;
                case Opcode::eqv: Line(std::format("{} = ~({} ^ {});", R(0), R(1), R(2))); Rc(R(0)); return true;
                case Opcode::slw: Line(std::format("{{ uint32_t n_ = (uint32_t){} & 63; {} = n_ > 31 ? 0 : (uint64_t)((uint32_t){} << n_); }}", R(2), R(0), R(1))); Rc(R(0)); return true;
                case Opcode::srw: Line(std::format("{{ uint32_t n_ = (uint32_t){} & 63; {} = n_ > 31 ? 0 : (uint64_t)((uint32_t){} >> n_); }}", R(2), R(0), R(1))); Rc(R(0)); return true;
                case Opcode::sld: Line(std::format("{{ uint32_t n_ = (uint32_t){} & 127; {} = n_ > 63 ? 0 : {} << n_; }}", R(2), R(0), R(1))); Rc(R(0)); return true;
                case Opcode::srd: Line(std::format("{{ uint32_t n_ = (uint32_t){} & 127; {} = n_ > 63 ? 0 : {} >> n_; }}", R(2), R(0), R(1))); Rc(R(0)); return true;
                case Opcode::sraw:
                    Line(std::format("{{ uint32_t n_ = (uint32_t){} & 63; int32_t s_ = (int32_t){}; if (n_ > 31) {{ {} = (uint64_t)(int64_t)(s_ >> 31); ctx->xer_ca = s_ < 0; }} "
                                     "else {{ {} = (uint64_t)(int64_t)(s_ >> n_); ctx->xer_ca = s_ < 0 && n_ && ((uint32_t)s_ & ((1u << n_) - 1)); }} }}",
                                     R(2), R(1), R(0), R(0)));
                    Rc(R(0)); return true;
                case Opcode::srawi:
                {
                    const auto n = Val(2);
                    if (n == 0) Line(std::format("{} = (uint64_t)(int64_t)(int32_t){}; ctx->xer_ca = 0;", R(0), R(1)));
                    else Line(std::format("{{ int32_t s_ = (int32_t){}; {} = (uint64_t)(int64_t)(s_ >> {}); ctx->xer_ca = s_ < 0 && ((uint32_t)s_ & 0x{:X}u); }}",
                                          R(1), R(0), n, (1u << n) - 1));
                    Rc(R(0)); return true;
                }
                case Opcode::srad:
                    Line(std::format("{{ uint32_t n_ = (uint32_t){} & 127; int64_t s_ = (int64_t){}; if (n_ > 63) {{ {} = (uint64_t)(s_ >> 63); ctx->xer_ca = s_ < 0; }} "
                                     "else {{ {} = (uint64_t)(s_ >> n_); ctx->xer_ca = s_ < 0 && n_ && ((uint64_t)s_ & ((1ull << n_) - 1)); }} }}",
                                     R(2), R(1), R(0), R(0)));
                    Rc(R(0)); return true;
                case Opcode::sradi:
                {
                    const auto n = Val(2);
                    if (n == 0) Line(std::format("{} = {}; ctx->xer_ca = 0;", R(0), R(1)));
                    else Line(std::format("{{ int64_t s_ = (int64_t){}; {} = (uint64_t)(s_ >> {}); ctx->xer_ca = s_ < 0 && ((uint64_t)s_ & 0x{:X}ULL); }}",
                                          R(1), R(0), n, (1ull << n) - 1));
                    Rc(R(0)); return true;
                }
                case Opcode::cntlzw: Line(std::format("{} = predator_cntlz32((uint32_t){});", R(0), R(1))); Rc(R(0)); return true;
                case Opcode::cntlzd: Line(std::format("{} = predator_cntlz64({});", R(0), R(1))); Rc(R(0)); return true;
                case Opcode::extsb: Line(std::format("{} = (uint64_t)(int64_t)(int8_t){};", R(0), R(1))); Rc(R(0)); return true;
                case Opcode::extsh: Line(std::format("{} = (uint64_t)(int64_t)(int16_t){};", R(0), R(1))); Rc(R(0)); return true;
                case Opcode::extsw: Line(std::format("{} = (uint64_t)(int64_t)(int32_t){};", R(0), R(1))); Rc(R(0)); return true;
                case Opcode::cmp:
                    if (Val(1)) Line(std::format("predator_cmp_s64(&ctx->cr[{}], (int64_t){}, (int64_t){}, ctx->xer_so);", Val(0), R(2), R(3)));
                    else Line(std::format("predator_cmp_s64(&ctx->cr[{}], (int32_t){}, (int32_t){}, ctx->xer_so);", Val(0), R(2), R(3)));
                    return true;
                case Opcode::cmpl:
                    if (Val(1)) Line(std::format("predator_cmp_u64(&ctx->cr[{}], {}, {}, ctx->xer_so);", Val(0), R(2), R(3)));
                    else Line(std::format("predator_cmp_u64(&ctx->cr[{}], (uint32_t){}, (uint32_t){}, ctx->xer_so);", Val(0), R(2), R(3)));
                    return true;

                // ---------------------------------------------------- cargas e armazenamentos (forma D)
                case Opcode::lwz: Line(std::format("{} = predator_ld32(base, {});", R(0), EA(1))); return true;
                case Opcode::lbz: Line(std::format("{} = predator_ld8(base, {});", R(0), EA(1))); return true;
                case Opcode::lhz: Line(std::format("{} = predator_ld16(base, {});", R(0), EA(1))); return true;
                case Opcode::lha: Line(std::format("{} = (uint64_t)(int64_t)(int16_t)predator_ld16(base, {});", R(0), EA(1))); return true;
                case Opcode::ld: Line(std::format("{} = predator_ld64(base, {});", R(0), EA(1))); return true;
                case Opcode::lwa: Line(std::format("{} = (uint64_t)(int64_t)(int32_t)predator_ld32(base, {});", R(0), EA(1))); return true;
                case Opcode::stw: Line(std::format("predator_st32(base, {}, (uint32_t){});", EA(1), R(0))); return true;
                case Opcode::stb: Line(std::format("predator_st8(base, {}, (uint8_t){});", EA(1), R(0))); return true;
                case Opcode::sth: Line(std::format("predator_st16(base, {}, (uint16_t){});", EA(1), R(0))); return true;
                case Opcode::std: Line(std::format("predator_st64(base, {}, {});", EA(1), R(0))); return true;
                case Opcode::lwzu: Update(std::format("{} = predator_ld32(base, ea_);", R(0))); return true;
                case Opcode::lbzu: Update(std::format("{} = predator_ld8(base, ea_);", R(0))); return true;
                case Opcode::lhzu: Update(std::format("{} = predator_ld16(base, ea_);", R(0))); return true;
                case Opcode::lhau: Update(std::format("{} = (uint64_t)(int64_t)(int16_t)predator_ld16(base, ea_);", R(0))); return true;
                case Opcode::ldu: Update(std::format("{} = predator_ld64(base, ea_);", R(0))); return true;
                case Opcode::stwu: Update(std::format("predator_st32(base, ea_, (uint32_t){});", R(0))); return true;
                case Opcode::stbu: Update(std::format("predator_st8(base, ea_, (uint8_t){});", R(0))); return true;
                case Opcode::sthu: Update(std::format("predator_st16(base, ea_, (uint16_t){});", R(0))); return true;
                case Opcode::stdu: Update(std::format("predator_st64(base, ea_, {});", R(0))); return true;
                case Opcode::lfs: Line(std::format("{} = (double)predator_ldf32(base, {});", F(0), EA(1))); return true;
                case Opcode::lfd: Line(std::format("{} = predator_ld64(base, {});", FU(0), EA(1))); return true;
                case Opcode::stfs: Line(std::format("predator_stf32(base, {}, (float){});", EA(1), F(0))); return true;
                case Opcode::stfd: Line(std::format("predator_st64(base, {}, {});", EA(1), FU(0))); return true;
                case Opcode::lfsu: Update(std::format("{} = (double)predator_ldf32(base, ea_);", F(0))); return true;
                case Opcode::lfdu: Update(std::format("{} = predator_ld64(base, ea_);", FU(0))); return true;
                case Opcode::stfsu: Update(std::format("predator_stf32(base, ea_, (float){});", F(0))); return true;
                case Opcode::stfdu: Update(std::format("predator_st64(base, ea_, {});", FU(0))); return true;
                case Opcode::lmw:
                    Line(std::format("{{ uint32_t ea_ = {}; for (int i_ = {}; i_ < 32; ++i_, ea_ += 4) ctx->r[i_] = predator_ld32(base, ea_); }}", EA(1), Val(0)));
                    return true;
                case Opcode::stmw:
                    Line(std::format("{{ uint32_t ea_ = {}; for (int i_ = {}; i_ < 32; ++i_, ea_ += 4) predator_st32(base, ea_, (uint32_t)ctx->r[i_]); }}", EA(1), Val(0)));
                    return true;

                // ---------------------------------------------------- cargas e armazenamentos indexados (forma X)
                case Opcode::lwzx: Line(std::format("{} = predator_ld32(base, {});", R(0), EAX(1, 2))); return true;
                case Opcode::lbzx: Line(std::format("{} = predator_ld8(base, {});", R(0), EAX(1, 2))); return true;
                case Opcode::lhzx: Line(std::format("{} = predator_ld16(base, {});", R(0), EAX(1, 2))); return true;
                case Opcode::lhax: Line(std::format("{} = (uint64_t)(int64_t)(int16_t)predator_ld16(base, {});", R(0), EAX(1, 2))); return true;
                case Opcode::ldx: Line(std::format("{} = predator_ld64(base, {});", R(0), EAX(1, 2))); return true;
                case Opcode::lwax: Line(std::format("{} = (uint64_t)(int64_t)(int32_t)predator_ld32(base, {});", R(0), EAX(1, 2))); return true;
                case Opcode::stwx: Line(std::format("predator_st32(base, {}, (uint32_t){});", EAX(1, 2), R(0))); return true;
                case Opcode::stbx: Line(std::format("predator_st8(base, {}, (uint8_t){});", EAX(1, 2), R(0))); return true;
                case Opcode::sthx: Line(std::format("predator_st16(base, {}, (uint16_t){});", EAX(1, 2), R(0))); return true;
                case Opcode::stdx: Line(std::format("predator_st64(base, {}, {});", EAX(1, 2), R(0))); return true;
                case Opcode::lwzux: UpdateX(std::format("{} = predator_ld32(base, ea_);", R(0))); return true;
                case Opcode::lbzux: UpdateX(std::format("{} = predator_ld8(base, ea_);", R(0))); return true;
                case Opcode::lhzux: UpdateX(std::format("{} = predator_ld16(base, ea_);", R(0))); return true;
                case Opcode::lhaux: UpdateX(std::format("{} = (uint64_t)(int64_t)(int16_t)predator_ld16(base, ea_);", R(0))); return true;
                case Opcode::ldux: UpdateX(std::format("{} = predator_ld64(base, ea_);", R(0))); return true;
                case Opcode::lwaux: UpdateX(std::format("{} = (uint64_t)(int64_t)(int32_t)predator_ld32(base, ea_);", R(0))); return true;
                case Opcode::stwux: UpdateX(std::format("predator_st32(base, ea_, (uint32_t){});", R(0))); return true;
                case Opcode::stbux: UpdateX(std::format("predator_st8(base, ea_, (uint8_t){});", R(0))); return true;
                case Opcode::sthux: UpdateX(std::format("predator_st16(base, ea_, (uint16_t){});", R(0))); return true;
                case Opcode::stdux: UpdateX(std::format("predator_st64(base, ea_, {});", R(0))); return true;
                case Opcode::lwbrx: Line(std::format("{} = predator_bswap32(predator_ld32(base, {}));", R(0), EAX(1, 2))); return true;
                case Opcode::lhbrx: Line(std::format("{} = predator_bswap16(predator_ld16(base, {}));", R(0), EAX(1, 2))); return true;
                case Opcode::ldbrx: Line(std::format("{} = predator_bswap64(predator_ld64(base, {}));", R(0), EAX(1, 2))); return true;
                case Opcode::stwbrx: Line(std::format("predator_st32(base, {}, predator_bswap32((uint32_t){}));", EAX(1, 2), R(0))); return true;
                case Opcode::sthbrx: Line(std::format("predator_st16(base, {}, predator_bswap16((uint16_t){}));", EAX(1, 2), R(0))); return true;
                case Opcode::stdbrx: Line(std::format("predator_st64(base, {}, predator_bswap64({}));", EAX(1, 2), R(0))); return true;
                case Opcode::lfsx: Line(std::format("{} = (double)predator_ldf32(base, {});", F(0), EAX(1, 2))); return true;
                case Opcode::lfdx: Line(std::format("{} = predator_ld64(base, {});", FU(0), EAX(1, 2))); return true;
                case Opcode::stfsx: Line(std::format("predator_stf32(base, {}, (float){});", EAX(1, 2), F(0))); return true;
                case Opcode::stfdx: Line(std::format("predator_st64(base, {}, {});", EAX(1, 2), FU(0))); return true;
                case Opcode::stfiwx: Line(std::format("predator_st32(base, {}, (uint32_t){});", EAX(1, 2), FU(0))); return true;
                case Opcode::lfsux: UpdateX(std::format("{} = (double)predator_ldf32(base, ea_);", F(0))); return true;
                case Opcode::lfdux: UpdateX(std::format("{} = predator_ld64(base, ea_);", FU(0))); return true;
                case Opcode::stfsux: UpdateX(std::format("predator_stf32(base, ea_, (float){});", F(0))); return true;
                case Opcode::stfdux: UpdateX(std::format("predator_st64(base, ea_, {});", FU(0))); return true;
                case Opcode::lwarx: Line(std::format("{} = predator_lwarx(ctx, base, {});", R(0), EAX(1, 2))); return true;
                case Opcode::ldarx: Line(std::format("{} = predator_ldarx(ctx, base, {});", R(0), EAX(1, 2))); return true;
                case Opcode::stwcx: Line(std::format("predator_stwcx(ctx, base, {}, (uint32_t){});", EAX(1, 2), R(0))); return true;
                case Opcode::stdcx: Line(std::format("predator_stdcx(ctx, base, {}, {});", EAX(1, 2), R(0))); return true;

                // ---------------------------------------------------- cache, sincronizacao, registradores especiais
                case Opcode::dcbz: Line(std::format("predator_zero(base, {}, 32);", EAX(0, 1))); return true;
                case Opcode::dcbz128: Line(std::format("predator_zero(base, {}, 128);", EAX(0, 1))); return true;
                case Opcode::dcbf: case Opcode::dcbst: case Opcode::dcbt: case Opcode::dcbtst: case Opcode::dcbi: case Opcode::icbi:
                case Opcode::sync: case Opcode::eieio: case Opcode::isync:
                    return true;   // sem efeito no PC
                case Opcode::mfspr:
                    switch (Val(1))
                    {
                    case 8: Line(std::format("{} = ctx->lr;", R(0))); return true;
                    case 9: Line(std::format("{} = {};", R(0), Ctr())); return true;
                    case 1: Line(std::format("{} = ((uint64_t)ctx->xer_so << 31) | ((uint64_t)ctx->xer_ov << 30) | ((uint64_t)ctx->xer_ca << 29) | ctx->xer_count;", R(0))); return true;
                    case 268: Line(std::format("{} = predator_timebase();", R(0))); return true;
                    default: Unimplemented(); return true;
                    }
                case Opcode::mtspr:
                    switch (Val(0))
                    {
                    case 8: Line(std::format("ctx->lr = {};", R(1))); return true;
                    case 9: Line(std::format("{} = {};", Ctr(), R(1))); return true;
                    case 1:
                        Line(std::format("ctx->xer_so = ({} >> 31) & 1; ctx->xer_ov = ({} >> 30) & 1; ctx->xer_ca = ({} >> 29) & 1; ctx->xer_count = (uint8_t){};",
                                         R(1), R(1), R(1), R(1)));
                        return true;
                    default: Unimplemented(); return true;
                    }
                case Opcode::mftb: Line(std::format("{} = {};", R(0), Val(1) == 269 ? "predator_timebase() >> 32" : "predator_timebase()")); return true;
                case Opcode::mfcr: case Opcode::mfocrf: Line(std::format("{} = predator_get_cr(ctx);", R(0))); return true;
                case Opcode::mtcrf: case Opcode::mtocrf: Line(std::format("predator_set_cr(ctx, (uint32_t){}, 0x{:X}u);", R(1), Val(0))); return true;
                case Opcode::mfmsr: Line(std::format("{} = ctx->msr;", R(0))); return true;
                case Opcode::mtmsr: case Opcode::mtmsrd: Line(std::format("ctx->msr = {};", R(0))); return true;

                // ---------------------------------------------------- logica do CR
                case Opcode::crand: CrOp("{a} & {b}"); return true;
                case Opcode::crandc: CrOp("{a} & !{b}"); return true;
                case Opcode::cror: CrOp("{a} | {b}"); return true;
                case Opcode::crorc: CrOp("{a} | !{b}"); return true;
                case Opcode::crxor: CrOp("{a} ^ {b}"); return true;
                case Opcode::crnand: CrOp("!({a} & {b})"); return true;
                case Opcode::crnor: CrOp("!({a} | {b})"); return true;
                case Opcode::creqv: CrOp("!({a} ^ {b})"); return true;
                case Opcode::mcrf: Line(std::format("ctx->cr[{}] = ctx->cr[{}];", Val(0), Val(1))); return true;

                // ---------------------------------------------------- ponto flutuante
                case Opcode::fadd: Line(std::format("{} = {} + {};", F(0), F(1), F(2))); return true;
                case Opcode::fsub: Line(std::format("{} = {} - {};", F(0), F(1), F(2))); return true;
                case Opcode::fmul: Line(std::format("{} = {} * {};", F(0), F(1), F(2))); return true;
                case Opcode::fdiv: Line(std::format("{} = {} / {};", F(0), F(1), F(2))); return true;
                case Opcode::fadds: Line(std::format("{} = predator_round_single({} + {});", F(0), F(1), F(2))); return true;
                case Opcode::fsubs: Line(std::format("{} = predator_round_single({} - {});", F(0), F(1), F(2))); return true;
                case Opcode::fmuls: Line(std::format("{} = predator_round_single({} * {});", F(0), F(1), F(2))); return true;
                case Opcode::fdivs: Line(std::format("{} = predator_round_single({} / {});", F(0), F(1), F(2))); return true;
                case Opcode::fmadd: Line(std::format("{} = {} * {} + {};", F(0), F(1), F(2), F(3))); return true;
                case Opcode::fmsub: Line(std::format("{} = {} * {} - {};", F(0), F(1), F(2), F(3))); return true;
                case Opcode::fnmadd: Line(std::format("{} = -({} * {} + {});", F(0), F(1), F(2), F(3))); return true;
                case Opcode::fnmsub: Line(std::format("{} = -({} * {} - {});", F(0), F(1), F(2), F(3))); return true;
                case Opcode::fmadds: Line(std::format("{} = predator_round_single({} * {} + {});", F(0), F(1), F(2), F(3))); return true;
                case Opcode::fmsubs: Line(std::format("{} = predator_round_single({} * {} - {});", F(0), F(1), F(2), F(3))); return true;
                case Opcode::fnmadds: Line(std::format("{} = predator_round_single(-({} * {} + {}));", F(0), F(1), F(2), F(3))); return true;
                case Opcode::fnmsubs: Line(std::format("{} = predator_round_single(-({} * {} - {}));", F(0), F(1), F(2), F(3))); return true;
                case Opcode::fsel: Line(std::format("{} = {} >= 0.0 ? {} : {};", F(0), F(1), F(2), F(3))); return true;
                case Opcode::fsqrt: Line(std::format("{} = sqrt({});", F(0), F(1))); return true;
                case Opcode::fsqrts: Line(std::format("{} = predator_round_single(sqrt({}));", F(0), F(1))); return true;
                case Opcode::fres: Line(std::format("{} = predator_round_single(1.0 / {});", F(0), F(1))); return true;
                case Opcode::frsqrte: Line(std::format("{} = 1.0 / sqrt({});", F(0), F(1))); return true;
                case Opcode::frsp: Line(std::format("{} = predator_round_single({});", F(0), F(1))); return true;
                case Opcode::fneg: Line(std::format("{} = {} ^ 0x8000000000000000ULL;", FU(0), FU(1))); return true;
                case Opcode::fabs_: Line(std::format("{} = {} & 0x7FFFFFFFFFFFFFFFULL;", FU(0), FU(1))); return true;
                case Opcode::fnabs: Line(std::format("{} = {} | 0x8000000000000000ULL;", FU(0), FU(1))); return true;
                case Opcode::fmr: Line(std::format("{} = {};", FU(0), FU(1))); return true;
                case Opcode::fctiw: Line(std::format("{} = (uint64_t)(uint32_t)predator_f64_to_s32(rint({}));", FU(0), F(1))); return true;
                case Opcode::fctiwz: Line(std::format("{} = (uint64_t)(uint32_t)predator_f64_to_s32({});", FU(0), F(1))); return true;
                case Opcode::fctid: Line(std::format("{} = (uint64_t)predator_f64_to_s64(rint({}));", FU(0), F(1))); return true;
                case Opcode::fctidz: Line(std::format("{} = (uint64_t)predator_f64_to_s64({});", FU(0), F(1))); return true;
                case Opcode::fcfid: Line(std::format("{} = (double)(int64_t){};", F(0), FU(1))); return true;
                case Opcode::fcmpu: case Opcode::fcmpo: Line(std::format("predator_cmp_f64(&ctx->cr[{}], {}, {});", Val(0), F(1), F(2))); return true;
                case Opcode::mffs: Line(std::format("{} = ctx->fpscr;", FU(0))); return true;
                case Opcode::mtfsf: Line(std::format("ctx->fpscr = (uint32_t){};", FU(1))); return true;
                case Opcode::mtfsfi:
                {
                    const auto shift = 28 - 4 * Val(0);
                    Line(std::format("ctx->fpscr = (ctx->fpscr & ~(0xFu << {0})) | ({1}u << {0});", shift, Val(1)));
                    return true;
                }
                case Opcode::mtfsb0: Line(std::format("ctx->fpscr &= ~(0x80000000u >> {});", Val(0))); return true;
                case Opcode::mtfsb1: Line(std::format("ctx->fpscr |= 0x80000000u >> {};", Val(0))); return true;

                // ---------------------------------------------------- vetores: memoria
                case Opcode::lvx: case Opcode::lvxl: case Opcode::lvx128: case Opcode::lvxl128:
                    Line(std::format("predator_lvx({}, base, {});", V(0), EAX(1, 2))); return true;
                case Opcode::stvx: case Opcode::stvxl: case Opcode::stvx128: case Opcode::stvxl128:
                    Line(std::format("predator_stvx({}, base, {});", V(0), EAX(1, 2))); return true;
                case Opcode::lvlx: case Opcode::lvlxl: case Opcode::lvlx128: case Opcode::lvlxl128:
                    Line(std::format("predator_lvlx({}, base, {});", V(0), EAX(1, 2))); return true;
                case Opcode::lvrx: case Opcode::lvrxl: case Opcode::lvrx128: case Opcode::lvrxl128:
                    Line(std::format("predator_lvrx({}, base, {});", V(0), EAX(1, 2))); return true;
                case Opcode::stvlx: case Opcode::stvlxl: case Opcode::stvlx128: case Opcode::stvlxl128:
                    Line(std::format("predator_stvlx({}, base, {});", V(0), EAX(1, 2))); return true;
                case Opcode::stvrx: case Opcode::stvrxl: case Opcode::stvrx128: case Opcode::stvrxl128:
                    Line(std::format("predator_stvrx({}, base, {});", V(0), EAX(1, 2))); return true;
                case Opcode::lvsl: case Opcode::lvsl128: Line(std::format("predator_lvsl({}, {});", V(0), EAX(1, 2))); return true;
                case Opcode::lvsr: case Opcode::lvsr128: Line(std::format("predator_lvsr({}, {});", V(0), EAX(1, 2))); return true;
                case Opcode::lvewx: case Opcode::lvewx128: Line(std::format("predator_lvewx({}, base, {});", V(0), EAX(1, 2))); return true;
                case Opcode::stvewx: case Opcode::stvewx128: Line(std::format("predator_stvewx({}, base, {});", V(0), EAX(1, 2))); return true;
                case Opcode::lvebx: Line(std::format("{{ uint32_t ea_ = {}; ctx->v[{}].b[ea_ & 15] = predator_ld8(base, ea_); }}", EAX(1, 2), Val(0))); return true;
                case Opcode::lvehx: Line(std::format("{{ uint32_t ea_ = {} & ~1u; memcpy(&ctx->v[{}].b[ea_ & 14], PREDATOR_PTR(base, ea_), 2); }}", EAX(1, 2), Val(0))); return true;
                case Opcode::stvebx: Line(std::format("{{ uint32_t ea_ = {}; predator_st8(base, ea_, ctx->v[{}].b[ea_ & 15]); }}", EAX(1, 2), Val(0))); return true;
                case Opcode::stvehx: Line(std::format("{{ uint32_t ea_ = {} & ~1u; memcpy(PREDATOR_PTR(base, ea_), &ctx->v[{}].b[ea_ & 14], 2); }}", EAX(1, 2), Val(0))); return true;
                case Opcode::mfvscr: Line(std::format("memset({0}, 0, 16); predator_vset_u32({0}, 3, ctx->vscr);", V(0))); return true;
                case Opcode::mtvscr: Line(std::format("ctx->vscr = predator_vget_u32({}, 3);", V(0))); return true;

                // ---------------------------------------------------- vetores: float
                case Opcode::vaddfp: case Opcode::vaddfp128: VFp(1, 2, 2, "a + b"); return true;
                case Opcode::vsubfp: case Opcode::vsubfp128: VFp(1, 2, 2, "a - b"); return true;
                case Opcode::vmulfp128: VFp(1, 2, 2, "a * b"); return true;
                case Opcode::vmaxfp: case Opcode::vmaxfp128: VFp(1, 2, 2, "a > b ? a : b"); return true;
                case Opcode::vminfp: case Opcode::vminfp128: VFp(1, 2, 2, "a < b ? a : b"); return true;
                case Opcode::vmaddfp: VFp(1, 3, 2, "a * c + b"); return true;          // vD = vA * vC + vB
                case Opcode::vnmsubfp: VFp(1, 3, 2, "-(a * c - b)"); return true;
                case Opcode::vmaddfp128: VFp(1, 2, 0, "a * b + c"); return true;       // vD = vA * vB + vD
                case Opcode::vmaddcfp128: VFp(1, 3, 2, "a * c + b"); return true;      // vD = vA * vD + vB (o2 = vD)
                case Opcode::vnmsubfp128: VFp(1, 2, 0, "-(a * b - c)"); return true;   // vD = -(vA * vB - vD)
                case Opcode::vrefp: case Opcode::vrefp128: VFp(1, 1, 1, "1.0f / a"); return true;
                case Opcode::vrsqrtefp: case Opcode::vrsqrtefp128: VFp(1, 1, 1, "predator_rsqrt(a)"); return true;
                case Opcode::vexptefp: case Opcode::vexptefp128: VFp(1, 1, 1, "exp2f(a)"); return true;
                case Opcode::vlogefp: case Opcode::vlogefp128: VFp(1, 1, 1, "log2f(a)"); return true;
                case Opcode::vrfin: case Opcode::vrfin128: VFp(1, 1, 1, "nearbyintf(a)"); return true;
                case Opcode::vrfiz: case Opcode::vrfiz128: VFp(1, 1, 1, "truncf(a)"); return true;
                case Opcode::vrfip: case Opcode::vrfip128: VFp(1, 1, 1, "ceilf(a)"); return true;
                case Opcode::vrfim: case Opcode::vrfim128: VFp(1, 1, 1, "floorf(a)"); return true;
                case Opcode::vmsum3fp128:
                case Opcode::vmsum4fp128:
                {
                    const int n = insn.opcode == Opcode::vmsum3fp128 ? 3 : 4;
                    Line(std::format("{{ float s_ = 0; for (int i_ = 0; i_ < {}; ++i_) s_ += predator_vget_f32({}, i_) * predator_vget_f32({}, i_); "
                                     "for (int i_ = 0; i_ < 4; ++i_) predator_vset_f32({}, i_, s_); }}", n, V(1), V(2), V(0)));
                    return true;
                }
                case Opcode::vcfsx: case Opcode::vcsxwfp128:
                    VConvert("(float)(int32_t)predator_vget_u32", "/", true); return true;
                case Opcode::vcfux: case Opcode::vcuxwfp128:
                    VConvert("(float)predator_vget_u32", "/", true); return true;
                case Opcode::vctsxs: case Opcode::vcfpsxws128:
                    VConvert("predator_f32_to_s32_sat", "*", false); return true;
                case Opcode::vctuxs: case Opcode::vcfpuxws128:
                    VConvert("predator_f32_to_u32_sat", "*", false); return true;

                // ---------------------------------------------------- vetores: comparacoes
                case Opcode::vcmpeqfp: case Opcode::vcmpeqfp128: VCmp("float", "predator_vget_f32", "a == b"); return true;
                case Opcode::vcmpgefp: case Opcode::vcmpgefp128: VCmp("float", "predator_vget_f32", "a >= b"); return true;
                case Opcode::vcmpgtfp: case Opcode::vcmpgtfp128: VCmp("float", "predator_vget_f32", "a > b"); return true;
                case Opcode::vcmpequw: case Opcode::vcmpequw128: VCmp("uint32_t", "predator_vget_u32", "a == b"); return true;
                case Opcode::vcmpgtuw: VCmp("uint32_t", "predator_vget_u32", "a > b"); return true;
                case Opcode::vcmpgtsw: VCmp("int32_t", "(int32_t)predator_vget_u32", "a > b"); return true;
                case Opcode::vcmpbfp: case Opcode::vcmpbfp128:
                    VCmp("float", "predator_vget_f32", "0", "((a <= b) ? 0u : 0x80000000u) | ((a >= -b) ? 0u : 0x40000000u)");
                    return true;

                // ---------------------------------------------------- vetores: logica e inteiros
                case Opcode::vand: case Opcode::vand128: VBytes("a & b"); return true;
                case Opcode::vandc: case Opcode::vandc128: VBytes("a & ~b"); return true;
                case Opcode::vor: case Opcode::vor128: VBytes("a | b"); return true;
                case Opcode::vxor: case Opcode::vxor128: VBytes("a ^ b"); return true;
                case Opcode::vnor: case Opcode::vnor128: VBytes("~(a | b)"); return true;
                case Opcode::vsel: case Opcode::vsel128:
                    Line(std::format("{{ PredatorVr t_; for (int i_ = 0; i_ < 16; ++i_) t_.b[i_] = (uint8_t)((ctx->v[{1}].b[i_] & ~ctx->v[{3}].b[i_]) | (ctx->v[{2}].b[i_] & ctx->v[{3}].b[i_])); ctx->v[{0}] = t_; }}",
                                     Val(0), Val(1), Val(2), Val(3)));
                    return true;
                case Opcode::vperm: case Opcode::vperm128: Line(std::format("predator_vperm({}, {}, {}, {});", V(0), V(1), V(2), V(3))); return true;
                case Opcode::vsldoi: case Opcode::vsldoi128: Line(std::format("predator_vsldoi({}, {}, {}, {});", V(0), V(1), V(2), Val(3))); return true;
                case Opcode::vadduwm: VU32("a + b"); return true;
                case Opcode::vsubuwm: VU32("a - b"); return true;
                case Opcode::vslw: case Opcode::vslw128: VU32("a << (b & 31)"); return true;
                case Opcode::vsrw: case Opcode::vsrw128: VU32("a >> (b & 31)"); return true;
                case Opcode::vsraw: case Opcode::vsraw128: VU32("(uint32_t)((int32_t)a >> (b & 31))"); return true;
                case Opcode::vrlw: case Opcode::vrlw128: VU32("predator_rotl32(a, b)"); return true;
                case Opcode::vmaxuw: VU32("a > b ? a : b"); return true;
                case Opcode::vminuw: VU32("a < b ? a : b"); return true;
                case Opcode::vmaxsw: VU32("(int32_t)a > (int32_t)b ? a : b"); return true;
                case Opcode::vminsw: VU32("(int32_t)a < (int32_t)b ? a : b"); return true;
                case Opcode::vmrghw: case Opcode::vmrghw128: VMerge(0); return true;
                case Opcode::vmrglw: case Opcode::vmrglw128: VMerge(2); return true;
                case Opcode::vspltw: case Opcode::vspltw128:
                    Line(std::format("{{ uint32_t x_ = predator_vget_u32({}, {}); for (int i_ = 0; i_ < 4; ++i_) predator_vset_u32({}, i_, x_); }}", V(1), Val(2) & 3, V(0)));
                    return true;
                case Opcode::vsplth:
                    Line(std::format("{{ uint16_t x_ = predator_vget_u16({}, {}); for (int i_ = 0; i_ < 8; ++i_) predator_vset_u16({}, i_, x_); }}", V(1), Val(2) & 7, V(0)));
                    return true;
                case Opcode::vspltb:
                    Line(std::format("memset(ctx->v[{}].b, ctx->v[{}].b[{}], 16);", Val(0), Val(1), Val(2) & 15));
                    return true;
                case Opcode::vspltisw: case Opcode::vspltisw128:
                    Line(std::format("for (int i_ = 0; i_ < 4; ++i_) predator_vset_u32({}, i_, (uint32_t){});", V(0), Val(1)));
                    return true;
                case Opcode::vspltish:
                    Line(std::format("for (int i_ = 0; i_ < 8; ++i_) predator_vset_u16({}, i_, (uint16_t){});", V(0), Val(1)));
                    return true;
                case Opcode::vspltisb:
                    Line(std::format("memset(ctx->v[{}].b, (uint8_t){}, 16);", Val(0), Val(1)));
                    return true;
                case Opcode::vaddubm: VBytes("a + b"); return true;
                case Opcode::vslb: VBytes("a << (b & 7)"); return true;
                case Opcode::vavgsb: VBytes("((int)(int8_t)a + (int)(int8_t)b + 1) >> 1"); return true;
                case Opcode::vavgsh:
                    Line(std::format("{{ PredatorVr t_; for (int i_ = 0; i_ < 8; ++i_) predator_vset_u16(&t_, i_, (uint16_t)(((int)(int16_t)predator_vget_u16({1}, i_) + (int)(int16_t)predator_vget_u16({2}, i_) + 1) >> 1)); ctx->v[{0}] = t_; }}",
                                     Val(0), V(1), V(2)));
                    return true;
                case Opcode::vadduws: VU32("a + b < a ? 0xFFFFFFFFu : a + b"); return true;
                case Opcode::vsl: Line(std::format("predator_vshift_left({}, {}, ctx->v[{}].b[15] & 7);", V(0), V(1), Val(2))); return true;
                case Opcode::vsr: Line(std::format("predator_vshift_right({}, {}, ctx->v[{}].b[15] & 7);", V(0), V(1), Val(2))); return true;
                case Opcode::vmrghb: case Opcode::vmrglb:
                    Line(std::format("{{ PredatorVr a_ = ctx->v[{1}], b_ = ctx->v[{2}]; for (int i_ = 0; i_ < 8; ++i_) {{ ctx->v[{0}].b[2 * i_] = a_.b[{3} + i_]; ctx->v[{0}].b[2 * i_ + 1] = b_.b[{3} + i_]; }} }}",
                                     Val(0), Val(1), Val(2), insn.opcode == Opcode::vmrghb ? 0 : 8));
                    return true;
                case Opcode::vmrghh: case Opcode::vmrglh:
                    Line(std::format("{{ PredatorVr a_ = ctx->v[{1}], b_ = ctx->v[{2}]; for (int i_ = 0; i_ < 4; ++i_) {{ predator_vset_u16({0}, 2 * i_, predator_vget_u16(&a_, {3} + i_)); predator_vset_u16({0}, 2 * i_ + 1, predator_vget_u16(&b_, {3} + i_)); }} }}",
                                     V(0), Val(1), Val(2), insn.opcode == Opcode::vmrghh ? 0 : 4));
                    return true;
                case Opcode::vupkhsb: case Opcode::vupklsb:
                    Line(std::format("{{ PredatorVr s_ = ctx->v[{1}]; for (int i_ = 0; i_ < 8; ++i_) predator_vset_u16({0}, i_, (uint16_t)(int16_t)(int8_t)s_.b[{2} + i_]); }}",
                                     V(0), Val(1), insn.opcode == Opcode::vupkhsb ? 0 : 8));
                    return true;
                case Opcode::vupkhsh: case Opcode::vupklsh:
                    Line(std::format("{{ PredatorVr s_ = ctx->v[{1}]; for (int i_ = 0; i_ < 4; ++i_) predator_vset_u32({0}, i_, (uint32_t)(int32_t)(int16_t)predator_vget_u16(&s_, {2} + i_)); }}",
                                     V(0), Val(1), insn.opcode == Opcode::vupkhsh ? 0 : 4));
                    return true;
                case Opcode::vpkswss:
                    Line(std::format("{{ PredatorVr a_ = ctx->v[{1}], b_ = ctx->v[{2}]; for (int i_ = 0; i_ < 8; ++i_) {{ int32_t x_ = (int32_t)predator_vget_u32(i_ < 4 ? &a_ : &b_, i_ & 3); "
                                     "predator_vset_u16({0}, i_, (uint16_t)(int16_t)(x_ > 32767 ? 32767 : x_ < -32768 ? -32768 : x_)); }} }}",
                                     V(0), Val(1), Val(2)));
                    return true;
                case Opcode::vupkd3d128:
                    Line(std::format("{{ PredatorVr s_ = ctx->v[{1}]; if (!predator_vupkd3d({0}, &s_, {2}u)) predator_unimplemented(ctx, base, 0x{3:08X}u, \"vupkd3d128\"); }}",
                                     V(0), Val(1), Val(2) >> 2, insn.address));
                    return true;
                case Opcode::vpkd3d128:
                    Line(std::format("{{ PredatorVr s_ = ctx->v[{1}]; if (!predator_vpkd3d({0}, &s_, {2}u, {3}u, {4}u)) predator_unimplemented(ctx, base, 0x{5:08X}u, \"vpkd3d128\"); }}",
                                     V(0), Val(1), Val(2), Val(3), Val(4), insn.address));
                    return true;
                case Opcode::vrlimi128:
                {
                    // gira vB 'z' palavras para a esquerda e copia para vD as palavras marcadas em IMM (bit 3 = palavra 0)
                    const auto imm = Val(2), z = Val(3);
                    Line(std::format("{{ PredatorVr s_ = ctx->v[{1}]; for (int i_ = 0; i_ < 4; ++i_) if ({2} & (8 >> i_)) predator_vset_u32({0}, i_, predator_vget_u32(&s_, (i_ + {3}) & 3)); }}",
                                     V(0), Val(1), imm, z));
                    return true;
                }
                case Opcode::vpermwi128:
                {
                    const auto p = Val(2);
                    Line(std::format("{{ PredatorVr s_ = ctx->v[{1}]; predator_vset_u32({0}, 0, predator_vget_u32(&s_, {2})); predator_vset_u32({0}, 1, predator_vget_u32(&s_, {3})); "
                                     "predator_vset_u32({0}, 2, predator_vget_u32(&s_, {4})); predator_vset_u32({0}, 3, predator_vget_u32(&s_, {5})); }}",
                                     V(0), Val(1), (p >> 6) & 3, (p >> 4) & 3, (p >> 2) & 3, p & 3));
                    return true;
                }

                default:
                    if (!insn.Valid())
                    {
                        ++m_stats.dataWords;
                        Line(std::format("predator_unimplemented(ctx, base, 0x{:08X}u, \"dado no meio do codigo\");", insn.address));
                        return true;
                    }
                    Unimplemented();
                    return true;
                }
            }

            // ------------------------------------------------------------ auxiliares de traducao
            static std::uint64_t Mask(std::int64_t mb, std::int64_t me)
            {
                const auto b = static_cast<unsigned>(mb & 63), e = static_cast<unsigned>(me & 63);
                const std::uint64_t a = ~0ull >> b, c = ~0ull << (63 - e);
                return b <= e ? (a & c) : (a | c);
            }

            // add/subf/neg: resultado em o0; com OE marca estouro (sinais iguais na entrada, diferente no resultado)
            void Arith(const std::string& expr, const std::string& x, const std::string& y, bool)
            {
                if (m_insn->Has(kFlagOE))
                    Line(std::format("{{ uint64_t x_ = {}, y_ = {}, r_ = {}; ctx->xer_ov = (uint8_t)((((x_ ^ r_) & (y_ ^ r_)) >> 31) & 1); ctx->xer_so |= ctx->xer_ov; {} = r_; }}",
                                     x, y, expr, R(0)));
                else
                    Line(std::format("{} = {};", R(0), expr));
                Rc(R(0));
            }

            // adde/subfe/addze/subfze/addme/subfme: a + b + CA; o novo CA e o "vai um" da conta de 32 bits (modo 32 bits)
            void ExtendedAdd(const std::string& a, const std::string& b)
            {
                Line(std::format("{{ uint64_t a_ = {}, b_ = {}, c_ = ctx->xer_ca; ctx->xer_ca = (uint8_t)((((uint64_t)(uint32_t)a_ + (uint32_t)b_ + c_) >> 32) & 1); {} = a_ + b_ + c_; }}",
                                 a, b, R(0)));
                Rc(R(0));
            }

            // addi/addis: "li"/"lis" quando rA = 0
            void AddImmediate(std::int64_t value)
            {
                const std::string ra = RZ(1);
                if (ra == "0") Line(std::format("{} = {};", R(0), S64(value)));
                else Line(std::format("{} = {} + {};", R(0), ra, S64(value)));
            }

            void Trap(std::uint32_t to, const std::string& a, const std::string& b, bool word)
            {
                const std::string sa = word ? "(int32_t)" + a : "(int64_t)" + a;
                const std::string sb = word ? "(int32_t)(" + b + ")" : "(int64_t)(" + b + ")";
                const std::string ua = word ? "(uint32_t)" + a : "(uint64_t)" + a;
                const std::string ub = word ? "(uint32_t)(" + b + ")" : "(uint64_t)(" + b + ")";
                std::vector<std::string> parts;
                if (to & 16) parts.push_back(sa + " < " + sb);
                if (to & 8) parts.push_back(sa + " > " + sb);
                if (to & 4) parts.push_back(sa + " == " + sb);
                if (to & 2) parts.push_back(ua + " < " + ub);
                if (to & 1) parts.push_back(ua + " > " + ub);
                if (parts.empty()) return;
                std::string cond;
                for (std::size_t i = 0; i < parts.size(); ++i) cond += (i ? " || " : "") + std::string("(") + parts[i] + ")";
                Line(std::format("if ({}) predator_trap(ctx, base, 0x{:08X}u);", cond, m_insn->address));
            }

            // forma D com atualizacao: ea = rA + d; operacao; rA = ea
            void Update(const std::string& op)
            {
                Line(std::format("{{ uint32_t ea_ = (uint32_t)({} + {}); {} {} = ea_; }}", BaseReg(1), S64(m_insn->operands[1].value), op, BaseReg(1)));
            }
            // forma X com atualizacao: ea = rA + rB; operacao; rA = ea
            void UpdateX(const std::string& op)
            {
                Line(std::format("{{ uint32_t ea_ = (uint32_t)({} + {}); {} {} = ea_; }}", R(1), R(2), op, R(1)));
            }

            void CrOp(const std::string& pattern)
            {
                std::string expr = pattern;
                auto replace = [&](const std::string& key, const std::string& value) {
                    for (std::size_t p; (p = expr.find(key)) != std::string::npos;) expr.replace(p, key.size(), value);
                };
                replace("{a}", std::format("*predator_crbit(ctx, {})", Val(1)));
                replace("{b}", std::format("*predator_crbit(ctx, {})", Val(2)));
                Line(std::format("*predator_crbit(ctx, {}) = (uint8_t)(({}) & 1);", Val(0), expr));
            }

            void VFp(int a, int b, int c, const std::string& expr)
            {
                Line(std::format("PREDATOR_VFP3({}, {}, {}, {}, {});", V(0), V(a), V(b), V(c), expr));
            }
            void VU32(const std::string& expr) { Line(std::format("PREDATOR_VU32({}, {}, {}, {});", V(0), V(1), V(2), expr)); }
            void VBytes(const std::string& expr)
            {
                Line(std::format("{{ PredatorVr t_; for (int i_ = 0; i_ < 16; ++i_) {{ uint8_t a = ctx->v[{1}].b[i_], b = ctx->v[{2}].b[i_]; t_.b[i_] = (uint8_t)({3}); }} ctx->v[{0}] = t_; }}",
                                 Val(0), Val(1), Val(2), expr));
            }
            void VMerge(int first)
            {
                Line(std::format("{{ PredatorVr a_ = ctx->v[{1}], b_ = ctx->v[{2}]; predator_vset_u32({0}, 0, predator_vget_u32(&a_, {3})); predator_vset_u32({0}, 1, predator_vget_u32(&b_, {3})); "
                                 "predator_vset_u32({0}, 2, predator_vget_u32(&a_, {4})); predator_vset_u32({0}, 3, predator_vget_u32(&b_, {4})); }}",
                                 V(0), Val(1), Val(2), first, first + 1));
            }
            // conversoes com escala 2^uimm: int -> float divide, float -> int multiplica
            void VConvert(const std::string& fn, const std::string& op, bool toFloat)
            {
                const double scale = static_cast<double>(1u << (Val(2) & 31));
                if (toFloat)
                    Line(std::format("for (int i_ = 0; i_ < 4; ++i_) predator_vset_f32({0}, i_, {1}({2}, i_) {3} {4:.1f}f);", V(0), fn, V(1), op, scale));
                else
                    Line(std::format("for (int i_ = 0; i_ < 4; ++i_) predator_vset_u32({0}, i_, (uint32_t){1}(predator_vget_f32({2}, i_) {3} {4:.1f}f));", V(0), fn, V(1), op, scale));
            }
            // comparacao elemento a elemento; 'value' (opcional) troca a mascara de todos-os-bits pelo valor calculado
            void VCmp(const std::string& type, const std::string& get, const std::string& cond, const std::string& value = {})
            {
                const std::string result = value.empty() ? std::format("({}) ? 0xFFFFFFFFu : 0u", cond) : value;
                Line(std::format("{{ PredatorVr t_; for (int i_ = 0; i_ < 4; ++i_) {{ {1} a = {2}({3}, i_), b = {2}({4}, i_); predator_vset_u32(&t_, i_, {5}); }} ctx->v[{0}] = t_; predator_vcmp_result(ctx, &t_, {6}); }}",
                                 Val(0), type, get, V(1), V(2), result, m_insn->Has(kFlagRc) ? 1 : 0));
            }

            const loader::Executable& m_exe;
            const ProgramAnalysis& m_analysis;
            const Function& m_f;
            CodeGenStats& m_stats;
            bool m_comments;
            std::set<std::uint32_t> m_labels;
            const Instruction* m_insn = nullptr;
            std::string m_out;
            std::string m_code;
            bool m_local = false;          // registradores em variaveis locais (CollectRegisters)
            std::set<int> m_gprs, m_fprs;  // registradores usados pela funcao
            bool m_ctr = false;            // a funcao usa o CTR
        };

        bool WriteFile(const std::filesystem::path& path, std::string_view text, CodeGenStats& stats, Diagnostics& diag)
        {
            std::ofstream out(path, std::ios::binary);
            if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size())))
            {
                diag.Error("nao foi possivel gravar " + path.string());
                return false;
            }
            stats.bytes += text.size();
            return true;
        }
    }

    std::string GenerateFunction(const loader::Executable& exe, const ProgramAnalysis& analysis, const Function& function, CodeGenStats& stats,
                                 bool comments)
    {
        return FunctionEmitter(exe, analysis, function, stats, comments).Emit();
    }

    bool GenerateCode(const loader::Executable& exe, const ProgramAnalysis& analysis, const CodeGenSettings& settings, CodeGenStats& stats,
                      Diagnostics& diag)
    {
        std::error_code ec;
        std::filesystem::create_directories(settings.outputDir, ec);
        if (ec)
        {
            diag.Error("nao foi possivel criar " + settings.outputDir.string() + ": " + ec.message());
            return false;
        }

        if (!WriteFile(settings.outputDir / "predator_cpu.h", RuntimeHeader(), stats, diag)) return false;

        // prototipos
        std::string protos = "/* gerado pelo PredatorRecomp: prototipos de todas as funcoes */\n#ifndef PREDATOR_FUNCTIONS_H\n#define PREDATOR_FUNCTIONS_H\n"
                             "#include \"predator_cpu.h\"\n\n";
        for (const auto& f : analysis.functions)
            protos += std::format("void {0}(PredatorCpuContext* ctx, uint8_t* base);\nvoid {0}_original(PredatorCpuContext* ctx, uint8_t* base);\n", FuncName(f.start));
        protos += "\n#endif\n";
        if (!WriteFile(settings.outputDir / "predator_functions.h", protos, stats, diag)) return false;

        // tabela endereco -> funcao
        std::string table = "/* gerado pelo PredatorRecomp: funcoes por endereco (ordenadas) */\n#include \"predator_functions.h\"\n\n"
                            "typedef struct PredatorFunctionEntry { uint32_t address; PredatorFunction function; } PredatorFunctionEntry;\n\n";
        table += std::format("const uint32_t predator_function_count = {};\n", analysis.functions.size());
        table += "const PredatorFunctionEntry predator_functions[] = {\n";
        for (const auto& f : analysis.functions) table += std::format("    {{ 0x{:08X}u, {} }},\n", f.start, FuncName(f.start));
        table += "    { 0, 0 }\n};\n";   // terminador
        if (!WriteFile(settings.outputDir / "predator_table.c", table, stats, diag)) return false;

        // codigo
        std::string file;
        std::uint64_t inFile = 0;
        auto flush = [&]() -> bool {
            if (file.empty()) return true;
            const auto name = std::format("predator_code_{:03}.c", stats.files);
            ++stats.files;
            const std::string text = "/* gerado pelo PredatorRecomp */\n#include \"predator_functions.h\"\n\n" + file;
            file.clear();
            inFile = 0;
            return WriteFile(settings.outputDir / name, text, stats, diag);
        };
        for (const auto& f : analysis.functions)
        {
            const std::uint64_t before = stats.instructions;
            file += GenerateFunction(exe, analysis, f, stats, settings.comments);
            inFile += stats.instructions - before;
            if (inFile >= settings.instructionsPerFile && !flush()) return false;
        }
        return flush();
    }
}
