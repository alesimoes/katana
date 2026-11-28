/* vkgemm_mobile: mede, no aparelho real (sem matriz cooperativa), o ganho de organizar o shader do
 * produto em lote - ladrilho + memoria compartilhada + leitura vetorizada (sem nenhum hardware especial)
 * e, por cima, o produto inteiro empacotado (VK_KHR_shader_integer_dot_product) - contra o shader ingenuo
 * (uma saida por thread, sem ladrilho). Mesma progressao que tools/vkgemm mediu no notebook (roadmap,
 * secao 0: 0.51 -> 1.17-1.75 -> 1.75-2.6 T mult/s), reproduzida aqui para o Xclipse 540 (Galaxy A56).
 * Confere cada resultado contra uma referencia na CPU antes de cronometrar (validacao numerica). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#define KVK_IMPL
#include "../../vk/kvk.h"
#include "naive.h"
#include "tiled_vec.h"
#include "tiled_int8dp.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* meio-float (IEEE 754 binary16) so para gravar os buffers de entrada; o shader faz a aritmetica */
static uint16_t f32_to_f16(float f) {
    uint32_t x; memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    int32_t exp = ((x >> 23) & 0xFF) - 127 + 15;
    uint32_t man = x & 0x7FFFFF;
    if (exp <= 0) return (uint16_t)sign;
    if (exp >= 31) return (uint16_t)(sign | 0x7C00);
    return (uint16_t)(sign | (exp << 10) | (man >> 13));
}

