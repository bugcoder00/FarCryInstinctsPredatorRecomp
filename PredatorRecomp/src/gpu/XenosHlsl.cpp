#include "gpu/XenosHlsl.h"

#include "gpu/XenosMicrocode.h"

#include <algorithm>
#include <format>
#include <string_view>

namespace predator::gpu
{
    namespace
    {
        constexpr char kComponent[] = "xyzw";
        constexpr std::string_view kFltMax = "3.402823466e+38";

        // componente i de um swizzle de ALU (2 bits por componente, relativos a posicao)
        unsigned AluComponent(std::uint32_t swizzle, unsigned i) { return ((swizzle >> (2 * i)) + i) & 3; }

        std::string ComponentMask(std::uint32_t mask)
        {
            std::string s;
            for (unsigned i = 0; i < 4; ++i)
                if (mask & (1u << i)) s += kComponent[i];
            return s;
        }

        std::string Bool(bool b) { return b ? "true" : "false"; }

        class Emitter
        {
        public:
            Emitter(const Microcode& microcode, const HlslOptions& options, HlslShader& out)
                : m_mc(microcode), m_opt(options), m_out(out)
            {
            }

            void Run()
            {
                const auto& cf = m_mc.controlFlow;
                bool needsLoop = m_opt.forceLoop;
                bool hasJump = false;
                for (std::size_t i = 0; i < cf.size(); ++i)
                {
                    switch (cf[i].opcode)
                    {
                    case CfOpcode::LoopStart: case CfOpcode::LoopEnd: case CfOpcode::CondCall: case CfOpcode::Return:
                        needsLoop = true;
                        break;
                    case CfOpcode::CondJump:
                        hasJump = true;
                        if (cf[i].address <= i) needsLoop = true;   // salto para tras
                        break;
                    default:
                        break;
                    }
                }
                m_out.loopMode = needsLoop;

                Line("bool p0 = false;");
                Line("int a0 = 0, aL = 0;");
                Line("float ps = 0.0;");
                if (needsLoop) EmitStateMachine();
                else EmitStraight(hasJump);
            }

        private:
            // ------------------------------------------------------------ saida de texto
            void Line(std::string_view s)
            {
                m_out.body.append(static_cast<std::size_t>(m_indent), '\t');
                m_out.body += s;
                m_out.body += '\n';
            }
            void Open(std::string_view s) { Line(s); ++m_indent; }
            void Close(std::string_view s = "}") { --m_indent; Line(s); }

            void Warn(std::string w)
            {
                if (std::find(m_out.warnings.begin(), m_out.warnings.end(), w) == m_out.warnings.end())
                    m_out.warnings.push_back(std::move(w));
            }

            // ------------------------------------------------------------ controle de fluxo
            // Sem lacos, chamadas nem saltos para tras: os blocos saem em sequencia. Um salto para frente so muda
            // "skip" (os blocos com indice menor que ele sao pulados); um fim de programa desliga "run".
            void EmitStraight(bool hasJump)
            {
                if (hasJump) Line("uint skip = 0u; bool run = true;");
                const auto& cf = m_mc.controlFlow;
                for (std::size_t i = 0; i < cf.size(); ++i)
                {
                    const CfInstruction& c = cf[i];
                    if (c.opcode == CfOpcode::Nop || c.opcode == CfOpcode::Alloc || c.opcode == CfOpcode::MarkVsFetchDone) continue;
                    if (hasJump) Open(std::format("if (run && skip <= {}u) {{", i));
                    if (c.IsExec()) EmitExec(c);
                    else if (c.opcode == CfOpcode::CondJump) Line(std::format("if ({}) skip = {}u;", JumpCondition(c), c.address));
                    else Warn(std::format("controle de fluxo {} ignorado", static_cast<int>(c.opcode)));
                    if (c.EndsProgram() && hasJump) Line("run = false;");
                    if (hasJump) Close();
                    if (c.EndsProgram() && !hasJump) return;   // o resto nao e alcancado
                }
            }

