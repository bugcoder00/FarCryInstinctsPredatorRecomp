// Controles genericos (DirectInput / HID) e o Steam Controller, lidos direto pelo HID (sem programas extras). Tudo vira o
// XINPUT_GAMEPAD do Xbox 360, como os controles Sony de controles_pad.cpp (que chama HidProcurar a cada 2 s).
//
// Genericos ("hid1".."hid8"): qualquer controle HID que se apresenta como joystick ou gamepad (Usage Page 1, Usage 4 ou
// 5) e nao e Sony (controles_pad.cpp), XInput (o caminho tem "IG_": o XInput ja le) nem Valve (abaixo). O relatorio e
// lido pela descricao que o proprio controle da (HidP_GetUsages / HidP_GetUsageValue), entao vale para qualquer modelo:
//   botoes 1..N (pagina 9) -> botoes do Xbox pela lista "generico_botoes" do controles.ini (padrao: o arranjo mais comum
//   dos controles DirectInput: 1 = X, 2 = A, 3 = B, 4 = Y, 5 = LB, 6 = RB, 7 = LT, 8 = RT, 9 = BACK, 10 = START,
//   11 = LS, 12 = RS); direcional (Hat) -> direcional; X/Y -> analogico esquerdo; analogico direito = Z/Rz (o mais
//   comum nos DirectInput) ou Rx/Ry (quando nao ha Rz); gatilhos analogicos = Rx/Ry quando o direito e Z/Rz, ou os
//   eixos de acelerador/freio (pagina 2); sem eles, os botoes LT/RT da lista.
//
// Steam Controller ("steam1".."steam4"; Valve 0x28DE, cabo 0x1102 ou receptor sem fio 0x1142 com 4 vagas): interface
// do fabricante (Usage Page 0xFF00), relatorio de 64 bytes descrito pela comunidade (sem o programa da Steam):
//   [2] tipo (1 = entrada), [8] RT LT RB LB Y B X A (bits 0-7, gatilhos = clique no fim do curso),
//   [9] direcional do touchpad esquerdo (cima, direita, esquerda, baixo), BACK, Steam, START, grip esquerdo,
//   [10] grip direito, clique do pad esquerdo, clique do pad direito, toque no pad esquerdo, toque no pad direito,
//   -, clique do analogico, [11] LT, [12] RT, [16..19] analogico esquerdo X/Y (int16; com o pad esquerdo tocado sao as
//   coordenadas do pad), [20..23] pad direito X/Y (vira o analogico direito).
//   Ao achar o controle, o "modo teclado e mouse" (o controle fingindo mouse/teclado sem a Steam) e desligado com o
//   relatorio de recurso 0x81 (limpa as teclas) e 0x87 (pad direito sem mouse). Com a Steam aberta, a propria Steam
//   tambem cria um controle XInput: escolha um dos dois na janela F1.
#include "idioma.h"
#include "controles.h"
#include "guest.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace
{
    struct Dispositivo
    {
        std::wstring caminho;
        std::string nome;
        bool steam = false;
        HANDLE h = INVALID_HANDLE_VALUE;
        PHIDP_PREPARSED_DATA pp = nullptr;
        USHORT tamEntrada = 0;
        std::atomic<bool> ligado{ true };
        std::atomic<bool> respondeu{ false };   // ja mandou um relatorio valido (o receptor da Steam tem 4 vagas vazias)
        std::mutex m;
        XINPUT_GAMEPAD estado{};
        ULONGLONG ultimoMovimento = 0;
        // genericos: eixos achados (pagina, uso) e a faixa de cada um
        struct Eixo { USAGE pagina = 0, uso = 0; LONG min = 0, max = 0; bool tem = false; };
        Eixo x, y, z, rx, ry, rz, hat, acel, freio;
        SHORT esqX = 0, esqY = 0;              // Steam: ultimo analogico esquerdo (com o pad tocado o campo e o pad)
    };

    std::mutex g_mutex;
    std::vector<std::shared_ptr<Dispositivo>> g_hid, g_steam;   // vagas: hid1.., steam1..

    // botao generico N (1..) -> botao do Xbox (0x10000 = LT, 0x20000 = RT); trocada pelo controles.ini
    std::mutex g_mapaMutex;
    std::vector<DWORD> g_mapa;
    constexpr DWORD LT = 0x10000, RT = 0x20000;

    struct NomeBotao { const char* nome; DWORD bit; };
    const NomeBotao NOMES[] = {
        { "A", XINPUT_GAMEPAD_A }, { "B", XINPUT_GAMEPAD_B }, { "X", XINPUT_GAMEPAD_X }, { "Y", XINPUT_GAMEPAD_Y },
        { "LB", XINPUT_GAMEPAD_LEFT_SHOULDER }, { "RB", XINPUT_GAMEPAD_RIGHT_SHOULDER }, { "LT", LT }, { "RT", RT },
        { "BACK", XINPUT_GAMEPAD_BACK }, { "START", XINPUT_GAMEPAD_START }, { "LS", XINPUT_GAMEPAD_LEFT_THUMB },
        { "RS", XINPUT_GAMEPAD_RIGHT_THUMB }, { "CIMA", XINPUT_GAMEPAD_DPAD_UP }, { "BAIXO", XINPUT_GAMEPAD_DPAD_DOWN },
        { "ESQUERDA", XINPUT_GAMEPAD_DPAD_LEFT }, { "DIREITA", XINPUT_GAMEPAD_DPAD_RIGHT }, { "-", 0 } };

    std::vector<DWORD> LerMapa(const std::string& texto)
    {
        std::vector<DWORD> v;
        std::stringstream ss(texto);
        for (std::string k; std::getline(ss, k, ',');)
        {
            k.erase(0, k.find_first_not_of(" \t"));
            k.erase(k.find_last_not_of(" \t") + 1);
            for (auto& c : k) c = (char)toupper((unsigned char)c);
            DWORD bit = 0;
            for (auto& n : NOMES) if (k == n.nome) bit = n.bit;
            v.push_back(bit);
        }
        return v;
    }

    float Normal(const Dispositivo::Eixo& e, LONG v)   // 0..1
    {
        if (e.max <= e.min) return 0.5f;
        return std::clamp((float)(v - e.min) / (float)(e.max - e.min), 0.0f, 1.0f);
    }
    SHORT Analogico(float t, bool inverte)   // 0..1 -> -32767..32767
    {
        float a = t * 2.0f - 1.0f;
        if (inverte) a = -a;
        return (SHORT)std::clamp((int)std::lround(a * 32767.0f), -32767, 32767);
    }

    bool Valor(Dispositivo& d, const Dispositivo::Eixo& e, const uint8_t* r, ULONG n, LONG& v)
    {
        if (!e.tem) return false;
        ULONG u = 0;
        if (HidP_GetUsageValue(HidP_Input, e.pagina, 0, e.uso, &u, d.pp, (PCHAR)r, n) != HIDP_STATUS_SUCCESS) return false;
        v = (LONG)u;
        return true;
    }

    bool InterpretarGenerico(Dispositivo& d, const uint8_t* r, ULONG n, XINPUT_GAMEPAD& g)
    {
        g = {};
        std::vector<DWORD> mapa;
        { std::lock_guard l(g_mapaMutex); mapa = g_mapa; }
        // botoes
        USAGE usos[128]; ULONG nu = 128;
        if (HidP_GetUsages(HidP_Input, 0x09, 0, usos, &nu, d.pp, (PCHAR)r, n) == HIDP_STATUS_SUCCESS)
            for (ULONG i = 0; i < nu; i++)
            {
                const DWORD b = usos[i] >= 1 && usos[i] <= mapa.size() ? mapa[usos[i] - 1] : 0;
                if (b == LT) g.bLeftTrigger = 255;
                else if (b == RT) g.bRightTrigger = 255;
                else g.wButtons |= (WORD)b;
            }
        LONG v;
        if (Valor(d, d.x, r, n, v)) g.sThumbLX = Analogico(Normal(d.x, v), false);
        if (Valor(d, d.y, r, n, v)) g.sThumbLY = Analogico(Normal(d.y, v), true);
        const bool direitoZ = d.z.tem && d.rz.tem;
        const auto& dx = direitoZ ? d.z : d.rx;
        const auto& dy = direitoZ ? d.rz : d.ry;
        if (Valor(d, dx, r, n, v)) g.sThumbRX = Analogico(Normal(dx, v), false);
        if (Valor(d, dy, r, n, v)) g.sThumbRY = Analogico(Normal(dy, v), true);
        // gatilhos analogicos (repouso no minimo): Rx/Ry quando o direito e Z/Rz, ou acelerador/freio
        const auto& gl = direitoZ && d.rx.tem ? d.rx : d.freio;
        const auto& gr = direitoZ && d.ry.tem ? d.ry : d.acel;
        if (Valor(d, gl, r, n, v)) g.bLeftTrigger = std::max(g.bLeftTrigger, (BYTE)std::lround(Normal(gl, v) * 255));
        if (Valor(d, gr, r, n, v)) g.bRightTrigger = std::max(g.bRightTrigger, (BYTE)std::lround(Normal(gr, v) * 255));
        // direcional: 8 direcoes a partir do minimo (cima, cima-direita, ...); fora da faixa = solto
        if (Valor(d, d.hat, r, n, v) && v >= d.hat.min && v <= d.hat.max && d.hat.max - d.hat.min >= 7)
        {
            static const WORD tabela[8] = {
                XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT,
                XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_DOWN,
                XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT,
                XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT };
            const LONG k = v - d.hat.min;
            if (k < 8) g.wButtons |= tabela[k];
        }
        return true;
    }

    SHORT I16(const uint8_t* p) { return (SHORT)(p[0] | (p[1] << 8)); }

    bool InterpretarSteam(Dispositivo& d, const uint8_t* r, ULONG n, XINPUT_GAMEPAD& g)
    {
        const uint8_t* p = r + 1;   // r[0] = numero do relatorio (0: o controle nao usa numeros)
        if (n < 25 || p[2] != 0x01) return false;
        g = {};
        const uint8_t b8 = p[8], b9 = p[9], b10 = p[10];
        if (b8 & 0x04) g.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (b8 & 0x08) g.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        if (b8 & 0x10) g.wButtons |= XINPUT_GAMEPAD_Y;
        if (b8 & 0x20) g.wButtons |= XINPUT_GAMEPAD_B;
        if (b8 & 0x40) g.wButtons |= XINPUT_GAMEPAD_X;
        if (b8 & 0x80) g.wButtons |= XINPUT_GAMEPAD_A;
        if (b9 & 0x01) g.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
        if (b9 & 0x02) g.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (b9 & 0x04) g.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        if (b9 & 0x08) g.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (b9 & 0x10) g.wButtons |= XINPUT_GAMEPAD_BACK;
        if (b9 & 0x40) g.wButtons |= XINPUT_GAMEPAD_START;
        if (b9 & 0x80) g.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;    // grip esquerdo
        if (b10 & 0x01) g.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;  // grip direito
        if (b10 & 0x40) g.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;   // clique do analogico
        if (b10 & 0x04) g.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;  // clique do pad direito
        g.bLeftTrigger = std::max<BYTE>(p[11], (b8 & 0x02) ? 255 : 0);
        g.bRightTrigger = std::max<BYTE>(p[12], (b8 & 0x01) ? 255 : 0);
        if (!(b10 & 0x08)) { d.esqX = I16(p + 16); d.esqY = I16(p + 18); }   // pad esquerdo nao tocado: e o analogico
        g.sThumbLX = d.esqX; g.sThumbLY = d.esqY;
        if (b10 & 0x10) { g.sThumbRX = I16(p + 20); g.sThumbRY = I16(p + 22); }   // pad direito tocado = analogico direito
        return true;
    }

    void Ler(std::shared_ptr<Dispositivo> d)
    {
        std::vector<uint8_t> buf(std::max<USHORT>(d->tamEntrada, 65) + 16);
        OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        int falhas = 0;
        while (d->ligado)
        {
            ResetEvent(ov.hEvent);
            DWORD n = 0;
            BOOL ok = ReadFile(d->h, buf.data(), (DWORD)buf.size(), nullptr, &ov);
            if (!ok && GetLastError() != ERROR_IO_PENDING) { if (++falhas > 3) break; Sleep(50); continue; }
            if (WaitForSingleObject(ov.hEvent, 1000) != WAIT_OBJECT_0) { CancelIoEx(d->h, &ov); GetOverlappedResult(d->h, &ov, &n, TRUE); continue; }
            if (!GetOverlappedResult(d->h, &ov, &n, FALSE)) { if (++falhas > 3) break; continue; }
            falhas = 0;
            XINPUT_GAMEPAD g;
            if (!n || !(d->steam ? InterpretarSteam(*d, buf.data(), n, g) : InterpretarGenerico(*d, buf.data(), n, g))) continue;
            if (!d->respondeu.exchange(true)) Log("[controles] %s respondeu", d->nome.c_str());
            std::lock_guard lock(d->m);
            const bool mexeu = g.wButtons || g.bLeftTrigger > 30 || g.bRightTrigger > 30 || std::abs(g.sThumbLX) > 12000 ||
                               std::abs(g.sThumbLY) > 12000 || std::abs(g.sThumbRX) > 12000 || std::abs(g.sThumbRY) > 12000;
            if (mexeu) d->ultimoMovimento = GetTickCount64();
            d->estado = g;
        }
        CloseHandle(ov.hEvent);
        d->ligado = false;
        CloseHandle(d->h); d->h = INVALID_HANDLE_VALUE;
        if (d->pp) { HidD_FreePreparsedData(d->pp); d->pp = nullptr; }
        Log("[controles] %s desligado", d->nome.c_str());
    }

    // eixos do controle generico pela descricao dele
    void AcharEixos(Dispositivo& d, const HIDP_CAPS& caps)
    {
        std::vector<HIDP_VALUE_CAPS> vc(caps.NumberInputValueCaps);
        USHORT nv = caps.NumberInputValueCaps;
        if (!nv || HidP_GetValueCaps(HidP_Input, vc.data(), &nv, d.pp) != HIDP_STATUS_SUCCESS) return;
        for (USHORT i = 0; i < nv; i++)
        {
            const auto& c = vc[i];
            const USAGE uso = c.IsRange ? c.Range.UsageMin : c.NotRange.Usage;
            Dispositivo::Eixo e{ c.UsagePage, uso, c.LogicalMin, c.LogicalMax, true };
            if (e.max <= e.min && c.BitSize > 0 && c.BitSize < 32) { e.min = 0; e.max = (LONG)((1u << c.BitSize) - 1); }   // faixa sem sinal
            Dispositivo::Eixo* alvo = nullptr;
            if (c.UsagePage == 0x01)
                switch (uso)
                {
                case 0x30: alvo = &d.x; break;  case 0x31: alvo = &d.y; break;  case 0x32: alvo = &d.z; break;
                case 0x33: alvo = &d.rx; break; case 0x34: alvo = &d.ry; break; case 0x35: alvo = &d.rz; break;
                case 0x39: alvo = &d.hat; break;
                }
            else if (c.UsagePage == 0x02) alvo = uso == 0xC4 ? &d.acel : uso == 0xC5 ? &d.freio : nullptr;
            if (alvo && !alvo->tem) *alvo = e;
        }
    }

    std::string Texto(const wchar_t* w)
    {
        std::string s;
        for (; *w; w++) s += *w < 128 ? (char)*w : '?';
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }

    // Steam Controller: desliga o "modo teclado e mouse" (relatorio de recurso 0 com o comando no primeiro byte)
    void SteamSemTecladoMouse(HANDLE h)
    {
        uint8_t f[65] = { 0x00, 0x81 };   // limpa as teclas que os botoes mandam
        HidD_SetFeature(h, f, sizeof(f));
        uint8_t c[65] = { 0x00, 0x87, 0x03, 0x08, 0x07, 0x00 };   // registro 0x08 (modo do pad direito) = 7 (nenhum: sem mouse)
        HidD_SetFeature(h, c, sizeof(c));
    }
}

