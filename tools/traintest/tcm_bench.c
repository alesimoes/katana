/* Medida das unidades de matriz da placa nos produtos do treino, sem modelo: teto sintetico, tempo so da GPU (marcas de
   tempo) e ponta a ponta por forma, contra o shader fp32 (tmm), e o erro contra a CPU (desquantizacao do motor, fp32).
     sh tools/traintest/gen_tcm.sh      (gera os cabecalhos SPIR-V desta medida)
     gcc -O2 -mavx2 -mfma -mf16c -ffast-math -w -I . -I src -I vk/include tools/traintest/tcm_bench.c -o /tmp/tcm_bench -lm -lpthread -ldl
     /tmp/tcm_bench peak | prod [filtro] | mem | int8 | class [filtro]   (filtro: Q4_K, Q8_0-8192...) */
#include "tools/traintest/tgcm_vk.h"
#define main katana_main
#include "src/katana.c"
#undef main
#include "vk/shaders/tcm16.h"
#include "vk/shaders/tcm64.h"
#include "tools/traintest/tcm_var.h"
#include "tools/traintest/tcm_peak.h"
#include "tools/traintest/tcm_peak8.h"
#include "tools/traintest/tcm_peak2.h"
#include "tools/traintest/tcm8.h"
#define KG kvk__g

static KvkPipe pk(const uint32_t *spv, size_t n, const uint32_t *vals, int nv, int nb, int push, int local) {
    uint32_t ids[12], v[12]; int k = 0;
    for (; k < nv; k++) { ids[k] = (uint32_t)k; v[k] = vals[k]; }
    if (local) { ids[k] = 10; v[k++] = (uint32_t)local; }
    KvkPipe p = kvk_pipe_create(spv, n, nb, push, ids, v, k);
    if (!p.p) printf("  pipeline: %s\n", kvk_error());
    return p;
}

static void do_peak(KvkInfo *I) {
    KvkBuf a = kvk_buf_create((size_t)64 << 20, 0), b = kvk_buf_create((size_t)64 << 20, 0), o = kvk_buf_create((size_t)64 << 20, 0);
    void *z = calloc(64 << 20, 1); kvk_buf_write(a, 0, z, 64 << 20); kvk_buf_write(b, 0, z, 64 << 20);
    for (int c = 0; c < I->n_coop; c++) {
        const KvkCoop *q = &I->coop[c];
        int i8 = 0;
        if (q->scope != 3) continue;
        if (q->a_type == 0 && q->b_type == 0 && q->c_type == 1 && q->r_type == 1) i8 = 0;
        else if (q->a_type == 3 && q->b_type == 3 && q->c_type == 5 && q->r_type == 5) i8 = 1;
        else continue;
        for (int src = 0; src < 3; src++) for (int na = 1; na <= 4; na *= 2) for (int sg = 4; sg <= 8; sg += 4) {
            if (src && (q->m != 16 || q->n != 16 || q->k != 16)) continue;
            if (src == 1 && (sg / 4) * na > 4) continue;
            const uint32_t v[5] = { (uint32_t)src, (uint32_t)na, q->m, q->n, q->k };
            KvkPipe p = pk(i8 ? tcm_peak8_spv : tcm_peak_spv, i8 ? sizeof tcm_peak8_spv : sizeof tcm_peak_spv, v, 5, 3, 4, sg * 32);
            if (!p.p) continue;
            KvkBuf bs[3] = { a, b, o }; uint32_t n = src ? 2048 : 8192, wg = 128;
            double best = 1e9;
            for (int r = 0; r < 6; r++) { bv_t0(); kvk_dispatch(p, bs, 3, &n, 4, src == 2 ? 64 : wg, 1, 1); bv_t1(); kvk_wait(); double t = bv_dt(); if (t < best) best = t; }
            double mac = (double)q->m * q->n * q->k * n * na * sg * (src == 2 ? 64 : wg);
            printf("peak %s %ux%ux%u src %s acc %d subgroups %d: %.2f Tmac/s\n", i8 ? "int8" : "fp16", q->m, q->n, q->k, src == 0 ? "reg   " : src == 1 ? "shared" : "buffer", na, sg, mac / best * 1e-12);
            kvk_pipe_destroy(&p);
        }
    }
    static const int fx[][4] = { { 64, 16, 16, 32 }, { 64, 64, 64, 64 }, { 64, 64, 64, 128 }, { 64, 16, 64, 128 }, { 64, 64, 64, 256 }, { 64, 32, 64, 256 }, { 128, 64, 64, 256 } };
    if (bv_cm2) for (int i = 0; i < 7; i++) for (int src = 0; src < 3; src += 2) {
        const uint32_t v[5] = { (uint32_t)src, 1, (uint32_t)fx[i][0], (uint32_t)fx[i][1], (uint32_t)fx[i][2] };
        KvkPipe p = pk(tcm_peak2_spv, sizeof tcm_peak2_spv, v, 5, 3, 4, fx[i][3]);
        if (!p.p) continue;
        KvkBuf bs[3] = { a, b, o }; uint32_t n = 512;
        double best = 1e9;
        for (int r = 0; r < 6; r++) { bv_t0(); kvk_dispatch(p, bs, 3, &n, 4, 64, 1, 1); bv_t1(); kvk_wait(); double t = bv_dt(); if (t < best) best = t; }
        printf("peak fp16 workgroup scope %dx%dx%d, %d invocations, src %s: %.2f Tmac/s\n", fx[i][0], fx[i][1], fx[i][2], fx[i][3], src ? "buffer" : "reg", (double)fx[i][0] * fx[i][1] * fx[i][2] * n * 64 / best * 1e-12);
        kvk_pipe_destroy(&p);
    }
    kvk_buf_destroy(&a); kvk_buf_destroy(&b); kvk_buf_destroy(&o); free(z);
}

