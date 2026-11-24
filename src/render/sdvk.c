/* ============================ render: Stable Diffusion 1.5 na GPU (Vulkan, camada kvk) ===============================
 * O mesmo caminho do render/sdcu.c (que escolhe a camada), com os kernels em GLSL (vk/shaders/sd_*.comp, F32 com pesos F16; sem matriz
 * cooperativa, funciona em qualquer GPU Vulkan 1.2 com subgrupos de 32 ou mais). A memoria e uma arena so (um buffer grande, ate o limite
 * de maxStorageBufferRange); os "ponteiros" que o sdcu.c passa sao deslocamentos em bytes nela; um alocador simples (primeiro que cabe,
 * com juncao) faz o papel do cuMemAlloc. Os kernels veem a arena como palavras de 32 bits (F32 inteiro, F16 em pares). */
#include "vk/shaders/sd_conv.h"
#include "vk/shaders/sd_gn.h"
#include "vk/shaders/sd_flash40.h"
#include "vk/shaders/sd_flash80.h"
#include "vk/shaders/sd_flash160.h"
#include "vk/shaders/sd_ln.h"
#include "vk/shaders/sd_gemv.h"
#include "vk/shaders/sd_sm.h"
#include "vk/shaders/sd_tr.h"
#include "vk/shaders/sd_geglu.h"
#include "vk/shaders/sd_cat.h"
#include "vk/shaders/sd_add.h"

#define SVK_NBLK 8192
static struct {
    int tried, ok;
    KvkBuf arena; uint64_t size;
    KvkPipe conv, gn, fl[3], ln, gemv, sm, tr, geglu, cat, add;
    struct { uint64_t off, size; int used; } blk[SVK_NBLK]; int nblk;
} g_svk;

static int svk_init(void) {
    if (g_svk.tried) return g_svk.ok;
    g_svk.tried = 1;
    { const char *e = getenv("KATANA_STABLE_VULKAN"); if (e && !atoi(e)) return 0; }
    if ((getenv("KATANA_CPU") && atoi(getenv("KATANA_CPU")))) return 0;
    { const char *be = getenv("KATANA_BACKEND"); if (be && *be && strcmp(be, "vulkan")) return 0; }
    KvkInfo I;
    if (!kvk_init(&I)) { fprintf(stderr, "[stable] vulkan: indisponivel (%s)\n", kvk_error()); return 0; }
    if (I.subgroup_size < 32 || I.max_shared < 16384) { fprintf(stderr, "[stable] vulkan: a GPU nao serve (subgrupo %u, memoria compartilhada %u)\n", I.subgroup_size, I.max_shared); return 0; }
    g_svk.conv = kvk_pipe_create(sd_conv_spv, sizeof sd_conv_spv, 1, 88, NULL, NULL, 0);
    g_svk.gn = kvk_pipe_create(sd_gn_spv, sizeof sd_gn_spv, 1, 32, NULL, NULL, 0);
    g_svk.fl[0] = kvk_pipe_create(sd_flash40_spv, sizeof sd_flash40_spv, 1, 44, NULL, NULL, 0);
    g_svk.fl[1] = kvk_pipe_create(sd_flash80_spv, sizeof sd_flash80_spv, 1, 44, NULL, NULL, 0);
    g_svk.fl[2] = kvk_pipe_create(sd_flash160_spv, sizeof sd_flash160_spv, 1, 44, NULL, NULL, 0);
    g_svk.ln = kvk_pipe_create(sd_ln_spv, sizeof sd_ln_spv, 1, 24, NULL, NULL, 0);
    g_svk.gemv = kvk_pipe_create(sd_gemv_spv, sizeof sd_gemv_spv, 1, 36, NULL, NULL, 0);
    g_svk.sm = kvk_pipe_create(sd_sm_spv, sizeof sd_sm_spv, 1, 12, NULL, NULL, 0);
    g_svk.tr = kvk_pipe_create(sd_tr_spv, sizeof sd_tr_spv, 1, 20, NULL, NULL, 0);
    g_svk.geglu = kvk_pipe_create(sd_geglu_spv, sizeof sd_geglu_spv, 1, 24, NULL, NULL, 0);
    g_svk.cat = kvk_pipe_create(sd_cat_spv, sizeof sd_cat_spv, 1, 24, NULL, NULL, 0);
    g_svk.add = kvk_pipe_create(sd_add_spv, sizeof sd_add_spv, 1, 24, NULL, NULL, 0);
    if (!g_svk.conv.p || !g_svk.gn.p || !g_svk.fl[0].p || !g_svk.fl[1].p || !g_svk.fl[2].p || !g_svk.ln.p || !g_svk.gemv.p || !g_svk.sm.p || !g_svk.tr.p || !g_svk.geglu.p || !g_svk.cat.p || !g_svk.add.p) {
        fprintf(stderr, "[stable] vulkan: os pipelines falharam (%s)\n", kvk_error()); kvk_shutdown(); return 0;
    }
    uint64_t budget = kvk_vram_budget(); if (!budget) budget = I.vram_bytes;
    uint64_t sz = budget / 100 * 94;
    { const char *cap = getenv("KATANA_STABLE_VRAM_MB"); if (cap && (uint64_t)atoll(cap) << 20 < sz) sz = (uint64_t)atoll(cap) << 20; }
    uint64_t lim = I.max_storage_range < I.max_alloc ? I.max_storage_range : I.max_alloc; if (lim > 0xFFFFFF00ull) lim = 0xFFFFFF00ull;
    if (sz > lim) sz = lim;
    sz &= ~255ull;
    while (sz >= ((uint64_t)512 << 20)) {
        g_svk.arena = kvk_buf_create((size_t)sz, KVK_USAGE_RW);
        if (g_svk.arena.b) break;
        sz = sz / 10 * 9 & ~255ull;
    }
    if (!g_svk.arena.b) { fprintf(stderr, "[stable] vulkan: nao consegui a arena de VRAM (%s)\n", kvk_error()); return 0; }
    g_svk.size = sz;
    g_svk.blk[0].off = 256; g_svk.blk[0].size = sz - 256; g_svk.blk[0].used = 0; g_svk.nblk = 1;
    g_svk.ok = 1;
    return 1;
}

