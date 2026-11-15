/* vkmv: porte dos kernels de produto matriz x vetor (um token) do katana de OpenCL para Vulkan,
   com teste de corretude contra uma referencia em double e medida de vazao.
   out[rows] = W[rows][cols] * x[cols], x em float32, W quantizada no layout da VRAM do katana.
   Uso: vkmv [-q] [-t tipo] [-v variante]    (-q: so corretude; tipo: f32 q8 q4k q5k q6k) */
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "vulkan/vulkan.h"
#include "mv_f32.h"
#include "mv_q8.h"
#include "mv_q4k.h"
#include "mv_q5k.h"
#include "mv_q6k.h"
#include "mv_q4k_alt.h"

#define F(name) static PFN_##name name
F(vkGetInstanceProcAddr); F(vkCreateInstance); F(vkEnumeratePhysicalDevices); F(vkGetPhysicalDeviceProperties);
F(vkGetPhysicalDeviceQueueFamilyProperties); F(vkCreateDevice); F(vkGetDeviceProcAddr); F(vkGetDeviceQueue);
F(vkGetPhysicalDeviceMemoryProperties); F(vkCreateBuffer); F(vkGetBufferMemoryRequirements); F(vkAllocateMemory);
F(vkBindBufferMemory); F(vkMapMemory); F(vkCreateShaderModule); F(vkCreateDescriptorSetLayout); F(vkCreatePipelineLayout);
F(vkCreateComputePipelines); F(vkCreateDescriptorPool); F(vkAllocateDescriptorSets); F(vkUpdateDescriptorSets);
F(vkCreateCommandPool); F(vkAllocateCommandBuffers); F(vkBeginCommandBuffer); F(vkCmdBindPipeline);
F(vkCmdBindDescriptorSets); F(vkCmdPushConstants); F(vkCmdDispatch); F(vkEndCommandBuffer); F(vkCmdCopyBuffer);
F(vkQueueSubmit); F(vkCreateFence); F(vkWaitForFences); F(vkResetFences); F(vkResetCommandBuffer);
F(vkCmdPipelineBarrier); F(vkDestroyBuffer); F(vkFreeMemory); F(vkResetDescriptorPool); F(vkUnmapMemory);
F(vkGetPhysicalDeviceProperties2);

static double now(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / f.QuadPart; }
#define CK(x) do { VkResult r_ = (x); if (r_) { printf("erro %d em %s (linha %d)\n", r_, #x, __LINE__); exit(1); } } while (0)

/* ---------- float16 ---------- */
static uint16_t f2h(float f) {
    uint32_t u; memcpy(&u, &f, 4);
    uint32_t s = (u >> 16) & 0x8000; int e = (int)((u >> 23) & 0xFF) - 127 + 15; uint32_t m = u & 0x7FFFFF;
    if (e <= 0) { if (e < -10) return (uint16_t)s; m |= 0x800000; return (uint16_t)(s | (m >> (14 - e))); }
    if (e >= 31) return (uint16_t)(s | 0x7C00);
    return (uint16_t)(s | (e << 10) | (m >> 13));
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

/* ---------- tipos ---------- */
enum { K_F32, K_Q8, K_Q4K, K_Q5K, K_Q6K, K_N };
static const char *kname[K_N] = { "f32", "q8", "q4k", "q5k", "q6k" };
static const char *kdesc[K_N] = { "float32", "Q8_0", "Q4_K", "Q5_K", "Q6_K" };

/* bytes de uma linha no formato GGUF */
static size_t rowbytes(int k, int cols) {
    switch (k) { case K_F32: return (size_t)cols * 4; case K_Q8: return (size_t)cols / 32 * 34; case K_Q4K: return (size_t)cols / 256 * 144;
                 case K_Q5K: return (size_t)cols / 256 * 176; default: return (size_t)cols / 256 * 210; }
}
/* palavras de 32 bits por linha no layout da VRAM (igual ao gpu_wpr do katana) */
static size_t wpr_of(int k, int cols) {
    switch (k) { case K_F32: return cols; case K_Q4K: return cols / 256 * 36; case K_Q5K: return cols / 256 * 44;
                 case K_Q6K: return cols / 256 * 53; default: return cols / 32 * 9; }
}
static int nblocks(int k, int cols) { return k == K_F32 ? 1 : k == K_Q8 ? cols / 32 : cols / 256; }

static void put_half(uint8_t *p, float v) { uint16_t h = f2h(v); memcpy(p, &h, 2); }
static void rnd_bytes(uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(rnd() >> 33); }

/* linha aleatoria diretamente no formato de bloco do GGUF (escalas pequenas para somas bem comportadas) */
static void gen_row(int k, int cols, uint8_t *d) {
    switch (k) {
    case K_F32: for (int i = 0; i < cols; i++) { float v = 0.05f * rndsym(); memcpy(d + 4 * i, &v, 4); } break;
    case K_Q8: for (int b = 0; b < cols / 32; b++, d += 34) { put_half(d, 0.002f + 0.01f * rnd01()); rnd_bytes(d + 2, 32); } break;
    case K_Q4K: for (int b = 0; b < cols / 256; b++, d += 144) { rnd_bytes(d, 144); put_half(d, 0.0005f + 0.002f * rnd01()); put_half(d + 2, 0.0005f + 0.002f * rnd01()); } break;
    case K_Q5K: for (int b = 0; b < cols / 256; b++, d += 176) { rnd_bytes(d, 176); put_half(d, 0.0005f + 0.002f * rnd01()); put_half(d + 2, 0.0005f + 0.002f * rnd01()); } break;
    default:    for (int b = 0; b < cols / 256; b++, d += 210) { rnd_bytes(d, 210); put_half(d + 208, 0.0003f + 0.001f * rnd01()); } break;
    }
}

static void scale_min_k4(int j, const uint8_t *q, int *d, int *m) {
    if (j < 4) { *d = q[j] & 63; *m = q[j + 4] & 63; }
    else { *d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4); *m = (q[j + 4] >> 4) | ((q[j] >> 6) << 4); }
}

