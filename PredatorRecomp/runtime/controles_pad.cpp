// Controles do PC: DualSense e DualShock 4 lidos direto pelo HID (USB ou Bluetooth, sem programas extras) e controles
// XInput (Xbox). Tudo vira o XINPUT_GAMEPAD do Xbox 360: Cruz = A, Bola = B, Quadrado = X, Triangulo = Y, L1/R1 = LB/RB,
// L2/R2 = gatilhos, Options = START, Create/Share = BACK.
//
// Relatorios de entrada (bytes depois do numero do relatorio):
//   DualSense completo (USB 0x01 de 64 bytes; Bluetooth 0x31, 1 byte a mais no inicio):
//     0 LX, 1 LY, 2 RX, 3 RY, 4 L2, 5 R2, 7 direcional (0-7, 8 = solto) + Quadrado/Cruz/Bola/Triangulo (bits 4-7),
//     8 L1 R1 L2 R2 Create Options L3 R3 (bits 0-7), 9 PS (bit 0)
//   DualShock 4 e modo simples do Bluetooth (DS4 USB 0x01; DS4 Bluetooth 0x11 com 2 bytes a mais; DualSense Bluetooth
//   0x01 antes de pedir o modo completo):
//     0 LX, 1 LY, 2 RX, 3 RY, 4 direcional + simbolos, 5 L1 R1 L2 R2 Share Options L3 R3, 6 PS, 7 L2, 8 R2
// O Bluetooth so manda o relatorio completo depois que o PC le o relatorio de calibracao (feature 0x05 no DualSense,
// 0x02 no DualShock 4).
#include "idioma.h"
#include "controles.h"
#include "guest.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <memory>
#include <algorithm>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "xinput.lib")

namespace
{
    enum class Modelo { DualSense, DualShock4 };

    struct Sony
    {
        std::wstring caminho;
        Modelo modelo = Modelo::DualSense;
        bool bluetooth = false;
        HANDLE h = INVALID_HANDLE_VALUE;
        USHORT tamEntrada = 0, tamSaida = 0;
        std::atomic<bool> ligado{ true };
        std::mutex m;
        XINPUT_GAMEPAD estado{};
        ULONGLONG ultimoMovimento = 0;   // para o "auto": o controle mexido por ultimo
        uint8_t seqSaida = 0;
        int ultimaVibracao = -1;         // nao repete a mesma vibracao (o jogo manda a cada quadro)
        std::mutex mSaida;
    };

    std::mutex g_mutex;
    std::vector<std::shared_ptr<Sony>> g_sony;   // na ordem em que foram achados (sony1, sony2...)
    std::once_flag g_inicio;

