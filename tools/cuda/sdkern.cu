// Kernels do Stable Diffusion 1.5 em CUDA C (UNet, ControlNet, VAE), compilados para PTX por tools/cuda/ptxgen (NVRTC, so no
// desenvolvimento) e embutidos em src/engine/cu_sdkern.inc. Sem cabecalhos do SDK. Regenerar: tools/cuda/gen.sh
// Convencao: ativacoes F32 NHWC na VRAM; pesos F16 [saida][Kp] (k*k*entrada reordenado em ky, kx, entrada; zeros ate Kp, multiplo de 32);
// produtos com mma.sync f16 (entrada convertida para F16 ao carregar o ladrilho), acumulador F32.
typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64;
#define EXT extern "C" __global__

__device__ __forceinline__ u32 pack2(float lo, float hi) { u32 r; asm("cvt.rn.f16x2.f32 %0,%1,%2;" : "=r"(r) : "f"(hi), "f"(lo)); return r; }
__device__ __forceinline__ float h2f(u16 h) { float f; asm("cvt.f32.f16 %0,%1;" : "=f"(f) : "h"(h)); return f; }
__device__ __forceinline__ u16 f2h(float f) { u16 h; asm("cvt.rn.f16.f32 %0,%1;" : "=h"(h) : "f"(f)); return h; }
__device__ __forceinline__ u32 smaddr(const void *p) { u64 a; asm("cvta.to.shared.u64 %0,%1;" : "=l"(a) : "l"(p)); return (u32)a; }
__device__ __forceinline__ void mma16816(float *c, u32 a0, u32 a1, u32 a2, u32 a3, u32 b0, u32 b1) {
    asm volatile("mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%0,%1,%2,%3},{%4,%5,%6,%7},{%8,%9},{%0,%1,%2,%3};"
                 : "+f"(c[0]), "+f"(c[1]), "+f"(c[2]), "+f"(c[3]) : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(b0), "r"(b1));
}
__device__ __forceinline__ void ldm4(u32 &r0, u32 &r1, u32 &r2, u32 &r3, u32 a) {
    asm volatile("ldmatrix.sync.aligned.m8n8.x4.shared.b16 {%0,%1,%2,%3},[%4];" : "=r"(r0), "=r"(r1), "=r"(r2), "=r"(r3) : "r"(a) : "memory");
}
__device__ __forceinline__ void ldm4t(u32 &r0, u32 &r1, u32 &r2, u32 &r3, u32 a) {
    asm volatile("ldmatrix.sync.aligned.m8n8.x4.trans.shared.b16 {%0,%1,%2,%3},[%4];" : "=r"(r0), "=r"(r1), "=r"(r2), "=r"(r3) : "r"(a) : "memory");
}

__device__ float block_sum(float v, float *red) {
    for (int o = 16; o; o >>= 1) v += __shfl_xor_sync(0xffffffffu, v, o);
    const int w = threadIdx.x >> 5, l = threadIdx.x & 31;
    __syncthreads();
    if (!l) red[w] = v;
    __syncthreads();
    float s = 0; for (int i = 0; i < (int)(blockDim.x >> 5); i++) s += red[i];
    return s;
}

// Coeficientes da norma por grupos (32 grupos): y = silu(ca[c] * x + cb[c]); somas em double como na CPU. Um bloco por grupo.
EXT void gn_coef(const float *X, int n, int C, const float *gamma, const float *beta, float eps, float *ca, float *cb) {
    __shared__ double rs[256], rs2[256];
    const int g = blockIdx.x, cpg = C / 32;
    double s = 0, s2 = 0;
    for (long i = threadIdx.x; i < (long)n * cpg; i += blockDim.x) { const long p = i / cpg; const int c = (int)(i - p * cpg); const double v = X[p * C + g * cpg + c]; s += v; s2 += v * v; }
    rs[threadIdx.x] = s; rs2[threadIdx.x] = s2; __syncthreads();
    for (int o = 128; o; o >>= 1) { if ((int)threadIdx.x < o) { rs[threadIdx.x] += rs[threadIdx.x + o]; rs2[threadIdx.x] += rs2[threadIdx.x + o]; } __syncthreads(); }
    const double cnt = (double)n * cpg, mean = rs[0] / cnt, var = rs2[0] / cnt - mean * mean;
    const float inv = (float)(1.0 / sqrt((var > 0 ? var : 0) + (double)eps)), m = (float)mean;
    if ((int)threadIdx.x < cpg) { const int ch = g * cpg + threadIdx.x; ca[ch] = gamma[ch] * inv; cb[ch] = beta[ch] - m * gamma[ch] * inv; }
}

