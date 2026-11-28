/* kvk.h: camada minima de computacao Vulkan para o katana. Arquivo unico.
   Em exatamente uma unidade de traducao: #define KVK_IMPL antes de incluir.
   Carrega vulkan-1.dll / libvulkan.so.1 em tempo de execucao; sem SDK, so os cabecalhos.
   Nao usa exit(): toda funcao devolve 0/NULL/handle vazio em falha e kvk_error() diz o motivo.
   Nao e thread-safe: use de uma thread so (como o laco de geracao do motor). */
#ifndef KVK_H
#define KVK_H
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <stddef.h>
#include <stdint.h>
#include "vulkan/vulkan.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KVK_MAXBUF 16      /* maximo de buffers por despacho */
#define KVK_MAXCOOP 32     /* formas de matriz cooperativa listadas */
#define KVK_USAGE_RW 0     /* uso do buffer: leitura/escrita pelo shader + copia (unico por enquanto; reservado) */

typedef struct { uint32_t m, n, k; int a_type, b_type, c_type, r_type, scope; } KvkCoop; /* valores VkComponentTypeKHR / VkScopeKHR */

typedef struct {
    char name[256];
    int device_index, n_devices, is_discrete, unified_memory;
    uint32_t vendor_id, device_id, api_version, subgroup_size;
    uint64_t vram_bytes;               /* maior heap DEVICE_LOCAL */
    uint64_t max_alloc, max_storage_range;
    uint32_t max_alloc_count, max_push, max_shared, max_wg_invocations;
    int coopmat, fp16, storage16, int8, storage8, int16, int_dot; /* recursos opcionais ativados */
    int n_coop; KvkCoop coop[KVK_MAXCOOP];
} KvkInfo;

typedef struct { VkBuffer b; VkDeviceMemory m; size_t size; } KvkBuf;   /* b==0: invalido */
typedef struct { VkPipeline p; VkPipelineLayout pl; VkDescriptorSetLayout dl; int nbuf, push; uint32_t id; } KvkPipe; /* p==0: invalido */
typedef struct { VkCommandBuffer cb; int n_disp; } KvkRec;                /* cb==0: invalido */
typedef struct { uint64_t dispatches, submits, ds_hits, ds_allocs, ds_pools, rec_submits; } KvkStats;

const char *kvk_error(void);
int  kvk_init(KvkInfo *info);          /* 1 ok; 0 falha (sem Vulkan, sem recurso, indice invalido). Chamar de novo apos kvk_shutdown e permitido */
void kvk_shutdown(void);
uint64_t kvk_vram_budget(void);       /* quanto este processo pode usar da VRAM agora (ja contando o que ele usa e descontando os outros programas); 0 = a placa nao informa */
int  kvk_active(void);

/* buffers (VRAM; so o shader enxerga o conteudo, host usa write/read) */
KvkBuf kvk_buf_create(size_t bytes, int usage);
void   kvk_buf_destroy(KvkBuf *b);                 /* espera a GPU, invalida descritores em cache */
size_t kvk_bytes_allocated(void);                  /* VRAM alocada pela camada (buffers do usuario) */
int    kvk_buf_write(KvkBuf b, size_t off, const void *src, size_t n);   /* completa antes de retornar */
int    kvk_buf_write_q(KvkBuf b, size_t off, const void *src, size_t n); /* entra na fila do fluxo atual; os dados sao copiados na hora, a copia da GPU ocorre no kvk_flush/kvk_wait. Nao vale dentro de kvk_rec_* */
int    kvk_buf_readv(const KvkBuf *b, void *const *dst, const size_t *n, int k);   /* k buffers (do inicio) com um envio e uma espera so */
int    kvk_buf_read(KvkBuf b, size_t off, void *dst, size_t n);          /* flush + espera + leitura */
int    kvk_buf_copy(KvkBuf dst, size_t doff, KvkBuf src, size_t soff, size_t n);   /* copia na VRAM, entra na fila do fluxo atual */
#define kvk_read_sync kvk_buf_read

/* pipelines: todos os bindings sao storage buffers (set 0, binding 0..n-1), push constants de push_bytes, local_size via spec constants */
KvkPipe kvk_pipe_create(const uint32_t *spirv, size_t bytes, int n_buffers, int push_bytes,
                        const uint32_t *spec_ids, const uint32_t *spec_vals, int n_spec);
void    kvk_pipe_destroy(KvkPipe *p);

/* fluxo de comandos */
int kvk_begin(void);                               /* opcional: kvk_dispatch/write_q abrem sozinhos */
int kvk_dispatch(KvkPipe p, const KvkBuf *bufs, int n, const void *push, int push_bytes, uint32_t gx, uint32_t gy, uint32_t gz);
int kvk_barrier(void);                             /* barreira manual (so util com kvk_set_auto_barrier(0)) */
void kvk_set_auto_barrier(int on);                 /* padrao 1: barreira antes de cada despacho/copia */
int kvk_flush(void);                               /* submete o gravado, nao espera */
int kvk_wait(void);                                /* espera tudo que foi submetido (faz flush antes) */

/* grafo gravado: gravar uma vez, submeter muitas. Os buffers e push constants ficam congelados;
   valores que mudam por token devem vir de buffers pequenos escritos com kvk_buf_write_q antes do submit. */
int     kvk_rec_begin(void);
KvkRec  kvk_rec_end(void);
void    kvk_rec_cancel(void);
int     kvk_rec_submit(KvkRec r);                  /* faz flush do fluxo pendente antes; nao espera */
void    kvk_rec_free(KvkRec *r);

void kvk_stats(KvkStats *s);

#ifdef __cplusplus
}
#endif
#endif /* KVK_H */

#ifdef KVK_IMPL
#ifndef KVK_IMPL_DONE
#define KVK_IMPL_DONE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define KVK_NSLOT 8                 /* anel de buffers de comando/fences */
#define KVK_STAGE_MAX ((size_t)64 << 20)
#define KVK_STAGE_MIN ((size_t)1 << 20)
#define KVK_POOL_SETS 256

/* tabela de funcoes (sem o prefixo vk) */
#define KVK_INST_FNS(X) X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceProperties2) X(GetPhysicalDeviceFeatures2) \
    X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) X(EnumeratePhysicalDevices) \
    X(EnumerateDeviceExtensionProperties) X(CreateDevice) X(GetDeviceProcAddr) X(DestroyInstance)
