// Visual das janelas do programa (launcher, instrucoes, controles) com as cores do menu do jogo: fundo escuro, ambar,
// vermelho escuro. Caixas de marcar e botoes sao desenhados aqui (BS_OWNERDRAW); listas e campos ficam escuros.
#pragma once
#include <windows.h>

namespace tema
{
    constexpr COLORREF FUNDO = RGB(16, 14, 11), PAINEL = RGB(30, 26, 20), CAMPO = RGB(36, 32, 26);
    constexpr COLORREF AMBAR = RGB(236, 174, 52), AMBAR_ESC = RGB(150, 104, 30), VERMELHO = RGB(128, 20, 16), VERMELHO_CLARO = RGB(178, 36, 26);
    constexpr COLORREF TEXTO = RGB(228, 222, 206), TEXTO_FRACO = RGB(150, 142, 124), VERDE = RGB(130, 205, 95), ERRO = RGB(235, 90, 64);

    void Iniciar(float escala);   // pinceis e fontes (uma vez; escala = DPI / 96)
    HBRUSH PincelFundo();
    HBRUSH PincelCampo();
    HFONT FonteSecao();
    HFONT FonteBotao();
    int S(int v);                 // tamanho com a escala da tela

    void TituloEscuro(HWND janela);                 // barra de titulo escura (Windows 10/11; cor propria no 11)
    void ListaEscura(HWND combo);                   // lista escura (WM_CTLCOLORLISTBOX tambem: CorCampo)
    void AcompanharMouse(HWND botao);               // a borda do botao acende com o mouse em cima
    void DesenharCaixa(const DRAWITEMSTRUCT* d, bool marcado, HFONT fonte);
    void DesenharBotao(const DRAWITEMSTRUCT* d, bool destaque, HFONT fonte);   // destaque = vermelho (Jogar, Salvar)
    // titulo de secao em ambar com a linha fina dos paineis do menu ate xFim (coordenadas ja na escala)
    void Secao(HDC dc, int x, int y, int xFim, const wchar_t* nome);
    LRESULT CorTexto(HDC dc, COLORREF cor);         // WM_CTLCOLORSTATIC: texto na cor dada sobre o fundo
    LRESULT CorCampo(HDC dc);                       // WM_CTLCOLOREDIT / WM_CTLCOLORLISTBOX
}
