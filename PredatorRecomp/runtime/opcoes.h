// Opcoes do jogo escolhidas no launcher (launcher.cpp), salvas em opcoes.ini ao lado do executavel
#pragma once
#include <filesystem>
#include <string>
#include <vector>

// listas do FSR (launcher, janela F2): Desligado, Ultra, Qualidade, Equilibrado, Desempenho. No opcoes.ini o ultra e o 4
// (veio depois; os outros valores continuam os mesmos)
inline int FsrDaLista(int i) { static const int v[] = { 0, 4, 1, 2, 3 }; return v[i < 0 ? 0 : i > 4 ? 4 : i]; }
inline int ListaDoFsr(int fsr) { static const int i[] = { 0, 2, 3, 4, 1 }; return i[fsr < 0 ? 0 : fsr > 4 ? 4 : fsr]; }

constexpr int FOV_MAX = 45;   // campo de visao: ate 95 + 45 = 140 graus (mais que isso estica demais as bordas)
constexpr int MIRA_MIN = 25, MIRA_MAX = 150;   // tamanho da mira em porcentagem (mira.cpp); 100 = o do jogo
constexpr int VOLUME_VOZ_MAX = 200;            // o chat de voz pode ser amplificado ate 2x (microfone baixo do outro lado)
// save state (save_state.cpp): teclas que podem salvar/carregar (codigos VK do Windows: F5, F6, F7, F8, F9, F11)
constexpr int SAVESTATE_TECLAS[] = { 0x74, 0x75, 0x76, 0x77, 0x78, 0x7A };
constexpr int SAVESTATE_N_TECLAS = 6;
inline int SaveStateIndice(int vk) { for (int i = 0; i < SAVESTATE_N_TECLAS; i++) if (SAVESTATE_TECLAS[i] == vk) return i; return -1; }
inline const wchar_t* SaveStateNomeTecla(int i) { static const wchar_t* n[] = { L"F5", L"F6", L"F7", L"F8", L"F9", L"F11" }; return n[i < 0 ? 0 : i >= SAVESTATE_N_TECLAS ? SAVESTATE_N_TECLAS - 1 : i]; }