#define KVK_DEV_FNS(X) X(GetDeviceQueue) X(DestroyDevice) X(DeviceWaitIdle) X(CreateBuffer) X(DestroyBuffer) \
    X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) \
    X(InvalidateMappedMemoryRanges) X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) \
    X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateComputePipelines) \
    X(DestroyPipeline) X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) \
    X(FreeDescriptorSets) X(UpdateDescriptorSets) X(CreateCommandPool) X(DestroyCommandPool) \
    X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) \
    X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDispatch) X(CmdCopyBuffer) \
    X(CmdPipelineBarrier) X(QueueSubmit) X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(GetFenceStatus)

typedef struct { uint64_t b[KVK_MAXBUF]; uint32_t pipe, hash; int n, pool, next; VkDescriptorSet set; } KvkDS;

static struct {
    int up;
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qfam;
    PFN_vkGetInstanceProcAddr gipa;
#define X(n) PFN_vk##n n;
    KVK_INST_FNS(X) KVK_DEV_FNS(X)
#undef X
    PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR CoopProps;
    PFN_vkGetPhysicalDeviceMemoryProperties2 MemProps2; int has_mb;      /* VK_EXT_memory_budget */
    VkPhysicalDeviceMemoryProperties mp;
    KvkInfo info;
    VkCommandPool cpool;
    VkCommandBuffer cb[KVK_NSLOT]; VkFence fence[KVK_NSLOT]; int state[KVK_NSLOT]; /* 0 livre 1 gravando 2 em voo */
    int head, cur;
    VkCommandBuffer rec; int rec_n;
    VkPipeline lastp; VkPipelineLayout lastpl; VkDescriptorSet lastds;
    int auto_barrier;
    VkBuffer up_b; VkDeviceMemory up_m; char *up_p; size_t up_cap, up_off;
    VkBuffer dn_b; VkDeviceMemory dn_m; char *dn_p; size_t dn_cap; int dn_coh;
    VkDescriptorPool *pools; int n_pools; int pool_sets;
    KvkDS *ds; int ds_n, ds_cap, ds_free, ds_live; int *bk; int nb;
    size_t bytes_alloc; uint32_t pipe_ctr;
    KvkStats st;
} kvk__g;
#define G kvk__g
#define F(n) G.n

static void *kvk__lib;
static char kvk__err[512];

static void kvk__seterr(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(kvk__err, sizeof kvk__err, fmt, ap); va_end(ap);
}
const char *kvk_error(void) { return kvk__err; }
int kvk_active(void) { return G.up; }
void kvk_stats(KvkStats *s) { *s = G.st; }
size_t kvk_bytes_allocated(void) { return G.bytes_alloc; }
#define KVK_NEED() do { if (!G.up) { kvk__seterr("kvk nao inicializado"); return 0; } } while (0)

static int kvk__memtype(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < G.mp.memoryTypeCount; i++)
        if ((bits >> i & 1) && (G.mp.memoryTypes[i].propertyFlags & want) == want && !(G.mp.memoryTypes[i].propertyFlags & avoid)) return (int)i;
    return -1;
}

/* cria buffer + memoria dedicada. host: 0 device-local, 1 upload (coerente), 2 download (cacheada se houver) */
static int kvk__mkbuf(size_t size, VkBufferUsageFlags usage, int host, VkBuffer *b, VkDeviceMemory *m, void **map, int *coh, size_t *real) {
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size; bci.usage = usage; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = F(CreateBuffer)(G.dev, &bci, NULL, b);
    if (r) { kvk__seterr("vkCreateBuffer(%zu) falhou: %d", size, (int)r); return 0; }
    VkMemoryRequirements mr; F(GetBufferMemoryRequirements)(G.dev, *b, &mr);
    int mt = -1; const VkMemoryPropertyFlags HV = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, HC = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        DL = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, CA = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    if (host == 0) { mt = kvk__memtype(mr.memoryTypeBits, DL, HV); if (mt < 0) mt = kvk__memtype(mr.memoryTypeBits, DL, 0); }
    else if (host == 1) { mt = kvk__memtype(mr.memoryTypeBits, HV | HC, DL); if (mt < 0) mt = kvk__memtype(mr.memoryTypeBits, HV | HC, 0); }
    else {
        mt = kvk__memtype(mr.memoryTypeBits, HV | CA, DL); if (mt < 0) mt = kvk__memtype(mr.memoryTypeBits, HV | CA, 0);
        if (mt < 0) mt = kvk__memtype(mr.memoryTypeBits, HV | HC, DL);
        if (mt < 0) mt = kvk__memtype(mr.memoryTypeBits, HV | HC, 0);
    }
    if (mt < 0) { F(DestroyBuffer)(G.dev, *b, NULL); *b = 0; kvk__seterr("sem tipo de memoria adequado"); return 0; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; mai.allocationSize = mr.size; mai.memoryTypeIndex = (uint32_t)mt;
    r = F(AllocateMemory)(G.dev, &mai, NULL, m);
    if (r) { F(DestroyBuffer)(G.dev, *b, NULL); *b = 0; kvk__seterr("vkAllocateMemory(%zu) falhou: %d (sem memoria?)", (size_t)mr.size, (int)r); return 0; }
    r = F(BindBufferMemory)(G.dev, *b, *m, 0);
    if (!r && host) {
        r = F(MapMemory)(G.dev, *m, 0, VK_WHOLE_SIZE, 0, map);
        if (coh) *coh = (G.mp.memoryTypes[mt].propertyFlags & HC) != 0;
    }
    if (r) { F(FreeMemory)(G.dev, *m, NULL); F(DestroyBuffer)(G.dev, *b, NULL); *b = 0; *m = 0; kvk__seterr("bind/map falhou: %d", (int)r); return 0; }
    if (real) *real = (size_t)mr.size;
    return 1;
}

static void kvk__rmbuf(VkBuffer *b, VkDeviceMemory *m, void **map) {
    if (*m && map && *map) F(UnmapMemory)(G.dev, *m);
    if (*b) F(DestroyBuffer)(G.dev, *b, NULL);
    if (*m) F(FreeMemory)(G.dev, *m, NULL);
    *b = 0; *m = 0; if (map) *map = NULL;
}

/* ---------- descritores ---------- */
static int kvk__newpool(void) {
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, KVK_POOL_SETS * KVK_MAXBUF };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; pi.maxSets = KVK_POOL_SETS; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
    VkDescriptorPool p; VkResult r = F(CreateDescriptorPool)(G.dev, &pi, NULL, &p);
    if (r) { kvk__seterr("vkCreateDescriptorPool falhou: %d", (int)r); return 0; }
    VkDescriptorPool *np = (VkDescriptorPool *)realloc(G.pools, (size_t)(G.n_pools + 1) * sizeof *np);
    if (!np) { F(DestroyDescriptorPool)(G.dev, p, NULL); kvk__seterr("sem memoria"); return 0; }
    G.pools = np; G.pools[G.n_pools++] = p; G.st.ds_pools++;
    return 1;
}

