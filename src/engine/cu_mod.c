/* CUDA: junta a camada de execucao, o emissor de PTX dos produtos e o PTX dos kernels de apoio; cu_kmod() compila os ultimos uma vez. */
#include "engine/cu_rt.c"
#include "engine/cu_ptx.c"
#include "engine/cu_kern.inc"
static void *cu_kmod(void) {
    static void *m; static int tried;
    if (!tried) { tried = 1; m = cu_module((const char *)cu_kern_ptx); }
    return m;
}
