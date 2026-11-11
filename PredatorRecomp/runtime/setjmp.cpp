// setjmp/longjmp do jogo (biblioteca C do Xbox, usada pelo Lua dos roteiros e por outras partes do jogo).
//
// No Xbox o longjmp restaura a pilha (r1), o lr e os registradores nao volateis gravados pelo setjmp e "retorna de
// novo" do setjmp. No codigo traduzido cada funcao do jogo e uma funcao do PC: restaurar os registradores do jogo nao
// volta para o ponto do setjmp, so retorna para quem chamou o longjmp. Por isso um erro de roteiro (normal e
// recuperavel no console) seguia em frente com r3 = 1 como se fosse o estado do Lua e o jogo fechava (lendo o
// endereco 1 em sub_823DF858, a funcao que mostra o erro).
//
// Agora: o gerador de C reconhece o setjmp do jogo e, no ponto da chamada, marca o retorno com o setjmp do PC num
// buffer guardado aqui (um por buffer do jogo, por thread). O longjmp do jogo roda a versao traduzida (que restaura os
// registradores do jogo no contexto) e salta com o longjmp do PC para essa marca; la os registradores sao relidos.
#include "guest.h"
#include <csetjmp>
#include <unordered_map>

namespace
{
    struct HostJump { jmp_buf buf; };
    thread_local std::unordered_map<uint32_t, HostJump> t_jumps;   // buffer do jogo -> marca do PC (nos estaveis)
}

// chamada pelo codigo traduzido no ponto de cada setjmp do jogo (buffer do jogo em r3)
extern "C" jmp_buf* predator_setjmp_buffer(PredatorCpuContext* ctx, uint8_t* base)
{
    (void)base;
    return &t_jumps[Gpr32(*ctx, 3)].buf;
}

// longjmp do jogo (r3 = buffer, r4 = valor)
// (sub_82630280 no jogo; o endereco de cada programa fica em GUEST_SUB_LONGJMP)
GUEST_DECLARE_ORIGINAL(GUEST_SUB_LONGJMP);
GUEST_FUNC(GUEST_SUB_LONGJMP)
{
    uint32_t guestBuf = Gpr32(ctx, 3);
    GUEST_ORIGINAL(GUEST_SUB_LONGJMP)(ctx, base);   // restaura pilha, lr, CR e nao volateis do jogo; r3 = valor (1 se era 0)
    auto it = t_jumps.find(guestBuf);
    if (it != t_jumps.end())
    {
        static int recovered = 0;
        if (recovered++ < 5) Log("[setjmp] longjmp recuperado: buffer %08X, volta para lr=%08X", guestBuf, (uint32_t)Lr(ctx));
        longjmp(it->second.buf, 1);
    }
    // buffer sem marca (setjmp chamado de um jeito que o gerador nao reconheceu): segue como antes, mas avisa
    static int logged = 0;
    if (logged++ < 5) Log("[setjmp] longjmp para o buffer %08X sem setjmp marcado (lr=%08X)", guestBuf, (uint32_t)Lr(ctx));
}
