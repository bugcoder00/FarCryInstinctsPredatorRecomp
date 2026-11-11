// Gerador de HLSL a partir do microcodigo Xenos (XenosMicrocode).
//
// Gera so o CORPO da funcao principal do shader. Quem chama (o runtime do jogo) monta o resto e fornece:
//   - float4 r[64]                        registradores temporarios (no pixel shader, r0-r15 ja com os interpoladores)
//   - float4 c[256]; uint4 loops[8]       constantes float e de laco do estagio
//   - vertex shader: float4 oPos, o[16], oDummy
//   - pixel shader:  float4 oC0, oC1, oC2, oC3, oDepth4, oDummy
//   - funcoes: BoolConst, VFetch, TFetch2D, TFetch2DOff, TFetchCube, TWeights, TComputedLod, XeCube, dst, max4
// O corpo declara o proprio estado (p0, a0, aL, ps e o controle de fluxo).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace predator::gpu
{
    struct HlslOptions
    {
        bool pixel = false;       // pixel shader (senao vertex shader)
        bool forceLoop = false;   // usa sempre a maquina de estados (laco + switch), mesmo sem saltos
    };

    struct HlslShader
    {
        std::string body;
        std::vector<std::uint32_t> fetchSlots;   // slots de vertex fetch (0-95) lidos, na ordem do primeiro uso
        std::uint32_t textureMask = 0;           // bit n = le a textura n
        std::uint32_t cubeMask = 0;              // bit n = a textura n e lida como cubo
        bool writesDepth = false;
        bool loopMode = false;                   // gerado como maquina de estados
        std::vector<std::string> warnings;       // sem repeticao
    };

    [[nodiscard]] HlslShader TranslateToHlsl(std::span<const std::uint32_t> code, const HlslOptions& options);
}
