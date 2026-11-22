/* ---------- produtos inteiros dos tipos de poucos bits (CPU) ----------
   Tipos: IQ2_XXS(16), IQ2_XS(17), IQ3_XXS(18), IQ3_S(21), IQ2_S(22), IQ4_XS(23), IQ4_NL(20) e Q2_0(42).
   Antes estes tipos so tinham dequantizacao: dot_row dequantizava cada bloco em float e multiplicava em float.
   Aqui cada trecho de ate 256 pesos vira 256 inteiros de 16 bits (a busca na grade, o sinal e a escala inteira do
   sub-bloco entram na decodificacao) e o produto com a ativacao de 16 bits (xq.q, a mesma de Q4_K/Q6_K) e feito
   com madd (AVX2) ou dpwssd (VNNI, vetores de 256 ou 512 bits), acumulando em 32 bits. Nos formatos com escala
   inteira por sub-bloco (IQ2/IQ3/IQ4_XS) a escala ja esta nos pesos, entao o bloco de 256 inteiro soma em int32
   (no pior caso 16 produtos por faixa, abaixo de 2^31) e so ha uma conversao para float por bloco. IQ4_NL e Q2_0
   tem escala f16 propria por 32/64 pesos: uma conversao por grupo de 32.
   Em lote (prompt) a linha de pesos e decodificada uma vez e multiplicada pelas ativacoes de ate 8 tokens.
   KATANA_LOWBIT=0 desliga (volta ao caminho em float), 1 = AVX2, 2 = VNNI 256 bits, 3 = AVX-512 (padrao quando a CPU
   tem AVX-512 VNNI; KATANA_LOWBIT_TILE=0 tira o bloco de 4 tokens do lote). O erro e o da quantizacao da ativacao em 16 bits, como nos tipos K. */
#define LB_MAXB 32      /* tokens por grupo no produto em lote */
#if defined(__GNUC__) && defined(__x86_64__) && defined(__AVX2__)
#define HAVE_LOWBIT 1
#define LB_512 __attribute__((target("avx2,fma,f16c,avx512f,avx512bw,avx512vl,avx512vnni")))
#define LB_256 __attribute__((target("avx2,fma,f16c,avx512f,avx512bw,avx512vl,avx512vnni")))

typedef void (*LbDec)(const uint8_t *src, int16_t *w, float *f, int n);
typedef struct { uint32_t type; int unit, ub, fold; LbDec dec, dec5; } LbT;     /* dec5: decodificador AVX-512 (modo 3) */

/* mascara de sinais (0xFF onde o bit esta ligado) para 32 bytes: 4 bytes de sinais, um por grupo de 8 */
static inline __m256i lb_signmask(uint32_t s4) {
    const __m256i sh = _mm256_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3);
    const __m256i bit = _mm256_set1_epi64x((long long)0x8040201008040201ULL);
    const __m256i v = _mm256_shuffle_epi8(_mm256_set1_epi32((int)s4), sh);
    return _mm256_cmpeq_epi8(_mm256_and_si256(v, bit), bit);
}
/* 32 valores de 8 bits (grade com sinal) vezes duas escalas inteiras (16 + 16) -> 32 int16 */
static inline void lb_store32(int16_t *w, __m256i v8, int sa, int sb) {
    const __m256i lo = _mm256_mullo_epi16(_mm256_cvtepi8_epi16(_mm256_castsi256_si128(v8)), _mm256_set1_epi16((short)sa));
    const __m256i hi = _mm256_mullo_epi16(_mm256_cvtepi8_epi16(_mm256_extracti128_si256(v8, 1)), _mm256_set1_epi16((short)sb));
    _mm256_storeu_si256((__m256i *)w, lo);
    _mm256_storeu_si256((__m256i *)(w + 16), hi);
}
static inline __m256i lb_negate(__m256i g, __m256i m) { return _mm256_sub_epi8(_mm256_xor_si256(g, m), m); }
static inline uint32_t lb_signs4(uint32_t a) {      /* 4 indices de 7 bits -> 4 bytes de sinais */
    return (uint32_t)q_ksigns[a & 127] | ((uint32_t)q_ksigns[(a >> 7) & 127] << 8) |
           ((uint32_t)q_ksigns[(a >> 14) & 127] << 16) | ((uint32_t)q_ksigns[(a >> 21) & 127] << 24);
}

