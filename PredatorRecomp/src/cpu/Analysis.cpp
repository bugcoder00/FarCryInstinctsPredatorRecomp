#include "cpu/Analysis.h"

#include "core/Diagnostics.h"
#include "cpu/Decoder.h"
#include "loader/ByteOrder.h"

#include <algorithm>
#include <optional>
#include <set>
#include <string_view>
#include <cstdio>

namespace predator::cpu
{
    namespace
    {
        bool InCode(const loader::Executable& exe, std::uint32_t a)
        {
            const auto* r = exe.RegionAt(a);
            return r && r->kind == loader::RegionKind::Code && (a & 3) == 0;
        }

        // ---------------------------------------------------------------- tabela de excecoes (.pdata)
        // Entradas de 8 bytes: inicio da funcao e um campo com o tamanho do prologo (8 bits) e da funcao em palavras
        // (22 bits). Os cabecalhos do PE podem estar ausentes, entao a tabela e procurada pelo formato: a maior sequencia
        // de entradas com inicios crescentes dentro do codigo, fora das regioes de codigo.
        struct PdataEntry { std::uint32_t start, words; };

        std::vector<PdataEntry> FindPdata(const loader::Executable& exe, std::uint32_t& tableAddress)
        {
            const std::uint32_t base = exe.Info().baseAddress;
            const auto image = exe.Image();
            auto runAt = [&](std::size_t off) {
                std::size_t n = 0;
                std::uint32_t prev = 0;
                while (off + 8 <= image.size())
                {
                    const std::uint32_t begin = loader::ReadBE32(image.data() + off);
                    if (!InCode(exe, begin) || begin <= prev) break;
                    prev = begin;
                    ++n;
                    off += 8;
                }
                return n;
            };

            std::size_t bestOffset = 0, bestCount = 0;
            for (const auto& region : exe.Info().regions)
            {
                if (region.kind == loader::RegionKind::Code) continue;
                std::size_t off = region.start - base;
                const std::size_t end = off + region.size;
                while (off + 8 <= end)
                {
                    const std::size_t n = runAt(off);
                    if (n > bestCount) { bestCount = n; bestOffset = off; }
                    off += n > 4 ? n * 8 : 4;
                }
            }

            std::vector<PdataEntry> out;
            if (bestCount < 64) return out;   // sequencia curta demais para ser a tabela
            tableAddress = base + static_cast<std::uint32_t>(bestOffset);
            for (std::size_t i = 0; i < bestCount; ++i)
            {
                const std::uint8_t* p = image.data() + bestOffset + i * 8;
                out.push_back({ loader::ReadBE32(p), (loader::ReadBE32(p + 4) >> 8) & 0x3FFFFF });
            }
            return out;
        }

        // ---------------------------------------------------------------- tabelas de switch
        // Valor simbolico de um registrador antes de um bctr
        struct Value
        {
            enum class Kind { Unknown, Const, Index, Load } kind = Kind::Unknown;
            std::uint32_t constant = 0;   // Const: valor; Load: endereco da tabela
            std::uint32_t loadSize = 0;   // Load: 1, 2 ou 4 bytes por elemento
            std::uint32_t shift = 0;      // Index/Load: deslocamento aplicado depois
            std::uint32_t offset = 0;     // Load: base somada ao elemento (tabelas relativas)
            std::uint8_t indexReg = 0;    // Index: registrador do indice
            std::uint8_t scaledReg = 0xFF; // registrador que o slwi/clrlwi leu (o que o cmplwi compara)
        };

        class JumpTableResolver
        {
        public:
            JumpTableResolver(const loader::Executable& exe, std::uint32_t funcStart) : m_exe(exe), m_funcStart(funcStart) {}

