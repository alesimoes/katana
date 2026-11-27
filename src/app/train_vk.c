/* ---- produtos do treino nas unidades de matriz da placa (VK_KHR_cooperative_matrix) ----
 * Incluido por train.c. vk/shaders/tcm.comp decodifica o peso quantizado para fp16 uma vez por ladrilho e multiplica nas
 * unidades de matriz, acumulando em fp32. Cada vetor de entrada e normalizado por uma potencia de 2 (o maior valor vai a
 * [2048, 4096): o fp16 cobre 2^-26 do maximo sem sair da faixa normal) e a saida e desfeita em fp32 no host. Peso e entrada
 * vao em duas partes fp16 (alta e resto, shader PREC 2): com uma parte so o erro por produto e ~2e-4, pequeno, mas troca
 * especialistas quase empatados no roteador e muda o |grad| em ~1%.
 * Varios produtos da mesma forma (os especialistas de uma camada) vao num despacho so. Entradas, indices e pesos que nao
 * moram na VRAM sao gravados direto na VRAM visivel pelo host, sem copia intermediaria, numa fila de TV_SLOTS areas: enquanto a
 * placa calcula uma, o host enche a seguinte.
 * Dos especialistas que nao moram na VRAM, os de poucos tokens (KATANA_TRAIN_COOPMAT_TOKENS) ficam na CPU, calculados
 * enquanto a placa trabalha nos outros: subir o peso custa mais que a conta.
 * KATANA_TRAIN_COOPMAT=0 desliga (volta tudo ao shader fp32); KATANA_TRAIN_COOPMAT_MOE=0 deixa os especialistas no
 * caminho antigo. Sem a extensao, sem VRAM visivel pelo host, com tamanhos que nao sao multiplos do ladrilho ou com valores
 * nao finitos, o produto segue pelo caminho fp32. */
#include "vk/shaders/tcm16.h"
#include "vk/shaders/tcm64.h"
#define TV_SLOTS 3
#define TV_XB ((size_t)32 << 20)      /* entradas em fp16 (parte alta e resto), por area */
#define TV_OB ((size_t)32 << 20)      /* saidas em fp32, por area */
#define TV_WB ((size_t)48 << 20)      /* pesos enviados na hora, por area */
#define TV_NJ 4096                    /* entradas de trabalho por area */
typedef struct { TPar *p; const float *in; float *out; int n; } TvJob;
typedef struct { KvkBuf b; char *m; int coh; } TvMap;
typedef struct {
    TvMap x, j, w, r; KvkBuf o;
    int fence;                        /* vaga do kvk em voo; -1 = livre */
    uint64_t sub;
    int mode, nj, v;                  /* o que esta em voo: trabalhos jb[0..nj) e vetores */
    TvJob jb[TV_NJ]; int v0[TV_NJ]; float *us;
} TvSlot;
static struct {
    int tried, on, moe, min_tok, cpu;        /* min_tok: tokens para valer subir um especialista; cpu: produto em andamento fica na CPU */
    KvkPipe pipe[2][6][2];            /* modo, tipo, ladrilho de 16 ou 64 tokens */
    TvSlot s[TV_SLOTS]; int cur;
    int exp; size_t exp_res, attn_bytes;      /* em andamento um lote de especialistas; bytes deles e dos demais pesos na VRAM */
    long calls, jobs, streamed, refused, njcpu; double t, t_up, t_cpu;
} tv;

static int tv_map(TvMap *t, size_t n, int host) {       /* host 0: VRAM visivel pelo host; 2: memoria do host com cache */
    void *m = NULL; size_t real;
    memset(t, 0, sizeof *t); t->coh = 1;
    if (host == 2) {
        if (!kvk__mkbuf(n, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 2, &t->b.b, &t->b.m, &m, &t->coh, &real)) return 0;
    } else {
        VkBufferCreateInfo c = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        c.size = n; c.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (kvk__g.CreateBuffer(kvk__g.dev, &c, NULL, &t->b.b)) return 0;
        VkMemoryRequirements mr; kvk__g.GetBufferMemoryRequirements(kvk__g.dev, t->b.b, &mr);
        const int mt = kvk__memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
        VkMemoryAllocateInfo a = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; a.allocationSize = mr.size; a.memoryTypeIndex = (uint32_t)mt;
        if (mt < 0 || kvk__g.AllocateMemory(kvk__g.dev, &a, NULL, &t->b.m)) { kvk__g.DestroyBuffer(kvk__g.dev, t->b.b, NULL); t->b.b = 0; return 0; }
        if (kvk__g.BindBufferMemory(kvk__g.dev, t->b.b, t->b.m, 0) || kvk__g.MapMemory(kvk__g.dev, t->b.m, 0, VK_WHOLE_SIZE, 0, &m)) return 0;
    }
    t->b.size = n; t->m = m;
    return 1;
}

