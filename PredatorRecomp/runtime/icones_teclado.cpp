// Icones dos botoes do controle nos textos do jogo -> teclas do teclado.
// Os textos do jogo tem codigos ({A}, {B}, {X}, {Y}, {WH}, {BL}, {START}, {BACK}, {ASL}, {ASR}, {DPU}..., {TRIGL},
// {TRIGR}) que o jogo troca por um caractere de icone da fonte "Geometr706 BdCn BT Bold 20" (tabela em 82053C84:
// {A} = Omega, {B} = pi, {X} = U+2202...). Os desenhos desses caracteres ficam em duas folhas DXT3 da fonte: pagina 0
// (512x256: A, B, X, Y, LB, RB, analogicos, direcional) e pagina 1 (256x256: letras + Start, Back, gatilhos).
// Quando o jogador 1 usa teclado e mouse e nao ha controle ligado, os icones sao redesenhados na memoria do jogo como
// teclas (com o nome da tecla configurada no controles.ini; gatilhos = mouse com o botao destacado). Com um controle
// ligado os blocos originais voltam. A textura e conferida por hash (xenos_draw.cpp), entao a troca aparece sozinha.
// FCRECOMP_SEMICONES=1 desliga; FCRECOMP_ICONESTECLADO=1 forca as teclas (teste com controle ligado);
// FCRECOMP_ICONESPISCA=ms alterna teclas/controle (teste da volta); FCRECOMP_ICONESLOG=1 anota as texturas recusadas.
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "guest.h"
#include "idioma.h"
#include "icones_teclado.h"

bool InputIconesTeclado();                                    // input.cpp: teclado do jogador 1 e nenhum controle
std::vector<std::string> InputTeclasDaAcao(const char* acao); // input.cpp: nomes das teclas da acao ("ESPACO"...)

namespace
{
    // posicao de cada icone na folha (texels, inclusivo; vem das coordenadas da fonte) e a(s) acao(oes) do controles.ini
    struct Icone { uint16_t ch; int pagina, x0, y0, x1, y1; const char* acao1; const char* acao2; };
    const Icone ICONES[] = {
        { 0x03A9, 0, 462, 68, 495, 100, "A", nullptr },
        { 0x03C0, 0, 462, 100, 495, 132, "B", nullptr },
        { 0x2202, 0, 469, 0, 502, 32, "X", nullptr },
        { 0x2206, 0, 462, 35, 495, 68, "Y", nullptr },
        { 0x220F, 0, 399, 78, 437, 105, "LB", nullptr },          // {WH} (botao branco do Xbox antigo = LB)
        { 0x2211, 0, 421, 156, 459, 183, "RB", nullptr },         // {BL} (botao preto = RB)
        { 0x2022, 0, 434, 0, 469, 35, "L3", nullptr },            // {ASL} clique no analogico esquerdo
        { 0x221E, 0, 456, 183, 491, 218, "R3", nullptr },         // {ASR}
        { 0x222B, 0, 388, 39, 427, 78, "CIMA", nullptr },         // {DPU}
        { 0x00AA, 0, 271, 42, 310, 81, "BAIXO", nullptr },        // {DPD}
        { 0x00BA, 0, 321, 78, 360, 117, "ESQUERDA", nullptr },    // {DPL}
        { 0x00F8, 0, 356, 0, 395, 39, "DIREITA", nullptr },       // {DPR}
        { 0x00A5, 0, 265, 194, 304, 233, "CIMA", "BAIXO" },       // {DPUD}
        { 0x2265, 0, 395, 0, 434, 39, "ESQUERDA", "DIREITA" },    // {DPLR}
        { 0x00A2, 1, 0, 0, 33, 29, "START", nullptr },
        { 0x00A3, 1, 0, 29, 33, 58, "BACK", nullptr },
        { 0x2248, 1, 33, 0, 60, 38, "LT", nullptr },              // {TRIGL}
        { 0x00FE, 1, 27, 120, 54, 158, "RT", nullptr },           // {TRIGR}
    };
    // impressao digital das folhas originais (hash dos blocos DXT3 de um pedaco, em little-endian): pagina 0 = blocos
    // x 104..127, y 0..15; pagina 1 = blocos x 0..7, y 0..7 (ha copias com compressao um pouco diferente)
    const uint64_t DIGITAIS0[] = { 0x889c0c5dde64ce89ull, 0xc8d3283672ac0871ull };
    const uint64_t DIGITAIS1[] = { 0x383e273c3334de49ull, 0x77785bdeb296ba25ull, 0x5a6f2c3ee3431ab6ull };

    uint64_t Fnv(const uint8_t* p, size_t n, uint64_t h = 0xcbf29ce484222325ull)
    {
        for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
        return h;
    }

    // mesma troca de bytes do envio das texturas (xenos_draw.cpp PrepareRow); vale para ida e volta
    void Endian(uint8_t* b, uint32_t endian)
    {
        if (endian == 1) for (int i = 0; i < 16; i += 2) std::swap(b[i], b[i + 1]);
        else if (endian == 2) for (int i = 0; i < 16; i += 4) { std::swap(b[i], b[i + 3]); std::swap(b[i + 1], b[i + 2]); }
        else if (endian == 3) for (int i = 0; i < 16; i += 4) { std::swap(b[i], b[i + 2]); std::swap(b[i + 1], b[i + 3]); }
    }

