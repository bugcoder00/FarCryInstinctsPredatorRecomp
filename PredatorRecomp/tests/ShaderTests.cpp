// Testes do tradutor de shaders: microcodigo montado a mao -> campos decodificados e HLSL gerado
#include "gpu/XenosHlsl.h"
#include "gpu/XenosMicrocode.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace predator::gpu;

namespace
{
    int g_failures = 0;

    void Check(const char* what, bool ok)
    {
        if (!ok)
        {
            std::printf("FALHOU: %s\n", what);
            ++g_failures;
        }
    }

    template <typename T, typename U>
    void Equal(const char* what, T got, U expected)
    {
        if (static_cast<unsigned long long>(got) != static_cast<unsigned long long>(expected))
        {
            std::printf("FALHOU: %s (veio %llu, esperado %llu)\n", what, static_cast<unsigned long long>(got),
                        static_cast<unsigned long long>(expected));
            ++g_failures;
        }
    }

    std::uint64_t Put(std::uint64_t value, unsigned lo) { return value << lo; }

    // instrucao de controle de fluxo exec (48 bits)
    std::uint64_t CfExec(CfOpcode op, std::uint32_t address, std::uint32_t count, std::uint32_t sequence,
                         std::uint32_t boolIndex = 0, bool condition = false)
    {
        return Put(address, 0) | Put(count, 12) | Put(sequence, 16) | Put(boolIndex, 34) | Put(condition ? 1 : 0, 42) |
               Put(static_cast<std::uint32_t>(op), 44);
    }

    // duas instrucoes de controle de fluxo -> 3 dwords
    void PushCfPair(std::vector<std::uint32_t>& code, std::uint64_t a, std::uint64_t b)
    {
        code.push_back(static_cast<std::uint32_t>(a));
        code.push_back(static_cast<std::uint32_t>((a >> 32) & 0xFFFF) | static_cast<std::uint32_t>((b & 0xFFFF) << 16));
        code.push_back(static_cast<std::uint32_t>(b >> 16));
    }

    struct AluFields
    {
        std::uint32_t vop = 0, sop = 50, vdst = 0, sdst = 0, vmask = 0, smask = 0;
        bool exportData = false;
        std::uint32_t reg[3] = { 0, 0, 0 }, swz[3] = { 0, 0, 0 };
        bool sel[3] = { true, true, true };
    };

    void PushAlu(std::vector<std::uint32_t>& code, const AluFields& f)
    {
        std::uint32_t w0 = f.vdst | (f.sdst << 8) | (f.exportData ? 1u << 15 : 0u) | (f.vmask << 16) | (f.smask << 20) | (f.sop << 26);
        std::uint32_t w1 = f.swz[2] | (f.swz[1] << 8) | (f.swz[0] << 16);
        std::uint32_t w2 = f.reg[2] | (f.reg[1] << 8) | (f.reg[0] << 16) | (f.vop << 24) | (f.sel[2] ? 1u << 29 : 0u) |
                           (f.sel[1] ? 1u << 30 : 0u) | (f.sel[0] ? 1u << 31 : 0u);
        code.push_back(w0);
        code.push_back(w1);
        code.push_back(w2);
    }

    bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
}

