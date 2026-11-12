// Launcher: janela que abre antes do jogo (dois cliques no PredatorGame.exe). Escolhe a pasta da copia do jogo,
// opcoes de video, abre a janela de controles e as instrucoes. "Jogar" confere a pasta, cria o image.bin a partir do
// default.xex (xex_imagem.cpp) se ainda nao existir e salva tudo em opcoes.ini. Com "abrir direto" marcado o launcher
// so aparece segurando Shift ao abrir o executavel. Visual com as cores do menu do jogo (ambar, vermelho escuro, preto).
// Item 32: duas colunas (pasta e video | perfil, internet, requisitos e outros), perfis (cada um com os seus jogos
// salvos: save\ = o principal, save\perfis\<nome>\ = os outros), requisitos minimos com o que este PC tem, e os
// textos em ingles na versao -Ingles (idioma.h).
#include "idioma.h"
#include "opcoes.h"
#include "controles.h"
#include "tema.h"
#include "guest.h"
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <fstream>
#include <algorithm>
#include <dxgi.h>

// versao mostrada no titulo do launcher e nas instrucoes (trocar a cada pacote novo)
#define VERSAO_RECOMP L"0.1.1"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{
    Opcoes g_op;
    std::filesystem::path g_arquivo, g_exeDir;

    std::string Utf8(const std::filesystem::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }
    std::filesystem::path DeUtf8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

    std::string Trim(std::string s)
    {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? "" : s.substr(a, b - a + 1);
    }
}

Opcoes& OpcoesAtuais() { return g_op; }

