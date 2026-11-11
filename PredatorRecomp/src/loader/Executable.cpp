#include "loader/Executable.h"

#include "loader/ByteOrder.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace predator::loader
{
    namespace
    {
        // chaves dos cabecalhos opcionais do XEX2 usadas aqui
        constexpr std::uint32_t kHeaderFileFormat = 0x000003FF;
        constexpr std::uint32_t kHeaderEntryPoint = 0x00010100;
        constexpr std::uint32_t kHeaderBaseAddress = 0x00010201;
        constexpr std::uint32_t kHeaderImportLibraries = 0x000103FF;
        constexpr std::uint32_t kHeaderOriginalName = 0x000183FF;
        constexpr std::uint32_t kHeaderDefaultStackSize = 0x00020200;

        // bloco de seguranca
        constexpr std::size_t kSecImageSize = 0x004;
        constexpr std::size_t kSecImageFlags = 0x10C;
        constexpr std::size_t kSecLoadAddress = 0x110;
        constexpr std::size_t kSecPageCount = 0x180;
        constexpr std::size_t kSecPages = 0x184;
        constexpr std::size_t kPageDescriptorSize = 24;   // u32 (paginas << 4 | tipo) + SHA-1 de 20 bytes
        constexpr std::uint32_t kImageFlagPage4K = 0x10000000;

        std::optional<std::vector<std::uint8_t>> ReadFile(const std::filesystem::path& path, std::string& error)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                error = "nao foi possivel abrir " + path.string();
                return std::nullopt;
            }
            in.seekg(0, std::ios::end);
            const auto size = static_cast<std::streamoff>(in.tellg());
            if (size < 0)
            {
                error = "nao foi possivel ler " + path.string();
                return std::nullopt;
            }
            in.seekg(0, std::ios::beg);
            std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
            if (!data.empty() && !in.read(reinterpret_cast<char*>(data.data()), size))
            {
                error = "erro ao ler " + path.string();
                return std::nullopt;
            }
            return data;
        }

        // Localiza os dados de um cabecalho opcional: o byte baixo da chave diz o formato
        //   0x00 / 0x01: o proprio valor e o dado; 0xFF: o valor e o deslocamento e o primeiro u32 la e o tamanho;
        //   outros: o valor e o deslocamento e o tamanho e (byte baixo * 4)
        std::span<const std::uint8_t> HeaderData(ByteReader& r, const OptionalHeader& h)
        {
            const std::uint32_t low = h.key & 0xFF;
            if (low <= 1) return {};
            if (low == 0xFF) return r.Bytes(h.value, r.U32(h.value));
            return r.Bytes(h.value, std::size_t{ low } * 4);
        }

        bool ParseImports(std::span<const std::uint8_t> data, std::vector<ImportLibrary>& out, std::string& error)
        {
            ByteReader r(data);
            const std::uint32_t stringTableSize = r.U32(4);
            const std::uint32_t libraryCount = r.U32(8);
            std::vector<std::string> names;
            for (std::size_t p = 12; p < 12 + std::size_t{ stringTableSize } && r.Has(p, 1);)
            {
                auto bytes = r.Bytes(p, 1);
                if (bytes[0] == 0) { ++p; continue; }   // preenchimento entre nomes
                std::size_t end = p;
                while (r.Has(end, 1) && data[end] != 0) ++end;
                names.emplace_back(reinterpret_cast<const char*>(data.data() + p), end - p);
                p = end + 1;
            }
            std::size_t p = 12 + std::size_t{ stringTableSize };
            for (std::uint32_t i = 0; i < libraryCount; ++i)
            {
                const std::uint32_t size = r.U32(p);
                ImportLibrary lib;
                lib.version = r.U32(p + 28);
                lib.minimumVersion = r.U32(p + 32);
                const std::uint16_t nameIndex = r.U16(p + 36);
                const std::uint16_t recordCount = r.U16(p + 38);
                for (std::uint16_t k = 0; k < recordCount; ++k) lib.records.push_back(r.U32(p + 40 + std::size_t{ k } * 4));
                lib.name = nameIndex < names.size() ? names[nameIndex] : "?";
                if (!r.Ok() || size < 40)
                {
                    error = "tabela de importacoes invalida";
                    return false;
                }
                out.push_back(std::move(lib));
                p += size;
            }
            return true;
        }
    }

    const char* RegionKindName(RegionKind kind) noexcept
    {
        switch (kind)
        {
        case RegionKind::Code: return "codigo";
        case RegionKind::Data: return "dados";
        case RegionKind::ReadOnly: return "somente leitura";
        case RegionKind::Unknown: break;
        }
        return "desconhecido";
    }

    const char* OptionalHeaderName(std::uint32_t key) noexcept
    {
        switch (key)
        {
        case 0x000002FF: return "recursos";
        case kHeaderFileFormat: return "formato dos dados";
        case kHeaderEntryPoint: return "ponto de entrada";
        case kHeaderBaseAddress: return "endereco base";
        case kHeaderImportLibraries: return "bibliotecas importadas";
        case 0x00018002: return "checksum/data";
        case kHeaderOriginalName: return "nome original do PE";
        case 0x000200FF: return "bibliotecas estaticas";
        case 0x00020104: return "TLS";
        case kHeaderDefaultStackSize: return "tamanho padrao da pilha";
        case 0x00040006: return "informacoes de execucao";
        case 0x00040310: return "classificacao etaria";
        case 0x00040404: return "chave de rede local";
        case 0x000405FF: return "logotipo";
        default: return "";
        }
    }

    std::optional<Executable> Executable::Load(const std::filesystem::path& xexPath, const std::filesystem::path& imagePath,
                                               std::string& error)
    {
        auto file = ReadFile(xexPath, error);
        if (!file) return std::nullopt;
        ByteReader r(*file);

        if (r.Size() < 24 || std::memcmp(file->data(), "XEX2", 4) != 0)
        {
            error = xexPath.string() + " nao e um executavel XEX2";
            return std::nullopt;
        }

        Executable exe;
        ExecutableInfo& info = exe.m_info;
        info.moduleFlags = r.U32(4);
        const std::uint32_t peOffset = r.U32(8);
        const std::uint32_t securityOffset = r.U32(16);
        const std::uint32_t headerCount = r.U32(20);
        if (headerCount > 4096)
        {
            error = "cabecalho XEX2 com numero de campos invalido";
            return std::nullopt;
        }

        std::vector<std::uint32_t> basicBlocks;   // compressao basica: pares (bytes de dados, bytes de zeros)
        for (std::uint32_t i = 0; i < headerCount; ++i)
        {
            OptionalHeader h{ r.U32(24 + std::size_t{ i } * 8), r.U32(28 + std::size_t{ i } * 8) };
            info.headers.push_back(h);
            switch (h.key)
            {
            case kHeaderEntryPoint: info.entryPoint = h.value; break;
            case kHeaderBaseAddress: info.baseAddress = h.value; break;
            case kHeaderDefaultStackSize: info.defaultStackSize = h.value; break;
            case kHeaderOriginalName:
            {
                auto d = HeaderData(r, h);
                if (d.size() > 4)
                {
                    auto name = d.subspan(4);
                    auto end = std::find(name.begin(), name.end(), std::uint8_t{ 0 });
                    info.originalName.assign(name.begin(), end);
                }
                break;
            }
            case kHeaderFileFormat:
            {
                auto d = HeaderData(r, h);
                ByteReader f(d);
                info.encryption = f.U16(4);
                info.compression = f.U16(6);
                if (info.compression == 1)
                    for (std::size_t p = 8; f.Has(p, 8); p += 8)
                    {
                        basicBlocks.push_back(f.U32(p));
                        basicBlocks.push_back(f.U32(p + 4));
                    }
                break;
            }
            case kHeaderImportLibraries:
                if (!ParseImports(HeaderData(r, h), info.imports, error)) return std::nullopt;
                break;
            default:
                break;
            }
        }

        info.imageSize = r.U32(securityOffset + kSecImageSize);
        const std::uint32_t imageFlags = r.U32(securityOffset + kSecImageFlags);
        if (!info.baseAddress) info.baseAddress = r.U32(securityOffset + kSecLoadAddress);
        info.pageSize = (imageFlags & kImageFlagPage4K) ? 0x1000u : 0x10000u;
        if (!r.Ok() || !info.baseAddress || !info.imageSize)
        {
            error = "cabecalho XEX2 incompleto (base ou tamanho da imagem ausentes)";
            return std::nullopt;
        }

        // mapa de paginas: cada descritor cobre N paginas de um tipo (codigo, dados, somente leitura)
        const std::uint32_t pageCount = r.U32(securityOffset + kSecPageCount);
        std::uint32_t address = info.baseAddress;
        for (std::uint32_t i = 0; i < pageCount && r.Ok(); ++i)
        {
            const std::uint32_t d = r.U32(securityOffset + kSecPages + std::size_t{ i } * kPageDescriptorSize);
            const std::uint32_t size = (d >> 4) * info.pageSize;
            const auto kind = static_cast<RegionKind>((d & 0xF) <= 3 ? (d & 0xF) : 0);
            if (!info.regions.empty() && info.regions.back().kind == kind && info.regions.back().start + info.regions.back().size == address)
                info.regions.back().size += size;
            else
                info.regions.push_back(Region{ address, size, kind });
            address += size;
        }
        if (!r.Ok())
        {
            error = "mapa de paginas do XEX2 invalido";
            return std::nullopt;
        }

        // imagem de memoria
        if (!imagePath.empty())
        {
            auto image = ReadFile(imagePath, error);
            if (!image) return std::nullopt;
            if (image->size() != info.imageSize)
            {
                error = "a imagem " + imagePath.string() + " tem " + std::to_string(image->size()) + " bytes; o XEX2 indica " +
                        std::to_string(info.imageSize);
                return std::nullopt;
            }
            exe.m_image = std::move(*image);
        }
        else if (info.encryption != 0)
        {
            error = "o executavel esta criptografado: informe a imagem ja descriptografada com --image <arquivo>";
            return std::nullopt;
        }
        else if (info.compression == 0 || info.compression == 1)
        {
            exe.m_image.assign(info.imageSize, 0);
            std::size_t src = peOffset, dst = 0;
            if (info.compression == 0) basicBlocks = { static_cast<std::uint32_t>(r.Size() - peOffset), 0 };
            for (std::size_t b = 0; b + 1 < basicBlocks.size(); b += 2)
            {
                const std::uint32_t dataSize = basicBlocks[b], zeroSize = basicBlocks[b + 1];
                auto bytes = r.Bytes(src, dataSize);
                if (!r.Ok() || dst + dataSize > exe.m_image.size())
                {
                    error = "dados do PE fora dos limites (compressao basica)";
                    return std::nullopt;
                }
                std::copy(bytes.begin(), bytes.end(), exe.m_image.begin() + static_cast<std::ptrdiff_t>(dst));
                src += dataSize;
                dst += std::size_t{ dataSize } + zeroSize;   // os zeros ja estao no buffer
            }
        }
        else
        {
            error = "compressao do XEX2 nao suportada (tipo " + std::to_string(info.compression) +
                    "): informe a imagem ja descomprimida com --image <arquivo>";
            return std::nullopt;
        }
        return exe;
    }

    Executable Executable::FromImage(std::uint32_t baseAddress, std::vector<std::uint8_t> image)
    {
        Executable exe;
        exe.m_info.baseAddress = baseAddress;
        exe.m_info.entryPoint = baseAddress;
        exe.m_info.imageSize = static_cast<std::uint32_t>(image.size());
        exe.m_info.regions.push_back({ baseAddress, exe.m_info.imageSize, RegionKind::Code });
        exe.m_image = std::move(image);
        return exe;
    }

    bool Executable::Contains(std::uint32_t address, std::uint32_t size) const noexcept
    {
        if (address < m_info.baseAddress) return false;
        const std::uint64_t offset = std::uint64_t{ address } - m_info.baseAddress;
        return offset + size <= m_image.size();
    }

    std::optional<std::uint32_t> Executable::Read32(std::uint32_t address) const noexcept
    {
        if ((address & 3) != 0 || !Contains(address, 4)) return std::nullopt;
        return ReadBE32(m_image.data() + (address - m_info.baseAddress));
    }

    const Region* Executable::RegionAt(std::uint32_t address) const noexcept
    {
        for (const auto& region : m_info.regions)
            if (address >= region.start && address - region.start < region.size) return &region;
        return nullptr;
    }
}