    // endereco de um bloco numa textura com tiling (o mesmo calculo de xenos_draw.cpp)
    uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2bpp)
    {
        uint32_t macro = ((y >> 5) * (pitch >> 5)) << (log2bpp + 7);
        uint32_t micro = ((y & 6) << 2) << log2bpp;
        uint32_t row = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2bpp)) + ((y & 1) << 4);
        macro = (x >> 5) << (log2bpp + 7);
        micro = (x & 7) << log2bpp;
        uint32_t off = row + macro + ((micro & ~0xFu) << 1) + (micro & 0xF);
        return ((off & ~0x1FFu) << 3) + ((off & 0x1C0) << 2) + (off & 0x3F) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
    }

    // ------------------------------------------------------------ DXT3
    struct Px { uint8_t r, g, b, a; };
    void Cor565(uint16_t c, Px& p) { p.r = (uint8_t)(((c >> 11) & 31) * 255 / 31); p.g = (uint8_t)(((c >> 5) & 63) * 255 / 63); p.b = (uint8_t)((c & 31) * 255 / 31); }
    uint16_t Para565(int r, int g, int b) { return (uint16_t)(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255)); }

    void Decodificar(const uint8_t* b, Px out[16])
    {
        uint64_t alfa; memcpy(&alfa, b, 8);
        uint16_t c0 = (uint16_t)(b[8] | b[9] << 8), c1 = (uint16_t)(b[10] | b[11] << 8);
        uint32_t idx; memcpy(&idx, b + 12, 4);
        Px p[4]; Cor565(c0, p[0]); Cor565(c1, p[1]);
        p[2] = { (uint8_t)((2 * p[0].r + p[1].r) / 3), (uint8_t)((2 * p[0].g + p[1].g) / 3), (uint8_t)((2 * p[0].b + p[1].b) / 3), 0 };
        p[3] = { (uint8_t)((p[0].r + 2 * p[1].r) / 3), (uint8_t)((p[0].g + 2 * p[1].g) / 3), (uint8_t)((p[0].b + 2 * p[1].b) / 3), 0 };
        for (int i = 0; i < 16; i++)
        {
            out[i] = p[(idx >> (2 * i)) & 3];
            out[i].a = (uint8_t)(((alfa >> (4 * i)) & 15) * 17);
        }
    }

    void Codificar(const Px in[16], uint8_t* b)
    {
        uint64_t alfa = 0;
        for (int i = 0; i < 16; i++) alfa |= (uint64_t)((in[i].a * 15 + 127) / 255) << (4 * i);
        memcpy(b, &alfa, 8);
        // pontas: testa cada par de cores do proprio bloco e fica com o de menor erro (os pixels transparentes nao contam)
        auto paleta = [](uint16_t c0, uint16_t c1, Px p[4]) {
            Cor565(c0, p[0]); Cor565(c1, p[1]);
            p[2] = { (uint8_t)((2 * p[0].r + p[1].r) / 3), (uint8_t)((2 * p[0].g + p[1].g) / 3), (uint8_t)((2 * p[0].b + p[1].b) / 3), 0 };
            p[3] = { (uint8_t)((p[0].r + 2 * p[1].r) / 3), (uint8_t)((p[0].g + 2 * p[1].g) / 3), (uint8_t)((p[0].b + 2 * p[1].b) / 3), 0 };
        };
        auto dist = [](const Px& a, const Px& b) { int dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b; return dr * dr * 2 + dg * dg * 4 + db * db; };
        uint16_t cs[16]; int nc = 0;
        for (int i = 0; i < 16; i++)
            if (in[i].a >= 16)
            {
                uint16_t c = Para565(in[i].r, in[i].g, in[i].b);
                if (std::find(cs, cs + nc, c) == cs + nc) cs[nc++] = c;
            }
        uint16_t c0 = 0, c1 = 0;
        if (nc == 1) c0 = c1 = cs[0];
        long long melhorErro = -1;
        for (int a = 0; a < nc; a++)
            for (int b2 = a + 1; b2 < nc; b2++)
            {
                Px p[4]; paleta(cs[a], cs[b2], p);
                long long e = 0;
                for (int i = 0; i < 16 && (melhorErro < 0 || e < melhorErro); i++)
                {
                    if (in[i].a < 16) continue;
                    int dm = 1 << 30;
                    for (int k = 0; k < 4; k++) dm = std::min(dm, dist(in[i], p[k]));
                    e += (long long)dm * in[i].a;
                }
                if (melhorErro < 0 || e < melhorErro) { melhorErro = e; c0 = cs[a]; c1 = cs[b2]; }
            }
        if (c0 < c1) std::swap(c0, c1);
        Px p[4]; paleta(c0, c1, p);
        uint32_t idx = 0;
        for (int i = 0; i < 16; i++)
        {
            int melhor = 0, dm = 1 << 30;
            for (int k = 0; k < 4; k++) { int d = dist(in[i], p[k]); if (d < dm) { dm = d; melhor = k; } }
            idx |= (uint32_t)melhor << (2 * i);
        }
        b[8] = (uint8_t)c0; b[9] = (uint8_t)(c0 >> 8); b[10] = (uint8_t)c1; b[11] = (uint8_t)(c1 >> 8);
        memcpy(b + 12, &idx, 4);
    }

    // ------------------------------------------------------------ desenho das teclas
    // O que desenhar para uma tecla: texto numa tecla, ou mouse com um botao destacado (1 esq., 2 dir., 3 meio/roda)
    struct Rotulo { std::wstring texto; bool simbolo = false; int mouse = 0; bool espaco = false; };

    Rotulo RotuloDe(const std::string& n)
    {
        Rotulo r;
        auto txt = [&](const wchar_t* t) { r.texto = t; };
        auto sim = [&](const wchar_t* t) { r.texto = t; r.simbolo = true; };
        if (n.size() == 1) r.texto = std::wstring(1, (wchar_t)n[0]);
        else if (n == "ESPACO") { r.espaco = true; r.texto = L" "; }
        else if (n == "ENTER") sim(L"↵");
        else if (n == "ESC") txt(L"ESC");
        else if (n == "TAB") txt(L"TAB");
        else if (n == "BACKSPACE") sim(L"⌫");
        else if (n == "SHIFT_ESQ" || n == "SHIFT_DIR") sim(L"⇧");
        else if (n == "CTRL_ESQ" || n == "CTRL_DIR") txt(L"CTRL");
        else if (n == "ALT_ESQ" || n == "ALT_DIR") txt(L"ALT");
        else if (n == "CAPSLOCK") txt(L"CAPS");
        else if (n == "SETA_CIMA") sim(L"↑");
        else if (n == "SETA_BAIXO") sim(L"↓");
        else if (n == "SETA_ESQUERDA") sim(L"←");
        else if (n == "SETA_DIREITA") sim(L"→");
        else if (n == "INSERT") txt(L"INS");
        else if (n == "DELETE") txt(L"DEL");
        else if (n == "HOME") txt(L"HOME");
        else if (n == "END") txt(L"END");
        else if (n == "PGUP") txt(L"PG↑");
        else if (n == "PGDN") txt(L"PG↓");
        else if (n == "MOUSE_ESQ") r.mouse = 1;
        else if (n == "MOUSE_DIR") r.mouse = 2;
        else if (n == "MOUSE_MEIO" || n == "RODA_CIMA" || n == "RODA_BAIXO") r.mouse = 3;
        else if (n == "MOUSE4") txt(L"M4");
        else if (n == "MOUSE5") txt(L"M5");
        else if (n == "MAIS") txt(L"+");
        else if (n == "MENOS") txt(L"-");
        else if (n == "NUM_MAIS") txt(L"N+");
        else if (n == "NUM_MENOS") txt(L"N-");
        else if (n == "VIRGULA") txt(L",");
        else if (n == "PONTO") txt(L".");
        else if (n.rfind("NUM", 0) == 0 && n.size() == 4) r.texto = L"N" + std::wstring(1, (wchar_t)n[3]);
        else if (n.size() <= 4) for (char c : n) r.texto += (wchar_t)c;   // F1..F12
        return r;
    }

    // distancia (negativa dentro) de um ponto a um retangulo de cantos redondos
    float DistRet(float x, float y, float x0, float y0, float x1, float y1, float raio)
    {
        float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hx = (x1 - x0) / 2 - raio, hy = (y1 - y0) / 2 - raio;
        float qx = std::fabs(x - cx) - hx, qy = std::fabs(y - cy) - hy;
        float fora = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f));
        return fora + std::min(std::max(qx, qy), 0.0f) - raio;
    }

    struct Img { int w = 0, h = 0; std::vector<Px> px; };

    // Mascara do texto (0..255) em supersample: GDI desenha o texto branco num fundo preto
    std::vector<uint8_t> MascaraTexto(int W, int H, const Rotulo& r, float fx0, float fy0, float fx1, float fy1)
    {
        std::vector<uint8_t> m((size_t)W * H, 0);
        if (r.texto.empty()) return m;
        BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dc || !bmp) { if (bmp) DeleteObject(bmp); if (dc) DeleteDC(dc); return m; }
        HGDIOBJ velho = SelectObject(dc, bmp);
        memset(bits, 0, (size_t)W * H * 4);
        const float largura = fx1 - fx0, altura = fy1 - fy0;
        int alt = (int)(altura * (r.simbolo ? 1.0f : r.texto.size() == 1 ? 0.80f : 0.62f));
        HFONT f = nullptr;
        SIZE sz{};
        for (; alt > 4; alt -= std::max(1, alt / 12))
        {
            if (f) DeleteObject(f);
            f = CreateFontW(-alt, 0, 0, 0, FW_HEAVY, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                            ANTIALIASED_QUALITY, DEFAULT_PITCH, r.simbolo ? L"Segoe UI Symbol" : L"Arial");
            SelectObject(dc, f);
            GetTextExtentPoint32W(dc, r.texto.c_str(), (int)r.texto.size(), &sz);
            if (sz.cx <= largura * 0.86f) break;
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT rc{ (LONG)fx0, (LONG)fy0, (LONG)fx1, (LONG)fy1 };
        DrawTextW(dc, r.texto.c_str(), (int)r.texto.size(), &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        const uint8_t* p = (const uint8_t*)bits;
        for (size_t i = 0; i < m.size(); i++) m[i] = p[i * 4 + 2];
        SelectObject(dc, velho);
        if (f) DeleteObject(f);
        DeleteObject(bmp);
        DeleteDC(dc);
        return m;
    }

    // Desenha 1 ou 2 teclas (lado a lado) numa imagem w x h (a borda de 1 texel fica transparente)
    Img DesenharIcone(int w, int h, const std::vector<Rotulo>& rs)
    {
        const int S = 4, W = w * S, H = h * S;
        std::vector<float> cr((size_t)W * H), cg(cr.size()), cb(cr.size()), ca(cr.size(), 0.0f);
        const float borda = 1.0f * S;
        const int n = (int)rs.size();
        for (int k = 0; k < n; k++)
        {
            const Rotulo& r = rs[k];
            // caixa da tecla (em pixels do supersample)
            float x0 = borda, x1 = (float)W - borda, y0 = borda, y1 = (float)H - borda;
            if (n == 2)
            {
                float lado = std::min(((float)W - 2 * borda - S) / 2, (float)H - 2 * borda);
                x0 = borda + k * (lado + S); x1 = x0 + lado;
                y0 = (H - lado) / 2; y1 = y0 + lado;
            }
            const float tam = std::min(x1 - x0, y1 - y0);
            const float linha = std::max(1.3f * S, tam * 0.045f);
            if (r.mouse)
            {
                // mouse: corpo arredondado, divisao dos botoes e o botao usado destacado em ambar
                float mw = tam * 0.66f, mh = (y1 - y0) * 0.94f;
                float mx0 = (x0 + x1 - mw) / 2, my0 = (y0 + y1 - mh) / 2, mx1 = mx0 + mw, my1 = my0 + mh;
                float raio = mw * 0.46f, divY = my0 + mh * 0.42f, meio = (mx0 + mx1) / 2;
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++)
                    {
                        float px = x + 0.5f, py = y + 0.5f;
                        float d = DistRet(px, py, mx0, my0, mx1, my1, raio);
                        if (d > 0) continue;
                        size_t i = (size_t)y * W + x;
                        float t = (py - my0) / mh;
                        float R = 232 - 50 * t, G = 232 - 50 * t, B = 236 - 46 * t;
                        bool botoes = py < divY;
                        bool esq = px < meio;
                        float roda = DistRet(px, py, meio - mw * 0.09f, my0 + mh * 0.12f, meio + mw * 0.09f, my0 + mh * 0.34f, mw * 0.09f);
                        if (botoes && ((r.mouse == 1 && esq) || (r.mouse == 2 && !esq))) { R = 236; G = 174 - 30 * t; B = 52; }
                        if (r.mouse == 3 && roda <= 0) { R = 236; G = 174; B = 52; }
                        else if (roda <= 0 && roda > -linha * 0.8f) { R = 40; G = 40; B = 44; }
                        if (d > -linha || (botoes && std::fabs(px - meio) < linha * 0.45f && roda > 0) ||
                            (std::fabs(py - divY) < linha * 0.45f))
                        { R = 30; G = 30; B = 34; }
                        cr[i] = R; cg[i] = G; cb[i] = B; ca[i] = 1;
                    }
                continue;
            }
            // tecla: corpo escuro por fora, aba de baixo cinza e o topo claro com o nome
            const float raio = tam * 0.2f;
            const float fx0 = x0 + linha + tam * 0.05f, fx1 = x1 - linha - tam * 0.05f;
            const float fy0 = y0 + linha + tam * 0.02f, fy1 = y1 - linha - tam * 0.13f;
            auto mascara = MascaraTexto(W, H, r, fx0, fy0, fx1, fy1);
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                {
                    float px = x + 0.5f, py = y + 0.5f;
                    float d = DistRet(px, py, x0, y0, x1, y1, raio);
                    if (d > 0) continue;
                    size_t i = (size_t)y * W + x;
                    float R, G, B;
                    if (d > -linha) { R = 28; G = 28; B = 32; }
                    else if (DistRet(px, py, fx0, fy0, fx1, fy1, raio * 0.7f) <= 0)
                    {
                        float t = (py - fy0) / (fy1 - fy0);
                        R = 246 - 34 * t; G = 246 - 34 * t; B = 248 - 30 * t;
                        float a = mascara[i] / 255.0f;
                        if (r.espaco)
                        {
                            // barra de espaco: um colchete deitado grande no meio da tecla
                            float ex0 = fx0 + (fx1 - fx0) * 0.2f, ex1 = fx1 - (fx1 - fx0) * 0.2f;
                            float ey = fy0 + (fy1 - fy0) * 0.66f, eh = (fy1 - fy0) * 0.26f, ew = linha * 1.15f;
                            bool base = px >= ex0 && px <= ex1 && py >= ey - ew && py <= ey + ew * 0.3f;
                            bool lados = (std::fabs(px - ex0 - ew * 0.5f) <= ew * 0.75f || std::fabs(px - ex1 + ew * 0.5f) <= ew * 0.75f) &&
                                         py >= ey - eh && py <= ey + ew * 0.3f;
                            a = (base || lados) ? 1.0f : 0.0f;
                        }
                        R += (30 - R) * a; G += (30 - G) * a; B += (34 - B) * a;
                    }
                    else { R = 150; G = 150; B = 158; }
                    cr[i] = R; cg[i] = G; cb[i] = B; ca[i] = 1;
                }
        }
        Img img; img.w = w; img.h = h; img.px.resize((size_t)w * h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                float R = 0, G = 0, B = 0, A = 0;
                for (int sy = 0; sy < S; sy++)
                    for (int sx = 0; sx < S; sx++)
                    {
                        size_t i = (size_t)(y * S + sy) * W + x * S + sx;
                        R += cr[i] * ca[i]; G += cg[i] * ca[i]; B += cb[i] * ca[i]; A += ca[i];
                    }
                Px& p = img.px[(size_t)y * w + x];
                if (A > 0) { p.r = (uint8_t)(R / A); p.g = (uint8_t)(G / A); p.b = (uint8_t)(B / A); }
                else p.r = p.g = p.b = 28;   // fora: cor da borda (o filtro bilinear nao clareia a beirada)
                p.a = (uint8_t)std::lround(A / (S * S) * 255);
            }
        return img;
    }

    // ------------------------------------------------------------ folhas na memoria do jogo
    struct Folha
    {
        int pagina = 0;
        std::vector<std::pair<uint32_t, std::array<uint8_t, 16>>> originais;   // deslocamento -> bloco (como no jogo)
        std::vector<std::pair<int, int>> blocos;                               // (bx, by) de cada bloco de originais
        std::string estado;          // "" = originais; senao os nomes aplicados
        uint64_t escrito = 0;        // hash dos blocos depois da ultima escrita
        uint64_t quadro = ~0ull;     // ultima conferencia
    };
    std::mutex g_mx;
    std::map<uint32_t, Folha> g_folhas;
    std::map<uint32_t, uint64_t> g_recusadas;   // endereco -> quadro em que nao bateu (confere de novo depois)

    // nomes das teclas de cada icone ("" = deixa o icone original); atualizados a cada 0,5 s
    std::vector<std::vector<std::string>> NomesAtuais(std::string& assinatura)
    {
        static std::vector<std::vector<std::string>> nomes;
        static std::string ass;
        static ULONGLONG prox = 0;
        static const bool forcar = getenv("FCRECOMP_ICONESTECLADO") != nullptr;
        ULONGLONG t = GetTickCount64();
        if (t >= prox)
        {
            prox = t + 500;
            nomes.clear(); ass.clear();
            bool teclado = forcar || InputIconesTeclado();
            // teste da volta aos icones do controle: FCRECOMP_ICONESPISCA=ms alterna teclas/controle
            static const int pisca = atoi(getenv("FCRECOMP_ICONESPISCA") ? getenv("FCRECOMP_ICONESPISCA") : "0");
            if (pisca > 0) teclado = (t / pisca) % 2 == 0;
            if (teclado)
                for (auto& ic : ICONES)
                {
                    std::vector<std::string> v;
                    for (const char* a : { ic.acao1, ic.acao2 })
                    {
                        if (!a) continue;
                        auto ts = InputTeclasDaAcao(a);
                        std::string esc;
                        for (auto& n : ts) if (!RotuloDe(n).texto.empty() || RotuloDe(n).mouse) { esc = n; break; }
                        if (esc.empty()) { v.clear(); break; }
                        v.push_back(esc);
                    }
                    for (auto& s : v) ass += s + ",";
                    ass += ";";
                    nomes.push_back(v);
                }
        }
        assinatura = ass;
        return nomes;
    }
}

