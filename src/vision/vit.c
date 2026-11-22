/* ---------- visao: codificador de imagem (ViT) do arquivo do projetor ----------
 * Pesos em GGUF (arquitetura "clip", projetor "qwen3vl_merger"), lidos do arquivo mapeado sem copia: recortes 16x16 por
 * projecao linear (os dois pesos temporais somados: a imagem parada e o mesmo quadro duas vezes), tabela de posicoes
 * interpolada (bilinear, cantos alinhados), blocos pre-norma com atencao global, RoPE de dois eixos (linha e coluna) e
 * feed-forward GELU; no fim norma, 4 vizinhos juntos e duas camadas ate a largura do modelo de linguagem.
 * O arquivo do projetor e conferido como os de imagem: erro vira mensagem, nao encerra o programa. */
typedef struct { const float *ln1g, *ln1b, *ln2g, *ln2b, *qkvb, *outb, *upb, *downb; VtMat qkv, out, up, down; } VitBlk;
typedef struct {
    const uint8_t *map; size_t size;
    int d, ff, depth, heads, ps, mg, side, outd;
    float eps, mean[3], std[3];
    VitBlk *b;
    float *patch; VtMat pos, mm0, mm2;      /* patch: [d][3*ps*ps] ja somado */
    const float *patchb, *pg, *pb, *mm0b, *mm2b;
    float *own[256]; int nown;              /* copias em float dos vetores que nao sao F32 alinhado */
} Vit;
static void (*vit_tap)(const char *stage, int layer, const float *x, size_t n);      /* conferencia: saida de cada etapa */

typedef struct { const uint8_t *p, *end; int bad; } VitRd;
static uint64_t vg_n(VitRd *r, int k) { uint64_t v = 0; if ((size_t)(r->end - r->p) < (size_t)k) { r->bad = 1; r->p = r->end; return 0; } memcpy(&v, r->p, (size_t)k); r->p += k; return v; }
static const char *vg_str(VitRd *r, size_t *n) {
    const uint64_t l = vg_n(r, 8);
    if (l > (uint64_t)(r->end - r->p)) { r->bad = 1; *n = 0; r->p = r->end; return ""; }
    const char *s = (const char *)r->p; r->p += l; *n = (size_t)l; return s;
}
static int vg_size(uint32_t t) { return t == 8 ? -1 : t <= 12 ? (t == 0 || t == 1 || t == 7 ? 1 : t == 2 || t == 3 ? 2 : t <= 6 ? 4 : t >= 10 ? 8 : 0) : 0; }
static double vg_num(const uint8_t *p, uint32_t t) {
    union { uint8_t b[8]; uint16_t u2; int16_t s2; uint32_t u4; int32_t s4; float f; uint64_t u8; int64_t s8; double d; } u;
    memset(&u, 0, sizeof u); memcpy(u.b, p, (size_t)vg_size(t));
    switch (t) { case 2: return u.u2; case 3: return u.s2; case 4: return u.u4; case 5: return u.s4; case 6: return u.f; case 10: return (double)u.u8; case 11: return (double)u.s8; case 12: return u.d; default: return u.b[0]; }
}
static int vg_is(const char *s, size_t n, const char *k) { return n == strlen(k) && !memcmp(s, k, n); }

