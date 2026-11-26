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
        // vertex shader generico: as leituras de vertice (slot, formato, stride, offset, swizzle, registradores) vem da
        // tabela vfp[] do cbuffer Draw a cada desenho, em vez de ficarem fixas no codigo. O D3D do jogo so muda essas
        // palavras do shader (conforme a declaracao de vertices), entao um shader do shaders.dat serve para todas as
        // variantes e pode ser compilado antes de jogar.
        bool genericFetch = false;
        // com genericFetch: os registradores de destino das leituras (vfDst) vem prontos do input assembler da placa, um
        // atributo por registrador, preenchidos no inicio do shader; o runtime monta o input layout de cada variante.
        // So quando as leituras sao as primeiras instrucoes (senao sai normal).
        bool attribFetch = false;
    };

    struct HlslShader
    {
        std::string body;
        std::vector<std::uint32_t> fetchSlots;   // slots de vertex fetch (0-95) lidos, na ordem do primeiro uso
        std::uint32_t textureMask = 0;           // bit n = le a textura n
        std::uint32_t cubeMask = 0;              // bit n = a textura n e lida como cubo
        bool writesDepth = false;
        bool loopMode = false;                   // gerado como maquina de estados
        bool earlyAlpha = false;                 // pixel shader: compensa calcular so a transparencia (oC0.w) antes e
                                                 // descartar cedo no alpha test (a parte dela e pequena perto do resto)
        std::vector<std::string> warnings;       // sem repeticao
        // genericFetch: saiu generico (senao, leituras com enderecamento relativo ou demais: traduzido do jeito normal)
        bool generic = false;
        std::vector<std::uint32_t> vfAddrs;      // posicao (dword) de cada leitura de vertice: entrada k de vfp[]
        std::vector<std::uint32_t> vfSrc;        // registradores de indice possiveis (vfp[k].x bits 24-27 escolhe)
        std::vector<std::uint32_t> vfDst;        // registradores de destino possiveis (vfp[k].z bits 16-19 escolhe)
    };

    [[nodiscard]] HlslShader TranslateToHlsl(std::span<const std::uint32_t> code, const HlslOptions& options);

    // posicoes (dwords, em ordem) das instrucoes de leitura de vertice dos blocos exec
    [[nodiscard]] std::vector<std::uint32_t> VertexFetchAddresses(std::span<const std::uint32_t> code);
    // hash do shader sem os campos que o D3D do jogo preenche nas leituras de vertice: igual para o molde do
    // shaders.dat e para todas as variantes dele
    [[nodiscard]] std::uint64_t VertexFetchMaskedHash(std::span<const std::uint32_t> code);
    constexpr std::uint32_t kMaxGenericFetches = 64;
}