void HidDefinirBotoes(const std::string& lista)
{
    auto v = LerMapa(lista);
    std::lock_guard l(g_mapaMutex);
    g_mapa = v;
}

std::string HidBotoesPadrao() { return "X,A,B,Y,LB,RB,LT,RT,BACK,START,LS,RS"; }

// chamado por controles_pad.cpp (Procurar) para cada interface HID que nao e Sony; true = ficou com ela
bool HidTalvezAdotar(const std::wstring& caminho, HANDLE h, const HIDD_ATTRIBUTES& at)
{
    {
        std::lock_guard l(g_mapaMutex);
        if (g_mapa.empty()) g_mapa = LerMapa(HidBotoesPadrao());
    }
    std::wstring baixo = caminho;
    for (auto& c : baixo) c = (wchar_t)towlower(c);
    if (baixo.find(L"ig_") != std::wstring::npos) return false;   // controle XInput (o XInput ja le)
    PHIDP_PREPARSED_DATA pp = nullptr;
    if (!HidD_GetPreparsedData(h, &pp)) return false;
    HIDP_CAPS caps{};
    HidP_GetCaps(pp, &caps);
    const bool valve = at.VendorID == 0x28DE && (at.ProductID == 0x1102 || at.ProductID == 0x1142);
    const bool steam = valve && caps.UsagePage >= 0xFF00;
    const bool generico = !valve && caps.UsagePage == 0x01 && (caps.Usage == 0x04 || caps.Usage == 0x05);
    if (!steam && !generico) { HidD_FreePreparsedData(pp); return false; }
    auto d = std::make_shared<Dispositivo>();
    d->caminho = caminho; d->h = h; d->pp = pp; d->steam = steam; d->tamEntrada = caps.InputReportByteLength;
    wchar_t produto[128] = {};
    HidD_GetProductString(h, produto, sizeof(produto));
    if (steam) d->nome = at.ProductID == 0x1142 ? "Steam Controller (sem fio)" : "Steam Controller (USB)";
    else d->nome = produto[0] ? Texto(produto) : "Controle generico";
    if (steam) SteamSemTecladoMouse(h);
    else AcharEixos(*d, caps);
    std::lock_guard lock(g_mutex);
    auto& lista = steam ? g_steam : g_hid;
    auto vaga = std::find_if(lista.begin(), lista.end(), [](auto& x) { return !x->ligado; });
    if (vaga != lista.end()) *vaga = d; else lista.push_back(d);
    Log("[controles] %s achado (%04X:%04X, %u botoes, eixos X%d Y%d Z%d Rx%d Ry%d Rz%d direcional%d)", d->nome.c_str(), at.VendorID, at.ProductID,
        caps.NumberInputButtonCaps, d->x.tem, d->y.tem, d->z.tem, d->rx.tem, d->ry.tem, d->rz.tem, d->hat.tem);
    std::thread(Ler, d).detach();
    return true;
}

