/*
 * ia.c - motor de inferencia de um transformer (decoder only) em C puro.
 * Sem dependencias externas: so a biblioteca padrao do C (e libm para expf/sqrtf/...).
 *
 * Uso:
 *   ia init <modelo.bin> [dim layers heads hidden ctx]   cria um modelo com pesos aleatorios
 *   ia run  <modelo.bin> "texto" [max_tokens] [temperatura]
 *
 * Formato do arquivo do modelo (little-endian):
 *   uint32 magic ("IAM1"), Config (6 x int32), depois todos os pesos float32 em sequencia.
 *
 * Tokenizador: byte a byte (vocab = 256). Simples de propósito; modelos reais usam BPE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>

#define MAGIC 0x314D4149u /* "IAM1" em little-endian */

typedef struct {
    int32_t vocab, dim, n_layers, n_heads, hidden, ctx;
} Config;

typedef struct {
    float *tok_emb;                       /* [vocab][dim] */
    float *rms_att;                       /* [L][dim] */
    float *wq, *wk, *wv, *wo;             /* [L][dim][dim] */
    float *rms_ffn;                       /* [L][dim] */
    float *w1;                            /* [L][hidden][dim] */
    float *w2;                            /* [L][dim][hidden] */
    float *rms_final;                     /* [dim] */
    float *lm_head;                       /* [vocab][dim] */
} Weights;

typedef struct {
    Config cfg;
    float *params;                        /* bloco unico com todos os pesos */
    Weights w;
    float *kcache, *vcache;               /* [L][ctx][dim] */
    float *x, *xb, *xb2, *q, *k, *v, *att, *hb, *logits;
} Model;

/* ---------- utilitarios ---------- */

static void *xalloc(size_t n, size_t sz) {
    void *p = calloc(n, sz);
    if (!p) {
        fprintf(stderr, "Sem memoria (%zu bytes)\n", n * sz);
        exit(1);
    }
    return p;
}

static size_t total_floats(const Config *c) {
    size_t d = c->dim, h = c->hidden, L = c->n_layers;
    return (size_t)c->vocab * d   /* tok_emb */
         + L * d                  /* rms_att */
         + L * 4 * d * d          /* wq wk wv wo */
         + L * d                  /* rms_ffn */
         + L * 2 * d * h         /* w1 w2 */
         + d                      /* rms_final */
         + (size_t)c->vocab * d;  /* lm_head */
}

static void bind_weights(Weights *w, float *p, const Config *c) {
    size_t d = c->dim, h = c->hidden, L = c->n_layers;
    w->tok_emb = p;   p += (size_t)c->vocab * d;
    w->rms_att = p;   p += L * d;
    w->wq = p;        p += L * d * d;
    w->wk = p;        p += L * d * d;
    w->wv = p;        p += L * d * d;
    w->wo = p;        p += L * d * d;
    w->rms_ffn = p;   p += L * d;
    w->w1 = p;        p += L * d * h;
    w->w2 = p;        p += L * d * h;
    w->rms_final = p; p += d;
    w->lm_head = p;
}

static void alloc_state(Model *m) {
    const Config *c = &m->cfg;
    size_t d = c->dim, h = c->hidden, ctx = c->ctx, L = c->n_layers;
    m->kcache = xalloc(L * ctx * d, sizeof(float));
    m->vcache = xalloc(L * ctx * d, sizeof(float));
    m->x      = xalloc(d, sizeof(float));
    m->xb     = xalloc(d, sizeof(float));
    m->xb2    = xalloc(d, sizeof(float));
    m->q      = xalloc(d, sizeof(float));
    m->k      = xalloc(d, sizeof(float));
    m->v      = xalloc(d, sizeof(float));
    m->att    = xalloc(ctx, sizeof(float));
    m->hb     = xalloc(h, sizeof(float));
    m->logits = xalloc(c->vocab, sizeof(float));
}

static void free_model(Model *m) {
    free(m->params); free(m->kcache); free(m->vcache);
    free(m->x); free(m->xb); free(m->xb2); free(m->q); free(m->k);
    free(m->v); free(m->att); free(m->hb); free(m->logits);
}

/* ---------- operacoes matematicas ---------- */

static void rmsnorm(float *out, const float *x, const float *g, int n) {
    float ss = 0.0f;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    float inv = 1.0f / sqrtf(ss / n + 1e-5f);
    for (int i = 0; i < n; i++) out[i] = g[i] * (x[i] * inv);
}

/* out[rows] = W[rows][cols] * x[cols] */
static void matvec(float *out, const float *W, const float *x, int rows, int cols) {
    for (int i = 0; i < rows; i++) {
        const float *row = W + (size_t)i * cols;
        float s = 0.0f;
        for (int j = 0; j < cols; j++) s += row[j] * x[j];
        out[i] = s;
    }
}

