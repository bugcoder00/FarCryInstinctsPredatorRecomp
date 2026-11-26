// Giro do mouse no mesmo quadro: camera, bracos, arma e corpo (o "quadro de atraso" do motor sai).
//
// Ordem de um passo do jogo (medida com FCRECOMP_CAMTARDIALOG=2 e vigias de leitura; jogo / editor):
//   sub_8220D008 / sub_82136FF0 (passo do jogo):
//   1. atualiza os personagens (sub_82316838 de 8220D0F4 / sub_82313240 de 821370B4): animacao, arma, corpo e a
//      camera do personagem, que copia os angulos do jogador (sub_825C3748 de 8225DC1C / sub_8236E478 de 821638F8)
//   2. atualiza o jogador (sub_82146038 / sub_821D9030): a mira (sub_8228BC78 / sub_82257300) calcula o giro em
//      entrada+188/+192; o jogo soma o giro aos angulos (jogador+932 vertical, +940 horizontal, graus) com os limites
//      dele (sub_822744E0 -> sub_821360C0 / sub_8223F900); depois a camera do desenho e copiada da do passo 1 + desvio
//      do personagem (sub_825C3748 de 82146878 / sub_8236E478 de 821D9690)
// Como 1 vem antes de 2, tudo o que se ve usava os angulos do quadro anterior. Adiantar so a camera (versao anterior
// deste arquivo) deixava a arma e os bracos um quadro atras dela (escorregavam na tela contra o giro).
// Agora: no comeco de 1, o giro do mouse deste passo (as contagens que ja chegaram; input.cpp InputGiroAntecipado) e
// somado aos angulos do jogador; no fim da mira (2) essa soma e desfeita e o jogo aplica o giro inteiro como sempre,
// com os limites dele. Os personagens e as cameras do passo 1 ja usam os angulos novos.
// O corpo do personagem (objeto do jogador +4) tem uma copia dos angulos que o passo 2 escreve depois de aplicar o giro
// (+932/+940 montado num veiculo, +884/+892 a pe); a camera, os bracos e a arma do passo 1 leem dela, entao a soma vai
// nela tambem (sem ela a camera nao andava; so na camera do passo 4, a arma escorregava contra o giro).
// Conferencia: em cada passo com giro, os angulos depois da aplicacao do jogo tem de ter andado exatamente o giro
// (-x na horizontal, +y na vertical). Se nao andaram (limite vertical, arma montada, cena...), nao adianta ate um passo
// normal conferir de novo; e os angulos tem de estar como o jogo os deixou no fim do passo anterior.
// FCRECOMP_CAMTARDIA=0 desliga (para comparar). FCRECOMP_CAMTARDIALOG=1 anota os passos adiantados; =2 a ordem do
// passo (FCRECOMP_CAMSEQT=segundos depois da 1a mira, padrao 4; FCRECOMP_CAMSEQVIGIA=1 vigia quem le os angulos).
#include "guest.h"
#include "perf.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <string>

bool InputGiroAntecipado(float& gx, float& gy);   // input.cpp
void QuadroNaHoraEsperar();   // quadro_na_hora.cpp
void QuadroPassoComecou();
void QuadroPasso4();
void QuadroDesenhoComeco();
int64_t RoteiroTiqueAtual();   // input.cpp
void WatchAddWriteRange(uint32_t addr, uint32_t size);   // watch.cpp (depuracao)
void WatchAddReadRange(uint32_t addr, uint32_t size);

