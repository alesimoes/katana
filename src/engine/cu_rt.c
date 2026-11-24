/* ============================ CUDA: camada de execucao (API do driver, sem SDK) ===============================
 * Decisao do dono (2026-11-24): a camada de GPU pode usar CUDA. Nada e ligado em tempo de compilacao: libcuda.so.1 (nvcuda.dll no
 * Windows) e carregada com dlopen/LoadLibrary, os kernels sao PTX em texto (cu_ptx.c) compilado pelo proprio driver
 * (cuModuleLoadDataEx). Sem libcuda ou sem placa, cu_init() devolve 0 e nada mais acontece.
 *
 * Interface: cu_init() -> 1 se ha placa; cu_alloc/cu_free, cu_up/cu_down (+ _async), cu_module(ptx), cu_fn(mod, nome),
 * cu_launch(fn, grade, bloco, smem, args...), cu_sync(), cu_ev_*; cu_dev (atributos reais: SMs, clock, banda). */
/* dlfcn vem de base.c */

typedef int CUresult_; typedef unsigned long long CUdptr_;
typedef struct {
    void *h;
    CUresult_ (*Init)(unsigned);
    CUresult_ (*DeviceGetCount)(int *);
    CUresult_ (*DeviceGet)(int *, int);
    CUresult_ (*DeviceGetName)(char *, int, int);
    CUresult_ (*DeviceGetAttribute)(int *, int, int);
    CUresult_ (*DeviceTotalMem)(size_t *, int);
    CUresult_ (*PrimaryRetain)(void **, int);
    CUresult_ (*CtxSetCurrent)(void *);
    CUresult_ (*MemGetInfo)(size_t *, size_t *);
    CUresult_ (*MemAlloc)(CUdptr_ *, size_t);
    CUresult_ (*MemFree)(CUdptr_);
    CUresult_ (*HtoD)(CUdptr_, const void *, size_t);
    CUresult_ (*DtoH)(void *, CUdptr_, size_t);
    CUresult_ (*HtoDA)(CUdptr_, const void *, size_t, void *);
    CUresult_ (*DtoHA)(void *, CUdptr_, size_t, void *);
    CUresult_ (*MemsetD8)(CUdptr_, unsigned char, size_t);
    CUresult_ (*HostAlloc)(void **, size_t, unsigned);
    CUresult_ (*HostFree)(void *);
    CUresult_ (*ModuleLoadDataEx)(void **, const void *, unsigned, int *, void **);
    CUresult_ (*ModuleGetFunction)(void **, void *, const char *);
    CUresult_ (*FuncSetAttribute)(void *, int, int);
    CUresult_ (*LaunchKernel)(void *, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, void *, void **, void **);
    CUresult_ (*StreamCreate)(void **, unsigned);
    CUresult_ (*StreamSynchronize)(void *);
    CUresult_ (*StreamWaitEvent)(void *, void *, unsigned);
    CUresult_ (*CtxSynchronize)(void);
    CUresult_ (*EventCreate)(void **, unsigned);
    CUresult_ (*EventRecord)(void *, void *);
    CUresult_ (*EventSynchronize)(void *);
    CUresult_ (*EventElapsed)(float *, void *, void *);
    CUresult_ (*GetErrorString)(CUresult_, const char **);
    CUresult_ (*StreamBeginCapture)(void *, int);
    CUresult_ (*StreamEndCapture)(void *, void **);
    CUresult_ (*GraphInstantiate)(void **, void *, void *, char *, size_t);
    CUresult_ (*GraphLaunch)(void *, void *);
    CUresult_ (*GraphExecDestroy)(void *);
    CUresult_ (*GraphDestroy)(void *);
} CuApi;
static CuApi cuapi;

typedef struct {
    int ok, tried;
    char name[128];
    int sm_count, cc_major, cc_minor, clock_khz, mem_clock_khz, bus_bits, smem_optin, l2;
    size_t vram_total, vram_free;
    double peak_tflops_f16, peak_gbps;     /* teoricos: f16 com acumulo f32 (GeForce: metade da taxa) e banda */
    void *ctx, *stream;
    int debug;
} CuDev;
static CuDev cu_dev;

static int cu_chk(CUresult_ r, const char *what) {
    if (r == 0) return 1;
    const char *s = "?"; if (cuapi.GetErrorString) cuapi.GetErrorString(r, &s);
    fprintf(stderr, "cuda: %s falhou (%d: %s)\n", what, r, s ? s : "?");
    return 0;
}

#ifdef _WIN32
static void *cu_dlsym(void *h, const char *n) { return (void *)GetProcAddress((HMODULE)h, n); }
#else
static void *cu_dlsym(void *h, const char *n) { return dlsym(h, n); }
#endif
#define CU_SYM(field, name) do { *(void **)&cuapi.field = cu_dlsym(cuapi.h, name); if (!cuapi.field) { if (opt) { cuapi.field = NULL; } else { fprintf(stderr, "cuda: simbolo %s ausente\n", name); return 0; } } } while (0)

