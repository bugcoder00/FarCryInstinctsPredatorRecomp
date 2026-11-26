// Opcoes de audio: dispositivos (saida do som do jogo, fone e microfone do chat de voz) e volumes. Usadas pela aba
// AUDIO do launcher e pela janela de audio que o botao AUDIO da janela F2 (video_janela.cpp) abre com o jogo rodando.
// - DispositivosSom: os dispositivos ligados do Windows (id + nome). O id vai para o opcoes.ini (som_saida,
//   voz_fone, voz_microfone); vazio = o padrao do Windows.
// - SomEscolhido/SomEscolher: os ids com trava (a janela troca enquanto audio.cpp e voz.cpp leem); SomVersao muda a
//   cada troca e e o que audio.cpp (saida do jogo) e voz.cpp (fone/microfone) olham para reabrir no dispositivo novo.
// - SomTestar: um bipe curto no dispositivo (para achar qual e qual); MicMedir*: nivel do microfone (barra que mexe
//   quando se fala), com o microfone aberto so pelo botao TESTAR da barra (ate PARAR ou fechar a janela/aba). Antes ele
//   abria junto com a janela: num fone Bluetooth (som e microfone no mesmo fone) o Windows passa o fone para o modo de
//   chamada enquanto o microfone esta aberto - o som do jogo parava ~0,8 s e voltava mono e abafado, e ao fechar a
//   janela ficava assim mais ~4 s e parava de novo ao voltar ao estereo (registro: "atraso da saida" 294 -> 36 -> 294 ms).
// - Volumes: lidos a cada quadro de som (audio.cpp) e a cada pacote de voz (voz.cpp): valem na hora.
#include "idioma.h"
#include "guest.h"
#include "opcoes.h"
#include "tema.h"
#include <windows.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <xaudio2.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

HWND GetGameWindow();   // gpu.cpp

namespace
{
    std::mutex g_somMutex;
    std::atomic<unsigned> g_somVersao{ 0 };

    // COM nesta thread enquanto o objeto existir (launcher e janelas ja podem ter iniciado: tudo bem)
    struct Com
    {
        bool meu;
        Com() { meu = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
        ~Com() { if (meu) CoUninitialize(); }
    };

    std::wstring NomeDe(IMMDevice* d)
    {
        std::wstring nome;
        ComPtr<IPropertyStore> props;
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props)) && SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
            nome = pv.pwszVal;
        PropVariantClear(&pv);
        return nome;
    }
}

std::vector<DispositivoSom> DispositivosSom(bool microfone)
{
    Com com;
    std::vector<DispositivoSom> r;
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDeviceCollection> lista;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->EnumAudioEndpoints(microfone ? eCapture : eRender, DEVICE_STATE_ACTIVE, &lista))) return r;
    UINT n = 0;
    lista->GetCount(&n);
    for (UINT i = 0; i < n; i++)
    {
        ComPtr<IMMDevice> d;
        LPWSTR id = nullptr;
        if (FAILED(lista->Item(i, &d)) || FAILED(d->GetId(&id))) continue;
        r.push_back({ id, NomeDe(d.Get()) });
        CoTaskMemFree(id);
    }
    std::sort(r.begin(), r.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.nome.c_str(), b.nome.c_str()) < 0; });
    return r;
}

std::wstring NomeSomPadrao(bool microfone)
{
    Com com;
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> d;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(microfone ? eCapture : eRender, microfone ? eCommunications : eConsole, &d))) return L"";
    return NomeDe(d.Get());
}

std::wstring SomEscolhido(int qual)
{
    std::lock_guard lk(g_somMutex);
    const Opcoes& o = OpcoesAtuais();
    return qual == SOM_SAIDA ? o.somSaida : qual == SOM_VOZ_FONE ? o.vozFone : o.vozMicrofone;
}

void SomEscolher(int qual, const std::wstring& id)
{
    {
        std::lock_guard lk(g_somMutex);
        Opcoes& o = OpcoesAtuais();
        (qual == SOM_SAIDA ? o.somSaida : qual == SOM_VOZ_FONE ? o.vozFone : o.vozMicrofone) = id;
    }
    g_somVersao++;
}

unsigned SomVersao() { return g_somVersao.load(); }