// C[pix][cout] = (bias + sum_k A[pix][k] W[cout][k]) * oscale + res; k = (ky*ks + kx)*Cin + ci. A vem de X [Hin*Win][Cin] F32 (ampliado 2x
// se up) com y = silu(ca*x + cb) opcional (zeros fora da imagem DEPOIS da transformacao). H, W: saida; LH, LW: tamanho logico da entrada
// (o da imagem ampliada, se up); passo stride e preenchimento pt (cima) e pl (esquerda). Saida F32 ou F16 (out16). Ladrilho 128 x 128 x 32.
EXT __launch_bounds__(256) void sconv(const float *X, const u16 *Wh, const float *bias, const float *res, void *Yv, const float *ca, const float *cb, int silu,
                                      int H, int W, int Cin, int Cout, int ks, int up, int Kp, int stride, int pt, int pl, int LH, int LW, float oscale, int out16) {
    extern __shared__ __align__(16) unsigned char sm[];
    const int tid = threadIdx.x, warp = tid >> 5, lane = tid & 31, wm = warp >> 2, wn = warp & 3;
    const int npix = H * W, p0 = blockIdx.y * 128, c0 = blockIdx.x * 128;
    const int K = ks * ks * Cin, nkt = (K + 31) / 32, Win = up ? LW >> 1 : LW;
    const u32 sb = smaddr(sm), sAh = sb, sBh = sb + 8192;
    const int row = tid >> 1, half = tid & 1, p = p0 + row, co = c0 + row, oy = p / W, ox = p - oy * W;
    const bool pv = p < npix, cv = co < Cout, tf = ca || silu;
    float acc[16][4]; for (int i = 0; i < 16; i++) for (int j = 0; j < 4; j++) acc[i][j] = 0.0f;
    float av[16]; uint4 bh0, bh1;
    auto fetch = [&](int kt) {
        const int k0 = kt * 32 + half * 16;
        if ((Cin & 15) == 0) {
            bool ok = pv && k0 < K; float4 v0 = make_float4(0, 0, 0, 0), v1 = v0, v2 = v0, v3 = v0; int ci = 0;
            if (ok) {
                const int cell = k0 / Cin; ci = k0 - cell * Cin; const int ky = cell / ks, kx = cell - ky * ks;
                int iy = oy * stride + ky - pt, ix = ox * stride + kx - pl;
                if (iy < 0 || iy >= LH || ix < 0 || ix >= LW) ok = false;
                else {
                    if (up) { iy >>= 1; ix >>= 1; }
                    const float4 *s = (const float4 *)(X + ((size_t)iy * Win + ix) * Cin + ci);
                    v0 = s[0]; v1 = s[1]; v2 = s[2]; v3 = s[3];
                }
            }
            float t[16] = { v0.x, v0.y, v0.z, v0.w, v1.x, v1.y, v1.z, v1.w, v2.x, v2.y, v2.z, v2.w, v3.x, v3.y, v3.z, v3.w };
            for (int e = 0; e < 16; e++) {
                float v = t[e];
                if (ok && tf) { if (ca) v = ca[ci + e] * v + cb[ci + e]; if (silu) v = v / (1.0f + __expf(-v)); }
                av[e] = ok ? v : 0.0f;
            }
        } else {
            for (int e = 0; e < 16; e++) {
                const int kk = k0 + e; float v = 0;
                if (pv && kk < K) {
                    const int cell = kk / Cin, ci = kk - cell * Cin, ky = cell / ks, kx = cell - ky * ks;
                    int iy = oy * stride + ky - pt, ix = ox * stride + kx - pl;
                    if (!(iy < 0 || iy >= LH || ix < 0 || ix >= LW)) {
                        if (up) { iy >>= 1; ix >>= 1; }
                        v = X[((size_t)iy * Win + ix) * Cin + ci];
                        if (tf) { if (ca) v = ca[ci] * v + cb[ci]; if (silu) v = v / (1.0f + __expf(-v)); }
                    }
                }
                av[e] = v;
            }
        }
        const uint4 z = make_uint4(0, 0, 0, 0);
        const size_t o = (size_t)co * Kp + kt * 32 + half * 16;
        bh0 = cv ? *(const uint4 *)(Wh + o) : z; bh1 = cv ? *(const uint4 *)(Wh + o + 8) : z;
    };
    auto store = [&]() {
        const int o0 = row * 64 + (((half * 2) ^ ((row >> 1) & 3)) << 4), o1 = row * 64 + (((half * 2 + 1) ^ ((row >> 1) & 3)) << 4);
        u32 h[8];
        for (int e = 0; e < 8; e++) h[e] = pack2(av[2 * e], av[2 * e + 1]);
        asm volatile("st.shared.v4.b32 [%0],{%1,%2,%3,%4};" :: "r"(sAh + o0), "r"(h[0]), "r"(h[1]), "r"(h[2]), "r"(h[3]) : "memory");
        asm volatile("st.shared.v4.b32 [%0],{%1,%2,%3,%4};" :: "r"(sAh + o1), "r"(h[4]), "r"(h[5]), "r"(h[6]), "r"(h[7]) : "memory");
        asm volatile("st.shared.v4.b32 [%0],{%1,%2,%3,%4};" :: "r"(sBh + o0), "r"(bh0.x), "r"(bh0.y), "r"(bh0.z), "r"(bh0.w) : "memory");
        asm volatile("st.shared.v4.b32 [%0],{%1,%2,%3,%4};" :: "r"(sBh + o1), "r"(bh1.x), "r"(bh1.y), "r"(bh1.z), "r"(bh1.w) : "memory");
    };
    fetch(0);
    for (int kt = 0; kt < nkt; kt++) {
        __syncthreads();
        store();
        __syncthreads();
        if (kt + 1 < nkt) fetch(kt + 1);
        for (int kk = 0; kk < 2; kk++) {
            u32 af[4][4], bf[2][4];
            for (int mi = 0; mi < 4; mi++) {
                const int r = wm * 64 + mi * 16 + (lane & 7) + (((lane >> 3) & 1) << 3), ch = 2 * kk + (lane >> 4);
                ldm4(af[mi][0], af[mi][1], af[mi][2], af[mi][3], sAh + r * 64 + ((ch ^ ((r >> 1) & 3)) << 4));
            }
            for (int nj = 0; nj < 2; nj++) {
                const int r = wn * 32 + nj * 16 + (lane & 7) + ((lane >> 4) << 3), ch = 2 * kk + ((lane >> 3) & 1);
                ldm4(bf[nj][0], bf[nj][1], bf[nj][2], bf[nj][3], sBh + r * 64 + ((ch ^ ((r >> 1) & 3)) << 4));
            }
            for (int mi = 0; mi < 4; mi++) for (int ni = 0; ni < 4; ni++) {
                const int nj = ni >> 1, h2 = (ni & 1) * 2;
                mma16816(acc[mi * 4 + ni], af[mi][0], af[mi][1], af[mi][2], af[mi][3], bf[nj][h2], bf[nj][h2 + 1]);
            }
        }
    }
    for (int mi = 0; mi < 4; mi++) for (int ni = 0; ni < 4; ni++) for (int hh = 0; hh < 2; hh++) {
        const int r = p0 + wm * 64 + mi * 16 + (lane >> 2) + hh * 8, c = c0 + wn * 32 + ni * 8 + 2 * (lane & 3);
        if (r >= npix) continue;
        for (int e = 0; e < 2; e++) {
            if (c + e >= Cout) continue;
            float v = (acc[mi * 4 + ni][hh * 2 + e] + (bias ? bias[c + e] : 0.0f)) * oscale;
            if (res) v += res[(size_t)r * Cout + c + e];
            if (out16) ((u16 *)Yv)[(size_t)r * Cout + c + e] = f2h(v); else ((float *)Yv)[(size_t)r * Cout + c + e] = v;
        }
    }
}