static void lb_dec_iq2xxs(const uint8_t *src, int16_t *w, float *f, int n) {
    (void)n; f[0] = h2f(src) * 0.125f;
    for (int ib = 0; ib < 8; ib++) {
        uint32_t a0, a1; memcpy(&a0, src + 2 + 8 * ib, 4); memcpy(&a1, src + 6 + 8 * ib, 4);
        const __m256i g = _mm256_set_epi64x((long long)q_iq2xxs_grid[a0 >> 24], (long long)q_iq2xxs_grid[(a0 >> 16) & 255],
                                            (long long)q_iq2xxs_grid[(a0 >> 8) & 255], (long long)q_iq2xxs_grid[a0 & 255]);
        const int sc = 2 * (int)(a1 >> 28) + 1;
        lb_store32(w + 32 * ib, lb_negate(g, lb_signmask(lb_signs4(a1))), sc, sc);
    }
}
static void lb_dec_iq2xs(const uint8_t *src, int16_t *w, float *f, int n) {
    (void)n; f[0] = h2f(src) * 0.125f;
    const uint8_t *scl = src + 66;
    for (int ib = 0; ib < 8; ib++) {
        uint16_t q[4]; memcpy(q, src + 2 + 8 * ib, 8);
        const __m256i g = _mm256_set_epi64x((long long)q_iq2xs_grid[q[3] & 511], (long long)q_iq2xs_grid[q[2] & 511],
                                            (long long)q_iq2xs_grid[q[1] & 511], (long long)q_iq2xs_grid[q[0] & 511]);
        const uint32_t s4 = (uint32_t)q_ksigns[q[0] >> 9] | ((uint32_t)q_ksigns[q[1] >> 9] << 8) |
                            ((uint32_t)q_ksigns[q[2] >> 9] << 16) | ((uint32_t)q_ksigns[q[3] >> 9] << 24);
        lb_store32(w + 32 * ib, lb_negate(g, lb_signmask(s4)), 2 * (scl[ib] & 15) + 1, 2 * (scl[ib] >> 4) + 1);
    }
}
static void lb_dec_iq2s(const uint8_t *src, int16_t *w, float *f, int n) {
    (void)n; f[0] = h2f(src) * 0.125f;
    const uint8_t *qs = src + 2, *sgn = src + 34, *qh = src + 66, *scl = src + 74;
    for (int ib = 0; ib < 8; ib++, qs += 4, sgn += 4) {
        const uint32_t h = qh[ib];
        const __m256i g = _mm256_set_epi64x((long long)q_iq2s_grid[qs[3] | ((h << 2) & 0x300)], (long long)q_iq2s_grid[qs[2] | ((h << 4) & 0x300)],
                                            (long long)q_iq2s_grid[qs[1] | ((h << 6) & 0x300)], (long long)q_iq2s_grid[qs[0] | ((h << 8) & 0x300)]);
        uint32_t s4; memcpy(&s4, sgn, 4);
        lb_store32(w + 32 * ib, lb_negate(g, lb_signmask(s4)), 2 * (scl[ib] & 15) + 1, 2 * (scl[ib] >> 4) + 1);
    }
}
static void lb_dec_iq3xxs(const uint8_t *src, int16_t *w, float *f, int n) {
    (void)n; f[0] = h2f(src) * 0.25f;
    const uint8_t *qs = src + 2, *ss = src + 66;
    for (int ib = 0; ib < 8; ib++, qs += 8) {
        uint32_t a; memcpy(&a, ss + 4 * ib, 4);
        const __m256i g = _mm256_i32gather_epi32((const int *)q_iq3xxs_grid, _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)qs)), 4);
        const int sc = 2 * (int)(a >> 28) + 1;
        lb_store32(w + 32 * ib, lb_negate(g, lb_signmask(lb_signs4(a))), sc, sc);
    }
}
static void lb_dec_iq3s(const uint8_t *src, int16_t *w, float *f, int n) {
    (void)n; f[0] = h2f(src);
    const uint8_t *qs = src + 2, *qh = src + 66, *sgn = src + 74, *scl = src + 106;
    const __m256i shl = _mm256_setr_epi32(8, 7, 6, 5, 4, 3, 2, 1), b8 = _mm256_set1_epi32(256);
    for (int p = 0; p < 4; p++)
        for (int h = 0; h < 2; h++) {
            const int g = 2 * p + h;
            __m256i idx = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(qs + 8 * g)));
            idx = _mm256_or_si256(idx, _mm256_and_si256(_mm256_sllv_epi32(_mm256_set1_epi32(qh[g]), shl), b8));
            const __m256i gr = _mm256_i32gather_epi32((const int *)q_iq3s_grid, idx, 4);
            uint32_t s4; memcpy(&s4, sgn + 4 * g, 4);
            const int sc = 1 + 2 * (h ? scl[p] >> 4 : scl[p] & 15);
            lb_store32(w + 32 * g, lb_negate(gr, lb_signmask(s4)), sc, sc);
        }
}
static void lb_dec_iq4xs(const uint8_t *src, int16_t *w, float *f, int n) {
    (void)n; f[0] = h2f(src);
    const __m128i tbl = _mm_loadu_si128((const __m128i *)q_iq4nl_vals), m4 = _mm_set1_epi8(15);
    for (int ib = 0; ib < 8; ib++) {
        const __m128i v = _mm_loadu_si128((const __m128i *)(src + 8 + 16 * ib));
        const __m256i q = _mm256_set_m128i(_mm_shuffle_epi8(tbl, _mm_and_si128(_mm_srli_epi16(v, 4), m4)), _mm_shuffle_epi8(tbl, _mm_and_si128(v, m4)));
        const int sc = iq4xs_scale(src, ib);
        lb_store32(w + 32 * ib, q, sc, sc);
    }
}
static void lb_dec_iq4nl(const uint8_t *src, int16_t *w, float *f, int n) {
    const __m128i tbl = _mm_loadu_si128((const __m128i *)q_iq4nl_vals), m4 = _mm_set1_epi8(15);
    for (int g = 0; g < n / 32; g++, src += 18) {
        f[g] = h2f(src);
        const __m128i v = _mm_loadu_si128((const __m128i *)(src + 2));
        const __m256i q = _mm256_set_m128i(_mm_shuffle_epi8(tbl, _mm_and_si128(_mm_srli_epi16(v, 4), m4)), _mm_shuffle_epi8(tbl, _mm_and_si128(v, m4)));
        lb_store32(w + 32 * g, q, 1, 1);
    }
}
/* Q2_0: 4 bytes = 16 elementos; o byte e/4 vai para a faixa de 16 bits do elemento, e uma multiplicacao leva o campo
   de 2 bits (deslocamento 2*(e%4)) para os bits 14-15, de onde um deslocamento logico o traz para 0..3 */