            // Lacos, chamadas ou saltos para tras: um laco com "switch (pc)", um caso por instrucao de controle de fluxo.
            void EmitStateMachine()
            {
                const auto& cf = m_mc.controlFlow;
                int maxDepth = 0, depth = 0;
                for (const auto& c : cf)
                {
                    if (c.opcode == CfOpcode::LoopStart) maxDepth = std::max(maxDepth, ++depth);
                    if (c.opcode == CfOpcode::LoopEnd && depth > 0) --depth;
                }
                for (int d = 0; d < maxDepth; ++d) Line(std::format("uint loopLeft{0} = 0u; int loopStep{0} = 0, savedAL{0} = 0;", d));
                Line("uint pc = 0u, retStack[4] = { 0u, 0u, 0u, 0u }, retDepth = 0u;");
                Open("[loop] for (uint guard = 0u; guard < 65536u; ++guard) {");
                Line("bool stop = false;");
                Open("switch (pc) {");
                depth = 0;
                for (std::size_t i = 0; i < cf.size(); ++i)
                {
                    const CfInstruction& c = cf[i];
                    Open(std::format("case {}u: {{", i));
                    Line(std::format("pc = {}u;", i + 1));
                    if (c.IsExec())
                    {
                        EmitExec(c);
                        if (c.EndsProgram()) Line("stop = true;");
                    }
                    else switch (c.opcode)
                    {
                    case CfOpcode::Nop: case CfOpcode::Alloc: case CfOpcode::MarkVsFetchDone:
                        break;
                    case CfOpcode::CondJump:
                        Line(std::format("if ({}) pc = {}u;", JumpCondition(c), c.address));
                        break;
                    case CfOpcode::LoopStart:
                    {
                        // constante de laco: contagem (bits 0-7), valor inicial de aL (8-15), passo (16-23, com sinal)
                        const int d = depth++;
                        const std::string lc = std::format("loops[{}][{}]", c.loopId >> 2, c.loopId & 3);
                        Open(std::format("if (({} & 0xFFu) == 0u) pc = {}u; else {{", lc, c.address));
                        Line(std::format("savedAL{} = aL;", d));
                        Line(std::format("loopLeft{} = {} & 0xFFu;", d, lc));
                        Line(std::format("loopStep{} = int({} << 8) >> 24;", d, lc));
                        Line(std::format("aL = int(({} >> 8) & 0xFFu);", lc));
                        Close();
                        break;
                    }
                    case CfOpcode::LoopEnd:
                    {
                        if (depth == 0) { Warn("fim de laco sem inicio"); break; }
                        const int d = --depth;
                        std::string again = std::format("--loopLeft{} > 0u", d);
                        if (c.predicated) again += std::format(" && p0 != {}", Bool(c.condition));   // "break" pelo predicado
                        Open(std::format("if ({}) {{", again));
                        Line(std::format("aL += loopStep{}; pc = {}u;", d, c.address));
                        Close("} else {");
                        ++m_indent;
                        Line(std::format("aL = savedAL{};", d));
                        Close();
                        break;
                    }
                    case CfOpcode::CondCall:
                        Line(std::format("if ({} && retDepth < 4u) {{ retStack[retDepth++] = {}u; pc = {}u; }}", JumpCondition(c), i + 1, c.address));
                        break;
                    case CfOpcode::Return:
                        Line("if (retDepth == 0u) stop = true; else pc = retStack[--retDepth];");
                        break;
                    default:
                        Warn(std::format("controle de fluxo {} ignorado", static_cast<int>(c.opcode)));
                        break;
                    }
                    Line("break;");
                    Close();
                }
                Line("default: stop = true; break;");
                Close();
                Line("if (stop) break;");
                Close();
            }

            std::string JumpCondition(const CfInstruction& c) const
            {
                if (c.unconditional) return "true";
                if (c.predicated) return std::format("p0 == {}", Bool(c.condition));
                return std::format("BoolConst({}u) == {}", c.boolIndex, Bool(c.condition));
            }

