/* vkatt: porte dos kernels NAO-matmul do katana (OpenCL C -> GLSL compute / Vulkan) com teste de corretude
   contra referencia em double escrita aqui e medida de tempo por despacho.
   Kernels: rms, rmsh, gather, moe_sum, qkv_post, att1, att1f (+ variante com subgrupos), att_scores/softmax/mix.
   Uso: vkatt [-v] [-t] [-k nome]   (-v: uma linha por caso; -t: so tempos; -k: so um kernel, ex.: att1f) */
#define KVK_IMPL
#include "../../vk/kvk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <windows.h>
#include "rms.h"
#include "rms_wg.h"
#include "rmsh.h"
#include "gather.h"
#include "moe_sum.h"
#include "qkv_post_kv32.h"
#include "qkv_post_kv16.h"
#include "att1_kv32.h"
#include "att1_kv16.h"
#include "att1f_kv32.h"
#include "att1f_kv16.h"
#include "att_scores_kv32.h"
#include "att_scores_kv16.h"
#include "att_softmax_kv32.h"
#include "att_softmax_kv16.h"
#include "att_mix_kv32.h"
#include "att_mix_kv16.h"

static double now(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart; }
static int g_verbose = 0;
static const char *g_only = NULL;

/* ---------- float16 (arredonda para o mais proximo; so para gerar dados e decodificar) ---------- */
static uint16_t f2h(float f) {
    uint32_t u; memcpy(&u, &f, 4);
    uint32_t s = (u >> 16) & 0x8000; int e = (int)((u >> 23) & 0xFF) - 127 + 15; uint32_t m = u & 0x7FFFFF;
    if (e <= 0) { if (e < -10) return (uint16_t)s; m |= 0x800000; uint32_t sh = 14 - e; uint32_t r = (m + (1u << (sh - 1))) >> sh; return (uint16_t)(s | r); }
    if (e >= 31) return (uint16_t)(s | 0x7C00);
    uint32_t r = (uint32_t)(e << 10) | (m >> 13);
    if ((m & 0x1000) && ((m & 0xFFF) || (r & 1))) r++;
    return (uint16_t)(s | r);
}
static float h2f(uint16_t h) {
    uint32_t s = (h >> 15) & 1, m = h & 0x3ff, u; int e = (h >> 10) & 0x1f;
    if (e == 0) { if (!m) u = s << 31; else { e = 1; while (!(m & 0x400)) { m <<= 1; e--; } m &= 0x3ff; u = (s << 31) | ((uint32_t)(e + 112) << 23) | (m << 13); } }
    else u = (s << 31) | ((uint32_t)(e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &u, 4); return f;
}

/* ---------- aleatorio ---------- */
static uint64_t rs = 88172645463325252ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static float rnd01(void) { return (float)(rnd() >> 40) / 16777216.0f; }
static float rndsym(void) { return 2.0f * rnd01() - 1.0f; }
static void fillr(float *p, size_t n, float sc) { for (size_t i = 0; i < n; i++) p[i] = sc * rndsym(); }
static void *xm(size_t n) { void *p = calloc(n ? n : 1, 1); if (!p) { printf("sem memoria\n"); exit(1); } return p; }

/* ---------- contabilidade de erros ---------- */
typedef struct { double mabs, mrel; int bad; long n; } Err;
static void err0(Err *e) { memset(e, 0, sizeof *e); }
static void cmp1(Err *e, double got, double ref, double atol, double rtol) {
    double d = fabs(got - ref);
    e->n++;
    if (!(d == d) || !(fabs(got) < 1e30)) { e->bad++; e->mabs = INFINITY; e->mrel = INFINITY; return; }
    double rel = d / fmax(fabs(ref), 1e-2);
    if (d > e->mabs) e->mabs = d;
    if (rel > e->mrel) e->mrel = rel;
    if (d > atol + rtol * fabs(ref)) e->bad++;
}
static void cmpf(Err *e, const float *got, const double *ref, size_t n, double atol, double rtol) { for (size_t i = 0; i < n; i++) cmp1(e, got[i], ref[i], atol, rtol); }

typedef struct { const char *name; int pass, total; double wabs, wrel; } Kst;
static Kst ks[24]; static int nks = 0, cur = -1;
static int sel(const char *n) { return !g_only || strstr(n, g_only) != NULL; }
static void kbegin(const char *name) {
    cur = -1;
    for (int i = 0; i < nks; i++) if (!strcmp(ks[i].name, name)) cur = i;
    if (cur < 0) { cur = nks++; ks[cur].name = name; }
}
static void kcase(const Err *e, const char *fmt, ...) {
    Kst *k = &ks[cur]; k->total++;
    int ok = e->bad == 0 && e->n > 0;
    if (ok) k->pass++;
    if (e->mabs > k->wabs || e->mabs != e->mabs) k->wabs = e->mabs;
    if (e->mrel > k->wrel) k->wrel = e->mrel;
    if (!ok || g_verbose) {
        char buf[256]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
        printf("[%s] %-10s %-58s abs=%.2e rel=%.2e (%ld val, %d fora)\n", ok ? "PASS" : "FAIL", k->name, buf, e->mabs, e->mrel, e->n, e->bad);
    }
}

/* ---------- GPU: buffers de uso geral reaproveitados (kvk nao sub-aloca; poucos buffers grandes) ---------- */
#define NV 16
#define VBYTES (2u << 20)
#define CBYTES (12u << 20)
static KvkBuf V[NV], KCB, VCB;
static KvkBuf mkbuf(size_t n) { KvkBuf b = kvk_buf_create(n, KVK_USAGE_RW); if (!b.b) { printf("buffer: %s\n", kvk_error()); exit(1); } return b; }
static void wr(KvkBuf b, const void *p, size_t n) { if (!kvk_buf_write(b, 0, p, n)) { printf("write: %s\n", kvk_error()); exit(1); } }
static void rd(KvkBuf b, void *p, size_t n) { if (!kvk_buf_read(b, 0, p, n)) { printf("read: %s\n", kvk_error()); exit(1); } }
static void disp(KvkPipe p, KvkBuf *b, int n, const void *push, int pb, uint32_t gx) {
    if (!kvk_dispatch(p, b, n, push, pb, gx, 1, 1)) { printf("dispatch: %s\n", kvk_error()); exit(1); }
}
static KvkPipe mkp(const uint32_t *spv, size_t bytes, int nb, int push, int hd) {
    uint32_t id = 0, val = (uint32_t)hd;
    KvkPipe p = kvk_pipe_create(spv, bytes, nb, push, hd ? &id : NULL, hd ? &val : NULL, hd ? 1 : 0);
    if (!p.p) { printf("pipeline: %s\n", kvk_error()); exit(1); }
    return p;
}
#define MKP(spv, nb, push, hd) mkp(spv, sizeof spv, nb, push, hd)
static uint32_t grp(int n, int wg) { return (uint32_t)((n + wg - 1) / wg); }

/* ---------- parametros de forma ---------- */
typedef struct { int nh, hd, nkv; } Cfg;
static const Cfg CFG[] = { {12, 128, 2}, {12, 128, 12}, {16, 128, 4}, {32, 64, 4}, {16, 64, 16} };
#define NCFG 5
static const int POS[4] = { 0, 1, 37, 600 };
static const int BS[3] = { 1, 4, 16 };
static void mkfreq(float *f, int hd) { for (int i = 0; i < hd / 2; i++) f[i] = (float)pow(10000.0, -2.0 * i / hd); }
/* modos: 0 = sem bias nem norma, 1 = bias q,k,v, 2 = norma vetor inteiro, 3 = norma por cabeca
   (bias e norma nunca juntos: o motor nao tem arquitetura com os dois e o original soma os quadrados sem o bias) */
static const char *MODEN[4] = { "plain", "bias", "norm1", "norm2" };
static void mode_fields(int mode, int *has_bias, int *norm) { *has_bias = mode == 1 ? 7 : 0; *norm = mode == 2 ? 1 : mode == 3 ? 2 : 0; }

/* ======================= rms / rmsh / gather / moe_sum ======================= */
static KvkPipe P_rms, P_rmswg, P_rmsh, P_gather, P_moe;

static void t_rms(void) {
    const int dims[2] = { 1536, 2048 };
    for (int di = 0; di < 2; di++) for (int bi = 0; bi < 3; bi++) {
        int n = dims[di], B = BS[bi]; size_t N = (size_t)n * B;
        float *x = xm(N * 4), *g = xm(n * 4), *o = xm(N * 4); double *r = xm(N * 8);
        fillr(x, N, 3.0f); for (int i = 0; i < n; i++) g[i] = 1.0f + 0.5f * rndsym();
        for (size_t i = 0; i < N; i++) o[i] = 12345.0f;
        wr(V[0], x, N * 4); wr(V[1], g, n * 4); wr(V[2], o, N * 4);
        struct { int n; float eps; int B; } pc = { n, 1e-5f, B };
        KvkBuf b[3] = { V[0], V[1], V[2] };
        for (int var = 0; var < 2; var++) {
        kbegin(var ? "rms_wg" : "rms");
        for (size_t i = 0; i < N; i++) o[i] = 12345.0f;
        wr(V[2], o, N * 4);
        if (var) disp(P_rmswg, b, 3, &pc, sizeof pc, (uint32_t)B); else disp(P_rms, b, 3, &pc, sizeof pc, grp(n, 128));
        rd(V[2], o, N * 4);
        for (int bb = 0; bb < B; bb++) {
            double ss = 0; for (int j = 0; j < n; j++) ss += (double)x[(size_t)j * B + bb] * x[(size_t)j * B + bb];
            double inv = 1.0 / sqrt(ss / n + 1e-5);
            for (int i = 0; i < n; i++) r[(size_t)i * B + bb] = g[i] * x[(size_t)i * B + bb] * inv;
        }
        Err e; err0(&e); cmpf(&e, o, r, N, 1e-5, 2e-5);
        kcase(&e, "n=%d B=%d", n, B);
        }
        free(x); free(g); free(o); free(r);
    }
}

static void t_rmsh(void) {
    kbegin("rmsh");
    const int cf[4][3] = { {12, 128, 1}, {16, 64, 4}, {32, 128, 16}, {12, 128, 16} };
    for (int c = 0; c < 4; c++) {
        int nh = cf[c][0], hd = cf[c][1], B = cf[c][2], n = nh * hd; size_t N = (size_t)n * B;
        float *x = xm(N * 4), *g = xm(hd * 4), *o = xm(N * 4); double *r = xm(N * 8);
        fillr(x, N, 3.0f); for (int i = 0; i < hd; i++) g[i] = 1.0f + 0.5f * rndsym();
        wr(V[0], x, N * 4); wr(V[1], g, hd * 4); wr(V[2], o, N * 4);
        struct { int n, hd; float eps; int B; } pc = { n, hd, 1e-6f, B };
        KvkBuf b[3] = { V[0], V[1], V[2] };
        disp(P_rmsh, b, 3, &pc, sizeof pc, grp(n, 128)); rd(V[2], o, N * 4);
        for (int h = 0; h < nh; h++) for (int bb = 0; bb < B; bb++) {
            double ss = 0; for (int j = 0; j < hd; j++) { double v = x[(size_t)(h * hd + j) * B + bb]; ss += v * v; }
            double inv = 1.0 / sqrt(ss / hd + 1e-6);
            for (int j = 0; j < hd; j++) r[(size_t)(h * hd + j) * B + bb] = g[j] * x[(size_t)(h * hd + j) * B + bb] * inv;
        }
        Err e; err0(&e); cmpf(&e, o, r, N, 1e-5, 2e-5);
        kcase(&e, "%dx%d B=%d", nh, hd, B);
        free(x); free(g); free(o); free(r);
    }
}

static void t_gather(void) {
    kbegin("gather");
    const int dims[2] = { 1536, 2048 }, Bg[2] = { 4, 16 };
    for (int di = 0; di < 2; di++) for (int bi = 0; bi < 2; bi++) for (int oi = 0; oi < 3; oi++) {
        int n = dims[di], B = Bg[bi], off = oi == 0 ? 0 : oi == 1 ? B - 1 : B / 2 + 1; size_t N = (size_t)n * B;
        float *s = xm(N * 4), *o = xm(n * 4); double *r = xm(n * 8);
        fillr(s, N, 2.0f); for (int i = 0; i < n; i++) o[i] = 777.0f;
        wr(V[0], s, N * 4); wr(V[1], o, n * 4);
        struct { int n, stride, off; } pc = { n, B, off };
        KvkBuf b[2] = { V[0], V[1] };
        disp(P_gather, b, 2, &pc, sizeof pc, grp(n, 128)); rd(V[1], o, n * 4);
        for (int i = 0; i < n; i++) r[i] = s[(size_t)i * B + off];
        Err e; err0(&e); cmpf(&e, o, r, n, 0, 0);
        kcase(&e, "n=%d B=%d off=%d", n, B, off);
        free(s); free(o); free(r);
    }
}

static void t_moe(void) {
    kbegin("moe_sum");
    const int dims[2] = { 1536, 2048 }, Ks[2] = { 2, 8 };
    for (int di = 0; di < 2; di++) for (int bi = 0; bi < 3; bi++) for (int ki = 0; ki < 2; ki++) {
        int d = dims[di], B = BS[bi], K = Ks[ki], R = B * K, woff = 6 * R; size_t N = (size_t)d * B;
        float *x = xm(N * 4), *t = xm((size_t)R * d * 4); double *r = xm(N * 8);
        size_t nm = (size_t)woff + 2 * (size_t)B * K; uint32_t *meta = xm(nm * 4);
        fillr(x, N, 2.0f); fillr(t, (size_t)R * d, 2.0f);
        for (size_t i = 0; i < (size_t)woff; i++) meta[i] = (uint32_t)rnd();     /* lixo (a tabela mtab fica aqui) */
        for (int b = 0; b < B; b++) for (int k = 0; k < K; k++) {
            float w = 0.05f + 0.5f * rnd01(); memcpy(&meta[woff + b * K + k], &w, 4);
            meta[woff + (B + b) * K + k] = (uint32_t)(rnd() % (uint64_t)R);
        }
        wr(V[0], x, N * 4); wr(V[1], t, (size_t)R * d * 4); wr(V[2], meta, nm * 4);
        struct { int woff, K, d, B; } pc = { woff, K, d, B };
        KvkBuf bb3[3] = { V[0], V[1], V[2] };
        disp(P_moe, bb3, 3, &pc, sizeof pc, grp(d * B, 128));
        float *o = xm(N * 4); rd(V[0], o, N * 4);
        for (int i = 0; i < d; i++) for (int b = 0; b < B; b++) {
            double s = x[(size_t)i * B + b];
            for (int k = 0; k < K; k++) { float w; memcpy(&w, &meta[woff + b * K + k], 4); s += (double)w * t[(size_t)meta[woff + (B + b) * K + k] * d + i]; }
            r[(size_t)i * B + b] = s;
        }
        Err e; err0(&e); cmpf(&e, o, r, N, 1e-5, 2e-5);
        kcase(&e, "d=%d B=%d K=%d", d, B, K);
        free(x); free(t); free(r); free(meta); free(o);
    }
}

/* ======================= cache KV no host ======================= */
/* kv16: guarda halves em h16 e a copia decodificada em sh (double); kv32: guarda floats em f32 e em sh */
typedef struct { int kv16; size_t n; float *f32; uint16_t *h16; double *sh; } Cache;
static void cache_init(Cache *c, int kv16, size_t n) {
    c->kv16 = kv16; c->n = n; c->f32 = xm(n * 4); c->h16 = xm(n * 2); c->sh = xm(n * 8);
    for (size_t i = 0; i < n; i++) {
        float v = 1.5f * rndsym();
        if (kv16) { c->h16[i] = f2h(v); c->sh[i] = h2f(c->h16[i]); } else { c->f32[i] = v; c->sh[i] = v; }
    }
}
static void cache_free(Cache *c) { free(c->f32); free(c->h16); free(c->sh); }
static void cache_up(const Cache *c, KvkBuf b) { wr(b, c->kv16 ? (void *)c->h16 : (void *)c->f32, c->n * (c->kv16 ? 2 : 4)); }
static void cache_down(const Cache *c, KvkBuf b, double *out) {
    if (c->kv16) { uint16_t *t = xm(c->n * 2); rd(b, t, c->n * 2); for (size_t i = 0; i < c->n; i++) out[i] = h2f(t[i]); free(t); }
    else { float *t = xm(c->n * 4); rd(b, t, c->n * 4); for (size_t i = 0; i < c->n; i++) out[i] = t[i]; free(t); }
}
static void cmp_cache(Err *e, const double *got, const double *ref, size_t n, int kv16) {
    double atol = kv16 ? 4e-4 : 1e-4, rtol = kv16 ? 1.2e-3 : 1e-4;
    for (size_t i = 0; i < n; i++) cmp1(e, got[i], ref[i], atol, rtol);
}

/* ======================= qkv_post ======================= */
static KvkPipe P_qkv[2];

static void t_qkv(void) {
    kbegin("qkv_post");
    for (int kv16 = 0; kv16 < 2; kv16++) for (int ci = 0; ci < NCFG; ci++) for (int bi = 0; bi < 3; bi++) for (int pi = 0; pi < 4; pi++)
    for (int neox = 0; neox < 2; neox++) for (int mode = 0; mode < 4; mode++) {
        Cfg c = CFG[ci]; int B = BS[bi], pos0 = POS[pi], hd = c.hd, d = c.nh * hd, kvd = c.nkv * hd;
        int has_bias, norm; mode_fields(mode, &has_bias, &norm);
        int ctx = pos0 + B + 4, layer = 1; size_t base = (size_t)layer * ctx * kvd, ncache = 2 * (size_t)ctx * kvd;
        size_t Nq = (size_t)d * B, Nk = (size_t)kvd * B;
        float *qs = xm(Nq * 4), *k = xm(Nk * 4), *v = xm(Nk * 4), *bq = xm(d * 4), *bk = xm(kvd * 4), *bv = xm(kvd * 4);
        float *fr = xm(hd * 4), *qn = xm(d * 4), *kn = xm(kvd * 4), *qo = xm(Nq * 4);
        fillr(qs, Nq, 2.0f); fillr(k, Nk, 2.0f); fillr(v, Nk, 2.0f); fillr(bq, d, 0.5f); fillr(bk, kvd, 0.5f); fillr(bv, kvd, 0.5f);
        for (int i = 0; i < d; i++) qn[i] = 1.0f + 0.3f * rndsym();
        for (int i = 0; i < kvd; i++) kn[i] = 1.0f + 0.3f * rndsym();
        mkfreq(fr, hd);
        Cache kc, vc; cache_init(&kc, kv16, ncache); cache_init(&vc, kv16, ncache);
        wr(V[1], k, Nk * 4); wr(V[2], v, Nk * 4); wr(V[3], bq, d * 4); wr(V[4], bk, kvd * 4); wr(V[5], bv, kvd * 4);
        wr(V[8], fr, hd * 4); wr(V[10], qn, d * 4); wr(V[11], kn, kvd * 4);
        cache_up(&kc, KCB); cache_up(&vc, VCB);
        KvkBuf qsrc;
        if (norm) { wr(V[9], qs, Nq * 4); wr(V[0], qo, Nq * 4); qsrc = V[9]; }   /* q separado do bruto */
        else { wr(V[0], qs, Nq * 4); qsrc = V[0]; }                              /* no lugar: mesmo buffer */
        struct { int base, pos0, hd, neox, has_bias, d, kvd, norm, B; float eps; } pc = { (int)base, pos0, hd, neox, has_bias, d, kvd, norm, B, 1e-6f };
        KvkBuf bf[12] = { V[0], V[1], V[2], V[3], V[4], V[5], KCB, VCB, V[8], qsrc, V[10], V[11] };
        disp(P_qkv[kv16], bf, 12, &pc, sizeof pc, grp(B * (d / 2 + kvd / 2 + kvd), 128));
        /* referencia */
        double *rq = xm(Nq * 8); double *rk = xm(ncache * 8), *rv = xm(ncache * 8);
        memcpy(rk, kc.sh, ncache * 8); memcpy(rv, vc.sh, ncache * 8);
        for (int b = 0; b < B; b++) {
            int pos = pos0 + b;
            double *xq = xm(d * 8), *xk = xm(kvd * 8);
            for (int i = 0; i < d; i++) xq[i] = qs[(size_t)i * B + b] + ((has_bias & 1) ? bq[i] : 0.0);
            for (int i = 0; i < kvd; i++) xk[i] = k[(size_t)i * B + b] + ((has_bias & 2) ? bk[i] : 0.0);
            if (norm) {
                for (int h = 0; h < c.nh; h++) {
                    int a0 = norm == 1 ? 0 : h * hd, len = norm == 1 ? d : hd;
                    if (norm == 1 && h > 0) continue;
                    double ss = 0; for (int j = 0; j < len; j++) ss += xq[a0 + j] * xq[a0 + j];
                    double inv = 1.0 / sqrt(ss / len + 1e-6);
                    for (int j = 0; j < len; j++) xq[a0 + j] *= qn[j] * inv;
                }
                for (int h = 0; h < c.nkv; h++) {
                    int a0 = norm == 1 ? 0 : h * hd, len = norm == 1 ? kvd : hd;
                    if (norm == 1 && h > 0) continue;
                    double ss = 0; for (int j = 0; j < len; j++) ss += xk[a0 + j] * xk[a0 + j];
                    double inv = 1.0 / sqrt(ss / len + 1e-6);
                    for (int j = 0; j < len; j++) xk[a0 + j] *= kn[j] * inv;
                }
            }
            /* RoPE */
            for (int pass = 0; pass < 2; pass++) {
                int nhh = pass == 0 ? c.nh : c.nkv; double *xx = pass == 0 ? xq : xk;
                for (int h = 0; h < nhh; h++) for (int i = 0; i < hd / 2; i++) {
                    int i0 = h * hd + (neox ? i : 2 * i), i1 = i0 + (neox ? hd / 2 : 1);
                    double ang = (double)((float)pos * fr[i]), cs = cos(ang), sn = sin(ang);
                    double a = xx[i0], bb2 = xx[i1];
                    xx[i0] = a * cs - bb2 * sn; xx[i1] = a * sn + bb2 * cs;
                }
            }
            for (int i = 0; i < d; i++) rq[(size_t)i * B + b] = xq[i];
            for (int i = 0; i < kvd; i++) {
                rk[base + (size_t)pos * kvd + i] = xk[i];
                rv[base + (size_t)pos * kvd + i] = v[(size_t)i * B + b] + ((has_bias & 4) ? bv[i] : 0.0);
            }
            free(xq); free(xk);
        }
        float *gq = xm(Nq * 4); rd(V[0], gq, Nq * 4);
        double *gk = xm(ncache * 8), *gv = xm(ncache * 8); cache_down(&kc, KCB, gk); cache_down(&vc, VCB, gv);
        Err e; err0(&e);
        cmpf(&e, gq, rq, Nq, 2e-4, 2e-4); cmp_cache(&e, gk, rk, ncache, kv16); cmp_cache(&e, gv, rv, ncache, kv16);
        kcase(&e, "%dx%d kv%d B=%2d pos0=%3d neox=%d %s kv%d", c.nh, hd, c.nkv, B, pos0, neox, MODEN[mode], kv16 ? 16 : 32);
        free(qs); free(k); free(v); free(bq); free(bk); free(bv); free(fr); free(qn); free(kn); free(qo); free(rq); free(rk); free(rv);
        free(gq); free(gk); free(gv); cache_free(&kc); cache_free(&vc);
    }
}

/* ======================= atencao de um token: att1f e att1 ======================= */
static KvkPipe P_att1f[2][2], P_att1[2][2];     /* [kv16][hd==128] ; att1 [kv16][hd==128] */

typedef struct { int pos, ctx, hd, kvd, group, neox, has_bias, norm, d; float scale, eps; int base; } AttPC;

/* referencia: atencao de um token. cache sh (kc/vc), t<pos lidos do cache; posicao pos usa k,v locais */
static void ref_att1(const Cfg *c, int pos, int ctx, size_t base, const double *kcs, const double *vcs, const double *qr, double *out, double *eatt, const double *kcur, const double *vcur) {
    int hd = c->hd, kvd = c->nkv * hd, group = c->nh / c->nkv; double scale = 1.0 / sqrt((double)hd);
    double *s = xm((pos + 1) * 8);
    for (int h = 0; h < c->nh; h++) {
        int g = h / group; double mx = -1e300;
        for (int t = 0; t <= pos; t++) {
            double a = 0;
            for (int j = 0; j < hd; j++) a += qr[h * hd + j] * (t == pos && kcur ? kcur[g * hd + j] : kcs[base + (size_t)t * kvd + g * hd + j]);
            s[t] = a * scale; if (s[t] > mx) mx = s[t];
        }
        double sum = 0;
        for (int t = 0; t <= pos; t++) { s[t] = exp(s[t] - mx); sum += s[t]; if (eatt) eatt[(size_t)h * ctx + t] = s[t]; }
        for (int j = 0; j < hd; j++) {
            double a = 0;
            for (int t = 0; t <= pos; t++) a += s[t] * (t == pos && vcur ? vcur[g * hd + j] : vcs[base + (size_t)t * kvd + g * hd + j]);
            out[h * hd + j] = a / sum;
        }
    }
    free(s);
}

static void t_att1f_one(int kv16, int ci, int pi, int neox, int mode, int quiet_name) {
    (void)quiet_name;
    Cfg c = CFG[ci]; int pos = POS[pi], hd = c.hd, d = c.nh * hd, kvd = c.nkv * hd, group = c.nh / c.nkv;
    int has_bias, norm; mode_fields(mode, &has_bias, &norm);
    int ctx = pos + 4, layer = 1; size_t base = (size_t)layer * ctx * kvd, ncache = 2 * (size_t)ctx * kvd;
    float *qs = xm(d * 4), *k = xm(kvd * 4), *v = xm(kvd * 4), *bq = xm(d * 4), *bk = xm(kvd * 4), *bv = xm(kvd * 4);
    float *fr = xm(hd * 4), *qn = xm(d * 4), *kn = xm(kvd * 4);
    fillr(qs, d, 2.0f); fillr(k, kvd, 2.0f); fillr(v, kvd, 2.0f); fillr(bq, d, 0.5f); fillr(bk, kvd, 0.5f); fillr(bv, kvd, 0.5f);
    for (int i = 0; i < d; i++) qn[i] = 1.0f + 0.3f * rndsym();
    for (int i = 0; i < kvd; i++) kn[i] = 1.0f + 0.3f * rndsym();
    mkfreq(fr, hd);
    Cache kc, vc; cache_init(&kc, kv16, ncache); cache_init(&vc, kv16, ncache);
    wr(V[0], qs, d * 4); wr(V[1], k, kvd * 4); wr(V[2], v, kvd * 4); wr(V[3], bq, d * 4); wr(V[4], bk, kvd * 4); wr(V[5], bv, kvd * 4);
    wr(V[10], fr, hd * 4); wr(V[11], qn, d * 4); wr(V[12], kn, kvd * 4);
    cache_up(&kc, KCB); cache_up(&vc, VCB);
    size_t natt = (size_t)c.nh * ctx; float *z = xm(natt * 4); for (size_t i = 0; i < natt; i++) z[i] = 9999.0f;
    wr(V[8], z, natt * 4); wr(V[9], z, (size_t)c.nh * hd < natt ? (size_t)c.nh * hd * 4 : natt * 4);
    struct { int base, pos, ctx, hd, kvd, group, neox, has_bias, norm, d; float scale, eps; } pc =
        { (int)base, pos, ctx, hd, kvd, group, neox, has_bias, norm, d, 1.0f / sqrtf((float)hd), 1e-6f };
    KvkBuf bf[13] = { V[0], V[1], V[2], V[3], V[4], V[5], KCB, VCB, V[8], V[9], V[10], V[11], V[12] };
    disp(P_att1f[kv16][hd == 128], bf, 13, &pc, sizeof pc, (uint32_t)c.nh);
    /* referencia: q e k pos-processados em double */
    double *xq = xm(d * 8), *xk = xm(kvd * 8), *xv = xm(kvd * 8);
    for (int i = 0; i < d; i++) xq[i] = qs[i] + ((has_bias & 1) ? bq[i] : 0.0);
    for (int i = 0; i < kvd; i++) { xk[i] = k[i] + ((has_bias & 2) ? bk[i] : 0.0); xv[i] = v[i] + ((has_bias & 4) ? bv[i] : 0.0); }
    if (norm) {
        for (int h = 0; h < c.nh; h++) { if (norm == 1 && h) break; int a0 = norm == 1 ? 0 : h * hd, len = norm == 1 ? d : hd;
            double ss = 0; for (int j = 0; j < len; j++) ss += xq[a0 + j] * xq[a0 + j]; double inv = 1.0 / sqrt(ss / len + 1e-6);
            for (int j = 0; j < len; j++) xq[a0 + j] *= qn[j] * inv; }
        for (int h = 0; h < c.nkv; h++) { if (norm == 1 && h) break; int a0 = norm == 1 ? 0 : h * hd, len = norm == 1 ? kvd : hd;
            double ss = 0; for (int j = 0; j < len; j++) ss += xk[a0 + j] * xk[a0 + j]; double inv = 1.0 / sqrt(ss / len + 1e-6);
            for (int j = 0; j < len; j++) xk[a0 + j] *= kn[j] * inv; }
    }
    for (int pass = 0; pass < 2; pass++) {
        int nhh = pass == 0 ? c.nh : c.nkv; double *xx = pass == 0 ? xq : xk;
        for (int h = 0; h < nhh; h++) for (int i = 0; i < hd / 2; i++) {
            int i0 = h * hd + (neox ? i : 2 * i), i1 = i0 + (neox ? hd / 2 : 1);
            double ang = (double)((float)pos * fr[i]), cs = cos(ang), sn = sin(ang), a = xx[i0], b2 = xx[i1];
            xx[i0] = a * cs - b2 * sn; xx[i1] = a * sn + b2 * cs;
        }
    }
    double *ro = xm(d * 8), *re = xm(natt * 8);
    ref_att1(&c, pos, ctx, base, kc.sh, vc.sh, xq, ro, re, xk, xv);
    double *rk = xm(ncache * 8), *rv = xm(ncache * 8); memcpy(rk, kc.sh, ncache * 8); memcpy(rv, vc.sh, ncache * 8);
    for (int i = 0; i < kvd; i++) { rk[base + (size_t)pos * kvd + i] = xk[i]; rv[base + (size_t)pos * kvd + i] = xv[i]; }
    float *go = xm(d * 4), *ga = xm(natt * 4); rd(V[9], go, d * 4); rd(V[8], ga, natt * 4);
    double *gk = xm(ncache * 8), *gv = xm(ncache * 8); cache_down(&kc, KCB, gk); cache_down(&vc, VCB, gv);
    Err e; err0(&e);
    cmpf(&e, go, ro, d, 3e-5, 1e-4);
    for (int h = 0; h < c.nh; h++) for (int t = 0; t <= pos; t++) cmp1(&e, ga[(size_t)h * ctx + t], re[(size_t)h * ctx + t], 1e-4, 2e-4);
    cmp_cache(&e, gk, rk, ncache, kv16); cmp_cache(&e, gv, rv, ncache, kv16);
    kcase(&e, "%dx%d kv%d pos=%3d neox=%d %s kv%d", c.nh, hd, c.nkv, pos, neox, MODEN[mode], kv16 ? 16 : 32);
    free(qs); free(k); free(v); free(bq); free(bk); free(bv); free(fr); free(qn); free(kn); free(z);
    free(xq); free(xk); free(xv); free(ro); free(re); free(rk); free(rv); free(go); free(ga); free(gk); free(gv);
    cache_free(&kc); cache_free(&vc);
}
static void t_att1f(void) {
    kbegin("att1f");
    for (int kv16 = 0; kv16 < 2; kv16++) for (int ci = 0; ci < NCFG; ci++) for (int pi = 0; pi < 4; pi++)
    for (int neox = 0; neox < 2; neox++) for (int mode = 0; mode < 4; mode++) t_att1f_one(kv16, ci, pi, neox, mode, 0);
}

static void t_att1(void) {
    kbegin("att1");
    for (int kv16 = 0; kv16 < 2; kv16++) for (int ci = 0; ci < NCFG; ci++) for (int pi = 0; pi < 4; pi++) {
        Cfg c = CFG[ci]; int pos = POS[pi], hd = c.hd, d = c.nh * hd, kvd = c.nkv * hd, group = c.nh / c.nkv;
        int ctx = pos + 4, layer = 1; size_t base = (size_t)layer * ctx * kvd, ncache = 2 * (size_t)ctx * kvd;
        float *q = xm(d * 4); fillr(q, d, 1.5f);
        double *qd = xm(d * 8); for (int i = 0; i < d; i++) qd[i] = q[i];
        Cache kc, vc; cache_init(&kc, kv16, ncache); cache_init(&vc, kv16, ncache);
        wr(V[0], q, d * 4); cache_up(&kc, KCB); cache_up(&vc, VCB);
        size_t natt = (size_t)c.nh * ctx; float *z = xm(natt * 4); wr(V[3], z, natt * 4); wr(V[4], z, d * 4);
        struct { int base, pos, ctx, hd, kvd, group; float scale; } pc = { (int)base, pos, ctx, hd, kvd, group, 1.0f / sqrtf((float)hd) };
        KvkBuf bf[5] = { V[0], KCB, VCB, V[3], V[4] };
        disp(P_att1[kv16][hd == 128], bf, 5, &pc, sizeof pc, (uint32_t)c.nh);
        double *ro = xm(d * 8), *re = xm(natt * 8);
        ref_att1(&c, pos, ctx, base, kc.sh, vc.sh, qd, ro, re, NULL, NULL);
        float *go = xm(d * 4), *ga = xm(natt * 4); rd(V[4], go, d * 4); rd(V[3], ga, natt * 4);
        Err e; err0(&e); cmpf(&e, go, ro, d, 3e-5, 1e-4);
        for (int h = 0; h < c.nh; h++) for (int t = 0; t <= pos; t++) cmp1(&e, ga[(size_t)h * ctx + t], re[(size_t)h * ctx + t], 1e-4, 2e-4);
        kcase(&e, "%dx%d kv%d pos=%3d kv%d", c.nh, hd, c.nkv, pos, kv16 ? 16 : 32);
        free(q); free(qd); free(z); free(ro); free(re); free(go); free(ga); cache_free(&kc); cache_free(&vc);
    }
}

/* ======================= atencao em lote: att_scores, att_softmax, att_mix ======================= */
static KvkPipe P_sc[2], P_sm[2], P_mx[2];

static void t_attsplit(void) {
    kbegin("att_scores"); int i_sc = cur; kbegin("att_softmax"); int i_sm = cur; kbegin("att_mix"); int i_mx = cur;
    for (int kv16 = 0; kv16 < 2; kv16++) for (int ci = 0; ci < NCFG; ci++) for (int bi = 0; bi < 3; bi++) for (int pi = 0; pi < 4; pi++) {
        Cfg c = CFG[ci]; int B = BS[bi], pos0 = POS[pi], hd = c.hd, nh = c.nh, d = nh * hd, kvd = c.nkv * hd, group = nh / c.nkv;
        int tmax = pos0 + B, ctx = tmax + 4, layer = 1; size_t base = (size_t)layer * ctx * kvd, ncache = 2 * (size_t)ctx * kvd;
        size_t Nq = (size_t)d * B, natt = (size_t)B * nh * ctx; float scale = 1.0f / sqrtf((float)hd);
        float *q = xm(Nq * 4); fillr(q, Nq, 1.5f);
        Cache kc, vc; cache_init(&kc, kv16, ncache); cache_init(&vc, kv16, ncache);
        float *z = xm(natt * 4); for (size_t i = 0; i < natt; i++) z[i] = 4242.0f;
        float *zo = xm(Nq * 4); for (size_t i = 0; i < Nq; i++) zo[i] = 4242.0f;
        wr(V[0], q, Nq * 4); wr(V[1], z, natt * 4); wr(V[2], zo, Nq * 4); cache_up(&kc, KCB); cache_up(&vc, VCB);
        /* referencia (double) */
        double *rs_ = xm(natt * 8), *rp = xm(natt * 8), *ro = xm(Nq * 8);
        for (int b = 0; b < B; b++) for (int h = 0; h < nh; h++) {
            int g = h / group; double *a = rs_ + ((size_t)b * nh + h) * ctx, *p = rp + ((size_t)b * nh + h) * ctx; int T = pos0 + b + 1; double mx = -1e300, sum = 0;
            for (int t = 0; t < T; t++) {
                double s = 0; for (int j = 0; j < hd; j++) s += (double)q[(size_t)(h * hd + j) * B + b] * kc.sh[base + (size_t)t * kvd + g * hd + j];
                a[t] = s * scale; if (a[t] > mx) mx = a[t];
            }
            for (int t = 0; t < T; t++) { p[t] = exp(a[t] - mx); sum += p[t]; }
            for (int t = 0; t < T; t++) p[t] /= sum;
            for (int j = 0; j < hd; j++) { double o = 0; for (int t = 0; t < T; t++) o += p[t] * vc.sh[base + (size_t)t * kvd + g * hd + j]; ro[(size_t)(h * hd + j) * B + b] = o; }
        }
        float *ga = xm(natt * 4), *go = xm(Nq * 4);
        /* estagio 1: notas */
        struct { int base, pos0, tmax, ctx, hd, kvd, group, nh, B; float scale; } p1 = { (int)base, pos0, tmax, ctx, hd, kvd, group, nh, B, scale };
        KvkBuf b1[3] = { V[0], KCB, V[1] };
        disp(P_sc[kv16], b1, 3, &p1, sizeof p1, grp(B * nh * tmax, 128));
        rd(V[1], ga, natt * 4);
        Err e1; err0(&e1);
        for (int b = 0; b < B; b++) for (int h = 0; h < nh; h++) for (int t = 0; t <= pos0 + b; t++) cmp1(&e1, ga[((size_t)b * nh + h) * ctx + t], rs_[((size_t)b * nh + h) * ctx + t], 3e-5, 1e-4);
        /* t > pos0+b nao pode ser escrito */
        for (int b = 0; b < B; b++) for (int h = 0; h < nh; h++) for (int t = pos0 + b + 1; t < ctx; t++) cmp1(&e1, ga[((size_t)b * nh + h) * ctx + t], 4242.0, 0, 0);
        cur = i_sc; kcase(&e1, "%dx%d kv%d B=%2d pos0=%3d kv%d", nh, hd, c.nkv, B, pos0, kv16 ? 16 : 32);
        /* estagio 2: softmax (parte do que o estagio 1 gravou) */
        struct { int pos0, ctx, nh, B; } p2 = { pos0, ctx, nh, B };
        KvkBuf b2[1] = { V[1] };
        disp(P_sm[kv16], b2, 1, &p2, sizeof p2, grp(B * nh, 128));
        rd(V[1], ga, natt * 4);
        Err e2; err0(&e2);
        for (int b = 0; b < B; b++) for (int h = 0; h < nh; h++) for (int t = 0; t <= pos0 + b; t++) cmp1(&e2, ga[((size_t)b * nh + h) * ctx + t], rp[((size_t)b * nh + h) * ctx + t], 2e-6, 2e-4);
        for (int b = 0; b < B; b++) for (int h = 0; h < nh; h++) for (int t = pos0 + b + 1; t < ctx; t++) cmp1(&e2, ga[((size_t)b * nh + h) * ctx + t], 4242.0, 0, 0);
        cur = i_sm; kcase(&e2, "%dx%d kv%d B=%2d pos0=%3d kv%d", nh, hd, c.nkv, B, pos0, kv16 ? 16 : 32);
        /* estagio 3: mistura */
        struct { int base, pos0, ctx, hd, kvd, group, nh, B; } p3 = { (int)base, pos0, ctx, hd, kvd, group, nh, B };
        KvkBuf b3[3] = { V[1], VCB, V[2] };
        disp(P_mx[kv16], b3, 3, &p3, sizeof p3, grp(B * d, 128));
        rd(V[2], go, Nq * 4);
        Err e3; err0(&e3); cmpf(&e3, go, ro, Nq, 3e-5, 1e-4);
        cur = i_mx; kcase(&e3, "%dx%d kv%d B=%2d pos0=%3d kv%d", nh, hd, c.nkv, B, pos0, kv16 ? 16 : 32);
        free(q); free(z); free(zo); free(rs_); free(rp); free(ro); free(ga); free(go); cache_free(&kc); cache_free(&vc);
    }
}

/* ======================= tempos ======================= */
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
/* grava N despachos iguais num grafo, submete varias vezes; devolve min e mediana em us por despacho */
static void bench(KvkPipe p, KvkBuf *b, int n, const void *push, int pb, uint32_t gx, double *tmin, double *tmed) {
    enum { N = 200, REP = 9 };
    kvk_wait();
    if (!kvk_rec_begin()) { printf("rec_begin: %s\n", kvk_error()); exit(1); }
    for (int i = 0; i < N; i++) if (!kvk_dispatch(p, b, n, push, pb, gx, 1, 1)) { printf("dispatch: %s\n", kvk_error()); exit(1); }
    KvkRec r = kvk_rec_end();
    kvk_rec_submit(r); kvk_wait();
    double t[REP];
    for (int k = 0; k < REP; k++) { double t0 = now(); kvk_rec_submit(r); kvk_wait(); t[k] = (now() - t0) / N * 1e6; }
    kvk_rec_free(&r);
    qsort(t, REP, sizeof(double), cmpd);
    *tmin = t[0]; *tmed = t[REP / 2];
}
static void tline(const char *name, KvkPipe p, KvkBuf *b, int n, const void *push, int pb, uint32_t gx) {
    double a, m; bench(p, b, n, push, pb, gx, &a, &m);
    printf("  %-34s min %8.2f us   mediana %8.2f us\n", name, a, m);
}

static void timing(void) {
    printf("\n== Tempos (us por despacho; %d despachos num grafo, 1 espera; inclui a barreira entre despachos) ==\n", 200);
    printf("   forma: dim 1536, 12 cabecas x 128, 2 cabecas kv (grupos de 6), posicao 600\n");
    const int hd = 128, nh = 12, nkv = 2, d = 1536, kvd = 256, group = 6, pos = 600, ctx = 640;
    size_t base = (size_t)1 * ctx * kvd;
    /* dados */
    float *t = xm(VBYTES);
    for (int i = 0; i < NV; i++) { fillr(t, VBYTES / 4, 1.0f); wr(V[i], t, VBYTES); }
    float fr[64]; mkfreq(fr, hd); wr(V[10], fr, sizeof fr);   /* freq (lida so ate hd/2) */
    for (int i = 0; i < 1536; i++) t[i] = 1.0f; wr(V[11], t, 1536 * 4); wr(V[12], t, 1536 * 4);
    free(t);
    size_t ncache = 2 * (size_t)ctx * kvd;
    Cache kc32, kc16; cache_init(&kc32, 0, ncache); cache_init(&kc16, 1, ncache);
    KvkBuf K32 = KCB, V32 = VCB;     /* os mesmos buffers servem para as duas larguras (so os bytes mudam) */
    int bi;
    for (int kv16 = 0; kv16 < 2; kv16++) {
        const Cache *c = kv16 ? &kc16 : &kc32; cache_up(c, K32); cache_up(c, V32);
        char nm[96]; const char *kn = kv16 ? "kv16" : "kv32";
        struct { int base, pos, ctx, hd, kvd, group, neox, has_bias, norm, d; float scale, eps; } pf = { (int)base, pos, ctx, hd, kvd, group, 1, 7, 0, d, 1.0f / sqrtf(128.0f), 1e-6f };
        KvkBuf bf[13] = { V[0], V[1], V[2], V[3], V[4], V[5], K32, V32, V[8], V[9], V[10], V[11], V[12] };
        /* att deve ter valores finitos: zerar */
        { float *z = xm(VBYTES); wr(V[8], z, VBYTES); wr(V[9], z, VBYTES); free(z); }
        /* V[8] e att E freq no att1f: freq = indice 10, att = 8; zera 8 (att), freq ja em 10 */
        snprintf(nm, sizeof nm, "att1f %s", kn); tline(nm, P_att1f[kv16][1], bf, 13, &pf, sizeof pf, nh);
        struct { int base, pos, ctx, hd, kvd, group; float scale; } p1 = { (int)base, pos, ctx, hd, kvd, group, 1.0f / sqrtf(128.0f) };
        KvkBuf b1[5] = { V[0], K32, V32, V[8], V[9] };
        snprintf(nm, sizeof nm, "att1 %s", kn); tline(nm, P_att1[kv16][1], b1, 5, &p1, sizeof p1, nh);
        for (bi = 0; bi < 3; bi += 2) {
            int B = BS[bi], pos0 = pos, tmax = pos0 + B;
            struct { int base, pos0, tmax, ctx, hd, kvd, group, nh, B; float scale; } s1 = { (int)base, pos0, tmax, ctx, hd, kvd, group, nh, B, 1.0f / sqrtf(128.0f) };
            KvkBuf bs1[3] = { V[0], K32, V[8] };
            snprintf(nm, sizeof nm, "att_scores %s B=%d", kn, B); tline(nm, P_sc[kv16], bs1, 3, &s1, sizeof s1, grp(B * nh * tmax, 128));
            struct { int pos0, ctx, nh, B; } s2 = { pos0, ctx, nh, B };
            KvkBuf bs2[1] = { V[8] };
            snprintf(nm, sizeof nm, "att_softmax B=%d", B); tline(nm, P_sm[kv16], bs2, 1, &s2, sizeof s2, grp(B * nh, 128));
            struct { int base, pos0, ctx, hd, kvd, group, nh, B; } s3 = { (int)base, pos0, ctx, hd, kvd, group, nh, B };
            KvkBuf bs3[3] = { V[8], V32, V[9] };
            snprintf(nm, sizeof nm, "att_mix %s B=%d", kn, B); tline(nm, P_mx[kv16], bs3, 3, &s3, sizeof s3, grp(B * d, 128));
        }
        for (bi = 0; bi < 3; bi += 2) {
            int B = BS[bi];
            struct { int base, pos0, hd, neox, has_bias, d, kvd, norm, B; float eps; } q = { (int)base, pos, hd, 1, 7, d, kvd, 0, B, 1e-6f };
            KvkBuf bq[12] = { V[0], V[1], V[2], V[3], V[4], V[5], K32, V32, V[10], V[0], V[11], V[12] };
            snprintf(nm, sizeof nm, "qkv_post %s B=%d (bias, neox)", kn, B); tline(nm, P_qkv[kv16], bq, 12, &q, sizeof q, grp(B * (d / 2 + kvd / 2 + kvd), 128));
        }
        struct { int base, pos0, hd, neox, has_bias, d, kvd, norm, B; float eps; } q1 = { (int)base, pos, hd, 1, 0, d, kvd, 1, 1, 1e-6f };
        KvkBuf bq1[12] = { V[0], V[1], V[2], V[3], V[4], V[5], K32, V32, V[10], V[9], V[11], V[12] };
        snprintf(nm, sizeof nm, "qkv_post %s B=1 (norm1)", kn); tline(nm, P_qkv[kv16], bq1, 12, &q1, sizeof q1, grp(d / 2 + kvd / 2 + kvd, 128));
        q1.norm = 2; snprintf(nm, sizeof nm, "qkv_post %s B=1 (norm2)", kn); tline(nm, P_qkv[kv16], bq1, 12, &q1, sizeof q1, grp(d / 2 + kvd / 2 + kvd, 128));
    }
    cache_free(&kc32); cache_free(&kc16);
    for (bi = 0; bi < 3; bi++) {
        int B = BS[bi]; char nm[64];
        struct { int n; float eps; int B; } pr = { d, 1e-5f, B };
        KvkBuf br[3] = { V[0], V[1], V[2] };
        snprintf(nm, sizeof nm, "rms n=1536 B=%d (fiel)", B); tline(nm, P_rms, br, 3, &pr, sizeof pr, grp(d, 128));
        snprintf(nm, sizeof nm, "rms_wg n=1536 B=%d (melhoria)", B); tline(nm, P_rmswg, br, 3, &pr, sizeof pr, (uint32_t)B);
    }
    { struct { int n, hd; float eps; int B; } ph = { d, hd, 1e-6f, 1 }; KvkBuf bh[3] = { V[0], V[1], V[2] };
      tline("rmsh 12x128 B=1", P_rmsh, bh, 3, &ph, sizeof ph, grp(d, 128)); }
    { struct { int n, stride, off; } pg = { d, 16, 15 }; KvkBuf bg[2] = { V[0], V[1] };
      tline("gather n=1536 stride=16", P_gather, bg, 2, &pg, sizeof pg, grp(d, 128)); }
    {   /* moe_sum: meta valida */
        const int Ks[2] = { 8, 8 }; uint32_t *meta = xm(4096 * 4);
        for (bi = 0; bi < 3; bi += 2) {
            int B = BS[bi], K = Ks[0], R = B * K, woff = 6 * R;
            for (int b = 0; b < B; b++) for (int k = 0; k < K; k++) { float w = 0.1f; memcpy(&meta[woff + b * K + k], &w, 4); meta[woff + (B + b) * K + k] = (uint32_t)((b * K + k) % R); }
            wr(V[2], meta, 4096 * 4);
            struct { int woff, K, d, B; } pm = { woff, K, d, B }; KvkBuf bm[3] = { V[0], V[1], V[2] }; char nm[64];
            snprintf(nm, sizeof nm, "moe_sum d=1536 K=8 B=%d", B); tline(nm, P_moe, bm, 3, &pm, sizeof pm, grp(d * B, 128));
        }
        free(meta);
    }
}

/* ======================= main ======================= */
int main(int argc, char **argv) {
    int only_time = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "-t")) only_time = 1;
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) g_only = argv[++i];
    }
    KvkInfo I;
    if (!kvk_init(&I)) { printf("kvk_init falhou: %s\n", kvk_error()); return 2; }
    printf("GPU: %s (subgrupo %u, fp16 %d, storage16 %d)\n", I.name, I.subgroup_size, I.fp16, I.storage16);
    if (!I.fp16 || !I.storage16) { printf("sem fp16/storage16: as variantes kv16 nao rodam\n"); return 2; }
    for (int i = 0; i < NV; i++) V[i] = mkbuf(VBYTES);
    KCB = mkbuf(CBYTES); VCB = mkbuf(CBYTES);
    P_rms = MKP(rms_spv, 3, 12, 0); P_rmswg = MKP(rms_wg_spv, 3, 12, 0); P_rmsh = MKP(rmsh_spv, 3, 16, 0); P_gather = MKP(gather_spv, 2, 12, 0); P_moe = MKP(moe_sum_spv, 3, 16, 0);
    P_qkv[0] = MKP(qkv_post_kv32_spv, 12, 40, 0); P_qkv[1] = MKP(qkv_post_kv16_spv, 12, 40, 0);
    const int hdv[2] = { 64, 128 };
    for (int h = 0; h < 2; h++) {
        P_att1f[0][h] = MKP(att1f_kv32_spv, 13, 48, hdv[h]); P_att1f[1][h] = MKP(att1f_kv16_spv, 13, 48, hdv[h]);
        P_att1[0][h] = MKP(att1_kv32_spv, 5, 28, hdv[h]); P_att1[1][h] = MKP(att1_kv16_spv, 5, 28, hdv[h]);
    }
    P_sc[0] = MKP(att_scores_kv32_spv, 3, 40, 0); P_sc[1] = MKP(att_scores_kv16_spv, 3, 40, 0);
    P_sm[0] = MKP(att_softmax_kv32_spv, 1, 16, 0); P_sm[1] = MKP(att_softmax_kv16_spv, 1, 16, 0);
    P_mx[0] = MKP(att_mix_kv32_spv, 3, 32, 0); P_mx[1] = MKP(att_mix_kv16_spv, 3, 32, 0);
    if (!only_time) {
        if (sel("rms")) { t_rms(); }
        if (sel("rmsh")) t_rmsh();
        if (sel("gather")) t_gather();
        if (sel("moe")) t_moe();
        if (sel("qkv")) t_qkv();
        if (sel("att1f")) t_att1f();
        if (!g_only || !strcmp(g_only, "att1")) t_att1();
        if (sel("att_") || sel("split")) t_attsplit();
        printf("\n== Resumo ==\n");
        int tp = 0, tt = 0;
        for (int i = 0; i < nks; i++) {
            printf("  %-12s %4d/%-4d %s   pior abs %.2e  pior rel %.2e\n", ks[i].name, ks[i].pass, ks[i].total, ks[i].pass == ks[i].total ? "PASS" : "FAIL", ks[i].wabs, ks[i].wrel);
            tp += ks[i].pass; tt += ks[i].total;
        }
        printf("  TOTAL %d/%d casos %s\n", tp, tt, tp == tt ? "PASS" : "FAIL");
        if (tp != tt) { kvk_shutdown(); return 1; }
    }
    if (!g_only || only_time) timing();
    kvk_shutdown();
    return 0;
}