typedef struct { const char *name; size_t nl; uint64_t dim[4]; int nd; uint32_t type; uint64_t off; } VitT;
typedef struct { VitT *t; int n; const uint8_t *data; size_t room; char *err; size_t cap; int bad; } VitTs;
/* tensor pelo nome, com a forma esperada (do mais rapido ao mais lento); so F32 e F16 */
static const void *vg_tensor(VitTs *s, const char *name, uint64_t d0, uint64_t d1, uint64_t d2, uint64_t d3, int *type) {
    for (int i = 0; i < s->n; i++) {
        const VitT *t = &s->t[i];
        if (!vg_is(t->name, t->nl, name)) continue;
        if (t->dim[0] != d0 || t->dim[1] != d1 || t->dim[2] != d2 || t->dim[3] != d3 || t->type > 1) break;
        const size_t bytes = (size_t)(d0 * d1 * d2 * d3) * (t->type ? 2 : 4);
        if (t->off > s->room || bytes > s->room - t->off) break;
        *type = t->type ? VT_F16 : VT_F32;
        return s->data + t->off;
    }
    if (!s->bad) snprintf(s->err, s->cap, "projector tensor %s is missing or has another shape", name);
    s->bad = 1; *type = VT_F32;
    return NULL;
}
static VtMat vg_mat(VitTs *s, const char *name, int rows, int cols) {
    VtMat m = { NULL, VT_F32, rows, cols };
    m.p = vg_tensor(s, name, (uint64_t)cols, (uint64_t)rows, 1, 1, &m.type);
    return m;
}
static const float *vg_vec(Vit *v, VitTs *s, const char *name, int n) {
    VtMat m = { NULL, VT_F32, 1, n };
    m.p = vg_tensor(s, name, (uint64_t)n, 1, 1, 1, &m.type);
    if (!m.p || v->nown >= 256) { s->bad = 1; return NULL; }
    float *o = vt_alloc((size_t)n * 4);      /* copia alinhada: os vetores somam menos de 1 MB */
    vt_row(&m, 0, o);
    v->own[v->nown++] = o;
    return o;
}
static void vit_free(Vit *v) {
    if (!v) return;
    for (int i = 0; i < v->nown; i++) vt_free(v->own[i]);
    vt_free(v->patch); free(v->b);
#ifndef _WIN32
    if (v->map) munmap((void *)v->map, v->size);
#else
    if (v->map) UnmapViewOfFile(v->map);
#endif
    free(v);
}
static const uint8_t *vit_map(const char *path, size_t *size) {
#ifndef _WIN32
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 64) { close(fd); return NULL; }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    *size = (size_t)st.st_size;
    return p == MAP_FAILED ? NULL : p;
#else
    (void)path; *size = 0; return NULL;