            void EmitExec(const CfInstruction& c)
            {
                bool guarded = true;
                switch (c.opcode)
                {
                case CfOpcode::CondExec: case CfOpcode::CondExecEnd:
                case CfOpcode::CondExecPredClean: case CfOpcode::CondExecPredCleanEnd:
                    Open(std::format("if (BoolConst({}u) == {}) {{", c.boolIndex, Bool(c.condition)));
                    break;
                case CfOpcode::CondPredExec: case CfOpcode::CondPredExecEnd:
                    Open(std::format("if (p0 == {}) {{", Bool(c.condition)));
                    break;
                default:
                    guarded = false;
                    break;
                }
                for (std::uint32_t k = 0; k < c.count; ++k)
                {
                    const std::size_t at = (std::size_t{ c.address } + k) * 3;
                    if (at + 3 > m_mc.code.size()) { Warn("bloco exec passa do fim do shader"); break; }
                    const std::uint32_t* words = m_mc.code.data() + at;
                    if ((c.sequence >> (2 * k)) & 1) EmitFetch(DecodeFetch(words));
                    else EmitAlu(DecodeAlu(words));
                }
                if (guarded) Close();
            }

            // ------------------------------------------------------------ registradores
            static std::string Temp(std::uint32_t index, bool relative)
            {
                return relative ? std::format("r[clamp({} + aL, 0, 63)]", index) : std::format("r[{}]", index);
            }

            void BeginPredicate(bool predicated, bool condition)
            {
                if (predicated) Open(std::format("if (p0 == {}) {{", Bool(condition)));
            }
            void EndPredicate(bool predicated)
            {
                if (predicated) Close();
            }

            // grava "value" (float4) no destino seguindo o swizzle de destino de uma leitura (3 bits por componente)
            void WriteFetchResult(const FetchInstruction& f, const std::string& value)
            {
                const std::string dst = Temp(f.dstRegister, f.dstRelative);
                std::string mask, from;
                for (unsigned i = 0; i < 4; ++i)
                {
                    const std::uint32_t s = (f.dstSwizzle >> (3 * i)) & 7;
                    if (s <= 3) { mask += kComponent[i]; from += kComponent[s]; }
                }
                if (!mask.empty()) Line(std::format("{}.{} = ({}).{};", dst, mask, value, from));
                for (unsigned i = 0; i < 4; ++i)
                {
                    const std::uint32_t s = (f.dstSwizzle >> (3 * i)) & 7;
                    if (s == 4 || s == 5) Line(std::format("{}.{} = {}.0;", dst, kComponent[i], s == 5 ? 1 : 0));
                }
            }

            static bool WritesSomething(std::uint32_t dstSwizzle)
            {
                for (unsigned i = 0; i < 4; ++i)
                    if (((dstSwizzle >> (3 * i)) & 7) <= 5) return true;
                return false;
            }

            // ------------------------------------------------------------ leituras
            void EmitFetch(const FetchInstruction& f)
            {
                if (f.srcRelative) Warn("leitura com registrador de origem relativo (aL)");
                if (f.opcode == FetchOpcode::Vertex) EmitVertexFetch(f);
                else EmitTextureFetch(f);
            }

            void EmitVertexFetch(const FetchInstruction& f)
            {
                if (!f.miniFetch)
                {
                    m_fetchSlot = f.constIndex;
                    m_fetchStride = f.stride;
                    m_fetchSrc = f.srcRegister;
                    m_fetchComponent = f.srcComponent;
                    m_fetchRound = f.roundIndex;
                    m_haveFetch = true;
                    if (std::find(m_out.fetchSlots.begin(), m_out.fetchSlots.end(), f.constIndex) == m_out.fetchSlots.end())
                        m_out.fetchSlots.push_back(f.constIndex);
                }
                else if (!m_haveFetch)
                {
                    Warn("leitura de vertice resumida sem leitura completa antes");
                    return;
                }
                if (!WritesSomething(f.dstSwizzle)) return;
                BeginPredicate(f.predicated, f.predicateCondition);
                const std::string index = m_fetchRound ? std::format("uint(floor(r[{}].{} + 0.5))", m_fetchSrc, kComponent[m_fetchComponent])
                                                       : std::format("uint(r[{}].{})", m_fetchSrc, kComponent[m_fetchComponent]);
                WriteFetchResult(f, std::format("VFetch({}, {}u, {}u, {}u, {}u, {}, {}, {})", index, m_fetchSlot, f.offset, m_fetchStride,
                                                f.format, Bool(f.isSigned), Bool(f.isInteger), f.expAdjust));
                EndPredicate(f.predicated);
            }