static void softmax(float *x, int n) {
    float mx = x[0];
    for (int i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    float sum = 0.0f;
    for (int i = 0; i < n; i++) { x[i] = expf(x[i] - mx); sum += x[i]; }
    for (int i = 0; i < n; i++) x[i] /= sum;
}

/* rotacao RoPE de um par (v[idx], v[idx+1]) */
static void rope(float *v, int idx, float cs, float sn) {
    float a = v[idx], b = v[idx + 1];
    v[idx]     = a * cs - b * sn;
    v[idx + 1] = a * sn + b * cs;
}

/* ---------- forward pass (um token por vez, com KV cache) ---------- */

static float *forward(Model *m, int token, int pos) {
    const Config *c = &m->cfg;
    const Weights *w = &m->w;
    const int d = c->dim, hd = d / c->n_heads, H = c->hidden, ctx = c->ctx;

    memcpy(m->x, w->tok_emb + (size_t)token * d, d * sizeof(float));

    for (int l = 0; l < c->n_layers; l++) {
        const size_t woff = (size_t)l * d * d;

        /* --- atencao --- */
        rmsnorm(m->xb, m->x, w->rms_att + (size_t)l * d, d);
        matvec(m->q, w->wq + woff, m->xb, d, d);
        matvec(m->k, w->wk + woff, m->xb, d, d);
        matvec(m->v, w->wv + woff, m->xb, d, d);

        for (int h = 0; h < c->n_heads; h++) {
            for (int i = 0; i < hd / 2; i++) {
                float freq = powf(10000.0f, -2.0f * i / hd);
                float ang = pos * freq;
                int idx = h * hd + 2 * i;
                rope(m->q, idx, cosf(ang), sinf(ang));
                rope(m->k, idx, cosf(ang), sinf(ang));
            }
        }

        float *kc = m->kcache + ((size_t)l * ctx + pos) * d;
        float *vc = m->vcache + ((size_t)l * ctx + pos) * d;
        memcpy(kc, m->k, d * sizeof(float));
        memcpy(vc, m->v, d * sizeof(float));

        for (int h = 0; h < c->n_heads; h++) {
            const float *qh = m->q + h * hd;
            for (int t = 0; t <= pos; t++) {
                const float *kh = m->kcache + ((size_t)l * ctx + t) * d + h * hd;
                float s = 0.0f;
                for (int i = 0; i < hd; i++) s += qh[i] * kh[i];
                m->att[t] = s / sqrtf((float)hd);
            }
            softmax(m->att, pos + 1);

            float *out = m->xb + h * hd;
            for (int i = 0; i < hd; i++) out[i] = 0.0f;
            for (int t = 0; t <= pos; t++) {
                const float *vh = m->vcache + ((size_t)l * ctx + t) * d + h * hd;
                float a = m->att[t];
                for (int i = 0; i < hd; i++) out[i] += a * vh[i];
            }
        }

        matvec(m->xb2, w->wo + woff, m->xb, d, d);
        for (int i = 0; i < d; i++) m->x[i] += m->xb2[i];

        /* --- feed-forward (SiLU) --- */
        rmsnorm(m->xb, m->x, w->rms_ffn + (size_t)l * d, d);
        matvec(m->hb, w->w1 + (size_t)l * d * H, m->xb, H, d);
        for (int i = 0; i < H; i++) {
            float v = m->hb[i];
            m->hb[i] = v / (1.0f + expf(-v));
        }
        matvec(m->xb2, w->w2 + (size_t)l * d * H, m->hb, d, H);
        for (int i = 0; i < d; i++) m->x[i] += m->xb2[i];
    }

    rmsnorm(m->x, m->x, w->rms_final, d);
    matvec(m->logits, w->lm_head, m->x, c->vocab, d);
    return m->logits;
}

/* ---------- amostragem ---------- */

static int sample(float *logits, int n, float temp) {
    if (temp <= 0.0f) {
        int best = 0;
        for (int i = 1; i < n; i++) if (logits[i] > logits[best]) best = i;
        return best;
    }
    for (int i = 0; i < n; i++) logits[i] /= temp;
    softmax(logits, n);
    float r = (float)rand() / ((float)RAND_MAX + 1.0f), acc = 0.0f;
    for (int i = 0; i < n; i++) {
        acc += logits[i];
        if (r < acc) return i;
    }
    return n - 1;
}

/* ---------- carregar / salvar ---------- */

static int load_model(Model *m, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Nao consegui abrir %s\n", path); return 0; }

    uint32_t magic = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != MAGIC) {
        fprintf(stderr, "Arquivo %s nao e um modelo IAM1 valido\n", path);
        fclose(f);
        return 0;
    }
    if (fread(&m->cfg, sizeof(Config), 1, f) != 1) { fclose(f); return 0; }

    size_t n = total_floats(&m->cfg);
    m->params = xalloc(n, sizeof(float));
    if (fread(m->params, sizeof(float), n, f) != n) {
        fprintf(stderr, "Arquivo %s esta truncado\n", path);
        fclose(f);
        return 0;
    }
    fclose(f);

    bind_weights(&m->w, m->params, &m->cfg);
    alloc_state(m);
    return 1;
}