// Atencao de 8 cabecas sem mascara (flash: nada de matriz nq x nk): Q [nq][ldq], K [nk][ldk], V [nk][ldv] F16 (cabeca h nas colunas h*HD..),
// O F32 [nq][ldo]. HD = tamanho da cabeca (40, 80, 160), HDP = HD arredondado a multiplo de 16. Um bloco = 64 consultas (4 warps de 16)
// de uma cabeca; ladrilhos de KT chaves em memoria compartilhada com passo de linha HDP*2+16 bytes (sem conflito no ldmatrix).
template <int HD, int HDP, int KT>
__device__ __forceinline__ void flash_impl(const u16 *Q, int ldq, const u16 *K, int ldk, const u16 *V, int ldv, float *O, int ldo, int nq, int nk, float sl2) {
    extern __shared__ __align__(16) unsigned char sm[];
    constexpr int PITCH = HDP * 2 + 16, NS = HDP / 16, NO = HDP / 8, CHP = HDP / 8;
    const int h = blockIdx.y, q0 = blockIdx.x * 64, tid = threadIdx.x, warp = tid >> 5, lane = tid & 31;
    const size_t hoff = (size_t)h * HD;
    const u32 sb = smaddr(sm), vbase = sb + KT * PITCH;
    u32 qf[NS][4];
    {
        int r0 = q0 + warp * 16 + (lane >> 2), r1 = r0 + 8; if (r0 > nq - 1) r0 = nq - 1; if (r1 > nq - 1) r1 = nq - 1;
        for (int ks = 0; ks < NS; ks++) {
            const int col = ks * 16 + 2 * (lane & 3);
            qf[ks][0] = col < HD ? *(const u32 *)(Q + (size_t)r0 * ldq + hoff + col) : 0u;
            qf[ks][1] = col < HD ? *(const u32 *)(Q + (size_t)r1 * ldq + hoff + col) : 0u;
            qf[ks][2] = col + 8 < HD ? *(const u32 *)(Q + (size_t)r0 * ldq + hoff + col + 8) : 0u;
            qf[ks][3] = col + 8 < HD ? *(const u32 *)(Q + (size_t)r1 * ldq + hoff + col + 8) : 0u;
        }
    }
    float oa[NO][4]; for (int i = 0; i < NO; i++) for (int j = 0; j < 4; j++) oa[i][j] = 0.0f;
    float mx0 = -1e30f, mx1 = -1e30f, l0 = 0.0f, l1 = 0.0f;
    const int nt = (nk + KT - 1) / KT;
    for (int kt = 0; kt < nt; kt++) {
        __syncthreads();
        for (int idx = tid; idx < KT * CHP; idx += 128) {
            const int row = idx / CHP, c = idx - row * CHP, key = kt * KT + row;
            uint4 kv = make_uint4(0, 0, 0, 0), vv = kv;
            if (key < nk && c * 8 < HD) { kv = *(const uint4 *)(K + (size_t)key * ldk + hoff + c * 8); vv = *(const uint4 *)(V + (size_t)key * ldv + hoff + c * 8); }
            *(uint4 *)(sm + row * PITCH + c * 16) = kv; *(uint4 *)(sm + KT * PITCH + row * PITCH + c * 16) = vv;
        }
        __syncthreads();
        float s[KT / 8][4]; for (int i = 0; i < KT / 8; i++) for (int j = 0; j < 4; j++) s[i][j] = 0.0f;
        for (int jp = 0; jp < KT / 16; jp++)
            for (int ks = 0; ks < NS; ks++) {
                const int key = jp * 16 + (lane & 7) + ((lane >> 4) << 3), ch = 2 * ks + ((lane >> 3) & 1);
                u32 b0, b1, b2, b3;
                ldm4(b0, b1, b2, b3, sb + key * PITCH + ch * 16);
                mma16816(s[2 * jp], qf[ks][0], qf[ks][1], qf[ks][2], qf[ks][3], b0, b1);
                mma16816(s[2 * jp + 1], qf[ks][0], qf[ks][1], qf[ks][2], qf[ks][3], b2, b3);
            }
        float m0 = -1e30f, m1 = -1e30f;
        for (int j = 0; j < KT / 8; j++) {
            const int k0 = kt * KT + j * 8 + 2 * (lane & 3);
            s[j][0] = k0 < nk ? s[j][0] * sl2 : -1e30f; s[j][1] = k0 + 1 < nk ? s[j][1] * sl2 : -1e30f;
            s[j][2] = k0 < nk ? s[j][2] * sl2 : -1e30f; s[j][3] = k0 + 1 < nk ? s[j][3] * sl2 : -1e30f;
            m0 = fmaxf(m0, fmaxf(s[j][0], s[j][1])); m1 = fmaxf(m1, fmaxf(s[j][2], s[j][3]));
        }
        m0 = fmaxf(m0, __shfl_xor_sync(0xffffffffu, m0, 1)); m0 = fmaxf(m0, __shfl_xor_sync(0xffffffffu, m0, 2));
        m1 = fmaxf(m1, __shfl_xor_sync(0xffffffffu, m1, 1)); m1 = fmaxf(m1, __shfl_xor_sync(0xffffffffu, m1, 2));
        const float n0 = fmaxf(mx0, m0), n1 = fmaxf(mx1, m1), al0 = exp2f(mx0 - n0), al1 = exp2f(mx1 - n1);
        mx0 = n0; mx1 = n1; l0 *= al0; l1 *= al1;
        for (int i = 0; i < NO; i++) { oa[i][0] *= al0; oa[i][1] *= al0; oa[i][2] *= al1; oa[i][3] *= al1; }
        u32 pf[KT / 16][4];
        for (int j = 0; j < KT / 8; j++) {
            const float p0 = exp2f(s[j][0] - n0), p1 = exp2f(s[j][1] - n0), p2 = exp2f(s[j][2] - n1), p3 = exp2f(s[j][3] - n1);
            l0 += p0 + p1; l1 += p2 + p3;
            pf[j >> 1][(j & 1) * 2] = pack2(p0, p1); pf[j >> 1][(j & 1) * 2 + 1] = pack2(p2, p3);
        }
        for (int kk = 0; kk < KT / 16; kk++)
            for (int dp = 0; dp < HDP / 16; dp++) {
                const int key = kk * 16 + (lane & 7) + (((lane >> 3) & 1) << 3), ch = dp * 2 + (lane >> 4);
                u32 b0, b1, b2, b3;
                ldm4t(b0, b1, b2, b3, vbase + key * PITCH + ch * 16);
                mma16816(oa[2 * dp], pf[kk][0], pf[kk][1], pf[kk][2], pf[kk][3], b0, b1);
                mma16816(oa[2 * dp + 1], pf[kk][0], pf[kk][1], pf[kk][2], pf[kk][3], b2, b3);
            }
    }
    l0 += __shfl_xor_sync(0xffffffffu, l0, 1); l0 += __shfl_xor_sync(0xffffffffu, l0, 2);
    l1 += __shfl_xor_sync(0xffffffffu, l1, 1); l1 += __shfl_xor_sync(0xffffffffu, l1, 2);
    const float i0 = 1.0f / l0, i1 = 1.0f / l1;
    const int r0 = q0 + warp * 16 + (lane >> 2), r1 = r0 + 8;
    for (int i = 0; i < NO; i++) {
        const int cl = i * 8 + 2 * (lane & 3);
        if (cl >= HD) continue;
        if (r0 < nq) *(float2 *)(O + (size_t)r0 * ldo + hoff + cl) = make_float2(oa[i][0] * i0, oa[i][1] * i0);
        if (r1 < nq) *(float2 *)(O + (size_t)r1 * ldo + hoff + cl) = make_float2(oa[i][2] * i1, oa[i][3] * i1);
    }
}
EXT __launch_bounds__(128) void flash40(const u16 *Q, int ldq, const u16 *K, int ldk, const u16 *V, int ldv, float *O, int ldo, int nq, int nk, float sl2) { flash_impl<40, 48, 64>(Q, ldq, K, ldk, V, ldv, O, ldo, nq, nk, sl2); }
EXT __launch_bounds__(128) void flash80(const u16 *Q, int ldq, const u16 *K, int ldk, const u16 *V, int ldv, float *O, int ldo, int nq, int nk, float sl2) { flash_impl<80, 80, 64>(Q, ldq, K, ldk, V, ldv, O, ldo, nq, nk, sl2); }
EXT __launch_bounds__(128) void flash160(const u16 *Q, int ldq, const u16 *K, int ldk, const u16 *V, int ldv, float *O, int ldo, int nq, int nk, float sl2) { flash_impl<160, 160, 64>(Q, ldq, K, ldk, V, ldv, O, ldo, nq, nk, sl2); }

