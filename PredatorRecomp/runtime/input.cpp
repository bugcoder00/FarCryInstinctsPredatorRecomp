// Controle: controles do PC (Xbox/XInput, DualSense, DualShock 4), teclado e mouse -> XINPUT_STATE do Xbox 360
//
// Cada jogador (1 a 4) usa um dispositivo escolhido na janela de configuracao (F1 no jogo, controles_janela.cpp); por
// padrao o jogador 1 e o teclado e mouse, o jogador 2 o primeiro controle ligado (usuario 1 do Xbox: tela dividida) e
// os jogadores 3 e 4 nenhum (usuarios 2 e 3: a tela dividida do jogo vai ate 4).
// "juntar" faz o dispositivo do jogador 2 tambem controlar o jogador 1 (e entao nao ha jogador 2). Tudo fica salvo em build\controles.ini (a janela escreve o arquivo).
// O mouse move a camera; o cursor fica preso e escondido na janela do jogo. F2 solta/prende o mouse, "+" e "-" mudam
// a sensibilidade (aparece no titulo da janela).
#include "idioma.h"
#include "guest.h"
#include "controles.h"
#include "diag.h"
#include "perf.h"
#include <windows.h>
#include <xinput.h>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <unordered_map>
#include <thread>
#include <chrono>
#include <array>
#include <cstdarg>

extern float g_cameraSpeedBoost;   // definido no fim do arquivo

#pragma comment(lib, "xinput.lib")

extern HWND GetGameWindow();

namespace
{
    std::atomic<uint32_t> g_packet{ 0 };
    const DWORD g_startTick = GetTickCount();

    // Momento em que o menu principal apareceu (1a leitura do perfil, content.cpp) e em que a fase comecou (gpu.cpp,
    // com FCRECOMP_MISSAO). 0 = ainda nao.
    std::atomic<DWORD> g_menuTick{ 0 }, g_missionTick{ 0 };

    // Roteiro de teste: FCRECOMP_AUTOINPUT="70:START,74:A,80:DOWN" aperta o botao por 0,3 s no segundo indicado;
    // "5-60/2:START" aperta a cada 2 s entre 5 e 60 s; "5-9:LUP" segura de 5 a 9 s. Com "M" na frente o tempo conta a
    // partir do menu principal ("M6:A,M9:DOWN"); com "F", a partir do inicio da fase ("F3-8:LUP" anda para a frente).
    // Quando o roteiro tem itens "M", os itens sem letra param de valer assim que o menu aparece (o START repetido para
    // pular os videos nao chega no menu). Analogicos: LUP/LDOWN/LLEFT/LRIGHT (esquerdo, andar), RUP/RDOWN/RLEFT/RRIGHT
    // (direito, olhar); gatilhos LT/RT.
    // Varias partes separadas por "|": uma para cada programa aberto em sequencia pelas trocas (jogo -> editor de mapas
    // -> jogo...; PREDATOR_ETAPA conta as trocas, xam.cpp). Sem "|" o mesmo roteiro vale em todas.
    const char* AutoInputEtapa()
    {
        static std::string parte;
        const char* env = getenv("FCRECOMP_AUTOINPUT");
        if (!env || !strchr(env, '|')) return env;
        int etapa = getenv("PREDATOR_ETAPA") ? atoi(getenv("PREDATOR_ETAPA")) : 0;
        const char* p = env;
        for (int i = 0; i < etapa && p; i++) p = strchr(p, '|') ? strchr(p, '|') + 1 : nullptr;
        if (!p) return nullptr;
        parte.assign(p, strchr(p, '|') ? strchr(p, '|') : p + strlen(p));
        return parte.c_str();
    }