/* desquantiza uma linha do formato GGUF (referencia independente do layout da GPU), em double */
static void dq_row(int k, const uint8_t *s, int cols, double *o) {
    switch (k) {
    case K_F32: for (int i = 0; i < cols; i++) { float v; memcpy(&v, s + 4 * i, 4); o[i] = v; } break;
    case K_Q8:
        for (int b = 0; b < cols / 32; b++, s += 34, o += 32) {
            uint16_t h; memcpy(&h, s, 2); const double d = h2f(h);
            for (int i = 0; i < 32; i++) o[i] = d * (int8_t)s[2 + i];
        }
        break;
    case K_Q4K: case K_Q5K: {
        const int five = k == K_Q5K;
        for (int b = 0; b < cols / 256; b++, s += five ? 176 : 144, o += 256) {
            uint16_t h0, h1; memcpy(&h0, s, 2); memcpy(&h1, s + 2, 2);
            const double d = h2f(h0), dmin = h2f(h1);
            const uint8_t *sc = s + 4, *qh = s + 16, *qs = s + (five ? 48 : 16);
            for (int c = 0; c < 4; c++) {
                int s1, m1, s2, m2;
                scale_min_k4(2 * c, sc, &s1, &m1); scale_min_k4(2 * c + 1, sc, &s2, &m2);
                for (int l = 0; l < 32; l++) {
                    int lo = qs[32 * c + l] & 15, hi = qs[32 * c + l] >> 4;
                    if (five) { lo += (qh[l] & (1 << (2 * c))) ? 16 : 0; hi += (qh[l] & (1 << (2 * c + 1))) ? 16 : 0; }
                    o[64 * c + l] = d * s1 * lo - dmin * m1;
                    o[64 * c + 32 + l] = d * s2 * hi - dmin * m2;
                }
            }
        }
        break; }
    default:
        for (int b = 0; b < cols / 256; b++, s += 210, o += 256) {
            uint16_t h; memcpy(&h, s + 208, 2); const double d = h2f(h);
            const uint8_t *ql = s, *qh = s + 128; const int8_t *sc = (const int8_t *)(s + 192);
            double *y = o;
            for (int n = 0; n < 256; n += 128, y += 128, ql += 64, qh += 32, sc += 8)
                for (int l = 0; l < 32; l++) {
                    const int is = l / 16;
                    const int q1 = ((ql[l] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32, q2 = ((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
                    const int q3 = ((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32, q4 = ((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32;
                    y[l] = d * sc[is] * q1; y[l + 32] = d * sc[is + 2] * q2; y[l + 64] = d * sc[is + 4] * q3; y[l + 96] = d * sc[is + 6] * q4;
                }
        }
        break;
    }
}

/* ---------- empacotamento no layout da VRAM (equivalente a gpu_pack_row / pack_job / gpu_build_buf do katana) ---------- */
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
static void pack_row(int k, const uint8_t *src, int cols, uint32_t *o) {
    uint16_t h;
    switch (k) {
    case K_F32: memcpy(o, src, (size_t)cols * 4); return;
    case K_Q4K: memcpy(o, src, (size_t)cols / 256 * 144); return;
    case K_Q5K: memcpy(o, src, (size_t)cols / 256 * 176); return;
    case K_Q8:
        for (int b = 0; b < cols / 32; b++, src += 34, o += 9) {
            memcpy(&h, src, 2);
            float d = h2f(h);
            memcpy(o, &d, 4); memcpy(o + 1, src + 2, 32);
        }
        return;
    default:
        for (int b = 0; b < cols / 256; b++, src += 210, o += 53) {
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
            pack_q6p(qv, (const int8_t *)(src + 192), h2f(h), o);
        }
        return;
    }
}
/* n matrizes de mesmo formato, intercaladas por grupo de 32 linhas */
static uint32_t *build_buf(int k, uint8_t **ms, int n, int rows, int cols, size_t *bytes) {
    const size_t wpr = wpr_of(k, cols), rowsP = ((size_t)rows + 127) & ~(size_t)127, rb = rowbytes(k, cols);
    uint32_t *buf = calloc(rowsP * wpr * n, 4), *tmp = malloc(wpr * 4);
    *bytes = rowsP * wpr * 4 * n;
    for (size_t g = 0; g < rowsP / 32 * n; g++) {
        uint8_t *M = ms[g % n];
        const size_t r0 = (g / n) * 32;
        uint32_t *base = buf + g * wpr * 32;
        for (size_t kk = 0; kk < 32 && r0 + kk < (size_t)rows; kk++) {
            pack_row(k, M + (r0 + kk) * rb, cols, tmp);
            for (size_t w = 0; w < wpr; w++) base[w * 32 + kk] = tmp[w];
        }
    }
    free(tmp);
    return buf;
}

/* ---------- Vulkan ---------- */
static VkDevice dev; static VkPhysicalDevice pd; static VkQueue q;
typedef struct { VkBuffer b; VkDeviceMemory m; VkDeviceSize size; } Buf;
static VkMemoryPropertyFlags HOST = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, DEVL = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
static Buf mkbuf(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want) {
    Buf r; r.size = size;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE };
    CK(vkCreateBuffer(dev, &bci, NULL, &r.b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, r.b, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mt = 99;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) { mt = i; break; }
    if (mt == 99) { printf("sem tipo de memoria\n"); exit(1); }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt };
    CK(vkAllocateMemory(dev, &mai, NULL, &r.m));
    CK(vkBindBufferMemory(dev, r.b, r.m, 0));
    return r;
}
static void rmbuf(Buf *b) { vkDestroyBuffer(dev, b->b, NULL); vkFreeMemory(dev, b->m, NULL); }
#define STORAGE (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT)

static VkCommandBuffer cb; static VkFence fence; static VkSubmitInfo si; static VkCommandBufferBeginInfo cbi;
static void begin(void) { CK(vkResetCommandBuffer(cb, 0)); CK(vkBeginCommandBuffer(cb, &cbi)); }
static void submit_wait(void) { CK(vkEndCommandBuffer(cb)); CK(vkQueueSubmit(q, 1, &si, fence)); CK(vkWaitForFences(dev, 1, &fence, 1, ~0ull)); CK(vkResetFences(dev, 1, &fence)); }

static void upload(Buf *dst, const void *data, size_t n) {
    Buf st = mkbuf(n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HOST);
    void *p; CK(vkMapMemory(dev, st.m, 0, VK_WHOLE_SIZE, 0, &p)); memcpy(p, data, n); vkUnmapMemory(dev, st.m);
    begin(); VkBufferCopy r = { 0, 0, n }; vkCmdCopyBuffer(cb, st.b, dst->b, 1, &r); submit_wait();
    rmbuf(&st);
}
static void download(Buf *src, void *data, size_t n) {
    Buf st = mkbuf(n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, HOST);
    begin(); VkBufferCopy r = { 0, 0, n }; vkCmdCopyBuffer(cb, src->b, st.b, 1, &r); submit_wait();
    void *p; CK(vkMapMemory(dev, st.m, 0, VK_WHOLE_SIZE, 0, &p)); memcpy(data, p, n); vkUnmapMemory(dev, st.m);
    rmbuf(&st);
}

static VkDescriptorSetLayout dsl; static VkPipelineLayout pl; static VkDescriptorPool dpool;
typedef struct { int kind, nr, nseg, var; VkPipeline p; } Pipe;
static Pipe pcache[256]; static int npc;
#define K_ALT K_N      /* variante experimental do Q4_K (mv_q4k_alt.comp) */
static VkShaderModule smod[K_N + 1];
static const uint32_t *spv_ptr[K_N + 1] = { mv_f32_spv, mv_q8_spv, mv_q4k_spv, mv_q5k_spv, mv_q6k_spv, mv_q4k_alt_spv };
static size_t spv_size[K_N + 1] = { sizeof mv_f32_spv, sizeof mv_q8_spv, sizeof mv_q4k_spv, sizeof mv_q5k_spv, sizeof mv_q6k_spv, sizeof mv_q4k_alt_spv };

static VkPipeline get_pipe(int kind, int nr, int nseg) {
    for (int i = 0; i < npc; i++) if (pcache[i].kind == kind && pcache[i].nr == nr && pcache[i].nseg == nseg) return pcache[i].p;
    if (!smod[kind]) {
        VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, spv_size[kind], spv_ptr[kind] };
        CK(vkCreateShaderModule(dev, &smi, NULL, &smod[kind]));
    }
    int sd[2] = { nr, nseg };
    VkSpecializationMapEntry me[2] = { { 0, 0, 4 }, { 1, 4, 4 } };
    VkSpecializationInfo sp = { 2, me, sizeof sd, sd };
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = smod[kind]; cpi.stage.pName = "main"; cpi.stage.pSpecializationInfo = &sp; cpi.layout = pl;
    VkPipeline p; CK(vkCreateComputePipelines(dev, NULL, 1, &cpi, NULL, &p));
    Pipe e = { kind, nr, nseg, 0, p }; pcache[npc++] = e;
    return p;
}

static VkDescriptorSet mkset(Buf *w, Buf *x, Buf *o, Buf *bi) {
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dpool, 1, &dsl };
    VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev, &dai, &ds));
    Buf *bs[4] = { w, x, o, bi };
    VkDescriptorBufferInfo dbi[4]; VkWriteDescriptorSet wr[4];
    for (int i = 0; i < 4; i++) {
        dbi[i].buffer = bs[i]->b; dbi[i].offset = 0; dbi[i].range = VK_WHOLE_SIZE;
        memset(&wr[i], 0, sizeof wr[i]); wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = ds; wr[i].dstBinding = i;
        wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[i].pBufferInfo = &dbi[i];
    }
    vkUpdateDescriptorSets(dev, 4, wr, 0, NULL);
    return ds;
}

/* ---------- casos de teste ---------- */
typedef struct { const char *name; int rows, cols, n, mode, gs, go, pair, sweep; } Case;
static const Case cases[] = {
    { "1536x1536",               1536, 1536, 1, 0, 1, 0, 0, 0 },
    { "256x1536 (segmentos)",     256, 1536, 1, 0, 1, 0, 0, 1 },
    { "8960x1536 par gate/up",   8960, 1536, 2, 8, 1, 0, 1, 0 },
    { "1536x8960 residual+bias", 1536, 8960, 1, 5, 1, 0, 0, 0 },
    { "1536x1536 gs=2 go=1",     1536, 1536, 2, 0, 2, 1, 0, 0 },
    { "1536x1536 swiglu+bias",   1536, 1536, 1, 6, 1, 0, 0, 0 },
};
#define NCASES ((int)(sizeof cases / sizeof cases[0]))

static size_t SEG_TARGET = 16384;
/* igual ao gpu_nseg do katana */
static int pick_nseg(size_t threads, int nb, int lxl) {
    const int maxseg = lxl > 32 ? 4 : 8; int best = 1;
    if (!SEG_TARGET || nb <= 1 || threads >= SEG_TARGET) return 1;
    for (int sg = 1; sg <= maxseg && sg <= nb; sg++) if (nb % sg == 0) { best = sg; if (threads * sg >= SEG_TARGET) break; }
    return best;
}

typedef struct { int cols, mode, rows, gs, go; } PCs;

static int total_fail, total_run;
static int do_bench = 1, do_alt = 1;
static double bench_best[K_N][NCASES][3];   /* [tipo][caso][nr] em G pesos/s */

/* layout alternativo: linhas inteiras em ordem, matrizes intercaladas por linha (gate 2r, up 2r+1) */
static uint32_t *build_rm(int k, uint8_t **ms, int n, int rows, int cols, size_t *bytes) {
    const size_t rb = rowbytes(k, cols); *bytes = rb * rows * n;
    uint8_t *b = malloc(*bytes);
    for (int r = 0; r < rows; r++) for (int m = 0; m < n; m++) memcpy(b + ((size_t)r * n + m) * rb, ms[m] + (size_t)r * rb, rb);
    return (uint32_t *)b;
}

static void run_kind(int kind, int var) {
    const int bits = kind;
    (void)bits;
    for (int ci = 0; ci < NCASES; ci++) {
        const Case *c = &cases[ci];
        const int rows = c->rows, cols = c->cols, n = c->n;
        if ((kind == K_Q8 ? cols % 32 : kind == K_F32 ? 0 : cols % 256) != 0) continue;
        if (var && (kind != K_Q4K || c->gs != 1 || (c->n == 2 && !c->pair))) continue;
        const size_t rb = rowbytes(kind, cols), rowsP = ((size_t)rows + 127) & ~(size_t)127;
        uint8_t *ms[2];
        for (int m = 0; m < n; m++) { ms[m] = malloc((size_t)rows * rb); for (int r = 0; r < rows; r++) gen_row(kind, cols, ms[m] + (size_t)r * rb); }
        float *x = malloc((size_t)cols * 4), *bias = malloc((size_t)rows * 4), *out0 = malloc((size_t)rows * 4), *got = malloc((size_t)rows * 4);
        for (int i = 0; i < cols; i++) x[i] = rndsym();
        for (int i = 0; i < rows; i++) { bias[i] = 0.5f * rndsym(); out0[i] = rndsym(); }
        /* referencia em double */
        double *ref = malloc((size_t)rows * 8), *wrow = malloc((size_t)cols * 8), *wrow2 = malloc((size_t)cols * 8);
        for (int r = 0; r < rows; r++) {
            double t = 0, t2 = 0;
            if (c->pair) {
                dq_row(kind, ms[0] + (size_t)r * rb, cols, wrow); dq_row(kind, ms[1] + (size_t)r * rb, cols, wrow2);
                for (int i = 0; i < cols; i++) { t += wrow[i] * x[i]; t2 += wrow2[i] * x[i]; }
                ref[r] = t / (1.0 + exp(-t)) * t2;
            } else {
                dq_row(kind, ms[c->go] + (size_t)r * rb, cols, wrow);
                for (int i = 0; i < cols; i++) t += wrow[i] * x[i];
                if (c->mode & 4) t += bias[r];
                if ((c->mode & 3) == 1) t += out0[r];
                else if ((c->mode & 3) == 2) { double g = out0[r]; t = g / (1.0 + exp(-g)) * t; }
                ref[r] = t;
            }
        }
        double rms = 0; for (int r = 0; r < rows; r++) rms += ref[r] * ref[r]; rms = sqrt(rms / rows);
        size_t wbytes; uint32_t *wp = var ? build_rm(kind, ms, n, rows, cols, &wbytes) : build_buf(kind, ms, n, rows, cols, &wbytes);
        Buf bw = mkbuf(wbytes, STORAGE, DEVL), bx = mkbuf((size_t)cols * 4, STORAGE, DEVL), bo = mkbuf((size_t)rows * 4, STORAGE, DEVL), bb = mkbuf((size_t)rows * 4, STORAGE, DEVL);
        upload(&bw, wp, wbytes); upload(&bx, x, (size_t)cols * 4); upload(&bb, bias, (size_t)rows * 4);
        free(wp);
        /* copias dos pesos para a medida (fora da L2) */
        int ncopy = (int)(160ull * 1024 * 1024 / wbytes); if (ncopy < 1) ncopy = 1; if (ncopy > 48) ncopy = 48;
        Buf *cw = malloc(sizeof(Buf) * ncopy); VkDescriptorSet *sets = malloc(sizeof(VkDescriptorSet) * ncopy);
        cw[0] = bw; sets[0] = mkset(&bw, &bx, &bo, &bb);
        if (do_bench) for (int i = 1; i < ncopy; i++) {
            cw[i] = mkbuf(wbytes, STORAGE, DEVL);
            begin(); VkBufferCopy r = { 0, 0, wbytes }; vkCmdCopyBuffer(cb, bw.b, cw[i].b, 1, &r); submit_wait();
            sets[i] = mkset(&cw[i], &bx, &bo, &bb);
        } else ncopy = 1;

        for (int vi = 0; vi < (var ? 4 : 2); vi++) {
            const int nr = var ? (1 << vi) : vi + 1;     /* var=1: nr = subgrupos (linhas) por grupo de trabalho */
            const int NRk = var ? (c->pair ? 2 : 1) : c->pair ? 2 * nr : nr;
            const size_t threads = rowsP / nr, gx = var ? ((size_t)rows + nr - 1) / nr : threads / 32;
            const int nb = nblocks(kind, cols), auto_seg = pick_nseg(threads, nb, c->pair ? nr * 2 : nr);
            int segs[6], ns = 0;
            segs[ns++] = kind == K_F32 || var ? 1 : auto_seg;
            if (c->sweep && kind != K_F32 && !var) { int cand[4] = { 1, 2, 3, 6 }; for (int i = 0; i < 4; i++) if (nb % cand[i] == 0 && cand[i] != auto_seg) segs[ns++] = cand[i]; }
            for (int si_ = 0; si_ < ns; si_++) {
                const int nseg = segs[si_];
                VkPipeline pipe = var ? get_pipe(K_ALT, NRk, nr) : get_pipe(kind, NRk, nseg);
                PCs pc = { cols, c->mode, rows, c->gs, c->go };
                /* corretude */
                upload(&bo, out0, (size_t)rows * 4);
                begin();
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &sets[0], 0, NULL);
                vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
                vkCmdDispatch(cb, (uint32_t)gx, 1, 1);
                submit_wait();
                download(&bo, got, (size_t)rows * 4);
                double maxrel = 0, maxn = 0; int bad = 0;
                for (int r = 0; r < rows; r++) {
                    const double e = fabs(got[r] - ref[r]), rel = e / (fabs(ref[r]) + 1e-2 * rms);
                    if (!(rel <= maxrel)) maxrel = rel;      /* pega NaN */
                    if (!(e / rms <= maxn)) maxn = e / rms;
                    if (!(rel < 1e-3)) bad++;
                }
                const int ok = bad == 0 && maxn < 1e-4;   /* f32 soma sequencial de ate 8960 termos: erro absoluto relativo ao rms ~1e-5 */
                total_run++; if (!ok) total_fail++;
                /* vazao */
                double gws = 0, ms_ = 0;
                if (do_bench && si_ == 0) {
                    const int ITER = 150;
                    begin();
                    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
                    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
                    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
                    for (int i = 0; i < ITER; i++) {
                        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &sets[i % ncopy], 0, NULL);
                        vkCmdDispatch(cb, (uint32_t)gx, 1, 1);
                        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
                    }
                    CK(vkEndCommandBuffer(cb));
                    double best = 1e9;
                    for (int rep = 0; rep < 4; rep++) {
                        double t0 = now();
                        CK(vkQueueSubmit(q, 1, &si, fence)); CK(vkWaitForFences(dev, 1, &fence, 1, ~0ull)); CK(vkResetFences(dev, 1, &fence));
                        double dt = (now() - t0) / ITER;
                        if (rep > 0 && dt < best) best = dt;     /* a primeira so aquece */
                    }
                    ms_ = best * 1e3;
                    gws = (double)rows * cols * (c->pair ? 2 : 1) / best / 1e9;
                    bench_best[kind][ci][nr] = gws;
                }
                printf("%-5s%s %-26s %s=%d nseg=%d  maxrel=%.2e maxabs/rms=%.2e  %s", kdesc[kind], var ? "-alt" : "", c->name, var ? "rpw" : "nr", nr, nseg, maxrel, maxn, ok ? "OK" : "FALHOU");
                if (gws > 0) printf("  %.3f ms  %.2f G pesos/s", ms_, gws);
                printf("\n");
                fflush(stdout);
            }
        }
        for (int i = 1; i < ncopy; i++) rmbuf(&cw[i]);
        rmbuf(&bw); rmbuf(&bx); rmbuf(&bo); rmbuf(&bb);
        vkResetDescriptorPool(dev, dpool, 0);
        free(cw); free(sets); free(x); free(bias); free(out0); free(got); free(ref); free(wrow); free(wrow2);
        for (int m = 0; m < n; m++) free(ms[m]);
    }
}

int main(int argc, char **argv) {
    const char *only = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q")) do_bench = 0;
        else if (!strcmp(argv[i], "-n")) do_alt = 0;
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) only = argv[++i];
    }
    if (getenv("VKMV_SEG")) SEG_TARGET = (size_t)atoll(getenv("VKMV_SEG"));
    HMODULE h = LoadLibraryA("vulkan-1.dll");
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(h, "vkGetInstanceProcAddr");
#define GI(n) n = (PFN_##n)vkGetInstanceProcAddr(inst, #n)
    VkInstance inst = NULL;
    GI(vkCreateInstance);
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkmv", 1, "none", 1, VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai };
    CK(vkCreateInstance(&ici, NULL, &inst));
    GI(vkEnumeratePhysicalDevices); GI(vkGetPhysicalDeviceProperties); GI(vkGetPhysicalDeviceQueueFamilyProperties);
    GI(vkCreateDevice); GI(vkGetDeviceProcAddr); GI(vkGetPhysicalDeviceMemoryProperties); GI(vkGetPhysicalDeviceProperties2);
    VkPhysicalDevice pds[8]; uint32_t npd = 8;
    vkEnumeratePhysicalDevices(inst, &npd, pds);
    for (uint32_t i = 0; i < npd; i++) {
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pds[i], &pp);
        if (!pd || pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) pd = pds[i];
    }
    {
        VkPhysicalDeviceSubgroupProperties sgp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sgp };
        vkGetPhysicalDeviceProperties2(pd, &p2);
        printf("GPU: %s  subgrupo=%u  maxStorage=%u MB\n", p2.properties.deviceName, sgp.subgroupSize, p2.properties.limits.maxStorageBufferRange >> 20);
    }
    VkQueueFamilyProperties qf[16]; uint32_t nq = 16, qi = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qi = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qi, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci };
    CK(vkCreateDevice(pd, &dci, NULL, &dev));
