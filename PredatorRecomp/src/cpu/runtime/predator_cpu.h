/* predator_cpu.h - contexto da CPU e operacoes usadas pelo codigo C gerado pelo PredatorRecomp
 *
 * C99 portavel (MSVC, Clang, GCC). O codigo gerado so depende deste arquivo e das funcoes "predator_*" declaradas
 * no fim, que o runtime do jogo implementa (chamadas indiretas, importacoes do sistema, armadilhas...).
 *
 * Convencoes:
 *   - registradores inteiros de 64 bits, como no Xenon (enderecos de memoria usam os 32 bits baixos);
 *   - a memoria do console comeca em 'base' (ponteiro do PC); tudo nela e big-endian;
 *   - registradores vetoriais guardam os 16 bytes na ordem da memoria (big-endian): carga e armazenamento sao copias
 *     diretas, e as operacoes por elemento convertem cada elemento.
 */
#ifndef PREDATOR_CPU_H
#define PREDATOR_CPU_H

#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define PREDATOR_INLINE static __forceinline
#else
#define PREDATOR_INLINE static inline __attribute__((always_inline))
#endif

/* Cada funcao traduzida e definida como <nome>_original, e <nome> e um apelido para ela. O runtime pode substituir
   uma funcao do jogo definindo <nome> ele mesmo (e chamar a versao traduzida por <nome>_original). */