bool HidJaAberto(const std::wstring& caminho)
{
    std::lock_guard lock(g_mutex);
    for (auto* l : { &g_hid, &g_steam })
        for (auto& d : *l) if (d->ligado && d->caminho == caminho) return true;
    return false;
}

namespace
{
    std::shared_ptr<Dispositivo> PorId(const std::string& id)
    {
        const bool steam = id.rfind("steam", 0) == 0;
        if (!steam && id.rfind("hid", 0) != 0) return nullptr;
        const size_t k = (size_t)atoi(id.c_str() + (steam ? 5 : 3));
        std::lock_guard lock(g_mutex);
        auto& lista = steam ? g_steam : g_hid;
        if (k < 1 || k > lista.size() || !lista[k - 1]->ligado || !lista[k - 1]->respondeu) return nullptr;
        return lista[k - 1];
    }
}

void HidIds(std::vector<std::string>& ids)
{
    std::lock_guard lock(g_mutex);
    for (size_t i = 0; i < g_hid.size(); i++) if (g_hid[i]->ligado && g_hid[i]->respondeu) ids.push_back("hid" + std::to_string(i + 1));
    for (size_t i = 0; i < g_steam.size(); i++) if (g_steam[i]->ligado && g_steam[i]->respondeu) ids.push_back("steam" + std::to_string(i + 1));
}

bool HidLer(const std::string& id, XINPUT_GAMEPAD& g, ULONGLONG* ultimo)
{
    auto d = PorId(id);
    if (!d) return false;
    std::lock_guard lock(d->m);
    g = d->estado;
    if (ultimo) *ultimo = d->ultimoMovimento;
    return true;
}

bool HidNome(const std::string& id, std::string& nome)
{
    const bool steam = id.rfind("steam", 0) == 0;
    if (!steam && id.rfind("hid", 0) != 0) return false;
    auto d = PorId(id);
    const std::string n = id.substr(steam ? 5 : 3);
    nome = d ? d->nome + " " + n : (steam ? "Steam Controller " : TR("Controle genérico ", "Generic controller ")) + n;
    return true;
}