static void lb_dec_q20(const uint8_t *src, int16_t *w, float *f, int n) {
    const __m256i ctl = _mm256_setr_epi8(0, -128, 0, -128, 0, -128, 0, -128, 1, -128, 1, -128, 1, -128, 1, -128,
                                         2, -128, 2, -128, 2, -128, 2, -128, 3, -128, 3, -128, 3, -128, 3, -128);
    const __m256i mul = _mm256_setr_epi16(1 << 14, 1 << 12, 1 << 10, 1 << 8, 1 << 14, 1 << 12, 1 << 10, 1 << 8,
                                          1 << 14, 1 << 12, 1 << 10, 1 << 8, 1 << 14, 1 << 12, 1 << 10, 1 << 8);
    const __m256i one = _mm256_set1_epi16(1);
    for (int u = 0; u < n / 64; u++, src += 18) {
        f[2 * u] = f[2 * u + 1] = h2f(src);
        for (int m = 0; m < 4; m++) {
            uint32_t b4; memcpy(&b4, src + 2 + 4 * m, 4);
            __m256i v = _mm256_shuffle_epi8(_mm256_set1_epi32((int)b4), ctl);
            v = _mm256_sub_epi16(_mm256_srli_epi16(_mm256_mullo_epi16(v, mul), 14), one);
            _mm256_storeu_si256((__m256i *)(w + 64 * u + 16 * m), v);
        }
    }
}

/* ---- decodificadores AVX-512 (64 pesos por passo): a busca na grade vira um gather, o sinal vira uma mascara de 64 bits
   (bit e = elemento e negativo) e a escala inteira entra por multiplicacao de 16 bits ---- */
LB_512 static inline void lb5_store64(int16_t *w, __m512i v8, __m512i s0, __m512i s1) {
    const __m512i lo = _mm512_cvtepi8_epi16(_mm512_castsi512_si256(v8)), hi = _mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(v8, 1));
    _mm512_storeu_si512((void *)w, _mm512_mullo_epi16(lo, s0));
    _mm512_storeu_si512((void *)(w + 32), _mm512_mullo_epi16(hi, s1));
}
/* 8 indices de sinais de 7 bits (um por faixa de 64 bits) -> mascara de 64 bits (a paridade vira o bit 7 do byte) */
LB_512 static inline __mmask64 lb5_signs(__m512i si) {
    __m512i p = _mm512_xor_si512(si, _mm512_srli_epi64(si, 4));
    p = _mm512_xor_si512(p, _mm512_srli_epi64(p, 2));
    p = _mm512_xor_si512(p, _mm512_srli_epi64(p, 1));
    const __m512i sb = _mm512_or_si512(si, _mm512_slli_epi64(_mm512_and_si512(p, _mm512_set1_epi64(1)), 7));
    return _cvtu64_mask64((uint64_t)_mm_cvtsi128_si64(_mm512_cvtepi64_epi8(sb)));
}
LB_512 static inline __m512i lb5_neg(__m512i g, __mmask64 k) { return _mm512_mask_sub_epi8(g, k, _mm512_setzero_si512(), g); }
/* 4 escalas de 16 pesos (a, b: duas por byte de nibbles) -> vetores de 32 faixas de 16 bits */
LB_512 static inline __m512i lb5_sc2(int a, int b) {
    return _mm512_inserti64x4(_mm512_castsi256_si512(_mm256_set1_epi16((short)a)), _mm256_set1_epi16((short)b), 1);
}
LB_512 static inline __m512i lb5_sgnidx(uint32_t a, uint32_t b) {   /* 4 campos de 7 bits de a e de b, um por faixa de 64 bits */
    const __m512i v = _mm512_mask_set1_epi64(_mm512_set1_epi64((long long)a), 0xF0, (long long)b);
    return _mm512_and_si512(_mm512_srlv_epi64(v, _mm512_setr_epi64(0, 7, 14, 21, 0, 7, 14, 21)), _mm512_set1_epi64(127));
}