// LayerNorm por linha (eps dado), F32 -> F32; um bloco por linha
EXT void lnorm(const float *X, float *Y, const float *g, const float *b, int C, float eps) {
    __shared__ float red[32];
    const float *x = X + (size_t)blockIdx.x * C; float *y = Y + (size_t)blockIdx.x * C;
    float s = 0; for (int i = threadIdx.x; i < C; i += blockDim.x) s += x[i];
    const float mean = block_sum(s, red) / (float)C;
    float v = 0; for (int i = threadIdx.x; i < C; i += blockDim.x) { const float d = x[i] - mean; v += d * d; }
    const float inv = rsqrtf(block_sum(v, red) / (float)C + eps);
    for (int i = threadIdx.x; i < C; i += blockDim.x) y[i] = (x[i] - mean) * inv * g[i] + b[i];
}

// GEGLU: Y[r][j] = P[r][j] * gelu(P[r][F + j]) (gelu exato, erf)
EXT void geglu(const float *P, float *Y, int F) {
    const float *p = P + (size_t)blockIdx.x * 2 * F; float *y = Y + (size_t)blockIdx.x * F;
    for (int j = threadIdx.x; j < F; j += blockDim.x) { const float g = p[F + j]; y[j] = p[j] * (0.5f * g * (1.0f + erff(g * 0.70710678118654752f))); }
}

