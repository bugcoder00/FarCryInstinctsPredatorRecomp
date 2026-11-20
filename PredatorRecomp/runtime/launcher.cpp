// Launcher: janela que abre antes do jogo (dois cliques no PredatorGame.exe). Escolhe a pasta da copia do jogo,
// opcoes de video, abre a janela de controles e as instrucoes. "Jogar" confere a pasta, cria o image.bin a partir do
// default.xex (xex_imagem.cpp) se ainda nao existir e salva tudo em opcoes.ini. Com "abrir direto" marcado o launcher
// so aparece segurando Shift ao abrir o executavel. Visual com as cores do menu do jogo (ambar, vermelho escuro, preto).
// Item 32: duas colunas (pasta e video | perfil, internet, requisitos e outros), perfis (cada um com os seus jogos
// salvos: save\ = o principal, save\perfis\<nome>\ = os outros), requisitos minimos com o que este PC tem.
// Idioma (portugues/ingles) escolhido na faixa do topo (idioma.h; na primeira vez, o do Windows); trocar recria a
// janela. Visual: faixa do topo, menu lateral sem caixas e o painel da categoria num cartao de cantos arredondados.
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
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <map>
#include <ctime>
#include "conquistas.h"
#include "iso.h"
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#include <functional>
void TecladoAbrir(std::wstring titulo, std::wstring descricao, std::wstring padrao, uint32_t maximo,
                  std::function<void(bool ok, const std::wstring& texto)> fim);   // teclado.cpp

// versao mostrada no titulo do launcher e nas instrucoes (trocar a cada pacote novo)
#define VERSAO_RECOMP L"1.0.0"

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
        else if (k == "msaa") g_op.msaa = std::clamp(n, 0, 3);
        else if (k == "escala") g_op.escala = std::clamp(n, 0, 3);
        else if (k == "fsr") g_op.fsr = std::clamp(n, 0, 4);
        else if (k == "fsr_nitidez") g_op.fsrNitidez = std::clamp(n, 0, 4);
        else if (k == "sombras") g_op.sombras = std::clamp(n, 0, 2);
        else if (k == "vsync") g_op.vsync = n != 0;
        else if (k == "auto_hdr") g_op.autoHdr = n != 0;
        else if (k == "hdr_falso") g_op.hdrFalso = n != 0;
        else if (k == "mostrar_fps") g_op.mostrarFps = n != 0;
        else if (k == "aviso_teclas") g_op.avisoTeclas = n != 0;
        else if (k == "filtro_textura") g_op.filtroTextura = std::clamp(n, 0, 4);
        else if (k == "distancia") g_op.distancia = std::clamp(n, -2, 3);
        else if (k == "detalhe") g_op.detalhe = std::clamp(n, -2, 3);
        else if (k == "ao") g_op.ao = std::clamp(n, 0, 3);
        else if (k == "formato") g_op.formato = std::clamp(n, 0, 2);
        else if (k == "fps") g_op.fps = std::clamp(n, 0, 2);
        else if (k == "fps60") g_op.fps = n != 0 ? 1 : 0;   // 0.1.1 de testes: caixa "60 FPS"
        else if (k == "mostrar_registro") g_op.mostrarRegistro = n != 0;
        else if (k == "abrir_direto") g_op.pularLauncher = n != 0;
        else if (k == "launcher_icones") g_op.launcherIcones = n != 0;
        else if (k == "ip_anfitriao") g_op.ipAnfitriao = v;
        else if (k == "perfil") g_op.perfil = PerfilValido(v);
        else if (k == "trapaca_municao") g_op.trapacaMunicao = n != 0;
        else if (k == "trapaca_adrenalina") g_op.trapacaAdrenalina = n != 0;
        else if (k == "trapaca_feral") g_op.trapacaFeral = n != 0;
        else if (k == "trapaca_fases") g_op.trapacaFases = n != 0;
        else if (k == "trapaca_mapas") g_op.trapacaMapas = n != 0;
        else if (k == "trapaca_invencivel") g_op.trapacaInvencivel = n != 0;
        else if (k == "idioma") g_op.idioma = v == "pt" ? 0 : v == "en" ? 1 : v == "fr" ? 2 : v == "de" ? 3 : v == "es" ? 4 : v == "it" ? 5 : -1;
    }
    // idioma dos textos do programa: o escolhido no launcher; na primeira vez, o da interface do Windows.
    // Frances/alemao/espanhol/italiano: o jogo nesse idioma (xam.cpp XGetLanguage) e o programa em ingles.
    idioma::ingles = g_op.idioma >= 0 ? g_op.idioma != 0 : IdiomaDoWindowsIngles();
    idioma::outro = g_op.idioma >= 2 && g_op.idioma <= 5 ? g_op.idioma - 1 : 0;
}

namespace
{
    // codigo do opcoes.ini para o idioma atual (g_op.idioma; -1 = o do Windows)
    const char* CodigoIdioma()
    {
        static const char* const cod[] = { "pt", "en", "fr", "de", "es", "it" };
        return cod[g_op.idioma >= 0 && g_op.idioma <= 5 ? g_op.idioma : IDIOMA_INGLES ? 1 : 0];
    }
}

bool IdiomaDoWindowsIngles()
{
    return PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_PORTUGUESE;
}

void OpcoesSalvar()
{
    std::ofstream f(g_arquivo);
    f << TR("; Opcoes do Far Cry Instincts Predator Recomp (use o launcher: abra o PredatorGame.exe segurando Shift)\n", "; Far Cry Instincts Predator Recomp options (use the launcher: open PredatorGame.exe holding Shift). The setting names are the same in both languages\n")
      << TR("; idioma: pt = portugues (jogo com a traducao PT-BR), en = ingles; fr/de/es/it = jogo em frances/alemao/espanhol/italiano (programa em ingles)\n", "; language (idioma): pt = Portuguese (game with the PT-BR translation), en = English; fr/de/es/it = game in French/German/Spanish/Italian (program in English)\n")
      << "idioma = " << CodigoIdioma() << "\n"
      << "pasta_jogo = " << Utf8(g_op.pastaJogo) << "\n"
      << TR("; 0 = automatico, 1 = 1280x720, 2 = 1600x900, 3 = 1920x1080, 4 = 2560x1440\n", "; window size (janela): 0 = automatic, 1 = 1280x720, 2 = 1600x900, 3 = 1920x1080, 4 = 2560x1440; tela_cheia = full screen\n")
      << "janela = " << g_op.janela << "\n"
      << "tela_cheia = " << g_op.telaCheia << "\n"
      << TR("; antisserrilhado: 0 desligado, 1 leve, 2 medio, 3 forte\n", "; anti-aliasing (antisserrilhado): 0 off, 1 low, 2 medium, 3 high\n")
      << "antisserrilhado = " << g_op.antisserrilhado << "\n"
      << TR("; MSAA (bordas lisas no desenho, pesa na placa): 0 desligado, 1 = 2x, 2 = 4x, 3 = 8x\n", "; MSAA (smooth edges while drawing, weighs on the graphics card): 0 off, 1 = 2x, 2 = 4x, 3 = 8x\n")
      << "msaa = " << g_op.msaa << "\n"
      << TR("; resolucao interna (o jogo desenha em mais pixels): 0 = 1x 1280x720, 1 = 1,5x 1920x1080, 2 = 2x 2560x1440, 3 = 3x 3840x2160 (4K)\n", "; internal resolution (escala; the game draws more pixels): 0 = 1x 1280x720, 1 = 1.5x 1920x1080, 2 = 2x 2560x1440, 3 = 3x 3840x2160 (4K)\n")
      << "escala = " << g_op.escala << "\n"
      << TR("; upscale AMD FSR 1 (desenha menor e amplia para o tamanho da janela; ligado, a resolucao interna nao vale): 0 desligado, 1 qualidade, 2 equilibrado, 3 desempenho, 4 ultra qualidade\n", "; AMD FSR 1 upscaling (fsr; draws smaller and upscales to the window size; when on, the internal resolution is ignored): 0 off, 1 quality, 2 balanced, 3 performance, 4 ultra quality\n")
      << "fsr = " << g_op.fsr << "\n"
      << TR("; nitidez do FSR: 0 desligada, 1 baixa, 2 media, 3 alta, 4 maxima\n", "; FSR sharpness (fsr_nitidez): 0 off, 1 low, 2 medium, 3 high, 4 maximum\n")
      << "fsr_nitidez = " << g_op.fsrNitidez << "\n"
      << TR("; formato da imagem: 0 = 16:9 (como o original), 1 = o do monitor (ultrawide 21:9 / 32:9, experimental), 2 = 16:9 com barras pretas (sem esticar)\n", "; aspect ratio (formato): 0 = 16:9 (like the original), 1 = the monitor's (ultrawide 21:9 / 32:9, experimental), 2 = 16:9 with black bars (not stretched)\n")
      << "formato = " << g_op.formato << "\n"
      << TR("; qualidade das sombras (mapa de sombras maior, bordas mais definidas): 0 = normal, 1 = alta 2x, 2 = muito alta 3x\n", "; shadow quality (sombras; bigger shadow map, sharper edges): 0 = normal, 1 = high 2x, 2 = very high 3x; vsync = vertical sync\n")
      << "sombras = " << g_op.sombras << "\n"
      << "vsync = " << g_op.vsync << "\n"
      << TR("; Auto HDR (imagem do jogo convertida para HDR, realces mais brilhantes; precisa do HDR ligado no Windows): 0 desligado, 1 ligado\n", "; Auto HDR (auto_hdr; game image converted to HDR, brighter highlights; needs HDR turned on in Windows): 0 off, 1 on\n")
      << "auto_hdr = " << g_op.autoHdr << "\n"
      << TR("; HDR falso (filtro para monitor comum: mais contraste, sombras mais claras, luzes e cores mais fortes; continua SDR): 0 desligado, 1 ligado\n", "; fake HDR (hdr_falso; filter for regular monitors: more contrast, brighter shadows, stronger highlights and colors; stays SDR): 0 off, 1 on\n")
      << "hdr_falso = " << g_op.hdrFalso << "\n"
      << TR("; mostrar FPS e ms no canto da tela: 0 nao, 1 sim; aviso das teclas F1/F2/F3 quando o jogo comeca: 0 nao, 1 sim\n", "; show FPS and ms in the corner of the screen (mostrar_fps): 0 no, 1 yes; F1/F2/F3 key hint when the game starts (aviso_teclas): 0 no, 1 yes\n")
      << "mostrar_fps = " << g_op.mostrarFps << "\n"
      << "aviso_teclas = " << g_op.avisoTeclas << "\n"
      << TR("; filtro de textura (anisotropico, texturas nitidas de longe): 0 = o do jogo, 1 = 2x, 2 = 4x, 3 = 8x, 4 = 16x\n", "; texture filtering (filtro_textura; anisotropic, sharp textures at a distance): 0 = the game's, 1 = 2x, 2 = 4x, 3 = 8x, 4 = 16x\n")
      << "filtro_textura = " << g_op.filtroTextura << "\n"
      << TR("; distancia de visao e detalhe dos objetos de longe: -2 = 0,5x, -1 = 0,75x, 0 = original, 1 = 1,5x, 2 = 2x, 3 = 3x\n", "; draw distance (distancia) and object detail at a distance (detalhe): -2 = 0.5x, -1 = 0.75x, 0 = original, 1 = 1.5x, 2 = 2x, 3 = 3x\n")
      << "distancia = " << g_op.distancia << "\n"
      << "detalhe = " << g_op.detalhe << "\n"
      << TR("; oclusao de ambiente (cantos e encontros mais escuros): 0 desligada, 1 leve, 2 media, 3 forte\n", "; ambient occlusion (ao; darker corners and contact areas): 0 off, 1 light, 2 medium, 3 strong\n")
      << "ao = " << g_op.ao << "\n"
      << TR("; quadros por segundo: 0 = 30 (como o original), 1 = 60, 2 = sem limite (experimental)\n", "; frames per second (fps): 0 = 30 (like the original), 1 = 60, 2 = unlocked (experimental); mostrar_registro = log window, abrir_direto = skip the launcher\n")
      << "fps = " << g_op.fps << "\n"
      << "mostrar_registro = " << g_op.mostrarRegistro << "\n"
      << "abrir_direto = " << g_op.pularLauncher << "\n"
      << "launcher_icones = " << g_op.launcherIcones << "\n"
      << TR("; SYSTEM LINK pela internet: IP (ou nome) do PC de quem criou a partida; vazio = so rede local\n", "; SYSTEM LINK over the internet (ip_anfitriao): IP (or name) of the PC that created the match; empty = local network only\n")
      << "ip_anfitriao = " << g_op.ipAnfitriao << "\n"
      << TR("; perfil (jogos salvos em save\\perfis\\<nome>); vazio = o principal (pasta save)\n", "; profile (perfil; saved games in save\\perfis\\<name>); empty = the main one (save folder)\n")
      << "perfil = " << g_op.perfil << "\n"
      << TR("; trapacas (1 = ligada): municao infinita, adrenalina infinita, poderes ferais, todas as fases, todos os mapas, vida sempre cheia\n",
            "; cheats (trapaca_*, 1 = on): infinite ammo, infinite adrenaline, feral abilities, all levels, all maps, health always full\n")
      << "trapaca_municao = " << g_op.trapacaMunicao << "\n"
      << "trapaca_adrenalina = " << g_op.trapacaAdrenalina << "\n"
      << "trapaca_feral = " << g_op.trapacaFeral << "\n"
      << "trapaca_fases = " << g_op.trapacaFases << "\n"
      << "trapaca_mapas = " << g_op.trapacaMapas << "\n"
      << "trapaca_invencivel = " << g_op.trapacaInvencivel << "\n";
}

