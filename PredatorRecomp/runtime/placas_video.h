// Placas de video do PC com DirectX 12, na ordem de alto desempenho do Windows (placas_video.cpp)
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

struct PlacaVideo
{
    std::wstring nome;        // DXGI_ADAPTER_DESC1::Description (o opcoes.ini guarda este nome)
    uint64_t memoriaMB = 0;   // memoria de video dedicada
    bool integrada = false;   // pouca memoria dedicada: a placa do processador
    LUID luid{};
};

std::vector<PlacaVideo> PlacasDeVideo();
// indice da placa a usar: FCRECOMP_PLACA, senao a do nome (opcoes.ini), senao 0 (a de alto desempenho); -1 = nenhuma
int PlacaEscolhida(const std::vector<PlacaVideo>& lista, const std::wstring& nome);
