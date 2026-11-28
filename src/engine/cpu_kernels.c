/* ================= matematica ================= */

static float dot(const float *a, const float *b, int n) {
    int j = 0;
    float s = 0.0f;
#ifdef __AVX2__
    __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
    for (; j + 16 <= n; j += 16) {
        acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + j), _mm256_loadu_ps(b + j), acc0);
        acc1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + j + 8), _mm256_loadu_ps(b + j + 8), acc1);
    }
    acc0 = _mm256_add_ps(acc0, acc1);
    __m128 lo = _mm_add_ps(_mm256_castps256_ps128(acc0), _mm256_extractf128_ps(acc0, 1));
    lo = _mm_hadd_ps(lo, lo); lo = _mm_hadd_ps(lo, lo);
    s = _mm_cvtss_f32(lo);
#elif defined(__ARM_NEON)
    /* NEON (celular/ARM, 2026-11-27): 16 de cada vez (quatro acumuladores de 4), igual ao passo do AVX2. Usado
       pelo caminho generico (dequant + dot) de Q4_K/Q6_K/etc no celular -- e o maior consumidor de tempo da
       geracao, por isso foi o primeiro a ganhar NEON de verdade (nao so compilar). */
    float32x4_t acc0 = vdupq_n_f32(0.0f), acc1 = vdupq_n_f32(0.0f), acc2 = vdupq_n_f32(0.0f), acc3 = vdupq_n_f32(0.0f);
    for (; j + 16 <= n; j += 16) {
        acc0 = vfmaq_f32(acc0, vld1q_f32(a + j), vld1q_f32(b + j));
        acc1 = vfmaq_f32(acc1, vld1q_f32(a + j + 4), vld1q_f32(b + j + 4));
        acc2 = vfmaq_f32(acc2, vld1q_f32(a + j + 8), vld1q_f32(b + j + 8));
        acc3 = vfmaq_f32(acc3, vld1q_f32(a + j + 12), vld1q_f32(b + j + 12));
    }
    s = vaddvq_f32(vaddq_f32(vaddq_f32(acc0, acc1), vaddq_f32(acc2, acc3)));
#endif
    for (; j < n; j++) s += a[j] * b[j];
    return s;
}

/* memoria livre para uso agora (bytes; 0 = nao sei): Linux (MemAvailable) e Windows (GlobalMemoryStatusEx).
   KATANA_RAM_AVAIL_MB forca o valor (so para teste, mesmo padrao de KATANA_VRAM_AVAIL_MB em vk_dense.c):
   simula uma maquina com menos RAM livre sem precisar reiniciar ou trocar de hardware. */
static uint64_t ram_avail(void) {
    if (getenv("KATANA_RAM_AVAIL_MB")) return (uint64_t)atoll(getenv("KATANA_RAM_AVAIL_MB")) << 20;
#ifdef _WIN32
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
    return GlobalMemoryStatusEx(&ms) ? (uint64_t)ms.ullAvailPhys : 0;
#elif defined(__linux__)
    FILE *f = fopen("/proc/meminfo", "r");
    char ln[160]; unsigned long long kb = 0;
    while (f && fgets(ln, sizeof ln, f)) if (sscanf(ln, "MemAvailable: %llu kB", &kb) == 1) break;
    if (f) fclose(f);
    return (uint64_t)kb << 10;
#else
    return 0;
#endif
}
/* RAM total da maquina (bytes; 0 = nao sei): mesmo padrao usado em catalog.c/launch.c/vk_moe.c.
   KATANA_RAM_TOTAL_MB forca o valor (so para teste, ver KATANA_RAM_AVAIL_MB acima). */
static uint64_t ram_total(void) {
    if (getenv("KATANA_RAM_TOTAL_MB")) return (uint64_t)atoll(getenv("KATANA_RAM_TOTAL_MB")) << 20;
#ifdef _WIN32
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
    return GlobalMemoryStatusEx(&ms) ? (uint64_t)ms.ullTotalPhys : 0;
#else
    return (uint64_t)sysconf(_SC_PHYS_PAGES) * (uint64_t)sysconf(_SC_PAGE_SIZE);
#endif
}
/* cache KV da CPU em Q8_0 (hibrido DeltaNet): KATANA_CPU_KV8=0/1 forca. Padrao: so acima de 262144 de contexto e so se o
   cache fp16 (kv16 bytes), os pesos (pages do arquivo) e uma folga de 2 GB nao couberem na RAM: o Q8_0 perde precisao
   (selftest --deep no Qwen3.6: KL 1,5e-3 e 30% de erro medio por camada, contra 1,1e-5 do fp16) */
static int kv8_escolha(size_t ctx, uint64_t kv16, uint64_t weights) {
    const char *e = getenv("KATANA_CPU_KV8");
    if (e) return atoi(e) != 0;
    if (ctx <= 262144) return 0;
    const uint64_t av = ram_avail(), need = kv16 + weights + ((uint64_t)2 << 30);
    if (av >= need) return 0;
    fprintf(stderr, "cache KV: fp16 (%.1f GB) com os pesos (%.1f GB) nao cabe na memoria livre (%.1f GB): Q8_0, com menos precisao (KATANA_CPU_KV8=0 forca fp16)\n",
            kv16 / 1073741824.0, weights / 1073741824.0, av / 1073741824.0);
    return 1;
}

/* grava n valores (n multiplo de 32) em blocos Q8_0: escala fp16 = max|x|/127 e 32 x int8 */
#ifdef __AVX2__
static void q8_put(uint8_t *dst, const float *x, int n) {
    for (int b = 0; b < n / 32; b++, dst += 34, x += 32) {
        __m256 am = _mm256_setzero_ps();
        const __m256 sm = _mm256_set1_ps(-0.0f);
        for (int i = 0; i < 32; i += 8) am = _mm256_max_ps(am, _mm256_andnot_ps(sm, _mm256_loadu_ps(x + i)));
        __m128 a4 = _mm_max_ps(_mm256_castps256_ps128(am), _mm256_extractf128_ps(am, 1));
        a4 = _mm_max_ps(a4, _mm_movehl_ps(a4, a4));
        a4 = _mm_max_ss(a4, _mm_shuffle_ps(a4, a4, 1));
        const float d = _mm_cvtss_f32(a4) / 127.0f;
        const uint16_t h = (uint16_t)_mm_extract_epi16(_mm_cvtps_ph(_mm_set_ss(d), 0), 0);
        memcpy(dst, &h, 2);
        const float id = d > 0.0f ? 1.0f / d : 0.0f;
        int8_t *q = (int8_t *)(dst + 2);
        for (int i = 0; i < 32; i++) { const int v = (int)lrintf(x[i] * id); q[i] = (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v); }
    }
}
#elif defined(__ARM_NEON)
/* NEON (celular/ARM, 2026-11-27): o maximo absoluto vetorizado de 4 em 4; a quantizacao (arredondamento e corte)
   fica escalar de proposito -- sao so 32 valores por bloco e o risco de um erro sutil de arredondamento (como o
   achado no qn_axpys) nao compensa o ganho aqui. Conferido no katana qtest. */
static void q8_put(uint8_t *dst, const float *x, int n) {
    for (int b = 0; b < n / 32; b++, dst += 34, x += 32) {
        float32x4_t am = vdupq_n_f32(0.0f);
        for (int i = 0; i < 32; i += 4) am = vmaxq_f32(am, vabsq_f32(vld1q_f32(x + i)));
        const float amax = vmaxvq_f32(am);
        const float d = amax / 127.0f;
        const uint16_t h = f32_to_f16(d);
        memcpy(dst, &h, 2);
        const float id = d > 0.0f ? 1.0f / d : 0.0f;
        int8_t *q = (int8_t *)(dst + 2);
        for (int i = 0; i < 32; i++) { const int v = (int)lrintf(x[i] * id); q[i] = (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v); }
    }
}
#else
/* porte ARM, 2026-11-27: sem SIMD */
static void q8_put(uint8_t *dst, const float *x, int n) {
    for (int b = 0; b < n / 32; b++, dst += 34, x += 32) {
        float amax = 0.0f;
        for (int i = 0; i < 32; i++) { const float a = fabsf(x[i]); if (a > amax) amax = a; }
        const float d = amax / 127.0f;
        const uint16_t h = f32_to_f16(d);
        memcpy(dst, &h, 2);
        const float id = d > 0.0f ? 1.0f / d : 0.0f;
        int8_t *q = (int8_t *)(dst + 2);
        for (int i = 0; i < 32; i++) { const int v = (int)lrintf(x[i] * id); q[i] = (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v); }
    }
}
#endif

/* abre n valores (n multiplo de 32) de blocos Q8_0 em float */
#ifdef __AVX2__
static void q8_get(float *out, const uint8_t *src, int n) {
    for (int b = 0; b < n / 32; b++, src += 34, out += 32) {
        uint16_t h; memcpy(&h, src, 2);
        const __m256 d = _mm256_set1_ps(_mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))));
        const uint8_t *qs = src + 2;
        for (int i = 0; i < 32; i += 8)
            _mm256_storeu_ps(out + i, _mm256_mul_ps(d, _mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_loadl_epi64((const __m128i *)(qs + i))))));
    }
}
#elif defined(__ARM_NEON)
/* NEON (celular/ARM, 2026-11-27): 8 int8 por vez, alargados pra int16 e depois pra int32 (duas metades de 4),
   convertidos a float e multiplicados pela escala. Conferido no katana qtest. */
static void q8_get(float *out, const uint8_t *src, int n) {
    for (int b = 0; b < n / 32; b++, src += 34, out += 32) {
        uint16_t h; memcpy(&h, src, 2);
        const float32x4_t d = vdupq_n_f32(f16_to_f32(h));
        const int8_t *qs = (const int8_t *)(src + 2);
        for (int i = 0; i < 32; i += 8) {
            const int16x8_t w16 = vmovl_s8(vld1_s8(qs + i));
            vst1q_f32(out + i, vmulq_f32(d, vcvtq_f32_s32(vmovl_s16(vget_low_s16(w16)))));
            vst1q_f32(out + i + 4, vmulq_f32(d, vcvtq_f32_s32(vmovl_s16(vget_high_s16(w16)))));
        }
    }
}
#else
/* porte ARM, 2026-11-27: sem SIMD */
static void q8_get(float *out, const uint8_t *src, int n) {
    for (int b = 0; b < n / 32; b++, src += 34, out += 32) {
        uint16_t h; memcpy(&h, src, 2);
        const float d = f16_to_f32(h);
        const int8_t *qs = (const int8_t *)(src + 2);
        for (int i = 0; i < 32; i++) out[i] = d * qs[i];
    }
}
#endif

/* produto de uma linha Q8_0 (blocos: f16 escala + 32 x int8) por um vetor float */
static float dot_q8(const uint8_t *row, const float *x, int cols) {
    const int nb = cols / 32;
#ifdef __AVX2__
    __m256 acc = _mm256_setzero_ps();
    for (int b = 0; b < nb; b++, row += 34, x += 32) {
        uint16_t h; memcpy(&h, row, 2);
        const __m256 d = _mm256_set1_ps(_mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))));
        const uint8_t *qs = row + 2;
#define Q8(o) _mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_loadl_epi64((const __m128i *)(qs + (o)))))
        __m256 s = _mm256_mul_ps(Q8(0), _mm256_loadu_ps(x));
        s = _mm256_fmadd_ps(Q8(8), _mm256_loadu_ps(x + 8), s);
        s = _mm256_fmadd_ps(Q8(16), _mm256_loadu_ps(x + 16), s);
        s = _mm256_fmadd_ps(Q8(24), _mm256_loadu_ps(x + 24), s);
#undef Q8
        acc = _mm256_fmadd_ps(s, d, acc);
    }
    __m128 lo = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
    lo = _mm_hadd_ps(lo, lo); lo = _mm_hadd_ps(lo, lo);
    return _mm_cvtss_f32(lo);
#else
    float total = 0.0f;
    for (int b = 0; b < nb; b++, row += 34, x += 32) {
        uint16_t h; memcpy(&h, row, 2);
        const int8_t *qs = (const int8_t *)(row + 2);
        float s = 0.0f;
        for (int j = 0; j < 32; j++) s += qs[j] * x[j];
        total += s * f16_to_f32(h);
    }
    return total;
#endif
}

#ifdef __AVX2__
static float hsum256(__m256 v) {
    __m128 lo = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    lo = _mm_hadd_ps(lo, lo); lo = _mm_hadd_ps(lo, lo);
    return _mm_cvtss_f32(lo);
}
/* soma de 16 bytes (sem sinal / com sinal) vezes 16 floats, em 8 acumuladores */
static __m256 mul16_u8(__m128i v, const float *x) {
    __m256 s = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(v)), _mm256_loadu_ps(x));
    return _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_srli_si128(v, 8))), _mm256_loadu_ps(x + 8), s);
}
static __m256 mul16_i8(__m128i v, const float *x) {
    __m256 s = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(v)), _mm256_loadu_ps(x));
    return _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_srli_si128(v, 8))), _mm256_loadu_ps(x + 8), s);
}
#endif

/* linha de um tipo de 4 bits com tabela (MXFP4, IQ4_NL, IQ4_XS, Q4_1) vezes vetor float.
   Os indices de 4 bits viram int8 por pshufb na tabela do tipo e a escala entra por bloco de 32. */
static float dot_lut4(uint32_t type, const uint8_t *row, const float *x, int cols) {
#ifdef __AVX2__
    static const int8_t ident[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    const int8_t *tab = type == T_MXFP4 ? q_fp4_vals : type == T_Q4_1 ? ident : q_iq4nl_vals;
    const __m128i lut = _mm_loadu_si128((const __m128i *)tab), m4 = _mm_set1_epi8(15);
    __m256 acc = _mm256_setzero_ps();
    float corr = 0.0f;
    for (int b = 0; b < cols / 32; b++, x += 32) {
        const uint8_t *qs;
        float d;
        if (type == T_MXFP4) { d = e8m0_half(row[17 * b]); qs = row + 17 * b + 1; }
        else if (type == T_IQ4_NL) { d = h2f(row + 18 * b); qs = row + 18 * b + 2; }
        else if (type == T_Q4_1) {
            const uint8_t *blk = row + 20 * b;
            d = h2f(blk); qs = blk + 4;
            const __m256 s8 = _mm256_add_ps(_mm256_add_ps(_mm256_loadu_ps(x), _mm256_loadu_ps(x + 8)),
                                            _mm256_add_ps(_mm256_loadu_ps(x + 16), _mm256_loadu_ps(x + 24)));
            corr += h2f(blk + 2) * hsum256(s8);
        } else {    /* IQ4_XS: 8 sub-blocos de 32 por bloco de 256 */
            const uint8_t *blk = row + 136 * (b / 8);
            d = h2f(blk) * iq4xs_scale(blk, b % 8); qs = blk + 8 + 16 * (b % 8);
        }
        const __m128i v = _mm_loadu_si128((const __m128i *)qs);
        const __m128i lo = _mm_shuffle_epi8(lut, _mm_and_si128(v, m4));
        const __m128i hi = _mm_shuffle_epi8(lut, _mm_and_si128(_mm_srli_epi16(v, 4), m4));
        acc = _mm256_fmadd_ps(_mm256_add_ps(mul16_i8(lo, x), mul16_i8(hi, x + 16)), _mm256_set1_ps(d), acc);
    }
    return hsum256(acc) + corr;
#else
    const int unit = qunit(type);
    const size_t ub = qbytes(type, unit);
    float tmp[256], total = 0.0f;
    for (int b = 0; b < cols / unit; b++) { dequant(type, row + b * ub, tmp, unit); total += dot(tmp, x + b * unit, unit); }
    return total;
#endif
}

/* linha Q4_K vezes vetor float. Cada peso vale d*sc*q - dmin*m, entao por sub-bloco de 32:
   soma = d*sc * SUM(q_i*x_i) - dmin*m * SUM(x_i); sx traz SUM(x_i) ja calculado por sub-bloco */
static float dot_q4k(const uint8_t *row, const float *x, const float *sx, int cols) {
#ifdef __AVX2__
    const __m256i m4 = _mm256_set1_epi8(0x0F);
    __m256 acc = _mm256_setzero_ps();
    float corr = 0.0f;
    for (int b = 0; b < cols / 256; b++, row += 144) {
        uint16_t h0, h1; memcpy(&h0, row, 2); memcpy(&h1, row + 2, 2);
        const float d = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h0)));
        const float dmin = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h1)));
        const uint8_t *sc = row + 4, *q = row + 16;
        for (int j = 0; j < 8; j += 2, q += 32, x += 64, sx += 2) {
            int s1, m1, s2, m2;
            k4_scale_min(j, sc, &s1, &m1); k4_scale_min(j + 1, sc, &s2, &m2);
            const __m256i v = _mm256_loadu_si256((const __m256i *)q);
            const __m256i lo = _mm256_and_si256(v, m4);
            const __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), m4);
            __m256 a = _mm256_add_ps(mul16_u8(_mm256_castsi256_si128(lo), x),
                                     mul16_u8(_mm256_extracti128_si256(lo, 1), x + 16));
            __m256 c = _mm256_add_ps(mul16_u8(_mm256_castsi256_si128(hi), x + 32),
                                     mul16_u8(_mm256_extracti128_si256(hi, 1), x + 48));
            acc = _mm256_fmadd_ps(a, _mm256_set1_ps(d * s1), acc);
            acc = _mm256_fmadd_ps(c, _mm256_set1_ps(d * s2), acc);
            corr += dmin * (m1 * sx[0] + m2 * sx[1]);
        }
    }
    return hsum256(acc) - corr;
#else
    float tmp[256], total = 0.0f;
    (void)sx;
    for (int b = 0; b < cols / 256; b++, row += 144, x += 256) {
        dequant(T_Q4_K, row, tmp, 256);
        total += dot(tmp, x, 256);
    }
    return total;
#endif
}

