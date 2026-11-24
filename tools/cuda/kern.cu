// Kernels de apoio do katana em CUDA C, compilados para PTX por tools/cuda/ptxgen (NVRTC, so no desenvolvimento) e embutidos em
// src/engine/cu_kern.inc. Sem cabecalhos do SDK: tipos e instrucoes de tensor core por asm inline. Regenerar: tools/cuda/gen.sh
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
__device__ __forceinline__ void cpa16(u32 dst, const void *src) { asm volatile("cp.async.cg.shared.global [%0],[%1],16;" :: "r"(dst), "l"(src) : "memory"); }

// F16 alto e residuo (x ~ hi + lo, 22 bits de mantissa): a base das contas "precisas" (produtos com 2 ou 3 termos)
__device__ __forceinline__ void split16(float x, u16 &hi, u16 &lo) { hi = f2h(x); lo = f2h(x - h2f(hi)); }
__device__ __forceinline__ void split2(float a, float b, u32 &hi, u32 &lo) { // (a em baixo, b em cima)
    const u32 h = pack2(a, b); const float ha = h2f((u16)(h & 0xffff)), hb = h2f((u16)(h >> 16)); hi = h; lo = pack2(a - ha, b - hb);
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

// F32 -> F16 (n valores)
EXT void cast_f32_f16(const float *x, u16 *y, u16 *ylo, long n) {
    for (long i = (long)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += (long)gridDim.x * blockDim.x) { u16 h, l; split16(x[i], h, l); y[i] = h; if (ylo) ylo[i] = l; }
}

// LayerNorm sem parametros (eps 1e-6) e modulacao: Y = LN(X) * (1 + scale) + shift; um bloco por linha; saida F16
EXT void ln_mod(const float *X, u16 *Y, u16 *Ylo, const float *shift, const float *scale, int D) {
    __shared__ float red[32];
    const float *x = X + (size_t)blockIdx.x * D; u16 *y = Y + (size_t)blockIdx.x * D, *yl = Ylo ? Ylo + (size_t)blockIdx.x * D : 0;
    float s = 0; for (int i = threadIdx.x; i < D; i += blockDim.x) s += x[i];
    const float mean = block_sum(s, red) / (float)D;
    float v = 0; for (int i = threadIdx.x; i < D; i += blockDim.x) { const float d = x[i] - mean; v += d * d; }
    const float inv = rsqrtf(block_sum(v, red) / (float)D + 1e-6f);
    for (int i = threadIdx.x; i < D; i += blockDim.x) { u16 h, l; split16((x[i] - mean) * inv * (1.0f + scale[i]) + shift[i], h, l); y[i] = h; if (yl) yl[i] = l; }
}

// q, k, v de um produto fundido src[tok][ld] (q, k, v lado a lado, 3072 cada): RMSNorm por cabeca em q e k (ganhos de 128), RoPE, F16.
// Um bloco por token, uma warp por cabeca (24); cs/sn [tok][128]. Q, K, V apontam ao primeiro token deste grupo (passo 3072).
EXT __launch_bounds__(768) void qkv_prep(const float *src, int ld, u16 *Q, u16 *K, u16 *V, u16 *Ql, u16 *Kl, u16 *Vl, const float *wq, const float *wk, const float *cs, const float *sn) {
    const int tok = blockIdx.x, h = threadIdx.x >> 5, l = threadIdx.x & 31, c = h * 128 + l * 4;
    const float *p = src + (size_t)tok * ld;
    const float4 q = *(const float4 *)(p + c), k = *(const float4 *)(p + 3072 + c), v = *(const float4 *)(p + 6144 + c);
    float sq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w, sk = k.x * k.x + k.y * k.y + k.z * k.z + k.w * k.w;
    for (int o = 16; o; o >>= 1) { sq += __shfl_xor_sync(0xffffffffu, sq, o); sk += __shfl_xor_sync(0xffffffffu, sk, o); }
    const float iq = rsqrtf(sq / 128.0f + 1e-6f), ik = rsqrtf(sk / 128.0f + 1e-6f);
    const float4 gq = *(const float4 *)(wq + l * 4), gk = *(const float4 *)(wk + l * 4);
    const float4 co = *(const float4 *)(cs + (size_t)tok * 128 + l * 4), si = *(const float4 *)(sn + (size_t)tok * 128 + l * 4);
    float a[4] = { q.x * iq * gq.x, q.y * iq * gq.y, q.z * iq * gq.z, q.w * iq * gq.w };
    float b[4] = { k.x * ik * gk.x, k.y * ik * gk.y, k.z * ik * gk.z, k.w * ik * gk.w };
    const float cc[4] = { co.x, co.y, co.z, co.w }, ss[4] = { si.x, si.y, si.z, si.w };
    float ra[4], rb[4];
    for (int i = 0; i < 4; i += 2) {
        ra[i] = a[i] * cc[i] - a[i + 1] * ss[i]; ra[i + 1] = a[i + 1] * cc[i + 1] + a[i] * ss[i + 1];
        rb[i] = b[i] * cc[i] - b[i + 1] * ss[i]; rb[i + 1] = b[i + 1] * cc[i + 1] + b[i] * ss[i + 1];
    }
    const size_t o = (size_t)tok * 3072 + c;
    u32 h0, l0, h1, l1;
    split2(ra[0], ra[1], h0, l0); split2(ra[2], ra[3], h1, l1); *(uint2 *)(Q + o) = make_uint2(h0, h1); if (Ql) *(uint2 *)(Ql + o) = make_uint2(l0, l1);
    split2(rb[0], rb[1], h0, l0); split2(rb[2], rb[3], h1, l1); *(uint2 *)(K + o) = make_uint2(h0, h1); if (Kl) *(uint2 *)(Kl + o) = make_uint2(l0, l1);
    split2(v.x, v.y, h0, l0); split2(v.z, v.w, h1, l1); *(uint2 *)(V + o) = make_uint2(h0, h1); if (Vl) *(uint2 *)(Vl + o) = make_uint2(l0, l1);
}

// SwiGLU: Y[r][i] = silu(H[r][i]) * H[r][M + i], H F32 com passo ldh, Y F16 com passo ldy
EXT void swiglu(const float *H, int ldh, u16 *Y, u16 *Ylo, int ldy, int M) {
    const float *h = H + (size_t)blockIdx.x * ldh; u16 *y = Y + (size_t)blockIdx.x * ldy, *yl = Ylo ? Ylo + (size_t)blockIdx.x * ldy : 0;
    for (int i = threadIdx.x; i < M; i += blockDim.x) { const float g = h[i]; u16 a, b; split16(g / (1.0f + __expf(-g)) * h[M + i], a, b); y[i] = a; if (yl) yl[i] = b; }
}

// atencao sem mascara, 24 cabecas de 128, Q K V [N][3072] F16 (cabeca h nas colunas 128h..), O F16 com passo ldo.
// Online softmax (nada de matriz N x N), mma m16n8k16 para QK^T e PV; um bloco = 64 consultas (4 warps de 16) de uma cabeca.
// PR = precisa: Q, K, V e P em duas metades (alto + residuo, vindos de qkv_prep) e produtos de 3 termos (hi*hi + lo*hi + hi*lo).
template <bool PR, int KT>
__device__ __forceinline__ void attn_impl(const u16 *Q, const u16 *K, const u16 *V, const u16 *Ql, const u16 *Kl, const u16 *Vl, u16 *O, u16 *Ol, int N, int ldo, float scale_log2) {
    extern __shared__ __align__(16) unsigned char sm[];
    const int h = blockIdx.y, q0 = blockIdx.x * 64, tid = threadIdx.x, warp = tid >> 5, lane = tid & 31;
    const size_t ld = 3072, hoff = (size_t)h * 128;
    const u32 sb = smaddr(sm);
    u32 qf[8][4], qlf[8][4];
    {
        int r0 = q0 + warp * 16 + (lane >> 2), r1 = r0 + 8; if (r0 > N - 1) r0 = N - 1; if (r1 > N - 1) r1 = N - 1;
        for (int ks = 0; ks < 8; ks++) {
            const int col = ks * 16 + 2 * (lane & 3);
            qf[ks][0] = *(const u32 *)(Q + r0 * ld + hoff + col); qf[ks][1] = *(const u32 *)(Q + r1 * ld + hoff + col);
            qf[ks][2] = *(const u32 *)(Q + r0 * ld + hoff + col + 8); qf[ks][3] = *(const u32 *)(Q + r1 * ld + hoff + col + 8);
            if (PR) {
                qlf[ks][0] = *(const u32 *)(Ql + r0 * ld + hoff + col); qlf[ks][1] = *(const u32 *)(Ql + r1 * ld + hoff + col);
                qlf[ks][2] = *(const u32 *)(Ql + r0 * ld + hoff + col + 8); qlf[ks][3] = *(const u32 *)(Ql + r1 * ld + hoff + col + 8);
            }
        }
    }
    float oa[16][4]; for (int i = 0; i < 16; i++) for (int j = 0; j < 4; j++) oa[i][j] = 0.0f;
    float mx0 = -1e30f, mx1 = -1e30f, l0 = 0.0f, l1 = 0.0f;
    const int nt = (N + KT - 1) / KT;
    const int TB = KT * 256, NTL = PR ? 4 : 2, STB = TB * NTL;      // bytes por ladrilho (K ou V) e por estagio
    auto load_tile = [&](int kt, int stage) {
        for (int i = 0; i < KT * 16 / 128; i++) {
            const int idx = tid + 128 * i, row = idx >> 4, c = idx & 15;
            int key = kt * KT + row; if (key > N - 1) key = N - 1;
            const u32 d = sb + stage * STB + row * 256 + ((c ^ (row & 7)) << 4);
            cpa16(d, K + key * ld + hoff + c * 8);
            cpa16(d + TB, V + key * ld + hoff + c * 8);
            if (PR) { cpa16(d + 2 * TB, Kl + key * ld + hoff + c * 8); cpa16(d + 3 * TB, Vl + key * ld + hoff + c * 8); }
        }
    };
    load_tile(0, 0); asm volatile("cp.async.commit_group;" ::: "memory");
    for (int kt = 0; kt < nt; kt++) {
        if (kt + 1 < nt) load_tile(kt + 1, (kt + 1) & 1);
        asm volatile("cp.async.commit_group;" ::: "memory");
        asm volatile("cp.async.wait_group 1;" ::: "memory");
        __syncthreads();
        const u32 kb = sb + (kt & 1) * STB, vb = kb + TB, klb = kb + 2 * TB, vlb = kb + 3 * TB;
        float s[KT / 8][4]; for (int i = 0; i < KT / 8; i++) for (int j = 0; j < 4; j++) s[i][j] = 0.0f;
        for (int jp = 0; jp < KT / 16; jp++) {
            for (int ks = 0; ks < 8; ks++) {
                const int key = jp * 16 + (lane & 7) + ((lane >> 4) << 3), ch = 2 * ks + ((lane >> 3) & 1);
                const u32 off = key * 256 + ((ch ^ (key & 7)) << 4);
                u32 b0, b1, b2, b3;
                ldm4(b0, b1, b2, b3, kb + off);
                mma16816(s[2 * jp], qf[ks][0], qf[ks][1], qf[ks][2], qf[ks][3], b0, b1);
                mma16816(s[2 * jp + 1], qf[ks][0], qf[ks][1], qf[ks][2], qf[ks][3], b2, b3);
                if (PR) {
                    mma16816(s[2 * jp], qlf[ks][0], qlf[ks][1], qlf[ks][2], qlf[ks][3], b0, b1);
                    mma16816(s[2 * jp + 1], qlf[ks][0], qlf[ks][1], qlf[ks][2], qlf[ks][3], b2, b3);
                    ldm4(b0, b1, b2, b3, klb + off);
                    mma16816(s[2 * jp], qf[ks][0], qf[ks][1], qf[ks][2], qf[ks][3], b0, b1);
                    mma16816(s[2 * jp + 1], qf[ks][0], qf[ks][1], qf[ks][2], qf[ks][3], b2, b3);
                }
            }
        }
        float m0 = -1e30f, m1 = -1e30f;
        for (int j = 0; j < KT / 8; j++) {
            const int k0 = kt * KT + j * 8 + 2 * (lane & 3);
            s[j][0] = k0 < N ? s[j][0] * scale_log2 : -1e30f; s[j][1] = k0 + 1 < N ? s[j][1] * scale_log2 : -1e30f;
            s[j][2] = k0 < N ? s[j][2] * scale_log2 : -1e30f; s[j][3] = k0 + 1 < N ? s[j][3] * scale_log2 : -1e30f;
            m0 = fmaxf(m0, fmaxf(s[j][0], s[j][1])); m1 = fmaxf(m1, fmaxf(s[j][2], s[j][3]));
        }
        m0 = fmaxf(m0, __shfl_xor_sync(0xffffffffu, m0, 1)); m0 = fmaxf(m0, __shfl_xor_sync(0xffffffffu, m0, 2));
        m1 = fmaxf(m1, __shfl_xor_sync(0xffffffffu, m1, 1)); m1 = fmaxf(m1, __shfl_xor_sync(0xffffffffu, m1, 2));
        const float n0 = fmaxf(mx0, m0), n1 = fmaxf(mx1, m1), al0 = exp2f(mx0 - n0), al1 = exp2f(mx1 - n1);
        mx0 = n0; mx1 = n1; l0 *= al0; l1 *= al1;
        for (int i = 0; i < 16; i++) { oa[i][0] *= al0; oa[i][1] *= al0; oa[i][2] *= al1; oa[i][3] *= al1; }
        u32 pf[KT / 16][4], plf[KT / 16][4];
        for (int j = 0; j < KT / 8; j++) {
            const float p0 = exp2f(s[j][0] - n0), p1 = exp2f(s[j][1] - n0), p2 = exp2f(s[j][2] - n1), p3 = exp2f(s[j][3] - n1);
            l0 += p0 + p1; l1 += p2 + p3;
            u32 a, b, c, d; split2(p0, p1, a, b); split2(p2, p3, c, d);
            pf[j >> 1][(j & 1) * 2] = a; pf[j >> 1][(j & 1) * 2 + 1] = c;
            if (PR) { plf[j >> 1][(j & 1) * 2] = b; plf[j >> 1][(j & 1) * 2 + 1] = d; }
        }
        for (int kk = 0; kk < KT / 16; kk++) {
            for (int dp = 0; dp < 8; dp++) {
                const int key = kk * 16 + (lane & 7) + (((lane >> 3) & 1) << 3), ch = dp * 2 + (lane >> 4);
                const u32 off = key * 256 + ((ch ^ (key & 7)) << 4);
                u32 b0, b1, b2, b3;
                ldm4t(b0, b1, b2, b3, vb + off);
                mma16816(oa[2 * dp], pf[kk][0], pf[kk][1], pf[kk][2], pf[kk][3], b0, b1);
                mma16816(oa[2 * dp + 1], pf[kk][0], pf[kk][1], pf[kk][2], pf[kk][3], b2, b3);
                if (PR) {
                    mma16816(oa[2 * dp], plf[kk][0], plf[kk][1], plf[kk][2], plf[kk][3], b0, b1);
                    mma16816(oa[2 * dp + 1], plf[kk][0], plf[kk][1], plf[kk][2], plf[kk][3], b2, b3);
                    ldm4t(b0, b1, b2, b3, vlb + off);
                    mma16816(oa[2 * dp], pf[kk][0], pf[kk][1], pf[kk][2], pf[kk][3], b0, b1);
                    mma16816(oa[2 * dp + 1], pf[kk][0], pf[kk][1], pf[kk][2], pf[kk][3], b2, b3);
                }
            }
        }
        __syncthreads();
    }
    l0 += __shfl_xor_sync(0xffffffffu, l0, 1); l0 += __shfl_xor_sync(0xffffffffu, l0, 2);
    l1 += __shfl_xor_sync(0xffffffffu, l1, 1); l1 += __shfl_xor_sync(0xffffffffu, l1, 2);
    const float i0 = 1.0f / l0, i1 = 1.0f / l1;
    const int r0 = q0 + warp * 16 + (lane >> 2), r1 = r0 + 8;
    for (int i = 0; i < 16; i++) {
        const int col = (int)hoff + i * 8 + 2 * (lane & 3);
        u32 a, b, c, d; split2(oa[i][0] * i0, oa[i][1] * i0, a, b); split2(oa[i][2] * i1, oa[i][3] * i1, c, d);
        if (r0 < N) { *(u32 *)(O + (size_t)r0 * ldo + col) = a; if (Ol) *(u32 *)(Ol + (size_t)r0 * ldo + col) = b; }
        if (r1 < N) { *(u32 *)(O + (size_t)r1 * ldo + col) = c; if (Ol) *(u32 *)(Ol + (size_t)r1 * ldo + col) = d; }
    }
}
EXT __launch_bounds__(128) void attn(const u16 *Q, const u16 *K, const u16 *V, u16 *O, int N, int ldo, float sl) { attn_impl<false, 64>(Q, K, V, 0, 0, 0, O, 0, N, ldo, sl); }
EXT __launch_bounds__(128) void attnx(const u16 *Q, const u16 *K, const u16 *V, const u16 *Ql, const u16 *Kl, const u16 *Vl, u16 *O, u16 *Ol, int N, int ldo, float sl) { attn_impl<true, 32>(Q, K, V, Ql, Kl, Vl, O, Ol, N, ldo, sl); }

// ---------------------------------------------------------------- VAE: convolucao como GEMM implicito, norma por grupos
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

// pesos F32 -> metades F16 [R][Kp] (zeros ate Kp); trans = a fonte e [scols][srows]... out[r][c] = trans ? src[c][r] : src[r][c]
EXT void wsplit(const float *src, long srows, long scols, int trans, int Kp, u16 *Wh, u16 *Wl) {
    const long R = trans ? scols : srows, Cc = trans ? srows : scols;
    for (long i = (long)blockIdx.x * blockDim.x + threadIdx.x; i < R * Kp; i += (long)gridDim.x * blockDim.x) {
        const long r = i / Kp, c = i - r * Kp; float v = 0;
        if (c < Cc) v = trans ? src[c * scols + r] : src[r * scols + c];
        u16 h, l; split16(v, h, l); Wh[i] = h; Wl[i] = l;
    }
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

// C[pix][cout] = bias + res + sum_k A[pix][k] W[cout][k]; k = (ky*ks + kx)*Cin + ci; A vem de X [Hin*Win][Cin] F32 (ampliado 2x se up) com
// y = silu(ca*x + cb) opcional (zeros fora da imagem DEPOIS da transformacao); W em metades F16 [Cout][Kp]. Ladrilho 128 x 128 x 32.
template <bool PR>
__device__ __forceinline__ void conv_impl(const float *X, const u16 *Wh, const u16 *Wl, const float *bias, const float *res, float *Y, const float *ca, const float *cb,
                                          int silu, int H, int W, int Cin, int Cout, int ks, int up, int Kp) {
    extern __shared__ __align__(16) unsigned char sm[];
    const int tid = threadIdx.x, warp = tid >> 5, lane = tid & 31, wm = warp >> 2, wn = warp & 3;
    const int npix = H * W, p0 = blockIdx.y * 128, c0 = blockIdx.x * 128;
    const int K = ks * ks * Cin, nkt = (K + 31) / 32, pad = ks >> 1, Hin = up ? H >> 1 : H, Win = up ? W >> 1 : W;
    const u32 sb = smaddr(sm), sAh = sb, sAl = sb + 8192, sBh = sb + (PR ? 16384 : 8192), sBl = sb + 24576;
    const int row = tid >> 1, half = tid & 1, p = p0 + row, co = c0 + row, oy = p / W, ox = p - oy * W;
    const bool pv = p < npix, cv = co < Cout;
    float acc[16][4]; for (int i = 0; i < 16; i++) for (int j = 0; j < 4; j++) acc[i][j] = 0.0f;
    float av[16]; uint4 bh0, bh1, bl0, bl1;
    auto fetch = [&](int kt) {
        const int k0 = kt * 32 + half * 16;
        const bool fast = (Cin & 15) == 0;
        if (fast) {
            bool ok = pv && k0 < K; float4 v0 = make_float4(0, 0, 0, 0), v1 = v0, v2 = v0, v3 = v0; int ci = 0;
            if (ok) {
                const int cell = k0 / Cin; ci = k0 - cell * Cin; const int ky = cell / ks, kx = cell - ky * ks;
                int iy = oy + ky - pad, ix = ox + kx - pad;
                if (iy < 0 || iy >= H || ix < 0 || ix >= W) ok = false;
                else {
                    if (up) { iy >>= 1; ix >>= 1; }
                    const float4 *s = (const float4 *)(X + ((size_t)iy * Win + ix) * Cin + ci);
                    v0 = s[0]; v1 = s[1]; v2 = s[2]; v3 = s[3];
                }
            }
            float t[16] = { v0.x, v0.y, v0.z, v0.w, v1.x, v1.y, v1.z, v1.w, v2.x, v2.y, v2.z, v2.w, v3.x, v3.y, v3.z, v3.w };
            for (int e = 0; e < 16; e++) {
                float v = t[e];
                if (ok && ca) { v = ca[ci + e] * v + cb[ci + e]; if (silu) v = v / (1.0f + __expf(-v)); }
                av[e] = ok ? v : 0.0f;
            }
        } else {
            for (int e = 0; e < 16; e++) {
                const int kk = k0 + e; float v = 0;
                if (pv && kk < K) {
                    const int cell = kk / Cin, ci = kk - cell * Cin, ky = cell / ks, kx = cell - ky * ks;
                    int iy = oy + ky - pad, ix = ox + kx - pad;
                    if (!(iy < 0 || iy >= H || ix < 0 || ix >= W)) {
                        if (up) { iy >>= 1; ix >>= 1; }
                        v = X[((size_t)iy * Win + ix) * Cin + ci];
                        if (ca) { v = ca[ci] * v + cb[ci]; if (silu) v = v / (1.0f + __expf(-v)); }
                    }
                }
                av[e] = v;
            }
        }
        const uint4 z = make_uint4(0, 0, 0, 0);
        const size_t o = (size_t)co * Kp + kt * 32 + half * 16;
        bh0 = cv ? *(const uint4 *)(Wh + o) : z; bh1 = cv ? *(const uint4 *)(Wh + o + 8) : z;
        if (PR) { bl0 = cv ? *(const uint4 *)(Wl + o) : z; bl1 = cv ? *(const uint4 *)(Wl + o + 8) : z; }
    };
    auto store = [&]() {
        const int o0 = row * 64 + (((half * 2) ^ ((row >> 1) & 3)) << 4), o1 = row * 64 + (((half * 2 + 1) ^ ((row >> 1) & 3)) << 4);
        u32 h[8], l[8];
        for (int e = 0; e < 8; e++) split2(av[2 * e], av[2 * e + 1], h[e], l[e]);
        asm volatile("st.shared.v4.b32 [%0],{%1,%2,%3,%4};" :: "r"(sAh + o0), "r"(h[0]), "r"(h[1]), "r"(h[2]), "r"(h[3]) : "memory");
        asm volatile("st.shared.v4.b32 [%0],{%1,%2,%3,%4};" :: "r"(sAh + o1), "r"(h[4]), "r"(h[5]), "r"(h[6]), "r"(h[7]) : "memory");
        asm volatile("st.shared.v4.b32 [%0],{%1,%2,%3,%4};" :: "r"(sBh + o0), "r"(bh0.x), "r"(bh0.y), "r"(bh0.z), "r"(bh0.w) : "memory");
        asm volatile("st.shared.v4.b32 [%0],{%1,%2,%3,%4};" :: "r"(sBh + o1), "r"(bh1.x), "r"(bh1.y), "r"(bh1.z), "r"(bh1.w) : "memory");
        if (PR) {
            asm volatile("st.shared.v4.b32 [%0],{%1,%2,%3,%4};" :: "r"(sAl + o0), "r"(l[0]), "r"(l[1]), "r"(l[2]), "r"(l[3]) : "memory");
            asm volatile("st.shared.v4.b32 [%0],{%1,%2,%3,%4};" :: "r"(sAl + o1), "r"(l[4]), "r"(l[5]), "r"(l[6]), "r"(l[7]) : "memory");
            asm volatile("st.shared.v4.b32 [%0],{%1,%2,%3,%4};" :: "r"(sBl + o0), "r"(bl0.x), "r"(bl0.y), "r"(bl0.z), "r"(bl0.w) : "memory");
            asm volatile("st.shared.v4.b32 [%0],{%1,%2,%3,%4};" :: "r"(sBl + o1), "r"(bl1.x), "r"(bl1.y), "r"(bl1.z), "r"(bl1.w) : "memory");
        }
    };
    fetch(0);
    for (int kt = 0; kt < nkt; kt++) {
        __syncthreads();
        store();
        __syncthreads();
        if (kt + 1 < nkt) fetch(kt + 1);
        for (int kk = 0; kk < 2; kk++) {
            u32 af[4][4], al[4][4], bf[2][4], bfl[2][4];
            for (int mi = 0; mi < 4; mi++) {
                const int r = wm * 64 + mi * 16 + (lane & 7) + (((lane >> 3) & 1) << 3), ch = 2 * kk + (lane >> 4);
                const u32 off = r * 64 + ((ch ^ ((r >> 1) & 3)) << 4);
                ldm4(af[mi][0], af[mi][1], af[mi][2], af[mi][3], sAh + off);
                if (PR) ldm4(al[mi][0], al[mi][1], al[mi][2], al[mi][3], sAl + off);
            }
            for (int nj = 0; nj < 2; nj++) {
                const int r = wn * 32 + nj * 16 + (lane & 7) + ((lane >> 4) << 3), ch = 2 * kk + ((lane >> 3) & 1);
                const u32 off = r * 64 + ((ch ^ ((r >> 1) & 3)) << 4);
                ldm4(bf[nj][0], bf[nj][1], bf[nj][2], bf[nj][3], sBh + off);
                if (PR) ldm4(bfl[nj][0], bfl[nj][1], bfl[nj][2], bfl[nj][3], sBl + off);
            }
            for (int mi = 0; mi < 4; mi++) for (int ni = 0; ni < 4; ni++) {
                const int nj = ni >> 1, h2 = (ni & 1) * 2;
                mma16816(acc[mi * 4 + ni], af[mi][0], af[mi][1], af[mi][2], af[mi][3], bf[nj][h2], bf[nj][h2 + 1]);
                if (PR) {
                    mma16816(acc[mi * 4 + ni], al[mi][0], al[mi][1], al[mi][2], al[mi][3], bf[nj][h2], bf[nj][h2 + 1]);
                    mma16816(acc[mi * 4 + ni], af[mi][0], af[mi][1], af[mi][2], af[mi][3], bfl[nj][h2], bfl[nj][h2 + 1]);
                }
            }
        }
    }
    for (int mi = 0; mi < 4; mi++) for (int ni = 0; ni < 4; ni++) for (int hh = 0; hh < 2; hh++) {
        const int r = p0 + wm * 64 + mi * 16 + (lane >> 2) + hh * 8, c = c0 + wn * 32 + ni * 8 + 2 * (lane & 3);
        if (r >= npix) continue;
        for (int e = 0; e < 2; e++) {
            if (c + e >= Cout) continue;
            float v = acc[mi * 4 + ni][hh * 2 + e] + (bias ? bias[c + e] : 0.0f);
            if (res) v += res[(size_t)r * Cout + c + e];
            Y[(size_t)r * Cout + c + e] = v;
        }
    }
}
EXT __launch_bounds__(256) void conv(const float *X, const u16 *Wh, const float *bias, const float *res, float *Y, const float *ca, const float *cb, int silu, int H, int W, int Cin, int Cout, int ks, int up, int Kp) {
    conv_impl<false>(X, Wh, 0, bias, res, Y, ca, cb, silu, H, W, Cin, Cout, ks, up, Kp);
}
EXT __launch_bounds__(256) void convx(const float *X, const u16 *Wh, const u16 *Wl, const float *bias, const float *res, float *Y, const float *ca, const float *cb, int silu, int H, int W, int Cin, int Cout, int ks, int up, int Kp) {
    conv_impl<true>(X, Wh, Wl, bias, res, Y, ca, cb, silu, H, W, Cin, Cout, ks, up, Kp);
}
