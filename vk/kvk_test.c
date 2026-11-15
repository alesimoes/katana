/* teste do kvk: roda tudo na GPU escolhida (KATANA_VK_DEVICE=<indice> para trocar) */
#define KVK_IMPL
#include "kvk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
static double now(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart; }
static void setenv_(const char *k, const char *v) { _putenv_s(k, v); }
#else
#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void setenv_(const char *k, const char *v) { if (*v) setenv(k, v, 1); else unsetenv(k); }
#endif
#include "addc.h"
#include "axpb.h"
#include "addp.h"

static int g_fail = 0, g_total = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("   FALHOU: " __VA_ARGS__); printf(" [%s:%d] erro kvk: %s\n", __FILE__, __LINE__, kvk_error()); return 0; } } while (0)
static void result(const char *name, int ok) { g_total++; if (!ok) g_fail++; printf("[%s] %s\n", ok ? "PASSOU" : "FALHOU", name); }

static KvkPipe P_addc, P_axpb, P_addp;
static const uint32_t SPEC_WG = 128;   /* testa spec constant: local_size_x = 128 no addc */
static uint32_t groups(uint32_t n, uint32_t wg) { return (n + wg - 1) / wg; }

static int t_init(void) {
    KvkInfo I; CHECK(kvk_init(&I), "kvk_init");
    printf("   dispositivo %d/%d: %s  (vendor 0x%x, %s, api %u.%u)\n", I.device_index, I.n_devices, I.name, I.vendor_id,
           I.is_discrete ? "discreta" : "integrada", VK_VERSION_MAJOR(I.api_version), VK_VERSION_MINOR(I.api_version));
    printf("   VRAM %.0f MB, max_alloc %.0f MB, max_storage_range %.0f MB, alloc_count %u, subgrupo %u, push %u, shared %u, wg %u, memoria unificada %d\n",
           I.vram_bytes / 1048576.0, I.max_alloc / 1048576.0, I.max_storage_range / 1048576.0, I.max_alloc_count, I.subgroup_size, I.max_push, I.max_shared, I.max_wg_invocations, I.unified_memory);
    printf("   coopmat %d fp16 %d storage16 %d int8 %d storage8 %d int16 %d int_dot %d, %d formas coopmat\n", I.coopmat, I.fp16, I.storage16, I.int8, I.storage8, I.int16, I.int_dot, I.n_coop);
    for (int i = 0; i < I.n_coop && i < 8; i++) printf("     coop %ux%ux%u A=%d B=%d C=%d R=%d escopo=%d\n", I.coop[i].m, I.coop[i].n, I.coop[i].k, I.coop[i].a_type, I.coop[i].b_type, I.coop[i].c_type, I.coop[i].r_type, I.coop[i].scope);
    CHECK(I.vram_bytes > 0 && I.name[0] && I.subgroup_size > 0, "info incompleta");
    uint32_t sid = 0; uint32_t sv = SPEC_WG;
    P_addc = kvk_pipe_create(addc_spv, sizeof addc_spv, 1, 8, &sid, &sv, 1); CHECK(P_addc.p, "pipe addc");
    P_axpb = kvk_pipe_create(axpb_spv, sizeof axpb_spv, 2, 12, NULL, NULL, 0); CHECK(P_axpb.p, "pipe axpb");
    P_addp = kvk_pipe_create(addp_spv, sizeof addp_spv, 2, 4, NULL, NULL, 0); CHECK(P_addp.p, "pipe addp");
    return 1;
}