static int cu_load(void) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA("nvcuda.dll");
    if (!h) return 0;
    cuapi.h = (void *)h;
#else
    cuapi.h = dlopen("libcuda.so.1", RTLD_NOW);
    if (!cuapi.h) cuapi.h = dlopen("libcuda.so", RTLD_NOW);
    if (!cuapi.h) return 0;
#endif
    int opt = 0;
    CU_SYM(Init, "cuInit"); CU_SYM(DeviceGetCount, "cuDeviceGetCount"); CU_SYM(DeviceGet, "cuDeviceGet"); CU_SYM(DeviceGetName, "cuDeviceGetName");
    CU_SYM(DeviceGetAttribute, "cuDeviceGetAttribute"); CU_SYM(DeviceTotalMem, "cuDeviceTotalMem_v2");
    CU_SYM(PrimaryRetain, "cuDevicePrimaryCtxRetain"); CU_SYM(CtxSetCurrent, "cuCtxSetCurrent"); CU_SYM(MemGetInfo, "cuMemGetInfo_v2");
    CU_SYM(MemAlloc, "cuMemAlloc_v2"); CU_SYM(MemFree, "cuMemFree_v2"); CU_SYM(HtoD, "cuMemcpyHtoD_v2"); CU_SYM(DtoH, "cuMemcpyDtoH_v2");
    CU_SYM(HtoDA, "cuMemcpyHtoDAsync_v2"); CU_SYM(DtoHA, "cuMemcpyDtoHAsync_v2"); CU_SYM(MemsetD8, "cuMemsetD8_v2");
    CU_SYM(HostAlloc, "cuMemHostAlloc"); CU_SYM(HostFree, "cuMemFreeHost");
    CU_SYM(ModuleLoadDataEx, "cuModuleLoadDataEx"); CU_SYM(ModuleGetFunction, "cuModuleGetFunction"); CU_SYM(FuncSetAttribute, "cuFuncSetAttribute");
    CU_SYM(LaunchKernel, "cuLaunchKernel"); CU_SYM(StreamCreate, "cuStreamCreate"); CU_SYM(StreamSynchronize, "cuStreamSynchronize");
    CU_SYM(StreamWaitEvent, "cuStreamWaitEvent"); CU_SYM(CtxSynchronize, "cuCtxSynchronize");
    CU_SYM(EventCreate, "cuEventCreate"); CU_SYM(EventRecord, "cuEventRecord"); CU_SYM(EventSynchronize, "cuEventSynchronize");
    CU_SYM(EventElapsed, "cuEventElapsedTime"); CU_SYM(GetErrorString, "cuGetErrorString");
    opt = 1;
    CU_SYM(StreamBeginCapture, "cuStreamBeginCapture_v2"); CU_SYM(StreamEndCapture, "cuStreamEndCapture"); CU_SYM(GraphInstantiate, "cuGraphInstantiateWithFlags");
    CU_SYM(GraphLaunch, "cuGraphLaunch"); CU_SYM(GraphExecDestroy, "cuGraphExecDestroy"); CU_SYM(GraphDestroy, "cuGraphDestroy");
    return 1;
}

/* atributos do cuDeviceGetAttribute */
enum { CUA_SMEM_OPTIN = 97, CUA_SM = 16, CUA_CLOCK = 13, CUA_MEMCLK = 36, CUA_BUS = 37, CUA_L2 = 38, CUA_MAJOR = 75, CUA_MINOR = 76 };