static void tv_unmap(TvMap *t) {
    if (t->b.b) kvk__g.DestroyBuffer(kvk__g.dev, t->b.b, NULL);
    if (t->b.m) kvk__g.FreeMemory(kvk__g.dev, t->b.m, NULL);
    memset(t, 0, sizeof *t);
}
static void tv_report(void) {
    fprintf(stderr, "unidades de matriz: %ld lotes, %ld produtos em %.2f s (%.2f s gravando entradas e pesos na VRAM, %.2f s de %ld produtos na CPU ao mesmo tempo), %ld pesos enviados na hora, %ld recusados\n",
            tv.calls, tv.jobs, tv.t, tv.t_up, tv.t_cpu, tv.njcpu, tv.streamed, tv.refused);
}
static int tv_init(void) {
    if (tv.tried) return tv.on;
    tv.tried = 1;
    const char *e = getenv("KATANA_TRAIN_COOPMAT");
    if (e && !strcmp(e, "0")) return 0;
#ifdef KATANA_TG_EMU
    return 0;
#endif
    KvkInfo I;
    if (!kvk_init(&I) || !I.coopmat || !I.fp16 || !I.storage16 || !I.int16 || I.subgroup_size != 32) return 0;
    int ok = 0;
    for (int i = 0; i < I.n_coop; i++)
        if (I.coop[i].m == 16 && I.coop[i].n == 16 && I.coop[i].k == 16 && I.coop[i].a_type == VK_COMPONENT_TYPE_FLOAT16_KHR && I.coop[i].b_type == VK_COMPONENT_TYPE_FLOAT16_KHR
            && I.coop[i].c_type == VK_COMPONENT_TYPE_FLOAT32_KHR && I.coop[i].r_type == VK_COMPONENT_TYPE_FLOAT32_KHR && I.coop[i].scope == VK_SCOPE_SUBGROUP_KHR) ok = 1;
    if (!ok) return 0;
    tv.min_tok = getenv("KATANA_TRAIN_COOPMAT_TOKENS") ? atoi(getenv("KATANA_TRAIN_COOPMAT_TOKENS")) : 2;
    tv.moe = !(getenv("KATANA_TRAIN_COOPMAT_MOE") && atoi(getenv("KATANA_TRAIN_COOPMAT_MOE")) == 0);
    for (int i = 0; i < TV_SLOTS; i++) {
        TvSlot *s = &tv.s[i];
        s->fence = -1;
        ok = ok && tv_map(&s->x, TV_XB, 0) && tv_map(&s->j, TV_NJ * 16, 0) && tv_map(&s->w, TV_WB, 0) && tv_map(&s->r, TV_OB, 2);
        if (ok) { s->o = kvk_buf_create(TV_OB, KVK_USAGE_RW); ok = s->o.b != 0; }
    }
    if (!ok) {
        for (int i = 0; i < TV_SLOTS; i++) { TvSlot *s = &tv.s[i]; tv_unmap(&s->x); tv_unmap(&s->j); tv_unmap(&s->w); tv_unmap(&s->r); kvk_buf_destroy(&s->o); }
        fprintf(stderr, "treino: sem memoria visivel para as unidades de matriz; produtos em fp32\n"); return 0;
    }
    const size_t own = TV_SLOTS * (TV_XB + TV_OB + TV_WB);
    tg.budget = tg.budget > own ? tg.budget - own : 0;
    tv.on = 1;
    if (getenv("KATANA_TRAIN_PROF")) atexit(tv_report);
    fprintf(stderr, "treino: produtos nas unidades de matriz da placa (fp16 em duas partes); especialistas fora da VRAM a partir de %d tokens\n", tv.min_tok);
    return 1;
}
static KvkPipe *tv_pipe(int mode, int wt, int big) {
    KvkPipe *p = &tv.pipe[mode][wt][big];
    if (!p->p) {
        const uint32_t ids[2] = { 0, 1 }, v[2] = { (uint32_t)mode, (uint32_t)wt };
        *p = big ? kvk_pipe_create(tcm64_spv, sizeof tcm64_spv, 4, 20, ids, v, 2) : kvk_pipe_create(tcm16_spv, sizeof tcm16_spv, 4, 20, ids, v, 2);
        if (!p->p) return NULL;
    }
    return p;
}
static int tv_wt(const TPar *p) {     /* tipos que o tcm le; -1 = fica no fp32 */
    const int t = tg_wt(p);
    return t == 0 || (t >= 2 && t <= 5) ? t : -1;
}
static int tv_shape(const TPar *p) {
    const int t = p->mat ? tv_wt(p) : -1;
    return t >= 0 && p->rows % 64 == 0 && p->cols % (t >= 3 ? 256 : 64) == 0 && p->cols >= 64;
}