/* linha Q6_K vezes vetor float: pesos de 6 bits (-32..31), escala int8 por sub-bloco de 16 */
static float dot_q6k(const uint8_t *row, const float *x, int cols) {
#ifdef __AVX2__
    const __m256i m4 = _mm256_set1_epi8(0x0F), m2 = _mm256_set1_epi8(3), off = _mm256_set1_epi8(32);
    __m256 acc = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 210) {
        const uint8_t *ql = row, *qh = row + 128;
        const int8_t *sc = (const int8_t *)(row + 192);
        uint16_t h; memcpy(&h, row + 208, 2);
        const float d = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h)));
        for (int c = 0; c < 2; c++, ql += 64, qh += 32, sc += 8, x += 128) {
            const __m256i l0 = _mm256_loadu_si256((const __m256i *)ql);
            const __m256i l1 = _mm256_loadu_si256((const __m256i *)(ql + 32));
            const __m256i hv = _mm256_loadu_si256((const __m256i *)qh);
            __m256i q[4];
            q[0] = _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(hv, m2), 4));
            q[1] = _mm256_or_si256(_mm256_and_si256(l1, m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 2), m2), 4));
            q[2] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 4), m2), 4));
            q[3] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 6), m2), 4));
            for (int k = 0; k < 4; k++) {
                const __m256i v = _mm256_sub_epi8(q[k], off);
                acc = _mm256_fmadd_ps(mul16_i8(_mm256_castsi256_si128(v), x + 32 * k),
                                      _mm256_set1_ps(d * sc[2 * k]), acc);
                acc = _mm256_fmadd_ps(mul16_i8(_mm256_extracti128_si256(v, 1), x + 32 * k + 16),
                                      _mm256_set1_ps(d * sc[2 * k + 1]), acc);
            }
        }
    }
    return hsum256(acc);
#else
    float tmp[256], total = 0.0f;
    for (int b = 0; b < cols / 256; b++, row += 210, x += 256) {
        dequant(T_Q6_K, row, tmp, 256);
        total += dot(tmp, x, 256);
    }
    return total;
#endif
}

/* ---------- produtos inteiros na CPU ----------
 * As ativacoes sao quantizadas em 16 bits por bloco de 256 colunas (x ~ q * xs[bloco]).
 * Os pesos (4 ou 6 bits) sao alargados para 16 bits e um madd (s16 x s16 -> s32) faz 16
 * produtos de uma vez, sem converter peso nenhum para float; so a soma de cada sub-bloco
 * vira float para receber a escala. Com 16 bits o erro de quantizacao das ativacoes fica
 * na casa de 1e-5 (com 8 bits, medido, chegava a trocar o token escolhido).
 * cpu_exact=1 volta aos produtos em float (referencia exata, usada pelo selftest). */
typedef struct { int16_t *q; int32_t *bs32; float *xs; int cap; int8_t *qh, *ql; int32_t *bs16;
                 int32_t *bs32_h, *bs16_h; } XQ;
/* bs32: soma de cada 32 valores; qh/ql: q = qh*256 + ql em 8 bits e bs16: soma de cada 16 (so com VNNI)
   bs32_h/bs16_h: soma de cada 32/16 valores de qh sozinho (ativacao de 8 bits, escala xs*256; secao 3
   do roadmap, "8-bit activations with scale per block of 256") */
static int cpu_vnni;
/* ativacao de 8 bits (escala por bloco de 256, roadmap 3 item 1) e VNNI de 512 bits (item 3): flags lidas
   em vnni_init, usadas pelos kernels dot_*_v8/dot_*_v8w/dot_*_r8 mais abaixo */
static int act8_opt = -1, cpu_vnni512;
static int act8_env_on(void) { const char *v = getenv("KATANA_ACT8"); return !(v && atoi(v) == 0); }
static int vnni512_env_on(void) { const char *v = getenv("KATANA_VNNI512"); return !(v && atoi(v) == 0); }
/* a entrada quantizada em uso: a global, ou outra escolhida por thread (xq_tls) quando varios
   produtos com entradas diferentes rodam no mesmo despacho do pool (especialistas do MoE) */
static XQ xq0;
#ifdef _MSC_VER
static __declspec(thread) XQ *xq_tls;
#else
static __thread XQ *xq_tls;
#endif
#define xq (*(xq_tls ? xq_tls : &xq0))
static int cpu_exact;
/* selftest --deep: ve o estado de cada (camada, posicao) e a escolha do roteador, em qualquer caminho que passe pela CPU.
   qwen35moe: entrada normalizada do feed-forward; gemma4: residuo no fim da camada */
static void (*layer_tap)(int l, int pos, const float *x);
static void (*route_tap)(int l, const int *sel, const float *w, int K, int unit);
static float *dbg_x; static int *dbg_sel, dbg_pos, dbg_on;   /* medida (KATANA_QN_DBG): residual apos o misturador [pos][camada][d] e escolhidos da CPU exata */

#ifdef __AVX2__
#define HAVE_INT_DOT 1
static void xq_prepare(const float *x, int cols) {
    if (cols > xq.cap) {
        free(xq.q); free(xq.bs32); free(xq.xs); free(xq.qh); free(xq.ql); free(xq.bs16);
        free(xq.bs32_h); free(xq.bs16_h);
        xq.qh = xalloc((size_t)cols + 64, 1); xq.ql = xalloc((size_t)cols + 64, 1);
        xq.bs16 = xalloc((size_t)cols / 16 + 16, sizeof(int32_t));
        xq.bs32_h = xalloc((size_t)cols / 32 + 16, sizeof(int32_t));
        xq.bs16_h = xalloc((size_t)cols / 16 + 16, sizeof(int32_t));
        xq.q = xalloc((size_t)cols + 64, sizeof(int16_t));
        xq.bs32 = xalloc((size_t)cols / 32 + 16, sizeof(int32_t));
        xq.xs = xalloc((size_t)cols / 256 + 1, sizeof(float));
        xq.cap = cols;
    }
    const __m256 absm = _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
    /* o ultimo bloco pode ter menos de 256 valores (multiplo de 32: entrada do down de especialistas
       com H = 704, por exemplo); os produtos dos tipos K so leem blocos inteiros */
    for (int b = 0; b < (cols + 255) / 256; b++) {
        const int len = cols - b * 256 < 256 ? cols - b * 256 : 256, nsub = len / 32;
        const float *xb = x + b * 256;
        int16_t *q = xq.q + b * 256;
        __m256 mx = _mm256_setzero_ps();
        for (int j = 0; j < nsub * 32; j += 8) mx = _mm256_max_ps(mx, _mm256_and_ps(_mm256_loadu_ps(xb + j), absm));
        float t[8], amax = 0.0f;
        _mm256_storeu_ps(t, mx);
        for (int j = 0; j < 8; j++) if (t[j] > amax) amax = t[j];
        if (amax == 0.0f) {
            memset(q, 0, 256 * sizeof(int16_t)); memset(xq.bs32 + b * 8, 0, 8 * sizeof(int32_t)); xq.xs[b] = 0.0f;
            memset(xq.qh + b * 256, 0, 256); memset(xq.ql + b * 256, 0, 256); memset(xq.bs16 + b * 16, 0, 16 * sizeof(int32_t));
            memset(xq.bs32_h + b * 8, 0, 8 * sizeof(int32_t)); memset(xq.bs16_h + b * 16, 0, 16 * sizeof(int32_t));
            continue;
        }
        const __m256 is = _mm256_set1_ps(32512.0f / amax);
        for (int sub = 0; sub < nsub; sub++) {
            __m256i sum = _mm256_setzero_si256();
            for (int j = 0; j < 32; j += 16) {
                const __m256i a = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(xb + sub * 32 + j), is));
                const __m256i c = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(xb + sub * 32 + j + 8), is));
                sum = _mm256_add_epi32(sum, _mm256_add_epi32(a, c));
                _mm256_storeu_si256((__m256i *)(q + sub * 32 + j), _mm256_permute4x64_epi64(_mm256_packs_epi32(a, c), 0xD8));
            }
            __m128i s4 = _mm_add_epi32(_mm256_castsi256_si128(sum), _mm256_extracti128_si256(sum, 1));
            s4 = _mm_add_epi32(s4, _mm_srli_si128(s4, 8));
            s4 = _mm_add_epi32(s4, _mm_srli_si128(s4, 4));
            xq.bs32[b * 8 + sub] = _mm_cvtsi128_si32(s4);
            if (cpu_vnni) {     /* q = alto*256 + baixo (ambos de 8 bits com sinal) e somas de grupos de 16 */
                const __m256i r128 = _mm256_set1_epi16(128), ones = _mm256_set1_epi16(1);
                const __m256i qa = _mm256_loadu_si256((const __m256i *)(q + sub * 32));
                const __m256i qb = _mm256_loadu_si256((const __m256i *)(q + sub * 32 + 16));
                const __m256i ha = _mm256_srai_epi16(_mm256_add_epi16(qa, r128), 8);
                const __m256i hb = _mm256_srai_epi16(_mm256_add_epi16(qb, r128), 8);
                const __m256i la = _mm256_sub_epi16(qa, _mm256_slli_epi16(ha, 8));
                const __m256i lb = _mm256_sub_epi16(qb, _mm256_slli_epi16(hb, 8));
                _mm256_storeu_si256((__m256i *)(xq.qh + b * 256 + sub * 32), _mm256_permute4x64_epi64(_mm256_packs_epi16(ha, hb), 0xD8));
                _mm256_storeu_si256((__m256i *)(xq.ql + b * 256 + sub * 32), _mm256_permute4x64_epi64(_mm256_packs_epi16(la, lb), 0xD8));
                const __m256i sh = _mm256_hadd_epi32(_mm256_madd_epi16(qa, ones), _mm256_madd_epi16(qb, ones));
                const __m256i s2 = _mm256_hadd_epi32(sh, sh);
                xq.bs16[b * 16 + sub * 2] = _mm256_extract_epi32(s2, 0) + _mm256_extract_epi32(s2, 4);
                xq.bs16[b * 16 + sub * 2 + 1] = _mm256_extract_epi32(s2, 1) + _mm256_extract_epi32(s2, 5);
                /* mesma soma, mas de ha/hb (qh sozinho = ativacao de 8 bits, escala xs*256) */
                const __m256i shh = _mm256_hadd_epi32(_mm256_madd_epi16(ha, ones), _mm256_madd_epi16(hb, ones));
                const __m256i s2h = _mm256_hadd_epi32(shh, shh);
                const int32_t h0 = _mm256_extract_epi32(s2h, 0) + _mm256_extract_epi32(s2h, 4);
                const int32_t h1 = _mm256_extract_epi32(s2h, 1) + _mm256_extract_epi32(s2h, 5);
                xq.bs16_h[b * 16 + sub * 2] = h0;
                xq.bs16_h[b * 16 + sub * 2 + 1] = h1;
                xq.bs32_h[b * 8 + sub] = h0 + h1;
            }
        }
        xq.xs[b] = amax / 32512.0f;
    }
}

/* soma de 32 pesos u8 (em v) vezes 32 ativacoes s16: devolve 8 parciais s32 */
static __m256i mul32_u8_s16(__m256i v, const int16_t *x) {
    const __m256i a = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(v));
    const __m256i c = _mm256_cvtepu8_epi16(_mm256_extracti128_si256(v, 1));
    return _mm256_add_epi32(_mm256_madd_epi16(a, _mm256_loadu_si256((const __m256i *)x)),
                            _mm256_madd_epi16(c, _mm256_loadu_si256((const __m256i *)(x + 16))));
}

/* Q4_K inteiro. Por bloco de 256 pesos: as 8 escalas e os 8 minimos de 6 bits saem de 12
   bytes com meia duzia de operacoes de 32 bits (sem desvios). Cada peso vale
   d*sc*q - dmin*m: o termo dos minimos usa as somas de x por sub-bloco (bs32). */
static float dot_q4k_i(const uint8_t *row, int cols) {
    const __m256i m4 = _mm256_set1_epi8(0x0F);
    const uint32_t k1 = 0x3f3f3f3fu, k2 = 0x0f0f0f0fu, k3 = 0x03030303u;
    const int16_t *xp = xq.q;
    const int32_t *bs = xq.bs32;
    __m256 acc = _mm256_setzero_ps(), accm = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 144, xp += 256, bs += 8) {
        uint16_t h0, h1; uint32_t u[4];
        memcpy(&h0, row, 2); memcpy(&h1, row + 2, 2); memcpy(u, row + 4, 12);
        u[3] = ((u[2] >> 4) & k2) | (((u[1] >> 6) & k3) << 4);     /* minimos 4..7 */
        const uint32_t aux = u[1] & k1;                              /* minimos 0..3 */
        u[1] = (u[2] & k2) | (((u[0] >> 6) & k3) << 4);             /* escalas 4..7 */
        u[2] = aux;
        u[0] &= k1;                                                  /* escalas 0..3 */
        const float xs = xq.xs[b];
        const float d = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h0))) * xs;
        const float dmin = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h1))) * xs;
        const __m256i sc32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)u));
        const __m256i mn32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(u + 2)));
        const __m256 scf = _mm256_mul_ps(_mm256_cvtepi32_ps(sc32), _mm256_set1_ps(d));   /* d*xs*escala[j] */
        accm = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_mullo_epi32(mn32, _mm256_loadu_si256((const __m256i *)bs))),
                               _mm256_set1_ps(dmin), accm);
        const uint8_t *q = row + 16;
        for (int j = 0; j < 4; j++) {
            const __m256i v = _mm256_loadu_si256((const __m256i *)(q + 32 * j));
            const __m256i lo = _mm256_and_si256(v, m4), hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), m4);
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(mul32_u8_s16(lo, xp + 64 * j)),
                                  _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * j)), acc);
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(mul32_u8_s16(hi, xp + 64 * j + 32)),
                                  _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * j + 1)), acc);
        }
    }
    return hsum256(acc) - hsum256(accm);
}

/* Q6_K inteiro: pesos de 6 bits (-32..31), uma escala int8 para cada 16 pesos */
static float dot_q6k_i(const uint8_t *row, int cols) {
    const __m256i m4 = _mm256_set1_epi8(0x0F), m2 = _mm256_set1_epi8(3), off = _mm256_set1_epi16(32);
    const int16_t *xp = xq.q;
    __m256 acc = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 210, xp += 256) {
        const uint8_t *ql = row, *qh = row + 128;
        uint16_t h; memcpy(&h, row + 208, 2);
        const __m256 dv = _mm256_set1_ps(_mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))) * xq.xs[b]);
        const __m128i s8 = _mm_loadu_si128((const __m128i *)(row + 192));
        for (int c = 0; c < 2; c++, ql += 64, qh += 32) {
            /* 8 escalas deste meio bloco, ja multiplicadas por d*xs */
            const __m256 scf = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(c ? _mm_srli_si128(s8, 8) : s8)), dv);
            const __m256i l0 = _mm256_loadu_si256((const __m256i *)ql);
            const __m256i l1 = _mm256_loadu_si256((const __m256i *)(ql + 32));
            const __m256i hv = _mm256_loadu_si256((const __m256i *)qh);
            __m256i q[4];
            q[0] = _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(hv, m2), 4));
            q[1] = _mm256_or_si256(_mm256_and_si256(l1, m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 2), m2), 4));
            q[2] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 4), m2), 4));
            q[3] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 6), m2), 4));
            for (int k = 0; k < 4; k++) {
                const int16_t *x = xp + c * 128 + k * 32;
                const __m256i a = _mm256_sub_epi16(_mm256_cvtepu8_epi16(_mm256_castsi256_si128(q[k])), off);
                const __m256i e = _mm256_sub_epi16(_mm256_cvtepu8_epi16(_mm256_extracti128_si256(q[k], 1)), off);
                acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_madd_epi16(a, _mm256_loadu_si256((const __m256i *)x))),
                                      _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * k)), acc);
                acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_madd_epi16(e, _mm256_loadu_si256((const __m256i *)(x + 16)))),
                                      _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * k + 1)), acc);
            }
        }
    }
    return hsum256(acc);
}

/* Q5_K inteiro: como o Q4_K, com o quinto bit de cada peso vindo de uma mascara de 32 bytes */
static float dot_q5k_i(const uint8_t *row, int cols) {
    const __m256i m4 = _mm256_set1_epi8(0x0F), one = _mm256_set1_epi8(1);
    const uint32_t k1 = 0x3f3f3f3fu, k2 = 0x0f0f0f0fu, k3 = 0x03030303u;
    const int16_t *xp = xq.q;
    const int32_t *bs = xq.bs32;
    __m256 acc = _mm256_setzero_ps(), accm = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 176, xp += 256, bs += 8) {
        uint16_t h0, h1; uint32_t u[4];
        memcpy(&h0, row, 2); memcpy(&h1, row + 2, 2); memcpy(u, row + 4, 12);
        u[3] = ((u[2] >> 4) & k2) | (((u[1] >> 6) & k3) << 4);
        const uint32_t aux = u[1] & k1;
        u[1] = (u[2] & k2) | (((u[0] >> 6) & k3) << 4);
        u[2] = aux;
        u[0] &= k1;
        const float xs = xq.xs[b];
        const float d = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h0))) * xs;
        const float dmin = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h1))) * xs;
        const __m256i sc32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)u));
        const __m256i mn32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(u + 2)));
        const __m256 scf = _mm256_mul_ps(_mm256_cvtepi32_ps(sc32), _mm256_set1_ps(d));
        accm = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_mullo_epi32(mn32, _mm256_loadu_si256((const __m256i *)bs))),
                               _mm256_set1_ps(dmin), accm);
        const __m256i hv = _mm256_loadu_si256((const __m256i *)(row + 16));
        const uint8_t *q = row + 48;
        for (int j = 0; j < 4; j++) {
            const __m256i v = _mm256_loadu_si256((const __m256i *)(q + 32 * j));
            const __m256i lo = _mm256_or_si256(_mm256_and_si256(v, m4),
                               _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 2 * j), one), 4));
            const __m256i hi = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(v, 4), m4),
                               _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 2 * j + 1), one), 4));
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(mul32_u8_s16(lo, xp + 64 * j)),
                                  _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * j)), acc);
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(mul32_u8_s16(hi, xp + 64 * j + 32)),
                                  _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * j + 1)), acc);
        }
    }
    return hsum256(acc) - hsum256(accm);
}