// bipe curto (dois tons, ~0,5 s) no dispositivo; id vazio = o padrao do Windows. Numa thread: nao trava a janela.
void SomTestar(const std::wstring& id)
{
    std::thread([id] {
        Com com;
        ComPtr<IXAudio2> xa;
        IXAudio2MasteringVoice* m = nullptr;
        IXAudio2SourceVoice* v = nullptr;
        if (FAILED(XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) return;
        if (FAILED(xa->CreateMasteringVoice(&m, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, id.empty() ? nullptr : id.c_str())))
        {
            Log("[audio] teste: o dispositivo escolhido nao abriu");
            return;
        }
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; wf.nChannels = 1; wf.nSamplesPerSec = 48000; wf.wBitsPerSample = 32;
        wf.nBlockAlign = 4; wf.nAvgBytesPerSec = 48000 * 4;
        std::vector<float> s(48000 / 2);
        for (size_t i = 0; i < s.size(); i++)
        {
            const double t = i / 48000.0, hz = t < 0.25 ? 660 : 880;
            const double env = std::min({ 1.0, t * 50, (0.5 - t) * 50 });   // sem estalo no comeco e no fim
            s[i] = (float)(0.3 * env * std::sin(2 * 3.14159265358979 * hz * t));
        }
        if (SUCCEEDED(xa->CreateSourceVoice(&v, &wf)))
        {
            XAUDIO2_BUFFER b{};
            b.AudioBytes = (UINT32)(s.size() * 4);
            b.pAudioData = (const BYTE*)s.data();
            b.Flags = XAUDIO2_END_OF_STREAM;
            v->SubmitSourceBuffer(&b);
            v->Start();
            Sleep(650);
            v->DestroyVoice();
        }
        m->DestroyVoice();
    }).detach();
}

// ------------------------------------------------------------------ nivel do microfone (barra das janelas)
namespace
{
    std::mutex g_micMutex;
    std::thread g_micThread;
    std::atomic<bool> g_micRodando{ false };
    std::atomic<float> g_micNivel{ 0 };
    std::atomic<bool> g_micAbriu{ false };

    void MedirMic(std::wstring id)
    {
        Com com;
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> d;
        ComPtr<IAudioClient> cli;
        ComPtr<IAudioCaptureClient> cap;
        g_micAbriu = false;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return;
        if (id.empty() || FAILED(en->GetDevice(id.c_str(), &d)))
            if (FAILED(en->GetDefaultAudioEndpoint(eCapture, eCommunications, &d))) return;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 1; wf.nSamplesPerSec = 16000; wf.wBitsPerSample = 16;
        wf.nBlockAlign = 2; wf.nAvgBytesPerSec = 32000;
        if (FAILED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)cli.GetAddressOf())) ||
            FAILED(cli->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                   2000000, 0, &wf, nullptr)) ||
            FAILED(cli->GetService(IID_PPV_ARGS(&cap))) || FAILED(cli->Start())) return;
        g_micAbriu = true;
        float nivel = 0;
        while (g_micRodando)
        {
            Sleep(30);
            int pico = 0;
            UINT32 n = 0;
            while (SUCCEEDED(cap->GetNextPacketSize(&n)) && n)
            {
                BYTE* p = nullptr; UINT32 q = 0; DWORD fl = 0;
                if (FAILED(cap->GetBuffer(&p, &q, &fl, nullptr, nullptr))) break;
                if (!(fl & AUDCLNT_BUFFERFLAGS_SILENT))
                    for (UINT32 i = 0; i < q; i++) pico = std::max(pico, std::abs((int)((const int16_t*)p)[i]));
                cap->ReleaseBuffer(q);
            }
            // escala em dB (-60 a 0) para a fala normal aparecer bem na barra; cai devagar
            const float db = pico > 0 ? 20.0f * std::log10(pico / 32768.0f) : -60.0f;
            nivel = std::max(std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f), nivel * 0.85f);
            g_micNivel = nivel;
        }
        cli->Stop();
        g_micNivel = 0;
    }
}

void MicMedirParar()
{
    std::lock_guard lk(g_micMutex);
    g_micRodando = false;
    if (g_micThread.joinable()) g_micThread.join();
}