// Y[p] = [A[p][0..Ca), B[p][0..Cb) + Bres[p][0..Cb)] (a concatenacao dos atalhos da UNet; Bres pode ser nulo)
EXT void cat2(const float *A, const float *B, const float *Bres, float *Y, int Ca, int Cb) {
    const size_t p = blockIdx.x; const int C = Ca + Cb;
    for (int c = threadIdx.x; c < C; c += blockDim.x) {
        float v;
        if (c < Ca) v = A[p * Ca + c]; else { v = B[p * Cb + c - Ca]; if (Bres) v += Bres[p * Cb + c - Ca]; }
        Y[p * C + c] = v;
    }
}

EXT void addv(float *Y, const float *X, long n) { for (long i = (long)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += (long)gridDim.x * blockDim.x) Y[i] += X[i]; }

// y[o] = silu?(sum_k W[o][k] x[k] + b1[o] + b2[o]); W F16 [nout][Kp]; um warp por saida (4 por bloco)
EXT void gemv(const u16 *Wh, int Kp, const float *x, const float *b1, const float *b2, float *y, int nout, int K, int silu) {
    const int o = blockIdx.x * 4 + (threadIdx.x >> 5), lane = threadIdx.x & 31;
    if (o >= nout) return;
    float s = 0;
    for (int k = lane * 2; k < K; k += 64) { const u32 w = *(const u32 *)(Wh + (size_t)o * Kp + k); s += h2f((u16)(w & 0xffff)) * x[k] + h2f((u16)(w >> 16)) * x[k + 1]; }
    for (int d = 16; d; d >>= 1) s += __shfl_xor_sync(0xffffffffu, s, d);
    if (!lane) { float v = s + (b1 ? b1[o] : 0.0f) + (b2 ? b2[o] : 0.0f); if (silu) v = v / (1.0f + expf(-v)); y[o] = v; }
}