static uint32_t kvk__hash(uint32_t pipe, const uint64_t *b, int n) {
    uint64_t h = 1469598103934665603ull ^ pipe;
    for (int i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; h ^= h >> 29; }
    return (uint32_t)(h ^ (h >> 32));
}

static int kvk__rebuild(int nb) {
    int *bk = (int *)malloc((size_t)nb * sizeof(int));
    if (!bk) { kvk__seterr("sem memoria"); return 0; }
    for (int i = 0; i < nb; i++) bk[i] = -1;
    for (int i = 0; i < G.ds_n; i++) if (G.ds[i].set) { uint32_t k = G.ds[i].hash % (uint32_t)nb; G.ds[i].next = bk[k]; bk[k] = i; }
    free(G.bk); G.bk = bk; G.nb = nb;
    return 1;
}

static VkDescriptorSet kvk__getds(KvkPipe p, const KvkBuf *bufs, int n) {
    uint64_t key[KVK_MAXBUF];
    for (int i = 0; i < n; i++) key[i] = (uint64_t)bufs[i].b;
    uint32_t h = kvk__hash(p.id, key, n);
    if (G.nb) for (int i = G.bk[h % (uint32_t)G.nb]; i >= 0; i = G.ds[i].next) {
        KvkDS *e = &G.ds[i];
        if (e->hash == h && e->pipe == p.id && e->n == n && !memcmp(e->b, key, (size_t)n * 8)) { G.st.ds_hits++; return e->set; }
    }
    /* alocar conjunto: pool atual; se cheio, novo pool */
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO }; ai.descriptorSetCount = 1; ai.pSetLayouts = &p.dl;
    VkDescriptorSet set = 0; VkResult r = VK_ERROR_OUT_OF_POOL_MEMORY;
    if (G.n_pools) { ai.descriptorPool = G.pools[G.n_pools - 1]; r = F(AllocateDescriptorSets)(G.dev, &ai, &set); }
    if (r == VK_ERROR_OUT_OF_POOL_MEMORY || r == VK_ERROR_FRAGMENTED_POOL || r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY) {
        if (!kvk__newpool()) return 0;
        ai.descriptorPool = G.pools[G.n_pools - 1]; r = F(AllocateDescriptorSets)(G.dev, &ai, &set);
    }
    if (r) { kvk__seterr("vkAllocateDescriptorSets falhou: %d", (int)r); return 0; }
    VkDescriptorBufferInfo bi[KVK_MAXBUF]; VkWriteDescriptorSet wr[KVK_MAXBUF];
    for (int i = 0; i < n; i++) {
        bi[i].buffer = bufs[i].b; bi[i].offset = 0; bi[i].range = VK_WHOLE_SIZE;
        memset(&wr[i], 0, sizeof wr[i]); wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = set; wr[i].dstBinding = (uint32_t)i;
        wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[i].pBufferInfo = &bi[i];
    }
    F(UpdateDescriptorSets)(G.dev, (uint32_t)n, wr, 0, NULL);
    G.st.ds_allocs++;
    int idx;
    if (G.ds_free >= 0) { idx = G.ds_free; G.ds_free = G.ds[idx].next; }
    else {
        if (G.ds_n == G.ds_cap) {
            int nc = G.ds_cap ? G.ds_cap * 2 : 256; KvkDS *nd = (KvkDS *)realloc(G.ds, (size_t)nc * sizeof *nd);
            if (!nd) { F(FreeDescriptorSets)(G.dev, ai.descriptorPool, 1, &set); kvk__seterr("sem memoria"); return 0; }
            G.ds = nd; G.ds_cap = nc;
        }
        idx = G.ds_n++;
    }
    KvkDS *e = &G.ds[idx]; memcpy(e->b, key, (size_t)n * 8); e->pipe = p.id; e->n = n; e->hash = h; e->set = set; e->pool = G.n_pools - 1;
    G.ds_live++;
    if (!G.nb || G.ds_live > G.nb * 2) { if (!kvk__rebuild(G.nb ? G.nb * 2 : 512)) { G.ds_live--; e->set = 0; e->next = G.ds_free; G.ds_free = idx; F(FreeDescriptorSets)(G.dev, ai.descriptorPool, 1, &set); return 0; } }
    else { uint32_t k = h % (uint32_t)G.nb; e->next = G.bk[k]; G.bk[k] = idx; }
    return set;
}

/* remove do cache entradas que citam um buffer (b != 0) ou um pipeline (pipe != 0); GPU ja ociosa */
static void kvk__purge(uint64_t buf, uint32_t pipe) {
    int any = 0;
    for (int i = 0; i < G.ds_n; i++) {
        KvkDS *e = &G.ds[i]; if (!e->set) continue;
        int hit = pipe && e->pipe == pipe;
        for (int j = 0; buf && j < e->n && !hit; j++) hit = e->b[j] == buf;
        if (!hit) continue;
        F(FreeDescriptorSets)(G.dev, G.pools[e->pool], 1, &e->set);
        e->set = 0; e->next = G.ds_free; G.ds_free = i; G.ds_live--; any = 1;
    }
    if (any && G.nb) {   /* refaz as cadeias; a lista livre usa 'next' das entradas mortas, entao reconstroi so as vivas */
        int *bk = G.bk; for (int i = 0; i < G.nb; i++) bk[i] = -1;
        for (int i = 0; i < G.ds_n; i++) if (G.ds[i].set) { uint32_t k = G.ds[i].hash % (uint32_t)G.nb; G.ds[i].next = bk[k]; bk[k] = i; }
    }
}

/* ---------- fluxo de comandos ---------- */
/* espera um fence: consulta em laco antes de dormir no driver (acordar do driver custa caro em
   alguns sistemas). KVK_SPIN=0 desliga a consulta. */
static VkResult kvk__fence_wait(VkFence f) {
    static int spin = -1;
    if (spin < 0) spin = getenv("KVK_SPIN") ? atoi(getenv("KVK_SPIN")) : 1;
    if (spin) for (long i = 0; i < 20000000L; i++) {
        VkResult r = F(GetFenceStatus)(G.dev, f);
        if (r != VK_NOT_READY) return r;
    }
    return F(WaitForFences)(G.dev, 1, &f, VK_TRUE, ~0ull);
}
static int kvk__slot_get(void) {
    int s = G.head; G.head = (G.head + 1) % KVK_NSLOT;
    if (G.state[s] == 2) {
        VkResult r = kvk__fence_wait(G.fence[s]);
        if (r) { kvk__seterr("vkWaitForFences falhou: %d", (int)r); return -1; }
        F(ResetFences)(G.dev, 1, &G.fence[s]); G.state[s] = 0;
    }
    return s;
}