    void AutoInput(XINPUT_GAMEPAD& g)
    {
        static const char* env = AutoInputEtapa();
        static bool logged = false;
        if (!logged) { logged = true; Log("[input] roteiro automatico: %s", env ? env : "(nenhum)"); }
        if (!env) return;
        static const bool hasMenuItems = strstr(env, ",M") || env[0] == 'M';
        DWORD menu = g_menuTick, mission = g_missionTick;
        double tStart = (GetTickCount() - g_startTick) / 1000.0;   // segundos desde o inicio do programa
        double tMenu = menu ? (GetTickCount() - menu) / 1000.0 : -1;
        double tMission = mission ? (GetTickCount() - mission) / 1000.0 : -1;
        struct { const char* name; WORD bit; } names[] = {
            { "START", XINPUT_GAMEPAD_START }, { "BACK", XINPUT_GAMEPAD_BACK }, { "A", XINPUT_GAMEPAD_A },
            { "B", XINPUT_GAMEPAD_B }, { "X", XINPUT_GAMEPAD_X }, { "Y", XINPUT_GAMEPAD_Y },
            { "UP", XINPUT_GAMEPAD_DPAD_UP }, { "DOWN", XINPUT_GAMEPAD_DPAD_DOWN },
            { "LEFT", XINPUT_GAMEPAD_DPAD_LEFT }, { "RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT },
            { "LB", XINPUT_GAMEPAD_LEFT_SHOULDER }, { "RB", XINPUT_GAMEPAD_RIGHT_SHOULDER },
            { "L3", XINPUT_GAMEPAD_LEFT_THUMB }, { "R3", XINPUT_GAMEPAD_RIGHT_THUMB } };
        struct { const char* name; SHORT XINPUT_GAMEPAD::* axis; SHORT v; } sticks[] = {
            { "LUP", &XINPUT_GAMEPAD::sThumbLY, 32767 }, { "LDOWN", &XINPUT_GAMEPAD::sThumbLY, -32767 },
            { "LLEFT", &XINPUT_GAMEPAD::sThumbLX, -32767 }, { "LRIGHT", &XINPUT_GAMEPAD::sThumbLX, 32767 },
            { "RUP", &XINPUT_GAMEPAD::sThumbRY, 32767 }, { "RDOWN", &XINPUT_GAMEPAD::sThumbRY, -32767 },
            { "RLEFT", &XINPUT_GAMEPAD::sThumbRX, -32767 }, { "RRIGHT", &XINPUT_GAMEPAD::sThumbRX, 32767 } };
        std::string s = env;
        size_t pos = 0;
        while (pos < s.size())
        {
            size_t comma = s.find(',', pos);
            std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            size_t colon = item.find(':');
            if (colon != std::string::npos)
            {
                // "inicio-fim/intervalo:BOTAO" repete o aperto (ex.: 5-60/2:START para pular video e telas)
                std::string tm = item.substr(0, colon);
                char base = (!tm.empty() && (tm[0] == 'M' || tm[0] == 'F')) ? tm[0] : 0;
                if (base) tm.erase(0, 1);
                double t = base == 'M' ? tMenu : base == 'F' ? tMission : tStart;
                bool skip = base == 'M' ? !menu : base == 'F' ? !mission : (hasMenuItems && menu);
                double when = atof(tm.c_str()), until = when + 0.3, every = 0;
                if (size_t dash = tm.find('-'); dash != std::string::npos)
                {
                    until = atof(tm.c_str() + dash + 1);
                    if (size_t slash = tm.find('/'); slash != std::string::npos) every = atof(tm.c_str() + slash + 1);
                }
                std::string b = item.substr(colon + 1);
                bool down = t >= when && t < until;
                if (every > 0) down = down && fmod(t - when, every) < 0.3;
                if (down && !skip)
                {
                    for (auto& n : names) if (b == n.name) g.wButtons |= n.bit;
                    for (auto& k : sticks) if (b == k.name) g.*(k.axis) = k.v;
                    if (b == "LT") g.bLeftTrigger = 255;
                    if (b == "RT") g.bRightTrigger = 255;
                }
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }

    // ------------------------------------------------------------ nomes das teclas no controles.ini
    // Codigos especiais (fora dos codigos do Windows) para a roda do mouse
    constexpr int VK_RODA_CIMA = 0x1000, VK_RODA_BAIXO = 0x1001;

    struct KeyName { const char* name; int vk; const char* en = nullptr; };   // en: nome mostrado na versao em ingles
    const KeyName KEY_NAMES[] = {
        { "ESPACO", VK_SPACE, "SPACE" }, { "ENTER", VK_RETURN }, { "ESC", VK_ESCAPE }, { "TAB", VK_TAB }, { "BACKSPACE", VK_BACK },
        { "SHIFT_ESQ", VK_LSHIFT, "LEFT_SHIFT" }, { "SHIFT_DIR", VK_RSHIFT, "RIGHT_SHIFT" }, { "CTRL_ESQ", VK_LCONTROL, "LEFT_CTRL" }, { "CTRL_DIR", VK_RCONTROL, "RIGHT_CTRL" },
        { "ALT_ESQ", VK_LMENU, "LEFT_ALT" }, { "ALT_DIR", VK_RMENU, "RIGHT_ALT" }, { "CAPSLOCK", VK_CAPITAL },
        { "SETA_CIMA", VK_UP, "UP_ARROW" }, { "SETA_BAIXO", VK_DOWN, "DOWN_ARROW" }, { "SETA_ESQUERDA", VK_LEFT, "LEFT_ARROW" }, { "SETA_DIREITA", VK_RIGHT, "RIGHT_ARROW" },
        { "INSERT", VK_INSERT }, { "DELETE", VK_DELETE }, { "HOME", VK_HOME }, { "END", VK_END },
        { "PGUP", VK_PRIOR }, { "PGDN", VK_NEXT },
        { "MOUSE_ESQ", VK_LBUTTON, "LEFT_MOUSE" }, { "MOUSE_DIR", VK_RBUTTON, "RIGHT_MOUSE" }, { "MOUSE_MEIO", VK_MBUTTON, "MIDDLE_MOUSE" },
        { "MOUSE4", VK_XBUTTON1 }, { "MOUSE5", VK_XBUTTON2 },
        { "RODA_CIMA", VK_RODA_CIMA, "WHEEL_UP" }, { "RODA_BAIXO", VK_RODA_BAIXO, "WHEEL_DOWN" },
        { "MAIS", VK_OEM_PLUS, "PLUS" }, { "MENOS", VK_OEM_MINUS, "MINUS" }, { "NUM_MAIS", VK_ADD, "NUM_PLUS" }, { "NUM_MENOS", VK_SUBTRACT, "NUM_MINUS" },
        { "VIRGULA", VK_OEM_COMMA, "COMMA" }, { "PONTO", VK_OEM_PERIOD, "PERIOD" },
    };

    int ParseKey(std::string n)
    {
        for (auto& c : n) c = (char)toupper((unsigned char)c);
        if (n.size() == 1 && ((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= '0' && n[0] <= '9'))) return n[0];
        if (n.size() >= 2 && n[0] == 'F' && isdigit((unsigned char)n[1]))
        {
            int f = atoi(n.c_str() + 1);
            if (f >= 1 && f <= 12) return VK_F1 + f - 1;
        }
        if (n.rfind("NUM", 0) == 0 && n.size() == 4 && isdigit((unsigned char)n[3])) return VK_NUMPAD0 + (n[3] - '0');
        for (auto& k : KEY_NAMES) if (n == k.name) return k.vk;
        return 0;
    }

    // ------------------------------------------------------------ acoes (botoes do controle do Xbox)
    enum Kind { Button, LeftTrigger, RightTrigger, Axis };
    struct Action
    {
        const char* name;       // nome no controles.ini
        Kind kind;
        WORD bit;               // botao
        int axis, dir;          // eixo: 0 LX, 1 LY, 2 RX, 3 RY; direcao -1/+1
        const char* defaults;   // teclas padrao
        const char* help;
        std::vector<int> keys;
    };
    Action g_actions[] = {
        { "A", Button, XINPUT_GAMEPAD_A, 0, 0, "ESPACO", TR("botão A", "A button") },
        { "B", Button, XINPUT_GAMEPAD_B, 0, 0, "T", TR("botão B", "B button") },
        { "X", Button, XINPUT_GAMEPAD_X, 0, 0, "R", TR("botão X", "X button") },
        { "Y", Button, XINPUT_GAMEPAD_Y, 0, 0, "F", TR("botão Y", "Y button") },
        { "LB", Button, XINPUT_GAMEPAD_LEFT_SHOULDER, 0, 0, "Q", TR("LB (botão de ombro esquerdo)", "LB (left bumper)") },
        { "RB", Button, XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0, "E", TR("RB (botão de ombro direito)", "RB (right bumper)") },
        { "LT", LeftTrigger, 0, 0, 0, "MOUSE_DIR, CTRL_ESQ", TR("LT (gatilho esquerdo, mirar)", "LT (left trigger, aim)") },
        { "RT", RightTrigger, 0, 0, 0, "MOUSE_ESQ", TR("RT (gatilho direito, atirar)", "RT (right trigger, shoot)") },
        { "START", Button, XINPUT_GAMEPAD_START, 0, 0, "ENTER", TR("botão Start (pausa)", "Start button (pause)") },
        { "BACK", Button, XINPUT_GAMEPAD_BACK, 0, 0, "ESC", TR("botão Back", "Back button") },
        { "L3", Button, XINPUT_GAMEPAD_LEFT_THUMB, 0, 0, "SHIFT_ESQ", TR("L3 (clique no analógico esquerdo)", "L3 (left stick click)") },
        { "R3", Button, XINPUT_GAMEPAD_RIGHT_THUMB, 0, 0, "C, MOUSE_MEIO", TR("R3 (clique no analógico direito)", "R3 (right stick click)") },
        { "CIMA", Button, XINPUT_GAMEPAD_DPAD_UP, 0, 0, "1, SETA_CIMA", TR("direcional para cima", "D-pad up") },
        { "BAIXO", Button, XINPUT_GAMEPAD_DPAD_DOWN, 0, 0, "2, SETA_BAIXO", TR("direcional para baixo", "D-pad down") },
        { "ESQUERDA", Button, XINPUT_GAMEPAD_DPAD_LEFT, 0, 0, "3, SETA_ESQUERDA", TR("direcional para a esquerda", "D-pad left") },
        { "DIREITA", Button, XINPUT_GAMEPAD_DPAD_RIGHT, 0, 0, "4, SETA_DIREITA", TR("direcional para a direita", "D-pad right") },
        { "ANDAR_FRENTE", Axis, 0, 1, +1, "W", TR("analógico esquerdo para cima", "left stick up") },
        { "ANDAR_TRAS", Axis, 0, 1, -1, "S", TR("analógico esquerdo para baixo", "left stick down") },
        { "ANDAR_ESQUERDA", Axis, 0, 0, -1, "A", TR("analógico esquerdo para a esquerda", "left stick left") },
        { "ANDAR_DIREITA", Axis, 0, 0, +1, "D", TR("analógico esquerdo para a direita", "left stick right") },
        { "OLHAR_CIMA", Axis, 0, 3, +1, "SETA_CIMA", TR("analógico direito para cima (além do mouse)", "right stick up (besides the mouse)") },
        { "OLHAR_BAIXO", Axis, 0, 3, -1, "SETA_BAIXO", TR("analógico direito para baixo", "right stick down") },
        { "OLHAR_ESQUERDA", Axis, 0, 2, -1, "SETA_ESQUERDA", TR("analógico direito para a esquerda", "right stick left") },
        { "OLHAR_DIREITA", Axis, 0, 2, +1, "SETA_DIREITA", TR("analógico direito para a direita", "right stick right") },
    };

    // ------------------------------------------------------------ configuracao do mouse
    struct MouseCfg
    {
        bool camera = true;          // mouse move a camera
        float sens = 1.0f;           // sensibilidade (1 = padrao)
        bool invertY = false;
        float deadZone = 0.24f;      // parte do analogico que o jogo ignora (o mouse comeca logo depois dela)
        float fullSpeed = 1500.0f;   // contagens do mouse por segundo que dao o analogico todo (com sensibilidade 1)
        float lookScale = 0.06f;     // mira direta: giro (unidade do jogo) por contagem do mouse com sensibilidade 1 (~0,07 grau)
        int releaseKey = VK_F2, sensUp[2] = { VK_OEM_PLUS, VK_ADD }, sensDown[2] = { VK_OEM_MINUS, VK_SUBTRACT };
    };
    MouseCfg g_mouse;
    // dispositivos dos jogadores 1 e 2 (ids de controles.h) e se o 2 tambem controla o 1
    std::string g_jogador[4] = { "teclado", "auto", "nenhum", "nenhum" };
    bool g_juntar = false;
    // camera direta no analogico direito (como a mira direta do mouse) e a sua velocidade
    bool g_padDireto = true;
    const ControlesCfg PADRAO_CAMERA;        // valores padrao da camera do controle (controles.h)
    std::atomic<float> g_padVel{ PADRAO_CAMERA.velocidadeCamera };
    std::atomic<float> g_padVelV{ PADRAO_CAMERA.velocidadeVertical };   // sensibilidade vertical
    std::atomic<float> g_padMira{ PADRAO_CAMERA.sensMirando };          // multiplicador com o gatilho esquerdo apertado (mirando)
    std::atomic<uint8_t> g_lTrig[4];         // gatilho esquerdo de cada usuario na ultima leitura
    std::atomic<float> g_padZona{ PADRAO_CAMERA.zonaMorta }, g_padAcel{ PADRAO_CAMERA.aceleracao }, g_padSuave{ PADRAO_CAMERA.suavizacao };   // zona morta, aceleracao, suavizacao
    std::atomic<uint32_t> g_rStick[4];   // analogico direito de cada usuario na ultima leitura (RX << 16 | RY)   // ligado: o dispositivo do jogador 2 tambem controla o jogador 1 e nao ha jogador 2
    std::mutex g_cfgMutex;   // a janela de configuracao troca as teclas enquanto o jogo le
    // Mira direta (padrao): o mouse soma ao giro da camera calculado pelo jogo, sem passar pelo analogico (sem zona morta,
    // suavizacao, aceleracao ou assistencia de mira). FCRECOMP_MOUSEANALOGICO=1 volta ao mouse-como-analogico.
    const bool g_directLook = getenv("FCRECOMP_MOUSEANALOGICO") == nullptr;
    std::atomic<float> g_sens{ 1.0f };
    std::filesystem::path g_iniPath;

    std::filesystem::path ExeDir()
    {
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(nullptr, p, MAX_PATH);
        return std::filesystem::path(p).parent_path();
    }

    std::string Trim(std::string s)
    {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? "" : s.substr(a, b - a + 1);
    }

    std::string KeyNameOf(int vk)
    {
        if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, (char)vk);
        if (vk >= VK_F1 && vk <= VK_F12) return "F" + std::to_string(vk - VK_F1 + 1);
        if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "NUM" + std::to_string(vk - VK_NUMPAD0);
        for (auto& k : KEY_NAMES) if (k.vk == vk) return k.name;
        return "";
    }

    std::string KeyListText(const std::vector<int>& keys)
    {
        std::string s;
        for (int k : keys) { std::string n = KeyNameOf(k); if (n.empty()) continue; if (!s.empty()) s += ", "; s += n; }
        return s;
    }

    // Grava o arquivo inteiro com os valores atuais (a janela de configuracao e as teclas +/- usam isto)
    void WriteIni()
    {
        std::ofstream f(g_iniPath);
        char sens[32]; snprintf(sens, sizeof(sens), "%.2f", g_sens.load());
        f << "; Controles do Far Cry Instincts Predator Recomp. Use a janela de configuracao (F1 no jogo ou\n"
             "; \"Configurar controles.bat\") em vez de editar aqui. Apague este arquivo para voltar ao padrao.\n"
             ";\n"
             "; Dispositivos: teclado, auto (o controle usado por ultimo), controle1-controle4 (1o-4o controle ligado),\n"
             "; sony1-sony4 (DualSense/DualShock 4),\n"
             "; xbox1-xbox4 (controles XInput), nenhum.\n"
             "; Nomes de teclas: A-Z, 0-9, F1-F12, ESPACO, ENTER, ESC, TAB, BACKSPACE, SHIFT_ESQ, SHIFT_DIR, CTRL_ESQ,\n"
             "; CTRL_DIR, ALT_ESQ, ALT_DIR, CAPSLOCK, SETA_CIMA, SETA_BAIXO, SETA_ESQUERDA, SETA_DIREITA, INSERT, DELETE,\n"
             "; HOME, END, PGUP, PGDN, NUM0-NUM9, NUM_MAIS, NUM_MENOS, MAIS, MENOS, VIRGULA, PONTO,\n"
             "; MOUSE_ESQ, MOUSE_DIR, MOUSE_MEIO, MOUSE4, MOUSE5, RODA_CIMA, RODA_BAIXO.\n"
             "\n[jogadores]\n"
             "jogador1 = " << g_jogador[0] << "\n"
             "jogador2 = " << g_jogador[1] << "\n"
             "jogador3 = " << g_jogador[2] << "\n"
             "jogador4 = " << g_jogador[3] << "\n"
             "; 1 = o dispositivo do jogador 2 tambem controla o jogador 1 (jogo de 1 pessoa)\n"
             "juntar = " << (g_juntar ? 1 : 0) << "\n"
             "\n[controle]\n"
             "; 1 = o analogico direito gira a camera direto, com curva de precisao (como o mouse); 0 = como o jogo\n"
             "camera_direta = " << (g_padDireto ? 1 : 0) << "\n"
             "; sensibilidade do analogico direito (0.2 a 5): horizontal e vertical\n"
             "velocidade_camera = " << g_padVel.load() << "\n"
             "velocidade_camera_vertical = " << g_padVelV.load() << "\n"
             "; multiplica a sensibilidade com o gatilho esquerdo apertado, mirando (0.1 a 2)\n"
             "sensibilidade_mirando = " << g_padMira.load() << "\n"
             "; parte do analogico que nao gira a camera (0 a 0.5)\n"
             "zona_morta_camera = " << g_padZona.load() << "\n"
             "; giro extra com o analogico no fim do curso, crescendo em ~0,3 s (0 a 2)\n"
             "aceleracao_camera = " << g_padAcel.load() << "\n"
             "; 0 = resposta na hora; ate 0.9 = o giro segue o analogico devagar (mais suave)\n"
             "suavizacao_camera = " << g_padSuave.load() << "\n"
             "\n[mouse]\n"
             "camera_com_mouse = " << (g_mouse.camera ? 1 : 0) << "\n"
             "sensibilidade = " << sens << "\n"
             "inverter_y = " << (g_mouse.invertY ? 1 : 0) << "\n"
             "zona_morta = " << g_mouse.deadZone << "\n"
             "soltar_mouse = " << KeyNameOf(g_mouse.releaseKey) << "\n"
             "sensibilidade_mais = " << KeyListText({ g_mouse.sensUp[0], g_mouse.sensUp[1] }) << "\n"
             "sensibilidade_menos = " << KeyListText({ g_mouse.sensDown[0], g_mouse.sensDown[1] }) << "\n"
             "\n[teclas]\n";
        for (auto& a : g_actions)
        {
            std::string line = std::string(a.name) + " = " + KeyListText(a.keys);
            f << line << std::string(line.size() < 34 ? 34 - line.size() : 1, ' ') << "; " << a.help << "\n";
        }
    }

    std::vector<int> ParseKeyList(const std::string& v, const char* what)
    {
        std::vector<int> keys;
        std::stringstream ss(v);
        for (std::string k; std::getline(ss, k, ',');)
        {
            k = Trim(k);
            if (k.empty()) continue;
            if (int vk = ParseKey(k)) keys.push_back(vk);
            else Log("[input] controles.ini: tecla desconhecida \"%s\" em %s", k.c_str(), what);
        }
        return keys;
    }

    void LoadConfig()
    {
        g_iniPath = ExeDir() / "controles.ini";
        bool existe = std::filesystem::exists(g_iniPath);
        for (auto& a : g_actions) a.keys = ParseKeyList(a.defaults, a.name);

        std::ifstream f(g_iniPath);
        bool temVertical = false;
        for (std::string line; std::getline(f, line);)
        {
            if (size_t c = line.find(';'); c != std::string::npos) line.resize(c);
            line = Trim(line);
            if (line.empty() || line[0] == '[') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = Trim(line.substr(0, eq)), val = Trim(line.substr(eq + 1));
            for (auto& ch : key) ch = (char)toupper((unsigned char)ch);
            if (key.size() == 8 && key.rfind("JOGADOR", 0) == 0 && key[7] >= '1' && key[7] <= '4')
            {
                for (auto& ch : val) ch = (char)tolower((unsigned char)ch);
                g_jogador[key[7] - '1'] = val;
            }
            else if (key == "JUNTAR") g_juntar = atoi(val.c_str()) != 0;
            else if (key == "CAMERA_DIRETA") g_padDireto = atoi(val.c_str()) != 0;
            else if (key == "VELOCIDADE_CAMERA") g_padVel = std::clamp((float)atof(val.c_str()), 0.2f, 5.0f);
            else if (key == "VELOCIDADE_CAMERA_VERTICAL") { g_padVelV = std::clamp((float)atof(val.c_str()), 0.2f, 5.0f); temVertical = true; }
            else if (key == "SENSIBILIDADE_MIRANDO") g_padMira = std::clamp((float)atof(val.c_str()), 0.1f, 2.0f);
            else if (key == "ZONA_MORTA_CAMERA") g_padZona = std::clamp((float)atof(val.c_str()), 0.0f, 0.5f);
            else if (key == "ACELERACAO_CAMERA") g_padAcel = std::clamp((float)atof(val.c_str()), 0.0f, 2.0f);
            else if (key == "SUAVIZACAO_CAMERA") g_padSuave = std::clamp((float)atof(val.c_str()), 0.0f, 0.9f);
            else if (key == "CAMERA_COM_MOUSE") g_mouse.camera = atoi(val.c_str()) != 0;
            else if (key == "SENSIBILIDADE") g_mouse.sens = std::clamp((float)atof(val.c_str()), 0.1f, 10.0f);
            else if (key == "INVERTER_Y") g_mouse.invertY = atoi(val.c_str()) != 0;
            else if (key == "ZONA_MORTA") g_mouse.deadZone = std::clamp((float)atof(val.c_str()), 0.0f, 0.9f);
            else if (key == "SOLTAR_MOUSE") { auto k = ParseKeyList(val, "soltar_mouse"); g_mouse.releaseKey = k.empty() ? 0 : k[0]; }
            else if (key == "SENSIBILIDADE_MAIS" || key == "SENSIBILIDADE_MENOS")
            {
                auto k = ParseKeyList(val, key.c_str());
                int* dst = key == "SENSIBILIDADE_MAIS" ? g_mouse.sensUp : g_mouse.sensDown;
                dst[0] = k.size() > 0 ? k[0] : 0; dst[1] = k.size() > 1 ? k[1] : 0;
            }
            else
            {
                bool found = false;
                for (auto& a : g_actions)
                    if (key == a.name) { a.keys = ParseKeyList(val, a.name); found = true; }
                if (!found) Log("[input] controles.ini: opcao desconhecida \"%s\"", key.c_str());
            }
        }
        g_sens = g_mouse.sens;
        for (int i = 1; i < 4; i++)   // o mesmo dispositivo em dois jogadores nao faz sentido: vai para "auto" ou nenhum
            for (int k = 0; k < i; k++)
                if (g_jogador[i] == g_jogador[k] && g_jogador[i] != "nenhum")
                {
                    std::string livre = "nenhum";
                    for (const char* c : { "auto" })
                        if (std::none_of(g_jogador, g_jogador + 4, [&](const std::string& s) { return s == c; })) { livre = c; break; }
                    Log("[input] jogador %d com o mesmo dispositivo do jogador %d (%s): passa para %s", i + 1, k + 1,
                        g_jogador[i].c_str(), livre.c_str());
                    g_jogador[i] = livre;
                    break;
                }
        if (!temVertical) g_padVelV = g_padVel.load();   // ini antigo: vertical igual a horizontal
        if (!existe) WriteIni();
        PadsIniciar();
        Log("[input] jogador 1: %s, jogador 2: %s, jogador 3: %s, jogador 4: %s, juntar %s", g_jogador[0].c_str(),
            g_jogador[1].c_str(), g_jogador[2].c_str(), g_jogador[3].c_str(), g_juntar ? "sim" : "nao");
        Log("[input] controles.ini lido: mouse na camera %s, sensibilidade %.2f, inverter Y %s, zona morta %.2f",
            g_mouse.camera ? "sim" : "nao", g_mouse.sens, g_mouse.invertY ? "sim" : "nao", g_mouse.deadZone);
    }

    void SaveSensitivity(float) { WriteIni(); }

    std::once_flag g_configOnce;
    void EnsureConfig() { std::call_once(g_configOnce, LoadConfig); }

    // ------------------------------------------------------------ mouse (Raw Input: movimento sem aceleracao do Windows)
    std::atomic<bool> g_captured{ false };
    std::atomic<bool> g_releasedByUser{ false };
    std::atomic<ULONGLONG> g_wheelUpUntil{ 0 }, g_wheelDownUntil{ 0 };
    std::atomic<ULONGLONG> g_sensShownUntil{ 0 };

    bool Focused() { HWND w = GetGameWindow(); return w && GetForegroundWindow() == w; }

    // Prende o cursor num ponto no centro da janela: o movimento vem do Raw Input (nao depende da posicao do cursor),
    // e com a area inteira da janela o cursor chegava nas bordas de baixo/direita (escala do Windows acima de 100%:
    // as coordenadas da janela e da tela nao batiam) e aparecia a seta de redimensionar
    void ClipToCenter(HWND hwnd)
    {
        RECT r; GetClientRect(hwnd, &r);
        POINT c{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
        ClientToScreen(hwnd, &c);
        RECT clip{ c.x, c.y, c.x + 1, c.y + 1 };
        ClipCursor(&clip);
    }

    void Capture(HWND hwnd, bool on)
    {
        if (on)
        {
            ClipToCenter(hwnd);
            SetCursor(nullptr);
        }
        else
            ClipCursor(nullptr);
        if (g_captured.exchange(on) != on) Log("[input] mouse %s", on ? "preso na janela (F2 solta)" : "solto");
    }

    bool Key(int vk)
    {
        if (vk == VK_RODA_CIMA) return GetTickCount64() < g_wheelUpUntil;
        if (vk == VK_RODA_BAIXO) return GetTickCount64() < g_wheelDownUntil;
        bool mouseButton = vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
        if (mouseButton && !g_captured) return false;   // cliques com o cursor solto nao viram tiro
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    }

    void ReadKeyboard(XINPUT_GAMEPAD& g)
    {
        if (!Focused()) return;
        std::lock_guard lock(g_cfgMutex);
        for (auto& a : g_actions)
        {
            bool down = false;
            for (int k : a.keys) down = down || Key(k);
            if (!down) continue;
            switch (a.kind)
            {
            case Button: g.wButtons |= a.bit; break;
            case LeftTrigger: g.bLeftTrigger = 255; break;
            case RightTrigger: g.bRightTrigger = 255; break;
            case Axis:
            {
                SHORT* ax[4] = { &g.sThumbLX, &g.sThumbLY, &g.sThumbRX, &g.sThumbRY };
                *ax[a.axis] = a.dir > 0 ? 32767 : -32767;
                break;
            }
            }
        }
    }

    // Movimentos do mouse com o horario em que chegaram (Raw Input). Cada lugar do jogo que le o controle (pelo
    // endereco de retorno) recebe o movimento desde a SUA leitura anterior, dividido pelo tempo: velocidade -> analogico
    // direito. Antes um contador unico era "gasto" por quem lesse primeiro e as outras leituras viam 0 (a camera andava
    // e parava: "pulando"). A aceleracao do analogico do jogo fica desligada pelo ajuste em fs.cpp (lua.dat).
    struct MouseEvent { int64_t t; long dx, dy; };
    std::mutex g_mouseMutex;
    std::vector<MouseEvent> g_mouseEvents;   // ultimos ~250 ms

    int64_t Qpc() { LARGE_INTEGER n; QueryPerformanceCounter(&n); return n.QuadPart; }
    double QpcFreq() { static double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return (double)q.QuadPart; }(); return f; }

    // Mira direta: contagens do mouse ainda nao aplicadas a camera (consumidas uma vez por quadro em sub_8228BC78)
    std::atomic<long> g_lookDX{ 0 }, g_lookDY{ 0 };
    // ultima vez que o jogo chamou o giro da camera do jogador (GUEST_SUB_MIRA). No editor de mapas, fora do "JUMP IN
    // MAP" (camera voando do editor) ninguem chama: ai o mouse volta a ser o analogico direito.
    std::atomic<ULONGLONG> g_miraTick{ 0 };
    bool MiraDireta()
    {
#ifdef PREDATOR_EDITOR
        return g_directLook && GetTickCount64() - g_miraTick.load() < 250;
#else
        return g_directLook;
#endif
    }

    void AddMouseMove(long dx, long dy)
    {
        g_lookDX += dx; g_lookDY += dy;
        std::lock_guard lock(g_mouseMutex);
        int64_t now = Qpc(), keep = (int64_t)(0.25 * QpcFreq());
        g_mouseEvents.push_back({ now, dx, dy });
        size_t drop = 0;
        while (drop < g_mouseEvents.size() && now - g_mouseEvents[drop].t > keep) drop++;
        if (drop) g_mouseEvents.erase(g_mouseEvents.begin(), g_mouseEvents.begin() + drop);
    }

    void ReadMouse(XINPUT_GAMEPAD& g, uint32_t caller)
    {
        struct Reader { int64_t last = 0; float outX = 0, outY = 0; };
        static std::mutex readersMutex;
        static std::unordered_map<uint32_t, Reader> readers;
        std::lock_guard rl(readersMutex);
        Reader& rd = readers[caller];
        if (!g_mouse.camera || !g_captured || !Focused()) { rd = {}; return; }
        if (HWND w = GetGameWindow()) ClipToCenter(w);   // o Windows pode soltar o cursor (ex.: outra janela): prende de novo
        if (MiraDireta()) return;   // mira direta: o mouse vai direto para o giro da camera (sub_8228BC78)

        int64_t now = Qpc();
        double dt = rd.last ? (now - rd.last) / QpcFreq() : 0.0;
        if (dt >= 0.004 || !rd.last)
        {
            long dx = 0, dy = 0;
            {
                std::lock_guard lock(g_mouseMutex);
                for (auto& e : g_mouseEvents)
                    if (e.t > rd.last && e.t <= now) { dx += e.dx; dy += e.dy; }
            }
            if (!rd.last || dt > 0.2) dx = dy = 0;   // primeira leitura ou o jogo ficou sem ler (carregamento)
            rd.last = now;
            float k = dt > 0 ? g_sens.load() / (g_mouse.fullSpeed * (float)dt) : 0.0f;
            auto curve = [&](float v) {   // velocidade do mouse -> deflexao do analogico, pulando a zona morta do jogo
                if (v == 0) return 0.0f;
                float m = std::min(std::fabs(v) / g_cameraSpeedBoost, 1.0f);   // giro maximo aumentado (fs.cpp)
                return std::copysign(g_mouse.deadZone + (1.0f - g_mouse.deadZone) * m, v);
            };
            rd.outX = curve(dx * k);
            rd.outY = curve(-dy * k * (g_mouse.invertY ? -1.0f : 1.0f));
            static const bool mouseLog = getenv("FCRECOMP_MOUSELOG") != nullptr;   // depuracao: valores do mouse
            if (mouseLog && (dx || dy))
                Log("[mouse] leitor %08X: %ld,%ld em %.1f ms -> analogico direito %.2f,%.2f", caller, dx, dy, dt * 1000, rd.outX, rd.outY);
        }

        // soma com o controle/teclado (o maior em modulo vence)
        auto mix = [](SHORT& axis, float v) {
            int m = (int)std::lround(v * 32767.0f);
            if (std::abs(m) > std::abs((int)axis)) axis = (SHORT)std::clamp(m, -32767, 32767);
        };
        mix(g.sThumbRX, rd.outX);
        mix(g.sThumbRY, rd.outY);
    }

    void ChangeSensitivity(float factor)
    {
        float s = std::clamp(std::round(g_sens.load() * factor * 100.0f) / 100.0f, 0.1f, 10.0f);
        g_sens = s;
        g_sensShownUntil = GetTickCount64() + 3000;
        Log("[input] sensibilidade do mouse: %.2f", s);
        SaveSensitivity(s);
    }
}

// Chamado por content.cpp na 1a leitura do perfil (o menu principal apareceu): marca o tempo dos itens "M" do roteiro
// Chamado por gpu.cpp quando a fase comeca (FCRECOMP_MISSAO): marca o tempo dos itens "F" do roteiro
void InputMarkMission()
{
    DWORD expected = 0;
    if (g_missionTick.compare_exchange_strong(expected, GetTickCount()))
        Log("[input] fase comecou aos %.1f s", (GetTickCount() - g_startTick) / 1000.0);
}

void InputMarkMenu()
{
    DWORD expected = 0;
    if (g_menuTick.compare_exchange_strong(expected, GetTickCount()))
    {
        Log("[input] menu principal aos %.1f s", (GetTickCount() - g_startTick) / 1000.0);
        // depuracao: FCRECOMP_POKEPTR=ponteiro+deslocamento=byte (hex) muda um byte de um objeto 3 s depois do menu
        if (const char* pk = getenv("FCRECOMP_POKEPTR"))
        {
            std::string v = pk;
            std::thread([v]
            {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                uint32_t ptr = (uint32_t)strtoul(v.c_str(), nullptr, 16);
                uint32_t off = v.find('+') != std::string::npos ? (uint32_t)strtoul(v.c_str() + v.find('+') + 1, nullptr, 16) : 0;
                uint32_t val = v.find('=') != std::string::npos ? (uint32_t)strtoul(v.c_str() + v.find('=') + 1, nullptr, 16) : 0;
                if (uint32_t obj = R32(ptr)) { Log("[input] poke %08X+%X: %02X -> %02X", obj, off, R8(obj + off), val); W8(obj + off, (uint8_t)val); }
            }).detach();
        }
        // depuracao: FCRECOMP_DUMPDADOS=arquivo grava a area de dados do executavel 6 s depois do menu principal
        // (arquivo_<etapa>.bin; comparar o estado do jogo entre dois caminhos)
        if (const char* arq = getenv("FCRECOMP_DUMPDADOS"))
        {
            std::string nome = std::string(arq) + "_" + (getenv("PREDATOR_ETAPA") ? getenv("PREDATOR_ETAPA") : "0") + ".bin";
            std::thread([nome]
            {
                std::this_thread::sleep_for(std::chrono::seconds(6));
                const uint32_t ini = 0x82880000, fim = (uint32_t)(GUEST_IMAGE_BASE + GUEST_IMAGE_SIZE);
                if (FILE* f = fopen(nome.c_str(), "wb")) { fwrite(G(ini), 1, fim - ini, f); fclose(f); Log("[input] dados gravados em %s", nome.c_str()); }
                // FCRECOMP_DUMPPTR=endereco: tambem os 0x2000 bytes do objeto apontado por [endereco] (arquivo_<etapa>_obj.bin)
                if (const char* pp = getenv("FCRECOMP_DUMPPTR"))
                    if (uint32_t obj = R32((uint32_t)strtoul(pp, nullptr, 16)))
                        if (FILE* f = fopen((nome + "_obj.bin").c_str(), "wb")) { fwrite(G(obj), 1, 0x2000, f); fclose(f); Log("[input] objeto %08X gravado", obj); }
            }).detach();
        }
    }
}

// Chamado pela janela do jogo (gpu.cpp) logo depois de cria-la
void InputInit(HWND hwnd)
{
    EnsureConfig();
    RAWINPUTDEVICE rid{ 0x01, 0x02, 0, hwnd };   // mouse generico
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) Log("[input] RegisterRawInputDevices falhou (%lu)", GetLastError());
}

// Mensagens da janela do jogo. Retorna true se a mensagem foi tratada (result = valor de retorno)
bool TecladoAberto();                      // teclado.cpp: caixa de digitacao do teclado do sistema

bool InputWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result)
{
    switch (msg)
    {
    case WM_INPUT:
    {
        RAWINPUT ri; UINT size = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && g_captured)
        {
            if (!(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && (ri.data.mouse.lLastX || ri.data.mouse.lLastY))
                AddMouseMove(ri.data.mouse.lLastX, ri.data.mouse.lLastY);
            if (ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)
            {
                SHORT d = (SHORT)ri.data.mouse.usButtonData;
                (d > 0 ? g_wheelUpUntil : g_wheelDownUntil) = GetTickCount64() + 120;   // aperto curto
            }
        }
        return false;   // DefWindowProc precisa ver o WM_INPUT
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) Capture(hwnd, false);
        else if (!g_releasedByUser) Capture(hwnd, true);   // Alt+Tab de volta
        return false;
    case WM_LBUTTONDOWN:
        if (!g_captured) { g_releasedByUser = false; Capture(hwnd, true); }   // clique na janela prende o mouse
        return false;
    case WM_SIZE: case WM_MOVE:
        if (g_captured) Capture(hwnd, true);   // atualiza a area em que o cursor fica preso
        return false;
    case WM_SETCURSOR:
        if (g_captured && LOWORD(lp) == HTCLIENT) { SetCursor(nullptr); result = TRUE; return true; }
        return false;
    case WM_KEYDOWN:
    {
        int vk = (int)wp;
        if (vk == VK_F1) { ControlesAbrirJanela(false); result = 0; return true; }   // janela de configuracao dos controles
        if (vk && vk == g_mouse.releaseKey)
        {
            bool release = g_captured;
            g_releasedByUser = release;
            Capture(hwnd, !release);
            result = 0; return true;
        }
        if (vk && (vk == g_mouse.sensUp[0] || vk == g_mouse.sensUp[1])) { ChangeSensitivity(1.1f); result = 0; return true; }
        if (vk && (vk == g_mouse.sensDown[0] || vk == g_mouse.sensDown[1])) { ChangeSensitivity(1.0f / 1.1f); result = 0; return true; }
        return false;
    }
    }
    return false;
}

