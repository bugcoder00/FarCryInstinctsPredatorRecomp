// Ponteiro do mouse nos menus do jogo.
// O jogo (feito para controle) nao tem ponteiro: os menus sao listas navegadas com o direcional. Aqui:
// - cada texto da interface desenhado no quadro (MM_TEXTO: r3 = objeto do texto, +4 = estilo com o retangulo em
//   +16/+18/+20/+22 = x0,x1,y0,y1 numa tela de 640x480 e a cor em +4) vira um retangulo na tela, somando a posicao do
//   pai (translacao da matriz do topo da pilha do desenhista MM_DESENHISTA: pilha = desenhista+4, matrizes em +8, topo
//   em +12);
// - menus do jogo (Magma): uma "lista" (MM_LISTA desenha; +8/+12 = inicio/fim do vetor de opcoes, +32 = opcao escolhida)
//   tem opcoes (+8 = a linha desenhada, MM_LINHA; +76 = marcas: 0x40 = pode escolher, 0x10 = escolhida; +60/64/68/72 =
//   vizinha de cima/baixo/esquerda/direita). O direcional (sub_824783A0) chama vtable[4] da opcao atual (perde a
//   marca), vtable[3] da vizinha (ganha) e MM_SINCRONIZA (lista+32 = a opcao com a marca). Cada texto desenhado dentro
//   de uma linha fica marcado com ela: o retangulo de cada opcao = os textos da sua linha. MenuMouseEscolher faz o mesmo
//   que o direcional, direto na opcao do mouse.
// - opcao com linhas ("Opcoes do controle": INVERTER ANALOGICO  SIM..., e a lista de valores que o A abre nela; o
//   SIM/NAO ou 1-10 e escolhido nessa lista): a opcao e uma so, desenhada por
//   MM_VALORES (objeto W = opcao+8; linhas de 64 bytes em W+136..W+140, linha escolhida = W+196 (16 bits), W+208 bit 0x40
//   = ligada). Cada linha da tela (textos na mesma altura) vira um item com sub = indice da linha; MM_VALORES_LINHA(W,
//   indice, 0, 1) escolhe a linha; depois, como o direcional dentro dela (sub_82470AD0), vtable[3] da opcao (o texto de
//   ajuda da tela acompanha), MM_VALORES_FIM1(W, -1) e MM_VALORES_FIM2(W, -1).
// - reserva (menus que nao passam por ai): os itens de uma lista sao o MESMO objeto de texto desenhado varias vezes (um
//   por linha) e o escolhido e o unico com cor diferente (branco; os outros cinza). Ai input.cpp anda com o direcional.
// FCRECOMP_UILOG=quadro: anota os textos desenhados nesse quadro e no 7o seguinte (investigacao).
// FCRECOMP_PONTEIROLOG=1: anota as listas reconhecidas quando mudam.
#include "guest.h"
#include "diag.h"
#include "opcoes.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>
#include <map>
#include <algorithm>
#include <windows.h>

#include "menu_mouse.h"
#include "icones_teclado.h"

// funcoes do menu em cada programa (o editor de mapas tem o mesmo codigo em outros enderecos)
// (achados pelas chamadas: a lista do editor sub_82310118 e a sincronizacao sub_8230F860 sao identicas as do jogo; a
// linha sub_824A7FA0 chama o desenho dos nos sub_8239ABD0; o texto sub_825DF150 e a opcao com linhas sub_82346DE8 tem as
// mesmas chamadas, na mesma ordem, que as do jogo). O desenho dos nos do editor (MM_NO) e sub_8239ABD0: la a foto do
// controle esta no "Mapa do controle" (pausa > CONTROLES).
#ifdef PREDATOR_EDITOR
#define MM_TEXTO sub_825DF150
#define MM_LISTA sub_82310118
#define MM_LINHA sub_824A7FA0
#define MM_VALORES sub_82346DE8
#define MM_SINC sub_8230F860
#define MM_NO sub_8239ABD0
constexpr uint32_t MM_OUVINTES = 0x82317D58;
constexpr uint32_t MM_DESENHISTA = 0x828F6ABC, MM_SINCRONIZA = 0x8230F860, MM_VALORES_END = 0x82346DE8, MM_VALORES_LINHA = 0x82345FD0,
                   MM_VALORES_FIM1 = 0x82346AA0, MM_VALORES_FIM2 = 0x823469C8;
#else
#define MM_TEXTO sub_8247D1A8
#define MM_LISTA sub_8246D2E8
#define MM_LINHA sub_8248B580
#define MM_VALORES sub_824700B8
#define MM_SINC sub_8246CC20
#define MM_NO sub_82479130
constexpr uint32_t MM_DESENHISTA = 0x82912C30, MM_SINCRONIZA = 0x8246CC20, MM_VALORES_END = 0x824700B8, MM_VALORES_LINHA = 0x82471970,
                   MM_VALORES_FIM1 = 0x82471BC8, MM_VALORES_FIM2 = 0x82471AF0;
// vtables dos objetos desenhados por MM_NO (no+8): pagina, texto, linha, opcao com linhas de valores
constexpr uint32_t MM_VT_PAGINA = 0x82078B70, MM_VT_TEXTO = 0x82078CC8, MM_VT_LINHA = 0x82073FD8, MM_VT_VALORES = 0x82078A40;
#endif