// nome de perfil aceito: letras sem acento, numeros, espaco, - e _, ate 15 letras (como o nome do jogador no Xbox)
std::string PerfilValido(const std::string& s)
{
    std::string r;
    for (char c : s)
        if (r.size() < 15 && (isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_')) r += c;
    size_t a = r.find_first_not_of(' '), b = r.find_last_not_of(' ');
    return a == std::string::npos ? "" : r.substr(a, b - a + 1);
}

std::filesystem::path PastaDoPerfil(const std::filesystem::path& exeDir, const std::string& perfil)
{
    return perfil.empty() ? exeDir / "save" : exeDir / "save" / "perfis" / perfil;
}

void OpcoesCarregar(const std::filesystem::path& exeDir)
{
    g_exeDir = exeDir;
    g_arquivo = exeDir / "opcoes.ini";
    std::ifstream f(g_arquivo);
    for (std::string l; std::getline(f, l);)
    {
        if (size_t c = l.find(';'); c == 0) continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Trim(l.substr(0, eq)), v = Trim(l.substr(eq + 1));
        int n = atoi(v.c_str());
        if (k == "pasta_jogo") g_op.pastaJogo = DeUtf8(v);
        else if (k == "janela") g_op.janela = std::clamp(n, 0, 4);
        else if (k == "tela_cheia") g_op.telaCheia = n != 0;
        else if (k == "antisserrilhado") g_op.antisserrilhado = std::clamp(n, 0, 3);
        else if (k == "escala") g_op.escala = std::clamp(n, 0, 2);
        else if (k == "sombras") g_op.sombras = std::clamp(n, 0, 2);
        else if (k == "vsync") g_op.vsync = n != 0;
        else if (k == "mostrar_registro") g_op.mostrarRegistro = n != 0;
        else if (k == "abrir_direto") g_op.pularLauncher = n != 0;
        else if (k == "ip_anfitriao") g_op.ipAnfitriao = v;
        else if (k == "perfil") g_op.perfil = PerfilValido(v);
    }
}

void OpcoesSalvar()
{
    std::ofstream f(g_arquivo);
    f << "; Opcoes do Far Cry Instincts Predator Recomp (use o launcher: abra o PredatorGame.exe segurando Shift)\n"
      << "pasta_jogo = " << Utf8(g_op.pastaJogo) << "\n"
      << "; 0 = automatico, 1 = 1280x720, 2 = 1600x900, 3 = 1920x1080, 4 = 2560x1440\n"
      << "janela = " << g_op.janela << "\n"
      << "tela_cheia = " << g_op.telaCheia << "\n"
      << "; antisserrilhado: 0 desligado, 1 leve, 2 medio, 3 forte\n"
      << "antisserrilhado = " << g_op.antisserrilhado << "\n"
      << "; resolucao interna (o jogo desenha em mais pixels): 0 = 1x 1280x720, 1 = 1,5x 1920x1080, 2 = 2x 2560x1440\n"
      << "escala = " << g_op.escala << "\n"
      << "; qualidade das sombras (mapa de sombras maior, bordas mais definidas): 0 = normal, 1 = alta 2x, 2 = muito alta 3x\n"
      << "sombras = " << g_op.sombras << "\n"
      << "vsync = " << g_op.vsync << "\n"
      << "mostrar_registro = " << g_op.mostrarRegistro << "\n"
      << "abrir_direto = " << g_op.pularLauncher << "\n"
      << "; SYSTEM LINK pela internet: IP (ou nome) do PC de quem criou a partida; vazio = so rede local\n"
      << "ip_anfitriao = " << g_op.ipAnfitriao << "\n"
      << "; perfil (jogos salvos em save\\perfis\\<nome>); vazio = o principal (pasta save)\n"
      << "perfil = " << g_op.perfil << "\n";
}

namespace
{
    enum : int
    {
        ID_PASTA = 100, ID_PROCURAR = 101, ID_SITUACAO = 102,
        ID_JANELA = 110, ID_TELACHEIA = 111, ID_AA = 112, ID_VSYNC = 113, ID_REGISTRO = 114, ID_DIRETO = 115, ID_ESCALA = 116, ID_SOMBRAS = 117,
        ID_CONTROLES = 120, ID_INSTRUCOES = 121, ID_SAIR = 122, ID_JOGAR = 123, ID_MEUIP = 124,
        ID_DICA = 130, ID_DICA_NET = 131, ID_ANFITRIAO = 132, ID_PERFIL = 133, ID_DICA_PERFIL = 134,
        ID_REQ_MIN = 135, ID_REQ_PC = 136,
    };

    using namespace tema;   // cores do menu do jogo e desenho dos controles (tema.h)
    // RAM usada pelo jogo no pico de uma fase (22_DUNEB, medido em 07/11/2026: 1,48 GB em uso, 1,15 GB privada)
    #define RAM_JOGO_PT L"1,5"
    #define RAM_JOGO_EN L"1.5"
    // duas colunas: esquerda x 24-560, direita x 600-1136
    constexpr int LARGURA = 1160, ALTURA = 676, TOPO = 104, DIR = 600;

    HFONT g_fonte, g_fonteGrande, g_fonteSub;
    HICON g_iconeTopo;
    bool g_jogar = false;
    bool g_marcado[ID_DICA];       // caixas de marcar (desenhadas aqui): ID_TELACHEIA, ID_VSYNC, ID_REGISTRO, ID_DIRETO
    bool g_pcAtende = true;        // este PC atende os requisitos minimos (cor da linha "Este PC")
    struct SecaoTela { int x, y, fim; const wchar_t* nome; };
    const SecaoTela SECOES[] = {
        { 24, 116, 560, TR(L"PASTA DO JOGO", L"GAME FOLDER") }, { 24, 232, 560, TR(L"VÍDEO", L"VIDEO") },
        { 24, 518, 560, TR(L"OUTROS", L"OTHER") },
        { DIR, 116, 1136, TR(L"PERFIL", L"PROFILE") }, { DIR, 232, 1136, TR(L"INTERNET (SYSTEM LINK)", L"INTERNET (SYSTEM LINK)") },
        { DIR, 380, 1136, TR(L"REQUISITOS MÍNIMOS", L"MINIMUM REQUIREMENTS") } };

    std::string PerfilPrincipal();
    std::vector<std::string> PerfisExistentes();
    std::wstring DescreverEstePc(bool& atende);
    bool Caixa(int id) { return id == ID_TELACHEIA || id == ID_VSYNC || id == ID_REGISTRO || id == ID_DIRETO; }
    bool Botao(int id) { return id == ID_PROCURAR || (id >= ID_CONTROLES && id <= ID_MEUIP); }

    HWND Criar(HWND pai, const wchar_t* classe, const wchar_t* texto, DWORD estilo, int x, int y, int w, int h, int id, HFONT fonte = nullptr)
    {
        if (Caixa(id) || Botao(id)) estilo |= BS_OWNERDRAW;
        HWND c = CreateWindowExW(0, classe, texto, WS_CHILD | WS_VISIBLE | estilo, S(x), S(y), S(w), S(h), pai,
                                 (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)(fonte ? fonte : g_fonte), TRUE);
        if (!wcscmp(classe, WC_COMBOBOXW)) ListaEscura(c);
        if (Botao(id)) AcompanharMouse(c);
        return c;
    }

    HWND Lista(HWND j, int id, int y, const wchar_t* rotulo, std::initializer_list<const wchar_t*> itens, int sel)
    {
        Criar(j, L"STATIC", rotulo, 0, 24, y + 4, 166, 20, -1);
        HWND cb = Criar(j, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 190, y, 370, 240, id);
        for (const wchar_t* t : itens) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t);
        SendMessageW(cb, CB_SETCURSEL, sel, 0);
        return cb;
    }

    bool PastaValida(const std::filesystem::path& p)
    {
        std::error_code ec;
        return !p.empty() && std::filesystem::exists(p / "default.xex", ec);
    }

    void AtualizarSituacao(HWND j)
    {
        wchar_t b[1024] = {}; GetDlgItemTextW(j, ID_PASTA, b, 1024);
        std::filesystem::path p = b;
        const wchar_t* t = p.empty() ? TR(L"Escolha a pasta onde estão os arquivos do jogo (a que tem o default.xex).",
                                          L"Choose the folder with the game files (the one with default.xex).")
                         : PastaValida(p) ? TR(L"✔ default.xex encontrado.", L"✔ default.xex found.")
                                          : TR(L"✘ Esta pasta não tem o default.xex. Escolha a pasta com os arquivos extraídos do jogo.",
                                               L"✘ This folder has no default.xex. Choose the folder with the extracted game files.");
        SetDlgItemTextW(j, ID_SITUACAO, t);
    }

    void Procurar(HWND j)
    {
        IFileOpenDialog* d = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
        DWORD o = 0; d->GetOptions(&o);
        d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        d->SetTitle(TR(L"Pasta com os arquivos do jogo (default.xex)", L"Folder with the game files (default.xex)"));
        if (SUCCEEDED(d->Show(j)))
        {
            IShellItem* it = nullptr;
            if (SUCCEEDED(d->GetResult(&it)))
            {
                PWSTR caminho = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &caminho)))
                {
                    SetDlgItemTextW(j, ID_PASTA, caminho);
                    CoTaskMemFree(caminho);
                }
                it->Release();
            }
        }
        d->Release();
        AtualizarSituacao(j);
    }

    const wchar_t* INSTRUCOES = TR(
        L"FAR CRY INSTINCTS PREDATOR RECOMP\r\n"
        L"Versão para PC feita por recompilação estática do jogo de Xbox 360, sem ligação com a Ubisoft ou a\r\n"
        L"Microsoft. Nenhum arquivo do jogo vem junto (fases, gráficos, sons, vídeos): você usa a sua própria cópia.\r\n"
        L"\r\n"
        L"O QUE VOCÊ PRECISA\r\n"
        L"• Windows 10 ou 11 de 64 bits e uma placa de vídeo com DirectX 12.\r\n"
        L"• A SUA cópia do jogo Far Cry Instincts Predator para Xbox 360 (versão USA/Europa), com os arquivos\r\n"
        L"  extraídos numa pasta (a pasta precisa ter o default.xex e os arquivos .dat). Para extrair o ISO do\r\n"
        L"  disco, use um programa como o extract-xiso. Nenhum arquivo do jogo vem junto com este programa.\r\n"
        L"\r\n"
        L"COMO JOGAR\r\n"
        L"1. Em \"Pasta do jogo\", clique em Procurar e escolha a pasta com o default.xex.\r\n"
        L"2. Ajuste o vídeo e os controles se quiser e clique em Jogar.\r\n"
        L"3. Na primeira vez o programa prepara os arquivos e os gráficos: pode levar alguns segundos a mais.\r\n"
        L"Para ver esta janela de novo depois de marcar \"Abrir direto o jogo\", abra o PredatorGame.exe segurando Shift.\r\n"
        L"\r\n"
        L"CONTROLES\r\n"
        L"• Controle: DualSense, DualShock 4 (USB ou Bluetooth) ou controle de Xbox. Teclado e mouse também.\r\n"
        L"• Teclado e mouse (padrão): W A S D andar, mouse olhar, botão esquerdo atirar, botão direito mirar,\r\n"
        L"  Espaço = A (pular), T = B, R = X, F = Y, Q = LB, E = RB, Enter = Start, Esc = Back.\r\n"
        L"• Os jogadores 1 a 4 escolhem seus dispositivos em \"Controles\" (tela dividida até 4 pessoas). O mesmo\r\n"
        L"  dispositivo não pode ficar em dois jogadores.\r\n"
        L"\r\n"
        L"PERFIS\r\n"
        L"• Cada perfil tem os seus jogos salvos e o seu progresso. Para criar um, escreva um nome novo em \"Perfil\".\r\n"
        L"\r\n"
        L"MULTIPLAYER PELA INTERNET (SYSTEM LINK)\r\n"
        L"• Quem cria a partida (anfitrião): clique em MEU IP e mande o número para os amigos.\r\n"
        L"  No jogo: MULTIPLAYER > SYSTEM LINK > criar partida. O jogo tenta abrir as portas do roteador\r\n"
        L"  sozinho (UPnP); se não der, abra à mão as portas UDP 1000, 1004, 1005, 1010 e 49001 para o seu PC\r\n"
        L"  (veja o LEIA-ME).\r\n"
        L"• Quem entra: escreva o IP do anfitrião em \"IP do anfitrião\", clique em Jogar e procure a partida em\r\n"
        L"  MULTIPLAYER > SYSTEM LINK.\r\n"
        L"• Na primeira vez o Windows pergunta se o jogo pode usar a rede: clique em Permitir.\r\n"
        L"\r\n"
        L"TECLAS DURANTE O JOGO\r\n"
        L"• F1: configurar controles\r\n"
        L"• F2: soltar/prender o mouse na janela\r\n"
        L"• + e -: sensibilidade do mouse\r\n"
        L"• Alt+Enter: tela cheia / janela\r\n"
        L"• F12: guardar um relatório do problema (pasta bugs) quando algo der errado\r\n"
        L"\r\n"
        L"ONDE FICAM AS COISAS\r\n"
        L"• Jogos salvos: pasta \"save\" ao lado do PredatorGame.exe (os outros perfis em save\\perfis).\r\n"
        L"• Opções: opcoes.ini e controles.ini (use o launcher em vez de editar).\r\n"
        L"• Registro para relatar problemas: fcrecomp_log.txt.\r\n",
        L"FAR CRY INSTINCTS PREDATOR RECOMP\r\n"
        L"PC version made by static recompilation of the Xbox 360 game, not affiliated with Ubisoft or Microsoft.\r\n"
        L"No game files are included (levels, graphics, sounds, videos): you use your own copy.\r\n"
        L"\r\n"
        L"WHAT YOU NEED\r\n"
        L"• 64-bit Windows 10 or 11 and a DirectX 12 graphics card.\r\n"
        L"• YOUR copy of Far Cry Instincts Predator for Xbox 360 (USA/Europe version), with the files extracted to a\r\n"
        L"  folder (it must have default.xex and the .dat files). To extract the disc ISO, use a tool such as\r\n"
        L"  extract-xiso. No game files come with this program.\r\n"
        L"\r\n"
        L"HOW TO PLAY\r\n"
        L"1. In \"Game folder\", click Browse and choose the folder with default.xex.\r\n"
        L"2. Adjust video and controls if you want and click Play.\r\n"
        L"3. The first time, the program prepares the files and the graphics: it may take a few more seconds.\r\n"
        L"To see this window again after checking \"Start the game directly\", open PredatorGame.exe holding Shift.\r\n"
        L"\r\n"
        L"CONTROLS\r\n"
        L"• Controller: DualSense, DualShock 4 (USB or Bluetooth) or Xbox controller. Keyboard and mouse too.\r\n"
        L"• Keyboard and mouse (default): W A S D move, mouse look, left button shoot, right button aim,\r\n"
        L"  Space = A (jump), T = B, R = X, F = Y, Q = LB, E = RB, Enter = Start, Esc = Back.\r\n"
        L"• Players 1 to 4 choose their devices in \"Controls\" (split screen for up to 4 people). The same device\r\n"
        L"  cannot be used by two players.\r\n"
        L"\r\n"
        L"PROFILES\r\n"
        L"• Each profile has its own saved games and progress. To create one, type a new name in \"Profile\".\r\n"
        L"\r\n"
        L"MULTIPLAYER OVER THE INTERNET (SYSTEM LINK)\r\n"
        L"• Whoever creates the match (host): click MY IP and send the number to your friends.\r\n"
        L"  In the game: MULTIPLAYER > SYSTEM LINK > create match. The game tries to open the router ports\r\n"
        L"  by itself (UPnP); if it can't, open UDP ports 1000, 1004, 1005, 1010 and 49001 to your PC by hand\r\n"
        L"  (see the README).\r\n"
        L"• Whoever joins: type the host's IP in \"Host IP\", click Play and look for the match in\r\n"
        L"  MULTIPLAYER > SYSTEM LINK.\r\n"
        L"• The first time, Windows asks whether the game may use the network: click Allow.\r\n"
        L"\r\n"
        L"KEYS DURING THE GAME\r\n"
        L"• F1: controller settings\r\n"
        L"• F2: release/capture the mouse in the window\r\n"
        L"• + and -: mouse sensitivity\r\n"
        L"• Alt+Enter: full screen / window\r\n"
        L"• F12: save a problem report (bugs folder) when something goes wrong\r\n"
        L"\r\n"
        L"WHERE THINGS ARE\r\n"
        L"• Saved games: \"save\" folder next to PredatorGame.exe (other profiles in save\\perfis).\r\n"
        L"• Options: opcoes.ini and controles.ini (use the launcher instead of editing them).\r\n"
        L"• Log for reporting problems: fcrecomp_log.txt.\r\n");


    LRESULT CALLBACK ProcInstrucoes(HWND j, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_CREATE)
        {
            RECT r; GetClientRect(j, &r);
            HWND e = CreateWindowExW(0, L"EDIT", INSTRUCOES, WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                     S(16), S(16), r.right - S(32), r.bottom - S(106), j, nullptr, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(e, WM_SETFONT, (WPARAM)g_fonte, TRUE);
            // autor e versao: no fim da janela, centralizados na faixa entre a caixa de texto e a borda de baixo
            // (fora da caixa de texto, que nao centraliza uma linha so); altura exata de 2 linhas da fonte
            int alt2 = S(40);
            if (HDC dc = GetDC(j)) {
                HGDIOBJ antes = SelectObject(dc, g_fonte);
                TEXTMETRICW tm{};
                if (GetTextMetricsW(dc, &tm)) alt2 = 2 * tm.tmHeight;
                SelectObject(dc, antes);
                ReleaseDC(j, dc);
            }
            const int faixaTopo = r.bottom - S(90);   // a caixa de texto acaba aqui (S(16) + r.bottom - S(106))
            HWND c = CreateWindowExW(0, L"STATIC", TR(L"Autor: bugcoder00\r\nVersão " VERSAO_RECOMP,
                                                      L"Author: bugcoder00\r\nVersion " VERSAO_RECOMP), WS_CHILD | WS_VISIBLE | SS_CENTER,
                                     S(16), faixaTopo + (S(90) - alt2) / 2, r.right - S(32), alt2, j, (HMENU)(INT_PTR)1, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g_fonte, TRUE);
            SetWindowTheme(e, L"DarkMode_Explorer", nullptr);   // barra de rolagem escura
            return 0;
        }
        if (msg == WM_CTLCOLORSTATIC)   // texto somente leitura
        {
            return CorTexto((HDC)wp, TEXTO);
        }
        if (msg == WM_CLOSE) { DestroyWindow(j); return 0; }
        return DefWindowProcW(j, msg, wp, lp);
    }

    void MostrarInstrucoes(HWND pai)
    {
        static bool registrada = false;
        if (!registrada)
        {
            WNDCLASSW wc{}; wc.lpfnWndProc = ProcInstrucoes; wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); wc.hbrBackground = PincelFundo();
            wc.lpszClassName = L"PredatorInstrucoes"; RegisterClassW(&wc); registrada = true;
        }
        RECT area; SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);   // centro da tela (sem a barra de tarefas)
        HWND j = CreateWindowExW(0, L"PredatorInstrucoes", TR(L"Instruções", L"Instructions"), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                 area.left + std::max(0, (int)(area.right - area.left) - S(780)) / 2,
                                 area.top + std::max(0, (int)(area.bottom - area.top) - S(640)) / 2,
                                 S(780), S(640), pai, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!j) return;
        TituloEscuro(j);
        AplicarIcone(j);
        ShowWindow(j, SW_SHOW);
    }

    void LerTela(HWND j)
    {
        wchar_t b[1024] = {}; GetDlgItemTextW(j, ID_PASTA, b, 1024);
        g_op.pastaJogo = b;
        g_op.janela = std::max(0, (int)SendDlgItemMessageW(j, ID_JANELA, CB_GETCURSEL, 0, 0));
        g_op.telaCheia = g_marcado[ID_TELACHEIA];
        g_op.antisserrilhado = std::max(0, (int)SendDlgItemMessageW(j, ID_AA, CB_GETCURSEL, 0, 0));
        g_op.escala = std::max(0, (int)SendDlgItemMessageW(j, ID_ESCALA, CB_GETCURSEL, 0, 0));
        g_op.sombras = std::max(0, (int)SendDlgItemMessageW(j, ID_SOMBRAS, CB_GETCURSEL, 0, 0));
        g_op.vsync = g_marcado[ID_VSYNC];
        g_op.mostrarRegistro = g_marcado[ID_REGISTRO];
        g_op.pularLauncher = g_marcado[ID_DIRETO];
        wchar_t ip[256] = {}; GetDlgItemTextW(j, ID_ANFITRIAO, ip, 256);
        g_op.ipAnfitriao.clear();
        for (wchar_t* c = ip; *c; c++) if (*c > 32 && *c < 127) g_op.ipAnfitriao += (char)*c;   // IP ou nome: so ASCII, sem espacos
        // perfil: o 1o item da lista ("User 1") = pasta save
        wchar_t pf[64] = {}, primeiro[64] = {};
        GetDlgItemTextW(j, ID_PERFIL, pf, 64);
        SendDlgItemMessageW(j, ID_PERFIL, CB_GETLBTEXT, 0, (LPARAM)primeiro);
        std::string perfil;
        for (wchar_t* c = pf; *c; c++) if (*c < 127) perfil += (char)*c;
        perfil = PerfilValido(perfil);
        g_op.perfil = !wcscmp(pf, primeiro) || perfil == PerfilPrincipal() ? "" : perfil;
    }

    // botao MEU IP: pergunta ao roteador (UPnP, upnp.cpp) o IP de internet deste PC, para dizer aos amigos
    void MostrarMeuIp(HWND j)
    {
        SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32514)));   // ampulheta (ate uns 5 segundos)
        std::string ip, erro, aviso;
        std::wstring t;
        if (UpnpIpExterno(ip, erro, &aviso))
        {
            std::wstring w(ip.begin(), ip.end());
            t = TR(L"Seu IP na internet: ", L"Your internet IP: ") + w +
                TR(L"\r\n\r\nPara jogar com amigos pela internet: diga este número a eles. Eles escrevem no campo "
                   L"\"IP do anfitrião\" e você cria a partida em MULTIPLAYER > SYSTEM LINK. O jogo tenta abrir as portas do roteador sozinho.",
                   L"\r\n\r\nTo play with friends over the internet: tell them this number. They type it in the "
                   L"\"Host IP\" field and you create the match in MULTIPLAYER > SYSTEM LINK. The game tries to open the router ports by itself.");
            if (OpenClipboard(j))   // ja fica copiado (Ctrl+V para colar na conversa com os amigos)
            {
                EmptyClipboard();
                if (HGLOBAL m = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t)))
                {
                    memcpy(GlobalLock(m), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
                    GlobalUnlock(m);
                    SetClipboardData(CF_UNICODETEXT, m);
                    t += TR(L"\r\n\r\n(O IP já foi copiado: é só colar com Ctrl+V.)", L"\r\n\r\n(The IP was already copied: just paste it with Ctrl+V.)");
                }
                CloseClipboard();
            }
            if (!aviso.empty())
                t += TR(L"\r\n\r\nATENÇÃO: o roteador recebeu um IP privado da operadora (CGNAT ou outro roteador na frente). "
                        L"Assim os amigos não conseguem entrar abrindo portas: use uma rede virtual (Radmin VPN, ZeroTier) ou peça "
                        L"um IP público para a operadora. Veja o LEIA-ME.",
                        L"\r\n\r\nWARNING: the router got a private IP from the provider (CGNAT or another router in front). "
                        L"Friends cannot join by opening ports: use a virtual network (Radmin VPN, ZeroTier) or ask the provider "
                        L"for a public IP. See the README.");
            MessageBoxW(j, t.c_str(), TR(L"Meu IP", L"My IP"), aviso.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
        }
        else
        {
            std::wstring e(erro.begin(), erro.end());
            t = TR(L"Não deu para perguntar o IP ao roteador (", L"Could not ask the router for the IP (") + e +
                TR(L").\r\n\r\nO IP de internet aparece na página de "
                   L"configuração do roteador (ou pesquise \"meu ip\" no navegador). Sem o UPnP do roteador as portas também "
                   L"precisam ser abertas à mão: veja o LEIA-ME.",
                   L").\r\n\r\nThe internet IP shows on the router's settings page (or search \"my ip\" in the browser). "
                   L"Without the router's UPnP the ports also have to be opened by hand: see the README.");
            MessageBoxW(j, t.c_str(), TR(L"Meu IP", L"My IP"), MB_ICONWARNING);
        }
    }

    bool Preparar(HWND j)
    {
        LerTela(j);
        if (!PastaValida(g_op.pastaJogo))
        {
            MessageBoxW(j, TR(L"Escolha a pasta com os arquivos extraídos do jogo (precisa ter o default.xex).",
                              L"Choose the folder with the extracted game files (it must have default.xex)."),
                        TR(L"Pasta do jogo", L"Game folder"), MB_ICONWARNING);
            return false;
        }
        std::error_code ec;
        auto imagem = g_exeDir / GUEST_IMAGE_FILE;
        if (std::filesystem::file_size(imagem, ec) != GUEST_IMAGE_SIZE)
        {
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32514)));   // ampulheta
            std::string erro;
            if (!XexCriarImagem(g_op.pastaJogo / GUEST_XEX_NAME, imagem, erro))
            {
                std::wstring w(erro.begin(), erro.end());
                MessageBoxW(j, (TR(L"Não deu para preparar o jogo: ", L"Could not prepare the game: ") + w).c_str(),
                            TR(L"Pasta do jogo", L"Game folder"), MB_ICONERROR);
                return false;
            }
        }
        OpcoesSalvar();
        return true;
    }

    // ---------------------------------------------------------------- perfis
    // nome do perfil principal (pasta save) na lista do launcher: "User 1" (pedido do usuario; no jogo o nome continua o
    // do usuario do Windows, content.cpp)
    std::string PerfilPrincipal() { return "User 1"; }

    // [0] = o principal; depois as pastas de save\perfis
    std::vector<std::string> PerfisExistentes()
    {
        std::vector<std::string> v{ PerfilPrincipal() };
        std::error_code ec;
        for (auto& d : std::filesystem::directory_iterator(g_exeDir / "save" / "perfis", ec))
            if (d.is_directory())
            {
                std::string n = PerfilValido(d.path().filename().string());
                if (!n.empty() && n != v[0]) v.push_back(n);
            }
        if (!g_op.perfil.empty() && std::find(v.begin(), v.end(), g_op.perfil) == v.end()) v.push_back(g_op.perfil);
        return v;
    }

    // ---------------------------------------------------------------- requisitos: o que este PC tem
    std::wstring DescreverEstePc(bool& atende)
    {
        wchar_t cpu[128] = {}; DWORD tam = sizeof(cpu);
        RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, cpu, &tam);
        std::wstring c = cpu;
        while (!c.empty() && c.back() == L' ') c.pop_back();
        while (!c.empty() && c.front() == L' ') c.erase(c.begin());
        DWORD threads = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        ULONGLONG kb = 0;
        double ram = GetPhysicallyInstalledSystemMemory(&kb) ? kb / 1048576.0 : 0;
        if (ram <= 0) { MEMORYSTATUSEX m{ sizeof(m) }; GlobalMemoryStatusEx(&m); ram = m.ullTotalPhys / 1073741824.0; }
        std::wstring gpu;
        IDXGIFactory1* f = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))))
        {
            IDXGIAdapter1* a = nullptr;
            for (UINT i = 0; gpu.empty() && f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++)
            {
                DXGI_ADAPTER_DESC1 d{};
                if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) gpu = d.Description;
                a->Release();
            }
            f->Release();
        }
        atende = threads >= 8 && ram >= 7.5;
        wchar_t b[512];
        swprintf(b, 512, TR(L"Este PC: %ls (%lu threads), %ls, %.0f GB de RAM.  %ls",
                            L"This PC: %ls (%lu threads), %ls, %.0f GB of RAM.  %ls"),
                 c.empty() ? TR(L"processador desconhecido", L"unknown processor") : c.c_str(), threads,
                 gpu.empty() ? TR(L"vídeo desconhecido", L"unknown graphics") : gpu.c_str(), ram,
                 atende ? TR(L"✔ Atende o mínimo.", L"✔ Meets the minimum.")
                        : TR(L"⚠ Abaixo do mínimo: o jogo pode ficar lento.", L"⚠ Below the minimum: the game may run slowly."));
        return b;
    }

    // fundo da janela: faixa do topo (ambar com a pincelada vermelha, icone e titulo) e os titulos das secoes com a
    // linha fina dos paineis do menu
    void PintarFundo(HDC dc, int w, int h)
    {
        RECT tudo{ 0, 0, w, h };
        FillRect(dc, &tudo, PincelFundo());
        int topo = S(TOPO);
        TRIVERTEX v[2] = { { 0, 0, 0x4C00, 0x3200, 0x0C00, 0 }, { w, topo, 0x1C00, 0x1700, 0x1000, 0 } };
        GRADIENT_RECT gr{ 0, 1 };
        GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_H);
        // pincelada vermelha na esquerda (como a do menu)
        HBRUSH verm = CreateSolidBrush(VERMELHO);
        HPEN semBorda = (HPEN)GetStockObject(NULL_PEN);
        HGDIOBJ penAntes = SelectObject(dc, semBorda), pincelAntes = SelectObject(dc, verm);
        POINT p[] = { { S(30), 0 }, { S(118), 0 }, { S(70), topo }, { 0, topo }, { 0, S(40) } };
        Polygon(dc, p, 5);
        SelectObject(dc, pincelAntes); SelectObject(dc, penAntes);
        DeleteObject(verm);
        if (g_iconeTopo) DrawIconEx(dc, S(22), (topo - S(64)) / 2, g_iconeTopo, S(64), S(64), 0, nullptr, DI_NORMAL);

        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ fonteAntes = SelectObject(dc, g_fonteGrande);
        SetTextColor(dc, RGB(250, 246, 236));
        SetTextCharacterExtra(dc, S(1));
        TextOutW(dc, S(104), S(16), L"FAR CRY INSTINCTS PREDATOR", 26);
        SelectObject(dc, g_fonteSub);
        SetTextColor(dc, AMBAR);
        SetTextCharacterExtra(dc, S(3));
        const wchar_t* sub = TR(L"RECOMP  •  VERSÃO PARA PC", L"RECOMP  •  PC VERSION");
        TextOutW(dc, S(106), S(62), sub, (int)wcslen(sub));
        // linha do topo com o degrau em diagonal (como as linhas dos paineis do menu)
        HPEN linha = CreatePen(PS_SOLID, std::max(1, S(2)), AMBAR_ESC);
        SelectObject(dc, linha);
        MoveToEx(dc, 0, topo, nullptr);
        LineTo(dc, w * 55 / 100, topo);
        LineTo(dc, w * 55 / 100 + S(14), topo - S(10));
        LineTo(dc, w, topo - S(10));
        // caixa do campo da pasta (o EDIT fica dentro, com margem)
        {
            RECT caixa{ S(24), S(144), S(408), S(178) };
            FillRect(dc, &caixa, PincelCampo());
            HBRUSH borda = CreateSolidBrush(AMBAR_ESC);
            FrameRect(dc, &caixa, borda);
            DeleteObject(borda);
        }
        {
            RECT caixa{ S(DIR + 130), S(256), S(DIR + 380), S(288) };   // campo do IP do anfitriao
            FillRect(dc, &caixa, PincelCampo());
            HBRUSH borda = CreateSolidBrush(AMBAR_ESC);
            FrameRect(dc, &caixa, borda);
            DeleteObject(borda);
        }
        SetTextCharacterExtra(dc, 0);
        SelectObject(dc, fonteAntes);
        SelectObject(dc, penAntes);
        DeleteObject(linha);
        for (const SecaoTela& s : SECOES) Secao(dc, S(s.x), S(s.y), S(s.fim), s.nome);
    }

    LRESULT CALLBACK Proc(HWND j, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_CREATE:
        {
            // ---- coluna da esquerda: pasta, video, outros
            // campo da pasta dentro de uma caixa desenhada no fundo (PintarFundo), com margem
            Criar(j, L"EDIT", g_op.pastaJogo.wstring().c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 32, 151, 368, 20, ID_PASTA);
            Criar(j, L"BUTTON", TR(L"PROCURAR", L"BROWSE"), WS_TABSTOP, 420, 144, 140, 34, ID_PROCURAR);
            Criar(j, L"STATIC", L"", 0, 24, 184, 536, 40, ID_SITUACAO);

            Lista(j, ID_JANELA, 262, TR(L"Tamanho da janela:", L"Window size:"),
                  { TR(L"Automático (o maior que cabe na tela)", L"Automatic (the largest that fits the screen)"), L"1280 × 720", L"1600 × 900", L"1920 × 1080", L"2560 × 1440" }, g_op.janela);
            Criar(j, L"BUTTON", TR(L"Tela cheia (Alt+Enter troca durante o jogo)", L"Full screen (Alt+Enter switches during the game)"), WS_TABSTOP, 24, 294, 536, 24, ID_TELACHEIA);
            Lista(j, ID_AA, 324, TR(L"Antisserrilhado:", L"Anti-aliasing:"),
                  { TR(L"Desligado", L"Off"), TR(L"1 - leve", L"1 - light"), TR(L"2 - médio", L"2 - medium"), TR(L"3 - forte (bordas mais lisas)", L"3 - strong (smoother edges)") }, g_op.antisserrilhado);
            Lista(j, ID_ESCALA, 358, TR(L"Resolução interna:", L"Internal resolution:"),
                  { TR(L"1x - 1280 × 720 (mais leve)", L"1x - 1280 × 720 (lightest)"), TR(L"1,5x - 1920 × 1080", L"1.5x - 1920 × 1080"),
                    TR(L"2x - 2560 × 1440 (mais nítido, mais pesado)", L"2x - 2560 × 1440 (sharper, heavier)") }, g_op.escala);
            Lista(j, ID_SOMBRAS, 392, TR(L"Sombras:", L"Shadows:"),
                  { L"Normal", TR(L"Alta (bordas mais definidas)", L"High (sharper edges)"), TR(L"Muito alta (mais pesada)", L"Very high (heavier)") }, g_op.sombras);
            Criar(j, L"BUTTON", TR(L"Sincronia vertical (evita \"rasgos\" na imagem, pode baixar o FPS)", L"Vertical sync (avoids image \"tearing\", may lower the FPS)"),
                  WS_TABSTOP, 24, 426, 536, 24, ID_VSYNC);
            Criar(j, L"STATIC", TR(L"Resolução interna e sombras maiores deixam a imagem mais nítida, mas pesam na placa de vídeo"
                                   L" (vídeo integrado: 1x e sombras normais ou altas).",
                                   L"Higher internal resolution and shadows make the image sharper, but weigh on the graphics card"
                                   L" (integrated graphics: 1x and normal or high shadows)."), SS_LEFT, 24, 456, 536, 44, ID_DICA);

            Criar(j, L"BUTTON", TR(L"Abrir direto o jogo da próxima vez (Shift ao abrir mostra esta janela)",
                                   L"Start the game directly next time (Shift while opening shows this window)"), WS_TABSTOP, 24, 546, 536, 24, ID_DIRETO);
            Criar(j, L"BUTTON", TR(L"Mostrar a janela de registro (para depurar)", L"Show the log window (for debugging)"), WS_TABSTOP, 24, 574, 536, 24, ID_REGISTRO);

            // ---- coluna da direita: perfil, internet, requisitos
            {
                Criar(j, L"STATIC", TR(L"Perfil:", L"Profile:"), 0, DIR, 150, 120, 20, -1);
                HWND cb = Criar(j, WC_COMBOBOXW, L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_TABSTOP | WS_VSCROLL, DIR + 130, 146, 250, 240, ID_PERFIL);
                SendMessageW(cb, CB_LIMITTEXT, 15, 0);
                std::vector<std::string> nomes = PerfisExistentes();
                int sel = 0;
                for (size_t i = 0; i < nomes.size(); i++)
                {
                    std::wstring w(nomes[i].begin(), nomes[i].end());
                    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)w.c_str());
                    if (i > 0 && nomes[i] == g_op.perfil) sel = (int)i;
                }
                SendMessageW(cb, CB_SETCURSEL, sel, 0);
                Criar(j, L"STATIC", TR(L"Cada perfil tem os seus jogos salvos e o seu progresso. Para criar outro, escreva um nome novo"
                                       L" (até 15 letras) e clique em Jogar.",
                                       L"Each profile has its own saved games and progress. To create another, type a new name"
                                       L" (up to 15 letters) and click Play."), SS_LEFT, DIR, 180, 536, 40, ID_DICA_PERFIL);
            }
            {
                Criar(j, L"STATIC", TR(L"IP do anfitrião:", L"Host IP:"), 0, DIR, 264, 126, 20, -1);
                std::wstring ip(g_op.ipAnfitriao.begin(), g_op.ipAnfitriao.end());
                Criar(j, L"EDIT", ip.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, DIR + 138, 263, 234, 20, ID_ANFITRIAO);
                Criar(j, L"BUTTON", TR(L"MEU IP", L"MY IP"), WS_TABSTOP, 996, 255, 140, 34, ID_MEUIP);
                Criar(j, L"STATIC", TR(L"Para entrar na partida de um amigo pela internet, escreva o IP dele (ele vê o dele em MEU IP)."
                                       L" Deixe vazio para jogar só em rede local. Quem cria a partida não escreve nada aqui.",
                                       L"To join a friend's match over the internet, type their IP (they see theirs in MY IP)."
                                       L" Leave it empty to play only on the local network. Whoever creates the match types nothing here."),
                      SS_LEFT, DIR, 298, 536, 62, ID_DICA_NET);
            }
            {
                std::wstring pc = DescreverEstePc(g_pcAtende);
                Criar(j, L"STATIC", TR(L"Sistema: Windows 10 ou 11 de 64 bits\n"
                                       L"Processador: 4 núcleos e 8 threads\n"
                                       L"Memória: 8 GB de RAM (o jogo usa ~" RAM_JOGO_PT L" GB)\n"
                                       L"Vídeo: placa com DirectX 12 (vídeo integrado serve)",
                                       L"System: 64-bit Windows 10 or 11\n"
                                       L"Processor: 4 cores and 8 threads\n"
                                       L"Memory: 8 GB of RAM (the game uses ~" RAM_JOGO_EN L" GB)\n"
                                       L"Graphics: DirectX 12 card (integrated graphics works)"), SS_LEFT, DIR, 408, 536, 82, ID_REQ_MIN);
                Criar(j, L"STATIC", pc.c_str(), SS_LEFT, DIR, 494, 536, 62, ID_REQ_PC);
            }

            Criar(j, L"BUTTON", TR(L"CONTROLES", L"CONTROLS"), WS_TABSTOP, 24, 618, 170, 38, ID_CONTROLES);
            Criar(j, L"BUTTON", TR(L"INSTRUÇÕES", L"INSTRUCTIONS"), WS_TABSTOP, 204, 618, 190, 38, ID_INSTRUCOES);
            Criar(j, L"BUTTON", TR(L"SAIR", L"QUIT"), WS_TABSTOP, 862, 618, 120, 38, ID_SAIR);
            Criar(j, L"BUTTON", TR(L"JOGAR", L"PLAY"), WS_TABSTOP, 992, 618, 144, 38, ID_JOGAR);

            g_marcado[ID_TELACHEIA] = g_op.telaCheia;
            g_marcado[ID_VSYNC] = g_op.vsync;
            g_marcado[ID_REGISTRO] = g_op.mostrarRegistro;
            g_marcado[ID_DIRETO] = g_op.pularLauncher;
            AtualizarSituacao(j);
            return 0;
        }
        case WM_COMMAND:
        {
            int id = LOWORD(wp);
            if (Caixa(id) && (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == BN_DOUBLECLICKED))
            {
                g_marcado[id] = !g_marcado[id];
                InvalidateRect((HWND)lp, nullptr, FALSE);
                return 0;
            }
            switch (id)
            {
            case ID_PASTA: if (HIWORD(wp) == EN_CHANGE) AtualizarSituacao(j); return 0;
            case ID_PROCURAR: Procurar(j); return 0;
            case ID_CONTROLES: ControlesAbrirJanela(false); return 0;
            case ID_INSTRUCOES: MostrarInstrucoes(j); return 0;
            case ID_MEUIP: MostrarMeuIp(j); return 0;
            case ID_SAIR: case IDCANCEL: DestroyWindow(j); return 0;
            case ID_JOGAR: case IDOK: if (Preparar(j)) { g_jogar = true; DestroyWindow(j); } return 0;   // Enter = Jogar
            }
            break;
        }
        case WM_DRAWITEM:
        {
            auto* d = (const DRAWITEMSTRUCT*)lp;
            if (Caixa((int)d->CtlID)) DesenharCaixa(d, g_marcado[d->CtlID], g_fonte); else DesenharBotao(d, d->CtlID == ID_JOGAR, FonteBotao());
            return TRUE;
        }
        case WM_LBUTTONDOWN: SetFocus(j); return 0;   // clique fora das caixas: tira a selecao (azul) da caixa com foco
        case WM_ERASEBKGND: return 1;   // o WM_PAINT pinta tudo (sem piscar)
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(j, &ps);
            RECT r; GetClientRect(j, &r);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
            HGDIOBJ antes = SelectObject(mem, bmp);
            PintarFundo(mem, r.right, r.bottom);
            BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, antes);
            DeleteObject(bmp); DeleteDC(mem);
            EndPaint(j, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        {
            HDC dc = (HDC)wp;
            int id = GetDlgCtrlID((HWND)lp);
            COLORREF cor = TEXTO;
            if (id == ID_DICA || id == ID_DICA_NET || id == ID_DICA_PERFIL || id == ID_REQ_MIN) cor = TEXTO_FRACO;
            if (id == ID_REQ_PC) cor = g_pcAtende ? VERDE : AMBAR;
            if (id == ID_SITUACAO)
            {
                wchar_t b[8] = {}; GetWindowTextW((HWND)lp, b, 8);
                cor = b[0] == L'✔' ? VERDE : b[0] == L'✘' ? ERRO : TEXTO_FRACO;
            }
            return CorTexto(dc, cor);
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return CorCampo((HDC)wp);
        case WM_CLOSE: DestroyWindow(j); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(j, msg, wp, lp);
    }
}