struct Opcoes
{
    std::filesystem::path pastaJogo;   // pasta com o default.xex e os .dat da copia do jogo de quem joga
    int janela = 0;                    // 0 = o maior 16:9 que cabe na tela; 1 = 1280x720, 2 = 1600x900, 3 = 1920x1080, 4 = 2560x1440
    bool telaCheia = false;            // sem bordas, cobrindo o monitor (Alt+Enter troca no jogo)
    int antisserrilhado = 1;           // suaviza as bordas da imagem final: 0 desligado, 1 leve, 2 medio, 3 forte
    int msaa = 0;                      // MSAA nas imagens da tela (xenos_draw.cpp): 0 desligado, 1 = 2x, 2 = 4x, 3 = 8x
    int smaa = 0;                      // SMAA na cena 3D (render.cpp SMAA_HLSL): 0 desligado, 1 baixo, 2 medio, 3 alto, 4 ultra
    int escala = 0;                    // resolucao interna: 0 = 1x (1280x720), 1 = 1,5x (1920x1080), 2 = 2x (2560x1440), 3 = 3x (3840x2160, 4K)
    int fsr = 0;                       // upscale AMD FSR 1 (render.cpp): 0 desligado, 1 qualidade (0,75x), 2 equilibrado (0,5x), 3 desempenho (0,5x, so bilinear),
                                       // 4 ultra qualidade (1x); ligado, a resolucao interna e essa (xenos_draw.cpp EscalaMeios) e "escala" nao vale
    int fsrNitidez = 2;                // nitidez do FSR (RCAS): 0 desligada, 1 baixa, 2 media, 3 alta, 4 maxima
    int sombras = 0;                   // mapa de sombras: 0 = normal (como a resolucao interna), 1 = alta (2x), 2 = muito alta (3x)
    bool vsync = false;                // espera o monitor (pode baixar o FPS)
    bool autoHdr = false;              // Auto HDR: imagem SDR do jogo convertida para HDR10 com realces mais brilhantes (render.cpp);
                                       // so vale com o HDR ligado no Windows no monitor do jogo (senao fica SDR)
    bool hdrFalso = false;             // HDR falso: filtro em SDR (contraste local, sombras mais claras, luzes e cores mais fortes; render.cpp)
    bool mostrarFps = false;           // contador de FPS e ms no canto da tela (render.cpp ContadorDesenhar)
    bool avisoTeclas = true;           // aviso das teclas F1/F2/F3 quando o jogo comeca (render.cpp)
    int formato = 0;                   // formato da imagem: 0 = 16:9 (original), 1 = o do monitor (ultrawide 21:9, 32:9), 2 = 16:9 com barras pretas
    int filtroTextura = 0;             // filtro anisotropico: 0 = o do jogo, 1 = 2x, 2 = 4x, 3 = 8x, 4 = 16x (xenos_draw.cpp)
    int distancia = 0;                 // distancia de visao (objetos, vegetacao): -2 = 0,5x, -1 = 0,75x, 0 = original, 1 = 1,5x, 2 = 2x, 3 = 3x (graficos_extra.cpp)
    int detalhe = 0;                   // nivel de detalhe dos objetos (LOD) mais longe: -2 = 0,5x, -1 = 0,75x, 0 = original, 1 = 1,5x, 2 = 2x, 3 = 3x
    int ao = 0;                        // oclusao de ambiente (cantos mais escuros): 0 desligada, 1 leve, 2 media, 3 forte (xenos_draw.cpp)
    std::wstring placaVideo;           // placa de video (nome; placas_video.cpp): vazio = automatica (a de alto desempenho)
    int fov = 0;                       // campo de visao: 0 = o do jogo; senao graus a mais (fov.cpp), ate FOV_MAX
    int mira = 100;                    // tamanho da mira em porcentagem (mira.cpp): 100 = o do jogo, de MIRA_MIN a MIRA_MAX
    int fps = 0;                       // quadros por segundo: 0 = 30 (como o original), 1 = 60, 2 = 45 (era "sem limite" ate a 1.0.1: bugs acima de 60); gpu.cpp sub_8273E6F8
    bool mostrarRegistro = false;      // janela de console com o registro
    bool pularLauncher = false;        // abre direto o jogo (Shift ao abrir mostra o launcher)
    bool launcherIcones = false;       // launcher: saves e conquistas em grade de icones (false = lista)
    std::string ipAnfitriao;           // SYSTEM LINK pela internet: IP (ou nome) do PC de quem criou a partida; vazio = rede local
    // audio (aba AUDIO do launcher e janela de audio do F2; audio_janela.cpp). Dispositivos pelo id do Windows (vazio =
    // o padrao). Com o jogo rodando, leia e troque os dispositivos so por SomEscolhido/SomEscolher (trava).
    std::wstring somSaida;             // alto-falantes/fones do som do jogo (audio.cpp)
    std::wstring vozFone;              // fone do chat de voz (voz.cpp); vazio = o mesmo do som do jogo
    std::wstring vozMicrofone;         // microfone do chat de voz; vazio = o padrao de comunicacao do Windows
    int volumeJogo = 100;              // volume do som do jogo em porcentagem (0 a 100)
    int volumeVoz = 100;               // volume do chat de voz (o que os outros falam) em porcentagem (0 a VOLUME_VOZ_MAX)
    std::string perfil;                // perfil (item 32): vazio = o principal (pasta save); senao save\perfis\<nome>
    int idioma = -1;                   // textos do programa: -1 = ainda nao escolhido (vale o do Windows), 0 = portugues, 1 = ingles (idioma.h); 2..5 = jogo em frances, alemao, espanhol, italiano (xam.cpp XGetLanguage; programa em ingles)
    // trapacas (aba TRAPACAS do launcher; trapacas.cpp): codigos do jogo ligados sem digitar e o "invencivel" (novo)
    bool trapacaMunicao = false;       // UnleashHell: municao infinita
    bool trapacaAdrenalina = false;    // Bloodlust: adrenalina infinita
    bool trapacaFeral = false;         // FeralAttack: todos os poderes ferais
    bool trapacaFases = false;         // GiveMeItAll: todas as fases
    bool trapacaMapas = false;         // GiveMeTheMaps: todos os mapas
    bool trapacaInvencivel = false;    // vida sempre cheia (nao existe no jogo; o ImJackCarver enche uma vez)
    // save state (aba SAVE STATE do launcher e botao SAVE STATE da janela F2; save_state.cpp)
    bool saveState = true;             // teclas de salvar/carregar o estado ligadas
    int saveStateSalvar = 0x74;        // tecla que salva (VK; uma de SAVESTATE_TECLAS): F5
    int saveStateCarregar = 0x78;      // tecla que carrega: F9
    bool modoTeste = false;            // (nao salvo) pasta na linha de comando ou FCRECOMP_*: sem launcher, titulo completo
};