            std::optional<JumpTable> Resolve(std::uint32_t bctr, std::uint32_t funcEnd)
            {
                // mtctr rX logo antes
                std::uint32_t a = bctr;
                std::optional<std::uint8_t> ctrReg;
                for (int i = 0; i < 4 && a > m_funcStart; ++i)
                {
                    a -= 4;
                    const Instruction insn = At(a);
                    if (insn.opcode == Opcode::mtspr && insn.operands[0].value == 9)
                    {
                        ctrReg = static_cast<std::uint8_t>(insn.operands[1].value);
                        break;
                    }
                }
                if (!ctrReg) return std::nullopt;

                const Value v = Eval(*ctrReg, a, 24);
                if (v.kind != Value::Kind::Load) return std::nullopt;

                // sem comparacao com o indice: le enquanto os elementos apontarem para dentro da funcao
                const auto count = v.indexReg != 0xFF ? BoundBefore(a, v.indexReg) : std::nullopt;
                if (count && (*count == 0 || *count > 4096)) return std::nullopt;

                JumpTable table;
                table.address = bctr;
                for (std::uint32_t k = 0; k < (count ? *count : 4096u); ++k)
                {
                    const std::uint32_t at = v.constant + k * v.loadSize;
                    std::uint32_t element = 0;
                    const bool read = ReadElement(at, v.loadSize, element);
                    const std::uint32_t target = v.offset + (element << v.shift);
                    if (!read || target < m_funcStart || target >= funcEnd || (target & 3))
                    {
                        if (count) return std::nullopt;
                        break;
                    }
                    table.targets.push_back(target);
                }
                if (table.targets.empty()) return std::nullopt;
                return table;
            }

        private:
            Instruction At(std::uint32_t a) const
            {
                auto w = m_exe.Read32(a);
                return w ? Decode(*w, a) : Instruction{};
            }

            bool ReadElement(std::uint32_t at, std::uint32_t size, std::uint32_t& out) const
            {
                if (!m_exe.Contains(at, size)) return false;
                const std::uint8_t* p = m_exe.Image().data() + (at - m_exe.Info().baseAddress);
                out = size == 4 ? loader::ReadBE32(p) : size == 2 ? loader::ReadBE16(p) : p[0];
                return true;
            }

            // limite do indice: "cmplwi crN,rX,M" antes do desvio (M + 1 entradas)
            std::optional<std::uint32_t> BoundBefore(std::uint32_t a, std::uint8_t indexReg) const
            {
                for (int i = 0; i < 32 && a > m_funcStart; ++i)
                {
                    a -= 4;
                    const Instruction insn = At(a);
                    if (insn.opcode == Opcode::cmpli && insn.operands[1].value == 0 && insn.operands[2].value == indexReg)
                        return static_cast<std::uint32_t>(insn.operands[3].value) + 1;
                }
                return std::nullopt;
            }