/* Q3_K inteiro: pesos de 3 bits (-4..3) = 2 bits + 1 bit alto numa mascara; 16 escalas de 6 bits */
static float dot_q3k_i(const uint8_t *row, int cols) {
    const __m256i m3 = _mm256_set1_epi8(3), one = _mm256_set1_epi8(1), four = _mm256_set1_epi16(4);
    const uint32_t km1 = 0x03030303u, km2 = 0x0f0f0f0fu;
    const int16_t *xp = xq.q;
    __m256 acc = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 110, xp += 256) {
        uint32_t aux[4]; uint16_t h;
        memcpy(aux, row + 96, 12); memcpy(&h, row + 108, 2);
        const uint32_t tmp = aux[2];
        aux[2] = ((aux[0] >> 4) & km2) | (((tmp >> 4) & km1) << 4);
        aux[3] = ((aux[1] >> 4) & km2) | (((tmp >> 6) & km1) << 4);
        aux[0] = (aux[0] & km2) | (((tmp >> 0) & km1) << 4);
        aux[1] = (aux[1] & km2) | (((tmp >> 2) & km1) << 4);
        const __m128i s8 = _mm_sub_epi8(_mm_loadu_si128((const __m128i *)aux), _mm_set1_epi8(32));
        const __m256 dv = _mm256_set1_ps(_mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))) * xq.xs[b]);
        const __m256i hm = _mm256_loadu_si256((const __m256i *)row);
        for (int j = 0; j < 2; j++) {
            const __m256 scf = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(j ? _mm_srli_si128(s8, 8) : s8)), dv);
            const __m256i qv = _mm256_loadu_si256((const __m256i *)(row + 32 + 32 * j));
            for (int k = 0; k < 4; k++) {
                const int16_t *x = xp + (j * 4 + k) * 32;
                const __m256i u = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(qv, 2 * k), m3),
                                  _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hm, 4 * j + k), one), 2));
                const __m256i a = _mm256_sub_epi16(_mm256_cvtepu8_epi16(_mm256_castsi256_si128(u)), four);
                const __m256i e = _mm256_sub_epi16(_mm256_cvtepu8_epi16(_mm256_extracti128_si256(u, 1)), four);
                acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_madd_epi16(a, _mm256_loadu_si256((const __m256i *)x))),
                                      _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * k)), acc);
                acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_madd_epi16(e, _mm256_loadu_si256((const __m256i *)(x + 16)))),
                                      _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * k + 1)), acc);
            }
        }
    }
    return hsum256(acc);
}

/* Q8_0 inteiro: blocos de 32 pesos int8 com escala fp16; a ativacao de 16 bits tem uma escala a
   cada 256 valores (xs[b/8]) */
static float dot_q8_i(const uint8_t *row, int cols) {
    const int16_t *xp = xq.q;
    __m256 acc = _mm256_setzero_ps();
    for (int b = 0; b < cols / 32; b++, row += 34, xp += 32) {
        uint16_t h; memcpy(&h, row, 2);
        const float dd = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))) * xq.xs[b >> 3];
        const __m256i w = _mm256_loadu_si256((const __m256i *)(row + 2));
        const __m256i a = _mm256_cvtepi8_epi16(_mm256_castsi256_si128(w)), c = _mm256_cvtepi8_epi16(_mm256_extracti128_si256(w, 1));
        const __m256i p = _mm256_add_epi32(_mm256_madd_epi16(a, _mm256_loadu_si256((const __m256i *)xp)),
                                           _mm256_madd_epi16(c, _mm256_loadu_si256((const __m256i *)(xp + 16))));
        acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(p), _mm256_set1_ps(dd), acc);
    }
    return hsum256(acc);
}

/* Q5_1 inteiro: peso = d*q + m com q de 5 bits (4 num nibble + 1 de uma mascara de 32 bits);
   o termo de m usa a soma das ativacoes de cada bloco de 32 (bs32) */
static float dot_q51_i(const uint8_t *row, int cols) {
    const __m128i m4 = _mm_set1_epi8(0x0F);
    const __m256i shuf = _mm256_setr_epi8(0,0,0,0,0,0,0,0, 1,1,1,1,1,1,1,1, 2,2,2,2,2,2,2,2, 3,3,3,3,3,3,3,3);
    const __m256i bitm = _mm256_set1_epi64x(0x8040201008040201LL), f16b = _mm256_set1_epi8(0x10);
    const int16_t *xp = xq.q;
    const int32_t *bs = xq.bs32;
    __m256 acc = _mm256_setzero_ps();
    float accm = 0.0f;
    for (int b = 0; b < cols / 32; b++, row += 24, xp += 32) {
        uint32_t qh; memcpy(&qh, row + 4, 4);
        int32_t dm; memcpy(&dm, row, 4);
        const __m128 hd = _mm_cvtph_ps(_mm_cvtsi32_si128(dm));   /* d, m */
        const float xs = xq.xs[b >> 3];
        const float d = _mm_cvtss_f32(hd) * xs, mm = _mm_cvtss_f32(_mm_shuffle_ps(hd, hd, 1)) * xs;
        const __m128i v = _mm_loadu_si128((const __m128i *)(row + 8));
        const __m256i nib = _mm256_set_m128i(_mm_and_si128(_mm_srli_epi16(v, 4), m4), _mm_and_si128(v, m4));
        __m256i hb = _mm256_shuffle_epi8(_mm256_set1_epi32((int)qh), shuf);
        hb = _mm256_and_si256(_mm256_cmpeq_epi8(_mm256_and_si256(hb, bitm), bitm), f16b);
        const __m256i p = mul32_u8_s16(_mm256_or_si256(nib, hb), xp);
        acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(p), _mm256_set1_ps(d), acc);
        accm += mm * (float)bs[b];
    }
    return hsum256(acc) + accm;
}
#else
#define HAVE_INT_DOT 0
static float dot_q8_i(const uint8_t *row, int cols) { (void)row; (void)cols; return 0.0f; }
static float dot_q51_i(const uint8_t *row, int cols) { (void)row; (void)cols; return 0.0f; }
static float dot_q5k_i(const uint8_t *row, int cols) { (void)row; (void)cols; return 0.0f; }
static float dot_q3k_i(const uint8_t *row, int cols) { (void)row; (void)cols; return 0.0f; }
static void xq_prepare(const float *x, int cols) { (void)x; (void)cols; }
static float dot_q4k_i(const uint8_t *row, int cols) { (void)row; (void)cols; return 0.0f; }
static float dot_q6k_i(const uint8_t *row, int cols) { (void)row; (void)cols; return 0.0f; }
#endif

/* ---------- produto inteiro com VNNI (AVX-512, vetores de 256 bits) ----------
   Mesma conta do produto de 16 bits acima, mas o peso (4 ou 6 bits, sem sinal em 0..63) fica em 8 bits
   e a ativacao de 16 bits vira duas de 8 bits: x = alto*256 + baixo (os dois com sinal). Dois
   _mm256_dpbusd_epi32 (u8 x s8, somando 4 produtos por faixa direto em 32 bits) fazem o trabalho de
   dois alargamentos + dois madd; o resultado inteiro e exatamente o mesmo (sem perda de precisao).
   So roda se a CPU tiver AVX512 VL/BW/VNNI (detectado em tempo de execucao; KATANA_VNNI=0 desliga);
   as funcoes levam o atributo target, entao o binario continua rodando em maquina sem AVX-512. */
#if defined(__GNUC__) && defined(__x86_64__) && defined(__AVX2__)
#define HAVE_VNNI 1
#define VNNI_T __attribute__((target("avx2,fma,f16c,avx512f,avx512vl,avx512bw,avx512vnni")))
static void vnni_init(void) {
    __builtin_cpu_init();
    cpu_vnni = __builtin_cpu_supports("avx512vnni") && __builtin_cpu_supports("avx512vl") &&
               __builtin_cpu_supports("avx512bw") && !(getenv("KATANA_VNNI") && atoi(getenv("KATANA_VNNI")) == 0);
    act8_opt = act8_env_on();
    cpu_vnni512 = cpu_vnni && __builtin_cpu_supports("avx512f") && vnni512_env_on();
}

/* Q4_K: escalas e minimos como em dot_q4k_i; o termo dos minimos usa bs32 */
VNNI_T static float dot_q4k_v(const uint8_t *row, int cols) {
    const __m256i m4 = _mm256_set1_epi8(0x0F), zero = _mm256_setzero_si256();
    const uint32_t k1 = 0x3f3f3f3fu, k2 = 0x0f0f0f0fu, k3 = 0x03030303u;
    const int8_t *xh = xq.qh, *xl = xq.ql;
    const int32_t *bs = xq.bs32;
    __m256 acc = _mm256_setzero_ps(), accm = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 144, xh += 256, xl += 256, bs += 8) {
        uint16_t h0, h1; uint32_t u[4];
        memcpy(&h0, row, 2); memcpy(&h1, row + 2, 2); memcpy(u, row + 4, 12);
        u[3] = ((u[2] >> 4) & k2) | (((u[1] >> 6) & k3) << 4);
        const uint32_t aux = u[1] & k1;
        u[1] = (u[2] & k2) | (((u[0] >> 6) & k3) << 4);
        u[2] = aux;
        u[0] &= k1;
        const float xs = xq.xs[b];
        const float d = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h0))) * xs;
        const float dmin = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h1))) * xs;
        const __m256i sc32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)u));
        const __m256i mn32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(u + 2)));
        const __m256 scf = _mm256_mul_ps(_mm256_cvtepi32_ps(sc32), _mm256_set1_ps(d));
        accm = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_mullo_epi32(mn32, _mm256_loadu_si256((const __m256i *)bs))),
                               _mm256_set1_ps(dmin), accm);
        const uint8_t *q = row + 16;
        for (int j = 0; j < 4; j++) {
            const __m256i v = _mm256_loadu_si256((const __m256i *)(q + 32 * j));
            const __m256i lo = _mm256_and_si256(v, m4), hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), m4);
            __m256i p = _mm256_dpbusd_epi32(zero, lo, _mm256_loadu_si256((const __m256i *)(xh + 64 * j)));
            p = _mm256_dpbusd_epi32(_mm256_slli_epi32(p, 8), lo, _mm256_loadu_si256((const __m256i *)(xl + 64 * j)));
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(p), _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * j)), acc);
            p = _mm256_dpbusd_epi32(zero, hi, _mm256_loadu_si256((const __m256i *)(xh + 64 * j + 32)));
            p = _mm256_dpbusd_epi32(_mm256_slli_epi32(p, 8), hi, _mm256_loadu_si256((const __m256i *)(xl + 64 * j + 32)));
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(p), _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * j + 1)), acc);
        }
    }
    return hsum256(acc) - hsum256(accm);
}

/* Q6_K: pesos u8 em 0..63 (o deslocamento -32 sai somando 32 * soma das ativacoes de cada grupo de 16, bs16) */
VNNI_T static float dot_q6k_v(const uint8_t *row, int cols) {
    const __m256i m4 = _mm256_set1_epi8(0x0F), m2 = _mm256_set1_epi8(3), zero = _mm256_setzero_si256();
    const int8_t *xh = xq.qh, *xl = xq.ql;
    const int32_t *bs = xq.bs16;
    __m256 acc = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 210, xh += 256, xl += 256, bs += 16) {
        const uint8_t *ql = row, *qh = row + 128;
        const int8_t *sc = (const int8_t *)(row + 192);
        uint16_t h; memcpy(&h, row + 208, 2);
        const float dv = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))) * xq.xs[b];
        for (int c = 0; c < 2; c++, ql += 64, qh += 32, sc += 8) {
            const __m256i l0 = _mm256_loadu_si256((const __m256i *)ql);
            const __m256i l1 = _mm256_loadu_si256((const __m256i *)(ql + 32));
            const __m256i hv = _mm256_loadu_si256((const __m256i *)qh);
            __m256i q[4];
            q[0] = _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(hv, m2), 4));
            q[1] = _mm256_or_si256(_mm256_and_si256(l1, m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 2), m2), 4));
            q[2] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 4), m2), 4));
            q[3] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 6), m2), 4));
            for (int k = 0; k < 4; k++) {
                const int o = c * 128 + k * 32;
                __m256i p = _mm256_dpbusd_epi32(zero, q[k], _mm256_loadu_si256((const __m256i *)(xh + o)));
                p = _mm256_dpbusd_epi32(_mm256_slli_epi32(p, 8), q[k], _mm256_loadu_si256((const __m256i *)(xl + o)));
                /* o deslocamento entra uma vez por grupo de 16 (numa faixa so; as faixas sao somadas no fim) */
                const __m256i corr = _mm256_set_epi32(0, 0, 0, 32 * bs[c * 8 + 2 * k + 1], 0, 0, 0, 32 * bs[c * 8 + 2 * k]);
                const __m256 scv = _mm256_insertf128_ps(_mm256_castps128_ps256(_mm_set1_ps(dv * sc[2 * k])),
                                                        _mm_set1_ps(dv * sc[2 * k + 1]), 1);
                acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_sub_epi32(p, corr)), scv, acc);
            }
        }
    }
    return hsum256(acc);
}

/* ---------- 4 linhas por passo (VNNI): a ativacao de cada bloco e lida uma vez e usada por 4 linhas,
   com acumuladores independentes; mesmo resultado inteiro de dot_q4k_v/dot_q6k_v (so muda a ordem das somas em float).
   KATANA_MOE_ROWS=0 desliga (moe_opt_rows). Escreve out[0..3] ---------- */
VNNI_T static void dot_q4k_v4(const uint8_t *row0, size_t stride, int cols, float *out) {
    const __m256i m4 = _mm256_set1_epi8(0x0F), zero = _mm256_setzero_si256();
    const uint32_t k1 = 0x3f3f3f3fu, k2 = 0x0f0f0f0fu, k3 = 0x03030303u;
    const int8_t *xh = xq.qh, *xl = xq.ql;
    const int32_t *bs = xq.bs32;
    __m256 acc[4], accm[4];
    for (int r = 0; r < 4; r++) { acc[r] = _mm256_setzero_ps(); accm[r] = _mm256_setzero_ps(); }
    const uint8_t *rw[4] = { row0, row0 + stride, row0 + 2 * stride, row0 + 3 * stride };
    for (int b = 0; b < cols / 256; b++, xh += 256, xl += 256, bs += 8) {
        __m256 scf[4];
        const float xs = xq.xs[b];
        const __m256i bsv = _mm256_loadu_si256((const __m256i *)bs);
        for (int r = 0; r < 4; r++) {
            const uint8_t *row = rw[r] + (size_t)b * 144;
            uint16_t h0, h1; uint32_t u[4];
            memcpy(&h0, row, 2); memcpy(&h1, row + 2, 2); memcpy(u, row + 4, 12);
            u[3] = ((u[2] >> 4) & k2) | (((u[1] >> 6) & k3) << 4);
            const uint32_t aux = u[1] & k1;
            u[1] = (u[2] & k2) | (((u[0] >> 6) & k3) << 4);
            u[2] = aux;
            u[0] &= k1;
            const float d = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h0))) * xs;
            const float dmin = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h1))) * xs;
            const __m256i sc32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)u));
            const __m256i mn32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(u + 2)));
            scf[r] = _mm256_mul_ps(_mm256_cvtepi32_ps(sc32), _mm256_set1_ps(d));
            accm[r] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_mullo_epi32(mn32, bsv)), _mm256_set1_ps(dmin), accm[r]);
        }
        for (int j = 0; j < 4; j++) {
            const __m256i xh0 = _mm256_loadu_si256((const __m256i *)(xh + 64 * j));
            const __m256i xl0 = _mm256_loadu_si256((const __m256i *)(xl + 64 * j));
            const __m256i xh1 = _mm256_loadu_si256((const __m256i *)(xh + 64 * j + 32));
            const __m256i xl1 = _mm256_loadu_si256((const __m256i *)(xl + 64 * j + 32));
            const __m256i sj0 = _mm256_set1_epi32(2 * j), sj1 = _mm256_set1_epi32(2 * j + 1);
            for (int r = 0; r < 4; r++) {
                const __m256i v = _mm256_loadu_si256((const __m256i *)(rw[r] + (size_t)b * 144 + 16 + 32 * j));
                const __m256i lo = _mm256_and_si256(v, m4), hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), m4);
                __m256i p = _mm256_dpbusd_epi32(zero, lo, xh0);
                p = _mm256_dpbusd_epi32(_mm256_slli_epi32(p, 8), lo, xl0);
                acc[r] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(p), _mm256_permutevar8x32_ps(scf[r], sj0), acc[r]);
                p = _mm256_dpbusd_epi32(zero, hi, xh1);
                p = _mm256_dpbusd_epi32(_mm256_slli_epi32(p, 8), hi, xl1);
                acc[r] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(p), _mm256_permutevar8x32_ps(scf[r], sj1), acc[r]);
            }
        }
    }
    for (int r = 0; r < 4; r++) out[r] = hsum256(acc[r]) - hsum256(accm[r]);
}

VNNI_T static void dot_q6k_v4(const uint8_t *row0, size_t stride, int cols, float *out) {
    const __m256i m4 = _mm256_set1_epi8(0x0F), m2 = _mm256_set1_epi8(3), zero = _mm256_setzero_si256();
    const int8_t *xh = xq.qh, *xl = xq.ql;
    const int32_t *bs = xq.bs16;
    __m256 acc[4];
    for (int r = 0; r < 4; r++) acc[r] = _mm256_setzero_ps();
    const uint8_t *rw[4] = { row0, row0 + stride, row0 + 2 * stride, row0 + 3 * stride };
    for (int b = 0; b < cols / 256; b++, xh += 256, xl += 256, bs += 16) {
        float dv[4];
        for (int r = 0; r < 4; r++) {
            uint16_t h; memcpy(&h, rw[r] + (size_t)b * 210 + 208, 2);
            dv[r] = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))) * xq.xs[b];
        }
        for (int c = 0; c < 2; c++) {
            __m256i xhv[4], xlv[4];
            for (int k = 0; k < 4; k++) {
                xhv[k] = _mm256_loadu_si256((const __m256i *)(xh + c * 128 + k * 32));
                xlv[k] = _mm256_loadu_si256((const __m256i *)(xl + c * 128 + k * 32));
            }
            for (int r = 0; r < 4; r++) {
                const uint8_t *row = rw[r] + (size_t)b * 210;
                const uint8_t *ql = row + 64 * c, *qh = row + 128 + 32 * c;
                const int8_t *sc = (const int8_t *)(row + 192) + 8 * c;
                const __m256i l0 = _mm256_loadu_si256((const __m256i *)ql);
                const __m256i l1 = _mm256_loadu_si256((const __m256i *)(ql + 32));
                const __m256i hv = _mm256_loadu_si256((const __m256i *)qh);
                __m256i q[4];
                q[0] = _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(hv, m2), 4));
                q[1] = _mm256_or_si256(_mm256_and_si256(l1, m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 2), m2), 4));
                q[2] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 4), m2), 4));
                q[3] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 6), m2), 4));
                for (int k = 0; k < 4; k++) {
                    __m256i p = _mm256_dpbusd_epi32(zero, q[k], xhv[k]);
                    p = _mm256_dpbusd_epi32(_mm256_slli_epi32(p, 8), q[k], xlv[k]);
                    const __m256i corr = _mm256_set_epi32(0, 0, 0, 32 * bs[c * 8 + 2 * k + 1], 0, 0, 0, 32 * bs[c * 8 + 2 * k]);
                    const __m256 scv = _mm256_insertf128_ps(_mm256_castps128_ps256(_mm_set1_ps(dv[r] * sc[2 * k])),
                                                            _mm_set1_ps(dv[r] * sc[2 * k + 1]), 1);
                    acc[r] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_sub_epi32(p, corr)), scv, acc[r]);
                }
            }
        }
    }
    for (int r = 0; r < 4; r++) out[r] = hsum256(acc[r]);
}

