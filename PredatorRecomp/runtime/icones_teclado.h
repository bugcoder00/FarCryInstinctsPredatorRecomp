// Icones dos botoes do controle nos textos do jogo trocados por desenhos das teclas do teclado (icones_teclado.cpp)
#pragma once
#include <cstdint>

// Chamado por GetTexture (xenos_draw.cpp) antes de conferir a textura: se for uma das folhas de icones da fonte do
// jogo, redesenha (ou devolve) os icones na memoria do jogo. mem = memoria da textura (nivel 0).
void IconesTecladoTextura(uint8_t* mem, uint32_t base, uint32_t width, uint32_t height, uint32_t format,
                          uint32_t endian, bool tiled, uint32_t pitchBlocks, uint64_t quadro);
// O mesmo para a foto do controle da tela "Configuracao do controle": com teclado e mouse ela vira um desenho do teclado
// e do mouse com o aviso do F1; com um controle ligado a foto volta.
void ControleImagemTextura(uint8_t* mem, uint32_t base, uint32_t width, uint32_t height, uint32_t format,
                           uint32_t endian, bool tiled, uint32_t pitchBlocks, uint64_t quadro);
// a foto esta na tela com o desenho do teclado (menu_mouse.cpp esconde os textos e as linhas que apontam os botoes)
bool ControleTecladoNaTela();
