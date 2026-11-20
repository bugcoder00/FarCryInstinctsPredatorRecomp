// Janela das opcoes de video durante o jogo (F2; input.cpp). As mesmas opcoes do launcher (aba VIDEO) que podem mudar
// com o jogo rodando: cada mudanca vale na hora e vai para o opcoes.ini (no modo de teste nao grava). Tamanho da janela,
// formato da imagem e tela cheia (Alt+Enter) ficam no launcher.
// Como mudam: resolucao interna, FSR, MSAA e sombras refazem as imagens do desenho no fim do quadro (xdraw::PedirNovaEscala);
// antisserrilhado recompila no proximo quadro (xdraw::RecarregarSuavizacao); nitidez do FSR, filtro de textura,
// oclusao de ambiente, FPS e sincronia vertical sao lidos a cada quadro; distancia e detalhe de longe pelo fio de
// graficos_extra.cpp (250 ms).
// Roda na sua propria thread com o seu laco de mensagens, como a janela dos controles (controles_janela.cpp).
// O botao TRAPACAS abre a janela das trapacas (as mesmas da aba TRAPACAS do launcher, trapacas.cpp): cada caixa vale
// na hora (trapacas.cpp le as opcoes a cada quadro) e vai para o opcoes.ini.
#include "idioma.h"
#include "controles.h"
#include "guest.h"
#include "opcoes.h"
#include "tema.h"
#include "xenos_draw.h"
#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <mutex>
#include <thread>

HWND GetGameWindow();   // gpu.cpp
void TrapacasAbrirJanela();   // abaixo

namespace
{
    enum : int
    {
        ID_ESCALA = 100, ID_FSR, ID_NITIDEZ, ID_AA, ID_MSAA, ID_SOMBRAS, ID_FILTRO, ID_DISTANCIA, ID_DETALHE, ID_AO, ID_FPS,
        ID_VSYNC = 120, ID_DICA = 121, ID_FECHAR = 122, ID_TRAPACAS = 123, ID_HDR = 124, ID_FALSO = 125, ID_MOSTRARFPS = 126,
    };
    constexpr int N_LISTAS = ID_FPS - ID_ESCALA + 1;
    constexpr int LARGURA = 660, Y0 = 44, PASSO = 32;

    std::atomic<HWND> g_janela{ nullptr };
    std::mutex g_abrir;
    HFONT g_fonte = nullptr;
    float g_escala = 1.0f;
    bool g_vsync = false;
    bool g_hdr = false;
    bool g_falso = false;        // HDR falso (render.cpp)
    bool g_mostrarFps = false;   // contador de FPS e ms na tela (render.cpp)

    int S(int v) { return (int)(v * g_escala + 0.5f); }

    HWND Criar(HWND pai, const wchar_t* classe, const wchar_t* texto, DWORD estilo, int x, int y, int w, int h, int id)
    {
        if (id == ID_VSYNC || id == ID_HDR || id == ID_FALSO || id == ID_MOSTRARFPS || id == ID_FECHAR || id == ID_TRAPACAS) estilo |= BS_OWNERDRAW;
        HWND c = CreateWindowExW(0, classe, texto, WS_CHILD | WS_VISIBLE | estilo, S(x), S(y), S(w), S(h), pai,
                                 (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_fonte, TRUE);
        if (!wcscmp(classe, WC_COMBOBOXW)) tema::ListaEscura(c);
        if (id == ID_FECHAR || id == ID_TRAPACAS) tema::AcompanharMouse(c);
        return c;
    }

    // rotulo + lista na linha i (os textos sao os mesmos do launcher: as traducoes ja existem)
    void Lista(HWND j, int id, const wchar_t* rotulo, std::initializer_list<const wchar_t*> itens, int sel)
    {
        const int y = Y0 + (id - ID_ESCALA) * PASSO;
        Criar(j, L"STATIC", rotulo, 0, 16, y + 4, 200, 20, -1);
        HWND cb = Criar(j, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 220, y, LARGURA - 236, 300, id);
        for (const wchar_t* t : itens) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t);
        SendMessageW(cb, CB_SETCURSEL, sel, 0);
    }

