// Campo de visao (FOV): opcao "Campo de visao" do launcher (opcoes.ini "fov" = graus a mais; 0 = o do jogo).
//
// A camera do jogo (CCamera) e atualizada por sub_825C37D8 (r3 = camera+4; fov em radianos em r3+60). Em jogo o fov e
// 95,4 graus; nos menus 90. Ela e atualizada por varios lugares; so mexemos nas duas cameras de quem joga:
//  - a do jogador (sub_82146038: chamadas em 82146588/82146590/821469CC/82146B98);
//  - a do jogo, copiada dela (Game obj+284: 820B47EC; sub_8227E008, que recebe o fov: 8227E054/8227E068).
// As outras (sombras 90 graus, cameras auxiliares de 45/80 graus) ficam como estao.
// O aumento e proporcional (tan(novo/2) = tan(fov/2) * k): binoculo e mira com zoom continuam com o mesmo zoom.
// O valor escrito fica anotado (aumentado -> original): a camera do jogo recebe o fov ja aumentado da do jogador e o
// jogo pode reaproveitar o valor da camera no quadro seguinte; um valor ja aumentado nunca e aumentado de novo.
// Teste: FCRECOMP_FOV=n (graus a mais). FCRECOMP_FOVLOG=1 anota as cameras atualizadas (a cada 2 s).
#ifndef PREDATOR_EDITOR
#include "guest.h"
#include "opcoes.h"
#include "perf.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>

namespace
{
constexpr float kFovJogo = 95.4f * 3.14159265f / 180.0f;   // fov do jogo em jogo (1,665 rad)

bool CameraDeQuemJoga(uint32_t lr)
{
    switch (lr)
    {
    case 0x8214658C: case 0x82146594: case 0x821469D0: case 0x82146B9C:   // camera do jogador (sub_82146038)
    case 0x820B47F0: case 0x8227E058: case 0x8227E06C:                     // camera do jogo
        return true;
    }
    return false;
}

void Diagnostico(GuestCpu& ctx)
{
    static std::mutex m;
    static std::map<std::tuple<uint32_t, uint32_t, uint32_t>, int> vistos;   // (camera, quem chamou, fov) -> vezes
    static double ultimo = 0;
    std::lock_guard l(m);
    const uint32_t cam = Gpr32(ctx, 3);
    vistos[{ cam, (uint32_t)Lr(ctx), R32(cam + 60) }]++;
    if (perf::Now() - ultimo > 2000)
    {
        ultimo = perf::Now();
        for (auto& [k, n] : vistos)
            Log("[fov] camera %08X chamada de %08X fov %.2f graus (%u) x%d | +56 %.3f +64 %.3f +68 %.3f", std::get<0>(k), std::get<1>(k),
                std::bit_cast<float>(std::get<2>(k)) * 180.0f / 3.14159265f, std::get<2>(k), n,
                std::bit_cast<float>(R32(std::get<0>(k) + 56)), std::bit_cast<float>(R32(std::get<0>(k) + 64)), std::bit_cast<float>(R32(std::get<0>(k) + 68)));
        vistos.clear();
    }
}
}

GUEST_DECLARE_ORIGINAL(sub_825C37D8);
GUEST_FUNC(sub_825C37D8)
{
    // a janela F2 muda o valor durante o jogo: o fov ja aumentado volta ao original pela tabela e e refeito com o novo
    const int extra = std::clamp(OpcoesAtuais().fov, 0, FOV_MAX);
    static std::unordered_map<uint32_t, uint32_t> feitos;   // fov aumentado -> original (bits do float)
    if ((extra > 0 || !feitos.empty()) && CameraDeQuemJoga((uint32_t)Lr(ctx)))
    {
        static std::mutex m;
        const uint32_t end = Gpr32(ctx, 3) + 60;
        const uint32_t bits = R32(end);
        std::lock_guard l(m);
        const auto it = feitos.find(bits);
        const uint32_t orig = it != feitos.end() ? it->second : bits;
        const float fov = std::bit_cast<float>(orig);
        if (fov > 0.01f && fov < 3.0f)
        {
            const float k = std::tan((kFovJogo + extra * 3.14159265f / 180.0f) * 0.5f) / std::tan(kFovJogo * 0.5f);
            const float novo = extra > 0 ? std::min(2.0f * std::atan(std::tan(fov * 0.5f) * k), 170.0f * 3.14159265f / 180.0f) : fov;
            const uint32_t nb = std::bit_cast<uint32_t>(novo);
            if (feitos.size() > 4096) feitos.clear();   // nao cresce sem fim (zoom continuo gera muitos valores)
            if (nb != orig) feitos[nb] = orig;
            if (nb != bits) W32(end, nb);
        }
    }
    static const bool diag = getenv("FCRECOMP_FOVLOG") != nullptr;
    if (diag) Diagnostico(ctx);
    GUEST_ORIGINAL(sub_825C37D8)(ctx, base);
}
#endif