            // Valor do registrador 'reg' imediatamente antes do endereco 'a', procurando para tras
            Value Eval(std::uint8_t reg, std::uint32_t a, int budget) const
            {
                while (budget-- > 0 && a > m_funcStart)
                {
                    a -= 4;
                    const Instruction insn = At(a);
                    if (!insn.Valid()) return {};
                    const auto& o = insn.operands;
                    // uma chamada devolve valores novos em r3 (e apaga r0, r4-r12): o indice vem dela
                    if (insn.Has(kFlagLK) && (reg == 0 || (reg >= 3 && reg <= 12)))
                    {
                        Value v;
                        v.kind = Value::Kind::Index;
                        v.indexReg = reg;
                        return v;
                    }
                    // so interessa a instrucao que grava 'reg' (o primeiro operando nas formas usadas abaixo)
                    const std::string_view name = GetOpcodeInfo(insn.opcode).name;
                    const bool isStore = name.starts_with("st");   // armazenamentos: o primeiro operando e lido, nao gravado
                    const bool writesReg = !isStore && insn.operandCount > 0 && (o[0].kind == OperandKind::Gpr) && o[0].value == reg;
                    if (!writesReg)
                    {
                        // desvios no meio: o caminho fica incerto, mas o compilador costuma montar a tabela num bloco so
                        continue;
                    }
                    auto reg1 = [&](int i) { return static_cast<std::uint8_t>(o[i].value); };
                    switch (insn.opcode)
                    {
                    case Opcode::addis:
                    {
                        Value base = o[1].value == 0 ? Value{ Value::Kind::Const } : Eval(reg1(1), a, budget);
                        if (base.kind != Value::Kind::Const) return {};
                        base.constant += static_cast<std::uint32_t>(o[2].value) << 16;
                        return base;
                    }
                    case Opcode::addi:
                    {
                        Value base = o[1].value == 0 ? Value{ Value::Kind::Const } : Eval(reg1(1), a, budget);
                        if (base.kind == Value::Kind::Const) { base.constant += static_cast<std::uint32_t>(o[2].value); return base; }
                        return {};
                    }
                    case Opcode::ori:
                        // forma D: o0 = rA (destino), o1 = rS
                        {
                            Value base = Eval(reg1(1), a, budget);
                            if (base.kind != Value::Kind::Const) return {};
                            base.constant |= static_cast<std::uint32_t>(o[2].value);
                            return base;
                        }
                    case Opcode::or_:
                        if (o[1].value == o[2].value) return Eval(reg1(1), a, budget);   // mr
                        return {};
                    case Opcode::rlwinm:
                    {
                        Value v = Eval(reg1(1), a, budget);
                        const auto sh = static_cast<std::uint32_t>(o[2].value), mb = static_cast<std::uint32_t>(o[3].value),
                                   me = static_cast<std::uint32_t>(o[4].value);
                        if (sh != 0 && mb == 0 && me == 31 - sh && (v.kind == Value::Kind::Load || v.kind == Value::Kind::Index))
                        {
                            v.shift += sh;   // slwi
                            if (v.kind == Value::Kind::Index) v.scaledReg = reg1(1);
                            return v;
                        }
                        if (sh == 0 && me == 31)   // clrlwi: so limpa bits altos
                        {
                            if (v.kind == Value::Kind::Index) v.scaledReg = reg1(1);
                            return v;
                        }
                        return {};
                    }
                    case Opcode::lwzx:
                    case Opcode::lhzx:
                    case Opcode::lbzx:
                    {
                        // um lado e o endereco da tabela, o outro e o indice (ja multiplicado pelo tamanho)
                        const std::uint32_t size = insn.opcode == Opcode::lwzx ? 4u : insn.opcode == Opcode::lhzx ? 2u : 1u;
                        Value left = o[1].value == 0 ? Value{} : Eval(reg1(1), a, budget);
                        Value right = Eval(reg1(2), a, budget);
                        const Value* table = left.kind == Value::Kind::Const ? &left : right.kind == Value::Kind::Const ? &right : nullptr;
                        if (!table) return {};
                        const Value& index = table == &left ? right : left;
                        const std::uint8_t indexOperand = table == &left ? reg1(2) : reg1(1);
                        Value v;
                        v.kind = Value::Kind::Load;
                        v.constant = table->constant;
                        v.loadSize = size;
                        // o limite e comparado com o registrador do indice como ele chega aqui (antes de copias com mr)
                        v.indexReg = index.kind != Value::Kind::Index ? 0xFF : index.scaledReg != 0xFF ? index.scaledReg : indexOperand;
                        return v;
                    }
                    case Opcode::add:
                    {
                        Value x = Eval(reg1(1), a, budget), y = Eval(reg1(2), a, budget);
                        if (x.kind == Value::Kind::Const && y.kind == Value::Kind::Load) std::swap(x, y);
                        if (x.kind == Value::Kind::Load && y.kind == Value::Kind::Const)
                        {
                            x.offset += y.constant;   // tabela relativa: base + elemento
                            return x;
                        }
                        return {};
                    }
                    default:
                        return {};
                    }
                }
                Value v;
                v.kind = Value::Kind::Index;   // vem de antes: e o indice
                v.indexReg = reg;
                return v;
            }

            const loader::Executable& m_exe;
            std::uint32_t m_funcStart;
        };

        const char* SourceName(FunctionSource s)
        {
            switch (s)
            {
            case FunctionSource::Pdata: return "pdata";
            case FunctionSource::Call: return "chamada";
            case FunctionSource::SecondEntry: return "segunda entrada";
            case FunctionSource::Entry: return "entrada";
            case FunctionSource::Import: return "importacao";
            case FunctionSource::Gap: return "lacuna";
            case FunctionSource::Pointer: return "ponteiro";
            case FunctionSource::BranchEntry: return "entrada por desvio";
            }
            return "?";
        }
    }

