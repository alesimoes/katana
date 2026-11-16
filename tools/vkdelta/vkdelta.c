/* vkdelta: bancada dos kernels Vulkan da camada linear "Gated DeltaNet" (qwen35moe), conferidos contra uma
   referencia em double (recorrencia token a token) escrita aqui, e medidos.
   Kernels: dn_step (geracao, 1 lancamento por camada), dn_prep + dn_cstate + dn_chunk + dn_state + dn_norm (lote,
   forma em blocos de 64 tokens).
   Uso: vkdelta [-B lote] [-r repeticoes] [-k 0|1 (mapa q/k -> cabeca)] [-t (so tempos)] [-c (so conferencia)] */
#define KVK_IMPL
#include "../../vk/kvk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include "dn_step.h"
#include "dn_prep.h"
#include "dn_cstate.h"
#include "dn_chunk.h"
#include "dn_state.h"
#include "dn_norm.h"

#define D 128
#define NK 16
#define NV 32
#define CH (2 * NK * D + NV * D)   /* 8192 canais da convolucao */
#define DZ (NV * D)                /* 4096 */
#define CB 64                      /* tokens por bloco */

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static uint64_t rs = 88172645463325252ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static float rndsym(void) { return 2.0f * (float)(rnd() >> 40) / 16777216.0f - 1.0f; }
static void *xm(size_t n) { void *p = calloc(n ? n : 1, 1); if (!p) { printf("sem memoria\n"); exit(1); } return p; }
static float h2f(uint16_t h) {
    uint32_t s = (h >> 15) & 1, m = h & 0x3ff, u; int e = (h >> 10) & 0x1f;
    if (e == 0) { if (!m) u = s << 31; else { e = 1; while (!(m & 0x400)) { m <<= 1; e--; } m &= 0x3ff; u = (s << 31) | ((uint32_t)(e + 112) << 23) | (m << 13); } }
    else u = (s << 31) | ((uint32_t)(e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &u, 4); return f;
}
static int g_kmap = 0;
static const float EPS_L2 = 1e-6f, EPS_RMS = 1e-6f;

/* ---------- parametros da camada ---------- */
static float *cw, *pr;   /* cw[c*4 + tap] (tap 3 = token atual); pr = ssm_a | dt_bias | peso da norma */

/* ---------- referencia (double) ---------- */
typedef struct { double *S; double ring[4 * CH]; } Ref;
static int grp(int h) { return g_kmap == 0 ? h / (NV / NK) : h % NK; }
static double silu(double x) { return x / (1.0 + exp(-x)); }
/* um token: x = qkv cru (8192), z (4096), a, b (NV); devolve o cru (antes da norma) e y (saida com portao) */
static void ref_token(Ref *R, int pos, const float *x, const float *z, const float *a, const float *b, double *oraw, double *y) {
    static double cv[CH];
    for (int c = 0; c < CH; c++) {
        double acc = (double)cw[c * 4 + 3] * x[c];
        for (int tau = 1; tau <= 3; tau++) acc += (double)cw[c * 4 + 3 - tau] * R->ring[((pos - tau) & 3) * CH + c];
        cv[c] = silu(acc);
    }
    for (int c = 0; c < CH; c++) R->ring[(pos & 3) * CH + c] = x[c];
    for (int u = 0; u < 2 * NK; u++) {
        double ss = 0; for (int i = 0; i < D; i++) ss += cv[u * D + i] * cv[u * D + i];
        double inv = 1.0 / sqrt(ss + EPS_L2) * (u < NK ? 1.0 / sqrt((double)D) : 1.0);
        for (int i = 0; i < D; i++) cv[u * D + i] *= inv;
    }
    for (int h = 0; h < NV; h++) {
        const double *q = cv + grp(h) * D, *k = cv + NK * D + grp(h) * D, *v = cv + 2 * NK * D + h * D;
        double av = (double)a[h] + pr[NV + h];
        double sp = av > 20 ? av : log1p(exp(av));
        double beta = 1.0 / (1.0 + exp(-(double)b[h])), eg = exp((double)pr[h] * sp);
        double *S = R->S + (size_t)h * D * D;
        double o[D];
        for (int i = 0; i < D * D; i++) S[i] *= eg;
        for (int j = 0; j < D; j++) {
            double sk = 0; for (int i = 0; i < D; i++) sk += S[i * D + j] * k[i];
            double d = (v[j] - sk) * beta;
            for (int i = 0; i < D; i++) S[i * D + j] += k[i] * d;
            double so = 0; for (int i = 0; i < D; i++) so += S[i * D + j] * q[i];
            o[j] = so;
        }
        double ms = 0; for (int j = 0; j < D; j++) ms += o[j] * o[j];
        double inv = 1.0 / sqrt(ms / D + EPS_RMS);
        for (int j = 0; j < D; j++) {
            oraw[h * D + j] = o[j];
            y[h * D + j] = o[j] * inv * pr[2 * NV + j] * silu(z[h * D + j]);
        }
    }
}

/* ---------- erro ---------- */
typedef struct { double mabs, mref; long n; int nan; } Err;
static void eadd(Err *e, double got, double ref) {
    if (!(got == got)) e->nan++;
    double d = fabs(got - ref); if (d > e->mabs) e->mabs = d;
    if (fabs(ref) > e->mref) e->mref = fabs(ref);
    e->n++;
}
static void eprint(const char *nome, Err *e) {
    printf("  %-34s max|erro| %.3e  max|ref| %.3e  relativo ao maximo %.2e%s\n", nome, e->mabs, e->mref,
           e->mabs / (e->mref > 0 ? e->mref : 1), e->nan ? "  NaN!" : "");
}

/* ---------- GPU ---------- */
static KvkPipe p_step, p_prep, p_cst, p_chunk, p_state1, p_state2, p_norm;
static KvkBuf b_cw, b_pr, b_ring, b_st;
typedef struct {    /* buffers do lote */
    int B, Bp, nch;
    KvkBuf qkv, ab, z, qh, kh, vh, bg, ub, wb, qm, qg, kd, gl, o, y;
} Lote;
static KvkBuf mk(size_t n) { KvkBuf b = kvk_buf_create(n, KVK_USAGE_RW); if (!b.b) { printf("buffer: %s\n", kvk_error()); exit(1); } return b; }
static void lote_init(Lote *L, int B) {
    L->B = B; L->Bp = (B + CB - 1) / CB * CB; L->nch = L->Bp / CB;
    size_t Bp = L->Bp;
    L->qkv = mk((size_t)CH * B * 4); L->ab = mk((size_t)2 * NV * B * 4); L->z = mk((size_t)DZ * B * 4);
    L->qh = mk(NK * Bp * D * 2); L->kh = mk(NK * Bp * D * 2); L->vh = mk(NV * Bp * D * 2); L->bg = mk(2 * NV * Bp * 4);
    L->ub = mk(NV * Bp * D * 4); L->wb = mk(NV * Bp * D * 2); L->qm = mk((size_t)NV * L->nch * CB * CB * 2);
    L->qg = mk(NV * Bp * D * 2); L->kd = mk(NV * Bp * D * 2); L->gl = mk(NV * L->nch * 4);
    L->o = mk(NV * Bp * D * 4); L->y = mk((size_t)DZ * B * 2);
}
static void lote_free(Lote *L) {
    KvkBuf *v[] = { &L->qkv, &L->ab, &L->z, &L->qh, &L->kh, &L->vh, &L->bg, &L->ub, &L->wb, &L->qm, &L->qg, &L->kd, &L->gl, &L->o, &L->y };
    for (unsigned i = 0; i < sizeof v / sizeof *v; i++) kvk_buf_destroy(v[i]);
}
/* despachos de um lote; parte: mascara 1 prep, 2 cstate, 4 chunk, 8 state, 16 norm */
static void lote_disp(Lote *L, int pos0, int nc, int parte) {
    if (parte & 1) {
        struct { int pos0, B, Bp; float eps; } pc = { pos0, L->B, L->Bp, EPS_L2 };
        KvkBuf io[] = { L->qkv, L->ab, b_cw, b_pr, b_ring, L->qh, L->kh, L->vh, L->bg };
        kvk_dispatch(p_prep, io, 9, &pc, sizeof pc, 2 * NK + NV, L->Bp / 32, 1);
    }
    if (parte & 2) {
        struct { int pos0, B, ch; } pc = { pos0, L->B, CH };
        KvkBuf io[] = { L->qkv, b_ring };
        kvk_dispatch(p_cst, io, 2, &pc, sizeof pc, CH / 256, 1, 1);
    }
    if (parte & 4) {
        struct { int Bp, kmap; } pc = { L->Bp, g_kmap };
        KvkBuf io[] = { L->qh, L->kh, L->vh, L->bg, L->ub, L->wb, L->qm, L->qg, L->kd, L->gl };
        kvk_dispatch(p_chunk, io, 10, &pc, sizeof pc, L->nch, NV, 1);
    }
    if (parte & 8) {
        struct { int Bp, nch; } pc = { L->Bp, L->nch };
        KvkBuf io[] = { L->ub, L->wb, L->qm, L->qg, L->kd, L->gl, b_st, L->o };
        kvk_dispatch(nc == 2 ? p_state2 : p_state1, io, 8, &pc, sizeof pc, NV, D / (16 * nc), 1);
    }
    if (parte & 16) {
        struct { int B, Bp; float eps; } pc = { L->B, L->Bp, EPS_RMS };
        KvkBuf io[] = { L->o, L->z, b_pr, L->y };
        kvk_dispatch(p_norm, io, 4, &pc, sizeof pc, NV, (L->B + 31) / 32, 1);
    }
}
/* sobe as entradas token-major (x[t][CH], z[t][DZ], a[t][NV], b[t][NV]) transpostas */
static void lote_up(Lote *L, const float *x, const float *z, const float *a, const float *b) {
    int B = L->B;
    float *tx = xm((size_t)CH * B * 4), *tz = xm((size_t)DZ * B * 4), *tab = xm((size_t)2 * NV * B * 4);
    for (int t = 0; t < B; t++) {
        for (int c = 0; c < CH; c++) tx[(size_t)c * B + t] = x[(size_t)t * CH + c];
        for (int c = 0; c < DZ; c++) tz[(size_t)c * B + t] = z[(size_t)t * DZ + c];
        for (int h = 0; h < NV; h++) { tab[h * B + t] = a[t * NV + h]; tab[(NV + h) * B + t] = b[t * NV + h]; }
    }
    kvk_buf_write(L->qkv, 0, tx, (size_t)CH * B * 4); kvk_buf_write(L->z, 0, tz, (size_t)DZ * B * 4);
    kvk_buf_write(L->ab, 0, tab, (size_t)2 * NV * B * 4);
    free(tx); free(tz); free(tab);
}
static KvkBuf s_x, s_z, s_ab, s_o;
static void step_disp(int pos) {
    struct { int pos, kmap; float e1, e2; } pc = { pos, g_kmap, EPS_L2, EPS_RMS };
    KvkBuf io[] = { s_x, s_z, s_ab, b_cw, b_pr, b_ring, b_st, s_o };
    kvk_dispatch(p_step, io, 8, &pc, sizeof pc, NV, 1, 1);
}
static void cmp_state(Ref *R, const char *nome) {
    float *s = xm((size_t)NV * D * D * 4);
    kvk_buf_read(b_st, 0, s, (size_t)NV * D * D * 4);
    Err e = { 0 }; for (size_t i = 0; i < (size_t)NV * D * D; i++) eadd(&e, s[i], R->S[i]);
    eprint(nome, &e); free(s);
}
static void gen_tokens(int n, float **x, float **z, float **a, float **b) {
    *x = xm((size_t)n * CH * 4); *z = xm((size_t)n * DZ * 4); *a = xm((size_t)n * NV * 4); *b = xm((size_t)n * NV * 4);
    for (size_t i = 0; i < (size_t)n * CH; i++) (*x)[i] = 1.5f * rndsym();
    for (size_t i = 0; i < (size_t)n * DZ; i++) (*z)[i] = 2.0f * rndsym();
    for (int i = 0; i < n * NV; i++) { (*a)[i] = 2.0f * rndsym(); (*b)[i] = 2.0f * rndsym(); }
}

/* lote pela GPU contra a referencia; devolve a nova posicao */
static int confere_lote(Ref *R, int pos0, int B, int nc) {
    float *x, *z, *a, *b; gen_tokens(B, &x, &z, &a, &b);
    Lote L; lote_init(&L, B); lote_up(&L, x, z, a, b);
    lote_disp(&L, pos0, nc, 31);
    float *o = xm((size_t)NV * L.Bp * D * 4); uint16_t *y = xm((size_t)DZ * B * 2);
    kvk_buf_read(L.o, 0, o, (size_t)NV * L.Bp * D * 4);
    kvk_buf_read(L.y, 0, y, (size_t)DZ * B * 2);
    double *orf = xm(DZ * 8), *yrf = xm(DZ * 8);
    Err eo = { 0 }, ey = { 0 };
    for (int t = 0; t < B; t++) {
        ref_token(R, pos0 + t, x + (size_t)t * CH, z + (size_t)t * DZ, a + t * NV, b + t * NV, orf, yrf);
        for (int h = 0; h < NV; h++) for (int j = 0; j < D; j++) {
            eadd(&eo, o[((size_t)h * D + j) * L.Bp + t], orf[h * D + j]);
            eadd(&ey, h2f(y[(size_t)(h * D + j) * B + t]), yrf[h * D + j]);
        }
    }
    char nm[96];
    printf(" lote B=%d (pos %d, NC=%d):\n", B, pos0, nc);
    eprint("o (antes da norma)", &eo); eprint("saida com portao (fp16)", &ey);
    snprintf(nm, sizeof nm, "estado S apos o lote"); cmp_state(R, nm);
    lote_free(&L); free(o); free(y); free(orf); free(yrf); free(x); free(z); free(a); free(b);
    return pos0 + B;
}
static int confere_step(Ref *R, int pos0, int n) {
    float *x, *z, *a, *b; gen_tokens(n, &x, &z, &a, &b);
    double orf[DZ], yrf[DZ]; float o[DZ];
    Err ey = { 0 };
    for (int t = 0; t < n; t++) {
        kvk_buf_write(s_x, 0, x + (size_t)t * CH, CH * 4); kvk_buf_write(s_z, 0, z + (size_t)t * DZ, DZ * 4);
        float ab[2 * NV]; memcpy(ab, a + t * NV, NV * 4); memcpy(ab + NV, b + t * NV, NV * 4);
        kvk_buf_write(s_ab, 0, ab, sizeof ab);
        step_disp(pos0 + t);
        kvk_buf_read(s_o, 0, o, sizeof o);
        ref_token(R, pos0 + t, x + (size_t)t * CH, z + (size_t)t * DZ, a + t * NV, b + t * NV, orf, yrf);
        for (int i = 0; i < DZ; i++) eadd(&ey, o[i], yrf[i]);
    }
    printf(" geracao %d tokens (pos %d):\n", n, pos0);
    eprint("saida com portao (fp32)", &ey); cmp_state(R, "estado S");
    free(x); free(z); free(a); free(b);
    return pos0 + n;
}

/* tempo medio por submissao de um grafo gravado (melhor de 'rep' rodadas de 'k' submissoes) */
static double tempo(KvkRec r, int rep, int k) {
    double best = 1e9;
    kvk_rec_submit(r); kvk_wait();
    for (int i = 0; i < rep; i++) {
        double t0 = now();
        for (int j = 0; j < k; j++) kvk_rec_submit(r);
        kvk_wait();
        double t = (now() - t0) / k; if (t < best) best = t;
    }
    return best;
}

int main(int argc, char **argv) {
    int Bt = 256, rep = 5, so_t = 0, so_c = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-B") && i + 1 < argc) Bt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) rep = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) g_kmap = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t")) so_t = 1;
        else if (!strcmp(argv[i], "-c")) so_c = 1;
    }
    KvkInfo I;
    if (!kvk_init(&I)) { printf("vulkan: %s\n", kvk_error()); return 1; }
    printf("GPU %s, subgrupo %u, coopmat %d, compartilhada %u B\n", I.name, I.subgroup_size, I.coopmat, I.max_shared);
    uint32_t sid[3] = { 0, 1, 2 }, sv1[3] = { NK, NV, 1 }, sv2[3] = { NK, NV, 2 };
    p_step = kvk_pipe_create(dn_step_spv, sizeof dn_step_spv, 8, 16, sid, sv1, 2);
    p_prep = kvk_pipe_create(dn_prep_spv, sizeof dn_prep_spv, 9, 16, sid, sv1, 2);
    p_cst = kvk_pipe_create(dn_cstate_spv, sizeof dn_cstate_spv, 2, 12, NULL, NULL, 0);
    p_chunk = kvk_pipe_create(dn_chunk_spv, sizeof dn_chunk_spv, 10, 8, sid, sv1, 2);
    p_state1 = kvk_pipe_create(dn_state_spv, sizeof dn_state_spv, 8, 8, sid, sv1, 3);
    p_state2 = kvk_pipe_create(dn_state_spv, sizeof dn_state_spv, 8, 8, sid, sv2, 3);
    p_norm = kvk_pipe_create(dn_norm_spv, sizeof dn_norm_spv, 4, 12, sid, sv1, 2);
    if (!p_step.p || !p_prep.p || !p_cst.p || !p_chunk.p || !p_state1.p || !p_state2.p || !p_norm.p) { printf("pipeline: %s\n", kvk_error()); return 1; }

    cw = xm(CH * 4 * 4); pr = xm((2 * NV + D) * 4);
    for (int i = 0; i < CH * 4; i++) cw[i] = 0.6f * rndsym();
    for (int h = 0; h < NV; h++) { pr[h] = -expf(1.5f * rndsym() - 0.5f); pr[NV + h] = rndsym(); }
    for (int j = 0; j < D; j++) pr[2 * NV + j] = 1.0f + 0.2f * rndsym();
    b_cw = mk(CH * 4 * 4); b_pr = mk((2 * NV + D) * 4); b_ring = mk(4 * CH * 4); b_st = mk((size_t)NV * D * D * 4);
    kvk_buf_write(b_cw, 0, cw, CH * 4 * 4); kvk_buf_write(b_pr, 0, pr, (2 * NV + D) * 4);
    s_x = mk(CH * 4); s_z = mk(DZ * 4); s_ab = mk(2 * NV * 4); s_o = mk(DZ * 4);
    float *zero = xm((size_t)NV * D * D * 4);

    if (!so_t) {
        printf("conferencia (kmap %d) contra a recorrencia token a token em double:\n", g_kmap);
        for (int nc = 1; nc <= 2; nc++) {
            Ref R; R.S = xm((size_t)NV * D * D * 8); memset(R.ring, 0, sizeof R.ring);
            kvk_buf_write(b_st, 0, zero, (size_t)NV * D * D * 4); kvk_buf_write(b_ring, 0, zero, 4 * CH * 4);
            int pos = 0;
            pos = confere_lote(&R, pos, 200, nc);
            pos = confere_lote(&R, pos, 77, nc);
            if (nc == 1) { pos = confere_lote(&R, pos, 2, nc); pos = confere_step(&R, pos, 8); pos = confere_lote(&R, pos, 256, nc); }
            free(R.S);
        }
        {   /* geracao pura desde o estado zero, 64 tokens */
            Ref R; R.S = xm((size_t)NV * D * D * 8); memset(R.ring, 0, sizeof R.ring);
            kvk_buf_write(b_st, 0, zero, (size_t)NV * D * D * 4); kvk_buf_write(b_ring, 0, zero, 4 * CH * 4);
            confere_step(&R, 0, 64);
            free(R.S);
        }
    }
    if (!so_c) {
        printf("tempos (melhor de %d rodadas; indicativos, GPU compartilhada):\n", rep);
        float *x, *z, *a, *b; gen_tokens(1, &x, &z, &a, &b);
        kvk_buf_write(s_x, 0, x, CH * 4); kvk_buf_write(s_z, 0, z, DZ * 4);
        float ab[2 * NV]; memcpy(ab, a, NV * 4); memcpy(ab + NV, b, NV * 4); kvk_buf_write(s_ab, 0, ab, sizeof ab);
        free(x); free(z); free(a); free(b);
        kvk_buf_write(b_st, 0, zero, (size_t)NV * D * D * 4);
        const int K = 100;
        kvk_rec_begin(); for (int i = 0; i < K; i++) step_disp(i); KvkRec r = kvk_rec_end();
        double ts = tempo(r, rep, 4) / K;
        kvk_rec_free(&r);
        printf("  geracao dn_step: %.4f ms por camada (%d despachos seguidos num grafo; 4 MB de estado lidos+gravados -> %.0f GB/s)\n",
               ts * 1e3, K, 4.0 * NV * D * D / ts / 1e9);
        gen_tokens(Bt, &x, &z, &a, &b);
        Lote L; lote_init(&L, Bt); lote_up(&L, x, z, a, b);
        free(x); free(z); free(a); free(b);
        const char *nomes[] = { "prep", "cstate", "chunk", "state NC=1", "norm" };
        const int partes[] = { 1, 2, 4, 8, 16 };
        const int KB = 20;
        for (int i = 0; i < 5; i++) {
            kvk_rec_begin(); for (int j = 0; j < KB; j++) lote_disp(&L, 0, 1, partes[i]); r = kvk_rec_end();
            printf("  lote %d, %-11s %.4f ms\n", Bt, nomes[i], tempo(r, rep, 2) / KB * 1e3);
            kvk_rec_free(&r);
        }
        kvk_rec_begin(); for (int j = 0; j < KB; j++) lote_disp(&L, 0, 2, 8); r = kvk_rec_end();
        printf("  lote %d, %-11s %.4f ms\n", Bt, "state NC=2", tempo(r, rep, 2) / KB * 1e3);
        kvk_rec_free(&r);
        double tl[2];
        for (int nc = 1; nc <= 2; nc++) {
            kvk_rec_begin(); for (int j = 0; j < KB; j++) lote_disp(&L, 0, nc, 31); r = kvk_rec_end();
            tl[nc - 1] = tempo(r, rep, 2) / KB; kvk_rec_free(&r);
            printf("  lote %d completo (NC=%d): %.4f ms por camada (%.2f us/token)\n", Bt, nc, tl[nc - 1] * 1e3, tl[nc - 1] / Bt * 1e6);
        }
        kvk_rec_begin(); for (int j = 0; j < Bt; j++) step_disp(j); r = kvk_rec_end();
        double tr = tempo(r, rep, 2);
        kvk_rec_free(&r);
        printf("  recorrencia %d vezes (dn_step): %.4f ms  -> forma em blocos %.1fx mais rapida\n", Bt, tr * 1e3, tr / (tl[0] < tl[1] ? tl[0] : tl[1]));
        lote_free(&L);
    }
    kvk_shutdown();
    return 0;
}
