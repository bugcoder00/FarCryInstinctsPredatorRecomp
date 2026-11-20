// Opcoes graficas que mexem em variaveis do motor do jogo (CryEngine): distancia de visao e nivel de detalhe.
//
// sub_82319000 = console->RegisterFloat(console r3, nome r4, float* r5, padrao f1, flags r7, ajuda r8): devolve em f1 o
// valor que o chamador grava em *r5 (o motor le direto dessa variavel). Chamadas em sub_82602D?? (motor 3D):
// e_obj_lod_ratio, e_obj_view_dist_ratio, e_max_view_distance etc. (FCRECOMP_CVARLOG=1 lista todas no registro).
//
// Opcoes do launcher (opcoes.h): "Distancia de visao" multiplica e_obj_view_dist_ratio (120: ate onde objetos,
// arvores e personagens aparecem), e_obj_sprite_switch_dist_ratio (14,4: onde a vegetacao vira figura plana) e
// e_max_view_distance (0 = a da fase; se a fase der um limite, ele tambem cresce). "Detalhe de longe" multiplica
// e_obj_lod_ratio (20: onde os objetos trocam para o modelo simples). As duas tambem tem niveis abaixo do original
// (-1 = 0,75x, -2 = 0,5x: menos objetos e vegetacao de longe, mais leve). Um fio confere a cada 250 ms: se o jogo
// trocar o valor (fase, configuracao), a conta e refeita a partir do valor novo.
#ifndef PREDATOR_EDITOR
#include "guest.h"
#include "opcoes.h"
#include <windows.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace
{
    std::mutex g_mx;
    std::map<std::string, uint32_t> g_vars;   // nome -> endereco do float no jogo

    struct Ajuste { uint32_t var = 0; float base = 0, escrito = NAN; bool distancia = true; };
    std::map<std::string, Ajuste> g_ajustes;

    float Multiplicador(bool distancia)
    {
        static const float m[6] = { 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f };   // opcao -2..3
        const auto& o = OpcoesAtuais();
        return m[std::clamp(distancia ? o.distancia : o.detalhe, -2, 3) + 2];
    }
    float LerF(uint32_t a) { return std::bit_cast<float>(R32(a)); }

    void Conferir()
    {
        std::lock_guard l(g_mx);
        for (auto& [nome, a] : g_ajustes)
        {
            const float v = LerF(a.var);
            if (!(v == a.escrito)) a.base = v;   // primeira vez ou o jogo mudou: esse e o valor original
            const float novo = a.base * Multiplicador(a.distancia);
            if (novo != v)
            {
                W32(a.var, std::bit_cast<uint32_t>(novo));
                Log("[graficos] %s: %g -> %g", nome.c_str(), a.base, novo);
            }
            a.escrito = novo;
        }
    }

    void Acompanhar(const std::string& nome, uint32_t var)
    {
        bool distancia;
        if (nome == "e_obj_view_dist_ratio" || nome == "e_obj_sprite_switch_dist_ratio" || nome == "e_max_view_distance") distancia = true;
        else if (nome == "e_obj_lod_ratio") distancia = false;
        else return;
        if (!var) return;   // acompanha mesmo com 1x: a opcao pode mudar no jogo (janela F2, video_janela.cpp)
        {
            std::lock_guard l(g_mx);
            g_ajustes[nome] = Ajuste{ var, 0, NAN, distancia };
        }
        static std::once_flag fio;
        std::call_once(fio, [] { std::thread([] { for (;;) { Sleep(250); Conferir(); } }).detach(); });
    }

    // Valores fixos (testes): FCRECOMP_CVAR="nome=valor,nome=valor" (no carregar_bug.ps1 -Extra use virgula, o ";" separa
    // as variaveis de ambiente). Testado na praia (bug 10, 17/11/2026): e_max_shadow_map_size=2048, e_detail_objects=2 e
    // e_motion_blur=1 nao mudam nada na imagem (sobras da versao de PC); e_shadow_maps=1 so desenha um triangulo vermelho
    // de depuracao. Por isso nao viraram opcoes do launcher.
    // Aplicados na hora do registro (o valor devolvido ja e o novo: vale para o que o motor so le ao iniciar) e
    // mantidos pelo fio (se o jogo regravar).
    struct Fixo { uint32_t var = 0; bool inteiro = false; double valor = 0; };
    std::map<std::string, Fixo> g_fixos;

    bool ValorFixo(const std::string& nome, double& v)
    {
        static const std::map<std::string, double> amb = [] {
            std::map<std::string, double> m;
            const char* e = getenv("FCRECOMP_CVAR");
            std::string t = e ? e : "", item;
            for (size_t i = 0; i <= t.size(); i++)
            {
                if (i == t.size() || t[i] == ';' || t[i] == ',')
                {
                    const size_t ig = item.find('=');
                    if (ig != std::string::npos) m[item.substr(0, ig)] = atof(item.c_str() + ig + 1);
                    item.clear();
                }
                else item += t[i];
            }
            return m;
        }();
        if (auto it = amb.find(nome); it != amb.end()) { v = it->second; return true; }
        return false;
    }

    void ConferirFixos()
    {
        std::lock_guard l(g_mx);
        for (auto& [nome, f] : g_fixos)
        {
            const uint32_t bits = f.inteiro ? (uint32_t)(int32_t)f.valor : std::bit_cast<uint32_t>((float)f.valor);
            if (R32(f.var) != bits) W32(f.var, bits);
        }
    }

    void Fixar(const std::string& nome, uint32_t var, bool inteiro, double v)
    {
        {
            std::lock_guard l(g_mx);
            g_fixos[nome] = Fixo{ var, inteiro, v };
        }
        Log("[graficos] %s fixo em %g", nome.c_str(), v);
        static std::once_flag fio;
        std::call_once(fio, [] { std::thread([] { for (;;) { Sleep(250); ConferirFixos(); } }).detach(); });
    }

    std::string Texto(uint32_t a)
    {
        std::string s;
        for (int i = 0; a && i < 64; i++) { const char c = (char)*G(a + i); if (!c) break; s += c; }
        return s;
    }
}