    const Function* ProgramAnalysis::FunctionAt(std::uint32_t start) const noexcept
    {
        auto it = std::lower_bound(functions.begin(), functions.end(), start, [](const Function& f, std::uint32_t s) { return f.start < s; });
        return it != functions.end() && it->start == start ? &*it : nullptr;
    }

    namespace
    {
        Function MakeFunction(std::uint32_t start, std::uint32_t end, FunctionSource source)
        {
            Function f;
            f.start = start;
            f.end = end;
            f.bodyStart = start;
            f.source = source;
            return f;
        }

        // funcao primaria que contem o endereco, ou nullptr
        const Function* Containing(const std::map<std::uint32_t, Function>& byStart, std::uint32_t a)
        {
            auto it = byStart.upper_bound(a);
            while (it != byStart.begin())
            {
                --it;
                if (it->second.bodyStart != it->second.start) continue;   // entradas secundarias nao contam
                return a < it->second.end ? &it->second : nullptr;
            }
            return nullptr;
        }

        // fim das funcoes sem tamanho conhecido: ate a proxima funcao ou o fim da regiao de codigo
        void ComputeEnds(const loader::Executable& exe, std::map<std::uint32_t, Function>& byStart)
        {
            for (auto it = byStart.begin(); it != byStart.end(); ++it)
            {
                if (it->second.end) continue;
                std::uint32_t end = 0;
                if (const auto* r = exe.RegionAt(it->first)) end = r->start + r->size;
                if (auto next = std::next(it); next != byStart.end()) end = std::min(end, next->first);
                it->second.end = end;
            }
        }

        // destino de um desvio sem LK (b ou bc), se houver
        std::optional<std::uint32_t> BranchTarget(const Instruction& insn)
        {
            if (insn.Has(kFlagLK)) return std::nullopt;
            if (insn.opcode == Opcode::b) return static_cast<std::uint32_t>(insn.operands[0].value);
            if (insn.opcode == Opcode::bc) return static_cast<std::uint32_t>(insn.operands[2].value);
            return std::nullopt;
        }
    }

    ProgramAnalysis AnalyzeProgram(const loader::Executable& exe, Diagnostics& diag)
    {
        ProgramAnalysis result;
        std::map<std::uint32_t, Function> byStart;

        // 1. tabela de excecoes
        for (const auto& e : FindPdata(exe, result.pdataAddress))
        {
            if (e.words == 0) continue;
            byStart[e.start] = MakeFunction(e.start, e.start + e.words * 4, FunctionSource::Pdata);
            ++result.pdataCount;
        }
        if (result.pdataCount)
        {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "cpu: tabela de funcoes (.pdata) em 0x%08X: %u funcoes", result.pdataAddress, result.pdataCount);
            diag.Info(buf);
        }
        else
            diag.Warning("cpu: tabela de funcoes (.pdata) nao encontrada; as funcoes vem so das chamadas");

        // 2. importacoes: os registros vem em pares, a variavel (nos dados) e o "thunk" de 16 bytes (no codigo).
        //    A palavra de cada registro descreve a importacao: tipo (0 variavel, 1 funcao), biblioteca e ordinal.
        //    Imagens ja carregadas podem ter o thunk sobrescrito (nop/blr) e o descritor com os bytes invertidos,
        //    entao todo registro no codigo vira thunk e o ordinal vem dele ou do registro anterior.
        for (std::size_t lib = 0; lib < exe.Info().imports.size(); ++lib)
        {
            // tipo esperado: 1 no thunk (codigo), 0 na variavel (dados)
            auto descriptor = [&](std::uint32_t rec, std::uint32_t type) -> std::optional<std::uint32_t> {
                auto w = exe.Read32(rec);
                if (!w) return std::nullopt;
                for (std::uint32_t d : { *w, loader::ByteSwap32(*w) })
                    if ((d >> 24) == type && ((d >> 16) & 0xFF) == lib) return d & 0xFFFF;
                return std::nullopt;
            };
            const auto& records = exe.Info().imports[lib].records;
            for (std::size_t i = 0; i < records.size(); ++i)
            {
                const std::uint32_t rec = records[i];
                if (!InCode(exe, rec)) continue;
                auto ordinal = descriptor(rec, 1);
                if (!ordinal && i > 0 && !InCode(exe, records[i - 1])) ordinal = descriptor(records[i - 1], 0);
                if (!ordinal) continue;
                Function f = MakeFunction(rec, rec + 16, FunctionSource::Import);
                f.importLibrary = exe.Info().imports[lib].name;
                f.importOrdinal = *ordinal;
                byStart[rec] = f;
            }
        }

