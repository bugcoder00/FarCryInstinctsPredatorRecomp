// Controles: dispositivos de entrada do PC (controles_pad.cpp), configuracao dos jogadores (input.cpp) e a janela de
// configuracao (controles_janela.cpp, tecla F1 no jogo ou "PredatorGame.exe --controles")
#pragma once
#include <windows.h>
#include <xinput.h>
#include <string>
#include <vector>

// ---------------------------------------------------------------- dispositivos (controles_pad.cpp)
// Identificadores guardados na configuracao: "teclado" (teclado e mouse), "auto" (primeiro controle ligado),
// "sony1".."sony4" (DualSense/DualShock 4 lidos direto pelo USB/Bluetooth), "xbox1".."xbox4" (XInput), "nenhum"
struct PadInfo { std::string id, nome; };
void PadsIniciar();                                   // comeca a procurar controles (thread propria)
std::vector<PadInfo> PadsLigados();                   // controles ligados agora (Sony e XInput)
bool PadLer(const std::string& id, XINPUT_GAMEPAD& g); // false = nao esta ligado
void PadVibrar(const std::string& id, WORD esquerda, WORD direita);
std::string PadNome(const std::string& id);          // nome para mostrar ("DualSense (USB)", "Teclado e mouse"...)
std::string PadResolver(const std::string& id);      // "auto" -> id do controle que esta sendo usado ("" = nenhum)

// ---------------------------------------------------------------- configuracao (input.cpp)
struct ControlesCfg
{
    std::string jogador[4];      // dispositivo de cada jogador (3 e 4: tela dividida em 4)
    bool juntar = false;         // o dispositivo do jogador 2 tambem controla o jogador 1 (e nao ha jogador 2)
    bool mouseCamera = true, inverterY = false;
    float sensibilidade = 1.0f;
    // Camera do controle (item 32: valores ajustados para precisao com o analogico; "Voltar ao padrao" volta a estes)
    bool cameraDireta = true;    // analogico direito gira a camera direto (sem a zona morta/aceleracao do jogo)
    float velocidadeCamera = 0.80f;   // sensibilidade do analogico direito (horizontal): 0,8 = ~120 graus/s no fim do curso
    float velocidadeVertical = 0.65f; // sensibilidade vertical do analogico direito (~70 graus/s)
    float sensMirando = 0.60f;        // multiplica a sensibilidade com o gatilho esquerdo apertado (mirando)
    float zonaMorta = 0.08f;     // parte do analogico que nao gira a camera (0 a 0,5)
    float aceleracao = 0.60f;    // giro horizontal extra com o analogico no fim do curso, crescendo em ~0,3 s (0 a 2)
    float suavizacao = 0.30f;    // 0 = resposta na hora; ate 0,9 = o giro segue o analogico devagar
    struct Acao { std::string nome, ajuda; std::vector<int> teclas; };
    std::vector<Acao> acoes;     // botoes do controle do Xbox -> teclas (codigos do Windows; roda do mouse = 0x1000/0x1001)
};
ControlesCfg InputConfigAtual();
ControlesCfg InputConfigPadrao();
void InputAplicarConfig(const ControlesCfg& c);       // aplica na hora e salva em controles.ini
std::string InputNomeTecla(int vk);                  // codigo -> nome ("ESPACO", "MOUSE_ESQ"...)
std::vector<int> InputTodasTeclas();                 // teclas que a janela oferece

// ---------------------------------------------------------------- janela (controles_janela.cpp)
void ControlesAbrirJanela(bool esperarFechar);       // abre (ou traz para a frente) a janela de configuracao
