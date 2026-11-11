// Cria o image.bin (imagem de memoria do jogo) a partir do default.xex da copia do jogo de quem joga, para o pacote de
// distribuicao nao levar nenhum arquivo do jogo.
//
// XEX2 (cabecalho big-endian): +0x08 inicio dos dados do PE, +0x10 bloco de seguranca, +0x14 numero de cabecalhos
// opcionais, depois pares {chave, valor}. Chave 0x3FF = formato dos dados: {u32 tamanho, u16 criptografia,
// u16 compressao, blocos}. Na compressao "basica" (1) os blocos sao pares {u32 bytes de dados, u32 bytes de zeros}.
// Bloco de seguranca: +0x04 tamanho da imagem, +0x150 chave do arquivo (AES-128), que se abre com a chave de varejo do
// console (AES-ECB); com ela os dados se abrem em AES-CBC com vetor inicial zero.
// O resultado e conferido pelo SHA-256 da versao do jogo para a qual este executavel foi gerado.
#include "guest.h"
#include <windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace
{
    // SHA-256 da imagem montada daqui do default.xex de Far Cry Instincts Predator (USA/Europa) para o qual este
    // executavel foi gerado (o image.bin antigo do desenvolvimento difere so no cabecalho do PE, nos atalhos das
    // importacoes e no fim dos recursos, partes que o codigo traduzido nao usa; conferido jogando)
    // (o hash e o tamanho de cada programa, jogo ou editor de mapas, ficam em bridge/guest_cpu_backend.h)
    const char* const HASH_ESPERADO = GUEST_IMAGE_SHA256;
    const uint8_t CHAVE_VAREJO[16] = { 0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3, 0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91 };

    uint32_t Be32(const std::vector<uint8_t>& d, size_t p) { return p + 4 <= d.size() ? (d[p] << 24 | d[p + 1] << 16 | d[p + 2] << 8 | d[p + 3]) : 0; }
    uint16_t Be16(const std::vector<uint8_t>& d, size_t p) { return p + 2 <= d.size() ? (uint16_t)(d[p] << 8 | d[p + 1]) : 0; }

    // AES-128 sem preenchimento: ECB (iv nulo) ou CBC (iv = 16 zeros), no lugar
    bool Aes(const uint8_t* chave, uint8_t* dados, ULONG tamanho, bool cbc)
    {
        BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE k = nullptr;
        bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0;
        const wchar_t* modo = cbc ? BCRYPT_CHAIN_MODE_CBC : BCRYPT_CHAIN_MODE_ECB;
        ok = ok && BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)modo, (ULONG)((wcslen(modo) + 1) * 2), 0) == 0;
        ok = ok && BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, (PUCHAR)chave, 16, 0) == 0;
        uint8_t iv[16] = {};
        ULONG feito = 0;
        ok = ok && BCryptDecrypt(k, dados, tamanho, nullptr, cbc ? iv : nullptr, cbc ? 16 : 0, dados, tamanho, &feito, 0) == 0;
        if (k) BCryptDestroyKey(k);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        return ok;
    }

    std::string Sha256(const std::vector<uint8_t>& d)
    {
        BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
        uint8_t out[32] = {};
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
            BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0)
        {
            BCryptHashData(h, (PUCHAR)d.data(), (ULONG)d.size(), 0);
            BCryptFinishHash(h, out, 32, 0);
        }
        if (h) BCryptDestroyHash(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        std::string s;
        for (uint8_t b : out) { char t[3]; snprintf(t, 3, "%02x", b); s += t; }
        return s;
    }
}

// Cria "saida" a partir do default.xex (ou do ige.xex no editor). Devolve false com a explicacao em erro.
// conferir = false (so para depuracao, "--extrair-imagem"): aceita qualquer executavel, sem conferir tamanho nem hash.
bool XexCriarImagem(const std::filesystem::path& xex, const std::filesystem::path& saida, std::string& erro, bool conferir)
{
    std::ifstream f(xex, std::ios::binary);
    if (!f) { erro = "nao achei o default.xex na pasta do jogo"; return false; }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 0x200 || memcmp(d.data(), "XEX2", 4) != 0) { erro = "o default.xex nao e um executavel do Xbox 360 (XEX2)"; return false; }

    uint32_t dadosPe = Be32(d, 0x08), seg = Be32(d, 0x10), nCab = Be32(d, 0x14);
    uint32_t formato = 0;
    for (uint32_t i = 0; i < nCab && i < 64; i++)
        if (Be32(d, 0x18 + i * 8) == 0x3FF) formato = Be32(d, 0x18 + i * 8 + 4);
    uint32_t tamImagem = Be32(d, seg + 4);
    if (!formato || !tamImagem || (conferir && tamImagem != GUEST_IMAGE_SIZE) || dadosPe >= d.size())
    { erro = "o default.xex nao e de Far Cry Instincts Predator (tamanho da imagem diferente)"; return false; }
    uint32_t tamFormato = Be32(d, formato);
    uint16_t cripto = Be16(d, formato + 4), compressao = Be16(d, formato + 6);
    if (compressao != 1) { erro = "este default.xex usa outro tipo de compressao (versao diferente do jogo)"; return false; }

    std::vector<uint8_t> pe(d.begin() + dadosPe, d.end());
    pe.resize(pe.size() & ~size_t(15));
    if (cripto == 1)
    {
        uint8_t chave[16];
        memcpy(chave, d.data() + seg + 0x150, 16);
        if (!Aes(CHAVE_VAREJO, chave, 16, false) || !Aes(chave, pe.data(), (ULONG)pe.size(), true))
        { erro = "falha ao descriptografar o default.xex"; return false; }
    }

    std::vector<uint8_t> img(tamImagem, 0);
    size_t lido = 0, escrito = 0;
    for (uint32_t p = formato + 8; p + 8 <= formato + tamFormato; p += 8)
    {
        uint32_t dados = Be32(d, p), zeros = Be32(d, p + 4);
        if (lido + dados > pe.size() || escrito + dados + zeros > img.size()) { erro = "default.xex incompleto ou corrompido"; return false; }
        memcpy(img.data() + escrito, pe.data() + lido, dados);
        lido += dados;
        escrito += dados + zeros;
    }

    std::string hash = Sha256(img);
    if (!conferir) Log("[imagem] SHA-256 %s, %u bytes", hash.c_str(), tamImagem);
    else if (hash != HASH_ESPERADO)
    {
        erro = "a copia do jogo e de outra versao (ou esta corrompida): este executavel funciona so com Far Cry Instincts "
               "Predator para Xbox 360 (USA/Europa)";
        Log("[imagem] SHA-256 %s (esperado %s)", hash.c_str(), HASH_ESPERADO);
        if (getenv("FCRECOMP_IMAGEMDIFERENTE"))   // depuracao: grava a imagem que nao bateu para comparar
        { std::ofstream o(saida.string() + ".diferente", std::ios::binary); o.write((const char*)img.data(), img.size()); }
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(saida.parent_path(), ec);
    std::ofstream o(saida, std::ios::binary);
    if (!o.write((const char*)img.data(), img.size())) { erro = "nao consegui gravar " + saida.string(); return false; }
    Log("[imagem] %s criado a partir de %s", saida.filename().string().c_str(), xex.string().c_str());
    return true;
}
