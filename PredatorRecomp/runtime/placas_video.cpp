// Placas de video do PC (launcher: lista "Placa de video"; render.cpp: em qual placa o DirectX 12 e criado).
//
// Antes o DirectX 12 era criado com D3D12CreateDevice(nullptr): a "placa padrao" do Windows, que em notebook com duas
// placas costuma ser a integrada (Intel UHD / Radeon do processador). Relatorio de 24/11/2026: i9-13950HX + RTX 4070
// Laptop rodando na Intel UHD, com defeitos graficos no jogo. Agora a ordem e a de "alto desempenho" do Windows
// (IDXGIFactory6::EnumAdapterByGpuPreference: placa dedicada primeiro) e quem joga pode escolher a placa no launcher
// (opcoes.ini "placa_video" = nome da placa; vazio = automatico, a primeira da lista). FCRECOMP_PLACA=n (indice da
// lista) ou parte do nome escolhe nos testes.
#include "placas_video.h"
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

std::vector<PlacaVideo> PlacasDeVideo()
{
    std::vector<PlacaVideo> lista;
    ComPtr<IDXGIFactory1> f1;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f1)))) return lista;
    ComPtr<IDXGIFactory6> f6;
    f1.As(&f6);
    for (UINT i = 0;; i++)
    {
        ComPtr<IDXGIAdapter1> a;
        if (f6) { if (FAILED(f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)))) break; }
        else if (f1->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;   // "Microsoft Basic Render Driver"
        // so as que tem DirectX 12 (nivel 11_0, o mesmo que render.cpp pede); a pergunta nao cria nada
        if (FAILED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) continue;
        bool repetida = false;   // a mesma placa pode aparecer duas vezes (uma por saida de video)
        for (const auto& p : lista) repetida |= p.luid.LowPart == d.AdapterLuid.LowPart && p.luid.HighPart == d.AdapterLuid.HighPart;
        if (repetida) continue;
        PlacaVideo p;
        p.nome = d.Description;
        while (!p.nome.empty() && p.nome.back() == L' ') p.nome.pop_back();
        p.memoriaMB = (uint64_t)(d.DedicatedVideoMemory / (1024 * 1024));
        p.integrada = d.DedicatedVideoMemory < 512ull * 1024 * 1024;   // as integradas tem pouca (ou nenhuma) memoria propria
        p.luid = d.AdapterLuid;
        lista.push_back(p);
    }
    return lista;
}

int PlacaEscolhida(const std::vector<PlacaVideo>& lista, const std::wstring& nome)
{
    if (lista.empty()) return -1;
    if (const char* e = getenv("FCRECOMP_PLACA"); e && *e)
    {
        char* fim = nullptr;
        const long n = strtol(e, &fim, 10);
        if (fim && !*fim) return n >= 0 && n < (long)lista.size() ? (int)n : 0;
        std::wstring w(e, e + strlen(e));
        for (size_t i = 0; i < lista.size(); i++) if (lista[i].nome.find(w) != std::wstring::npos) return (int)i;
    }
    if (!nome.empty())
        for (size_t i = 0; i < lista.size(); i++) if (lista[i].nome == nome) return (int)i;
    return 0;   // automatico (ou a escolhida sumiu): a de alto desempenho
}