            void EmitTextureFetch(const FetchInstruction& f)
            {
                if (f.opcode == FetchOpcode::SetLod || f.opcode == FetchOpcode::SetGradientsHorizontal ||
                    f.opcode == FetchOpcode::SetGradientsVertical)
                {
                    Warn("ajuste de LOD/gradiente da textura ignorado");
                    return;
                }
                if (!WritesSomething(f.dstSwizzle)) return;

                const std::string src = Temp(f.srcRegister, false);
                auto coord = [&](unsigned i) { return kComponent[(f.srcSwizzle >> (2 * i)) & 3]; };
                const std::string uv = std::format("{}.{}{}", src, coord(0), coord(1));
                const std::string denorm = Bool(f.unnormalized);
                const std::string offset = std::format("float2({}, {})", f.offsetX * 0.5, f.offsetY * 0.5);
                const unsigned slot = f.constIndex;

                std::string value;
                switch (f.opcode)
                {
                case FetchOpcode::Texture:
                    m_out.textureMask |= 1u << slot;
                    if (f.dimension == TextureDimension::Cube)
                    {
                        m_out.cubeMask |= 1u << slot;
                        value = std::format("TFetchCube({}u, {}.{}{}{})", slot, src, coord(0), coord(1), coord(2));
                    }
                    else
                    {
                        if (f.dimension == TextureDimension::D3) Warn("textura 3D lida como 2D");
                        value = f.offsetX || f.offsetY ? std::format("TFetch2DOff({}u, {}, {}, {})", slot, uv, denorm, offset)
                                                       : std::format("TFetch2D({}u, {}, {})", slot, uv, denorm);
                    }
                    break;
                case FetchOpcode::GetWeights:
                    m_out.textureMask |= 1u << slot;
                    value = std::format("TWeights({}u, {}, {}, {})", slot, uv, denorm, offset);
                    break;
                case FetchOpcode::GetGradients:
                    if (m_opt.pixel)
                        value = std::format("float4(ddx_coarse({0}.{1}), ddy_coarse({0}.{1}), ddx_coarse({0}.{2}), ddy_coarse({0}.{2}))",
                                            src, coord(0), coord(1));
                    else value = "float4(0.0, 0.0, 0.0, 0.0)";
                    break;
                case FetchOpcode::GetComputedLod:
                    if (m_opt.pixel)
                    {
                        m_out.textureMask |= 1u << slot;
                        value = std::format("TComputedLod({}u, {}, {})", slot, uv, denorm);
                    }
                    else value = "float4(0.0, 0.0, 0.0, 0.0)";
                    break;
                default:
                    Warn(std::format("leitura especial {} tratada como zero", static_cast<int>(f.opcode)));
                    value = "float4(0.0, 0.0, 0.0, 0.0)";
                    break;
                }
                BeginPredicate(f.predicated, f.predicateCondition);
                WriteFetchResult(f, value);
                EndPredicate(f.predicated);
            }

            // ------------------------------------------------------------ ALU
            // Indice (0 ou 1) da constante lida pelo operando 'k' (0-2): cada instrucao le no maximo duas constantes,
            // na ordem dos operandos usados pela operacao de vetor; o escalar (src3) vem depois das de src1/src2.
            static unsigned ConstantSlot(const AluInstruction& a, unsigned k)
            {
                const unsigned used = static_cast<unsigned>(VectorOperandCount(a.vectorOpcode));
                unsigned slot = 0;
                for (unsigned j = 0; j < k && j < used; ++j)
                    if (!a.src[j].isRegister) ++slot;
                return std::min(slot, 1u);
            }

            std::string ConstantRef(const AluInstruction& a, std::uint32_t index, unsigned slot) const
            {
                if (!a.constRelative[slot]) return std::format("c[{}]", index);
                return std::format("c[clamp({} + {}, 0, 255)]", index, a.relativeUsesA0 ? "a0" : "aL");
            }