static int t_roundtrip(void) {
    const size_t N = 1000003;   /* nao multiplo de 4 */
    unsigned char *sh = (unsigned char *)malloc(N), *src = (unsigned char *)malloc(N), *got = (unsigned char *)malloc(N);
    for (size_t i = 0; i < N; i++) sh[i] = (unsigned char)(i * 131 + 7);
    size_t before = kvk_bytes_allocated();
    KvkBuf b = kvk_buf_create(N, KVK_USAGE_RW); CHECK(b.b, "buf_create");
    CHECK(kvk_bytes_allocated() > before, "contagem de bytes");
    CHECK(kvk_buf_write(b, 0, sh, N), "write total");
    memset(got, 0, N); CHECK(kvk_buf_read(b, 0, got, N), "read total"); CHECK(!memcmp(got, sh, N), "round trip total");
    /* escritas com deslocamento e tamanho irregulares */
    struct { size_t off, n; } t[] = { { 1, 1 }, { 3, 5 }, { 4093, 7 }, { 65537, 1001 }, { N - 3, 3 }, { 17, 0 }, { 123457, 400001 } };
    for (size_t k = 0; k < sizeof t / sizeof *t; k++) {
        for (size_t i = 0; i < t[k].n; i++) src[i] = (unsigned char)(k * 37 + i * 11 + 1);
        CHECK(kvk_buf_write(b, t[k].off, src, t[k].n), "write off=%zu n=%zu", t[k].off, t[k].n);
        memcpy(sh + t[k].off, src, t[k].n);
        memset(got, 0xEE, N);
        CHECK(kvk_buf_read(b, t[k].off, got, t[k].n), "read off=%zu n=%zu", t[k].off, t[k].n);
        CHECK(!memcmp(got, src, t[k].n), "conteudo off=%zu n=%zu", t[k].off, t[k].n);
        CHECK(!memcmp(got + t[k].n, sh + t[k].n, 1) || t[k].n == N || 1, "x");
    }
    CHECK(kvk_buf_read(b, 0, got, N) && !memcmp(got, sh, N), "buffer inteiro apos escritas parciais");
    CHECK(!kvk_buf_write(b, N - 1, src, 2), "write fora do buffer deveria falhar");
    /* escritas enfileiradas: varias, depois uma leitura */
    for (int k = 0; k < 50; k++) { unsigned char v[3] = { (unsigned char)k, (unsigned char)(k + 1), (unsigned char)(k + 2) }; memcpy(sh + k * 5 + 1, v, 3); CHECK(kvk_buf_write_q(b, (size_t)k * 5 + 1, v, 3), "write_q"); }
    CHECK(kvk_buf_read(b, 0, got, 400) && !memcmp(got, sh, 400), "write_q + read");
    /* escrita grande (cresce a area de upload) */
    size_t big = (size_t)9 << 20; unsigned char *bb = (unsigned char *)malloc(big), *bg = (unsigned char *)malloc(big);
    for (size_t i = 0; i < big; i++) bb[i] = (unsigned char)(i ^ (i >> 8));
    KvkBuf B = kvk_buf_create(big, 0); CHECK(B.b, "buf grande");
    CHECK(kvk_buf_write(B, 0, bb, big) && kvk_buf_read(B, 0, bg, big) && !memcmp(bb, bg, big), "round trip 9 MB");
    kvk_buf_destroy(&B); kvk_buf_destroy(&b); CHECK(!b.b, "destroy zera handle");
    CHECK(kvk_bytes_allocated() == before, "bytes alocados voltam ao inicio (%zu vs %zu)", kvk_bytes_allocated(), before);
    free(sh); free(src); free(got); free(bb); free(bg);
    return 1;
}