/* SiLU vetorizado: g/(1+exp(-g)) com exp por polinomio de grau 6 (erro relativo ~2e-7); KATANA_MOE_ACT=0 desliga */
VNNI_T static void silu_mul_v(float *g, const float *u, int n) {
    const __m256 l2e = _mm256_set1_ps(1.4426950409f), ln2 = _mm256_set1_ps(0.6931471806f), one = _mm256_set1_ps(1.0f);
    const __m256 c1 = _mm256_set1_ps(1.0f), c2 = _mm256_set1_ps(0.5f), c3 = _mm256_set1_ps(0.16666667f),
                 c4 = _mm256_set1_ps(0.041666668f), c5 = _mm256_set1_ps(0.008333334f), c6 = _mm256_set1_ps(0.0013888889f);
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256 x = _mm256_loadu_ps(g + i);
        __m256 t = _mm256_max_ps(_mm256_min_ps(_mm256_sub_ps(_mm256_setzero_ps(), x), _mm256_set1_ps(80.0f)), _mm256_set1_ps(-80.0f));
        const __m256 kf = _mm256_round_ps(_mm256_mul_ps(t, l2e), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
        const __m256 r = _mm256_fnmadd_ps(kf, ln2, t);
        __m256 p = _mm256_fmadd_ps(c6, r, c5);
        p = _mm256_fmadd_ps(p, r, c4); p = _mm256_fmadd_ps(p, r, c3); p = _mm256_fmadd_ps(p, r, c2); p = _mm256_fmadd_ps(p, r, c1);
        p = _mm256_fmadd_ps(p, r, one);
        const __m256i e = _mm256_slli_epi32(_mm256_add_epi32(_mm256_cvtps_epi32(kf), _mm256_set1_epi32(127)), 23);
        const __m256 ex = _mm256_mul_ps(p, _mm256_castsi256_ps(e));
        _mm256_storeu_ps(g + i, _mm256_mul_ps(_mm256_div_ps(x, _mm256_add_ps(one, ex)), _mm256_loadu_ps(u + i)));
    }
    for (; i < n; i++) g[i] = g[i] / (1.0f + expf(-g[i])) * u[i];
}

/* Q5_K: como o Q4_K com VNNI, o quinto bit vem da mascara de 32 bytes (pesos u8 em 0..31) */
VNNI_T static float dot_q5k_v(const uint8_t *row, int cols) {
    const __m256i m4 = _mm256_set1_epi8(0x0F), one = _mm256_set1_epi8(1), zero = _mm256_setzero_si256();
    const uint32_t k1 = 0x3f3f3f3fu, k2 = 0x0f0f0f0fu, k3 = 0x03030303u;
    const int8_t *xh = xq.qh, *xl = xq.ql;
    const int32_t *bs = xq.bs32;
    __m256 acc = _mm256_setzero_ps(), accm = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 176, xh += 256, xl += 256, bs += 8) {
        uint16_t h0, h1; uint32_t u[4];
        memcpy(&h0, row, 2); memcpy(&h1, row + 2, 2); memcpy(u, row + 4, 12);
        u[3] = ((u[2] >> 4) & k2) | (((u[1] >> 6) & k3) << 4);
        const uint32_t aux = u[1] & k1;
        u[1] = (u[2] & k2) | (((u[0] >> 6) & k3) << 4);
        u[2] = aux;
        u[0] &= k1;
        const float xs = xq.xs[b];
        const float d = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h0))) * xs;
        const float dmin = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h1))) * xs;
        const __m256i sc32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)u));
        const __m256i mn32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(u + 2)));
        const __m256 scf = _mm256_mul_ps(_mm256_cvtepi32_ps(sc32), _mm256_set1_ps(d));
        accm = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_mullo_epi32(mn32, _mm256_loadu_si256((const __m256i *)bs))),
                               _mm256_set1_ps(dmin), accm);
        const __m256i hv = _mm256_loadu_si256((const __m256i *)(row + 16));
        const uint8_t *q = row + 48;
        for (int j = 0; j < 4; j++) {
            const __m256i v = _mm256_loadu_si256((const __m256i *)(q + 32 * j));
            const __m256i lo = _mm256_or_si256(_mm256_and_si256(v, m4),
                               _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 2 * j), one), 4));
            const __m256i hi = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(v, 4), m4),
                               _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 2 * j + 1), one), 4));
            __m256i p = _mm256_dpbusd_epi32(zero, lo, _mm256_loadu_si256((const __m256i *)(xh + 64 * j)));
            p = _mm256_dpbusd_epi32(_mm256_slli_epi32(p, 8), lo, _mm256_loadu_si256((const __m256i *)(xl + 64 * j)));
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(p), _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * j)), acc);
            p = _mm256_dpbusd_epi32(zero, hi, _mm256_loadu_si256((const __m256i *)(xh + 64 * j + 32)));
            p = _mm256_dpbusd_epi32(_mm256_slli_epi32(p, 8), hi, _mm256_loadu_si256((const __m256i *)(xl + 64 * j + 32)));
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(p), _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * j + 1)), acc);
        }
    }
    return hsum256(acc) - hsum256(accm);
}

/* ---------- ativacao de 8 bits, escala por bloco de 256 (roadmap 3, item 1) ----------
   qh (acima) ja e round(q/256): um q de 16 bits (escala xs por bloco de 256) partido em dois de 8 bits
   (qh*256+ql) sem perda. Usando SO qh como ativacao (escala xs*256) perdemos os ~8 bits baixos de q, mas
   cada grupo de 32 pesos vira 1 vpdpbusd em vez de 2 (dobra os elementos por grupo de instrucao); o termo
   dos minimos (Q4_K) e o deslocamento (Q6_K) saem de bs32_h/bs16_h (soma de qh, nao de q). Isto É uma perda
   real de precisao (diferente do par qh/ql, que e exato); validado numericamente contra o produto em dobro
   (mat_row + double) no qtest/moebench. KATANA_ACT8=0 desliga (so volta ao caminho de cima). */

VNNI_T static float dot_q4k_v8(const uint8_t *row, int cols) {
    const __m256i m4 = _mm256_set1_epi8(0x0F), zero = _mm256_setzero_si256();
    const uint32_t k1 = 0x3f3f3f3fu, k2 = 0x0f0f0f0fu, k3 = 0x03030303u;
    const int8_t *xh = xq.qh;
    const int32_t *bs = xq.bs32_h;
    __m256 acc = _mm256_setzero_ps(), accm = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 144, xh += 256, bs += 8) {
        uint16_t h0, h1; uint32_t u[4];
        memcpy(&h0, row, 2); memcpy(&h1, row + 2, 2); memcpy(u, row + 4, 12);
        u[3] = ((u[2] >> 4) & k2) | (((u[1] >> 6) & k3) << 4);
        const uint32_t aux = u[1] & k1;
        u[1] = (u[2] & k2) | (((u[0] >> 6) & k3) << 4);
        u[2] = aux;
        u[0] &= k1;
        const float xs8 = xq.xs[b] * 256.0f;
        const float d = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h0))) * xs8;
        const float dmin = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h1))) * xs8;
        const __m256i sc32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)u));
        const __m256i mn32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(u + 2)));
        const __m256 scf = _mm256_mul_ps(_mm256_cvtepi32_ps(sc32), _mm256_set1_ps(d));
        accm = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_mullo_epi32(mn32, _mm256_loadu_si256((const __m256i *)bs))),
                               _mm256_set1_ps(dmin), accm);
        const uint8_t *q = row + 16;
        for (int j = 0; j < 4; j++) {
            const __m256i v = _mm256_loadu_si256((const __m256i *)(q + 32 * j));
            const __m256i lo = _mm256_and_si256(v, m4), hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), m4);
            const __m256i pl = _mm256_dpbusd_epi32(zero, lo, _mm256_loadu_si256((const __m256i *)(xh + 64 * j)));
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(pl), _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * j)), acc);
            const __m256i ph = _mm256_dpbusd_epi32(zero, hi, _mm256_loadu_si256((const __m256i *)(xh + 64 * j + 32)));
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(ph), _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * j + 1)), acc);
        }
    }
    return hsum256(acc) - hsum256(accm);
}

VNNI_T static float dot_q6k_v8(const uint8_t *row, int cols) {
    const __m256i m4 = _mm256_set1_epi8(0x0F), m2 = _mm256_set1_epi8(3), zero = _mm256_setzero_si256();
    const int8_t *xh = xq.qh;
    const int32_t *bs = xq.bs16_h;
    __m256 acc = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 210, xh += 256, bs += 16) {
        const uint8_t *ql = row, *qh = row + 128;
        const int8_t *sc = (const int8_t *)(row + 192);
        uint16_t h; memcpy(&h, row + 208, 2);
        const float dv = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))) * xq.xs[b] * 256.0f;
        for (int c = 0; c < 2; c++, ql += 64, qh += 32, sc += 8) {
            const __m256i l0 = _mm256_loadu_si256((const __m256i *)ql);
            const __m256i l1 = _mm256_loadu_si256((const __m256i *)(ql + 32));
            const __m256i hv = _mm256_loadu_si256((const __m256i *)qh);
            __m256i q[4];
            q[0] = _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(hv, m2), 4));
            q[1] = _mm256_or_si256(_mm256_and_si256(l1, m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 2), m2), 4));
            q[2] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 4), m2), 4));
            q[3] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 6), m2), 4));
            for (int k = 0; k < 4; k++) {
                const int o = c * 128 + k * 32;
                const __m256i p = _mm256_dpbusd_epi32(zero, q[k], _mm256_loadu_si256((const __m256i *)(xh + o)));
                const __m256i corr = _mm256_set_epi32(0, 0, 0, 32 * bs[c * 8 + 2 * k + 1], 0, 0, 0, 32 * bs[c * 8 + 2 * k]);
                const __m256 scv = _mm256_insertf128_ps(_mm256_castps128_ps256(_mm_set1_ps(dv * sc[2 * k])),
                                                        _mm_set1_ps(dv * sc[2 * k + 1]), 1);
                acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_sub_epi32(p, corr)), scv, acc);
            }
        }
    }
    return hsum256(acc);
}

/* ---------- AVX-512 largo (roadmap 3, item 3): o mesmo produto de 8 bits acima, mas lo+hi (Q4_K) ou
   duas faixas de 32 (Q6_K) viram um unico vpdpbusd de 512 bits (64 bytes) em vez de dois de 256: dobra a
   largura por cima do item 1 (que ja tinha dobrado os elementos por instrucao trocando o par qh/ql por so
   qh). So roda se a CPU tiver AVX-512 (mesmo alvo de funcao do VNNI_T, que ja exige avx512f); selecao em
   tempo de execucao por cpu_vnni512 (KATANA_VNNI512=0 desliga, cai no dot_q4k_v8/dot_q6k_v8 de 256 bits). */
VNNI_T static float dot_q4k_v8w(const uint8_t *row, int cols) {
    const __m256i m4 = _mm256_set1_epi8(0x0F);
    const uint32_t k1 = 0x3f3f3f3fu, k2 = 0x0f0f0f0fu, k3 = 0x03030303u;
    const int8_t *xh = xq.qh;
    const int32_t *bs = xq.bs32_h;
    __m256 acc = _mm256_setzero_ps(), accm = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 144, xh += 256, bs += 8) {
        uint16_t h0, h1; uint32_t u[4];
        memcpy(&h0, row, 2); memcpy(&h1, row + 2, 2); memcpy(u, row + 4, 12);
        u[3] = ((u[2] >> 4) & k2) | (((u[1] >> 6) & k3) << 4);
        const uint32_t aux = u[1] & k1;
        u[1] = (u[2] & k2) | (((u[0] >> 6) & k3) << 4);
        u[2] = aux;
        u[0] &= k1;
        const float xs8 = xq.xs[b] * 256.0f;
        const float d = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h0))) * xs8;
        const float dmin = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h1))) * xs8;
        const __m256i sc32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)u));
        const __m256i mn32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(u + 2)));
        const __m256 scf = _mm256_mul_ps(_mm256_cvtepi32_ps(sc32), _mm256_set1_ps(d));
        accm = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_mullo_epi32(mn32, _mm256_loadu_si256((const __m256i *)bs))),
                               _mm256_set1_ps(dmin), accm);
        const uint8_t *q = row + 16;
        for (int j = 0; j < 4; j++) {
            const __m256i v = _mm256_loadu_si256((const __m256i *)(q + 32 * j));
            const __m256i lo = _mm256_and_si256(v, m4), hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), m4);
            const __m512i w = _mm512_inserti64x4(_mm512_castsi256_si512(lo), hi, 1);
            const __m512i a = _mm512_loadu_si512((const void *)(xh + 64 * j));
            const __m512i p = _mm512_dpbusd_epi32(_mm512_setzero_si512(), w, a);
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm512_castsi512_si256(p)), _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * j)), acc);
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm512_extracti64x4_epi64(p, 1)), _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * j + 1)), acc);
        }
    }
    return hsum256(acc) - hsum256(accm);
}

VNNI_T static float dot_q6k_v8w(const uint8_t *row, int cols) {
    const __m256i m4 = _mm256_set1_epi8(0x0F), m2 = _mm256_set1_epi8(3);
    const int8_t *xh = xq.qh;
    const int32_t *bs = xq.bs16_h;
    __m256 acc = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 210, xh += 256, bs += 16) {
        const uint8_t *ql = row, *qh = row + 128;
        const int8_t *sc = (const int8_t *)(row + 192);
        uint16_t h; memcpy(&h, row + 208, 2);
        const float dv = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))) * xq.xs[b] * 256.0f;
        for (int c = 0; c < 2; c++, ql += 64, qh += 32, sc += 8) {
            const __m256i l0 = _mm256_loadu_si256((const __m256i *)ql);
            const __m256i l1 = _mm256_loadu_si256((const __m256i *)(ql + 32));
            const __m256i hv = _mm256_loadu_si256((const __m256i *)qh);
            __m256i q[4];
            q[0] = _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(hv, m2), 4));
            q[1] = _mm256_or_si256(_mm256_and_si256(l1, m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 2), m2), 4));
            q[2] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 4), m2), 4));
            q[3] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 6), m2), 4));
            for (int kk = 0; kk < 2; kk++) {
                const int k = kk * 2;
                const __m512i w = _mm512_inserti64x4(_mm512_castsi256_si512(q[k]), q[k + 1], 1);
                const __m512i a = _mm512_loadu_si512((const void *)(xh + c * 128 + k * 32));
                const __m512i p = _mm512_dpbusd_epi32(_mm512_setzero_si512(), w, a);
                const __m256i p0 = _mm512_castsi512_si256(p), p1 = _mm512_extracti64x4_epi64(p, 1);
                const __m256i corr0 = _mm256_set_epi32(0, 0, 0, 32 * bs[c * 8 + 2 * k + 1], 0, 0, 0, 32 * bs[c * 8 + 2 * k]);
                const __m256i corr1 = _mm256_set_epi32(0, 0, 0, 32 * bs[c * 8 + 2 * k + 3], 0, 0, 0, 32 * bs[c * 8 + 2 * k + 2]);
                const __m256 scv0 = _mm256_insertf128_ps(_mm256_castps128_ps256(_mm_set1_ps(dv * sc[2 * k])),
                                                         _mm_set1_ps(dv * sc[2 * k + 1]), 1);
                const __m256 scv1 = _mm256_insertf128_ps(_mm256_castps128_ps256(_mm_set1_ps(dv * sc[2 * k + 2])),
                                                         _mm_set1_ps(dv * sc[2 * k + 3]), 1);
                acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_sub_epi32(p0, corr0)), scv0, acc);
                acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_sub_epi32(p1, corr1)), scv1, acc);
            }
        }
    }
    return hsum256(acc);
}

/* ---------- 8 linhas por passo, ativacao de 8 bits (roadmap 3, item 2) ----------
   Como dot_q4k_v4/dot_q6k_v4 (4 linhas), mas 8 linhas e 8 acumuladores independentes; usa a ativacao de 8
   bits (qh sozinho, item 1) para que cada grupo de 32 pesos de CADA linha gaste so 1 vpdpbusd (a carga da
   ativacao e compartilhada pelas 8 linhas). KATANA_MOE_ROWS8=0 desliga. Escreve out[0..7]. */
