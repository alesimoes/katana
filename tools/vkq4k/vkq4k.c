/* vkq4k: produto matriz x lote com pesos Q4_K / Q6_K quantizados na VRAM (layout do katana),
   decodificados ao encher o ladrilho compartilhado, e matrizes cooperativas (fp16 x fp16).
   Y[rows][tokens] = W[rows][cols] * X[cols][tokens]. Uso: vkq4k [acc16] [q4|q6] */
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "vulkan/vulkan.h"
#include "q4k.h"
#include "q6k.h"
#include "q4k16.h"
#include "q6k16.h"
#include "red.h"

#define F(name) static PFN_##name name
F(vkGetInstanceProcAddr); F(vkCreateInstance); F(vkEnumeratePhysicalDevices); F(vkGetPhysicalDeviceProperties);
F(vkGetPhysicalDeviceQueueFamilyProperties); F(vkCreateDevice); F(vkGetDeviceProcAddr); F(vkGetDeviceQueue);
F(vkGetPhysicalDeviceMemoryProperties); F(vkCreateBuffer); F(vkGetBufferMemoryRequirements); F(vkAllocateMemory);
F(vkBindBufferMemory); F(vkMapMemory); F(vkCreateShaderModule); F(vkCreateDescriptorSetLayout); F(vkCreatePipelineLayout);
F(vkCreateComputePipelines); F(vkCreateDescriptorPool); F(vkAllocateDescriptorSets); F(vkUpdateDescriptorSets);
F(vkCreateCommandPool); F(vkAllocateCommandBuffers); F(vkBeginCommandBuffer); F(vkCmdBindPipeline);
F(vkCmdBindDescriptorSets); F(vkCmdPushConstants); F(vkCmdDispatch); F(vkEndCommandBuffer); F(vkCmdCopyBuffer);
F(vkCmdPipelineBarrier); F(vkQueueSubmit); F(vkCreateFence); F(vkWaitForFences); F(vkResetFences); F(vkResetCommandBuffer);

static double now(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / f.QuadPart; }
#define CK(x) do { VkResult r_ = (x); if (r_) { printf("erro %d em %s\n", r_, #x); exit(1); } } while (0)

