// Ponteiro do mouse nos menus (menu_mouse.cpp; usado por input.cpp)
#pragma once
#include <cstdint>
#include <vector>
#include "guest.h"

// retangulo na interface 640x480; botao: rodape (A/B); obj: opcao do menu do jogo (0 = lista achada so pela cor);
// sub: linha dentro de uma opcao com varias linhas ("Inverter analogico  SIM"; -1 = opcao comum)
struct MenuItem { int x0, y0, x1, y1; uint16_t botao; uint32_t obj = 0; int sub = -1; };
// lista ativa: lista = objeto do menu do jogo (0 = achada pela cor; ai so da para andar com o direcional)
// setas: cabecalho de paginas ("< OPCOES RAPIDAS >" do System Link) que o direcional esquerda/direita troca quando a
// opcao 'obj' esta escolhida; botao = 0x25 (esquerda) ou 0x27 (direita); e as setas ^ v de uma lista que rola (mapas,
// saves): 0x26 (cima) / 0x28 (baixo)
struct MenuLista { std::vector<MenuItem> itens; int escolhido = -1; uint32_t lista = 0; std::vector<MenuItem> setas; };
struct MenuQuadro
{
    uint64_t troca = 0;                 // numero do quadro (cresce a cada troca de tela)
    std::vector<MenuLista> listas;      // listas ativas (com um item escolhido)
    std::vector<MenuItem> rodape;       // "(A) ESCOLHER", "(B) VOLTAR"...
};

void MenuMouseTroca();                  // VdSwap: fecha o quadro
MenuQuadro MenuMouseQuadro();           // ultimo quadro completo
// editor de mapas: o painel do modo de edicao (camera voando do editor) esta na tela; fora dele ("ENTRAR NO MAPA") e o jogador
bool MenuMouseEditorEdicao();
// escolhe a opcao 'item' (e a linha 'sub' dela, se >= 0) da lista 'lista' na hora (o mesmo que o direcional faz, sem passar pelas do meio).
// Chamado na leitura do controle (ctx = thread do jogo). Volta 1 = escolhida; 0 = nao deu (lista mudou): use o
// direcional; 2 = pedida: a leitura do controle nao e na thread que cuida dos menus (dentro da fase), entao a escolha
// e feita por ela no proximo desenho da lista (chame de novo nos proximos quadros para saber o resultado).
int MenuMouseEscolher(GuestCpu& ctx, uint32_t lista, uint32_t item, int sub);
// cursor normalizado (0..1 na area da janela; cw, ch = tamanho da area) -> interface do jogo (640x480; no editor 853x480)
void MenuMouseParaJogo(float nx, float ny, float cw, float ch, float& ux, float& uy);
float AspectoTela();                    // gpu.cpp
