/* O que a placa oferece para o treino: formas de matriz cooperativa (fixas e flexiveis), tipos de memoria, filas.
     gcc -O2 -w -I . -I vk/include tools/traintest/tgcm_probe.c -o /tmp/tgcm_probe -ldl -lm */
#include "tools/traintest/tgcm_vk.h"
int main(void) {
    KvkInfo I; if (!bv_up(&I)) return 1;
    printf("%s vram %lu MB maxalloc %lu range %lu shared %u wg_inv %u subgroup %u period %g ns, transfer family %d\n", I.name, (unsigned long)(I.vram_bytes >> 20),
           (unsigned long)I.max_alloc, (unsigned long)I.max_storage_range, I.max_shared, I.max_wg_invocations, I.subgroup_size, bv_per * 1e9, bv_xfer);
    for (int i = 0; i < I.n_coop; i++) printf("coop M%u N%u K%u A%d B%d C%d R%d scope%d\n", I.coop[i].m, I.coop[i].n, I.coop[i].k, I.coop[i].a_type, I.coop[i].b_type, I.coop[i].c_type, I.coop[i].r_type, I.coop[i].scope);
    if (bv_cm2) {
        printf("cm2: workgroup %d flex %d reductions %d conversions %d perElement %d tensor %d blockLoads %d\n", bv_f2.cooperativeMatrixWorkgroupScope, bv_f2.cooperativeMatrixFlexibleDimensions,
               bv_f2.cooperativeMatrixReductions, bv_f2.cooperativeMatrixConversions, bv_f2.cooperativeMatrixPerElementOperations, bv_f2.cooperativeMatrixTensorAddressing, bv_f2.cooperativeMatrixBlockLoads);
        PFN_vkGetPhysicalDeviceCooperativeMatrixFlexibleDimensionsPropertiesNV fx = (PFN_vkGetPhysicalDeviceCooperativeMatrixFlexibleDimensionsPropertiesNV)bv_gipa(G.inst, "vkGetPhysicalDeviceCooperativeMatrixFlexibleDimensionsPropertiesNV");
        uint32_t n = 64; VkCooperativeMatrixFlexibleDimensionsPropertiesNV p[64]; memset(p, 0, sizeof p);
        for (int i = 0; i < 64; i++) p[i].sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_FLEXIBLE_DIMENSIONS_PROPERTIES_NV;
        fx(G.pd, &n, p);
        for (uint32_t i = 0; i < n; i++) printf("flex M%u N%u K%u A%d B%d C%d R%d scope%d wg%u\n", p[i].MGranularity, p[i].NGranularity, p[i].KGranularity, p[i].AType, p[i].BType, p[i].CType, p[i].ResultType, p[i].scope, p[i].workgroupInvocations);
        VkPhysicalDeviceCooperativeMatrix2PropertiesNV c2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_PROPERTIES_NV };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 }; p2.pNext = &c2;
        G.GetPhysicalDeviceProperties2(G.pd, &p2);
        printf("cm2: wg max %u, dim max %u, reserved shared %u\n", c2.cooperativeMatrixWorkgroupScopeMaxWorkgroupSize, c2.cooperativeMatrixFlexibleDimensionsMaxDimension, c2.cooperativeMatrixWorkgroupScopeReservedSharedMemory);
    }
    for (uint32_t i = 0; i < G.mp.memoryTypeCount; i++) printf("memory type %u heap %u (%lu MB) flags %x\n", i, G.mp.memoryTypes[i].heapIndex, (unsigned long)(G.mp.memoryHeaps[G.mp.memoryTypes[i].heapIndex].size >> 20), G.mp.memoryTypes[i].propertyFlags);
    VkQueueFamilyProperties qf[16]; uint32_t nq = 16; G.GetPhysicalDeviceQueueFamilyProperties(G.pd, &nq, qf);
    for (uint32_t i = 0; i < nq; i++) printf("queue family %u flags %x count %u\n", i, qf[i].queueFlags, qf[i].queueCount);
    return 0;
}