static float frnd(void) { return (float)rand() / RAND_MAX; }
static float gauss(void) { return sqrtf(-2 * logf(frnd() + 1e-9f)) * cosf(6.2831853f * frnd()); }
/* potencia de 2 que leva o maior valor a [2048, 4096) */
static float vnorm(const float *x, int n, _Float16 *h, _Float16 *rest, float ls) {
    float m = 0; for (int i = 0; i < n; i++) m = fmaxf(m, fabsf(x[i]));
    int e = 0; if (m > 0) frexpf(m, &e);
    float s = ldexpf(1.0f, 12 - e);
    for (int i = 0; i < n; i++) {
        float v = x[i] * s;
        if (rest) { union { float f; uint32_t u; } t; t.f = v; t.u &= 0xFFFFE000u; h[i] = (_Float16)t.f; rest[i] = (_Float16)((v - t.f) * ls); }
        else h[i] = (_Float16)v;
    }
    return 1.0f / s;
}
typedef struct { uint32_t M, K, ws, j0, xr; } CPush;

static int only = -1;
static void do_prod(const char *filt) {
    static const struct { uint32_t t; int wt; const char *n; int R, C; } Sh[] = {
        { T_Q4_K, 3, "Q4_K", 512, 2048 }, { T_Q5_K, 4, "Q5_K", 2048, 512 }, { T_Q8_0, 2, "Q8_0", 2048, 2048 }, { T_Q8_0, 2, "Q8_0", 8192, 2048 }, { T_Q6_K, 5, "Q6_K", 2048, 512 }, { T_Q8_0, 2, "Q8_0", 512, 2048 } };
    int nsh = 6;
    KvkPipe tm[2][7];
    const double peak = getenv("PEAK") ? atof(getenv("PEAK")) : 7.9;
    for (int s = 0; s < nsh; s++) {
        const int R = Sh[s].R, C = Sh[s].C, NW = R > 4096 ? 4 : R * C > (1 << 20) ? 16 : 96;
        char nm[64]; snprintf(nm, sizeof nm, "%s-%dx%d", Sh[s].n, R, C);
        if (filt && !strstr(nm, filt)) continue;
        const size_t rb = qbytes(Sh[s].t, (size_t)C);
        uint8_t *big = malloc(NW * (size_t)R * rb);
        float *w = malloc((size_t)C * 4);
        for (int r = 0; r < R; r++) { for (int c = 0; c < C; c++) w[c] = gauss() * 0.02f; enc_block(Sh[s].t, w, big + r * rb, (size_t)C); }
        for (int k = 1; k < NW; k++) memcpy(big + k * (size_t)R * rb, big, R * rb);
        KvkBuf W = kvk_buf_create(NW * (size_t)R * rb, 0); kvk_buf_write(W, 0, big, NW * (size_t)R * rb);
        float *dq = malloc((size_t)R * C * 4); for (int r = 0; r < R; r++) dequant(Sh[s].t, big + r * rb, dq + (size_t)r * C, (size_t)C);
        for (int mode = 0; mode < 2; mode++) {
            { uint32_t v[2] = { (uint32_t)mode, (uint32_t)Sh[s].wt }; tm[mode][0] = pk(tmm_spv, sizeof tmm_spv, v, 2, 3, 20, 0); }
            const int M = mode ? C : R, K = mode ? R : C;
            static const int Bs[] = { 1, 4, 16, 64, 128 };
            for (int bi = 0; bi < 5; bi++) {
                const int B = Bs[bi];
                for (int NJ = 1; NJ <= NW; NJ = NJ == 1 && NW > 1 ? NW : NW + 1) {
                    if (NJ > 1 && B > 16 && NW > 16) continue;      /* muitos especialistas so com poucos tokens */
                    const int V = B * NJ;
                    float *x = malloc((size_t)(V + 64) * K * 4), *ref = calloc((size_t)B * M, 4);
                    _Float16 *h = calloc((size_t)2 * (V + 64) * K, 2); float *us = malloc(V * 4);
                    for (int n = 0; n < V; n++) { float sc = powf(10.f, -8 * frnd()); for (int k = 0; k < K; k++) { float g = gauss(); x[(size_t)n * K + k] = sc * g * (frnd() < 0.02f ? 50 : 1); } }
                    for (int n = 0; n < V; n++) us[n] = vnorm(x + (size_t)n * K, K, h + (size_t)n * K, h + (size_t)(n + V + 64) * K, 2048.f);
                    for (int n = 0; n < B; n++) for (int r = 0; r < R; r++) {
                        if (mode == 0) ref[(size_t)n * M + r] = dot(dq + (size_t)r * C, x + (size_t)n * K, C);
                        else { float a = x[(size_t)n * K + r]; for (int c = 0; c < C; c++) ref[(size_t)n * M + c] += a * dq[(size_t)r * C + c]; }
                    }
                    KvkBuf X = kvk_buf_create((size_t)(V + 64) * K * 4, 0), O = kvk_buf_create((size_t)(V + 64) * M * 4, 0);
                    float *out = malloc((size_t)V * M * 4);
                    const double macs = (double)R * C * V;
                    /* variantes: 0 = tmm fp32; 1.. = matriz cooperativa */
                    for (int var = 0; var < NVR; var++) {
                        int bn = 0, sn = 1, prec = 0, dup = 1, pad = 8;
                        if (var) { bn = Vr[var].bn; sn = Vr[var].sn; prec = Vr[var].prec; dup = Vr[var].dup; pad = Vr[var].pad; }
                        if (bn > 16 && B <= bn / 2) continue;
                        if (only >= 0 && var && var != only) continue;
                        KvkPipe P; memset(&P, 0, sizeof P);
                        uint32_t *ent = malloc(16 * (size_t)(V + NJ)); int ne = 0;
                        KvkBuf Jb; memset(&Jb, 0, sizeof Jb);
                        if (var) {
                            uint32_t v[2] = { (uint32_t)mode, (uint32_t)Sh[s].wt };
                            P = pk(Vr[var].s, Vr[var].n, v, 2, 4, 20, 0);
                            if (!P.p) continue;
                            for (int k = 0; k < NJ; k++) for (int b0 = 0; b0 < B; b0 += bn) {
                                ent[ne * 4] = (uint32_t)(k * (size_t)R * rb); ent[ne * 4 + 1] = ent[ne * 4 + 2] = (uint32_t)(k * B + b0); ent[ne * 4 + 3] = (uint32_t)(B - b0 < bn ? B - b0 : bn); ne++; }
                            Jb = kvk_buf_create((size_t)ne * 16, 0);
                            kvk_buf_write(Jb, 0, ent, (size_t)ne * 16);
                        }
                        double kern = 1e9, e2e = 1e9;
                        for (int rep = 0; rep < 7; rep++) {
                            double t0 = bv_now();
                            if (!var) kvk_buf_write_q(X, 0, x, (size_t)V * K * 4);
                            else {
                                for (int n = 0; n < V; n++) vnorm(x + (size_t)n * K, K, h + (size_t)n * K, prec > 1 ? h + (size_t)(n + V + 64) * K : NULL, 2048.f);
                                kvk_buf_write_q(X, 0, h, (size_t)V * K * 2); if (prec > 1) kvk_buf_write_q(X, (size_t)(V + 64) * K * 2, h + (size_t)(V + 64) * K, (size_t)V * K * 2); }
                            bv_t0();
                            if (!var) {
                                kvk_set_auto_barrier(0);
                                /* o tmm nao tem deslocamento de entrada e saida: todos os trabalhos leem e gravam o mesmo lugar (mesma conta) */
                                for (int k = 0; k < NJ; k++) { struct { uint32_t M, N, K, ws, wo; } pc = { M, B, K, rb, (uint32_t)(k * (size_t)R * rb) }; KvkBuf bb[3] = { W, X, O }; kvk_dispatch(tm[mode][0], bb, 3, &pc, 20, (M + 63) / 64, (B + 63) / 64, 1); }
                                kvk_set_auto_barrier(1);
                            } else { CPush pc = { M, K, rb, 0, V + 64 }; KvkBuf bb[4] = { W, X, O, Jb }; kvk_dispatch(P, bb, 4, &pc, 20, M / 64, ne, 1); }
                            bv_t1();
                            if (!kvk_buf_read(O, 0, out, (size_t)V * M * 4)) { printf("erro %s\n", kvk_error()); exit(1); }
                            if (var) for (int n = 0; n < V; n++) for (int m = 0; m < M; m++) out[(size_t)n * M + m] *= us[n];
                            double t = bv_now() - t0; if (t < e2e) e2e = t;
                            t = bv_dt(); if (t < kern) kern = t;
                        }
                        /* erro por vetor: rms da diferenca / rms da saida; pior vetor, e o maior elemento */
                        double worst = 0, mxe = 0;
                        for (int n = 0; n < B; n++) {
                            double e = 0, r = 0, me = 0; for (int m = 0; m < M; m++) { double d = out[(size_t)n * M + m] - ref[(size_t)n * M + m]; e += d * d; r += (double)ref[(size_t)n * M + m] * ref[(size_t)n * M + m]; me = fmax(me, fabs(d)); }
                            worst = fmax(worst, sqrt(e / r)); mxe = fmax(mxe, me / sqrt(r / M)); }
                        { uint32_t *ub = (uint32_t *)out; long nf = 0; for (size_t q = 0; q < (size_t)V * M; q++) nf += (ub[q] & 0x7f800000u) == 0x7f800000u; if (nf) printf("NONFINITE %ld ", nf); }
                        printf("%-14s mode %d B %3d jobs %2d %s%s bn %2d sn %d prec %d dup %d pad %d: kernel %8.1f us %7.1f Gmac/s (%4.1f%% peak) e2e %8.1f us %6.1f Gmac/s  err rms %.1e max %.1e\n",
                               nm, mode, B, NJ, var ? "tcm" : "tmm", "", bn, sn, prec, dup, pad, kern * 1e6, macs / kern * 1e-9, macs / kern * 1e-12 / peak * 100, e2e * 1e6, macs / e2e * 1e-9, worst, mxe);
                        fflush(stdout);
                        if (var) { kvk_pipe_destroy(&P); kvk_buf_destroy(&Jb); }
                        free(ent);
                    }
                    kvk_buf_destroy(&X); kvk_buf_destroy(&O); free(x); free(ref); free(h); free(us); free(out);
                }
            }
            kvk_pipe_destroy(&tm[mode][0]);
        }
        kvk_buf_destroy(&W); free(big); free(w); free(dq);
    }
}