            static std::string Modifiers(std::string value, bool abs, bool negate)
            {
                if (abs) value = "abs(" + value + ")";
                if (negate) value = "-" + value;
                return value;
            }

            // operando de vetor (float4 com swizzle completo)
            std::string VectorSource(const AluInstruction& a, unsigned k) const
            {
                const AluSource& s = a.src[k];
                std::string base;
                bool abs;
                if (s.isRegister)
                {
                    base = Temp(s.index & 63, (s.index & 64) != 0);
                    abs = (s.index & 128) != 0;
                }
                else
                {
                    base = ConstantRef(a, s.index, ConstantSlot(a, k));
                    abs = a.absConstants;
                }
                std::string swz;
                for (unsigned i = 0; i < 4; ++i) swz += kComponent[AluComponent(s.swizzle, i)];
                if (swz != "xyzw") base += "." + swz;
                return Modifiers(base, abs, s.negate);
            }

            // operandos escalares: "a" = componente da posicao w do swizzle de src3, "b" = da posicao x
            std::string ScalarSource(const AluInstruction& a, bool second) const
            {
                const AluSource& s = a.src[2];
                const char comp = kComponent[second ? AluComponent(s.swizzle, 0) : AluComponent(s.swizzle, 3)];
                if (s.isRegister)
                    return Modifiers(std::format("{}.{}", Temp(s.index & 63, (s.index & 64) != 0), comp), (s.index & 128) != 0, s.negate);
                return Modifiers(std::format("{}.{}", ConstantRef(a, s.index, ConstantSlot(a, 2)), comp), a.absConstants, s.negate);
            }

            // MUL/ADD/SUB_CONST: "a" e a constante src3 (componente w), "b" um registrador temporario cujo numero vem
            // espalhado pela instrucao (bit 0 do opcode, src3_sel e os bits 2-5 do swizzle de src3)
            std::string ScalarConstOperand(const AluInstruction& a, bool temp) const
            {
                const AluSource& s = a.src[2];
                if (!temp)
                    return Modifiers(std::format("{}.{}", ConstantRef(a, s.index, ConstantSlot(a, 2)), kComponent[AluComponent(s.swizzle, 3)]),
                                     a.absConstants, s.negate);
                const std::uint32_t reg = (static_cast<std::uint32_t>(a.scalarOpcode) & 1) | (s.isRegister ? 2u : 0u) | (s.swizzle & 0x3C);
                return Modifiers(std::format("r[{}].{}", reg, kComponent[s.swizzle & 3]), a.absConstants, s.negate);
            }

            std::string ExportTarget(std::uint32_t index)
            {
                if (m_opt.pixel)
                {
                    if (index <= 3) return std::format("oC{}", index);
                    if (index == 61) { m_out.writesDepth = true; return "oDepth4"; }
                }
                else
                {
                    if (index == 62) return "oPos";
                    if (index <= 15) return std::format("o[{}]", index);
                    if (index != 63) Warn(std::format("exportacao {} (memexport) ignorada", index));
                }
                return "oDummy";
            }