void AplicarIcone(HWND__* janela)
{
    HINSTANCE inst = GetModuleHandleW(nullptr);
    UINT dpi = GetDpiForWindow(janela);
    auto carregar = [&](int metrica) {
        int lado = GetSystemMetricsForDpi(metrica, dpi ? dpi : 96);
        return (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, lado, lado, LR_DEFAULTCOLOR);
    };
    if (HICON grande = carregar(SM_CXICON)) SendMessageW(janela, WM_SETICON, ICON_BIG, (LPARAM)grande);
    if (HICON pequeno = carregar(SM_CXSMICON)) SendMessageW(janela, WM_SETICON, ICON_SMALL, (LPARAM)pequeno);
}

bool LauncherMostrar(const std::filesystem::path& exeDir)
{
    g_exeDir = exeDir;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    tema::Iniciar(GetDpiForSystem() / 96.0f);   // escala de tela do Windows (o programa avisa que cuida dela: main.cpp)
    g_fonte = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fonteGrande = CreateFontW(-S(34), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Impact");
    g_fonteSub = CreateFontW(-S(13), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_iconeTopo = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, S(64), S(64), LR_DEFAULTCOLOR);
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = PincelFundo();
    wc.lpszClassName = L"PredatorLauncher";
    RegisterClassW(&wc);
    RECT r{ 0, 0, S(LARGURA), S(ALTURA) };
    DWORD estilo = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    AdjustWindowRectExForDpi(&r, estilo, FALSE, 0, GetDpiForSystem());
    int w = r.right - r.left, h = r.bottom - r.top;
    RECT area; SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    HWND j = CreateWindowExW(0, L"PredatorLauncher", L"Far Cry Instincts Predator Recomp " VERSAO_RECOMP, estilo,
                             area.left + std::max(0L, (area.right - area.left - w) / 2), area.top + std::max(0L, (area.bottom - area.top - h) / 2),
                             w, h, nullptr, nullptr, wc.hInstance, nullptr);
    if (!j) return true;   // sem janela: segue para o jogo com as opcoes salvas
    TituloEscuro(j);
    AplicarIcone(j);
    ShowWindow(j, SW_SHOW);
    SetForegroundWindow(j);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (IsDialogMessageW(j, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return g_jogar;
}
