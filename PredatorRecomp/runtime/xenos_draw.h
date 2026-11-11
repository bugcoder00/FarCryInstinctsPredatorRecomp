// Execucao dos desenhos da GPU Xenos com DirectX 12
#pragma once
#include <cstdint>
#include <vector>
#include <string>

namespace xdraw
{
    struct ShaderRef
    {
        uint64_t hash = 0;
        const std::vector<uint32_t>* code = nullptr;
    };

    struct DrawInfo
    {
        uint32_t primType = 0;      // VGT_DRAW_INITIATOR.prim_type
        uint32_t count = 0;         // numero de indices/vertices
        uint32_t indexBase = 0;     // endereco fisico (0 = sem indices)
        bool index32 = false;
        uint32_t indexEndian = 0;
        ShaderRef vs, ps;
    };

    // Resolucao interna em meios (2 = 1x, 3 = 1,5x, 4 = 2x): opcao "escala" do launcher ou FCRECOMP_ESCALA
    uint32_t EscalaMeios();
    bool Init();
    // Progresso do aquecimento do Init para o titulo da janela ("" quando acabou)
    std::string WarmUpStatus();
    float WarmUpProgress();   // 0 a 1 (tela de preparacao)
    extern int g_expMode;   // FCRECOMP_EXPERIMENTO (medicao): configuracao atual
    void Shutdown();                 // libera os recursos da GPU (chamar antes de sair)
    void Draw(const DrawInfo& info);
    void Resolve();                  // RB_MODECONTROL = copia: EDRAM -> memoria
    bool PresentFrame(uint32_t frontBuffer);
    void PedirFoto(const std::string& arquivo);   // gpu.cpp: grava a imagem apresentada pelo proximo PresentFrame (BMP)
    void SetVideoFrame(bool video);   // gpu.cpp: o quadro que vai ser apresentado e de um video Bink (planos Y/Cb/Cr)

    // Consultas de oclusao do jogo (EVENT_WRITE_ZPD): os desenhos entre Begin e End (as "caixas de teste") sao contados
    // pela GPU do PC. O resultado chega ~2 quadros depois; Result devolve o ultimo resultado conhecido desse bloco
    // (ou "fallback" se ainda nao houver nenhum).
    void QueryBegin(uint32_t block);
    void QueryEnd(uint32_t block);
    uint32_t QueryResult(uint32_t block, uint32_t fallback);
}
