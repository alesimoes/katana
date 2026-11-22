/* Medida dos produtos do treino na GPU sem modelo: pesos Q4_K sinteticos do tamanho de um especialista, tg_prod nos modos
   0 e 1 com varios tamanhos de lote, pesos residentes e enviados na hora, conferidos contra a CPU.
     gcc -O2 -mavx2 -mfma -mf16c -ffast-math -w -I . -I src -I vk/include tools/traintest/tg_bench.c -o /tmp/tg_bench -lm -lpthread -ldl
     /tmp/tg_bench [linhas colunas] */
#define main katana_main
#include "src/katana.c"
#undef main

int main(int argc, char **argv) {
    const int R = argc > 2 ? atoi(argv[1]) : 512, C = argc > 2 ? atoi(argv[2]) : 2048, NW = 64;
    pool_init(); srand(5);
    const size_t rb = qbytes(T_Q4_K, (size_t)C);
    float *w = xalloc((size_t)C, 4);
    Mat *mat = xalloc(NW, sizeof(Mat)); TPar *par = xalloc(NW, sizeof(TPar));
    uint8_t *big = xalloc((size_t)NW * R * rb, 1);
    for (int k = 0; k < NW; k++) {
        for (int r = 0; r < R; r++) {
            for (int c = 0; c < C; c++) w[c] = ((float)rand() / RAND_MAX - 0.5f) * 0.1f;
            enc_block(T_Q4_K, w, big + ((size_t)k * R + r) * rb, (size_t)C);
        }
        memset(&mat[k], 0, sizeof(Mat)); mat[k].q = big + (size_t)k * R * rb; mat[k].type = T_Q4_K; mat[k].stride = rb;
        memset(&par[k], 0, sizeof(TPar)); par[k].mat = &mat[k]; par[k].rows = R; par[k].cols = C;
    }
    tg_init();
    if (!tg.on) { printf("sem GPU\n"); return 0; }
    tg.min_mac = 0;
    const int Bs[] = { 1, 4, 8, 16, 32, 64, 128 };
    for (int mode = 0; mode < (getenv("BENCH_FLOOR") ? 0 : 2); mode++) for (int res = 0; res < 2; res++) for (int bi = 0; bi < 7; bi++) {
        const int B = Bs[bi];
        const size_t nin = (size_t)B * (mode == 0 ? C : R), nout = (size_t)B * (mode == 0 ? R : C);
        float *in = xalloc(nin, 4), *o1 = xalloc(nout, 4), *o2 = xalloc(nout, 4);
        for (size_t i = 0; i < nin; i++) in[i] = (float)rand() / RAND_MAX - 0.5f;
        if (res) { tg.budget = (size_t)4 << 30; } else tg.budget = 0;
        for (int k = 0; k < NW; k++) { par[k].gstat = 0; }
        tg.resident = 0;
        double t0 = now_sec(); int n = 0;
        for (int rep = 0; rep < 2; rep++) for (int k = 0; k < NW; k++, n++) {
            if (mode == 0) { if (!tg_prod(0, &par[k], in, NULL, o1, B)) { printf("tg_prod recusou\n"); return 1; } }
            else { memset(o1, 0, nout * 4); if (!tg_prod(1, &par[k], in, NULL, o1, B)) { printf("tg_prod recusou\n"); return 1; } }
        }
        const double dt = (now_sec() - t0) / n;
        TMvJob j = { &par[NW - 1], o2, in, NULL, B };
        TMtJob jt = { &par[NW - 1], o2, in, B };
        if (mode == 0) pool_for(tmv_job, &j, R, 16); else { memset(o2, 0, nout * 4); pool_for(tmt_job, &jt, C, C / pool.n + 1); }
        double mx = 1e-30, er = 0;
        for (size_t i = 0; i < nout; i++) { mx = fmax(mx, fabs(o2[i])); er = fmax(er, fabs(o1[i] - o2[i])); }
        printf("modo %d %-9s B=%3d  %7.1f us por produto  erro relativo %.1e\n", mode, res ? "residente" : "na hora", B, dt * 1e6, er / mx);
        free(in); free(o1); free(o2);
    }
    /* piso por submissao: despachos do mesmo produto, 1 ou 16 por espera, e um despacho vazio */
    {
        tg.budget = (size_t)4 << 30; par[0].gstat = 0;
        float *in = xalloc((size_t)C * 4, 4), *o = xalloc((size_t)R * 4, 4);
        tg_prod(0, &par[0], in, NULL, o, 4);
        const int rr = tg_resident(&par[0]);
        KvkBuf wb = tg.ar[par[0].gar];
        const int per[3] = { 1, 4, 16 };
        for (int e = 0; e < 3; e++) {
            double t0 = now_sec(); const int it = 400;
            for (int i = 0; i < it; i++) {
                for (int q = 0; q < per[e]; q++) tg_run(3, 3, wb, tg.x, tg.o, (uint32_t)R, 4, (uint32_t)C, rb, par[0].goff);
                kvk_wait();
            }
            printf("residente=%d %2d despachos 512x2048 B=4 por espera: %.1f us por espera, %.1f us por despacho\n", rr, per[e], (now_sec() - t0) / it * 1e6, (now_sec() - t0) / it / per[e] * 1e6);
        }
        double t0 = now_sec();
        for (int i = 0; i < 400; i++) { tg_run(3, 0, wb, tg.x, tg.o, 8, 1, 8, 32, 0); kvk_wait(); }
        printf("despacho minimo + espera: %.1f us\n", (now_sec() - t0) / 400 * 1e6);
    }
    return 0;
}