static int save_model(const Model *m, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Nao consegui criar %s\n", path); return 0; }
    uint32_t magic = MAGIC;
    fwrite(&magic, 4, 1, f);
    fwrite(&m->cfg, sizeof(Config), 1, f);
    fwrite(m->params, sizeof(float), total_floats(&m->cfg), f);
    fclose(f);
    return 1;
}

/* ---------- comando init: pesos aleatorios (so para testar o pipeline) ---------- */

static float rnd_uniform(float scale) {
    return ((float)rand() / (float)RAND_MAX * 2.0f - 1.0f) * scale;
}

static int cmd_init(int argc, char **argv) {
    Model m;
    memset(&m, 0, sizeof(m));
    m.cfg.vocab   = 256;
    m.cfg.dim     = argc > 3 ? atoi(argv[3]) : 128;
    m.cfg.n_layers = argc > 4 ? atoi(argv[4]) : 4;
    m.cfg.n_heads = argc > 5 ? atoi(argv[5]) : 4;
    m.cfg.hidden  = argc > 6 ? atoi(argv[6]) : 512;
    m.cfg.ctx     = argc > 7 ? atoi(argv[7]) : 256;

    if (m.cfg.dim % m.cfg.n_heads != 0 || ((m.cfg.dim / m.cfg.n_heads) % 2) != 0) {
        fprintf(stderr, "dim/heads precisa ser inteiro e a cabeca precisa ter tamanho par\n");
        return 1;
    }

    size_t n = total_floats(&m.cfg);
    m.params = xalloc(n, sizeof(float));
    bind_weights(&m.w, m.params, &m.cfg);

    srand((unsigned)time(NULL));
    for (size_t i = 0; i < n; i++) m.params[i] = rnd_uniform(0.05f);

    /* ganhos do RMSNorm começam em 1 */
    size_t d = m.cfg.dim, L = m.cfg.n_layers;
    for (size_t i = 0; i < L * d; i++) m.w.rms_att[i] = 1.0f;
    for (size_t i = 0; i < L * d; i++) m.w.rms_ffn[i] = 1.0f;
    for (size_t i = 0; i < d; i++) m.w.rms_final[i] = 1.0f;

    int ok = save_model(&m, argv[2]);
    if (ok) printf("Modelo criado em %s (%zu parametros)\n", argv[2], n);
    free(m.params);
    return ok ? 0 : 1;
}

/* ---------- comando run: gera texto com streaming ---------- */

static int cmd_run(int argc, char **argv) {
    Model m;
    if (!load_model(&m, argv[2])) return 1;

    const char *prompt = argv[3];
    int max_tokens = argc > 4 ? atoi(argv[4]) : 200;
    float temp = argc > 5 ? (float)atof(argv[5]) : 0.8f;
    srand((unsigned)time(NULL));

    const int ctx = m.cfg.ctx;
    int pos = 0;
    float *lg = NULL;

    /* 1) pre-preenchimento: processa o prompt, token a token, guardando no KV cache */
    size_t plen = strlen(prompt);
    if (plen == 0) { prompt = "\n"; plen = 1; }
    for (size_t i = 0; i < plen; i++) {
        if (pos >= ctx) { fprintf(stderr, "Prompt maior que o contexto\n"); break; }
        lg = forward(&m, (unsigned char)prompt[i], pos++);
    }
    fwrite(prompt, 1, plen, stdout);
    fflush(stdout);

    /* 2) geracao: escolhe o proximo token e imprime assim que sai (streaming) */
    for (int g = 0; g < max_tokens && pos < ctx && lg; g++) {
        int next = sample(lg, m.cfg.vocab, temp);
        if (next == 0) break;              /* byte 0 = fim */
        fputc(next, stdout);
        fflush(stdout);
        lg = forward(&m, next, pos++);
    }
    printf("\n");

    free_model(&m);
    return 0;
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "init") == 0) return cmd_init(argc, argv);
    if (argc >= 4 && strcmp(argv[1], "run") == 0)  return cmd_run(argc, argv);

    fprintf(stderr,
        "Uso:\n"
        "  ia init <modelo.bin> [dim layers heads hidden ctx]\n"
        "  ia run  <modelo.bin> \"texto\" [max_tokens] [temperatura]\n");
    return 1;
}
