// Ferramenta de diagnostico: junta num so lugar tudo o que falta ou falha no jogo (funcoes do sistema nao
// implementadas, arquivos nao encontrados, recursos da GPU sem suporte, shaders com problema, travamentos...)
// e escreve o relatorio "diagnostico.txt" na pasta do executavel.
//
// O relatorio e escrito: ao fechar o jogo, num travamento e ao apertar F8 na janela do jogo.
// Teclas na janela do jogo (veja tambem DIAGNOSTICO.md):
//   F5  alterna os shaders "em linha reta" <-> "laco" (para achar shaders traduzidos de forma errada)
//   F6  liga/desliga os mipmaps das texturas
//   F7  liga/desliga o limite de cor 0-1 dos alvos de ponto fixo
//   F8  escreve o relatorio agora
//   F9  captura o proximo quadro inteiro (lista de desenhos + imagens intermediarias) em diag\quadro_N\
//   F11 copia a memoria do jogo para diag\memoria_N\ (para procurar textos, tabelas etc.)
#pragma once
#include <cstdint>
#include <string>

namespace diag
{
    // Categorias do relatorio (a ordem e a ordem das secoes)
    enum Cat
    {
        Crash,          // travamentos e chamadas para funcoes inexistentes
        Stub,           // funcoes do sistema (kernel/XAM) que o jogo chamou e ainda nao existem
        Kernel,         // partes de funcoes do sistema nao implementadas (classes, tipos de objeto...)
        File,           // arquivos que o jogo pediu e nao foram encontrados/abertos
        GpuPacket,      // comandos da GPU (PM4) ignorados
        GpuFeature,     // primitivas, formatos de textura e outros recursos sem suporte
        Shader,         // shaders que nao compilaram ou usam instrucoes nao traduzidas
        Other,
        CatCount
    };

    // Conta uma ocorrencia. "key" identifica o problema (agrupa as repeticoes); "detail" e guardado
    // na primeira vez (ex.: argumentos da chamada).
    void Note(Cat cat, const std::string& key, const std::string& detail = "");

    // Quadro atual (para registrar quando cada problema apareceu pela primeira vez)
    void SetFrame(uint64_t frame);
    uint64_t CurrentFrame();

    // Escreve diagnostico.txt (motivo aparece no topo)
    void WriteReport(const char* reason);

    // Interruptores alternados pelas teclas (lidos pelo resto do runtime)
    extern bool g_loopShaders;   // F5: true = todos os shaders no modo "laco"
    extern bool g_noMips;        // F6
    extern bool g_noSaturate;    // F7
    extern volatile bool g_flushShaders;    // pedido para recompilar os shaders (depois de F5/F7)
    extern volatile bool g_flushTextures;   // pedido para recarregar as texturas (depois de F6)

    // F9: o quadro a capturar (0 = nenhum). Captura(frame) responde se este quadro esta sendo capturado.
    void RequestCapture();
    void CaptureAt(uint64_t frame);   // captura um quadro escolhido (ex.: N quadros depois do inicio da missao)
    bool Capturing(uint64_t frame);
    std::string CaptureDir(uint64_t frame);   // pasta da captura (criada na primeira chamada)

    // F11 (ou FCRECOMP_MEMDUMP=quadro): copia a memoria do jogo para diag\memoria_N\ (um arquivo por regiao,
    // nome = endereco do Xbox). Chamado pela thread da GPU a cada quadro.
    void MaybeDumpMemory(uint64_t frame);

    // Tecla apertada na janela do jogo (chamada pelo WndProc). Retorna true se a tecla e do diagnostico.
    bool OnKey(unsigned vk);

    // Texto curto para o titulo da janela ("" se tudo no padrao)
    std::string Status();
}