LB_512 static void lb5_dec_iq2xxs(const uint8_t *src, int16_t *w, float *f, int n) {
    (void)n; f[0] = h2f(src) * 0.125f;
    const __m128i pick = _mm_setr_epi8(0, 1, 2, 3, 8, 9, 10, 11, -1, -1, -1, -1, -1, -1, -1, -1);
    for (int k = 0; k < 4; k++) {
        const uint8_t *p = src + 2 + 16 * k;
        const __m512i idx = _mm512_cvtepu8_epi64(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)p), pick));
        const __m512i g = _mm512_i64gather_epi64(idx, (const void *)q_iq2xxs_grid, 8);
        uint32_t a1, b1; memcpy(&a1, p + 4, 4); memcpy(&b1, p + 12, 4);
        const __m512i v = lb5_neg(g, lb5_signs(lb5_sgnidx(a1, b1)));
        lb5_store64(w + 64 * k, v, _mm512_set1_epi16((short)(2 * (a1 >> 28) + 1)), _mm512_set1_epi16((short)(2 * (b1 >> 28) + 1)));
    }
}
LB_512 static void lb5_dec_iq2xs(const uint8_t *src, int16_t *w, float *f, int n) {
    (void)n; f[0] = h2f(src) * 0.125f;
    const uint8_t *scl = src + 66;
    for (int k = 0; k < 4; k++) {
        const __m512i q = _mm512_cvtepu16_epi64(_mm_loadu_si128((const __m128i *)(src + 2 + 16 * k)));
        const __m512i g = _mm512_i64gather_epi64(_mm512_and_si512(q, _mm512_set1_epi64(511)), (const void *)q_iq2xs_grid, 8);
        const __m512i v = lb5_neg(g, lb5_signs(_mm512_srli_epi64(q, 9)));
        const int x0 = scl[2 * k], x1 = scl[2 * k + 1];
        lb5_store64(w + 64 * k, v, lb5_sc2(2 * (x0 & 15) + 1, 2 * (x0 >> 4) + 1), lb5_sc2(2 * (x1 & 15) + 1, 2 * (x1 >> 4) + 1));
    }
}
LB_512 static void lb5_dec_iq2s(const uint8_t *src, int16_t *w, float *f, int n) {
    (void)n; f[0] = h2f(src) * 0.125f;
    const uint8_t *qs = src + 2, *sgn = src + 34, *qh = src + 66, *scl = src + 74;
    const __m512i cnt = _mm512_setr_epi64(0, 2, 4, 6, 8, 10, 12, 14);
    for (int k = 0; k < 4; k++) {
        uint16_t h; memcpy(&h, qh + 2 * k, 2);
        __m512i idx = _mm512_cvtepu8_epi64(_mm_loadl_epi64((const __m128i *)(qs + 8 * k)));
        idx = _mm512_or_si512(idx, _mm512_slli_epi64(_mm512_and_si512(_mm512_srlv_epi64(_mm512_set1_epi64(h), cnt), _mm512_set1_epi64(3)), 8));
        const __m512i g = _mm512_i64gather_epi64(idx, (const void *)q_iq2s_grid, 8);
        uint64_t s8; memcpy(&s8, sgn + 8 * k, 8);
        const __m512i v = lb5_neg(g, _cvtu64_mask64(s8));
        const int x0 = scl[2 * k], x1 = scl[2 * k + 1];
        lb5_store64(w + 64 * k, v, lb5_sc2(2 * (x0 & 15) + 1, 2 * (x0 >> 4) + 1), lb5_sc2(2 * (x1 & 15) + 1, 2 * (x1 >> 4) + 1));
    }
}
LB_512 static void lb5_dec_iq3xxs(const uint8_t *src, int16_t *w, float *f, int n) {
    (void)n; f[0] = h2f(src) * 0.25f;
    const uint8_t *ss = src + 66;
    for (int k = 0; k < 4; k++) {
        const __m512i idx = _mm512_cvtepu8_epi32(_mm_loadu_si128((const __m128i *)(src + 2 + 16 * k)));
        const __m512i g = _mm512_i32gather_epi32(idx, (const void *)q_iq3xxs_grid, 4);
        uint32_t a, b; memcpy(&a, ss + 8 * k, 4); memcpy(&b, ss + 8 * k + 4, 4);
        const __m512i v = lb5_neg(g, lb5_signs(lb5_sgnidx(a, b)));
        lb5_store64(w + 64 * k, v, _mm512_set1_epi16((short)(2 * (a >> 28) + 1)), _mm512_set1_epi16((short)(2 * (b >> 28) + 1)));
    }
}
LB_512 static void lb5_dec_iq3s(const uint8_t *src, int16_t *w, float *f, int n) {
    (void)n; f[0] = h2f(src);
    const uint8_t *qs = src + 2, *qh = src + 66, *sgn = src + 74, *scl = src + 106;
    for (int k = 0; k < 4; k++) {
        uint16_t h; memcpy(&h, qh + 2 * k, 2);
        __m512i idx = _mm512_cvtepu8_epi32(_mm_loadu_si128((const __m128i *)(qs + 16 * k)));
        idx = _mm512_mask_or_epi32(idx, _cvtu32_mask16(h), idx, _mm512_set1_epi32(256));
        const __m512i g = _mm512_i32gather_epi32(idx, (const void *)q_iq3s_grid, 4);
        uint64_t s8; memcpy(&s8, sgn + 8 * k, 8);
        const __m512i v = lb5_neg(g, _cvtu64_mask64(s8));
        lb5_store64(w + 64 * k, v, _mm512_set1_epi16((short)(1 + 2 * (scl[k] & 15))), _mm512_set1_epi16((short)(1 + 2 * (scl[k] >> 4))));
    }
}
/* 2 blocos de 16 bytes (cada um 32 pesos: 16 baixos e 16 altos) -> 64 valores da tabela de 4 bits */
LB_512 static inline __m512i lb5_nib2(__m128i v0, __m128i v1) {
    const __m512i x = _mm512_inserti64x4(_mm512_castsi256_si512(_mm256_set_m128i(v0, v0)), _mm256_set_m128i(v1, v1), 1);
    const __m512i sh = _mm512_srlv_epi16(x, _mm512_set_epi64(0x0004000400040004LL, 0x0004000400040004LL, 0, 0, 0x0004000400040004LL, 0x0004000400040004LL, 0, 0));
    return _mm512_shuffle_epi8(_mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i *)q_iq4nl_vals)), _mm512_and_si512(sh, _mm512_set1_epi8(15)));
}
LB_512 static void lb5_dec_iq4xs(const uint8_t *src, int16_t *w, float *f, int n) {
    (void)n; f[0] = h2f(src);
    for (int s = 0; s < 4; s++) {
        const __m512i v = lb5_nib2(_mm_loadu_si128((const __m128i *)(src + 8 + 32 * s)), _mm_loadu_si128((const __m128i *)(src + 8 + 32 * s + 16)));
        lb5_store64(w + 64 * s, v, _mm512_set1_epi16((short)iq4xs_scale(src, 2 * s)), _mm512_set1_epi16((short)iq4xs_scale(src, 2 * s + 1)));
    }
}
LB_512 static void lb5_dec_iq4nl(const uint8_t *src, int16_t *w, float *f, int n) {
    int g = 0;
    for (; g + 2 <= n / 32; g += 2, src += 36) {
        f[g] = h2f(src); f[g + 1] = h2f(src + 18);
        lb5_store64(w + 32 * g, lb5_nib2(_mm_loadu_si128((const __m128i *)(src + 2)), _mm_loadu_si128((const __m128i *)(src + 20))),
                    _mm512_set1_epi16(1), _mm512_set1_epi16(1));
    }
    if (g < n / 32) lb_dec_iq4nl(src, w + 32 * g, f + g, 32);
}
LB_512 static void lb5_dec_q20(const uint8_t *src, int16_t *w, float *f, int n) {
    /* 8 bytes -> 32 palavras de 16 bits (cada byte repetido 4 vezes, uma por elemento); multiplicar por 2^(14 - 2 (e%4))
       leva o campo de 2 bits do elemento para os bits 14-15 e o deslocamento logico o traz para 0..3 */
    const __m512i rep = _mm512_set_epi16(7, 7, 7, 7, 6, 6, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 3, 3, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0);
    const __m512i mul = _mm512_broadcast_i32x4(_mm_setr_epi16(1 << 14, 1 << 12, 1 << 10, 1 << 8, 1 << 14, 1 << 12, 1 << 10, 1 << 8));
    const __m512i one = _mm512_set1_epi16(1);
    for (int u = 0; u < n / 64; u++, src += 18) {
        f[2 * u] = f[2 * u + 1] = h2f(src);
        const __m128i b = _mm_loadu_si128((const __m128i *)(src + 2));
        for (int h = 0; h < 2; h++) {
            const __m512i r = _mm512_permutexvar_epi16(rep, _mm512_cvtepu8_epi16(_mm256_castsi128_si256(h ? _mm_srli_si128(b, 8) : b)));
            _mm512_storeu_si512((void *)(w + 64 * u + 32 * h), _mm512_sub_epi16(_mm512_srli_epi16(_mm512_mullo_epi16(r, mul), 14), one));
        }
    }
}

