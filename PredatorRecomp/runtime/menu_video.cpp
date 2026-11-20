// "OPCOES DE VIDEO" no menu do perfil do jogo (Opcoes > perfil): as opcoes de video do launcher dentro do jogo.
//
// O menu do perfil (pagina MAGMA P_EditProfile) tem um item que o jogo esconde, "OPCOES DO MULTIJOGADOR"
// (B_MultiplayerSetup, escondido em sub_821E31D0: bit 0x80 de elemento+24 = visivel, 0x40 de +76 = escolhivel). Aqui ele
// volta a aparecer com o nome "OPCOES DE VIDEO" e, escolhido (sub_821E4210 = escolher item do perfil), abre a mesma
// pagina de "OPCOES DE JOGO" (P_Video, objeto da pagina guardado em 0x82911A24) em "modo video": as 5 linhas dela (2
// barras e 3 valores) viram RESOLUCAO INTERNA, SOMBRAS (barras), SUAVIZACAO, QUADROS POR SEGUNDO e SINCRONIA VERTICAL.
// Funcoes da P_Video (tabela de virtuais em 0x8205C430): [0] sub_821D8D28 = item focado (texto de ajuda em pagina+128),
// [2] sub_821D9210 = A no item (troca LEGENDAS etc.), [3] sub_821D91B8 = voltar, [5] sub_821D8D00 = tecla no item
// (r5 -> codigo: 37 esquerda, 39 direita; muda as barras), [9] sub_821D82B8 = pagina mostrada, [23] sub_821D8070 =
// montagem (guarda os widgets: valores em +96/+100/+104, barras em +112/+116, valores das barras em +120/+124).
// sub_821D8300 escreve os valores na tela (texto: sub_82158218 na wstring widget+60; barra: sub_8247A1F0(widget, f1)).
// Os textos dos rotulos ficam na wstring widget+60 (ponteiro +64 ou o proprio buffer se capacidade < 8, tamanho +80,
// capacidade +84) e sao trocados enquanto o modo video esta ligado (e devolvidos depois).
// Tudo vale na hora: resolucao interna e sombras refazem as imagens do desenho no fim do quadro
// (xdraw::PedirNovaEscala). O que muda vai para o opcoes.ini (o mesmo do launcher).
#ifndef PREDATOR_EDITOR
#include "guest.h"
#include "idioma.h"
#include "opcoes.h"
#include "xenos_draw.h"
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <windows.h>