namespace { std::string RoteiroStatus(); }   // abaixo: gravacao/repeticao do roteiro

// Texto extra para o titulo da janela
std::string InputStatus()
{
    std::string s;
    if (GetTickCount64() < g_sensShownUntil)
    {
        char b[64]; snprintf(b, sizeof(b), "  [sensibilidade do mouse %.2f]", g_sens.load());
        s += b;
    }
    if (g_mouse.camera && !g_captured && Focused()) s += "  [mouse solto: clique na janela ou F2 para prender]";
    return s + RoteiroStatus();
}

// Roteiro gravado de quem joga ("Gravar roteiro.bat" / tools\gravar_roteiro.ps1; repetir: tools\repetir_roteiro.ps1):
//   FCRECOMP_GRAVAR=arquivo     grava desde a abertura do programa (videos, menus, carregamentos, fase, editor de mapas)
//                               ate F4 ou fechar; continua nos programas reabertos pelas trocas (PREDATOR_ETAPA, xam.cpp)
//   FCRECOMP_REPRODUZIR=arquivo o jogo recebe o que foi gravado (controle, teclado, mouse e camera reais ignorados)
// Relogio do roteiro ("tique"): cada troca de tela (VdSwap) em que o jogo leu o controle 1. No menu, na fase e no editor
// o jogo le 1 vez por quadro; nos carregamentos nao le nada (o relogio para: a duracao do carregamento nao importa); no
// video de abertura le centenas de vezes por quadro (contam como 1 tique). Quando o jogo volta a ler depois de 15 trocas
// ou mais sem leitura (fim de um carregamento) ha um "marco": na repeticao o relogio e realinhado no mesmo marco.
// O giro da camera que o runtime soma no gancho da mira (mouse e camera direta do analogico, que dependem do tempo real
// entre quadros) e gravado ja calculado, em floats exatos; na repeticao o mesmo giro entra no mesmo tique.
// O jogador 2 fica desligado durante a gravacao e a repeticao (o roteiro e so do jogador 1).
// Arquivo (texto), uma secao por programa:
//   ROTEIRO 2                         1a linha
//   ETAPA n programa hora             inicio de cada programa (hora do sistema, FILETIME: a repeticao usa a mesma)
//   K tique nome n                    marco (n = quantos desse nome antes, neste programa)
//   E tique botoes LT RT LX LY RX RY  estado do controle 1 quando muda (botoes em hexadecimal)
//   Z tique 0|1                       camera direta ligada (o jogo recebe o analogico direito zerado no giro)
//   C tique dx dy                     giro somado pelo runtime neste tique (bits do float em hexadecimal)
//   P tique x y z                     posicao do ouvinte (camera) a cada 30 tiques, so no jogo: a repeticao compara
//   T tique                           a cada 30 tiques (a repeticao vai ate o ultimo)
//   FIM tique                         F4
// FCRECOMP_ROTEIROFOTOS="0:300,1:900" (etapa:tique) grava a imagem desses tiques em foto_e<etapa>_t<tique>.bmp (pasta
// FCRECOMP_ROTEIROFOTOSDIR ou a do jogo), na gravacao e na repeticao, para comparar.
void GpuPedirFoto(uint64_t troca, const std::string& arquivo);   // gpu.cpp
uint64_t HoraDoJogo();                                            // kernel.cpp
uint64_t AudioFramesSubmitted();                                  // audio.cpp
extern std::atomic<int64_t> g_relogioDesvio;                      // kernel.cpp
extern uint64_t g_tscFrequency;                                   // kernel.cpp (frequencia do mftb = rdtsc)
namespace
{
    int Etapa() { return getenv("PREDATOR_ETAPA") ? atoi(getenv("PREDATOR_ETAPA")) : 0; }

