// Visual das janelas do programa (launcher, instrucoes, controles, teclado) com as cores do menu do jogo: fundo escuro,
// ambar, vermelho escuro. Botoes (cantos arredondados), interruptores (no lugar das caixas de marcar) e itens do menu
// lateral sao desenhados aqui (BS_OWNERDRAW, GDI+ para as bordas suaves); listas e campos ficam escuros.
#pragma once
#include <windows.h>
#ifndef CLR_NONE
#define CLR_NONE 0xFFFFFFFFL   // "sem cor" (o mesmo do commctrl.h, que nem todos incluem)
#endif

namespace tema
{
    constexpr COLORREF FUNDO = RGB(16, 14, 11), PAINEL = RGB(30, 26, 20), CAMPO = RGB(36, 32, 26);
    constexpr COLORREF CARTAO = RGB(23, 21, 17), LINHA = RGB(52, 46, 36);   // painel arredondado do launcher e linhas finas
    constexpr COLORREF AMBAR = RGB(236, 174, 52), AMBAR_ESC = RGB(150, 104, 30), VERMELHO = RGB(128, 20, 16), VERMELHO_CLARO = RGB(178, 36, 26);
    constexpr COLORREF TEXTO = RGB(228, 222, 206), TEXTO_FRACO = RGB(150, 142, 124), VERDE = RGB(130, 205, 95), ERRO = RGB(235, 90, 64);

    void Iniciar(float escala);   // pinceis, fontes e GDI+ (uma vez; escala = DPI / 96)
    HBRUSH PincelFundo();
    HBRUSH PincelCampo();
    HBRUSH PincelCartao();
    HFONT FonteSecao();
    HFONT FonteBotao();
    HFONT FonteMenu();            // itens do menu lateral
    int S(int v);                 // tamanho com a escala da tela

    void TituloEscuro(HWND janela);                 // barra de titulo escura (Windows 10/11; cor propria no 11)
    void ListaEscura(HWND combo);                   // lista escura (WM_CTLCOLORLISTBOX tambem: CorCampo)
    void AcompanharMouse(HWND botao);               // o botao acende com o mouse em cima
    bool MouseSobre(HWND botao);
    // atras = cor de quem esta embaixo do controle (os cantos arredondados mostram ela)
    void DesenharCaixa(const DRAWITEMSTRUCT* d, bool marcado, HFONT fonte, COLORREF atras = FUNDO);   // interruptor
    void DesenharBotao(const DRAWITEMSTRUCT* d, bool destaque, HFONT fonte, COLORREF atras = FUNDO);  // destaque = vermelho (Jogar, Salvar)
    void DesenharItemMenu(const DRAWITEMSTRUCT* d, bool escolhido, HFONT fonte, COLORREF atras = FUNDO);
    // retangulo de cantos arredondados (GDI+, borda suave); borda = CLR_NONE para nao ter
    void Arredondado(HDC dc, RECT r, int raio, COLORREF cheio, COLORREF borda = CLR_NONE, int grossura = 1);
    // titulo de secao em ambar com a linha fina ate xFim (coordenadas ja na escala)
    void Secao(HDC dc, int x, int y, int xFim, const wchar_t* nome);
    LRESULT CorTexto(HDC dc, COLORREF cor);         // WM_CTLCOLORSTATIC: texto na cor dada sobre o fundo
    LRESULT CorTextoNoCartao(HDC dc, COLORREF cor); // o mesmo sobre o painel arredondado (CARTAO)
    LRESULT CorCampo(HDC dc);                       // WM_CTLCOLOREDIT / WM_CTLCOLORLISTBOX
}