        // 3. alvos de chamadas diretas e ponto de entrada (os que caem no meio de uma funcao ficam para o passo 6)
        std::set<std::uint32_t> callTargets;
        callTargets.insert(exe.Info().entryPoint);
        for (const auto& region : exe.Info().regions)
        {
            if (region.kind != loader::RegionKind::Code) continue;
            for (std::uint32_t a = region.start; a < region.start + region.size; a += 4)
            {
                auto w = exe.Read32(a);
                if (!w || (*w >> 26) != 18 || !(*w & 1)) continue;   // so "bl"
                const Instruction insn = Decode(*w, a);
                const auto target = static_cast<std::uint32_t>(insn.operands[0].value);
                if (InCode(exe, target)) callTargets.insert(target);
            }
        }
        std::set<std::uint32_t> innerTargets;
        for (std::uint32_t t : callTargets)
        {
            if (byStart.count(t)) continue;
            if (Containing(byStart, t)) { innerTargets.insert(t); continue; }
            byStart[t] = MakeFunction(t, 0, t == exe.Info().entryPoint ? FunctionSource::Entry : FunctionSource::Call);
        }
        ComputeEnds(exe, byStart);

        // 4. enderecos de codigo citados em dados (tabelas de metodos, ponteiros de funcao) ou montados com lis + addi
        std::set<std::uint32_t> pointers;
        std::set<std::uint32_t> dataPointers;   // so os que vem dos dados (tabelas de metodos, ponteiros de funcao)
        for (const auto& region : exe.Info().regions)
        {
            if (region.kind == loader::RegionKind::Code)
            {
                // percorre o codigo lembrando o valor de cada "lis rD" ate rD ser sobrescrito ou o fluxo mudar
                // (fim de funcao): "addi rX,rD,lo" ou "ori rX,rD,lo" depois dele forma um endereco completo
                std::optional<std::uint32_t> high[32];
                for (std::uint32_t a = region.start; a < region.start + region.size; a += 4)
                {
                    auto w = exe.Read32(a);
                    if (!w) break;
                    if (byStart.count(a)) for (auto& h : high) h.reset();
                    const Instruction insn = Decode(*w, a);
                    if (!insn.Valid()) { for (auto& h : high) h.reset(); continue; }
                    const auto& o = insn.operands;
                    if ((insn.opcode == Opcode::addi && o[1].value != 0) || insn.opcode == Opcode::ori)
                    {
                        const auto src = static_cast<std::size_t>(o[1].value);
                        if (src < 32 && high[src])
                        {
                            const std::uint32_t v = insn.opcode == Opcode::addi ? *high[src] + static_cast<std::uint32_t>(o[2].value)
                                                                                : *high[src] | static_cast<std::uint32_t>(o[2].value);
                            if (InCode(exe, v)) pointers.insert(v);
                        }
                    }
                    if (insn.opcode == Opcode::addis && o[1].value == 0)
                    {
                        high[static_cast<std::size_t>(o[0].value)] = static_cast<std::uint32_t>(o[2].value << 16);
                        continue;
                    }
                    // qualquer outra instrucao que grava um GPR apaga o valor lembrado dele
                    const std::string_view name = GetOpcodeInfo(insn.opcode).name;
                    if (insn.operandCount > 0 && o[0].kind == OperandKind::Gpr && !name.starts_with("st") && !name.starts_with("cmp") &&
                        !name.starts_with("t"))
                        high[static_cast<std::size_t>(o[0].value)].reset();
                    if (insn.opcode == Opcode::b || insn.opcode == Opcode::bclr || insn.opcode == Opcode::bcctr)
                        if (!insn.Has(kFlagLK) && insn.opcode != Opcode::bc) for (auto& h : high) h.reset();
                }
                continue;
            }
            for (std::uint32_t a = region.start; a + 4 <= region.start + region.size; a += 4)
                if (auto w = exe.Read32(a); w && InCode(exe, *w))
                {
                    pointers.insert(*w);
                    dataPointers.insert(*w);
                }
        }

