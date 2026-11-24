/* ---------- imagem: limites, inflate (RFC 1950/1951), PNG e BMP ----------
 * Os arquivos vem de fora (internet, ferramentas, clientes da API): todo tamanho e conferido antes de alocar, toda leitura
 * passa por uma funcao com limite e nada indexa memoria com valor do arquivo sem checagem. Um erro devolve 0 com a
 * mensagem; nunca encerra o programa. Saida sempre RGB de 8 bits; transparencia e composta sobre branco. */
#define IM_MAX_FILE ((size_t)64 << 20)      /* arquivo */
#define IM_MAX_DIM  16384                   /* lado */
#define IM_MAX_PIX  ((size_t)40000000)      /* pixels */
typedef struct { int w, h; uint8_t *rgb; uint8_t *alpha; } Img;      /* alpha: so se im_want_alpha estava ligado ao decodificar um PNG (cor sem misturar com branco + canal alfa); senao NULL */
static int im_want_alpha;

static int im_fail(char *err, size_t cap, const char *msg) { if (err && cap) snprintf(err, cap, "%s", msg); return 0; }
static uint32_t im_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t im_le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static uint32_t im_le16(const uint8_t *p) { return (uint32_t)p[1] << 8 | p[0]; }
static int im_dims_ok(uint64_t w, uint64_t h, char *err, size_t cap) {
    if (!w || !h) return im_fail(err, cap, "image has zero width or height");
    if (w > IM_MAX_DIM || h > IM_MAX_DIM || w * h > IM_MAX_PIX) return im_fail(err, cap, "image is too large (limit: 16384 pixels per side, 40 million pixels)");
    return 1;
}
static uint8_t im_white(int c, int a) { return (uint8_t)((c * a + 255 * (255 - a) + 127) / 255); }

/* ---- inflate: codigo de Huffman canonico por contagem (sem tabela grande), saida com tamanho exato conhecido ---- */
typedef struct { const uint8_t *p, *end; uint64_t bits; int n, over; uint8_t *out; size_t pos, cap; } Infl;
typedef struct { uint16_t count[16], sym[288]; uint16_t fast[512]; } InfHuff;      /* fast: 9 bits -> (simbolo << 4 | tamanho), 0 = longo */