int kvk_begin(void) {
    KVK_NEED();
    if (G.rec || G.cur >= 0) return 1;
    int s = kvk__slot_get(); if (s < 0) return 0;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    F(ResetCommandBuffer)(G.cb[s], 0);
    VkResult r = F(BeginCommandBuffer)(G.cb[s], &bi);
    if (r) { kvk__seterr("vkBeginCommandBuffer falhou: %d", (int)r); return 0; }
    G.cur = s; G.state[s] = 1; G.lastp = 0; G.lastpl = 0; G.lastds = 0;
    return 1;
}

static VkCommandBuffer kvk__ccb(void) {
    if (G.rec) return G.rec;
    if (!kvk_begin()) return 0;
    return G.cb[G.cur];
}

static void kvk__bar(VkCommandBuffer cb) {
    if (!G.auto_barrier) return;
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    F(CmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
void kvk_set_auto_barrier(int on) { G.auto_barrier = on; }
int kvk_barrier(void) {
    KVK_NEED(); VkCommandBuffer cb = kvk__ccb(); if (!cb) return 0;
    int a = G.auto_barrier; G.auto_barrier = 1; kvk__bar(cb); G.auto_barrier = a; return 1;
}

int kvk_flush(void) {
    KVK_NEED();
    if (G.rec) { kvk__seterr("flush durante gravacao de grafo"); return 0; }
    if (G.cur < 0) return 1;
    int s = G.cur; G.cur = -1;
    VkResult r = F(EndCommandBuffer)(G.cb[s]);
    if (r) { G.state[s] = 0; kvk__seterr("vkEndCommandBuffer falhou: %d", (int)r); return 0; }
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &G.cb[s];
    r = F(QueueSubmit)(G.q, 1, &si, G.fence[s]);
    if (r) { G.state[s] = 0; kvk__seterr("vkQueueSubmit falhou: %d", (int)r); return 0; }
    G.state[s] = 2; G.st.submits++;
    return 1;
}

int kvk_wait(void) {
    KVK_NEED();
    if (!kvk_flush()) return 0;
    for (int s = 0; s < KVK_NSLOT; s++) if (G.state[s] == 2) {
        VkResult r = kvk__fence_wait(G.fence[s]);
        if (r) { kvk__seterr("vkWaitForFences falhou: %d (dispositivo perdido?)", (int)r); return 0; }
        F(ResetFences)(G.dev, 1, &G.fence[s]); G.state[s] = 0;
    }
    G.up_off = 0;   /* nada em voo: a area de upload pode ser reutilizada */
    return 1;
}

/* ---------- buffers ---------- */
KvkBuf kvk_buf_create(size_t bytes, int usage) {
    KvkBuf r; memset(&r, 0, sizeof r); (void)usage;
    if (!G.up) { kvk__seterr("kvk nao inicializado"); return r; }
    size_t sz = (bytes + 15) & ~(size_t)15; if (sz < 16) sz = 16;
    if (sz > G.info.max_alloc) { kvk__seterr("buffer de %zu bytes excede maxMemoryAllocationSize (%llu)", sz, (unsigned long long)G.info.max_alloc); return r; }
    if (sz > G.info.max_storage_range) { kvk__seterr("buffer de %zu bytes excede maxStorageBufferRange (%llu)", sz, (unsigned long long)G.info.max_storage_range); return r; }
    size_t real = 0; void *nomap = NULL;
    if (!kvk__mkbuf(sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, &r.b, &r.m, &nomap, NULL, &real)) { memset(&r, 0, sizeof r); return r; }
    r.size = bytes; G.bytes_alloc += real;
    return r;
}

void kvk_buf_destroy(KvkBuf *b) {
    if (!G.up || !b || !b->b) { if (b) memset(b, 0, sizeof *b); return; }
    if (!G.rec) kvk_wait();
    kvk__purge((uint64_t)b->b, 0);
    size_t sz = (b->size + 15) & ~(size_t)15; if (sz < 16) sz = 16;
    VkMemoryRequirements mr; F(GetBufferMemoryRequirements)(G.dev, b->b, &mr);
    F(DestroyBuffer)(G.dev, b->b, NULL); F(FreeMemory)(G.dev, b->m, NULL);
    G.bytes_alloc -= (size_t)mr.size; (void)sz;
    memset(b, 0, sizeof *b);
}

static int kvk__grow_up(size_t need) {   /* GPU ociosa e up_off==0 */
    size_t cap = G.up_cap ? G.up_cap * 2 : KVK_STAGE_MIN;
    if (cap < need) cap = need;
    if (cap > KVK_STAGE_MAX) cap = KVK_STAGE_MAX;
    if (cap < need) cap = need;
    VkBuffer nb = 0; VkDeviceMemory nm = 0; void *map = NULL; size_t real;
    if (!kvk__mkbuf(cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1, &nb, &nm, &map, NULL, &real)) return 0;
    void *om = G.up_p; kvk__rmbuf(&G.up_b, &G.up_m, &om);
    G.up_b = nb; G.up_m = nm; G.up_p = (char *)map; G.up_cap = cap;
    return 1;
}

int kvk_buf_write_q(KvkBuf b, size_t off, const void *src, size_t n) {
    KVK_NEED();
    if (G.rec) { kvk__seterr("kvk_buf_write_q nao vale durante gravacao de grafo"); return 0; }
    if (!b.b || off + n > b.size) { kvk__seterr("write fora do buffer"); return 0; }
    const char *p = (const char *)src;
    while (n) {
        size_t chunk = n < KVK_STAGE_MAX ? n : KVK_STAGE_MAX;
        size_t o = (G.up_off + 15) & ~(size_t)15;
        if (o + chunk > G.up_cap) {          /* area cheia: esvazia a GPU e recomeca */
            if (!kvk_wait()) return 0;
            o = 0;
            if (chunk > G.up_cap && !kvk__grow_up(chunk)) return 0;
        }
        memcpy(G.up_p + o, p, chunk);
        VkCommandBuffer cb = kvk__ccb(); if (!cb) return 0;
        kvk__bar(cb);
        VkBufferCopy rg; rg.srcOffset = o; rg.dstOffset = off; rg.size = chunk;
        F(CmdCopyBuffer)(cb, G.up_b, b.b, 1, &rg);
        G.up_off = o + chunk; p += chunk; off += chunk; n -= chunk;
    }
    return 1;
}

int kvk_buf_copy(KvkBuf dst, size_t doff, KvkBuf src, size_t soff, size_t n) {
    KVK_NEED();
    if (G.rec) { kvk__seterr("kvk_buf_copy nao vale durante gravacao de grafo"); return 0; }
    if (!dst.b || !src.b || doff + n > dst.size || soff + n > src.size) { kvk__seterr("copia fora do buffer"); return 0; }
    if (!n) return 1;
    VkCommandBuffer cb = kvk__ccb(); if (!cb) return 0;
    kvk__bar(cb);
    VkBufferCopy rg; rg.srcOffset = soff; rg.dstOffset = doff; rg.size = n;
    F(CmdCopyBuffer)(cb, src.b, dst.b, 1, &rg);
    return 1;
}

int kvk_buf_write(KvkBuf b, size_t off, const void *src, size_t n) {
    if (!kvk_buf_write_q(b, off, src, n)) return 0;
    return kvk_wait();
}

/* garante a area de descida com pelo menos n bytes */
static int kvk__grow_dn(size_t n) {
    if (n <= G.dn_cap) return 1;
    if (!kvk_wait()) return 0;
    size_t cap = G.dn_cap ? G.dn_cap * 2 : KVK_STAGE_MIN; if (cap < n) cap = n; if (cap > KVK_STAGE_MAX) cap = KVK_STAGE_MAX;
    VkBuffer nb = 0; VkDeviceMemory nm = 0; void *map = NULL; int coh = 1; size_t real;
    if (!kvk__mkbuf(cap, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 2, &nb, &nm, &map, &coh, &real)) return 0;
    void *om = G.dn_p; kvk__rmbuf(&G.dn_b, &G.dn_m, &om);
    G.dn_b = nb; G.dn_m = nm; G.dn_p = (char *)map; G.dn_cap = cap; G.dn_coh = coh;
    return 1;
}
int kvk_buf_readv(const KvkBuf *b, void *const *dst, const size_t *n, int k) {
    KVK_NEED();
    if (G.rec) { kvk__seterr("leitura durante gravacao de grafo"); return 0; }
    size_t tot = 0;
    for (int i = 0; i < k; i++) {
        if (!b[i].b || n[i] > b[i].size) { kvk__seterr("read fora do buffer"); return 0; }
        tot += (n[i] + 15) & ~(size_t)15;
    }
    if (tot > KVK_STAGE_MAX) {      /* nao cabe de uma vez: um a um */
        for (int i = 0; i < k; i++) if (!kvk_buf_read(b[i], 0, dst[i], n[i])) return 0;
        return 1;
    }
    if (!kvk__grow_dn(tot)) return 0;
    VkCommandBuffer cb = kvk__ccb(); if (!cb) return 0;
    kvk__bar(cb);
    size_t o = 0;
    for (int i = 0; i < k; i++) {
        VkBufferCopy rg; rg.srcOffset = 0; rg.dstOffset = o; rg.size = n[i];
        F(CmdCopyBuffer)(cb, b[i].b, G.dn_b, 1, &rg);
        o += (n[i] + 15) & ~(size_t)15;
    }
    if (!kvk_wait()) return 0;
    if (!G.dn_coh) {
        VkMappedMemoryRange mr = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE }; mr.memory = G.dn_m; mr.offset = 0; mr.size = VK_WHOLE_SIZE;
        F(InvalidateMappedMemoryRanges)(G.dev, 1, &mr);
    }
    o = 0;
    for (int i = 0; i < k; i++) { memcpy(dst[i], G.dn_p + o, n[i]); o += (n[i] + 15) & ~(size_t)15; }
    return 1;
}

int kvk_buf_read(KvkBuf b, size_t off, void *dst, size_t n) {
    KVK_NEED();
    if (G.rec) { kvk__seterr("leitura durante gravacao de grafo"); return 0; }
    if (!b.b || off + n > b.size) { kvk__seterr("read fora do buffer"); return 0; }
    char *d = (char *)dst;
    while (n) {
        size_t chunk = n < KVK_STAGE_MAX ? n : KVK_STAGE_MAX;
        if (chunk > G.dn_cap) {
            if (!kvk_wait()) return 0;
            size_t cap = G.dn_cap ? G.dn_cap * 2 : KVK_STAGE_MIN; if (cap < chunk) cap = chunk; if (cap > KVK_STAGE_MAX) cap = KVK_STAGE_MAX;
            VkBuffer nb = 0; VkDeviceMemory nm = 0; void *map = NULL; int coh = 1; size_t real;
            if (!kvk__mkbuf(cap, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 2, &nb, &nm, &map, &coh, &real)) return 0;
            void *om = G.dn_p; kvk__rmbuf(&G.dn_b, &G.dn_m, &om);
            G.dn_b = nb; G.dn_m = nm; G.dn_p = (char *)map; G.dn_cap = cap; G.dn_coh = coh;
        }
        VkCommandBuffer cb = kvk__ccb(); if (!cb) return 0;
        kvk__bar(cb);
        VkBufferCopy rg; rg.srcOffset = off; rg.dstOffset = 0; rg.size = chunk;
        F(CmdCopyBuffer)(cb, b.b, G.dn_b, 1, &rg);
        if (!kvk_wait()) return 0;
        if (!G.dn_coh) {
            VkMappedMemoryRange mr = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE }; mr.memory = G.dn_m; mr.offset = 0; mr.size = VK_WHOLE_SIZE;
            F(InvalidateMappedMemoryRanges)(G.dev, 1, &mr);
        }
        memcpy(d, G.dn_p, chunk);
        d += chunk; off += chunk; n -= chunk;
    }
    return 1;
}