/* copia e normalizacao no pool: entradas em fp16 gravadas direto na VRAM */
typedef struct { TvSlot *s; int K, v; const TvJob *jb; const int *v0; int bad; } TvX;
#define TV_TMP 16384
#ifdef __AVX2__
static void tv_x_job(void *c, int a, int b) {
    TvX *t = (TvX *)c;
    uint16_t hb[TV_TMP], lb[TV_TMP];
    for (int i = a; i < b; i++) for (int n = 0; n < t->jb[i].n; n++) {
        const float *x = t->jb[i].in + (size_t)n * t->K;
        const size_t vi = (size_t)t->v0[i] + n;
        __m256 mx = _mm256_setzero_ps(); const __m256 sg = _mm256_set1_ps(-0.0f);
        for (int k = 0; k < t->K; k += 8) mx = _mm256_max_ps(mx, _mm256_andnot_ps(sg, _mm256_loadu_ps(x + k)));
        /* nao finito: -ffast-math nao deixa comparar com infinito; olha os bits */
        __m256i inf = _mm256_set1_epi32(0x7f800000), nf = _mm256_setzero_si256();
        for (int k = 0; k < t->K; k += 8) nf = _mm256_or_si256(nf, _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_loadu_si256((const __m256i *)(x + k)), inf), inf));
        if (!_mm256_testz_si256(nf, nf)) { t->bad = 1; return; }
        float m8[8]; _mm256_storeu_ps(m8, mx);
        float m = 0; for (int k = 0; k < 8; k++) if (m8[k] > m) m = m8[k];
        int e = 0; if (m > 0) frexpf(m, &e);
        e = e < -100 ? -100 : e > 100 ? 100 : e;
        const float sc = ldexpf(1.0f, 12 - e);
        t->s->us[vi] = 1.0f / sc;
        const __m256 vs = _mm256_set1_ps(sc);
        /* parte alta (mantissa cortada em 10 bits, exata em fp16) e o resto vezes 2048, tambem em fp16 */
        const __m256 msk = _mm256_castsi256_ps(_mm256_set1_epi32((int)0xFFFFE000u)), ls = _mm256_set1_ps(2048.0f);
        for (int k = 0; k < t->K; k += 8) {
            const __m256 v = _mm256_mul_ps(_mm256_loadu_ps(x + k), vs), h = _mm256_and_ps(v, msk);
            _mm_storeu_si128((__m128i *)(hb + k), _mm256_cvtps_ph(h, 0));
            _mm_storeu_si128((__m128i *)(lb + k), _mm256_cvtps_ph(_mm256_mul_ps(_mm256_sub_ps(v, h), ls), 0));
        }
        memcpy(t->s->x.m + vi * t->K * 2, hb, (size_t)t->K * 2);
        memcpy(t->s->x.m + (vi + t->v + 64) * t->K * 2, lb, (size_t)t->K * 2);
    }
}
#else
/* porte ARM, 2026-11-27: sem SIMD; mesma normalizacao e divisao em duas partes fp16, um valor por vez */
static void tv_x_job(void *c, int a, int b) {
    TvX *t = (TvX *)c;
    uint16_t hb[TV_TMP], lb[TV_TMP];
    for (int i = a; i < b; i++) for (int n = 0; n < t->jb[i].n; n++) {
        const float *x = t->jb[i].in + (size_t)n * t->K;
        const size_t vi = (size_t)t->v0[i] + n;
        float m = 0.0f;
        int bad = 0;
        for (int k = 0; k < t->K; k++) {
            uint32_t u; memcpy(&u, &x[k], 4);
            if ((u & 0x7f800000u) == 0x7f800000u) bad = 1;      /* nao finito (inf ou nan): olha os bits */
            const float a2 = fabsf(x[k]);
            if (a2 > m) m = a2;
        }
        if (bad) { t->bad = 1; return; }
        int e = 0; if (m > 0) frexpf(m, &e);
        e = e < -100 ? -100 : e > 100 ? 100 : e;
        const float sc = ldexpf(1.0f, 12 - e);
        t->s->us[vi] = 1.0f / sc;
        /* parte alta (mantissa cortada em 10 bits, exata em fp16) e o resto vezes 2048, tambem em fp16 */
        for (int k = 0; k < t->K; k++) {
            const float v = x[k] * sc;
            uint32_t uv; memcpy(&uv, &v, 4);
            uv &= 0xFFFFE000u;
            float h; memcpy(&h, &uv, 4);
            hb[k] = f32_to_f16(h);
            lb[k] = f32_to_f16((v - h) * 2048.0f);
        }
        memcpy(t->s->x.m + vi * t->K * 2, hb, (size_t)t->K * 2);
        memcpy(t->s->x.m + (vi + t->v + 64) * t->K * 2, lb, (size_t)t->K * 2);
    }
}
#endif
/* pesos que nao moram na VRAM: pedacos de 64 KB direto na VRAM */
typedef struct { const uint8_t *src; char *dst; size_t n; } TvCp;
static void tv_cp_job(void *c, int a, int b) { const TvCp *t = (const TvCp *)c; for (int i = a; i < b; i++) memcpy(t[i].dst, t[i].src, t[i].n); }

