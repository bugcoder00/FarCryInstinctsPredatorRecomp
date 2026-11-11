// Carregador do executavel do console (formato XEX2) e da imagem de memoria
//
// O XEX2 traz um cabecalho com campos opcionais (chave/valor), um bloco de seguranca (tamanho da imagem, chave AES,
// mapa de paginas) e os dados do PE, normalmente criptografados e comprimidos. Este carregador:
//   - le os cabecalhos (endereco base, ponto de entrada, bibliotecas importadas, formato dos dados);
//   - monta a imagem de memoria quando os dados nao estao criptografados (sem compressao ou compressao "basica");
//   - ou usa uma imagem ja descriptografada fornecida a parte (--image), conferindo o tamanho;
//   - classifica as regioes da imagem (codigo, dados, somente leitura) pelo mapa de paginas do bloco de seguranca.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace predator::loader
{
    enum class RegionKind : std::uint8_t
    {
        Unknown = 0,
        Code = 1,
        Data = 2,
        ReadOnly = 3,
    };

    struct Region
    {
        std::uint32_t start = 0;   // endereco virtual
        std::uint32_t size = 0;
        RegionKind kind = RegionKind::Unknown;
    };

    struct ImportLibrary
    {
        std::string name;
        std::uint32_t version = 0;
        std::uint32_t minimumVersion = 0;
        std::vector<std::uint32_t> records;   // enderecos das entradas importadas (variaveis e "thunks" de funcao)
    };

    struct OptionalHeader
    {
        std::uint32_t key = 0;
        std::uint32_t value = 0;   // valor direto ou deslocamento dos dados no arquivo
    };

    struct ExecutableInfo
    {
        std::uint32_t moduleFlags = 0;
        std::uint32_t baseAddress = 0;
        std::uint32_t entryPoint = 0;
        std::uint32_t imageSize = 0;
        std::uint32_t pageSize = 0;
        std::uint16_t encryption = 0;    // 0 nenhuma, 1 normal
        std::uint16_t compression = 0;   // 0 nenhuma, 1 basica, 2 normal (LZX), 3 delta
        std::uint32_t defaultStackSize = 0;
        std::string originalName;
        std::vector<OptionalHeader> headers;
        std::vector<ImportLibrary> imports;
        std::vector<Region> regions;
    };

    class Executable
    {
    public:
        // Carrega o XEX2. Se imagePath nao estiver vazio, a imagem de memoria vem desse arquivo (ja descriptografada).
        // Em caso de erro devolve nullopt e preenche error.
        [[nodiscard]] static std::optional<Executable> Load(const std::filesystem::path& xexPath,
                                                            const std::filesystem::path& imagePath, std::string& error);

        // Imagem montada a mao (testes): tudo vira uma unica regiao de codigo a partir de baseAddress
        [[nodiscard]] static Executable FromImage(std::uint32_t baseAddress, std::vector<std::uint8_t> image);

        [[nodiscard]] const ExecutableInfo& Info() const noexcept { return m_info; }
        [[nodiscard]] std::span<const std::uint8_t> Image() const noexcept { return m_image; }

        [[nodiscard]] bool Contains(std::uint32_t address, std::uint32_t size = 1) const noexcept;
        // Le uma palavra de 32 bits big-endian no endereco virtual (nullopt fora da imagem ou desalinhado)
        [[nodiscard]] std::optional<std::uint32_t> Read32(std::uint32_t address) const noexcept;
        [[nodiscard]] const Region* RegionAt(std::uint32_t address) const noexcept;

    private:
        ExecutableInfo m_info;
        std::vector<std::uint8_t> m_image;
    };

    [[nodiscard]] const char* RegionKindName(RegionKind kind) noexcept;
    [[nodiscard]] const char* OptionalHeaderName(std::uint32_t key) noexcept;
}