// softmax por linha no lugar: S[nr][nc] *= scale antes
EXT void softmax_rows(float *S, int nc, float scale) {
    __shared__ float red[32];
    float *s = S + (size_t)blockIdx.x * nc;
    float m = -1e30f; for (int i = threadIdx.x; i < nc; i += blockDim.x) m = fmaxf(m, s[i] * scale);
    for (int o = 16; o; o >>= 1) m = fmaxf(m, __shfl_xor_sync(0xffffffffu, m, o));
    if (!(threadIdx.x & 31)) red[threadIdx.x >> 5] = m;
    __syncthreads(); m = -1e30f; for (int i = 0; i < (int)(blockDim.x >> 5); i++) m = fmaxf(m, red[i]);
    float sum = 0; for (int i = threadIdx.x; i < nc; i += blockDim.x) { const float e = expf(s[i] * scale - m); s[i] = e; sum += e; }
    const float tot = block_sum(sum, red), inv = 1.0f / tot;
    for (int i = threadIdx.x; i < nc; i += blockDim.x) s[i] *= inv;
}

// V^T: out[d][j] (F16, passo ldo) = in[j][d] (F32 [n][C]); ladrilhos de 32 x 32
EXT void transp16(const float *in, u16 *out, int n, int C, int ldo) {
    __shared__ float t[32][33];
    const int j0 = blockIdx.x * 32, d0 = blockIdx.y * 32, tx = threadIdx.x & 31, ty = threadIdx.x >> 5;
    for (int r = ty; r < 32; r += 8) { const int j = j0 + r, d = d0 + tx; t[r][tx] = (j < n && d < C) ? in[(size_t)j * C + d] : 0.0f; }
    __syncthreads();
    for (int r = ty; r < 32; r += 8) { const int d = d0 + r, j = j0 + tx; if (d < C && j < ldo) out[(size_t)d * ldo + j] = f2h(j < n ? t[tx][r] : 0.0f); }
}

// F32 -> F16 (n valores)
EXT void cast16(const float *x, u16 *y, long n) { for (long i = (long)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += (long)gridDim.x * blockDim.x) y[i] = f2h(x[i]); }