void MicMedirIniciar(const std::wstring& id)
{
    MicMedirParar();
    std::lock_guard lk(g_micMutex);
    g_micRodando = true;
    g_micThread = std::thread(MedirMic, id);
}

float MicNivel() { return g_micNivel.load(); }
bool MicMedindo() { return g_micRodando.load(); }
bool MicAberto() { return g_micAbriu.load(); }

// barra do nivel (controle STATIC com SS_OWNERDRAW): trilho escuro e a parte do nivel em ambar (verde quando forte)
void DesenharNivelMic(const DRAWITEMSTRUCT* d, COLORREF atras)
{
    RECT r = d->rcItem;
    HBRUSH b = CreateSolidBrush(atras);
    FillRect(d->hDC, &r, b);
    DeleteObject(b);
    RECT t{ r.left, r.top + (r.bottom - r.top) / 2 - tema::S(5), r.right, r.top + (r.bottom - r.top) / 2 + tema::S(5) };
    tema::Arredondado(d->hDC, t, tema::S(4), tema::CAMPO, tema::LINHA);
    const float n = MicNivel();
    if (n > 0.01f)
    {
        RECT p = t;
        p.right = p.left + std::max<LONG>(tema::S(8), (LONG)((t.right - t.left) * n));
        tema::Arredondado(d->hDC, p, tema::S(4), n > 0.6f ? tema::VERDE : tema::AMBAR);
    }
}

// ------------------------------------------------------------------ janela de audio (botao AUDIO da janela F2)
namespace
{
    enum : int
    {
        ID_SAIDA = 300, ID_FONE = 301, ID_MIC = 302,                       // listas (linhas 0, 1, 2)
        ID_TESTE_SAIDA = 310, ID_TESTE_FONE = 311,                         // botoes TESTAR das saidas
        ID_NIVEL = 312, ID_TESTE_MIC = 313,                                // barra do nivel do microfone e TESTAR/PARAR (linha 3)
        ID_VOL_JOGO = 320, ID_VOL_JOGO_VALOR = 321, ID_VOL_VOZ = 322, ID_VOL_VOZ_VALOR = 323,
        ID_DICA_A = 330, ID_FECHAR_A = 331,
    };
    constexpr int LARG = 800, Y0 = 44, PASSO = 36, TIMER_NIVEL = 1;
    std::atomic<HWND> g_janelaA{ nullptr };
    std::mutex g_abrirA;
    HFONT g_fonteA = nullptr;
    float g_escalaA = 1.0f;
    std::vector<DispositivoSom> g_saidas, g_mics;

    int SA(int v) { return (int)(v * g_escalaA + 0.5f); }

