/* ============================ render: operacoes de imagem na CPU ===============================
 * Convolucao 2D, normalizacao por grupos, SiLU, ampliacao por vizinho mais proximo. float32, um quadro por vez (lote 1).
 *
 * Disposicao dos dados: NHWC, isto e, [linha][coluna][canal] com o canal contiguo. Assim um mapa de ativacao e uma matriz
 * [pixel][canal], que e o que a atencao e as camadas lineares do transformador da UNet ja esperam, e a convolucao vira um produto
 * de matrizes (im2col em blocos + vt_gemm_packed de vision/tensor.c). Os pesos de convolucao do GGUF estao em [saida][entrada][kh][kw]
 * (a ordem do safetensors); aqui eles sao reordenados uma vez para [saida][kh][kw][entrada], que casa com o im2col. */

typedef struct { VtMat W; void *own; const float *bias; int cin, cout, k; float *packed; } SdrConv;      /* packed: pesos ja nos paineis do produto (cache), ou NULL */       /* W: cout linhas x (k*k*cin) colunas */

/* monta uma convolucao a partir do GGUF: <nome>.weight [cout,cin,k,k] e <nome>.bias [cout]; 1 = ok */
static int sdr_conv_load(const SdgFile *g, const char *name, SdrConv *c) {
    char nm[200];
    snprintf(nm, sizeof nm, "%s.weight", name);
    const SdgInfo *w = sdg_find(g, nm);
    if (!w || w->nd != 4 || w->dim[0] != w->dim[1]) { fprintf(stderr, "render: convolucao %s ausente ou com forma inesperada\n", nm); return 0; }
    snprintf(nm, sizeof nm, "%s.bias", name);
    const SdgInfo *b = sdg_find(g, nm);
    if (!b || b->nd != 1 || b->type != 0) { fprintf(stderr, "render: vies %s ausente ou nao e F32\n", nm); return 0; }
    c->k = (int)w->dim[0]; c->cin = (int)w->dim[2]; c->cout = (int)w->dim[3];
    if ((int)b->dim[0] != c->cout) { fprintf(stderr, "render: vies de %s com tamanho errado\n", name); return 0; }
    if (w->type > 1) { fprintf(stderr, "render: pesos de %s com tipo %u\n", name, w->type); return 0; }
    const int es = w->type == 0 ? 4 : 2, K = c->k, Cin = c->cin, Cout = c->cout;
    uint8_t *dst = xalloc((size_t)Cout * K * K * Cin, (size_t)es);
    const uint8_t *src = sdg_data(g, w);
    for (int o = 0; o < Cout; o++)
        for (int i = 0; i < Cin; i++)
            for (int ky = 0; ky < K; ky++)
                for (int kx = 0; kx < K; kx++)
                    memcpy(dst + ((((size_t)o * K + ky) * K + kx) * Cin + i) * es, src + ((((size_t)o * Cin + i) * K + ky) * K + kx) * es, (size_t)es);
    c->W.p = dst; c->W.type = w->type == 0 ? VT_F32 : VT_F16; c->W.rows = Cout; c->W.cols = K * K * Cin;
    c->own = dst; c->packed = NULL;
    c->bias = (const float *)sdg_data(g, b);
    return 1;
}
static void sdr_conv_free(SdrConv *c) { free(c->own); c->own = NULL; if (c->packed) { vt_free(c->packed); c->packed = NULL; } }

typedef struct { float *col; const float *X; int H, W, Cin, K, stride, pt, pl, Wo, r0; } SdrIm2col;
static void sdr_im2col_job(void *cx, int i0, int i1) {
    const SdrIm2col *j = cx;
    const int Cin = j->Cin, K = j->K;
    for (int r = i0; r < i1; r++) {
        const int pix = j->r0 + r, oy = pix / j->Wo, ox = pix % j->Wo;
        float *row = j->col + (size_t)r * K * K * Cin;
        for (int ky = 0; ky < K; ky++) {
            const int iy = oy * j->stride - j->pt + ky;
            for (int kx = 0; kx < K; kx++) {
                const int ix = ox * j->stride - j->pl + kx;
                float *d = row + (size_t)(ky * K + kx) * Cin;
                if (iy < 0 || iy >= j->H || ix < 0 || ix >= j->W) memset(d, 0, (size_t)Cin * 4);
                else memcpy(d, j->X + ((size_t)iy * j->W + ix) * Cin, (size_t)Cin * 4);
            }
        }
    }
}