int main()
{
    // ---------------------------------------------------------------- decodificacao do controle de fluxo
    {
        const CfInstruction cf = DecodeCf(CfExec(CfOpcode::CondExecEnd, 0x123, 5, 0xABC, 77, true));
        Equal("cf opcode", static_cast<int>(cf.opcode), static_cast<int>(CfOpcode::CondExecEnd));
        Equal("cf endereco", cf.address, 0x123);
        Equal("cf contagem", cf.count, 5);
        Equal("cf sequencia", cf.sequence, 0xABC);
        Equal("cf booleana", cf.boolIndex, 77);
        Check("cf condicao", cf.condition);
        Check("cf termina", cf.EndsProgram());

        const CfInstruction jump = DecodeCf(Put(9, 0) | Put(1, 14) | Put(1, 42) | Put(11, 44));
        Equal("salto destino", jump.address, 9);
        Check("salto por predicado", jump.predicated && !jump.unconditional && jump.condition);

        const CfInstruction loop = DecodeCf(Put(6, 0) | Put(3, 16) | Put(7, 44));
        Equal("laco destino", loop.address, 6);
        Equal("laco constante", loop.loopId, 3);
    }

    // ---------------------------------------------------------------- decodificacao de ALU
    {
        AluFields f;
        f.vop = 11; f.sop = 25; f.vdst = 5; f.sdst = 9; f.vmask = 0xA; f.smask = 0x1;
        f.reg[0] = 3; f.reg[1] = 200; f.reg[2] = 0x80 | 7;
        f.swz[0] = 0x1B; f.sel[1] = false;
        std::vector<std::uint32_t> w;
        PushAlu(w, f);
        const AluInstruction a = DecodeAlu(w.data());
        Equal("alu vetor", static_cast<int>(a.vectorOpcode), 11);
        Equal("alu escalar", static_cast<int>(a.scalarOpcode), 25);
        Equal("alu destino vetor", a.vectorDest, 5);
        Equal("alu destino escalar", a.scalarDest, 9);
        Equal("alu mascara vetor", a.vectorWriteMask, 0xA);
        Equal("alu src1", a.src[0].index, 3);
        Check("alu src2 constante", !a.src[1].isRegister && a.src[1].index == 200);
        Equal("alu src3 (abs)", a.src[2].index, 0x87);
        Equal("alu swizzle src1", a.src[0].swizzle, 0x1B);
    }

    // ---------------------------------------------------------------- decodificacao de leitura de vertice
    {
        // opcode 0, src r1.y, dst r2, slot 4*3+1 = 13; dst xyz1; formato 6; stride 3; offset 2
        const std::uint32_t w[3] = {
            0u | (1u << 5) | (2u << 12) | (1u << 19) | (4u << 20) | (1u << 25) | (1u << 30),
            (0u | (1u << 3) | (2u << 6) | (5u << 9)) | (6u << 16) | (63u << 24),
            3u | (2u << 8),
        };
        const FetchInstruction f = DecodeFetch(w);
        Equal("vfetch slot", f.constIndex, 13);
        Equal("vfetch origem", f.srcRegister, 1);
        Equal("vfetch componente", f.srcComponent, 1);
        Equal("vfetch destino", f.dstRegister, 2);
        Equal("vfetch formato", f.format, 6);
        Check("vfetch ajuste de expoente -1", f.expAdjust == -1);
        Equal("vfetch stride", f.stride, 3);
        Equal("vfetch offset", f.offset, 2);
    }

    // ---------------------------------------------------------------- shader de vertices completo
    {
        // cf0: exec_end com 2 instrucoes (leitura + ALU) a partir da instrucao 1; cf1: nop
        std::vector<std::uint32_t> code;
        PushCfPair(code, CfExec(CfOpcode::ExecEnd, 1, 2, 0b0001), 0);
        code.push_back(0u | (0u << 5) | (1u << 12) | (1u << 19));   // vfetch r1 <- slot 0, indice em r0.x
        code.push_back(0u | (1u << 3) | (2u << 6) | (3u << 9) | (57u << 16));
        code.push_back(4u);
        AluFields f;   // oPos = r1 (max(r1, r1)), exportando
        f.vop = 2; f.vdst = 62; f.vmask = 0xF; f.exportData = true; f.reg[0] = 1; f.reg[1] = 1;
        PushAlu(code, f);

        const HlslShader s = TranslateToHlsl(code, HlslOptions{});
        Check("vs le o vertice", Contains(s.body, "VFetch(uint(r[0].x), 0u, 0u, 4u, 57u"));
        Check("vs exporta a posicao", Contains(s.body, "oPos.xyzw = vr.xyzw;"));
        Check("vs sem maquina de estados", !s.loopMode);
        Equal("vs slots", s.fetchSlots.size(), 1);
        if (g_failures) std::printf("%s\n", s.body.c_str());
    }

    // ---------------------------------------------------------------- vetor e escalar leem antes de gravar
    {
        // vetor: r0 = r0 + r0 (x); escalar: r1.x = r0.w (a) * r0.x (b), lendo o r0 antigo
        std::vector<std::uint32_t> code;
        PushCfPair(code, CfExec(CfOpcode::ExecEnd, 1, 1, 0), 0);
        AluFields f;
        f.vop = 0; f.sop = 2; f.vdst = 0; f.vmask = 1; f.sdst = 1; f.smask = 1;
        f.reg[0] = 0; f.reg[1] = 0; f.reg[2] = 0; f.swz[2] = 0;
        PushAlu(code, f);
        const HlslShader s = TranslateToHlsl(code, HlslOptions{ true, false });
        const auto read = s.body.find("float sa = r[0].w;");
        const auto write = s.body.find("r[0].x = vr.x;");
        Check("escalar le r0 antes do vetor gravar", read != std::string::npos && write != std::string::npos && read < write);
        if (g_failures) std::printf("%s\n", s.body.c_str());
    }

    // ---------------------------------------------------------------- laco usa a maquina de estados
    {
        std::vector<std::uint32_t> code;
        PushCfPair(code, Put(2, 0) | Put(0, 16) | Put(7, 44), CfExec(CfOpcode::Exec, 2, 0, 0));
        PushCfPair(code, Put(1, 0) | Put(0, 16) | Put(8, 44), CfExec(CfOpcode::ExecEnd, 2, 0, 0));
        const HlslShader s = TranslateToHlsl(code, HlslOptions{});
        Check("laco: maquina de estados", s.loopMode);
        Check("laco: le a constante de laco", Contains(s.body, "loops[0][0]"));
    }

    std::printf(g_failures ? "%d falha(s)\n" : "tradutor de shaders: tudo certo\n", g_failures);
    return g_failures ? 1 : 0;
}