            void EmitAlu(const AluInstruction& a)
            {
                const VectorOpcode vop = a.vectorOpcode;
                const ScalarOpcode sop = a.scalarOpcode;
                const bool vectorKill = vop >= VectorOpcode::KillEq && vop <= VectorOpcode::KillNe;
                const bool vectorPush = vop >= VectorOpcode::PredSetEqPush && vop <= VectorOpcode::PredSetGePush;
                const bool vectorNeeded = a.vectorWriteMask || vectorKill || vectorPush || vop == VectorOpcode::MaxA;
                const bool scalarNeeded = sop != ScalarOpcode::RetainPrev || (a.scalarWriteMask && !a.exportData) ||
                                          (a.exportData && (a.scalarWriteMask & ~a.vectorWriteMask));
                const bool exportConstants = a.exportData && a.scalarDestRelative;   // pode gravar so zeros na saida
                if (!vectorNeeded && !scalarNeeded && !exportConstants) return;
                if (static_cast<unsigned>(vop) > 29) Warn(std::format("operacao de vetor {} desconhecida", static_cast<unsigned>(vop)));

                BeginPredicate(a.predicated, a.predicateCondition);
                Open("{");

                // 1) le todos os operandos, 2) calcula, 3) efeitos (predicado, a0, descarte), 4) grava os destinos
                if (vectorNeeded)
                {
                    const int n = VectorOperandCount(vop);
                    for (int k = 0; k < n; ++k) Line(std::format("float4 v{} = {};", k, VectorSource(a, static_cast<unsigned>(k))));
                }
                if (scalarNeeded && sop != ScalarOpcode::RetainPrev)
                {
                    if (IsScalarConstOp(sop))
                    {
                        Line(std::format("float sa = {};", ScalarConstOperand(a, false)));
                        Line(std::format("float sb = {};", ScalarConstOperand(a, true)));
                    }
                    else
                    {
                        Line(std::format("float sa = {};", ScalarSource(a, false)));
                        if (ScalarUsesSecondOperand(sop)) Line(std::format("float sb = {};", ScalarSource(a, true)));
                    }
                }

                if (vectorNeeded)
                {
                    std::string e = VectorExpression(vop);
                    if (a.vectorSaturate) e = "saturate(" + e + ")";
                    Line("float4 vr = " + e + ";");
                }
                if (scalarNeeded)
                {
                    std::string e = ScalarExpression(sop);
                    if (a.scalarSaturate) e = "saturate(" + e + ")";
                    Line("float sr = " + e + ";");
                }

                // efeitos
                if (vectorPush) Line(std::format("p0 = v0.w == 0.0 && v1.w {} 0.0;", PushCompare(vop)));
                if (vop == VectorOpcode::MaxA) Line("a0 = int(clamp(floor(v0.w + 0.5), -256.0, 255.0));");
                switch (sop)
                {
                case ScalarOpcode::PredSetEq: Line("p0 = sa == 0.0;"); break;
                case ScalarOpcode::PredSetNe: Line("p0 = sa != 0.0;"); break;
                case ScalarOpcode::PredSetGt: Line("p0 = sa > 0.0;"); break;
                case ScalarOpcode::PredSetGe: Line("p0 = sa >= 0.0;"); break;
                case ScalarOpcode::PredSetInv: Line("p0 = sa == 1.0;"); break;
                case ScalarOpcode::PredSetPop: Line("p0 = sa - 1.0 <= 0.0;"); break;
                case ScalarOpcode::PredSetClear: Line("p0 = false;"); break;
                case ScalarOpcode::PredSetRestore: Line("p0 = sa == 0.0;"); break;
                case ScalarOpcode::MaxA: Line("a0 = int(clamp(floor(sa + 0.5), -256.0, 255.0));"); break;
                case ScalarOpcode::MaxAFloor: Line("a0 = int(clamp(floor(sa), -256.0, 255.0));"); break;
                default: break;
                }
                const bool scalarKill = sop >= ScalarOpcode::KillEq && sop <= ScalarOpcode::KillOne;
                if (m_opt.pixel && vectorKill) Line("if (vr.x != 0.0) discard;");
                if (m_opt.pixel && scalarKill) Line("if (sr != 0.0) discard;");
                if (sop != ScalarOpcode::RetainPrev) Line("ps = sr;");

                // destinos
                if (a.exportData)
                {
                    const std::string dst = ExportTarget(a.vectorDest);
                    const std::uint32_t vmask = a.vectorWriteMask & ~a.scalarWriteMask;
                    const std::uint32_t smask = a.scalarWriteMask & ~a.vectorWriteMask;
                    const std::uint32_t ones = a.vectorWriteMask & a.scalarWriteMask;
                    const std::uint32_t zeros = a.scalarDestRelative ? (~(a.vectorWriteMask | a.scalarWriteMask) & 15u) : 0u;
                    if (vmask) Line(std::format("{0}.{1} = vr.{1};", dst, ComponentMask(vmask)));
                    if (smask) Line(std::format("{}.{} = sr;", dst, ComponentMask(smask)));
                    if (ones) Line(std::format("{}.{} = 1.0;", dst, ComponentMask(ones)));
                    if (zeros) Line(std::format("{}.{} = 0.0;", dst, ComponentMask(zeros)));
                }
                else
                {
                    if (a.vectorWriteMask)
                        Line(std::format("{0}.{1} = vr.{1};", Temp(a.vectorDest, a.vectorDestRelative), ComponentMask(a.vectorWriteMask)));
                    if (a.scalarWriteMask)
                        Line(std::format("{}.{} = sr;", Temp(a.scalarDest, a.scalarDestRelative), ComponentMask(a.scalarWriteMask)));
                }

                Close();
                EndPredicate(a.predicated);
            }

