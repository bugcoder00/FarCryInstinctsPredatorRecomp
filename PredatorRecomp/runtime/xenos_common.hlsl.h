// Cabecalho HLSL incluido em todo shader traduzido do Xenos (como string C++)
#pragma once

static const char* XENOS_COMMON_HLSL = R"HLSL(
// ---- constantes do estagio (copiadas dos registradores da GPU a cada desenho) ----
cbuffer Consts : register(b0)
{
    float4 c[256];      // constantes float do estagio
    uint4 bools[2];     // 256 booleanos (bits)
    uint4 loops[8];     // 32 constantes de loop: contagem (bits 0-7), inicio (8-15), passo (16-23)
};

cbuffer Draw : register(b1)
{
    uint4 vfetch[48];   // 96 fetch constants de vertice (2 dwords cada)
    uint indexBase;     // posicao dos indices dentro de "mem"
    uint indexInfo;     // bit 0: indices de 32 bits; bits 1-2: endian; bit 3: ha indices
    uint vertexOffset;
    uint primMode;      // 0 = direto, 1 = lista de quads, 2 = leque de triangulos
    float4 vpScale;     // PA_CL_VPORT_*SCALE (x, y, z)
    float4 vpOffset;    // PA_CL_VPORT_*OFFSET (x, y, z)
    float4 rtSize;      // largura, altura do alvo; zw = deslocamento da janela (PA_SC_WINDOW_OFFSET) em pixels
    uint vteCntl;       // PA_CL_VTE_CNTL
    uint paramGen;      // bit 31: o PS recebe "param gen" no registrador (bits 0-7) = posicao do pixel e face
    uint alphaCtl;      // RB_COLORCONTROL: bits 0-2 funcao do alpha test, bit 3 ativo; bit 16: limitar a cor a 0-1
    float alphaRef;     // RB_ALPHA_REF
    float pixShift;     // +0,5 se PA_SU_VTX_CNTL.pix_center = 0 (centro do pixel no inteiro, convencao do D3D9)
    uint clipCntl;      // PA_CL_CLIP_CNTL: bits 0-5 planos de corte do usuario ligados, bit 16 corte desligado
    float resScale;     // escala do alvo deste desenho: pixels da placa por pixel do jogo (1 a 3)
    uint scaledTex;     // bit por slot: textura com imagem da tela (resolve), texScale vezes o tamanho do jogo
    float4 ucp[6];      // PA_CL_UCP_0..5 (0x2388-0x239F: planos no espaco de recorte, aplicados a posicao que o shader escreve)
    float texScale;     // resolucao interna (escala das texturas marcadas em scaledTex)
};

// Planos de corte do usuario (reflexo da agua: corta o que esta abaixo da superficie). Distancia < 0 = cortado.
void UserClip(float4 p, out float4 c0, out float2 c1)
{
    c0 = 1; c1 = 1;
    if ((clipCntl & 63u) == 0 || (clipCntl & (1u << 16))) return;
    if (clipCntl & 1u)  c0.x = dot(p, ucp[0]);
    if (clipCntl & 2u)  c0.y = dot(p, ucp[1]);
    if (clipCntl & 4u)  c0.z = dot(p, ucp[2]);
    if (clipCntl & 8u)  c0.w = dot(p, ucp[3]);
    if (clipCntl & 16u) c1.x = dot(p, ucp[4]);
    if (clipCntl & 32u) c1.y = dot(p, ucp[5]);
}

// posicao do Xenos -> clip space do D3D12 (como o hardware faria com PA_CL_VTE_CNTL)
float4 XenosToClip(float4 p)
{
    if ((vteCntl & (1u << 10)) == 0) p.w = rcp(p.w);   // VTX_W0_FMT: senao, w vem como 1/w
    if (vteCntl & (1u << 8)) p.xy *= p.w;              // VTX_XY_FMT: xy ja dividido por w
    if (vteCntl & (1u << 9)) p.z *= p.w;               // VTX_Z_FMT
    float2 scale = float2((vteCntl & 1) ? vpScale.x : 1.0, (vteCntl & 4) ? vpScale.y : 1.0);
    float2 offset = float2((vteCntl & 2) ? vpOffset.x : 0.0, (vteCntl & 8) ? vpOffset.y : 0.0);
    float2 screen = p.xy * scale + (offset + rtSize.zw) * p.w;   // em pixels (com deslocamento da janela), vezes w
    // Bug 37 (brechas na borda direita/de baixo em 3x/4K): os quadrados 2D do jogo vao de -0,5 a 1279,5 (centro do pixel
    // no inteiro). No Xbox uma borda em x,5 cobre ate o pixel inteiro seguinte; com o meio pixel da placa (pixShift) a
    // borda 1279,5 caia em 3839 na 3x e a ultima coluna/linha ficava sem pintar. Acima de 1x, bordas exatamente em x,5
    // de desenhos 2D (w = 1) sobem para o inteiro seguinte; bordas inteiras (tela dividida, bug 32) ficam como estao.
    if (pixShift != 0 && resScale > 1.0 && abs(p.w - 1.0) < 1e-6)
    {
        float2 f = screen - floor(screen);
        screen += select(abs(f - 0.5) < 1e-3, 0.5, 0.0);
    }
    screen += pixShift * p.w;
    float4 o;
    o.x = screen.x * 2.0 / rtSize.x - p.w;
    o.y = p.w - screen.y * 2.0 / rtSize.y;
    o.z = p.z * ((vteCntl & 16) ? vpScale.z : 1.0) + ((vteCntl & 32) ? vpOffset.z : 0.0) * p.w;
    o.w = p.w;
    return o;
}