        // 5. codigo fora de qualquer funcao: o inicio de cada lacuna e cada ponteiro que cai nela viram funcoes
        {
            std::set<std::uint32_t> starts;
            std::uint32_t coveredUntil = 0;
            auto it = byStart.begin();
            for (const auto& region : exe.Info().regions)
            {
                if (region.kind != loader::RegionKind::Code) continue;
                bool inGap = false;
                for (std::uint32_t a = region.start; a < region.start + region.size; a += 4)
                {
                    while (it != byStart.end() && it->first <= a) { coveredUntil = std::max(coveredUntil, it->second.end); ++it; }
                    if (a < coveredUntil) { inGap = false; continue; }
                    auto w = exe.Read32(a);
                    if (!w || *w == 0) { inGap = false; continue; }
                    const bool valid = Decode(*w, a).Valid();
                    // um ponteiro so abre funcao nova logo depois de um retorno ou desvio incondicional
                    // (evita confundir tabelas de switch, que vem depois de um bctr, com funcoes)
                    auto prev = exe.Read32(a - 4);
                    const bool afterEnd = prev && (*prev == 0x4E800020u || ((*prev >> 26) == 18 && !(*prev & 1)));
                    if (valid && (!inGap || (pointers.count(a) && afterEnd))) starts.insert(a);
                    inGap = true;
                }
            }
            for (std::uint32_t a : starts)
                byStart[a] = MakeFunction(a, 0, pointers.count(a) ? FunctionSource::Pointer : FunctionSource::Gap);
            ComputeEnds(exe, byStart);
        }

