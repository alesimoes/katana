/* vkmm: produto matriz x lote com pesos quantizados (Q4_K, Q6_K, Q5_K, Q8_0, F32) no layout do katana,
   decodificados ao encher o ladrilho compartilhado; matrizes cooperativas fp16 x fp16 -> fp32.
   Y[rows][tokens] = W[rows][cols] * X[cols][tokens]. Os shaders vem de spv/ (gen2.sh).
   Uso: vkmm [-t q4,q6,q5,q8,f32] [-s RxC,...] [-b n,n,...] [-tiles 64x64,...|all] [-mode plain|add|glu]
             [-x 0|1|cvt] [-sc 0|1] [-splits 1,2,4] [-nocheck] [-reps n] */
#define VK_NO_PROTOTYPES
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <time.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "vulkan/vulkan.h"

#define F(name) static PFN_##name name
F(vkGetInstanceProcAddr); F(vkCreateInstance); F(vkEnumeratePhysicalDevices); F(vkGetPhysicalDeviceProperties);
F(vkGetPhysicalDeviceQueueFamilyProperties); F(vkCreateDevice); F(vkGetDeviceProcAddr); F(vkGetDeviceQueue);
F(vkGetPhysicalDeviceMemoryProperties); F(vkCreateBuffer); F(vkGetBufferMemoryRequirements); F(vkAllocateMemory);
F(vkBindBufferMemory); F(vkMapMemory); F(vkCreateShaderModule); F(vkCreateDescriptorSetLayout); F(vkCreatePipelineLayout);
F(vkCreateComputePipelines); F(vkCreateDescriptorPool); F(vkAllocateDescriptorSets); F(vkUpdateDescriptorSets);
F(vkCreateCommandPool); F(vkAllocateCommandBuffers); F(vkBeginCommandBuffer); F(vkCmdBindPipeline);
F(vkCmdBindDescriptorSets); F(vkCmdPushConstants); F(vkCmdDispatch); F(vkEndCommandBuffer); F(vkCmdCopyBuffer);
F(vkCmdPipelineBarrier); F(vkQueueSubmit); F(vkCreateFence); F(vkWaitForFences); F(vkResetFences); F(vkResetCommandBuffer);
F(vkDestroyBuffer); F(vkFreeMemory);

#ifdef _WIN32
static double now(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / f.QuadPart; }
#else
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
#endif
#define CK(x) do { VkResult r_ = (x); if (r_) { printf("erro %d em %s\n", r_, #x); exit(1); } } while (0)