    HWND CriarA(HWND pai, const wchar_t* classe, const wchar_t* texto, DWORD estilo, int x, int y, int w, int h, int id)
    {
        if (!wcscmp(classe, L"BUTTON")) estilo |= BS_OWNERDRAW;
        HWND c = CreateWindowExW(0, classe, texto, WS_CHILD | WS_VISIBLE | estilo, SA(x), SA(y), SA(w), SA(h), pai,
                                 (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_fonteA, TRUE);
        if (!wcscmp(classe, WC_COMBOBOXW)) tema::ListaEscura(c);
        if (!wcscmp(classe, L"BUTTON")) tema::AcompanharMouse(c);
        return c;
    }

    void Gravar()
    {
        if (OpcoesAtuais().modoTeste) Log("[audio] modo teste: opcoes.ini nao foi gravado");
        else OpcoesSalvar();
    }

    // lista de dispositivos: item 0 = o padrao (com o nome dele); escolhido = id (vazio = item 0)
    void ListaDisp(HWND j, int id, int linha, const wchar_t* rotulo, const std::vector<DispositivoSom>& ds, const std::wstring& escolhido,
                   const wchar_t* primeiro, bool teste)
    {
        const int y = Y0 + linha * PASSO;
        CriarA(j, L"STATIC", rotulo, 0, 16, y + 4, 196, 20, -1);
        HWND cb = CriarA(j, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 212, y, LARG - 212 - 16 - (teste ? 106 : 0), 300, id);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)primeiro);
        int sel = 0;
        for (size_t i = 0; i < ds.size(); i++)
        {
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)ds[i].nome.c_str());
            if (!escolhido.empty() && ds[i].id == escolhido) sel = (int)i + 1;
        }
        SendMessageW(cb, CB_SETCURSEL, sel, 0);
        if (teste) CriarA(j, L"BUTTON", TR(L"TESTAR", L"TEST"), WS_TABSTOP, LARG - 16 - 96, y - 3, 96, 30, id == ID_SAIDA ? ID_TESTE_SAIDA : ID_TESTE_FONE);
    }

    std::wstring IdDaLista(HWND j, int id, const std::vector<DispositivoSom>& ds)
    {
        const int s = (int)SendDlgItemMessageW(j, id, CB_GETCURSEL, 0, 0);
        return s >= 1 && s <= (int)ds.size() ? ds[s - 1].id : std::wstring();
    }

    void TextoVolume(HWND j, int id, int v)
    {
        wchar_t t[16];
        swprintf(t, 16, L"%d%%", v);
        SetDlgItemTextW(j, id, t);
    }

    HWND Barra(HWND j, int id, int idValor, int linha, const wchar_t* rotulo, int maximo, int v)
    {
        const int y = Y0 + linha * PASSO;
        CriarA(j, L"STATIC", rotulo, 0, 16, y + 4, 196, 20, -1);
        HWND b = CriarA(j, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 204, y - 3, LARG - 204 - 16 - 80, 32, id);
        SendMessageW(b, TBM_SETRANGE, FALSE, MAKELPARAM(0, maximo));
        SendMessageW(b, TBM_SETLINESIZE, 0, 5);
        SendMessageW(b, TBM_SETPAGESIZE, 0, 10);
        SendMessageW(b, TBM_SETPOS, TRUE, v);
        tema::BarraEscura(b);
        CriarA(j, L"STATIC", L"", SS_LEFT, LARG - 16 - 72, y + 4, 72, 20, idValor);
        TextoVolume(j, idValor, v);
        return b;
    }

    void Montar(HWND j)
    {
        const Opcoes& o = OpcoesAtuais();
        g_saidas = DispositivosSom(false);
        g_mics = DispositivosSom(true);
        const std::wstring padraoSaida = std::wstring(TR(L"Padrão do Windows", L"Windows default")) + L" (" + NomeSomPadrao(false) + L")";
        const std::wstring padraoMic = std::wstring(TR(L"Padrão do Windows", L"Windows default")) + L" (" + NomeSomPadrao(true) + L")";
        ListaDisp(j, ID_SAIDA, 0, TR(L"Som do jogo:", L"Game sound:"), g_saidas, SomEscolhido(SOM_SAIDA), padraoSaida.c_str(), true);
        ListaDisp(j, ID_FONE, 1, TR(L"Fone do chat de voz:", L"Voice chat headset:"), g_saidas, SomEscolhido(SOM_VOZ_FONE),
                  TR(L"O mesmo do som do jogo", L"The same as the game sound"), true);
        ListaDisp(j, ID_MIC, 2, TR(L"Microfone:", L"Microphone:"), g_mics, SomEscolhido(SOM_VOZ_MIC), padraoMic.c_str(), false);
        {
            const int y = Y0 + 3 * PASSO;
            CriarA(j, L"STATIC", TR(L"Nível do microfone:", L"Microphone level:"), 0, 16, y + 4, 196, 20, -1);
            CriarA(j, L"STATIC", L"", SS_OWNERDRAW, 212, y, LARG - 212 - 16 - 106, 28, ID_NIVEL);
            CriarA(j, L"BUTTON", TR(L"TESTAR", L"TEST"), WS_TABSTOP, LARG - 16 - 96, y - 1, 96, 30, ID_TESTE_MIC);
        }
        Barra(j, ID_VOL_JOGO, ID_VOL_JOGO_VALOR, 4, TR(L"Volume do jogo:", L"Game volume:"), 100, std::clamp(o.volumeJogo, 0, 100));
        Barra(j, ID_VOL_VOZ, ID_VOL_VOZ_VALOR, 5, TR(L"Volume do chat de voz:", L"Voice chat volume:"), VOLUME_VOZ_MAX, std::clamp(o.volumeVoz, 0, VOLUME_VOZ_MAX));
        const int y = Y0 + 6 * PASSO;
        CriarA(j, L"STATIC", TR(L"TESTAR toca um bipe no dispositivo; no microfone, abre ele (fale: a barra deve mexer). Num fone Bluetooth,"
                                L" com o microfone aberto o som fica pior (modo de chamada do Windows) até PARAR. O chat de voz funciona nas"
                                L" partidas SYSTEM LINK com \"Chat de voz: Sim\". As mudanças valem na hora e ficam no opcoes.ini.",
                                L"TEST plays a beep on the device; on the microphone, it opens it (speak: the bar should move). On a Bluetooth"
                                L" headset the sound gets worse while the microphone is open (Windows call mode) until STOP. Voice chat works in"
                                L" SYSTEM LINK matches with \"Voice chat: Yes\". Changes apply right away and are saved in opcoes.ini."),
               SS_LEFT, 16, y + 4, LARG - 32, 76, ID_DICA_A);
        CriarA(j, L"BUTTON", TR(L"FECHAR", L"CLOSE"), WS_TABSTOP, LARG - 16 - 140, y + 88, 140, 36, ID_FECHAR_A);
    }

    void MicTeste(HWND j, bool ligar)
    {
        if (ligar) { MicMedirIniciar(SomEscolhido(SOM_VOZ_MIC)); SetTimer(j, TIMER_NIVEL, 50, nullptr); }
        else { KillTimer(j, TIMER_NIVEL); MicMedirParar(); InvalidateRect(GetDlgItem(j, ID_NIVEL), nullptr, FALSE); }
        SetDlgItemTextW(j, ID_TESTE_MIC, ligar ? TR(L"PARAR", L"STOP") : TR(L"TESTAR", L"TEST"));
        Log("[audio] janela de audio: microfone %s", ligar ? "aberto (TESTAR)" : "fechado");
    }

    std::wstring SaidaDaVoz()   // o fone da voz; vazio = o mesmo do jogo
    {
        std::wstring f = SomEscolhido(SOM_VOZ_FONE);
        return f.empty() ? SomEscolhido(SOM_SAIDA) : f;
    }

    LRESULT CALLBACK ProcA(HWND j, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_CREATE: Montar(j); return 0;
        case WM_TIMER:
            if (wp == TIMER_NIVEL) InvalidateRect(GetDlgItem(j, ID_NIVEL), nullptr, FALSE);
            return 0;
        case WM_COMMAND:
        {
            const int id = LOWORD(wp);
            if ((id == ID_SAIDA || id == ID_FONE || id == ID_MIC) && HIWORD(wp) == CBN_SELCHANGE)
            {
                const int qual = id == ID_SAIDA ? SOM_SAIDA : id == ID_FONE ? SOM_VOZ_FONE : SOM_VOZ_MIC;
                const std::wstring novo = IdDaLista(j, id, id == ID_MIC ? g_mics : g_saidas);
                SomEscolher(qual, novo);
                if (id == ID_MIC && MicMedindo()) MicMedirIniciar(novo);
                Log("[audio] janela de audio: dispositivo %d trocado (%s)", qual, novo.empty() ? "padrao" : "escolhido");
                Gravar();
                return 0;
            }
            if (id == ID_TESTE_SAIDA && HIWORD(wp) == BN_CLICKED) { SomTestar(SomEscolhido(SOM_SAIDA)); return 0; }
            if (id == ID_TESTE_FONE && HIWORD(wp) == BN_CLICKED) { SomTestar(SaidaDaVoz()); return 0; }
            if (id == ID_TESTE_MIC && HIWORD(wp) == BN_CLICKED) { MicTeste(j, !MicMedindo()); return 0; }
            if (id == ID_FECHAR_A || id == IDCANCEL || id == IDOK) { DestroyWindow(j); return 0; }
            break;
        }
        case WM_HSCROLL:
            if ((HWND)lp == GetDlgItem(j, ID_VOL_JOGO) || (HWND)lp == GetDlgItem(j, ID_VOL_VOZ))
            {
                const bool jogo = (HWND)lp == GetDlgItem(j, ID_VOL_JOGO);
                const int v = std::clamp((int)SendMessageW((HWND)lp, TBM_GETPOS, 0, 0), 0, jogo ? 100 : VOLUME_VOZ_MAX);
                (jogo ? OpcoesAtuais().volumeJogo : OpcoesAtuais().volumeVoz) = v;   // audio.cpp e voz.cpp leem na hora
                TextoVolume(j, jogo ? ID_VOL_JOGO_VALOR : ID_VOL_VOZ_VALOR, v);
                if (LOWORD(wp) == TB_ENDTRACK) { Log("[audio] janela de audio: volume %s = %d%%", jogo ? "do jogo" : "do chat de voz", v); Gravar(); }
                return 0;
            }
            break;
        case WM_NOTIFY:
            if (auto* n = (const NMHDR*)lp; (n->idFrom == ID_VOL_JOGO || n->idFrom == ID_VOL_VOZ) && n->code == NM_CUSTOMDRAW) return tema::DesenharBarra(n);
            break;
        case WM_CTLCOLORSTATIC:
            return tema::CorTexto((HDC)wp, GetDlgCtrlID((HWND)lp) == ID_DICA_A ? tema::TEXTO_FRACO : tema::TEXTO);
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return tema::CorCampo((HDC)wp);
        case WM_DRAWITEM:
        {
            auto* d = (const DRAWITEMSTRUCT*)lp;
            if (d->CtlID == ID_NIVEL) DesenharNivelMic(d, tema::FUNDO);
            else tema::DesenharBotao(d, false, tema::FonteBotao());
            return TRUE;
        }
        case WM_LBUTTONDOWN: SetFocus(j); return 0;
        case WM_CTLCOLORBTN: return (LRESULT)tema::PincelFundo();
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
            tema::Secao(mem, SA(16), SA(12), r.right - SA(16), TR(L"ÁUDIO", L"AUDIO"));
            BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, antes);
            DeleteObject(bmp); DeleteDC(mem);
            EndPaint(j, &ps);
            return 0;
        }
        case WM_CLOSE: DestroyWindow(j); return 0;
        case WM_DESTROY:
            KillTimer(j, TIMER_NIVEL);
            if (MicMedindo()) Log("[audio] janela de audio: microfone fechado");
            MicMedirParar();
            g_janelaA = nullptr;
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(j, msg, wp, lp);
    }

    void RodarA()
    {
        Com com;
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
        InitCommonControlsEx(&icc);
        HDC dc = GetDC(nullptr);
        g_escalaA = GetDeviceCaps(dc, LOGPIXELSY) / 96.0f;
        ReleaseDC(nullptr, dc);
        tema::Iniciar(g_escalaA);
        if (!g_fonteA) g_fonteA = CreateFontW(-SA(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        static bool registrada = false;
        if (!registrada)
        {
            WNDCLASSW wc{};
            wc.lpfnWndProc = ProcA;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            wc.hbrBackground = tema::PincelFundo();
            wc.lpszClassName = L"PredatorAudio";
            RegisterClassW(&wc);
            registrada = true;
        }
        RECT area{ 0, 0, SA(LARG), SA(Y0 + 6 * PASSO + 88 + 36 + 16) };
        const DWORD estilo = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
        AdjustWindowRect(&area, estilo, FALSE);
        HWND j = CreateWindowExW(WS_EX_TOPMOST, L"PredatorAudio", TR(L"Opções de áudio - Far Cry Instincts Predator", L"Audio options - Far Cry Instincts Predator"),
                                 estilo, CW_USEDEFAULT, CW_USEDEFAULT, area.right - area.left, area.bottom - area.top,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!j) { Log("[audio] a janela de opcoes de audio nao abriu (erro %lu)", GetLastError()); return; }
        Log("[audio] janela de opcoes de audio aberta");
        g_janelaA = j;
        AplicarIcone(j);
        tema::TituloEscuro(j);
        RECT r; GetWindowRect(j, &r);
        const int w = r.right - r.left, h = r.bottom - r.top;
        SetWindowPos(j, nullptr, (GetSystemMetrics(SM_CXSCREEN) - w) / 2 + SA(40), std::max(0, (GetSystemMetrics(SM_CYSCREEN) - h) / 2 + SA(40)), 0, 0,
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

void AudioAbrirJanela()
{
    std::lock_guard lock(g_abrirA);
    if (HWND j = g_janelaA) { ShowWindow(j, SW_RESTORE); SetForegroundWindow(j); return; }
    std::thread(RodarA).detach();
}