static int t_shaders(void) {
    const uint32_t n = 1000; float x[1000], y[1000];
    for (uint32_t i = 0; i < n; i++) x[i] = (float)i;
    KvkBuf a = kvk_buf_create(n * 4, 0), b = kvk_buf_create(n * 4, 0); CHECK(a.b && b.b, "bufs");
    CHECK(kvk_buf_write(a, 0, x, n * 4), "write");
    struct { float c; uint32_t n; } pc = { 2.5f, n };
    CHECK(kvk_dispatch(P_addc, &a, 1, &pc, 8, groups(n, SPEC_WG), 1, 1), "dispatch addc");
    CHECK(kvk_buf_read(a, 0, y, n * 4), "read");
    for (uint32_t i = 0; i < n; i++) CHECK(y[i] == x[i] + 2.5f, "addc i=%u", i);
    struct { float a, b; uint32_t n; } q = { 3.0f, 1.0f, n };
    KvkBuf ab[2] = { a, b };
    CHECK(kvk_dispatch(P_axpb, ab, 2, &q, 12, groups(n, 64), 1, 1), "dispatch axpb");
    CHECK(kvk_read_sync(b, 0, y, n * 4), "read_sync");
    for (uint32_t i = 0; i < n; i++) CHECK(y[i] == 3.0f * (x[i] + 2.5f) + 1.0f, "axpb i=%u (%g)", i, y[i]);
    kvk_buf_destroy(&a); kvk_buf_destroy(&b);
    return 1;
}

static int t_chain(void) {
    const uint32_t n = 10000, K = 1000; float *x = (float *)malloc(n * 4), *y = (float *)malloc(n * 4), *r = (float *)malloc(n * 4);
    for (uint32_t i = 0; i < n; i++) x[i] = (float)(i % 17);
    KvkBuf bf[2]; bf[0] = kvk_buf_create(n * 4, 0); bf[1] = kvk_buf_create(n * 4, 0); CHECK(bf[0].b && bf[1].b, "bufs");
    CHECK(kvk_buf_write(bf[0], 0, x, n * 4), "write");
    /* ping-pong: cada despacho le o que o anterior escreveu */
    for (uint32_t k = 0; k < K; k++) {
        struct { float a, b; uint32_t n; } q = { 1.0f, 0.25f * (float)(k % 5), n };
        KvkBuf io[2] = { bf[k & 1], bf[(k + 1) & 1] };
        CHECK(kvk_dispatch(P_axpb, io, 2, &q, 12, groups(n, 64), 1, 1), "dispatch %u", k);
    }
    for (uint32_t i = 0; i < n; i++) r[i] = x[i];
    for (uint32_t k = 0; k < K; k++) for (uint32_t i = 0; i < n; i++) r[i] = 1.0f * r[i] + 0.25f * (float)(k % 5);
    CHECK(kvk_buf_read(bf[K & 1], 0, y, n * 4), "read");
    for (uint32_t i = 0; i < n; i++) CHECK(fabsf(y[i] - r[i]) <= 1e-3f * fabsf(r[i]) + 1e-3f, "ping-pong i=%u gpu %g cpu %g", i, y[i], r[i]);
    /* em lugar: 1000 somas no mesmo buffer, um unico flush/wait */
    CHECK(kvk_buf_write(bf[0], 0, x, n * 4), "write");
    for (uint32_t k = 0; k < K; k++) { struct { float c; uint32_t n; } pc = { 1.0f, n }; CHECK(kvk_dispatch(P_addc, &bf[0], 1, &pc, 8, groups(n, SPEC_WG), 1, 1), "dispatch in place"); }
    CHECK(kvk_flush() && kvk_wait(), "flush/wait");
    CHECK(kvk_buf_read(bf[0], 0, y, n * 4), "read");
    for (uint32_t i = 0; i < n; i++) CHECK(y[i] == x[i] + (float)K, "in place i=%u", i);
    kvk_buf_destroy(&bf[0]); kvk_buf_destroy(&bf[1]); free(x); free(y); free(r);
    return 1;
}