void IconesTecladoTextura(uint8_t* mem, uint32_t base, uint32_t width, uint32_t height, uint32_t format,
                          uint32_t endian, bool tiled, uint32_t pitchBlocks, uint64_t quadro)
{
    if (format != 19 && format != 52) return;
    const int pagina = (width == 512 && height == 256) ? 0 : (width == 256 && height == 256) ? 1 : -1;
    if (pagina < 0) return;
    static const bool desligado = getenv("FCRECOMP_SEMICONES") != nullptr;
    if (desligado) return;
    const uint32_t pitchAl = (pitchBlocks + 31) & ~31u;
    auto desloc = [&](int bx, int by) -> uint32_t {
        return tiled ? TiledOffset(bx, by, pitchAl, 4) : ((uint32_t)by * pitchBlocks + bx) * 16;
    };
    auto blocoLE = [&](int bx, int by, uint8_t* b) { memcpy(b, mem + desloc(bx, by), 16); Endian(b, endian); };

    std::lock_guard lk(g_mx);
    auto it = g_folhas.find(base);
    if (it == g_folhas.end())
    {
        // endereco que nao bateu: confere de novo so depois de 300 quadros (a memoria pode ser reaproveitada)
        if (auto r = g_recusadas.find(base); r != g_recusadas.end() && quadro - r->second < 300) return;
        // impressao digital
        uint64_t h = 0xcbf29ce484222325ull;
        const int bx1 = pagina == 0 ? 128 : 8, bx0 = pagina == 0 ? 104 : 0, by1 = pagina == 0 ? 16 : 8;
        for (int by = 0; by < by1; by++)
            for (int bx = bx0; bx < bx1; bx++) { uint8_t b[16]; blocoLE(bx, by, b); h = Fnv(b, 16, h); }
        bool bate = false;
        for (uint64_t d : pagina == 0 ? std::vector<uint64_t>(std::begin(DIGITAIS0), std::end(DIGITAIS0))
                                      : std::vector<uint64_t>(std::begin(DIGITAIS1), std::end(DIGITAIS1)))
            bate |= d == h;
        if (!bate)
        {
            static const bool logar = getenv("FCRECOMP_ICONESLOG") != nullptr;
            if (logar && g_recusadas.find(base) == g_recusadas.end())
                Log("[icones] %08X %ux%u formato %u endian %u %s: impressao %016llX (nao e a folha de icones)", base, width,
                    height, format, endian, tiled ? "tiled" : "linear", (unsigned long long)h);
            g_recusadas[base] = quadro;
            return;
        }
        g_recusadas.erase(base);
        Folha f;
        f.pagina = pagina;
        std::map<std::pair<int, int>, bool> vistos;
        for (auto& ic : ICONES)
        {
            if (ic.pagina != pagina) continue;
            for (int by = ic.y0 / 4; by <= ic.y1 / 4; by++)
                for (int bx = ic.x0 / 4; bx <= ic.x1 / 4; bx++)
                    if (!vistos[{ bx, by }])
                    {
                        vistos[{ bx, by }] = true;
                        std::array<uint8_t, 16> b; memcpy(b.data(), mem + desloc(bx, by), 16);
                        f.originais.push_back({ desloc(bx, by), b });
                        f.blocos.push_back({ bx, by });
                    }
        }
        f.escrito = 0;
        for (auto& [o, b] : f.originais) f.escrito = Fnv(b.data(), 16, f.escrito);
        Log("[icones] folha de icones da fonte (pagina %d) em %08X %s endian %u: %zu blocos", pagina, base,
            tiled ? "tiled" : "linear", endian, f.originais.size());
        it = g_folhas.emplace(base, std::move(f)).first;
    }
    Folha& f = it->second;
    // uma conferencia por quadro: a memoria ainda e a nossa? (o jogo pode ter carregado outra coisa no lugar)
    if (f.quadro != quadro)
    {
        f.quadro = quadro;
        uint64_t h = 0;
        for (auto& [o, b] : f.originais) h = Fnv(mem + o, 16, h);
        if (h != f.escrito)
        {
            Log("[icones] %08X mudou (o jogo trocou a textura): esquecida", base);
            g_folhas.erase(it);
            return;
        }
    }
    std::string ass;
    auto nomes = NomesAtuais(ass);
    if (ass == f.estado) return;

    if (ass.empty())
        for (auto& [o, b] : f.originais) memcpy(mem + o, b.data(), 16);
    else
    {
        // desenho de cada icone desta folha
        std::vector<std::pair<const Icone*, Img>> imgs;
        for (size_t k = 0; k < std::size(ICONES); k++)
        {
            const Icone& ic = ICONES[k];
            if (ic.pagina != f.pagina || k >= nomes.size() || nomes[k].empty()) continue;
            std::vector<Rotulo> rs;
            for (auto& n : nomes[k]) rs.push_back(RotuloDe(n));
            imgs.push_back({ &ic, DesenharIcone(ic.x1 - ic.x0 + 1, ic.y1 - ic.y0 + 1, rs) });
        }
        for (size_t j = 0; j < f.originais.size(); j++)
        {
            auto& [o, orig] = f.originais[j];
            auto [bx, by] = f.blocos[j];
            uint8_t b[16]; memcpy(b, orig.data(), 16); Endian(b, endian);
            Px px[16]; Decodificar(b, px);
            bool mudou = false;
            for (auto& [ic, img] : imgs)
                for (int i = 0; i < 16; i++)
                {
                    int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x < ic->x0 || x > ic->x1 || y < ic->y0 || y > ic->y1) continue;
                    px[i] = img.px[(size_t)(y - ic->y0) * img.w + (x - ic->x0)];
                    mudou = true;
                }
            if (!mudou) { memcpy(mem + o, orig.data(), 16); continue; }
            Codificar(px, b);
            Endian(b, endian);
            memcpy(mem + o, b, 16);
        }
    }
    f.escrito = 0;
    for (auto& [o, b] : f.originais) f.escrito = Fnv(mem + o, 16, f.escrito);
    Log("[icones] %08X: %s", base, ass.empty() ? "icones do controle" : ("teclas " + ass).c_str());
    f.estado = ass;
}