typedef struct { TvSlot *s; int M; } TvR;
static void tv_r_job(void *c, int a, int b) {
    TvR *t = (TvR *)c; TvSlot *s = t->s; const int M = t->M;
    for (int i = a; i < b; i++) for (int n = 0; n < s->jb[i].n; n++) {
        const size_t vi = (size_t)s->v0[i] + n;
        const float *r = (const float *)s->r.m + vi * M, us = s->us[vi];
        float *o = s->jb[i].out + (size_t)n * M;
        if (s->mode == 0) for (int m = 0; m < M; m++) o[m] = r[m] * us;
        else for (int m = 0; m < M; m++) o[m] += r[m] * us;
    }
}
static int tv_harvest(TvSlot *s) {    /* espera a area e entrega as saidas */
    if (s->fence < 0) return 1;
    if (kvk__g.state[s->fence] == 2 && kvk__fence_wait(kvk__g.fence[s->fence])) return 0;
    s->fence = -1;
    if (!s->r.coh) { VkMappedMemoryRange mr = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE }; mr.memory = s->r.b.m; mr.size = VK_WHOLE_SIZE; kvk__g.InvalidateMappedMemoryRanges(kvk__g.dev, 1, &mr); }
    TvR t = { s, (int)(s->mode == 0 ? s->jb[0].p->rows : s->jb[0].p->cols) };
    pool_for(tv_r_job, &t, s->nj, 1);
    return 1;
}
/* os especialistas so moram na VRAM com o que sobra dos pesos usados em todo passo (atencao, DeltaNet, saida) */
static int tv_res(TPar *p, size_t bytes) {
    if (!tv.exp || p->gstat == 1) return tg_resident(p);
    if (tv.exp_res + bytes + tv.attn_bytes > tg.budget) return 0;
    if (!tg_resident(p)) return 0;
    tv.exp_res += bytes;
    return 1;
}
/* nj trabalhos da mesma forma e tipo (mode 0: out = W in; 1: out += W^T in; trabalhos diferentes tem saidas diferentes).
   cpu[0..nc): trabalhos feitos na CPU enquanto a placa termina. 0 = nada foi feito (caminho fp32); erro da placa no meio
   encerra, como no tg_prod */