Opcoes& OpcoesAtuais();
float AspectoTela();   // gpu.cpp: proporcao da imagem do jogo (16/9 ou a do monitor com a opcao ultrawide)
void OpcoesCarregar(const std::filesystem::path& exeDir);
void OpcoesSalvar();
std::string PerfilValido(const std::string& nome);   // letras sem acento, numeros, espaco, - e _, ate 15
std::filesystem::path PastaDoPerfil(const std::filesystem::path& exeDir, const std::string& perfil);   // pasta dos saves
bool LauncherMostrar(const std::filesystem::path& exeDir);   // false = quem joga fechou ou clicou em Sair
// upnp.cpp: IP de internet deste PC perguntado ao roteador (botao "MEU IP" do launcher); aviso = CGNAT
bool UpnpIpExterno(std::string& ip, std::string& erro, std::string* aviso);
struct HWND__;
void AplicarIcone(HWND__* janela);   // icone "PR" do exe (recurso 1) na barra de titulo e na barra de tarefas
// dispositivos de som do Windows (audio_janela.cpp): saidas (alto-falantes, fones) ou microfones ligados
struct DispositivoSom { std::wstring id, nome; };
std::vector<DispositivoSom> DispositivosSom(bool microfone);
std::wstring NomeSomPadrao(bool microfone);            // nome do padrao do Windows (microfone: o de comunicacao)
enum SomQual { SOM_SAIDA = 0, SOM_VOZ_FONE = 1, SOM_VOZ_MIC = 2 };
std::wstring SomEscolhido(int qual);                    // copia do id escolhido (vazio = padrao), com trava
void SomEscolher(int qual, const std::wstring& id);     // troca com o jogo rodando (audio.cpp/voz.cpp reabrem)
unsigned SomVersao();                                   // muda a cada SomEscolher
void AudioAbrirJanela();                                // janela de audio (botao AUDIO da janela F2)
void SaveStateAbrirJanela();                            // janela do save state (botao SAVE STATE da janela F2; video_janela.cpp)
void SaveStatePedir(bool carregar);                     // salva/carrega (o mais novo) no proximo quadro do jogo (save_state.cpp)
void SaveStateCarregarId(unsigned id);                  // carrega o estado id da lista
void SaveStateApagarId(unsigned id);                    // tira o estado id da lista
unsigned SaveStateVersao();                             // muda a cada mudanca da lista
std::vector<std::pair<unsigned, std::wstring>> SaveStateLista();   // (id, texto), o mais novo primeiro
// estados salvos no disco de um perfil (<pasta do perfil>\savestates\<fase>\*.fcss; aba SAVE STATE do launcher)
struct SaveStateArquivo
{
    std::string fase;                    // ex. 01_beach
    std::filesystem::path arquivo;
    int ano = 0, mes = 0, dia = 0, hora = 0, minuto = 0, segundo = 0;
    float x = 0, y = 0, z = 0;           // posicao do jogador
};
std::vector<SaveStateArquivo> SaveStatesDoPerfil(const std::filesystem::path& pastaPerfil);   // o mais novo primeiro
void SomTestar(const std::wstring& id);                 // bipe curto no dispositivo (vazio = o padrao)
void MicMedirIniciar(const std::wstring& id);           // abre o microfone so para medir o nivel (barra das janelas)
void MicMedirParar();
bool MicMedindo();                                     // microfone aberto para a barra de nivel (botao TESTAR)
float MicNivel();                                       // 0 a 1 (escala em dB)
struct tagDRAWITEMSTRUCT;
void DesenharNivelMic(const tagDRAWITEMSTRUCT* d, unsigned long atras);   // barra do nivel (STATIC com SS_OWNERDRAW)
// cria o image.bin a partir do default.xex (xex_imagem.cpp)
bool XexCriarImagem(const std::filesystem::path& xex, const std::filesystem::path& saida, std::string& erro, bool conferir = true);

// lista de FPS nas telas (30, 45, 60) <-> opcao fps (0 = 30, 1 = 60, 2 = 45): a mesma troca vale nos dois sentidos
inline int FpsLista(int v) { return v == 1 ? 2 : v == 2 ? 1 : 0; }