VNNI_T static void dot_q4k_r8(const uint8_t *row0, size_t stride, int cols, float *out) {
    const __m256i m4 = _mm256_set1_epi8(0x0F), zero = _mm256_setzero_si256();
    const uint32_t k1 = 0x3f3f3f3fu, k2 = 0x0f0f0f0fu, k3 = 0x03030303u;
    const int8_t *xh = xq.qh;
    const int32_t *bs = xq.bs32_h;
    __m256 acc[8], accm[8];
    for (int r = 0; r < 8; r++) { acc[r] = _mm256_setzero_ps(); accm[r] = _mm256_setzero_ps(); }
    const uint8_t *rw[8];
    for (int r = 0; r < 8; r++) rw[r] = row0 + r * stride;
    for (int b = 0; b < cols / 256; b++, xh += 256, bs += 8) {
        __m256 scf[8];
        const float xs8 = xq.xs[b] * 256.0f;
        const __m256i bsv = _mm256_loadu_si256((const __m256i *)bs);
        for (int r = 0; r < 8; r++) {
            const uint8_t *row = rw[r] + (size_t)b * 144;
            uint16_t h0, h1; uint32_t u[4];
            memcpy(&h0, row, 2); memcpy(&h1, row + 2, 2); memcpy(u, row + 4, 12);
            u[3] = ((u[2] >> 4) & k2) | (((u[1] >> 6) & k3) << 4);
            const uint32_t aux = u[1] & k1;
            u[1] = (u[2] & k2) | (((u[0] >> 6) & k3) << 4);
            u[2] = aux;
            u[0] &= k1;
            const float d = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h0))) * xs8;
            const float dmin = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h1))) * xs8;
            const __m256i sc32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)u));
            const __m256i mn32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(u + 2)));
            scf[r] = _mm256_mul_ps(_mm256_cvtepi32_ps(sc32), _mm256_set1_ps(d));
            accm[r] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_mullo_epi32(mn32, bsv)), _mm256_set1_ps(dmin), accm[r]);
        }
        for (int j = 0; j < 4; j++) {
            const __m256i xh0 = _mm256_loadu_si256((const __m256i *)(xh + 64 * j));
            const __m256i xh1 = _mm256_loadu_si256((const __m256i *)(xh + 64 * j + 32));
            const __m256i sj0 = _mm256_set1_epi32(2 * j), sj1 = _mm256_set1_epi32(2 * j + 1);
            for (int r = 0; r < 8; r++) {
                const __m256i v = _mm256_loadu_si256((const __m256i *)(rw[r] + (size_t)b * 144 + 16 + 32 * j));
                const __m256i lo = _mm256_and_si256(v, m4), hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), m4);
                const __m256i pl = _mm256_dpbusd_epi32(zero, lo, xh0);
                acc[r] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(pl), _mm256_permutevar8x32_ps(scf[r], sj0), acc[r]);
                const __m256i ph = _mm256_dpbusd_epi32(zero, hi, xh1);
                acc[r] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(ph), _mm256_permutevar8x32_ps(scf[r], sj1), acc[r]);
            }
        }
    }
    for (int r = 0; r < 8; r++) out[r] = hsum256(acc[r]) - hsum256(accm[r]);
}

VNNI_T static void dot_q6k_r8(const uint8_t *row0, size_t stride, int cols, float *out) {
    const __m256i m4 = _mm256_set1_epi8(0x0F), m2 = _mm256_set1_epi8(3), zero = _mm256_setzero_si256();
    const int8_t *xh = xq.qh;
    const int32_t *bs = xq.bs16_h;
    __m256 acc[8];
    for (int r = 0; r < 8; r++) acc[r] = _mm256_setzero_ps();
    const uint8_t *rw[8];
    for (int r = 0; r < 8; r++) rw[r] = row0 + r * stride;
    for (int b = 0; b < cols / 256; b++, xh += 256, bs += 16) {
        float dv[8];
        for (int r = 0; r < 8; r++) {
            uint16_t h; memcpy(&h, rw[r] + (size_t)b * 210 + 208, 2);
            dv[r] = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))) * xq.xs[b] * 256.0f;
        }
        for (int c = 0; c < 2; c++) {
            __m256i xhv[4];
            for (int k = 0; k < 4; k++) xhv[k] = _mm256_loadu_si256((const __m256i *)(xh + c * 128 + k * 32));
            for (int r = 0; r < 8; r++) {
                const uint8_t *row = rw[r] + (size_t)b * 210;
                const uint8_t *ql = row + 64 * c, *qh = row + 128 + 32 * c;
                const int8_t *sc = (const int8_t *)(row + 192) + 8 * c;
                const __m256i l0 = _mm256_loadu_si256((const __m256i *)ql);
                const __m256i l1 = _mm256_loadu_si256((const __m256i *)(ql + 32));
                const __m256i hv = _mm256_loadu_si256((const __m256i *)qh);
                __m256i q[4];
                q[0] = _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(hv, m2), 4));
                q[1] = _mm256_or_si256(_mm256_and_si256(l1, m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 2), m2), 4));
                q[2] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 4), m2), 4));
                q[3] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 6), m2), 4));
                for (int k = 0; k < 4; k++) {
                    const __m256i p = _mm256_dpbusd_epi32(zero, q[k], xhv[k]);
                    const __m256i corr = _mm256_set_epi32(0, 0, 0, 32 * bs[c * 8 + 2 * k + 1], 0, 0, 0, 32 * bs[c * 8 + 2 * k]);
                    const __m256 scv = _mm256_insertf128_ps(_mm256_castps128_ps256(_mm_set1_ps(dv[r] * sc[2 * k])),
                                                            _mm_set1_ps(dv[r] * sc[2 * k + 1]), 1);
                    acc[r] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_sub_epi32(p, corr)), scv, acc[r]);
                }
            }
        }
    }
    for (int r = 0; r < 8; r++) out[r] = hsum256(acc[r]);
}
#else
#define HAVE_VNNI 0
static void vnni_init(void) {}
#endif

/* kernels inteiros dos tipos de poucos bits (IQ2/IQ3/IQ4 e Q2_0), em cpu_lowbit.c */
static int lowbit_ok(uint32_t type);
static float lowbit_dot(const Mat *W, const uint8_t *row, int cols);
static float lowbit_bf16_dot(const uint8_t *row, const float *x, int cols);

/* a matriz usa o produto inteiro? (so tipos K, e se nao estiver no modo exato) */
static int mat_int(const Mat *W) {
    return HAVE_INT_DOT && !cpu_exact && W->q &&
           (W->type == T_Q4_K || W->type == T_Q6_K || W->type == T_Q5_K || W->type == T_Q3_K || lowbit_ok(W->type));
}

static float dot_row(const Mat *W, size_t i, const float *x, const float *sx, int cols) {
    if (W->f) return dot(W->f + i * cols, x, cols);
    const uint8_t *row = W->q + i * W->stride;
#if defined(__AVX2__) && HAVE_VNNI
    if (cpu_vnni && mat_int(W)) {
        if (W->type == T_Q4_K) return dot_q4k_v(row, cols);
        if (W->type == T_Q6_K) return dot_q6k_v(row, cols);
        if (W->type == T_Q5_K) return dot_q5k_v(row, cols);
    }
#endif
    if (mat_int(W) && lowbit_ok(W->type)) return lowbit_dot(W, row, cols);
    if (W->type == T_BF16 && x) return lowbit_bf16_dot(row, x, cols);
    if (mat_int(W)) return W->type == T_Q4_K ? dot_q4k_i(row, cols) : W->type == T_Q6_K ? dot_q6k_i(row, cols)
                         : W->type == T_Q5_K ? dot_q5k_i(row, cols) : dot_q3k_i(row, cols);
    if (W->type == T_Q4_K) return dot_q4k(row, x, sx, cols);
    if (W->type == T_Q6_K) return dot_q6k(row, x, cols);
    if (W->type == T_Q8_0) return dot_q8(row, x, cols);
    if (W->type == T_MXFP4 || W->type == T_IQ4_NL || W->type == T_IQ4_XS || W->type == T_Q4_1) return dot_lut4(W->type, row, x, cols);
    {   /* demais tipos (Q4_0, Q5_x, Q2_K, Q3_K, Q5_K, IQ1..IQ3, TQ): dequantiza cada bloco e multiplica em float */
        const int unit = qunit(W->type);
        const size_t ub = qbytes(W->type, unit);
        float tmp[256], total = 0.0f;
        for (int b = 0; b < cols / unit; b++) {
            dequant(W->type, row + b * ub, tmp, unit);
            total += dot(tmp, x + b * unit, unit);
        }
        return total;
    }
}

/* somas de x por sub-bloco de 32 (usadas pelo Q4_K) */
static void block_sums(float *sx, const float *x, int cols) {
    for (int b = 0; b < cols / 32; b++) {
        float s = 0.0f;
        for (int j = 0; j < 32; j++) s += x[b * 32 + j];
        sx[b] = s;
    }
}

#define MAX_SX 2048   /* suporta ate 65536 colunas */

/* ================= GPU: OpenCL carregado em tempo de execucao =================
 * Sem SDK: as funcoes vem do OpenCL.dll / libOpenCL.so do driver e os kernels sao
 * compilados pelo proprio driver. Os pesos sobem quantizados, como estao no arquivo,
 * e sao decodificados dentro do kernel.
 *
 * Dois modos:
 *   grafo completo - tudo cabe na VRAM: o token inteiro roda na GPU (pesos, cache KV,
 *                    atencao, normas); por token ha 1 escrita (embedding) e 1 leitura (logits).
 *   hibrido        - so parte cabe: as matrizes que couberam fazem o produto na GPU.
 * KATANA_CPU=1 desliga a GPU.
 */

