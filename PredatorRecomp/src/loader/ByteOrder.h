// Leitura de inteiros big-endian (o formato do executavel e da memoria do console)
#pragma once

#include <cstdint>
#include <cstring>
#include <span>

namespace predator::loader
{
    [[nodiscard]] inline std::uint16_t ReadBE16(const std::uint8_t* p) noexcept
    {
        return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
    }

    [[nodiscard]] inline std::uint32_t ReadBE32(const std::uint8_t* p) noexcept
    {
        return (std::uint32_t{ p[0] } << 24) | (std::uint32_t{ p[1] } << 16) | (std::uint32_t{ p[2] } << 8) | std::uint32_t{ p[3] };
    }

    [[nodiscard]] constexpr std::uint32_t ByteSwap32(std::uint32_t v) noexcept
    {
        return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    }

    // Leitor com conferencia de limites: qualquer leitura fora do buffer marca o leitor como invalido e devolve 0
    class ByteReader
    {
    public:
        explicit ByteReader(std::span<const std::uint8_t> data) : m_data(data) {}

        [[nodiscard]] bool Has(std::size_t offset, std::size_t size) const noexcept
        {
            return offset <= m_data.size() && size <= m_data.size() - offset;
        }
        [[nodiscard]] std::uint16_t U16(std::size_t offset) noexcept
        {
            if (!Has(offset, 2)) { m_ok = false; return 0; }
            return ReadBE16(m_data.data() + offset);
        }
        [[nodiscard]] std::uint32_t U32(std::size_t offset) noexcept
        {
            if (!Has(offset, 4)) { m_ok = false; return 0; }
            return ReadBE32(m_data.data() + offset);
        }
        [[nodiscard]] std::span<const std::uint8_t> Bytes(std::size_t offset, std::size_t size) noexcept
        {
            if (!Has(offset, size)) { m_ok = false; return {}; }
            return m_data.subspan(offset, size);
        }
        [[nodiscard]] bool Ok() const noexcept { return m_ok; }
        [[nodiscard]] std::size_t Size() const noexcept { return m_data.size(); }

    private:
        std::span<const std::uint8_t> m_data;
        bool m_ok = true;
    };
}