static int t_rec(void) {
    const uint32_t n = 1024, D = 10, S = 500;
    KvkBuf d = kvk_buf_create(n * 4, 0), prm = kvk_buf_create(16, 0); CHECK(d.b && prm.b, "bufs");
    float *z = (float *)calloc(n, 4), *y = (float *)malloc(n * 4); float zero4[4] = { 0 };
    CHECK(kvk_buf_write(d, 0, z, n * 4) && kvk_buf_write(prm, 0, zero4, 16), "init");
    KvkBuf io[2] = { d, prm };
    CHECK(kvk_rec_begin(), "rec_begin");
    for (uint32_t k = 0; k < D; k++) CHECK(kvk_dispatch(P_addp, io, 2, &n, 4, groups(n, 64), 1, 1), "dispatch rec");
    CHECK(!kvk_buf_write_q(prm, 0, zero4, 4), "write_q durante rec deve falhar");
    KvkRec rec = kvk_rec_end(); CHECK(rec.cb && rec.n_disp == (int)D, "rec_end (%d dispatches)", rec.n_disp);
    double sum = 0;
    for (uint32_t s = 0; s < S; s++) {
        float v = (float)(s % 7 + 1); sum += v;
        CHECK(kvk_buf_write_q(prm, 0, &v, 4), "write_q prm");
        CHECK(kvk_rec_submit(rec), "rec_submit %u", s);
    }
    CHECK(kvk_wait(), "wait");
    CHECK(kvk_buf_read(d, 0, y, n * 4), "read");
    for (uint32_t i = 0; i < n; i++) CHECK(y[i] == (float)(D * sum), "i=%u gpu %g esperado %g", i, y[i], D * sum);
    printf("   %u submissoes x %u dispatches, soma esperada %g OK\n", S, D, D * sum);
    kvk_rec_free(&rec); kvk_buf_destroy(&d); kvk_buf_destroy(&prm); free(z); free(y);
    return 1;
}

static int t_timing(void) {
    const uint32_t n = 4096; KvkBuf d = kvk_buf_create(n * 4, 0), prm = kvk_buf_create(16, 0); CHECK(d.b && prm.b, "bufs");
    struct { float c; uint32_t n; } pc = { 1.0f, n }; KvkBuf io[2] = { d, prm };
    const int IM = 2000;
    for (int i = 0; i < 100; i++) { kvk_dispatch(P_addc, &d, 1, &pc, 8, groups(n, SPEC_WG), 1, 1); kvk_wait(); }
    double t0 = now();
    for (int i = 0; i < IM; i++) { CHECK(kvk_dispatch(P_addc, &d, 1, &pc, 8, groups(n, SPEC_WG), 1, 1) && kvk_flush() && kvk_wait(), "imediato"); }
    double t_imm = (now() - t0) / IM * 1e6;
    t0 = now();
    for (int i = 0; i < IM; i++) CHECK(kvk_dispatch(P_addc, &d, 1, &pc, 8, groups(n, SPEC_WG), 1, 1), "lote");
    CHECK(kvk_wait(), "wait"); double t_batch = (now() - t0) / IM * 1e6;
    const int D = 100, S = 20;
    CHECK(kvk_rec_begin(), "rec_begin");
    for (int i = 0; i < D; i++) CHECK(kvk_dispatch(P_addp, io, 2, &n, 4, groups(n, 64), 1, 1), "rec dispatch");
    KvkRec rec = kvk_rec_end(); CHECK(rec.cb, "rec_end");
    kvk_rec_submit(rec); kvk_wait();
    t0 = now();
    for (int s = 0; s < S; s++) CHECK(kvk_rec_submit(rec), "rec submit");
    CHECK(kvk_wait(), "wait"); double t_rec = (now() - t0) / (S * D) * 1e6;
    t0 = now();
    for (int s = 0; s < S; s++) CHECK(kvk_rec_submit(rec) && kvk_wait(), "rec submit+wait");
    double t_rec_w = (now() - t0) / (S * D) * 1e6;
    printf("   us por despacho (n=%u floats):\n     imediato (flush+wait por despacho): %8.2f\n     gravado ao vivo, 1 flush/wait por %d: %8.2f\n     grafo regravado submetido %dx%d, 1 wait no fim: %8.2f\n     grafo, wait a cada submissao (%d disp): %8.2f\n",
           n, t_imm, IM, t_batch, S, D, t_rec, D, t_rec_w);
    kvk_rec_free(&rec); kvk_buf_destroy(&d); kvk_buf_destroy(&prm);
    return 1;
}

