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
    case RESOLUCAO: return std::clamp(o.escala, 0, 3);
    case SOMBRAS: return std::clamp(o.sombras, 0, 2);
    case SUAVIZACAO: return std::clamp(o.antisserrilhado, 0, 3);
    case FPS: return std::clamp(o.fps, 0, 2);
    default: return o.vsync ? 1 : 0;
    }
}

int Quantos(Linha l) { return l == RESOLUCAO || l == SUAVIZACAO ? 4 : l == VSYNC ? 2 : 3; }

std::string NomeValor(Linha l, int v)
{
    switch (l)
    {
    case RESOLUCAO: { static const char* n[] = { "1x (1280x720)", "1,5x (1920x1080)", "2x (2560x1440)", "3x (3840x2160)" };
                      static const char* e[] = { "1x (1280x720)", "1.5x (1920x1080)", "2x (2560x1440)", "3x (3840x2160)" }; return TR(n, e)[v]; }
    case SOMBRAS: { static const char* n[] = { "NORMAL", "ALTA", "MUITO ALTA" }, *e[] = { "NORMAL", "HIGH", "VERY HIGH" }; return TR(n, e)[v]; }
    case SUAVIZACAO: { static const char* n[] = { "DESLIGADA", "LEVE", "MÉDIA", "FORTE" }, *e[] = { "OFF", "LOW", "MEDIUM", "HIGH" }; return TR(n, e)[v]; }
    case FPS: { static const char* n[] = { "30", "60", "SEM LIMITE" }, *e[] = { "30", "60", "UNLOCKED" }; return TR(n, e)[v]; }
    default: return v ? TR("SIM", "YES") : TR("NÃO", "NO");
    }
}

const char* NomeLinha(Linha l)
{
    static const char* n[] = { "RESOLUÇÃO INTERNA", "SOMBRAS", "SUAVIZAÇÃO", "QUADROS POR SEGUNDO", "SINCRONIA VERTICAL" };
    static const char* e[] = { "INTERNAL RESOLUTION", "SHADOWS", "ANTI-ALIASING", "FRAMES PER SECOND", "VERTICAL SYNC" };
    return TR(n, e)[l];
}

const char* Ajuda(Linha l)
{
    static const char* n[] = {
        "Imagem mais nítida, mas pesa mais na placa de vídeo.",
        "Sombras com bordas mais definidas (mais pesadas).",
        "Suaviza as bordas serrilhadas da imagem.",
        "30 como o original, 60 mais fluido. Sem limite é experimental.",
        "Evita \"rasgos\" na imagem, mas pode baixar o FPS.",
    };
    static const char* e[] = {
        "Sharper image, but heavier on the graphics card.",
        "Shadows with sharper edges (heavier).",
        "Smooths the jagged edges of the image.",
        "30 like the original, 60 smoother. Unlocked is experimental.",
        "Avoids image \"tearing\", but may lower the FPS.",
    };
    return TR(n, e)[l];
}

void Mudar(Linha l, int v)
{
    Opcoes& o = OpcoesAtuais();
    v = std::clamp(v, 0, Quantos(l) - 1);
    if (v == Valor(l)) return;
    switch (l)
    {
    case RESOLUCAO: o.escala = v; xdraw::PedirNovaEscala(); break;
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

// voltar: sai do modo video
GUEST_DECLARE_ORIGINAL(sub_821D91B8);
GUEST_FUNC(sub_821D91B8)
{
    GUEST_ORIGINAL(sub_821D91B8)(ctx, base);
    if (g_video) { g_video = false; DevolverRotulos(ctx); }
}
#endif

