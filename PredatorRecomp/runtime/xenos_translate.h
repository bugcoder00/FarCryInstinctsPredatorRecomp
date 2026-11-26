// Tradutor de shaders Xenos -> HLSL
#pragma once
#include <string>
#include <vector>
#include <cstdint>

// code: microcode em dwords (ordem do host). Retorna HLSL com ponto de entrada "vs" ou "ps".
// fetchSlots (opcional): slots de vertex fetch (0-95) que o shader le. textureMask: bit n = usa a textura n;
// cubeMask: bit n = a textura n e lida como cubo (TextureCube).
// generico (opcional, so vertex shader): pede o modo de leitura de vertice generico (XenosHlsl.h genericFetch). Na
// volta, generico->ok diz se saiu generico e as listas dizem como montar a tabela vfp[] a cada desenho.
// atributo (entrada): registradores de destino das leituras pelo input layout do pipeline (XenosHlsl.h attribFetch);
// a entrada i (ATTRi, vfp[i]) preenche o registrador dst[i].
struct VsGenerico { bool atributo = false; bool ok = false; std::vector<uint32_t> addrs, src, dst; };
std::string TranslateXenosShader(const std::vector<uint32_t>& code, bool pixelShader, std::string* warnings = nullptr,
    std::vector<uint32_t>* fetchSlots = nullptr, uint32_t* textureMask = nullptr, uint32_t* cubeMask = nullptr,
    VsGenerico* generico = nullptr);
