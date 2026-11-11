// Tradutor de shaders Xenos (microcode da GPU do Xbox 360) para HLSL.
// O corpo do shader vem do tradutor do PredatorRecomp (PredatorRecomp/src/gpu/XenosHlsl); aqui so se monta o resto:
// cabecalho comum (xenos_common.hlsl.h), entradas/saidas, alpha test e os interruptores de depuracao.
#include "xenos_translate.h"
#include "diag.h"
#include "xenos_common.hlsl.h"
#include "gpu/XenosHlsl.h"

#include <cstdlib>
#include <cstring>

uint64_t g_translateHash = 0;   // hash do shader sendo traduzido (depuracao)

std::string TranslateXenosShader(const std::vector<uint32_t>& code, bool pixelShader, std::string* warnings,
    std::vector<uint32_t>* fetchSlots, uint32_t* textureMask, uint32_t* cubeMask)
{
    // Sem saltos para tras, lacos ou chamadas o corpo sai em linha reta (bem mais rapido na GPU que o laco com switch).
    // Depuracao: FCRECOMP_NOLINEAR=1 usa o laco em todos; =HASH so nesse shader. F5 do diagnostico: todos.
    predator::gpu::HlslOptions options;
    options.pixel = pixelShader;
    static const char* noLinear = getenv("FCRECOMP_NOLINEAR");
    if (noLinear) options.forceLoop = strlen(noLinear) <= 2 || strtoull(noLinear, nullptr, 16) == g_translateHash;
    if (diag::g_loopShaders) options.forceLoop = true;

    const predator::gpu::HlslShader t = predator::gpu::TranslateToHlsl(code, options);
    if (warnings)
    {
        warnings->clear();
        for (const auto& w : t.warnings) *warnings += w + "\n";
    }
    if (fetchSlots) *fetchSlots = t.fetchSlots;
    if (textureMask) *textureMask = t.textureMask;
    if (cubeMask) *cubeMask = t.cubeMask;

    static const bool noTex = getenv("FCRECOMP_NOTEX") != nullptr;
    std::string s = noTex ? "#define NOTEX\n" : "";
    if (!pixelShader) s += "#define XE_VERTEX\n";   // no shader de vertices a textura e lida no nivel 0 (sem derivadas)
    s += XENOS_COMMON_HLSL;
    s += "struct Interp { float4 pos : SV_Position; float4 o[16] : TEXCOORD0; float4 clip0 : SV_ClipDistance0; float2 clip1 : SV_ClipDistance1; };\n";
    if (pixelShader)
    {
        s += "struct PSOut { float4 c0 : SV_Target0; };\n";
        s += "PSOut ps(Interp input, bool frontFace : SV_IsFrontFace)\n{\n";
        s += "\tfloat4 r[64] = (float4[64])0;\n";
        s += "\t[unroll] for (int i = 0; i < 16; i++) r[i] = input.o[i];\n";
        // param gen (SQ_PROGRAM_CNTL.param_gen): x = posicao x na tela (negativo se a face e traseira),
        // y = posicao y, zw = coordenada de ponto (nao usada aqui)
        // indices fixos (unroll): um indice variavel mandaria o array r[] inteiro para a memoria lenta da GPU
        // A posicao e a da EDRAM (ja com o deslocamento da janela), como no hardware: cada faixa do predicated
        // tiling le a sua propria copia da cena (resolvida a partir de x=0) na refracao da agua.
        // Com resolucao interna maior a posicao volta para pixels do jogo (/ resScale).
        s += "\tif (paramGen & 0x80000000u) { float2 sp = input.pos.xy / resScale; float4 pg = float4(frontFace ? sp.x : -sp.x, sp.y, 0, 0);\n"
             "\t\t[unroll] for (uint i = 0; i < 16; i++) if ((paramGen & 63u) == i) r[i] = pg; }\n";
        s += "\tfloat4 oC0 = 0, oC1 = 0, oC2 = 0, oC3 = 0, oDepth4 = 0, oDummy = 0;\n";
    }
    else
    {
        s += "Interp vs(uint vertexId : SV_VertexID)\n{\n";
        s += "\tfloat4 r[64] = (float4[64])0;\n";
        s += "\tr[0].x = float(LoadIndex(vertexId));\n";
        s += "\tfloat4 oPos = float4(0, 0, 0, 1), oDummy = 0;\n";
        s += "\tfloat4 o[16] = (float4[16])0;\n";
    }
    static const bool simplePs = getenv("FCRECOMP_SIMPLEPS") != nullptr;   // depuracao: mede o custo dos pixel shaders
    if (pixelShader && simplePs) s += "\toC0 = float4(0.5, 0.5, 0.5, 1);\n"; else s += t.body;
    // depuracao: FCRECOMP_PSTESTE=HASH:modo troca a saida desse pixel shader (1 = textura x cor do vertice, sem a
    // mistura de nevoa; 2 = mostra os fatores de nevoa interpolados o[1] como cor)
    if (static const char* pt = getenv("FCRECOMP_PSTESTE"); pixelShader && pt && strtoull(pt, nullptr, 16) == g_translateHash)
    {
        int mode = strchr(pt, ':') ? atoi(strchr(pt, ':') + 1) : 1;
        if (mode == 1) s += "\toC0.xyz = TFetch2D(0, input.o[0].xy, false).xyz * input.o[2].xyz;\n";
        else s += "\toC0 = float4(saturate(input.o[1].xyz), 1);\n";
    }
    if (pixelShader)
    {
        // alvo de ponto fixo no Xbox: a cor gravada fica entre 0 e 1 (nossa EDRAM e de ponto flutuante)
        s += "\tif (alphaCtl & 0x10000u) oC0 = saturate(oC0);\n";
        // alpha test (RB_COLORCONTROL): descarta o pixel se a comparacao falhar. So existe na variante compilada
        // com XE_ALPHA_TEST: um "discard" no shader desliga o teste de profundidade antecipado (early-Z) da GPU,
        // e com ele em todos os shaders cada pixel escondido rodava o shader inteiro.
        s += "#ifdef XE_ALPHA_TEST\n";
        s += "\tif (alphaCtl & 8u) { uint f = alphaCtl & 7u; float a = oC0.a; bool pass =\n"
             "\t\tf == 0 ? false : f == 1 ? a < alphaRef : f == 2 ? a == alphaRef : f == 3 ? a <= alphaRef :\n"
             "\t\tf == 4 ? a > alphaRef : f == 5 ? a != alphaRef : f == 6 ? a >= alphaRef : true;\n"
             "\t\tif (!pass) discard; }\n";
        s += "#endif\n";
        // depuracao: pinta de rosa as saidas invalidas (NaN/infinito)
        static const bool nanCheck = getenv("FCRECOMP_NANCHECK") != nullptr;
        if (nanCheck)
            s += "\tif (any(isnan(oC0)) || any(isinf(oC0)) || any(abs(oC0) > 1000.0)) oC0 = float4(1, 0, 1, 1);\n";
        s += "\tPSOut res;\n\tres.c0 = oC0;\n\treturn res;\n}\n";
    }
    else
        s += "\tInterp res;\n\tres.pos = XenosToClip(oPos);\n\tres.o = o;\n\tUserClip(oPos, res.clip0, res.clip1);\n\treturn res;\n}\n";
    return s;
}
