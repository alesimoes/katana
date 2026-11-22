/* GPU de mentira para o treino (so com -DKATANA_TG_EMU, em teste): as chamadas kvk_* do treinador vao para buffers
   na memoria e o produto roda o codigo do shader na CPU (tools/traintest/tg_emu.cpp). Confere o lado do host:
   fatias, pesos residentes ou enviados na hora, reenvio dos mestres depois do passo, acumulacao. */
void tg_emu_run(int mode, int wt, const void *w, const float *x, float *o, uint32_t M, uint32_t N, uint32_t K,
                uint32_t ws, uint32_t wo, uint32_t gx, uint32_t gy);
static int tge_mode[64], tge_wt[64], tge_np;
static size_t tge_bytes;
static int tge_init(KvkInfo *I) {
    memset(I, 0, sizeof *I);
    snprintf(I->name, sizeof I->name, "GPU emulada (teste)");
    I->vram_bytes = (uint64_t)1 << 30; I->max_storage_range = (uint64_t)1 << 30; I->max_alloc = (uint64_t)1 << 30;
    return 1;
}
static uint64_t tge_budget(void) { return (uint64_t)1 << 30; }
static void tge_shutdown(void) {}
static const char *tge_error(void) { return "emulada"; }
static KvkPipe tge_pipe(const uint32_t *spv, size_t bytes, int nb, int push, const uint32_t *ids, const uint32_t *vals, int ns) {
    (void)spv; (void)bytes; (void)nb; (void)push; (void)ids; (void)ns;
    KvkPipe p; memset(&p, 0, sizeof p);
    tge_mode[tge_np] = (int)vals[0]; tge_wt[tge_np] = (int)vals[1];
    p.id = (uint32_t)tge_np++; p.p = (VkPipeline)(uintptr_t)(p.id + 1);
    return p;
}
static KvkBuf tge_create(size_t bytes, int usage) {
    (void)usage;
    KvkBuf b; memset(&b, 0, sizeof b);
    void *m = calloc(bytes + 4, 1);
    b.b = (VkBuffer)(uintptr_t)m; b.size = bytes; tge_bytes += bytes;
    return b;
}
static int tge_write(KvkBuf b, size_t off, const void *src, size_t n) {
    if (off + n > b.size) { fprintf(stderr, "tg_emu: escrita fora do buffer\n"); return 0; }
    memcpy((char *)(uintptr_t)b.b + off, src, n); return 1;
}
static int tge_read(KvkBuf b, size_t off, void *dst, size_t n) {
    if (off + n > b.size) { fprintf(stderr, "tg_emu: leitura fora do buffer\n"); return 0; }
    memcpy(dst, (char *)(uintptr_t)b.b + off, n); return 1;
}
static int tge_dispatch(KvkPipe p, const KvkBuf *bufs, int n, const void *push, int pb, uint32_t gx, uint32_t gy, uint32_t gz) {
    (void)n; (void)pb; (void)gz;
    const uint32_t *pc = (const uint32_t *)push;      /* M, N, K, ws, wo */
    const int mode = tge_mode[p.id];
    /* confere que o produto nao passa do fim dos buffers (o que a GPU de verdade leria como lixo) */
    const size_t on = mode == 2 ? (size_t)pc[0] * pc[1] : (size_t)pc[0] * pc[1];
    if (on * 4 > bufs[2].size) { fprintf(stderr, "tg_emu: saida passa do buffer\n"); return 0; }
    tg_emu_run(mode, tge_wt[p.id], (const void *)(uintptr_t)bufs[0].b, (const float *)(uintptr_t)bufs[1].b,
               (float *)(uintptr_t)bufs[2].b, pc[0], pc[1], pc[2], pc[3], pc[4], gx, gy);
    return 1;
}
static int tge_flush(void) { return 1; }
#define kvk_buf_write_q tge_write
#define kvk_flush tge_flush
#define kvk_init tge_init
#define kvk_vram_budget tge_budget
#define kvk_shutdown tge_shutdown
#define kvk_error tge_error
#define kvk_pipe_create tge_pipe
#define kvk_buf_create tge_create
#define kvk_buf_write tge_write
#define kvk_buf_read tge_read
#define kvk_dispatch tge_dispatch
