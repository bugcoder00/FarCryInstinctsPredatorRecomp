// Modo de teste do tradutor: FCRECOMP_SHADERTEST=pasta traduz e compila todos os shaders .bin da pasta
#include "guest.h"
#include "render.h"
#include "xenos_translate.h"
extern uint64_t g_translateHash;   // xenos_translate.cpp
#include <filesystem>
#include <fstream>

int RunShaderTest(const char* dir)
{
    namespace fs = std::filesystem;
    int ok = 0, fail = 0;
    for (auto& e : fs::directory_iterator(dir))
    {
        if (e.path().extension() != ".bin") continue;
        std::ifstream f(e.path(), std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
        std::vector<uint32_t> code(bytes.size() / 4);
        for (size_t i = 0; i < code.size(); i++)
            code[i] = __builtin_bswap32(*(uint32_t*)&bytes[i * 4]);

        bool pixel = e.path().filename().string().rfind("ps_", 0) == 0;
        // hash do nome (ps_HASH.bin): o tradutor usa para as excecoes por shader
        g_translateHash = strtoull(e.path().stem().string().substr(3).c_str(), nullptr, 16);
        std::string warnings, errors;
        std::string hlsl = TranslateXenosShader(code, pixel, &warnings);
        auto hlslPath = e.path();
        hlslPath.replace_extension(".hlsl");
        std::ofstream(hlslPath) << hlsl;

        auto blob = render::CompileShader(hlsl, pixel ? L"ps_6_0" : L"vs_6_0", &errors);
        if (blob.empty())
        {
            fail++;
            Log("[teste] FALHOU %s:\n%s", e.path().filename().string().c_str(), errors.c_str());
        }
        else
        {
            ok++;
            Log("[teste] ok %s (%zu bytes DXIL)%s%s", e.path().filename().string().c_str(), blob.size(),
                warnings.empty() ? "" : " avisos: ", warnings.c_str());
        }
    }
    Log("[teste] %d ok, %d falharam", ok, fail);
    return fail ? 1 : 0;
}