    void Habilitar(HWND j)
    {
        const bool fsr = OpcoesAtuais().fsr > 0;
        EnableWindow(GetDlgItem(j, ID_ESCALA), !fsr);
        EnableWindow(GetDlgItem(j, ID_NITIDEZ), fsr);
    }

    void Gravar()
    {
        if (OpcoesAtuais().modoTeste) Log("[video] modo teste: opcoes.ini nao foi gravado");
        else OpcoesSalvar();
    }

    // uma lista mudou: aplica e grava
    void Mudou(HWND j, int id)
    {
        Opcoes& o = OpcoesAtuais();
        const int v = std::max(0, (int)SendDlgItemMessageW(j, id, CB_GETCURSEL, 0, 0));
        switch (id)
        {
        case ID_ESCALA: o.escala = v; xdraw::PedirNovaEscala(); break;
        case ID_FSR: o.fsr = FsrDaLista(v); xdraw::PedirNovaEscala(); Habilitar(j); break;
        case ID_NITIDEZ: o.fsrNitidez = v; break;
        case ID_AA: o.antisserrilhado = v; xdraw::RecarregarSuavizacao(); break;
        case ID_MSAA: o.msaa = v; xdraw::PedirNovaEscala(); break;   // refaz as superficies da tela no fim do quadro
        case ID_SOMBRAS: o.sombras = v; xdraw::PedirNovaEscala(); break;
        case ID_FILTRO: o.filtroTextura = v; break;
        case ID_DISTANCIA: o.distancia = v - 2; break;   // listas com os niveis menores no topo (como no launcher)
        case ID_DETALHE: o.detalhe = v - 2; break;
        case ID_AO: o.ao = v; break;
        case ID_FPS: o.fps = v; break;
        default: return;
        }
        Log("[video] janela F2: opcao %d = %d", id - ID_ESCALA, v);
        Gravar();
    }

