// Renderizador DirectX 12: dispositivo, swapchain, quadros, memoria de upload, descritores e DXC
#include "opcoes.h"
#include "perf.h"
#include "render.h"
#include "guest.h"
#include "diag.h"
#include <dxcapi.h>
#include <mutex>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

void (*g_onGpuHang)(uint32_t) = nullptr;   // xenos_draw mostra o desenho que travou

namespace render
{
    namespace
    {
        constexpr UINT FRAMES = 2;
        constexpr UINT64 UPLOAD_SIZE = 96ull << 20;   // por quadro
        // 65 descritores por desenho; a missao passa de 1700 desenhos por quadro (~110 mil). Com 60000 a contagem
        // dava a volta e sobrescrevia os descritores dos primeiros desenhos do quadro (a faixa esquerda do
        // predicated tiling): vertices e texturas errados -> metade esquerda rosa, estilhacada e piscando.
        // 2 x 480000 fica abaixo do limite de 1 milhao do heap visivel aos shaders (Tier 1).
        constexpr UINT SRV_PER_FRAME = 480000;
        constexpr UINT SAMPLER_HEAP_SIZE = 2048;      // limite do D3D12; alocacao permanente (tabelas reaproveitadas)

        ComPtr<ID3D12Device> g_device;
        ComPtr<ID3D12InfoQueue> g_infoQueue;   // FCRECOMP_D3DDEBUG

        // mensagens da camada de verificacao (avisos e erros) para o log
        void LogD3DMessages()
        {
            if (!g_infoQueue) return;
            static int total = 0;
            UINT64 n = g_infoQueue->GetNumStoredMessages();
            for (UINT64 i = 0; i < n && total < 400; i++)
            {
                SIZE_T len = 0;
                g_infoQueue->GetMessage(i, nullptr, &len);
                std::vector<uint8_t> buf(len);
                auto* m = (D3D12_MESSAGE*)buf.data();
                // avisos antigos e inofensivos (alinhamento das texturas, valor da limpeza) nao contam
                if (SUCCEEDED(g_infoQueue->GetMessage(i, m, &len)) && m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING &&
                    !strstr(m->pDescription, "Alignment cannot be 4KB") && !strstr(m->pDescription, "clear values do not match"))
                {
                    Log("[d3d] %s", m->pDescription);
                    total++;
                }
            }
            g_infoQueue->ClearStoredMessages();
        }
        ComPtr<ID3D12CommandQueue> g_queue;
        ComPtr<IDXGIAdapter3> g_adapter3;   // consulta de uso da memoria de video
        ComPtr<ID3D12QueryHeap> g_tsHeap;           // 2 timestamps por quadro (tempo de GPU de cada quadro)
        ComPtr<ID3D12Resource> g_tsReadback;
        UINT64 g_tsFreq = 0;
        bool g_tsUsed[2] = {};
        double g_lastGpuMs = 0;
        // Consultas de oclusao (contagem de amostras que passaram no teste de profundidade), lidas 2 quadros depois
        constexpr UINT OCC_PER_FRAME = 2048;
        ComPtr<ID3D12QueryHeap> g_occHeap;
        ComPtr<ID3D12Resource> g_occReadback;
        std::vector<uint64_t> g_occTags[2];   // etiqueta de cada consulta do quadro (quem pediu)
        UINT g_occResolved[2] = {};           // quantas foram copiadas para o buffer de leitura
        int g_occOpen = -1;                   // consulta aberta na lista atual
        uint64_t g_frameSerial = 0;   // muda a cada lista de quadro aberta (o que foi enviado antes deixa de valer)
        ComPtr<IDXGISwapChain3> g_swapChain;
        ComPtr<ID3D12DescriptorHeap> g_rtvHeap, g_srvHeap, g_samplerHeap;
        ComPtr<ID3D12Resource> g_backBuffers[FRAMES];
        ComPtr<ID3D12CommandAllocator> g_allocators[FRAMES];
        ComPtr<ID3D12GraphicsCommandList> g_list;
        ComPtr<ID3D12Fence> g_fence;
        UINT64 g_fenceValues[FRAMES] = {};
        UINT64 g_nextFence = 1;
        HANDLE g_fenceEvent = nullptr;
        UINT g_rtvSize = 0, g_srvSize = 0, g_samplerSize = 0;
        uint32_t g_width = 0, g_height = 0;