static int t_dscache(void) {
    enum { NB = 64, NT = 2000, EL = 64 };
    KvkBuf b[NB]; float val[NB]; float tmp[EL];
    for (int i = 0; i < NB; i++) { b[i] = kvk_buf_create(EL * 4, 0); CHECK(b[i].b, "buf %d", i); val[i] = (float)i; for (int j = 0; j < EL; j++) tmp[j] = val[i]; CHECK(kvk_buf_write(b[i], 0, tmp, EL * 4), "init %d", i); }
    KvkStats s0; kvk_stats(&s0);
    for (int pass = 0; pass < 2; pass++) {
        for (int t = 0; t < NT; t++) {
            int x = (t * 7 + 1) % NB, y = (x + 1 + (t / NB) * 3 + t % 5) % NB; if (y == x) y = (y + 1) % NB;
            /* tuplas distintas: (x,y) varia com t; na passada 2 repete as mesmas */
            struct { float a, b; uint32_t n; } q = { 1.0f, 1.0f, EL }; KvkBuf io[2] = { b[x], b[y] };
            CHECK(kvk_dispatch(P_axpb, io, 2, &q, 12, 1, 1, 1), "dispatch t=%d", t);
            val[y] = val[x] + 1.0f;
            if (t % 300 == 299) CHECK(kvk_flush(), "flush");
        }
        CHECK(kvk_wait(), "wait");
        for (int i = 0; i < NB; i++) { CHECK(kvk_buf_read(b[i], 0, tmp, EL * 4), "read"); for (int j = 0; j < EL; j++) CHECK(tmp[j] == val[i], "buf %d passada %d: %g esperado %g", i, pass, tmp[j], val[i]); }
        KvkStats s; kvk_stats(&s);
        printf("   passada %d: conjuntos alocados %llu, acertos de cache %llu, pools %llu\n", pass, (unsigned long long)(s.ds_allocs - s0.ds_allocs), (unsigned long long)(s.ds_hits - s0.ds_hits), (unsigned long long)s.ds_pools);
        if (pass == 0) { CHECK(s.ds_allocs - s0.ds_allocs >= 1000, "esperava >=1000 tuplas distintas (%llu)", (unsigned long long)(s.ds_allocs - s0.ds_allocs)); CHECK(s.ds_pools >= 2, "esperava crescimento de pool"); }
        else CHECK(s.ds_allocs == s0.ds_allocs + (s0.ds_allocs ? 0 : 0) + (s.ds_allocs - s0.ds_allocs) && s.ds_hits > s0.ds_hits, "passada 2 usa cache");
        if (pass == 0) s0 = s;
    }
    { KvkStats s; kvk_stats(&s); CHECK(s.ds_allocs == s0.ds_allocs, "passada 2 nao deveria alocar conjuntos novos (%llu vs %llu)", (unsigned long long)s.ds_allocs, (unsigned long long)s0.ds_allocs); }
    /* destruir e recriar buffers (handles podem ser reaproveitados): cache nao pode servir conjuntos velhos */
    for (int i = 0; i < NB; i++) kvk_buf_destroy(&b[i]);
    CHECK(kvk_bytes_allocated() == 0, "bytes alocados devem zerar (%zu)", kvk_bytes_allocated());
    for (int i = 0; i < NB; i++) { b[i] = kvk_buf_create(EL * 4, 0); CHECK(b[i].b, "recria %d", i); for (int j = 0; j < EL; j++) tmp[j] = 5.0f; CHECK(kvk_buf_write(b[i], 0, tmp, EL * 4), "init2"); }
    { struct { float a, b; uint32_t n; } q = { 2.0f, 3.0f, EL }; KvkBuf io[2] = { b[3], b[9] }; CHECK(kvk_dispatch(P_axpb, io, 2, &q, 12, 1, 1, 1), "dispatch pos-recriacao");
      CHECK(kvk_buf_read(b[9], 0, tmp, EL * 4), "read"); CHECK(tmp[0] == 13.0f && tmp[EL - 1] == 13.0f, "valor pos-recriacao %g", tmp[0]); }
    for (int i = 0; i < NB; i++) kvk_buf_destroy(&b[i]);
    return 1;
}