/* 1 se ha libcuda e ao menos uma placa com cc >= 7.5. KATANA_CPU=1 e KATANA_BACKEND=cpu/vulkan desligam. */
static int cu_init(void) {
    CuDev *d = &cu_dev;
    if (d->tried) return d->ok;
    d->tried = 1;
    const char *be = getenv("KATANA_BACKEND");
    if ((getenv("KATANA_CPU") && atoi(getenv("KATANA_CPU"))) || (be && be[0] && strcmp(be, "cuda"))) return 0;
    d->debug = getenv("KATANA_CUDA_DEBUG") != NULL;
    if (!cu_load()) return 0;
    int n = 0, dev = 0;
    if (cuapi.Init(0) != 0 || cuapi.DeviceGetCount(&n) != 0 || n < 1) return 0;
    if (cuapi.DeviceGet(&dev, 0) != 0) return 0;
    cuapi.DeviceGetName(d->name, sizeof d->name, dev);
    cuapi.DeviceGetAttribute(&d->sm_count, CUA_SM, dev); cuapi.DeviceGetAttribute(&d->clock_khz, CUA_CLOCK, dev);
    cuapi.DeviceGetAttribute(&d->mem_clock_khz, CUA_MEMCLK, dev); cuapi.DeviceGetAttribute(&d->bus_bits, CUA_BUS, dev);
    cuapi.DeviceGetAttribute(&d->l2, CUA_L2, dev); cuapi.DeviceGetAttribute(&d->smem_optin, CUA_SMEM_OPTIN, dev);
    cuapi.DeviceGetAttribute(&d->cc_major, CUA_MAJOR, dev); cuapi.DeviceGetAttribute(&d->cc_minor, CUA_MINOR, dev);
    if (d->cc_major * 10 + d->cc_minor < 75) { fprintf(stderr, "cuda: %s tem cc %d.%d (< 7.5), sem mma f16 nem cp.async\n", d->name, d->cc_major, d->cc_minor); return 0; }
    if (!cu_chk(cuapi.PrimaryRetain(&d->ctx, dev), "cuDevicePrimaryCtxRetain") || !cu_chk(cuapi.CtxSetCurrent(d->ctx), "cuCtxSetCurrent")) return 0;
    if (!cu_chk(cuapi.StreamCreate(&d->stream, 0), "cuStreamCreate")) return 0;
    cuapi.MemGetInfo(&d->vram_free, &d->vram_total);
    /* f16 com acumulo f32: GA10x consumer = 256 FMA/clock/SM (a metade do acumulo f16); banda = 2 * clock da memoria * barramento / 8 */
    d->peak_tflops_f16 = (double)d->sm_count * 256.0 * 2.0 * (double)d->clock_khz * 1e3 / 1e12;
    d->peak_gbps = 2.0 * (double)d->mem_clock_khz * 1e3 * (double)d->bus_bits / 8.0 / 1e9;
    d->ok = 1;
    return 1;
}

static CUdptr_ cu_alloc(size_t n) { CUdptr_ p = 0; if (!cu_chk(cuapi.MemAlloc(&p, n ? n : 1), "cuMemAlloc")) return 0; return p; }
static void cu_free(CUdptr_ p) { if (p) cuapi.MemFree(p); }
static int cu_up(CUdptr_ d, const void *h, size_t n) { return cu_chk(cuapi.HtoD(d, h, n), "cuMemcpyHtoD"); }
static int cu_down(void *h, CUdptr_ d, size_t n) { return cu_chk(cuapi.DtoH(h, d, n), "cuMemcpyDtoH"); }
static int cu_up_async(CUdptr_ d, const void *h, size_t n) { return cu_chk(cuapi.HtoDA(d, h, n, cu_dev.stream), "cuMemcpyHtoDAsync"); }
static int cu_down_async(void *h, CUdptr_ d, size_t n) { return cu_chk(cuapi.DtoHA(h, d, n, cu_dev.stream), "cuMemcpyDtoHAsync"); }
static void cu_zero(CUdptr_ d, size_t n) { cuapi.MemsetD8(d, 0, n); }
static int cu_sync(void) { return cu_chk(cuapi.StreamSynchronize(cu_dev.stream), "cuStreamSynchronize"); }

/* modulo a partir de PTX em texto; o log do JIT vai a stderr se der erro */
static void *cu_module(const char *ptx) {
    char elog[8192] = {0};
    int opts[3] = { 5 /* ERROR_LOG_BUFFER */, 6 /* ERROR_LOG_BUFFER_SIZE_BYTES */, 0 };
    void *ov[3] = { elog, (void *)(size_t)sizeof elog, NULL };
    void *m = NULL;
    CUresult_ r = cuapi.ModuleLoadDataEx(&m, ptx, 2, opts, ov);
    if (r != 0) { fprintf(stderr, "cuda: o driver recusou o PTX (erro %d, driver antigo demais ou placa sem suporte?)\n%s\n", r, elog); return NULL; }
    return m;
}
static void *cu_fn(void *mod, const char *name) {
    void *f = NULL;
    if (!cu_chk(cuapi.ModuleGetFunction(&f, mod, name), name)) return NULL;
    return f;
}
/* lancamento com argumentos por ponteiro (array de void*) */
static int cu_launch(void *fn, int gx, int gy, int gz, int bx, int smem, void **args) {
    return cu_chk(cuapi.LaunchKernel(fn, (unsigned)gx, (unsigned)gy, (unsigned)gz, (unsigned)bx, 1, 1, (unsigned)smem, cu_dev.stream, args, NULL), "cuLaunchKernel");
}
static void *cu_ev_new(void) { void *e = NULL; cuapi.EventCreate(&e, 0); return e; }
static void cu_ev_rec(void *e) { cuapi.EventRecord(e, cu_dev.stream); }
static float cu_ev_ms(void *a, void *b) { float ms = 0; cuapi.EventSynchronize(b); cuapi.EventElapsed(&ms, a, b); return ms; }