static uint64_t svk_alloc(size_t n) {
    n = (n + 255) & ~(size_t)255; if (!n) n = 256;
    for (int i = 0; i < g_svk.nblk; i++) {
        if (g_svk.blk[i].used || g_svk.blk[i].size < n) continue;
        const uint64_t off = g_svk.blk[i].off;
        if (g_svk.blk[i].size > n && g_svk.nblk < SVK_NBLK) {
            memmove(&g_svk.blk[i + 2], &g_svk.blk[i + 1], (size_t)(g_svk.nblk - i - 1) * sizeof g_svk.blk[0]);
            g_svk.blk[i + 1].off = off + n; g_svk.blk[i + 1].size = g_svk.blk[i].size - n; g_svk.blk[i + 1].used = 0;
            g_svk.blk[i].size = n; g_svk.nblk++;
        }
        g_svk.blk[i].used = 1;
        return off;
    }
    fprintf(stderr, "[stable] vulkan: sem memoria na arena (%zu MB pedidos)\n", n >> 20);
    return 0;
}
static void svk_free(uint64_t p) {
    if (!p) return;
    for (int i = 0; i < g_svk.nblk; i++) {
        if (g_svk.blk[i].off != p || !g_svk.blk[i].used) continue;
        g_svk.blk[i].used = 0;
        if (i + 1 < g_svk.nblk && !g_svk.blk[i + 1].used) { g_svk.blk[i].size += g_svk.blk[i + 1].size; memmove(&g_svk.blk[i + 1], &g_svk.blk[i + 2], (size_t)(g_svk.nblk - i - 2) * sizeof g_svk.blk[0]); g_svk.nblk--; }
        if (i > 0 && !g_svk.blk[i - 1].used) { g_svk.blk[i - 1].size += g_svk.blk[i].size; memmove(&g_svk.blk[i], &g_svk.blk[i + 1], (size_t)(g_svk.nblk - i - 1) * sizeof g_svk.blk[0]); g_svk.nblk--; }
        return;
    }
}
static size_t svk_free_bytes(void) { size_t s = 0; for (int i = 0; i < g_svk.nblk; i++) if (!g_svk.blk[i].used) s += (size_t)g_svk.blk[i].size; return s; }
static int svk_up(uint64_t d, const void *h, size_t n) { return kvk_buf_write(g_svk.arena, (size_t)d, h, n); }
static int svk_down(void *h, uint64_t d, size_t n) { return kvk_buf_read(g_svk.arena, (size_t)d, h, n); }
static int svk_sync(void) { return kvk_wait(); }