            static std::string_view PushCompare(VectorOpcode op)
            {
                switch (op)
                {
                case VectorOpcode::PredSetEqPush: return "==";
                case VectorOpcode::PredSetNePush: return "!=";
                case VectorOpcode::PredSetGtPush: return ">";
                default: return ">=";
                }
            }

            std::string VectorExpression(VectorOpcode op)
            {
                switch (op)
                {
                case VectorOpcode::Add: return "v0 + v1";
                case VectorOpcode::Mul: return "v0 * v1";
                case VectorOpcode::Max: case VectorOpcode::MaxA: return "max(v0, v1)";
                case VectorOpcode::Min: return "min(v0, v1)";
                case VectorOpcode::SetEq: return "float4(v0 == v1)";
                case VectorOpcode::SetGt: return "float4(v0 > v1)";
                case VectorOpcode::SetGe: return "float4(v0 >= v1)";
                case VectorOpcode::SetNe: return "float4(v0 != v1)";
                case VectorOpcode::Frac: return "frac(v0)";
                case VectorOpcode::Trunc: return "trunc(v0)";
                case VectorOpcode::Floor: return "floor(v0)";
                case VectorOpcode::MulAdd: return "v0 * v1 + v2";
                case VectorOpcode::CndEq: return "select(v0 == 0.0, v1, v2)";
                case VectorOpcode::CndGe: return "select(v0 >= 0.0, v1, v2)";
                case VectorOpcode::CndGt: return "select(v0 > 0.0, v1, v2)";
                case VectorOpcode::Dot4: return "dot(v0, v1).xxxx";
                case VectorOpcode::Dot3: return "dot(v0.xyz, v1.xyz).xxxx";
                case VectorOpcode::Dot2Add: return "(dot(v0.xy, v1.xy) + v2.x).xxxx";
                case VectorOpcode::Cube: return "XeCube(v0)";
                case VectorOpcode::Max4: return "max4(v0).xxxx";
                case VectorOpcode::PredSetEqPush: case VectorOpcode::PredSetNePush:
                case VectorOpcode::PredSetGtPush: case VectorOpcode::PredSetGePush:
                    return std::format("(v0.x == 0.0 && v1.x {} 0.0 ? 0.0 : v0.x + 1.0).xxxx", PushCompare(op));
                case VectorOpcode::KillEq: return "float(any(v0 == v1)).xxxx";
                case VectorOpcode::KillGt: return "float(any(v0 > v1)).xxxx";
                case VectorOpcode::KillGe: return "float(any(v0 >= v1)).xxxx";
                case VectorOpcode::KillNe: return "float(any(v0 != v1)).xxxx";
                case VectorOpcode::Dst: return "dst(v0, v1)";
                }
                return "float4(0.0, 0.0, 0.0, 0.0)";
            }