static void inf_fill(Infl *z) {
    while (z->n <= 48) {
        if (z->p < z->end) z->bits |= (uint64_t)*z->p++ << z->n;
        else if (++z->over > 16) { z->n = 56; return; }      /* acabou a entrada: zeros, e o laco principal para */
        z->n += 8;
    }
}
static unsigned inf_bits(Infl *z, int k) {
    if (z->n < k) inf_fill(z);
    const unsigned v = (unsigned)(z->bits & ((1ull << k) - 1));
    z->bits >>= k; z->n -= k;
    return v;
}
/* 1 = codigo completo, 0 = invalido; incompleto so vale com um codigo (distancia unica) */
static int inf_build(InfHuff *h, const uint8_t *len, int n) {
    uint16_t offs[16]; uint32_t code = 0, next[16];
    memset(h, 0, sizeof *h);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    const int used = n - h->count[0];
    h->count[0] = 0;
    int left = 1;
    for (int l = 1; l < 16; l++) { left = left * 2 - h->count[l]; if (left < 0) return 0; }
    if (left > 0 && used > 1) return 0;
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = (uint16_t)(offs[l] + h->count[l]);
    for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (uint16_t)i;
    for (int l = 1; l < 16; l++) { next[l] = code; code = (code + h->count[l]) << 1; }
    for (int i = 0; i < n; i++) {
        const int l = len[i];
        if (!l || l > 9) { if (l) next[l]++; continue; }
        uint32_t c = next[l]++, r = 0;
        for (int b = 0; b < l; b++) r |= ((c >> b) & 1) << (l - 1 - b);
        for (uint32_t k = r; k < 512; k += 1u << l) h->fast[k] = (uint16_t)(i << 4 | l);
    }
    return 1;
}
static int inf_sym(Infl *z, const InfHuff *h) {
    if (z->n < 16) inf_fill(z);
    const uint16_t f = h->fast[z->bits & 511];
    if (f) { z->bits >>= f & 15; z->n -= f & 15; return f >> 4; }
    int code = 0, first = 0, idx = 0;
    for (int l = 1; l < 16; l++) {
        code |= (int)(z->bits & 1); z->bits >>= 1; z->n--;
        const int c = h->count[l];
        if (code - first < c) return h->sym[idx + code - first];
        idx += c; first = (first + c) << 1; code <<= 1;
    }
    return -1;
}
static int inf_codes(Infl *z, const InfHuff *L, const InfHuff *D) {
    static const uint16_t lb[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint8_t le[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t db[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint8_t de[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for (;;) {
        int s = inf_sym(z, L);
        if (s < 0 || z->over > 16) return 0;
        if (s < 256) { if (z->pos >= z->cap) return 0; z->out[z->pos++] = (uint8_t)s; continue; }
        if (s == 256) return 1;
        s -= 257;
        if (s >= 29) return 0;
        const size_t len = lb[s] + inf_bits(z, le[s]);
        const int d = inf_sym(z, D);
        if (d < 0 || d >= 30) return 0;
        const size_t dist = db[d] + inf_bits(z, de[d]);
        if (dist > z->pos || len > z->cap - z->pos) return 0;
        uint8_t *o = z->out + z->pos;
        for (size_t i = 0; i < len; i++) o[i] = o[i - dist];
        z->pos += len;
    }
}
static int im_lax;      /* so o teste de arquivos estragados: ignora as somas, para os estragos chegarem mais fundo */
/* descomprime um fluxo zlib em exatamente cap bytes */
static int zlib_inflate(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    if (n < 6 || (in[0] & 15) != 8 || (in[0] >> 4) > 7 || ((in[0] << 8 | in[1]) % 31) || (in[1] & 32)) return 0;
    Infl z = { in + 2, in + n, 0, 0, 0, out, 0, cap };
    InfHuff *L = xalloc(2, sizeof(InfHuff)), *D = L + 1;
    int last = 0, ok = 1;
    while (ok && !last) {
        last = (int)inf_bits(&z, 1);
        const int t = (int)inf_bits(&z, 2);
        uint8_t len[320];
        if (z.over > 16) { ok = 0; break; }
        if (t == 0) {
            inf_bits(&z, z.n & 7);
            const unsigned a = inf_bits(&z, 16), b = inf_bits(&z, 16);
            if ((a ^ b) != 0xFFFF || a > cap - z.pos) { ok = 0; break; }
            for (unsigned i = 0; i < a && z.over <= 16; i++) z.out[z.pos++] = (uint8_t)inf_bits(&z, 8);
            ok = z.over <= 16;
        } else if (t == 1) {
            int i = 0;
            while (i < 144) len[i++] = 8;
            while (i < 256) len[i++] = 9;
            while (i < 280) len[i++] = 7;
            while (i < 288) len[i++] = 8;
            inf_build(L, len, 288);
            for (i = 0; i < 30; i++) len[i] = 5;
            for (; i < 32; i++) len[i] = 5;
            inf_build(D, len, 32);
            ok = inf_codes(&z, L, D);
        } else if (t == 2) {
            static const uint8_t ord[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            const int nl = (int)inf_bits(&z, 5) + 257, nd = (int)inf_bits(&z, 5) + 1, nc = (int)inf_bits(&z, 4) + 4;
            if (nl > 286 || nd > 30) { ok = 0; break; }
            memset(len, 0, sizeof len);
            for (int i = 0; i < nc; i++) len[ord[i]] = (uint8_t)inf_bits(&z, 3);
            if (!inf_build(L, len, 19)) { ok = 0; break; }
            uint8_t l2[320]; int i = 0;
            memset(l2, 0, sizeof l2);
            while (i < nl + nd) {
                const int s = inf_sym(&z, L);
                int rep, v = 0;
                if (s < 0 || z.over > 16) { ok = 0; break; }
                if (s < 16) { l2[i++] = (uint8_t)s; continue; }
                if (s == 16) { if (!i) { ok = 0; break; } v = l2[i - 1]; rep = 3 + (int)inf_bits(&z, 2); }
                else if (s == 17) rep = 3 + (int)inf_bits(&z, 3);
                else rep = 11 + (int)inf_bits(&z, 7);
                if (i + rep > nl + nd) { ok = 0; break; }
                while (rep--) l2[i++] = (uint8_t)v;
            }
            if (!ok || !l2[256] || !inf_build(L, l2, nl) || !inf_build(D, l2 + nl, nd)) { ok = 0; break; }
            ok = inf_codes(&z, L, D);
        } else ok = 0;
    }
    free(L);
    if (!ok || z.pos != cap) return 0;
    /* Adler-32: os 4 bytes depois do fluxo (sobram bits inteiros no acumulador) */
    inf_bits(&z, z.n & 7);
    uint32_t a = 1, b = 0, want = 0;
    for (int i = 0; i < 4; i++) want = want << 8 | inf_bits(&z, 8);
    for (size_t i = 0; i < cap; ) {
        const size_t e = cap - i < 5552 ? cap : i + 5552;
        for (; i < e; i++) { a += out[i]; b += a; }
        a %= 65521; b %= 65521;
    }
    return im_lax || (z.over <= 16 && want == (b << 16 | a));
}

static uint32_t im_crc32(const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}

/* ---- PNG: todos os tipos de cor e profundidades, entrelacado ou nao ---- */
static int png_decode(const uint8_t *f, size_t n, Img *im, int dims_only, char *err, size_t cap) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (n < 45 || memcmp(f, sig, 8) || im_be32(f + 8) != 13 || memcmp(f + 12, "IHDR", 4)) return im_fail(err, cap, "invalid PNG header");
    if (im_crc32(f + 12, 17) != im_be32(f + 29)) return im_fail(err, cap, "PNG header is damaged (CRC)");
    const uint32_t w = im_be32(f + 16), h = im_be32(f + 20);
    const int bd = f[24], ct = f[25], lace = f[28];
    if (!im_dims_ok(w, h, err, cap)) return 0;
    const int nch = ct == 0 || ct == 3 ? 1 : ct == 2 ? 3 : ct == 4 ? 2 : ct == 6 ? 4 : 0;
    const int bdok = bd == 8 || (bd == 16 && ct != 3) || ((bd == 1 || bd == 2 || bd == 4) && (ct == 0 || ct == 3));
    if (!nch || !bdok || f[26] || f[27] || lace > 1) return im_fail(err, cap, "unsupported PNG format");
    im->w = (int)w; im->h = (int)h; im->rgb = NULL; im->alpha = NULL;
    if (dims_only) return 1;
    const int bpp = nch * bd;
    size_t raw = 0;      /* bytes descomprimidos, com o byte de filtro de cada linha */
    static const uint8_t x0[7] = {0,4,0,2,0,1,0}, y0[7] = {0,0,4,0,2,0,1}, dx[7] = {8,8,4,4,2,2,1}, dy[7] = {8,8,8,4,4,2,2};
    const int np = lace ? 7 : 1;
    uint32_t pw[7], ph[7]; size_t rb[7];
    for (int p = 0; p < np; p++) {
        pw[p] = lace ? (w > x0[p] ? (w - x0[p] + dx[p] - 1) / dx[p] : 0) : w;
        ph[p] = lace ? (h > y0[p] ? (h - y0[p] + dy[p] - 1) / dy[p] : 0) : h;
        rb[p] = ((size_t)pw[p] * bpp + 7) / 8;
        if (pw[p] && ph[p]) raw += (rb[p] + 1) * ph[p];
    }
    uint8_t pal[256][4]; int npal = 0, has_key = 0; uint32_t key[3] = {0, 0, 0};
    memset(pal, 255, sizeof pal);
    uint8_t *z = xalloc(n, 1); size_t zn = 0;
    int end = 0, seen = 0, ok = 1;
    for (size_t p = 33; ok && !end && p + 12 <= n; ) {
        const uint32_t len = im_be32(f + p);
        if (len > n - p - 12) break;
        const uint8_t *ty = f + p + 4, *d = f + p + 8;
        const int crit = !(ty[0] & 32);
        if ((crit || !memcmp(ty, "tRNS", 4)) && im_crc32(ty, (size_t)len + 4) != im_be32(d + len)) { ok = im_fail(err, cap, "PNG data is damaged (CRC)"); break; }
        if (!memcmp(ty, "PLTE", 4)) {
            if (len % 3 || len > 768 || seen) { ok = im_fail(err, cap, "invalid PNG palette"); break; }
            npal = (int)(len / 3);
            for (int i = 0; i < npal; i++) memcpy(pal[i], d + 3 * i, 3);
        } else if (!memcmp(ty, "tRNS", 4)) {
            if (ct == 3) { for (uint32_t i = 0; i < len && i < 256; i++) pal[i][3] = d[i]; }
            else if (ct == 0 && len >= 2) { has_key = 1; key[0] = (uint32_t)d[0] << 8 | d[1]; }
            else if (ct == 2 && len >= 6) { has_key = 1; for (int i = 0; i < 3; i++) key[i] = (uint32_t)d[2 * i] << 8 | d[2 * i + 1]; }
        } else if (!memcmp(ty, "IDAT", 4)) { memcpy(z + zn, d, len); zn += len; seen = 1; }
        else if (!memcmp(ty, "IEND", 4)) end = 1;
        else if (crit) { ok = im_fail(err, cap, "unknown critical chunk in the PNG"); break; }
        p += (size_t)len + 12;
    }
    if (ok && ct == 3 && !npal) ok = im_fail(err, cap, "PNG without its palette");
    uint8_t *o = NULL;
    if (ok) {
        o = malloc(raw);
        im->rgb = malloc((size_t)w * h * 3);
        if (im_want_alpha) im->alpha = malloc((size_t)w * h);
        if (!o || !im->rgb || (im_want_alpha && !im->alpha)) ok = im_fail(err, cap, "out of memory for the image");
        else if (!zlib_inflate(z, zn, o, raw)) ok = im_fail(err, cap, "PNG data is damaged or incomplete");
    }
    free(z);
    const int fb = bpp >= 8 ? bpp / 8 : 1;
    const uint8_t *s = o;
    for (int p = 0; ok && p < np; p++) {
        if (!pw[p] || !ph[p]) continue;
        const size_t R = rb[p];
        uint8_t *prev = NULL;
        for (uint32_t y = 0; ok && y < ph[p]; y++) {
            const int ft = *s++;
            uint8_t *r = o + (s - o);
            s += R;
            switch (ft) {      /* filtros da linha (as linhas ja desfeitas ficam no proprio buffer) */
            case 0: break;
            case 1: for (size_t i = (size_t)fb; i < R; i++) r[i] = (uint8_t)(r[i] + r[i - fb]); break;
            case 2: if (prev) for (size_t i = 0; i < R; i++) r[i] = (uint8_t)(r[i] + prev[i]); break;
            case 3: for (size_t i = 0; i < R; i++) r[i] = (uint8_t)(r[i] + (((i >= (size_t)fb ? r[i - fb] : 0) + (prev ? prev[i] : 0)) >> 1)); break;
            case 4: for (size_t i = 0; i < R; i++) {
                const int a = i >= (size_t)fb ? r[i - fb] : 0, b = prev ? prev[i] : 0, c = prev && i >= (size_t)fb ? prev[i - fb] : 0;
                const int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);
                r[i] = (uint8_t)(r[i] + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c));
            } break;
            default: ok = im_fail(err, cap, "invalid PNG filter");
            }
            prev = r;
            if (!ok) break;
            const uint32_t yy = lace ? y0[p] + y * dy[p] : y;
            uint8_t *line = im->rgb + (size_t)yy * w * 3;
            for (uint32_t x = 0; x < pw[p]; x++) {
                const size_t xx = lace ? x0[p] + x * dx[p] : x;
                uint8_t *px = line + xx * 3;
                uint8_t *al = im->alpha ? im->alpha + (size_t)yy * w + xx : NULL;
                uint32_t v[4] = {0, 0, 0, 0};
                if (bd == 8) for (int k = 0; k < nch; k++) v[k] = r[(size_t)x * nch + k];
                else if (bd == 16) for (int k = 0; k < nch; k++) v[k] = (uint32_t)r[2 * ((size_t)x * nch + k)] << 8 | r[2 * ((size_t)x * nch + k) + 1];
                else v[0] = (r[(size_t)x * bd / 8] >> (8 - bd - (int)((size_t)x * bd % 8))) & ((1u << bd) - 1);
                if (ct == 3) {
                    const uint8_t *c = pal[v[0]];      /* indice fora da paleta: branco */
                    if (al) { memcpy(px, c, 3); *al = c[3]; } else if (c[3] == 255) memcpy(px, c, 3); else for (int k = 0; k < 3; k++) px[k] = im_white(c[k], c[3]);
                    continue;
                }
                int a = 255;
                if (has_key && v[0] == key[0] && (ct == 0 || (v[1] == key[1] && v[2] == key[2]))) a = 0;
                if (ct == 4) a = (int)(bd == 16 ? v[1] >> 8 : v[1]);
                if (ct == 6) a = (int)(bd == 16 ? v[3] >> 8 : v[3]);
                for (int k = 0; k < 3; k++) {
                    const uint32_t q = v[ct & 2 ? k : 0];
                    const int c8 = (int)(bd == 16 ? q >> 8 : bd == 8 ? q : q * 255 / ((1u << bd) - 1));
                    px[k] = a == 255 || al ? (uint8_t)c8 : im_white(c8, a);
                }
                if (al) *al = (uint8_t)a;
            }
        }
    }
    free(o);
    if (!ok) { free(im->rgb); im->rgb = NULL; free(im->alpha); im->alpha = NULL; }
    return ok;
}

/* ---- BMP sem compressao: 1, 4, 8, 16, 24 e 32 bits ---- */
static int bmp_decode(const uint8_t *f, size_t n, Img *im, int dims_only, char *err, size_t cap) {
    if (n < 54 || f[0] != 'B' || f[1] != 'M') return im_fail(err, cap, "invalid BMP header");
    const uint32_t off = im_le32(f + 10), hs = im_le32(f + 14);
    if (hs < 40 || hs > 200 || 14 + (size_t)hs > n) return im_fail(err, cap, "unsupported BMP header");
    const int32_t w0 = (int32_t)im_le32(f + 18), h0 = (int32_t)im_le32(f + 22);
    const int bpp = (int)im_le16(f + 28); const uint32_t comp = im_le32(f + 30);
    const int down = h0 < 0;
    if (w0 <= 0 || h0 == 0 || h0 == INT32_MIN) return im_fail(err, cap, "image has zero width or height");
    const uint32_t w = (uint32_t)w0, h = (uint32_t)(down ? -h0 : h0);
    if (!im_dims_ok(w, h, err, cap)) return 0;
    if (!(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32) || !(comp == 0 || (comp == 3 && (bpp == 16 || bpp == 32))))
        return im_fail(err, cap, "unsupported BMP format (compressed)");
    const size_t rb = (((size_t)w * bpp + 31) / 32) * 4;
    if (off > n || rb * h > n - off) return im_fail(err, cap, "BMP file is incomplete");
    im->w = (int)w; im->h = (int)h; im->rgb = NULL; im->alpha = NULL;
    if (dims_only) return 1;
    uint32_t mk[3] = { 0x7C00, 0x03E0, 0x001F };
    if (bpp == 32) { mk[0] = 0xFF0000; mk[1] = 0xFF00; mk[2] = 0xFF; }
    if (comp == 3) { if (n < 66) return im_fail(err, cap, "BMP file is incomplete"); for (int k = 0; k < 3; k++) mk[k] = im_le32(f + 54 + 4 * k); }
    int sh[3], bits[3];
    for (int k = 0; k < 3; k++) {
        sh[k] = 0; bits[k] = 0;
        if (!mk[k]) continue;
        while (!((mk[k] >> sh[k]) & 1)) sh[k]++;
        while (sh[k] + bits[k] < 32 && ((mk[k] >> (sh[k] + bits[k])) & 1)) bits[k]++;
    }
    size_t ncol = im_le32(f + 46);
    if (bpp <= 8 && (!ncol || ncol > 1u << bpp)) ncol = 1u << bpp;
    if (bpp <= 8 && 14 + (size_t)hs + ncol * 4 > n) return im_fail(err, cap, "BMP file is incomplete");
    im->rgb = malloc((size_t)w * h * 3);
    if (!im->rgb) return im_fail(err, cap, "out of memory for the image");
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *r = f + off + rb * (down ? y : h - 1 - y);
        uint8_t *o = im->rgb + (size_t)y * w * 3;
        for (uint32_t x = 0; x < w; x++, o += 3) {
            if (bpp == 24) { o[0] = r[3 * x + 2]; o[1] = r[3 * x + 1]; o[2] = r[3 * x]; }
            else if (bpp >= 16) {
                const uint32_t v = bpp == 16 ? im_le16(r + 2 * x) : im_le32(r + 4 * x);
                for (int k = 0; k < 3; k++) {
                    uint32_t c = bits[k] ? (v & mk[k]) >> sh[k] : 0;
                    if (bits[k] > 8) c >>= bits[k] - 8; else if (bits[k] && bits[k] < 8) c = c * 255 / ((1u << bits[k]) - 1);
                    o[k] = (uint8_t)c;
                }
            } else {
                const uint32_t v = (r[(size_t)x * bpp / 8] >> (8 - bpp - (int)((size_t)x * bpp % 8))) & ((1u << bpp) - 1);
                const uint8_t *c = v < ncol ? f + 14 + hs + 4 * (size_t)v : NULL;
                o[0] = c ? c[2] : 0; o[1] = c ? c[1] : 0; o[2] = c ? c[0] : 0;
            }
        }
    }
    return 1;
}