static const LbT lb_tab[] = {
    { T_IQ2_XXS, 256, 66, 1, lb_dec_iq2xxs, lb5_dec_iq2xxs }, { T_IQ2_XS, 256, 74, 1, lb_dec_iq2xs, lb5_dec_iq2xs },
    { T_IQ2_S, 256, 82, 1, lb_dec_iq2s, lb5_dec_iq2s }, { T_IQ3_XXS, 256, 98, 1, lb_dec_iq3xxs, lb5_dec_iq3xxs },
    { T_IQ3_S, 256, 110, 1, lb_dec_iq3s, lb5_dec_iq3s }, { T_IQ4_XS, 256, 136, 1, lb_dec_iq4xs, lb5_dec_iq4xs },
    { T_IQ4_NL, 32, 18, 0, lb_dec_iq4nl, lb5_dec_iq4nl }, { T_Q2_0, 64, 18, 0, lb_dec_q20, lb5_dec_q20 } };
static const LbT *lb_by_type(uint32_t type) {
    for (size_t i = 0; i < sizeof lb_tab / sizeof lb_tab[0]; i++) if (lb_tab[i].type == type) return &lb_tab[i];
    return NULL;
}

static int lb_mode_v = -1;
static int lb_tile = 1;     /* lote em AVX-512 com 4 tokens por vez */
static int lb_mode(void) {
    if (lb_mode_v >= 0) return lb_mode_v;
    __builtin_cpu_init();
    int m = 1;
    if (__builtin_cpu_supports("avx512vnni") && __builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("avx512bw") &&
        __builtin_cpu_supports("avx512f")) m = 3;       /* medido: o AVX-512 (decodificacao e produto) vence o de 256 bits na maioria dos tipos */
    const char *e = getenv("KATANA_LOWBIT");
    if (e) { const int v = atoi(e); if (v == 0) m = 0; else if (v == 1) m = 1; else if (m >= 2 && v <= 3) m = v; }
    if (getenv("KATANA_LOWBIT_TILE")) lb_tile = atoi(getenv("KATANA_LOWBIT_TILE")) != 0;
    lb_mode_v = m;
    return m;
}
/* tipos tratados (mascara de bits: lowbit_ok esta no caminho de todo produto, tem que custar quase nada) */
#define LB_MASK ((1ull << T_IQ2_XXS) | (1ull << T_IQ2_XS) | (1ull << T_IQ3_XXS) | (1ull << T_IQ4_NL) | (1ull << T_IQ3_S) | \
                 (1ull << T_IQ2_S) | (1ull << T_IQ4_XS) | (1ull << T_Q2_0))