ByteAddressBuffer mem : register(t0);           // copias dos buffers de vertices/indices usados no quadro
Texture2D tex[32] : register(t1);
TextureCube texCube[32] : register(t33);        // mesmos slots, para as texturas lidas como cubo
SamplerState samp[32] : register(s0);

uint GpuSwap(uint v, uint endian)
{
    if (endian == 1) return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    if (endian == 2) return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    if (endian == 3) return (v << 16) | (v >> 16);
    return v;
}

uint LoadIndex(uint vertexId)
{
    // primitivas que o D3D12 nao tem viram listas de triangulos
    if (primMode == 1)      // quads: 0 1 2, 0 2 3
    {
        uint k = vertexId % 6;
        vertexId = (vertexId / 6) * 4 + (k < 3 ? k : (k == 3 ? 0 : k - 2));
    }
    else if (primMode == 2) // leque: 0, i+1, i+2
    {
        uint k = vertexId % 3;
        vertexId = k == 0 ? 0 : vertexId / 3 + k;
    }
    if ((indexInfo & 8) == 0) return vertexId + vertexOffset;
    uint endian = (indexInfo >> 1) & 3;
    if (indexInfo & 1)
        return GpuSwap(mem.Load(indexBase + vertexId * 4), endian) + vertexOffset;
    uint addr = indexBase + vertexId * 2;
    uint w = GpuSwap(mem.Load(addr & ~3u), endian);
    return ((addr & 2) ? (w >> 16) : (w & 0xFFFF)) + vertexOffset;
}

float SNorm(uint v, uint bits) { int s = int(v << (32 - bits)) >> (32 - bits); return max(float(s) / float((1u << (bits - 1)) - 1), -1.0); }
float UNorm(uint v, uint bits) { return float(v) / float((1u << bits) - 1); }
float SInt(uint v, uint bits) { return float(int(v << (32 - bits)) >> (32 - bits)); }