GUEST_DECLARE_ORIGINAL(sub_82319000);
GUEST_FUNC(sub_82319000)
{
    const std::string nome = Texto(Gpr32(ctx, 4));
    const uint32_t ptr = Gpr32(ctx, 5);
    const double padrao = Fpr(ctx, 1);
    GUEST_ORIGINAL(sub_82319000)(ctx, base);
    double fixo;
    const bool temFixo = ptr && ValorFixo(nome, fixo);
    if (temFixo) { SetFpr(ctx, 1, (double)(float)fixo); Fixar(nome, ptr, false, fixo); }
    {
        std::lock_guard l(g_mx);
        g_vars[nome] = ptr;
    }
    static const bool log = getenv("FCRECOMP_CVARLOG") != nullptr;
    if (log) Log("[cvar] %s = %g (padrao %g, var %08X)", nome.c_str(), Fpr(ctx, 1), padrao, ptr);
    if (!temFixo) Acompanhar(nome, ptr);   // o chamador grava f1 em *r5 logo depois; o fio ajusta em seguida
}

// sub_82319278 = console->RegisterInt(console r3, nome r4, int* r5, padrao r6, flags r7, ajuda r8): devolve em r3
GUEST_DECLARE_ORIGINAL(sub_82319278);
GUEST_FUNC(sub_82319278)
{
    const std::string nome = Texto(Gpr32(ctx, 4));
    const uint32_t ptr = Gpr32(ctx, 5), padrao = Gpr32(ctx, 6);
    GUEST_ORIGINAL(sub_82319278)(ctx, base);
    double fixo;
    if (ptr && ValorFixo(nome, fixo)) { SetGpr(ctx, 3, (uint64_t)(int64_t)(int32_t)fixo); Fixar(nome, ptr, true, fixo); }
    {
        std::lock_guard l(g_mx);
        g_vars[nome] = ptr;
    }
    static const bool log = getenv("FCRECOMP_CVARLOG") != nullptr;
    if (log) Log("[cvar] %s = %d (padrao %d, var %08X, int)", nome.c_str(), (int)Gpr32(ctx, 3), (int)padrao, ptr);
}
#endif