#if defined(_MSC_VER)
#define PREDATOR_FUNCTION(name) \
    __pragma(comment(linker, "/alternatename:" #name "=" #name "_original")) \
    void name##_original(PredatorCpuContext* ctx, uint8_t* base)
#else
#define PREDATOR_FUNCTION(name) \
    void name(PredatorCpuContext* ctx, uint8_t* base) __attribute__((weak, alias(#name "_original"))); \
    void name##_original(PredatorCpuContext* ctx, uint8_t* base)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- contexto */

typedef struct PredatorCrField
{
    uint8_t lt, gt, eq, so;   /* bits 0-3 do campo (so = "summary overflow" ou "unordered" no ponto flutuante) */
} PredatorCrField;

typedef union PredatorFpr
{
    double f;
    uint64_t u;
} PredatorFpr;

typedef struct PredatorVr
{
    uint8_t b[16];   /* ordem big-endian: b[0] e o byte mais significativo do elemento 0 */
} PredatorVr;

typedef struct PredatorCpuContext
{
    uint64_t r[32];          /* GPR */
    PredatorFpr f[32];       /* FPR */
    PredatorVr v[128];       /* VR: VMX usa v0-v31, VMX128 v0-v127 */
    PredatorCrField cr[8];   /* CR0-CR7 */
    uint64_t lr;
    uint64_t ctr;
    uint8_t xer_so, xer_ov, xer_ca, xer_count;
    uint32_t fpscr;
    uint32_t vscr;
    uint64_t msr;
    uint32_t reserve_address;   /* lwarx/ldarx: endereco reservado */
    uint64_t reserve_value;     /* lwarx/ldarx: valor lido (bytes como estao na memoria) */
    void* user;                 /* livre para o runtime */
} PredatorCpuContext;

typedef void (*PredatorFunction)(PredatorCpuContext* ctx, uint8_t* base);

/* ---------------------------------------------------------------- funcoes do runtime (implementadas pelo jogo) */

void predator_call_indirect(PredatorCpuContext* ctx, uint8_t* base, uint32_t target);        /* bctrl, bl para fora */
void predator_jump_indirect(PredatorCpuContext* ctx, uint8_t* base, uint32_t from, uint32_t target); /* bctr sem tabela */
void predator_import(PredatorCpuContext* ctx, uint8_t* base, const char* library, uint32_t ordinal);
void predator_syscall(PredatorCpuContext* ctx, uint8_t* base, uint32_t address);
void predator_trap(PredatorCpuContext* ctx, uint8_t* base, uint32_t address);
void predator_unimplemented(PredatorCpuContext* ctx, uint8_t* base, uint32_t address, const char* name);
uint64_t predator_timebase(void);
/* setjmp do jogo: o codigo traduzido marca o ponto de retorno com o setjmp do PC neste buffer (um por buffer do jogo,
   endereco em r3). O longjmp do jogo restaura os registradores dele e salta para ca (o setjmp "retorna de novo"). */
jmp_buf* predator_setjmp_buffer(PredatorCpuContext* ctx, uint8_t* base);

/* ---------------------------------------------------------------- inteiros */

PREDATOR_INLINE uint16_t predator_bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
PREDATOR_INLINE uint32_t predator_bswap32(uint32_t v)
{
#if defined(_MSC_VER) && !defined(__clang__)
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}
PREDATOR_INLINE uint64_t predator_bswap64(uint64_t v)
{
#if defined(_MSC_VER) && !defined(__clang__)
    return _byteswap_uint64(v);
#else
    return __builtin_bswap64(v);
#endif
}

PREDATOR_INLINE uint32_t predator_rotl32(uint32_t v, uint32_t n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
PREDATOR_INLINE uint64_t predator_rotl64(uint64_t v, uint32_t n) { n &= 63; return n ? (v << n) | (v >> (64 - n)) : v; }

/* mascara de MB a ME (bits numerados 0-63 a partir do mais significativo, como no manual), com volta quando MB > ME */
PREDATOR_INLINE uint64_t predator_mask64(uint32_t mb, uint32_t me)
{
    uint64_t a = ~0ull >> mb;
    uint64_t b = ~0ull << (63 - me);
    return mb <= me ? (a & b) : (a | b);
}

/* rlwinm/rlwimi/rlwnm: a rotacao de 32 bits aparece nas duas metades do registro de 64 bits */
PREDATOR_INLINE uint64_t predator_rotl32x2(uint64_t v, uint32_t n)
{
    uint64_t r = predator_rotl32((uint32_t)v, n);
    return (r << 32) | r;
}

PREDATOR_INLINE uint32_t predator_cntlz32(uint32_t v)
{
    if (!v) return 32;
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long i; _BitScanReverse(&i, v); return 31 - i;
#else
    return (uint32_t)__builtin_clz(v);
#endif
}
PREDATOR_INLINE uint32_t predator_cntlz64(uint64_t v)
{
    if (!v) return 64;
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long i; _BitScanReverse64(&i, v); return 63 - i;
#else
    return (uint32_t)__builtin_clzll(v);
#endif
}

/* parte alta do produto de 64 x 64 bits */
PREDATOR_INLINE uint64_t predator_mulhu64(uint64_t a, uint64_t b)
{
#if defined(_MSC_VER) && !defined(__clang__)
    return __umulh(a, b);
#else
    return (uint64_t)(((unsigned __int128)a * b) >> 64);
#endif
}
PREDATOR_INLINE int64_t predator_mulhs64(int64_t a, int64_t b)
{
#if defined(_MSC_VER) && !defined(__clang__)
    return __mulh(a, b);
#else
    return (int64_t)(((__int128)a * b) >> 64);
#endif
}

/* divisoes: o resultado de divisao por zero (ou estouro) e indefinido no hardware; aqui vira 0 */
PREDATOR_INLINE int32_t predator_divw(int32_t a, int32_t b) { return (b == 0 || (a == INT32_MIN && b == -1)) ? 0 : a / b; }
PREDATOR_INLINE uint32_t predator_divwu(uint32_t a, uint32_t b) { return b ? a / b : 0; }
PREDATOR_INLINE int64_t predator_divd(int64_t a, int64_t b) { return (b == 0 || (a == INT64_MIN && b == -1)) ? 0 : a / b; }
PREDATOR_INLINE uint64_t predator_divdu(uint64_t a, uint64_t b) { return b ? a / b : 0; }

/* ---------------------------------------------------------------- registrador de condicao */

PREDATOR_INLINE void predator_cmp_s64(PredatorCrField* c, int64_t a, int64_t b, uint8_t so)
{
    c->lt = a < b; c->gt = a > b; c->eq = a == b; c->so = so;
}
PREDATOR_INLINE void predator_cmp_u64(PredatorCrField* c, uint64_t a, uint64_t b, uint8_t so)
{
    c->lt = a < b; c->gt = a > b; c->eq = a == b; c->so = so;
}
/* forma "." (Rc): CR0 a partir do resultado de 64 bits */
/* Os jogos rodam com o processador em modo 32 bits (MSR[SF] = 0): o registro de resultado (Rc) compara so os
   32 bits de baixo, com sinal, e o carry (XER[CA]) e o estouro saem da conta de 32 bits. Os registradores continuam
   guardando 64 bits. */
PREDATOR_INLINE void predator_set_cr0(PredatorCpuContext* ctx, uint64_t result)
{
    predator_cmp_s64(&ctx->cr[0], (int64_t)(int32_t)result, 0, ctx->xer_so);
}
PREDATOR_INLINE void predator_cmp_f64(PredatorCrField* c, double a, double b)
{
    int un = (a != a) || (b != b);
    c->lt = !un && a < b; c->gt = !un && a > b; c->eq = !un && a == b; c->so = (uint8_t)un;
}

PREDATOR_INLINE uint8_t* predator_crbit(PredatorCpuContext* ctx, uint32_t bit)
{
    PredatorCrField* f = &ctx->cr[bit >> 2];
    switch (bit & 3) { case 0: return &f->lt; case 1: return &f->gt; case 2: return &f->eq; default: return &f->so; }
}

PREDATOR_INLINE uint32_t predator_get_cr(const PredatorCpuContext* ctx)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; ++i)
    {
        const PredatorCrField* f = &ctx->cr[i];
        v |= (uint32_t)((f->lt << 3) | (f->gt << 2) | (f->eq << 1) | f->so) << (28 - 4 * i);
    }
    return v;
}
PREDATOR_INLINE void predator_set_cr(PredatorCpuContext* ctx, uint32_t value, uint32_t fieldMask)
{
    for (int i = 0; i < 8; ++i)
    {
        if (!(fieldMask & (0x80u >> i))) continue;
        uint32_t n = (value >> (28 - 4 * i)) & 0xF;
        PredatorCrField* f = &ctx->cr[i];
        f->lt = (n >> 3) & 1; f->gt = (n >> 2) & 1; f->eq = (n >> 1) & 1; f->so = n & 1;
    }
}

/* ---------------------------------------------------------------- memoria (big-endian) */

#define PREDATOR_PTR(base, addr) ((base) + (uint32_t)(addr))

PREDATOR_INLINE uint8_t predator_ld8(const uint8_t* base, uint32_t a) { return *PREDATOR_PTR(base, a); }
PREDATOR_INLINE uint16_t predator_ld16(const uint8_t* base, uint32_t a) { uint16_t v; memcpy(&v, PREDATOR_PTR(base, a), 2); return predator_bswap16(v); }
PREDATOR_INLINE uint32_t predator_ld32(const uint8_t* base, uint32_t a) { uint32_t v; memcpy(&v, PREDATOR_PTR(base, a), 4); return predator_bswap32(v); }
PREDATOR_INLINE uint64_t predator_ld64(const uint8_t* base, uint32_t a) { uint64_t v; memcpy(&v, PREDATOR_PTR(base, a), 8); return predator_bswap64(v); }
PREDATOR_INLINE void predator_st8(uint8_t* base, uint32_t a, uint8_t v) { *PREDATOR_PTR(base, a) = v; }
PREDATOR_INLINE void predator_st16(uint8_t* base, uint32_t a, uint16_t v) { v = predator_bswap16(v); memcpy(PREDATOR_PTR(base, a), &v, 2); }
PREDATOR_INLINE void predator_st32(uint8_t* base, uint32_t a, uint32_t v) { v = predator_bswap32(v); memcpy(PREDATOR_PTR(base, a), &v, 4); }
PREDATOR_INLINE void predator_st64(uint8_t* base, uint32_t a, uint64_t v) { v = predator_bswap64(v); memcpy(PREDATOR_PTR(base, a), &v, 8); }

PREDATOR_INLINE float predator_ldf32(const uint8_t* base, uint32_t a) { uint32_t u = predator_ld32(base, a); float f; memcpy(&f, &u, 4); return f; }
PREDATOR_INLINE double predator_ldf64(const uint8_t* base, uint32_t a) { uint64_t u = predator_ld64(base, a); double d; memcpy(&d, &u, 8); return d; }
PREDATOR_INLINE void predator_stf32(uint8_t* base, uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); predator_st32(base, a, u); }
PREDATOR_INLINE void predator_stf64(uint8_t* base, uint32_t a, double d) { uint64_t u; memcpy(&u, &d, 8); predator_st64(base, a, u); }

/* dcbz / dcbz128: zera a linha de cache inteira */
PREDATOR_INLINE void predator_zero(uint8_t* base, uint32_t a, uint32_t lineSize) { memset(PREDATOR_PTR(base, a & ~(lineSize - 1)), 0, lineSize); }

/* lwarx/stwcx. e ldarx/stdcx.: reserva = (endereco, valor lido); o armazenamento condicional so acontece se a memoria
   ainda tiver o mesmo valor (troca atomica), e CR0.eq diz se deu certo */
PREDATOR_INLINE uint32_t predator_lwarx(PredatorCpuContext* ctx, const uint8_t* base, uint32_t a)
{
    uint32_t raw; memcpy(&raw, PREDATOR_PTR(base, a), 4);
    ctx->reserve_address = a; ctx->reserve_value = raw;
    return predator_bswap32(raw);
}
PREDATOR_INLINE uint64_t predator_ldarx(PredatorCpuContext* ctx, const uint8_t* base, uint32_t a)
{
    uint64_t raw; memcpy(&raw, PREDATOR_PTR(base, a), 8);
    ctx->reserve_address = a; ctx->reserve_value = raw;
    return predator_bswap64(raw);
}
PREDATOR_INLINE void predator_stwcx(PredatorCpuContext* ctx, uint8_t* base, uint32_t a, uint32_t value)
{
    int ok = 0;
    if (ctx->reserve_address == a)
    {
        uint32_t expected = (uint32_t)ctx->reserve_value, desired = predator_bswap32(value);
        volatile uint32_t* p = (volatile uint32_t*)PREDATOR_PTR(base, a);
#if defined(_MSC_VER) && !defined(__clang__)
        ok = (uint32_t)_InterlockedCompareExchange((volatile long*)p, (long)desired, (long)expected) == expected;
#else
        ok = __atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#endif
    }
    ctx->reserve_address = 0xFFFFFFFFu;
    ctx->cr[0].lt = 0; ctx->cr[0].gt = 0; ctx->cr[0].eq = (uint8_t)ok; ctx->cr[0].so = ctx->xer_so;
}
PREDATOR_INLINE void predator_stdcx(PredatorCpuContext* ctx, uint8_t* base, uint32_t a, uint64_t value)
{
    int ok = 0;
    if (ctx->reserve_address == a)
    {
        uint64_t expected = ctx->reserve_value, desired = predator_bswap64(value);
        volatile uint64_t* p = (volatile uint64_t*)PREDATOR_PTR(base, a);
#if defined(_MSC_VER) && !defined(__clang__)
        ok = (uint64_t)_InterlockedCompareExchange64((volatile long long*)p, (long long)desired, (long long)expected) == expected;
#else
        ok = __atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#endif
    }
    ctx->reserve_address = 0xFFFFFFFFu;
    ctx->cr[0].lt = 0; ctx->cr[0].gt = 0; ctx->cr[0].eq = (uint8_t)ok; ctx->cr[0].so = ctx->xer_so;
}

/* ---------------------------------------------------------------- ponto flutuante */

PREDATOR_INLINE double predator_round_single(double v) { return (double)(float)v; }
PREDATOR_INLINE uint64_t predator_f64_bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
PREDATOR_INLINE double predator_bits_f64(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

/* fctiw/fctiwz/fctid/fctidz: conversao com saturacao (NaN -> menor valor) */
PREDATOR_INLINE int64_t predator_f64_to_s32(double v)
{
    if (v != v || v <= -2147483648.0) return INT32_MIN;
    if (v >= 2147483647.0) return INT32_MAX;
    return (int32_t)v;
}
PREDATOR_INLINE int64_t predator_f64_to_s64(double v)
{
    if (v != v || v <= -9223372036854775808.0) return INT64_MIN;
    if (v >= 9223372036854775807.0) return INT64_MAX;
    return (int64_t)v;
}

/* ---------------------------------------------------------------- vetores (elementos em big-endian dentro de b[]) */

PREDATOR_INLINE uint32_t predator_vget_u32(const PredatorVr* v, int i) { uint32_t x; memcpy(&x, &v->b[i * 4], 4); return predator_bswap32(x); }
PREDATOR_INLINE void predator_vset_u32(PredatorVr* v, int i, uint32_t x) { x = predator_bswap32(x); memcpy(&v->b[i * 4], &x, 4); }
PREDATOR_INLINE float predator_vget_f32(const PredatorVr* v, int i) { uint32_t u = predator_vget_u32(v, i); float f; memcpy(&f, &u, 4); return f; }
PREDATOR_INLINE void predator_vset_f32(PredatorVr* v, int i, float f) { uint32_t u; memcpy(&u, &f, 4); predator_vset_u32(v, i, u); }
PREDATOR_INLINE uint16_t predator_vget_u16(const PredatorVr* v, int i) { return (uint16_t)((v->b[i * 2] << 8) | v->b[i * 2 + 1]); }
PREDATOR_INLINE void predator_vset_u16(PredatorVr* v, int i, uint16_t x) { v->b[i * 2] = (uint8_t)(x >> 8); v->b[i * 2 + 1] = (uint8_t)x; }

PREDATOR_INLINE void predator_lvx(PredatorVr* d, const uint8_t* base, uint32_t a) { memcpy(d->b, PREDATOR_PTR(base, a & ~15u), 16); }
PREDATOR_INLINE void predator_stvx(const PredatorVr* s, uint8_t* base, uint32_t a) { memcpy(PREDATOR_PTR(base, a & ~15u), s->b, 16); }

/* lvlx: bytes de a ate o fim do bloco de 16, encostados a esquerda; lvrx: bytes do inicio do bloco ate a, a direita */
PREDATOR_INLINE void predator_lvlx(PredatorVr* d, const uint8_t* base, uint32_t a)
{
    uint32_t off = a & 15; PredatorVr t; memset(&t, 0, sizeof(t));
    memcpy(t.b, PREDATOR_PTR(base, a), 16 - off); *d = t;
}
PREDATOR_INLINE void predator_lvrx(PredatorVr* d, const uint8_t* base, uint32_t a)
{
    uint32_t off = a & 15; PredatorVr t; memset(&t, 0, sizeof(t));
    if (off) memcpy(t.b + (16 - off), PREDATOR_PTR(base, a & ~15u), off);
    *d = t;
}
PREDATOR_INLINE void predator_stvlx(const PredatorVr* s, uint8_t* base, uint32_t a) { memcpy(PREDATOR_PTR(base, a), s->b, 16 - (a & 15)); }
PREDATOR_INLINE void predator_stvrx(const PredatorVr* s, uint8_t* base, uint32_t a)
{
    uint32_t off = a & 15;
    if (off) memcpy(PREDATOR_PTR(base, a & ~15u), s->b + (16 - off), off);
}
PREDATOR_INLINE void predator_lvsl(PredatorVr* d, uint32_t a) { for (int i = 0; i < 16; ++i) d->b[i] = (uint8_t)((a & 15) + i); }
PREDATOR_INLINE void predator_lvsr(PredatorVr* d, uint32_t a) { for (int i = 0; i < 16; ++i) d->b[i] = (uint8_t)(16 - (a & 15) + i); }
PREDATOR_INLINE void predator_lvewx(PredatorVr* d, const uint8_t* base, uint32_t a) { memcpy(&d->b[a & 12], PREDATOR_PTR(base, a & ~3u), 4); }
PREDATOR_INLINE void predator_stvewx(const PredatorVr* s, uint8_t* base, uint32_t a) { memcpy(PREDATOR_PTR(base, a & ~3u), &s->b[a & 12], 4); }

/* vperm: cada byte do resultado escolhe um dos 32 bytes de (a,b) pelos 5 bits baixos do byte de controle */
PREDATOR_INLINE void predator_vperm(PredatorVr* d, const PredatorVr* a, const PredatorVr* b, const PredatorVr* c)
{
    PredatorVr t;
    for (int i = 0; i < 16; ++i) { uint8_t k = c->b[i] & 31; t.b[i] = k < 16 ? a->b[k] : b->b[k - 16]; }
    *d = t;
}
PREDATOR_INLINE void predator_vsldoi(PredatorVr* d, const PredatorVr* a, const PredatorVr* b, int sh)
{
    PredatorVr t;
    for (int i = 0; i < 16; ++i) { int k = i + sh; t.b[i] = k < 16 ? a->b[k] : b->b[k - 16]; }
    *d = t;
}

/* operacao elemento a elemento em floats: o corpo usa a, b, c (entradas) e devolve o resultado */
#define PREDATOR_VFP3(d, va, vb, vc, expr)                                                     \
    do {                                                                                     \
        PredatorVr t_;                                                                       \
        for (int i_ = 0; i_ < 4; ++i_) {                                                     \
            float a = predator_vget_f32(va, i_), b = predator_vget_f32(vb, i_), c = predator_vget_f32(vc, i_); \
            (void)a; (void)b; (void)c;                                                       \
            predator_vset_f32(&t_, i_, (float)(expr));                                       \
        }                                                                                    \
        *(d) = t_;                                                                           \
    } while (0)

#define PREDATOR_VU32(d, va, vb, expr)                                                         \
    do {                                                                                     \
        PredatorVr t_;                                                                       \
        for (int i_ = 0; i_ < 4; ++i_) {                                                     \
            uint32_t a = predator_vget_u32(va, i_), b = predator_vget_u32(vb, i_);          \
            (void)a; (void)b;                                                                \
            predator_vset_u32(&t_, i_, (uint32_t)(expr));                                    \
        }                                                                                    \
        *(d) = t_;                                                                           \
    } while (0)

/* comparacoes vetoriais: elementos verdadeiros viram 0xFFFFFFFF; com Rc, CR6 = (todos verdadeiros, 0, nenhum, 0) */
PREDATOR_INLINE void predator_vcmp_result(PredatorCpuContext* ctx, const PredatorVr* d, int record)
{
    if (!record) return;
    int all = 1, none = 1;
    for (int i = 0; i < 4; ++i) { uint32_t x = predator_vget_u32(d, i); all &= x == 0xFFFFFFFFu; none &= x == 0; }
    ctx->cr[6].lt = (uint8_t)all; ctx->cr[6].gt = 0; ctx->cr[6].eq = (uint8_t)none; ctx->cr[6].so = 0;
}

PREDATOR_INLINE float predator_rsqrt(float v) { return 1.0f / sqrtf(v); }

/* conversoes float <-> inteiro com saturacao e escala 2^uimm (vctsxs/vcfsx e as versoes 128) */
PREDATOR_INLINE int32_t predator_f32_to_s32_sat(float v)
{
    if (v != v) return 0;
    if (v >= 2147483647.0f) return INT32_MAX;
    if (v <= -2147483648.0f) return INT32_MIN;
    return (int32_t)v;
}
PREDATOR_INLINE uint32_t predator_f32_to_u32_sat(float v)
{
    if (v != v || v <= 0.0f) return 0;
    if (v >= 4294967295.0f) return UINT32_MAX;
    return (uint32_t)v;
}

/* vsl / vsr: desloca o registrador inteiro (128 bits) de 0 a 7 bits */
PREDATOR_INLINE void predator_vshift_left(PredatorVr* d, const PredatorVr* a, unsigned n)
{
    PredatorVr t;
    for (int i = 0; i < 16; ++i) t.b[i] = (uint8_t)((a->b[i] << n) | (i < 15 && n ? a->b[i + 1] >> (8 - n) : 0));
    *d = t;
}
PREDATOR_INLINE void predator_vshift_right(PredatorVr* d, const PredatorVr* a, unsigned n)
{
    PredatorVr t;
    for (int i = 0; i < 16; ++i) t.b[i] = (uint8_t)((a->b[i] >> n) | (i > 0 && n ? a->b[i - 1] << (8 - n) : 0));
    *d = t;
}

/* meio-float (IEEE 754 binary16) -> float */
PREDATOR_INLINE float predator_half_to_float(uint16_t h)
{
    uint32_t sign = (uint32_t)(h >> 15) << 31, exp = (h >> 10) & 0x1F, man = h & 0x3FF, bits;
    if (exp == 0)
    {
        if (man == 0) bits = sign;
        else
        {
            exp = 113;
            while (!(man & 0x400)) { man <<= 1; --exp; }
            bits = sign | (exp << 23) | ((man & 0x3FF) << 13);
        }
    }
    else if (exp == 31) bits = sign | 0x7F800000u | (man << 13);
    else bits = sign | ((exp + 112) << 23) | (man << 13);
    float f; memcpy(&f, &bits, 4); return f;
}

/* vupkd3d128: desempacota formatos de vertice do Direct3D do Xbox 360 a partir da ultima palavra de vB.
   Os inteiros saem "embutidos" num float com expoente fixo (1.0 + x/2^23 ou 3.0 + x/2^22), como faz o hardware.
   Devolve 0 se o tipo nao for conhecido. */
PREDATOR_INLINE int predator_vupkd3d(PredatorVr* d, const PredatorVr* b, unsigned type)
{
    const uint32_t w = predator_vget_u32(b, 3);
    switch (type)
    {
    case 0: /* D3DCOLOR (ARGB8) -> R, G, B, A, como 3.0 + byte/2^22 (0x404000xx) */
        predator_vset_u32(d, 0, 0x40400000u | ((w >> 16) & 0xFF));
        predator_vset_u32(d, 1, 0x40400000u | ((w >> 8) & 0xFF));
        predator_vset_u32(d, 2, 0x40400000u | (w & 0xFF));
        predator_vset_u32(d, 3, 0x40400000u | (w >> 24));
        return 1;
    case 1: /* NORMSHORT2 -> (3 + x/2^22, 3 + y/2^22, 0, 1) */
        predator_vset_u32(d, 0, 0x40400000u + (uint32_t)(int32_t)(int16_t)(w >> 16));
        predator_vset_u32(d, 1, 0x40400000u + (uint32_t)(int32_t)(int16_t)w);
        predator_vset_u32(d, 2, 0);
        predator_vset_u32(d, 3, 0x3F800000u);
        return 1;
    case 3: /* FLOAT16_2 -> (x, y, 0, 1) */
        predator_vset_f32(d, 0, predator_half_to_float((uint16_t)(w >> 16)));
        predator_vset_f32(d, 1, predator_half_to_float((uint16_t)w));
        predator_vset_u32(d, 2, 0);
        predator_vset_u32(d, 3, 0x3F800000u);
        return 1;
    case 5: /* FLOAT16_4 (palavras 2 e 3) */
    {
        const uint32_t w2 = predator_vget_u32(b, 2);
        predator_vset_f32(d, 0, predator_half_to_float((uint16_t)(w2 >> 16)));
        predator_vset_f32(d, 1, predator_half_to_float((uint16_t)w2));
        predator_vset_f32(d, 2, predator_half_to_float((uint16_t)(w >> 16)));
        predator_vset_f32(d, 3, predator_half_to_float((uint16_t)w));
        return 1;
    }
    default:
        return 0;
    }
}

/* float -> meio-float (binary16), arredondando para o par mais proximo; estouro vira infinito */
PREDATOR_INLINE uint16_t predator_float_to_half(float f)
{
    uint32_t x; memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exp = (int32_t)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t man = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFF) == 0xFF) return (uint16_t)(sign | 0x7C00u | (man ? 0x200u : 0));   /* inf / NaN */
    if (exp >= 31) return (uint16_t)(sign | 0x7C00u);
    if (exp <= 0)
    {
        if (exp < -10) return (uint16_t)sign;
        man |= 0x800000u;
        const uint32_t shift = (uint32_t)(14 - exp);
        uint32_t h = man >> shift;
        const uint32_t rest = man & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rest > half || (rest == half && (h & 1))) ++h;
        return (uint16_t)(sign | h);
    }
    uint32_t h = ((uint32_t)exp << 10) | (man >> 13);
    const uint32_t rest = man & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (h & 1))) ++h;   /* pode subir o expoente, como deve */
    return (uint16_t)(sign | h);
}