/* int8 x int8: so Q8_0 na ida, o caso mais favoravel (o peso ja e int8) */
static void do_int8(void) {
    const int R = 8192, C = 2048, B = 128;
    const size_t rb = qbytes(T_Q8_0, C);
    uint8_t *w = malloc(R * rb); float *f = malloc(C * 4), *x = malloc(B * C * 4), *ref = malloc(B * R * 4), *out = malloc(B * R * 4), *us = malloc(B * 4);
    for (int r = 0; r < R; r++) { for (int c = 0; c < C; c++) f[c] = gauss() * 0.02f; enc_block(T_Q8_0, f, w + r * rb, C); }
    for (int heavy = 0; heavy < 2; heavy++) {
        for (int n = 0; n < B * C; n++) x[n] = gauss() * (heavy && frnd() < 0.02f ? 50 : 1);
        for (int r = 0; r < R; r++) { dequant(T_Q8_0, w + r * rb, f, C); for (int n = 0; n < B; n++) ref[(size_t)n * R + r] = dot(f, x + (size_t)n * C, C); }
        KvkBuf W = kvk_buf_create(R * rb, 0), X = kvk_buf_create(2 * (B + 64) * C, 0), O = kvk_buf_create((B + 64) * R * 4, 0), J = kvk_buf_create(32, 0);
        kvk_buf_write(W, 0, w, R * rb);
        uint32_t ent[8] = { 0, 0, 0, 64, 0, 64, 64, 64 }; kvk_buf_write(J, 0, ent, 32);
        for (int parts = 1; parts < 3; parts++) {
            int8_t *q = calloc(2 * (B + 64), C);
            for (int n = 0; n < B; n++) {
                float m = 0; for (int k = 0; k < C; k++) m = fmaxf(m, fabsf(x[n * C + k]));
                const float sc = (parts == 1 ? 127.f : 127.f * 128 - 1) / m; us[n] = 1 / sc;
                for (int k = 0; k < C; k++) {
                    int v = (int)lrintf(x[n * C + k] * sc);
                    if (parts == 1) q[n * C + k] = (int8_t)v;
                    else { int hi = (v + 64) >> 7; if (hi > 127) hi = 127; q[n * C + k] = (int8_t)hi; q[(size_t)(n + B + 64) * C + k] = (int8_t)(v - hi * 128); }
                }
            }
            kvk_buf_write(X, 0, q, 2 * (B + 64) * C);
            uint32_t v[3] = { (uint32_t)parts, 64, 2 }; KvkPipe P = pk(tcm8_spv, sizeof tcm8_spv, v, 3, 4, 20, 256); if (!P.p) return;
            CPush pc = { R, C, (uint32_t)rb, 0, B + 64 }; KvkBuf bb[4] = { W, X, O, J }; double best = 1e9;
            for (int r = 0; r < 8; r++) { bv_t0(); kvk_dispatch(P, bb, 4, &pc, 20, R / 64, 2, 1); bv_t1(); kvk_wait(); double t = bv_dt(); if (t < best) best = t; }
            kvk_buf_read(O, 0, out, (size_t)B * R * 4);
            double worst = 0; for (int n = 0; n < B; n++) { double e = 0, a = 0; for (int r = 0; r < R; r++) { double d = out[(size_t)n * R + r] * us[n] - ref[(size_t)n * R + r]; e += d * d; a += (double)ref[(size_t)n * R + r] * ref[(size_t)n * R + r]; } worst = fmax(worst, sqrt(e / a)); }
            printf("int8 Q8_0 8192x2048 B 128 forward, %s input, parts %d: kernel %.1f us %.1f Gmac/s err rms %.1e\n", heavy ? "heavy-tailed" : "gaussian", parts, best * 1e6, (double)R * C * B / best * 1e-9, worst);
            kvk_pipe_destroy(&P); free(q);
        }
        kvk_buf_destroy(&W); kvk_buf_destroy(&X); kvk_buf_destroy(&O); kvk_buf_destroy(&J);
    }
}