    // ------------------------------------------------------------ CRC-32 (Bluetooth do DualSense pede no fim da saida)
    uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0xFFFFFFFFu)
    {
        for (size_t i = 0; i < n; i++)
        {
            crc ^= p[i];
            for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
        }
        return crc;
    }

    SHORT Eixo(uint8_t v, bool inverte)
    {
        int x = ((int)v - 128) * 258;
        if (inverte) x = -x;
        return (SHORT)std::clamp(x, -32767, 32767);
    }

    WORD Direcional(uint8_t hat)
    {
        static const WORD tabela[8] = {
            XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT,
            XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_DOWN,
            XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT,
            XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT };
        return hat < 8 ? tabela[hat] : 0;
    }

    // simbolos (byte do direcional) e ombros/gatilhos/Options (byte seguinte): iguais nos dois formatos
    WORD Botoes(uint8_t b0, uint8_t b1)
    {
        WORD w = Direcional(b0 & 0x0F);
        if (b0 & 0x10) w |= XINPUT_GAMEPAD_X;              // Quadrado
        if (b0 & 0x20) w |= XINPUT_GAMEPAD_A;              // Cruz
        if (b0 & 0x40) w |= XINPUT_GAMEPAD_B;              // Bola
        if (b0 & 0x80) w |= XINPUT_GAMEPAD_Y;              // Triangulo
        if (b1 & 0x01) w |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        if (b1 & 0x02) w |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (b1 & 0x10) w |= XINPUT_GAMEPAD_BACK;           // Create / Share
        if (b1 & 0x20) w |= XINPUT_GAMEPAD_START;          // Options
        if (b1 & 0x40) w |= XINPUT_GAMEPAD_LEFT_THUMB;
        if (b1 & 0x80) w |= XINPUT_GAMEPAD_RIGHT_THUMB;
        return w;
    }

    bool Interpretar(Sony& s, const uint8_t* r, DWORD n, XINPUT_GAMEPAD& g)
    {
        const uint8_t* d = nullptr;
        bool completoDualSense = false;
        if (s.modelo == Modelo::DualSense && r[0] == 0x01 && n >= 64) { d = r + 1; completoDualSense = true; }
        else if (s.modelo == Modelo::DualSense && r[0] == 0x31 && n >= 64) { d = r + 2; completoDualSense = true; }
        else if (s.modelo == Modelo::DualShock4 && r[0] == 0x11 && n >= 12) d = r + 3;
        else if (r[0] == 0x01 && n >= 10) d = r + 1;   // DualShock 4 USB ou modo simples do Bluetooth
        if (!d) return false;
        g = {};
        g.sThumbLX = Eixo(d[0], false); g.sThumbLY = Eixo(d[1], true);
        g.sThumbRX = Eixo(d[2], false); g.sThumbRY = Eixo(d[3], true);
        if (completoDualSense) { g.bLeftTrigger = d[4]; g.bRightTrigger = d[5]; g.wButtons = Botoes(d[7], d[8]); }
        else { g.wButtons = Botoes(d[4], d[5]); g.bLeftTrigger = d[7]; g.bRightTrigger = d[8]; }
        return true;
    }

    void Escrever(Sony& s, const uint8_t* buf, DWORD n)
    {
        OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD feito = 0;
        if (!WriteFile(s.h, buf, n, nullptr, &ov) && GetLastError() == ERROR_IO_PENDING)
        {
            if (WaitForSingleObject(ov.hEvent, 100) != WAIT_OBJECT_0) CancelIoEx(s.h, &ov);
            GetOverlappedResult(s.h, &ov, &feito, TRUE);
        }
        CloseHandle(ov.hEvent);
    }

    void LerSony(std::shared_ptr<Sony> s)
    {
        std::vector<uint8_t> buf(std::max<USHORT>(s->tamEntrada, 64) + 16);
        OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        int falhas = 0;
        while (s->ligado)
        {
            ResetEvent(ov.hEvent);
            DWORD n = 0;
            BOOL ok = ReadFile(s->h, buf.data(), (DWORD)buf.size(), nullptr, &ov);
            if (!ok && GetLastError() != ERROR_IO_PENDING) { if (++falhas > 3) break; Sleep(50); continue; }
            DWORD w = WaitForSingleObject(ov.hEvent, 1000);
            if (w != WAIT_OBJECT_0) { CancelIoEx(s->h, &ov); GetOverlappedResult(s->h, &ov, &n, TRUE); continue; }   // parado: sem dados
            if (!GetOverlappedResult(s->h, &ov, &n, FALSE)) { if (++falhas > 3) break; continue; }
            falhas = 0;
            XINPUT_GAMEPAD g;
            if (n && Interpretar(*s, buf.data(), n, g))
            {
                std::lock_guard lock(s->m);
                bool mexeu = g.wButtons || g.bLeftTrigger > 30 || g.bRightTrigger > 30 || std::abs(g.sThumbLX) > 12000 ||
                             std::abs(g.sThumbLY) > 12000 || std::abs(g.sThumbRX) > 12000 || std::abs(g.sThumbRY) > 12000;
                if (mexeu) s->ultimoMovimento = GetTickCount64();
                s->estado = g;
            }
        }
        CloseHandle(ov.hEvent);
        s->ligado = false;
        CloseHandle(s->h); s->h = INVALID_HANDLE_VALUE;
        Log("[controles] %s desligado", s->modelo == Modelo::DualSense ? "DualSense" : "DualShock 4");
    }

    void Procurar()
    {
        GUID hid; HidD_GetHidGuid(&hid);
        HDEVINFO lista = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (lista == INVALID_HANDLE_VALUE) return;
        SP_DEVICE_INTERFACE_DATA it{ sizeof(it) };
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(lista, nullptr, &hid, i, &it); i++)
        {
            DWORD tam = 0;
            SetupDiGetDeviceInterfaceDetailW(lista, &it, nullptr, 0, &tam, nullptr);
            std::vector<uint8_t> mem(tam + 8);
            auto* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)mem.data();
            det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(lista, &it, det, tam, nullptr, nullptr)) continue;
            std::wstring caminho = det->DevicePath;
            {
                std::lock_guard lock(g_mutex);
                if (std::any_of(g_sony.begin(), g_sony.end(), [&](auto& s) { return s->ligado && s->caminho == caminho; })) continue;
            }
            HANDLE h = CreateFileW(caminho.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;
            HIDD_ATTRIBUTES at{ sizeof(at) };
            bool sony = HidD_GetAttributes(h, &at) && at.VendorID == 0x054C;
            Modelo modelo = Modelo::DualSense;
            if (sony && (at.ProductID == 0x0CE6 || at.ProductID == 0x0DF2)) modelo = Modelo::DualSense;           // DualSense / Edge
            else if (sony && (at.ProductID == 0x05C4 || at.ProductID == 0x09CC || at.ProductID == 0x0BA0)) modelo = Modelo::DualShock4;
            else sony = false;
            HIDP_CAPS caps{};
            PHIDP_PREPARSED_DATA pp = nullptr;
            if (sony && HidD_GetPreparsedData(h, &pp)) { HidP_GetCaps(pp, &caps); HidD_FreePreparsedData(pp); }
            // so a interface de controle (Usage Page 1, Usage 5 = gamepad)
            if (!sony || caps.UsagePage != 0x01 || caps.Usage != 0x05) { CloseHandle(h); continue; }
            auto s = std::make_shared<Sony>();
            s->caminho = caminho; s->modelo = modelo; s->h = h;
            s->tamEntrada = caps.InputReportByteLength; s->tamSaida = caps.OutputReportByteLength;
            s->bluetooth = caps.InputReportByteLength != 64;
            if (s->bluetooth)
            {
                // pede o relatorio de calibracao: o controle passa a mandar o relatorio completo
                uint8_t f[64] = { (uint8_t)(modelo == Modelo::DualSense ? 0x05 : 0x02) };
                HidD_GetFeature(h, f, modelo == Modelo::DualSense ? 41 : 37);
            }
            std::lock_guard lock(g_mutex);
            // reaproveita a vaga de um controle desligado (o mesmo controle religado volta a ser "sony1")
            auto vaga = std::find_if(g_sony.begin(), g_sony.end(), [](auto& x) { return !x->ligado; });
            if (vaga != g_sony.end()) *vaga = s; else g_sony.push_back(s);
            Log("[controles] %s ligado (%s)", modelo == Modelo::DualSense ? "DualSense" : "DualShock 4", s->bluetooth ? "Bluetooth" : "USB");
            std::thread(LerSony, s).detach();
        }
        SetupDiDestroyDeviceInfoList(lista);
    }

    std::shared_ptr<Sony> SonyPorId(const std::string& id)
    {
        if (id.rfind("sony", 0) != 0) return nullptr;
        size_t k = (size_t)atoi(id.c_str() + 4);
        std::lock_guard lock(g_mutex);
        if (k < 1 || k > g_sony.size() || !g_sony[k - 1]->ligado) return nullptr;
        return g_sony[k - 1];
    }

    int XboxPorId(const std::string& id) { return id.rfind("xbox", 0) == 0 ? atoi(id.c_str() + 4) - 1 : -1; }

    // XInputGetState de controles desligados demora (o Windows procura o controle): cada vaga vazia so e
    // consultada de novo depois de 2 s
    bool XboxLer(int k, XINPUT_STATE& st)
    {
        static std::atomic<ULONGLONG> proximaTentativa[4];
        if (k < 0 || k > 3) return false;
        ULONGLONG agora = GetTickCount64();
        if (agora < proximaTentativa[k]) return false;
        if (XInputGetState(k, &st) == ERROR_SUCCESS) return true;
        proximaTentativa[k] = agora + 2000;
        return false;
    }
}

