/* Executa o C gerado por predator_codegen_sample e confere os resultados */
#include "predator_cpu.h"

#include <stdio.h>
#include <stdlib.h>

void sub_00010000(PredatorCpuContext* ctx, uint8_t* base);
void sub_00010040(PredatorCpuContext* ctx, uint8_t* base);
void sub_00010100(PredatorCpuContext* ctx, uint8_t* base);
void sub_00010140(PredatorCpuContext* ctx, uint8_t* base);
void sub_00010180(PredatorCpuContext* ctx, uint8_t* base);
void sub_00010200(PredatorCpuContext* ctx, uint8_t* base);

/* ganchos do runtime: nenhum deve ser chamado por estas funcoes */
static int g_unexpected = 0;
void predator_call_indirect(PredatorCpuContext* ctx, uint8_t* base, uint32_t target) { (void)ctx; (void)base; printf("chamada indireta inesperada 0x%08X\n", target); ++g_unexpected; }
void predator_jump_indirect(PredatorCpuContext* ctx, uint8_t* base, uint32_t from, uint32_t target) { (void)ctx; (void)base; printf("desvio indireto inesperado 0x%08X -> 0x%08X\n", from, target); ++g_unexpected; }
void predator_import(PredatorCpuContext* ctx, uint8_t* base, const char* library, uint32_t ordinal) { (void)ctx; (void)base; printf("importacao inesperada %s %u\n", library, ordinal); ++g_unexpected; }
void predator_syscall(PredatorCpuContext* ctx, uint8_t* base, uint32_t address) { (void)ctx; (void)base; printf("sc inesperado 0x%08X\n", address); ++g_unexpected; }
void predator_trap(PredatorCpuContext* ctx, uint8_t* base, uint32_t address) { (void)ctx; (void)base; printf("trap inesperado 0x%08X\n", address); ++g_unexpected; }
void predator_unimplemented(PredatorCpuContext* ctx, uint8_t* base, uint32_t address, const char* name) { (void)ctx; (void)base; printf("sem traducao 0x%08X %s\n", address, name); ++g_unexpected; }
uint64_t predator_timebase(void) { return 0; }

static int g_failures = 0;
static void Check(const char* what, uint64_t got, uint64_t expected)
{
    if (got != expected)
    {
        printf("FALHOU %s: obtido %llu, esperado %llu\n", what, (unsigned long long)got, (unsigned long long)expected);
        ++g_failures;
    }
}

int main(void)
{
    /* 1 MB de "memoria do console": a pilha fica no fim, os dados em 0x40000 */
    uint8_t* base = (uint8_t*)calloc(1, 0x100000);
    PredatorCpuContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.r[1] = 0xF0000;

    ctx.r[3] = 5;
    sub_00010000(&ctx, base);
    Check("soma 1..5", ctx.r[3], 15);

    ctx.r[3] = 5;
    sub_00010040(&ctx, base);
    Check("chamada + rlwinm + cmpwi", ctx.r[3], 1);
    Check("pilha restaurada", ctx.r[1], 0xF0000);

    ctx.r[3] = 6;
    sub_00010040(&ctx, base);
    Check("desvio bne", ctx.r[3], 0);

    ctx.r[4] = 0x40000;
    sub_00010100(&ctx, base);
    Check("vadduwm + stvx + lwz", ctx.r[3], 7);
    Check("byte big-endian", base[0x40007], 7);

    predator_stf32(base, 0x40000, 1.25f);
    sub_00010140(&ctx, base);
    Check("fadds (bits)", predator_ld32(base, 0x40004), 0x40200000u); /* 2.5f */

    ctx.r[3] = 0xFFFFFFFEull;
    sub_00010180(&ctx, base);
    Check("addic. em modo 32 bits", ctx.r[3], 1);

    /* registradores em variaveis locais: gravados no contexto antes da chamada (r4) e relidos depois (r5) */
    sub_00010200(&ctx, base);
    Check("copias locais em volta de uma chamada", ctx.r[3], 34);
    Check("r5 no contexto depois do retorno", ctx.r[5], 20);

    {
        /* vpkd3d128 / vupkd3d128: cor ARGB ida e volta */
        PredatorVr src, packed, back;
        memset(&packed, 0, sizeof(packed));
        predator_vset_u32(&src, 0, 0x40400012u);   /* R: 3.0 + 0x12/2^22 */
        predator_vset_u32(&src, 1, 0x40400034u);   /* G */
        predator_vset_u32(&src, 2, 0x40400056u);   /* B */
        predator_vset_u32(&src, 3, 0x40400078u);   /* A */
        predator_vpkd3d(&packed, &src, 0, 1, 3);
        Check("vpkd3d128 D3DCOLOR", predator_vget_u32(&packed, 0), 0x78123456u);
        predator_vset_u32(&packed, 3, predator_vget_u32(&packed, 0));
        predator_vupkd3d(&back, &packed, 0);
        Check("vupkd3d128 D3DCOLOR (R)", predator_vget_u32(&back, 0), 0x40400012u);
        Check("vupkd3d128 D3DCOLOR (A)", predator_vget_u32(&back, 3), 0x40400078u);
        /* limites: abaixo de 3.0 (1.0) -> 0, acima -> FF, NaN -> 0 (menu branco do bug 1 vinha daqui) */
        predator_vset_f32(&src, 0, 1.0f);
        predator_vset_f32(&src, 1, 100.0f);
        predator_vset_u32(&src, 2, 0xFFFFFFFFu);
        predator_vset_u32(&src, 3, 0x40400096u);
        predator_vpkd3d(&packed, &src, 0, 1, 0);
        Check("vpkd3d128 D3DCOLOR (limites)", predator_vget_u32(&packed, 3), 0x9600FF00u);
        predator_vset_f32(&src, 0, 1.5f);
        predator_vset_f32(&src, 1, -2.0f);
        predator_vpkd3d(&packed, &src, 3, 1, 0);
        Check("vpkd3d128 FLOAT16_2", predator_vget_u32(&packed, 3), 0x3E00C000u);
    }

    free(base);
    if (g_failures || g_unexpected)
    {
        printf("%d falhas, %d chamadas inesperadas\n", g_failures, g_unexpected);
        return 1;
    }
    printf("codigo gerado: todos os testes passaram\n");
    return 0;
}
