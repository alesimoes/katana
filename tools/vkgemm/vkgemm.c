/* vkgemm: mede a vazao de um produto matriz x lote com matrizes cooperativas (nucleos tensoriais)
   em Vulkan. Y[rows][tokens] = W[rows][cols] * X[cols][tokens], tudo em meia precisao.
   Prototipo, fora do motor. Uso: vkgemm [rows cols tokens] */
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "vulkan/vulkan.h"
#include "gemm_spv.h"

#define F(name) static PFN_##name name
F(vkGetInstanceProcAddr); F(vkCreateInstance); F(vkEnumeratePhysicalDevices); F(vkGetPhysicalDeviceProperties);
F(vkGetPhysicalDeviceQueueFamilyProperties); F(vkCreateDevice); F(vkGetDeviceProcAddr); F(vkGetDeviceQueue);
F(vkGetPhysicalDeviceMemoryProperties); F(vkCreateBuffer); F(vkGetBufferMemoryRequirements); F(vkAllocateMemory);
F(vkBindBufferMemory); F(vkMapMemory); F(vkCreateShaderModule); F(vkCreateDescriptorSetLayout); F(vkCreatePipelineLayout);
F(vkCreateComputePipelines); F(vkCreateDescriptorPool); F(vkAllocateDescriptorSets); F(vkUpdateDescriptorSets);
F(vkCreateCommandPool); F(vkAllocateCommandBuffers); F(vkBeginCommandBuffer); F(vkCmdBindPipeline);
F(vkCmdBindDescriptorSets); F(vkCmdPushConstants); F(vkCmdDispatch); F(vkEndCommandBuffer); F(vkCmdCopyBuffer);
F(vkQueueSubmit); F(vkCreateFence); F(vkWaitForFences); F(vkResetFences); F(vkResetCommandBuffer);
F(vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR);

static double now(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / f.QuadPart; }
#define CK(x) do { VkResult r_ = (x); if (r_) { printf("erro %d em %s\n", r_, #x); exit(1); } } while (0)

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