namespace
{
    struct Texto { int x0, y0, x1, y1; uint32_t cor, w; uint16_t c0; int ordem = 0; uint32_t linha = 0; };
    std::mutex g_mx;
    std::vector<Texto> g_atual;              // textos do quadro sendo desenhado
    std::vector<uint32_t> g_listasAtual;     // listas do menu desenhadas no quadro (ordem de desenho)
    MenuQuadro g_pronto;                     // ultimo quadro completo (listas montadas)
    uint64_t g_trocas = 0;
    std::atomic<uint64_t> g_edicaoUltima{ 0 };   // editor: ultimo quadro com o painel do modo de edicao (ferramenta + RT)
    std::atomic<DWORD> g_threadMenu{ 0 };    // thread do jogo que cuida dos menus (MM_SINCRONIZA; o desenho e em outra)
    std::map<uint32_t, int> g_desloc;        // opcao com linhas de valores: indice da 1a linha da tela (rolagem)
    struct ValPos { int tx, ty, ordem; };
    std::map<uint32_t, ValPos> g_valoresPos; // opcao com linhas desenhada neste quadro: translacao do pai, ordem de desenho
    thread_local std::vector<uint32_t> t_linhas;   // linhas sendo desenhadas (a de dentro por ultimo)

    bool Codigo(uint32_t a) { return a >= GUEST_CODE_BASE && a < GUEST_CODE_BASE + GUEST_CODE_SIZE && !(a & 3); }
    bool Memoria(uint32_t a) { return a >= 0x40000000 && a < 0xF0000000 && !(a & 3); }
    bool Objeto(uint32_t a) { return Memoria(a) && Memoria(a + 80) && R32(a) >= GUEST_IMAGE_BASE && R32(a) < GUEST_IMAGE_BASE + GUEST_IMAGE_SIZE; }

    std::string Texto16(uint32_t p, int max = 40)
    {
        std::string s;
        for (int i = 0; i < max; i++)
        {
            uint16_t c = R16(p + i * 2);
            if (!c) break;
            s += (c >= 32 && c < 127) ? (char)c : '?';
        }
        return s;
    }

    // opcoes de uma lista do menu (vetor em +8..+12); vazio se nao parecer uma lista
    std::vector<uint32_t> OpcoesDaLista(uint32_t L)
    {
        std::vector<uint32_t> r;
        if (!Objeto(L)) return r;
        const uint32_t ini = R32(L + 8), fim = R32(L + 12);
        if (!ini || !Memoria(ini) || fim < ini || fim - ini > 64 * 4) return r;
        for (uint32_t p = ini; p < fim; p += 4)
        {
            const uint32_t it = R32(p);
            if (!Objeto(it) || !Objeto(R32(it + 8))) return {};
            r.push_back(it);
        }
        return r;
    }

