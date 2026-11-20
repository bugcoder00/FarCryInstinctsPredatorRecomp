// Aviso na tela quando o jogo desbloqueia uma conquista (xam.cpp chama ConquistaAvisar).
//
// O jogo desenha pela placa de video (D3D12), entao o aviso e uma janelinha transparente (camada do Windows, sem foco e
// sem receber o mouse) presa a janela do jogo, embaixo no meio: icone da conquista, "CONQUISTA DESBLOQUEADA", o nome e
// os pontos. Entra subindo e clareando, fica ~5 s e some. Varias seguidas entram uma depois da outra.
// O som e um acorde curto de sino gerado aqui mesmo (tres notas subindo), tocado pelo PlaySound.
// FCRECOMP_TESTE_CONQUISTA=id[,id...]: mostra esses avisos ~4 s depois do 1o quadro (sem anotar nada no perfil).
#include "conquistas.h"
#include "guest.h"
#include "idioma.h"
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <mmsystem.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shlwapi.lib")

namespace fs = std::filesystem;
extern fs::path g_gameDir;
HWND GetGameWindow();

namespace
{
    std::mutex g_mx;
    std::condition_variable g_cv;
    std::deque<uint16_t> g_fila;
    bool g_rodando = false;

    // acorde de sino: tres notas (mi, si, mi agudo) entrando uma depois da outra, cada uma com 2 harmonicos e caindo devagar
    std::vector<uint8_t> MontarSom()
    {
        const int taxa = 44100, n = taxa * 13 / 10;
        std::vector<float> s(n, 0.0f);
        const struct { float freq, ini, vol; } notas[] = { { 659.25f, 0.00f, 0.9f }, { 987.77f, 0.11f, 0.8f }, { 1318.5f, 0.22f, 1.0f } };
        for (const auto& nt : notas)
            for (int i = (int)(nt.ini * taxa); i < n; i++)
            {
                const float t = (float)i / taxa - nt.ini;
                const float ataque = std::min(1.0f, t / 0.006f), queda = std::exp(-t * 3.2f);
                const float w = 6.2831853f * nt.freq * t;
                s[i] += nt.vol * ataque * queda * (std::sin(w) + 0.35f * std::sin(2 * w) * std::exp(-t * 6) + 0.12f * std::sin(3 * w) * std::exp(-t * 9));
            }
        float pico = 0;
        for (float v : s) pico = std::max(pico, std::fabs(v));
        const float ganho = pico > 0 ? 0.32f / pico : 0;
        std::vector<uint8_t> wav(44 + n * 2);
        auto u32 = [&](size_t o, uint32_t v) { memcpy(&wav[o], &v, 4); };
        auto u16 = [&](size_t o, uint16_t v) { memcpy(&wav[o], &v, 2); };
        memcpy(&wav[0], "RIFF", 4); u32(4, (uint32_t)wav.size() - 8); memcpy(&wav[8], "WAVEfmt ", 8);
        u32(16, 16); u16(20, 1); u16(22, 1); u32(24, taxa); u32(28, taxa * 2); u16(32, 2); u16(34, 16);
        memcpy(&wav[36], "data", 4); u32(40, n * 2);
        for (int i = 0; i < n; i++) u16(44 + i * 2, (uint16_t)(int16_t)std::lround(s[i] * ganho * 32767));
        return wav;
    }

    Gdiplus::Bitmap* Png(const std::vector<uint8_t>& d)
    {
        if (d.empty()) return nullptr;
        IStream* st = SHCreateMemStream(d.data(), (UINT)d.size());
        if (!st) return nullptr;
        Gdiplus::Bitmap* b = Gdiplus::Bitmap::FromStream(st);
        st->Release();
        if (b && b->GetLastStatus() != Gdiplus::Ok) { delete b; b = nullptr; }
        return b;
    }