static uint16_t f2h(float f) {
    uint32_t u; memcpy(&u, &f, 4);
    uint32_t s = (u >> 16) & 0x8000; int e = (int)((u >> 23) & 0xFF) - 127 + 15; uint32_t m = u & 0x7FFFFF;
    if (e <= 0) { if (e < -10) return (uint16_t)s; m |= 0x800000; return (uint16_t)(s | (m >> (14 - e))); }
    if (e >= 31) return (uint16_t)(s | 0x7C00);
    { uint32_t r = ((uint32_t)e << 10) | (m >> 13); const uint32_t rem = m & 0x1FFF; if (rem > 0x1000 || (rem == 0x1000 && (r & 1))) r++; if (r >= 0x7C00) r = 0x7C00; return (uint16_t)(s | r); }
}
static float h2f(uint16_t h) {
    uint32_t s = (h >> 15) & 1, m = h & 0x3ff, u; int e = (h >> 10) & 0x1f;
    if (e == 0) { if (!m) u = s << 31; else { e = 1; while (!(m & 0x400)) { m <<= 1; e--; } m &= 0x3ff; u = (s << 31) | ((uint32_t)(e + 112) << 23) | (m << 13); } }
    else u = (s << 31) | ((uint32_t)(e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &u, 4); return f;
}
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

/* ---------- tipos de peso: 0 Q4_K, 1 Q6_K, 2 Q5_K, 3 Q8_0, 4 F32 ---------- */
static const char *TN[] = { "Q4_K", "Q6_K", "Q5_K", "Q8_0", "F32" };
static size_t type_wpr(int t, size_t cols) { return t == 0 ? cols / 256 * 36 : t == 1 ? cols / 256 * 53 : t == 2 ? cols / 256 * 44 : t == 3 ? cols / 32 * 9 : cols; }

static void k4_scale_min(int j, const uint8_t *q, int *d, int *m) {
    if (j < 4) { *d = q[j] & 63; *m = q[j + 4] & 63; }
    else { *d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4); *m = (q[j + 4] >> 4) | ((q[j] >> 6) << 4); }
}
static void dequant_q4k(const uint8_t *src, float *out, size_t n) {
    for (size_t b = 0; b < n / 256; b++, src += 144) {
        uint16_t h0, h1; memcpy(&h0, src, 2); memcpy(&h1, src + 2, 2);
        const float d = h2f(h0), dmin = h2f(h1);
        const uint8_t *sc = src + 4, *q = src + 16;
        for (int j = 0; j < 8; j += 2, q += 32) {
            int s1, m1, s2, m2;
            k4_scale_min(j, sc, &s1, &m1); k4_scale_min(j + 1, sc, &s2, &m2);
            for (int l = 0; l < 32; l++) *out++ = d * s1 * (q[l] & 0xF) - dmin * m1;
            for (int l = 0; l < 32; l++) *out++ = d * s2 * (q[l] >> 4) - dmin * m2;
        }
    }
}
static void dequant_q5k(const uint8_t *src, float *out, size_t n) {
    for (size_t b = 0; b < n / 256; b++, src += 176) {
        uint16_t h0, h1; memcpy(&h0, src, 2); memcpy(&h1, src + 2, 2);
        const float d = h2f(h0), dmin = h2f(h1);
        const uint8_t *sc = src + 4, *qh = src + 16, *ql = src + 48;
        int u1 = 1, u2 = 2;
        for (int j = 0; j < 8; j += 2, ql += 32, u1 <<= 2, u2 <<= 2) {
            int s1, m1, s2, m2;
            k4_scale_min(j, sc, &s1, &m1); k4_scale_min(j + 1, sc, &s2, &m2);
            for (int l = 0; l < 32; l++) *out++ = d * s1 * ((ql[l] & 0xF) + ((qh[l] & u1) ? 16 : 0)) - dmin * m1;
            for (int l = 0; l < 32; l++) *out++ = d * s2 * ((ql[l] >> 4) + ((qh[l] & u2) ? 16 : 0)) - dmin * m2;
        }
    }
}
static void dequant_q6k(const uint8_t *src, float *out, size_t n) {
    for (size_t b = 0; b < n / 256; b++, src += 210) {
        const uint8_t *ql = src, *qh = src + 128;
        const int8_t *sc = (const int8_t *)(src + 192);
        uint16_t h; memcpy(&h, src + 208, 2);
        const float d = h2f(h);
        for (int c = 0; c < 2; c++, out += 128, ql += 64, qh += 32, sc += 8) {
            for (int l = 0; l < 32; l++) {
                const int is = l / 16;
                const int q1 = (int)((ql[l] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
                const int q2 = (int)((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
                const int q3 = (int)((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32;
                const int q4 = (int)((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32;
                out[l] = d * sc[is] * q1; out[l + 32] = d * sc[is + 2] * q2;
                out[l + 64] = d * sc[is + 4] * q3; out[l + 96] = d * sc[is + 6] * q4;
            }
        }
    }
}
static void pack_q6p(const uint8_t *qv, const int8_t *sc, float d, uint32_t *o) {
    for (int g = 0; g < 16; g++) {
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
static void pack_q6row(const uint8_t *src, size_t cols, uint32_t *o) {
    uint16_t h;
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
        pack_q6p(qv, (const int8_t *)(src + 192), h2f(h), o);
    }
}

/* gera uma linha aleatoria: palavras do layout da GPU em o[wpr] e pesos decodificados em wd[cols] */
static void gen_row(int t, size_t cols, uint32_t *o, float *wd) {
    const size_t nb = cols / 256;
    uint8_t raw[64 * 1024];
    if (t == 3) {
        for (size_t b = 0; b < cols / 32; b++) {
            const float d = 0.001f + (rnd() % 1000) * 2e-5f; memcpy(o + b * 9, &d, 4);
            int8_t q[32]; for (int i = 0; i < 32; i++) q[i] = (int8_t)(rnd() & 255);
            memcpy(o + b * 9 + 1, q, 32);
            for (int i = 0; i < 32; i++) wd[b * 32 + i] = d * q[i];
        }
        return;
    }
    if (t == 4) { for (size_t i = 0; i < cols; i++) { const float v = ((int)(rnd() % 2001) - 1000) * 5e-5f; memcpy(o + i, &v, 4); wd[i] = v; } return; }
    const size_t bs = t == 0 ? 144 : t == 2 ? 176 : 210;
    for (size_t b = 0; b < nb; b++) {
        uint8_t *p = raw + b * bs;
        for (size_t i = 0; i < bs; i++) p[i] = (uint8_t)rnd();
        const uint16_t d = f2h(0.0005f + (rnd() % 1000) * 1e-5f), dm = f2h(0.0005f + (rnd() % 1000) * 1e-5f);
        if (t == 1) { int8_t *sc = (int8_t *)(p + 192); for (int i = 0; i < 16; i++) sc[i] = (int8_t)((int)(rnd() % 64) - 32); memcpy(p + 208, &d, 2); }
        else { memcpy(p, &d, 2); memcpy(p + 2, &dm, 2); }
    }
    if (t == 0) { memcpy(o, raw, nb * 144); dequant_q4k(raw, wd, cols); }
    else if (t == 2) { memcpy(o, raw, nb * 176); dequant_q5k(raw, wd, cols); }
    else { pack_q6row(raw, cols, o); dequant_q6k(raw, wd, cols); }
}

/* ---------- Vulkan ---------- */
static VkDevice dev; static VkPhysicalDevice pd;
static VkBuffer mkbuf(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, VkDeviceMemory *mem) {
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer b; CK(vkCreateBuffer(dev, &bci, NULL, &b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mt = 99;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) { mt = i; break; }
    if (mt == 99) { printf("sem tipo de memoria\n"); exit(1); }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt };
    CK(vkAllocateMemory(dev, &mai, NULL, mem));
    CK(vkBindBufferMemory(dev, b, *mem, 0));
    return b;
}

static VkQueue q; static VkCommandBuffer cb; static VkFence fence; static VkSubmitInfo si; static VkCommandBufferBeginInfo bi;
static VkDescriptorSetLayout dl; static VkPipelineLayout pl; static VkDescriptorPool dp;
static VkPipeline redpipe, cvtpipe;
#define RUN() do { CK(vkEndCommandBuffer(cb)); CK(vkQueueSubmit(q, 1, &si, fence)); CK(vkWaitForFences(dev, 1, &fence, 1, ~0ull)); CK(vkResetFences(dev, 1, &fence)); } while (0)

static VkPipeline mkpipe(const char *file) {
    FILE *f = fopen(file, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *spv = malloc(n); if (fread(spv, 1, n, f) != (size_t)n) exit(2); fclose(f);
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, (size_t)n, spv };
    VkShaderModule sm; CK(vkCreateShaderModule(dev, &smi, NULL, &sm));
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = sm; cpi.stage.pName = "main"; cpi.layout = pl;
    VkPipeline pipe; CK(vkCreateComputePipelines(dev, NULL, 1, &cpi, NULL, &pipe));
    free(spv);
    return pipe;
}

static uint8_t *hstage; static float *hy; static VkBuffer stage, yhost; static const size_t STAGE_BYTES = 256u << 20;
static void upload(VkBuffer dst, size_t bytes) {
    CK(vkResetCommandBuffer(cb, 0)); CK(vkBeginCommandBuffer(cb, &bi));
    VkBufferCopy r = { 0, 0, bytes }; vkCmdCopyBuffer(cb, stage, dst, 1, &r); RUN();
}
#define DEVL (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT)
static VkDescriptorSet mkset(VkBuffer a, VkBuffer b, VkBuffer c) {
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dl };
    VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev, &dai, &ds));
    VkDescriptorBufferInfo di[3] = { { a, 0, VK_WHOLE_SIZE }, { b, 0, VK_WHOLE_SIZE }, { c, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet wr[3];
    for (int i = 0; i < 3; i++) { memset(&wr[i], 0, sizeof wr[i]); wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = ds; wr[i].dstBinding = i; wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[i].pBufferInfo = &di[i]; }
    vkUpdateDescriptorSets(dev, 3, wr, 0, NULL);
    return ds;
}
static void barrier_cs(void) {
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* ---------- ladrilhos ---------- */
typedef struct { const char *name; int bm, bn, glu_ok; } Tile;
static const Tile TILES[] = { { "64x64", 64, 64, 1 }, { "128x16", 128, 16, 1 }, { "64x16", 64, 16, 1 }, { "128x32", 128, 32, 1 },
                              { "64x32", 64, 32, 1 }, { "64x16w4", 64, 16, 0 }, { "64x32w4", 64, 32, 0 },
                              { "64x64w8", 64, 64, 0 }, { "64x32s22", 64, 32, 1 }, { "128x32w8", 128, 32, 1 }, { "128x64w8", 128, 64, 1 } };
#define NTILES ((int)(sizeof TILES / sizeof TILES[0]))

static struct { int type, mode /*0 plain 1 add 2 glu*/, check, reps, sc; int xmode /*0 fp16, 1 fp32 direto, 2 fp32 + conversao*/; } opt = { 0, 0, 1, 20, 0, 0 };
static int nfail = 0, nok = 0;

typedef struct { int type; uint32_t rows, cols; size_t wpr; int gs; VkBuffer bw; VkDeviceMemory mw; float *Wd[2]; } WSet;

static void wset_build(WSet *w, int type, uint32_t rows, uint32_t cols, int gs) {
    w->type = type; w->rows = rows; w->cols = cols; w->gs = gs; w->wpr = type_wpr(type, cols);
    const size_t rowsP = (rows + 31) & ~31u, bytes = rowsP * w->wpr * 4 * gs;
    if (bytes > STAGE_BYTES) { printf("grande demais\n"); exit(1); }
    uint32_t *pk = (uint32_t *)hstage;
    memset(pk, 0, bytes);
    for (int m = 0; m < gs; m++) {
        w->Wd[m] = malloc((size_t)rows * cols * 4);
        uint32_t *tmp = malloc(w->wpr * 4);
        for (uint32_t r = 0; r < rows; r++) {
            gen_row(type, cols, tmp, w->Wd[m] + (size_t)r * cols);
            uint32_t *base = pk + ((size_t)(r / 32) * gs + m) * w->wpr * 32;
            for (size_t k = 0; k < w->wpr; k++) base[k * 32 + r % 32] = tmp[k];
        }
        free(tmp);
        for (size_t i = 0; i < (size_t)rows * cols; i++) w->Wd[m][i] = h2f(f2h(w->Wd[m][i]));   /* como a GPU enxerga */
    }
    w->bw = mkbuf(bytes, DEVL, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &w->mw);
    upload(w->bw, bytes);
}
static void wset_free(WSet *w) { vkDestroyBuffer(dev, w->bw, NULL); vkFreeMemory(dev, w->mw, NULL); for (int m = 0; m < w->gs; m++) free(w->Wd[m]); }

static double silu(double g) { return g / (1.0 + exp(-g)); }

/* um lote: gera X, referencia, e roda todas as variantes pedidas */
static void run_batch(const WSet *w, uint32_t tokens, const char **tiles, int ntiles, const int *splits, int nsplits) {
    const uint32_t rows = w->rows, cols = w->cols;
    const int glu = opt.mode == 2, add = opt.mode == 1;
    const size_t xn = (size_t)cols * tokens, yn = (size_t)rows * tokens;
    /* ativacoes: [col*tokens + token], valores ja representaveis em fp16 */
    float *X = malloc(xn * 4);
    for (size_t i = 0; i < xn; i++) X[i] = h2f(f2h(((int)(rnd() % 2001) - 1000) * 0.001f));
    VkDeviceMemory m1, m2, m3, m4;
    VkBuffer bx32 = mkbuf(xn * 4, DEVL, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &m1);
    VkBuffer bx16 = mkbuf((xn * 2 + 15) & ~15ull, DEVL, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &m2);
    VkBuffer by = mkbuf(yn * 4, DEVL, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &m3);
    VkBuffer bp = mkbuf(yn * 4 * 8, DEVL, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &m4);
    memcpy(hstage, X, xn * 4); upload(bx32, xn * 4);
    { uint16_t *h = (uint16_t *)hstage; for (size_t i = 0; i < xn; i++) h[i] = f2h(X[i]); upload(bx16, xn * 2); }
    /* referencia (double): o produto, e Y0 inicial */
    float *Y0 = malloc(yn * 4);
    for (size_t i = 0; i < yn; i++) Y0[i] = add ? ((int)(rnd() % 2001) - 1000) * 0.001f : 777.0f;
    double *ref = NULL, rms = 1;
    if (opt.check) {
        float *Xt = malloc(xn * 4);
        for (size_t k = 0; k < cols; k++) for (uint32_t t = 0; t < tokens; t++) Xt[(size_t)t * cols + k] = X[k * tokens + t];
        ref = malloc(yn * 8);
        double ss = 0; const int nr = (int)rows; int r;
#pragma omp parallel for reduction(+:ss) schedule(static)
        for (r = 0; r < nr; r++) { for (uint32_t t = 0; t < tokens; t++) {
            const float *x = Xt + (size_t)t * cols;
            double a = 0, b = 0; const float *wa = w->Wd[0] + (size_t)r * cols;
            for (uint32_t k = 0; k < cols; k++) a += (double)wa[k] * x[k];
            if (glu) { const float *wb = w->Wd[1] + (size_t)r * cols; for (uint32_t k = 0; k < cols; k++) b += (double)wb[k] * x[k]; a = silu(a) * b; }
            ref[(size_t)r * tokens + t] = a; ss += a * a;
        } }
        rms = sqrt(ss / (double)yn);
        free(Xt);
    }
    for (int ti = 0; ti < ntiles; ti++) {
        const Tile *tl = NULL;
        for (int i = 0; i < NTILES; i++) if (!strcmp(TILES[i].name, tiles[ti])) tl = &TILES[i];
        if (!tl || (glu && !tl->glu_ok)) continue;
        char fn[128];
        snprintf(fn, sizeof fn, "spv/t%d_%s_x%d_s%d_g%d.spv", w->type, tl->name, opt.xmode == 1, opt.sc, glu);
        VkPipeline pipe = mkpipe(fn);
        if (!pipe) { printf("falta %s\n", fn); continue; }
        VkBuffer xin = opt.xmode == 1 ? bx32 : bx16;
        VkDescriptorSet dsY = mkset(w->bw, xin, by), dsP = mkset(w->bw, xin, bp), dsR = mkset(w->bw, bp, by), dsC = mkset(w->bw, bx32, bx16);
        const uint32_t gm = glu ? tl->bm / 2 : tl->bm;
        const uint32_t nblk = cols / 256;
        for (int si_ = 0; si_ < nsplits; si_++) {
            const int split = splits[si_];
            const uint32_t kper = ((nblk + split - 1) / split) * 256;
            if (split > 1 && (glu || (cols + kper - 1) / kper != (uint32_t)split)) continue;
            const uint32_t pc[8] = { rows, cols, tokens, (uint32_t)w->wpr, kper, rows * tokens, split > 1 ? 0u : (uint32_t)add, 0 };
            const uint32_t pr[4] = { rows * tokens, (uint32_t)split, rows * tokens, (uint32_t)add };
            const uint32_t n4 = (uint32_t)(xn / 4);
            int goodcvt = !(opt.xmode == 2 && (xn & 3));
            if (!goodcvt) { printf("conversao exige cols*tokens multiplo de 4\n"); continue; }
#define RECORD() do { \
            if (opt.xmode == 2) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cvtpipe); vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dsC, 0, NULL); \
                vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &n4); vkCmdDispatch(cb, (n4 + 255) / 256, 1, 1); barrier_cs(); } \
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe); \
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, split > 1 ? &dsP : &dsY, 0, NULL); \
            vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, pc); \
            vkCmdDispatch(cb, (rows + gm - 1) / gm, (tokens + tl->bn - 1) / tl->bn, split); \
            if (split > 1) { barrier_cs(); vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, redpipe); \
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dsR, 0, NULL); \
                vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pr); vkCmdDispatch(cb, (pr[0] + 255) / 256, 1, 1); } \
            barrier_cs(); } while (0)
            double worst = 0, worst_rms = 0; size_t bad = 0;
            if (opt.check) {
                memcpy(hstage, Y0, yn * 4); upload(by, yn * 4);
                CK(vkResetCommandBuffer(cb, 0)); CK(vkBeginCommandBuffer(cb, &bi));
                RECORD();
                { VkBufferCopy r = { 0, 0, yn * 4 }; vkCmdCopyBuffer(cb, by, yhost, 1, &r); } RUN();
                for (size_t i = 0; i < yn; i++) {
                    const double g = hy[i], rf = ref[i] + (add ? Y0[i] : 0.0), e = fabs(g - rf);
                    if (!(e == e)) { bad++; continue; }
                    const double er = e / (fabs(rf) + 0.05 * rms);
                    if (er > worst) worst = er;
                    if (e / rms > worst_rms) worst_rms = e / rms;
                }
            }
            /* tempo: opt.reps repeticoes gravadas num comando */
            CK(vkResetCommandBuffer(cb, 0)); CK(vkBeginCommandBuffer(cb, &bi));
            for (int i = 0; i < opt.reps; i++) RECORD();
            CK(vkEndCommandBuffer(cb));
            for (int i = 0; i < 3; i++) { CK(vkQueueSubmit(q, 1, &si, fence)); CK(vkWaitForFences(dev, 1, &fence, 1, ~0ull)); CK(vkResetFences(dev, 1, &fence)); }
            double best = 1e9;
            for (int rep = 0; rep < 5; rep++) {
                const double t0 = now();
                CK(vkQueueSubmit(q, 1, &si, fence)); CK(vkWaitForFences(dev, 1, &fence, 1, ~0ull)); CK(vkResetFences(dev, 1, &fence));
                const double dt = (now() - t0) / opt.reps; if (dt < best) best = dt;
            }
            const double macs = (double)rows * cols * tokens * (glu ? 2 : 1);
            const int ok = opt.check ? (worst < 1e-3 && !bad) : 1;
            if (opt.check) { if (ok) nok++; else nfail++; }
            printf("%-4s %-7s %-6s%s%s sp%d %5u x %5u lote %3u: %.4f ms  %.2f T%s", TN[w->type], tl->name, opt.mode == 2 ? "glu" : opt.mode == 1 ? "add" : "plain",
                   opt.xmode == 1 ? " x32" : opt.xmode == 2 ? " cvt" : " x16", opt.sc ? " tab" : "", split, rows, cols, tokens, best * 1e3, macs / best / 1e12, "");
            if (opt.check) printf("  erro/rms %.1e relmax %.1e nan %zu %s", worst_rms, worst, bad, ok ? "(OK)" : "(DIFERENTE)");
            printf("\n"); fflush(stdout);
        }
    }
    free(X); free(Y0); free(ref);
    vkDestroyBuffer(dev, bx32, NULL); vkDestroyBuffer(dev, bx16, NULL); vkDestroyBuffer(dev, by, NULL); vkDestroyBuffer(dev, bp, NULL);
    vkFreeMemory(dev, m1, NULL); vkFreeMemory(dev, m2, NULL); vkFreeMemory(dev, m3, NULL); vkFreeMemory(dev, m4, NULL);
}