static int svk_go(KvkPipe p, const void *push, int bytes, uint32_t gx, uint32_t gy) { return kvk_dispatch(p, &g_svk.arena, 1, push, bytes, gx, gy, 1); }
#define SVK_U(v) ((uint32_t)(v))
static int svk_conv(uint64_t X, uint64_t W, uint64_t bias, uint64_t res, uint64_t Y, uint64_t ca, uint64_t cb, int silu, int H, int Wd, int Cin, int Cout, int ks, int up, int Kp, int stride, int pt, int pl, int LH, int LW, float oscale, int out16) {
    uint32_t pc[22] = { SVK_U(X), SVK_U(W), SVK_U(bias), SVK_U(res), SVK_U(Y), SVK_U(ca), SVK_U(cb), SVK_U(silu), SVK_U(H), SVK_U(Wd), SVK_U(Cin), SVK_U(Cout), SVK_U(ks), SVK_U(up), SVK_U(Kp),
        SVK_U(stride), SVK_U(pt), SVK_U(pl), SVK_U(LH), SVK_U(LW), SVK_U(out16), 0 };
    memcpy(&pc[21], &oscale, 4);
    return svk_go(g_svk.conv, pc, 88, SVK_U((Cout + 63) / 64), SVK_U(((size_t)H * Wd + 63) / 64));
}
static int svk_gn(uint64_t X, int n, int C, uint64_t gamma, uint64_t beta, float eps, uint64_t ca, uint64_t cb) {
    uint32_t pc[8] = { SVK_U(X), SVK_U(gamma), SVK_U(beta), SVK_U(ca), SVK_U(cb), SVK_U(n), SVK_U(C), 0 };
    memcpy(&pc[7], &eps, 4);
    return svk_go(g_svk.gn, pc, 32, 32, 1);
}
static int svk_ln(uint64_t X, uint64_t Y, uint64_t g, uint64_t b, int n, int C, float eps) {
    uint32_t pc[6] = { SVK_U(X), SVK_U(Y), SVK_U(g), SVK_U(b), SVK_U(C), 0 };
    memcpy(&pc[5], &eps, 4);
    return svk_go(g_svk.ln, pc, 24, SVK_U(n), 1);
}
static int svk_gemv(uint64_t W, uint64_t x, uint64_t b1, uint64_t b2, uint64_t y, int Kp, int nout, int K, int silu) {
    uint32_t pc[9] = { SVK_U(W), SVK_U(x), SVK_U(b1), SVK_U(b2), SVK_U(y), SVK_U(Kp), SVK_U(nout), SVK_U(K), SVK_U(silu) };
    return svk_go(g_svk.gemv, pc, 36, SVK_U((nout + 7) / 8), 1);
}
static int svk_ew(KvkPipe p, uint64_t a, uint64_t b, uint64_t c, uint64_t y, uint32_t n1, uint32_t n2, uint32_t gx) {
    uint32_t pc[6] = { SVK_U(a), SVK_U(b), SVK_U(c), SVK_U(y), n1, n2 };
    return svk_go(p, pc, 24, gx, 1);
}
static int svk_geglu(uint64_t P, uint64_t Y, int n, int F) { return svk_ew(g_svk.geglu, P, 0, 0, Y, SVK_U(F), 0, SVK_U(n)); }
static int svk_cat(uint64_t A, uint64_t B, uint64_t Br, uint64_t Y, int n, int Ca, int Cb) { return svk_ew(g_svk.cat, A, B, Br, Y, SVK_U(Ca), SVK_U(Cb), SVK_U(n)); }
static int svk_add(uint64_t Y, uint64_t X, long n) { return svk_ew(g_svk.add, Y, X, 0, 0, SVK_U(n), 0, SVK_U((n + 255) / 256 > 4096 ? 4096 : (n + 255) / 256)); }
static int svk_flash(uint64_t Q, int ldq, uint64_t K, int ldk, uint64_t V, int ldv, uint64_t O, int ldo, int nq, int nk, int C, float sl2) {
    const int hd = C / 8, ix = hd == 40 ? 0 : hd == 80 ? 1 : hd == 160 ? 2 : -1;
    if (ix < 0) return 0;
    const int qpg = 256 / (hd / 5);
    uint32_t pc[11] = { SVK_U(Q), SVK_U(K), SVK_U(V), SVK_U(O), SVK_U(ldq), SVK_U(ldk), SVK_U(ldv), SVK_U(ldo), SVK_U(nq), SVK_U(nk), 0 };
    memcpy(&pc[10], &sl2, 4);
    return svk_go(g_svk.fl[ix], pc, 44, SVK_U((nq + qpg - 1) / qpg), 8);
}
static int svk_softmax(uint64_t S, int nr, int nc, float scale) {
    uint32_t pc[3] = { SVK_U(S), SVK_U(nc), 0 };
    memcpy(&pc[2], &scale, 4);
    return svk_go(g_svk.sm, pc, 12, SVK_U(nr), 1);
}
static int svk_transp(uint64_t in, uint64_t out, int n, int C, int ldo) {
    uint32_t pc[5] = { SVK_U(in), SVK_U(out), SVK_U(n), SVK_U(C), SVK_U(ldo) };
    return svk_go(g_svk.tr, pc, 20, SVK_U((n + 31) / 32), SVK_U((C + 31) / 32));
}