int main(int argc, char **argv) {
    const uint32_t ROWS = argc > 1 ? (uint32_t)atoi(argv[1]) : 2048;
    const uint32_t COLS = argc > 2 ? (uint32_t)atoi(argv[2]) : 2048;
    const uint32_t TOK  = argc > 3 ? (uint32_t)atoi(argv[3]) : 256;
    const int ITERS = argc > 4 ? atoi(argv[4]) : 20;
    printf("forma: ROWS=%u COLS=%u TOKENS=%u, %d iteracoes\n", ROWS, COLS, TOK, ITERS);

    KvkInfo I;
    if (!kvk_init(&I)) { printf("sem Vulkan: %s\n", kvk_error()); return 1; }
    printf("device\t%s\tsubgroup=%u\tshared=%u\tcoopmat=%d\tint_dot=%d\tfp16=%d\n",
           I.name, I.subgroup_size, I.max_shared, I.coopmat, I.int_dot, I.fp16);

    srand(12345);
    float *Wf = malloc((size_t)ROWS * COLS * 4), *Xf = malloc((size_t)COLS * TOK * 4);
    for (size_t i = 0; i < (size_t)ROWS * COLS; i++) Wf[i] = (rand() / (float)RAND_MAX - 0.5f) * 0.1f;
    for (size_t i = 0; i < (size_t)COLS * TOK; i++) Xf[i] = (rand() / (float)RAND_MAX - 0.5f) * 0.1f;

    /* referencia fp32 na CPU, para conferir naive e tiled_vec (mesma entrada, mesmo layout logico) */
    double *Yref = malloc((size_t)ROWS * TOK * 8);
    for (uint32_t r = 0; r < ROWS; r++)
        for (uint32_t t = 0; t < TOK; t++) {
            double acc = 0;
            for (uint32_t c = 0; c < COLS; c++) acc += (double)Wf[(size_t)r * COLS + c] * (double)Xf[(size_t)c * TOK + t];
            Yref[(size_t)r * TOK + t] = acc;
        }
    printf("referencia CPU calculada (fp64)\n");

    /* buffers fp16 (W linha a linha, X transposto: x[col*TOK+tok]) */
    uint16_t *Wh = malloc((size_t)ROWS * COLS * 2), *Xh = malloc((size_t)COLS * TOK * 2);
    for (size_t i = 0; i < (size_t)ROWS * COLS; i++) Wh[i] = f32_to_f16(Wf[i]);
    for (size_t i = 0; i < (size_t)COLS * TOK; i++) Xh[i] = f32_to_f16(Xf[i]);

    KvkBuf wb = kvk_buf_create((size_t)ROWS * COLS * 2, KVK_USAGE_RW);
    KvkBuf xb = kvk_buf_create((size_t)COLS * TOK * 2, KVK_USAGE_RW);
    KvkBuf yb = kvk_buf_create((size_t)ROWS * TOK * 4, KVK_USAGE_RW);
    if (!wb.b || !xb.b || !yb.b) { printf("falha ao alocar buffers: %s\n", kvk_error()); return 1; }
    kvk_buf_write(wb, 0, Wh, (size_t)ROWS * COLS * 2);
    kvk_buf_write(xb, 0, Xh, (size_t)COLS * TOK * 2);

    float *Yout = malloc((size_t)ROWS * TOK * 4);
    const double tflop_mults = (double)ROWS * COLS * TOK;   /* uma multiplicacao-soma por elemento da saida x COLS */

    /* ---- naive ---- */
    {
        KvkPipe p = kvk_pipe_create(naive_spv, sizeof naive_spv, 3, 12, NULL, NULL, 0);
        if (!p.p) { printf("naive: pipeline falhou: %s\n", kvk_error()); return 1; }
        const KvkBuf bufs[3] = { wb, xb, yb };
        struct { uint32_t rows, cols, tokens; } pc = { ROWS, COLS, TOK };
        const uint32_t ng = (ROWS * TOK + 255) / 256;
        if (!kvk_dispatch(p, bufs, 3, &pc, sizeof pc, ng, 1, 1) || !kvk_wait()) { printf("naive: despacho falhou: %s\n", kvk_error()); return 1; }
        kvk_buf_read(yb, 0, Yout, (size_t)ROWS * TOK * 4);
        double maxerr = 0; for (size_t i = 0; i < (size_t)ROWS * TOK; i++) { double e = fabs(Yout[i] - Yref[i]); if (e > maxerr) maxerr = e; }
        printf("naive: erro maximo vs CPU = %.6g\n", maxerr);

        double t0 = now();
        for (int it = 0; it < ITERS; it++) kvk_dispatch(p, bufs, 3, &pc, sizeof pc, ng, 1, 1);
        kvk_wait();
        double dt = (now() - t0) / ITERS;
        printf("naive: %.2f ms/passo, %.3f T mult/s\n", dt * 1000, tflop_mults / dt / 1e12);
        kvk_pipe_destroy(&p);
    }

    /* ---- tiled + vetorizado (fp16) ---- */
    {
        KvkPipe p = kvk_pipe_create(tiled_vec_spv, sizeof tiled_vec_spv, 3, 12, NULL, NULL, 0);
        if (!p.p) { printf("tiled_vec: pipeline falhou: %s\n", kvk_error()); return 1; }
        const KvkBuf bufs[3] = { wb, xb, yb };
        struct { uint32_t rows, cols, tokens; } pc = { ROWS, COLS, TOK };
        const uint32_t gx = ROWS / 64, gy = TOK / 64;
        if (gx * 64 != ROWS || gy * 64 != TOK) { printf("tiled_vec: ROWS e TOKENS precisam ser multiplos de 64 (pulado)\n"); }
        else {
            if (!kvk_dispatch(p, bufs, 3, &pc, sizeof pc, gx, gy, 1) || !kvk_wait()) { printf("tiled_vec: despacho falhou: %s\n", kvk_error()); return 1; }
            kvk_buf_read(yb, 0, Yout, (size_t)ROWS * TOK * 4);
            double maxerr = 0; for (size_t i = 0; i < (size_t)ROWS * TOK; i++) { double e = fabs(Yout[i] - Yref[i]); if (e > maxerr) maxerr = e; }
            printf("tiled_vec: erro maximo vs CPU = %.6g\n", maxerr);

            double t0 = now();
            for (int it = 0; it < ITERS; it++) kvk_dispatch(p, bufs, 3, &pc, sizeof pc, gx, gy, 1);
            kvk_wait();
            double dt = (now() - t0) / ITERS;
            printf("tiled_vec: %.2f ms/passo, %.3f T mult/s\n", dt * 1000, tflop_mults / dt / 1e12);
        }
        kvk_pipe_destroy(&p);
    }

    /* ---- ladrilho + produto inteiro empacotado (int8) ---- */
    if (I.int_dot) {
        /* layout proprio: W e X empacotados ao longo de K (cols), 4 int8 por uint32; X por linha de token
           (nao transposto). Entrada int8 diretamente aleatoria (nao veio da quantizacao de Wf/Xf: o alvo
           aqui e conferir a instrucao de produto, nao uma pipeline de quantizacao) */
        if (COLS % 4 != 0) { printf("tiled_int8dp: COLS precisa ser multiplo de 4 (pulado)\n"); }
        else {
            const uint32_t cols4 = COLS / 4;
            int8_t *Wi = malloc((size_t)ROWS * COLS), *Xi = malloc((size_t)TOK * COLS);
            for (size_t i = 0; i < (size_t)ROWS * COLS; i++) Wi[i] = (int8_t)((rand() % 41) - 20);
            for (size_t i = 0; i < (size_t)TOK * COLS; i++) Xi[i] = (int8_t)((rand() % 41) - 20);
            int64_t *Yrefi = malloc((size_t)ROWS * TOK * 8);
            for (uint32_t r = 0; r < ROWS; r++)
                for (uint32_t t = 0; t < TOK; t++) {
                    int64_t acc = 0;
                    for (uint32_t c = 0; c < COLS; c++) acc += (int64_t)Wi[(size_t)r * COLS + c] * (int64_t)Xi[(size_t)t * COLS + c];
                    Yrefi[(size_t)r * TOK + t] = acc;
                }
            printf("referencia CPU (int8dp) calculada\n");
            uint32_t *Wp = malloc((size_t)ROWS * cols4 * 4), *Xp = malloc((size_t)TOK * cols4 * 4);
            for (uint32_t r = 0; r < ROWS; r++) for (uint32_t c4 = 0; c4 < cols4; c4++) {
                uint32_t v = 0; for (int b = 0; b < 4; b++) v |= ((uint32_t)(uint8_t)Wi[(size_t)r * COLS + c4 * 4 + b]) << (8 * b);
                Wp[(size_t)r * cols4 + c4] = v;
            }
            for (uint32_t t = 0; t < TOK; t++) for (uint32_t c4 = 0; c4 < cols4; c4++) {
                uint32_t v = 0; for (int b = 0; b < 4; b++) v |= ((uint32_t)(uint8_t)Xi[(size_t)t * COLS + c4 * 4 + b]) << (8 * b);
                Xp[(size_t)t * cols4 + c4] = v;
            }
            KvkBuf wib = kvk_buf_create((size_t)ROWS * cols4 * 4, KVK_USAGE_RW);
            KvkBuf xib = kvk_buf_create((size_t)TOK * cols4 * 4, KVK_USAGE_RW);
            KvkBuf yib = kvk_buf_create((size_t)ROWS * TOK * 4, KVK_USAGE_RW);
            kvk_buf_write(wib, 0, Wp, (size_t)ROWS * cols4 * 4);
            kvk_buf_write(xib, 0, Xp, (size_t)TOK * cols4 * 4);

            KvkPipe p = kvk_pipe_create(tiled_int8dp_spv, sizeof tiled_int8dp_spv, 3, 12, NULL, NULL, 0);
            if (!p.p) { printf("tiled_int8dp: pipeline falhou: %s\n", kvk_error()); return 1; }
            const KvkBuf bufs[3] = { wib, xib, yib };
            struct { uint32_t rows, cols, tokens; } pc = { ROWS, COLS, TOK };
            const uint32_t gx = ROWS / 64, gy = TOK / 64;
            if (gx * 64 != ROWS || gy * 64 != TOK) printf("tiled_int8dp: ROWS e TOKENS precisam ser multiplos de 64 (pulado)\n");
            else {
                if (!kvk_dispatch(p, bufs, 3, &pc, sizeof pc, gx, gy, 1) || !kvk_wait()) { printf("tiled_int8dp: despacho falhou: %s\n", kvk_error()); return 1; }
                int32_t *Yi = malloc((size_t)ROWS * TOK * 4);
                kvk_buf_read(yib, 0, Yi, (size_t)ROWS * TOK * 4);
                int64_t maxerr = 0; for (size_t i = 0; i < (size_t)ROWS * TOK; i++) { int64_t e = llabs((int64_t)Yi[i] - Yrefi[i]); if (e > maxerr) maxerr = e; }
                printf("tiled_int8dp: erro maximo vs CPU (inteiro, deve ser 0) = %lld\n", (long long)maxerr);

                double t0 = now();
                for (int it = 0; it < ITERS; it++) kvk_dispatch(p, bufs, 3, &pc, sizeof pc, gx, gy, 1);
                kvk_wait();
                double dt = (now() - t0) / ITERS;
                printf("tiled_int8dp: %.2f ms/passo, %.3f T mult/s\n", dt * 1000, tflop_mults / dt / 1e12);
                free(Yi);
            }
            kvk_pipe_destroy(&p);
            kvk_buf_destroy(&wib); kvk_buf_destroy(&xib); kvk_buf_destroy(&yib);
            free(Wi); free(Xi); free(Yrefi); free(Wp); free(Xp);
        }
    } else {
        printf("tiled_int8dp: VK_KHR_shader_integer_dot_product ausente neste aparelho, pulado\n");
    }

    kvk_shutdown();
    return 0;
}