/* vpkd3d128: o inverso de vupkd3d128. Converte vB no formato 'type' (32 ou 64 bits, alinhado no fim do vetor) e
   copia o resultado para vD: 'pack' 1 = uma palavra, 2 = duas; 'shift' desloca para as palavras anteriores
   (shift 3 com uma palavra = palavra 0). Os inteiros vem "embutidos" em floats de expoente fixo, como no unpack.
   Devolve 0 se a combinacao nao for conhecida. */
PREDATOR_INLINE int predator_vpkd3d(PredatorVr* d, const PredatorVr* b, unsigned type, unsigned pack, unsigned shift)
{
    uint32_t w2 = 0, w3 = 0;
    switch (type)
    {
    case 0: /* D3DCOLOR: cada canal limitado a 3.0 .. 3.0 + 255/2^22 (0x40400000..0x404000FF) e o byte de baixo
               vira o componente; NaN vira 0 (o jogo empacota 0xFFFFFFFF para obter zero) */
    {
        uint32_t c[4];
        for (int i = 0; i < 4; ++i)
        {
            uint32_t v = predator_vget_u32(b, i);
            if ((v & 0x7FFFFFFFu) > 0x7F800000u || (int32_t)v < 0x40400000) v = 0x40400000u;
            if (v > 0x404000FFu) v = 0x404000FFu;
            c[i] = v & 0xFF;
        }
        w3 = (c[3] << 24) | (c[0] << 16) | (c[1] << 8) | c[2];
        break;
    }
    case 1: /* NORMSHORT2: 3.0 +- 32767/2^22, os 16 bits de baixo */
    {
        uint32_t c[2];
        for (int i = 0; i < 2; ++i)
        {
            int32_t v = (int32_t)predator_vget_u32(b, i);
            if (v < 0x40400000 - 32767) v = 0x40400000 - 32767;
            if (v > 0x40400000 + 32767) v = 0x40400000 + 32767;
            c[i] = (uint32_t)(v - 0x40400000) & 0xFFFFu;
        }
        w3 = (c[0] << 16) | c[1];
        break;
    }
    case 3: /* FLOAT16_2 */
        w3 = ((uint32_t)predator_float_to_half(predator_vget_f32(b, 0)) << 16) | predator_float_to_half(predator_vget_f32(b, 1));
        break;
    case 5: /* FLOAT16_4 */
        w2 = ((uint32_t)predator_float_to_half(predator_vget_f32(b, 0)) << 16) | predator_float_to_half(predator_vget_f32(b, 1));
        w3 = ((uint32_t)predator_float_to_half(predator_vget_f32(b, 2)) << 16) | predator_float_to_half(predator_vget_f32(b, 3));
        break;
    default:
        return 0;
    }
    if (pack == 1)
        predator_vset_u32(d, (int)(3 - shift), w3);
    else if (pack == 2)
    {
        if (shift < 3) predator_vset_u32(d, (int)(2 - shift), w2);   /* com shift 3 so a palavra de baixo cabe */
        predator_vset_u32(d, (int)(3 - shift), w3);
    }
    else
        return 0;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* PREDATOR_CPU_H */
