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

    // Resolucao interna em quartos (4 = 1x, 6 = 1,5x, 8 = 2x, 12 = 3x; 3 = 0,75x e 2 = 0,5x so com o FSR): opcao
    // "escala" do launcher, FSR ou FCRECOMP_ESCALA
    uint32_t EscalaMeios();
    void RecarregarSuavizacao();
    void PedirNovaEscala();        // resolucao interna/sombras mudaram (OPCOES DE VIDEO): troca no fim do quadro   // a opcao antisserrilhado mudou (OPCOES DE VIDEO no jogo): vale no proximo quadro
    bool Init();
    // Progresso do aquecimento do Init para o titulo da janela ("" quando acabou)
    std::string WarmUpStatus();
    float WarmUpProgress();   // 0 a 1 (tela de preparacao)
    // Etapa da preparacao para o texto da tela (gpu.cpp): 0 nenhuma (DirectX iniciando), 1 superficies, 2 shaders da
    // receita, 3 pipelines, 4 shaders do jogo (1a vez); feitos/total = contagem da etapa (0/0 = sem contagem)
    enum { ETAPA_NADA, ETAPA_SUPERFICIES, ETAPA_SHADERS, ETAPA_PIPELINES, ETAPA_SHADERS_JOGO, ETAPA_FIM };   // FIM: aquecimento acabou, falta pouco
    int WarmUpEtapa(uint32_t& feitos, uint32_t& total);
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
    void NotaCaptura(const char* texto);   // linha extra no desenhos.txt da captura (F9/F12)
    void QueryEnd(uint32_t block);
    uint32_t QueryResult(uint32_t block, uint32_t fallback);
    uint32_t QuerySerial();   // numero da ultima consulta aberta (QueryBegin)
    bool QueryAberta();       // ha uma consulta de oclusao aberta
    // "mar otimista": caixa 3D sem tesoura na faixa 1 (o mar da 26_SHANTYNIGHT herda a tesoura vazia do ultimo pedaco
    // da grade). A resposta chega 2-4 quadros depois e o jogo escondia o mar ao virar: essa consulta nunca responde 0.
    // FCRECOMP_MARREAL=1 desliga.
    bool QueryOtimista(uint32_t block);
    // contagem da propria caixa (consulta 'serial' ou mais nova); fallback se ainda nao chegou da placa
    uint32_t QueryResultDesde(uint32_t block, uint32_t serial, uint32_t fallback);
}