void PadsIniciar()
{
    std::call_once(g_inicio, [] {
        std::thread([] { for (;;) { Procurar(); Sleep(2000); } }).detach();
    });
}

std::vector<PadInfo> PadsLigados()
{
    PadsIniciar();
    std::vector<PadInfo> v;
    {
        std::lock_guard lock(g_mutex);
        for (size_t i = 0; i < g_sony.size(); i++)
            if (g_sony[i]->ligado) v.push_back({ "sony" + std::to_string(i + 1), "" });
    }
    for (auto& p : v) p.nome = PadNome(p.id);   // fora da trava (PadNome tambem a usa)
    for (int k = 0; k < 4; k++)
    {
        XINPUT_STATE st;
        if (XboxLer(k, st)) v.push_back({ "xbox" + std::to_string(k + 1), PadNome("xbox" + std::to_string(k + 1)) });
    }
    return v;
}

std::string PadNome(const std::string& id)
{
    if (id == "teclado") return TR("Teclado e mouse", "Keyboard and mouse");
    if (id == "auto") return TR("Controle (o primeiro que estiver ligado)", "Controller (the first one connected)");
    if (id == "controle1") return TR("Controle 1 (o 1º controle ligado)", "Controller 1 (the 1st connected)");
    if (id == "controle2") return TR("Controle 2 (o 2º controle ligado)", "Controller 2 (the 2nd connected)");
    if (id == "controle3") return TR("Controle 3 (o 3º controle ligado)", "Controller 3 (the 3rd connected)");
    if (id == "controle4") return TR("Controle 4 (o 4º controle ligado)", "Controller 4 (the 4th connected)");
    if (id == "nenhum" || id.empty()) return TR("Nenhum", "None");
    if (int k = XboxPorId(id); k >= 0) return TR("Controle Xbox/XInput ", "Xbox/XInput controller ") + std::to_string(k + 1);
    if (id.rfind("sony", 0) == 0)
    {
        auto s = SonyPorId(id);
        std::string n = !s ? TR("Controle PlayStation ", "PlayStation controller ") + id.substr(4)
                           : std::string(s->modelo == Modelo::DualSense ? "DualSense" : "DualShock 4") + " " + id.substr(4) +
                                 (s->bluetooth ? " (Bluetooth)" : " (USB)");
        return n;
    }
    return id;
}

