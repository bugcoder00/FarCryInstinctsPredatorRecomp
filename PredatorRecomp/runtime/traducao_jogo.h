// Traducao PT-BR do jogo aplicada na leitura dos pacotes .fat/.dat (traducao_jogo.cpp)
#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace traducao
{
    // como o jogo ve um arquivo: inteiro montado na memoria (.fat) ou o do disco ate inicioExtra e depois "extra" (.dat)
    struct Virtual
    {
        std::shared_ptr<const std::vector<uint8_t>> inteiro;
        uint64_t inicioExtra = 0;
        std::shared_ptr<const std::vector<uint8_t>> extra;
        uint64_t tamanho = 0;
    };
    // false = o arquivo e lido do disco como sempre
    bool Arquivo(const std::filesystem::path& caminho, uint64_t tamanhoNoDisco, Virtual& v);
    uint32_t Ler(const Virtual& v, HANDLE h, uint64_t at, uint8_t* buf, uint32_t n, uint64_t base = 0);   // base: comeco do arquivo em h (ISO)
    bool Tamanho(const std::filesystem::path& caminho, uint64_t& tamanho);   // tamanho que o jogo ve
}
