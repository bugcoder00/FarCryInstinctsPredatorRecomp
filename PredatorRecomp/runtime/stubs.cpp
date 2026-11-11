// GERADO por tools/gen_stubs.py - nao editar. Funcoes do sistema ainda nao implementadas.
#include "guest.h"
#include "diag.h"
#include <atomic>

static void Stub(const char* name, std::atomic<bool>& warned, GuestCpu& ctx)
{
    char args[96];
    snprintf(args, sizeof(args), "argumentos %08X, %08X, %08X, %08X (chamada de %08X)", Gpr32(ctx, 3), Gpr32(ctx, 4), Gpr32(ctx, 5), Gpr32(ctx, 6), (uint32_t)Lr(ctx));
    diag::Note(diag::Stub, name, args);
    if (!warned.exchange(true))
        Log("[stub] %s(%08X, %08X, %08X, %08X) ainda nao implementada", name, Gpr32(ctx, 3), Gpr32(ctx, 4), Gpr32(ctx, 5), Gpr32(ctx, 6));
    SetGpr(ctx, 3, 0);
}

KFUNC(RtlCaptureContext) { static std::atomic<bool> w; Stub("RtlCaptureContext", w, ctx); }
KFUNC(RtlRaiseException) { static std::atomic<bool> w; Stub("RtlRaiseException", w, ctx); }
KFUNC(RtlUnwind) { static std::atomic<bool> w; Stub("RtlUnwind", w, ctx); }
KFUNC(XNetLogonGetTitleID) { static std::atomic<bool> w; Stub("XNetLogonGetTitleID", w, ctx); }
KFUNC(XamShowDirtyDiscErrorUI) { static std::atomic<bool> w; Stub("XamShowDirtyDiscErrorUI", w, ctx); }
KFUNC(XamShowFriendsUI) { static std::atomic<bool> w; Stub("XamShowFriendsUI", w, ctx); }
KFUNC(XamShowGamerCardUIForXUID) { static std::atomic<bool> w; Stub("XamShowGamerCardUIForXUID", w, ctx); }
KFUNC(XamShowPlayerReviewUI) { static std::atomic<bool> w; Stub("XamShowPlayerReviewUI", w, ctx); }
KFUNC(XamWriteGamerTile) { static std::atomic<bool> w; Stub("XamWriteGamerTile", w, ctx); }
KFUNC(__C_specific_handler) { static std::atomic<bool> w; Stub("__C_specific_handler", w, ctx); }
