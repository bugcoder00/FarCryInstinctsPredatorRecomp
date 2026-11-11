// Tradutor de shaders Xenos -> HLSL
#pragma once
#include <string>
#include <vector>
#include <cstdint>

// code: microcode em dwords (ordem do host). Retorna HLSL com ponto de entrada "vs" ou "ps".
// fetchSlots (opcional): slots de vertex fetch (0-95) que o shader le. textureMask: bit n = usa a textura n;
// cubeMask: bit n = a textura n e lida como cubo (TextureCube).
std::string TranslateXenosShader(const std::vector<uint32_t>& code, bool pixelShader, std::string* warnings = nullptr,
    std::vector<uint32_t>* fetchSlots = nullptr, uint32_t* textureMask = nullptr, uint32_t* cubeMask = nullptr);