/* Y[Ho*Wo][cout] = convolucao de X[H*W][cin]. stride e preenchimento por lado (pt/pl em cima/esquerda, pb/pr embaixo/direita). Devolve Ho e Wo.
 * O im2col e feito em blocos de linhas de saida, para a memoria nao crescer com a imagem (um bloco tem no maximo ~64 MB). */
static void sdr_conv2d(float *Y, const float *X, int H, int W, const SdrConv *c, int stride, int pt, int pl, int pb, int pr, int *Ho_out, int *Wo_out) {
    const int Ho = (H + pt + pb - c->k) / stride + 1, Wo = (W + pl + pr - c->k) / stride + 1;
    if (Ho_out) *Ho_out = Ho;
    if (Wo_out) *Wo_out = Wo;
    const int N = Ho * Wo, Kd = c->k * c->k * c->cin;
    size_t blk = ((size_t)64 << 20) / ((size_t)Kd * 4);
    if (blk < 64) blk = 64;
    if (blk > (size_t)N) blk = (size_t)N;
    float *col = vt_alloc(blk * (size_t)Kd * 4);
    float *B = c->packed ? c->packed : vt_pack(&c->W);
    for (int r0 = 0; r0 < N; r0 += (int)blk) {
        const int n = r0 + (int)blk > N ? N - r0 : (int)blk;
        SdrIm2col ij = { col, X, H, W, c->cin, c->k, stride, pt, pl, Wo, r0 };
        pool_for(sdr_im2col_job, &ij, n, 64);
        VtLinJob lj = { Y + (size_t)r0 * c->cout, col, B, c->bias, n, Kd, c->cout, (size_t)c->cout, (size_t)Kd };
        pool_for(vt_lin_job, &lj, (n + VT_ROWS - 1) / VT_ROWS, 1);
    }
    if (!c->packed) vt_free(B);
    vt_free(col);
}

/* normalizacao por grupos: o canal c pertence ao grupo c / (C/G); media e variancia sobre todos os pixels e os canais do grupo
 * (somas em double); depois ganho e vies por canal; silu = 1 aplica SiLU em seguida (o resnet do SD sempre faz norma + SiLU). */
typedef struct { float *Y; const float *X, *g, *b; int n, C, G; float eps; int silu; } SdrGn;
static void sdr_gn_job(void *cx, int g0, int g1) {
    const SdrGn *j = cx;
    const int cpg = j->C / j->G;
    for (int g = g0; g < g1; g++) {
        double s = 0.0, s2 = 0.0;
        for (int p = 0; p < j->n; p++) {
            const float *x = j->X + (size_t)p * j->C + (size_t)g * cpg;
            for (int c = 0; c < cpg; c++) { const double v = x[c]; s += v; s2 += v * v; }
        }
        const double cnt = (double)j->n * cpg, mean = s / cnt, var = s2 / cnt - mean * mean;
        const float m = (float)mean, inv = (float)(1.0 / sqrt((var > 0 ? var : 0) + (double)j->eps));
        for (int p = 0; p < j->n; p++) {
            const float *x = j->X + (size_t)p * j->C + (size_t)g * cpg;
            float *y = j->Y + (size_t)p * j->C + (size_t)g * cpg;
            for (int c = 0; c < cpg; c++) {
                float v = (x[c] - m) * inv * j->g[g * cpg + c] + j->b[g * cpg + c];
                if (j->silu) v = v / (1.0f + expf(-v));
                y[c] = v;
            }
        }
    }
}
static void sdr_groupnorm(float *Y, const float *X, int n, int C, int G, const float *gain, const float *bias, float eps, int silu) {
    SdrGn j = { Y, X, gain, bias, n, C, G, eps, silu };
    pool_for(sdr_gn_job, &j, G, 1);
}

typedef struct { float *X; } SdrSilu;
static void sdr_silu_job(void *cx, int i0, int i1) { float *x = ((SdrSilu *)cx)->X; for (int i = i0; i < i1; i++) x[i] = x[i] / (1.0f + expf(-x[i])); }
static void sdr_silu(float *X, size_t n) { SdrSilu j = { X }; pool_for(sdr_silu_job, &j, (int)n, 65536); }

/* ampliacao 2x por vizinho mais proximo: Y[2H*2W][C] */
static void sdr_upsample2x(float *Y, const float *X, int H, int W, int C) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const float *s = X + ((size_t)y * W + x) * C;
            for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) memcpy(Y + ((size_t)(2 * y + a) * 2 * W + 2 * x + b) * C, s, (size_t)C * 4);
        }
}
