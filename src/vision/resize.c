/* ---------- visao: tamanho, redimensionamento e recortes ----------
 * O tamanho final segue a regra de referencia (lados multiplos do fator, area entre minimo e maximo, proporcao mantida);
 * o teto de area e o orcamento de tokens. Redimensionamento bicubico (a = -0,5) com suporte ampliado na reducao, duas
 * passadas separaveis em ponto fixo de 22 bits com resultado de 8 bits entre elas: igual pixel a pixel ao de referencia. */

/* arredonda como a referencia: meio vai para o par */
static double vr_round(double x) { const double f = floor(x), d = x - f; return d > 0.5 || (d == 0.5 && fmod(f, 2.0) != 0.0) ? f + 1 : f; }
static int vr_size(int h, int w, int factor, double min_px, double max_px, int *oh, int *ow) {
    if ((double)(h > w ? h : w) / (h < w ? h : w) > 200.0) return 0;
    double hb = vr_round((double)h / factor) * factor, wb = vr_round((double)w / factor) * factor;
    if (hb * wb > max_px) {
        const double beta = sqrt((double)h * w / max_px);
        hb = floor(h / beta / factor) * factor; wb = floor(w / beta / factor) * factor;
        if (hb < factor) hb = factor;
        if (wb < factor) wb = factor;
    } else if (hb * wb < min_px) {
        const double beta = sqrt(min_px / ((double)h * w));
        hb = ceil(h * beta / factor) * factor; wb = ceil(w * beta / factor) * factor;
    }
    *oh = (int)hb; *ow = (int)wb;
    return 1;
}

/* coeficientes de uma passada: para cada saida, primeiro pixel, quantos e os pesos inteiros */
typedef struct { int ks, *b0, *bn; int32_t *k; } VrCoef;
static double vr_cubic(double x) {
    const double a = -0.5;
    if (x < 0.0) x = -x;
    if (x < 1.0) return ((a + 2.0) * x - (a + 3.0)) * x * x + 1;
    if (x < 2.0) return (((x - 5) * x + 8) * x - 4) * a;
    return 0.0;
}
#ifdef __GNUC__
__attribute__((optimize("no-fast-math")))      /* as contas precisam sair na mesma ordem da referencia */
#endif
static VrCoef vr_coef(int in, int out) {
    const double scale = (double)in / out, fs = scale < 1.0 ? 1.0 : scale, support = 2.0 * fs, ss = 1.0 / fs;
    VrCoef c; c.ks = (int)ceil(support) * 2 + 1;
    c.b0 = xalloc((size_t)out, sizeof(int)); c.bn = xalloc((size_t)out, sizeof(int));
    c.k = xalloc((size_t)out * c.ks, sizeof(int32_t));
    double *w = xalloc((size_t)c.ks, sizeof(double));
    for (int x = 0; x < out; x++) {
        const double center = (x + 0.5) * scale;
        double sum = 0.0;
        int lo = (int)(center - support + 0.5), hi = (int)(center + support + 0.5);
        if (lo < 0) lo = 0;
        if (hi > in) hi = in;
        hi -= lo;
        for (int i = 0; i < hi; i++) { w[i] = vr_cubic((i + lo - center + 0.5) * ss); sum += w[i]; }
        for (int i = 0; i < hi; i++) {
            if (sum != 0.0) w[i] /= sum;
            c.k[(size_t)x * c.ks + i] = (int32_t)(w[i] * (1 << 22) + (w[i] < 0 ? -0.5 : 0.5));
        }
        c.b0[x] = lo; c.bn[x] = hi;
    }
    free(w);
    return c;
}
static void vr_coef_free(VrCoef *c) { free(c->b0); free(c->bn); free(c->k); }
static uint8_t vr_clip(int32_t v) { v >>= 22; return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
typedef struct { const uint8_t *in; uint8_t *out; const VrCoef *c; int iw, ow; } VrJob;
static void vr_horiz(void *cx, int y0, int y1) {
    const VrJob *j = cx;
    for (int y = y0; y < y1; y++) {
        const uint8_t *s = j->in + (size_t)y * j->iw * 3; uint8_t *o = j->out + (size_t)y * j->ow * 3;
        for (int x = 0; x < j->ow; x++) {
            const int32_t *k = j->c->k + (size_t)x * j->c->ks; const uint8_t *p = s + (size_t)j->c->b0[x] * 3;
            int32_t a = 1 << 21, b = a, c = a;
            for (int i = 0; i < j->c->bn[x]; i++) { a += p[3 * i] * k[i]; b += p[3 * i + 1] * k[i]; c += p[3 * i + 2] * k[i]; }
            o[3 * x] = vr_clip(a); o[3 * x + 1] = vr_clip(b); o[3 * x + 2] = vr_clip(c);
        }
    }
}
static void vr_vert(void *cx, int y0, int y1) {
    const VrJob *j = cx;
    const size_t rb = (size_t)j->iw * 3;
    int32_t *acc = xalloc(rb, sizeof(int32_t));
    for (int y = y0; y < y1; y++) {
        const int32_t *k = j->c->k + (size_t)y * j->c->ks;
        for (size_t x = 0; x < rb; x++) acc[x] = 1 << 21;
        for (int i = 0; i < j->c->bn[y]; i++) {
            const uint8_t *s = j->in + (size_t)(j->c->b0[y] + i) * rb;
            for (size_t x = 0; x < rb; x++) acc[x] += s[x] * k[i];
        }
        uint8_t *o = j->out + (size_t)y * rb;
        for (size_t x = 0; x < rb; x++) o[x] = vr_clip(acc[x]);
    }
    free(acc);
}
/* troca im pela versao redimensionada */
static int vr_resize(Img *im, int ow, int oh) {
    if (ow != im->w) {
        uint8_t *t = malloc((size_t)ow * im->h * 3);
        if (!t) return 0;
        VrCoef c = vr_coef(im->w, ow);
        VrJob j = { im->rgb, t, &c, im->w, ow };
        pool_for(vr_horiz, &j, im->h, 16);
        vr_coef_free(&c); free(im->rgb); im->rgb = t; im->w = ow;
    }
    if (oh != im->h) {
        uint8_t *t = malloc((size_t)im->w * oh * 3);
        if (!t) return 0;
        VrCoef c = vr_coef(im->h, oh);
        VrJob j = { im->rgb, t, &c, im->w, im->w };
        pool_for(vr_vert, &j, oh, 16);
        vr_coef_free(&c); free(im->rgb); im->rgb = t; im->h = oh;
    }
    return 1;
}
/* recortes em ordem de blocos de fusao: X[n][3*ps*ps] = canal, linha, coluna; valor (v/255 - media) / desvio */
static void vr_patches(const Img *im, int ps, int mg, const float *mean, const float *std, float *X) {
    float lut[3][256];
    for (int c = 0; c < 3; c++) for (int v = 0; v < 256; v++) lut[c][v] = ((float)v * (1.0f / 255.0f) - mean[c]) / std[c];
    const int gh = im->h / ps, gw = im->w / ps;
    for (int by = 0; by < gh; by += mg) for (int bx = 0; bx < gw; bx += mg)
        for (int my = 0; my < mg; my++) for (int mx = 0; mx < mg; mx++)
            for (int c = 0; c < 3; c++) for (int y = 0; y < ps; y++) {
                const uint8_t *s = im->rgb + (((size_t)(by + my) * ps + y) * im->w + (size_t)(bx + mx) * ps) * 3 + c;
                for (int x = 0; x < ps; x++) *X++ = lut[c][s[3 * x]];
            }
}