/* ---------- pipelines ---------- */
KvkPipe kvk_pipe_create(const uint32_t *spirv, size_t bytes, int nbuf, int push, const uint32_t *ids, const uint32_t *vals, int nspec) {
    KvkPipe p; memset(&p, 0, sizeof p);
    if (!G.up) { kvk__seterr("kvk nao inicializado"); return p; }
    if (nbuf < 1 || nbuf > KVK_MAXBUF || push < 0 || push > (int)G.info.max_push || (push & 3) || nspec < 0 || nspec > 32 || (bytes & 3) || !bytes) {
        kvk__seterr("parametros de pipeline invalidos (buffers %d, push %d, spec %d)", nbuf, push, nspec); return p;
    }
    VkShaderModule sm = 0; VkDescriptorSetLayout dl = 0; VkPipelineLayout pl = 0; VkPipeline pi = 0; VkResult r;
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO }; smi.codeSize = bytes; smi.pCode = spirv;
    if ((r = F(CreateShaderModule)(G.dev, &smi, NULL, &sm))) { kvk__seterr("vkCreateShaderModule falhou: %d", (int)r); return p; }
    VkDescriptorSetLayoutBinding lb[KVK_MAXBUF];
    for (int i = 0; i < nbuf; i++) { memset(&lb[i], 0, sizeof lb[i]); lb[i].binding = (uint32_t)i; lb[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; lb[i].descriptorCount = 1; lb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO }; dli.bindingCount = (uint32_t)nbuf; dli.pBindings = lb;
    if ((r = F(CreateDescriptorSetLayout)(G.dev, &dli, NULL, &dl))) { kvk__seterr("vkCreateDescriptorSetLayout falhou: %d", (int)r); goto fail; }
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)push };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO }; pli.setLayoutCount = 1; pli.pSetLayouts = &dl;
    if (push) { pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr; }
    if ((r = F(CreatePipelineLayout)(G.dev, &pli, NULL, &pl))) { kvk__seterr("vkCreatePipelineLayout falhou: %d", (int)r); goto fail; }
    VkSpecializationMapEntry se[32]; VkSpecializationInfo si; uint32_t sv[32];
    for (int i = 0; i < nspec; i++) { se[i].constantID = ids[i]; se[i].offset = (uint32_t)i * 4; se[i].size = 4; sv[i] = vals[i]; }
    si.mapEntryCount = (uint32_t)nspec; si.pMapEntries = se; si.dataSize = (size_t)nspec * 4; si.pData = sv;
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = sm;
    ci.stage.pName = "main"; ci.stage.pSpecializationInfo = nspec ? &si : NULL; ci.layout = pl;
    if ((r = F(CreateComputePipelines)(G.dev, VK_NULL_HANDLE, 1, &ci, NULL, &pi))) { kvk__seterr("vkCreateComputePipelines falhou: %d", (int)r); goto fail; }
    F(DestroyShaderModule)(G.dev, sm, NULL);
    p.p = pi; p.pl = pl; p.dl = dl; p.nbuf = nbuf; p.push = push; p.id = ++G.pipe_ctr;
    return p;
