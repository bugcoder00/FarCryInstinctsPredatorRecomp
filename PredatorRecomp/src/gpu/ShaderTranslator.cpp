#include "gpu/ShaderTranslator.h"

#include "core/Diagnostics.h"
#include "gpu/XenosHlsl.h"

#include <format>
#include <fstream>
#include <system_error>

namespace predator::gpu
{
    namespace
    {
        namespace fs = std::filesystem;

        // Shader extraido pelo runtime (FCRECOMP_DUMPSHADERS): vs_HASH.bin / ps_HASH.bin, dwords big-endian
        bool ReadShader(const fs::path& path, std::vector<std::uint32_t>& code)
        {
            std::ifstream f(path, std::ios::binary);
            if (!f) return false;
            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), {});
            if (bytes.size() % 4) return false;
            code.resize(bytes.size() / 4);
            for (std::size_t i = 0; i < code.size(); ++i)
            {
                const unsigned char* p = bytes.data() + i * 4;
                code[i] = (std::uint32_t{ p[0] } << 24) | (std::uint32_t{ p[1] } << 16) | (std::uint32_t{ p[2] } << 8) | p[3];
            }
            return true;
        }

        void Collect(const fs::path& input, std::vector<fs::path>& files)
        {
            std::error_code ec;
            if (fs::is_directory(input, ec))
            {
                for (const auto& e : fs::directory_iterator(input, ec))
                    if (e.path().extension() == ".bin") files.push_back(e.path());
            }
            else files.push_back(input);
        }
    }

    ShaderStatus TranslateShaders(const ShaderTranslatorSettings& settings, Diagnostics& diag)
    {
        std::vector<fs::path> files;
        for (const auto& input : settings.inputs) Collect(input, files);
        std::error_code ec;
        fs::create_directories(settings.outputDir, ec);

        std::size_t ok = 0, failed = 0, withWarnings = 0, loops = 0;
        for (const auto& path : files)
        {
            std::vector<std::uint32_t> code;
            if (!ReadShader(path, code))
            {
                diag.Error("shaders: nao foi possivel ler " + path.string());
                ++failed;
                continue;
            }
            const std::string name = path.stem().string();
            HlslOptions options;
            options.pixel = name.starts_with("ps_");
            const HlslShader shader = TranslateToHlsl(code, options);

            std::string header = std::format("// {} ({} shader, {} dwords){}\n", name, options.pixel ? "pixel" : "vertex", code.size(),
                                             shader.loopMode ? " - maquina de estados" : "");
            if (!shader.fetchSlots.empty())
            {
                header += "// vertex fetch:";
                for (auto s : shader.fetchSlots) header += std::format(" {}", s);
                header += "\n";
            }
            if (shader.textureMask) header += std::format("// texturas: {:08X} (cubo {:08X})\n", shader.textureMask, shader.cubeMask);
            for (const auto& w : shader.warnings) header += "// aviso: " + w + "\n";

            std::ofstream out(settings.outputDir / (name + ".hlsl"));
            out << header << shader.body;
            if (!out)
            {
                diag.Error("shaders: nao foi possivel gravar " + name + ".hlsl");
                ++failed;
                continue;
            }
            ++ok;
            if (shader.loopMode) ++loops;
            if (!shader.warnings.empty())
            {
                ++withWarnings;
                for (const auto& w : shader.warnings) diag.Debug("shaders: " + name + ": " + w);
            }
        }
        diag.Info(std::format("shaders: {} traduzidos ({} como maquina de estados, {} com avisos), {} falharam", ok, loops, withWarnings, failed));
        return failed ? ShaderStatus::Failed : ShaderStatus::Ok;
    }
}