// ---------------------------------------------------------------- foto do controle ("Configuracao do controle")
// A tela "Configuracao do controle" mostra uma foto do controle do Xbox 360 (textura 512x512 DXT3 com tiling; na tela
// ela ocupa um quadrado da interface 640x480, ou seja, 4:3 na tela 16:9). Com teclado e mouse (mesma regra dos icones)
// ela vira um desenho de teclado e mouse com "Aperte F1 para ver ou mudar os controles"; os textos e as linhas que
// apontam os botoes do controle somem (menu_mouse.cpp, ControleTecladoNaTela). Com um controle ligado a foto volta.
namespace
{
    const uint64_t DIGITAIS_CONTROLE[] = { 0xbded68e12e982ba5ull };   // blocos x 40..87, y 60..75 (little-endian)
    constexpr int CTL_BX0 = 40, CTL_BX1 = 88, CTL_BY0 = 60, CTL_BY1 = 76;

    struct FotoControle
    {
        std::vector<uint8_t> originais;   // a textura inteira como no jogo
        bool teclado = false;             // desenho do teclado aplicado
        uint64_t escrito = 0;             // impressao do pedaco conferido depois da ultima escrita
        uint64_t quadro = ~0ull;
    };
    std::map<uint32_t, FotoControle> g_fotos;
    std::map<uint32_t, uint64_t> g_fotosRecusadas;
    std::atomic<ULONGLONG> g_tecladoNaTela{ 0 };   // ultima vez que a foto foi usada com o desenho do teclado