fail:
    if (pl) F(DestroyPipelineLayout)(G.dev, pl, NULL);
    if (dl) F(DestroyDescriptorSetLayout)(G.dev, dl, NULL);
    F(DestroyShaderModule)(G.dev, sm, NULL);
    return p;
}

void kvk_pipe_destroy(KvkPipe *p) {
    if (!G.up || !p || !p->p) { if (p) memset(p, 0, sizeof *p); return; }
    if (!G.rec) kvk_wait();
    kvk__purge(0, p->id);
    F(DestroyPipeline)(G.dev, p->p, NULL); F(DestroyPipelineLayout)(G.dev, p->pl, NULL); F(DestroyDescriptorSetLayout)(G.dev, p->dl, NULL);
    memset(p, 0, sizeof *p);
}

int kvk_dispatch(KvkPipe p, const KvkBuf *bufs, int n, const void *push, int pbytes, uint32_t gx, uint32_t gy, uint32_t gz) {
    KVK_NEED();
    if (!p.p || n != p.nbuf || pbytes < 0 || pbytes > p.push || (pbytes & 3)) { kvk__seterr("kvk_dispatch: pipeline/buffers/push incompativeis"); return 0; }
    for (int i = 0; i < n; i++) if (!bufs[i].b) { kvk__seterr("kvk_dispatch: buffer %d invalido", i); return 0; }
    VkCommandBuffer cb = kvk__ccb(); if (!cb) return 0;
    VkDescriptorSet ds = kvk__getds(p, bufs, n); if (!ds) return 0;
    kvk__bar(cb);
    if (G.lastp != p.p) { F(CmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.p); G.lastp = p.p; }
    if (G.lastds != ds || G.lastpl != p.pl) { F(CmdBindDescriptorSets)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.pl, 0, 1, &ds, 0, NULL); G.lastds = ds; G.lastpl = p.pl; }
    if (pbytes) F(CmdPushConstants)(cb, p.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)pbytes, push);
    F(CmdDispatch)(cb, gx, gy, gz);
    G.st.dispatches++; if (G.rec) G.rec_n++;
    return 1;
}

/* ---------- grafo gravado ---------- */
int kvk_rec_begin(void) {
    KVK_NEED();
    if (G.rec) { kvk__seterr("kvk_rec_begin: ja gravando"); return 0; }
    if (!kvk_flush()) return 0;
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO }; ai.commandPool = G.cpool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cb; VkResult r = F(AllocateCommandBuffers)(G.dev, &ai, &cb);
    if (r) { kvk__seterr("vkAllocateCommandBuffers falhou: %d", (int)r); return 0; }
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    r = F(BeginCommandBuffer)(cb, &bi);
    if (r) { F(FreeCommandBuffers)(G.dev, G.cpool, 1, &cb); kvk__seterr("vkBeginCommandBuffer falhou: %d", (int)r); return 0; }
    G.rec = cb; G.rec_n = 0; G.lastp = 0; G.lastpl = 0; G.lastds = 0;
    return 1;
}

KvkRec kvk_rec_end(void) {
    KvkRec r; memset(&r, 0, sizeof r);
    if (!G.up || !G.rec) { kvk__seterr("kvk_rec_end sem kvk_rec_begin"); return r; }
    VkCommandBuffer cb = G.rec; G.rec = 0; G.lastp = 0; G.lastpl = 0; G.lastds = 0;
    VkResult v = F(EndCommandBuffer)(cb);
    if (v) { F(FreeCommandBuffers)(G.dev, G.cpool, 1, &cb); kvk__seterr("vkEndCommandBuffer falhou: %d", (int)v); return r; }
    r.cb = cb; r.n_disp = G.rec_n;
    return r;
}

void kvk_rec_cancel(void) {
    if (!G.up || !G.rec) return;
    VkCommandBuffer cb = G.rec; G.rec = 0; G.lastp = 0; G.lastpl = 0; G.lastds = 0;
    F(FreeCommandBuffers)(G.dev, G.cpool, 1, &cb);
}

int kvk_rec_submit(KvkRec r) {
    KVK_NEED();
    if (G.rec || !r.cb) { kvk__seterr("kvk_rec_submit: grafo invalido ou gravacao em andamento"); return 0; }
    if (!kvk_flush()) return 0;
    int s = kvk__slot_get(); if (s < 0) return 0;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &r.cb;
    VkResult v = F(QueueSubmit)(G.q, 1, &si, G.fence[s]);
    if (v) { kvk__seterr("vkQueueSubmit falhou: %d", (int)v); return 0; }
    G.state[s] = 2; G.st.submits++; G.st.rec_submits++;
    return 1;
}

void kvk_rec_free(KvkRec *r) {
    if (!G.up || !r || !r->cb) { if (r) memset(r, 0, sizeof *r); return; }
    kvk_wait();
    F(FreeCommandBuffers)(G.dev, G.cpool, 1, &r->cb);
    memset(r, 0, sizeof *r);
}

