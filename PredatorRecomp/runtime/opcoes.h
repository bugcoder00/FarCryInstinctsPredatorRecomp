// Opcoes do jogo escolhidas no launcher (launcher.cpp), salvas em opcoes.ini ao lado do executavel
#pragma once
#include <filesystem>
#include <string>

struct Opcoes
{
    std::filesystem::path pastaJogo;   // pasta com o default.xex e os .dat da copia do jogo de quem joga
    int janela = 0;                    // 0 = o maior 16:9 que cabe na tela; 1 = 1280x720, 2 = 1600x900, 3 = 1920x1080, 4 = 2560x1440
    bool telaCheia = false;            // sem bordas, cobrindo o monitor (Alt+Enter troca no jogo)
    int antisserrilhado = 1;           // suaviza as bordas da imagem final: 0 desligado, 1 leve, 2 medio, 3 forte
    int escala = 0;                    // resolucao interna: 0 = 1x (1280x720), 1 = 1,5x (1920x1080), 2 = 2x (2560x1440), 3 = 3x (3840x2160, 4K)
    int sombras = 0;                   // mapa de sombras: 0 = normal (como a resolucao interna), 1 = alta (2x), 2 = muito alta (3x)
    bool vsync = false;                // espera o monitor (pode baixar o FPS)
    int fps = 0;                       // quadros por segundo: 0 = 30 (como o original), 1 = 60, 2 = sem limite (experimental); gpu.cpp sub_8273E6F8
    bool mostrarRegistro = false;      // janela de console com o registro
    bool pularLauncher = false;        // abre direto o jogo (Shift ao abrir mostra o launcher)
    std::string ipAnfitriao;           // SYSTEM LINK pela internet: IP (ou nome) do PC de quem criou a partida; vazio = rede local
    std::string perfil;                // perfil (item 32): vazio = o principal (pasta save); senao save\perfis\<nome>
    bool modoTeste = false;            // (nao salvo) pasta na linha de comando ou FCRECOMP_*: sem launcher, titulo completo
};

Opcoes& OpcoesAtuais();
void OpcoesCarregar(const std::filesystem::path& exeDir);
void OpcoesSalvar();
std::string PerfilValido(const std::string& nome);   // letras sem acento, numeros, espaco, - e _, ate 15
std::filesystem::path PastaDoPerfil(const std::filesystem::path& exeDir, const std::string& perfil);   // pasta dos saves
bool LauncherMostrar(const std::filesystem::path& exeDir);   // false = quem joga fechou ou clicou em Sair
// upnp.cpp: IP de internet deste PC perguntado ao roteador (botao "MEU IP" do launcher); aviso = CGNAT
bool UpnpIpExterno(std::string& ip, std::string& erro, std::string* aviso);
struct HWND__;
void AplicarIcone(HWND__* janela);   // icone "PR" do exe (recurso 1) na barra de titulo e na barra de tarefas
// cria o image.bin a partir do default.xex (xex_imagem.cpp)
bool XexCriarImagem(const std::filesystem::path& xex, const std::filesystem::path& saida, std::string& erro, bool conferir = true);