static int lowbit_ok(uint32_t type) { return type < 64 && ((LB_MASK >> type) & 1) && lb_mode() > 0; }

/* o corpo dos produtos, igual para as tres larguras: DOT(p, w, x) soma em p os produtos de 32 pesos por 32 ativacoes */
#define LB_ROW(NAME, ATTR, DEC, VI, VF, ZI, ZF, DOT, CVT, FMA, SET1, HSUM) \
ATTR static float NAME(const LbT *T, const uint8_t *row, int cols) { \
    int16_t w[256] __attribute__((aligned(64))); float f[8]; \
    const int16_t *xq_q = xq.q; const float *xs = xq.xs; \
    VF acc = ZF; \
    for (int c = 0; c < cols; c += 256) { \
        const int n = cols - c < 256 ? cols - c : 256; \
        DEC(T)(row + (size_t)(c / T->unit) * T->ub, w, f, n); \
        const int16_t *x = xq_q + c; const float xsc = xs[c >> 8]; \
        if (T->fold) { \
            VI p = ZI; \
            for (int g = 0; g < n; g += 32) p = DOT(p, w + g, x + g); \
            acc = FMA(CVT(p), SET1(f[0] * xsc), acc); \
        } else { \
            VF ba = ZF; \
            for (int g = 0; g < n; g += 32) { VI p = DOT(ZI, w + g, x + g); ba = FMA(CVT(p), SET1(f[g / 32]), ba); } \
            acc = FMA(ba, SET1(xsc), acc); \
        } \
    } \
    return HSUM(acc); \
} \
ATTR static void NAME##_b(const LbT *T, const uint8_t *row, XQ *xqs, int t0, int t1, int cols, float *out) { \
    int16_t w[256] __attribute__((aligned(64))); float f[8]; \
    VF acc[LB_MAXB]; \
    for (int t = 0; t < t1 - t0; t++) acc[t] = ZF; \
    for (int c = 0; c < cols; c += 256) { \
        const int n = cols - c < 256 ? cols - c : 256; \
        DEC(T)(row + (size_t)(c / T->unit) * T->ub, w, f, n); \
        for (int t = t0; t < t1; t++) { \
            const int16_t *x = xqs[t].q + c; const float xsc = xqs[t].xs[c >> 8]; \
            VF a = acc[t - t0]; \
            if (T->fold) { \
                VI p = ZI; \
                for (int g = 0; g < n; g += 32) p = DOT(p, w + g, x + g); \
                a = FMA(CVT(p), SET1(f[0] * xsc), a); \
            } else { \
                VF ba = ZF; \
                for (int g = 0; g < n; g += 32) { VI p = DOT(ZI, w + g, x + g); ba = FMA(CVT(p), SET1(f[g / 32]), ba); } \
                a = FMA(ba, SET1(xsc), a); \
            } \
            acc[t - t0] = a; \
        } \
    } \
    for (int t = 0; t < t1 - t0; t++) out[t] = HSUM(acc[t]); \
}

#define LB_D2(T) ((T)->dec)
#define LB_D5(T) ((T)->dec5)
#define LB_L(p) _mm256_loadu_si256((const __m256i *)(p))
#define LB_DOT_AVX2(p, w, x) _mm256_add_epi32(p, _mm256_add_epi32(_mm256_madd_epi16(LB_L(w), LB_L(x)), _mm256_madd_epi16(LB_L((w) + 16), LB_L((x) + 16))))
#define LB_DOT_V256(p, w, x) _mm256_dpwssd_epi32(_mm256_dpwssd_epi32(p, LB_L(w), LB_L(x)), LB_L((w) + 16), LB_L((x) + 16))
#define LB_DOT_V512(p, w, x) _mm512_dpwssd_epi32(p, _mm512_loadu_si512((const void *)(w)), _mm512_loadu_si512((const void *)(x)))
LB_ROW(lb_row_avx2, , LB_D2, __m256i, __m256, _mm256_setzero_si256(), _mm256_setzero_ps(), LB_DOT_AVX2, _mm256_cvtepi32_ps, _mm256_fmadd_ps, _mm256_set1_ps, hsum256)
LB_ROW(lb_row_v256, LB_256, LB_D2, __m256i, __m256, _mm256_setzero_si256(), _mm256_setzero_ps(), LB_DOT_V256, _mm256_cvtepi32_ps, _mm256_fmadd_ps, _mm256_set1_ps, hsum256)
LB_ROW(lb_row_v512, LB_512, LB_D5, __m512i, __m512, _mm512_setzero_si512(), _mm512_setzero_ps(), LB_DOT_V512, _mm512_cvtepi32_ps, _mm512_fmadd_ps, _mm512_set1_ps, _mm512_reduce_add_ps)