/* ---------- init / shutdown ---------- */
uint64_t kvk_vram_budget(void) {
    if (!G.dev || !G.has_mb) return 0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
    VkPhysicalDeviceMemoryProperties2 p = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 };
    p.pNext = &b;
    G.MemProps2(G.pd, &p);
    uint64_t size = 0, bud = 0;       /* o maior heap DEVICE_LOCAL, o mesmo de vram_bytes */
    for (uint32_t i = 0; i < p.memoryProperties.memoryHeapCount; i++)
        if ((p.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && p.memoryProperties.memoryHeaps[i].size > size) {
            size = p.memoryProperties.memoryHeaps[i].size; bud = b.heapBudget[i];
        }
    return bud;
}

void kvk_shutdown(void) {
    if (G.dev) {
        if (G.rec) kvk_rec_cancel();
        F(DeviceWaitIdle)(G.dev);
        for (int i = 0; i < G.ds_n; i++) if (G.ds[i].set) F(FreeDescriptorSets)(G.dev, G.pools[G.ds[i].pool], 1, &G.ds[i].set);
        for (int i = 0; i < G.n_pools; i++) F(DestroyDescriptorPool)(G.dev, G.pools[i], NULL);
        for (int i = 0; i < KVK_NSLOT; i++) if (G.fence[i]) F(DestroyFence)(G.dev, G.fence[i], NULL);
        if (G.cpool) F(DestroyCommandPool)(G.dev, G.cpool, NULL);
        void *m = G.up_p; kvk__rmbuf(&G.up_b, &G.up_m, &m); m = G.dn_p; kvk__rmbuf(&G.dn_b, &G.dn_m, &m);
        F(DestroyDevice)(G.dev, NULL);
    }
    if (G.inst && G.DestroyInstance) F(DestroyInstance)(G.inst, NULL);
    free(G.pools); free(G.ds); free(G.bk);
    memset(&G, 0, sizeof G);
}

static void *kvk__sym(const char *n) {
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)kvk__lib, n);
#else
    return dlsym(kvk__lib, n);
#endif
}

static int kvk__hasext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

int kvk_init(KvkInfo *info) {
    if (G.up) { if (info) *info = G.info; return 1; }
    memset(&G, 0, sizeof G); G.cur = -1; G.auto_barrier = 1; G.ds_free = -1; kvk__err[0] = 0;
    VkResult r;
    if (!kvk__lib) {
#ifdef _WIN32
        kvk__lib = (void *)LoadLibraryA("vulkan-1.dll");
#else
        kvk__lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!kvk__lib) kvk__lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);      /* Android: sem o .1 (celular, 2026-11-27) */
#endif
        if (!kvk__lib) { kvk__seterr("biblioteca Vulkan nao encontrada"); return 0; }
    }
    G.gipa = (PFN_vkGetInstanceProcAddr)kvk__sym("vkGetInstanceProcAddr");
    PFN_vkCreateInstance createInst = G.gipa ? (PFN_vkCreateInstance)G.gipa(NULL, "vkCreateInstance") : NULL;
    if (!createInst) { kvk__seterr("vkCreateInstance indisponivel"); return 0; }
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO }; ai.pApplicationName = "katana"; ai.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ici.pApplicationInfo = &ai;
    if ((r = createInst(&ici, NULL, &G.inst))) { kvk__seterr("vkCreateInstance falhou: %d", (int)r); G.inst = 0; goto fail; }
#define X(n) G.n = (PFN_vk##n)G.gipa(G.inst, "vk" #n); if (!G.n) { kvk__seterr("funcao vk" #n " ausente"); goto fail; }
    KVK_INST_FNS(X)