static int split_csv(const char *s, char out[][32], int max) {
    int n = 0; char buf[512]; strncpy(buf, s, 511); buf[511] = 0;
    for (char *p = strtok(buf, ","); p && n < max; p = strtok(NULL, ",")) { strncpy(out[n], p, 31); out[n][31] = 0; n++; }
    return n;
}

int main(int argc, char **argv) {
    const char *types = "q4,q6", *shapes = "8960x1536", *batches = "64,256", *tiles = "all", *splits_s = "1";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) types = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) shapes = argv[++i];
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) batches = argv[++i];
        else if (!strcmp(argv[i], "-tiles") && i + 1 < argc) tiles = argv[++i];
        else if (!strcmp(argv[i], "-splits") && i + 1 < argc) splits_s = argv[++i];
        else if (!strcmp(argv[i], "-reps") && i + 1 < argc) opt.reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-sc") && i + 1 < argc) opt.sc = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-nocheck")) opt.check = 0;
        else if (!strcmp(argv[i], "-mode") && i + 1 < argc) { i++; opt.mode = !strcmp(argv[i], "add") ? 1 : !strcmp(argv[i], "glu") ? 2 : 0; }
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) { i++; opt.xmode = !strcmp(argv[i], "1") ? 1 : !strcmp(argv[i], "cvt") ? 2 : 0; }
    }