#endif
}
/* head_only: so confere o cabecalho e le as medidas (para saber se o projetor serve, sem tocar nos pesos) */
static Vit *vit_load(const char *path, int head_only, char *err, size_t cap) {
    Vit *v = xalloc(1, sizeof *v);
    v->map = vit_map(path, &v->size);
    if (!v->map) { im_fail(err, cap, "cannot open the projector file"); free(v); return NULL; }
    VitRd r = { v->map, v->map + v->size, 0 };
    char proj[64] = ""; uint64_t align = 32;
    v->eps = 1e-6f; v->ps = 16; v->mg = 2;
    for (int i = 0; i < 3; i++) v->mean[i] = v->std[i] = 0.5f;
    const uint32_t magic = (uint32_t)vg_n(&r, 4), ver = (uint32_t)vg_n(&r, 4);
    const uint64_t nt = vg_n(&r, 8), nk = vg_n(&r, 8);
    if (magic != 0x46554747 || ver < 2 || ver > 3 || nt > 100000 || nk > 100000) { im_fail(err, cap, "the projector is not a GGUF file"); vit_free(v); return NULL; }
    for (uint64_t i = 0; i < nk && !r.bad; i++) {
        size_t kl, sl; const char *k = vg_str(&r, &kl);
        const uint32_t t = (uint32_t)vg_n(&r, 4);
        if (t == 8) {
            const char *s = vg_str(&r, &sl);
            if (vg_is(k, kl, "clip.projector_type")) snprintf(proj, sizeof proj, "%.*s", (int)(sl < 60 ? sl : 60), s);
        } else if (t == 9) {
            const uint32_t et = (uint32_t)vg_n(&r, 4); const uint64_t n = vg_n(&r, 8);
            if (et == 8) { for (uint64_t j = 0; j < n && !r.bad; j++) vg_str(&r, &sl); continue; }
            const int es = vg_size(et);
            if (es <= 0 || n > (uint64_t)(r.end - r.p) / (uint64_t)es) { r.bad = 1; break; }
            float *dst = vg_is(k, kl, "clip.vision.image_mean") ? v->mean : vg_is(k, kl, "clip.vision.image_std") ? v->std : NULL;
            if (dst && n == 3) for (int j = 0; j < 3; j++) dst[j] = (float)vg_num(r.p + j * es, et);
            r.p += n * (uint64_t)es;
        } else {
            const int es = vg_size(t);
            if (es <= 0 || (size_t)(r.end - r.p) < (size_t)es) { r.bad = 1; break; }
            const double x = vg_num(r.p, t); r.p += es;
            const int xi = x >= 0 && x < 1e9 ? (int)x : -1;
            if (vg_is(k, kl, "clip.vision.embedding_length")) v->d = xi;
            else if (vg_is(k, kl, "clip.vision.feed_forward_length")) v->ff = xi;
            else if (vg_is(k, kl, "clip.vision.block_count")) v->depth = xi;
            else if (vg_is(k, kl, "clip.vision.attention.head_count")) v->heads = xi;
            else if (vg_is(k, kl, "clip.vision.patch_size")) v->ps = xi;
            else if (vg_is(k, kl, "clip.vision.spatial_merge_size")) v->mg = xi;
            else if (vg_is(k, kl, "clip.vision.projection_dim")) v->outd = xi;
            else if (vg_is(k, kl, "clip.vision.attention.layer_norm_epsilon")) v->eps = (float)x;
            else if (vg_is(k, kl, "general.alignment")) align = xi > 0 ? (uint64_t)xi : 32;
        }
    }
    const char *why = NULL;
    if (r.bad) why = "the projector file is damaged";
    else if (strcmp(proj, "qwen3vl_merger")) why = "this projector type is not supported";
    else if (v->d < 8 || v->d > 8192 || v->ff < 8 || v->ff > 65536 || v->depth < 1 || v->depth > 27 * 4 || v->heads < 1 || v->d % v->heads || (v->d / v->heads) % 4 || v->d / v->heads > 2048 || v->outd < 1 || v->outd > 65536)
        why = "projector with unsupported dimensions";
    else if (v->ps < 2 || v->ps > 64 || v->mg < 1 || v->mg > 4 || v->std[0] == 0 || v->std[1] == 0 || v->std[2] == 0 || (align & (align - 1))) why = "projector with unsupported dimensions";
    if (why) { im_fail(err, cap, why); vit_free(v); return NULL; }
    if (head_only) return v;
    VitTs ts = { xalloc((size_t)nt + 1, sizeof(VitT)), (int)nt, NULL, 0, err, cap, 0 };
    for (uint64_t i = 0; i < nt && !r.bad; i++) {
        VitT *t = &ts.t[i];
        t->name = vg_str(&r, &t->nl);
        t->nd = (int)vg_n(&r, 4);
        if (t->nd < 1 || t->nd > 4) { r.bad = 1; break; }
        for (int k = 0; k < 4; k++) { t->dim[k] = k < t->nd ? vg_n(&r, 8) : 1; if (t->dim[k] > 1u << 24) r.bad = 1; }
        t->type = (uint32_t)vg_n(&r, 4); t->off = vg_n(&r, 8);
    }
    const size_t head = ((size_t)(r.p - v->map) + align - 1) / align * align;
    if (r.bad || head > v->size) { free(ts.t); im_fail(err, cap, "the projector file is damaged"); vit_free(v); return NULL; }
    ts.data = v->map + head; ts.room = v->size - head;
    const int d = v->d, pd = 3 * v->ps * v->ps, md = d * v->mg * v->mg;
    v->b = xalloc((size_t)v->depth, sizeof(VitBlk));
    for (int l = 0; l < v->depth && !ts.bad; l++) {
        VitBlk *b = &v->b[l]; char nm[96];
#define N(x) (snprintf(nm, sizeof nm, "v.blk.%d." x, l), nm)
        b->ln1g = vg_vec(v, &ts, N("ln1.weight"), d); b->ln1b = vg_vec(v, &ts, N("ln1.bias"), d);
        b->ln2g = vg_vec(v, &ts, N("ln2.weight"), d); b->ln2b = vg_vec(v, &ts, N("ln2.bias"), d);
        b->qkv = vg_mat(&ts, N("attn_qkv.weight"), 3 * d, d); b->qkvb = vg_vec(v, &ts, N("attn_qkv.bias"), 3 * d);
        b->out = vg_mat(&ts, N("attn_out.weight"), d, d); b->outb = vg_vec(v, &ts, N("attn_out.bias"), d);
        b->up = vg_mat(&ts, N("ffn_up.weight"), v->ff, d); b->upb = vg_vec(v, &ts, N("ffn_up.bias"), v->ff);
        b->down = vg_mat(&ts, N("ffn_down.weight"), d, v->ff); b->downb = vg_vec(v, &ts, N("ffn_down.bias"), d);
#undef N
    }
    int npos = 0;
    for (int i = 0; i < ts.n; i++) if (vg_is(ts.t[i].name, ts.t[i].nl, "v.position_embd.weight")) npos = (int)ts.t[i].dim[1];
    v->side = (int)(sqrt((double)npos) + 0.5);
    if (npos < 4 || v->side * v->side != npos) { if (!ts.bad) snprintf(err, cap, "projector without its position table"); ts.bad = 1; }
    if (!ts.bad) {
        v->pos = vg_mat(&ts, "v.position_embd.weight", npos, d);
        v->patchb = vg_vec(v, &ts, "v.patch_embd.bias", d);
        v->pg = vg_vec(v, &ts, "v.post_ln.weight", d); v->pb = vg_vec(v, &ts, "v.post_ln.bias", d);
        v->mm0 = vg_mat(&ts, "mm.0.weight", md, md); v->mm0b = vg_vec(v, &ts, "mm.0.bias", md);
        v->mm2 = vg_mat(&ts, "mm.2.weight", v->outd, md); v->mm2b = vg_vec(v, &ts, "mm.2.bias", v->outd);
        VtMat a = { NULL, 0, d, pd }, b = a;
        const uint64_t ps = (uint64_t)v->ps;
        a.p = vg_tensor(&ts, "v.patch_embd.weight", ps, ps, 3, (uint64_t)d, &a.type);
        b.p = vg_tensor(&ts, "v.patch_embd.weight.1", ps, ps, 3, (uint64_t)d, &b.type);
        if (!ts.bad) {
            v->patch = vt_alloc((size_t)d * pd * 4);
            float *t = vt_alloc((size_t)pd * 4);
            for (int i = 0; i < d; i++) { float *o = v->patch + (size_t)i * pd; vt_row(&a, i, o); vt_row(&b, i, t); for (int k = 0; k < pd; k++) o[k] += t[k]; }
            vt_free(t);
        }
    }
    free(ts.t);
    if (ts.bad) { vit_free(v); return NULL; }
    return v;
}