        // 6. entradas secundarias: chamadas (bl) e desvios (b/bc) que caem no meio de outra funcao.
        //    A entrada traduz o corpo inteiro da funcao que a contem e comeca com um salto para o alvo,
        //    assim lacos que voltam para antes do alvo continuam funcionando.
        auto addInner = [&](std::uint32_t t, FunctionSource source) {
            if (byStart.count(t)) return;
            const Function* host = Containing(byStart, t);
            if (!host) return;
            Function f = MakeFunction(t, host->end, source);
            f.bodyStart = host->bodyStart;
            byStart[t] = f;
        };
        for (std::uint32_t t : innerTargets) addInner(t, FunctionSource::SecondEntry);
        // ponteiros dos dados que caem dentro de uma funcao conhecida, logo depois de um fim (blr, b ou bctr):
        // funcoes pequenas que a tabela .pdata juntou com a anterior
        for (std::uint32_t t : dataPointers)
        {
            auto prev = exe.Read32(t - 4);
            const bool afterEnd = prev && (*prev == 0x4E800020u || *prev == 0x4E800420u || ((*prev >> 26) == 18 && !(*prev & 1)));
            if (afterEnd && InCode(exe, t)) addInner(t, FunctionSource::Pointer);
        }
        // enderecos montados com lis + addi, logo depois de um blr ou b. Depois de bctr costuma ser tabela de switch:
        // so vale se esse bctr nao tiver tabela (ex.: "mtctr r4; bctr" em 8236C810, logo depois de outro trampolim)
        for (std::uint32_t t : pointers)
        {
            if (dataPointers.count(t)) continue;
            auto prev = exe.Read32(t - 4);
            bool afterEnd = prev && (*prev == 0x4E800020u || ((*prev >> 26) == 18 && !(*prev & 1)));
            if (!afterEnd && prev && *prev == 0x4E800420u)
                if (const Function* host = Containing(byStart, t))
                {
                    auto w = exe.Read32(t);
                    afterEnd = w && Decode(*w, t).Valid() && !JumpTableResolver(exe, host->bodyStart).Resolve(t - 4, host->end);
                }
            if (afterEnd) addInner(t, FunctionSource::Pointer);
        }
        // codigo logo depois de preenchimento com zeros, dentro de uma funcao de tamanho estimado: outra funcao
        for (const auto& region : exe.Info().regions)
        {
            if (region.kind != loader::RegionKind::Code) continue;
            for (std::uint32_t a = region.start + 4; a < region.start + region.size; a += 4)
            {
                auto w = exe.Read32(a), prev = exe.Read32(a - 4);
                if (w && prev && *w != 0 && *prev == 0 && Decode(*w, a).Valid()) addInner(a, FunctionSource::Gap);
            }
        }
        {
            std::vector<std::uint32_t> branchTargets;
            for (const auto& [start, f] : byStart)
            {
                if (f.source == FunctionSource::Import || f.bodyStart != f.start) continue;
                for (std::uint32_t a = f.start; a < f.end; a += 4)
                {
                    auto w = exe.Read32(a);
                    if (!w) break;
                    const auto t = BranchTarget(Decode(*w, a));
                    if (t && (*t < f.start || *t >= f.end) && InCode(exe, *t)) branchTargets.push_back(*t);
                }
            }
            for (std::uint32_t t : branchTargets) addInner(t, FunctionSource::BranchEntry);
        }
        for (auto& [s, f] : byStart) result.functions.push_back(f);

        // 7. cobertura: palavras de codigo (nao zero) fora de qualquer funcao
        {
            std::vector<std::pair<std::uint32_t, std::uint32_t>> spans;
            for (const auto& f : result.functions) spans.emplace_back(f.start, f.end);
            std::sort(spans.begin(), spans.end());
            std::size_t si = 0;
            std::uint32_t coveredUntil = 0;
            for (const auto& region : exe.Info().regions)
            {
                if (region.kind != loader::RegionKind::Code) continue;
                for (std::uint32_t a = region.start; a < region.start + region.size; a += 4)
                {
                    while (si < spans.size() && spans[si].first <= a) { coveredUntil = std::max(coveredUntil, spans[si].second); ++si; }
                    if (a < coveredUntil) continue;
                    auto w = exe.Read32(a);
                    if (w && *w != 0) ++result.uncoveredWords;
                }
            }
        }

        // 8. tabelas de switch: cada bctr (sem LK) dentro das funcoes primarias
        for (const auto& f : result.functions)
        {
            if (f.source == FunctionSource::Import || f.bodyStart != f.start) continue;
            JumpTableResolver resolver(exe, f.start);
            for (std::uint32_t a = f.start; a < f.end; a += 4)
            {
                auto w = exe.Read32(a);
                if (!w || *w != 0x4E800420u || result.jumpTables.count(a)) continue;   // bctr
                if (auto table = resolver.Resolve(a, f.end)) result.jumpTables[a] = std::move(*table);
                else ++result.unresolvedJumps;
            }
        }

        std::map<FunctionSource, std::size_t> bySource;
        for (const auto& f : result.functions) ++bySource[f.source];
        std::string summary = "cpu: " + std::to_string(result.functions.size()) + " funcoes (";
        bool first = true;
        for (const auto& [s, n] : bySource)
        {
            summary += (first ? "" : ", ") + std::to_string(n) + " " + SourceName(s);
            first = false;
        }
        summary += "), " + std::to_string(result.uncoveredWords) + " palavras de codigo fora de funcoes";
        diag.Info(summary);
        diag.Info("cpu: " + std::to_string(result.jumpTables.size()) + " tabelas de switch resolvidas, " +
                  std::to_string(result.unresolvedJumps) + " bctr sem tabela (viram desvio indireto)");
        return result;
    }
}
