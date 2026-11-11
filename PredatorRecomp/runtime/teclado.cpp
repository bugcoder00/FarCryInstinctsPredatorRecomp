// Teclado do sistema (XamShowKeyboardUI): no Xbox o jogo abre o teclado virtual do console para o jogador digitar
// (codigo no MENU DE TRAPACAS, por exemplo). No PC abre esta caixinha com o titulo e a explicacao que o jogo mandou;
// o jogador digita com o teclado e confirma (Enter / OK) ou cancela (Esc / Cancelar / B no controle).
// A caixa roda na sua thread; o jogo continua desenhando e espera a resposta pelo "overlapped" (content.cpp).
#include "idioma.h"
#include "guest.h"
#include "opcoes.h"
#include "tema.h"
#include <windows.h>
#include <xinput.h>
#include <thread>
#include <atomic>
#include <functional>
#include <string>
#include <algorithm>
#pragma comment(lib, "xinput.lib")

HWND GetGameWindow();   // gpu.cpp

void TecladoAbrir(std::wstring titulo, std::wstring descricao, std::wstring padrao, uint32_t maximo,
                  std::function<void(bool ok, const std::wstring& texto)> fim);
bool TecladoAberto();

namespace
{
    std::atomic<bool> g_aberto{ false };
    float g_escala = 1.0f;
    int S(int v) { return (int)(v * g_escala + 0.5f); }
    HFONT g_fonte = nullptr, g_fonteCampo = nullptr;
    enum { ID_DESC = 10, ID_CAMPO, ID_OK, ID_CANCELAR };
    constexpr UINT_PTR TIMER_PAD = 1;

    struct Estado
    {
        std::wstring titulo, descricao, padrao;
        uint32_t maximo = 0;
        bool ok = false;
        std::wstring texto;
    };