static void tv_cpu(int mode, const TvJob *c, int nc) {
    tv.cpu = 1;
    for (int i = 0; i < nc; i++) if (mode == 0) tp_mv(c[i].p, c[i].out, c[i].in, c[i].n); else tp_mvT(c[i].p, c[i].out, c[i].in, c[i].n);
    tv.cpu = 0;
}
static int tv_run(int mode, const TvJob *jobs, int nj, const TvJob *cpu, int nc) {
    if (!nj) { tv_cpu(mode, cpu, nc); return 1; }
    if (!tv_init()) return 0;
    const TPar *p0 = jobs[0].p;
    if (!tv_shape(p0)) { tv.refused++; return 0; }
    const int wt = tv_wt(p0), K = (int)(mode == 0 ? p0->cols : p0->rows), M = (int)(mode == 0 ? p0->rows : p0->cols);
    if (K > TV_TMP) return 0;
    const size_t st = tg_stride(p0), wbytes = p0->rows * st;
    const size_t vx = TV_XB / ((size_t)K * 4), vo = TV_OB / ((size_t)M * 4);
    const int vmax = (int)(vx < vo ? vx : vo) - 64;
    if (vmax < 64) return 0;
    for (int i = 0; i < nj; i++) {
        const TPar *p = jobs[i].p;
        if (jobs[i].n > vmax || p->rows != p0->rows || p->cols != p0->cols || tv_wt(p) != wt) return 0;
        if (wbytes > TV_WB - 4096 && !tg_resident((TPar *)p)) return 0;
    }
    KvkPipe *ps = tv_pipe(mode, wt, 0), *pb = tv_pipe(mode, wt, 1);
    if (!ps || !pb) return 0;
    const double t0 = now_sec();
    int done = 0, any = 0, bad = 0;
    while (done < nj) {
        TvSlot *s = &tv.s[tv.cur]; tv.cur = (tv.cur + 1) % TV_SLOTS;
        if (!tv_harvest(s)) goto fail;
        if (!s->us) s->us = xalloc(TV_XB / 128, 4);
        /* quantos cabem nesta area; e[i][0..3] montado na ordem: lista por (arena, ladrilho) */
        int n = 0, v = 0, ne = 0; size_t wu = 0;
        static uint32_t ent[TV_NJ][4]; static int key[TV_NJ]; static TvCp cp[TV_NJ * 4]; int ncp = 0;
        for (; done + n < nj && n < TV_NJ; n++) {
            const TvJob *jb = &jobs[done + n];
            const int res = tv_res(jb->p, wbytes);
            if (n && (v + jb->n > vmax || ne + jb->n / 16 + 4 > TV_NJ || (!res && (wu + wbytes > TV_WB || ncp + (int)(wbytes >> 18) + 2 > TV_NJ * 4)))) break;
            size_t wo; int ar;
            if (res) { wo = jb->p->goff; ar = jb->p->gar; }
            else {
                wo = wu; ar = -1; tv.streamed++;
                const uint8_t *src = tg_src(jb->p);
                for (size_t o = 0; o < wbytes; o += (size_t)1 << 18) { cp[ncp].src = src + o; cp[ncp].dst = s->w.m + wu + o; cp[ncp].n = wbytes - o < ((size_t)1 << 18) ? wbytes - o : (size_t)1 << 18; ncp++; }
                wu = (wu + wbytes + 255) & ~(size_t)255;
            }
            s->jb[n] = *jb; s->v0[n] = v;
            for (int b = 0; b < jb->n;) {
                const int left = jb->n - b, big = left > 32, c = big ? (left < 64 ? left : 64) : (left < 16 ? left : 16);
                ent[ne][0] = (uint32_t)wo; ent[ne][1] = ent[ne][2] = (uint32_t)(v + b); ent[ne][3] = (uint32_t)c;
                key[ne++] = (ar + 1) * 2 + big; b += c;
            }
            v += jb->n;
        }
        TvX tx = { s, K, v, s->jb, s->v0, 0 };
        const double u0 = now_sec();
        pool_for(tv_x_job, &tx, n, 1);
        if (tx.bad) { bad = 1; break; }
        if (ncp) pool_for(tv_cp_job, cp, ncp, 1);
        tv.t_up += now_sec() - u0;
        uint32_t *J = (uint32_t *)s->j.m; int j0 = 0;
        for (int k = 0; k < 2 * (tg.nar + 1) && j0 < ne; k++) {
            int c = 0;
            for (int i = 0; i < ne; i++) if (key[i] == k) memcpy(J + 4 * (j0 + c++), ent[i], 16);
            if (!c) continue;
            const struct { uint32_t M, K, ws, j0, xr; } pc = { (uint32_t)M, (uint32_t)K, (uint32_t)st, (uint32_t)j0, (uint32_t)v + 64 };
            const KvkBuf bb[4] = { k < 2 ? s->w.b : tg.ar[k / 2 - 1], s->x.b, s->o, s->j.b };
            kvk_set_auto_barrier(j0 == 0);          /* os grupos desta area nao dependem uns dos outros */
            const int okd = kvk_dispatch(*(k & 1 ? pb : ps), bb, 4, &pc, sizeof pc, (uint32_t)M / 64, (uint32_t)c, 1);
            kvk_set_auto_barrier(1);
            if (!okd) goto fail;
            j0 += c;
        }
        {   /* saida para a memoria do host */
            VkCommandBuffer cb = kvk__ccb(); if (!cb) goto fail;
            kvk__bar(cb);
            VkBufferCopy rg = { 0, 0, (size_t)v * M * 4 };
            kvk__g.CmdCopyBuffer(cb, s->o.b, s->r.b.b, 1, &rg);
            VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER }; mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            kvk__g.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        }
        s->fence = kvk__g.cur;
        if (!kvk_flush()) goto fail;
        s->mode = mode; s->nj = n; s->v = v;
        done += n; any = 1;
    }
    if (!bad || any) { const double c0 = now_sec(); tv_cpu(mode, cpu, nc); tv.t_cpu += now_sec() - c0; tv.njcpu += nc; }
    for (int i = 0; i < TV_SLOTS; i++) if (!tv_harvest(&tv.s[(tv.cur + i) % TV_SLOTS])) goto fail;
    if (bad) {                                        /* valor nao finito: o resto (ou tudo) pelo caminho fp32 */
        tv.refused++;
        if (!any) return 0;
        tv_cpu(mode, jobs + done, nj - done);
    }
    tv.calls++; tv.jobs += nj; tv.t += now_sec() - t0;
    return 1;