static uint16_t f2h(float f) {
    uint32_t u; memcpy(&u, &f, 4);
    uint32_t s = (u >> 16) & 0x8000; int e = (int)((u >> 23) & 0xFF) - 127 + 15; uint32_t m = u & 0x7FFFFF;
    if (e <= 0) { if (e < -10) return (uint16_t)s; m |= 0x800000; return (uint16_t)(s | (m >> (14 - e))); }
    if (e >= 31) return (uint16_t)(s | 0x7C00);
    { uint32_t r = ((uint32_t)e << 10) | (m >> 13); const uint32_t rem = m & 0x1FFF; if (rem > 0x1000 || (rem == 0x1000 && (r & 1))) r++; if (r >= 0x7C00) r = 0x7C00; return (uint16_t)(s | r); }   /* arredonda para o mais proximo */
}
static float h2f(uint16_t h) {
    uint32_t s = (h >> 15) & 1, m = h & 0x3ff, u; int e = (h >> 10) & 0x1f;
    if (e == 0) { if (!m) u = s << 31; else { e = 1; while (!(m & 0x400)) { m <<= 1; e--; } m &= 0x3ff; u = (s << 31) | ((uint32_t)(e + 112) << 23) | (m << 13); } }
    else u = (s << 31) | ((uint32_t)(e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &u, 4); return f;
}
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

/* ---------- decodificacao e repacote (copiados do katana) ---------- */
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
static void pack_row(int q6, const uint8_t *src, size_t cols, uint32_t *o) {
    uint16_t h;
    if (!q6) { memcpy(o, src, cols / 256 * 144); return; }
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
static VkPipeline redpipe; static VkDescriptorSetLayout dl; static VkPipelineLayout pl; static VkDescriptorPool dp;
#define RUN() do { CK(vkEndCommandBuffer(cb)); CK(vkQueueSubmit(q, 1, &si, fence)); CK(vkWaitForFences(dev, 1, &fence, 1, ~0ull)); CK(vkResetFences(dev, 1, &fence)); } while (0)

static VkPipeline mkpipe(const uint32_t *spv, size_t size) {
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, spv };
    VkShaderModule sm; CK(vkCreateShaderModule(dev, &smi, NULL, &sm));
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = sm; cpi.stage.pName = "main"; cpi.layout = pl;
    VkPipeline pipe; CK(vkCreateComputePipelines(dev, NULL, 1, &cpi, NULL, &pipe));
    return pipe;
}

static uint8_t *hstage; static float *hy_f; static uint16_t *hy_h;   /* staging (host) e leitura de Y */
static VkBuffer stage, yhost; static const size_t STAGE_BYTES = 64u << 20;

static void upload(VkBuffer dst, size_t bytes) {
    CK(vkResetCommandBuffer(cb, 0)); CK(vkBeginCommandBuffer(cb, &bi));
    VkBufferCopy r = { 0, 0, bytes }; vkCmdCopyBuffer(cb, stage, dst, 1, &r); RUN();
}

/* um caso: q6 = 0 (Q4_K) ou 1 (Q6_K) */
static int run_case(VkPipeline pipe, int q6, int acc16, uint32_t rows, uint32_t cols, uint32_t tokens) {
    const uint32_t rowsP = (rows + 127) & ~127u, tokP = (tokens + 63) & ~63u;
    const size_t wpr = cols / 256 * (q6 ? 53 : 36), bpr = cols / 256 * (q6 ? 210 : 144);
    const size_t wbytes = (size_t)rowsP * wpr * 4, xbytes = (size_t)cols * tokP * 2;
    const size_t ebytes = acc16 ? 2 : 4, ybytes = (size_t)rowsP * tokP * ebytes;
    if (wbytes > STAGE_BYTES || xbytes > STAGE_BYTES || ybytes > STAGE_BYTES) { printf("grande demais\n"); return 1; }
    /* matriz aleatoria no formato do arquivo */
    uint8_t *gg = malloc((size_t)rows * bpr);
    for (uint32_t r = 0; r < rows; r++) {
        uint8_t *row = gg + (size_t)r * bpr;
        for (size_t b = 0; b < cols / 256; b++) {
            uint8_t *p = row + b * (q6 ? 210 : 144);
            for (int i = 0; i < (q6 ? 210 : 144); i++) p[i] = (uint8_t)rnd();
            const uint16_t d = f2h(0.0005f + (rnd() % 1000) * 1e-5f), dm = f2h(0.0005f + (rnd() % 1000) * 1e-5f);
            if (q6) { int8_t *sc = (int8_t *)(p + 192); for (int i = 0; i < 16; i++) sc[i] = (int8_t)((int)(rnd() % 64) - 32); memcpy(p + 208, &d, 2); }
            else { memcpy(p, &d, 2); memcpy(p + 2, &dm, 2); }
        }
    }
    /* layout do katana: palavra w da linha i em W[(i/32)*wpr*32 + w*32 + i%32] */
    uint32_t *pk = (uint32_t *)hstage, *tmp = malloc(wpr * 4);
    memset(pk, 0, wbytes);
    for (uint32_t r = 0; r < rows; r++) {
        pack_row(q6, gg + (size_t)r * bpr, cols, tmp);
        uint32_t *base = pk + (size_t)(r / 32) * wpr * 32;
        for (size_t w = 0; w < wpr; w++) base[w * 32 + r % 32] = tmp[w];
    }
    const int MAXSPL = acc16 ? 1 : 8;
    const size_t slicebytes = (size_t)rowsP * tokP * ebytes;
    VkDeviceMemory m1, m2, m3, m4;
    VkBuffer bw = mkbuf(wbytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &m1);
    VkBuffer bx = mkbuf(xbytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &m2);
    VkBuffer bp = mkbuf(slicebytes * MAXSPL, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &m3);
    VkBuffer by = mkbuf(slicebytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &m4);
    upload(bw, wbytes);
    /* ativacoes fp16 (preenchimento ate tokP com zeros) */
    float *X = calloc((size_t)cols * tokP, 4);
    uint16_t *hx = (uint16_t *)hstage;
    for (size_t k = 0; k < cols; k++) for (uint32_t t = 0; t < tokP; t++) {
        const float v = t < tokens ? ((int)(rnd() % 2001) - 1000) * 0.001f : 0.0f;
        hx[k * tokP + t] = f2h(v); X[k * tokP + t] = h2f(f2h(v));
    }
    upload(bx, xbytes);

    /* referencia na CPU: pesos exatos e pesos arredondados a fp16 (como a GPU os enxerga); soma em double */
    float *Wd = malloc((size_t)rows * cols * 4);
    for (uint32_t r = 0; r < rows; r++) (q6 ? dequant_q6k : dequant_q4k)(gg + (size_t)r * bpr, Wd + (size_t)r * cols, cols);
    float *Xt = malloc((size_t)tokens * cols * 4);
    for (size_t k = 0; k < cols; k++) for (uint32_t t = 0; t < tokens; t++) Xt[(size_t)t * cols + k] = X[k * tokP + t];
    double *ref = malloc((size_t)rows * tokens * 8), *ref2 = malloc((size_t)rows * tokens * 8), ss = 0;
    for (uint32_t r = 0; r < rows; r++) for (uint32_t t = 0; t < tokens; t++) {
        double a = 0; const float *w = Wd + (size_t)r * cols, *x = Xt + (size_t)t * cols;
        for (uint32_t k = 0; k < cols; k++) a += (double)w[k] * x[k];
        ref[(size_t)r * tokens + t] = a; ss += a * a;
    }
    for (size_t i = 0; i < (size_t)rows * cols; i++) Wd[i] = h2f(f2h(Wd[i]));
    for (uint32_t r = 0; r < rows; r++) for (uint32_t t = 0; t < tokens; t++) {
        double a = 0; const float *w = Wd + (size_t)r * cols, *x = Xt + (size_t)t * cols;
        for (uint32_t k = 0; k < cols; k++) a += (double)w[k] * x[k];
        ref2[(size_t)r * tokens + t] = a;
    }
    const double rms = sqrt(ss / ((double)rows * tokens));

    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dl };
    VkDescriptorSet ds, ds2; CK(vkAllocateDescriptorSets(dev, &dai, &ds)); CK(vkAllocateDescriptorSets(dev, &dai, &ds2));
    VkDescriptorBufferInfo dbi[3] = { { bw, 0, VK_WHOLE_SIZE }, { bx, 0, VK_WHOLE_SIZE }, { bp, 0, VK_WHOLE_SIZE } };
    VkDescriptorBufferInfo dbj[3] = { { bw, 0, VK_WHOLE_SIZE }, { bp, 0, VK_WHOLE_SIZE }, { by, 0, VK_WHOLE_SIZE } };   /* reducao */
    VkWriteDescriptorSet wr[6];
    for (int i = 0; i < 6; i++) { memset(&wr[i], 0, sizeof wr[i]); wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = i < 3 ? ds : ds2; wr[i].dstBinding = i % 3; wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[i].pBufferInfo = i < 3 ? &dbi[i] : &dbj[i - 3]; }
    vkUpdateDescriptorSets(dev, 6, wr, 0, NULL);

    const uint32_t nblk = cols / 256;
    for (int split = 1; split <= MAXSPL; split *= 2) {
        const uint32_t kper = ((nblk + split - 1) / split) * 256;
        const uint32_t gz = (cols + kper - 1) / kper;       /* fatias de fato nao vazias */
        if (split > 1 && gz != (uint32_t)split) continue;
        uint32_t pc[6] = { rows, cols, tokP, (uint32_t)wpr, kper, rowsP * tokP };
        uint32_t pr[3] = { rowsP * tokP / 4, (uint32_t)split, rowsP * tokP / 4 };
        CK(vkResetCommandBuffer(cb, 0)); CK(vkBeginCommandBuffer(cb, &bi));
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 24, pc);
        vkCmdDispatch(cb, (rows + 63) / 64, tokP / 64, split);
        if (split > 1) {
            VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, redpipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds2, 0, NULL);
            vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, pr);
            vkCmdDispatch(cb, (pr[0] + 255) / 256, 1, 1);
        }
        CK(vkEndCommandBuffer(cb));
        for (int i = 0; i < 5; i++) { CK(vkQueueSubmit(q, 1, &si, fence)); CK(vkWaitForFences(dev, 1, &fence, 1, ~0ull)); CK(vkResetFences(dev, 1, &fence)); }
        double best = 1e9;
        for (int rep = 0; rep < 3; rep++) {
            double t0 = now();
            for (int i = 0; i < 20; i++) { CK(vkQueueSubmit(q, 1, &si, fence)); CK(vkWaitForFences(dev, 1, &fence, 1, ~0ull)); CK(vkResetFences(dev, 1, &fence)); }
            const double dt = (now() - t0) / 20; if (dt < best) best = dt;
        }
        const double macs = (double)rows * cols * tokens, macsP = (double)rows * cols * tokP;
        /* resultado completo */
        CK(vkResetCommandBuffer(cb, 0)); CK(vkBeginCommandBuffer(cb, &bi));
        { VkBufferCopy r = { 0, 0, slicebytes }; vkCmdCopyBuffer(cb, split > 1 ? by : bp, yhost, 1, &r); } RUN();
        double worst = 0, worst_rms = 0, worst_ex = 0; size_t bad = 0;
        for (uint32_t r = 0; r < rows; r++) for (uint32_t t = 0; t < tokens; t++) {
            const double g = acc16 ? h2f(hy_h[(size_t)r * tokP + t]) : hy_f[(size_t)r * tokP + t];
            const double rf = ref2[(size_t)r * tokens + t], e = fabs(g - rf);
            if (!(e == e)) { bad++; continue; }
            const double er = e / (fabs(rf) + 0.05 * rms);
            if (er > worst) worst = er;
            if (e / rms > worst_rms) worst_rms = e / rms;
            const double ex = fabs(g - ref[(size_t)r * tokens + t]) / rms;
            if (ex > worst_ex) worst_ex = ex;
        }
        printf("%s %s split %d %5u x %5u lote %3u (pad %3u): %.3f ms  %.2f T mult/s (com pad %.2f)  vs pesos fp16: erro rel max %.2e (erro/rms %.2e); vs pesos exatos: erro/rms %.2e; nan %zu %s\n",
               q6 ? "Q6_K" : "Q4_K", acc16 ? "acc16" : "acc32", split, rows, cols, tokens, tokP, best * 1e3, macs / best / 1e12, macsP / best / 1e12,
               worst, worst_rms, worst_ex, bad, (worst < (acc16 ? 5e-2 : 1e-3) && !bad) ? "(OK)" : "(DIFERENTE)");
        fflush(stdout);
    }
    free(gg); free(tmp); free(X); free(Wd); free(Xt); free(ref); free(ref2);
    return 0;
}

