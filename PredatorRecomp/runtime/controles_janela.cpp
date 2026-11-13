// Janela de configuracao dos controles (F1 no jogo, ou "PredatorGame.exe --controles" / "Configurar controles.bat").
// Roda na sua propria thread com o seu laco de mensagens (nao trava o jogo). Mostra, para os jogadores 1 a 4, qual
// dispositivo usar e os botoes apertados agora (para testar), as opcoes do mouse e as teclas de cada botao do controle.
// "Salvar" aplica na hora e grava em controles.ini. Mesmo visual do launcher (tema.h: cores do menu do jogo).
#include "idioma.h"
#include "controles.h"
#include "guest.h"
#include "perf.h"
#include "opcoes.h"
#include "tema.h"
#include <commctrl.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")

namespace
{
    enum : int
    {
        ID_JOG = 100,        // 100-103: dispositivo dos jogadores 1 a 4
        ID_ESTADO = 110,     // 110-113: o que o jogador esta apertando agora
        ID_JUNTAR = 104,
        ID_MOUSECAM = 120, ID_INVERTER = 121, ID_SENS = 122, ID_PADDIRETO = 123, ID_PADVEL = 124, ID_PADZONA = 125, ID_PADACEL = 126, ID_PADSUAVE = 127,
        ID_PADVELV = 128, ID_PADMIRA = 129,
        ID_TECLA = 200,      // 200 + 2*acao (+1): teclas 1 e 2 de cada botao
        ID_SALVAR = 300, ID_CANCELAR = 301, ID_PADRAO = 302,
        TIMER_ESTADO = 1,
        WM_CRIAR_TECLAS = WM_APP + 1,   // monta as listas de teclas depois que a janela aparece
    };

    std::atomic<HWND> g_janela{ nullptr };
    std::mutex g_abrir;
    HFONT g_fonte = nullptr;
    // caixas de marcar desenhadas pelo tema (estado guardado aqui, indice = id)
    bool g_marca[128];
    bool Caixa(int id) { return id == ID_JUNTAR || id == ID_MOUSECAM || id == ID_INVERTER || id == ID_PADDIRETO; }
    bool Botao(int id) { return id == ID_SALVAR || id == ID_CANCELAR || id == ID_PADRAO; }
    void Marcar(HWND j, int id, bool v) { g_marca[id] = v; InvalidateRect(GetDlgItem(j, id), nullptr, FALSE); }
    ControlesCfg g_cfg;
    std::vector<std::string> g_ids[4];   // ids dos itens das listas de dispositivos
    std::string g_escolhidoAntes[4];     // dispositivo de cada jogador antes da ultima troca (regra: nada repetido)
    std::vector<int> g_teclas;           // codigos dos itens das listas de teclas (0 = nenhuma)
    std::vector<std::wstring> g_nomesTeclas;   // nomes dos itens (montados uma vez)
    // As ~40 listas de teclas comecam so com a tecla escolhida e recebem a lista inteira quando ganham o foco (clique
    // ou Tab): encher todas ao abrir (~120 itens cada) levava ~0,75 s e a janela demorava a aparecer.
    // g_teclaLista[id] = tecla escolhida enquanto a lista ainda nao foi enchida.
    std::vector<int> g_teclaLista;
    std::vector<bool> g_listaCheia;
    ControlesCfg g_cfgTeclas;   // configuracao mostrada (as listas de teclas sao criadas depois, com ela)
    int IndiceTecla(int vk) { auto it = std::find(g_teclas.begin(), g_teclas.end(), vk); return it == g_teclas.end() ? 0 : (int)(it - g_teclas.begin()); }