fail:
    fprintf(stderr, "treino: erro na GPU (%s)\n", kvk_error());
    die("treino: a GPU falhou no meio de um produto; rode de novo com KATANA_TRAIN_COOPMAT=0 ou KATANA_TRAIN_GPU=0");
    return 0;
}

static int tv_cpu_only(void) { return tv.cpu; }       /* produto que o lote separou para a CPU */
/* gancho do tg_prod: um produto so */
static int tv_prod(int mode, TPar *p, const float *in, float *out, int B) {
    if (mode > 1 || tv.cpu) return 0;
    const TvJob j = { p, in, out, B };
    return tv_run(mode, &j, 1, NULL, 0);
}

/* varios trabalhos que podem ter tipos diferentes: todos ou nenhum */
static int tv_frozen(const TPar *p) { return p->mat && !p->w && !p->r && !p->g && tv_shape(p); }

/* especialistas da camada l num lote so, os tres produtos. Agrupa os pares (token, k) por especialista em ix; 0 = caminho antigo */
typedef struct { int e, n, i0; } TvExp;
static int tv_moe_list(Model *m, TTab *tt, int l, int E, int TK, const int *sel, int *ix, TvExp *ex) {
    if (!tv.moe || m->cfg.hidden < 64) return -1;
    if (!tv.attn_bytes) {
        for (int i = 0; i < tt->n; i++) if (i != tt->tok && tt->p[i].mat) tv.attn_bytes += tt->p[i].rows * tg_stride(&tt->p[i]);
        for (int k = 0; k < m->cfg.n_layers; k++) if (tt->rt[k] >= 0) for (int e = 0; e < 3 * E; e++) { const TPar *p = &tt->p[tt->ex0[k] + e]; tv.attn_bytes -= p->rows * tg_stride(p); }
    }
    int *cnt = xalloc((size_t)E + 1, sizeof(int)), ne = 0, at = 0;
    memset(cnt, 0, ((size_t)E + 1) * sizeof(int));
    for (int j = 0; j < TK; j++) cnt[sel[j]]++;
    for (int e = 0; e < E; e++) if (cnt[e]) {
        const int base = tt->ex0[l] + 3 * e;
        for (int k = 0; k < 3; k++) if (!tv_frozen(&tt->p[base + k])) { free(cnt); return -1; }
        ex[ne].e = e; ex[ne].n = 0; ex[ne].i0 = at; at += cnt[e]; cnt[e] = ne++;
    }
    for (int j = 0; j < TK; j++) { TvExp *x = &ex[cnt[sel[j]]]; ix[x->i0 + x->n++] = j; }
    free(cnt);
    return ne;
}
typedef struct { const int *ix; const float *src; float *dst; int d, div; const float *w; } TvG;
static void tv_gather_job(void *c, int a, int b) {
    const TvG *t = (const TvG *)c;
    for (int i = a; i < b; i++) {
        const float *s = t->src + (size_t)(t->ix[i] / t->div) * t->d; float *o = t->dst + (size_t)i * t->d;
        if (!t->w) memcpy(o, s, (size_t)t->d * 4);
        else { const float w = t->w[t->ix[i]]; for (int q = 0; q < t->d; q++) o[q] = w * s[q]; }
    }
}
static void tv_scatter_job(void *c, int a, int b) {      /* inverso: dst[ix[i]] = src[i] */
    const TvG *t = (const TvG *)c;
    for (int i = a; i < b; i++) memcpy(t->dst + (size_t)t->ix[i] * t->d, t->src + (size_t)i * t->d, (size_t)t->d * 4);
}
static void tv_gather(const int *ix, int n, const float *src, float *dst, int d, int div, const float *w) { TvG t = { ix, src, dst, d, div, w }; pool_for(tv_gather_job, &t, n, 8); }
static void tv_scatter(const int *ix, int n, const float *src, float *dst, int d) { TvG t = { ix, src, dst, d, 1, NULL }; pool_for(tv_scatter_job, &t, n, 8); }
typedef struct { float *x, *y, *z; const float *u; int H; } TvA;
static float t_act(float g);
static float t_dact(float g);
static void tv_act_job(void *c, int a, int b) { TvA *t = (TvA *)c; for (size_t i = (size_t)a * t->H; i < (size_t)b * t->H; i++) t->z[i] = t_act(t->x[i]) * t->y[i]; }
static void tv_dact_job(void *c, int a, int b) {         /* x = dh, y = a, z = da, u = u; du fica em x */
    TvA *t = (TvA *)c;
    for (size_t i = (size_t)a * t->H; i < (size_t)b * t->H; i++) { const float dh = t->x[i]; t->x[i] = dh * t_act(t->y[i]); t->z[i] = dh * t->u[i] * t_dact(t->y[i]); }
}
/* monta e roda os trabalhos de uma parte (0 gate, 1 up, 2 down) dos especialistas, divididos entre a placa e a CPU */
static int tv_part(int mode, TTab *tt, int l, const TvExp *ex, int ne, int part, const float *in, int di, float *out, int dq, TvJob *J) {
    for (int i = 0; i < ne; i++) { J[i].p = &tt->p[tt->ex0[l] + 3 * ex[i].e + part]; J[i].in = in + (size_t)ex[i].i0 * di; J[i].out = out + (size_t)ex[i].i0 * dq; J[i].n = ex[i].n; }
    int ng = 0, nc = 0;
    TvJob *C = J + ne;
    for (int i = 0; i < ne; i++) {
        const int g = J[i].n >= tv.min_tok || tv_res(J[i].p, J[i].p->rows * tg_stride(J[i].p));
        if (g) J[ng++] = J[i]; else C[nc++] = J[i];
    }
    return tv_run(mode, J, ng, C, nc) ? 1 : 0;
}
static void tv_all(int mode, TTab *tt, int l, const TvExp *ex, int ne, int part, const float *in, int di, float *out, int dq, TvJob *J) {   /* tudo na CPU */
    for (int i = 0; i < ne; i++) { J[i].p = &tt->p[tt->ex0[l] + 3 * ex[i].e + part]; J[i].in = in + (size_t)ex[i].i0 * di; J[i].out = out + (size_t)ex[i].i0 * dq; J[i].n = ex[i].n; }
    tv_cpu(mode, J, ne);
}
/* ida: soma em out; guarda ativacoes em ea, eu, eh, eo (por par) */
static int tv_moe_fwd(Model *m, TTab *tt, int l, int T, int E, int Kx, const int *sel, const float *ew, const float *x, float *ea, float *eu, float *eh, float *eo, float *out) {
    const int d = m->cfg.dim, He = m->cfg.hidden, TK = T * Kx;
    if (!tg_init() || !tv_init()) return 0;
    int *ix = xalloc(TK, sizeof(int)); TvExp *ex = xalloc(E, sizeof *ex); TvJob *J = xalloc(2 * E, sizeof *J);
    const int ne = tv_moe_list(m, tt, l, E, TK, sel, ix, ex);
    int ok = ne > 0;
    if (ok) {
        float *xb = xalloc((size_t)TK * d, 4), *ab = xalloc((size_t)TK * He, 4), *ub = xalloc((size_t)TK * He, 4), *hb = xalloc((size_t)TK * He, 4), *ob = xalloc((size_t)TK * d, 4);
        tv_gather(ix, TK, x, xb, d, Kx, NULL);
        const double p0 = now_sec(), c0 = tprof[TP_MV] + tprof[TP_MVT];
        tv.exp = 1;
        ok = tv_part(0, tt, l, ex, ne, 0, xb, d, ab, He, J);
        if (ok) { if (!tv_part(0, tt, l, ex, ne, 1, xb, d, ub, He, J)) tv_all(0, tt, l, ex, ne, 1, xb, d, ub, He, J); }
        if (ok) {
            TvA a = { ab, ub, hb, NULL, He }; pool_for(tv_act_job, &a, TK, 16);
            if (!tv_part(0, tt, l, ex, ne, 2, hb, He, ob, d, J)) tv_all(0, tt, l, ex, ne, 2, hb, He, ob, d, J);
            tprof[TP_MV] += now_sec() - p0 - (tprof[TP_MV] + tprof[TP_MVT] - c0);
            tv_scatter(ix, TK, ab, ea, He); tv_scatter(ix, TK, ub, eu, He); tv_scatter(ix, TK, hb, eh, He); tv_scatter(ix, TK, ob, eo, d);
            /* mesma ordem de soma do caminho antigo (especialista crescente) */
            for (int i = 0; i < TK; i++) { const float w = ew[ix[i]]; float *ot = out + (size_t)(ix[i] / Kx) * d; const float *oi = ob + (size_t)i * d; for (int q = 0; q < d; q++) ot[q] += w * oi[q]; }
        }
        tv.exp = 0;
        free(xb); free(ab); free(ub); free(hb); free(ob);
    }
    free(ix); free(ex); free(J);
    return ok;
}
/* volta: dx += soma dos especialistas (pesos congelados: nao ha gradiente deles) */
static int tv_moe_back(Model *m, TTab *tt, int l, int T, int E, int Kx, const int *sel, const float *ew, const float *ea, const float *eu, const float *dy, float *dx) {
    const int d = m->cfg.dim, He = m->cfg.hidden, TK = T * Kx;
    if (!tg_init() || !tv_init()) return 0;
    int *ix = xalloc(TK, sizeof(int)); TvExp *ex = xalloc(E, sizeof *ex); TvJob *J = xalloc(2 * E, sizeof *J);
    const int ne = tv_moe_list(m, tt, l, E, TK, sel, ix, ex);
    int ok = ne > 0;
    if (ok) {
        float *db = xalloc((size_t)TK * d, 4), *dh = xalloc((size_t)TK * He, 4), *ab = xalloc((size_t)TK * He, 4), *ub = xalloc((size_t)TK * He, 4), *da = xalloc((size_t)TK * He, 4), *xb = xalloc((size_t)TK * d, 4);
        tv_gather(ix, TK, dy, db, d, Kx, ew); tv_gather(ix, TK, ea, ab, He, 1, NULL); tv_gather(ix, TK, eu, ub, He, 1, NULL);
        memset(dh, 0, (size_t)TK * He * 4); memset(xb, 0, (size_t)TK * d * 4);
        const double p0 = now_sec(), c0 = tprof[TP_MV] + tprof[TP_MVT];
        tv.exp = 1;
        ok = tv_part(1, tt, l, ex, ne, 2, db, d, dh, He, J);
        if (ok) {
            TvA a = { dh, ab, da, ub, He }; pool_for(tv_dact_job, &a, TK, 16);
            if (!tv_part(1, tt, l, ex, ne, 0, da, He, xb, d, J)) tv_all(1, tt, l, ex, ne, 0, da, He, xb, d, J);
            if (!tv_part(1, tt, l, ex, ne, 1, dh, He, xb, d, J)) tv_all(1, tt, l, ex, ne, 1, dh, He, xb, d, J);
            tprof[TP_MVT] += now_sec() - p0 - (tprof[TP_MV] + tprof[TP_MVT] - c0);
            for (int i = 0; i < TK; i++) { float *dt = dx + (size_t)(ix[i] / Kx) * d; const float *q0 = xb + (size_t)i * d; for (int q = 0; q < d; q++) dt[q] += q0[q]; }
        }
        tv.exp = 0;
        free(db); free(dh); free(ab); free(ub); free(da); free(xb);
    }
    free(ix); free(ex); free(J);
    return ok;
}