namespace
{
    float BitsFloat(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
    uint32_t FloatBits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
    float Graus180(float a) { while (a > 180.0f) a -= 360.0f; while (a < -180.0f) a += 360.0f; return a; }

    const bool g_ligado = !getenv("FCRECOMP_CAMTARDIA") || atoi(getenv("FCRECOMP_CAMTARDIA")) != 0;
    const int g_log = getenv("FCRECOMP_CAMTARDIALOG") ? atoi(getenv("FCRECOMP_CAMTARDIALOG")) : 0;

#ifdef PREDATOR_EDITOR
    constexpr uint32_t LR_PERSONAGENS = 0x821370B8u, LR_PASSO4 = 0x821D9694u, LR_DESENHO_INI = 0;   // editor: o desenho ja vem depois do passo
#define SUB_PERSONAGENS sub_82313240
#define CAM_ANGULOS sub_8236E478
#define APLICA_GIRO sub_82174B80
#else
    constexpr uint32_t LR_PERSONAGENS = 0x8220D0F8u, LR_PASSO4 = 0x8214687Cu, LR_DESENHO_INI = 0x825C585Cu;
#define SUB_PERSONAGENS sub_82316838
#define CAM_ANGULOS sub_825C3748
#define APLICA_GIRO sub_8226F5E8
#endif

    // tudo na thread do jogo
    thread_local uint32_t t_jogador = 0;          // jogador da ultima mira (entrada - 908)
    thread_local bool t_ok = true;                // a ultima conferencia passou
    thread_local bool t_conhecido = false;        // t_fimP/t_fimY valem (fim do passo anterior)
    thread_local uint32_t t_fimP = 0, t_fimY = 0; // angulos (bits) como o jogo os deixou no passo 4 anterior
    thread_local bool t_pre = false;              // adiantado neste passo (falta desfazer)
    thread_local float t_preGx = 0, t_preGy = 0;
    thread_local bool t_miraRodou = false;        // a mira rodou neste passo (vale a conferencia do passo 4)
    thread_local float t_restP = 0, t_restY = 0;  // angulos no fim da mira (sem o adiantamento)
    thread_local float t_gx = 0, t_gy = 0;        // giro final da mira

    double g_iniMira = 0;
    bool SeqLigada()   // =2: ordem do passo por 0,3 s a partir de FCRECOMP_CAMSEQT segundos depois da 1a mira
    {
        if (g_log != 2 || g_iniMira == 0) return false;
        static const double ini = getenv("FCRECOMP_CAMSEQT") ? atof(getenv("FCRECOMP_CAMSEQT")) * 1000 : 4000;
        const double t = perf::Now() - g_iniMira;
        return t > ini && t < ini + 300;
    }
    std::string Pilha(uint32_t sp)   // (declarada antes da marca)   // enderecos de retorno da pilha do Xbox (depuracao)
    {
        std::string t;
        for (int i = 0; i < 20 && sp; i++)
        {
            const uint32_t ant = R32(sp);
            if (!ant || ant <= sp) break;
            char c[12]; snprintf(c, sizeof(c), " %08X", R32(ant - 8)); t += c;
            sp = ant;
        }
        return t;
    }

    void Somar(uint32_t j, float dp, float dy)   // vertical += dp, horizontal += dy
    {
        W32(j + 932, FloatBits(BitsFloat(R32(j + 932)) + dp));
        W32(j + 940, FloatBits(BitsFloat(R32(j + 940)) + dy));
    }
    // copia dos angulos no corpo do personagem (jogador+4 -> entidade +932/+940): o passo 2 a reescreve depois da
    // aplicacao do giro; o passo 1 (camera do personagem, bracos, arma) le dela
    thread_local uint32_t t_ent = 0, t_entP = 0, t_entY = 0;   // angulos adiantados no corpo (endereco do vertical) e o que foi escrito
    thread_local uint32_t t_dono = 0;                           // objeto do jogador (r3 da aplicacao do giro; +4 = corpo)
    void Desfazer()
    {
        if (!t_pre) return;
        Somar(t_jogador, -t_preGy, t_preGx);
        if (t_ent && R32(t_ent) == t_entP && R32(t_ent + 8) == t_entY) Somar(t_ent - 932, -t_preGy, t_preGx);
        t_ent = 0;
        t_pre = false;
    }
}

// marca temporaria de tempo (depuracao do caminho do quadro)
void CamSeqMarca(const char* o, uint64_t n)
{
    if (SeqLigada()) Log("[camseq] %.2f ms %s %llu thread %lu", perf::Now(), o, (unsigned long long)n, GetCurrentThreadId());
}

// passo 1: comeco da atualizacao dos personagens
GUEST_DECLARE_ORIGINAL(SUB_PERSONAGENS);
GUEST_FUNC(SUB_PERSONAGENS)
{
    if ((uint32_t)Lr(ctx) == LR_PERSONAGENS) { QuadroNaHoraEsperar(); QuadroPassoComecou(); }   // le o mouse o mais perto possivel da troca
    if (g_ligado && (uint32_t)Lr(ctx) == LR_PERSONAGENS)
    {
        Desfazer();   // a mira do passo anterior nao rodou: tira o que ficou (as contagens continuam para a mira)
        // so com o jogador como o jogo o deixou no fim do passo anterior (mesmo objeto, nada mexeu nos angulos depois)
        const uint32_t j = t_jogador;
        if (j && t_ok && t_conhecido && R32(j + 932) == t_fimP && R32(j + 940) == t_fimY)
        {
            float gx, gy;
            if (InputGiroAntecipado(gx, gy) && (gx != 0 || gy != 0) && std::isfinite(gx) && std::isfinite(gy))
            {
                // a copia do corpo so se ainda for igual aos angulos do jogador (o passo 2 anterior a escreveu)
                const uint32_t ent = t_dono ? R32(t_dono + 4) : 0;
                // montado (veiculo): +932/+940 (angulos locais); a pe: +884/+892 (angulos do mundo)
                uint32_t copia = 0;
                if (ent && ent != j)
                    for (const uint32_t o : { 932u, 884u })
                        if (R32(ent + o) == t_fimP && R32(ent + o + 8) == t_fimY) { copia = ent + o; break; }
                Somar(j, gy, -gx);
                t_ent = 0;
                if (static uint32_t ultimo = 0; g_log == 1 && copia && copia - ent != ultimo)
                {
                    ultimo = copia - ent;
                    Log("[camtardia] corpo %s (copia dos angulos em +%u), tique do roteiro %lld", ultimo == 884 ? "a pe" : "montado", ultimo, (long long)RoteiroTiqueAtual());
                }
                if (copia)
                {
                    Somar(copia - 932, gy, -gx);
                    t_ent = copia; t_entP = R32(copia); t_entY = R32(copia + 8);
                }
                else if (g_log)
                    if (static int n = 0; n++ < 20) Log("[camtardia] corpo %08X sem a copia dos angulos (+932 %.4f %.4f, +884 %.4f %.4f; jogador %.4f %.4f)", ent, ent ? BitsFloat(R32(ent + 932)) : 0.f,
                        ent ? BitsFloat(R32(ent + 940)) : 0.f, ent ? BitsFloat(R32(ent + 884)) : 0.f, ent ? BitsFloat(R32(ent + 892)) : 0.f, BitsFloat(t_fimP), BitsFloat(t_fimY));
                t_pre = true; t_preGx = gx; t_preGy = gy;
                if (g_log == 1)
                    if (static int n = 0; n++ < 300) Log("[camtardia] giro adiantado %.4f %.4f", gx, gy);
            }
        }
        if (SeqLigada())
        {
            float gx = 0, gy = 0;
            CamSeqMarca("personagens", 0);
            Log("[camseq] personagens (adiantado %d %.4f %.4f; jogador %08X ok %d conhecido %d iguais %d %d) pilha%s", (int)t_pre, t_preGx, t_preGy, j, (int)t_ok,
                (int)t_conhecido, j ? (int)(R32(j + 932) == t_fimP) : -1, j ? (int)(R32(j + 940) == t_fimY) : -1, Pilha(Gpr32(ctx, 1)).c_str());
            (void)gx; (void)gy;
        }
        t_conhecido = false;
    }
    GUEST_ORIGINAL(SUB_PERSONAGENS)(ctx, base);
}

// input.cpp (fim do gancho da mira, giro final calculado; o jogo ainda vai soma-lo aos angulos)
void CameraTardiaGiro(uint32_t entrada)
{
    if (g_iniMira == 0) g_iniMira = perf::Now();
    if (t_jogador != entrada - 908) { t_pre = false; t_ok = true; t_dono = 0; t_ent = 0; }   // outro jogador (fase nova): nada a desfazer nele
    t_jogador = entrada - 908;
    Desfazer();
    t_restP = BitsFloat(R32(t_jogador + 932));
    t_restY = BitsFloat(R32(t_jogador + 940));
    t_gx = BitsFloat(R32(entrada + 188));
    t_gy = BitsFloat(R32(entrada + 192));
    t_miraRodou = true;
    if (static bool vigia = false; SeqLigada() && !vigia && getenv("FCRECOMP_CAMSEQVIGIA"))
    {
        vigia = true;   // quem le os angulos do jogador (jogador+932..+943) daqui em diante
        WatchAddReadRange(t_jogador + 932, 12);
    }
    if (SeqLigada())
        CamSeqMarca("mira", 0), Log("[camseq] fim da mira thread %lu jogador %08X ang %.4f %.4f giro %.4f %.4f", GetCurrentThreadId(), t_jogador,
            t_restP, t_restY, t_gx, t_gy);
}

// aplicacao do giro (r3 = objeto do jogador, r4 = entrada da mira): guarda o objeto para achar o corpo
GUEST_DECLARE_ORIGINAL(APLICA_GIRO);
GUEST_FUNC(APLICA_GIRO)
{
    if (t_jogador && Gpr32(ctx, 4) == t_jogador + 908) t_dono = Gpr32(ctx, 3);
    GUEST_ORIGINAL(APLICA_GIRO)(ctx, base);
}

// passo 4 (camera do desenho): o jogo ja aplicou o giro; confere e guarda os angulos do fim do passo
GUEST_DECLARE_ORIGINAL(CAM_ANGULOS);
GUEST_FUNC(CAM_ANGULOS)
{
    const uint32_t lr = (uint32_t)Lr(ctx), cam = Gpr32(ctx, 3), fonte = Gpr32(ctx, 4);
    if (LR_DESENHO_INI && lr == LR_DESENHO_INI) QuadroDesenhoComeco();   // thread de desenho: espera o passo atual
    GUEST_ORIGINAL(CAM_ANGULOS)(ctx, base);
    if (lr == LR_PASSO4) QuadroPasso4();
    if (static bool vigia = false; SeqLigada() && !vigia && getenv("FCRECOMP_CAMSEQFONTE") && lr == strtoul(getenv("FCRECOMP_CAMSEQFONTE"), nullptr, 16))
    {
        vigia = true;   // quem escreve os angulos que essa camera copia
        WatchAddWriteRange(fonte, 12);
    }
    if (SeqLigada())
        CamSeqMarca("camera", lr), Log("[camseq] camera de %08X thread %lu cam %08X fonte %08X (jogador%+d) ang %.4f %.4f %.4f pilha%s", lr, GetCurrentThreadId(), cam, fonte, (int)(fonte - t_jogador),
            BitsFloat(R32(cam + 16)), BitsFloat(R32(cam + 20)), BitsFloat(R32(cam + 24)), Pilha(Gpr32(ctx, 1)).c_str());
    if (!g_ligado || lr != LR_PASSO4 || !t_miraRodou || !t_jogador) return;
    t_miraRodou = false;
    const uint32_t j = t_jogador;
    if (std::fabs(t_gx) + std::fabs(t_gy) > 1e-4f)
    {
        const float dp = BitsFloat(R32(j + 932)) - t_restP, dy = Graus180(BitsFloat(R32(j + 940)) - t_restY);
        const bool ok = std::fabs(dy + t_gx) < 0.01f && std::fabs(dp - t_gy) < 0.01f;
        if (ok != t_ok && g_log == 1)
            if (static int n = 0; n++ < 100) Log("[camtardia] conferencia %s (giro %.4f %.4f, angulos andaram %.4f %.4f)", ok ? "voltou" : "falhou: sem adiantar", t_gx, t_gy, -dy, dp);
        t_ok = ok;
    }
    t_fimP = R32(j + 932); t_fimY = R32(j + 940); t_conhecido = true;
}