static double now_sec(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

typedef void *clh;

static struct {
    int32_t (*GetPlatformIDs)(uint32_t, clh *, uint32_t *);
    int32_t (*GetDeviceIDs)(clh, uint64_t, uint32_t, clh *, uint32_t *);
    int32_t (*GetDeviceInfo)(clh, uint32_t, size_t, void *, size_t *);
    clh     (*CreateContext)(const intptr_t *, uint32_t, const clh *, void *, void *, int32_t *);
    clh     (*CreateCommandQueue)(clh, clh, uint64_t, int32_t *);
    clh     (*CreateProgramWithSource)(clh, uint32_t, const char **, const size_t *, int32_t *);
    int32_t (*BuildProgram)(clh, uint32_t, const clh *, const char *, void *, void *);
    int32_t (*GetProgramBuildInfo)(clh, clh, uint32_t, size_t, void *, size_t *);
    clh     (*CreateKernel)(clh, const char *, int32_t *);
    clh     (*CreateBuffer)(clh, uint64_t, size_t, void *, int32_t *);
    int32_t (*SetKernelArg)(clh, uint32_t, size_t, const void *);
    int32_t (*EnqueueNDRangeKernel)(clh, clh, uint32_t, const size_t *, const size_t *, const size_t *, uint32_t, const clh *, clh *);
    int32_t (*EnqueueReadBuffer)(clh, clh, uint32_t, size_t, size_t, void *, uint32_t, const clh *, clh *);
    int32_t (*EnqueueWriteBuffer)(clh, clh, uint32_t, size_t, size_t, const void *, uint32_t, const clh *, clh *);
    int32_t (*Finish)(clh);
    int32_t (*ReleaseMemObject)(clh);
    int32_t (*Flush)(clh);
    int32_t (*GetEventInfo)(clh, uint32_t, size_t, void *, size_t *);
    int32_t (*ReleaseEvent)(clh);
    int32_t (*EnqueueFillBuffer)(clh, clh, const void *, size_t, size_t, size_t, uint32_t, const clh *, clh *);
} cl;

#define GPU_BMAX 16     /* maior lote de tokens por passada */

/* um conjunto de kernels compilado para um tamanho de lote fixo (NB) */
typedef struct {
    int nb, nr;     /* tokens por passada; linhas da matriz por thread */
    clh k_f32, k_q8, k_q4k, k_q5k, k_q6k, k_bsum;
    clh kp_f32, kp_q8, kp_q4k, kp_q5k, kp_q6k;      /* mesmos kernels com 2*nr linhas por thread (par gate/up) */
    clh k_moe_sum, k_rmsh, k_att1f;
    clh k_rms, k_qkv, k_gather, k_att_scores, k_att_softmax, k_att_mix;
    clh k_mv3, k_att1;      /* fundidos da geracao; so existem com nb=1 e nr=1 */
} KSet;

typedef struct {
    int on, full, force_cpu, debug, test_mv, kv16, n_shard;
    int mv_host;        /* grafo na CPU com os produtos das matrizes que subiram feitos na GPU (MLA) */
    int want_all;                                       /* devolver os logits dos 4 tokens do lote */
    clh blog4; float *logits_all;                       /* logits transpostos [vocab][4] */
    int cfg_rows, cfg_rowsb;                            /* linhas por thread: geracao e lote */
    size_t cfg_seg;                                     /* alvo de threads para segmentar colunas */
    clh dev;
    int n_gl;                                           /* camadas inteiras na GPU (as primeiras) */
    clh ctx, queue;
    KSet set[4]; int n_sets;                            /* nb decrescente; o ultimo e nb=1 */
    clh bpart, bsx;                                     /* parciais dos segmentos; somas de x */
    size_t seg_target, local;                           /* ajuste: KATANA_SEG, KATANA_LOCAL */
    clh xbuf, obuf;                                     /* modo hibrido */
    int vk, vk_nbuf;                                    /* backend Vulkan ativo; buffers de pesos criados */
    KvkBuf vxb, vob;                                    /* Vulkan: x e saida do modo hibrido */
    clh bx, bxb, bq, bk, bv, bhb, blog, batt, bfreq, bkc, bvc, bon;   /* grafo completo */
    clh *an, *fn, *lbq, *lbk, *lbv, *lbo;               /* normas e bias por camada */
    float *xT;                                          /* embeddings do lote, transpostos */
    char name[128];
    uint64_t vram, used;
    uint64_t avail0;        /* VRAM que este processo podia usar na carga (0 = a placa nao informa) */
    int n_up, n_total;
} Gpu;

static Gpu gpu;

/* fracao da VRAM que o motor pode ocupar (o resto fica para o driver e o sistema) */
/* Outro programa ocupando a placa: o orcamento desce para o que esta livre de verdade, menos uma folga de 5%.
   So atua quando mais de 20% da placa esta com outros (com a placa livre o orcamento medido nao muda). */
static uint64_t vram_cap(uint64_t b, uint64_t avail) {
    if (!avail || avail >= gpu.vram / 100 * 80) return b;
    const uint64_t margin = gpu.vram / 20, lim = avail > margin ? avail - margin : 0;
    return lim < b ? lim : b;
}
static uint64_t gpu_budget(void) {
    /* 92%: medido com um 7B numa placa de 4 GB. A 95% cabia uma camada a mais, mas a geracao
       caia de 13,4 para 9,8 tokens/s (a placa cheia demais passa a usar memoria compartilhada). */
#ifdef _WIN32
    const int def = 92;
#else
    const int def = 88;     /* Linux: o driver reserva mais; a 92% o cache de especialistas do Qwen3-30B estoura (erro -4) */
#endif
    int pct = getenv("KATANA_VRAM_PCT") ? atoi(getenv("KATANA_VRAM_PCT")) : def;
    if (pct < 10 || pct > 99) pct = def;
    return vram_cap(gpu.vram / 100 * (uint64_t)pct, gpu.avail0);
}
/* MB de VRAM a mais que o motor usaria se fosse carregado agora (outro programa liberou a placa); 0 = nada a ganhar */
static int gpu_regrow_mb(void) {
    if (!gpu.vk || !gpu.avail0) return 0;
    const uint64_t now = kvk_vram_budget();
    if (!now) return 0;
    const uint64_t full = gpu.vram / 100 * 88, cur = vram_cap(full, gpu.avail0), pot = vram_cap(full, now);
    return pot > cur ? (int)((pot - cur) >> 20) : 0;
}
static int32_t gerr;
static struct { clh k; const char *name; double t; long calls; double elems; } prof[64];
static int n_prof;
static double prof_elems;   /* elementos de peso do produto que esta sendo enfileirado */

static const char *GPU_SRC =
/* O mesmo fonte e compilado uma vez por tamanho de lote (-DNB=1, 4, 16).
   Ativacoes ficam transpostas: X[coluna*NB + token]. Assim os NB valores de uma coluna
   sao um vetor so (FB) e cada peso decodificado faz UMA multiplicacao vetorial pelo lote:
   o peso e lido e decodificado uma vez para NB tokens. */
"#if NB == 1\n"
"#define FB float\n#define LOADB(p, k) ((p)[k])\n#define STOREB(v, p, k) (p)[k] = (v)\n"
"#elif NB == 4\n"
"#define FB float4\n#define LOADB(p, k) vload4(k, p)\n#define STOREB(v, p, k) vstore4(v, k, p)\n"
"#elif NB == 8\n"
"#define FB float8\n#define LOADB(p, k) vload8(k, p)\n#define STOREB(v, p, k) vstore8(v, k, p)\n"
"#else\n"
"#define FB float16\n#define LOADB(p, k) vload16(k, p)\n#define STOREB(v, p, k) vstore16(v, k, p)\n"
"#endif\n"
/* Pesos (layout nosso, nao o do arquivo): cada linha vira `wpr` palavras de 32 bits e as
   linhas sao entrelacadas de 32 em 32: palavra w da linha i em W[(i/32)*wpr*32 + w*32 + i%32].
   As 32 threads de um warp leem 32 palavras consecutivas a cada passo (acesso coalescido).
   Cada thread calcula NR linhas de uma vez (de NR grupos de 32 vizinhos): os valores de x
   sao carregados uma vez e reusados nas NR linhas (ladrilhamento em registradores).
   Um buffer pode conter varias matrizes intercaladas por grupo (gs = quantas, go = qual). */
"#if NB * NR > 32\n#define MAXSEG 4\n#else\n#define MAXSEG 8\n#endif\n"
/* cache KV em float32 ou, para economizar VRAM, em float16 */
"#if KV16\n#define KVT half\n#define KVL(p, i) vload_half(i, p)\n#define KVS(v, i, p) vstore_half(v, i, p)\n"
"#else\n#define KVT float\n#define KVL(p, i) ((p)[i])\n#define KVS(v, i, p) (p)[i] = (v)\n#endif\n"
"#define GIDX(r) (((i >> 5) * NR + (r)) * gs + go)\n"
"#define RPTR(r, wpr) (W + (size_t)GIDX(r) * (size_t)(wpr) * 32 + (i & 31))\n"
"#define RROW(r) ((((i >> 5) * NR + (r)) << 5) + (i & 31))\n"
/* fecho: bias opcional (bit 4) e modo: 0 grava, 1 soma (residual), 2 SwiGLU com out */
"#define FIN(t, row) { FB t_ = (t); if (mode & 4) t_ += bias[row]; int mm = mode & 3;\\\n"
"  if (mm == 1) t_ += LOADB(out, row);\\\n"
"  else if (mm == 2) { FB g_ = LOADB(out, row); t_ = g_ / (1.0f + exp(-g_)) * t_; }\\\n"
"  STOREB(t_, out, row); }\n"
/* modo 8 (par): as linhas pares do buffer sao o gate e as impares o up da mesma linha;
   a thread tem os dois produtos e ja grava silu(gate)*up */
"#if NR >= 2\n"
"#define PAIRS if (mode & 8) { for (int r = 0; r < NR; r += 2) {\\\n"
"    int row = ((((i >> 5) * NR + r) >> 1) << 5) + (i & 31);\\\n"
"    if (row < rows) { FB g_ = total[r]; STOREB(g_ / (1.0f + exp(-g_)) * total[r + 1], out, row); } }\\\n"
"    return; }\n"
"#else\n"
"#define PAIRS\n"
"#endif\n"
/* com segmentos de colunas (dimensao 1 do NDRange) as threads do mesmo grupo de trabalho
   (32 linhas x nseg segmentos) somam os parciais pela memoria local: um kernel so */
"#define FINISH \\\n"
"  if (nseg > 1) { int sg = get_global_id(1), lane = i & 31;\\\n"
"    for (int r = 0; r < NR; r++) lp[(r * MAXSEG + sg) * 32 + lane] = total[r];\\\n"
"    barrier(CLK_LOCAL_MEM_FENCE);\\\n"
"    if (sg != 0) return;\\\n"
"    for (int r = 0; r < NR; r++) { FB t = (FB)(0.0f);\\\n"
"      for (int q_ = 0; q_ < nseg; q_++) t += lp[(r * MAXSEG + q_) * 32 + lane];\\\n"
"      total[r] = t; } }\\\n"
"  PAIRS\\\n"
"  for (int r = 0; r < NR; r++) { int row = RROW(r); if (row < rows) FIN(total[r], row) }\n"
"#define MVARGS __global const uint *W, __global const float *x, __global float *out,\\\n"
"  __global const float *bias, __global const float *sx,\\\n"
"  int cols, int mode, int rows, int nseg, int gs, int go,\\\n"
"  __global const uint *mtab, int mtp, int mxrow, int morow, int mcol\n"
/* varias matrizes num lancamento (especialistas do MoE): as threads sao divididas em faixas de
   mtp; a faixa k_ tem 3 entradas em mtab: (inicio da matriz em W, em blocos de 32 palavras;
   indice do vetor de entrada; indice do vetor de saida). Antes: le x a partir
   de k_*mxrow e grava a partir de k_*morow. mtp = 0 desliga. */
"#define MOEPRE if (mtp > 0) { int k_ = i / mtp; i -= k_ * mtp;\\\n"
"  W += (size_t)mtab[6 * k_ + mcol] * 32; x += (size_t)mtab[6 * k_ + mcol + 1] * mxrow; out += (size_t)mtab[6 * k_ + mcol + 2] * morow; }\n"
"#define MVLOCAL __local FB lp[NR * MAXSEG * 32];\n"
"inline float h2f(uint h) {\n"
"  uint e = (h >> 10) & 31, m = h & 1023;\n"
"  float v = e == 0 ? ldexp((float)m, -24) : ldexp((float)(m | 1024), (int)e - 25);\n"
"  return (h & 0x8000) ? -v : v;\n"
"}\n"
"inline uint sb(const uint *hw, int k) { return (hw[k >> 2] >> ((k & 3) * 8)) & 255; }\n"
"inline void k4(int j, const uint *hw, int *d, int *m) {\n"
"  if (j < 4) { *d = sb(hw, j) & 63; *m = sb(hw, j + 4) & 63; }\n"
"  else { *d = (sb(hw, j + 4) & 0xF) | ((sb(hw, j - 4) >> 6) << 4); *m = (sb(hw, j + 4) >> 4) | ((sb(hw, j) >> 6) << 4); }\n"
"}\n"
"__kernel void mv_f32(MVARGS) {\n"
"  MVLOCAL\n"
"  int i = get_global_id(0);\n"
"  MOEPRE\n"
"  __global const uint *p[NR]; FB total[NR];\n"
"  for (int r = 0; r < NR; r++) { p[r] = RPTR(r, cols); total[r] = (FB)(0.0f); }\n"
"  for (int j = 0; j < cols; j++) {\n"
"    FB xv = LOADB(x, j);\n"
"    for (int r = 0; r < NR; r++) total[r] += as_float(p[r][(size_t)j * 32]) * xv;\n"
"  }\n"
"  FINISH\n"
"}\n"
/* Q8: 9 palavras por bloco de 32 pesos: escala float + 32 int8 */
"__kernel void mv_q8(MVARGS) {\n"
"  MVLOCAL\n"
"  int i = get_global_id(0);\n"
"  MOEPRE\n"
"  int nb = cols >> 5, bps = nb / nseg, b0 = get_global_id(1) * bps;\n"
"  __global const uint *p[NR]; FB total[NR];\n"
"  for (int r = 0; r < NR; r++) { p[r] = RPTR(r, nb * 9) + (size_t)b0 * (9 * 32); total[r] = (FB)(0.0f); }\n"
"  __global const float *xb = x + (size_t)b0 * 32 * NB;\n"
"  for (int b = 0; b < bps; b++, xb += 32 * NB) {\n"
"    FB a[NR];\n"
"    for (int r = 0; r < NR; r++) a[r] = (FB)(0.0f);\n"
"    for (int l = 0; l < 8; l++) {\n"
"      FB x0 = LOADB(xb, 4 * l), x1 = LOADB(xb, 4 * l + 1), x2 = LOADB(xb, 4 * l + 2), x3 = LOADB(xb, 4 * l + 3);\n"
"      for (int r = 0; r < NR; r++) {\n"
"        int w = (int)p[r][(1 + l) * 32];\n"
"        a[r] += (float)((w << 24) >> 24) * x0 + (float)((w << 16) >> 24) * x1\n"
"              + (float)((w << 8) >> 24) * x2 + (float)(w >> 24) * x3;\n"
"      }\n"
"    }\n"
"    for (int r = 0; r < NR; r++) { total[r] += as_float(p[r][0]) * a[r]; p[r] += 9 * 32; }\n"
"  }\n"
"  FINISH\n"
"}\n"
/* Q4_K: 36 palavras por bloco de 256 pesos (mesmos bytes do arquivo). Cada peso vale
   d*sc*q - dmin*m; o termo do minimo usa a soma de x por sub-bloco, igual para todas as
   linhas: calculada uma vez por thread quando NB=1, ou vinda do kernel bsum (sx) em lote. */
"__kernel void mv_q4k(MVARGS) {\n"
"  MVLOCAL\n"
"  int i = get_global_id(0);\n"
"  MOEPRE\n"
"  int nb = cols >> 8, bps = nb / nseg, b0 = get_global_id(1) * bps;\n"
"  __global const uint *p[NR]; FB total[NR];\n"
"  for (int r = 0; r < NR; r++) { p[r] = RPTR(r, nb * 36) + (size_t)b0 * (36 * 32); total[r] = (FB)(0.0f); }\n"
"  __global const float *xb = x + (size_t)b0 * 256 * NB;\n"
"  for (int b = 0; b < bps; b++) {\n"
"    float d[NR], dmin[NR]; uint hw[NR][3];\n"
"    for (int r = 0; r < NR; r++) {\n"
"      uint h0 = p[r][0];\n"
"      hw[r][0] = p[r][32]; hw[r][1] = p[r][64]; hw[r][2] = p[r][96];\n"
"      d[r] = h2f(h0 & 0xFFFF); dmin[r] = h2f(h0 >> 16);\n"
"    }\n"
"    for (int j = 0; j < 8; j += 2, xb += 64 * NB) {\n"
"      FB a[NR], c[NR];\n"
"      for (int r = 0; r < NR; r++) { a[r] = (FB)(0.0f); c[r] = (FB)(0.0f); }\n"
"#if NB == 1\n"
"      FB xa = 0.0f, xc = 0.0f;\n"
"#else\n"
"      FB xa = LOADB(sx, (b0 + b) * 8 + j), xc = LOADB(sx, (b0 + b) * 8 + j + 1);\n"
"#endif\n"
"      for (int l = 0; l < 8; l++) {\n"
"        FB x0 = LOADB(xb, 4 * l), x1 = LOADB(xb, 4 * l + 1), x2 = LOADB(xb, 4 * l + 2), x3 = LOADB(xb, 4 * l + 3);\n"
"        FB y0 = LOADB(xb, 32 + 4 * l), y1 = LOADB(xb, 33 + 4 * l), y2 = LOADB(xb, 34 + 4 * l), y3 = LOADB(xb, 35 + 4 * l);\n"
"#if NB == 1\n"
"        xa += x0 + x1 + x2 + x3; xc += y0 + y1 + y2 + y3;\n"
"#endif\n"
"        for (int r = 0; r < NR; r++) {\n"
"          uint w = p[r][(4 + j * 4 + l) * 32];\n"
"          a[r] += (float)(w & 15) * x0 + (float)((w >> 8) & 15) * x1 + (float)((w >> 16) & 15) * x2 + (float)((w >> 24) & 15) * x3;\n"
"          c[r] += (float)((w >> 4) & 15) * y0 + (float)((w >> 12) & 15) * y1 + (float)((w >> 20) & 15) * y2 + (float)(w >> 28) * y3;\n"
"        }\n"
"      }\n"
"      for (int r = 0; r < NR; r++) {\n"
"        int s1, m1, s2, m2;\n"
"        k4(j, hw[r], &s1, &m1); k4(j + 1, hw[r], &s2, &m2);\n"
"        total[r] += d[r] * ((float)s1 * a[r] + (float)s2 * c[r]) - dmin[r] * ((float)m1 * xa + (float)m2 * xc);\n"
"      }\n"
"    }\n"
"    for (int r = 0; r < NR; r++) p[r] += 36 * 32;\n"
"  }\n"
"  FINISH\n"
"}\n"
/* Q5_K: 44 palavras por bloco (mesmos bytes do arquivo): cabecalho do Q4_K, 8 palavras com o
   quinto bit de cada peso e 32 palavras de nibbles */
"__kernel void mv_q5k(MVARGS) {\n"
"  MVLOCAL\n"
"  int i = get_global_id(0);\n"
"  MOEPRE\n"
"  int nb = cols >> 8, bps = nb / nseg, b0 = get_global_id(1) * bps;\n"
"  __global const uint *p[NR]; FB total[NR];\n"
"  for (int r = 0; r < NR; r++) { p[r] = RPTR(r, nb * 44) + (size_t)b0 * (44 * 32); total[r] = (FB)(0.0f); }\n"
"  __global const float *xb = x + (size_t)b0 * 256 * NB;\n"
"  for (int b = 0; b < bps; b++) {\n"
"    float d[NR], dmin[NR]; uint hw[NR][3];\n"
"    for (int r = 0; r < NR; r++) {\n"
"      uint h0 = p[r][0];\n"
"      hw[r][0] = p[r][32]; hw[r][1] = p[r][64]; hw[r][2] = p[r][96];\n"
"      d[r] = h2f(h0 & 0xFFFF); dmin[r] = h2f(h0 >> 16);\n"
"    }\n"
"    for (int j = 0; j < 8; j += 2, xb += 64 * NB) {\n"
"      FB a[NR], c[NR];\n"
"      for (int r = 0; r < NR; r++) { a[r] = (FB)(0.0f); c[r] = (FB)(0.0f); }\n"
"#if NB == 1\n"
"      FB xa = 0.0f, xc = 0.0f;\n"
"#else\n"
"      FB xa = LOADB(sx, (b0 + b) * 8 + j), xc = LOADB(sx, (b0 + b) * 8 + j + 1);\n"
"#endif\n"
"      for (int l = 0; l < 8; l++) {\n"
"        FB x0 = LOADB(xb, 4 * l), x1 = LOADB(xb, 4 * l + 1), x2 = LOADB(xb, 4 * l + 2), x3 = LOADB(xb, 4 * l + 3);\n"
"        FB y0 = LOADB(xb, 32 + 4 * l), y1 = LOADB(xb, 33 + 4 * l), y2 = LOADB(xb, 34 + 4 * l), y3 = LOADB(xb, 35 + 4 * l);\n"
"#if NB == 1\n"
"        xa += x0 + x1 + x2 + x3; xc += y0 + y1 + y2 + y3;\n"
"#endif\n"
"        for (int r = 0; r < NR; r++) {\n"
"          uint w = p[r][(12 + j * 4 + l) * 32], hb = p[r][(4 + l) * 32] >> j;\n"
"          a[r] += (float)((w & 15) | ((hb & 1) << 4)) * x0 + (float)(((w >> 8) & 15) | (((hb >> 8) & 1) << 4)) * x1 + (float)(((w >> 16) & 15) | (((hb >> 16) & 1) << 4)) * x2 + (float)(((w >> 24) & 15) | (((hb >> 24) & 1) << 4)) * x3;\n"
"          c[r] += (float)(((w >> 4) & 15) | (((hb >> 1) & 1) << 4)) * y0 + (float)(((w >> 12) & 15) | (((hb >> 9) & 1) << 4)) * y1 + (float)(((w >> 20) & 15) | (((hb >> 17) & 1) << 4)) * y2 + (float)((w >> 28) | (((hb >> 25) & 1) << 4)) * y3;\n"
"        }\n"
"      }\n"
"      for (int r = 0; r < NR; r++) {\n"
"        int s1, m1, s2, m2;\n"
"        k4(j, hw[r], &s1, &m1); k4(j + 1, hw[r], &s2, &m2);\n"
"        total[r] += d[r] * ((float)s1 * a[r] + (float)s2 * c[r]) - dmin[r] * ((float)m1 * xa + (float)m2 * xc);\n"
"      }\n"
"    }\n"
"    for (int r = 0; r < NR; r++) p[r] += 44 * 32;\n"
"  }\n"
"  FINISH\n"
"}\n"
/* Q6 reempacotado: 53 palavras por bloco de 256 pesos:
   48 palavras de pesos (16 pesos de 6 bits a cada 3 palavras), 4 de escalas int8, 1 float d */
"#define QT(v, k) ((float)((int)((v) & 63) - 32) * xv[k])\n"
"__kernel void mv_q6k(MVARGS) {\n"
"  MVLOCAL\n"
"  int i = get_global_id(0);\n"
"  MOEPRE\n"
"  int nb = cols >> 8, bps = nb / nseg, b0 = get_global_id(1) * bps;\n"
"  __global const uint *p[NR]; FB total[NR];\n"
"  for (int r = 0; r < NR; r++) { p[r] = RPTR(r, nb * 53) + (size_t)b0 * (53 * 32); total[r] = (FB)(0.0f); }\n"
"  __global const float *xb = x + (size_t)b0 * 256 * NB;\n"
"  for (int b = 0; b < bps; b++) {\n"
"    FB acc[NR];\n"
"    for (int r = 0; r < NR; r++) acc[r] = (FB)(0.0f);\n"
"    for (int g = 0; g < 4; g++) {\n"
"      for (int k = 0; k < 4; k++, xb += 16 * NB) {\n"
"        FB xv[16];\n"
"        for (int u = 0; u < 16; u++) xv[u] = LOADB(xb, u);\n"
"        for (int r = 0; r < NR; r++) {\n"
"          __global const uint *q = p[r] + (g * 4 + k) * 3 * 32;\n"
"          uint w0 = q[0], w1 = q[32], w2 = q[64];\n"
"          int scw = (int)p[r][(48 + g) * 32];\n"
"          FB s = QT(w0, 0) + QT(w0 >> 6, 1) + QT(w0 >> 12, 2) + QT(w0 >> 18, 3) + QT(w0 >> 24, 4)\n"
"               + QT((w0 >> 30) | (w1 << 2), 5) + QT(w1 >> 4, 6) + QT(w1 >> 10, 7) + QT(w1 >> 16, 8)\n"
"               + QT(w1 >> 22, 9) + QT((w1 >> 28) | (w2 << 4), 10) + QT(w2 >> 2, 11) + QT(w2 >> 8, 12)\n"
"               + QT(w2 >> 14, 13) + QT(w2 >> 20, 14) + QT(w2 >> 26, 15);\n"
"          acc[r] += (float)((scw << (24 - 8 * k)) >> 24) * s;\n"
"        }\n"
"      }\n"
"    }\n"
"    for (int r = 0; r < NR; r++) { total[r] += as_float(p[r][52 * 32]) * acc[r]; p[r] += 53 * 32; }\n"
"  }\n"
"  FINISH\n"
"}\n"
/* ---- kernels fundidos da geracao (um token por vez: NB=1, NR=1) ----
   O custo fixo de lancar um kernel domina a geracao (um modelo de 135M e um de 1,1B geravam
   na mesma velocidade). Aqui q, k e v saem de um lancamento so, mesmo com tipos diferentes,
   e a atencao inteira (notas, softmax e mistura) de outro. */
/* soma dos quadrados de len valores do token b a partir de start (16 por passo com um token) */
"inline float sumsq(__global const float *p, int start, int len, int b) {\n"
"  float ss = 0.0f; int j = 0;\n"
"#if NB == 1\n"
"  float16 acc = (float16)(0.0f);\n"
"  for (; j + 16 <= len; j += 16) { float16 v = vload16(0, p + start + j); acc += v * v; }\n"
"  float8 a8 = acc.lo + acc.hi; float4 a4 = a8.lo + a8.hi; float2 a2 = a4.lo + a4.hi;\n"
"  ss = a2.x + a2.y;\n"
"#endif\n"
"  for (; j < len; j++) { float v = p[(size_t)(start + j) * NB + b]; ss += v * v; }\n"
"  return ss;\n"
"}\n"
"#if NB == 1 && NR == 1\n"
/* soma de uma linha Q4_K (five=0) ou Q5_K (five=1) sobre bps blocos */
"inline float rd_q45(__global const uint *p, __global const float *xb, int bps, int five) {\n"
"  const int wpb = five ? 44 : 36, q0 = five ? 12 : 4;\n"
"  float total = 0.0f;\n"
"  for (int b = 0; b < bps; b++, p += wpb * 32) {\n"
"    uint h0 = p[0], hw[3];\n"
"    hw[0] = p[32]; hw[1] = p[64]; hw[2] = p[96];\n"
"    float d = h2f(h0 & 0xFFFF), dmin = h2f(h0 >> 16);\n"
"    for (int j = 0; j < 8; j += 2, xb += 64) {\n"
"      int s1, m1, s2, m2;\n"
"      k4(j, hw, &s1, &m1); k4(j + 1, hw, &s2, &m2);\n"
"      float a = 0.0f, c = 0.0f, xa = 0.0f, xc = 0.0f;\n"
"      for (int l = 0; l < 8; l++) {\n"
"        uint w = p[(q0 + j * 4 + l) * 32], hb = five ? p[(4 + l) * 32] >> j : 0;\n"
"        float x0 = xb[4 * l], x1 = xb[4 * l + 1], x2 = xb[4 * l + 2], x3 = xb[4 * l + 3];\n"
"        float y0 = xb[32 + 4 * l], y1 = xb[33 + 4 * l], y2 = xb[34 + 4 * l], y3 = xb[35 + 4 * l];\n"
"        a += (float)((w & 15) | ((hb & 1) << 4)) * x0 + (float)(((w >> 8) & 15) | (((hb >> 8) & 1) << 4)) * x1\n"
"           + (float)(((w >> 16) & 15) | (((hb >> 16) & 1) << 4)) * x2 + (float)(((w >> 24) & 15) | (((hb >> 24) & 1) << 4)) * x3;\n"
"        c += (float)(((w >> 4) & 15) | (((hb >> 1) & 1) << 4)) * y0 + (float)(((w >> 12) & 15) | (((hb >> 9) & 1) << 4)) * y1\n"
"           + (float)(((w >> 20) & 15) | (((hb >> 17) & 1) << 4)) * y2 + (float)((w >> 28) | (((hb >> 25) & 1) << 4)) * y3;\n"
"        xa += x0 + x1 + x2 + x3; xc += y0 + y1 + y2 + y3;\n"
"      }\n"
"      total += d * ((float)s1 * a + (float)s2 * c) - dmin * ((float)m1 * xa + (float)m2 * xc);\n"
"    }\n"
"  }\n"
"  return total;\n"
"}\n"
"inline float rd_q6(__global const uint *p, __global const float *xb, int bps) {\n"
"  float total = 0.0f;\n"
"  for (int b = 0; b < bps; b++, p += 53 * 32) {\n"
"    float acc = 0.0f;\n"
"    for (int g = 0; g < 4; g++) {\n"
"      int scw = (int)p[(48 + g) * 32];\n"
"      for (int k = 0; k < 4; k++, xb += 16) {\n"
"        float xv[16];\n"
"        for (int u = 0; u < 16; u++) xv[u] = xb[u];\n"
"        __global const uint *q = p + (g * 4 + k) * 3 * 32;\n"
"        uint w0 = q[0], w1 = q[32], w2 = q[64];\n"
"        float s = QT(w0, 0) + QT(w0 >> 6, 1) + QT(w0 >> 12, 2) + QT(w0 >> 18, 3) + QT(w0 >> 24, 4)\n"
"                + QT((w0 >> 30) | (w1 << 2), 5) + QT(w1 >> 4, 6) + QT(w1 >> 10, 7) + QT(w1 >> 16, 8)\n"
"                + QT(w1 >> 22, 9) + QT((w1 >> 28) | (w2 << 4), 10) + QT(w2 >> 2, 11) + QT(w2 >> 8, 12)\n"
"                + QT(w2 >> 14, 13) + QT(w2 >> 20, 14) + QT(w2 >> 26, 15);\n"
"        acc += (float)((scw << (24 - 8 * k)) >> 24) * s;\n"
"      }\n"
"    }\n"
"    total += as_float(p[52 * 32]) * acc;\n"
"  }\n"
"  return total;\n"
"}\n"
/* tres matrizes de mesmas colunas (q, k, v) num lancamento: a thread descobre pela faixa de
   linhas qual matriz e, calcula o produto no tipo dela e grava no buffer de saida dela.
   kinds: 12 = Q4_K, 13 = Q5_K, 14 = 6 bits. rp0/rp1: linhas (arredondadas) de q e de k. */
"__kernel void mv3(__global const uint *W0, __global const uint *W1, __global const uint *W2,\n"
"    __global const float *x, __global float *o0, __global float *o1, __global float *o2,\n"
"    int k0, int k1, int k2, int rp0, int rp1, int r0, int r1, int r2, int cols, int nseg) {\n"
"  __local float lp[MAXSEG * 32];\n"
"  int i = get_global_id(0), sg = get_global_id(1), lane = i & 31;\n"
"  int which = i < rp0 ? 0 : i < rp0 + rp1 ? 1 : 2;\n"
"  int row = which == 0 ? i : which == 1 ? i - rp0 : i - rp0 - rp1;\n"
"  int kind = which == 0 ? k0 : which == 1 ? k1 : k2;\n"
"  int rows = which == 0 ? r0 : which == 1 ? r1 : r2;\n"
"  __global const uint *W = which == 0 ? W0 : which == 1 ? W1 : W2;\n"
"  __global float *out = which == 0 ? o0 : which == 1 ? o1 : o2;\n"
"  int nb = cols >> 8, bps = nb / nseg, b0 = sg * bps;\n"
"  int wpb = kind == 12 ? 36 : kind == 13 ? 44 : 53;\n"
"  __global const uint *p = W + (size_t)(row >> 5) * (size_t)(nb * wpb) * 32 + lane + (size_t)b0 * (wpb * 32);\n"
"  __global const float *xb = x + (size_t)b0 * 256;\n"
"  float total = kind == 14 ? rd_q6(p, xb, bps) : rd_q45(p, xb, bps, kind == 13);\n"
"  if (nseg > 1) {\n"
"    lp[sg * 32 + lane] = total;\n"
"    barrier(CLK_LOCAL_MEM_FENCE);\n"
"    if (sg != 0) return;\n"
"    total = 0.0f;\n"
"    for (int q_ = 0; q_ < nseg; q_++) total += lp[q_ * 32 + lane];\n"
"  }\n"
"  if (row < rows) out[row] = total;\n"
"}\n"
/* atencao de um token num lancamento: um grupo de trabalho por cabeca, hd threads cada.
   As threads repartem as posicoes para as notas, juntam maximo e soma pela memoria local
   (softmax) e depois cada uma mistura um elemento da saida. */
"__kernel void att1(__global const float *q, __global const KVT *kc, __global const KVT *vc,\n"
"    __global float *att, __global float *out, int base, int pos, int ctx, int hd, int kvd, int group, float scale) {\n"
"  __local float lm[256];\n"
"  int h = get_group_id(0), l = get_local_id(0), T = pos + 1;\n"
"  __global float *a = att + (size_t)h * ctx;\n"
"  __global const float *qh = q + h * hd;\n"
"  float mx = -3.0e38f;\n"
"  for (int t = l; t < T; t += hd) {\n"
"    __global const KVT *kh = kc + base + t * kvd + (h / group) * hd;\n"
"    float s = 0.0f;\n"
"    for (int j = 0; j < hd; j++) s += qh[j] * KVL(kh, j);\n"
"    s *= scale; a[t] = s; mx = max(mx, s);\n"
"  }\n"
"  lm[l] = mx;\n"
"  barrier(CLK_LOCAL_MEM_FENCE);\n"
"  float gm = lm[0];\n"
"  for (int k = 1; k < hd; k++) gm = max(gm, lm[k]);\n"
"  barrier(CLK_LOCAL_MEM_FENCE);\n"
"  float sum = 0.0f;\n"
"  for (int t = l; t < T; t += hd) { float e = exp(a[t] - gm); a[t] = e; sum += e; }\n"
"  lm[l] = sum;\n"
"  barrier(CLK_LOCAL_MEM_FENCE | CLK_GLOBAL_MEM_FENCE);\n"
"  float tot = 0.0f;\n"
"  for (int k = 0; k < hd; k++) tot += lm[k];\n"
"  __global const KVT *v = vc + base + (h / group) * hd + l;\n"
"  float s = 0.0f;\n"
"  for (int t = 0; t < T; t++) s += a[t] * KVL(v, t * kvd);\n"
"  out[h * hd + l] = s / tot;\n"
"}\n"
/* A atencao inteira de um token, da saida crua das projecoes ate a mistura: bias e norma
   opcionais de q e k, RoPE, gravacao de k e v no cache, notas, softmax e mistura. Um grupo de hd
   threads por cabeca. As threads 0..hd/2-1 giram um par de q; as hd/2..hd-1 giram um par de k
   da cabeca kv correspondente. A posicao atual usa k e v locais (o cache so e lido para t < pos),
   e so a primeira cabeca de cada grupo kv grava no cache. */
"__kernel void att1f(__global const float *qs, __global const float *ks_, __global const float *vs,\n"
"    __global const float *bq, __global const float *bk, __global const float *bv,\n"
"    __global KVT *kc, __global KVT *vc, __global float *att, __global float *out,\n"
"    __global const float *freq, __global const float *qn, __global const float *kn,\n"
"    int base, int pos, int ctx, int hd, int kvd, int group, int neox, int has_bias, int norm, int d,\n"
"    float scale, float eps) {\n"
"  __local float lm[256]; __local float lq[256]; __local float lk[256];\n"
"  int h = get_group_id(0), l = get_local_id(0), T = pos + 1, g = h / group, half_ = hd / 2;\n"
"  int off = base + pos * kvd + g * hd;\n"
"  __global float *a = att + (size_t)h * ctx;\n"
"  {\n"
"    int isk = l >= half_, i = isk ? l - half_ : l;\n"
"    int i0 = neox ? i : 2 * i, i1 = i0 + (neox ? half_ : 1);\n"
"    float ang = pos * freq[i], cs = cos(ang), sn = sin(ang);\n"
"    if (isk) {\n"
"      float x0 = ks_[g * hd + i0], x1 = ks_[g * hd + i1];\n"
"      if (has_bias & 2) { x0 += bk[g * hd + i0]; x1 += bk[g * hd + i1]; }\n"
"      if (norm) { int r0 = norm == 1 ? 0 : g * hd, len = norm == 1 ? kvd : hd;\n"
"        float inv = 1.0f / sqrt(sumsq(ks_, r0, len, 0) / (float)len + eps);\n"
"        x0 *= kn[g * hd + i0 - r0] * inv; x1 *= kn[g * hd + i1 - r0] * inv; }\n"
"      float y0 = x0 * cs - x1 * sn, y1 = x0 * sn + x1 * cs;\n"
"      lk[i0] = y0; lk[i1] = y1;\n"
"      if (h % group == 0) { KVS(y0, off + i0, kc); KVS(y1, off + i1, kc); }\n"
"    } else {\n"
"      float x0 = qs[h * hd + i0], x1 = qs[h * hd + i1];\n"
"      if (has_bias & 1) { x0 += bq[h * hd + i0]; x1 += bq[h * hd + i1]; }\n"
"      if (norm) { int r0 = norm == 1 ? 0 : h * hd, len = norm == 1 ? d : hd;\n"
"        float inv = 1.0f / sqrt(sumsq(qs, r0, len, 0) / (float)len + eps);\n"
"        x0 *= qn[h * hd + i0 - r0] * inv; x1 *= qn[h * hd + i1 - r0] * inv; }\n"
"      lq[i0] = x0 * cs - x1 * sn; lq[i1] = x0 * sn + x1 * cs;\n"
"    }\n"
"  }\n"
"  float vl = vs[g * hd + l] + ((has_bias & 4) ? bv[g * hd + l] : 0.0f);\n"
"  if (h % group == 0) KVS(vl, off + l, vc);\n"
"  barrier(CLK_LOCAL_MEM_FENCE);\n"
"  float mx = -3.0e38f;\n"
"  for (int t = l; t < T; t += hd) {\n"
"    float s = 0.0f;\n"
"    if (t == pos) { for (int j = 0; j < hd; j++) s += lq[j] * lk[j]; }\n"
"    else { __global const KVT *kh = kc + base + t * kvd + g * hd;\n"
"      for (int j = 0; j < hd; j++) s += lq[j] * KVL(kh, j); }\n"
"    s *= scale; a[t] = s; mx = max(mx, s);\n"
"  }\n"
"  lm[l] = mx;\n"
"  barrier(CLK_LOCAL_MEM_FENCE);\n"
"  float gm = lm[0];\n"
"  for (int k = 1; k < hd; k++) gm = max(gm, lm[k]);\n"
"  barrier(CLK_LOCAL_MEM_FENCE);\n"
"  float sum = 0.0f;\n"
"  for (int t = l; t < T; t += hd) { float e = exp(a[t] - gm); a[t] = e; sum += e; }\n"
"  lm[l] = sum;\n"
"  barrier(CLK_LOCAL_MEM_FENCE | CLK_GLOBAL_MEM_FENCE);\n"
"  float tot = 0.0f;\n"
"  for (int k = 0; k < hd; k++) tot += lm[k];\n"
"  __global const KVT *v = vc + base + g * hd + l;\n"
"  float s = a[pos] * vl;\n"
"  for (int t = 0; t < pos; t++) s += a[t] * KVL(v, t * kvd);\n"
"  out[h * hd + l] = s / tot;\n"
"}\n"
"#endif\n"
/* somas de x por sub-bloco de 32 colunas (termo do minimo do Q4_K em lote) */
"__kernel void bsum(__global const float *x, __global float *sx) {\n"
"  int i = get_global_id(0);\n"
"  FB s = (FB)(0.0f);\n"
"  for (int j = 0; j < 32; j++) s += LOADB(x, i * 32 + j);\n"
"  STOREB(s, sx, i);\n"
"}\n"
/* RMSNorm num kernel so: cada thread refaz a soma dos quadrados (n e pequeno e x esta no cache) */
"__kernel void rms(__global const float *x, __global const float *g, __global float *out, int n, float eps) {\n"
"  int i = get_global_id(0);\n"
"#if NB == 1\n"
"  FB ss = sumsq(x, 0, n, 0);\n"
"#else\n"
"  FB ss = (FB)(0.0f);\n"
"  for (int j = 0; j < n; j++) { FB v = LOADB(x, j); ss += v * v; }\n"
"#endif\n"
"  STOREB(g[i] * (LOADB(x, i) / sqrt(ss / (float)n + eps)), out, i);\n"
"}\n"
/* RMSNorm por cabeca (q e k do qwen3): cada bloco de hd valores e normalizado sozinho */
"__kernel void rmsh(__global const float *x, __global const float *g, __global float *out, int hd, float eps) {\n"
"  int i = get_global_id(0), h0 = (i / hd) * hd;\n"
"  FB ss = (FB)(0.0f);\n"
"  for (int j = 0; j < hd; j++) { FB v = LOADB(x, h0 + j); ss += v * v; }\n"
"  STOREB(g[i - h0] * (LOADB(x, i) / sqrt(ss / (float)hd + eps)), out, i);\n"
"}\n"
/* MoE: x += soma dos n especialistas, cada um com o seu peso (t tem um vetor por especialista) */
"__kernel void moe_sum(__global float *x, __global const float *t, __global const uint *meta, int woff, int K, int d, int B) {\n"
"  int g = get_global_id(0), i = g / B, b = g % B;\n"
"  float s_ = 0.0f;\n"
"  for (int k = 0; k < K; k++) s_ += as_float(meta[woff + b * K + k]) * t[(size_t)meta[woff + (B + b) * K + k] * d + i];\n"
"  x[(size_t)i * B + b] += s_;\n"
"}\n"
/* copia a coluna `off` de um buffer transposto para um vetor simples */
"__kernel void gather(__global const float *src, __global float *dst, int stride, int off) {\n"
"  int i = get_global_id(0);\n"
"  dst[i] = src[(size_t)i * stride + off];\n"
"}\n"
/* pos-projecao de q, k e v num kernel so: bias opcional, RoPE em q (no lugar) e em k
   (direto no cache KV) e copia de v para o cache; o token b do lote esta na posicao pos0+b */
"__kernel void qkv_post(__global float *q, __global const float *k, __global const float *v,\n"
"    __global const float *bq, __global const float *bk, __global const float *bv,\n"
"    __global KVT *kc, __global KVT *vc, __global const float *freq,\n"
"    int base, int pos0, int hd, int neox, int has_bias, int d, int kvd,\n"
"    __global const float *qs, __global const float *qn, __global const float *kn, int norm, float eps) {\n"
"  int gid = get_global_id(0), b = gid % NB, p = gid / NB, half_ = hd / 2;\n"
"  int pos = pos0 + b, off = base + pos * kvd;\n"
"  if (p >= d / 2 + kvd / 2) {\n"
"    p -= d / 2 + kvd / 2;\n"
"    KVS(v[p * NB + b] + ((has_bias & 4) ? bv[p] : 0.0f), off + p, vc);\n"
"    return;\n"
"  }\n"
"  int isk = p >= d / 2;\n"
"  if (isk) p -= d / 2;\n"
"  int h = p / half_, i = p % half_;\n"
"  int i0 = h * hd + (neox ? i : 2 * i), i1 = i0 + (neox ? half_ : 1);\n"
"  float ang = pos * freq[i], cs = cos(ang), sn = sin(ang);\n"
"  if (isk) {\n"
"    float a = k[i0 * NB + b], c = k[i1 * NB + b];\n"
"    if (has_bias & 2) { a += bk[i0]; c += bk[i1]; }\n"
"    if (norm) { int r0 = norm == 1 ? 0 : h * hd, len = norm == 1 ? kvd : hd;\n"
"      float inv = 1.0f / sqrt(sumsq(k, r0, len, b) / (float)len + eps);\n"
"      a *= kn[i0 - r0] * inv; c *= kn[i1 - r0] * inv; }\n"
"    KVS(a * cs - c * sn, off + i0, kc); KVS(a * sn + c * cs, off + i1, kc);\n"
"  } else {\n"
"    float a = qs[i0 * NB + b], c = qs[i1 * NB + b];\n"
"    if (has_bias & 1) { a += bq[i0]; c += bq[i1]; }\n"
"    if (norm) { int r0 = norm == 1 ? 0 : h * hd, len = norm == 1 ? d : hd;\n"
"      float inv = 1.0f / sqrt(sumsq(qs, r0, len, b) / (float)len + eps);\n"
"      a *= qn[i0 - r0] * inv; c *= qn[i1 - r0] * inv; }\n"
"    q[i0 * NB + b] = a * cs - c * sn; q[i1 * NB + b] = a * sn + c * cs;\n"
"  }\n"
"}\n"
/* atencao: notas (token x cabeca x posicao; so t <= posicao do token), softmax, mistura */
"__kernel void att_scores(__global const float *q, __global const KVT *kc, __global float *att,\n"
"    int base, int pos0, int tmax, int ctx, int hd, int kvd, int group, int nh, float scale) {\n"
"  int gid = get_global_id(0), t = gid % tmax, bh = gid / tmax, h = bh % nh, b = bh / nh;\n"
"  if (t > pos0 + b) return;\n"
"  __global const KVT *kh = kc + base + t * kvd + (h / group) * hd;\n"
"  float s = 0.0f;\n"
"  for (int j = 0; j < hd; j++) s += q[(h * hd + j) * NB + b] * KVL(kh, j);\n"
"  att[(size_t)bh * ctx + t] = s * scale;\n"
"}\n"
"__kernel void att_softmax(__global float *att, int pos0, int ctx, int nh) {\n"
"  int gid = get_global_id(0), T = pos0 + gid / nh + 1;\n"
"  __global float *a = att + (size_t)gid * ctx;\n"
"  float mx = a[0], sum = 0.0f;\n"
"  for (int t = 1; t < T; t++) mx = max(mx, a[t]);\n"
"  for (int t = 0; t < T; t++) { float e = exp(a[t] - mx); a[t] = e; sum += e; }\n"
"  float inv = 1.0f / sum;\n"
"  for (int t = 0; t < T; t++) a[t] *= inv;\n"
"}\n"
"__kernel void att_mix(__global const float *att, __global const KVT *vc, __global float *out,\n"
"    int base, int pos0, int ctx, int hd, int kvd, int group, int nh) {\n"
"  int gid = get_global_id(0), b = gid % NB, i = gid / NB, h = i / hd, j = i % hd;\n"
"  __global const float *a = att + (size_t)(b * nh + h) * ctx;\n"
"  __global const KVT *v = vc + base + (h / group) * hd + j;\n"
"  float s = 0.0f;\n"
"  for (int t = 0; t <= pos0 + b; t++) s += a[t] * KVL(v, t * kvd);\n"
"  out[i * NB + b] = s;\n"
"}\n";

static int cl_load(void) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA("OpenCL.dll");
    if (!h) return 0;
#define SYM(f) if ((*(void **)&cl.f = (void *)GetProcAddress(h, "cl" #f)) == NULL) return 0
#else
    void *h = dlopen("libOpenCL.so.1", RTLD_NOW);
    if (!h) h = dlopen("libOpenCL.so", RTLD_NOW);
    if (!h) return 0;
#define SYM(f) if ((*(void **)&cl.f = dlsym(h, "cl" #f)) == NULL) return 0
#endif
    SYM(GetPlatformIDs); SYM(GetDeviceIDs); SYM(GetDeviceInfo); SYM(CreateContext);
    SYM(CreateCommandQueue); SYM(CreateProgramWithSource); SYM(BuildProgram);
    SYM(GetProgramBuildInfo); SYM(CreateKernel); SYM(CreateBuffer); SYM(SetKernelArg);
    SYM(EnqueueNDRangeKernel); SYM(EnqueueReadBuffer); SYM(EnqueueWriteBuffer); SYM(Finish); SYM(ReleaseMemObject); SYM(Flush); SYM(EnqueueFillBuffer); SYM(GetEventInfo); SYM(ReleaseEvent);
#undef SYM
    return 1;
}