    void Beeps(int n)   // aviso sonoro (em tela cheia o titulo da janela nao aparece)
    {
        std::thread([n] { for (int i = 0; i < n; i++) { Beep(i ? 1320 : 880, 150); Sleep(80); } }).detach();
    }

    std::mutex g_recMutex;   // tudo abaixo (thread do jogo e F4)
    FILE* g_recFile = nullptr;
    enum class RecState { Off, Recording, Done } g_recState = RecState::Off;

    // ------------------------------------------------------------ relogio
    std::atomic<uint64_t> g_trocas{ 0 };   // VdSwap pedidas pela CPU neste programa
    uint64_t g_trocaLeitura = 0;           // troca da ultima leitura do controle 1
    int64_t g_tique = 0;                   // tiques deste programa
    int64_t g_desvio = 0;                  // repeticao: tique da gravacao = g_tique + g_desvio
    std::unordered_map<std::string, int> g_marcosVistos;
    std::atomic<bool> g_fotoArmada{ false };
    std::string g_fotoNome;

    // ------------------------------------------------------------ repeticao
    bool g_rep = false;
    struct Marco { std::string nome; int n; int64_t tique; };
    std::vector<Marco> g_repMarcos;
    std::vector<std::pair<int64_t, XINPUT_GAMEPAD>> g_repPad;   // em ordem de tique
    std::vector<std::pair<int64_t, bool>> g_repDireta;
    std::unordered_map<int64_t, std::pair<float, float>> g_repGiro;
    std::unordered_map<int64_t, std::array<float, 3>> g_repPos;
    std::unordered_map<int64_t, uint64_t> g_repDur;
    std::unordered_map<int64_t, std::pair<float, float>> g_repTotal;   // giro final da camera em cada tique, linhas "A"
    int64_t g_repUltimoTotal = -1;   // duracao gravada de cada tique (ns), linhas "D"
    int64_t g_repFim = -1;
    bool g_repUltimaEtapa = true;   // a gravacao termina neste programa (senao segura o ultimo estado ate a troca)
    bool g_repAcabou = false;
    int64_t g_repUltimoGiro = -1;
    double g_maiorDif = 0;
    int64_t g_maiorDifTique = 0;

    int64_t TiqueRoteiro() { return g_tique + g_desvio; }