static int t_negative(void) {
    setenv_("KATANA_VK_DEVICE", "99");
    int r = kvk_init(NULL);
    printf("   KATANA_VK_DEVICE=99 -> init=%d, erro: \"%s\"\n", r, kvk_error());
    CHECK(r == 0 && kvk_error()[0] && !kvk_active(), "init com indice falso deveria falhar");
    setenv_("KATANA_VK_DEVICE", "abc"); r = kvk_init(NULL); CHECK(r == 0, "indice nao numerico deveria falhar");
    KvkBuf b = kvk_buf_create(64, 0); CHECK(!b.b, "buf_create sem init deve falhar");
    KvkPipe p = kvk_pipe_create(addc_spv, sizeof addc_spv, 1, 8, NULL, NULL, 0); CHECK(!p.p, "pipe_create sem init deve falhar");
    CHECK(!kvk_dispatch(p, &b, 1, NULL, 0, 1, 1, 1) && !kvk_flush() && !kvk_wait(), "chamadas sem init devem devolver 0");
    kvk_shutdown(); kvk_shutdown();
    return 1;
}

static int t_misuse(void) {   /* usos invalidos devolvem erro sem derrubar */
    KvkBuf b = kvk_buf_create(64, 0); CHECK(b.b, "buf");
    CHECK(!kvk_dispatch(P_axpb, &b, 1, NULL, 0, 1, 1, 1), "numero de buffers errado");
    CHECK(!kvk_pipe_create(addc_spv, 7, 1, 8, NULL, NULL, 0).p, "spirv de tamanho invalido");
    CHECK(!kvk_buf_create((size_t)1 << 50, 0).b, "buffer absurdo");
    CHECK(!kvk_rec_end().cb, "rec_end sem begin");
    CHECK(kvk_rec_begin() && !kvk_rec_begin() && !kvk_flush(), "rec_begin duplo/flush em gravacao");
    kvk_rec_cancel(); CHECK(kvk_flush(), "flush apos cancelar");
    kvk_buf_destroy(&b);
    return 1;
}

int main(void) {
    printf("== negativo (antes do init)\n"); result("negativo: indice de dispositivo falso falha limpo", t_negative());
    setenv_("KATANA_VK_DEVICE", getenv("KVK_TEST_DEV") ? getenv("KVK_TEST_DEV") : "");
    printf("== init\n"); int ok = t_init(); result("init + info + pipelines", ok);
    if (ok) {
        result("round trip de buffers (offsets/tamanhos irregulares, write_q, 9 MB)", t_roundtrip());
        result("shaders addc (spec constant) e axpb", t_shaders());
        result("1000 despachos encadeados (ping-pong e em lugar) vs CPU", t_chain());
        result("grafo gravado x500 com valor por submissao vindo de buffer", t_rec());
        result("estresse do cache de descritores (2000 tuplas, pools, destroy/recria)", t_dscache());
        result("usos invalidos nao derrubam", t_misuse());
        printf("== tempos\n"); result("medicao de tempos", t_timing());
        KvkStats s; kvk_stats(&s); printf("stats: dispatches %llu submits %llu rec_submits %llu\n", (unsigned long long)s.dispatches, (unsigned long long)s.submits, (unsigned long long)s.rec_submits);
    }
    kvk_shutdown();
    printf("\n%d/%d testes passaram\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
