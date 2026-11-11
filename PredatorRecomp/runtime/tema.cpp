// Visual das janelas do programa com as cores do menu do jogo (veja tema.h)
#include "tema.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <atomic>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")
// controles comuns versao 6 (visual atual do Windows; necessario para as listas escuras)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace tema
{
    namespace
    {
        float g_escala = 1.0f;
        HBRUSH g_fundo, g_campo;
        HFONT g_secao, g_botao;
        std::atomic<HWND> g_sobre{ nullptr };   // botao com o mouse em cima

        LRESULT CALLBACK SubBotao(HWND b, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
        {
            if (msg == WM_MOUSEMOVE && g_sobre != b)
            {
                HWND antes = g_sobre.exchange(b);
                TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, b, 0 };
                TrackMouseEvent(&t);
                InvalidateRect(b, nullptr, FALSE);
                if (antes) InvalidateRect(antes, nullptr, FALSE);
            }
            else if (msg == WM_MOUSELEAVE && g_sobre == b)
            {
                g_sobre = nullptr;
                InvalidateRect(b, nullptr, FALSE);
            }
            return DefSubclassProc(b, msg, wp, lp);
        }
    }

    void Iniciar(float escala)
    {
        if (g_fundo) return;
        g_escala = escala;
        g_fundo = CreateSolidBrush(FUNDO);
        g_campo = CreateSolidBrush(CAMPO);
        g_secao = CreateFontW(-S(14), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_botao = CreateFontW(-S(15), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    }
    HBRUSH PincelFundo() { return g_fundo; }
    HBRUSH PincelCampo() { return g_campo; }
    HFONT FonteSecao() { return g_secao; }
    HFONT FonteBotao() { return g_botao; }
    int S(int v) { return (int)(v * g_escala + 0.5f); }

    void TituloEscuro(HWND j)
    {
        BOOL escuro = TRUE;
        DwmSetWindowAttribute(j, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &escuro, sizeof(escuro));
        COLORREF cor = RGB(24, 20, 15), texto = AMBAR;
        DwmSetWindowAttribute(j, 35 /* DWMWA_CAPTION_COLOR */, &cor, sizeof(cor));
        DwmSetWindowAttribute(j, 36 /* DWMWA_TEXT_COLOR */, &texto, sizeof(texto));
    }

    void ListaEscura(HWND combo) { SetWindowTheme(combo, L"DarkMode_CFD", nullptr); }
    void AcompanharMouse(HWND botao) { SetWindowSubclass(botao, SubBotao, 1, 0); }

    void DesenharCaixa(const DRAWITEMSTRUCT* d, bool marcado, HFONT fonte)
    {
        HDC dc = d->hDC;
        RECT r = d->rcItem;
        FillRect(dc, &r, g_fundo);
        int lado = S(16), y = (r.top + r.bottom - lado) / 2;
        RECT q{ r.left + S(2), y, r.left + S(2) + lado, y + lado };
        HBRUSH borda = CreateSolidBrush((d->itemState & ODS_FOCUS) || g_sobre == d->hwndItem ? AMBAR : AMBAR_ESC);
        FrameRect(dc, &q, borda);
        DeleteObject(borda);
        if (marcado)
        {
            RECT dentro{ q.left + S(3), q.top + S(3), q.right - S(3), q.bottom - S(3) };
            HBRUSH cheio = CreateSolidBrush(AMBAR);
            FillRect(dc, &dentro, cheio);
            DeleteObject(cheio);
        }
        wchar_t t[256] = {}; GetWindowTextW(d->hwndItem, t, 256);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, TEXTO);
        HGDIOBJ antes = SelectObject(dc, fonte);
        RECT rt{ q.right + S(10), r.top, r.right, r.bottom };
        DrawTextW(dc, t, -1, &rt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, antes);
    }

    void DesenharBotao(const DRAWITEMSTRUCT* d, bool destaque, HFONT fonte)
    {
        HDC dc = d->hDC;
        RECT r = d->rcItem;
        bool apertado = d->itemState & ODS_SELECTED;
        bool desativado = d->itemState & ODS_DISABLED;   // apagado: sem vermelho, sem acender, texto fraco
        if (desativado) destaque = false;
        bool aceso = !desativado && ((d->itemState & ODS_FOCUS) || g_sobre == d->hwndItem);
        COLORREF fundo = destaque ? (apertado ? VERMELHO : VERMELHO_CLARO) : (apertado ? CAMPO : PAINEL);
        HBRUSH f = CreateSolidBrush(fundo);
        FillRect(dc, &r, f);
        DeleteObject(f);
        if (destaque)   // como o item selecionado do menu: vermelho que escurece para a direita
        {
            TRIVERTEX v[2] = { { r.left, r.top, (COLOR16)(GetRValue(fundo) << 8), (COLOR16)(GetGValue(fundo) << 8), (COLOR16)(GetBValue(fundo) << 8), 0 },
                               { r.right, r.bottom, 0x4000, 0x0800, 0x0600, 0 } };
            GRADIENT_RECT gr{ 0, 1 };
            GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_H);
        }
        HBRUSH borda = CreateSolidBrush(aceso ? AMBAR : AMBAR_ESC);
        FrameRect(dc, &r, borda);
        if (aceso) { RECT r2{ r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 }; FrameRect(dc, &r2, borda); }
        DeleteObject(borda);
        wchar_t t[64] = {}; GetWindowTextW(d->hwndItem, t, 64);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, desativado ? RGB(90, 84, 72) : destaque || aceso ? RGB(255, 250, 240) : TEXTO);
        HGDIOBJ antes = SelectObject(dc, fonte);
        SetTextCharacterExtra(dc, S(2));
        DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SetTextCharacterExtra(dc, 0);
        SelectObject(dc, antes);
    }

    void Secao(HDC dc, int x, int y, int xFim, const wchar_t* nome)
    {
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ fonteAntes = SelectObject(dc, g_secao);
        SetTextCharacterExtra(dc, S(2));
        SetTextColor(dc, AMBAR);
        int n = (int)wcslen(nome);
        TextOutW(dc, x, y, nome, n);
        SIZE t; GetTextExtentPoint32W(dc, nome, n, &t);
        HPEN fina = CreatePen(PS_SOLID, 1, AMBAR_ESC);
        HGDIOBJ penAntes = SelectObject(dc, fina);
        MoveToEx(dc, x + t.cx + S(12), y + t.cy / 2, nullptr);
        LineTo(dc, xFim, y + t.cy / 2);
        SelectObject(dc, penAntes);
        DeleteObject(fina);
        SetTextCharacterExtra(dc, 0);
        SelectObject(dc, fonteAntes);
    }

    LRESULT CorTexto(HDC dc, COLORREF cor)
    {
        SetTextColor(dc, cor);
        SetBkColor(dc, FUNDO);
        return (LRESULT)g_fundo;
    }

    LRESULT CorCampo(HDC dc)
    {
        SetTextColor(dc, TEXTO);
        SetBkColor(dc, CAMPO);
        return (LRESULT)g_campo;
    }
}