    HWND Criar(HWND pai, const wchar_t* classe, const wchar_t* texto, DWORD estilo, DWORD ex, int x, int y, int w, int h, int id, HFONT f)
    {
        HWND c = CreateWindowExW(ex, classe, texto, WS_CHILD | WS_VISIBLE | estilo, S(x), S(y), S(w), S(h), pai,
                                 (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        return c;
    }

    void Fechar(HWND j, bool ok)
    {
        auto e = (Estado*)GetWindowLongPtrW(j, GWLP_USERDATA);
        if (e)
        {
            e->ok = ok;
            if (ok)
            {
                int n = GetWindowTextLengthW(GetDlgItem(j, ID_CAMPO));
                std::wstring t(n + 1, L'\0');
                GetWindowTextW(GetDlgItem(j, ID_CAMPO), t.data(), n + 1);
                t.resize(n);
                e->texto = t;
            }
        }
        DestroyWindow(j);
    }

    LRESULT CALLBACK Proc(HWND j, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_COMMAND:
            if (HIWORD(wp) == BN_CLICKED && (LOWORD(wp) == ID_OK || LOWORD(wp) == ID_CANCELAR)) Fechar(j, LOWORD(wp) == ID_OK);
            return 0;
        case WM_TIMER:
        {
            // B no controle cancela (o Enter do teclado confirma); so quando o botao e apertado agora
            static bool antes = true;
            XINPUT_STATE s{};
            bool b = XInputGetState(0, &s) == ERROR_SUCCESS && (s.Gamepad.wButtons & XINPUT_GAMEPAD_B);
            if (b && !antes) Fechar(j, false);
            antes = b;
            return 0;
        }
        case WM_DRAWITEM:
        {
            auto d = (const DRAWITEMSTRUCT*)lp;
            tema::DesenharBotao(d, d->CtlID == ID_OK, tema::FonteBotao());
            return TRUE;
        }
        case WM_CTLCOLORSTATIC: return tema::CorTexto((HDC)wp, tema::TEXTO);
        case WM_CTLCOLOREDIT: return tema::CorCampo((HDC)wp);
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps; HDC dc = BeginPaint(j, &ps);
            RECT r; GetClientRect(j, &r);
            FillRect(dc, &r, tema::PincelFundo());
            auto e = (Estado*)GetWindowLongPtrW(j, GWLP_USERDATA);
            tema::Secao(dc, S(16), S(12), r.right - S(16), e && !e->titulo.empty() ? e->titulo.c_str() : TR(L"TECLADO", L"KEYBOARD"));
            EndPaint(j, &ps);
            return 0;
        }
        case WM_CLOSE: Fechar(j, false); return 0;
        case WM_DESTROY: KillTimer(j, TIMER_PAD); PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(j, msg, wp, lp);
    }

    void Rodar(Estado e, std::function<void(bool, const std::wstring&)> fim)
    {
        HDC dc = GetDC(nullptr);
        g_escala = GetDeviceCaps(dc, LOGPIXELSY) / 96.0f;
        ReleaseDC(nullptr, dc);
        tema::Iniciar(g_escala);
        if (!g_fonte) g_fonte = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        if (!g_fonteCampo) g_fonteCampo = CreateFontW(-S(18), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        static bool registrada = false;
        if (!registrada)
        {
            WNDCLASSW wc{};
            wc.lpfnWndProc = Proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // seta (IDC_ARROW)
            wc.hbrBackground = tema::PincelFundo();
            wc.lpszClassName = L"PredatorTeclado";
            RegisterClassW(&wc);
            registrada = true;
        }
        constexpr int LARG = 480, Y_DESC = 40, Y_CAMPO = 96, Y_BOT = 146, ALT = 196;
        RECT r{ 0, 0, S(LARG), S(ALT) };
        const DWORD estilo = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        AdjustWindowRect(&r, estilo, FALSE);
        int w = r.right - r.left, h = r.bottom - r.top;
        HWND j = CreateWindowExW(WS_EX_TOPMOST, L"PredatorTeclado", TR(L"Teclado - Far Cry Instincts Predator", L"Keyboard - Far Cry Instincts Predator"), estilo,
                                 (GetSystemMetrics(SM_CXSCREEN) - w) / 2, std::max(0, (GetSystemMetrics(SM_CYSCREEN) - h) / 2), w, h,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!j)
        {
            Log("[teclado] a caixa nao abriu (erro %lu)", GetLastError());
            g_aberto = false;
            fim(false, L"");
            return;
        }
        SetWindowLongPtrW(j, GWLP_USERDATA, (LONG_PTR)&e);
        Criar(j, L"STATIC", e.descricao.c_str(), SS_LEFT, 0, 16, Y_DESC, LARG - 32, 48, ID_DESC, g_fonte);
        HWND campo = Criar(j, L"EDIT", e.padrao.c_str(), ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, 0, 16, Y_CAMPO, LARG - 32, 34, ID_CAMPO, g_fonteCampo);
        if (e.maximo > 1) SendMessageW(campo, EM_SETLIMITTEXT, e.maximo - 1, 0);
        SendMessageW(campo, EM_SETSEL, 0, -1);
        Criar(j, L"BUTTON", L"OK", BS_OWNERDRAW | WS_TABSTOP, 0, LARG - 16 - 2 * 120 - 10, Y_BOT, 120, 34, ID_OK, g_fonte);
        Criar(j, L"BUTTON", TR(L"Cancelar", L"Cancel"), BS_OWNERDRAW | WS_TABSTOP, 0, LARG - 16 - 120, Y_BOT, 120, 34, ID_CANCELAR, g_fonte);
        AplicarIcone(j);
        tema::TituloEscuro(j);
        SetTimer(j, TIMER_PAD, 50, nullptr);
        ShowWindow(j, SW_SHOW);
        SetForegroundWindow(j);
        SetFocus(campo);
        Log("[teclado] caixa aberta");
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0)
        {
            if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN && IsWindow(j)) { Fechar(j, true); continue; }
            if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE && IsWindow(j)) { Fechar(j, false); continue; }
            if (IsWindow(j) && IsDialogMessageW(j, &m)) continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        g_aberto = false;
        // devolve o foco ao jogo
        if (HWND jogo = GetGameWindow()) SetForegroundWindow(jogo);
        fim(e.ok, e.texto);
    }
}

void TecladoAbrir(std::wstring titulo, std::wstring descricao, std::wstring padrao, uint32_t maximo,
                  std::function<void(bool ok, const std::wstring& texto)> fim)
{
    if (g_aberto.exchange(true)) { fim(false, L""); return; }
    Estado e;
    e.titulo = std::move(titulo); e.descricao = std::move(descricao); e.padrao = std::move(padrao); e.maximo = maximo;
    std::thread(Rodar, std::move(e), std::move(fim)).detach();
}

bool TecladoAberto() { return g_aberto; }