    // listas pelos objetos do menu: cada opcao que pode ser escolhida e tem texto desenhado na sua linha
    void MontarObjetos(MenuQuadro& q, const std::vector<Texto>& ts, const std::vector<uint32_t>& listas)
    {
        struct Achada { MenuLista l; int primeiro, ultimo; };
        std::vector<Achada> achadas;
        std::vector<uint32_t> vistas;
        for (uint32_t L : listas)
        {
            if (std::find(vistas.begin(), vistas.end(), L) != vistas.end()) continue;
            vistas.push_back(L);
            const uint32_t atual = R32(L + 32);
            if (!atual) continue;
            const std::vector<uint32_t> ops = OpcoesDaLista(L);
            Achada a{ {}, 1 << 30, -1 };
            a.l.lista = L;
            // so as opcoes que o direcional alcanca a partir da atual (seguindo as vizinhas; as que nao podem ser
            // escolhidas o direcional atravessa): o titulo da tela tambem e uma opcao, mas sem vizinhas; e com um
            // painel aberto (editor: "Criar um novo mapa") a lista de fora fica de fora ate voltar
            std::vector<uint32_t> alcancadas{ atual };
            for (size_t k = 0; k < alcancadas.size() && alcancadas.size() < 128; k++)
                for (int d = 0; d < 4; d++)
                {
                    const uint32_t v = R32(alcancadas[k] + 60 + d * 4);
                    if (v && std::find(ops.begin(), ops.end(), v) != ops.end() &&
                        std::find(alcancadas.begin(), alcancadas.end(), v) == alcancadas.end())
                        alcancadas.push_back(v);
                }
            for (uint32_t it : ops)
            {
                if (!(R8(it + 76) & 0x40)) continue;
                if (std::find(alcancadas.begin(), alcancadas.end(), it) == alcancadas.end()) continue;
                const uint32_t linha = R32(it + 8);
                if (MM_VALORES_END && R32(R32(linha) + 8) == MM_VALORES_END)
                {
                    // opcao com linhas de valores: textos agrupados por altura (uma linha da tela cada)
                    struct Lin { MenuItem m; uint32_t corEsq; int xEsq; };
                    std::vector<Lin> lins;
                    for (auto& t : ts)
                    {
                        if (t.linha != linha) continue;
                        Lin* achou = nullptr;
                        for (auto& x : lins)
                            if (t.y0 < x.m.y1 && t.y1 > x.m.y0) { achou = &x; break; }
                        if (!achou)
                        {
                            lins.push_back({ { t.x0, t.y0, t.x1, t.y1, 0, it }, t.cor, t.x0 });
                            achou = &lins.back();
                        }
                        Lin& x = *achou;
                        if (t.x0 <= x.xEsq) { x.xEsq = t.x0; x.corEsq = t.cor; }
                        x.m.x0 = std::min(x.m.x0, t.x0); x.m.y0 = std::min(x.m.y0, t.y0);
                        x.m.x1 = std::max(x.m.x1, t.x1); x.m.y1 = std::max(x.m.y1, t.y1);
                        a.primeiro = std::min(a.primeiro, t.ordem); a.ultimo = std::max(a.ultimo, t.ordem);
                    }
                    if (lins.empty())
                    {
                        // so imagens (grade de logos: uma linha da grade por opcao): o retangulo da opcao (W+172 x0,x1;
                        // W+176 y0,y1) dividido igualmente pelas celulas, na direcao mais comprida
                        auto pos = g_valoresPos.find(linha);
                        if (pos == g_valoresPos.end()) continue;
                        const int n = (int)((R32(linha + 140) - R32(linha + 136)) / 64), cur = (int16_t)R16(linha + 196);
                        const ValPos& vp = pos->second;
                        const int x0 = (int16_t)R16(linha + 172) + vp.tx, x1 = (int16_t)R16(linha + 174) + vp.tx;
                        const int y0 = (int16_t)R16(linha + 176) + vp.ty, y1 = (int16_t)R16(linha + 178) + vp.ty;
                        if (n <= 0 || n > 64 || x1 <= x0 || y1 <= y0) continue;
                        const bool deitada = x1 - x0 >= y1 - y0;
                        for (int k = 0; k < n; k++)
                        {
                            MenuItem m{ x0, y0, x1, y1, 0, it, k };
                            if (deitada) { m.x0 = x0 + (x1 - x0) * k / n; m.x1 = x0 + (x1 - x0) * (k + 1) / n; }
                            else { m.y0 = y0 + (y1 - y0) * k / n; m.y1 = y0 + (y1 - y0) * (k + 1) / n; }
                            if (it == atual && k == cur) a.l.escolhido = (int)a.l.itens.size();
                            a.l.itens.push_back(m);
                        }
                        a.primeiro = std::min(a.primeiro, vp.ordem); a.ultimo = std::max(a.ultimo, vp.ordem);
                        continue;
                    }
                    std::sort(lins.begin(), lins.end(), [](const Lin& x, const Lin& y) { return x.m.y0 < y.m.y0; });
                    const int n = (int)((R32(linha + 140) - R32(linha + 136)) / 64), cur = (int16_t)R16(linha + 196);
                    if (it == atual)
                    {
                        // a linha escolhida e a unica com outra cor no texto da esquerda: da a rolagem da lista
                        std::map<uint32_t, int> conta;
                        for (auto& x : lins) conta[x.corEsq]++;
                        if (lins.size() >= 2 && conta.size() == 2)
                            for (size_t v = 0; v < lins.size(); v++)
                                if (conta[lins[v].corEsq] == 1) g_desloc[linha] = cur - (int)v;
                    }
                    const int desloc = g_desloc.count(linha) ? g_desloc[linha] : 0;
                    for (size_t v = 0; v < lins.size(); v++)
                    {
                        const int sub = (int)v + desloc;
                        if (sub < 0 || sub >= n) continue;
                        if (it == atual && sub == cur) a.l.escolhido = (int)a.l.itens.size();
                        lins[v].m.sub = sub;
                        a.l.itens.push_back(lins[v].m);
                    }
                    continue;
                }
                MenuItem m{ 1 << 20, 1 << 20, -(1 << 20), -(1 << 20), 0, it };
                bool tem = false;
                for (auto& t : ts)
                    if (t.linha == linha)
                    {
                        m.x0 = std::min(m.x0, t.x0); m.y0 = std::min(m.y0, t.y0); m.x1 = std::max(m.x1, t.x1); m.y1 = std::max(m.y1, t.y1);
                        a.primeiro = std::min(a.primeiro, t.ordem); a.ultimo = std::max(a.ultimo, t.ordem);
                        tem = true;
                    }
                if (!tem) continue;
                if (it == atual) a.l.escolhido = (int)a.l.itens.size();
                a.l.itens.push_back(m);
            }
            if (a.l.escolhido >= 0) achadas.push_back(a);
        }
        // janela por cima de um menu (ex.: "OK / CANCELAR" sobre o menu de pausa): fica so o desenhado por ultimo
        int ultimoInicio = 0;
        for (auto& a : achadas) ultimoInicio = std::max(ultimoInicio, a.primeiro);
        for (auto& a : achadas)
            if (a.ultimo >= ultimoInicio) q.listas.push_back(a.l);
    }