    float BitsFloat(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
    uint32_t FloatBits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

    void CarregarRoteiro(const char* path)
    {
        FILE* f = fopen(path, "r");
        if (!f) { Log("[roteiro] nao abriu %s", path); return; }
        const int etapa = Etapa();
        char line[200];
        if (!fgets(line, sizeof(line), f) || strncmp(line, "ROTEIRO 2", 9) != 0)
        {
            Log("[roteiro] %s e de um formato antigo (gravado so a partir da fase); grave de novo", path);
            fclose(f);
            return;
        }
        int secao = -1, ultimaSecao = -1;
        bool achou = false;
        while (fgets(line, sizeof(line), f))
        {
            int e; char prog[64]; unsigned long long hora;
            if (sscanf(line, "ETAPA %d %63s %llu", &e, prog, &hora) == 3)
            {
                secao = e;
                ultimaSecao = std::max(ultimaSecao, e);
                if (e == etapa)
                {
                    achou = true;
                    g_relogioDesvio = (int64_t)(hora - HoraDoJogo());   // a hora do jogo volta para a da gravacao
                    Log("[roteiro] etapa %d gravada no programa %s", e, prog);
                }
                continue;
            }
            if (secao != etapa) continue;
            long long t; unsigned b, lt, rt, ux, uy; int lx, ly, rx, ry, n, z; char nome[32]; float x, y, zz;
            if (sscanf(line, "E %lld %x %u %u %d %d %d %d", &t, &b, &lt, &rt, &lx, &ly, &rx, &ry) == 8)
            {
                XINPUT_GAMEPAD g{};
                g.wButtons = (WORD)b; g.bLeftTrigger = (BYTE)lt; g.bRightTrigger = (BYTE)rt;
                g.sThumbLX = (SHORT)lx; g.sThumbLY = (SHORT)ly; g.sThumbRX = (SHORT)rx; g.sThumbRY = (SHORT)ry;
                g_repPad.push_back({ t, g });
            }
            else if (sscanf(line, "C %lld %x %x", &t, &ux, &uy) == 3)
            {
                auto& c = g_repGiro[t]; c.first += BitsFloat(ux); c.second += BitsFloat(uy);
            }
            else if (sscanf(line, "Z %lld %d", &t, &z) == 2) g_repDireta.push_back({ t, z != 0 });
            else if (sscanf(line, "K %lld %31s %d", &t, nome, &n) == 3) g_repMarcos.push_back({ nome, n, t });
            else if (sscanf(line, "P %lld %f %f %f", &t, &x, &y, &zz) == 4) g_repPos[t] = { x, y, zz };
            else if (unsigned long long ns; sscanf(line, "D %lld %llu", &t, &ns) == 2) g_repDur[t] = ns;
            else if (sscanf(line, "A %lld %x %x", &t, &ux, &uy) == 3)
            {
                auto& c = g_repTotal[t]; c.first += BitsFloat(ux); c.second += BitsFloat(uy);
            }
            else if (sscanf(line, "FIM %lld", &t) == 1) { g_repFim = t; continue; }
            else if (sscanf(line, "T %lld", &t) == 1) {}
            else continue;
            g_repFim = std::max<int64_t>(g_repFim, t);
        }
        fclose(f);
        if (!achou) { Log("[roteiro] %s nao tem a etapa %d: este programa nao repete nada", path, etapa); return; }
        g_rep = true;
        g_repUltimaEtapa = ultimaSecao == etapa;
        Log("[roteiro] repetindo %s, etapa %d: %zu mudancas do controle, %zu tiques com giro da camera, %zu marcos, ate o tique %lld",
            path, etapa, g_repPad.size(), g_repGiro.size(), g_repMarcos.size(), (long long)g_repFim);
        if (g_repDur.empty())
            Log("[roteiro] gravacao sem a duracao dos tiques (feita antes do relogio do roteiro): cada tique vale 1/30 s "
                "(as repeticoes ficam iguais entre si, mas podem se afastar um pouco da gravacao)");
        else
            Log("[roteiro] duracao gravada de %zu tiques: o relogio do jogo repete a da gravacao", g_repDur.size());
        if (!g_repTotal.empty()) Log("[roteiro] giro final da camera gravado em %zu tiques: a repeticao usa esse giro", g_repTotal.size());
    }

    void Escrever(const char* fmt, ...)
    {
        if (g_recState != RecState::Recording) return;
        va_list ap; va_start(ap, fmt); vfprintf(g_recFile, fmt, ap); va_end(ap);
        fflush(g_recFile);   // o jogo pode ser fechado a qualquer momento
    }

    bool Repetindo() { return g_rep && !g_repAcabou; }

    void Marcar(const char* nome)
    {
        int n = g_marcosVistos[nome]++;
        Escrever("K %lld %s %d\n", (long long)g_tique, nome, n);
        if (!Repetindo()) return;
        auto m = std::find_if(g_repMarcos.begin(), g_repMarcos.end(), [&](const Marco& k) { return k.nome == nome && k.n == n; });
        if (m == g_repMarcos.end()) { Log("[roteiro] marco %s %d nao existe na gravacao (tique %lld)", nome, n, (long long)TiqueRoteiro()); return; }
        int64_t novo = m->tique - g_tique;
        Log("[roteiro] marco %s %d: tique %lld, gravado %lld (ajuste %+lld)", nome, n, (long long)TiqueRoteiro(), (long long)m->tique,
            (long long)(novo - g_desvio));
        g_desvio = novo;
    }

    // ------------------------------------------------------------ relogio do jogo (mftb) no roteiro
    // O jogo mede o tempo de cada quadro com o mftb (predator_timebase = rdtsc) e anda/vira/simula conforme esse tempo.
    // Na repeticao cada quadro durava um pouco diferente da gravacao (o PC nao e igual a cada vez): o personagem andava um
    // pouco mais ou menos em cada tique e a diferenca crescia (1 m no tique 450, 50 m no 3060 da gravacao 2026-11-07_10-35).
    // Agora a gravacao anota a duracao de cada tique ("D tique ns") e na repeticao o relogio que o jogo le anda exatamente
    // isso por tique: ele comeca cada tique onde o anterior devia terminar e, se o quadro real demorar mais, para no fim do
    // tique (ate FOLGA de tempo real; depois disso - carregamento, travada - volta a andar, para nada ficar esperando
    // para sempre). Gravacoes sem linhas "D": 1/30 s por tique. FCRECOMP_ROTEIRORELOGIOREAL=1 desliga (relogio real).
    // Pausa do relogio (item 33): enquanto a placa compila os shaders de uma fase nova a imagem fica parada
    // (xenos_draw.cpp, assinc::EsperarFila) e o relogio do jogo tambem, para a cutscene/o jogo nao andar escondido.
    // Relogio sem pausa = rdtsc - g_pausaDesvio; parado = g_pausaCongelado. Ao voltar o desvio muda antes de soltar,
    // entao o relogio nunca anda para tras.
    std::atomic<uint64_t> g_pausaDesvio{ 0 }, g_pausaCongelado{ 0 };
    uint64_t TscSemPausa(uint64_t real)
    {
        if (uint64_t c = g_pausaCongelado.load(std::memory_order_acquire)) return c;
        return real - g_pausaDesvio.load(std::memory_order_acquire);
    }
    struct EstadoRelogio { int64_t desvio; uint64_t limite; };   // relogio do jogo = rdtsc + desvio, parado em "limite"
    EstadoRelogio g_relEstados[64] = {};
    std::atomic<uint32_t> g_relIdx{ 0 };
    std::atomic<bool> g_relLigado{ false };
    uint64_t g_relFolga = 0;            // tempo real (unidades do rdtsc) que o relogio pode ficar parado no fim do tique
    uint64_t g_gravInicioTique = 0;     // gravacao: rdtsc do inicio do tique atual

    uint64_t RelogioVirtual(const EstadoRelogio& e, uint64_t real)
    {
        uint64_t v = real + (uint64_t)e.desvio;
        if (v > e.limite) { uint64_t passou = v - e.limite; v = passou < g_relFolga ? e.limite : v - g_relFolga; }
        return v;
    }

    // inicio de um tique (thread do jogo, com g_recMutex)
    void RelogioTique()
    {
        const uint64_t real = TscSemPausa(__rdtsc());
        if (g_recState == RecState::Recording)
        {
            if (g_gravInicioTique && g_tscFrequency)
                Escrever("D %lld %llu\n", (long long)(g_tique - 1),
                    (unsigned long long)((double)(real - g_gravInicioTique) * 1e9 / (double)g_tscFrequency));
            g_gravInicioTique = real;
        }
        static const bool real_ = getenv("FCRECOMP_ROTEIRORELOGIOREAL") != nullptr;
        if (!g_rep || real_ || !g_tscFrequency) return;
        const uint32_t idx = g_relIdx.load();
        const bool ligado = g_relLigado.load();
        const EstadoRelogio atual = g_relEstados[idx & 63];
        const uint64_t agora = ligado ? RelogioVirtual(atual, real) : real;
        EstadoRelogio novo{ ligado ? atual.desvio : 0, UINT64_MAX };
        if (Repetindo())
        {
            // comeca onde o tique anterior devia terminar (se o quadro real foi mais curto, pula para la)
            const uint64_t inicio = ligado && atual.limite != UINT64_MAX ? std::max(atual.limite, agora) : agora;
            const int64_t t = TiqueRoteiro();
            auto it = g_repDur.find(t);
            uint64_t ns = it != g_repDur.end() ? it->second : g_repDur.empty() ? 33333333ull : 0;
            if (ns)   // tique sem duracao numa gravacao que tem (ultimo de uma etapa): relogio real
            {
                novo.desvio = (int64_t)(inicio - real);
                novo.limite = inicio + (uint64_t)((double)ns * (double)g_tscFrequency / 1e9);
            }
            else novo.desvio = (int64_t)(agora - real);
        }
        else novo.desvio = (int64_t)(agora - real);   // fim da repeticao: continua daqui, sem parar
        if (!g_relFolga) g_relFolga = g_tscFrequency / 10;   // 100 ms
        g_relEstados[(idx + 1) & 63] = novo;
        g_relIdx.store(idx + 1, std::memory_order_release);
        g_relLigado.store(true, std::memory_order_release);
    }

    // cada leitura do controle 1: avanca o relogio na 1a leitura depois de uma troca de tela
    void AvancarRelogio()
    {
        uint64_t t = g_trocas.load();
        if (g_tique && t == g_trocaLeitura) return;
        bool retomada = !g_tique || t - g_trocaLeitura >= 15;
        g_trocaLeitura = t;
        g_tique++;
        if (g_tique % 30 == 0) Escrever("T %lld\n", (long long)g_tique);   // a repeticao vai ate o fim da gravacao
        if (g_tique % 30 == 0 && Repetindo())   // para alinhar o som gravado (FCRECOMP_SOMWAV) com os tiques
            Log("[roteiro] tique %lld: quadro de som %llu", (long long)TiqueRoteiro(), (unsigned long long)AudioFramesSubmitted());
        if (retomada) Marcar("retomada");
        RelogioTique();   // depois do marco: o tique da gravacao ja esta realinhado
        // fotos pedidas (FCRECOMP_ROTEIROFOTOS): a imagem deste tique sai na proxima troca de tela
        static const char* fotos = getenv("FCRECOMP_ROTEIROFOTOS");
        if (fotos && (Repetindo() || g_recState == RecState::Recording))
        {
            const int etapa = Etapa();
            const int64_t tq = TiqueRoteiro();
            for (const char* p = fotos; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : nullptr)
                if (atoi(p) == etapa && strchr(p, ':') && atoll(strchr(p, ':') + 1) == tq)
                {
                    const char* dir = getenv("FCRECOMP_ROTEIROFOTOSDIR");
                    char b[64]; snprintf(b, sizeof(b), "foto_e%d_t%06lld.bmp", etapa, (long long)tq);
                    g_fotoNome = dir ? std::string(dir) + "\\" + b : std::string(b);
                    g_fotoArmada = true;
                }
        }
        // FCRECOMP_ROTEIROCAPTURA=etapa:tique[,...]: captura completa (como o F9) nesses tiques da repeticao
        static const char* capturas = getenv("FCRECOMP_ROTEIROCAPTURA");
        if (capturas && Repetindo())
        {
            const int etapa = Etapa();
            const int64_t tq = TiqueRoteiro();
            for (const char* p = capturas; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : nullptr)
                if (atoi(p) == etapa && strchr(p, ':') && atoll(strchr(p, ':') + 1) == tq)
                {
                    Log("[roteiro] captura pedida no tique %lld", (long long)tq);
                    diag::RequestCapture();
                }
        }
    }

    // leitura do controle 1 (thread do jogo): relogio, repeticao e gravacao
    void RoteiroLeitura(XINPUT_GAMEPAD& g)
    {
        std::lock_guard lock(g_recMutex);
        AvancarRelogio();
        if (Repetindo())
        {
            int64_t t = TiqueRoteiro();
            // FCRECOMP_ROTEIROATE=tique: termina antes (o jogo fica naquele ponto, com o controle real)
            static const int64_t ate = getenv("FCRECOMP_ROTEIROATE") ? atoll(getenv("FCRECOMP_ROTEIROATE")) : 0;
            const int64_t fim = ate > 0 && ate < g_repFim ? ate : g_repFim;
            if (t > fim && g_repUltimaEtapa)   // depois do fim: o controle volta a ser o real
            {
                g_repAcabou = true;
                Log("[roteiro] fim da gravacao no tique %lld; maior diferenca de posicao da camera: %.3f m (tique %lld)",
                    (long long)t, g_maiorDif, (long long)g_maiorDifTique);
                Beeps(2);
            }
            else
            {
                auto it = std::upper_bound(g_repPad.begin(), g_repPad.end(), t, [](int64_t v, const auto& e) { return v < e.first; });
                g = it == g_repPad.begin() ? XINPUT_GAMEPAD{} : std::prev(it)->second;
            }
        }
        if (g_recState == RecState::Recording)
        {
            static XINPUT_GAMEPAD last{};
            static bool first = true;
            if (first || memcmp(&g, &last, sizeof(g)))
            {
                first = false;
                last = g;
                Escrever("E %lld %04X %u %u %d %d %d %d\n", (long long)g_tique, g.wButtons, g.bLeftTrigger, g.bRightTrigger,
                    g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY);
            }
        }
    }

    // gancho da mira (thread do jogo), jogador 1: camera direta ligada neste tique (repeticao: a gravada)
    bool RoteiroCameraDireta(bool real)
    {
        std::lock_guard lock(g_recMutex);
        if (Repetindo())
        {
            int64_t t = TiqueRoteiro();
            auto it = std::upper_bound(g_repDireta.begin(), g_repDireta.end(), t, [](int64_t v, const auto& e) { return v < e.first; });
            return it != g_repDireta.begin() && std::prev(it)->second;
        }
        static int ultimo = -1;
        if (g_recState == RecState::Recording && ultimo != (int)real) { ultimo = real; Escrever("Z %lld %d\n", (long long)g_tique, (int)real); }
        return real;
    }

    bool RoteiroGiroRepetindo() { std::lock_guard lock(g_recMutex); return Repetindo(); }

    // repeticao: giro gravado para este tique (uma vez por tique)
    bool RoteiroGiro(float& dx, float& dy)
    {
        std::lock_guard lock(g_recMutex);
        if (!Repetindo()) return false;
        dx = dy = 0;
        int64_t t = TiqueRoteiro();
        if (t == g_repUltimoGiro) return true;
        g_repUltimoGiro = t;
        if (auto it = g_repGiro.find(t); it != g_repGiro.end()) { dx = it->second.first; dy = it->second.second; }
        return true;
    }

    // gravacao: giro somado pelo runtime neste chamado da mira
    void RoteiroAnotarGiro(float dx, float dy)
    {
        std::lock_guard lock(g_recMutex);
        if (dx != 0 || dy != 0) Escrever("C %lld %08X %08X\n", (long long)g_tique, FloatBits(dx), FloatBits(dy));
    }

    // Giro FINAL da camera no quadro (entrada+188/+192 depois do jogo e do runtime). O jogo soma ao analogico a
    // assistencia de mira, que puxa a camera para os inimigos: como eles nunca ficam exatamente no mesmo lugar, a
    // camera virava diferente em cada repeticao (posicao a 0,3 m da gravacao, mas olhando para outro lado). Agora a
    // gravacao anota o giro final ("A tique x y") e a repeticao poe exatamente esse giro (o 1o chamado do tique leva
    // tudo, os outros do mesmo tique ficam com zero). Gravacoes sem linhas "A": como antes.
    void RoteiroGiroFinal(uint8_t* base, uint32_t in)
    {
        std::lock_guard lock(g_recMutex);
        if (Repetindo())
        {
            if (g_repTotal.empty()) return;
            const int64_t t = TiqueRoteiro();
            float x = 0, y = 0;
            if (t != g_repUltimoTotal)
            {
                g_repUltimoTotal = t;
                if (auto it = g_repTotal.find(t); it != g_repTotal.end()) { x = it->second.first; y = it->second.second; }
            }
            W32(in + 188, FloatBits(x));
            W32(in + 192, FloatBits(y));
            return;
        }
        if (g_recState != RecState::Recording) return;
        const uint32_t ux = R32(in + 188), uy = R32(in + 192);
        if ((ux & 0x7FFFFFFF) || (uy & 0x7FFFFFFF)) Escrever("A %lld %08X %08X\n", (long long)g_tique, ux, uy);
    }

    // posicao do ouvinte (camera) a cada 30 tiques: gravada, e na repeticao comparada com a gravada
    void RoteiroPosicao()
    {
#ifndef PREDATOR_EDITOR   // o editor tem a lista de ouvintes em outro endereco
        std::lock_guard lock(g_recMutex);
        if (!Repetindo() && g_recState != RecState::Recording) return;
        static int64_t ultimo = -1;
        int64_t t = Repetindo() ? TiqueRoteiro() : g_tique;
        if (t % 30 || t == ultimo) return;
        ultimo = t;
        uint32_t ouv = R32(0x828D9C68);
        if (!ouv) return;
        float x = BitsFloat(R32(ouv)), y = BitsFloat(R32(ouv + 4)), z = BitsFloat(R32(ouv + 8));
        Escrever("P %lld %.3f %.3f %.3f\n", (long long)g_tique, x, y, z);
        if (!Repetindo()) return;
        auto it = g_repPos.find(t);
        if (it == g_repPos.end()) return;
        double d = std::sqrt(std::pow(x - it->second[0], 2) + std::pow(y - it->second[1], 2) + std::pow(z - it->second[2], 2));
        if (d > g_maiorDif) { g_maiorDif = d; g_maiorDifTique = t; }
        Log("[roteiro] tique %lld: camera em %.2f %.2f %.2f, gravada %.2f %.2f %.2f (diferenca %.3f m)", (long long)t, x, y, z,
            it->second[0], it->second[1], it->second[2], d);
#endif
    }

    std::string RoteiroStatus()
    {
        std::lock_guard lock(g_recMutex);
        char b[96];
        if (Repetindo()) { snprintf(b, sizeof(b), "  |  REPETINDO roteiro: tique %lld de %lld", (long long)TiqueRoteiro(), (long long)g_repFim); return b; }
        if (g_rep) return "  |  roteiro repetido ate o fim";
        switch (g_recState)
        {
        case RecState::Recording: return "  |  GRAVANDO roteiro (F4 termina)";
        case RecState::Done: return "  |  roteiro gravado - pode fechar o jogo";
        default: return "";
        }
    }
}

// main.cpp, antes da thread do jogo: abre a gravacao (secao deste programa) ou carrega a repeticao (acerta a hora)
void RoteiroIniciar()
{
    if (const char* path = getenv("FCRECOMP_REPRODUZIR")) CarregarRoteiro(path);
    if (const char* path = getenv("FCRECOMP_GRAVAR"))
    {
        const int etapa = Etapa();
        g_recFile = fopen(path, etapa ? "a" : "w");
        if (!g_recFile) { Log("[roteiro] nao abriu %s", path); return; }
        g_recState = RecState::Recording;
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::string nome = std::filesystem::path(exe).filename().string();
        if (!etapa) Escrever("ROTEIRO 2\n");
        Escrever("ETAPA %d %s %llu\n", etapa, nome.c_str(), (unsigned long long)HoraDoJogo());
        Log("[roteiro] gravando em %s desde a abertura (etapa %d; F4 termina)", path, etapa);
        if (!etapa) Beeps(1);
    }
}

// tique atual do roteiro (na repeticao, o da gravacao; -1 sem roteiro): para registros de depuracao
// predator_bridge.cpp (mftb): relogio que o jogo le; na repeticao de um roteiro anda a duracao gravada de cada tique
uint64_t RoteiroTimebase(uint64_t real)
{
    real = TscSemPausa(real);
    if (!g_relLigado.load(std::memory_order_acquire)) return real;
    return RelogioVirtual(g_relEstados[g_relIdx.load(std::memory_order_acquire) & 63], real);
}

// xenos_draw.cpp: para (true) ou solta (false) o relogio do jogo
void RelogioPausar(bool parar)
{
    if (parar)
    {
        if (!g_pausaCongelado.load()) g_pausaCongelado.store(std::max<uint64_t>(1, __rdtsc() - g_pausaDesvio.load()), std::memory_order_release);
        return;
    }
    uint64_t c = g_pausaCongelado.load();
    if (!c) return;
    g_pausaDesvio.store(__rdtsc() - c, std::memory_order_release);
    g_pausaCongelado.store(0, std::memory_order_release);
}

int64_t RoteiroTiqueAtual()
{
    std::lock_guard lock(g_recMutex);
    if (Repetindo()) return TiqueRoteiro();
    return g_recState == RecState::Recording ? g_tique : -1;
}

// gravacao ou repeticao em andamento? (o jogador 2 fica desligado; content.cpp tambem pergunta)
bool RoteiroAtivo()
{
    std::lock_guard lock(g_recMutex);
    return g_recState == RecState::Recording || Repetindo();
}

// gpu.cpp, no VdSwap (thread do jogo): conta as trocas de tela do relogio do roteiro.
// Depuracao: FCRECOMP_LEITURASLOG=1 anota quantas leituras do controle 1 e giros da mira o jogo faz entre trocas de
// tela, em trechos ("trocas A-B: N leituras")
// ------------------------------------------------------------ comandos ao vivo (depuracao com o jogo aberto)
// Para investigar sem fechar e abrir o jogo a cada teste (ex.: repetir_roteiro.ps1 -Segundos 0 deixa o jogo parado no
// fim do roteiro): o jogo rele build_game\\ao_vivo.txt quando o arquivo muda (olha a cada ~0,5 s). Linhas CHAVE=valor:
//   PULARPS=HASH,HASH   nao desenha esses pixel shaders (como FCRECOMP_SKIPPS); vazio = desenha tudo
//   PULARVS=HASH,HASH   nao desenha esses vertex shaders
//   FOTO=arquivo.bmp    salva a proxima imagem (caminho relativo a build_game ou completo)
//   CAPTURA=1           captura completa do proximo quadro (como o F9: build_game\diag\quadro_N)
// FOTO e CAPTURA valem uma vez a cada vez que o arquivo e salvo. Cada leitura aparece no registro como "[aovivo]".
namespace
{
    std::mutex g_aoVivoMutex;
    std::string g_aoVivoPs, g_aoVivoVs;
    std::atomic<bool> g_aoVivoPula{ false };
    std::unordered_map<std::string, std::string> g_aoVivoOutros;   // outras chaves (testes, veja AoVivoLigado)
    std::atomic<bool> g_aoVivoTemOutros{ false };