    void Montar(HWND j)
    {
        const Opcoes& o = OpcoesAtuais();
        Lista(j, ID_ESCALA, TR(L"Resolução interna:", L"Internal resolution:"),
              { TR(L"1x - 1280 × 720 (mais leve)", L"1x - 1280 × 720 (lightest)"), TR(L"1,5x - 1920 × 1080", L"1.5x - 1920 × 1080"),
                TR(L"2x - 2560 × 1440 (mais nítido, mais pesado)", L"2x - 2560 × 1440 (sharper, heavier)"),
                TR(L"3x - 3840 × 2160 / 4K (experimental)", L"3x - 3840 × 2160 / 4K (experimental)") }, std::clamp(o.escala, 0, 3));
        Lista(j, ID_FSR, TR(L"Upscale AMD FSR 1:", L"AMD FSR 1 upscaling:"),
              { TR(L"Desligado", L"Off"), TR(L"Ultra qualidade (desenha em 1x: 1280 × 720, mais nítido)", L"Ultra quality (draws at 1x: 1280 × 720, sharpest)"),
                TR(L"Qualidade (desenha em 0,75x: 960 × 540)", L"Quality (draws at 0.75x: 960 × 540)"),
                TR(L"Equilibrado (desenha em 0,5x: 640 × 360)", L"Balanced (draws at 0.5x: 640 × 360)"),
                TR(L"Desempenho (0,5x, ampliação simples, mais leve)", L"Performance (0.5x, simple upscaling, lightest)") }, ListaDoFsr(o.fsr));
        Lista(j, ID_NITIDEZ, TR(L"Nitidez do FSR:", L"FSR sharpness:"),
              { TR(L"Desligada", L"Off"), TR(L"Baixa", L"Low"), TR(L"Média", L"Medium"), TR(L"Alta", L"High"), TR(L"Máxima", L"Maximum") },
              std::clamp(o.fsrNitidez, 0, 4));
        Lista(j, ID_AA, TR(L"Antisserrilhado (FXAA):", L"Anti-aliasing (FXAA):"),
              { TR(L"Desligado", L"Off"), TR(L"1 - leve", L"1 - light"), TR(L"2 - médio", L"2 - medium"), TR(L"3 - forte (bordas mais lisas)", L"3 - strong (smoother edges)") },
              std::clamp(o.antisserrilhado, 0, 3));
        Lista(j, ID_MSAA, TR(L"Antisserrilhado MSAA:", L"MSAA anti-aliasing:"),
              { TR(L"Desligado", L"Off"), L"2x", L"4x", TR(L"8x (mais pesado)", L"8x (heavier)") }, std::clamp(o.msaa, 0, 3));
        Lista(j, ID_SOMBRAS, TR(L"Sombras:", L"Shadows:"),
              { L"Normal", TR(L"Alta (bordas mais definidas)", L"High (sharper edges)"), TR(L"Muito alta (mais pesada)", L"Very high (heavier)") },
              std::clamp(o.sombras, 0, 2));
        Lista(j, ID_FILTRO, TR(L"Filtro de textura:", L"Texture filtering:"),
              { TR(L"Original", L"Original"), TR(L"Anisotrópico 2x", L"Anisotropic 2x"), TR(L"Anisotrópico 4x", L"Anisotropic 4x"),
                TR(L"Anisotrópico 8x", L"Anisotropic 8x"), TR(L"Anisotrópico 16x (chão nítido de longe)", L"Anisotropic 16x (sharp ground at a distance)") },
              std::clamp(o.filtroTextura, 0, 4));
        Lista(j, ID_DISTANCIA, TR(L"Distância de visão:", L"Draw distance:"),
              { TR(L"Mínima (0,5x, mais leve)", L"Shortest (0.5x, lighter)"), TR(L"Curta (0,75x)", L"Short (0.75x)"),
                TR(L"Original", L"Original"), TR(L"Longe (1,5x)", L"Far (1.5x)"), TR(L"Muito longe (2x)", L"Very far (2x)"),
                TR(L"Máxima (3x, mais pesada)", L"Maximum (3x, heavier)") }, std::clamp(o.distancia, -2, 3) + 2);
        Lista(j, ID_DETALHE, TR(L"Detalhe de longe:", L"Detail at a distance:"),
              { TR(L"Mínimo (0,5x, mais leve)", L"Lowest (0.5x, lighter)"), TR(L"Baixo (0,75x)", L"Low (0.75x)"),
                TR(L"Original", L"Original"), TR(L"Alto (1,5x)", L"High (1.5x)"), TR(L"Muito alto (2x)", L"Very high (2x)"),
                TR(L"Máximo (3x, mais pesado)", L"Maximum (3x, heavier)") }, std::clamp(o.detalhe, -2, 3) + 2);
        Lista(j, ID_AO, TR(L"Oclusão de ambiente:", L"Ambient occlusion:"),
              { TR(L"Desligada (original)", L"Off (original)"), TR(L"Leve", L"Light"), TR(L"Média", L"Medium"),
                TR(L"Forte (cantos bem marcados)", L"Strong (well-marked corners)") }, std::clamp(o.ao, 0, 3));
        Lista(j, ID_FPS, TR(L"Quadros por segundo:", L"Frames per second:"),
              { TR(L"30 FPS (como o jogo original)", L"30 FPS (like the original game)"), TR(L"60 FPS (mais fluido, pesa o dobro)", L"60 FPS (smoother, twice the load)"),
                TR(L"Sem limite (experimental)", L"Unlocked (experimental)") }, std::clamp(o.fps, 0, 2));
        const int y = Y0 + N_LISTAS * PASSO;
        g_vsync = o.vsync;
        Criar(j, L"BUTTON", TR(L"Sincronia vertical (evita \"rasgos\" na imagem, pode baixar o FPS)", L"Vertical sync (avoids image \"tearing\", may lower the FPS)"),
              WS_TABSTOP, 16, y + 4, LARGURA - 32, 24, ID_VSYNC);
        g_hdr = o.autoHdr;
        Criar(j, L"BUTTON", TR(L"Auto HDR (realces mais brilhantes; precisa do HDR ligado no Windows)", L"Auto HDR (brighter highlights; needs HDR turned on in Windows)"),
              WS_TABSTOP, 16, y + 32, LARGURA - 32, 24, ID_HDR);
        g_falso = o.hdrFalso;
        Criar(j, L"BUTTON", TR(L"HDR falso (mais contraste e cor em monitor comum, sem HDR)", L"Fake HDR (more contrast and color on a regular monitor, no HDR)"),
              WS_TABSTOP, 16, y + 60, LARGURA - 32, 24, ID_FALSO);
        g_mostrarFps = o.mostrarFps;
        Criar(j, L"BUTTON", TR(L"Mostrar FPS e ms no canto da tela", L"Show FPS and ms in the corner of the screen"),
              WS_TABSTOP, 16, y + 88, LARGURA - 32, 24, ID_MOSTRARFPS);
        Criar(j, L"STATIC", TR(L"As mudanças valem na hora e ficam gravadas no opcoes.ini. Tamanho da janela, formato da imagem e"
                               L" tela cheia (Alt+Enter) ficam no launcher.",
                               L"Changes apply right away and are saved in opcoes.ini. Window size, aspect ratio and full screen"
                               L" (Alt+Enter) stay in the launcher."), SS_LEFT, 16, y + 124, LARGURA - 32, 40, ID_DICA);
        Criar(j, L"BUTTON", TR(L"TRAPAÇAS", L"CHEATS"), WS_TABSTOP, 16, y + 176, 160, 36, ID_TRAPACAS);
        Criar(j, L"BUTTON", TR(L"FECHAR", L"CLOSE"), WS_TABSTOP, LARGURA - 16 - 140, y + 176, 140, 36, ID_FECHAR);
        Habilitar(j);
    }