/* lote em AVX-512 com 4 tokens por vez: cada vetor de pesos (carregado uma vez) serve a 4 produtos independentes */
LB_512 static void lb_row_v512_t(const LbT *T, const uint8_t *row, XQ *xqs, int t0, int t1, int cols, float *out) {
    int16_t w[256] __attribute__((aligned(64))); float f[8];
    __m512 acc[LB_MAXB];
    for (int t = 0; t < t1 - t0; t++) acc[t] = _mm512_setzero_ps();
    for (int c = 0; c < cols; c += 256) {
        const int n = cols - c < 256 ? cols - c : 256, ng = n / 32;
        T->dec5(row + (size_t)(c / T->unit) * T->ub, w, f, n);
        __m512i wv[8];
        for (int g = 0; g < ng; g++) wv[g] = _mm512_loadu_si512((const void *)(w + 32 * g));
        int t = t0;
        for (; t + 4 <= t1; t += 4) {
            const int16_t *x0 = xqs[t].q + c, *x1 = xqs[t + 1].q + c, *x2 = xqs[t + 2].q + c, *x3 = xqs[t + 3].q + c;
            const float s0 = xqs[t].xs[c >> 8], s1 = xqs[t + 1].xs[c >> 8], s2 = xqs[t + 2].xs[c >> 8], s3 = xqs[t + 3].xs[c >> 8];
            if (T->fold) {
                __m512i p0 = _mm512_setzero_si512(), p1 = p0, p2 = p0, p3 = p0;
                for (int g = 0; g < ng; g++) {
                    p0 = _mm512_dpwssd_epi32(p0, wv[g], _mm512_loadu_si512((const void *)(x0 + 32 * g)));
                    p1 = _mm512_dpwssd_epi32(p1, wv[g], _mm512_loadu_si512((const void *)(x1 + 32 * g)));
                    p2 = _mm512_dpwssd_epi32(p2, wv[g], _mm512_loadu_si512((const void *)(x2 + 32 * g)));
                    p3 = _mm512_dpwssd_epi32(p3, wv[g], _mm512_loadu_si512((const void *)(x3 + 32 * g)));
                }
                acc[t - t0] = _mm512_fmadd_ps(_mm512_cvtepi32_ps(p0), _mm512_set1_ps(f[0] * s0), acc[t - t0]);
                acc[t - t0 + 1] = _mm512_fmadd_ps(_mm512_cvtepi32_ps(p1), _mm512_set1_ps(f[0] * s1), acc[t - t0 + 1]);
                acc[t - t0 + 2] = _mm512_fmadd_ps(_mm512_cvtepi32_ps(p2), _mm512_set1_ps(f[0] * s2), acc[t - t0 + 2]);
                acc[t - t0 + 3] = _mm512_fmadd_ps(_mm512_cvtepi32_ps(p3), _mm512_set1_ps(f[0] * s3), acc[t - t0 + 3]);
            } else {
                __m512 b0 = _mm512_setzero_ps(), b1 = b0, b2 = b0, b3 = b0;
                const __m512i z = _mm512_setzero_si512();
                for (int g = 0; g < ng; g++) {
                    const __m512 fg = _mm512_set1_ps(f[g]);
                    b0 = _mm512_fmadd_ps(_mm512_cvtepi32_ps(_mm512_dpwssd_epi32(z, wv[g], _mm512_loadu_si512((const void *)(x0 + 32 * g)))), fg, b0);
                    b1 = _mm512_fmadd_ps(_mm512_cvtepi32_ps(_mm512_dpwssd_epi32(z, wv[g], _mm512_loadu_si512((const void *)(x1 + 32 * g)))), fg, b1);
                    b2 = _mm512_fmadd_ps(_mm512_cvtepi32_ps(_mm512_dpwssd_epi32(z, wv[g], _mm512_loadu_si512((const void *)(x2 + 32 * g)))), fg, b2);
                    b3 = _mm512_fmadd_ps(_mm512_cvtepi32_ps(_mm512_dpwssd_epi32(z, wv[g], _mm512_loadu_si512((const void *)(x3 + 32 * g)))), fg, b3);
                }
                acc[t - t0] = _mm512_fmadd_ps(b0, _mm512_set1_ps(s0), acc[t - t0]);
                acc[t - t0 + 1] = _mm512_fmadd_ps(b1, _mm512_set1_ps(s1), acc[t - t0 + 1]);
                acc[t - t0 + 2] = _mm512_fmadd_ps(b2, _mm512_set1_ps(s2), acc[t - t0 + 2]);
                acc[t - t0 + 3] = _mm512_fmadd_ps(b3, _mm512_set1_ps(s3), acc[t - t0 + 3]);
            }
        }
        for (; t < t1; t++) {
            const int16_t *x = xqs[t].q + c; const float xsc = xqs[t].xs[c >> 8];
            if (T->fold) {
                __m512i p = _mm512_setzero_si512();
                for (int g = 0; g < ng; g++) p = _mm512_dpwssd_epi32(p, wv[g], _mm512_loadu_si512((const void *)(x + 32 * g)));
                acc[t - t0] = _mm512_fmadd_ps(_mm512_cvtepi32_ps(p), _mm512_set1_ps(f[0] * xsc), acc[t - t0]);
            } else {
                __m512 b = _mm512_setzero_ps();
                for (int g = 0; g < ng; g++)
                    b = _mm512_fmadd_ps(_mm512_cvtepi32_ps(_mm512_dpwssd_epi32(_mm512_setzero_si512(), wv[g], _mm512_loadu_si512((const void *)(x + 32 * g)))), _mm512_set1_ps(f[g]), b);
                acc[t - t0] = _mm512_fmadd_ps(b, _mm512_set1_ps(xsc), acc[t - t0]);
            }
        }
    }
    for (int t = 0; t < t1 - t0; t++) out[t] = _mm512_reduce_add_ps(acc[t]);
}

