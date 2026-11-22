/* Apoio das medidas do treino na GPU: kvk.h com o dispositivo criado pela medida (todas as extensoes de matriz cooperativa,
   fila de transferencia, linha do tempo) sem mexer no kvk.h, mais marcas de tempo da GPU. */
#define _GNU_SOURCE
#include <time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
static void *bv_sym(void *lib, const char *n);
#define dlsym bv_sym
#define KVK_IMPL
#include "vk/kvk.h"
#undef dlsym
static PFN_vkGetInstanceProcAddr bv_gipa;
static PFN_vkCreateDevice bv_cd;
static int bv_cm2, bv_xfer = -1;       /* extensao 2 ativa; familia da fila so de transferencia */
static VkPhysicalDeviceCooperativeMatrix2FeaturesNV bv_f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_FEATURES_NV };
static VKAPI_ATTR VkResult VKAPI_CALL bv_create(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *al, VkDevice *d) {
    VkExtensionProperties *ext = malloc(512 * sizeof *ext); uint32_t n = 512;
    G.EnumerateDeviceExtensionProperties(pd, NULL, &n, ext);
    const char *e[16]; uint32_t ne = 0;
    for (; ne < ci->enabledExtensionCount; ne++) e[ne] = ci->ppEnabledExtensionNames[ne];
    VkDeviceCreateInfo c = *ci;
    for (VkBaseOutStructure *p = (VkBaseOutStructure *)c.pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            ((VkPhysicalDeviceVulkan12Features *)p)->timelineSemaphore = 1; ((VkPhysicalDeviceVulkan12Features *)p)->vulkanMemoryModel = 1; }
    if (kvk__hasext(ext, n, "VK_NV_cooperative_matrix2")) {
        VkPhysicalDeviceFeatures2 q = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 }; q.pNext = &bv_f2; bv_f2.pNext = NULL;
        G.GetPhysicalDeviceFeatures2(pd, &q);
        e[ne++] = "VK_NV_cooperative_matrix2"; bv_f2.pNext = (void *)c.pNext; c.pNext = &bv_f2; bv_cm2 = 1;
    }
    VkQueueFamilyProperties qf[16]; uint32_t nq = 16; G.GetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    for (uint32_t i = 0; i < nq; i++)
        if ((qf[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(qf[i].queueFlags & ~(VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT)) && bv_xfer < 0) bv_xfer = (int)i;
    float pr = 1; VkDeviceQueueCreateInfo qi[2]; qi[0] = ci->pQueueCreateInfos[0];
    if (bv_xfer >= 0) { qi[1] = qi[0]; qi[1].queueFamilyIndex = (uint32_t)bv_xfer; qi[1].pQueuePriorities = &pr; c.pQueueCreateInfos = qi; c.queueCreateInfoCount = 2; }
    c.ppEnabledExtensionNames = e; c.enabledExtensionCount = ne;
    free(ext);
    return bv_cd(pd, &c, al, d);
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL bv_gp(VkInstance i, const char *n) {
    PFN_vkVoidFunction f = bv_gipa(i, n);
    if (!strcmp(n, "vkCreateDevice")) { bv_cd = (PFN_vkCreateDevice)f; return (PFN_vkVoidFunction)bv_create; }
    return f;
}
static void *bv_sym(void *lib, const char *n) {
    void *f = dlsym(lib, n);
    if (!strcmp(n, "vkGetInstanceProcAddr")) { bv_gipa = (PFN_vkGetInstanceProcAddr)f; return (void *)bv_gp; }
    return f;
}
static double bv_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
/* marcas de tempo */
static PFN_vkCmdWriteTimestamp bv_ts; static PFN_vkCmdResetQueryPool bv_rq; static PFN_vkGetQueryPoolResults bv_gr;
static VkQueryPool bv_qp; static double bv_per;
#define BVF(v, n) *(void **)&v = (void *)G.GetDeviceProcAddr(G.dev, n)
static int bv_up(KvkInfo *I) {
    if (!kvk_init(I)) { printf("sem GPU: %s\n", kvk_error()); return 0; }
    BVF(bv_ts, "vkCmdWriteTimestamp"); BVF(bv_rq, "vkCmdResetQueryPool"); BVF(bv_gr, "vkGetQueryPoolResults");
    PFN_vkCreateQueryPool cq; BVF(cq, "vkCreateQueryPool");
    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO }; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2;
    cq(G.dev, &qi, NULL, &bv_qp);
    VkPhysicalDeviceProperties pp; G.GetPhysicalDeviceProperties(G.pd, &pp); bv_per = pp.limits.timestampPeriod * 1e-9;
    return 1;
}
static void bv_t0(void) { VkCommandBuffer c = kvk__ccb(); bv_rq(c, bv_qp, 0, 2); kvk_barrier(); bv_ts(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, bv_qp, 0); }
static void bv_t1(void) { kvk_barrier(); bv_ts(kvk__ccb(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, bv_qp, 1); }
static double bv_dt(void) {           /* depois de kvk_wait */
    uint64_t q[2]; bv_gr(G.dev, bv_qp, 0, 2, sizeof q, q, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    return (double)(q[1] - q[0]) * bv_per;
}
