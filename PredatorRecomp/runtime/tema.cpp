// Visual das janelas do programa com as cores do menu do jogo (veja tema.h)
#include "tema.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <atomic>
#include <algorithm>
#include <objidl.h>
#include <gdiplus.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "gdiplus.lib")
// controles comuns versao 6 (visual atual do Windows; necessario para as listas escuras)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace tema
{
    namespace
    {
        float g_escala = 1.0f;
        HBRUSH g_fundo, g_campo, g_cartao;
        HFONT g_secao, g_botao, g_menu;
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

        Gdiplus::Color Cor(COLORREF c, BYTE a = 255) { return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

        // mistura duas cores (t = 0 -> a, 1 -> b)
        COLORREF Misturar(COLORREF a, COLORREF b, float t)
        {
            auto m = [t](int x, int y) { return (BYTE)(x + (y - x) * t + 0.5f); };
            return RGB(m(GetRValue(a), GetRValue(b)), m(GetGValue(a), GetGValue(b)), m(GetBValue(a), GetBValue(b)));
        }

        void Caminho(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float raio)
        {
            float d = std::min({ 2 * raio, w, h });
            if (d <= 0.5f) { p.AddRectangle(Gdiplus::RectF(x, y, w, h)); return; }
            p.AddArc(x, y, d, d, 180.0f, 90.0f);
            p.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
            p.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
            p.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
            p.CloseFigure();
        }

        void Texto(HDC dc, const wchar_t* t, RECT r, COLORREF cor, HFONT fonte, UINT formato, int espaco)
        {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, cor);
            HGDIOBJ antes = SelectObject(dc, fonte);
            SetTextCharacterExtra(dc, espaco);
            DrawTextW(dc, t, -1, &r, formato | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            SetTextCharacterExtra(dc, 0);
            SelectObject(dc, antes);
        }
    }

    void Iniciar(float escala)
    {
        if (g_fundo) return;
        static ULONG_PTR gdiplus = 0;   // GDI+ para as bordas arredondadas (o launcher tambem liga; conta as chamadas)
        Gdiplus::GdiplusStartupInput gi;
        Gdiplus::GdiplusStartup(&gdiplus, &gi, nullptr);
        g_escala = escala;
        g_fundo = CreateSolidBrush(FUNDO);
        g_campo = CreateSolidBrush(CAMPO);
        g_cartao = CreateSolidBrush(CARTAO);
        g_secao = CreateFontW(-S(13), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_botao = CreateFontW(-S(14), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_menu = CreateFontW(-S(15), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    }
    HBRUSH PincelFundo() { return g_fundo; }
    HBRUSH PincelCampo() { return g_campo; }
    HBRUSH PincelCartao() { return g_cartao; }
    HFONT FonteSecao() { return g_secao; }
    HFONT FonteBotao() { return g_botao; }
    HFONT FonteMenu() { return g_menu; }
    int S(int v) { return (int)(v * g_escala + 0.5f); }

    void TituloEscuro(HWND j)
    {
        BOOL escuro = TRUE;
        DwmSetWindowAttribute(j, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &escuro, sizeof(escuro));
        COLORREF cor = RGB(20, 18, 15), texto = AMBAR;
        DwmSetWindowAttribute(j, 35 /* DWMWA_CAPTION_COLOR */, &cor, sizeof(cor));
        DwmSetWindowAttribute(j, 36 /* DWMWA_TEXT_COLOR */, &texto, sizeof(texto));
        int cantos = 2;   // DWMWCP_ROUND: janela com cantos arredondados (Windows 11)
        DwmSetWindowAttribute(j, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cantos, sizeof(cantos));
    }

    void ListaEscura(HWND combo) { SetWindowTheme(combo, L"DarkMode_CFD", nullptr); }
    void AcompanharMouse(HWND botao) { SetWindowSubclass(botao, SubBotao, 1, 0); }
    bool MouseSobre(HWND botao) { return g_sobre == botao; }

    void Arredondado(HDC dc, RECT r, int raio, COLORREF cheio, COLORREF borda, int grossura)
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        float x = (float)r.left, y = (float)r.top, w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
        if (cheio != CLR_NONE)
        {
            Gdiplus::GraphicsPath p;
            Caminho(p, x, y, w, h, (float)raio);
            Gdiplus::SolidBrush b(Cor(cheio));
            g.FillPath(&b, &p);
        }
        if (borda != CLR_NONE && grossura > 0)
        {
            const float m = grossura / 2.0f;
            Gdiplus::GraphicsPath p;
            Caminho(p, x + m, y + m, w - grossura, h - grossura, std::max(0.0f, raio - m));
            Gdiplus::Pen caneta(Cor(borda), (float)grossura);
            g.DrawPath(&caneta, &p);
        }
    }

    // interruptor (liga/desliga) no lugar da caixa de marcar: trilho arredondado com a bolinha; ambar = ligado
    void DesenharCaixa(const DRAWITEMSTRUCT* d, bool marcado, HFONT fonte, COLORREF atras)
    {
        HDC dc = d->hDC;
        RECT r = d->rcItem;
        HBRUSH f = CreateSolidBrush(atras);
        FillRect(dc, &r, f);
        DeleteObject(f);
        const bool aceso = (d->itemState & ODS_FOCUS) || g_sobre == d->hwndItem;
        const int alt = S(18), larg = S(34), y = (r.top + r.bottom - alt) / 2, x = r.left + S(2);
        RECT trilho{ x, y, x + larg, y + alt };
        COLORREF cheio = marcado ? (aceso ? RGB(248, 192, 76) : AMBAR) : (aceso ? RGB(70, 63, 52) : RGB(56, 50, 41));
        Arredondado(dc, trilho, alt / 2, cheio, marcado ? CLR_NONE : (aceso ? AMBAR_ESC : LINHA));
        const int bola = alt - S(6), bx = marcado ? trilho.right - S(3) - bola : trilho.left + S(3);
        RECT rb{ bx, y + S(3), bx + bola, y + S(3) + bola };
        Arredondado(dc, rb, bola / 2, marcado ? RGB(30, 24, 14) : TEXTO_FRACO);
        wchar_t t[256] = {}; GetWindowTextW(d->hwndItem, t, 256);
        RECT rt{ trilho.right + S(12), r.top, r.right, r.bottom };
        Texto(dc, t, rt, aceso ? RGB(250, 246, 236) : TEXTO, fonte, DT_LEFT, 0);
    }

    void DesenharBotao(const DRAWITEMSTRUCT* d, bool destaque, HFONT fonte, COLORREF atras)
    {
        HDC dc = d->hDC;
        RECT r = d->rcItem;
        const bool apertado = d->itemState & ODS_SELECTED;
        const bool desativado = d->itemState & ODS_DISABLED;   // apagado: sem vermelho, sem acender, texto fraco
        if (desativado) destaque = false;
        const bool aceso = !desativado && ((d->itemState & ODS_FOCUS) || g_sobre == d->hwndItem);
        HBRUSH f = CreateSolidBrush(atras);
        FillRect(dc, &r, f);
        DeleteObject(f);
        const int raio = S(7);
        if (destaque)   // vermelho do item escolhido do menu do jogo, escurecendo para baixo
        {
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            Gdiplus::GraphicsPath p;
            Caminho(p, (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top), (float)raio);
            COLORREF cima = apertado ? VERMELHO : aceso ? RGB(204, 52, 36) : VERMELHO_CLARO;
            COLORREF baixo = apertado ? RGB(90, 14, 10) : aceso ? RGB(150, 28, 20) : RGB(118, 22, 16);
            Gdiplus::LinearGradientBrush b(Gdiplus::PointF(0, (float)r.top), Gdiplus::PointF(0, (float)r.bottom + 1), Cor(cima), Cor(baixo));
            g.FillPath(&b, &p);
            Gdiplus::Pen brilho(Cor(aceso ? AMBAR : RGB(214, 70, 50), aceso ? 255 : 120), 1.0f);
            Gdiplus::GraphicsPath p2;
            Caminho(p2, r.left + 0.5f, r.top + 0.5f, (float)(r.right - r.left) - 1, (float)(r.bottom - r.top) - 1, raio - 0.5f);
            g.DrawPath(&brilho, &p2);
        }
        else
        {
            COLORREF cheio = desativado ? RGB(27, 24, 20) : apertado ? RGB(30, 27, 22) : aceso ? RGB(50, 44, 36) : RGB(38, 34, 28);
            COLORREF borda = desativado ? RGB(40, 36, 30) : aceso ? AMBAR_ESC : LINHA;
            Arredondado(dc, r, raio, cheio, borda);
        }
        wchar_t t[64] = {}; GetWindowTextW(d->hwndItem, t, 64);
        COLORREF cor = desativado ? RGB(86, 80, 68) : destaque || aceso ? RGB(255, 250, 240) : TEXTO;
        if (apertado) OffsetRect(&r, 0, 1);
        Texto(dc, t, r, cor, fonte, DT_CENTER, S(1));
    }

    // item do menu lateral: sem caixa; com o mouse em cima aparece um fundo; o escolhido tem o fundo, a barrinha ambar
    // na esquerda e o texto ambar
    void DesenharItemMenu(const DRAWITEMSTRUCT* d, bool escolhido, HFONT fonte, COLORREF atras)
    {
        HDC dc = d->hDC;
        RECT r = d->rcItem;
        const bool aceso = (d->itemState & ODS_FOCUS) || g_sobre == d->hwndItem;
        HBRUSH f = CreateSolidBrush(atras);
        FillRect(dc, &r, f);
        DeleteObject(f);
        if (escolhido) Arredondado(dc, r, S(8), Misturar(atras, AMBAR, 0.10f));
        else if (aceso) Arredondado(dc, r, S(8), Misturar(atras, TEXTO, 0.07f));
        if (escolhido)
        {
            const int h = (r.bottom - r.top) / 2;
            RECT barra{ r.left + S(8), r.top + (r.bottom - r.top - h) / 2, r.left + S(12), r.top + (r.bottom - r.top + h) / 2 };
            Arredondado(dc, barra, S(2), AMBAR);
        }
        wchar_t t[64] = {}; GetWindowTextW(d->hwndItem, t, 64);
        RECT rt{ r.left + S(26), r.top, r.right - S(8), r.bottom };
        Texto(dc, t, rt, escolhido ? AMBAR : aceso ? RGB(250, 246, 236) : RGB(196, 188, 170), fonte, DT_LEFT, S(1));
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
        if (x + t.cx + S(12) < xFim)
        {
            HPEN fina = CreatePen(PS_SOLID, 1, LINHA);
            HGDIOBJ penAntes = SelectObject(dc, fina);
            MoveToEx(dc, x + t.cx + S(12), y + t.cy / 2, nullptr);
            LineTo(dc, xFim, y + t.cy / 2);
            SelectObject(dc, penAntes);
            DeleteObject(fina);
        }
        SetTextCharacterExtra(dc, 0);
        SelectObject(dc, fonteAntes);
    }

    LRESULT CorTexto(HDC dc, COLORREF cor)
    {
        SetTextColor(dc, cor);
        SetBkColor(dc, FUNDO);
        return (LRESULT)g_fundo;
    }

    LRESULT CorTextoNoCartao(HDC dc, COLORREF cor)
    {
        SetTextColor(dc, cor);
        SetBkColor(dc, CARTAO);
        return (LRESULT)g_cartao;
    }

    LRESULT CorCampo(HDC dc)
    {
        SetTextColor(dc, TEXTO);
        SetBkColor(dc, CAMPO);
        return (LRESULT)g_campo;
    }
}