    void Arredondado(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r)
    {
        p.AddArc(x, y, 2 * r, 2 * r, 180, 90);
        p.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 90);
        p.AddArc(x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 0, 90);
        p.AddArc(x, y + h - 2 * r, 2 * r, 2 * r, 90, 90);
        p.CloseFigure();
    }

    // desenha o cartao numa imagem 32 bits (alfa pre-multiplicado, como a camada do Windows quer)
    void DesenharCartao(void* bits, int w, int h, float u, const Conquista& c, Gdiplus::Bitmap* icone)
    {
        Gdiplus::Bitmap alvo(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Gdiplus::Graphics g(&alvo);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        Gdiplus::GraphicsPath caixa;
        Arredondado(caixa, 1, 1, w - 2.0f, h - 2.0f, 12 * u);
        Gdiplus::LinearGradientBrush fundo(Gdiplus::PointF(0, 0), Gdiplus::PointF(0, (float)h),
                                           Gdiplus::Color(240, 34, 30, 24), Gdiplus::Color(240, 14, 12, 10));
        g.FillPath(&fundo, &caixa);
        Gdiplus::Pen borda(Gdiplus::Color(255, 214, 160, 60), 2 * u);
        g.DrawPath(&borda, &caixa);

        // icone (ou um circulo dourado se a imagem nao veio)
        const float m = 12 * u, lado = h - 2 * m;
        if (icone) g.DrawImage(icone, m, m, lado, lado);
        else
        {
            Gdiplus::SolidBrush ouro(Gdiplus::Color(255, 214, 160, 60));
            g.FillEllipse(&ouro, m, m, lado, lado);
        }
        Gdiplus::Pen moldura(Gdiplus::Color(255, 120, 90, 40), 1.5f * u);
        g.DrawRectangle(&moldura, m, m, lado, lado);

        const float tx = m + lado + 14 * u;
        Gdiplus::FontFamily familia(L"Segoe UI");
        Gdiplus::Font pequena(&familia, 13 * u, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font grande(&familia, 22 * u, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font pontos(&familia, 18 * u, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush ambar(Gdiplus::Color(255, 236, 174, 52)), branco(Gdiplus::Color(255, 250, 246, 238));
        Gdiplus::StringFormat fmt;
        fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        wchar_t pts[16]; swprintf(pts, 16, L"%u G", c.pontos);
        Gdiplus::RectF caixaPts;
        g.MeasureString(pts, -1, &pontos, Gdiplus::PointF(0, 0), &caixaPts);
        const float px = w - m - caixaPts.Width;
        g.DrawString(TR(L"CONQUISTA DESBLOQUEADA", L"ACHIEVEMENT UNLOCKED"), -1, &pequena, Gdiplus::RectF(tx, m + 2 * u, px - tx, 20 * u), &fmt, &ambar);
        g.DrawString(c.nome.c_str(), -1, &grande, Gdiplus::RectF(tx, m + 22 * u, px - tx - 6 * u, 32 * u), &fmt, &branco);
        g.DrawString(pts, -1, &pontos, Gdiplus::PointF(px, (h - caixaPts.Height) / 2), &ambar);
    }

    LRESULT CALLBACK ProcAviso(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (msg == WM_NCHITTEST) return HTTRANSPARENT;
        return DefWindowProcW(h, msg, wp, lp);
    }

    void Mostrar(const Conquista& c, Gdiplus::Bitmap* icone, const std::vector<uint8_t>& som)
    {
        HWND jogo = GetGameWindow();
        if (!jogo) return;
        RECT cr; GetClientRect(jogo, &cr);
        const float u = std::clamp(cr.bottom / 720.0f, 0.6f, 3.0f);
        const int w = (int)(480 * u), h = (int)(96 * u);

        BITMAPINFO bi{};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
        void* bits = nullptr;
        HDC tela = GetDC(nullptr), mem = CreateCompatibleDC(tela);
        HBITMAP bmp = CreateDIBSection(tela, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp) { DeleteDC(mem); ReleaseDC(nullptr, tela); return; }
        HGDIOBJ antes = SelectObject(mem, bmp);
        DesenharCartao(bits, w, h, u, c, icone);

        HWND janela = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                      L"PredatorAvisoConquista", L"", WS_POPUP, 0, 0, w, h, jogo, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (janela)
        {
            PlaySoundW((LPCWSTR)som.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
            const ULONGLONG ini = GetTickCount64();
            const int entrada = 350, fica = 4800, saida = 500, total = entrada + fica + saida;
            bool visivel = false;
            for (;;)
            {
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
                const int t = (int)(GetTickCount64() - ini);
                if (t >= total || !IsWindow(jogo)) break;
                float a = 1.0f, sobe = 0.0f;
                if (t < entrada) { const float k = t / (float)entrada; a = k; sobe = (1 - k) * (1 - k) * 24 * u; }
                else if (t > entrada + fica) a = 1.0f - (t - entrada - fica) / (float)saida;
                // embaixo no meio da janela do jogo (acompanha se ela mudar de lugar ou de tamanho)
                RECT c2; GetClientRect(jogo, &c2);
                POINT o{ 0, 0 }; ClientToScreen(jogo, &o);
                POINT pos{ o.x + (c2.right - w) / 2, o.y + c2.bottom - h - (int)(70 * u) + (int)sobe };
                SIZE tam{ w, h };
                POINT zero{ 0, 0 };
                BLENDFUNCTION bf{ AC_SRC_OVER, 0, (BYTE)std::clamp((int)(a * 255), 0, 255), AC_SRC_ALPHA };
                UpdateLayeredWindow(janela, tela, &pos, &tam, mem, &zero, 0, &bf, ULW_ALPHA);
                if (!visivel) { ShowWindow(janela, SW_SHOWNOACTIVATE); visivel = true; }
                Sleep(15);
            }
            DestroyWindow(janela);
        }
        SelectObject(mem, antes);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, tela);
    }

    void Fio()
    {
        ULONG_PTR gdiplus = 0;
        Gdiplus::GdiplusStartupInput gi;
        Gdiplus::GdiplusStartup(&gdiplus, &gi, nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = ProcAviso;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PredatorAvisoConquista";
        RegisterClassW(&wc);

        // a lista (nomes, pontos e icones) so e lida no 1o aviso: o image.bin novo ja tem a tabela; o antigo monta do xex
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::vector<Conquista> lista;
        std::map<uint32_t, std::vector<uint8_t>> imagens;
        const bool lida = ConquistasLer(fs::path(exe).parent_path(), g_gameDir, lista, imagens);
        Log("[conquista] aviso na tela: lista %s (%zu)", lida ? "lida" : "nao lida", lista.size());
        std::map<uint32_t, Gdiplus::Bitmap*> icones;
        for (auto& [id, png] : imagens) icones[id] = Png(png);
        const std::vector<uint8_t> som = MontarSom();

        for (;;)
        {
            uint16_t id;
            {
                std::unique_lock l(g_mx);
                g_cv.wait(l, [] { return !g_fila.empty(); });
                id = g_fila.front();
                g_fila.pop_front();
            }
            Conquista c;
            c.id = id;
            c.nome = TR(L"Conquista ", L"Achievement ") + std::to_wstring(id);
            for (const auto& x : lista)
                if (x.id == id) c = x;
            Mostrar(c, icones.count(c.imagem) ? icones[c.imagem] : nullptr, som);
        }
    }
}

void ConquistaAvisar(uint16_t id)
{
    std::lock_guard l(g_mx);
    g_fila.push_back(id);
    if (!g_rodando) { g_rodando = true; std::thread(Fio).detach(); }
    g_cv.notify_one();
}