        // estado do quadro em gravacao
        bool g_frameOpen = false;
        UINT g_frame = 0;   // indice do buffer de upload/descritores (alterna 0/1)
        ComPtr<ID3D12Resource> g_upload[FRAMES];
        uint8_t* g_uploadCpu[FRAMES] = {};
        UINT64 g_uploadUsed = 0;
        UINT g_srvUsed = 0, g_samplerUsed = 0;

        // apresentacao
        ComPtr<ID3D12RootSignature> g_blitRoot;
        ComPtr<ID3D12PipelineState> g_blitPso, g_aaPso[3];   // antisserrilhado niveis 1-3 (saida RGBA16F: a cena do jogo)
        ComPtr<ID3D12Resource> g_pixelsTexture;
        uint32_t g_pixelsW = 0, g_pixelsH = 0;

        // Compilador de shaders: uma instancia por thread (a DXC nao aceita duas compilacoes ao mesmo tempo na mesma
        // instancia), para o aquecimento poder compilar varios shaders em paralelo
        DxcCreateInstanceProc DxcCreate()
        {
            static DxcCreateInstanceProc create = [] {
                HMODULE dll = LoadLibraryW(L"dxcompiler.dll");
                return dll ? (DxcCreateInstanceProc)GetProcAddress(dll, "DxcCreateInstance") : nullptr;
            }();
            return create;
        }
        IDxcCompiler3* ThreadCompiler()
        {
            thread_local ComPtr<IDxcCompiler3> compiler;
            if (!compiler)
                if (auto create = DxcCreate()) create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
            return compiler.Get();
        }

        const char* BLIT_HLSL = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID)
{
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(VSOut i) : SV_Target { return float4(tex.Sample(samp, i.uv).rgb, 1); }
)";

