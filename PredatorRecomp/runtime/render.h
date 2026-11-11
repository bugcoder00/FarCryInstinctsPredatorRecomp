// Renderizador DirectX 12 do FCRecomp
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// chamado quando a GPU trava, com o numero de desenhos completos na lista que travou
extern void (*g_onGpuHang)(uint32_t);

namespace render
{
    bool Init(HWND window, uint32_t width, uint32_t height);
    ID3D12Device* Device();

    // Compila HLSL com o DXC. target ex.: "vs_6_0", "ps_6_0", "gs_6_0" (entrada = vs/ps/gs). Vazio em erro.
    std::vector<uint8_t> CompileShader(const std::string& source, const wchar_t* target, std::string* errors = nullptr);

    // ---- quadro atual ----
    // Lista de comandos do quadro (aberta sob demanda)
    ID3D12GraphicsCommandList* FrameList();
    // A mesma lista com sombreamento de taxa variavel (VRS nivel 1), ou nullptr se a placa nao tem (ou FCRECOMP_SEMVRS=1)
    ID3D12GraphicsCommandList5* ListaVrs();

    // Memoria temporaria do quadro (some depois que a GPU termina o quadro)
    struct Upload { void* cpu; D3D12_GPU_VIRTUAL_ADDRESS gpu; ID3D12Resource* resource; UINT64 offset; };
    Upload UploadAlloc(size_t size, size_t align = 256);
    ID3D12Resource* UploadBuffer();     // buffer de upload do quadro atual (inteiro)
    UINT64 UploadCapacity();

    // Descritores do quadro (heaps visiveis ao shader)
    struct Descriptors { D3D12_CPU_DESCRIPTOR_HANDLE cpu; D3D12_GPU_DESCRIPTOR_HANDLE gpu; UINT increment; };
    Descriptors AllocSrv(UINT count);
    Descriptors AllocSampler(UINT count);   // permanente; cpu.ptr == 0 se acabou o espaco
    void ResetSamplers();                   // espera a GPU e libera todos os samplers
    D3D12_GPU_DESCRIPTOR_HANDLE SamplerReserva();   // 32 samplers validos (linear, repete) para quando o heap enche
    ID3D12DescriptorHeap* SrvHeap();
    ID3D12DescriptorHeap* SamplerHeap();

    // Ticks por segundo dos timestamps da GPU (perfilador FCRECOMP_GPUPROF)
    UINT64 TimestampFrequency();
    // Tempo de GPU do quadro mais recente ja terminado (inicio -> fim da lista do quadro), em ms
    double LastGpuFrameMs();
    // Numero da lista de quadro atual (dados de UploadAlloc so valem enquanto ele nao muda)
    uint64_t FrameSerial();

    // Memoria de video em uso / limite dado pelo Windows (local) e memoria do sistema usada pela GPU, em MB
    void VideoMemory(double& usageMB, double& budgetMB, double& sysUsageMB);

    // Consultas de oclusao: conta as amostras dos desenhos entre Begin e End (uma aberta por vez). O resultado chega
    // ~2 quadros depois por g_onOcclusion(etiqueta, amostras). Begin devolve -1 se nao deu para abrir.
    int OcclusionBegin(uint64_t tag);
    void OcclusionEnd(int index);
    extern void (*g_onOcclusion)(uint64_t tag, uint64_t samples);

    // Espera a placa de video terminar tudo que ja foi enviado
    void WaitIdle();

    // Apresenta uma textura (estado PIXEL_SHADER_RESOURCE) na janela e termina o quadro. nullptr = termina o quadro
    // (envia a lista) sem trocar a imagem da janela: ela continua com a ultima apresentada
    void PresentTexture(ID3D12Resource* texture);

    // Antisserrilhado (nivel 1-3) de src (estado PIXEL_SHADER_RESOURCE) para o alvo rtv (RGBA16F, w x h, estado
    // RENDER_TARGET). Troca assinatura, pipeline, heaps, viewport e alvo da lista. false = nivel indisponivel.
    bool Antisserrilhar(ID3D12GraphicsCommandList* list, ID3D12Resource* src, D3D12_CPU_DESCRIPTOR_HANDLE rtv, UINT w, UINT h, int nivel);

    // Mostra uma imagem BGRA (ex.: quadro de video) na janela
    void PresentPixels(const uint32_t* pixels, uint32_t width, uint32_t height);
}