int main(int argc, char **argv) {
    const uint32_t rows = argc > 3 ? atoi(argv[1]) : 8960, cols = argc > 3 ? atoi(argv[2]) : 1536, tokens = argc > 3 ? atoi(argv[3]) : 64;
    const int ITER = 30;
    HMODULE h = LoadLibraryA("vulkan-1.dll");
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(h, "vkGetInstanceProcAddr");
#define GI(n) n = (PFN_##n)vkGetInstanceProcAddr(inst, #n)
    VkInstance inst = NULL;
    GI(vkCreateInstance);
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkgemm", 1, "none", 1, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai };
    CK(vkCreateInstance(&ici, NULL, &inst));
    GI(vkEnumeratePhysicalDevices); GI(vkGetPhysicalDeviceProperties); GI(vkGetPhysicalDeviceQueueFamilyProperties);
    GI(vkCreateDevice); GI(vkGetDeviceProcAddr); GI(vkGetPhysicalDeviceMemoryProperties); GI(vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR);
    VkPhysicalDevice pds[8]; uint32_t npd = 8;
    vkEnumeratePhysicalDevices(inst, &npd, pds);
    for (uint32_t i = 0; i < npd; i++) {
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pds[i], &pp);
        if (!pd || pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) pd = pds[i];
    }
    {   /* combinacoes de matriz cooperativa que a placa aceita */
        static VkCooperativeMatrixPropertiesKHR cm[64]; uint32_t n = 64;
        for (int i = 0; i < 64; i++) cm[i].sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
        if (vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR && vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(pd, &n, cm) >= 0)
            for (uint32_t i = 0; i < n; i++)
                printf("coopmat %ux%ux%u  A=%d B=%d C=%d R=%d  escopo=%d\n", cm[i].MSize, cm[i].NSize, cm[i].KSize,
                       cm[i].AType, cm[i].BType, cm[i].CType, cm[i].ResultType, cm[i].scope);
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
    GD(vkQueueSubmit); GD(vkCreateFence); GD(vkWaitForFences); GD(vkResetFences); GD(vkResetCommandBuffer);
    VkQueue q; vkGetDeviceQueue(dev, qi, 0, &q);

    const VkDeviceSize wsz = (VkDeviceSize)rows * cols * 2, xsz = (VkDeviceSize)cols * tokens * 2, ysz = (VkDeviceSize)rows * tokens * 4;
    const VkMemoryPropertyFlags HOST = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, DEVL = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkDeviceMemory mst, mw, mx, my, myh;
    VkBuffer stage = mkbuf(wsz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HOST, &mst);
    VkBuffer bw = mkbuf(wsz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, DEVL, &mw);
    VkBuffer bx = mkbuf(xsz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, DEVL, &mx);
    VkBuffer by = mkbuf(ysz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, DEVL, &my);
    VkBuffer byh = mkbuf(ysz, VK_BUFFER_USAGE_TRANSFER_DST_BIT, HOST, &myh);
    uint16_t *hs; float *hy;
    CK(vkMapMemory(dev, mst, 0, VK_WHOLE_SIZE, 0, (void **)&hs));
    CK(vkMapMemory(dev, myh, 0, VK_WHOLE_SIZE, 0, (void **)&hy));

    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof gemm_spv, gemm_spv };
    VkShaderModule sm; CK(vkCreateShaderModule(dev, &smi, NULL, &sm));
    VkDescriptorSetLayoutBinding b[3];
    for (int i = 0; i < 3; i++) { memset(&b[i], 0, sizeof b[i]); b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 3, b };
    VkDescriptorSetLayout dl; CK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 12 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dl, 1, &pcr };
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = sm; cpi.stage.pName = "main"; cpi.layout = pl;
    VkPipeline pipe; CK(vkCreateComputePipelines(dev, NULL, 1, &cpi, NULL, &pipe));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps };
    VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dl };
    VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev, &dai, &ds));
    VkDescriptorBufferInfo dbi[3] = { { bw, 0, VK_WHOLE_SIZE }, { bx, 0, VK_WHOLE_SIZE }, { by, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet wr[3];
    for (int i = 0; i < 3; i++) { memset(&wr[i], 0, sizeof wr[i]); wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = ds; wr[i].dstBinding = i; wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[i].pBufferInfo = &dbi[i]; }
    vkUpdateDescriptorSets(dev, 3, wr, 0, NULL);

    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qi };
    VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence; CK(vkCreateFence(dev, &fci, NULL, &fence));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
#define RUN() do { CK(vkEndCommandBuffer(cb)); CK(vkQueueSubmit(q, 1, &si, fence)); CK(vkWaitForFences(dev, 1, &fence, 1, ~0ull)); CK(vkResetFences(dev, 1, &fence)); } while (0)

    /* pesos e ativacoes pequenos e deterministicos; sobem por copia */
    float *W = malloc((size_t)rows * cols * 4), *X = malloc((size_t)cols * tokens * 4);
    for (size_t i = 0; i < (size_t)rows * cols; i++) W[i] = 0.01f * (float)((int)((i * 7 + i / cols * 3) % 13) - 6);
    for (size_t i = 0; i < (size_t)cols * tokens; i++) X[i] = 0.02f * (float)((int)((i * 5) % 11) - 5);
    for (size_t i = 0; i < (size_t)rows * cols; i++) hs[i] = f2h(W[i]);
    CK(vkResetCommandBuffer(cb, 0)); CK(vkBeginCommandBuffer(cb, &bi));
    { VkBufferCopy r = { 0, 0, wsz }; vkCmdCopyBuffer(cb, stage, bw, 1, &r); } RUN();
    for (size_t i = 0; i < (size_t)cols * tokens; i++) hs[i] = f2h(X[i]);
    CK(vkResetCommandBuffer(cb, 0)); CK(vkBeginCommandBuffer(cb, &bi));
    { VkBufferCopy r = { 0, 0, xsz }; vkCmdCopyBuffer(cb, stage, bx, 1, &r); } RUN();

    uint32_t pc[3] = { rows, cols, tokens };
    CK(vkResetCommandBuffer(cb, 0)); CK(vkBeginCommandBuffer(cb, &bi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, pc);
    vkCmdDispatch(cb, rows / 64, tokens / 64, 1);
    CK(vkEndCommandBuffer(cb));
    for (int i = 0; i < 5; i++) { CK(vkQueueSubmit(q, 1, &si, fence)); CK(vkWaitForFences(dev, 1, &fence, 1, ~0ull)); CK(vkResetFences(dev, 1, &fence)); }
    double t0 = now();
    for (int i = 0; i < ITER; i++) { CK(vkQueueSubmit(q, 1, &si, fence)); CK(vkWaitForFences(dev, 1, &fence, 1, ~0ull)); CK(vkResetFences(dev, 1, &fence)); }
    double dt = (now() - t0) / ITER;
    const double macs = (double)rows * cols * tokens;
    printf("%u x %u, lote %u: %.3f ms por produto, %.2f T multiplicacoes/s\n", rows, cols, tokens, dt * 1e3, macs / dt / 1e12);
    printf("equivalente a um modelo de 1,5 B de pesos: %.0f tokens/s de prompt (so os produtos)\n", macs / dt / 1.5e9);

    /* confere alguns resultados contra a CPU */
    CK(vkResetCommandBuffer(cb, 0)); CK(vkBeginCommandBuffer(cb, &bi));
    { VkBufferCopy r = { 0, 0, ysz }; vkCmdCopyBuffer(cb, by, byh, 1, &r); } RUN();
    double worst = 0.0;
    for (int s = 0; s < 200; s++) {
        const uint32_t r = (uint32_t)(s * 7919u) % rows, t = (uint32_t)(s * 31u) % tokens;
        double ref = 0.0;
        for (uint32_t k = 0; k < cols; k++) ref += (double)h2f(f2h(W[(size_t)r * cols + k])) * h2f(f2h(X[(size_t)k * tokens + t]));
        const double got = hy[(size_t)r * tokens + t], e = fabs(got - ref) / (fabs(ref) + 1e-3);
        if (e > worst) worst = e;
    }
    printf("conferencia com a CPU em 200 posicoes: pior erro relativo %.4f %s\n", worst, worst < 0.02 ? "(OK)" : "(DIFERENTE)");
    return 0;
}