    // tela de supersample W x H; Ret pinta a area de um retangulo de cantos redondos com a cor que 'cor' der
    struct Tela
    {
        int W, H;
        std::vector<float> r, g, b, a;
        Tela(int w, int h) : W(w), H(h), r((size_t)w * h), g(r.size()), b(r.size()), a(r.size(), 0.0f) {}
        template <class F> void Ret(float x0, float y0, float x1, float y1, float raio, F cor)
        {
            const int ix0 = std::max(0, (int)x0), ix1 = std::min(W, (int)std::ceil(x1));
            const int iy0 = std::max(0, (int)y0), iy1 = std::min(H, (int)std::ceil(y1));
            for (int y = iy0; y < iy1; y++)
                for (int x = ix0; x < ix1; x++)
                {
                    const float px = x + 0.5f, py = y + 0.5f, d = DistRet(px, py, x0, y0, x1, y1, raio);
                    if (d > 0) continue;
                    float R, G, B;
                    cor(px, py, d, R, G, B);
                    const size_t i = (size_t)y * W + x;
                    r[i] = R; g[i] = G; b[i] = B; a[i] = 1;
                }
        }
        void Solido(float x0, float y0, float x1, float y1, float raio, float R, float G, float B)
        {
            Ret(x0, y0, x1, y1, raio, [&](float, float, float, float& r2, float& g2, float& b2) { r2 = R; g2 = G; b2 = B; });
        }
    };