static float lowbit_dot(const Mat *W, const uint8_t *row, int cols) {
    const LbT *T = lb_by_type(W->type);
    switch (lb_mode_v) {
    case 3: return lb_row_v512(T, row, cols);
    case 2: return lb_row_v256(T, row, cols);
    default: return lb_row_avx2(T, row, cols);
    }
}
/* uma linha de pesos contra as ativacoes (ja quantizadas) dos tokens t0..t1-1 (no maximo LB_MAXB) */
static void lowbit_dot_batch(const Mat *W, const uint8_t *row, XQ *xqs, int t0, int t1, int cols, float *out) {
    const LbT *T = lb_by_type(W->type);
    switch (lb_mode_v) {
    case 3: if (lb_tile) lb_row_v512_t(T, row, xqs, t0, t1, cols, out); else lb_row_v512_b(T, row, xqs, t0, t1, cols, out); break;
    case 2: lb_row_v256_b(T, row, xqs, t0, t1, cols, out); break;
    default: lb_row_avx2_b(T, row, xqs, t0, t1, cols, out); break;
    }
}
/* BF16: o peso e o alto de um float32 (deslocamento de 16 bits); produto em float com a ativacao em float (sem
   quantizar a ativacao). So vale se o tensor ficar em BF16 na memoria (KATANA_BF16_RAW=1 em get_mat); senao o
   carregador converte para float32 (o dobro da memoria) e este caminho nao e usado. */
static float lowbit_bf16_dot(const uint8_t *row, const float *x, int cols) {
    __m256 a0 = _mm256_setzero_ps(), a1 = a0, a2 = a0, a3 = a0;
    int j = 0;
    for (; j + 32 <= cols; j += 32) {
        const __m256i v0 = _mm256_slli_epi32(_mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i *)(row + 2 * j))), 16);
        const __m256i v1 = _mm256_slli_epi32(_mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i *)(row + 2 * j + 16))), 16);
        const __m256i v2 = _mm256_slli_epi32(_mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i *)(row + 2 * j + 32))), 16);
        const __m256i v3 = _mm256_slli_epi32(_mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i *)(row + 2 * j + 48))), 16);
        a0 = _mm256_fmadd_ps(_mm256_castsi256_ps(v0), _mm256_loadu_ps(x + j), a0);
        a1 = _mm256_fmadd_ps(_mm256_castsi256_ps(v1), _mm256_loadu_ps(x + j + 8), a1);
        a2 = _mm256_fmadd_ps(_mm256_castsi256_ps(v2), _mm256_loadu_ps(x + j + 16), a2);
        a3 = _mm256_fmadd_ps(_mm256_castsi256_ps(v3), _mm256_loadu_ps(x + j + 24), a3);
    }
    float s = hsum256(_mm256_add_ps(_mm256_add_ps(a0, a1), _mm256_add_ps(a2, a3)));
    for (; j < cols; j++) { uint16_t h; memcpy(&h, row + 2 * j, 2); s += bf16_to_f32(h) * x[j]; }
    return s;
}
#else
#define HAVE_LOWBIT 0
static int lowbit_ok(uint32_t type) { (void)type; return 0; }
static float lowbit_bf16_dot(const uint8_t *row, const float *x, int cols) {
    float s = 0; for (int j = 0; j < cols; j++) { uint16_t h; memcpy(&h, row + 2 * j, 2); s += bf16_to_f32(h) * x[j]; } return s;
}
static float lowbit_dot(const Mat *W, const uint8_t *row, int cols) { (void)W; (void)row; (void)cols; return 0.0f; }
static void lowbit_dot_batch(const Mat *W, const uint8_t *row, XQ *xqs, int t0, int t1, int cols, float *out) {
    (void)W; (void)row; (void)xqs; (void)t0; (void)t1; (void)cols; (void)out;
}
#endif
#include "cpu_lowbit_test.c"      /* katana lbtest: conferencia e medidas */
