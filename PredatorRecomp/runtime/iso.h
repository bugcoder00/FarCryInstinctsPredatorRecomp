// Jogo direto da ISO do disco (sem extrair): leitura do sistema de arquivos do Xbox (XDVDFS) dentro de um .iso
// (iso.cpp). A "pasta do jogo" pode ser o proprio arquivo .iso: um caminho como <arquivo.iso>\levels\x.dat e lido de
// dentro da ISO; qualquer outro caminho e lido do disco como sempre.
#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace iso
{
    struct Entrada
    {
        std::string nome;          // como esta na ISO
        uint64_t inicio = 0;       // posicao no arquivo .iso
        uint64_t tamanho = 0;
        bool pasta = false;
        std::vector<size_t> filhos;   // pasta: indices das entradas dentro dela
    };

    // o caminho e um .iso que existe (a pasta do jogo escolhida pode ser ele)
    bool EIso(const std::filesystem::path& p);
    // entrada da ISO para um caminho <arquivo.iso>[\resto]; nullptr = o caminho nao passa por uma ISO.
    // naoAchou = true quando passa por uma ISO mas o arquivo nao esta nela.
    const Entrada* Achar(const std::filesystem::path& p, bool* naoAchou = nullptr);
    const Entrada& Filho(const Entrada& pasta, size_t i);   // i = indice de pasta.filhos
    // le da ISO do caminho (posicao relativa ao comeco da entrada); devolve os bytes lidos
    uint32_t Ler(const std::filesystem::path& p, const Entrada& e, uint64_t at, void* buf, uint32_t n);
    // abre (duplica) um HANDLE so de leitura da ISO (quem chama fecha)
    HANDLE AbrirHandle(const std::filesystem::path& p);
}

// arquivos do jogo, da pasta ou de dentro da ISO
std::vector<uint8_t> JogoLer(const std::filesystem::path& p);
bool JogoTamanho(const std::filesystem::path& p, uint64_t& tamanho);
bool JogoExiste(const std::filesystem::path& p);