// Le um elemento de vertice: slot = indice da fetch constant (0-95), offset/stride em dwords
float4 VFetch(uint index, uint slot, uint offset, uint stride, uint format, bool isSigned, bool isInteger, int expAdjust)
{
    uint2 f = (slot & 1) ? vfetch[slot >> 1].zw : vfetch[slot >> 1].xy;
    uint base = f.x & 0xFFFFFFFCu;
    uint endian = f.y & 3;
    uint addr = base + (index * stride + offset) * 4;
    uint w0 = GpuSwap(mem.Load(addr), endian);
    float4 r = float4(0, 0, 0, 1);
    switch (format)
    {
    // os formatos do Xenos sao empacotados a partir do bit 0: x nos bits mais baixos
    case 6:  // 8_8_8_8
        if (isInteger) r = isSigned ? float4(SInt(w0, 8), SInt(w0 >> 8, 8), SInt(w0 >> 16, 8), SInt(w0 >> 24, 8))
                                    : float4(w0 & 255, (w0 >> 8) & 255, (w0 >> 16) & 255, w0 >> 24);
        else r = isSigned ? float4(SNorm(w0, 8), SNorm(w0 >> 8, 8), SNorm(w0 >> 16, 8), SNorm(w0 >> 24, 8))
                          : float4(UNorm(w0 & 255, 8), UNorm((w0 >> 8) & 255, 8), UNorm((w0 >> 16) & 255, 8), UNorm(w0 >> 24, 8));
        break;
    case 7:  // 2_10_10_10 (x = bits baixos)
        r = isSigned ? float4(SNorm(w0, 10), SNorm(w0 >> 10, 10), SNorm(w0 >> 20, 10), SNorm(w0 >> 30, 2))
                     : float4(UNorm(w0 & 1023, 10), UNorm((w0 >> 10) & 1023, 10), UNorm((w0 >> 20) & 1023, 10), UNorm(w0 >> 30, 2));
        if (isInteger) r = isSigned ? float4(SInt(w0, 10), SInt(w0 >> 10, 10), SInt(w0 >> 20, 10), SInt(w0 >> 30, 2))
                                    : float4(w0 & 1023, (w0 >> 10) & 1023, (w0 >> 20) & 1023, w0 >> 30);
        break;
    case 16: // 10_11_11 (x = 11 bits baixos, y = 11, z = 10)
        r.xyz = isSigned ? float3(SNorm(w0, 11), SNorm(w0 >> 11, 11), SNorm(w0 >> 22, 10))
                         : float3(UNorm(w0 & 2047, 11), UNorm((w0 >> 11) & 2047, 11), UNorm(w0 >> 22, 10));
        if (isInteger) r.xyz = isSigned ? float3(SInt(w0, 11), SInt(w0 >> 11, 11), SInt(w0 >> 22, 10))
                                        : float3(w0 & 2047, (w0 >> 11) & 2047, w0 >> 22);
        break;
    case 17: // 11_11_10 (x = 10 bits baixos, y = 11, z = 11)
        r.xyz = isSigned ? float3(SNorm(w0, 10), SNorm(w0 >> 10, 11), SNorm(w0 >> 21, 11))
                         : float3(UNorm(w0 & 1023, 10), UNorm((w0 >> 10) & 2047, 11), UNorm(w0 >> 21, 11));
        if (isInteger) r.xyz = isSigned ? float3(SInt(w0, 10), SInt(w0 >> 10, 11), SInt(w0 >> 21, 11))
                                        : float3(w0 & 1023, (w0 >> 10) & 2047, w0 >> 21);
        break;
    case 34: // 32_32 (inteiro)
    {
        uint w1 = GpuSwap(mem.Load(addr + 4), endian);
        r.xy = isSigned ? float2(int(w0), int(w1)) : float2(w0, w1);
        break;
    }
    case 35: // 32_32_32_32 (inteiro)
    {
        uint4 w = uint4(w0, GpuSwap(mem.Load(addr + 4), endian), GpuSwap(mem.Load(addr + 8), endian), GpuSwap(mem.Load(addr + 12), endian));
        r = isSigned ? float4(int4(w)) : float4(w);
        break;
    }
    case 25: // 16_16
        r.xy = isInteger ? (isSigned ? float2(SInt(w0, 16), SInt(w0 >> 16, 16)) : float2(w0 & 0xFFFF, w0 >> 16))
                         : (isSigned ? float2(SNorm(w0, 16), SNorm(w0 >> 16, 16)) : float2(UNorm(w0 & 0xFFFF, 16), UNorm(w0 >> 16, 16)));
        r.zw = float2(0, 1);
        break;
    case 26: // 16_16_16_16
    {
        uint w1 = GpuSwap(mem.Load(addr + 4), endian);
        if (isInteger) r = isSigned ? float4(SInt(w0, 16), SInt(w0 >> 16, 16), SInt(w1, 16), SInt(w1 >> 16, 16))
                                    : float4(w0 & 0xFFFF, w0 >> 16, w1 & 0xFFFF, w1 >> 16);
        else r = isSigned ? float4(SNorm(w0, 16), SNorm(w0 >> 16, 16), SNorm(w1, 16), SNorm(w1 >> 16, 16))
                          : float4(UNorm(w0 & 0xFFFF, 16), UNorm(w0 >> 16, 16), UNorm(w1 & 0xFFFF, 16), UNorm(w1 >> 16, 16));
        break;
    }
    case 31: // 16_16_FLOAT
        r = float4(f16tof32(w0 & 0xFFFF), f16tof32(w0 >> 16), 0, 1);
        break;
    case 32: // 16_16_16_16_FLOAT
    {
        uint w1 = GpuSwap(mem.Load(addr + 4), endian);
        r = float4(f16tof32(w0 & 0xFFFF), f16tof32(w0 >> 16), f16tof32(w1 & 0xFFFF), f16tof32(w1 >> 16));
        break;
    }
    case 33: // 32 (inteiro)
        r = float4(isSigned ? float(int(w0)) : float(w0), 0, 0, 1);
        break;
    case 36: // 32_FLOAT
        r = float4(asfloat(w0), 0, 0, 1);
        break;
    case 37: // 32_32_FLOAT
        r = float4(asfloat(w0), asfloat(GpuSwap(mem.Load(addr + 4), endian)), 0, 1);
        break;
    case 57: // 32_32_32_FLOAT
        r = float4(asfloat(w0), asfloat(GpuSwap(mem.Load(addr + 4), endian)), asfloat(GpuSwap(mem.Load(addr + 8), endian)), 1);
        break;
    case 38: // 32_32_32_32_FLOAT
        r = float4(asfloat(w0), asfloat(GpuSwap(mem.Load(addr + 4), endian)), asfloat(GpuSwap(mem.Load(addr + 8), endian)),
                   asfloat(GpuSwap(mem.Load(addr + 12), endian)));
        break;
    }
    return r * exp2(float(expAdjust));
}