/* RoPE de dois eixos em q e k: cos/sin [n][hd/2]: metade das frequencias pela linha, metade pela coluna */
typedef struct { float *qkv; const float *cs, *sn; int n, d, heads, hd; } VitRopeJob;
static void vit_rope_job(void *cx, int t0, int t1) {
    const VitRopeJob *j = cx;
    const int h2 = j->hd / 2;
    for (int t = t0; t < t1; t++) {
        const float *c = j->cs + (size_t)t * h2, *s = j->sn + (size_t)t * h2;
        for (int h = 0; h < 2 * j->heads; h++) {      /* q e k sao vizinhos na linha */
            float *a = j->qkv + (size_t)t * 3 * j->d + (size_t)h * j->hd, *b = a + h2;
            for (int i = 0; i < h2; i++) { const float x = a[i], y = b[i]; a[i] = x * c[i] - y * s[i]; b[i] = y * c[i] + x * s[i]; }
        }
    }
}
/* tabela de posicoes interpolada e somada em X (recortes em ordem de blocos de fusao) */
static void vit_positions(const Vit *v, int gh, int gw, float *X, float *cs, float *sn) {
    const int d = v->d, mg = v->mg, S = v->side, hd = d / v->heads, q = hd / 4;
    float *tb = vt_alloc((size_t)S * S * d * 4), fr[512];
    for (int i = 0; i < S * S; i++) vt_row(&v->pos, i, tb + (size_t)i * d);
    for (int i = 0; i < q; i++) fr[i] = 1.0f / powf(10000.0f, (float)(2 * i) / (float)(hd / 2));
    size_t n = 0;
    for (int by = 0; by < gh; by += mg) for (int bx = 0; bx < gw; bx += mg)
        for (int my = 0; my < mg; my++) for (int mx = 0; mx < mg; mx++, n++) {
            const int row = by + my, col = bx + mx;
            const float sy = (float)row * (S - 1) / (gh > 1 ? gh - 1 : 1), sx = (float)col * (S - 1) / (gw > 1 ? gw - 1 : 1);
            const int y0 = (int)floorf(sy), x0 = (int)floorf(sx), y1 = y0 + 1 < S ? y0 + 1 : S - 1, x1 = x0 + 1 < S ? x0 + 1 : S - 1;
            const float wy = sy - y0, wx = sx - x0;
            const float *a = tb + ((size_t)y0 * S + x0) * d, *b = tb + ((size_t)y0 * S + x1) * d, *c = tb + ((size_t)y1 * S + x0) * d, *e = tb + ((size_t)y1 * S + x1) * d;
            const float w0 = (1 - wy) * (1 - wx), w1 = (1 - wy) * wx, w2 = wy * (1 - wx), w3 = wy * wx;
            float *x = X + n * d;
            for (int i = 0; i < d; i++) x[i] += a[i] * w0 + b[i] * w1 + c[i] * w2 + e[i] * w3;
            for (int i = 0; i < q; i++) {
                cs[n * 2 * q + i] = cosf(row * fr[i]); sn[n * 2 * q + i] = sinf(row * fr[i]);
                cs[n * 2 * q + q + i] = cosf(col * fr[i]); sn[n * 2 * q + q + i] = sinf(col * fr[i]);
            }
        }
    vt_free(tb);
}
typedef struct { float *a; const float *b; size_t per; } VitAddJob;
static void vit_add_job(void *cx, int r0, int r1) { const VitAddJob *j = cx; for (size_t i = (size_t)r0 * j->per; i < (size_t)r1 * j->per; i++) j->a[i] += j->b[i]; }
/* imagem ja no tamanho final -> vetores [tokens][outd] (vt_free); *n = tokens */
static float *vit_encode(const Vit *v, const Img *im, int *ntok) {
    const int gh = im->h / v->ps, gw = im->w / v->ps, n = gh * gw, d = v->d, pd = 3 * v->ps * v->ps, u = v->mg * v->mg, md = d * u;
    float *P = vt_alloc((size_t)n * pd * 4), *X = vt_alloc((size_t)n * d * 4), *N = vt_alloc((size_t)n * d * 4);
    float *W = vt_alloc((size_t)n * (3 * d > v->ff ? 3 * d : v->ff) * 4);
    float *cs = vt_alloc((size_t)n * d / v->heads * 2), *sn = vt_alloc((size_t)n * d / v->heads * 2);
    vr_patches(im, v->ps, v->mg, v->mean, v->std, P);
    if (vit_tap) vit_tap("patches", 0, P, (size_t)n * pd);
    const VtMat pm = { v->patch, VT_F32, d, pd };
    vt_linear(X, P, n, &pm, v->patchb);
    vt_free(P);
    vit_positions(v, gh, gw, X, cs, sn);
    if (vit_tap) vit_tap("embed", 0, X, (size_t)n * d);
    for (int l = 0; l < v->depth; l++) {
        const VitBlk *b = &v->b[l];
        vt_layernorm(N, X, n, d, b->ln1g, b->ln1b, v->eps);
        vt_linear(W, N, n, &b->qkv, b->qkvb);
        VitRopeJob rj = { W, cs, sn, n, d, v->heads, d / v->heads };
        pool_for(vit_rope_job, &rj, n, 32);
        vt_attention(N, W, n, d, v->heads);
        vt_linear(W, N, n, &b->out, b->outb);
        VitAddJob aj = { X, W, (size_t)d };
        pool_for(vit_add_job, &aj, n, 64);
        vt_layernorm(N, X, n, d, b->ln2g, b->ln2b, v->eps);
        vt_linear(W, N, n, &b->up, b->upb);
        vt_gelu(W, n, (size_t)v->ff, 0);
        vt_linear(N, W, n, &b->down, b->downb);
        aj.b = N;
        pool_for(vit_add_job, &aj, n, 64);
        if (vit_tap) vit_tap("block", l, X, (size_t)n * d);
    }
    vt_layernorm(N, X, n, d, v->pg, v->pb, v->eps);
    vt_free(X); vt_free(W); vt_free(cs); vt_free(sn);
    const int nt = n / u;
    float *H = vt_alloc((size_t)nt * md * 4), *O = vt_alloc((size_t)nt * v->outd * 4);
    vt_linear(H, N, nt, &v->mm0, v->mm0b);
    vt_gelu(H, nt, (size_t)md, 1);
    vt_linear(O, H, nt, &v->mm2, v->mm2b);
    vt_free(N); vt_free(H);
    if (vit_tap) vit_tap("merger", 0, O, (size_t)nt * v->outd);
    *ntok = nt;
    return O;
}