namespace
{
    enum : int
    {
        ID_PASTA = 100, ID_PROCURAR = 101, ID_SITUACAO = 102, ID_ISO = 103,
        ID_JANELA = 110, ID_TELACHEIA = 111, ID_AA = 112, ID_VSYNC = 113, ID_REGISTRO = 114, ID_DIRETO = 115, ID_ESCALA = 116, ID_SOMBRAS = 117, ID_FPS = 118, ID_FORMATO = 119, ID_FILTRO_TEX = 170, ID_DISTANCIA = 171, ID_DETALHE = 172, ID_AO = 173, ID_FSR = 175, ID_FSR_NITIDEZ = 176, ID_AUTOHDR = 177, ID_HDRFALSO = 178, ID_MOSTRARFPS = 179, ID_AVISOTECLAS = 186, ID_MSAA = 187,
        ID_CONTROLES = 120, ID_INSTRUCOES = 121, ID_SAIR = 122, ID_JOGAR = 123, ID_MEUIP = 124, ID_SALVAR = 125,
        ID_DICA = 130, ID_DICA_NET = 131, ID_ANFITRIAO = 132, ID_PERFIL = 133, ID_DICA_PERFIL = 134,
        ID_REQ_MIN = 135, ID_REQ_PC = 136, ID_IDIOMA = 174,
        ID_CAT = 140,   // 140..146: botoes do menu da esquerda (uma categoria cada)
        ID_SAVES = 150, ID_RENOMEAR = 151, ID_DUPLICAR = 152, ID_APAGAR = 153, ID_ABRIRSAVE = 154,
        ID_CONQ = 160, ID_CONQ_RESUMO = 161, ID_VISTA_SAVES = 155, ID_VISTA_CONQ = 162, ID_FILTRO_SAVES = 156,
        ID_TRAPACA = 180,        // 180..185: caixas das trapacas (municao, adrenalina, feral, fases, mapas, invencivel)
        ID_TRAPACA_DESC = 190,   // 190..195: descricao de cada uma
        ID_TRAPACA_AVISO = 196,
        N_IDS = 200,
    };

    using namespace tema;   // cores do menu do jogo e desenho dos controles (tema.h)
    // RAM usada pelo jogo no pico de uma fase (22_DUNEB, medido em 07/11/2026: 1,48 GB em uso, 1,15 GB privada)
    #define RAM_JOGO_PT L"1,5"
    #define RAM_JOGO_EN L"1.5"
    // menu de categorias na esquerda (x 24-264) e o painel da categoria escolhida na direita (x 300-1136)
    // faixa do topo ate TOPO_FAIXA; o cartao do painel vai de CARTAO_X/TOPO ate RODAPE (os botoes de baixo ficam abaixo)
    constexpr int LARGURA = 1160, ALTURA = 744, TOPO = 104, DIR = 600, PAINEL_X = 300;
    constexpr int TOPO_FAIXA = 90, CARTAO_X = 284, RODAPE = 672, IDIOMA_X = 936;

    HFONT g_fonte, g_fonteGrande, g_fonteSub;
    HICON g_iconeTopo;
    bool g_jogar = false;
    bool g_recriar = false;          // trocou o idioma: a janela e criada de novo com os textos novos
    int g_categoriaInicial = -1;     // categoria a mostrar na janela recriada
    RECT g_lugar{};                  // onde a janela estava (a recriada abre no mesmo lugar)
    bool g_marcado[N_IDS];         // caixas de marcar (desenhadas aqui): ID_TELACHEIA, ID_VSYNC, ID_REGISTRO, ID_DIRETO, trapacas
    bool g_pcAtende = true;        // este PC atende os requisitos minimos (cor da linha "Este PC")
    // categorias do menu da esquerda: ao clicar, os controles dela aparecem no painel ao lado. Cada controle foi
    // desenhado nas coordenadas da tela antiga de duas colunas; (dx, dy) leva a secao para o topo do painel.
    struct Categoria
    {
        const wchar_t *botaoPt, *botaoEn, *tituloPt, *tituloEn;   // TR2: a tabela e iniciada antes de o idioma ser lido
        int dx, dy;
        const wchar_t* botao() const { return TR(botaoPt, botaoEn); }
        const wchar_t* titulo() const { return TR(tituloPt, tituloEn); }
    };
    const Categoria CATEGORIAS[] = {
        { TR2(L"PASTA DO JOGO", L"GAME FOLDER"), TR2(L"PASTA DO JOGO", L"GAME FOLDER"), PAINEL_X - 24, 0 },
        { TR2(L"VÍDEO", L"VIDEO"), TR2(L"VÍDEO", L"VIDEO"), PAINEL_X - 24, -112 },
        { TR2(L"PERFIL", L"PROFILE"), TR2(L"PERFIL", L"PROFILE"), PAINEL_X - DIR, 0 },
        { TR2(L"CONQUISTAS", L"ACHIEVEMENTS"), TR2(L"CONQUISTAS", L"ACHIEVEMENTS"), 0, 0 },
        { TR2(L"TRAPAÇAS", L"CHEATS"), TR2(L"TRAPAÇAS", L"CHEATS"), 0, 0 },
        { TR2(L"INTERNET", L"INTERNET"), TR2(L"INTERNET (SYSTEM LINK)", L"INTERNET (SYSTEM LINK)"), PAINEL_X - DIR, -105 },
        { TR2(L"OUTROS", L"OTHER"), TR2(L"OUTROS", L"OTHER"), PAINEL_X - 24, -464 },
        { TR2(L"REQUISITOS", L"REQUIREMENTS"), TR2(L"REQUISITOS MÍNIMOS", L"MINIMUM REQUIREMENTS"), PAINEL_X - DIR, -256 } };
    constexpr int N_CAT = (int)(sizeof(CATEGORIAS) / sizeof(CATEGORIAS[0]));
    enum { CAT_PASTA, CAT_VIDEO, CAT_PERFIL, CAT_CONQUISTAS, CAT_TRAPACAS, CAT_INTERNET, CAT_OUTROS, CAT_REQUISITOS };   // mesma ordem de CATEGORIAS
    int g_criando = -1;                              // categoria dos controles sendo criados (-1 = sempre visiveis)
    int g_categoria = CAT_VIDEO;                     // a que aparece no painel
    std::vector<std::pair<HWND, int>> g_daCategoria; // controle -> categoria

    std::string PerfilPrincipal();
    std::vector<std::string> PerfisExistentes();

    std::wstring DescreverEstePc(bool& atende);
    bool Caixa(int id) { return id == ID_TELACHEIA || id == ID_VSYNC || id == ID_AUTOHDR || id == ID_HDRFALSO || id == ID_MOSTRARFPS || id == ID_AVISOTECLAS || id == ID_REGISTRO || id == ID_DIRETO || (id >= ID_TRAPACA && id < ID_TRAPACA + 6); }
    bool Botao(int id) { return id == ID_PROCURAR || id == ID_ISO || (id >= ID_CONTROLES && id <= ID_SALVAR) || (id >= ID_CAT && id < ID_CAT + N_CAT) || (id >= ID_RENOMEAR && id <= ID_ABRIRSAVE) || id == ID_VISTA_SAVES || id == ID_VISTA_CONQ || id == ID_FILTRO_SAVES; }