bool BoolConst(uint i) { return (bools[i >> 7][(i >> 5) & 3] >> (i & 31)) & 1; }
uint LoopCount(uint i) { return loops[i >> 2][i & 3] & 0xFF; }

// Tamanho da textura como o jogo ve: imagens da tela sao texScale vezes maiores na placa, mas as coordenadas em
// texels e os deslocamentos do jogo sao no tamanho dele. O mapa de sombras nao e marcado: o filtro dele usa o tamanho real.
float2 TexSize(uint slot)
{
    uint w, h;
    tex[slot].GetDimensions(w, h);
    float2 s = float2(w, h);
    return ((scaledTex >> slot) & 1u) ? s / texScale : s;
}

float4 TFetch2D(uint slot, float2 uv, bool denorm)
{
    if (denorm) uv /= TexSize(slot);
#ifdef NOTEX
    return float4(0.5, 0.5, 0.5, 1);   // depuracao: mede o custo da leitura de texturas
#endif
#ifdef XE_VERTEX
    return tex[slot].SampleLevel(samp[slot], uv, 0);   // shader de vertices: sem derivadas
#else
    return tex[slot].Sample(samp[slot], uv);
#endif
}

// Leitura com deslocamento em texels (OffsetX/Y da instrucao; usado, por exemplo, no PCF das sombras)
float4 TFetch2DOff(uint slot, float2 uv, bool denorm, float2 off)
{
    float2 size = TexSize(slot);
    if (denorm) uv /= size;
    uv += off / size;
#ifdef XE_VERTEX
    return tex[slot].SampleLevel(samp[slot], uv, 0);
#else
    return tex[slot].Sample(samp[slot], uv);
#endif
}

// getWeights: fracoes da interpolacao bilinear na posicao da leitura; w (mip) = 0
float4 TWeights(uint slot, float2 uv, bool denorm, float2 off)
{
    float2 t = denorm ? uv : uv * TexSize(slot);
    return float4(frac(t + off - 0.5 + 1.5 / 1024.0), 0, 0);
}

#ifndef XE_VERTEX
// getCompTexLOD: nivel de detalhe que a leitura usaria
float4 TComputedLod(uint slot, float2 uv, bool denorm)
{
    if (denorm) uv /= TexSize(slot);
    return tex[slot].CalculateLevelOfDetailUnclamped(samp[slot], uv).xxxx;
}
#endif

// Instrucao "cube" do Xenos: a entrada vem com a ordem .zzxy (z em x, x em z, y em w).
// Resultado: (TC, SC, 2 * eixo principal, face 0-5: +X -X +Y -Y +Z -Z)
float4 XeCube(float4 s)
{
    float z = s.x, x = s.z, y = s.w;
    if (abs(z) >= abs(x) && abs(z) >= abs(y)) return float4(-y, z < 0 ? -x : x, 2.0 * z, z < 0 ? 5.0 : 4.0);
    if (abs(y) >= abs(x)) return float4(y < 0 ? -z : z, x, 2.0 * y, y < 0 ? 3.0 : 2.0);
    return float4(-y, x < 0 ? z : -z, 2.0 * x, x < 0 ? 1.0 : 0.0);
}

// Leitura de cubo: c = (SC, TC, face) com SC/TC entre 1 e 2 (o jogo soma 1.5 depois de dividir pelo eixo);
// volta para uma direcao 3D, do jeito oposto ao da instrucao cube
float4 TFetchCube(uint slot, float3 c)
{
    float sc = c.x * 2.0 - 3.0, tc = c.y * 2.0 - 3.0;
    uint face = min(uint(max(c.z, 0.0)), 5u);
    bool neg = (face & 1u) != 0;
    float3 dir;
    if ((face >> 1) == 0) dir = float3(neg ? -1.0 : 1.0, -tc, neg ? sc : -sc);
    else if ((face >> 1) == 1) dir = float3(sc, neg ? -1.0 : 1.0, neg ? -tc : tc);
    else dir = float3(neg ? -sc : sc, -tc, neg ? -1.0 : 1.0);
#ifdef XE_VERTEX
    return texCube[slot].SampleLevel(samp[slot], dir, 0);
#else
    return texCube[slot].Sample(samp[slot], dir);
#endif
}

float4 dst(float4 a, float4 b) { return float4(1, a.y * b.y, a.z, b.w); }
float max4(float4 v) { return max(max(v.x, v.y), max(v.z, v.w)); }
)HLSL";