    // reserva: textos agrupados pelo objeto; lista ativa = 2+ itens e exatamente um com cor diferente
    void MontarCores(MenuQuadro& q, const std::vector<Texto>& ts)
    {
        std::map<uint32_t, std::vector<const Texto*>> grupos;
        std::vector<std::pair<int, int>> faixas;   // ordem de desenho (primeiro, ultimo) de cada lista
        for (auto& t : ts) grupos[t.w].push_back(&t);
        for (auto& [w, its] : grupos)
        {
            if (its.size() < 2) continue;
            std::map<uint32_t, int> conta;
            for (auto* t : its) conta[t->cor]++;
            if (conta.size() != 2) continue;
            uint32_t unica = 0; bool achou = false;
            for (auto& [c, n] : conta) if (n == 1) { unica = c; achou = true; }
            if (!achou) continue;
            if (its.size() == 2)
            {
                // 2 itens: o escolhido e o mais claro (soma de R+G+B, depois alfa)
                auto brilho = [](uint32_t c) { return ((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255) + (c >> 24) / 4; };
                uint32_t outra = its[0]->cor == unica ? its[1]->cor : its[0]->cor;
                if (brilho(unica) <= brilho(outra)) unica = outra;
            }
            MenuLista l;
            faixas.push_back({ its.front()->ordem, its.back()->ordem });
            for (auto* t : its)
            {
                if (t->cor == unica) l.escolhido = (int)l.itens.size();
                l.itens.push_back({ t->x0, t->y0, t->x1, t->y1, 0 });
            }
            q.listas.push_back(l);
        }
        if (q.listas.size() > 1)
        {
            int ultimoInicio = 0;
            for (auto& f : faixas) ultimoInicio = std::max(ultimoInicio, f.first);
            std::vector<MenuLista> frente;
            for (size_t i = 0; i < q.listas.size(); i++)
                if (faixas[i].second >= ultimoInicio) frente.push_back(q.listas[i]);
            q.listas.swap(frente);
        }
    }

    MenuQuadro Montar(const std::vector<Texto>& ts, const std::vector<uint32_t>& listas)
    {
        MenuQuadro q;
        // rodape "(A) ESCOLHER" / "(B) VOLTAR": o 1o caractere e o desenho do botao na fonte do jogo (Omega = A, pi = B)
        for (auto& t : ts)
            if (t.c0 == 0x03A9 || t.c0 == 0x03C0)
            {
#ifdef PREDATOR_EDITOR
                // no editor o rodape da direita e preso na borda direita da tela de 853 (o texto vem em 640x480)
                const int d = t.x0 >= 320 ? 213 : 0;
#else
                const int d = 0;
#endif
                q.rodape.push_back({ t.x0 + d, t.y0, t.x1 + d, t.y1, t.c0 });
            }
        static const bool soCores = getenv("FCRECOMP_PONTEIROCORES") != nullptr;   // testes: so o jeito antigo
        if (!soCores) MontarObjetos(q, ts, listas);
        if (q.listas.empty()) MontarCores(q, ts);
        return q;
    }
}

// Chamado a cada troca de tela (VdSwap, input.cpp InputOnSwap): o quadro desenhado ate aqui fica pronto
void MenuMouseTroca()
{
    std::lock_guard lk(g_mx);
    g_pronto = Montar(g_atual, g_listasAtual);
    g_pronto.troca = ++g_trocas;
    static const bool plog = getenv("FCRECOMP_PONTEIROLOG") != nullptr;
    if (plog)
    {
        std::string d;
        for (auto& l : g_pronto.listas)
        {
            char b[128];
            snprintf(b, sizeof(b), " [%s %08X: %zu itens, escolhido %d, 1o %d,%d-%d,%d]", l.lista ? "menu" : "cor", l.lista, l.itens.size(),
                     l.escolhido, l.itens[0].x0, l.itens[0].y0, l.itens[0].x1, l.itens[0].y1);
            d += b;
            if (l.lista)
                for (auto& it : l.itens)
                {
                    snprintf(b, sizeof(b), " (%d,%d-%d,%d %08X/%d)", it.x0, it.y0, it.x1, it.y1, it.obj, it.sub);
                    d += b;
                }
        }
        for (auto& r : g_pronto.rodape) { char b[64]; snprintf(b, sizeof(b), " rodape %04X %d,%d", r.botao, r.x0, r.y0); d += b; }
        static std::string antes;
        if (d != antes)
        {
            Log("[ponteiro] quadro %llu:%s", (unsigned long long)g_trocas, d.c_str());
            antes = d;
        }
    }
    g_atual.clear();
    g_listasAtual.clear();
    g_valoresPos.clear();
}

bool MenuMouseEditorEdicao()
{
    std::lock_guard lk(g_mx);
    const uint64_t u = g_edicaoUltima.load();
    return u && g_trocas - u <= 15;
}

MenuQuadro MenuMouseQuadro()
{
    std::lock_guard lk(g_mx);
    return g_pronto;
}

bool MenuMouseEscolher(GuestCpu& ctx, uint32_t L, uint32_t alvo, int sub)
{
    static const bool plog = getenv("FCRECOMP_PONTEIROLOG") != nullptr;
    if (!MM_SINCRONIZA || GetCurrentThreadId() != g_threadMenu.load())   // so na thread que cuida dos menus
    {
        if (plog) Log("[ponteiro] escolher direto: thread %lu, a dos menus e %lu", GetCurrentThreadId(), g_threadMenu.load());
        return false;
    }
    const std::vector<uint32_t> ops = OpcoesDaLista(L);
    if (std::find(ops.begin(), ops.end(), alvo) == ops.end() || !(R8(alvo + 76) & 0x40)) return false;
    const uint32_t atual = R32(L + 32);
    const GuestCpu salvo = ctx;
    if (atual != alvo)
    {
        if (!atual || std::find(ops.begin(), ops.end(), atual) == ops.end()) return false;
        const uint32_t perde = R32(R32(atual) + 16), ganha = R32(R32(alvo) + 12);
        if (!Codigo(perde) || !Codigo(ganha)) return false;
        // o mesmo que o direcional (sub_824783A0 + MM_SINCRONIZA): a atual perde a marca, a nova ganha
        SetGpr(ctx, 3, atual); GuestCall(ctx, perde);
        ctx = salvo; SetGpr(ctx, 3, alvo); GuestCall(ctx, ganha);
        ctx = salvo; SetGpr(ctx, 3, L); GuestCall(ctx, MM_SINCRONIZA);
        ctx = salvo;
        const bool ok = R32(L + 32) == alvo;
        if (plog) Log("[ponteiro] escolhe direto %08X -> %08X na lista %08X: %s", atual, alvo, L, ok ? "ok" : "falhou");
        if (!ok) return false;
#ifdef PREDATOR_EDITOR
        // Editor: o texto de ajuda (embaixo) so muda quando a opcao recebe o evento do botao: no direcional o gerente da
        // tela (sub_82191258) manda o evento (codigo 40 = baixo, 38 = cima) para a lista (sub_8230FBA0) -> linha escolhida
        // -> MM_OUVINTES(opcao, evento), que passa para os "ouvintes" da opcao (um deles troca o texto). Aqui o mesmo
        // evento vai direto para os ouvintes da nova opcao (o evento fica na pilha do jogo, abaixo da atual).
        {
            const auto ia = std::find(ops.begin(), ops.end(), atual), ib = std::find(ops.begin(), ops.end(), alvo);
            const uint32_t ev = (Gpr32(ctx, 1) - 0x400) & ~15u;
            W32(ev, ib > ia ? 40 : 38); W16(ev + 4, 0); W32(ev + 8, 0);
            SetGpr(ctx, 1, ev - 0x200); SetGpr(ctx, 3, alvo); SetGpr(ctx, 4, ev);
            GuestCall(ctx, MM_OUVINTES);
            ctx = salvo;
        }
#endif
    }
    if (sub < 0) return true;
    // linha de uma opcao com valores
    const uint32_t W = R32(alvo + 8);
    if (!MM_VALORES_LINHA || R32(R32(W) + 8) != MM_VALORES_END || !(R8(W + 208) & 0x40)) return false;
    const int n = (int)((R32(W + 140) - R32(W + 136)) / 64), antes = (int16_t)R16(W + 196);
    if (sub >= n) return false;
    if (antes != sub)
    {
        SetGpr(ctx, 3, W); SetGpr(ctx, 4, (uint64_t)(int64_t)sub); SetGpr(ctx, 5, 0); SetGpr(ctx, 6, 1);
        GuestCall(ctx, MM_VALORES_LINHA);
        ctx = salvo;
        if ((int16_t)R16(W + 196) == sub)
        {
            const uint32_t ganha = R32(R32(alvo) + 12);
            if (Codigo(ganha)) { SetGpr(ctx, 3, alvo); GuestCall(ctx, ganha); ctx = salvo; }
            SetGpr(ctx, 3, W); SetGpr(ctx, 4, (uint64_t)(int64_t)-1); GuestCall(ctx, MM_VALORES_FIM1); ctx = salvo;
            SetGpr(ctx, 3, W); SetGpr(ctx, 4, (uint64_t)(int64_t)-1); GuestCall(ctx, MM_VALORES_FIM2); ctx = salvo;
        }
    }
    const bool ok = (int16_t)R16(W + 196) == sub;
    if (plog && antes != sub) Log("[ponteiro] linha %d -> %d da opcao %08X: %s", antes, sub, alvo, ok ? "ok" : "falhou");
    return ok;
}

// Cursor (0..1 na area da janela) -> coordenadas da interface do jogo (640x480)
void MenuMouseParaJogo(float nx, float ny, float cw, float ch, float& ux, float& uy)
{
    const Opcoes& op = OpcoesAtuais();
    float ax = nx, ay = ny;
    static const bool barras = op.formato == 2 || getenv("FCRECOMP_BARRAS") != nullptr;
    if (barras && cw > 0 && ch > 0)
    {
        // imagem 16:9 no maior retangulo que cabe, centralizado (render.cpp)
        const float alvo = 16.0f / 9.0f, janela = cw / ch;
        if (janela > alvo) { float frac = alvo / janela; ax = 0.5f + (nx - 0.5f) / frac; }
        else { float frac = janela / alvo; ay = 0.5f + (ny - 0.5f) / frac; }
    }
    else
    {
        // ultrawide: a interface (2D) e apertada para o centro na proporcao (16/9)/aspecto (xenos, hudEsc)
        const float a = AspectoTela();
        if (a > 16.0f / 9.0f + 0.01f) ax = 0.5f + (nx - 0.5f) / ((16.0f / 9.0f) / a);
    }
#ifdef PREDATOR_EDITOR
    ux = ax * 480.0f * 16.0f / 9.0f;   // o editor monta a interface numa tela 16:9 de 853x480 (o jogo, 640x480 esticada)
#else
    ux = ax * 640.0f;
#endif
    uy = ay * 480.0f;
}

// lista do menu: anota que foi desenhada neste quadro
GUEST_DECLARE_ORIGINAL(MM_LISTA);
GUEST_FUNC(MM_LISTA)
{
    {
        std::lock_guard lk(g_mx);
        if (g_listasAtual.size() < 64) g_listasAtual.push_back(Gpr32(ctx, 3));
    }
    GUEST_ORIGINAL(MM_LISTA)(ctx, base);
}

// linha de uma lista: os textos desenhados dentro dela ficam marcados com ela
GUEST_DECLARE_ORIGINAL(MM_LINHA);
GUEST_FUNC(MM_LINHA)
{
    const size_t n = t_linhas.size();
    t_linhas.push_back(Gpr32(ctx, 3));
    GUEST_ORIGINAL(MM_LINHA)(ctx, base);
    t_linhas.resize(n);
}

// lista+32 = a opcao com a marca (depois de cada botao): anota a thread que cuida dos menus
GUEST_DECLARE_ORIGINAL(MM_SINC);
GUEST_FUNC(MM_SINC)
{
    g_threadMenu = GetCurrentThreadId();
    GUEST_ORIGINAL(MM_SINC)(ctx, base);
}

// opcao com linhas de valores: os textos dela ficam marcados com ela
GUEST_DECLARE_ORIGINAL(MM_VALORES);
GUEST_FUNC(MM_VALORES)
{
    const uint32_t ms = R32(R32(MM_DESENHISTA) + 4);
    if (ms)
    {
        const uint32_t top = R32(ms + 8) + R32(ms + 12) * 64;
        uint32_t ux = R32(top + 48), uy = R32(top + 52);
        float tx, ty; memcpy(&tx, &ux, 4); memcpy(&ty, &uy, 4);
        std::lock_guard lk(g_mx);
        g_valoresPos[Gpr32(ctx, 3)] = { (int)tx, (int)ty, (int)g_atual.size() };
    }
    const size_t n = t_linhas.size();
    t_linhas.push_back(Gpr32(ctx, 3));
    GUEST_ORIGINAL(MM_VALORES)(ctx, base);
    t_linhas.resize(n);
}

// Tela "Configuracao do controle" com teclado e mouse (a foto do controle vira um desenho do teclado, icones_teclado.cpp):
// cada pagina ("Botoes", "Analogicos") e um no MM_VT_PAGINA com os textos (MM_VT_TEXTO) e as linhas (MM_VT_LINHA) que
// apontam os botoes do controle; a foto e outro no. Enquanto o desenho do teclado esta na tela, os textos e linhas das
// paginas com linhas e o titulo das paginas com as setas (MM_VT_VALORES na raiz) nao sao desenhados.
#ifdef PREDATOR_EDITOR
std::string InputTeclasTexto(const char* acao, int max);   // input.cpp
namespace
{
// Mapa do controle do editor (pausa > CONTROLES) com o desenho do teclado: cada rotulo (objeto vt 8200CAC0, frase UTF-16
// em obj+0x40, um rotulo por botao do controle) ganha no fim o icone do botao, que com teclado ja e a tecla desenhada
// ("TESTAR MAPA: [TAB]"). As frases sao as do
// common_eddfad3a.xml (secoes ControllerMap_*), em ingles e na traducao; bi_White = LB, bi_black = RB.
constexpr uint32_t ED_VT_ROTULO = 0x8200CAC0, ED_VT_LINHA = 0x8201BD10;
const char* BotaoDoRotulo(const std::string& t)
{
    static const std::map<std::string, const char*> tabela = {
        { "EDITING OPTIONS", "RB" }, { "OPÇÕES DE EDIÇÃO", "RB" },
        { "MOVE CAMERA (CLICK TO TILT CAMERA)", "LS" }, { "MOVER CÂMERA (CLIQUE PARA INCLINAR)", "LS" },
        { "TOOL SELECTOR", "RT" }, { "ESCOLHER FERRAMENTA", "RT" },
        { "ROTATE CAMERA", "RS" }, { "GIRAR CÂMERA", "RS" },
        { "TEST MAP", "BACK" }, { "TESTAR MAPA", "BACK" },
        { "START MENU", "START" }, { "MENU START", "START" },
        { "BUMP", "Y" }, { "RELEVO", "Y" }, { "RAISE", "Y" }, { "ELEVAR", "Y" }, { "EXTEND", "Y" }, { "ESTENDER", "Y" },
        { "ERASE", "Y" }, { "APAGAR", "Y" },
        { "TRIM", "X" }, { "APARAR", "X" }, { "SMOOTH", "B" }, { "SUAVIZAR", "B" },
        { "DIG", "A" }, { "CAVAR", "A" }, { "LOWER", "A" }, { "BAIXAR", "A" }, { "SET HEIGHT", "A" }, { "DEFINIR ALTURA", "A" },
        { "PAINT", "A" }, { "PINTAR", "A" },
        { "CHANGE CURSOR STYLE", "LB" }, { "MUDAR ESTILO DO CURSOR", "LB" },
        { "TERRAIN TOOL SELECTOR", "LT" }, { "ESCOLHER FERRAMENTA DE TERRENO", "LT" },
        { "SWITCH TO EDIT OBJECT TOOL", "Y" }, { "MUDAR PARA EDITAR OBJETO", "Y" },
        { "SWITCH TO ADD OBJECT TOOL", "Y" }, { "MUDAR PARA ADICIONAR OBJETO", "Y" },
        { "SINK/RAISE OBJECT", "X" }, { "AFUNDAR/ELEVAR OBJETO", "X" },
        { "ROTATE OBJECT", "B" }, { "GIRAR OBJETO", "B" },
        { "INSERT OBJECT", "A" }, { "INSERIR OBJETO", "A" }, { "MOVE OBJECT", "A" }, { "MOVER OBJETO", "A" },
        { "DELETE SELECTED OBJECT", "LB" }, { "APAGAR OBJETO SELECIONADO", "LB" },
    };
    auto it = tabela.find(t);
    if (it != tabela.end()) return it->second;
    // frases do direcional: "ESQUERDA-DIREITA: ...", "CIMA-BAIXO: ...", "LEFT-RIGHT: ...", "UP-DOWN: ..."
    if (t.rfind("ESQUERDA-DIREITA", 0) == 0 || t.rfind("CIMA-BAIXO", 0) == 0 || t.rfind("LEFT-RIGHT", 0) == 0 || t.rfind("UP-DOWN", 0) == 0)
        return "DPAD";
    return nullptr;
}

// caractere de icone da fonte (o mesmo dos codigos {A}, {B}... dos textos, icones_teclado.cpp), em UTF-8; com teclado e
// mouse o desenho do icone ja e a tecla configurada
std::string Glifo(uint16_t u)
{
    std::string r;
    if (u < 0x800) { r += (char)(0xC0 | (u >> 6)); r += (char)(0x80 | (u & 0x3F)); }
    else { r += (char)(0xE0 | (u >> 12)); r += (char)(0x80 | ((u >> 6) & 0x3F)); r += (char)(0x80 | (u & 0x3F)); }
    return r;
}

std::string TeclasDoBotao(const char* b, const std::string& t)
{
    static const std::map<std::string, uint16_t> icones = {
        { "A", 0x03A9 }, { "B", 0x03C0 }, { "X", 0x2202 }, { "Y", 0x2206 }, { "LB", 0x220F }, { "RB", 0x2211 },
        { "START", 0x00A2 }, { "BACK", 0x00A3 }, { "LT", 0x2248 }, { "RT", 0x00FE },
    };
    if (auto it = icones.find(b); it != icones.end()) return Glifo(it->second);
    auto primeira = [](const char* acao) { return InputTeclasTexto(acao, 1); };
    if (!strcmp(b, "RS")) return "MOUSE";
    if (!strcmp(b, "LS"))
    {
        std::string r = primeira("ANDAR_FRENTE") + "/" + primeira("ANDAR_ESQUERDA") + "/" + primeira("ANDAR_TRAS") + "/" + primeira("ANDAR_DIREITA");
        if (t.find("CLIQUE") != std::string::npos || t.find("CLICK") != std::string::npos) r += " + " + Glifo(0x2022);
        return r;
    }
    if (!strcmp(b, "DPAD"))
    {
        const bool lados = t.find("ESQUERDA-DIREITA") != std::string::npos || t.find("LEFT-RIGHT") != std::string::npos;
        const bool cima = t.find("CIMA-BAIXO") != std::string::npos || t.find("UP-DOWN") != std::string::npos;
        std::string r;
        if (cima) r = Glifo(0x222B) + Glifo(0x00AA);
        if (lados) r += Glifo(0x00BA) + Glifo(0x00F8);
        return r;
    }
    return InputTeclasTexto(b, 2);
}

// A frase do rotulo e uma wstring em obj+0x3C (+4 = ponteiro dos caracteres, +0x14 = tamanho, +0x18 = capacidade); o
// desenho do texto (sub_825DF150) copia ela inteira a cada quadro. Enquanto o rotulo e desenhado, a wstring aponta para
// "FRASE: TECLAS" (memoria nossa) e depois volta ao que era.
struct FraseSalva { uint32_t ptr = 0, tam = 0, cap = 0; };
FraseSalva RotuloComTeclas(uint32_t obj)
{
    const uint32_t p = R32(obj + 0x40), tam = R32(obj + 0x50), cap = R32(obj + 0x54);
    if (p < 0xA0000000 || p >= 0xB0000000 || cap < 8 || tam == 0 || tam > cap || tam > 120) return {};   // frase na memoria do jogo
    std::string t;
    for (uint32_t i = 0; i < tam; i++)
    {
        const uint16_t c = R16(p + i * 2);
        if (!c) break;
        if (c < 0x80) t += (char)c;
        else if (c < 0x800) { t += (char)(0xC0 | (c >> 6)); t += (char)(0x80 | (c & 0x3F)); }
        else { t += (char)(0xE0 | (c >> 12)); t += (char)(0x80 | ((c >> 6) & 0x3F)); t += (char)(0x80 | (c & 0x3F)); }
    }
    const char* b = BotaoDoRotulo(t);
    if (!b) return {};
    const std::string teclas = TeclasDoBotao(b, t);
    if (teclas.empty()) return {};
    static std::map<uint32_t, uint32_t> buffers;   // rotulo -> texto novo na memoria do jogo
    uint32_t& buf = buffers[obj];
    if (!buf) buf = GuestAlloc(512);
    if (!buf) return {};
    // UTF-8 -> UTF-16 (big-endian na memoria do jogo)
    std::string novo = t + ": " + teclas;
    uint32_t n = 0;
    for (size_t i = 0; i < novo.size() && n < 250;)
    {
        unsigned char c = (unsigned char)novo[i];
        uint16_t u;
        if (c < 0x80) { u = c; i += 1; }
        else if ((c >> 5) == 6 && i + 1 < novo.size()) { u = (uint16_t)(((c & 0x1F) << 6) | (novo[i + 1] & 0x3F)); i += 2; }
        else if (i + 2 < novo.size()) { u = (uint16_t)(((c & 0x0F) << 12) | ((novo[i + 1] & 0x3F) << 6) | (novo[i + 2] & 0x3F)); i += 3; }
        else break;
        W16(buf + n * 2, u); n++;
    }
    W16(buf + n * 2, 0);
    W32(obj + 0x40, buf); W32(obj + 0x50, n); W32(obj + 0x54, 255);
    return { p, tam, cap };
}
}
#endif

#ifdef MM_NO
GUEST_DECLARE_ORIGINAL(MM_NO);
GUEST_FUNC(MM_NO)
{
    static std::vector<uint32_t> nos;                 // nos sendo desenhados (o de dentro por ultimo)
    const uint32_t no = Gpr32(ctx, 3), obj = R32(no + 8), vt = R32(obj);
#ifdef PREDATOR_EDITOR
    // editor: com o desenho do teclado, as linhas (vt 8201BD10) somem e cada rotulo ganha as teclas
    FraseSalva fraseOriginal;
    if (ControleTecladoNaTela())
    {
        if (vt == ED_VT_LINHA) return;
        if (vt == ED_VT_ROTULO) fraseOriginal = RotuloComTeclas(obj);
    }
    nos.push_back(no);
    GUEST_ORIGINAL(MM_NO)(ctx, base);
    nos.pop_back();
    if (fraseOriginal.ptr) { W32(obj + 0x40, fraseOriginal.ptr); W32(obj + 0x50, fraseOriginal.tam); W32(obj + 0x54, fraseOriginal.cap); }
#else
    static std::map<uint32_t, ULONGLONG> comLinhas;   // pagina -> ultima vez que desenhou uma linha
    static ULONGLONG ultimaPagina = 0;
    const uint32_t pai = nos.empty() ? 0 : nos.back();
    const ULONGLONG agora = GetTickCount64();
    bool pular = false;
    if (pai && vt == MM_VT_LINHA && R32(R32(pai + 8)) == MM_VT_PAGINA)
    {
        if (comLinhas.size() > 64) comLinhas.clear();
        comLinhas[pai] = agora;
        ultimaPagina = agora;
    }
    if (ControleTecladoNaTela())
    {
        if (pai && (vt == MM_VT_TEXTO || vt == MM_VT_LINHA))
        {
            auto it = comLinhas.find(pai);
            pular = it != comLinhas.end() && agora - it->second < 300;
        }
        else if (!pai && vt == MM_VT_VALORES) pular = agora - ultimaPagina < 300;
    }
    if (pular) return;
    nos.push_back(no);
    GUEST_ORIGINAL(MM_NO)(ctx, base);
    nos.pop_back();
#endif
}
#endif

GUEST_DECLARE_ORIGINAL(MM_TEXTO);
GUEST_FUNC(MM_TEXTO)
{
    const uint32_t w = Gpr32(ctx, 3), l = R32(w + 4);
    const uint32_t ms = R32(R32(MM_DESENHISTA) + 4);
    if (l && ms)
    {
        const uint32_t top = R32(ms + 8) + R32(ms + 12) * 64;
        uint32_t ux = R32(top + 48), uy = R32(top + 52);
        float tx, ty; memcpy(&tx, &ux, 4); memcpy(&ty, &uy, 4);
        const uint32_t cap = R32(w + 12 + 24), sp = cap >= 8 ? R32(w + 16) : w + 16;
        Texto t{ (int)tx + (int16_t)R16(l + 16), (int)ty + (int16_t)R16(l + 20), (int)tx + (int16_t)R16(l + 18),
                 (int)ty + (int16_t)R16(l + 22), R32(l + 4), w, R16(sp) };
        t.linha = t_linhas.empty() ? 0 : t_linhas.back();
        // texto invisivel (alfa 0) nao conta
        if ((t.cor >> 24) != 0 && t.x1 > t.x0 && t.y1 > t.y0)
        {
            std::lock_guard lk(g_mx);
            t.ordem = (int)g_atual.size();
            if (g_atual.size() < 512) g_atual.push_back(t);
#ifdef PREDATOR_EDITOR
            // modo de edicao do editor: o nome da ferramenta no alto termina no desenho do RT (caractere 0x00FE da fonte)
            for (int i = 0; i < 64; i++) { const uint16_t c = R16(sp + i * 2); if (!c) break; if (c == 0x00FE) { g_edicaoUltima = g_trocas; break; } }
#endif
        }
        static const char* env = getenv("FCRECOMP_UILOG");
        if (env)
        {
            const uint64_t q = strtoull(env, nullptr, 10), f = diag::CurrentFrame();
            if (f == q || f == q + 7)
                Log("[ui] q%llu w=%08X '%s' c0=%04X ret %d,%d-%d,%d cor %08X linha %08X", (unsigned long long)f, w, Texto16(sp, 20).c_str(),
                    t.c0, t.x0, t.y0, t.x1, t.y1, t.cor, t.linha);
        }
    }
    GUEST_ORIGINAL(MM_TEXTO)(ctx, base);
}