    LRESULT CALLBACK Proc(HWND j, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_CREATE: Montar(j); return 0;
        case WM_COMMAND:
        {
            const int id = LOWORD(wp);
            if (id >= ID_ESCALA && id <= ID_FPS && HIWORD(wp) == CBN_SELCHANGE) { Mudou(j, id); return 0; }
            if (id == ID_VSYNC && (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == BN_DOUBLECLICKED))
            {
                g_vsync = !g_vsync;
                OpcoesAtuais().vsync = g_vsync;
                InvalidateRect((HWND)lp, nullptr, FALSE);
                Log("[video] janela F2: sincronia vertical = %d", g_vsync ? 1 : 0);
                Gravar();
                return 0;
            }
            if (id == ID_HDR && (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == BN_DOUBLECLICKED))
            {
                g_hdr = !g_hdr;
                OpcoesAtuais().autoHdr = g_hdr;   // render.cpp troca a saida no proximo quadro (fica SDR se o monitor nao tiver HDR)
                InvalidateRect((HWND)lp, nullptr, FALSE);
                Log("[video] janela F2: auto HDR = %d", g_hdr ? 1 : 0);
                Gravar();
                return 0;
            }
            if ((id == ID_FALSO || id == ID_MOSTRARFPS) && (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == BN_DOUBLECLICKED))
            {
                bool& v = id == ID_FALSO ? g_falso : g_mostrarFps;
                v = !v;
                (id == ID_FALSO ? OpcoesAtuais().hdrFalso : OpcoesAtuais().mostrarFps) = v;   // render.cpp le a cada quadro
                InvalidateRect((HWND)lp, nullptr, FALSE);
                Log("[video] janela F2: %s = %d", id == ID_FALSO ? "HDR falso" : "mostrar FPS", v ? 1 : 0);
                Gravar();
                return 0;
            }
            if (id == ID_TRAPACAS && (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == BN_DOUBLECLICKED)) { TrapacasAbrirJanela(); return 0; }
            if (id == ID_FECHAR || id == IDCANCEL || id == IDOK) { DestroyWindow(j); return 0; }
            break;
        }
        case WM_CTLCOLORSTATIC:
            return tema::CorTexto((HDC)wp, GetDlgCtrlID((HWND)lp) == ID_DICA ? tema::TEXTO_FRACO : tema::TEXTO);
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return tema::CorCampo((HDC)wp);
        case WM_DRAWITEM:
        {
            auto* d = (const DRAWITEMSTRUCT*)lp;
            if (d->CtlID == ID_VSYNC) tema::DesenharCaixa(d, g_vsync, g_fonte);
            else if (d->CtlID == ID_HDR) tema::DesenharCaixa(d, g_hdr, g_fonte);
            else if (d->CtlID == ID_FALSO) tema::DesenharCaixa(d, g_falso, g_fonte);
            else if (d->CtlID == ID_MOSTRARFPS) tema::DesenharCaixa(d, g_mostrarFps, g_fonte);
            else tema::DesenharBotao(d, false, tema::FonteBotao());
            return TRUE;
        }
        case WM_LBUTTONDOWN: SetFocus(j); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(j, &ps);
            RECT r; GetClientRect(j, &r);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
            HGDIOBJ antes = SelectObject(mem, bmp);
            FillRect(mem, &r, tema::PincelFundo());
            tema::Secao(mem, S(16), S(12), r.right - S(16), TR(L"VÍDEO", L"VIDEO"));
            BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, antes);
            DeleteObject(bmp); DeleteDC(mem);
            EndPaint(j, &ps);
            return 0;
        }
        case WM_CLOSE: DestroyWindow(j); return 0;
        case WM_DESTROY:
            g_janela = nullptr;
            if (HWND jogo = GetGameWindow()) SetForegroundWindow(jogo);   // volta para o jogo (o mouse e preso de novo)
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(j, msg, wp, lp);
    }

    void Rodar()
    {
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icc);
        HDC dc = GetDC(nullptr);
        g_escala = GetDeviceCaps(dc, LOGPIXELSY) / 96.0f;
        ReleaseDC(nullptr, dc);
        tema::Iniciar(g_escala);
        if (!g_fonte) g_fonte = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        static bool registrada = false;
        if (!registrada)
        {
            WNDCLASSW wc{};
            wc.lpfnWndProc = Proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            wc.hbrBackground = tema::PincelFundo();
            wc.lpszClassName = L"PredatorVideo";
            RegisterClassW(&wc);
            registrada = true;
        }
        // tamanho da area de dentro: as 10 listas, a caixa da sincronia, a dica e o botao
        RECT area{ 0, 0, S(LARGURA), S(Y0 + N_LISTAS * PASSO + 176 + 36 + 16) };
        const DWORD estilo = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
        AdjustWindowRect(&area, estilo, FALSE);
        HWND j = CreateWindowExW(WS_EX_TOPMOST, L"PredatorVideo", TR(L"Opções de vídeo - Far Cry Instincts Predator", L"Video options - Far Cry Instincts Predator"),
                                 estilo, CW_USEDEFAULT, CW_USEDEFAULT, area.right - area.left, area.bottom - area.top,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!j) { Log("[video] a janela de opcoes de video nao abriu (erro %lu)", GetLastError()); return; }
        Log("[video] janela de opcoes de video aberta (F2)");
        g_janela = j;
        AplicarIcone(j);
        tema::TituloEscuro(j);
        RECT r; GetWindowRect(j, &r);
        const int w = r.right - r.left, h = r.bottom - r.top;
        SetWindowPos(j, nullptr, (GetSystemMetrics(SM_CXSCREEN) - w) / 2, std::max(0, (GetSystemMetrics(SM_CYSCREEN) - h) / 2), 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER);
        ShowWindow(j, SW_SHOW);
        SetForegroundWindow(j);
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0)
        {
            if (IsDialogMessageW(j, &m)) continue;   // Tab entre os campos, Esc = fechar
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}