#define GD(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    GD(vkGetDeviceQueue); GD(vkCreateBuffer); GD(vkGetBufferMemoryRequirements); GD(vkAllocateMemory); GD(vkBindBufferMemory);
    GD(vkMapMemory); GD(vkCreateShaderModule); GD(vkCreateDescriptorSetLayout); GD(vkCreatePipelineLayout);
    GD(vkCreateComputePipelines); GD(vkCreateDescriptorPool); GD(vkAllocateDescriptorSets); GD(vkUpdateDescriptorSets);
    GD(vkCreateCommandPool); GD(vkAllocateCommandBuffers); GD(vkBeginCommandBuffer); GD(vkCmdBindPipeline);
    GD(vkCmdBindDescriptorSets); GD(vkCmdPushConstants); GD(vkCmdDispatch); GD(vkEndCommandBuffer); GD(vkCmdCopyBuffer);
    GD(vkQueueSubmit); GD(vkCreateFence); GD(vkWaitForFences); GD(vkResetFences); GD(vkResetCommandBuffer);
    GD(vkCmdPipelineBarrier); GD(vkDestroyBuffer); GD(vkFreeMemory); GD(vkResetDescriptorPool); GD(vkUnmapMemory);
    vkGetDeviceQueue(dev, qi, 0, &q);

    VkDescriptorSetLayoutBinding b[4];
    for (int i = 0; i < 4; i++) { memset(&b[i], 0, sizeof b[i]); b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 4, b };
    CK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PCs) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pcr };
    CK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * 128 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 128, 1, &ps };
    CK(vkCreateDescriptorPool(dev, &dpi, NULL, &dpool));
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qi };
    VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    CK(vkCreateFence(dev, &fci, NULL, &fence));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    for (int k = 0; k < K_N; k++) if (!only || !strcmp(only, kname[k])) run_kind(k, 0);
    if (do_alt && (!only || !strcmp(only, "q4k"))) run_kind(K_Q4K, 1);
    printf("\nresumo: %d execucoes verificadas, %d falhas\n", total_run, total_fail);
    return total_fail != 0;
}