#undef X
    G.CoopProps = (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)G.gipa(G.inst, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
    G.MemProps2 = (PFN_vkGetPhysicalDeviceMemoryProperties2)G.gipa(G.inst, "vkGetPhysicalDeviceMemoryProperties2");

    VkPhysicalDevice pds[16]; uint32_t npd = 16;
    r = F(EnumeratePhysicalDevices)(G.inst, &npd, pds);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || npd == 0) { kvk__seterr("nenhum dispositivo Vulkan"); goto fail; }
    int want = -1; const char *ev = getenv("KATANA_VK_DEVICE");
    if (ev && *ev) {
        char *end; long v = strtol(ev, &end, 10);
        if (*end || v < 0 || v >= (long)npd) { kvk__seterr("KATANA_VK_DEVICE=%s invalido (ha %u dispositivos)", ev, npd); goto fail; }
        want = (int)v;
    } else {
        int best = -1, bs = -1;
        for (uint32_t i = 0; i < npd; i++) {
            VkPhysicalDeviceProperties pp; F(GetPhysicalDeviceProperties)(pds[i], &pp);
            int sc = pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
            if (pp.apiVersion < VK_API_VERSION_1_2) sc = 0;
            if (sc > bs) { bs = sc; best = (int)i; }
        }
        want = best < 0 ? 0 : best;
    }
    G.pd = pds[want];
    VkPhysicalDeviceProperties pp; F(GetPhysicalDeviceProperties)(G.pd, &pp);
    if (pp.apiVersion < VK_API_VERSION_1_2) { kvk__seterr("dispositivo '%s' so suporta Vulkan %u.%u (precisa 1.2)", pp.deviceName, VK_VERSION_MAJOR(pp.apiVersion), VK_VERSION_MINOR(pp.apiVersion)); goto fail; }
    {   /* familia de fila */
        VkQueueFamilyProperties qf[16]; uint32_t nq = 16; int found = 0;
        F(GetPhysicalDeviceQueueFamilyProperties)(G.pd, &nq, qf);
        for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { G.qfam = i; found = 1; break; }
        if (!found) { kvk__seterr("sem fila de computacao"); goto fail; }
    }
    uint32_t next = 0; VkExtensionProperties *ext = NULL;
    F(EnumerateDeviceExtensionProperties)(G.pd, NULL, &next, NULL);
    ext = (VkExtensionProperties *)calloc(next + 1, sizeof *ext);
    if (!ext) { kvk__seterr("sem memoria"); goto fail; }
    F(EnumerateDeviceExtensionProperties)(G.pd, NULL, &next, ext);
    int has_cm = kvk__hasext(ext, next, "VK_KHR_cooperative_matrix"), has_dp = kvk__hasext(ext, next, "VK_KHR_shader_integer_dot_product");
    G.has_mb = G.MemProps2 && kvk__hasext(ext, next, "VK_EXT_memory_budget");
    free(ext);

    /* o que a placa suporta */
    VkPhysicalDeviceVulkan11Features q11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceVulkan12Features q12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR qcm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR };
    VkPhysicalDeviceShaderIntegerDotProductFeaturesKHR qdp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES_KHR };
    VkPhysicalDeviceFeatures2 q2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    q2.pNext = &q11; q11.pNext = &q12; void **tail = &q12.pNext;
    if (has_cm) { *tail = &qcm; tail = &qcm.pNext; }
    if (has_dp) { *tail = &qdp; tail = &qdp.pNext; }
    F(GetPhysicalDeviceFeatures2)(G.pd, &q2);
    int o_cm = has_cm && qcm.cooperativeMatrix, o_dp = has_dp && qdp.shaderIntegerDotProduct;

    VkPhysicalDeviceVulkan11Features e11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceVulkan12Features e12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR ecm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR };
    VkPhysicalDeviceShaderIntegerDotProductFeaturesKHR edp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES_KHR };
    VkPhysicalDeviceFeatures2 e2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    e11.storageBuffer16BitAccess = q11.storageBuffer16BitAccess;
    e12.shaderFloat16 = q12.shaderFloat16; e12.shaderInt8 = q12.shaderInt8; e12.storageBuffer8BitAccess = q12.storageBuffer8BitAccess;
    e2.features.shaderInt16 = q2.features.shaderInt16;
    ecm.cooperativeMatrix = o_cm; edp.shaderIntegerDotProduct = o_dp;
    e2.pNext = &e11; e11.pNext = &e12; tail = &e12.pNext;
    const char *exts[3]; uint32_t ne = 0;
    if (G.has_mb) exts[ne++] = "VK_EXT_memory_budget";
    if (o_cm) { *tail = &ecm; tail = &ecm.pNext; exts[ne++] = "VK_KHR_cooperative_matrix"; }
    if (o_dp) { *tail = &edp; tail = &edp.pNext; exts[ne++] = "VK_KHR_shader_integer_dot_product"; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO }; qci.queueFamilyIndex = G.qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO }; dci.pNext = &e2; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = ne; dci.ppEnabledExtensionNames = exts;
    if ((r = F(CreateDevice)(G.pd, &dci, NULL, &G.dev))) { kvk__seterr("vkCreateDevice falhou: %d", (int)r); G.dev = 0; goto fail; }
    { const char *miss = NULL;
#define X(n) G.n = (PFN_vk##n)G.GetDeviceProcAddr(G.dev, "vk" #n); if (!G.n) miss = #n;
    KVK_DEV_FNS(X)
#undef X
      if (miss) { kvk__seterr("funcao vk%s ausente no dispositivo", miss); G.DestroyDevice = NULL; G.dev = 0; goto fail; } }
    F(GetDeviceQueue)(G.dev, G.qfam, 0, &G.q);
    F(GetPhysicalDeviceMemoryProperties)(G.pd, &G.mp);

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpi.queueFamilyIndex = G.qfam;
    if ((r = F(CreateCommandPool)(G.dev, &cpi, NULL, &G.cpool))) { kvk__seterr("vkCreateCommandPool falhou: %d", (int)r); goto fail; }
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO }; cai.commandPool = G.cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = KVK_NSLOT;
    if ((r = F(AllocateCommandBuffers)(G.dev, &cai, G.cb))) { kvk__seterr("vkAllocateCommandBuffers falhou: %d", (int)r); goto fail; }
    for (int i = 0; i < KVK_NSLOT; i++) {
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if ((r = F(CreateFence)(G.dev, &fi, NULL, &G.fence[i]))) { kvk__seterr("vkCreateFence falhou: %d", (int)r); goto fail; }
    }

    /* informacoes */
    KvkInfo *I = &G.info; memset(I, 0, sizeof *I);
    snprintf(I->name, sizeof I->name, "%s", pp.deviceName);
    I->device_index = want; I->n_devices = (int)npd; I->is_discrete = pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    I->vendor_id = pp.vendorID; I->device_id = pp.deviceID; I->api_version = pp.apiVersion;
    I->max_storage_range = pp.limits.maxStorageBufferRange; I->max_alloc_count = pp.limits.maxMemoryAllocationCount;
    I->max_push = pp.limits.maxPushConstantsSize; I->max_shared = pp.limits.maxComputeSharedMemorySize; I->max_wg_invocations = pp.limits.maxComputeWorkGroupInvocations;
    for (uint32_t i = 0; i < G.mp.memoryHeapCount; i++)
        if ((G.mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && G.mp.memoryHeaps[i].size > I->vram_bytes) I->vram_bytes = G.mp.memoryHeaps[i].size;
    for (uint32_t i = 0; i < G.mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = G.mp.memoryTypes[i].propertyFlags;
        if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && G.mp.memoryHeaps[G.mp.memoryTypes[i].heapIndex].size > (uint64_t)1 << 30) I->unified_memory = 1;
    }
    {
        VkPhysicalDeviceSubgroupProperties sp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
        VkPhysicalDeviceMaintenance3Properties m3 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 }; p2.pNext = &sp; sp.pNext = &m3;
        F(GetPhysicalDeviceProperties2)(G.pd, &p2);
        I->subgroup_size = sp.subgroupSize; I->max_alloc = m3.maxMemoryAllocationSize;
        if (!I->max_alloc) I->max_alloc = (uint64_t)1 << 30;
    }
    I->coopmat = o_cm; I->fp16 = e12.shaderFloat16; I->storage16 = e11.storageBuffer16BitAccess;
    I->int8 = e12.shaderInt8 && e12.storageBuffer8BitAccess; I->storage8 = e12.storageBuffer8BitAccess; I->int16 = e2.features.shaderInt16; I->int_dot = o_dp;
    if (o_cm && G.CoopProps) {
        VkCooperativeMatrixPropertiesKHR cm[64]; uint32_t n = 64;
        for (int i = 0; i < 64; i++) { memset(&cm[i], 0, sizeof cm[i]); cm[i].sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR; }
        r = G.CoopProps(G.pd, &n, cm);
        if (r == VK_SUCCESS || r == VK_INCOMPLETE) for (uint32_t i = 0; i < n && I->n_coop < KVK_MAXCOOP; i++) {
            KvkCoop *c = &I->coop[I->n_coop++]; c->m = cm[i].MSize; c->n = cm[i].NSize; c->k = cm[i].KSize;
            c->a_type = (int)cm[i].AType; c->b_type = (int)cm[i].BType; c->c_type = (int)cm[i].CType; c->r_type = (int)cm[i].ResultType; c->scope = (int)cm[i].scope;
        }
    }
    G.up = 1;
    if (info) *info = G.info;
    return 1;
fail:
    {   char keep[512]; memcpy(keep, kvk__err, sizeof keep);
        kvk_shutdown();
        memcpy(kvk__err, keep, sizeof keep); }
    return 0;
}
#endif /* KVK_IMPL_DONE */
#endif /* KVK_IMPL */