    HWND Criar(HWND pai, const wchar_t* classe, const wchar_t* texto, DWORD estilo, int x, int y, int w, int h, int id, HFONT fonte = nullptr)
    {
        if (Caixa(id) || Botao(id)) estilo |= BS_OWNERDRAW;
        if (g_criando >= 0) { x += CATEGORIAS[g_criando].dx; y += CATEGORIAS[g_criando].dy; }
        HWND c = CreateWindowExW(0, classe, texto, WS_CHILD | (g_criando < 0 || g_criando == g_categoria ? WS_VISIBLE : 0) | estilo,
                                 S(x), S(y), S(w), S(h), pai, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        if (g_criando >= 0) g_daCategoria.push_back({ c, g_criando });
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
        return !p.empty() && JogoExiste(p / "default.xex");   // pasta extraida ou o .iso do disco (iso.h)
    }

    void AtualizarSituacao(HWND j)
    {
        wchar_t b[1024] = {}; GetDlgItemTextW(j, ID_PASTA, b, 1024);
        std::filesystem::path p = b;
        const bool ehIso = _wcsicmp(p.extension().c_str(), L".iso") == 0;
        const wchar_t* t = p.empty() ? TR(L"Escolha a pasta com os arquivos do jogo (a que tem o default.xex) ou o arquivo .iso do disco.",
                                          L"Choose the folder with the game files (the one with default.xex) or the disc .iso file.")
                         : PastaValida(p) ? (ehIso ? TR(L"✔ ISO do jogo encontrada: o jogo roda direto dela, sem extrair.", L"✔ Game ISO found: the game runs straight from it, no extracting.")
                                                   : TR(L"✔ default.xex encontrado.", L"✔ default.xex found."))
                         : ehIso ? TR(L"✘ Este arquivo não é a ISO do jogo (não achei o default.xex dentro dele).",
                                      L"✘ This file is not the game ISO (default.xex not found inside it).")
                                 : TR(L"✘ Esta pasta não tem o default.xex. Escolha a pasta com os arquivos extraídos do jogo ou o arquivo .iso.",
                                      L"✘ This folder has no default.xex. Choose the folder with the extracted game files or the .iso file.");
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

    // o arquivo .iso do disco do jogo (o jogo le direto dele, iso.cpp)
    void ProcurarIso(HWND j)
    {
        IFileOpenDialog* d = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
        DWORD o = 0; d->GetOptions(&o);
        d->SetOptions(o | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        const COMDLG_FILTERSPEC filtro[] = { { L"ISO (*.iso)", L"*.iso" } };
        d->SetFileTypes(1, filtro);
        d->SetTitle(TR(L"ISO do disco do jogo (.iso)", L"Game disc ISO (.iso)"));
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

    const wchar_t* Instrucoes() { return TR(
        L"FAR CRY INSTINCTS PREDATOR RECOMP\r\n"
        L"Versão para PC feita por recompilação estática do jogo de Xbox 360, sem ligação com a Ubisoft ou a\r\n"
        L"Microsoft. Nenhum arquivo do jogo vem junto (fases, gráficos, sons, vídeos): você usa a sua própria cópia.\r\n"
        L"\r\n"
        L"O QUE VOCÊ PRECISA\r\n"
        L"• Windows 10 ou 11 de 64 bits e uma placa de vídeo com DirectX 12.\r\n"
        L"• A SUA cópia do jogo Far Cry Instincts Predator para Xbox 360 (versão USA/Europa): o arquivo .iso do\r\n"
        L"  disco (o jogo roda direto dele) ou os arquivos extraídos numa pasta (a pasta precisa ter o default.xex\r\n"
        L"  e os arquivos .dat). Nenhum arquivo do jogo vem junto com este programa.\r\n"
        L"\r\n"
        L"COMO JOGAR\r\n"
        L"1. Em \"Pasta do jogo\", clique em Arquivo ISO e escolha o .iso do disco, ou em Pasta e escolha a pasta\r\n"
        L"   com o default.xex.\r\n"
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
        L"IDIOMA\r\n"
        L"• Escolha no canto de cima (IDIOMA • LANGUAGE). Em português, os menus e textos do jogo também ficam em\r\n"
        L"  português (a tradução é aplicada na hora, sem mudar os arquivos da sua cópia).\r\n"
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
        L"• F2: opções de vídeo (mudam na hora)\r\n"
        L"• F3: soltar/prender o mouse na janela\r\n"
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
        L"• YOUR copy of Far Cry Instincts Predator for Xbox 360 (USA/Europe version): the disc .iso file (the\r\n"
        L"  game runs straight from it) or the files extracted to a folder (it must have default.xex and the .dat\r\n"
        L"  files). No game files come with this program.\r\n"
        L"\r\n"
        L"HOW TO PLAY\r\n"
        L"1. In \"Game folder\", click ISO file and choose the disc .iso, or Folder and choose the folder with\r\n"
        L"   default.xex.\r\n"
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
        L"LANGUAGE\r\n"
        L"• Choose it at the top (IDIOMA • LANGUAGE). In Portuguese the game's menus and texts are in Portuguese too\r\n"
        L"  (applied on the fly, your copy's files are not changed); in English the game keeps its original texts.\r\n"
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
        L"• F2: video options (they change right away)\r\n"
        L"• F3: release/capture the mouse in the window\r\n"
        L"• + and -: mouse sensitivity\r\n"
        L"• Alt+Enter: full screen / window\r\n"
        L"• F12: save a problem report (bugs folder) when something goes wrong\r\n"
        L"\r\n"
        L"WHERE THINGS ARE\r\n"
        L"• Saved games: \"save\" folder next to PredatorGame.exe (other profiles in save\\perfis).\r\n"
        L"• Options: opcoes.ini and controles.ini (use the launcher instead of editing them).\r\n"
        L"• Log for reporting problems: fcrecomp_log.txt.\r\n"); }


    LRESULT CALLBACK ProcInstrucoes(HWND j, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_CREATE)
        {
            RECT r; GetClientRect(j, &r);
            HWND e = CreateWindowExW(0, L"EDIT", Instrucoes(), WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
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
            HWND c = CreateWindowExW(0, L"STATIC", (std::wstring(TR(L"Autor: bugcoder00\r\nVersão ",
                                                      L"Author: bugcoder00\r\nVersion ")) + VERSAO_RECOMP).c_str(), WS_CHILD | WS_VISIBLE | SS_CENTER,
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
        g_op.msaa = std::max(0, (int)SendDlgItemMessageW(j, ID_MSAA, CB_GETCURSEL, 0, 0));
        g_op.escala = std::max(0, (int)SendDlgItemMessageW(j, ID_ESCALA, CB_GETCURSEL, 0, 0));
        g_op.fsr = FsrDaLista(std::max(0, (int)SendDlgItemMessageW(j, ID_FSR, CB_GETCURSEL, 0, 0)));
        g_op.fsrNitidez = std::max(0, (int)SendDlgItemMessageW(j, ID_FSR_NITIDEZ, CB_GETCURSEL, 0, 0));
        g_op.sombras = std::max(0, (int)SendDlgItemMessageW(j, ID_SOMBRAS, CB_GETCURSEL, 0, 0));
        g_op.fps = std::max(0, (int)SendDlgItemMessageW(j, ID_FPS, CB_GETCURSEL, 0, 0));
        g_op.filtroTextura = std::max(0, (int)SendDlgItemMessageW(j, ID_FILTRO_TEX, CB_GETCURSEL, 0, 0));
        // listas com os niveis menores no topo: item = valor + 2 (o valor 0 continua sendo o original no ini antigo)
        g_op.distancia = std::max(0, (int)SendDlgItemMessageW(j, ID_DISTANCIA, CB_GETCURSEL, 0, 0)) - 2;
        g_op.detalhe = std::max(0, (int)SendDlgItemMessageW(j, ID_DETALHE, CB_GETCURSEL, 0, 0)) - 2;
        g_op.ao = std::max(0, (int)SendDlgItemMessageW(j, ID_AO, CB_GETCURSEL, 0, 0));
        g_op.formato = std::max(0, (int)SendDlgItemMessageW(j, ID_FORMATO, CB_GETCURSEL, 0, 0));
        g_op.vsync = g_marcado[ID_VSYNC];
        g_op.autoHdr = g_marcado[ID_AUTOHDR];
        g_op.hdrFalso = g_marcado[ID_HDRFALSO];
        g_op.mostrarFps = g_marcado[ID_MOSTRARFPS];
        g_op.avisoTeclas = g_marcado[ID_AVISOTECLAS];
        g_op.mostrarRegistro = g_marcado[ID_REGISTRO];
        g_op.pularLauncher = g_marcado[ID_DIRETO];
        bool* trapacas[] = { &g_op.trapacaMunicao, &g_op.trapacaAdrenalina, &g_op.trapacaFeral, &g_op.trapacaFases, &g_op.trapacaMapas, &g_op.trapacaInvencivel };
        for (int i = 0; i < 6; i++) *trapacas[i] = g_marcado[ID_TRAPACA + i];
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
            if (IDIOMA_INGLES)   // as mensagens de upnp.cpp sao em portugues: traduz as conhecidas
            {
                static const std::pair<const char*, const wchar_t*> trad[] = {
                    { "o roteador nao respondeu", L"the router did not answer (UPnP is off on the router, or Windows blocked the search)" },
                    { "o roteador respondeu, mas", L"the router answered, but it does not have the port opening service" },
                    { "nao foi possivel procurar", L"could not search for the router" },
                    { "o roteador nao informou o IP", L"the router did not report the internet IP" },
                };
                for (auto& [pt, en] : trad)
                    if (erro.rfind(pt, 0) == 0)
                    {
                        e = en;
                        if (const size_t c = erro.find("(codigo "); c != std::string::npos)
                            e += L" (code " + std::wstring(erro.begin() + c + 8, erro.end());
                        break;
                    }
            }
            t = TR(L"Não deu para perguntar o IP ao roteador (", L"Could not ask the router for the IP (") + e +
                TR(L").\r\n\r\nO IP de internet aparece na página de "
                   L"configuração do roteador (ou pesquise \"meu ip\" no navegador). Sem o UPnP do roteador as portas também "
                   L"precisam ser abertas à mão: veja o LEIA-ME.",
                   L").\r\n\r\nThe internet IP shows on the router's settings page (or search \"my ip\" in the browser). "
                   L"Without the router's UPnP the ports also have to be opened by hand: see the README.");
            MessageBoxW(j, t.c_str(), TR(L"Meu IP", L"My IP"), MB_ICONWARNING);
        }
    }

    // ---------------------------------------------------------------- imagens (PNG -> bitmap) com o GDI+
    // desenha o PNG em lado x lado sobre a cor de fundo; cinza = sem cor e escurecido
    HBITMAP PngParaBitmap(const std::vector<uint8_t>& png, int lado, COLORREF fundo)
    {
        if (png.empty()) return nullptr;
        IStream* st = SHCreateMemStream(png.data(), (UINT)png.size());
        if (!st) return nullptr;
        HBITMAP r = nullptr;
        {
            Gdiplus::Bitmap img(st);
            if (img.GetLastStatus() == Gdiplus::Ok)
            {
                Gdiplus::Bitmap tela(lado, lado, PixelFormat32bppRGB);
                Gdiplus::Graphics g(&tela);
                g.Clear(Gdiplus::Color(GetRValue(fundo), GetGValue(fundo), GetBValue(fundo)));
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                g.DrawImage(&img, 0, 0, lado, lado);
                tela.GetHBITMAP(Gdiplus::Color(0, 0, 0), &r);
            }
        }
        st->Release();
        return r;
    }

    void DesenharBitmap(HDC dc, HBITMAP b, int x, int y, int lado, int origem = 0)
    {
        HDC m = CreateCompatibleDC(dc);
        HGDIOBJ a = SelectObject(m, b);
        if (origem && origem != lado)
        {
            SetStretchBltMode(dc, HALFTONE);
            StretchBlt(dc, x, y, lado, lado, m, 0, 0, origem, origem, SRCCOPY);
        }
        else BitBlt(dc, x, y, lado, lado, m, 0, 0, SRCCOPY);
        SelectObject(m, a);
        DeleteDC(m);
    }

    HFONT g_fonteNome;   // nome do save / da conquista

    // perfil escolhido na tela agora ("" = o principal, pasta save)
    std::string PerfilDaTela(HWND j)
    {
        wchar_t pf[64] = {}, primeiro[64] = {};
        GetDlgItemTextW(j, ID_PERFIL, pf, 64);
        SendDlgItemMessageW(j, ID_PERFIL, CB_GETLBTEXT, 0, (LPARAM)primeiro);
        std::string perfil;
        for (wchar_t* c = pf; *c; c++) if (*c < 127) perfil += (char)*c;
        perfil = PerfilValido(perfil);
        return !wcscmp(pf, primeiro) || perfil == PerfilPrincipal() ? "" : perfil;
    }

    // ---------------------------------------------------------------- lista ou grade de icones
    // As duas listas (saves e conquistas) sao LISTBOX desenhadas aqui. Na vista de icones cada linha da LISTBOX tem
    // varios itens lado a lado (POR_LINHA); a vista fica em opcoes.ini (launcher_icones).
    constexpr int POR_LINHA_SAVES = 5, POR_LINHA_CONQ = 6;
    int g_saveSel = -1;   // save escolhido (na grade a LISTBOX so sabe a linha)

    void PreencherLinhas(HWND lb, int n, int porLinha, int altura)
    {
        SendMessageW(lb, WM_SETREDRAW, FALSE, 0);
        SendMessageW(lb, LB_RESETCONTENT, 0, 0);
        SendMessageW(lb, LB_SETITEMHEIGHT, 0, altura);
        const int linhas = g_op.launcherIcones ? (n + porLinha - 1) / porLinha : n;
        for (int i = 0; i < linhas; i++) SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)L"");
        SendMessageW(lb, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(lb, nullptr, TRUE);
    }

    void DesenharLinhaOuGrade(const DRAWITEMSTRUCT* d, int n, int porLinha, void (*linha)(HDC, RECT, int), void (*icone)(HDC, RECT, int))
    {
        RECT r = d->rcItem;
        FillRect(d->hDC, &r, PincelCartao());
        if ((int)d->itemID < 0) return;
        if (!g_op.launcherIcones)
        {
            if ((int)d->itemID < n) linha(d->hDC, r, (int)d->itemID);
            return;
        }
        const int w = (r.right - r.left) / porLinha;
        for (int c = 0; c < porLinha; c++)
        {
            int i = (int)d->itemID * porLinha + c;
            if (i >= n) break;
            RECT t{ r.left + c * w, r.top, r.left + (c + 1) * w, r.bottom };
            icone(d->hDC, t, i);
        }
    }

    // ---------------------------------------------------------------- jogos salvos do perfil
    // cada save e uma pasta save[\perfis\<nome>]\conteudo\00000001\<arquivo> com _nome.txt (Latin-1) e _miniatura.png
    struct SaveItem { std::filesystem::path dir; std::wstring nome, quando; HBITMAP mini = nullptr; bool mapa = false; };
    std::vector<SaveItem> g_saves;
    // filtro da aba PERFIL: 0 = tudo, 1 = so jogos salvos, 2 = so mapas do editor. Os dois ficam na mesma pasta
    // (conteudo 00001); o FC2_Save do mapa comeca com 00000001, o do jogo salvo com 00000000.
    int g_filtroSaves = 0;
    const wchar_t* TextoFiltro()
    {
        return g_filtroSaves == 1 ? TR(L"SÓ SAVES", L"SAVES ONLY") : g_filtroSaves == 2 ? TR(L"SÓ MAPAS", L"MAPS ONLY") : TR(L"TUDO", L"ALL");
    }

    std::wstring Latin1(const std::string& s) { std::wstring w; for (unsigned char c : s) w += (wchar_t)c; return w; }
    std::string ParaLatin1(const std::wstring& w) { std::string s; for (wchar_t c : w) s += c < 256 ? (char)c : '?'; return s; }

    void LimparSaves()
    {
        for (auto& it : g_saves) if (it.mini) DeleteObject(it.mini);
        g_saves.clear();
    }

    void CarregarSaves(HWND j)
    {
        HWND lb = GetDlgItem(j, ID_SAVES);
        if (!lb) return;
        LimparSaves();
        std::error_code ec;
        const auto pasta = PastaDoPerfil(g_exeDir, PerfilDaTela(j)) / "conteudo" / "00000001";
        std::vector<std::pair<std::filesystem::file_time_type, SaveItem>> v;
        for (auto& d : std::filesystem::directory_iterator(pasta, ec))
        {
            if (!d.is_directory()) continue;
            SaveItem it;
            it.dir = d.path();
            std::ifstream n(it.dir / "_nome.txt", std::ios::binary);
            std::string nome; std::getline(n, nome);
            while (!nome.empty() && (nome.back() == '\r' || nome.back() == '\n')) nome.pop_back();
            it.nome = nome.empty() ? it.dir.filename().wstring() : Latin1(nome);
            auto t = std::filesystem::last_write_time(std::filesystem::exists(it.dir / "FC2_Save", ec) ? it.dir / "FC2_Save" : it.dir, ec);
            // data e hora local
            const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
            const time_t tt = std::chrono::system_clock::to_time_t(sys);
            tm lt{}; localtime_s(&lt, &tt);
            wchar_t q[64]; wcsftime(q, 64, TR(L"%d/%m/%Y às %H:%M", L"%m/%d/%Y at %H:%M"), &lt);
            it.quando = q;
            std::ifstream f(it.dir / "_miniatura.png", std::ios::binary);
            std::vector<uint8_t> png((std::istreambuf_iterator<char>(f)), {});
            {
                std::ifstream c(it.dir / "FC2_Save", std::ios::binary);
                unsigned char cab[4] = {};
                c.read((char*)cab, 4);
                it.mapa = c.gcount() == 4 && cab[0] == 0 && cab[1] == 0 && cab[2] == 0 && cab[3] == 1;
            }
            if ((g_filtroSaves == 1 && it.mapa) || (g_filtroSaves == 2 && !it.mapa)) continue;
            it.mini = PngParaBitmap(png, S(96), CAMPO);   // grade: 96; lista: reduzida para 64
            v.push_back({ t, std::move(it) });
        }
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });   // o mais novo primeiro
        for (auto& [t, it] : v) g_saves.push_back(std::move(it));
        g_saveSel = -1;
        PreencherLinhas(lb, (int)g_saves.size(), POR_LINHA_SAVES, S(g_op.launcherIcones ? 168 : 80));
        for (int id = ID_RENOMEAR; id <= ID_ABRIRSAVE; id++) EnableWindow(GetDlgItem(j, id), FALSE);
        InvalidateRect(j, nullptr, FALSE);
    }

    int SaveSelecionado(HWND) { return g_saveSel >= 0 && g_saveSel < (int)g_saves.size() ? g_saveSel : -1; }

    // save debaixo do mouse (na grade a linha da lista tem varios; a coluna sai da posicao do clique)
    int SaveNoMouse(HWND lb)
    {
        int linha = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0);
        if (linha < 0) return -1;
        if (!g_op.launcherIcones) return linha < (int)g_saves.size() ? linha : -1;
        DWORD pos = GetMessagePos();
        POINT pt{ (short)LOWORD(pos), (short)HIWORD(pos) };
        ScreenToClient(lb, &pt);
        RECT rc; GetClientRect(lb, &rc);
        int col = std::clamp((int)(pt.x * POR_LINHA_SAVES / std::max(1L, rc.right)), 0, POR_LINHA_SAVES - 1);
        int i = linha * POR_LINHA_SAVES + col;
        return i < (int)g_saves.size() ? i : -1;
    }

    // um quadradinho da grade: miniatura grande em cima, nome e data embaixo
    void DesenharSaveIcone(HDC dc, RECT r, int idx)
    {
        const SaveItem& it = g_saves[idx];
        const bool sel = idx == g_saveSel;
        RECT caixa{ r.left + S(6), r.top + S(6), r.right - S(6), r.bottom - S(6) };
        Arredondado(dc, caixa, S(10), CAMPO, sel ? AMBAR : LINHA, sel ? S(2) : 1);
        const int lado = S(96), x = (caixa.left + caixa.right - lado) / 2, y = caixa.top + S(10);
        if (it.mini) DesenharBitmap(dc, it.mini, x, y, lado);
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ antes = SelectObject(dc, g_fonte);
        SetTextColor(dc, sel ? AMBAR : TEXTO);
        RECT rt{ caixa.left + S(6), y + lado + S(6), caixa.right - S(6), y + lado + S(26) };
        DrawTextW(dc, it.nome.c_str(), -1, &rt, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextColor(dc, TEXTO_FRACO);
        RECT rq{ rt.left, rt.bottom, rt.right, rt.bottom + S(20) };
        std::wstring q = it.quando.substr(0, 10);   // so a data
        DrawTextW(dc, q.c_str(), -1, &rq, DT_CENTER | DT_SINGLELINE);
        SelectObject(dc, antes);
    }

    void DesenharSaveLinha(HDC dc, RECT r, int idx)
    {
        const bool sel = idx == g_saveSel;
        const SaveItem& it = g_saves[idx];
        RECT fundo{ r.left + S(4), r.top + S(3), r.right - S(4), r.bottom - S(3) };
        Arredondado(dc, fundo, S(8), sel ? CAMPO : CARTAO, sel ? AMBAR_ESC : CLR_NONE);
        if (sel) Arredondado(dc, RECT{ fundo.left + S(6), fundo.top + S(14), fundo.left + S(10), fundo.bottom - S(14) }, S(2), AMBAR);
        const int lado = S(64), x = r.left + S(18), y = r.top + (r.bottom - r.top - lado) / 2;
        if (it.mini) DesenharBitmap(dc, it.mini, x, y, lado, S(96));
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ antes = SelectObject(dc, g_fonteNome);
        SetTextColor(dc, sel ? AMBAR : TEXTO);
        RECT rt{ x + lado + S(16), r.top + S(14), r.right - S(10), r.top + S(40) };
        DrawTextW(dc, it.nome.c_str(), -1, &rt, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, g_fonte);
        SetTextColor(dc, TEXTO_FRACO);
        RECT rq{ rt.left, r.top + S(42), r.right - S(10), r.bottom - S(6) };
        std::wstring linha = TR(L"Salvo em ", L"Saved on ") + it.quando;
        DrawTextW(dc, linha.c_str(), -1, &rq, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, antes);
    }

    void DesenharSave(const DRAWITEMSTRUCT* d)
    {
        DesenharLinhaOuGrade(d, (int)g_saves.size(), POR_LINHA_SAVES, DesenharSaveLinha, DesenharSaveIcone);
    }

    constexpr UINT WM_RENOMEADO = WM_APP + 1;   // resposta da caixa de digitacao (teclado.cpp), em outra thread

    void RenomearSave(HWND j)
    {
        int i = SaveSelecionado(j);
        if (i < 0) return;
        EnableWindow(j, FALSE);
        auto dir = new std::filesystem::path(g_saves[i].dir);
        TecladoAbrir(TR(L"RENOMEAR", L"RENAME"), TR(L"Novo nome do jogo salvo:", L"New name for the saved game:"), g_saves[i].nome, 32,
                     [j, dir](bool ok, const std::wstring& texto) {
                         PostMessageW(j, WM_RENOMEADO, ok, (LPARAM)new std::pair<std::filesystem::path, std::wstring>(*dir, texto));
                         delete dir;
                     });
    }

    void DuplicarSave(HWND j)
    {
        int i = SaveSelecionado(j);
        if (i < 0) return;
        const auto& orig = g_saves[i].dir;
        std::error_code ec;
        std::filesystem::path novo;
        for (int n = 2; n < 1000; n++)   // nome de arquivo do conteudo: ate 41 letras
        {
            std::string nome = orig.filename().string().substr(0, 36) + "_" + std::to_string(n);
            novo = orig.parent_path() / nome;
            if (!std::filesystem::exists(novo, ec)) break;
        }
        std::filesystem::copy(orig, novo, std::filesystem::copy_options::recursive, ec);
        if (ec)
        {
            MessageBoxW(j, TR(L"Não deu para copiar o jogo salvo.", L"Could not copy the saved game."), TR(L"Duplicar", L"Duplicate"), MB_ICONWARNING);
            return;
        }
        std::wstring nome = g_saves[i].nome.substr(0, 24) + TR(L" (cópia)", L" (copy)");
        std::ofstream(novo / "_nome.txt", std::ios::binary) << ParaLatin1(nome);
        std::filesystem::last_write_time(novo / "FC2_Save", std::filesystem::file_time_type::clock::now(), ec);
        CarregarSaves(j);
    }

    void ApagarSave(HWND j)
    {
        int i = SaveSelecionado(j);
        if (i < 0) return;
        std::wstring p = TR(L"Apagar o jogo salvo \"", L"Delete the saved game \"") + g_saves[i].nome +
                         TR(L"\"?\r\n\r\nEle vai para a Lixeira do Windows (dá para recuperar de lá).",
                            L"\"?\r\n\r\nIt goes to the Windows Recycle Bin (it can be restored from there).");
        if (MessageBoxW(j, p.c_str(), TR(L"Apagar", L"Delete"), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        std::wstring de = g_saves[i].dir.wstring();
        de.push_back(L'\0');   // lista terminada com dois zeros
        SHFILEOPSTRUCTW op{};
        op.hwnd = j; op.wFunc = FO_DELETE; op.pFrom = de.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
        if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted)
            MessageBoxW(j, TR(L"Não deu para apagar o jogo salvo.", L"Could not delete the saved game."), TR(L"Apagar", L"Delete"), MB_ICONWARNING);
        CarregarSaves(j);
    }

    // ---------------------------------------------------------------- conquistas
    std::vector<Conquista> g_conq;
    std::map<uint16_t, int64_t> g_conqFeitas;
    std::map<uint32_t, HBITMAP> g_conqIcone;
    bool g_conqLidas = false;
    int g_conqPontos = 0, g_conqTotal = 0;   // barra de pontos

    // trofeu desenhado aqui (taca, alcas, haste e base); cor de platina quando tem todos os pontos, apagado antes
    void DesenharTrofeu(HDC dc, int x, int y, int lado, bool completo)
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const float u = lado / 32.0f;
        auto P = [&](float a, float b) { return Gdiplus::PointF(x + a * u, y + b * u); };
        Gdiplus::Color claro = completo ? Gdiplus::Color(255, 240, 244, 252) : Gdiplus::Color(255, 92, 86, 76);
        Gdiplus::Color escuro = completo ? Gdiplus::Color(255, 130, 142, 168) : Gdiplus::Color(255, 52, 48, 42);
        Gdiplus::LinearGradientBrush pincel(P(4, 2), P(28, 30), claro, escuro);
        Gdiplus::Pen contorno(completo ? Gdiplus::Color(255, 90, 100, 125) : Gdiplus::Color(255, 40, 37, 32), 1.2f * u);
        // alcas
        Gdiplus::Pen alca(completo ? Gdiplus::Color(255, 190, 198, 215) : Gdiplus::Color(255, 70, 65, 58), 2.4f * u);
        g.DrawArc(&alca, x + 2 * u, y + 5 * u, 9 * u, 10 * u, 90.0f, 180.0f);
        g.DrawArc(&alca, x + 21 * u, y + 5 * u, 9 * u, 10 * u, 270.0f, 180.0f);
        // taca
        Gdiplus::GraphicsPath taca;
        taca.AddLine(P(7, 3), P(25, 3));
        taca.AddBezier(P(25, 3), P(25, 14), P(21, 19), P(16, 19));
        taca.AddBezier(P(16, 19), P(11, 19), P(7, 14), P(7, 3));
        taca.CloseFigure();
        g.FillPath(&pincel, &taca);
        g.DrawPath(&contorno, &taca);
        // haste e base
        Gdiplus::PointF haste[] = { P(14, 19), P(18, 19), P(19, 25), P(13, 25) };
        g.FillPolygon(&pincel, haste, 4);
        g.DrawPolygon(&contorno, haste, 4);
        Gdiplus::RectF base(x + 9 * u, y + 25 * u, 14 * u, 4.5f * u);
        g.FillRectangle(&pincel, base);
        g.DrawRectangle(&contorno, base);
        if (completo)   // brilho
        {
            Gdiplus::SolidBrush brilho(Gdiplus::Color(200, 255, 255, 255));
            g.FillEllipse(&brilho, x + 10 * u, y + 5 * u, 3 * u, 7 * u);
        }
    }

    // barra de pontos da aba CONQUISTAS (embaixo do resumo), com o trofeu no fim
    void DesenharBarraPontos(HDC dc)
    {
        if (!g_conqTotal) return;
        const int lado = S(34), tx = S(1136) - lado, x0 = S(PAINEL_X), x1 = tx - S(12), y0 = S(184), y1 = S(204);
        RECT fundo{ x0, y0, x1, y1 };
        const int raio = (y1 - y0) / 2;
        Arredondado(dc, fundo, raio, CAMPO, LINHA);
        const int cheio = x0 + (int)((long long)(x1 - x0) * g_conqPontos / g_conqTotal);
        if (cheio > x0)
        {
            // vermelho -> ambar, so dentro da pilula
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            Gdiplus::GraphicsPath p;
            const float w = (float)std::max(cheio - x0, 2 * raio), d = 2.0f * raio, hh = (float)(y1 - y0);
            p.AddArc((float)x0, (float)y0, d, hh, 90.0f, 180.0f);
            p.AddArc(x0 + w - d, (float)y0, d, hh, 270.0f, 180.0f);
            p.CloseFigure();
            Gdiplus::LinearGradientBrush b(Gdiplus::PointF((float)x0, 0), Gdiplus::PointF((float)x1, 0),
                                           Gdiplus::Color(255, 178, 36, 26), Gdiplus::Color(255, 236, 174, 52));
            g.FillPath(&b, &p);
        }
        wchar_t t[64]; swprintf(t, 64, L"%d / %d G  (%d%%)", g_conqPontos, g_conqTotal, g_conqPontos * 100 / g_conqTotal);
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ antes = SelectObject(dc, g_fonte);
        SetTextColor(dc, RGB(255, 250, 240));
        DrawTextW(dc, t, -1, &fundo, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, antes);
        DesenharTrofeu(dc, tx, (y0 + y1) / 2 - lado / 2, lado, g_conqPontos >= g_conqTotal);
    }

    void CarregarConquistas(HWND j)
    {
        HWND lb = GetDlgItem(j, ID_CONQ);
        if (!lb) return;
        if (!g_conqLidas)
        {
            wchar_t b[1024] = {}; GetDlgItemTextW(j, ID_PASTA, b, 1024);
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32514)));   // ampulheta (montar a imagem leva ~1 s)
            std::map<uint32_t, std::vector<uint8_t>> imgs;
            g_conqLidas = ConquistasLer(g_exeDir, b, g_conq, imgs);
            for (auto& [id, png] : imgs) g_conqIcone[id] = PngParaBitmap(png, S(64), CAMPO);
        }
        g_conqFeitas = ConquistasDesbloqueadas(PastaDoPerfil(g_exeDir, PerfilDaTela(j)));
        // desbloqueadas primeiro (a mais recente no topo), depois a ordem do jogo
        std::stable_sort(g_conq.begin(), g_conq.end(), [](const Conquista& a, const Conquista& b) {
            auto fa = g_conqFeitas.find(a.id), fb = g_conqFeitas.find(b.id);
            const bool ta = fa != g_conqFeitas.end(), tb = fb != g_conqFeitas.end();
            if (ta != tb) return ta;
            if (ta) return fa->second > fb->second;
            return a.id < b.id;
        });
        PreencherLinhas(lb, (int)g_conq.size(), POR_LINHA_CONQ, S(g_op.launcherIcones ? 150 : 84));
        int feitas = 0, pontos = 0, total = 0;
        for (auto& c : g_conq)
        {
            total += c.pontos;
            if (g_conqFeitas.count(c.id)) { feitas++; pontos += c.pontos; }
        }
        g_conqPontos = pontos; g_conqTotal = total;
        InvalidateRect(j, nullptr, FALSE);
        wchar_t r[256];
        if (g_conq.empty())
            swprintf(r, 256, L"%ls", TR(L"Não deu para ler as conquistas: escolha a pasta do jogo em PASTA DO JOGO.",
                                        L"Could not read the achievements: choose the game folder in GAME FOLDER."));
        else
            swprintf(r, 256, TR(L"%d de %d conquistas desbloqueadas  •  %d de %d pontos  •  perfil %ls",
                                L"%d of %d achievements unlocked  •  %d of %d points  •  profile %ls"),
                     feitas, (int)g_conq.size(), pontos, total, PerfilDaTela(j).empty() ? L"User 1" : std::wstring(PerfilDaTela(j).begin(), PerfilDaTela(j).end()).c_str());
        SetDlgItemTextW(j, ID_CONQ_RESUMO, r);
    }

    // icone da conquista (desbloqueada) ou o quadro escuro com cadeado (bloqueada), com moldura
    void IconeConquista(HDC dc, const Conquista& c, bool tem, int x, int y, int lado)
    {
        if (tem && g_conqIcone[c.imagem]) DesenharBitmap(dc, g_conqIcone[c.imagem], x, y, lado);
        else
        {
            RECT q{ x, y, x + lado, y + lado };
            HBRUSH e = CreateSolidBrush(FUNDO);
            FillRect(dc, &q, e);
            DeleteObject(e);
            static HFONT simbolo = CreateFontW(-S(30), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Symbol");
            HGDIOBJ a = SelectObject(dc, simbolo);
            SetTextColor(dc, AMBAR_ESC);
            DrawTextW(dc, L"\U0001F512", -1, &q, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, a);
        }
        RECT moldura{ x - 1, y - 1, x + lado + 1, y + lado + 1 };
        HBRUSH b = CreateSolidBrush(tem ? AMBAR : AMBAR_ESC);
        FrameRect(dc, &moldura, b);
        DeleteObject(b);
    }

    // grade: icone, nome (ate 2 linhas) e pontos; clicando, a explicacao aparece na linha do resumo
    void DesenharConquistaIcone(HDC dc, RECT r, int idx)
    {
        const Conquista& c = g_conq[idx];
        const bool tem = g_conqFeitas.count(c.id) > 0;
        RECT caixa{ r.left + S(5), r.top + S(5), r.right - S(5), r.bottom - S(5) };
        Arredondado(dc, caixa, S(10), tem ? RGB(44, 38, 28) : CAMPO, tem ? AMBAR_ESC : LINHA);
        const int lado = S(64), x = (caixa.left + caixa.right - lado) / 2, y = caixa.top + S(10);
        SetBkMode(dc, TRANSPARENT);
        IconeConquista(dc, c, tem, x, y, lado);
        HGDIOBJ antes = SelectObject(dc, g_fonte);
        SetTextColor(dc, tem ? AMBAR : TEXTO);
        RECT rt{ caixa.left + S(4), y + lado + S(6), caixa.right - S(4), y + lado + S(44) };
        DrawTextW(dc, c.nome.c_str(), -1, &rt, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
        wchar_t pts[16]; swprintf(pts, 16, L"%u G", c.pontos);
        SetTextColor(dc, tem ? VERDE : TEXTO_FRACO);
        RECT rp{ caixa.left, caixa.bottom - S(22), caixa.right, caixa.bottom - S(4) };
        DrawTextW(dc, pts, -1, &rp, DT_CENTER | DT_SINGLELINE);
        SelectObject(dc, antes);
    }

    void DesenharConquistaLinha(HDC dc, RECT r, int idx)
    {
        const Conquista& c = g_conq[idx];
        auto feita = g_conqFeitas.find(c.id);
        const bool tem = feita != g_conqFeitas.end();
        Arredondado(dc, RECT{ r.left + S(4), r.top + S(3), r.right - S(4), r.bottom - S(3) }, S(8), tem ? RGB(36, 32, 25) : CARTAO, tem ? LINHA : CLR_NONE);
        const int lado = S(64), x = r.left + S(14), y = r.top + (r.bottom - r.top - lado) / 2;
        SetBkMode(dc, TRANSPARENT);
        IconeConquista(dc, c, tem, x, y, lado);
        // pontos na direita
        HGDIOBJ antes = SelectObject(dc, g_fonteNome);
        wchar_t pts[16]; swprintf(pts, 16, L"%u G", c.pontos);
        RECT rp{ r.right - S(90), r.top + S(14), r.right - S(14), r.top + S(40) };
        SetTextColor(dc, tem ? AMBAR : TEXTO_FRACO);
        DrawTextW(dc, pts, -1, &rp, DT_RIGHT | DT_SINGLELINE);
        RECT rt{ x + lado + S(16), r.top + S(10), r.right - S(100), r.top + S(34) };
        SetTextColor(dc, tem ? AMBAR : TEXTO);
        DrawTextW(dc, c.nome.c_str(), -1, &rt, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, g_fonte);
        SetTextColor(dc, TEXTO_FRACO);
        RECT rd{ rt.left, r.top + S(36), r.right - S(100), r.top + S(56) };
        DrawTextW(dc, c.comoObter.c_str(), -1, &rd, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT rq{ rt.left, r.top + S(56), r.right - S(100), r.bottom - S(4) };
        if (tem)
        {
            const time_t tt = (time_t)feita->second;
            tm lt{}; localtime_s(&lt, &tt);
            wchar_t q[64]; wcsftime(q, 64, TR(L"✔ Desbloqueada em %d/%m/%Y às %H:%M", L"✔ Unlocked on %m/%d/%Y at %H:%M"), &lt);
            SetTextColor(dc, VERDE);
            DrawTextW(dc, q, -1, &rq, DT_LEFT | DT_SINGLELINE);
        }
        else
        {
            SetTextColor(dc, AMBAR_ESC);
            DrawTextW(dc, TR(L"Bloqueada", L"Locked"), -1, &rq, DT_LEFT | DT_SINGLELINE);
        }
        SelectObject(dc, antes);
    }

    void DesenharConquista(const DRAWITEMSTRUCT* d)
    {
        DesenharLinhaOuGrade(d, (int)g_conq.size(), POR_LINHA_CONQ, DesenharConquistaLinha, DesenharConquistaIcone);
    }

    void MostrarCategoria(HWND j, int cat)
    {
        if (cat == CAT_CONQUISTAS) CarregarConquistas(j);
        g_categoria = cat;
        for (auto& [c, k] : g_daCategoria) ShowWindow(c, k == cat ? SW_SHOW : SW_HIDE);
        for (int i = 0; i < N_CAT; i++) InvalidateRect(GetDlgItem(j, ID_CAT + i), nullptr, FALSE);
        InvalidateRect(j, nullptr, FALSE);
    }

    // guarda so a vista (lista/icones) sem mexer no resto do que esta na tela: le o opcoes.ini atual e regrava
    void OpcoesSalvarSoVista()
    {
        const bool icones = g_op.launcherIcones;
        Opcoes copia = g_op;
        OpcoesCarregar(g_exeDir);
        g_op.launcherIcones = icones;
        OpcoesSalvar();
        g_op = copia;
    }

    bool Preparar(HWND j)
    {
        LerTela(j);
        if (!PastaValida(g_op.pastaJogo))
        {
            MostrarCategoria(j, CAT_PASTA);
            MessageBoxW(j, TR(L"Escolha a pasta com os arquivos extraídos do jogo (precisa ter o default.xex) ou o arquivo .iso do disco.",
                              L"Choose the folder with the extracted game files (it must have default.xex) or the disc .iso file."),
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

    // fundo da janela: faixa do topo (icone, titulo, idioma) com a linha vermelho -> ambar embaixo, o menu da esquerda
    // direto no fundo e o painel da categoria num cartao de cantos arredondados (CARTAO)
    void PintarFundo(HDC dc, int w, int h)
    {
        RECT tudo{ 0, 0, w, h };
        FillRect(dc, &tudo, PincelFundo());
        const int topo = S(TOPO_FAIXA);
        TRIVERTEX v[2] = { { 0, 0, 0x2600, 0x2000, 0x1700, 0 }, { w, topo, 0x1000, 0x0E00, 0x0B00, 0 } };
        GRADIENT_RECT gr{ 0, 1 };
        GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
        // linha fina embaixo da faixa: vermelho na esquerda, ambar no meio, sumindo na direita
        TRIVERTEX l1[2] = { { 0, topo, 0xB200, 0x2400, 0x1A00, 0 }, { w / 2, topo + S(2), 0xEC00, 0xAE00, 0x3400, 0 } };
        TRIVERTEX l2[2] = { { w / 2, topo, 0xEC00, 0xAE00, 0x3400, 0 }, { w, topo + S(2), 0x1000, 0x0E00, 0x0B00, 0 } };
        GradientFill(dc, l1, 2, &gr, 1, GRADIENT_FILL_RECT_H);
        GradientFill(dc, l2, 2, &gr, 1, GRADIENT_FILL_RECT_H);
        // arte do topo (recurso 2, tools\gerar_icone.py): o "PR" do icone na mesma escala, sem o quadrado e a borda, com
        // as marcas de garra pintadas na faixa (transparentes, somem antes do titulo). Sem ela, o icone como antes
        static Gdiplus::Bitmap* arte = [] () -> Gdiplus::Bitmap* {
            HMODULE m = GetModuleHandleW(nullptr);
            HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(2), MAKEINTRESOURCEW(10));   // 10 = RT_RCDATA
            HGLOBAL g = r ? LoadResource(m, r) : nullptr;
            const void* p = g ? LockResource(g) : nullptr;
            IStream* st = p ? SHCreateMemStream((const BYTE*)p, SizeofResource(m, r)) : nullptr;
            if (!st) return nullptr;
            Gdiplus::Bitmap* copia = nullptr;
            {
                Gdiplus::Bitmap png(st);
                if (png.GetLastStatus() == Gdiplus::Ok)
                {
                    copia = new Gdiplus::Bitmap(png.GetWidth(), png.GetHeight(), PixelFormat32bppPARGB);
                    Gdiplus::Graphics gc(copia);
                    gc.DrawImage(&png, 0, 0, png.GetWidth(), png.GetHeight());
                }
            }
            st->Release();
            return copia;
        }();
        if (arte)
        {
            Gdiplus::Graphics g(dc);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            g.DrawImage(arte, Gdiplus::Rect(0, 0, S(170), topo), 0, 0, arte->GetWidth(), arte->GetHeight(), Gdiplus::UnitPixel);
        }
        else if (g_iconeTopo) DrawIconEx(dc, S(24), (topo - S(56)) / 2, g_iconeTopo, S(56), S(56), 0, nullptr, DI_NORMAL);

        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ fonteAntes = SelectObject(dc, g_fonteGrande);
        SetTextColor(dc, RGB(250, 246, 236));
        SetTextCharacterExtra(dc, S(1));
        TextOutW(dc, S(96), S(14), L"FAR CRY INSTINCTS PREDATOR", 26);
        SelectObject(dc, g_fonteSub);
        SetTextColor(dc, AMBAR);
        SetTextCharacterExtra(dc, S(3));
        const std::wstring subTexto = std::wstring(TR(L"RECOMP  •  VERSÃO PARA PC  •  ", L"RECOMP  •  PC VERSION  •  ")) + VERSAO_RECOMP;
        const wchar_t* sub = subTexto.c_str();
        TextOutW(dc, S(98), S(56), sub, (int)wcslen(sub));
        // rotulo do idioma (a lista fica embaixo, na direita da faixa)
        SetTextColor(dc, TEXTO_FRACO);
        SetTextCharacterExtra(dc, S(2));
        TextOutW(dc, S(IDIOMA_X), S(16), L"IDIOMA  •  LANGUAGE", 19);
        SetTextCharacterExtra(dc, 0);
        SelectObject(dc, fonteAntes);

        // cartao do painel da categoria
        RECT cartao{ S(CARTAO_X), S(TOPO), S(LARGURA - 12), S(RODAPE) };
        Arredondado(dc, cartao, S(12), CARTAO, LINHA);
        // caixas dos campos de texto (o EDIT fica dentro, com margem)
        if (g_categoria == CAT_PASTA)
        {
            const int dx = CATEGORIAS[CAT_PASTA].dx, dy = CATEGORIAS[CAT_PASTA].dy;
            Arredondado(dc, RECT{ S(24 + dx), S(144 + dy), S(280 + dx), S(178 + dy) }, S(6), CAMPO, LINHA);
        }
        if (g_categoria == CAT_INTERNET)
        {
            const int dx = CATEGORIAS[CAT_INTERNET].dx, dy = CATEGORIAS[CAT_INTERNET].dy;
            Arredondado(dc, RECT{ S(DIR + 130 + dx), S(256 + dy), S(DIR + 380 + dx), S(288 + dy) }, S(6), CAMPO, LINHA);   // IP do anfitriao
        }
        Secao(dc, S(24), S(118), S(264), TR(L"OPÇÕES", L"OPTIONS"));
        if (g_categoria == CAT_CONQUISTAS) DesenharBarraPontos(dc);
        if (g_categoria == CAT_PERFIL)
        {
            wchar_t t[96];
            swprintf(t, 96, g_filtroSaves == 1 ? TR(L"JOGOS SALVOS DESTE PERFIL (%d)", L"SAVED GAMES OF THIS PROFILE (%d)")
                          : g_filtroSaves == 2 ? TR(L"MAPAS DESTE PERFIL (%d)", L"MAPS OF THIS PROFILE (%d)")
                                               : TR(L"SAVES E MAPAS DESTE PERFIL (%d)", L"SAVES AND MAPS OF THIS PROFILE (%d)"), (int)g_saves.size());
            Secao(dc, S(PAINEL_X), S(236), S(1136 - 386), t);
        }
        Secao(dc, S(PAINEL_X), S(118), S(1136), CATEGORIAS[g_categoria].titulo());
    }

    LRESULT CALLBACK Proc(HWND j, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_CREATE:
        {
            // ---- coluna da esquerda: pasta, video, outros
            // campo da pasta dentro de uma caixa desenhada no fundo (PintarFundo), com margem
            // menu de categorias; comeca na PASTA DO JOGO enquanto ela nao foi escolhida
            g_daCategoria.clear();
            g_categoria = g_categoriaInicial >= 0 ? g_categoriaInicial : PastaValida(g_op.pastaJogo) ? CAT_VIDEO : CAT_PASTA;
            g_categoriaInicial = -1;
            {
                // idioma do launcher e das janelas do programa (opcoes.ini "idioma"); trocar recria a janela
                HWND cb = Criar(j, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDIOMA_X, 38, 200, 200, ID_IDIOMA);
                SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Português (Brasil)");
                SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"English");
                // jogo em outro idioma do disco (textos do programa em ingles); XGetLanguage em xam.cpp
                SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Français");
                SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Deutsch");
                SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Español");
                SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Italiano");
                SendMessageW(cb, CB_SETCURSEL, g_op.idioma >= 0 && g_op.idioma <= 5 ? g_op.idioma : IDIOMA_INGLES ? 1 : 0, 0);
            }
            for (int i = 0; i < N_CAT; i++) Criar(j, L"BUTTON", CATEGORIAS[i].botao(), WS_TABSTOP, 24, 150 + i * 52, 240, 42, ID_CAT + i);
            g_criando = CAT_PASTA;
            Criar(j, L"EDIT", g_op.pastaJogo.wstring().c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 32, 151, 240, 20, ID_PASTA);
            Criar(j, L"BUTTON", TR(L"PASTA...", L"FOLDER..."), WS_TABSTOP, 288, 144, 116, 34, ID_PROCURAR);
            Criar(j, L"BUTTON", TR(L"ARQUIVO ISO...", L"ISO FILE..."), WS_TABSTOP, 412, 144, 148, 34, ID_ISO);
            Criar(j, L"STATIC", L"", 0, 24, 184, 536, 40, ID_SITUACAO);

            g_criando = CAT_VIDEO;
            Lista(j, ID_JANELA, 262, TR(L"Tamanho da janela:", L"Window size:"),
                  { TR(L"Automático (o maior que cabe na tela)", L"Automatic (the largest that fits the screen)"), L"1280 × 720", L"1600 × 900", L"1920 × 1080", L"2560 × 1440" }, g_op.janela);
            Criar(j, L"BUTTON", TR(L"Tela cheia (Alt+Enter troca durante o jogo)", L"Full screen (Alt+Enter switches during the game)"), WS_TABSTOP, 24, 294, 536, 24, ID_TELACHEIA);
            Lista(j, ID_FORMATO, 322, TR(L"Formato da imagem:", L"Aspect ratio:"),
                  { TR(L"16:9 (como o jogo original)", L"16:9 (like the original game)"),
                    TR(L"Monitor - ultrawide 21:9 / 32:9 (experimental)", L"Monitor - ultrawide 21:9 / 32:9 (experimental)"),
                    TR(L"16:9 com barras pretas (ultrawide sem esticar)", L"16:9 with black bars (ultrawide without stretching)") }, g_op.formato);
            Lista(j, ID_AA, 354, TR(L"Antisserrilhado (FXAA):", L"Anti-aliasing (FXAA):"),
                  { TR(L"Desligado", L"Off"), TR(L"1 - leve", L"1 - light"), TR(L"2 - médio", L"2 - medium"), TR(L"3 - forte (bordas mais lisas)", L"3 - strong (smoother edges)") }, g_op.antisserrilhado);
            Lista(j, ID_MSAA, 386, TR(L"Antisserrilhado MSAA:", L"MSAA anti-aliasing:"),
                  { TR(L"Desligado", L"Off"), L"2x", L"4x", TR(L"8x (mais pesado)", L"8x (heavier)") }, g_op.msaa);
            Lista(j, ID_ESCALA, 418, TR(L"Resolução interna:", L"Internal resolution:"),
                  { TR(L"1x - 1280 × 720 (mais leve)", L"1x - 1280 × 720 (lightest)"), TR(L"1,5x - 1920 × 1080", L"1.5x - 1920 × 1080"),
                    TR(L"2x - 2560 × 1440 (mais nítido, mais pesado)", L"2x - 2560 × 1440 (sharper, heavier)"),
                    TR(L"3x - 3840 × 2160 / 4K (experimental)", L"3x - 3840 × 2160 / 4K (experimental)") }, g_op.escala);
            // FSR ligado: a resolucao interna sai do tamanho da janela (a lista de cima fica desligada)
            Lista(j, ID_FSR, 450, TR(L"Upscale AMD FSR 1:", L"AMD FSR 1 upscaling:"),
                  { TR(L"Desligado", L"Off"), TR(L"Ultra qualidade (desenha em 1x: 1280 × 720, mais nítido)", L"Ultra quality (draws at 1x: 1280 × 720, sharpest)"),
                    TR(L"Qualidade (desenha em 0,75x: 960 × 540)", L"Quality (draws at 0.75x: 960 × 540)"),
                    TR(L"Equilibrado (desenha em 0,5x: 640 × 360)", L"Balanced (draws at 0.5x: 640 × 360)"),
                    TR(L"Desempenho (0,5x, ampliação simples, mais leve)", L"Performance (0.5x, simple upscaling, lightest)") }, ListaDoFsr(g_op.fsr));
            Lista(j, ID_FSR_NITIDEZ, 482, TR(L"Nitidez do FSR:", L"FSR sharpness:"),
                  { TR(L"Desligada", L"Off"), TR(L"Baixa", L"Low"), TR(L"Média", L"Medium"), TR(L"Alta", L"High"),
                    TR(L"Máxima", L"Maximum") }, g_op.fsrNitidez);
            EnableWindow(GetDlgItem(j, ID_ESCALA), g_op.fsr == 0);
            EnableWindow(GetDlgItem(j, ID_FSR_NITIDEZ), g_op.fsr != 0);
            Lista(j, ID_SOMBRAS, 514, TR(L"Sombras:", L"Shadows:"),
                  { L"Normal", TR(L"Alta (bordas mais definidas)", L"High (sharper edges)"), TR(L"Muito alta (mais pesada)", L"Very high (heavier)") }, g_op.sombras);
            Lista(j, ID_FILTRO_TEX, 546, TR(L"Filtro de textura:", L"Texture filtering:"),
                  { TR(L"Original", L"Original"), TR(L"Anisotrópico 2x", L"Anisotropic 2x"), TR(L"Anisotrópico 4x", L"Anisotropic 4x"),
                    TR(L"Anisotrópico 8x", L"Anisotropic 8x"), TR(L"Anisotrópico 16x (chão nítido de longe)", L"Anisotropic 16x (sharp ground at a distance)") }, g_op.filtroTextura);
            Lista(j, ID_DISTANCIA, 578, TR(L"Distância de visão:", L"Draw distance:"),
                  { TR(L"Mínima (0,5x, mais leve)", L"Shortest (0.5x, lighter)"), TR(L"Curta (0,75x)", L"Short (0.75x)"),
                    TR(L"Original", L"Original"), TR(L"Longe (1,5x)", L"Far (1.5x)"), TR(L"Muito longe (2x)", L"Very far (2x)"),
                    TR(L"Máxima (3x, mais pesada)", L"Maximum (3x, heavier)") }, g_op.distancia + 2);
            Lista(j, ID_DETALHE, 610, TR(L"Detalhe de longe:", L"Detail at a distance:"),
                  { TR(L"Mínimo (0,5x, mais leve)", L"Lowest (0.5x, lighter)"), TR(L"Baixo (0,75x)", L"Low (0.75x)"),
                    TR(L"Original", L"Original"), TR(L"Alto (1,5x)", L"High (1.5x)"), TR(L"Muito alto (2x)", L"Very high (2x)"),
                    TR(L"Máximo (3x, mais pesado)", L"Maximum (3x, heavier)") }, g_op.detalhe + 2);
            Lista(j, ID_AO, 642, TR(L"Oclusão de ambiente:", L"Ambient occlusion:"),
                  { TR(L"Desligada (original)", L"Off (original)"), TR(L"Leve", L"Light"), TR(L"Média", L"Medium"),
                    TR(L"Forte (cantos bem marcados)", L"Strong (well-marked corners)") }, g_op.ao);
            Lista(j, ID_FPS, 674, TR(L"Quadros por segundo:", L"Frames per second:"),
                  { TR(L"30 FPS (como o jogo original)", L"30 FPS (like the original game)"), TR(L"60 FPS (mais fluido, pesa o dobro)", L"60 FPS (smoother, twice the load)"),
                    TR(L"Sem limite (experimental)", L"Unlocked (experimental)") }, g_op.fps);
            Criar(j, L"BUTTON", TR(L"Sincronia vertical (evita \"rasgos\" na imagem, pode baixar o FPS)", L"Vertical sync (avoids image \"tearing\", may lower the FPS)"),
                  WS_TABSTOP, 24, 701, 536, 24, ID_VSYNC);
            Criar(j, L"BUTTON", TR(L"Auto HDR (realces mais brilhantes; precisa do HDR ligado no Windows)", L"Auto HDR (brighter highlights; needs HDR turned on in Windows)"),
                  WS_TABSTOP, 24, 726, 536, 24, ID_AUTOHDR);
            Criar(j, L"BUTTON", TR(L"HDR falso (mais contraste e cor em monitor comum, sem HDR)", L"Fake HDR (more contrast and color on a regular monitor, no HDR)"),
                  WS_TABSTOP, 24, 751, 536, 24, ID_HDRFALSO);
            Criar(j, L"STATIC", TR(L"Opções mais altas deixam a imagem mais bonita, mas pesam na placa de vídeo.",
                                   L"Higher options make the image nicer, but weigh on the graphics card."), SS_LEFT, 584, 262, 270, 60, ID_DICA);   // no lado direito (a coluna da esquerda vai ate o rodape)

            g_criando = CAT_OUTROS;
            Criar(j, L"BUTTON", TR(L"Abrir direto o jogo da próxima vez (Shift ao abrir mostra esta janela)",
                                   L"Start the game directly next time (Shift while opening shows this window)"), WS_TABSTOP, 24, 614, 536, 24, ID_DIRETO);
            Criar(j, L"BUTTON", TR(L"Mostrar a janela de registro (para depurar)", L"Show the log window (for debugging)"), WS_TABSTOP, 24, 642, 536, 24, ID_REGISTRO);
            Criar(j, L"BUTTON", TR(L"Mostrar FPS e ms no canto da tela", L"Show FPS and ms in the corner of the screen"), WS_TABSTOP, 24, 670, 536, 24, ID_MOSTRARFPS);
            Criar(j, L"BUTTON", TR(L"Mostrar o aviso das teclas F1, F2 e F3 quando o jogo começa", L"Show the F1, F2 and F3 key hint when the game starts"),
                  WS_TABSTOP, 24, 698, 536, 24, ID_AVISOTECLAS);

            // ---- coluna da direita: perfil, internet, requisitos
            g_criando = CAT_PERFIL;
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
                // jogos salvos do perfil: miniatura, nome e data; os botoes valem para o selecionado
                HWND lb = Criar(j, L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP,
                                DIR, 266, 836, 336, ID_SAVES);
                SetWindowTheme(lb, L"DarkMode_Explorer", nullptr);
                Criar(j, L"BUTTON", g_op.launcherIcones ? TR(L"VER EM LISTA", L"LIST VIEW") : TR(L"VER EM ÍCONES", L"ICON VIEW"),
                      WS_TABSTOP, DIR + 836 - 180, 228, 180, 30, ID_VISTA_SAVES);
                Criar(j, L"BUTTON", TextoFiltro(), WS_TABSTOP, DIR + 836 - 370, 228, 180, 30, ID_FILTRO_SAVES);
                Criar(j, L"BUTTON", TR(L"RENOMEAR", L"RENAME"), WS_TABSTOP, DIR, 614, 160, 38, ID_RENOMEAR);
                Criar(j, L"BUTTON", TR(L"DUPLICAR", L"DUPLICATE"), WS_TABSTOP, DIR + 170, 614, 160, 38, ID_DUPLICAR);
                Criar(j, L"BUTTON", TR(L"APAGAR", L"DELETE"), WS_TABSTOP, DIR + 340, 614, 160, 38, ID_APAGAR);
                Criar(j, L"BUTTON", TR(L"ABRIR PASTA", L"OPEN FOLDER"), WS_TABSTOP, DIR + 510, 614, 180, 38, ID_ABRIRSAVE);
            }
            g_criando = CAT_INTERNET;
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
            g_criando = CAT_REQUISITOS;
            {
                std::wstring pc = DescreverEstePc(g_pcAtende);
                Criar(j, L"STATIC", TR(L"Sistema: Windows 10 ou 11 de 64 bits\n"
                                       L"Processador: 4 núcleos e 8 threads\n"
                                       L"Memória: 8 GB de RAM (o jogo usa ~1,5 GB)\n"
                                       L"Vídeo: placa com DirectX 12 (vídeo integrado serve)",
                                       L"System: 64-bit Windows 10 or 11\n"
                                       L"Processor: 4 cores and 8 threads\n"
                                       L"Memory: 8 GB of RAM (the game uses ~1.5 GB)\n"
                                       L"Graphics: DirectX 12 card (integrated graphics works)"), SS_LEFT, DIR, 408, 536, 82, ID_REQ_MIN);
                Criar(j, L"STATIC", pc.c_str(), SS_LEFT, DIR, 494, 536, 62, ID_REQ_PC);
            }

            // trapacas do jogo (codigos da tela TRAPACAS do menu de pausa) ligadas sem digitar, e o "invencivel" (trapacas.cpp)
            g_criando = CAT_TRAPACAS;
            {
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
                    Criar(j, L"BUTTON", t[i].nome, WS_TABSTOP, PAINEL_X, 150 + i * 70, 836, 24, ID_TRAPACA + i);
                    Criar(j, L"STATIC", t[i].desc, SS_LEFT, PAINEL_X + 32, 176 + i * 70, 804, 40, ID_TRAPACA_DESC + i);
                }
                Criar(j, L"STATIC", TR(L"As trapaças valem só no modo história (o jogo não deixa no multijogador). Um save feito com"
                                       L" munição, adrenalina ou poderes ferais ligados guarda essas trapaças, como no jogo original.",
                                       L"Cheats work only in the story mode (the game does not allow them in multiplayer). A save made with"
                                       L" ammo, adrenaline or feral abilities on keeps those cheats, like in the original game."),
                      SS_LEFT, PAINEL_X, 580, 836, 44, ID_TRAPACA_AVISO);
            }

            g_criando = CAT_CONQUISTAS;
            Criar(j, L"STATIC", L"", SS_LEFT, PAINEL_X, 150, 640, 22, ID_CONQ_RESUMO);
            Criar(j, L"BUTTON", g_op.launcherIcones ? TR(L"VER EM LISTA", L"LIST VIEW") : TR(L"VER EM ÍCONES", L"ICON VIEW"),
                  WS_TABSTOP, 1136 - 180, 142, 180, 30, ID_VISTA_CONQ);
            {
                HWND lb = Criar(j, L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP,
                                PAINEL_X, 220, 836, 444, ID_CONQ);
                SetWindowTheme(lb, L"DarkMode_Explorer", nullptr);
            }
            g_criando = -1;
            Criar(j, L"BUTTON", TR(L"CONTROLES", L"CONTROLS"), WS_TABSTOP, 24, 686, 170, 38, ID_CONTROLES);
            Criar(j, L"BUTTON", TR(L"INSTRUÇÕES", L"INSTRUCTIONS"), WS_TABSTOP, 204, 686, 190, 38, ID_INSTRUCOES);
            Criar(j, L"BUTTON", TR(L"SALVAR", L"SAVE"), WS_TABSTOP, 702, 686, 150, 38, ID_SALVAR);
            Criar(j, L"BUTTON", TR(L"SAIR", L"QUIT"), WS_TABSTOP, 862, 686, 120, 38, ID_SAIR);
            Criar(j, L"BUTTON", TR(L"JOGAR", L"PLAY"), WS_TABSTOP, 992, 686, 144, 38, ID_JOGAR);

            g_marcado[ID_TELACHEIA] = g_op.telaCheia;
            g_marcado[ID_VSYNC] = g_op.vsync;
            g_marcado[ID_AUTOHDR] = g_op.autoHdr;
            g_marcado[ID_HDRFALSO] = g_op.hdrFalso;
            g_marcado[ID_MOSTRARFPS] = g_op.mostrarFps;
            g_marcado[ID_AVISOTECLAS] = g_op.avisoTeclas;
            g_marcado[ID_REGISTRO] = g_op.mostrarRegistro;
            g_marcado[ID_DIRETO] = g_op.pularLauncher;
            {
                const bool trapacas[] = { g_op.trapacaMunicao, g_op.trapacaAdrenalina, g_op.trapacaFeral, g_op.trapacaFases, g_op.trapacaMapas, g_op.trapacaInvencivel };
                for (int i = 0; i < 6; i++) g_marcado[ID_TRAPACA + i] = trapacas[i];
            }
            AtualizarSituacao(j);
            CarregarSaves(j);
            if (g_categoria == CAT_CONQUISTAS) CarregarConquistas(j);
            return 0;
        }
        case WM_MEASUREITEM:
        {
            auto* m = (MEASUREITEMSTRUCT*)lp;
            if (m->CtlID == ID_SAVES || m->CtlID == ID_CONQ) { m->itemHeight = S(m->CtlID == ID_SAVES ? 80 : 84); return TRUE; }
            break;
        }
        case WM_RENOMEADO:
        {
            auto* r = (std::pair<std::filesystem::path, std::wstring>*)lp;
            EnableWindow(j, TRUE);
            SetForegroundWindow(j);
            std::wstring nome = r->second;
            while (!nome.empty() && nome.back() == L' ') nome.pop_back();
            if (wp && !nome.empty()) std::ofstream(r->first / "_nome.txt", std::ios::binary) << ParaLatin1(nome);
            delete r;
            CarregarSaves(j);
            return 0;
        }
        case WM_TIMER:
            if (wp == ID_SALVAR)
            {
                KillTimer(j, ID_SALVAR);
                SetDlgItemTextW(j, ID_SALVAR, TR(L"SALVAR", L"SAVE"));
                InvalidateRect(GetDlgItem(j, ID_SALVAR), nullptr, FALSE);
                return 0;
            }
            break;
        case WM_COMMAND:
        {
            int id = LOWORD(wp);
            if (Caixa(id) && (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == BN_DOUBLECLICKED))
            {
                g_marcado[id] = !g_marcado[id];
                InvalidateRect((HWND)lp, nullptr, FALSE);
                return 0;
            }
            if (id >= ID_CAT && id < ID_CAT + N_CAT && HIWORD(wp) == BN_CLICKED) { MostrarCategoria(j, id - ID_CAT); return 0; }
            switch (id)
            {
            case ID_IDIOMA:
                if (HIWORD(wp) == CBN_SELCHANGE)
                {
                    const int esc = std::max(0, (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0));
                    const bool en = esc != 0;
                    if (esc == (g_op.idioma >= 0 ? g_op.idioma : IDIOMA_INGLES ? 1 : 0)) return 0;
                    LerTela(j);   // o que ja foi mexido na tela continua na janela nova (sem salvar)
                    // grava so o idioma no opcoes.ini (o resto fica como estava no arquivo)
                    const Opcoes copia = g_op;
                    OpcoesCarregar(g_exeDir);
                    g_op.idioma = esc;
                    idioma::ingles = en;
                    idioma::outro = esc >= 2 ? esc - 1 : 0;
                    OpcoesSalvar();
                    g_op = copia;
                    g_op.idioma = esc;
                    g_categoriaInicial = g_categoria;
                    g_conqLidas = false;   // nomes e explicacoes das conquistas no idioma novo
                    g_recriar = true;
                    GetWindowRect(j, &g_lugar);
                    DestroyWindow(j);
                }
                return 0;
            case ID_FSR:   // FSR ligado: a resolucao interna sai do tamanho da janela
                if (HIWORD(wp) == CBN_SELCHANGE)
                {
                    const bool ligado = SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0) > 0;
                    EnableWindow(GetDlgItem(j, ID_ESCALA), !ligado);
                    EnableWindow(GetDlgItem(j, ID_FSR_NITIDEZ), ligado);
                }
                return 0;
            case ID_PASTA: if (HIWORD(wp) == EN_CHANGE) { AtualizarSituacao(j); g_conqLidas = false; } return 0;
            case ID_PERFIL:   // outro perfil: os saves dele
                if (HIWORD(wp) == CBN_SELCHANGE)
                {
                    // na troca pela lista o texto da caixa so muda depois desta mensagem
                    PostMessageW(j, WM_COMMAND, MAKEWPARAM(ID_PERFIL, CBN_EDITCHANGE), lp);
                    return 0;
                }
                if (HIWORD(wp) == CBN_EDITCHANGE) CarregarSaves(j);
                return 0;
            case ID_SAVES:
                if (HIWORD(wp) == LBN_SELCHANGE || HIWORD(wp) == LBN_DBLCLK)
                {
                    g_saveSel = SaveNoMouse((HWND)lp);
                    InvalidateRect((HWND)lp, nullptr, FALSE);
                    for (int id = ID_RENOMEAR; id <= ID_ABRIRSAVE; id++) EnableWindow(GetDlgItem(j, id), SaveSelecionado(j) >= 0);
                }
                if (HIWORD(wp) == LBN_DBLCLK && SaveSelecionado(j) >= 0) RenomearSave(j);
                return 0;
            case ID_CONQ:   // na grade: clicou numa conquista -> a explicacao dela na linha do resumo
                if (HIWORD(wp) == LBN_SELCHANGE && g_op.launcherIcones)
                {
                    int linha = (int)SendMessageW((HWND)lp, LB_GETCURSEL, 0, 0);
                    DWORD pos = GetMessagePos();
                    POINT pt{ (short)LOWORD(pos), (short)HIWORD(pos) };
                    ScreenToClient((HWND)lp, &pt);
                    RECT rc; GetClientRect((HWND)lp, &rc);
                    int i = linha * POR_LINHA_CONQ + std::clamp((int)(pt.x * POR_LINHA_CONQ / std::max(1L, rc.right)), 0, POR_LINHA_CONQ - 1);
                    if (linha >= 0 && i < (int)g_conq.size())
                        SetDlgItemTextW(j, ID_CONQ_RESUMO, (g_conq[i].nome + L": " + g_conq[i].comoObter).c_str());
                }
                return 0;
            case ID_VISTA_SAVES: case ID_VISTA_CONQ:   // lista <-> icones (vale para as duas abas; fica salvo)
            {
                g_op.launcherIcones = !g_op.launcherIcones;
                const wchar_t* t = g_op.launcherIcones ? TR(L"VER EM LISTA", L"LIST VIEW") : TR(L"VER EM ÍCONES", L"ICON VIEW");
                SetDlgItemTextW(j, ID_VISTA_SAVES, t);
                SetDlgItemTextW(j, ID_VISTA_CONQ, t);
                InvalidateRect(GetDlgItem(j, ID_VISTA_SAVES), nullptr, FALSE);
                InvalidateRect(GetDlgItem(j, ID_VISTA_CONQ), nullptr, FALSE);
                CarregarSaves(j);
                if (!g_conq.empty()) CarregarConquistas(j);
                OpcoesSalvarSoVista();
                return 0;
            }
            case ID_FILTRO_SAVES:   // tudo -> so saves -> so mapas
                g_filtroSaves = (g_filtroSaves + 1) % 3;
                SetDlgItemTextW(j, ID_FILTRO_SAVES, TextoFiltro());
                InvalidateRect(GetDlgItem(j, ID_FILTRO_SAVES), nullptr, FALSE);
                CarregarSaves(j);
                return 0;
            case ID_RENOMEAR: RenomearSave(j); return 0;
            case ID_DUPLICAR: DuplicarSave(j); return 0;
            case ID_APAGAR: ApagarSave(j); return 0;
            case ID_ABRIRSAVE:
                if (int i = SaveSelecionado(j); i >= 0) ShellExecuteW(j, L"open", g_saves[i].dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            case ID_PROCURAR: Procurar(j); return 0;
            case ID_ISO: ProcurarIso(j); return 0;
            case ID_CONTROLES: ControlesAbrirJanela(false); return 0;
            case ID_INSTRUCOES: MostrarInstrucoes(j); return 0;
            case ID_MEUIP: MostrarMeuIp(j); return 0;
            case ID_SALVAR:   // salva as opcoes no opcoes.ini sem abrir o jogo; o botao mostra "SALVO" por 2 s
                LerTela(j);
                OpcoesSalvar();
                SetDlgItemTextW(j, ID_SALVAR, TR(L"SALVO", L"SAVED"));
                SetTimer(j, ID_SALVAR, 2000, nullptr);
                return 0;
            case ID_SAIR: case IDCANCEL: DestroyWindow(j); return 0;
            case ID_JOGAR: case IDOK: if (Preparar(j)) { g_jogar = true; DestroyWindow(j); } return 0;   // Enter = Jogar
            }
            break;
        }
        case WM_DRAWITEM:
        {
            auto* d = (const DRAWITEMSTRUCT*)lp;
            if (d->CtlID == ID_SAVES) { DesenharSave(d); return TRUE; }
            if (d->CtlID == ID_CONQ) { DesenharConquista(d); return TRUE; }
            const int id = (int)d->CtlID;
            if (Caixa(id)) DesenharCaixa(d, g_marcado[id], g_fonte, CARTAO);
            else if (id >= ID_CAT && id < ID_CAT + N_CAT) DesenharItemMenu(d, id == ID_CAT + g_categoria, FonteMenu(), FUNDO);
            else DesenharBotao(d, id == ID_JOGAR, FonteBotao(), id >= ID_CONTROLES && id <= ID_SALVAR && id != ID_MEUIP ? FUNDO : CARTAO);
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
            if (id == ID_CONQ_RESUMO) cor = AMBAR;
            if (id == ID_DICA || id == ID_DICA_NET || id == ID_DICA_PERFIL || id == ID_REQ_MIN) cor = TEXTO_FRACO;
            if (id >= ID_TRAPACA_DESC && id < ID_TRAPACA_DESC + 6) cor = TEXTO_FRACO;
            if (id == ID_TRAPACA_AVISO) cor = AMBAR;
            if (id == ID_REQ_PC) cor = g_pcAtende ? VERDE : AMBAR;
            if (id == ID_SITUACAO)
            {
                wchar_t b[8] = {}; GetWindowTextW((HWND)lp, b, 8);
                cor = b[0] == L'✔' ? VERDE : b[0] == L'✘' ? ERRO : TEXTO_FRACO;
            }
            return CorTextoNoCartao(dc, cor);
        }
        case WM_CTLCOLORLISTBOX:   // saves e conquistas: no fundo do cartao (os itens sao desenhados aqui)
            if (int id = GetDlgCtrlID((HWND)lp); id == ID_SAVES || id == ID_CONQ) return CorTextoNoCartao((HDC)wp, TEXTO);
            return CorCampo((HDC)wp);
        case WM_CTLCOLOREDIT:
            return CorCampo((HDC)wp);
        case WM_CLOSE: DestroyWindow(j); return 0;
        case WM_DESTROY: LimparSaves(); PostQuitMessage(0); return 0;
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
    g_fonteGrande = CreateFontW(-S(32), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Impact");
    static ULONG_PTR gdiplus = 0;
    if (!gdiplus) { Gdiplus::GdiplusStartupInput gi; Gdiplus::GdiplusStartup(&gdiplus, &gi, nullptr); }
    g_fonteNome = CreateFontW(-S(17), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fonteSub = CreateFontW(-S(12), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_iconeTopo = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, S(56), S(56), LR_DEFAULTCOLOR);
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
    do
    {
        // trocar o idioma fecha a janela (g_recriar) e ela abre de novo no mesmo lugar, com os textos novos
        const bool deNovo = g_recriar;
        g_recriar = false;
        HWND j = CreateWindowExW(0, L"PredatorLauncher", L"Far Cry Instincts Predator Recomp " VERSAO_RECOMP, estilo,
                                 deNovo ? g_lugar.left : area.left + std::max(0L, (area.right - area.left - w) / 2),
                                 deNovo ? g_lugar.top : area.top + std::max(0L, (area.bottom - area.top - h) / 2),
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
    } while (g_recriar);
    return g_jogar;
}