            std::string ScalarExpression(ScalarOpcode op)
            {
                // recíprocos, raizes e logaritmos ficam limitados a +-FLT_MAX: sem a regra do console de "0 * inf = 0",
                // um infinito aqui viraria NaN logo adiante
                auto clampMax = [](std::string_view e) { return std::format("clamp({}, -{}, {})", e, kFltMax, kFltMax); };
                switch (op)
                {
                case ScalarOpcode::Add: return "sa + sb";
                case ScalarOpcode::AddPrev: return "sa + ps";
                case ScalarOpcode::Mul: return "sa * sb";
                case ScalarOpcode::MulPrev: return "sa * ps";
                case ScalarOpcode::MulPrev2:
                    return std::format("(ps == -{0} || isinf(ps) || isnan(ps) || isnan(sb) || sb <= 0.0) ? -{0} : sa * ps", kFltMax);
                case ScalarOpcode::Max: case ScalarOpcode::MaxA: case ScalarOpcode::MaxAFloor: return "max(sa, sb)";
                case ScalarOpcode::Min: return "min(sa, sb)";
                case ScalarOpcode::SetEq: return "float(sa == 0.0)";
                case ScalarOpcode::SetGt: return "float(sa > 0.0)";
                case ScalarOpcode::SetGe: return "float(sa >= 0.0)";
                case ScalarOpcode::SetNe: return "float(sa != 0.0)";
                case ScalarOpcode::Frac: return "frac(sa)";
                case ScalarOpcode::Trunc: return "trunc(sa)";
                case ScalarOpcode::Floor: return "floor(sa)";
                case ScalarOpcode::Exp: return "exp2(sa)";
                case ScalarOpcode::LogClamp: case ScalarOpcode::Log: return clampMax("log2(sa)");
                case ScalarOpcode::RcpClamp: case ScalarOpcode::Rcp: return clampMax("rcp(sa)");
                case ScalarOpcode::RcpFlush: return "(sa == 0.0 ? 0.0 : rcp(sa))";
                case ScalarOpcode::RsqClamp: case ScalarOpcode::Rsq: return clampMax("rsqrt(sa)");
                case ScalarOpcode::RsqFlush: return "(sa == 0.0 ? 0.0 : rsqrt(sa))";
                case ScalarOpcode::Sub: return "sa - sb";
                case ScalarOpcode::SubPrev: return "sa - ps";
                case ScalarOpcode::PredSetEq: return "(sa == 0.0 ? 0.0 : 1.0)";
                case ScalarOpcode::PredSetNe: return "(sa != 0.0 ? 0.0 : 1.0)";
                case ScalarOpcode::PredSetGt: return "(sa > 0.0 ? 0.0 : 1.0)";
                case ScalarOpcode::PredSetGe: return "(sa >= 0.0 ? 0.0 : 1.0)";
                case ScalarOpcode::PredSetInv: return "(sa == 1.0 ? 0.0 : (sa == 0.0 ? 1.0 : sa))";
                case ScalarOpcode::PredSetPop: return "(sa - 1.0 <= 0.0 ? 0.0 : sa - 1.0)";
                case ScalarOpcode::PredSetClear: return std::string(kFltMax);
                case ScalarOpcode::PredSetRestore: return "(sa == 0.0 ? 0.0 : sa)";
                case ScalarOpcode::KillEq: return "float(sa == 0.0)";
                case ScalarOpcode::KillGt: return "float(sa > 0.0)";
                case ScalarOpcode::KillGe: return "float(sa >= 0.0)";
                case ScalarOpcode::KillNe: return "float(sa != 0.0)";
                case ScalarOpcode::KillOne: return "float(sa == 1.0)";
                case ScalarOpcode::Sqrt: return "sqrt(sa)";
                case ScalarOpcode::MulConst0: case ScalarOpcode::MulConst1: return "sa * sb";
                case ScalarOpcode::AddConst0: case ScalarOpcode::AddConst1: return "sa + sb";
                case ScalarOpcode::SubConst0: case ScalarOpcode::SubConst1: return "sa - sb";
                case ScalarOpcode::Sin: return "sin(sa)";
                case ScalarOpcode::Cos: return "cos(sa)";
                case ScalarOpcode::RetainPrev: return "ps";
                }
                Warn(std::format("operacao escalar {} desconhecida", static_cast<unsigned>(op)));
                return "0.0";
            }

            const Microcode& m_mc;
            const HlslOptions& m_opt;
            HlslShader& m_out;
            int m_indent = 1;

            // ultima leitura de vertice completa (as "resumidas" reaproveitam slot, stride e o registrador do indice)
            bool m_haveFetch = false;
            std::uint32_t m_fetchSlot = 0, m_fetchStride = 0, m_fetchSrc = 0, m_fetchComponent = 0;
            bool m_fetchRound = false;
        };
    }

    HlslShader TranslateToHlsl(std::span<const std::uint32_t> code, const HlslOptions& options)
    {
        HlslShader out;
        const Microcode mc = DecodeShader(code);
        Emitter(mc, options, out).Run();
        return out;
    }
}