int main(int argc, char **argv) {
    int acc16 = 0, which = 2; /* 0 q4, 1 q6, 2 todos */
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "acc16")) acc16 = 1; if (!strcmp(argv[i], "q4")) which = 0; if (!strcmp(argv[i], "q6")) which = 1; }
    HMODULE h = LoadLibraryA("vulkan-1.dll");
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(h, "vkGetInstanceProcAddr");
#define GI(n) n = (PFN_##n)vkGetInstanceProcAddr(inst, #n)
    VkInstance inst = NULL;
    GI(vkCreateInstance);
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkq4k", 1, "none", 1, VK_API_VERSION_1_3 };
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
    vkGetDeviceQueue(dev, qi, 0, &q);
    const VkMemoryPropertyFlags HOST = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkDeviceMemory mst, myh;
    stage = mkbuf(STAGE_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HOST, &mst);
    yhost = mkbuf(STAGE_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT, HOST, &myh);
    CK(vkMapMemory(dev, mst, 0, VK_WHOLE_SIZE, 0, (void **)&hstage));
    CK(vkMapMemory(dev, myh, 0, VK_WHOLE_SIZE, 0, (void **)&hy_f)); hy_h = (uint16_t *)hy_f;
    VkDescriptorSetLayoutBinding b[3];
    for (int i = 0; i < 3; i++) { memset(&b[i], 0, sizeof b[i]); b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 3, b };
    CK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 24 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dl, 1, &pcr };
    CK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * 64 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 64, 1, &ps };
    CK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qi };
    VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    CK(vkCreateFence(dev, &fci, NULL, &fence));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    redpipe = mkpipe(red_spv, sizeof red_spv);
    static const uint32_t shapes[][3] = { { 8960, 1536, 64 }, { 8960, 1536, 256 }, { 1536, 8960, 256 }, { 2048, 2048, 16 },
                                          { 8900, 1536, 70 } /* linhas e lote fora do multiplo de 64 */ };
    for (int t = 0; t < 2; t++) {
        if (which != 2 && which != t) continue;
        VkPipeline pipe = acc16 ? (t ? mkpipe(q6k16_spv, sizeof q6k16_spv) : mkpipe(q4k16_spv, sizeof q4k16_spv))
                                : (t ? mkpipe(q6k_spv, sizeof q6k_spv) : mkpipe(q4k_spv, sizeof q4k_spv));
        for (int s = 0; s < 5; s++) run_case(pipe, t, acc16, shapes[s][0], shapes[s][1], shapes[s][2]);
    }
    return 0;
}