    // hora em que o jogo abriu (inicializada com o programa, antes de qualquer arquivo aparecer)
    const FILETIME g_aoVivoInicio = [] { FILETIME t; GetSystemTimeAsFileTime(&t); return t; }();

    void AoVivoLer(uint64_t troca)
    {
        static uint64_t ultimaOlhada = 0;
        static FILETIME ultimaMudanca{};
        if (troca - ultimaOlhada < 15) return;
        ultimaOlhada = troca;
        WIN32_FILE_ATTRIBUTE_DATA at;
        if (!GetFileAttributesExA("ao_vivo.txt", GetFileExInfoStandard, &at)) return;
        // so vale arquivo escrito com o jogo aberto: um ao_vivo.txt esquecido de outra sessao (ex.: SEMCORTE=1)
        // desligava correcoes sem ninguem perceber
        if (CompareFileTime(&at.ftLastWriteTime, &g_aoVivoInicio) < 0) return;
        if (CompareFileTime(&at.ftLastWriteTime, &ultimaMudanca) == 0) return;
        ultimaMudanca = at.ftLastWriteTime;
        std::ifstream f("ao_vivo.txt");
        std::string linha, ps, vs;
        std::unordered_map<std::string, std::string> outros;
        while (std::getline(f, linha))
        {
            while (!linha.empty() && (linha.back() == '\r' || linha.back() == ' ')) linha.pop_back();
            size_t eq = linha.find('=');
            if (linha.empty() || linha[0] == '#' || eq == std::string::npos) continue;
            std::string k = linha.substr(0, eq), v = linha.substr(eq + 1);
            for (auto& c : k) c = (char)toupper((unsigned char)c);
            for (auto& c : v) if (k != "FOTO") c = (char)toupper((unsigned char)c);
            if (k == "PULARPS") ps = v;
            else if (k == "PULARVS") vs = v;
            else if (k == "FOTO" && !v.empty()) { GpuPedirFoto(troca + 1, v); Log("[aovivo] foto pedida: %s", v.c_str()); }
            else if (k == "CAPTURA" && v != "0") { Log("[aovivo] captura pedida"); diag::RequestCapture(); }
            else { outros[k] = v; Log("[aovivo] %s=%s", k.c_str(), v.c_str()); }
        }
        {
            std::lock_guard lock(g_aoVivoMutex);
            g_aoVivoPs = ps; g_aoVivoVs = vs;
            g_aoVivoPula = !ps.empty() || !vs.empty();
            g_aoVivoOutros = outros;
            g_aoVivoTemOutros = !outros.empty();
        }
        Log("[aovivo] ao_vivo.txt lido: pular ps [%s] vs [%s]", ps.c_str(), vs.c_str());
    }
}

// valor de uma chave do ao_vivo.txt (vazio se nao houver)
std::string AoVivoValor(const char* chave)
{
    if (!g_aoVivoTemOutros.load(std::memory_order_relaxed)) return {};
    std::lock_guard lock(g_aoVivoMutex);
    auto it = g_aoVivoOutros.find(chave);
    return it != g_aoVivoOutros.end() ? it->second : std::string();
}

// teste ligado no ao_vivo.txt (CHAVE=1)
bool AoVivoLigado(const char* chave)
{
    if (!g_aoVivoTemOutros.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(g_aoVivoMutex);
    auto it = g_aoVivoOutros.find(chave);
    return it != g_aoVivoOutros.end() && !it->second.empty() && it->second != "0";
}

// xenos_draw.cpp: este desenho foi mandado pular pelo ao_vivo.txt?
bool AoVivoPular(uint64_t vsHash, uint64_t psHash)
{
    if (!g_aoVivoPula.load(std::memory_order_relaxed)) return false;
    char h[2][20];
    snprintf(h[0], sizeof(h[0]), "%016llX", (unsigned long long)vsHash);
    snprintf(h[1], sizeof(h[1]), "%016llX", (unsigned long long)psHash);
    std::lock_guard lock(g_aoVivoMutex);
    return (!g_aoVivoVs.empty() && g_aoVivoVs.find(h[0]) != std::string::npos) ||
           (!g_aoVivoPs.empty() && g_aoVivoPs.find(h[1]) != std::string::npos);
}

std::atomic<uint32_t> g_dbgLeituras{ 0 }, g_dbgMiras{ 0 };
void InputOnSwap()
{
    uint64_t troca = ++g_trocas;
    AoVivoLer(troca);
    if (g_fotoArmada.exchange(false))
    {
        std::lock_guard lock(g_recMutex);
        GpuPedirFoto(troca, g_fotoNome);
    }
    static const bool on = getenv("FCRECOMP_LEITURASLOG") != nullptr;
    if (!on) return;
    static uint64_t runStart = 0;
    static uint32_t runR = ~0u, runM = ~0u;
    static DWORD runTick = 0;
    uint32_t r = g_dbgLeituras.exchange(0), m = g_dbgMiras.exchange(0);
    if (r != runR || m != runM)
    {
        if (runR != ~0u)
            Log("[leituras] trocas %llu-%llu (%.2f s): %u leituras, %u miras por troca", (unsigned long long)runStart,
                (unsigned long long)(troca - 1), (GetTickCount() - runTick) / 1000.0, runR, runM);
        runR = r; runM = m; runStart = troca; runTick = GetTickCount();
    }
}

// F4 (diag.cpp): termina a gravacao do roteiro sem fechar o jogo (os programas abertos depois nao gravam)
bool InputStopRecording()
{
    std::lock_guard lock(g_recMutex);
    if (g_recState != RecState::Recording) return false;
    Escrever("FIM %lld\n", (long long)g_tique);
    fclose(g_recFile);
    g_recFile = nullptr;
    g_recState = RecState::Done;
    SetEnvironmentVariableW(L"FCRECOMP_GRAVAR", nullptr);
    Log("[roteiro] gravacao terminada (F4) no tique %lld da etapa %d", (long long)g_tique, Etapa());
    Beeps(2);
    return true;
}

// XamInputGetState(usuario, flags, XINPUT_STATE*): { u32 pacote; u16 botoes; u8 LT; u8 RT; s16 LX, LY, RX, RY }
KFUNC(XamInputGetState)
{
    EnsureConfig();
    uint32_t user = Gpr32(ctx, 3), out = Gpr32(ctx, 5);
    if (user != 0 && RoteiroAtivo()) { Ret(ctx, 0x48F); return; }   // roteiro gravado: so o jogador 1
    static std::atomic<int> calls{ 0 };
    int n = calls++;
    if (n == 0)
        Log("[input] primeira leitura do controle aos %.1f s", (GetTickCount() - g_startTick) / 1000.0);
    // dispositivos deste usuario: jogador 1 = o seu (+ o do jogador 2 se "juntar"); jogador 2 = o seu (se nao juntar);
    // jogadores 3 e 4 = o seu
    std::string devs[2]; int nDevs = 0;
    {
        std::lock_guard lock(g_cfgMutex);
        if (user == 0) { devs[nDevs++] = g_jogador[0]; if (g_juntar) devs[nDevs++] = g_jogador[1]; }
        else if (user == 1 && !g_juntar) devs[nDevs++] = g_jogador[1];
        else if (user == 2 || user == 3) devs[nDevs++] = g_jogador[user];
    }
    bool connected = false, keyboard = false;
    XINPUT_GAMEPAD g{};
    for (int i = 0; i < nDevs; i++)
    {
        if (devs[i] == "teclado") { keyboard = connected = true; continue; }
        XINPUT_GAMEPAD p{};
        if (devs[i] == "nenhum" || !PadLer(devs[i], p)) continue;
        connected = true;
        // junta com o que ja tem: botoes somados, o maior gatilho/analogico vence
        g.wButtons |= p.wButtons;
        g.bLeftTrigger = std::max(g.bLeftTrigger, p.bLeftTrigger);
        g.bRightTrigger = std::max(g.bRightTrigger, p.bRightTrigger);
        auto maior = [](SHORT& a, SHORT b) { if (std::abs((int)b) > std::abs((int)a)) a = b; };
        maior(g.sThumbLX, p.sThumbLX); maior(g.sThumbLY, p.sThumbLY); maior(g.sThumbRX, p.sThumbRX); maior(g.sThumbRY, p.sThumbRY);
    }
    // o jogador 1 sempre aparece ligado (o jogo pede o controle 1 na tela de inicio)
    if (!connected && user != 0) { Ret(ctx, 0x48F); return; }
    if (keyboard) ReadKeyboard(g);
    // giro maximo da camera aumentado no lua.dat (fs.cpp): o analogico direito do controle/teclado e reduzido na mesma
    // proporcao depois da zona morta, para continuar com a mesma velocidade
    if (g_cameraSpeedBoost != 1.0f)
    {
        float dz = g_mouse.deadZone;
        auto slow = [&](SHORT& a) {
            float v = a / 32767.0f, m = std::fabs(v);
            if (m > dz) a = (SHORT)std::lround(std::copysign(dz + (m - dz) / g_cameraSpeedBoost, v) * 32767.0f);
        };
        slow(g.sThumbRX); slow(g.sThumbRY);
    }
    if (keyboard) ReadMouse(g, (uint32_t)Lr(ctx));
    // com a caixa de digitacao aberta o jogo nao recebe o controle (como o teclado virtual do Xbox)
    if (TecladoAberto()) g = XINPUT_GAMEPAD{};
    if (user == 0)
    {
        g_dbgLeituras++;
        AutoInput(g);
        RoteiroLeitura(g);   // roteiro gravado: relogio; na repeticao substitui o controle real; na gravacao anota
    }
    if (user < 4) g_rStick[user] = (uint32_t)(uint16_t)g.sThumbRX << 16 | (uint16_t)g.sThumbRY;   // camera direta
    if (user < 4) g_lTrig[user] = g.bLeftTrigger;
    static WORD lastButtonsUser[4] = {};
    WORD& lastButtons = lastButtonsUser[user & 3];
    if (g.wButtons != lastButtons && user == 0)
    {
        Log("[input] botoes = %04X", g.wButtons);
        lastButtons = g.wButtons;
    }
    // analogico esquerdo: registra quando muda de direcao (confere roteiros como "F4-20:LUP")
    {
        static int lastDir = 0;
        int dir = (g.sThumbLY > 16000 ? 1 : g.sThumbLY < -16000 ? 2 : 0) | (g.sThumbLX > 16000 ? 4 : g.sThumbLX < -16000 ? 8 : 0);
        if (dir != lastDir && user == 0) { Log("[input] analogico esquerdo = %d,%d", g.sThumbLX, g.sThumbLY); lastDir = dir; }
    }

    if (out)
    {
        W32(out, ++g_packet);
        W16(out + 4, g.wButtons);
        W8(out + 6, g.bLeftTrigger);
        W8(out + 7, g.bRightTrigger);
        W16(out + 8, (uint16_t)g.sThumbLX);
        W16(out + 10, (uint16_t)g.sThumbLY);
        W16(out + 12, (uint16_t)g.sThumbRX);
        W16(out + 14, (uint16_t)g.sThumbRY);
    }
    Ret(ctx, 0);
}

// XamInputSetState(usuario, flags, XINPUT_VIBRATION*): vibracao { u16 esquerda; u16 direita }
KFUNC(XamInputSetState)
{
    uint32_t user = Gpr32(ctx, 3);
    std::string devs[2]; int nDevs = 0;
    {
        std::lock_guard lock(g_cfgMutex);
        if (user == 0) { devs[nDevs++] = g_jogador[0]; if (g_juntar) devs[nDevs++] = g_jogador[1]; }
        else if (user == 1 && !g_juntar) devs[nDevs++] = g_jogador[1];
        else if (user == 2 || user == 3) devs[nDevs++] = g_jogador[user];
    }
    if (!nDevs) { Ret(ctx, 0x48F); return; }
    if (Gpr32(ctx, 5))
        for (int i = 0; i < nDevs; i++)
            if (devs[i] != "teclado" && devs[i] != "nenhum")
                PadVibrar(devs[i], R16(Gpr32(ctx, 5)), R16(Gpr32(ctx, 5) + 2));
    Ret(ctx, 0);
}

float g_cameraSpeedBoost = 1.0f;   // fs.cpp: giro maximo da camera aumentado no lua.dat

// fs.cpp: o mouse controla a camera? (liga o ajuste que desliga a aceleracao do analogico)
bool InputMouseCamera() { EnsureConfig(); return g_mouse.camera && !g_directLook; }   // so no modo antigo (analogico)

// O teclado e mouse estao neste usuario do Xbox (0 = jogador 1 ... 3 = jogador 4)?
static bool TecladoNoUsuario(uint32_t u)
{
    std::lock_guard lock(g_cfgMutex);
    if (u == 0) return g_jogador[0] == "teclado" || (g_juntar && g_jogador[1] == "teclado");
    return ((u == 1 && !g_juntar) || u == 2 || u == 3) && g_jogador[u] == "teclado";
}

// content.cpp: o usuario aparece conectado (perfil "Jogador"/"Jogador 2/3/4")? Jogador 2 so quando separado do 1;
// jogadores 3 e 4 quando tem dispositivo
bool InputUsuarioLigado(uint32_t u)
{
    EnsureConfig();
    if (u != 0 && RoteiroAtivo()) return false;   // roteiro gravado: so o jogador 1
    std::lock_guard lock(g_cfgMutex);
    return u == 0 || (u == 1 && !g_juntar && g_jogador[1] != "nenhum") || ((u == 2 || u == 3) && g_jogador[u] != "nenhum");
}

// ---------------------------------------------------------------- mira direta com o mouse
// sub_8228BC78(controlador de mira, entrada do jogador, f1/f2 = analogico direito processado): calcula quanto a camera
// gira neste quadro (analogico x ControllerSpeeds x tempo do quadro, mais a assistencia de mira) e grava em
// entrada+188 (horizontal) / +192 (vertical); o jogador soma isso ao angulo. Chamado por sub_82146038.
// No editor de mapas (ige.xex) a mesma funcao e sub_82257300 (GUEST_SUB_MIRA); o jogador, a entrada (+908), o
// gerente de controles (+1232, +204, +48*idx+28) e as opcoes do perfil (+1296, byte +184) estao nos mesmos lugares.
GUEST_DECLARE_ORIGINAL(GUEST_SUB_MIRA);
#ifndef PREDATOR_EDITOR
GUEST_DECLARE_ORIGINAL(sub_821D3B78);
#endif
// declaradas aqui (e nao dentro do gancho) para manter a ligacao C++ mesmo quando GUEST_FUNC e extern "C"
void WatchAddWriteRange(uint32_t addr, uint32_t size);
void WatchAddReadRange(uint32_t addr, uint32_t size);
extern std::atomic<uint64_t> g_missionStartFrame;
// Opcao "INVERT STICK" do menu do jogo (CONTROLLER OPTIONS): sub_821D3B78 devolve o objeto de opcoes do perfil em uso
// e o byte +184 e a opcao (a tela do menu le dali). Chamada com uma copia do contexto, abaixo da pilha atual.
// No editor: as opcoes do perfil que o proprio jogador guarda (jogador+1296; o editor le o +184 dali).
static bool GameInvertLook(GuestCpu& ctx, uint8_t* base, uint32_t in)
{
#ifdef PREDATOR_EDITOR
    (void)ctx; (void)base;
    uint32_t opts = R32(in - 908 + 1296);
    return opts && R8(opts + 184) != 0;
#else
    (void)in;
    GuestCpu tmp = ctx;
    SetGpr(tmp, 1, Gpr32(ctx, 1) - 0x400);
    GUEST_ORIGINAL(sub_821D3B78)(tmp, base);
    uint32_t opts = Gpr32(tmp, 3);
    return opts && R8(opts + 184) != 0;
#endif
}
GUEST_FUNC(GUEST_SUB_MIRA)
{
    uint32_t in = Gpr32(ctx, 4);
    float sx = (float)Fpr(ctx, 1), sy = (float)Fpr(ctx, 2);
    g_dbgMiras++;
    // voltando de um trecho sem giro do jogador (editor de mapas): o mouse acumulado nesse meio tempo ja foi usado
    // como analogico, nao vira um giro de uma vez so
    if (GetTickCount64() - g_miraTick.exchange(GetTickCount64()) >= 250) { g_lookDX = 0; g_lookDY = 0; }
    // depuracao: FCRECOMP_WATCHOLHAR=1 vigia quem grava o analogico direito no jogador (jogador+108/+112)
    if (static bool once = false; !once && getenv("FCRECOMP_WATCHOLHAR") && diag::CurrentFrame() > 1200)
    {
        once = true;
        WatchAddWriteRange(in - 908 + 108, 8);
    }
    // tela dividida: o mouse so gira a camera do jogador que usa o teclado. O jogador (entrada - 908) guarda o numero
    // do seu controle em gerente+idx*48+28 (gerente = jogador+1232, idx = gerente+204; sub_82146038 le assim)
    bool doTeclado = true;
    uint32_t pad = 0xFFFFFFFFu;
    {
        uint32_t jogador = in - 908, gerente = R32(jogador + 1232);
        pad = gerente ? R32(gerente + R32(gerente + 204) * 48 + 28) : 0xFFFFFFFFu;
        if (pad < 4) doTeclado = TecladoNoUsuario(pad);
        static uint32_t ultimoPad = 0xFFFFFFFEu;
        if (pad != ultimoPad) { Log("[input] jogador %08X usa o controle %d (mouse %s)", jogador, (int)pad, doTeclado ? "sim" : "nao"); ultimoPad = pad; }
    }
    // roteiro gravado (so o jogador 1): o giro somado aqui e anotado; na repeticao entra o giro gravado no lugar
    const bool jogador1 = pad == 0 || pad >= 4;
    const bool repetindo = jogador1 && RoteiroGiroRepetindo();
    float somaX = 0, somaY = 0;
    auto add = [&](uint32_t addr, float v) {
        uint32_t u = R32(addr); float fv; memcpy(&fv, &u, 4); fv += v; memcpy(&u, &fv, 4); W32(addr, u);
        (addr == in + 188 ? somaX : somaY) += v;
    };
    // Camera direta no analogico direito: o jogo recebe o analogico zerado (fica so a assistencia de mira dele) e o giro
    // vem do analogico cru: zona morta redonda, curva de precisao (devagar perto do centro), um impulso que cresce em
    // ~0,3 s com o analogico no fim (virar rapido) e suavizacao opcional (padrao: resposta imediata, sem a
    // aceleracao/suavizacao do jogo). Zona morta, impulso e suavizacao sao ajustaveis na janela (F1).
    // Unidade: a mesma da mira do mouse (graus por quadro; o analogico do jogo no maximo da ~4,8).
    bool padDireto = g_padDireto && pad < 4;
    if (jogador1) padDireto = RoteiroCameraDireta(padDireto);   // repeticao: a da gravacao
    if (padDireto) { SetFpr(ctx, 1, 0.0); SetFpr(ctx, 2, 0.0); }
    GUEST_ORIGINAL(GUEST_SUB_MIRA)(ctx, base);
    if (repetindo)
    {
        float gx, gy;
        if (RoteiroGiro(gx, gy) && (gx != 0 || gy != 0)) { add(in + 188, gx); add(in + 192, gy); }
        g_lookDX = 0; g_lookDY = 0;   // o mouse real nao conta
    }
    else if (padDireto)
    {
        uint32_t st = g_rStick[pad].load();
        float x = (int16_t)(st >> 16) / 32767.0f, y = (int16_t)(st & 0xFFFF) / 32767.0f;
        float mag = std::min(std::sqrt(x * x + y * y), 1.0f);
        constexpr float CURVA = 1.7f, MAX_H = 5.0f, MAX_V = 3.6f;
        const float ZONA = g_padZona.load(), IMPULSO = g_padAcel.load(), SUAVE = g_padSuave.load();
        struct Estado { int64_t ultimo = 0; float impulso = 0, gx = 0, gy = 0; };
        static Estado est[4];
        Estado& e = est[pad];
        int64_t agora = Qpc();
        float dt = e.ultimo ? (float)((agora - e.ultimo) / QpcFreq()) : 1.0f / 30;
        e.ultimo = agora;
        float quadros = std::clamp(dt * 30.0f, 0.25f, 2.0f);   // o giro e por quadro de 30 qps
        // giro alvo deste quadro (graus por quadro de 30 qps)
        float alvoX = 0, alvoY = 0;
        if (mag > ZONA)
        {
            float m = std::pow((mag - ZONA) / std::max(1.0f - ZONA, 0.05f), CURVA);
            // mirando (gatilho esquerdo mais que a metade): a sensibilidade de mira multiplica as duas
            const bool mirando = g_lTrig[pad].load() > 128;
            float mira = mirando ? g_padMira.load() : 1.0f;
            // impulso (item 32): so com o analogico no fim do curso e mais para o lado (virar rapido), depois de 0,12 s
            // (um toque rapido para corrigir a mira nao dispara), so na horizontal e nunca mirando. Antes valia tambem
            // para cima/baixo e na hora: a camera passava do ponto ao olhar para cima e ao mirar.
            bool lado = std::fabs(x) > 0.7f * mag;
            if (mag > 0.95f && lado && !mirando) e.impulso = std::min(1.0f, e.impulso + dt / 0.45f);
            else e.impulso = 0.0f;
            float impulso = std::clamp((e.impulso - 0.27f) / 0.73f, 0.0f, 1.0f);   // 0 nos primeiros 0,12 s, cheio aos 0,45 s
            float k = m * mira / mag;
            alvoX = x * k * (1.0f + IMPULSO * impulso) * g_padVel.load() * MAX_H;
            alvoY = -y * k * g_padVelV.load() * MAX_V;   // positivo = olhar para baixo: analogico para cima olha para cima
        }
        else e.impulso = 0;
        // suavizacao: o giro chega ao alvo aos poucos (SUAVE = quanto do giro anterior fica a cada quadro)
        float f = SUAVE > 0 ? std::pow(SUAVE, quadros) : 0.0f;
        e.gx = e.gx * f + alvoX * (1.0f - f);
        e.gy = e.gy * f + alvoY * (1.0f - f);
        if (std::fabs(e.gx) < 1e-4f && alvoX == 0) e.gx = 0;
        if (std::fabs(e.gy) < 1e-4f && alvoY == 0) e.gy = 0;
        if (e.gx != 0 || e.gy != 0)
        {
            add(in + 188, e.gx * quadros);
            add(in + 192, e.gy * quadros * (GameInvertLook(ctx, base, in) ? -1.0f : 1.0f));   // INVERT LOOK do jogo
        }
    }
    if (g_directLook && doTeclado && !repetindo)
    {
        long dx = g_lookDX.exchange(0), dy = g_lookDY.exchange(0);
        // teste sem mouse: FCRECOMP_MOUSETESTE=inicio-fim:dx,dy (segundos desde o inicio da fase; contagens por quadro)
        if (static const char* mt = getenv("FCRECOMP_MOUSETESTE"); mt)
        {
            uint64_t st = g_missionStartFrame.load(), fr = diag::CurrentFrame();
            // varios trechos separados por "|": "3-3.15:20,0|4.5-4.65:-20,0"
            for (const char* p = mt; p && *p; p = strchr(p, '|') ? strchr(p, '|') + 1 : nullptr)
            {
                float t0 = (float)atof(p), t1 = strchr(p, '-') ? (float)atof(strchr(p, '-') + 1) : t0;
                const char* c = strchr(p, ':');
                if (st && c && fr >= st + (uint64_t)(t0 * 30) && fr < st + (uint64_t)(t1 * 30))
                { dx += atol(c + 1); if (const char* k = strchr(c, ',')) dy += atol(k + 1); }
            }
        }
        static const bool mouseTest = getenv("FCRECOMP_MOUSETESTE") != nullptr;
        bool active = g_mouse.camera && ((g_captured && Focused()) || mouseTest);
        if (active && (dx || dy))
        {
            // unidade do jogo: graus por quadro antes da sensibilidade do menu (analogico no maximo ~4.8 na horizontal)
            float k = g_mouse.lookScale * g_sens.load();
            add(in + 188, dx * k);
            // positivo = olhar para baixo; inverte se o menu do jogo (INVERT STICK) ou o controles.ini (inverter_y) pedir
            bool inv = GameInvertLook(ctx, base, in) != g_mouse.invertY;
            static int lastInv = -1;
            if (lastInv != (int)inv) { Log("[input] mouse vertical %s (menu do jogo: INVERT STICK %s)", inv ? "invertido" : "normal", GameInvertLook(ctx, base, in) ? "YES" : "NO"); lastInv = inv; }
            add(in + 192, dy * k * (inv ? -1.0f : 1.0f));
        }
    }
#ifndef PREDATOR_EDITOR
    // depuracao: FCRECOMP_OLHARLOG=1 mostra, a cada quadro, o giro pedido (entrada+188/+192) e a posicao/direcao do
    // ouvinte (camera) para ver se a camera segue o mouse no mesmo quadro ou "flutua" depois
    if (static const bool olharLog = getenv("FCRECOMP_OLHARLOG") != nullptr; olharLog && jogador1)
    {
        uint32_t ouv = R32(0x828D9C68);
        // FCRECOMP_OLHARVIGIA=<quadro>: quem grava a direcao do ouvinte (ouvinte+16) e em que ordem com o giro deste quadro
        if (static bool vig = false; !vig && ouv && getenv("FCRECOMP_OLHARVIGIA") && diag::CurrentFrame() >= (uint64_t)atoi(getenv("FCRECOMP_OLHARVIGIA")))
        {
            vig = true;
            WatchAddWriteRange(ouv + 16, 4);
        }
        float v[16] = {};
        if (ouv) for (int i = 0; i < 16; i++) v[i] = BitsFloat(R32(ouv + i * 4));
        // FCRECOMP_OLHARLEITURA=<quadro>: quem le o giro do quadro (entrada+188) depois de calculado
        if (static bool lei = false; !lei && getenv("FCRECOMP_OLHARLEITURA") && diag::CurrentFrame() >= (uint64_t)atoi(getenv("FCRECOMP_OLHARLEITURA")))
        {
            lei = true;
            ::WatchAddReadRange(in + 188, 8);
        }
        // FCRECOMP_OLHARANGULO=<quadro>: quem le/grava o angulo do jogador (jogador+932 vertical, +940 horizontal)
        if (static bool ang = false; !ang && getenv("FCRECOMP_OLHARANGULO") && diag::CurrentFrame() >= (uint64_t)atoi(getenv("FCRECOMP_OLHARANGULO")))
        {
            ang = true;
            ::WatchAddReadRange(in - 908 + 932, 12);
        }
        // FCRECOMP_OLHARDIF=1: floats do jogador (jogador-0x400 .. +0x1000) que mudaram desde a leitura anterior
        // (para achar o angulo da camera: o que muda junto com o giro)
        if (static const bool dif = getenv("FCRECOMP_OLHARDIF") != nullptr; dif)
        {
            static uint32_t antes[0x500];
            static uint32_t dono = 0;
            const uint32_t ini = in - 908 - 0x400;
            std::string s;
            for (uint32_t i = 0; i < 0x500; i++)
            {
                uint32_t w = R32(ini + i * 4);
                if (dono == ini && w != antes[i])
                {
                    float a0 = BitsFloat(antes[i]), a1 = BitsFloat(w);
                    if (std::isfinite(a0) && std::isfinite(a1) && std::fabs(a1) < 1e6f && std::fabs(a0) < 1e6f)
                    { char b[48]; snprintf(b, sizeof(b), " %d:%.4f", (int)(i * 4) - 0x400, a1 - a0); s += b; }
                }
                antes[i] = w;
            }
            dono = ini;
            if (!s.empty() && s.size() < 6000) Log("[olhardif] q=%llu%s", (unsigned long long)diag::CurrentFrame(), s.c_str());
        }
        Log("[olhar] t=%.1f q=%llu giro %.4f %.4f | ouv %.3f %.3f %.3f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f %.4f",
            perf::Now(), (unsigned long long)diag::CurrentFrame(), BitsFloat(R32(in + 188)), BitsFloat(R32(in + 192)),
            v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
    }
#endif
    if (jogador1)
    {
        if (!repetindo) RoteiroAnotarGiro(somaX, somaY);
        RoteiroGiroFinal(base, in);   // giro final do quadro: gravado / repetido exatamente
        RoteiroPosicao();
    }
    static const bool lookLog = getenv("FCRECOMP_LOOKLOG") != nullptr;   // depuracao: valores do giro
    if (lookLog && (sx != 0 || sy != 0))
    {
        uint32_t a = R32(in + 188), b = R32(in + 192);
        float fa, fb; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
        Log("[look] analogico %.3f,%.3f -> giro do quadro %.5f,%.5f", sx, sy, fa, fb);
    }
}

// ---------------------------------------------------------------- configuracao para a janela (controles.h)
ControlesCfg InputConfigAtual()
{
    EnsureConfig();
    std::lock_guard lock(g_cfgMutex);
    ControlesCfg c;
    for (int i = 0; i < 4; i++) c.jogador[i] = g_jogador[i];
    c.juntar = g_juntar;
    c.mouseCamera = g_mouse.camera; c.inverterY = g_mouse.invertY; c.sensibilidade = g_sens.load();
    c.cameraDireta = g_padDireto; c.velocidadeCamera = g_padVel.load();
    c.velocidadeVertical = g_padVelV.load(); c.sensMirando = g_padMira.load();
    c.zonaMorta = g_padZona.load(); c.aceleracao = g_padAcel.load(); c.suavizacao = g_padSuave.load();
    for (auto& a : g_actions) c.acoes.push_back({ a.name, a.help, a.keys });
    return c;
}

ControlesCfg InputConfigPadrao()
{
    ControlesCfg c;
    c.jogador[0] = "teclado"; c.jogador[1] = "auto"; c.jogador[2] = c.jogador[3] = "nenhum"; c.juntar = false;
    MouseCfg m;
    c.mouseCamera = m.camera; c.inverterY = m.invertY; c.sensibilidade = m.sens;
    for (auto& a : g_actions) c.acoes.push_back({ a.name, a.help, ParseKeyList(a.defaults, a.name) });
    return c;
}

void InputAplicarConfig(const ControlesCfg& c)
{
    EnsureConfig();
    {
        std::lock_guard lock(g_cfgMutex);
        for (int i = 0; i < 4; i++) g_jogador[i] = c.jogador[i];
        g_juntar = c.juntar;
        g_mouse.camera = c.mouseCamera; g_mouse.invertY = c.inverterY;
        g_mouse.sens = std::clamp(c.sensibilidade, 0.1f, 10.0f); g_sens = g_mouse.sens;
        g_padDireto = c.cameraDireta; g_padVel = std::clamp(c.velocidadeCamera, 0.2f, 5.0f);
        g_padVelV = std::clamp(c.velocidadeVertical, 0.2f, 5.0f); g_padMira = std::clamp(c.sensMirando, 0.1f, 2.0f);
        g_padZona = std::clamp(c.zonaMorta, 0.0f, 0.5f); g_padAcel = std::clamp(c.aceleracao, 0.0f, 2.0f);
        g_padSuave = std::clamp(c.suavizacao, 0.0f, 0.9f);
        for (auto& a : g_actions)
            for (auto& n : c.acoes) if (n.nome == a.name) a.keys = n.teclas;
        WriteIni();
    }
    Log("[input] configuracao salva pela janela: jogador 1 %s, jogador 2 %s, jogador 3 %s, jogador 4 %s, juntar %s, sensibilidade %.2f",
        c.jogador[0].c_str(), c.jogador[1].c_str(), c.jogador[2].c_str(), c.jogador[3].c_str(), c.juntar ? "sim" : "nao", c.sensibilidade);
}

std::string InputNomeTecla(int vk)
{
    if (IDIOMA_INGLES)   // nome em ingles so para mostrar (o controles.ini guarda o nome de sempre)
        for (auto& k : KEY_NAMES) if (k.vk == vk && k.en) return k.en;
    return KeyNameOf(vk);
}

std::vector<int> InputTodasTeclas()
{
    std::vector<int> v;
    for (int c = 'A'; c <= 'Z'; c++) v.push_back(c);
    for (int c = '0'; c <= '9'; c++) v.push_back(c);
    for (auto& k : KEY_NAMES) v.push_back(k.vk);
    for (int k = 0; k < 10; k++) v.push_back(VK_NUMPAD0 + k);
    for (int k = 0; k < 12; k++) if (VK_F1 + k != VK_F1 && VK_F1 + k != VK_F10 && VK_F1 + k != VK_F12) v.push_back(VK_F1 + k);   // F1 = esta janela, F10 = tecla de sistema do Windows, F12 = bug
    return v;
}