#ifdef _WIN32
    HMODULE h = LoadLibraryA("vulkan-1.dll");
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(h, "vkGetInstanceProcAddr");
#else
    void *h = dlopen("libvulkan.so.1", RTLD_NOW);
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
#endif
#define GI(n) n = (PFN_##n)vkGetInstanceProcAddr(inst, #n)
    VkInstance inst = NULL;
    GI(vkCreateInstance);
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkmm", 1, "none", 1, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai };
    CK(vkCreateInstance(&ici, NULL, &inst));
    GI(vkEnumeratePhysicalDevices); GI(vkGetPhysicalDeviceProperties); GI(vkGetPhysicalDeviceQueueFamilyProperties);
    GI(vkCreateDevice); GI(vkGetDeviceProcAddr); GI(vkGetPhysicalDeviceMemoryProperties);
    VkPhysicalDevice pds[8]; uint32_t npd = 8;
    vkEnumeratePhysicalDevices(inst, &npd, pds);
    for (uint32_t i = 0; i < npd; i++) {
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pds[i], &pp);
        if (!pd || pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) pd = pds[i];
    }
    VkQueueFamilyProperties qf[16]; uint32_t nq = 16, qi = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qi = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qi, 1, &prio };
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR fcm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR };
    fcm.cooperativeMatrix = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.shaderFloat16 = VK_TRUE; f12.pNext = &fcm;
    VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    f11.storageBuffer16BitAccess = VK_TRUE; f11.pNext = &f12;
    const char *exts[] = { "VK_KHR_cooperative_matrix" };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f11, 0, 1, &qci };
    dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = exts;
    CK(vkCreateDevice(pd, &dci, NULL, &dev));