/* ---- memoria: preparo -> VRAM por area intermediaria, direto na VRAM visivel pelo host, fila de transferencia ---- */
typedef struct { char *d; const char *s; size_t n; } CpJ;
static void cp_job(void *c, int a, int b) { CpJ *j = c; memcpy(j->d + ((size_t)a << 20), j->s + ((size_t)a << 20), (size_t)(b - a) << 20); }
static void do_mem(void) {
    const size_t N = (size_t)256 << 20;
    char *src = malloc(N); memset(src, 1, N);
    VkBuffer b; VkDeviceMemory m; void *map = NULL; KvkBuf D = kvk_buf_create(N, 0);
    /* 1. area intermediaria (kvk_buf_write) */
    for (int r = 0; r < 3; r++) { double t = bv_now(); kvk_buf_write(D, 0, src, N); printf("mem: staging kvk_buf_write %.2f GB/s\n", N / (bv_now() - t) * 1e-9); }
    char *rd = malloc(N);
    for (int r = 0; r < 2; r++) { double t = bv_now(); kvk_buf_read(D, 0, rd, N); printf("mem: kvk_buf_read %.2f GB/s\n", N / (bv_now() - t) * 1e-9); }
    /* 2. VRAM visivel pelo host */
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO }; bci.size = N; bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    for (int kind = 0; kind < 2; kind++) {       /* 0: VRAM visivel, 1: host */
        if (KG.CreateBuffer(KG.dev, &bci, NULL, &b)) return;
        VkMemoryRequirements mr; KG.GetBufferMemoryRequirements(KG.dev, b, &mr);
        int mt = kind == 0 ? kvk__memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0)
                           : kvk__memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (mt < 0) { printf("mem: sem tipo\n"); KG.DestroyBuffer(KG.dev, b, NULL); continue; }
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; mai.allocationSize = mr.size; mai.memoryTypeIndex = (uint32_t)mt;
        if (KG.AllocateMemory(KG.dev, &mai, NULL, &m) || KG.BindBufferMemory(KG.dev, b, m, 0) || KG.MapMemory(KG.dev, m, 0, VK_WHOLE_SIZE, 0, &map)) { printf("mem: aloc\n"); return; }
        for (int nt = 0; nt < 2; nt++) for (int r = 0; r < 3; r++) {
            CpJ j = { map, src, N }; double t = bv_now();
            if (nt) pool_for(cp_job, &j, (int)(N >> 20), 8); else memcpy(map, src, N);
            printf("mem: memcpy to %s (type %d) %s %.2f GB/s\n", kind ? "host staging" : "device-local host-visible", mt, nt ? "pool" : "1 thread", N / (bv_now() - t) * 1e-9);
        }
        if (kind == 0) for (int r = 0; r < 2; r++) { double t = bv_now(); memcpy(rd, map, 16 << 20); printf("mem: memcpy READ from device-local host-visible %.3f GB/s\n", (16 << 20) / (bv_now() - t) * 1e-9); }
        /* copia pela fila de computacao e pela de transferencia */
        if (kind == 1) {
            PFN_vkGetDeviceQueue gq; *(void **)&gq = (void *)KG.GetDeviceProcAddr(KG.dev, "vkGetDeviceQueue");
            for (int q = 0; q < 2; q++) {
                if (q && bv_xfer < 0) break;
                VkQueue Q; gq(KG.dev, q ? (uint32_t)bv_xfer : KG.qfam, 0, &Q);
                VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; cpi.queueFamilyIndex = q ? (uint32_t)bv_xfer : KG.qfam; VkCommandPool cp; KG.CreateCommandPool(KG.dev, &cpi, NULL, &cp);
                VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO }; ca.commandPool = cp; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1; VkCommandBuffer cb; KG.AllocateCommandBuffers(KG.dev, &ca, &cb);
                VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; KG.BeginCommandBuffer(cb, &bi);
                VkBufferCopy rg = { 0, 0, N }; KG.CmdCopyBuffer(cb, b, D.b, 1, &rg); KG.EndCommandBuffer(cb);
                VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence f; KG.CreateFence(KG.dev, &fi, NULL, &f);
                for (int r = 0; r < 3; r++) {
                    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
                    double t = bv_now(); KG.QueueSubmit(Q, 1, &si, f); KG.WaitForFences(KG.dev, 1, &f, 1, ~0ull); KG.ResetFences(KG.dev, 1, &f);
                    printf("mem: DMA host -> VRAM, %s queue: %.2f GB/s\n", q ? "transfer" : "compute", N / (bv_now() - t) * 1e-9);
                }
            }
        }
        KG.UnmapMemory(KG.dev, m); KG.DestroyBuffer(KG.dev, b, NULL); KG.FreeMemory(KG.dev, m, NULL);
    }
    KG.DeviceWaitIdle(KG.dev);
    /* custo fixo de submeter e esperar */
    double t = bv_now(); for (int i = 0; i < 200; i++) kvk_buf_write(D, 0, src, 4096);
    printf("mem: write 4 KB + submit + wait %.1f us\n", (bv_now() - t) / 200 * 1e6);
}