// "controle1".."controle4" (vagas fixas da janela de controles): o 1o..4o controle ligado, na ordem Sony e depois XInput
// (a mesma da lista de controles ligados). "" = nao ha.
static std::string PadPorOrdem(size_t n)
{
    std::vector<std::string> ids;
    {
        std::lock_guard lock(g_mutex);
        for (size_t i = 0; i < g_sony.size(); i++)
            if (g_sony[i]->ligado) ids.push_back("sony" + std::to_string(i + 1));
    }
    for (int k = 0; k < 4 && ids.size() <= n; k++) { XINPUT_STATE st; if (XboxLer(k, st)) ids.push_back("xbox" + std::to_string(k + 1)); }
    return n < ids.size() ? ids[n] : "";
}

std::string PadResolver(const std::string& id)
{
    if (id.size() == 9 && id.rfind("controle", 0) == 0 && id[8] >= '1' && id[8] <= '4') { PadsIniciar(); return PadPorOrdem(id[8] - '1'); }
    if (id != "auto") return id;
    PadsIniciar();
    // o controle Sony mexido por ultimo; sem Sony, o primeiro XInput ligado
    std::string melhor; ULONGLONG t = 0; bool algum = false;
    {
        std::lock_guard lock(g_mutex);
        for (size_t i = 0; i < g_sony.size(); i++)
            if (g_sony[i]->ligado)
            {
                std::lock_guard l2(g_sony[i]->m);
                if (!algum || g_sony[i]->ultimoMovimento > t) { melhor = "sony" + std::to_string(i + 1); t = g_sony[i]->ultimoMovimento; algum = true; }
            }
    }
    if (algum) return melhor;
    for (int k = 0; k < 4; k++) { XINPUT_STATE st; if (XboxLer(k, st)) return "xbox" + std::to_string(k + 1); }
    return "";
}

bool PadLer(const std::string& id0, XINPUT_GAMEPAD& g)
{
    std::string id = PadResolver(id0);
    if (auto s = SonyPorId(id)) { std::lock_guard lock(s->m); g = s->estado; return true; }
    XINPUT_STATE st;
    if (int k = XboxPorId(id); k >= 0 && XboxLer(k, st)) { g = st.Gamepad; return true; }
    return false;
}

void PadVibrar(const std::string& id0, WORD esquerda, WORD direita)
{
    std::string id = PadResolver(id0);
    if (int k = XboxPorId(id); k >= 0) { XINPUT_VIBRATION v{ esquerda, direita }; XInputSetState(k, &v); return; }
    auto s = SonyPorId(id);
    if (!s || s->h == INVALID_HANDLE_VALUE) return;
    uint8_t forte = (uint8_t)(esquerda >> 8), fraco = (uint8_t)(direita >> 8);
    std::lock_guard lock(s->mSaida);   // uma escrita por vez
    if (s->ultimaVibracao == (forte << 8 | fraco)) return;
    s->ultimaVibracao = forte << 8 | fraco;
    if (s->modelo == Modelo::DualSense)
    {
        // dados comuns: [0] marcas (bit 0 vibracao compativel, bit 1 escolhe vibracao), [2] motor direito, [3] esquerdo
        uint8_t dados[47] = {};
        dados[0] = 0x03; dados[2] = fraco; dados[3] = forte;
        if (!s->bluetooth)
        {
            uint8_t r[48] = { 0x02 };
            memcpy(r + 1, dados, 47);
            Escrever(*s, r, 48);
        }
        else
        {
            // 0x31, sequencia (4 bits de cima), marca 0x10, dados comuns, reservado, CRC-32 de 0xA2 + relatorio
            uint8_t r[78] = { 0x31, (uint8_t)((s->seqSaida++ & 15) << 4), 0x10 };
            memcpy(r + 3, dados, 47);
            uint8_t cab = 0xA2;
            uint32_t crc = ~Crc32(r, 74, Crc32(&cab, 1));
            r[74] = (uint8_t)crc; r[75] = (uint8_t)(crc >> 8); r[76] = (uint8_t)(crc >> 16); r[77] = (uint8_t)(crc >> 24);
            Escrever(*s, r, 78);
        }
    }
    else if (!s->bluetooth)
    {
        uint8_t r[32] = { 0x05, 0x01 };   // so a vibracao (a luz fica como esta)
        r[4] = fraco; r[5] = forte;
        Escrever(*s, r, 32);
    }
}