#define GD(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    GD(vkGetDeviceQueue); GD(vkCreateBuffer); GD(vkGetBufferMemoryRequirements); GD(vkAllocateMemory); GD(vkBindBufferMemory);
    GD(vkMapMemory); GD(vkCreateShaderModule); GD(vkCreateDescriptorSetLayout); GD(vkCreatePipelineLayout);
    GD(vkCreateComputePipelines); GD(vkCreateDescriptorPool); GD(vkAllocateDescriptorSets); GD(vkUpdateDescriptorSets);
    GD(vkCreateCommandPool); GD(vkAllocateCommandBuffers); GD(vkBeginCommandBuffer); GD(vkCmdBindPipeline);
    GD(vkCmdBindDescriptorSets); GD(vkCmdPushConstants); GD(vkCmdDispatch); GD(vkEndCommandBuffer); GD(vkCmdCopyBuffer);
    GD(vkCmdPipelineBarrier); GD(vkQueueSubmit); GD(vkCreateFence); GD(vkWaitForFences); GD(vkResetFences); GD(vkResetCommandBuffer);
    GD(vkDestroyBuffer); GD(vkFreeMemory);
    vkGetDeviceQueue(dev, qi, 0, &q);
    const VkMemoryPropertyFlags HOST = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkDeviceMemory mst, myh;
    stage = mkbuf(STAGE_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HOST, &mst);
    yhost = mkbuf(STAGE_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT, HOST, &myh);
    CK(vkMapMemory(dev, mst, 0, VK_WHOLE_SIZE, 0, (void **)&hstage));
    CK(vkMapMemory(dev, myh, 0, VK_WHOLE_SIZE, 0, (void **)&hy));
    VkDescriptorSetLayoutBinding b[3];
    for (int i = 0; i < 3; i++) { memset(&b[i], 0, sizeof b[i]); b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 3, b };
    CK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dl, 1, &pcr };
    CK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * 8192 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 8192, 1, &ps };
    CK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qi };
    VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    CK(vkCreateFence(dev, &fci, NULL, &fence));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    redpipe = mkpipe("spv/red2.spv"); cvtpipe = mkpipe("spv/cvt.spv");

    static char tl[16][32], ty[8][32], sh[16][32], ba[16][32], sp[8][32];
    int ntl, nty = split_csv(types, ty, 8), nsh = split_csv(shapes, sh, 16), nba = split_csv(batches, ba, 16), nsp = split_csv(splits_s, sp, 8);
    const char *tp[16]; int spl[8];
    if (!strcmp(tiles, "all")) { ntl = NTILES; for (int i = 0; i < ntl; i++) tp[i] = TILES[i].name; }
    else { ntl = split_csv(tiles, tl, 16); for (int i = 0; i < ntl; i++) tp[i] = tl[i]; }
    for (int i = 0; i < nsp; i++) spl[i] = atoi(sp[i]);
    for (int a = 0; a < nty; a++) {
        const int t = !strcmp(ty[a], "q4") ? 0 : !strcmp(ty[a], "q6") ? 1 : !strcmp(ty[a], "q5") ? 2 : !strcmp(ty[a], "q8") ? 3 : 4;
        for (int s = 0; s < nsh; s++) {
            unsigned R, C; sscanf(sh[s], "%ux%u", &R, &C);
            WSet w; wset_build(&w, t, R, C, opt.mode == 2 ? 2 : 1);
            for (int k = 0; k < nba; k++) run_batch(&w, (uint32_t)atoi(ba[k]), tp, ntl, spl, nsp);
            wset_free(&w);
        }
    }
    if (opt.check) printf("RESUMO: %d OK, %d DIFERENTE\n", nok, nfail);
    return nfail != 0;
}