/* compila o fonte para um tamanho de lote e um numero de linhas por thread */
static clh gpu_compile(int nb, int nr, clh dev) {
    int32_t err = 0;
    char opts[160];
    snprintf(opts, sizeof opts, "-cl-mad-enable %s-DNB=%d -DNR=%d -DKV16=%d",
             getenv("KATANA_FASTMATH") && atoi(getenv("KATANA_FASTMATH")) ? "-cl-fast-relaxed-math " : "", nb, nr, gpu.kv16);
    clh prog = cl.CreateProgramWithSource(gpu.ctx, 1, &GPU_SRC, NULL, &err);
    if (err) return NULL;
    if (cl.BuildProgram(prog, 1, &dev, opts, NULL, NULL) != 0) {
        static char log[8192];
        cl.GetProgramBuildInfo(prog, dev, 0x1183, sizeof log - 1, log, NULL);
        fprintf(stderr, "GPU: falha ao compilar os kernels (lote %d, linhas %d):\n%s\n", nb, nr, log);
        return NULL;
    }
    return prog;
}

static int gpu_build_set(KSet *ks, int nb, int nr, clh dev) {
    int32_t err = 0;
    clh prog = gpu_compile(nb, nr, dev), prog2 = gpu_compile(nb, nr * 2, dev);
    if (!prog || !prog2) return 0;
    ks->nb = nb; ks->nr = nr;
#define KERN(pr, field, kname) ks->field = cl.CreateKernel(pr, kname, &err); if (err) return 0; \
    if (n_prof < 64) { prof[n_prof].k = ks->field; prof[n_prof++].name = kname; }
    KERN(prog, k_f32, "mv_f32"); KERN(prog, k_q8, "mv_q8"); KERN(prog, k_q4k, "mv_q4k"); KERN(prog, k_q5k, "mv_q5k"); KERN(prog, k_q6k, "mv_q6k");
    KERN(prog2, kp_f32, "mv_f32"); KERN(prog2, kp_q8, "mv_q8"); KERN(prog2, kp_q4k, "mv_q4k"); KERN(prog2, kp_q5k, "mv_q5k"); KERN(prog2, kp_q6k, "mv_q6k");
    KERN(prog, k_bsum, "bsum"); KERN(prog, k_rms, "rms"); KERN(prog, k_qkv, "qkv_post");
    KERN(prog, k_gather, "gather"); KERN(prog, k_att_scores, "att_scores"); KERN(prog, k_moe_sum, "moe_sum"); KERN(prog, k_rmsh, "rmsh");
    KERN(prog, k_att_softmax, "att_softmax"); KERN(prog, k_att_mix, "att_mix");
    {   /* modo de varias matrizes desligado por padrao em todos os kernels de produto */
        clh mk[10] = { ks->k_f32, ks->k_q8, ks->k_q4k, ks->k_q5k, ks->k_q6k, ks->kp_f32, ks->kp_q8, ks->kp_q4k, ks->kp_q5k, ks->kp_q6k };
        static clh dummy;
        int32_t z = 0, ed = 0;
        if (!dummy) dummy = cl.CreateBuffer(gpu.ctx, 4, 64, NULL, &ed);
        for (int i = 0; i < 10; i++) {
            cl.SetKernelArg(mk[i], 11, sizeof(clh), &dummy); cl.SetKernelArg(mk[i], 12, sizeof z, &z);
            cl.SetKernelArg(mk[i], 13, sizeof z, &z);    cl.SetKernelArg(mk[i], 14, sizeof z, &z);
            cl.SetKernelArg(mk[i], 15, sizeof z, &z);
        }
    }
    ks->k_mv3 = ks->k_att1 = ks->k_att1f = NULL;
    if (nb == 1 && nr == 1 && !getenv("KATANA_NOFUSE")) { KERN(prog, k_mv3, "mv3"); KERN(prog, k_att1, "att1"); KERN(prog, k_att1f, "att1f"); }
#undef KERN
    return 1;
}