/* por classe de peso e tokens por produto: microssegundos por produto na CPU, na placa com o peso residente e com o peso
   subindo na hora, todos pelo codigo do treino (tv_run / tp_mv), lotes de pesos diferentes */
static void do_class(const char *filt) {
    static const struct { uint32_t t; const char *n; int R, C, NW; } Sh[] = {
        { T_Q4_K, "Q4_K", 512, 2048, 96 }, { T_Q5_K, "Q5_K", 2048, 512, 96 }, { T_Q6_K, "Q6_K", 2048, 512, 96 }, { T_Q8_0, "Q8_0", 512, 2048, 16 }, { T_Q8_0, "Q8_0", 4096, 2048, 4 }, { T_Q8_0, "Q8_0", 8192, 2048, 4 }, { T_Q8_0, "Q8_0", 2048, 4096, 4 } };
    if (!tg_init() || !tv_init()) { printf("sem placa\n"); return; }
    tg.min_mac = 0;
    for (int s = 0; s < 7; s++) {
        const int R = Sh[s].R, C = Sh[s].C, NW = Sh[s].NW;
        char nm[64]; snprintf(nm, sizeof nm, "%s-%dx%d", Sh[s].n, R, C);
        if (filt && !strstr(nm, filt)) continue;
        const size_t rb = qbytes(Sh[s].t, (size_t)C);
        uint8_t *big = malloc(NW * (size_t)R * rb); float *w = malloc((size_t)C * 4);
        for (int r = 0; r < R; r++) { for (int c = 0; c < C; c++) w[c] = gauss() * 0.02f; enc_block(Sh[s].t, w, big + r * rb, (size_t)C); }
        for (int k = 1; k < NW; k++) memcpy(big + k * (size_t)R * rb, big, R * rb);
        Mat *mat = calloc(NW, sizeof(Mat)); TPar *par = calloc(NW, sizeof(TPar)); TvJob *J = calloc(NW, sizeof(TvJob));
        for (int k = 0; k < NW; k++) { mat[k].q = big + (size_t)k * R * rb; mat[k].type = Sh[s].t; mat[k].stride = rb; par[k].mat = &mat[k]; par[k].rows = R; par[k].cols = C; }
        static const int Bs[] = { 1, 2, 4, 8, 16, 32, 64, 128, 512 };
        for (int mode = 0; mode < 2; mode++) for (int bi = 0; bi < 9; bi++) {
            const int B = Bs[bi], K = mode ? R : C, M = mode ? C : R;
            if (NW == 96 && B > 64) continue;
            if (NW < 96 && B < 16) continue;
            float *in = malloc((size_t)NW * B * K * 4), *o1 = calloc((size_t)NW * B * M, 4), *o2 = calloc((size_t)NW * B * M, 4);
            for (size_t i = 0; i < (size_t)NW * B * K; i++) in[i] = gauss();
            double t[3] = { 1e9, 1e9, 1e9 };
            for (int rep = 0; rep < 4; rep++) for (int v = 0; v < 3; v++) {
                float *o = v ? o2 : o1;
                for (int k = 0; k < NW; k++) { J[k].p = &par[k]; J[k].in = in + (size_t)k * B * K; J[k].out = o + (size_t)k * B * M; J[k].n = B; }
                memset(o, 0, (size_t)NW * B * M * 4);
                if (v == 1) { tg.budget = (size_t)2 << 30; for (int k = 0; k < NW; k++) tg_resident(&par[k]); }
                if (v == 2) { tg.budget = 0; for (int k = 0; k < NW; k++) par[k].gstat = -1; }
                double q = bv_now();
                if (!v) tv_cpu(mode, J, NW);
                else if (!tv_run(mode, J, NW, NULL, 0)) { printf("%s recusado\n", nm); v = 3; break; }
                q = bv_now() - q; if (q < t[v]) t[v] = q;
                if (v == 2) { for (int k = 0; k < NW; k++) par[k].gstat = 0; tg.resident = 0; for (int a = 0; a < tg.nar; a++) tg.ar_used[a] = 0; }
            }
            double worst = 0;
            for (int n = 0; n < NW * B; n++) { double e = 0, a = 0; for (int m = 0; m < M; m++) { double d = o2[(size_t)n * M + m] - o1[(size_t)n * M + m]; e += d * d; a += (double)o1[(size_t)n * M + m] * o1[(size_t)n * M + m]; } worst = fmax(worst, sqrt(e / a)); }
            printf("class %-14s mode %d tokens %3d: us per product CPU %8.1f  GPU resident %8.1f  GPU streamed %8.1f   err rms vs CPU %.1e\n", nm, mode, B, t[0] / NW * 1e6, t[1] / NW * 1e6, t[2] / NW * 1e6, worst);
            free(in); free(o1); free(o2);
        }
        free(big); free(w); free(mat); free(par); free(J);
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    KvkInfo I; if (!bv_up(&I)) return 1;
    pool_init(); srand(5);
    const char *c = argc > 1 ? argv[1] : "peak";
    if (!strcmp(c, "peak")) do_peak(&I); else if (!strcmp(c, "mem")) do_mem(); else if (!strcmp(c, "int8")) do_int8(); else if (!strcmp(c, "class")) do_class(argc > 2 ? argv[2] : NULL); else { if (argc > 3) only = atoi(argv[3]); do_prod(argc > 2 ? argv[2] : NULL); }
    return 0;
}