    // mascara (0..255) de textos desenhados pelo GDI na tela W x H: cada texto centrado no seu retangulo, com a fonte
    // diminuida ate caber
    struct TextoGdi { std::wstring s; float x0, y0, x1, y1; int altura; };
    std::vector<uint8_t> MascaraTextos(int W, int H, const std::vector<TextoGdi>& ts)
    {
        std::vector<uint8_t> m((size_t)W * H, 0);
        BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (!dc || !bmp) { if (bmp) DeleteObject(bmp); if (dc) DeleteDC(dc); return m; }
        HGDIOBJ velho = SelectObject(dc, bmp);
        HGDIOBJ fonteVelha = GetCurrentObject(dc, OBJ_FONT);
        memset(bits, 0, (size_t)W * H * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        for (auto& t : ts)
        {
            HFONT f = nullptr;
            RECT rc{};
            for (int alt = t.altura; alt > 4; alt -= std::max(1, alt / 16))
            {
                if (f) { SelectObject(dc, fonteVelha); DeleteObject(f); }
                f = CreateFontW(-alt, 0, 0, 0, FW_HEAVY, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Arial");
                SelectObject(dc, f);
                rc = { 0, 0, W, H };
                DrawTextW(dc, t.s.c_str(), (int)t.s.size(), &rc, DT_CALCRECT | DT_NOPREFIX);
                if (rc.right <= t.x1 - t.x0 && rc.bottom <= t.y1 - t.y0) break;
            }
            const LONG h = rc.bottom;
            RECT dst{ (LONG)t.x0, (LONG)((t.y0 + t.y1 - h) / 2), (LONG)t.x1, (LONG)((t.y0 + t.y1 + h) / 2) };
            DrawTextW(dc, t.s.c_str(), (int)t.s.size(), &dst, DT_CENTER | DT_NOPREFIX);
            SelectObject(dc, fonteVelha);
            if (f) DeleteObject(f);
        }
        GdiFlush();
        const uint8_t* p = (const uint8_t*)bits;
        for (size_t i = 0; i < m.size(); i++) m[i] = p[i * 4 + 2];
        SelectObject(dc, velho);
        DeleteObject(bmp);
        DeleteDC(dc);
        return m;
    }

    // Desenho do teclado com o mouse ao lado e o aviso do F1, numa imagem 512x512 que o jogo mostra achatada em 4:3
    // (feito numa tela 4:3 de 2048x1536 e reduzido 4x3 por pixel; as medidas abaixo sao numa tela 1024x768)
    Img DesenharTecladoMouse()
    {
        const int SX = 4, SY = 3, W = 512 * SX, H = 512 * SY;
        const float E = W / 1024.0f;
        Tela t(W, H);
        // teclado: 15 unidades de largura; a fila das teclas F (mais baixa) + 5 filas
        const float kx0 = 34 * E, kx1 = 776 * E, pad = 16 * E;
        const float u = (kx1 - kx0 - 2 * pad) / 15;
        const float altF = 0.72f * u, vaoF = 0.32f * u;
        const float ky0 = 64 * E, ky1 = ky0 + 2 * pad + altF + vaoF + 5 * u;
        const float linha = 2.2f * E;
        t.Ret(kx0, ky0, kx1, ky1, 18 * E, [&](float, float py, float d, float& R, float& G, float& B) {
            if (d > -linha) { R = 150; G = 150; B = 158; return; }
            const float k = (py - ky0) / (ky1 - ky0);
            R = 52 - 18 * k; G = 52 - 18 * k; B = 58 - 18 * k;
        });
        struct Tecla { float x0, y0, x1, y1; const wchar_t* nome; bool destaque; };
        std::vector<Tecla> teclas;
        const float tx = kx0 + pad;
        {
            // Esc, vao, F1-F4, vao, F5-F8, vao, F9-F12 (F1 em destaque: abre a janela dos controles)
            const float y0 = ky0 + pad, y1 = y0 + altF;
            static const wchar_t* fs[] = { L"F1", L"F2", L"F3", L"F4", L"F5", L"F6", L"F7", L"F8", L"F9", L"F10", L"F11", L"F12" };
            teclas.push_back({ tx, y0, tx + u, y1, nullptr, false });
            for (int i = 0; i < 12; i++)
            {
                const float x = tx + u * (2 + i + (i / 4) * 0.5f);
                teclas.push_back({ x, y0, x + u, y1, i == 0 ? fs[i] : nullptr, i == 0 });
            }
        }
        static const std::vector<float> filas[] = {
            { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 },
            { 1.5f, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1.5f },
            { 1.75f, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2.25f },
            { 2.25f, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2.75f },
            { 1.25f, 1.25f, 1.25f, 6.25f, 1.25f, 1.25f, 1.25f, 1.25f },
        };
        for (int f = 0; f < 5; f++)
        {
            const float y0 = ky0 + pad + altF + vaoF + f * u;
            float x = tx;
            for (size_t k = 0; k < filas[f].size(); k++)
            {
                const float w = filas[f][k] * u;
                const wchar_t* nome = nullptr;
                if (f == 1 && k == 2) nome = L"W";   // W A S D (andar) em destaque
                if (f == 2 && k == 1) nome = L"A";
                if (f == 2 && k == 2) nome = L"S";
                if (f == 2 && k == 3) nome = L"D";
                teclas.push_back({ x, y0, x + w, y0 + u, nome, nome != nullptr });
                x += w;
            }
        }
        std::vector<TextoGdi> rotulos;
        for (auto& k : teclas)
        {
            const float m = 0.07f * u;
            const float x0 = k.x0 + m, y0 = k.y0 + m, x1 = k.x1 - m, y1 = k.y1 - m;
            const float h = y1 - y0;
            const float tx0 = x0 + 0.1f * u, tx1 = x1 - 0.1f * u, ty0 = y0 + 0.05f * h, ty1 = y1 - 0.22f * h;
            const bool dq = k.destaque;
            t.Ret(x0, y0, x1, y1, 0.16f * u, [&](float px, float py, float d, float& R, float& G, float& B) {
                if (d > -linha) { R = 22; G = 22; B = 26; return; }
                if (DistRet(px, py, tx0, ty0, tx1, ty1, 0.12f * u) <= 0)
                {
                    const float s = (py - ty0) / (ty1 - ty0);
                    if (dq) { R = 240; G = 182 - 26 * s; B = 60; }
                    else { R = 214 - 30 * s; G = 214 - 30 * s; B = 220 - 28 * s; }
                    return;
                }
                if (dq) { R = 176; G = 116; B = 30; }
                else { R = 128; G = 128; B = 136; }
            });
            if (k.nome) rotulos.push_back({ k.nome, tx0, ty0, tx1, ty1, (int)(0.8f * (ty1 - ty0)) });
        }
        {
            // mouse a direita do teclado, com o fio subindo
            const float mw = 150 * E, mh = 236 * E;
            const float mx0 = 836 * E, my0 = (ky0 + ky1 - mh) / 2, mx1 = mx0 + mw, my1 = my0 + mh;
            const float divY = my0 + mh * 0.42f, meio = (mx0 + mx1) / 2;
            const float lm = 3.0f * E;
            t.Solido(meio - 2.5f * E, 8 * E, meio + 2.5f * E, my0 + 4 * E, 2.5f * E, 22, 22, 26);
            t.Ret(mx0, my0, mx1, my1, mw * 0.46f, [&](float px, float py, float d, float& R, float& G, float& B) {
                const float s = (py - my0) / mh;
                R = 222 - 56 * s; G = 222 - 56 * s; B = 228 - 52 * s;
                const bool botoes = py < divY;
                const float roda = DistRet(px, py, meio - mw * 0.085f, my0 + mh * 0.12f, meio + mw * 0.085f, my0 + mh * 0.32f, mw * 0.085f);
                if (roda <= 0) { R = 70; G = 70; B = 76; }
                if (d > -lm || (botoes && std::fabs(px - meio) < lm * 0.5f && roda > 0) || std::fabs(py - divY) < lm * 0.5f)
                { R = 22; G = 22; B = 26; }
            });
        }
        // aviso do F1 (duas linhas) embaixo, claro com contorno escuro
        const TextoGdi aviso{ TR(L"Aperte F1 para ver\nou mudar os controles", L"Press F1 to see\nor change the controls"),
                              30 * E, 470 * E, 994 * E, 700 * E, (int)(78 * E) };
        const std::vector<uint8_t> mt = MascaraTextos(W, H, { aviso });
        const std::vector<uint8_t> mr = MascaraTextos(W, H, rotulos);
        std::vector<uint8_t> cont(mt.size(), 0), tmp(mt.size(), 0);
        const int ay0 = std::max(0, (int)(440 * E)), ay1 = std::min(H, (int)(730 * E));
        for (int y = ay0; y < ay1; y++)
            for (int x = 0; x < W; x++)
            {
                uint8_t v = 0;
                for (int k = std::max(0, x - 6); k <= std::min(W - 1, x + 6); k++) v = std::max(v, mt[(size_t)y * W + k]);
                tmp[(size_t)y * W + x] = v;
            }
        for (int y = ay0; y < ay1; y++)
            for (int x = 0; x < W; x++)
            {
                uint8_t v = 0;
                for (int k = std::max(ay0, y - 5); k <= std::min(ay1 - 1, y + 5); k++) v = std::max(v, tmp[(size_t)k * W + x]);
                cont[(size_t)y * W + x] = v;
            }
        for (size_t i = 0; i < mt.size(); i++)
        {
            if (mr[i])
            {
                const float a = mr[i] / 255.0f;
                t.r[i] += (24 - t.r[i]) * a; t.g[i] += (24 - t.g[i]) * a; t.b[i] += (28 - t.b[i]) * a;
            }
            if (cont[i])
            {
                if (t.a[i] == 0) { t.r[i] = 16; t.g[i] = 16; t.b[i] = 18; }
                t.a[i] = std::max(t.a[i], cont[i] / 255.0f * 0.8f);
            }
            if (mt[i])
            {
                const float a = mt[i] / 255.0f;
                t.r[i] += (238 - t.r[i]) * a; t.g[i] += (236 - t.g[i]) * a; t.b[i] += (228 - t.b[i]) * a;
                t.a[i] = std::max(t.a[i], a);
            }
        }
        Img img; img.w = 512; img.h = 512; img.px.resize(512 * 512);
        for (int y = 0; y < 512; y++)
            for (int x = 0; x < 512; x++)
            {
                float R = 0, G = 0, B = 0, A = 0;
                for (int sy = 0; sy < SY; sy++)
                    for (int sx = 0; sx < SX; sx++)
                    {
                        const size_t i = (size_t)(y * SY + sy) * W + x * SX + sx;
                        R += t.r[i] * t.a[i]; G += t.g[i] * t.a[i]; B += t.b[i] * t.a[i]; A += t.a[i];
                    }
                Px& p = img.px[(size_t)y * 512 + x];
                if (A > 0) { p.r = (uint8_t)(R / A); p.g = (uint8_t)(G / A); p.b = (uint8_t)(B / A); }
                else p.r = p.g = p.b = 24;
                p.a = (uint8_t)std::lround(A / (SX * SY) * 255);
            }
        return img;
    }
}

bool ControleTecladoNaTela()
{
    return GetTickCount64() - g_tecladoNaTela.load() < 200;
}

void ControleImagemTextura(uint8_t* mem, uint32_t base, uint32_t width, uint32_t height, uint32_t format,
                           uint32_t endian, bool tiled, uint32_t pitchBlocks, uint64_t quadro)
{
    if (format != 19 || width != 512 || height != 512 || !tiled || pitchBlocks > 128) return;
    static const bool desligado = getenv("FCRECOMP_SEMICONES") != nullptr;
    if (desligado) return;
    constexpr uint32_t pitchAl = 128, total = 128 * 128 * 16;
    auto impressao = [&]() {
        uint64_t h = 0xcbf29ce484222325ull;
        for (int by = CTL_BY0; by < CTL_BY1; by++)
            for (int bx = CTL_BX0; bx < CTL_BX1; bx++)
            {
                uint8_t b[16]; memcpy(b, mem + TiledOffset(bx, by, pitchAl, 4), 16); Endian(b, endian);
                h = Fnv(b, 16, h);
            }
        return h;
    };
    std::lock_guard lk(g_mx);
    auto it = g_fotos.find(base);
    if (it == g_fotos.end())
    {
        if (auto r = g_fotosRecusadas.find(base); r != g_fotosRecusadas.end() && quadro - r->second < 300) return;
        const uint64_t h = impressao();
        if (std::find(std::begin(DIGITAIS_CONTROLE), std::end(DIGITAIS_CONTROLE), h) == std::end(DIGITAIS_CONTROLE))
        {
            static const bool logar = getenv("FCRECOMP_ICONESLOG") != nullptr;
            if (logar && g_fotosRecusadas.find(base) == g_fotosRecusadas.end())
                Log("[icones] %08X 512x512 DXT3: impressao %016llX (nao e a foto do controle)", base, (unsigned long long)h);
            g_fotosRecusadas[base] = quadro;
            return;
        }
        g_fotosRecusadas.erase(base);
        FotoControle f;
        f.originais.assign(mem, mem + total);
        f.escrito = h;
        Log("[icones] foto do controle em %08X", base);
        it = g_fotos.emplace(base, std::move(f)).first;
    }
    FotoControle& f = it->second;
    // uma conferencia por quadro: a memoria ainda e a nossa?
    if (f.quadro != quadro)
    {
        f.quadro = quadro;
        if (impressao() != f.escrito)
        {
            Log("[icones] %08X mudou (o jogo trocou a textura): esquecida", base);
            g_fotos.erase(it);
            return;
        }
    }
    std::string ass;
    NomesAtuais(ass);
    const bool teclado = !ass.empty();
    if (teclado) g_tecladoNaTela = GetTickCount64();
    if (teclado == f.teclado) return;
    if (teclado)
    {
        // o desenho codificado fica guardado: a troca seguinte e so uma copia
        static std::vector<uint8_t> pronto;
        static uint32_t prontoEndian = ~0u;
        if (prontoEndian != endian)
        {
            const ULONGLONG t0 = GetTickCount64();
            const Img img = DesenharTecladoMouse();
            pronto.assign(total, 0);
            for (int by = 0; by < 128; by++)
                for (int bx = 0; bx < 128; bx++)
                {
                    Px px[16];
                    for (int i = 0; i < 16; i++) px[i] = img.px[(size_t)(by * 4 + (i >> 2)) * 512 + bx * 4 + (i & 3)];
                    uint8_t b[16];
                    Codificar(px, b);
                    Endian(b, endian);
                    memcpy(pronto.data() + TiledOffset(bx, by, pitchAl, 4), b, 16);
                }
            prontoEndian = endian;
            Log("[icones] desenho do teclado e mouse feito em %llu ms", (unsigned long long)(GetTickCount64() - t0));
        }
        memcpy(mem, pronto.data(), total);
    }
    else memcpy(mem, f.originais.data(), total);
    f.escrito = impressao();
    f.teclado = teclado;
    Log("[icones] %08X: %s", base, teclado ? "desenho do teclado e mouse" : "foto do controle");
}