/* escolhe a GPU (prefere placa dedicada), cria contexto, fila e kernels */
static int gpu_init(void) {
    if (!cl_load()) return 0;
    clh plats[8], devs[8], best = NULL;
    uint32_t np = 0;
    int best_score = -1;
    if (cl.GetPlatformIDs(8, plats, &np) != 0) return 0;
    for (uint32_t i = 0; i < np && i < 8; i++) {
        uint32_t nd = 0;
        if (cl.GetDeviceIDs(plats[i], 4 /* GPU */, 8, devs, &nd) != 0) continue;
        for (uint32_t j = 0; j < nd && j < 8; j++) {
            char vendor[128] = {0}, name[128] = {0};
            cl.GetDeviceInfo(devs[j], 0x102C, sizeof vendor - 1, vendor, NULL);
            cl.GetDeviceInfo(devs[j], 0x102B, sizeof name - 1, name, NULL);
            int score = (strstr(vendor, "NVIDIA") || strstr(vendor, "Advanced Micro") || strstr(vendor, "AMD")) ? 2 : 1;
            if (score > best_score) {
                best_score = score; best = devs[j];
                memcpy(gpu.name, name, sizeof gpu.name);
                cl.GetDeviceInfo(devs[j], 0x101F, sizeof gpu.vram, &gpu.vram, NULL);
            }
        }
    }
    if (!best) return 0;

    int32_t err = 0;
    gpu.ctx = cl.CreateContext(NULL, 1, &best, NULL, NULL, &err);
    if (err) return 0;
    gpu.queue = cl.CreateCommandQueue(gpu.ctx, best, 0, &err);
    if (err) return 0;
    gpu.debug = getenv("KATANA_DEBUG") != NULL;

    gpu.dev = best;
    return 1;
}

/* compila os conjuntos de kernels (depois de decidir o formato do cache KV).
   Lotes: KATANA_BATCH limita o maior (1 = sem lote). */
static int gpu_build_all(void) {
    static const int sizes[] = { 16, 4, 1 };
    int maxb = getenv("KATANA_BATCH") ? atoi(getenv("KATANA_BATCH")) : GPU_BMAX;
    gpu.n_sets = 0;
    for (int i = 0; i < 3; i++) {
        if (sizes[i] > maxb && sizes[i] != 1) continue;
        const char *ev = getenv(sizes[i] == 1 ? "KATANA_ROWS" : "KATANA_ROWSB");
        int nr = ev ? atoi(ev) : (sizes[i] == 1 ? gpu.cfg_rows : gpu.cfg_rowsb);
        if (nr != 1 && nr != 2) nr = 1;      /* o par gate/up usa 2*nr <= 4 */
        if (!gpu_build_set(&gpu.set[gpu.n_sets], sizes[i], nr, gpu.dev)) return 0;
        gpu.n_sets++;
    }
    return 1;
}

/* Especie do layout na VRAM. Q4_0, Q5_0, MXFP4, IQ4_NL e IQ4_XS viram o layout de 8 bits
   (escala float por bloco de 32 + 32 int8) e Q3_K o de 6 bits: as conversoes sao exatas (os
   mesmos valores inteiros e escalas; no IQ4_XS a escala do sub-bloco e d*(ls-32), exata em
   float; no MXFP4 os valores E2M1 dobrados sao inteiros e a escala 2^(e-128) e float), so
   ocupam mais VRAM. Q2_K sobe cru e tem kernel proprio (so no Vulkan). */
static uint32_t gpu_kind(const Mat *W) {
    if (!W->q) return T_F32;
    switch (W->type) {
        case T_Q4_0: case T_Q5_0: case T_MXFP4: case T_IQ4_NL: case T_IQ4_XS: return T_Q8_0;
        case T_Q4_1: return T_Q8_0;    /* requantizado (aproximado, ver gpu_pack_row) */
        case T_Q5_1: return gpu.vk ? T_Q5_1 : T_Q8_0;  /* Vulkan: layout exato (o bloco do arquivo reordenado, 6 palavras) */
        case T_Q3_K: return T_Q6_K;
        default: return W->type;
    }
}

/* a matriz tem layout de GPU? (as demais ficam na CPU: a camada que as usa roda na CPU) */
static int gpu_has(const Mat *W) {
    const uint32_t k = gpu_kind(W);
    return k == T_F32 || k == T_Q8_0 || k == T_Q4_K || k == T_Q5_K || k == T_Q6_K || ((k == T_Q2_K || k == T_Q5_1) && gpu.vk);
}

static size_t gpu_wpr(const Mat *W, size_t cols) {
    switch (gpu_kind(W)) {
        case T_F32:  return cols;
        case T_Q4_K: return cols / 256 * 36;
        case T_Q5_K: return cols / 256 * 44;
        case T_Q6_K: return cols / 256 * 53;
        case T_Q2_K: return cols / 256 * 21;
        case T_Q5_1: return cols / 32 * 6;
        default:     return cols / 32 * 9;
    }
}

/* bloco de 6 bits da GPU: 256 valores (0..63, com +32) em 48 palavras, 16 escalas int8, float d */
static void pack_q6p(const uint8_t *qv, const int8_t *sc, float d, uint32_t *o) {
    for (int g = 0; g < 16; g++) {          /* 16 pesos de 6 bits em 96 bits */
        uint64_t lo = 0, hi = 0;
        for (int k = 0; k < 16; k++) {
            const int bit = 6 * k;
            const uint64_t v = qv[g * 16 + k];
            if (bit < 64) { lo |= v << bit; if (bit > 58) hi |= v >> (64 - bit); }
            else hi |= v << (bit - 64);
        }
        o[g * 3] = (uint32_t)lo; o[g * 3 + 1] = (uint32_t)(lo >> 32); o[g * 3 + 2] = (uint32_t)hi;
    }
    memcpy(o + 48, sc, 16);
    memcpy(o + 52, &d, 4);
}

/* converte a linha r para as palavras do layout da GPU */
static void gpu_pack_row(const Mat *W, size_t r, size_t cols, uint32_t *o) {
    if (!W->q) { memcpy(o, W->f + r * cols, cols * 4); return; }
    const uint8_t *src = W->q + r * W->stride;
    uint16_t h;
    switch (W->type) {
    case T_Q4_K: memcpy(o, src, cols / 256 * 144); return;
    case T_Q5_K: memcpy(o, src, cols / 256 * 176); return;
    case T_Q2_K: memcpy(o, src, cols / 256 * 84); return;
    case T_Q5_0: case T_MXFP4: case T_IQ4_NL: case T_IQ4_XS: {   /* -> layout de 8 bits, exato */
        const int unit = qunit(W->type);
        const size_t ub = qbytes(W->type, unit);
        for (size_t b = 0; b < cols / unit; b++, src += ub) {
            for (int s = 0; s < unit / 32; s++, o += 9) {
                int8_t q[32];
                float d;
                if (W->type == T_Q5_0) {
                    d = h2f(src);
                    uint32_t qh; memcpy(&qh, src + 2, 4);
                    for (int j = 0; j < 16; j++) {
                        q[j] = (int8_t)((int)((src[6 + j] & 0xF) | (((qh >> j) & 1) << 4)) - 16);
                        q[j + 16] = (int8_t)((int)((src[6 + j] >> 4) | (((qh >> (j + 16)) & 1) << 4)) - 16);
                    }
                } else if (W->type == T_MXFP4) {
                    d = e8m0_half(src[0]);
                    for (int j = 0; j < 16; j++) { q[j] = q_fp4_vals[src[1 + j] & 0xF]; q[j + 16] = q_fp4_vals[src[1 + j] >> 4]; }
                } else if (W->type == T_IQ4_NL) {
                    d = h2f(src);
                    for (int j = 0; j < 16; j++) { q[j] = q_iq4nl_vals[src[2 + j] & 0xF]; q[j + 16] = q_iq4nl_vals[src[2 + j] >> 4]; }
                } else {
                    const uint8_t *qs = src + 8 + 16 * s;
                    d = h2f(src) * iq4xs_scale(src, s);
                    for (int j = 0; j < 16; j++) { q[j] = q_iq4nl_vals[qs[j] & 0xF]; q[j + 16] = q_iq4nl_vals[qs[j] >> 4]; }
                }
                memcpy(o, &d, 4); memcpy(o + 1, q, 32);
            }
        }
        return;
    }
    case T_Q5_1: if (gpu.vk) {     /* layout exato da GPU (6 palavras): d e m fp16, quinto bit de cada valor,
                                      4 bits baixos com o valor j na palavra 2 + j/8, bits 4*(j%8) */
        for (size_t b = 0; b < cols / 32; b++, src += 24, o += 6) {
            memcpy(o, src, 4); memcpy(o + 1, src + 4, 4);
            uint32_t lo[4] = { 0, 0, 0, 0 };
            for (int j = 0; j < 16; j++) {
                lo[j / 8] |= (uint32_t)(src[8 + j] & 0xF) << (4 * (j % 8));
                lo[(j + 16) / 8] |= (uint32_t)(src[8 + j] >> 4) << (4 * (j % 8));
            }
            memcpy(o + 2, lo, 16);
        }
        return;
    }
    /* fall through */
    case T_Q4_1: {     /* d*q + m nao tem forma exata sem deslocamento: requantiza cada bloco em 8 bits */
        const size_t ub = qbytes(W->type, 32);
        for (size_t b = 0; b < cols / 32; b++, src += ub, o += 9) {
            float v[32], mx = 0.0f;
            int8_t q[32];
            dequant(W->type, src, v, 32);
            for (int j = 0; j < 32; j++) if (fabsf(v[j]) > mx) mx = fabsf(v[j]);
            const float d = mx / 127.0f, id = d > 0.0f ? 1.0f / d : 0.0f;
            for (int j = 0; j < 32; j++) q[j] = (int8_t)lrintf(v[j] * id);
            memcpy(o, &d, 4); memcpy(o + 1, q, 32);
        }
        return;
    }
    case T_Q8_0:
        for (size_t b = 0; b < cols / 32; b++, src += 34, o += 9) {
            memcpy(&h, src, 2);
            float d = f16_to_f32(h);
            memcpy(o, &d, 4); memcpy(o + 1, src + 2, 32);
        }
        return;
    case T_Q4_0:
        for (size_t b = 0; b < cols / 32; b++, src += 18, o += 9) {
            int8_t v[32];
            memcpy(&h, src, 2);
            float d = f16_to_f32(h);
            for (int j = 0; j < 16; j++) { v[j] = (int8_t)((src[2 + j] & 0xF) - 8); v[j + 16] = (int8_t)((src[2 + j] >> 4) - 8); }
            memcpy(o, &d, 4); memcpy(o + 1, v, 32);
        }
        return;
    case T_Q3_K:
        for (size_t b = 0; b < cols / 256; b++, src += 110, o += 53) {
            int8_t val[256], sc[16]; uint8_t qv[256];
            q3k_unpack(src, val, sc);
            for (int i = 0; i < 256; i++) qv[i] = (uint8_t)(val[i] + 32);
            memcpy(&h, src + 108, 2);
            pack_q6p(qv, sc, f16_to_f32(h), o);
        }
        return;
    default:    /* T_Q6_K */
        for (size_t b = 0; b < cols / 256; b++, src += 210, o += 53) {
            uint8_t qv[256];
            const uint8_t *ql = src, *qh = src + 128;
            for (int c = 0; c < 2; c++, ql += 64, qh += 32) {
                uint8_t *y = qv + c * 128;
                for (int l = 0; l < 32; l++) {
                    y[l]      = (uint8_t)((ql[l] & 0xF) | (((qh[l] >> 0) & 3) << 4));
                    y[l + 32] = (uint8_t)((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4));
                    y[l + 64] = (uint8_t)((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4));
                    y[l + 96] = (uint8_t)((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4));
                }
            }
            memcpy(&h, src + 208, 2);
            pack_q6p(qv, (const int8_t *)(src + 192), f16_to_f32(h), o);
        }
        return;
    }
}