        // Antisserrilhado da cena 3D (opcao do launcher; xenos_draw aplica na imagem da cena antes do HUD e dos menus). Onde a luminancia em volta do pixel varia muito (borda),
        // descobre se a borda e mais horizontal ou vertical pelos gradientes 3x3, desloca meio pixel para o lado de maior
        // contraste e tira a media de 4 amostras bilineares ao longo da borda; mistura com o pixel original conforme o
        // contraste. Nas areas lisas devolve o pixel sem mexer.
        // NIVEL 1-3 (definido ao compilar): quanto menor o contraste que ainda conta como borda, quantas amostras ao
        // longo dela e quanto se mistura
        const char* AA_HLSL = R"(
#if NIVEL == 1
#define LIMIAR_ABS 0.04
#define LIMIAR_REL 0.125
#define AMOSTRAS 2
#define MISTURA 0.8
#elif NIVEL == 2
#define LIMIAR_ABS 0.03
#define LIMIAR_REL 0.09
#define AMOSTRAS 3
#define MISTURA 0.9
#else
#define LIMIAR_ABS 0.02
#define LIMIAR_REL 0.06
#define AMOSTRAS 4
#define MISTURA 1.0
#endif
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID)
{
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float Lum(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float3 T(float2 uv) { return tex.SampleLevel(samp, uv, 0).rgb; }
float4 ps(VSOut i) : SV_Target
{
    float2 tam; tex.GetDimensions(tam.x, tam.y);
    float2 px = 1.0 / tam;
    float4 c4 = tex.SampleLevel(samp, i.uv, 0);   // o alfa da cena passa sem mudar
    float3 c = c4.rgb;
    float lC = Lum(c);
    float lN = Lum(T(i.uv + float2(0, -px.y))), lS = Lum(T(i.uv + float2(0, px.y)));
    float lW = Lum(T(i.uv + float2(-px.x, 0))), lE = Lum(T(i.uv + float2(px.x, 0)));
    float lMin = min(lC, min(min(lN, lS), min(lW, lE)));
    float lMax = max(lC, max(max(lN, lS), max(lW, lE)));
    float contraste = lMax - lMin;
    if (contraste < max(LIMIAR_ABS, lMax * LIMIAR_REL)) return c4;
    float lNW = Lum(T(i.uv + float2(-px.x, -px.y))), lNE = Lum(T(i.uv + float2(px.x, -px.y)));
    float lSW = Lum(T(i.uv + float2(-px.x, px.y))), lSE = Lum(T(i.uv + float2(px.x, px.y)));
    float gV = abs(lNW + 2 * lN + lNE - lSW - 2 * lS - lSE);   // varia na vertical: borda horizontal
    float gH = abs(lNW + 2 * lW + lSW - lNE - 2 * lE - lSE);   // varia na horizontal: borda vertical
    bool horizontal = gV >= gH;
    float2 ao = horizontal ? float2(px.x, 0) : float2(0, px.y);
    float2 atravessa = horizontal ? float2(0, px.y) : float2(px.x, 0);
    float l1 = horizontal ? lN : lW, l2 = horizontal ? lS : lE;
    float lado = abs(l1 - lC) >= abs(l2 - lC) ? -0.5 : 0.5;
    float2 uv = i.uv + atravessa * lado;
    float3 m = 0;
    [unroll] for (int k = 0; k < AMOSTRAS; k++) m += T(uv - ao * (k + 0.5)) + T(uv + ao * (k + 0.5));
    m /= 2 * AMOSTRAS;
    float peso = saturate(contraste / max(lMax, 0.001)) * MISTURA;
    return float4(lerp(c, m, peso), c4.a);
}
)";

        void WaitFence(UINT64 value)
        {
            if (g_fence->GetCompletedValue() < value)
            {
                perf::Scope ps(perf::Fence);
                g_fence->SetEventOnCompletion(value, g_fenceEvent);
                WaitForSingleObject(g_fenceEvent, INFINITE);
            }
        }

        bool CreateBlitPipeline()
        {
            D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
            D3D12_ROOT_PARAMETER param{};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            param.DescriptorTable = { 1, &range };
            param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_STATIC_SAMPLER_DESC samp{};
            samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            samp.MaxLOD = D3D12_FLOAT32_MAX;
            D3D12_ROOT_SIGNATURE_DESC rs{ 1, &param, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE };
            ComPtr<ID3DBlob> blob, err;
            if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
                return false;
            g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_blitRoot));

            auto vs = CompileShader(BLIT_HLSL, L"vs_6_0");
            auto ps = CompileShader(BLIT_HLSL, L"ps_6_0");
            if (vs.empty() || ps.empty())
                return false;

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = g_blitRoot.Get();
            pso.VS = { vs.data(), vs.size() };
            pso.PS = { ps.data(), ps.size() };
            pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pso.SampleMask = UINT_MAX;
            pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso.NumRenderTargets = 1;
            pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            pso.SampleDesc.Count = 1;
            for (int n = 0; n < 3; n++)
            {
                auto psAa = CompileShader("#define NIVEL " + std::to_string(n + 1) + "\n" + AA_HLSL, L"ps_6_0");
                D3D12_GRAPHICS_PIPELINE_STATE_DESC aa = pso;
                aa.PS = { psAa.data(), psAa.size() };
                aa.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;   // formato das imagens da EDRAM
                if (psAa.empty() || FAILED(g_device->CreateGraphicsPipelineState(&aa, IID_PPV_ARGS(&g_aaPso[n]))))
                    Log("[render] antisserrilhado nivel %d indisponivel", n + 1);
            }
            return SUCCEEDED(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_blitPso)));
        }
    }

    ID3D12Device* Device() { return g_device.Get(); }
    ID3D12DescriptorHeap* SrvHeap() { return g_srvHeap.Get(); }
    ID3D12DescriptorHeap* SamplerHeap() { return g_samplerHeap.Get(); }

    std::vector<uint8_t> CompileShader(const std::string& source, const wchar_t* target, std::string* errors)
    {
        // Pode ser chamada de varias threads ao mesmo tempo (aquecimento em paralelo).
        // cache em disco (build\shadercache): o mesmo texto HLSL nao e compilado de novo nas proximas execucoes.
        // Sem ele o inicio da missao ficava ~20 s entre 7 e 20 quadros/s compilando centenas de shaders.
        // FCRECOMP_NOSHADERCACHE=1 desliga. O nome do arquivo e o hash do texto + alvo (mudar o tradutor muda o texto).
        static const bool noCache = getenv("FCRECOMP_NOSHADERCACHE") != nullptr;
        uint64_t h = 1469598103934665603ull;
        for (char ch : source) { h ^= (uint8_t)ch; h *= 1099511628211ull; }
        for (const wchar_t* t = target; *t; t++) { h ^= (uint8_t)*t; h *= 1099511628211ull; }
        // Sem -Gis (contas IEEE estritas): o compilador pode juntar multiplicacao e soma e simplificar. Na vista da vila
        // (bug 7, Radeon 610M) a GPU caiu de ~36 para ~30 ms por quadro, sem diferenca visivel. FCRECOMP_COMGIS=1 volta
        // ao -Gis (outro arquivo no cache).
        static const bool noGis = getenv("FCRECOMP_COMGIS") == nullptr;
        if (noGis) { h ^= 0x5A; h *= 1099511628211ull; }
        char cachePath[64];
        snprintf(cachePath, sizeof(cachePath), "shadercache/%016llX.dxil", (unsigned long long)h);
        if (!noCache)
            if (FILE* f = fopen(cachePath, "rb"))
            {
                std::vector<uint8_t> data;
                fseek(f, 0, SEEK_END);
                long size = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (size > 0) { data.resize(size); if (fread(data.data(), 1, size, f) != (size_t)size) data.clear(); }
                fclose(f);
                if (!data.empty()) return data;
            }
        IDxcCompiler3* dxc = ThreadCompiler();
        if (!dxc)
        {
            Log("[render] dxcompiler.dll nao encontrado");
            return {};
        }

        const wchar_t* entry = target[0] == L'v' ? L"vs" : target[0] == L'g' ? L"gs" : L"ps";
        const wchar_t* args[] = { L"-E", entry, L"-T", target, L"-O3", L"-Qstrip_debug", L"-Gis" };
        DxcBuffer buf{ source.data(), source.size(), DXC_CP_UTF8 };
        ComPtr<IDxcResult> result;
        dxc->Compile(&buf, args, _countof(args) - (noGis ? 1 : 0), nullptr, IID_PPV_ARGS(&result));

        ComPtr<IDxcBlobUtf8> err;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&err), nullptr);
        HRESULT status;
        result->GetStatus(&status);
        if (FAILED(status))
        {
            std::string msg = err && err->GetStringLength() ? err->GetStringPointer() : "erro desconhecido";
            if (errors) *errors = msg;
            else Log("[render] erro ao compilar shader: %s", msg.c_str());
            return {};
        }
        ComPtr<IDxcBlob> obj;
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr);
        auto* p = (const uint8_t*)obj->GetBufferPointer();
        std::vector<uint8_t> dxil(p, p + obj->GetBufferSize());
        if (!noCache)
        {
            // grava com nome temporario e renomeia: duas threads com o mesmo shader nunca deixam um arquivo pela metade
            CreateDirectoryA("shadercache", nullptr);
            char tmpPath[96];
            snprintf(tmpPath, sizeof(tmpPath), "%s.%lu.tmp", cachePath, GetCurrentThreadId());
            if (FILE* f = fopen(tmpPath, "wb"))
            {
                bool ok = fwrite(dxil.data(), 1, dxil.size(), f) == dxil.size();
                ok = fclose(f) == 0 && ok;
                if (!ok || !MoveFileExA(tmpPath, cachePath, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmpPath);
            }
        }
        return dxil;
    }

    bool Init(HWND window, uint32_t width, uint32_t height)
    {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
            return false;
        // DRED ("migalhas"): se a placa cair, o registro diz ate qual comando ela chegou. Ligado sempre desde o item 42
        // (antes so com FCRECOMP_DRED=1); FCRECOMP_SEMDRED=1 desliga.
        if (!getenv("FCRECOMP_SEMDRED"))
        {
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred))))
            {
                dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                Log("[render] DRED ligado");
            }
        }
        // FCRECOMP_D3DDEBUG=1: camada de verificacao do DirectX (lenta); os erros vao para o log a cada quadro ([d3d])
        static const bool d3dDebug = getenv("FCRECOMP_D3DDEBUG") != nullptr;
        if (d3dDebug)
        {
            ComPtr<ID3D12Debug> dbg;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); Log("[render] verificacao do DirectX ligada"); }
        }
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device))))
        {
            Log("[render] DirectX 12 indisponivel nesta placa de video");
            return false;
        }
        if (d3dDebug)
        {
            g_device.As(&g_infoQueue);
            // aviso na hora do erro, com os enderecos de quem chamou (RVA: traduzir com o FCRecomp.map)
            ComPtr<ID3D12InfoQueue1> iq1;
            DWORD cookie = 0;
            if (SUCCEEDED(g_device.As(&iq1)))
                iq1->RegisterMessageCallback([](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID, LPCSTR desc, void*) {
                    if (sev > D3D12_MESSAGE_SEVERITY_ERROR) return;
                    void* pilha[12];
                    USHORT n = CaptureStackBackTrace(1, 12, pilha, nullptr);
                    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
                    char b[400]; int o = 0;
                    for (USHORT i = 0; i < n && o < 380; i++) o += snprintf(b + o, sizeof(b) - o, " %llX", (unsigned long long)((uintptr_t)pilha[i] - base));
                    Log("[d3d] ERRO NA HORA: %s | pilha (RVA):%s", desc, b);
                }, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
        }
        {
            DXGI_ADAPTER_DESC1 desc;
            ComPtr<IDXGIAdapter1> adapter;
            if (SUCCEEDED(factory->EnumAdapterByLuid(g_device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) && SUCCEEDED(adapter->GetDesc1(&desc)))
            {
                char name[128];
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
                Log("[render] DirectX 12 em: %s", name);
            }
            adapter.As(&g_adapter3);
        }

        D3D12_COMMAND_QUEUE_DESC qd{ D3D12_COMMAND_LIST_TYPE_DIRECT };
        g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue));

        g_width = width;
        g_height = height;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = width;
        sd.Height = height;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = FRAMES;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Scaling = DXGI_SCALING_STRETCH;
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(factory->CreateSwapChainForHwnd(g_queue.Get(), window, &sd, nullptr, nullptr, &sc1)))
            return false;
        sc1.As(&g_swapChain);
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

        D3D12_DESCRIPTOR_HEAP_DESC rh{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, FRAMES };
        g_device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtvHeap));
        g_rtvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT i = 0; i < FRAMES; i++)
        {
            g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i]));
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += i * g_rtvSize;
            g_device->CreateRenderTargetView(g_backBuffers[i].Get(), nullptr, h);
            g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocators[i]));

            D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD };
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = UPLOAD_SIZE;
            bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            g_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_upload[i]));
            g_upload[i]->Map(0, nullptr, (void**)&g_uploadCpu[i]);
        }

        D3D12_DESCRIPTOR_HEAP_DESC sh{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SRV_PER_FRAME * FRAMES, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
        g_device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_srvHeap));
        g_srvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_DESCRIPTOR_HEAP_DESC smh{ D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SAMPLER_HEAP_SIZE, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
        g_device->CreateDescriptorHeap(&smh, IID_PPV_ARGS(&g_samplerHeap));
        g_samplerSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        // item 41: o heap inteiro comeca com um sampler valido (linear, repete). Uma tabela le sempre 32 entradas a
        // partir do inicio dela; antes as que nunca tinham sido escritas ficavam com lixo. As ultimas 32 sao a tabela
        // reserva (SamplerReserva), usada quando o heap enche no meio de um quadro.
        {
            D3D12_SAMPLER_DESC sd{};
            sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sd.MaxAnisotropy = 1;
            sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            D3D12_CPU_DESCRIPTOR_HANDLE c = g_samplerHeap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < SAMPLER_HEAP_SIZE; i++, c.ptr += g_samplerSize) g_device->CreateSampler(&sd, c);
        }

        g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0].Get(), nullptr, IID_PPV_ARGS(&g_list));
        g_list->Close();
        g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
        g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        if (!CreateBlitPipeline())
        {
            Log("[render] falha ao criar o pipeline de apresentacao");
            return false;
        }
        Log("[render] DirectX 12 pronto (%ux%u)", width, height);
        return true;
    }

    ID3D12GraphicsCommandList5* ListaVrs()
    {
        static ID3D12GraphicsCommandList5* l5 = [] {
            ID3D12GraphicsCommandList5* l = nullptr;
            D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
            bool tem = SUCCEEDED(g_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6))) &&
                       o6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_1;
            if (getenv("FCRECOMP_SEMVRS")) tem = false;
            // a referencia fica guardada ate o fim (a lista do quadro e sempre a mesma, so e reaberta)
            if (tem && FAILED(g_list->QueryInterface(IID_PPV_ARGS(&l)))) l = nullptr;
            Log("[render] sombreamento de taxa variavel (particulas 2x2): %s", l ? "ligado" : "a placa nao tem (ou FCRECOMP_SEMVRS)");
            return l;
        }();
        return l5;
    }

    ID3D12GraphicsCommandList* FrameList()
    {
        if (!g_frameOpen)
        {
            WaitFence(g_fenceValues[g_frame]);   // os recursos deste indice estao livres
            g_allocators[g_frame]->Reset();
            g_list->Reset(g_allocators[g_frame].Get(), nullptr);
            g_uploadUsed = 0;
            g_srvUsed = 0;
            g_frameOpen = true;
            g_frameSerial++;
            // tempo de GPU de cada quadro (inicio -> fim da lista), sem parar nada: o deste indice (2 quadros atras)
            // ja terminou, entao le o resultado dele e marca o inicio do novo
            if (!g_tsHeap)
            {
                D3D12_QUERY_HEAP_DESC qd{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, FRAMES * 2, 0 };
                g_device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_tsHeap));
                D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
                D3D12_RESOURCE_DESC bd{ D3D12_RESOURCE_DIMENSION_BUFFER, 0, FRAMES * 16, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
                g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_tsReadback));
                g_queue->GetTimestampFrequency(&g_tsFreq);
            }
            if (g_tsUsed[g_frame])
            {
                uint64_t* ts = nullptr;
                D3D12_RANGE r{ g_frame * 16, g_frame * 16 + 16 };
                if (SUCCEEDED(g_tsReadback->Map(0, &r, (void**)&ts)))
                {
                    uint64_t t0 = ts[g_frame * 2], t1 = ts[g_frame * 2 + 1];
                    if (t1 > t0 && g_tsFreq) g_lastGpuMs = (t1 - t0) * 1000.0 / g_tsFreq;
                    D3D12_RANGE none{ 0, 0 };
                    g_tsReadback->Unmap(0, &none);
                }
            }
            g_list->EndQuery(g_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g_frame * 2);
            // resultados das consultas de oclusao deste indice (o quadro de 2 atras ja terminou)
            if (g_occResolved[g_frame])
            {
                uint64_t* v = nullptr;
                D3D12_RANGE r{ g_frame * OCC_PER_FRAME * 8, (g_frame * OCC_PER_FRAME + g_occResolved[g_frame]) * 8 };
                if (SUCCEEDED(g_occReadback->Map(0, &r, (void**)&v)))
                {
                    for (UINT i = 0; i < g_occResolved[g_frame]; i++)
                        if (g_onOcclusion) g_onOcclusion(g_occTags[g_frame][i], v[g_frame * OCC_PER_FRAME + i]);
                    D3D12_RANGE none{ 0, 0 };
                    g_occReadback->Unmap(0, &none);
                }
            }
            g_occTags[g_frame].clear();
            g_occResolved[g_frame] = 0;
            g_occOpen = -1;
        }
        return g_list.Get();
    }

    Upload UploadAlloc(size_t size, size_t align)
    {
        FrameList();
        UINT64 offset = (g_uploadUsed + align - 1) & ~(UINT64)(align - 1);
        if (offset + size > UPLOAD_SIZE)
        {
            Log("[render] memoria de upload do quadro esgotada (%zu bytes)", size);
            offset = 0;   // sobrescreve (imagem errada, mas nao trava)
        }
        g_uploadUsed = offset + size;
        return { g_uploadCpu[g_frame] + offset, g_upload[g_frame]->GetGPUVirtualAddress() + offset, g_upload[g_frame].Get(), offset };
    }

    void VideoMemory(double& usageMB, double& budgetMB, double& sysUsageMB)
    {
        usageMB = budgetMB = sysUsageMB = 0;
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, sys{};
        if (g_adapter3 && SUCCEEDED(g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
        {
            usageMB = local.CurrentUsage / 1048576.0;
            budgetMB = local.Budget / 1048576.0;
        }
        if (g_adapter3 && SUCCEEDED(g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &sys)))
            sysUsageMB = sys.CurrentUsage / 1048576.0;
    }

    double LastGpuFrameMs() { return g_lastGpuMs; }
    uint64_t FrameSerial() { FrameList(); return g_frameSerial; }

    UINT64 TimestampFrequency() { UINT64 f = 0; g_queue->GetTimestampFrequency(&f); return f; }

    ID3D12Resource* UploadBuffer() { FrameList(); return g_upload[g_frame].Get(); }
    UINT64 UploadCapacity() { return UPLOAD_SIZE; }

    Descriptors AllocSrv(UINT count)
    {
        FrameList();
        if (g_srvUsed + count > SRV_PER_FRAME)
        {
            static int warned = 0;
            if (warned++ < 3) Log("[render] descritores do quadro esgotados (%u): a imagem pode sair errada", SRV_PER_FRAME);
            diag::Note(diag::GpuFeature, "descritores do quadro esgotados (desenhos do inicio do quadro saem errados)");
            g_srvUsed = 0;
        }
        UINT index = g_frame * SRV_PER_FRAME + g_srvUsed;
        g_srvUsed += count;
        Descriptors d;
        d.cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
        d.cpu.ptr += (SIZE_T)index * g_srvSize;
        d.gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
        d.gpu.ptr += (UINT64)index * g_srvSize;
        d.increment = g_srvSize;
        return d;
    }

    // Samplers: alocacao permanente (nao volta a cada quadro). Retorna cpu.ptr = 0 se o heap encheu.
    Descriptors AllocSampler(UINT count)
    {
        Descriptors d{};
        // a tabela do root signature tem 32 entradas: o inicio precisa deixar 32 dentro do heap
        if (g_samplerUsed + std::max<UINT>(count, 32) > SAMPLER_HEAP_SIZE - 32) return d;   // as ultimas 32: reserva
        UINT index = g_samplerUsed;
        g_samplerUsed += count;
        d.cpu = g_samplerHeap->GetCPUDescriptorHandleForHeapStart();
        d.cpu.ptr += (SIZE_T)index * g_samplerSize;
        d.gpu = g_samplerHeap->GetGPUDescriptorHandleForHeapStart();
        d.gpu.ptr += (UINT64)index * g_samplerSize;
        d.increment = g_samplerSize;
        return d;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE SamplerReserva()
    {
        D3D12_GPU_DESCRIPTOR_HANDLE h = g_samplerHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += (UINT64)(SAMPLER_HEAP_SIZE - 32) * g_samplerSize;
        return h;
    }

    void ResetSamplers()
    {
        WaitIdle();
        g_samplerUsed = 0;
    }

    void PresentTexture(ID3D12Resource* texture)
    {
        auto* list = FrameList();
        UINT back = g_swapChain->GetCurrentBackBufferIndex();
        if (texture) {
        auto srv = AllocSrv(1);
        g_device->CreateShaderResourceView(texture, nullptr, srv.cpu);

        D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        b.Transition = { g_backBuffers[back].Get(), 0, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
        list->ResourceBarrier(1, &b);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += back * g_rtvSize;
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{ 0, 0, (float)g_width, (float)g_height, 0, 1 };
        D3D12_RECT sc{ 0, 0, (LONG)g_width, (LONG)g_height };
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);
        // sem antisserrilhado aqui: na imagem final ele achava "bordas" no texto do HUD, dos menus (inclusive o de pausa)
        // e nos videos e borrava tudo; agora vale so na cena 3D (Antisserrilhar, chamado por xenos_draw)
        list->SetPipelineState(g_blitPso.Get());
        list->SetGraphicsRootSignature(g_blitRoot.Get());
        ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get(), g_samplerHeap.Get() };
        list->SetDescriptorHeaps(2, heaps);
        list->SetGraphicsRootDescriptorTable(0, srv.gpu);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);

        b.Transition = { g_backBuffers[back].Get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT };
        list->ResourceBarrier(1, &b);
        }   // texture
        list->EndQuery(g_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g_frame * 2 + 1);
        list->ResolveQueryData(g_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g_frame * 2, 2, g_tsReadback.Get(), g_frame * 16);
        if (g_occOpen >= 0) OcclusionEnd(g_occOpen);   // consulta que o jogo nao fechou neste quadro
        if (UINT n = (UINT)g_occTags[g_frame].size())
        {
            list->ResolveQueryData(g_occHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, g_frame * OCC_PER_FRAME, n, g_occReadback.Get(),
                (UINT64)g_frame * OCC_PER_FRAME * 8);
            g_occResolved[g_frame] = n;
        }
        g_tsUsed[g_frame] = true;
        list->Close();
        ID3D12CommandList* lists[] = { list };
        g_queue->ExecuteCommandLists(1, lists);
        // Sem vsync do PC: o jogo ja se ritma pela interrupcao de vsync emulada (60 Hz). Esperar tambem o monitor
        // fazia os quadros perderem a vez e cairem para ~17 qps. Em janela (modelo flip) o Windows compoe a
        // imagem, entao nao ha "rasgos". FCRECOMP_VSYNC=1 volta a esperar o monitor.
        static const bool vsync = getenv("FCRECOMP_VSYNC") != nullptr || OpcoesAtuais().vsync;   // opcao do launcher
        LogD3DMessages();
        HRESULT phr = texture ? g_swapChain->Present(vsync ? 1 : 0, 0) : S_OK;   // sem textura: a janela fica como esta
        if (static int n = 0; perf::AvLog() && n++ < 6) Log("[av] %.1f ms: imagem %d entregue ao Windows (Present)", perf::Now(), n);
        if (FAILED(phr))
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                Log("[render] a placa de video foi removida/reiniciada: Present=%08X motivo=%08X",
                    (unsigned)phr, (unsigned)g_device->GetDeviceRemovedReason());
                ComPtr<ID3D12DeviceRemovedExtendedData> dred;
                D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
                if (SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&dred))) && SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs)))
                {
                    for (auto* n = crumbs.pHeadAutoBreadcrumbNode; n; n = n->pNext)
                    {
                        uint32_t done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
                        if (done >= n->BreadcrumbCount) continue;   // esta lista terminou
                        uint32_t draws = 0;
                        for (uint32_t i = 0; i < done; i++)
                            if (n->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED) draws++;
                        Log("[render] DRED: lista parou na operacao %u de %u (op=%d), depois de %u desenhos completos",
                            done, n->BreadcrumbCount, (int)n->pCommandHistory[done], draws);
                        if (g_onGpuHang) g_onGpuHang(draws);
                    }
                }
                D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
                if (dred && SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)) && pf.PageFaultVA)
                    Log("[render] DRED: falha de pagina no endereco da GPU %llX", (unsigned long long)pf.PageFaultVA);
            }
        }
        g_fenceValues[g_frame] = g_nextFence;
        g_queue->Signal(g_fence.Get(), g_nextFence++);
        g_frameOpen = false;
        g_frame ^= 1;
    }

    bool Antisserrilhar(ID3D12GraphicsCommandList* list, ID3D12Resource* src, D3D12_CPU_DESCRIPTOR_HANDLE rtv, UINT w, UINT h, int nivel)
    {
        if (nivel < 1 || nivel > 3 || !g_aaPso[nivel - 1]) return false;
        auto srv = AllocSrv(1);
        g_device->CreateShaderResourceView(src, nullptr, srv.cpu);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0, 1 };
        D3D12_RECT sc{ 0, 0, (LONG)w, (LONG)h };
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);
        list->SetPipelineState(g_aaPso[nivel - 1].Get());
        list->SetGraphicsRootSignature(g_blitRoot.Get());
        ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get(), g_samplerHeap.Get() };
        list->SetDescriptorHeaps(2, heaps);
        list->SetGraphicsRootDescriptorTable(0, srv.gpu);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        return true;
    }

    void (*g_onOcclusion)(uint64_t tag, uint64_t samples) = nullptr;

    int OcclusionBegin(uint64_t tag)
    {
        auto* list = FrameList();
        if (!g_occHeap)
        {
            D3D12_QUERY_HEAP_DESC qd{ D3D12_QUERY_HEAP_TYPE_OCCLUSION, FRAMES * OCC_PER_FRAME, 0 };
            g_device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_occHeap));
            D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC bd{ D3D12_RESOURCE_DIMENSION_BUFFER, 0, FRAMES * OCC_PER_FRAME * 8, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
            g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_occReadback));
        }
        if (!g_occHeap || !g_occReadback) return -1;
        if (g_occOpen >= 0) OcclusionEnd(g_occOpen);   // uma por vez
        auto& tags = g_occTags[g_frame];
        if (tags.size() >= OCC_PER_FRAME) return -1;
        int index = (int)(g_frame * OCC_PER_FRAME + tags.size());
        tags.push_back(tag);
        list->BeginQuery(g_occHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, index);
        g_occOpen = index;
        return index;
    }

    void OcclusionEnd(int index)
    {
        if (index < 0 || index != g_occOpen || !g_frameOpen) return;
        g_list->EndQuery(g_occHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, index);
        g_occOpen = -1;
    }

    void WaitIdle()
    {
        WaitFence(g_nextFence - 1);
    }

    void PresentPixels(const uint32_t* pixels, uint32_t width, uint32_t height)
    {
        if (!g_device) return;
        auto* list = FrameList();

        if (g_pixelsW != width || g_pixelsH != height)
        {
            // espera a GPU antes de trocar a textura
            WaitFence(g_nextFence - 1);
            g_pixelsW = width;
            g_pixelsH = height;
            D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = width;
            td.Height = height;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            g_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_pixelsTexture));
        }
        else
        {
            D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
            b.Transition = { g_pixelsTexture.Get(), 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST };
            list->ResourceBarrier(1, &b);
        }

        uint32_t rowPitch = (width * 4 + 255) & ~255u;
        auto up = UploadAlloc((size_t)rowPitch * height, 512);
        for (uint32_t y = 0; y < height; y++)
            memcpy((uint8_t*)up.cpu + (size_t)y * rowPitch, pixels + (size_t)y * width, width * 4);

        D3D12_TEXTURE_COPY_LOCATION dst{ g_pixelsTexture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{ up.resource, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        src.PlacedFootprint.Offset = up.offset;
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, width, height, 1, rowPitch };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        b.Transition = { g_pixelsTexture.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        list->ResourceBarrier(1, &b);
        PresentTexture(g_pixelsTexture.Get());
    }
}