namespace
{
constexpr uint32_t BUSCA_ELEMENTO = 0x82469EC8;   // (r3 = conjunto de elementos da pagina, r4 = id) -> elemento
constexpr uint32_t HASH_NOME = 0x8246A210;        // (r3 = saida, r4 = nome) -> id do elemento (= MAGMA_ELEMENT_ID)
constexpr uint32_t TEXTO_AJUDA = 0x8216A6E8;      // (r3 = widget, r4 = texto UTF-16) muda o texto
constexpr uint32_t WSTRING_COPIA = 0x82158218;    // (r3 = wstring, r4 = texto, r5 = tamanho)
constexpr uint32_t BARRA_VALOR = 0x8247A1F0;      // (r3 = widget, f1 = valor)
constexpr uint32_t BARRA_LIMITES = 0x82479FE8;    // (r3 = widget, r4 = minimo, r5 = maximo)
constexpr uint32_t PAGINA_ESCREVE = 0x821D8300;   // P_Video: valores na tela

// ids dos elementos (MAGMA_ELEMENT_ID do common_ea58b11a.xml)
constexpr uint32_t ID_MULTIJOGADOR = 1606581513, ID_VIDEO = 3355706169;
constexpr uint32_t ID_BRILHO = 963279783, ID_SOM = 1280121489, ID_LEGENDAS = 1853723532, ID_DIVISAO = 2410242335,
                   ID_TROCA_ARMA = 3155260287;

bool g_video = false;   // a pagina P_Video esta (ou vai ser) mostrada como OPCOES DE VIDEO

// ------------------------------------------------------------ chamadas ao jogo
uint32_t Chamar(GuestCpu& ctx, uint32_t f, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0)
{
    GuestCpu salvo = ctx;
    SetGpr(ctx, 3, r3); SetGpr(ctx, 4, r4); SetGpr(ctx, 5, r5);
    GuestCall(ctx, f);
    const uint32_t r = Gpr32(ctx, 3);
    ctx = salvo;
    return r;
}

uint32_t Elemento(GuestCpu& ctx, uint32_t conjunto, uint32_t id) { return conjunto ? Chamar(ctx, BUSCA_ELEMENTO, conjunto, id) : 0; }
uint32_t Widget(GuestCpu& ctx, uint32_t conjunto, uint32_t id) { const uint32_t e = Elemento(ctx, conjunto, id); return e ? R32(e + 8) : 0; }

uint32_t IdDoNome(GuestCpu& ctx, const char* nome)
{
    static std::map<std::string, uint32_t> ids;
    if (auto it = ids.find(nome); it != ids.end()) return it->second;
    const uint32_t n = (uint32_t)strlen(nome);
    const uint32_t buf = GuestAlloc(n + 1 + 16);
    if (!buf) return 0;
    memcpy(G(buf + 16), nome, n + 1);
    const uint32_t r = Chamar(ctx, HASH_NOME, buf, buf + 16);
    return ids[nome] = R32(r);
}

// texto UTF-8 -> UTF-16 big-endian na memoria do jogo (um buffer por texto, guardado)
uint32_t TextoNoJogo(const std::string& s, uint32_t* tamanho = nullptr)
{
    static std::map<std::string, std::pair<uint32_t, uint32_t>> textos;
    auto it = textos.find(s);
    if (it == textos.end())
    {
        std::u16string u;
        for (size_t i = 0; i < s.size();)
        {
            const unsigned char c = (unsigned char)s[i];
            if (c < 0x80) { u += c; i += 1; }
            else if ((c >> 5) == 6 && i + 1 < s.size()) { u += (char16_t)(((c & 0x1F) << 6) | (s[i + 1] & 0x3F)); i += 2; }
            else if (i + 2 < s.size()) { u += (char16_t)(((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F)); i += 3; }
            else break;
        }
        const uint32_t buf = GuestAlloc((uint32_t)(u.size() + 1) * 2);
        if (!buf) return 0;
        for (size_t i = 0; i <= u.size(); i++) W16(buf + (uint32_t)i * 2, i < u.size() ? u[i] : 0);
        it = textos.emplace(s, std::make_pair(buf, (uint32_t)u.size())).first;
    }
    if (tamanho) *tamanho = it->second.second;
    return it->second.first;
}

// texto de um widget (wstring em widget+60)
std::u16string LerTexto(uint32_t w)
{
    const uint32_t tam = R32(w + 80), cap = R32(w + 84);
    const uint32_t p = cap >= 8 ? R32(w + 64) : w + 64;
    std::u16string r;
    if (tam > 400 || !p) return r;
    for (uint32_t i = 0; i < tam; i++) r += (char16_t)R16(p + i * 2);
    return r;
}

void EscreverTexto(GuestCpu& ctx, uint32_t w, const std::string& s)
{
    uint32_t n = 0;
    if (const uint32_t t = TextoNoJogo(s, &n)) Chamar(ctx, WSTRING_COPIA, w + 60, t, n);
}

void EscreverTextoU16(GuestCpu& ctx, uint32_t w, const std::u16string& u)
{
    static std::map<std::u16string, uint32_t> bufs;
    uint32_t& buf = bufs[u];
    if (!buf)
    {
        buf = GuestAlloc((uint32_t)(u.size() + 1) * 2);
        if (!buf) return;
        for (size_t i = 0; i <= u.size(); i++) W16(buf + (uint32_t)i * 2, i < u.size() ? u[i] : 0);
    }
    Chamar(ctx, WSTRING_COPIA, w + 60, buf, (uint32_t)u.size());
}

void Barra(GuestCpu& ctx, uint32_t w, float v)
{
    if (!w) return;
    GuestCpu salvo = ctx;
    SetGpr(ctx, 3, w); SetFpr(ctx, 1, v);
    GuestCall(ctx, BARRA_VALOR);
    ctx = salvo;
}

// ------------------------------------------------------------ as 5 opcoes
enum Linha { RESOLUCAO, SOMBRAS, SUAVIZACAO, FPS, VSYNC, LINHAS };

int Valor(Linha l)
{
    const Opcoes& o = OpcoesAtuais();
    switch (l)
    {
    case RESOLUCAO: return o.fsr > 0 ? 3 + ListaDoFsr(o.fsr) : std::clamp(o.escala, 0, 3);   // 4..7 = FSR ultra, qualidade, equilibrado, desempenho (render.cpp)
    case SOMBRAS: return std::clamp(o.sombras, 0, 2);
    case SUAVIZACAO: return std::clamp(o.antisserrilhado, 0, 3);
    case FPS: return std::clamp(o.fps, 0, 2);
    default: return o.vsync ? 1 : 0;
    }
}

int Quantos(Linha l) { return l == RESOLUCAO ? 8 : l == SUAVIZACAO ? 4 : l == VSYNC ? 2 : 3; }

std::string NomeValor(Linha l, int v)
{
    switch (l)
    {
    case RESOLUCAO: { static const char* n[] = { "1x (1280x720)", "1,5x (1920x1080)", "2x (2560x1440)", "3x (3840x2160)",
                                                 "FSR ULTRA QUALIDADE", "FSR QUALIDADE", "FSR EQUILIBRADO", "FSR DESEMPENHO" };
                      static const char* e[] = { "1x (1280x720)", "1.5x (1920x1080)", "2x (2560x1440)", "3x (3840x2160)",
                                                 "FSR ULTRA QUALITY", "FSR QUALITY", "FSR BALANCED", "FSR PERFORMANCE" }; return TR(n[v], e[v]); }
    case SOMBRAS: { static const char* n[] = { "NORMAL", "ALTA", "MUITO ALTA" }, *e[] = { "NORMAL", "HIGH", "VERY HIGH" }; return TR(n[v], e[v]); }
    case SUAVIZACAO: { static const char* n[] = { "DESLIGADA", "LEVE", "MÉDIA", "FORTE" }, *e[] = { "OFF", "LOW", "MEDIUM", "HIGH" }; return TR(n[v], e[v]); }
    case FPS: { static const char* n[] = { "30", "60", "SEM LIMITE" }, *e[] = { "30", "60", "UNLOCKED" }; return TR(n[v], e[v]); }
    default: return v ? TR("SIM", "YES") : TR("NÃO", "NO");
    }
}

const char* NomeLinha(Linha l)
{
    static const char* n[] = { "RESOLUÇÃO INTERNA", "SOMBRAS", "SUAVIZAÇÃO", "QUADROS POR SEGUNDO", "SINCRONIA VERTICAL" };
    static const char* e[] = { "INTERNAL RESOLUTION", "SHADOWS", "ANTI-ALIASING", "FRAMES PER SECOND", "VERTICAL SYNC" };
    return TR(n[l], e[l]);
}

const char* Ajuda(Linha l)
{
    static const char* n[] = {
        "Imagem mais nítida, mas pesa mais na placa de vídeo. FSR desenha menor e amplia para a janela (mais leve).",
        "Sombras com bordas mais definidas (mais pesadas).",
        "Suaviza as bordas serrilhadas da imagem.",
        "30 como o original, 60 mais fluido. Sem limite é experimental.",
        "Evita \"rasgos\" na imagem, mas pode baixar o FPS.",
    };
    static const char* e[] = {
        "Sharper image, but heavier on the graphics card. FSR draws smaller and upscales to the window (lighter).",
        "Shadows with sharper edges (heavier).",
        "Smooths the jagged edges of the image.",
        "30 like the original, 60 smoother. Unlocked is experimental.",
        "Avoids image \"tearing\", but may lower the FPS.",
    };
    return TR(n[l], e[l]);
}

void Mudar(Linha l, int v)
{
    Opcoes& o = OpcoesAtuais();
    v = std::clamp(v, 0, Quantos(l) - 1);
    if (v == Valor(l)) return;
    switch (l)
    {
    case RESOLUCAO: if (v <= 3) { o.escala = v; o.fsr = 0; } else o.fsr = FsrDaLista(v - 3); xdraw::PedirNovaEscala(); break;
    case SOMBRAS: o.sombras = v; xdraw::PedirNovaEscala(); break;
    case SUAVIZACAO: o.antisserrilhado = v; xdraw::RecarregarSuavizacao(); break;
    case FPS: o.fps = v; break;
    default: o.vsync = v != 0; break;
    }
    Log("[video] %s = %s", NomeLinha(l), NomeValor(l, v).c_str());
    if (o.modoTeste) Log("[video] modo teste: opcoes.ini nao foi gravado");
    else OpcoesSalvar();
}

// linha da tela pelo elemento (as 2 barras e as 3 linhas de valor da P_Video)
int LinhaDoId(uint32_t id)
{
    switch (id)
    {
    case ID_BRILHO: return RESOLUCAO;
    case ID_SOM: return SOMBRAS;
    case ID_LEGENDAS: return SUAVIZACAO;
    case ID_DIVISAO: return FPS;
    case ID_TROCA_ARMA: return VSYNC;
    default: return -1;
    }
}

uint32_t Conjunto(uint32_t pagina) { return R32(R32(pagina + 80)); }

// rotulos trocados (widget -> texto original)
std::map<uint32_t, std::u16string> g_originais;

void TrocarRotulo(GuestCpu& ctx, uint32_t w, const std::string& s)
{
    if (!w) return;
    if (!g_originais.count(w)) g_originais[w] = LerTexto(w);
    EscreverTexto(ctx, w, s);
}

void DevolverRotulos(GuestCpu& ctx)
{
    for (auto& [w, t] : g_originais) EscreverTextoU16(ctx, w, t);
    g_originais.clear();
}

uint32_t TituloDaPagina(GuestCpu& ctx, uint32_t pagina)
{
    const uint32_t id = IdDoNome(ctx, "B_TitleButton");
    for (int k = 0; k < 2; k++)
        if (const uint32_t w = Widget(ctx, R32(R32(pagina + 80) + 4 * k), id)) return w;
    return 0;
}

// rotulos e valores da pagina no modo video
void EscreverPaginaVideo(GuestCpu& ctx, uint32_t pagina)
{
    const uint32_t c = Conjunto(pagina);
    TrocarRotulo(ctx, Widget(ctx, c, ID_BRILHO), std::string(NomeLinha(RESOLUCAO)) + ": " + NomeValor(RESOLUCAO, Valor(RESOLUCAO)));
    TrocarRotulo(ctx, Widget(ctx, c, ID_SOM), std::string(NomeLinha(SOMBRAS)) + ": " + NomeValor(SOMBRAS, Valor(SOMBRAS)));
    TrocarRotulo(ctx, Widget(ctx, c, ID_LEGENDAS), NomeLinha(SUAVIZACAO));
    TrocarRotulo(ctx, Widget(ctx, c, ID_DIVISAO), NomeLinha(FPS));
    TrocarRotulo(ctx, Widget(ctx, c, ID_TROCA_ARMA), NomeLinha(VSYNC));
    TrocarRotulo(ctx, TituloDaPagina(ctx, pagina), TR("OPÇÕES DE VÍDEO", "VIDEO OPTIONS"));
    if (const uint32_t w = R32(pagina + 96)) EscreverTexto(ctx, w, NomeValor(SUAVIZACAO, Valor(SUAVIZACAO)));
    if (const uint32_t w = R32(pagina + 100)) EscreverTexto(ctx, w, NomeValor(FPS, Valor(FPS)));
    if (const uint32_t w = R32(pagina + 104)) EscreverTexto(ctx, w, NomeValor(VSYNC, Valor(VSYNC)));
    W32(pagina + 120, Valor(RESOLUCAO)); Barra(ctx, R32(pagina + 112), (float)Valor(RESOLUCAO));
    W32(pagina + 124, Valor(SOMBRAS)); Barra(ctx, R32(pagina + 116), (float)Valor(SOMBRAS));
}

void Ajudar(GuestCpu& ctx, uint32_t pagina, int linha)
{
    if (linha < 0) return;
    if (const uint32_t w = R32(pagina + 128))
        if (const uint32_t t = TextoNoJogo(Ajuda((Linha)linha))) Chamar(ctx, TEXTO_AJUDA, w, t);
}
}

// ---------------------------------------------------------------- menu do perfil (P_EditProfile)
// montagem: o jogo esconde "OPCOES DO MULTIJOGADOR"; volta a mostrar com o nome "OPCOES DE VIDEO"
GUEST_DECLARE_ORIGINAL(sub_821E31D0);
GUEST_FUNC(sub_821E31D0)
{
    const uint32_t pagina = Gpr32(ctx, 3);
    const uint32_t e = Elemento(ctx, R32(R32(pagina + 80) + 4), ID_MULTIJOGADOR);
    const uint8_t b24 = e ? R8(e + 24) : 0, b76 = e ? R8(e + 76) : 0;
    // sem o bit 0x40 o jogo nao desliga o item nem redesenha o widget como desligado (ficava apagado)
    if (e) W8(e + 76, b76 & ~0x40);
    GUEST_ORIGINAL(sub_821E31D0)(ctx, base);
    if (!e) return;
    W8(e + 24, b24); W8(e + 76, b76);
    if (const uint32_t w = R32(e + 8)) EscreverTexto(ctx, w, TR("OPÇÕES DE VÍDEO", "VIDEO OPTIONS"));
}

// item focado: texto de ajuda (widget em pagina+500)
GUEST_DECLARE_ORIGINAL(sub_821E4648);
GUEST_FUNC(sub_821E4648)
{
    const uint32_t pagina = Gpr32(ctx, 3), ev = Gpr32(ctx, 4);
    GUEST_ORIGINAL(sub_821E4648)(ctx, base);
    if (R32(ev + 4) != ID_MULTIJOGADOR) return;
    if (const uint32_t w = R32(pagina + 500))
        if (const uint32_t t = TextoNoJogo(TR("Resolução interna, sombras, suavização, quadros por segundo e sincronia vertical.",
                                               "Internal resolution, shadows, anti-aliasing, frames per second and vertical sync.")))
            Chamar(ctx, TEXTO_AJUDA, w, t);
}

// escolher um item: OPCOES DE VIDEO abre a pagina de OPCOES DE JOGO no modo video
GUEST_DECLARE_ORIGINAL(sub_821E4210);
GUEST_FUNC(sub_821E4210)
{
    const uint32_t ev = Gpr32(ctx, 4);
    const uint32_t id = R32(ev + 4);
    if (id == ID_VIDEO) g_video = false;
    if (id != ID_MULTIJOGADOR) { GUEST_ORIGINAL(sub_821E4210)(ctx, base); return; }
    g_video = true;
    W32(ev + 4, ID_VIDEO);
    GUEST_ORIGINAL(sub_821E4210)(ctx, base);
    W32(ev + 4, ID_MULTIJOGADOR);
}

// ---------------------------------------------------------------- pagina P_Video
// pagina mostrada: limites das barras (video: 0..3 e 0..2; jogo: 0..20) e rotulos
GUEST_DECLARE_ORIGINAL(sub_821D82B8);
GUEST_FUNC(sub_821D82B8)
{
    const uint32_t pagina = Gpr32(ctx, 3);
    const uint32_t b1 = R32(pagina + 112), b2 = R32(pagina + 116);
    if (!g_video) DevolverRotulos(ctx);
    if (b1) Chamar(ctx, BARRA_LIMITES, b1, 0, g_video ? Quantos(RESOLUCAO) - 1 : 20);
    if (b2) Chamar(ctx, BARRA_LIMITES, b2, 0, g_video ? Quantos(SOMBRAS) - 1 : 20);
    GUEST_ORIGINAL(sub_821D82B8)(ctx, base);
    if (g_video) Ajudar(ctx, pagina, RESOLUCAO);   // o primeiro item (barra de cima) ja vem focado
}

// valores na tela
GUEST_DECLARE_ORIGINAL(sub_821D8300);
GUEST_FUNC(sub_821D8300)
{
    if (!g_video) { GUEST_ORIGINAL(sub_821D8300)(ctx, base); return; }
    EscreverPaginaVideo(ctx, Gpr32(ctx, 3));
}

// item focado: ajuda
GUEST_DECLARE_ORIGINAL(sub_821D8D28);
GUEST_FUNC(sub_821D8D28)
{
    const uint32_t pagina = Gpr32(ctx, 3), ev = Gpr32(ctx, 4);
    GUEST_ORIGINAL(sub_821D8D28)(ctx, base);
    if (g_video) Ajudar(ctx, pagina, LinhaDoId(R32(ev + 4)));
}

// A no item: proximo valor (volta ao primeiro depois do ultimo)
GUEST_DECLARE_ORIGINAL(sub_821D9210);
GUEST_FUNC(sub_821D9210)
{
    if (!g_video) { GUEST_ORIGINAL(sub_821D9210)(ctx, base); return; }
    const uint32_t pagina = Gpr32(ctx, 3);
    const int l = LinhaDoId(R32(Gpr32(ctx, 4) + 4));
    if (l >= 0)
    {
        Mudar((Linha)l, (Valor((Linha)l) + 1) % Quantos((Linha)l));
        EscreverPaginaVideo(ctx, pagina);
    }
    SetGpr(ctx, 3, 0);
}

// esquerda/direita no item
GUEST_DECLARE_ORIGINAL(sub_821D8D00);
GUEST_FUNC(sub_821D8D00)
{
    if (!g_video) { GUEST_ORIGINAL(sub_821D8D00)(ctx, base); return; }
    const uint32_t pagina = Gpr32(ctx, 3);
    const int l = LinhaDoId(R32(Gpr32(ctx, 4) + 4));
    const uint32_t tecla = R32(Gpr32(ctx, 5));
    if (l >= 0 && (tecla == 37 || tecla == 39))
    {
        Mudar((Linha)l, Valor((Linha)l) + (tecla == 39 ? 1 : -1));
        EscreverPaginaVideo(ctx, pagina);
    }
    SetGpr(ctx, 3, 0);
}

// ---------------------------------------------------------------- menu de pausa (pular_cena.cpp)
// Dentro da fase a pagina de OPCOES DE JOGO e outra (pacote CryInGame): o menu de pausa (P_StartMenu) guarda a sua em
// pausa+248 (sub_821AF940: B_Video -> R32(pausa+248)->[9](pagina, 1), se pausa+244 existir), da mesma classe da P_Video
// (mesma tabela de virtuais). O item escondido do menu de pausa abre ela no modo video; o voltar dela tira a pagina da
// pilha e o menu de pausa volta.
constexpr uint32_t VT_VIDEO = 0x8205C430;

uint32_t PaginaVideoDaPausa(uint32_t pausa) { return pausa && R32(pausa + 244) ? R32(pausa + 248) : 0; }

bool MenuVideoDisponivel(uint32_t pausa)
{
    const uint32_t p = PaginaVideoDaPausa(pausa);
    const bool ok = p && R32(p) == VT_VIDEO && R32(p + 80) && Conjunto(p);
    static uint32_t antes = 1;
    if (!ok && p != antes)
        Log("[video] pagina de video da pausa indisponivel: %08X vt %08X conjuntos %08X", p, p ? R32(p) : 0, p ? R32(p + 80) : 0);
    antes = ok ? 1 : p;
    return ok;
}

bool MenuVideoAbrir(GuestCpu& ctx, uint32_t pausa)
{
    if (!MenuVideoDisponivel(pausa)) return false;
    const uint32_t p = PaginaVideoDaPausa(pausa);
    g_video = true;
    GuestCpu salvo = ctx;
    SetGpr(ctx, 3, p); SetGpr(ctx, 4, 1);
    GuestCall(ctx, R32(VT_VIDEO + 9 * 4));
    ctx = salvo;
    Log("[video] OPCOES DE VIDEO aberta pelo menu de pausa (pagina %08X)", p);
    return true;
}

// menu de pausa aberto (devolver = true) ou montado numa fase nova (os widgets da fase anterior ja nao existem): se a
// pagina de video ficou no modo video (o menu fechou com START em cima dela), sai dele
void MenuVideoFechar(GuestCpu& ctx, bool devolver)
{
    if (!g_video && g_originais.empty()) return;
    if (devolver) DevolverRotulos(ctx); else g_originais.clear();
    g_video = false;
}

// ---------------------------------------------------------------- pausa > OPCOES > "OPCOES DE VIDEO"
// A pagina OPCOES da pausa (P_EditProfile do pacote CryInGame) so tem OPCOES DO CONTROLE (B_ControlSetup) e OPCOES DE
// JOGO (B_Video), sem item escondido. Na primeira vez que a lista dela e desenhada (MM_LISTA, menu_mouse.cpp, thread
// dos menus), o item OPCOES DE JOGO e copiado como um terceiro, embaixo dele, com o id do B_MultiplayerSetup:
//  - lista (+8/+12/+16 = vetor dos elementos da pagina, que o jogo desenha e o direcional percorre): vetor novo com o
//    item copiado no fim; vizinhos (+60 cima, +64 baixo) refeitos: CONTROLE <-> JOGO <-> VIDEO <-> CONTROLE.
//  - elemento (80 bytes, tabela 0x82078BC4): +4 id, +8 widget, +12 estados (animacoes de foco; dono = elemento),
//    +48..+56 vetor dos ouvintes (a pagina: o A no item chega a sub_821AF940 com o id do item).
//  - widget (128 bytes, tabela 0x820791C0): +24 posicao (x, y int16), wstring do texto em +60.
// Tudo com o alocador dos menus do jogo (sub_823ECDA8), que o jogo libera do jeito dele quando a pagina e desfeita.
// Os estados (+12) sao os do OPCOES DE JOGO (os mesmos objetos): no destrutor do item copiado o +12 e zerado antes,
// para nao serem liberados duas vezes. Os estados trazem a posicao do OPCOES DE JOGO: a altura do item copiado e
// refeita a cada desenho (a do OPCOES DE JOGO + o espaco entre os dois primeiros). A ajuda do item (embaixo) e
// escrita em pular_cena.cpp (sub_821AFD28, item focado do menu de pausa).
namespace
{
constexpr uint32_t ID_CONTROLE = 2003165906;
constexpr uint32_t VT_ELEMENTO = 0x82078BC4, VT_WIDGET_TEXTO = 0x820791C0;
constexpr uint32_t ALOCA_MENU = 0x823ECDA8;   // (r3 = 2872, r4 = 19, r5 = 196, r6 = tamanho, r7 = 0) -> memoria

struct ItemCopiado { uint32_t lista, controle, jogo, item, widget; };
std::vector<ItemCopiado> g_copiados;

// posicao do item copiado a cada desenho: a mesma distancia do OPCOES DE JOGO que ele tem do OPCOES DO CONTROLE
// (os itens entram animados: durante a entrada a copia anda junto); os estados copiados levariam para a do JOGO
void PosicionarCopia(const ItemCopiado& c)
{
    const uint32_t w1 = R32(c.controle + 8), w2 = R32(c.jogo + 8);
    if (!w1 || !w2 || c.jogo == c.item) return;
    W16(c.widget + 24, R16(w2 + 24));
    W16(c.widget + 26, (uint16_t)(2 * (int16_t)R16(w2 + 26) - (int16_t)R16(w1 + 26)));
}

uint32_t AlocarMenu(GuestCpu& ctx, uint32_t tam)
{
    GuestCpu salvo = ctx;
    SetGpr(ctx, 3, 2872); SetGpr(ctx, 4, 19); SetGpr(ctx, 5, 196); SetGpr(ctx, 6, tam); SetGpr(ctx, 7, 0);
    GuestCall(ctx, ALOCA_MENU);
    const uint32_t r = Gpr32(ctx, 3);
    ctx = salvo;
    return r;
}

void CopiarItemVideo(GuestCpu& ctx, uint32_t L)
{
    const uint32_t ini = R32(L + 8), fim = R32(L + 12);
    if (!ini || fim < ini || fim - ini > 32 * 4) return;
    uint32_t e1 = 0, e2 = 0;
    for (uint32_t p = ini; p < fim; p += 4)
    {
        const uint32_t e = R32(p);
        if (!e || R32(e) != VT_ELEMENTO) continue;
        const uint32_t id = R32(e + 4);
        if (id == ID_MULTIJOGADOR || id == 766419915u || id == 1011537169u) return;   // ja tem / menu do perfil
        if (id == ID_CONTROLE) e1 = e;
        if (id == ID_VIDEO) e2 = e;
    }
    if (!e1 || !e2) return;
    const uint32_t w1 = R32(e1 + 8), w2 = R32(e2 + 8);
    // o OPCOES DE JOGO sem foco, sem filhos e ligado so ao OPCOES DO CONTROLE (a copia sai como ele esta)
    if (!w1 || !w2 || R32(w2) != VT_WIDGET_TEXTO || R32(w2 + 92) || R32(w2 + 96) || R32(e2 + 16) || R32(L + 32) == e2 ||
        R32(e1 + 64) != e2 || R32(e2 + 60) != e1 || R32(e2 + 64) != e1)
        return;
    const uint32_t n = (fim - ini) / 4, o0 = R32(e2 + 48), o1 = R32(e2 + 52);
    const uint32_t no = o1 >= o0 && o1 - o0 <= 64 * 4 ? (o1 - o0) / 4 : 0;
    const uint32_t w = AlocarMenu(ctx, 128), e = AlocarMenu(ctx, 80), v = AlocarMenu(ctx, (n + 1) * 4),
                   ouv = AlocarMenu(ctx, std::max(no, 1u) * 4);
    if (!w || !e || !v || !ouv)
    {
        Log("[video] pausa > OPCOES: sem memoria para o item OPCOES DE VIDEO");
        return;
    }
    // widget: copia do OPCOES DE JOGO, com texto proprio, mais embaixo
    memcpy(G(w), G(w2), 128);
    W32(w + 4, w + 8);
    memset(G(w + 64), 0, 16); W32(w + 80, 0); W32(w + 84, 7);   // wstring vazia (sem buffer)
    EscreverTexto(ctx, w, TR("OPÇÕES DE VÍDEO", "VIDEO OPTIONS"));
    // elemento
    memcpy(G(e), G(e2), 80);
    W32(e + 4, ID_MULTIJOGADOR);
    W32(e + 8, w);
    for (uint32_t k = 0; k < no; k++) W32(ouv + k * 4, R32(o0 + k * 4));
    W32(e + 48, ouv); W32(e + 52, ouv + no * 4); W32(e + 56, ouv + no * 4);
    // vizinhos: CONTROLE <-> JOGO <-> VIDEO <-> CONTROLE
    W32(e2 + 64, e); W32(e + 60, e2); W32(e + 64, e1); W32(e1 + 60, e);
    // vetor da lista com o item novo no fim (o antigo fica para o jogo, sem uso)
    for (uint32_t k = 0; k < n; k++) W32(v + k * 4, R32(ini + k * 4));
    W32(v + n * 4, e);
    W32(L + 8, v); W32(L + 12, v + (n + 1) * 4); W32(L + 16, v + (n + 1) * 4);
    g_copiados.push_back({ L, e1, e2, e, w });
    PosicionarCopia(g_copiados.back());
    Log("[video] pausa > OPCOES: item OPCOES DE VIDEO %08X (widget %08X) copiado de %08X na lista %08X (%u ouvintes)", e, w, e2, L, no);
}
}

// MM_LISTA (menu_mouse.cpp): lista do menu desenhada (thread dos menus)
static void PosicionarSair();   // menu principal (mais abaixo)
void MenuVideoLista(GuestCpu& ctx, uint32_t L)
{
    if (!L) return;
    PosicionarSair();   // menu principal: SAIR DO JOGO na coluna dos outros itens
    for (auto& c : g_copiados)
        if (c.lista == L)
        {
            PosicionarCopia(c);
            return;
        }
    CopiarItemVideo(ctx, L);
}

// destrutor dos elementos: o item copiado nao libera os estados (sao os do OPCOES DE JOGO)
GUEST_DECLARE_ORIGINAL(sub_82478088);
GUEST_FUNC(sub_82478088)
{
    const uint32_t e = Gpr32(ctx, 3);
    for (size_t k = 0; k < g_copiados.size(); k++)
    {
        auto& c = g_copiados[k];
        if (c.item != e && c.jogo != e && c.controle != e) continue;
        W32(c.item + 12, 0);
        if (c.item == e) g_copiados.erase(g_copiados.begin() + k);
        else c.jogo = c.controle = c.item;   // os outros itens foram embora: a altura fica onde esta
        break;
    }
    GUEST_ORIGINAL(sub_82478088)(ctx, base);
}

// pausa > OPCOES: A num item. OPCOES DE JOGO abre a pagina normal (nunca no modo video); o item copiado abre o modo video
GUEST_DECLARE_ORIGINAL(sub_821AF940);
GUEST_FUNC(sub_821AF940)
{
    const uint32_t ev = Gpr32(ctx, 4);
    if (ev && R32(ev + 4) == ID_MULTIJOGADOR) { MenuVideoAbrir(ctx, Gpr32(ctx, 3)); return; }   // OPCOES DE VIDEO (item copiado)
    if (ev && R32(ev + 4) == ID_VIDEO && g_video) { g_video = false; DevolverRotulos(ctx); }
    GUEST_ORIGINAL(sub_821AF940)(ctx, base);
}

// voltar: sai do modo video
GUEST_DECLARE_ORIGINAL(sub_821D91B8);
GUEST_FUNC(sub_821D91B8)
{
    GUEST_ORIGINAL(sub_821D91B8)(ctx, base);
    if (g_video) { g_video = false; DevolverRotulos(ctx); }
}

// ---------------------------------------------------------------- menu principal (P_Main): "SAIR DO JOGO"
// A P_Main tem um item de depuracao que o jogo sempre esconde, "CHEATS (DEBUG)" (B_CheatsDebug): sub_821B0DE0 (montagem
// dos menus, r3 = pagina, guardada em 0x82911A2C) zera o bit 0x80 de elemento+24 (visivel) e o 0x40 de +76
// (escolhivel). Aqui ele volta com o nome "SAIR DO JOGO" e, escolhido (sub_821B1FD0 = A num item da P_Main, r4 = evento
// com o id em +4), fecha o jogo pelo mesmo caminho do fechar da janela (evento FCRecomp_Sair, gpu.cpp).
namespace
{
constexpr uint32_t PAGINA_MAIN = 0x82911A2C;
uint32_t g_sair = 0, g_sairAcima = 0;   // o item e o ultimo item visivel acima dele (MENU DE TRAPACAS ou CREDITOS)

// o item fica onde o jogo o pos no XML (fora da coluna): a cada desenho da lista vai para baixo do item de cima, com o
// mesmo espaco entre os dois itens de cima (os estados de animacao do item mexem na posicao)
}
static void PosicionarSair()
{
    if (!g_sair || R32(g_sair) != VT_ELEMENTO || !g_sairAcima || R32(g_sairAcima) != VT_ELEMENTO) return;
    const uint32_t antes = R32(g_sairAcima + 60);
    const uint32_t w = R32(g_sair + 8), wa = R32(g_sairAcima + 8), wb = antes ? R32(antes + 8) : 0;
    if (!w || !wa || !wb) return;
    W16(w + 24, R16(wa + 24));
    W16(w + 26, (uint16_t)(2 * (int16_t)R16(wa + 26) - (int16_t)R16(wb + 26)));
}
namespace
{

uint32_t ItemSair(GuestCpu& ctx, uint32_t pagina, uint32_t* irmao = nullptr)
{
    if (!pagina || !R32(pagina + 80)) return 0;
    const uint32_t conjunto = R32(R32(pagina + 80) + 4);
    if (irmao)
    {
        // ultimo item visivel: MENU DE TRAPACAS (B_Cheats) ou, sem ele, CREDITOS
        const uint32_t trapacas = Elemento(ctx, conjunto, IdDoNome(ctx, "B_Cheats"));
        *irmao = trapacas && (R8(trapacas + 24) & 0x80) ? trapacas : Elemento(ctx, conjunto, IdDoNome(ctx, "B_Credits"));
    }
    return Elemento(ctx, conjunto, IdDoNome(ctx, "B_CheatsDebug"));
}
}

GUEST_DECLARE_ORIGINAL(sub_821B0DE0);
GUEST_FUNC(sub_821B0DE0)
{
    GUEST_ORIGINAL(sub_821B0DE0)(ctx, base);
    GuestCpu salvo = ctx;
    uint32_t acima = 0;
    const uint32_t e = ItemSair(ctx, R32(PAGINA_MAIN), &acima);
    if (e && acima)
    {
        W8(e + 24, R8(e + 24) | 0x80);
        W8(e + 76, R8(acima + 76));   // escolhivel como o item de cima (sem o 0x40 o jogo desenha apagado)
        // direcional: ... -> item de cima <-> SAIR DO JOGO <-> o que vinha depois do item de cima (o primeiro)
        if (e != g_sair || R32(acima + 64) != e)
        {
            const uint32_t depois = R32(acima + 64);
            W32(acima + 64, e); W32(e + 60, acima);
            if (depois && depois != e) { W32(e + 64, depois); W32(depois + 60, e); }
        }
        g_sair = e; g_sairAcima = acima;
        PosicionarSair();
        if (const uint32_t w = R32(e + 8))
        {
            EscreverTexto(ctx, w, TR("SAIR DO JOGO", "QUIT GAME"));
            SetGpr(ctx, 3, w); SetGpr(ctx, 4, e);   // como em 821B1044: r3 = widget, r4 = elemento
            GuestCall(ctx, R32(R32(w) + 72));   // o mesmo que o jogo chama depois de mudar o estado do item
        }
        static bool uma = false;
        if (!uma) { uma = true; Log("[menu] SAIR DO JOGO no menu principal (item %08X)", e); }
    }
    ctx = salvo;
}

GUEST_DECLARE_ORIGINAL(sub_821B1FD0);
GUEST_FUNC(sub_821B1FD0)
{
    const uint32_t ev = Gpr32(ctx, 4);
    if (ev && R32(ev + 4) == IdDoNome(ctx, "B_CheatsDebug"))
    {
        Log("[menu] SAIR DO JOGO escolhido no menu principal");
        if (HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"FCRecomp_Sair")) { SetEvent(h); CloseHandle(h); }
        SetGpr(ctx, 3, 0);
        return;
    }
    GUEST_ORIGINAL(sub_821B1FD0)(ctx, base);
}
#endif