void VideoAbrirJanela()
{
    std::lock_guard lock(g_abrir);
    if (HWND j = g_janela) { ShowWindow(j, SW_RESTORE); SetForegroundWindow(j); return; }
    std::thread(Rodar).detach();
}

// ------------------------------------------------------------------ janela das trapacas (botao TRAPACAS da janela F2)
namespace
{
    namespace trap
    {
        enum : int { ID_CAIXA = 200, ID_DESC = 210, ID_AVISO = 220, ID_FECHAR_T = 221 };   // 200..205 caixas, 210..215 descricoes
        constexpr int LARG = 780, Y0T = 44, PASSO_T = 66;
        std::atomic<HWND> g_janelaT{ nullptr };
        bool g_marca[6] = {};

        bool* Opcao(int i)
        {
            Opcoes& o = OpcoesAtuais();
            bool* v[] = { &o.trapacaMunicao, &o.trapacaAdrenalina, &o.trapacaFeral, &o.trapacaFases, &o.trapacaMapas, &o.trapacaInvencivel };
            return v[i];
        }

        HWND CriarT(HWND pai, const wchar_t* classe, const wchar_t* texto, DWORD estilo, int x, int y, int w, int h, int id)
        {
            if (!wcscmp(classe, L"BUTTON")) estilo |= BS_OWNERDRAW;
            HWND c = CreateWindowExW(0, classe, texto, WS_CHILD | WS_VISIBLE | estilo, S(x), S(y), S(w), S(h), pai,
                                     (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g_fonte, TRUE);
            if (id == ID_FECHAR_T) tema::AcompanharMouse(c);
            return c;
        }

        void Montar(HWND j)
        {
            // os mesmos textos da aba TRAPACAS do launcher (as traducoes ja existem)
            struct { const wchar_t* nome; const wchar_t* desc; } t[] = {
                { TR(L"Munição infinita (código UnleashHell)", L"Infinite ammo (code UnleashHell)"),
                  TR(L"As armas não gastam munição: atire à vontade sem recarregar o estoque.", L"Weapons use no ammo: shoot as much as you like without running out.") },
                { TR(L"Adrenalina infinita (código Bloodlust)", L"Infinite adrenaline (code Bloodlust)"),
                  TR(L"A barra de adrenalina fica sempre cheia para usar os poderes ferais.", L"The adrenaline bar is always full for the feral abilities.") },
                { TR(L"Todos os poderes ferais (código FeralAttack)", L"All feral abilities (code FeralAttack)"),
                  TR(L"Libera os poderes ferais (visão, faro, velocidade, ataque) desde o começo.", L"Unlocks the feral abilities (vision, smell, speed, attack) from the start.") },
                { TR(L"Todas as fases (código GiveMeItAll)", L"All levels (code GiveMeItAll)"),
                  TR(L"Libera todas as fases do modo história no perfil. Fica gravado quando o jogo salvar o perfil.", L"Unlocks every story level in the profile. It stays saved once the game saves the profile.") },
                { TR(L"Todos os mapas (código GiveMeTheMaps)", L"All maps (code GiveMeTheMaps)"),
                  TR(L"Libera todos os mapas no perfil. Também fica gravado quando o jogo salvar o perfil.", L"Unlocks every map in the profile. It also stays saved once the game saves the profile.") },
                { TR(L"Invencível (novo, não existe no jogo original)", L"Invincible (new, not in the original game)"),
                  TR(L"A vida volta ao máximo a cada quadro, como o código ImJackCarver o tempo todo.", L"Health goes back to full every frame, like the code ImJackCarver all the time.") } };
            for (int i = 0; i < 6; i++)
            {
                g_marca[i] = *Opcao(i);
                CriarT(j, L"BUTTON", t[i].nome, WS_TABSTOP, 16, Y0T + i * PASSO_T, LARG - 32, 24, ID_CAIXA + i);
                CriarT(j, L"STATIC", t[i].desc, SS_LEFT, 48, Y0T + i * PASSO_T + 26, LARG - 64, 38, ID_DESC + i);
            }
            const int y = Y0T + 6 * PASSO_T;
            CriarT(j, L"STATIC", TR(L"As trapaças valem só no modo história (o jogo não deixa no multijogador). Um save feito com"
                                    L" munição, adrenalina ou poderes ferais ligados guarda essas trapaças, como no jogo original.",
                                    L"Cheats work only in the story mode (the game does not allow them in multiplayer). A save made with"
                                    L" ammo, adrenaline or feral abilities on keeps those cheats, like in the original game."),
                   SS_LEFT, 16, y + 4, LARG - 32, 52, ID_AVISO);
            CriarT(j, L"BUTTON", TR(L"FECHAR", L"CLOSE"), WS_TABSTOP, LARG - 16 - 140, y + 64, 140, 36, ID_FECHAR_T);
        }

        LRESULT CALLBACK Proc(HWND j, UINT msg, WPARAM wp, LPARAM lp)
        {
            switch (msg)
            {
            case WM_CREATE: Montar(j); return 0;
            case WM_COMMAND:
            {
                const int id = LOWORD(wp);
                if (id >= ID_CAIXA && id < ID_CAIXA + 6 && (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == BN_DOUBLECLICKED))
                {
                    const int i = id - ID_CAIXA;
                    g_marca[i] = !g_marca[i];
                    *Opcao(i) = g_marca[i];
                    InvalidateRect((HWND)lp, nullptr, FALSE);
                    Log("[video] janela de trapacas: trapaca %d = %d", i, g_marca[i] ? 1 : 0);
                    Gravar();
                    return 0;
                }
                if (id == ID_FECHAR_T || id == IDCANCEL || id == IDOK) { DestroyWindow(j); return 0; }
                break;
            }
            case WM_CTLCOLORSTATIC:
            {
                const int id = GetDlgCtrlID((HWND)lp);
                return tema::CorTexto((HDC)wp, id >= ID_DESC && id < ID_DESC + 6 ? tema::TEXTO_FRACO : tema::TEXTO);
            }
            case WM_DRAWITEM:
            {
                auto* d = (const DRAWITEMSTRUCT*)lp;
                if (d->CtlID >= ID_CAIXA && d->CtlID < ID_CAIXA + 6) tema::DesenharCaixa(d, g_marca[d->CtlID - ID_CAIXA], g_fonte);
                else tema::DesenharBotao(d, false, tema::FonteBotao());
                return TRUE;
            }
            case WM_LBUTTONDOWN: SetFocus(j); return 0;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(j, &ps);
                RECT r; GetClientRect(j, &r);
                HDC mem = CreateCompatibleDC(dc);
                HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
                HGDIOBJ antes = SelectObject(mem, bmp);
                FillRect(mem, &r, tema::PincelFundo());
                tema::Secao(mem, S(16), S(12), r.right - S(16), TR(L"TRAPAÇAS", L"CHEATS"));
                BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
                SelectObject(mem, antes);
                DeleteObject(bmp); DeleteDC(mem);
                EndPaint(j, &ps);
                return 0;
            }
            case WM_CLOSE: DestroyWindow(j); return 0;
            case WM_DESTROY:
                g_janelaT = nullptr;
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcW(j, msg, wp, lp);
        }

        void Rodar()
        {
            static bool registrada = false;
            if (!registrada)
            {
                WNDCLASSW wc{};
                wc.lpfnWndProc = Proc;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
                wc.hbrBackground = tema::PincelFundo();
                wc.lpszClassName = L"PredatorTrapacas";
                RegisterClassW(&wc);
                registrada = true;
            }
            RECT area{ 0, 0, S(LARG), S(Y0T + 6 * PASSO_T + 64 + 36 + 16) };
            const DWORD estilo = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
            AdjustWindowRect(&area, estilo, FALSE);
            HWND j = CreateWindowExW(WS_EX_TOPMOST, L"PredatorTrapacas", TR(L"Trapaças - Far Cry Instincts Predator", L"Cheats - Far Cry Instincts Predator"),
                                     estilo, CW_USEDEFAULT, CW_USEDEFAULT, area.right - area.left, area.bottom - area.top,
                                     nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!j) { Log("[video] a janela de trapacas nao abriu (erro %lu)", GetLastError()); return; }
            Log("[video] janela de trapacas aberta");
            g_janelaT = j;
            AplicarIcone(j);
            tema::TituloEscuro(j);
            RECT r; GetWindowRect(j, &r);
            const int w = r.right - r.left, h = r.bottom - r.top;
            SetWindowPos(j, nullptr, (GetSystemMetrics(SM_CXSCREEN) - w) / 2 + S(40), std::max(0, (GetSystemMetrics(SM_CYSCREEN) - h) / 2 + S(40)), 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER);
            ShowWindow(j, SW_SHOW);
            SetForegroundWindow(j);
            MSG m;
            while (GetMessageW(&m, nullptr, 0, 0) > 0)
            {
                if (IsDialogMessageW(j, &m)) continue;
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
    }
}

void TrapacasAbrirJanela()
{
    std::lock_guard lock(g_abrir);
    if (HWND j = trap::g_janelaT) { ShowWindow(j, SW_RESTORE); SetForegroundWindow(j); return; }
    std::thread(trap::Rodar).detach();
}