    void MostrarTecla(HWND j, int idx, int vk)
    {
        HWND cb = GetDlgItem(j, ID_TECLA + idx);
        if (g_listaCheia[idx]) { SendMessageW(cb, CB_SETCURSEL, IndiceTecla(vk), 0); return; }
        g_teclaLista[idx] = g_teclas[IndiceTecla(vk)];
        SendMessageW(cb, CB_RESETCONTENT, 0, 0);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)g_nomesTeclas[IndiceTecla(vk)].c_str());
        SendMessageW(cb, CB_SETCURSEL, 0, 0);
    }

    void EncherLista(HWND j, int idx)
    {
        if (g_listaCheia[idx]) return;
        HWND cb = GetDlgItem(j, ID_TECLA + idx);
        SendMessageW(cb, WM_SETREDRAW, FALSE, 0);
        SendMessageW(cb, CB_RESETCONTENT, 0, 0);
        SendMessageW(cb, CB_INITSTORAGE, g_nomesTeclas.size(), 32 * sizeof(wchar_t));
        for (auto& n : g_nomesTeclas) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)n.c_str());
        SendMessageW(cb, CB_SETCURSEL, IndiceTecla(g_teclaLista[idx]), 0);
        SendMessageW(cb, WM_SETREDRAW, TRUE, 0);
        g_listaCheia[idx] = true;
    }

    int TeclaDaLista(HWND j, int idx)
    {
        if (!g_listaCheia[idx]) return g_teclaLista[idx];
        int i = (int)SendDlgItemMessageW(j, ID_TECLA + idx, CB_GETCURSEL, 0, 0);
        return i > 0 && i < (int)g_teclas.size() ? g_teclas[i] : 0;
    }
    float g_escala = 1.0f;

    int S(int v) { return (int)(v * g_escala + 0.5f); }

    std::wstring W(const std::string& s)
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n ? n - 1 : 0, L'\0');
        if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    }

    HWND Criar(HWND pai, const wchar_t* classe, const wchar_t* texto, DWORD estilo, int x, int y, int w, int h, int id)
    {
        if (Caixa(id) || Botao(id)) estilo = (estilo & ~(BS_AUTOCHECKBOX | BS_DEFPUSHBUTTON)) | BS_OWNERDRAW;
        HWND c = CreateWindowExW(0, classe, texto, WS_CHILD | WS_VISIBLE | estilo, S(x), S(y), S(w), S(h), pai,
                                 (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_fonte, TRUE);
        if (!wcscmp(classe, WC_COMBOBOXW)) tema::ListaEscura(c);
        if (Botao(id)) tema::AcompanharMouse(c);
        return c;
    }

    std::string Escolhido(HWND j, int jog);
    // lista de dispositivos: teclado, auto, os controles ligados agora (DualSense, Xbox... pelo nome), o escolhido (mesmo
    // desligado) e nenhum. As vagas "controle1".."controle4" sairam da lista (pedido do usuario) mas continuam valendo no ini.
    void PreencherDispositivos(HWND j, int jog, const std::string& escolhido)
    {
        HWND cb = GetDlgItem(j, ID_JOG + jog);
        SendMessageW(cb, CB_RESETCONTENT, 0, 0);
        auto& ids = g_ids[jog];
        ids = { "teclado", "auto" };
        for (auto& p : PadsLigados()) ids.push_back(p.id);
        bool tem = std::find(ids.begin(), ids.end(), escolhido) != ids.end() || escolhido == "nenhum";
        if (!tem && !escolhido.empty()) ids.push_back(escolhido);
        ids.push_back("nenhum");
        int sel = 0;
        for (size_t i = 0; i < ids.size(); i++)
        {
            std::string nome = PadNome(ids[i]);
            if (!tem && ids[i] == escolhido && escolhido.rfind("controle", 0) != 0) nome += TR(" - desligado agora", " - disconnected now");
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)W(nome).c_str());
            if (ids[i] == escolhido) sel = (int)i;
        }
        SendMessageW(cb, CB_SETCURSEL, sel, 0);
        g_escolhidoAntes[jog] = ids[sel];
    }

    // Regra: o mesmo dispositivo (ex.: o teclado) nao fica em dois jogadores. Escolher no jogador X o que outro usa
    // troca: o outro fica com o que X tinha antes (ou um controle livre).
    void TrocouDispositivo(HWND j, int jog)
    {
        std::string novo = Escolhido(j, jog), antes = g_escolhidoAntes[jog];
        for (int outro = 0; outro < 4; outro++)
            if (outro != jog && novo != "nenhum" && novo == Escolhido(j, outro))
            {
                // o que X tinha antes; senao o "auto" ou um controle ligado que ninguem usa; senao nenhum
                std::string dar = antes != novo ? antes : "nenhum";
                if (dar == "nenhum")
                {
                    std::vector<std::string> cand{ "auto" };
                    for (auto& p : PadsLigados()) cand.push_back(p.id);
                    for (auto& c : cand)
                    {
                        bool usado = false;
                        for (int k = 0; k < 4; k++) if (Escolhido(j, k) == c) usado = true;
                        if (!usado) { dar = c; break; }
                    }
                }
                PreencherDispositivos(j, outro, dar);
            }
        g_escolhidoAntes[jog] = novo;
    }

    std::string Escolhido(HWND j, int jog)
    {
        int i = (int)SendMessageW(GetDlgItem(j, ID_JOG + jog), CB_GETCURSEL, 0, 0);
        return i >= 0 && i < (int)g_ids[jog].size() ? g_ids[jog][i] : "nenhum";
    }

    void MostrarConfig(HWND j, const ControlesCfg& c)
    {
        for (int jog = 0; jog < 4; jog++) PreencherDispositivos(j, jog, c.jogador[jog]);
        Marcar(j, ID_JUNTAR, c.juntar);
        Marcar(j, ID_MOUSECAM, c.mouseCamera);
        Marcar(j, ID_INVERTER, c.inverterY);
        wchar_t b[32]; swprintf(b, 32, L"%.2f", c.sensibilidade);
        SetDlgItemTextW(j, ID_SENS, b);
        Marcar(j, ID_PADDIRETO, c.cameraDireta);
        swprintf(b, 32, L"%.2f", c.velocidadeCamera);
        SetDlgItemTextW(j, ID_PADVEL, b);
        swprintf(b, 32, L"%.2f", c.zonaMorta); SetDlgItemTextW(j, ID_PADZONA, b);
        swprintf(b, 32, L"%.2f", c.aceleracao); SetDlgItemTextW(j, ID_PADACEL, b);
        swprintf(b, 32, L"%.2f", c.suavizacao); SetDlgItemTextW(j, ID_PADSUAVE, b);
        swprintf(b, 32, L"%.2f", c.velocidadeVertical); SetDlgItemTextW(j, ID_PADVELV, b);
        swprintf(b, 32, L"%.2f", c.sensMirando); SetDlgItemTextW(j, ID_PADMIRA, b);
        g_cfgTeclas = c;
        for (size_t a = 0; a < c.acoes.size() && a * 2 + 1 < g_teclaLista.size(); a++)   // listas ja criadas
            for (int k = 0; k < 2; k++)
            {
                int vk = k < (int)c.acoes[a].teclas.size() ? c.acoes[a].teclas[k] : 0;
                MostrarTecla(j, (int)a * 2 + k, vk);
            }
    }

    ControlesCfg LerConfig(HWND j)
    {
        ControlesCfg c = g_cfg;
        for (int jog = 0; jog < 4; jog++) c.jogador[jog] = Escolhido(j, jog);
        c.juntar = g_marca[ID_JUNTAR];
        c.mouseCamera = g_marca[ID_MOUSECAM];
        c.inverterY = g_marca[ID_INVERTER];
        wchar_t b[32] = {}; GetDlgItemTextW(j, ID_SENS, b, 32);
        for (auto& ch : b) if (ch == L',') ch = L'.';
        float s = (float)_wtof(b);
        c.sensibilidade = s > 0 ? std::clamp(s, 0.1f, 10.0f) : g_cfg.sensibilidade;
        c.cameraDireta = g_marca[ID_PADDIRETO];
        GetDlgItemTextW(j, ID_PADVEL, b, 32);
        for (auto& ch : b) if (ch == L',') ch = L'.';
        float v = (float)_wtof(b);
        c.velocidadeCamera = v > 0 ? std::clamp(v, 0.2f, 5.0f) : g_cfg.velocidadeCamera;
        // campo vazio ou invalido = mantem o valor anterior; "0" vale
        auto numero = [&](int id, float atual, float lo, float hi) {
            wchar_t t[32] = {}; GetDlgItemTextW(j, id, t, 32);
            for (auto& ch : t) if (ch == L',') ch = L'.';
            wchar_t* fim = nullptr; float n = (float)wcstod(t, &fim);
            return fim == t ? atual : std::clamp(n, lo, hi);
        };
        c.zonaMorta = numero(ID_PADZONA, g_cfg.zonaMorta, 0.0f, 0.5f);
        c.aceleracao = numero(ID_PADACEL, g_cfg.aceleracao, 0.0f, 2.0f);
        c.suavizacao = numero(ID_PADSUAVE, g_cfg.suavizacao, 0.0f, 0.9f);
        c.velocidadeVertical = numero(ID_PADVELV, g_cfg.velocidadeVertical, 0.2f, 5.0f);
        c.sensMirando = numero(ID_PADMIRA, g_cfg.sensMirando, 0.1f, 2.0f);
        for (size_t a = 0; a < c.acoes.size() && a * 2 + 1 < g_teclaLista.size(); a++)   // listas ainda nao criadas: mantem
        {
            std::vector<int> t;
            for (int k = 0; k < 2; k++)
            {
                int vk = TeclaDaLista(j, (int)a * 2 + k);
                if (vk && std::find(t.begin(), t.end(), vk) == t.end()) t.push_back(vk);
            }
            c.acoes[a].teclas = t;
        }
        return c;
    }

    // "Agora: A, LB, analogico esquerdo" do dispositivo escolhido (para testar sem abrir o jogo)
    std::wstring Estado(const std::string& id)
    {
        if (id == "teclado") return TR(L"Teclado e mouse: as teclas estão na lista abaixo", L"Keyboard and mouse: the keys are in the list below");
        if (id == "nenhum") return TR(L"Sem controle", L"No controller");
        XINPUT_GAMEPAD g{};
        if (!PadLer(id, g))
            return id == "auto" || id == "controle1" ? TR(L"Nenhum controle ligado agora", L"No controller connected now")
                 : id == "controle2" ? TR(L"Não há um 2º controle ligado agora", L"No 2nd controller connected now")
                                     : TR(L"Desligado agora", L"Disconnected now");
        std::wstring s = TR(L"Ligado: ", L"Connected: ") + W(PadNome(PadResolver(id))) + TR(L"  |  apertando:", L"  |  pressing:");
        struct { WORD bit; const wchar_t* nome; } bot[] = {
            { XINPUT_GAMEPAD_A, L" A" }, { XINPUT_GAMEPAD_B, L" B" }, { XINPUT_GAMEPAD_X, L" X" }, { XINPUT_GAMEPAD_Y, L" Y" },
            { XINPUT_GAMEPAD_LEFT_SHOULDER, L" LB" }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, L" RB" },
            { XINPUT_GAMEPAD_START, L" START" }, { XINPUT_GAMEPAD_BACK, L" BACK" },
            { XINPUT_GAMEPAD_LEFT_THUMB, L" L3" }, { XINPUT_GAMEPAD_RIGHT_THUMB, L" R3" },
            { XINPUT_GAMEPAD_DPAD_UP, TR(L" cima", L" up") }, { XINPUT_GAMEPAD_DPAD_DOWN, TR(L" baixo", L" down") },
            { XINPUT_GAMEPAD_DPAD_LEFT, TR(L" esquerda", L" left") }, { XINPUT_GAMEPAD_DPAD_RIGHT, TR(L" direita", L" right") } };
        size_t antes = s.size();
        for (auto& b : bot) if (g.wButtons & b.bit) s += b.nome;
        if (g.bLeftTrigger > 30) s += L" LT";
        if (g.bRightTrigger > 30) s += L" RT";
        if (std::abs(g.sThumbLX) > 8000 || std::abs(g.sThumbLY) > 8000) s += TR(L" analógico-esq", L" left-stick");
        if (std::abs(g.sThumbRX) > 8000 || std::abs(g.sThumbRY) > 8000) s += TR(L" analógico-dir", L" right-stick");
        if (s.size() == antes) s += TR(L" nada", L" nothing");
        return s;
    }

    LRESULT CALLBACK Proc(HWND j, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_CREATE:
        {
            double t0 = perf::Now();
            g_cfg = InputConfigAtual();
            const wchar_t* rotulo[4] = { TR(L"Jogador 1:", L"Player 1:"), TR(L"Jogador 2:", L"Player 2:"),
                                         TR(L"Jogador 3:", L"Player 3:"), TR(L"Jogador 4:", L"Player 4:") };
            for (int jog = 0; jog < 4; jog++)   // 1 e 2 a esquerda, 3 e 4 a direita (tela dividida do jogo: ate 4)
            {
                int x = 16 + (jog / 2) * 540, y = 42 + (jog % 2) * 58;
                Criar(j, L"STATIC", rotulo[jog], 0, x, y + 4, 80, 20, -1);
                Criar(j, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x + 84, y, 360, 300, ID_JOG + jog);
                Criar(j, L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, x + 84, y + 28, 440, 20, ID_ESTADO + jog);
            }
            Criar(j, L"BUTTON", TR(L"Juntar: o dispositivo do jogador 2 também controla o jogador 1 (desligado = jogador 2 separado, para tela dividida)",
                                   L"Merge: player 2's device also controls player 1 (off = separate player 2, for split screen)"),
                  BS_AUTOCHECKBOX | WS_TABSTOP, 16, 160, 970, 22, ID_JUNTAR);

            Criar(j, L"BUTTON", TR(L"Mouse move a câmera", L"Mouse moves the camera"), BS_AUTOCHECKBOX | WS_TABSTOP, 16, 224, 200, 22, ID_MOUSECAM);
            Criar(j, L"BUTTON", TR(L"Inverter o eixo vertical", L"Invert vertical axis"), BS_AUTOCHECKBOX | WS_TABSTOP, 230, 224, 200, 22, ID_INVERTER);
            Criar(j, L"STATIC", TR(L"Sensibilidade (0,1 a 10):", L"Sensitivity (0.1 to 10):"), 0, 450, 227, 240, 20, -1);
            Criar(j, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 690, 224, 60, 22, ID_SENS);

            Criar(j, L"BUTTON", TR(L"Câmera precisa no analógico direito (como o mouse)", L"Precise camera on the right stick (like the mouse)"), BS_AUTOCHECKBOX | WS_TABSTOP, 16, 288, 420, 22, ID_PADDIRETO);
            Criar(j, L"STATIC", TR(L"Sensibilidade do controle (0,2 a 5):", L"Controller sensitivity (0.2 to 5):"), 0, 450, 291, 240, 20, -1);
            Criar(j, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 690, 288, 60, 22, ID_PADVEL);
            Criar(j, L"STATIC", TR(L"Zona morta (0 a 0,5):", L"Dead zone (0 to 0.5):"), 0, 36, 321, 150, 20, -1);
            Criar(j, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 186, 318, 60, 22, ID_PADZONA);
            Criar(j, L"STATIC", TR(L"Aceleração no fim do curso (0 a 2):", L"Turn boost at full tilt (0 to 2):"), 0, 270, 321, 230, 20, -1);
            Criar(j, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 500, 318, 60, 22, ID_PADACEL);
            Criar(j, L"STATIC", TR(L"Suavização (0 = na hora, até 0,9):", L"Smoothing (0 = instant, up to 0.9):"), 0, 584, 321, 230, 20, -1);
            Criar(j, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 814, 318, 60, 22, ID_PADSUAVE);
            Criar(j, L"STATIC", TR(L"Sensibilidade vertical (0,2 a 5):", L"Vertical sensitivity (0.2 to 5):"), 0, 36, 351, 210, 20, -1);
            Criar(j, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 246, 348, 60, 22, ID_PADVELV);
            Criar(j, L"STATIC", TR(L"Ao mirar com o gatilho esquerdo (x 0,1 a 2):", L"While aiming with the left trigger (x 0.1 to 2):"), 0, 330, 351, 300, 20, -1);
            Criar(j, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 630, 348, 60, 22, ID_PADMIRA);
            // As ~40 listas de teclas levam ~0,4 s para criar (~7 ms cada lista do Windows): sao criadas logo depois que a
            // janela aparece (WM_CRIAR_TECLAS), para ela abrir na hora
            g_teclaLista.clear();
            g_listaCheia.clear();
            size_t metade = (g_cfg.acoes.size() + 1) / 2;
            int yb = 416 + (int)metade * 30 + 14;
            Criar(j, L"BUTTON", TR(L"VOLTAR AO PADRÃO", L"RESTORE DEFAULTS"), WS_TABSTOP, 16, yb, 250, 36, ID_PADRAO);
            Criar(j, L"BUTTON", TR(L"CANCELAR", L"CANCEL"), WS_TABSTOP, 788, yb, 136, 36, ID_CANCELAR);
            Criar(j, L"BUTTON", TR(L"SALVAR", L"SAVE"), WS_TABSTOP, 934, yb, 140, 36, ID_SALVAR);
            MostrarConfig(j, g_cfg);
            SetTimer(j, TIMER_ESTADO, 100, nullptr);
            RECT r{ 0, 0, S(1100), S(yb + 52) };
            AdjustWindowRect(&r, GetWindowLongW(j, GWL_STYLE), FALSE);
            SetWindowPos(j, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
            PostMessageW(j, WM_CRIAR_TECLAS, 0, 0);
            Log("[controles] janela montada em %.0f ms", perf::Now() - t0);
            return 0;
        }
        case WM_CRIAR_TECLAS:
        {
            double t0 = perf::Now();
            g_teclas = { 0 };
            for (int vk : InputTodasTeclas()) g_teclas.push_back(vk);
            g_nomesTeclas.clear();
            for (int vk : g_teclas) g_nomesTeclas.push_back(vk ? W(InputNomeTecla(vk)) : std::wstring(TR(L"(nenhuma)", L"(none)")));
            size_t n = g_cfg.acoes.size(), metade = (n + 1) / 2;
            SendMessageW(j, WM_SETREDRAW, FALSE, 0);   // aparece tudo de uma vez no fim
            // ordem do Tab = ordem das janelas: as listas entram depois dos campos de cima, antes dos botoes de baixo
            HWND anterior = GetDlgItem(j, ID_PADMIRA);
            auto naOrdem = [&](HWND c) { SetWindowPos(c, anterior, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); anterior = c; };
            for (size_t a = 0; a < n; a++)
            {
                int col = a < metade ? 0 : 1, lin = (int)(a < metade ? a : a - metade);
                int x = 16 + col * 540, y = 416 + lin * 30;
                std::wstring nome = W(g_cfg.acoes[a].ajuda);
                if (!nome.empty()) nome[0] = towupper(nome[0]);
                naOrdem(Criar(j, L"STATIC", nome.c_str(), SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, x, y + 4, 250, 20, -1));
                for (int k = 0; k < 2; k++)
                    naOrdem(Criar(j, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x + 254 + k * 136, y, 132, 400,
                                  ID_TECLA + (int)a * 2 + k));   // itens: MostrarTecla / EncherLista
            }
            g_teclaLista.assign(n * 2, 0);
            g_listaCheia.assign(n * 2, false);
            MostrarConfig(j, g_cfgTeclas);
            SendMessageW(j, WM_SETREDRAW, TRUE, 0);
            RedrawWindow(j, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
            Log("[controles] listas de teclas criadas em %.0f ms (com a janela ja na tela)", perf::Now() - t0);
            return 0;
        }
        case WM_TIMER:
        {
            // estado ao vivo; a cada ~2 s refaz as listas se um controle foi ligado/desligado
            for (int jog = 0; jog < 4; jog++)
            {
                std::wstring s = Estado(Escolhido(j, jog));
                wchar_t atual[512] = {}; GetDlgItemTextW(j, ID_ESTADO + jog, atual, 512);
                if (s != atual) SetDlgItemTextW(j, ID_ESTADO + jog, s.c_str());
            }
            static int voltas = 0;
            if (++voltas % 20 == 0)
            {
                std::vector<std::string> ligados;
                for (auto& p : PadsLigados()) ligados.push_back(p.id);
                static std::vector<std::string> antes;
                if (ligados != antes)
                {
                    antes = ligados;
                    for (int jog = 0; jog < 4; jog++)
                        if (!SendMessageW(GetDlgItem(j, ID_JOG + jog), CB_GETDROPPEDSTATE, 0, 0))
                            PreencherDispositivos(j, jog, Escolhido(j, jog));
                }
            }
            return 0;
        }
        case WM_COMMAND:
            if (int id = LOWORD(wp); Caixa(id) && (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == BN_DOUBLECLICKED))
            {
                Marcar(j, id, !g_marca[id]);
                return 0;
            }
            if (int id = LOWORD(wp); id >= ID_TECLA && id < ID_TECLA + (int)g_listaCheia.size() &&
                (HIWORD(wp) == CBN_SETFOCUS || HIWORD(wp) == CBN_DROPDOWN))
            {
                EncherLista(j, id - ID_TECLA);
                return 0;
            }
            if (int id = LOWORD(wp); id >= ID_JOG && id < ID_JOG + 4 && HIWORD(wp) == CBN_SELCHANGE)
            {
                TrocouDispositivo(j, id - ID_JOG);
                return 0;
            }
            switch (LOWORD(wp))
            {
            case ID_SALVAR: case IDOK: g_cfg = LerConfig(j); InputAplicarConfig(g_cfg); DestroyWindow(j); return 0;
            case ID_CANCELAR: case IDCANCEL: DestroyWindow(j); return 0;   // Esc = cancelar
            case ID_PADRAO: MostrarConfig(j, InputConfigPadrao()); return 0;
            }
            break;
        case WM_CTLCOLORSTATIC:
        {
            int id = GetDlgCtrlID((HWND)lp);
            return tema::CorTexto((HDC)wp, id >= ID_ESTADO && id < ID_ESTADO + 4 ? tema::TEXTO_FRACO : tema::TEXTO);
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return tema::CorCampo((HDC)wp);
        case WM_DRAWITEM:
        {
            auto* d = (const DRAWITEMSTRUCT*)lp;
            if (Caixa((int)d->CtlID)) tema::DesenharCaixa(d, g_marca[d->CtlID], g_fonte);
            else tema::DesenharBotao(d, d->CtlID == ID_SALVAR, tema::FonteBotao());
            return TRUE;
        }
        case WM_LBUTTONDOWN: SetFocus(j); return 0;   // clique fora das caixas: tira a selecao (azul) da caixa com foco
        case WM_ERASEBKGND: return 1;   // o WM_PAINT pinta tudo (sem piscar)
        case WM_PAINT:
        {
            // fundo e titulos das secoes (com a linha fina dos paineis do menu)
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(j, &ps);
            RECT r; GetClientRect(j, &r);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
            HGDIOBJ antes = SelectObject(mem, bmp);
            FillRect(mem, &r, tema::PincelFundo());
            tema::Secao(mem, S(16), S(12), r.right - S(16), TR(L"JOGADORES", L"PLAYERS"));
            tema::Secao(mem, S(16), S(196), r.right - S(16), TR(L"MOUSE", L"MOUSE"));
            tema::Secao(mem, S(16), S(260), r.right - S(16), TR(L"CONTROLE (ANALÓGICO DIREITO)", L"CONTROLLER (RIGHT STICK)"));
            tema::Secao(mem, S(16), S(386), r.right - S(16), TR(L"TECLAS DO TECLADO E MOUSE  •  ATÉ 2 POR BOTÃO DO CONTROLE", L"KEYBOARD AND MOUSE KEYS  •  UP TO 2 PER CONTROLLER BUTTON"));
            BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, antes);
            DeleteObject(bmp); DeleteDC(mem);
            EndPaint(j, &ps);
            return 0;
        }
        case WM_CLOSE: DestroyWindow(j); return 0;
        case WM_DESTROY: KillTimer(j, TIMER_ESTADO); g_janela = nullptr; PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(j, msg, wp, lp);
    }

    void Rodar()
    {
        double t0 = perf::Now();
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icc);
        HDC dc = GetDC(nullptr);
        g_escala = GetDeviceCaps(dc, LOGPIXELSY) / 96.0f;
        ReleaseDC(nullptr, dc);
        tema::Iniciar(g_escala);
        if (!g_fonte)
        {
            g_fonte = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        }
        static bool registrada = false;
        if (!registrada)
        {
            WNDCLASSW wc{};
            wc.lpfnWndProc = Proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // seta (IDC_ARROW)
            wc.hbrBackground = tema::PincelFundo();
            wc.lpszClassName = L"PredatorControles";
            RegisterClassW(&wc);
            registrada = true;
        }
        HWND j = CreateWindowExW(WS_EX_TOPMOST, L"PredatorControles", TR(L"Configurar controles - Far Cry Instincts Predator", L"Controller settings - Far Cry Instincts Predator"),
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                 S(1090), S(700), nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!j) { Log("[controles] a janela de configuracao nao abriu (erro %lu)", GetLastError()); return; }
        Log("[controles] janela de configuracao aberta");
        g_janela = j;
        AplicarIcone(j);
        tema::TituloEscuro(j);
        // no centro da tela
        RECT r; GetWindowRect(j, &r);
        int w = r.right - r.left, h = r.bottom - r.top;
        SetWindowPos(j, nullptr, (GetSystemMetrics(SM_CXSCREEN) - w) / 2, std::max(0, (GetSystemMetrics(SM_CYSCREEN) - h) / 2), 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER);
        ShowWindow(j, SW_SHOW);
        SetForegroundWindow(j);
        Log("[controles] janela na tela %.0f ms depois do pedido", perf::Now() - t0);
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0)
        {
            if (IsDialogMessageW(j, &m)) continue;   // Tab entre os campos, Enter = Salvar
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}

void ControlesAbrirJanela(bool esperarFechar)
{
    std::lock_guard lock(g_abrir);
    if (HWND j = g_janela) { ShowWindow(j, SW_RESTORE); SetForegroundWindow(j); return; }
    PadsIniciar();
    std::thread t(Rodar);
    if (esperarFechar) t.join(); else t.detach();
}
