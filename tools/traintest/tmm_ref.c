/* casos de teste do produto do treino na GPU (vk/shaders/tmm_core.glsl): pesos codificados pelo proprio katana em
   cada formato, entradas aleatorias e as respostas calculadas com o dequant da CPU, em double. Grava um arquivo que o
   tools/traintest/tmm_emu.cpp le (ele roda o codigo do shader na CPU e compara).
     gcc -O1 -mavx2 -mfma -mf16c -w -I . -I src -I vk/include tools/traintest/tmm_ref.c -o /tmp/tmm_ref -lm -lpthread -ldl
     /tmp/tmm_ref /tmp/tmm_cases.bin */
#define main katana_main
#include "src/katana.c"
#undef main

static void put(FILE *f, const void *p, size_t n) { if (fwrite(p, 1, n, f) != n) die("tmm_ref: escrita"); }

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "uso: tmm_ref <saida>\n"); return 1; }
    FILE *f = fopen(argv[1], "wb");
    if (!f) die("tmm_ref: nao abriu a saida");
    /* WT do shader: 0 F32, 1 F16, 2 Q8_0, 3 Q4_K, 4 Q5_K, 5 Q6_K, 6 BF16 */
    const uint32_t types[7] = { T_F32, T_F16, T_Q8_0, T_Q4_K, T_Q5_K, T_Q6_K, T_BF16 };
    const int R = 70, C = 512, N = 37, WO = 12;      /* bordas fora do ladrilho de 64 e peso deslocado no buffer */
    srand(3);
    uint32_t n = 7; put(f, &n, 4);
    for (int wt = 0; wt < 7; wt++) {
        const uint32_t t = types[wt];
        float *w = xalloc((size_t)R * C, 4), *wd = xalloc((size_t)R * C, 4);
        for (size_t i = 0; i < (size_t)R * C; i++) w[i] = ((float)rand() / RAND_MAX - 0.5f) * (1.0f + (float)(i % 7));
        const size_t rb = qbytes(t, (size_t)C);
        uint8_t *q = xalloc((size_t)R * rb + WO, 1);
        for (int r = 0; r < R; r++) {
            uint8_t *dst = q + WO + (size_t)r * rb;
            const float *x = w + (size_t)r * C;
            if (t == T_F32) memcpy(dst, x, (size_t)C * 4);
            else if (t == T_F16) for (int c = 0; c < C; c++) { const uint16_t h = f32_to_f16(x[c]); memcpy(dst + 2 * c, &h, 2); }
            else if (t == T_BF16) for (int c = 0; c < C; c++) { uint32_t u; memcpy(&u, x + c, 4); const uint16_t h = (uint16_t)(u >> 16); memcpy(dst + 2 * c, &h, 2); }
            else enc_block(t, x, dst, (size_t)C);
            dequant(t, dst, wd + (size_t)r * C, (size_t)C);      /* o que a CPU le deste formato */
        }
        float *x = xalloc((size_t)N * C, 4), *dy = xalloc((size_t)N * R, 4);
        for (size_t i = 0; i < (size_t)N * C; i++) x[i] = (float)rand() / RAND_MAX - 0.5f;
        for (size_t i = 0; i < (size_t)N * R; i++) dy[i] = (float)rand() / RAND_MAX - 0.5f;
        float *y = xalloc((size_t)N * R, 4), *dx = xalloc((size_t)N * C, 4), *g = xalloc((size_t)R * C, 4);
        for (int b = 0; b < N; b++) for (int r = 0; r < R; r++) {
            double s = 0; for (int c = 0; c < C; c++) s += (double)wd[(size_t)r * C + c] * x[(size_t)b * C + c];
            y[(size_t)b * R + r] = (float)s;
        }
        for (int b = 0; b < N; b++) for (int c = 0; c < C; c++) {
            double s = 0; for (int r = 0; r < R; r++) s += (double)dy[(size_t)b * R + r] * wd[(size_t)r * C + c];
            dx[(size_t)b * C + c] = (float)s;
        }
        for (int r = 0; r < R; r++) for (int c = 0; c < C; c++) {
            double s = 0; for (int b = 0; b < N; b++) s += (double)dy[(size_t)b * R + r] * x[(size_t)b * C + c];
            g[(size_t)r * C + c] = (float)s;
        }
        uint32_t hdr[6] = { (uint32_t)wt, (uint32_t)R, (uint32_t)C, (uint32_t)N, (uint32_t)rb, (uint32_t)WO };
        const uint32_t qn = (uint32_t)((size_t)R * rb + WO);
        put(f, hdr, sizeof hdr); put(f, &qn, 4); put(f, q, qn);
        put(f, x, (size_t)N * C * 4); put(f, dy, (size_t)N * R * 4);
        put(f, y, (size_t)N * R * 4); put(f, dx, (size_t)N * C * 4); put(f, g, (size_t)R * C * 4);
        free(w); free(wd); free(q); free(x); free(dy); free(y); free(dx); free(g);
    }
    fclose(f);
    return 0;
}
