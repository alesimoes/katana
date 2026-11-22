/* ---------- imagem: JPEG (ITU T.81) sequencial e progressivo, 8 bits, Huffman ----------
 * Os coeficientes de toda a imagem ficam na memoria (int16 por amostra) e a transformada roda no fim, para os dois modos
 * seguirem o mesmo caminho. Transformada inversa inteira de 13 bits (fatoracao de 11 produtos), croma ampliado com o
 * filtro triangular, cores em ponto fixo de 16 bits. A orientacao do EXIF e aplicada. Limites: os de png.c, mais um
 * teto de varreduras (arquivo progressivo malicioso com milhares de varreduras). */
typedef struct { uint8_t size[256], val[256]; uint16_t code_max_ok; int32_t maxcode[18]; int valptr[17], mincode[17], ok; uint16_t fast[512]; } JHuff;
typedef struct { int id, hs, vs, tq, bw, bh, w, h, dcpred, td, ta; int16_t *co; uint8_t *px; size_t stride; } JComp;
typedef struct {
    const uint8_t *p, *end;
    uint32_t bits; int nb, marker, pad;
    int w, h, nc, prog, hmax, vmax, restart, adobe, orient;
    uint16_t q[4][64]; int qok[4];
    JHuff dc[4], ac[4];
    JComp c[4];
    int eobrun, scans;
    const char *why;
} Jpg;
static const uint8_t J_ZZ[64] = { 0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63 };

static int j_err(Jpg *j, const char *m) { if (!j->why) j->why = m; return 0; }
static int j_bit_fill(Jpg *j) {
    while (j->nb <= 24) {
        uint32_t b = 0;
        if (j->marker || j->p >= j->end) j->pad++;      /* acabou o trecho: zeros */
        else {
            b = *j->p++;
            if (b == 0xFF) {
                while (j->p < j->end && *j->p == 0xFF) j->p++;
                const int m = j->p < j->end ? *j->p++ : 0xD9;
                if (m) { j->marker = m; b = 0; j->pad++; }
            }
        }
        j->bits |= b << (24 - j->nb); j->nb += 8;
    }
    return 1;
}
static int j_bits(Jpg *j, int n) {
    if (!n) return 0;
    if (j->nb < n) j_bit_fill(j);
    const int v = (int)(j->bits >> (32 - n));
    j->bits <<= n; j->nb -= n;
    return v;
}
static int j_ext(int v, int n) { return n && v < 1 << (n - 1) ? v - (1 << n) + 1 : v; }
static int j_huff_build(JHuff *h, const uint8_t *cnt, int nv) {
    int k = 0, code = 0;
    memset(h->fast, 0, sizeof h->fast);
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k; h->mincode[l] = code;
        for (int i = 0; i < cnt[l - 1]; i++, k++, code++) {
            if (code >= 1 << l) return 0;
            if (l <= 9 && k < nv) {
                const int f = code << (9 - l);
                for (int q = 0; q < 1 << (9 - l); q++) h->fast[f + q] = (uint16_t)(h->val[k] << 4 | l);
            }
        }
        h->maxcode[l] = cnt[l - 1] ? code - 1 : -1;
        code <<= 1;
    }
    h->ok = k == nv;
    return h->ok;
}
static int j_sym(Jpg *j, const JHuff *h) {
    if (j->nb < 16) j_bit_fill(j);
    const uint16_t f = h->fast[j->bits >> 23];
    if (f) { j->bits <<= f & 15; j->nb -= f & 15; return f >> 4; }
    int code = (int)(j->bits >> 23);
    for (int l = 10; l <= 16; l++) {
        code = (int)(j->bits >> (32 - l));
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l] && code >= h->mincode[l]) {
            j->bits <<= l; j->nb -= l;
            return h->val[(h->valptr[l] + code - h->mincode[l]) & 255];
        }
    }
    return -1;
}
/* bloco sequencial */
static int j_block(Jpg *j, JComp *c, int16_t *b) {
    const int t = j_sym(j, &j->dc[c->td]);
    if (t < 0 || t > 15) return j_err(j, "invalid JPEG code");
    c->dcpred = (int)((unsigned)c->dcpred + (unsigned)j_ext(j_bits(j, t), t));
    b[0] = (int16_t)c->dcpred;
    for (int k = 1; k < 64; ) {
        const int rs = j_sym(j, &j->ac[c->ta]);
        if (rs < 0) return j_err(j, "invalid JPEG code");
        const int r = rs >> 4, s = rs & 15;
        if (!s) { if (r < 15) break; k += 16; continue; }
        k += r;
        if (k > 63) return j_err(j, "invalid JPEG coefficient");
        b[J_ZZ[k++]] = (int16_t)j_ext(j_bits(j, s), s);
    }
    return 1;
}
/* blocos progressivos: primeira passada e refinamento, de DC e AC */
static int j_block_prog(Jpg *j, JComp *c, int16_t *b, int ss, int se, int ah, int al) {
    if (ss == 0) {
        if (ah) { if (j_bits(j, 1)) b[0] |= (int16_t)(1 << al); return 1; }
        const int t = j_sym(j, &j->dc[c->td]);
        if (t < 0 || t > 15) return j_err(j, "invalid JPEG code");
        c->dcpred = (int)((unsigned)c->dcpred + (unsigned)j_ext(j_bits(j, t), t));
        b[0] = (int16_t)((unsigned)c->dcpred << al);
        return 1;
    }
    const JHuff *h = &j->ac[c->ta];
    if (!ah) {
        if (j->eobrun) { j->eobrun--; return 1; }
        for (int k = ss; k <= se; ) {
            const int rs = j_sym(j, h);
            if (rs < 0) return j_err(j, "invalid JPEG code");
            const int r = rs >> 4, s = rs & 15;
            if (!s) {
                if (r < 15) { j->eobrun = (1 << r) - 1 + j_bits(j, r); break; }
                k += 16; continue;
            }
            k += r;
            if (k > se) return j_err(j, "invalid JPEG coefficient");
            b[J_ZZ[k++]] = (int16_t)((unsigned)j_ext(j_bits(j, s), s) << al);
        }
        return 1;
    }
    const int p1 = 1 << al, m1 = -(1 << al);
    int k = ss;
    if (!j->eobrun) {
        while (k <= se) {
            const int rs = j_sym(j, h);
            if (rs < 0) return j_err(j, "invalid JPEG code");
            int r = rs >> 4, s = rs & 15, v = 0;
            if (s) { if (s != 1) return j_err(j, "invalid JPEG coefficient"); v = j_bits(j, 1) ? p1 : m1; }
            else if (r < 15) { j->eobrun = (1 << r) + j_bits(j, r); break; }
            for (; k <= se; k++) {
                int16_t *q = &b[J_ZZ[k]];
                if (*q) { if (j_bits(j, 1) && !(*q & p1)) *q = (int16_t)(*q + (*q >= 0 ? p1 : m1)); }
                else if (r-- == 0) break;
            }
            if (v && k <= se) b[J_ZZ[k]] = (int16_t)v;
            k++;
        }
    }
    if (j->eobrun) {
        for (; k <= se; k++) {
            int16_t *q = &b[J_ZZ[k]];
            if (*q && j_bits(j, 1) && !(*q & p1)) *q = (int16_t)(*q + (*q >= 0 ? p1 : m1));
        }
        j->eobrun--;
    }
    return 1;
}
static int j_restart(Jpg *j) {      /* marca RSTn: alinha e zera as previsoes */
    j->bits = 0; j->nb = 0; j->eobrun = 0;
    for (int i = 0; i < j->nc; i++) j->c[i].dcpred = 0;
    if (!j->marker) {
        while (j->p + 1 < j->end && !(j->p[0] == 0xFF && j->p[1] && j->p[1] != 0xFF)) j->p++;
        if (j->p + 1 >= j->end) return 0;
        j->marker = j->p[1]; j->p += 2;
    }
    if (j->marker < 0xD0 || j->marker > 0xD7) return 0;      /* outra marca: o trecho acabou antes da hora */
    j->marker = 0; j->pad = 0;
    return 1;
}
static int j_scan(Jpg *j, const int *ci, int ns, int ss, int se, int ah, int al) {
    j->bits = 0; j->nb = 0; j->marker = 0; j->pad = 0; j->eobrun = 0;
    for (int i = 0; i < j->nc; i++) j->c[i].dcpred = 0;
    int mw, mh;
    if (ns == 1) { const JComp *c = &j->c[ci[0]]; mw = (c->w + 7) / 8; mh = (c->h + 7) / 8; }
    else { mw = (j->w + 8 * j->hmax - 1) / (8 * j->hmax); mh = (j->h + 8 * j->vmax - 1) / (8 * j->vmax); }
    long left = j->restart ? j->restart : -1;
    for (int my = 0; my < mh; my++)
        for (int mx = 0; mx < mw; mx++) {
            if (left == 0) { if (!j_restart(j)) return 1; left = j->restart; }
            if (left > 0) left--;
            if (j->pad > 64) return 1;      /* arquivo cortado: o resto fica como esta (cinza ou a passada anterior) */
            for (int s = 0; s < ns; s++) {
                JComp *c = &j->c[ci[s]];
                const int nx = ns == 1 ? 1 : c->hs, ny = ns == 1 ? 1 : c->vs;
                for (int v = 0; v < ny; v++) for (int u = 0; u < nx; u++) {
                    const int bx = mx * nx + u, by = my * ny + v;
                    int16_t *b = c->co + ((size_t)by * c->bw + bx) * 64;
                    if (!(j->prog ? j_block_prog(j, c, b, ss, se, ah, al) : j_block(j, c, b))) return 0;
                }
            }
        }
    return 1;
}

/* transformada inversa 8x8: duas passadas com constantes de 13 bits; a primeira guarda 2 bits a mais */
#define J_D(x, n) ((int32_t)((x) + (1u << ((n) - 1))) >> (n))
/* tudo em uint32_t: coeficiente absurdo de arquivo estragado da a volta em vez de estouro indefinido */
static inline void j_idct1(const uint32_t *s, uint32_t *e, uint32_t *o) {      /* parte par em e[4], impar em o[4] */
    const uint32_t z1 = (s[2] + s[6]) * 4433u, t2 = z1 - s[6] * 15137u, t3 = z1 + s[2] * 6270u;
    const uint32_t t0 = (s[0] + s[4]) << 13, t1 = (s[0] - s[4]) << 13;
    e[0] = t0 + t3; e[3] = t0 - t3; e[1] = t1 + t2; e[2] = t1 - t2;
    uint32_t a = s[7], b = s[5], c = s[3], d = s[1];
    uint32_t y1 = a + d, y2 = b + c, y3 = a + c, y4 = b + d;
    const uint32_t y5 = (y3 + y4) * 9633u;
    a *= 2446u; b *= 16819u; c *= 25172u; d *= 12299u;
    y1 *= (uint32_t)-7373; y2 *= (uint32_t)-20995; y3 = y3 * (uint32_t)-16069 + y5; y4 = y4 * (uint32_t)-3196 + y5;
    o[3] = a + y1 + y3; o[2] = b + y2 + y4; o[1] = c + y2 + y3; o[0] = d + y1 + y4;
}
static void j_idct(const int16_t *co, const uint16_t *q, uint8_t *out, size_t stride) {
    uint32_t w[64], s[8], e[4], o[4];
    for (int x = 0; x < 8; x++) {
        for (int k = 0; k < 8; k++) {
            s[k] = (uint32_t)((int32_t)co[8 * k + x] * (int32_t)q[8 * k + x]);
        }
        j_idct1(s, e, o);
        for (int k = 0; k < 4; k++) { w[8 * k + x] = (uint32_t)J_D(e[k] + o[k], 11); w[8 * (7 - k) + x] = (uint32_t)J_D(e[k] - o[k], 11); }
    }
    for (int y = 0; y < 8; y++) {
        j_idct1(w + 8 * y, e, o);
        for (int k = 0; k < 4; k++) {
            const int32_t a = J_D(e[k] + o[k], 18) + 128, b = J_D(e[k] - o[k], 18) + 128;
            out[y * stride + k] = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
            out[y * stride + 7 - k] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
        }
    }
}
/* linha y (na resolucao cheia) de um componente, ampliada para j->w amostras */
static void j_row(const Jpg *j, const JComp *c, int y, uint8_t *out, int *tmp) {
    const int rx = j->hmax / c->hs, ry = j->vmax / c->vs, exact = j->hmax % c->hs == 0 && j->vmax % c->vs == 0;
    if (exact && rx == 2 && (ry == 1 || ry == 2) && c->w > 2) {
        const int cy = y / ry, ny = ry == 1 ? cy : (y & 1) ? (cy + 1 < c->h ? cy + 1 : cy) : (cy > 0 ? cy - 1 : 0);
        const uint8_t *a = c->px + (size_t)cy * c->stride, *b = c->px + (size_t)ny * c->stride;
        const int n = c->w;
        if (ry == 1) {
            for (int x = 0; x < j->w; x++) {
                const int cx = x >> 1, nx = x & 1 ? cx + 1 : cx - 1;
                out[x] = nx < 0 || nx >= n ? a[cx] : (uint8_t)((3 * a[cx] + a[nx] + (x & 1 ? 2 : 1)) >> 2);
            }
        } else {
            for (int x = 0; x < n; x++) tmp[x] = 3 * a[x] + b[x];
            for (int x = 0; x < j->w; x++) {
                const int cx = x >> 1, nx = x & 1 ? cx + 1 : cx - 1;
                const int rnd = x & 1 ? 7 : 8;
                out[x] = (uint8_t)(nx < 0 || nx >= n ? (tmp[cx] * 4 + rnd) >> 4 : (tmp[cx] * 3 + tmp[nx] + rnd) >> 4);
            }
        }
        return;
    }
    const int cy = y * c->vs / j->vmax;
    const uint8_t *a = c->px + (size_t)(cy < c->h ? cy : c->h - 1) * c->stride;
    for (int x = 0; x < j->w; x++) { const int cx = x * c->hs / j->hmax; out[x] = a[cx < c->w ? cx : c->w - 1]; }
}
static uint8_t j_clip(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* orientacao (marca 0x0112 do primeiro diretorio do EXIF) */
static void j_exif(Jpg *j, const uint8_t *d, size_t n) {
    if (n < 14 || memcmp(d, "Exif\0\0", 6)) return;
    d += 6; n -= 6;
    const int le = d[0] == 'I' && d[1] == 'I';
    if (!le && !(d[0] == 'M' && d[1] == 'M')) return;
#define E16(p) (le ? im_le16(p) : (uint32_t)(p)[0] << 8 | (p)[1])
#define E32(p) (le ? im_le32(p) : im_be32(p))
    const size_t o = E32(d + 4);
    if (o < 8 || o > n - 2 || n < 10) return;
    const size_t k = E16(d + o);
    for (size_t i = 0; i < k && o + 2 + (i + 1) * 12 <= n; i++) {
        const uint8_t *e = d + o + 2 + i * 12;
        if (E16(e) == 0x0112 && E16(e + 2) == 3) { const int v = (int)E16(e + 8); if (v >= 1 && v <= 8) j->orient = v; return; }
    }
#undef E16
#undef E32
}
static int j_frame(Jpg *j, const uint8_t *d, size_t n, int dims_only, char *err, size_t cap) {
    if (n < 6 || d[0] != 8) return j_err(j, "unsupported JPEG (only 8 bits per sample)");
    if (j->nc) return j_err(j, "invalid JPEG (two frames)");
    j->h = d[1] << 8 | d[2]; j->w = d[3] << 8 | d[4];
    const int nc = d[5];
    if (!im_dims_ok((uint64_t)j->w, (uint64_t)j->h, err, cap)) return j_err(j, err);
    if ((nc != 1 && nc != 3 && nc != 4) || n < 6 + 3 * (size_t)nc) return j_err(j, "unsupported JPEG components");
    j->hmax = j->vmax = 1;
    for (int i = 0; i < nc; i++) {
        JComp *c = &j->c[i];
        c->id = d[6 + 3 * i]; c->hs = d[7 + 3 * i] >> 4; c->vs = d[7 + 3 * i] & 15; c->tq = d[8 + 3 * i];
        if (c->hs < 1 || c->hs > 4 || c->vs < 1 || c->vs > 4 || c->tq > 3) return j_err(j, "invalid JPEG sampling");
        if (nc == 1) c->hs = c->vs = 1;
        if (c->hs > j->hmax) j->hmax = c->hs;
        if (c->vs > j->vmax) j->vmax = c->vs;
    }
    j->nc = nc;
    if (dims_only) return 1;
    const int mw = (j->w + 8 * j->hmax - 1) / (8 * j->hmax), mh = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
    for (int i = 0; i < nc; i++) {
        JComp *c = &j->c[i];
        c->bw = mw * c->hs; c->bh = mh * c->vs;
        c->w = (j->w * c->hs + j->hmax - 1) / j->hmax; c->h = (j->h * c->vs + j->vmax - 1) / j->vmax;
        c->stride = (size_t)c->bw * 8;
        c->co = calloc((size_t)c->bw * c->bh, 64 * sizeof(int16_t));
        if (!c->co) return j_err(j, "out of memory for the image");
    }
    return 1;
}
static int jpeg_decode(const uint8_t *f, size_t n, Img *im, int dims_only, char *err, size_t cap) {
    if (n < 4 || f[0] != 0xFF || f[1] != 0xD8) return im_fail(err, cap, "invalid JPEG header");
    Jpg *j = xalloc(1, sizeof *j);
    char e2[160] = "";
    j->p = f + 2; j->end = f + n; j->adobe = -1; j->orient = 1;
    im->rgb = NULL;
    int ok = 1, done = 0;
    while (ok && !done) {
        int m = j->marker;
        j->marker = 0;
        if (!m) {
            while (j->p < j->end && *j->p != 0xFF) j->p++;
            while (j->p < j->end && *j->p == 0xFF) j->p++;
            if (j->p >= j->end) break;
            m = *j->p++;
        }
        if (!m || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (m == 0xD9) break;
        if (j->end - j->p < 2) break;
        const size_t len = (size_t)(j->p[0] << 8 | j->p[1]);
        if (len < 2 || len > (size_t)(j->end - j->p)) break;
        const uint8_t *d = j->p + 2; size_t dn = len - 2;
        j->p += len;
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            j->prog = m == 0xC2;
            ok = j_frame(j, d, dn, dims_only, e2, sizeof e2);
            if (ok && dims_only) done = 1;
        } else if (m >= 0xC3 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) ok = j_err(j, "unsupported JPEG type (lossless or arithmetic)");
        else if (m == 0xC4) {
            while (ok && dn >= 17) {
                int nv = 0;
                for (int i = 0; i < 16; i++) nv += d[1 + i];
                const int cl = d[0] >> 4, id = d[0] & 15;
                if (nv > 256 || dn < 17 + (size_t)nv || cl > 1 || id > 3) { ok = j_err(j, "invalid JPEG Huffman table"); break; }
                JHuff *h = cl ? &j->ac[id] : &j->dc[id];
                memset(h->val, 0, sizeof h->val);
                memcpy(h->val, d + 17, (size_t)nv);
                if (!j_huff_build(h, d + 1, nv)) ok = j_err(j, "invalid JPEG Huffman table");
                d += 17 + nv; dn -= 17 + (size_t)nv;
            }
        } else if (m == 0xDB) {
            while (ok && dn >= 65) {
                const int pq = d[0] >> 4, id = d[0] & 15;
                const size_t sz = pq ? 129 : 65;
                if (pq > 1 || id > 3 || dn < sz) { ok = j_err(j, "invalid JPEG quantization table"); break; }
                for (int k = 0; k < 64; k++) j->q[id][J_ZZ[k]] = (uint16_t)(pq ? d[1 + 2 * k] << 8 | d[2 + 2 * k] : d[1 + k]);
                j->qok[id] = 1;
                d += sz; dn -= sz;
            }
        } else if (m == 0xDD) { if (dn >= 2) j->restart = d[0] << 8 | d[1]; }
        else if (m == 0xE1) j_exif(j, d, dn);
        else if (m == 0xEE) { if (dn >= 12 && !memcmp(d, "Adobe", 5)) j->adobe = d[11]; }
        else if (m == 0xDA) {
            const int ns = dn ? d[0] : 0;
            if (!j->nc || ns < 1 || ns > j->nc || dn < 4 + 2 * (size_t)ns) { ok = j_err(j, "invalid JPEG scan"); break; }
            if (++j->scans > 64) { ok = j_err(j, "JPEG with too many scans"); break; }
            int ci[4], blocks = 0;
            for (int s = 0; ok && s < ns; s++) {
                ci[s] = -1;
                for (int i = 0; i < j->nc; i++) if (j->c[i].id == d[1 + 2 * s]) { ci[s] = i; break; }
                if (ci[s] < 0) { ok = j_err(j, "invalid JPEG scan"); break; }
                for (int q = 0; q < s; q++) if (ci[q] == ci[s]) ok = j_err(j, "invalid JPEG scan");
                JComp *c = &j->c[ci[s]];
                c->td = d[2 + 2 * s] >> 4; c->ta = d[2 + 2 * s] & 15;
                if (c->td > 3 || c->ta > 3) ok = j_err(j, "invalid JPEG scan");
                blocks += c->hs * c->vs;
            }
            if (!ok) break;
            int ss = d[1 + 2 * ns], se = d[2 + 2 * ns];
            const int ah = d[3 + 2 * ns] >> 4, al = d[3 + 2 * ns] & 15;
            if (!j->prog) { ss = 0; se = 63; }
            else if (ss > se || se > 63 || (ss == 0 && se != 0) || (ss && ns != 1) || al > 13 || (ah && ah != al + 1)) { ok = j_err(j, "invalid JPEG scan"); break; }
            if (ns > 1 && blocks > 10) { ok = j_err(j, "invalid JPEG sampling"); break; }
            for (int s = 0; ok && s < ns; s++) {
                const JComp *c = &j->c[ci[s]];
                if ((!j->prog || (ss == 0 && !ah)) && !j->dc[c->td].ok) ok = j_err(j, "JPEG without a Huffman table");
                if ((!j->prog || ss) && !j->ac[c->ta].ok) ok = j_err(j, "JPEG without a Huffman table");
            }
            if (ok) ok = j_scan(j, ci, ns, ss, se, ah, al);
            if (ok && j->marker >= 0xD0 && j->marker <= 0xD7) j->marker = 0;
        }
    }
    if (ok && !j->nc) ok = j_err(j, "JPEG without image data");
    if (ok && !dims_only && !j->scans) ok = j_err(j, "JPEG without image data");
    const int swap = j->orient >= 5;
    if (ok) { im->w = swap ? j->h : j->w; im->h = swap ? j->w : j->h; }
    if (ok && !dims_only) {
        for (int i = 0; ok && i < j->nc; i++) {
            JComp *c = &j->c[i];
            if (!j->qok[c->tq]) { ok = j_err(j, "JPEG without a quantization table"); break; }
            c->px = malloc(c->stride * c->bh * 8);
            if (!c->px) { ok = j_err(j, "out of memory for the image"); break; }
            for (int by = 0; by < c->bh; by++) for (int bx = 0; bx < c->bw; bx++)
                j_idct(c->co + ((size_t)by * c->bw + bx) * 64, j->q[c->tq], c->px + (size_t)by * 8 * c->stride + (size_t)bx * 8, c->stride);
            free(c->co); c->co = NULL;
        }
        im->rgb = ok ? malloc((size_t)j->w * j->h * 3) : NULL;
        uint8_t *rows = ok ? malloc((size_t)j->w * 4) : NULL;
        int *tmp = ok ? malloc(((size_t)j->w + 16) * sizeof(int)) : NULL;
        if (ok && (!im->rgb || !rows || !tmp)) ok = j_err(j, "out of memory for the image");
        const int rgb3 = j->nc == 3 && (j->adobe == 0 || (j->adobe < 0 && j->c[0].id == 'R' && j->c[1].id == 'G' && j->c[2].id == 'B'));
        for (int y = 0; ok && y < j->h; y++) {
            for (int i = 0; i < j->nc; i++) j_row(j, &j->c[i], y, rows + (size_t)i * j->w, tmp);
            for (int x = 0; x < j->w; x++) {
                int v[4] = { rows[x], 0, 0, 0 };
                for (int i = 1; i < j->nc; i++) v[i] = rows[(size_t)i * j->w + x];
                int r, g, b;
                if (j->nc == 1) r = g = b = v[0];
                else if (rgb3 || (j->nc == 4 && j->adobe != 2)) { r = v[0]; g = v[1]; b = v[2]; }
                else {
                    const int cb = v[1] - 128, cr = v[2] - 128;
                    r = j_clip(v[0] + ((91881 * cr + 32768) >> 16));
                    g = j_clip(v[0] + ((-22554 * cb - 46802 * cr + 32768) >> 16));
                    b = j_clip(v[0] + ((116130 * cb + 32768) >> 16));
                }
                if (j->nc == 4) {      /* CMYK (com a marca da Adobe, gravado invertido) */
                    int k = v[3];
                    if (j->adobe < 0) { r = 255 - r; g = 255 - g; b = 255 - b; k = 255 - k; }
                    r = (r * k + 127) / 255; g = (g * k + 127) / 255; b = (b * k + 127) / 255;
                }
                int ox = x, oy = y;
                switch (j->orient) {
                case 2: ox = j->w - 1 - x; break;
                case 3: ox = j->w - 1 - x; oy = j->h - 1 - y; break;
                case 4: oy = j->h - 1 - y; break;
                case 5: ox = y; oy = x; break;
                case 6: ox = j->h - 1 - y; oy = x; break;
                case 7: ox = j->h - 1 - y; oy = j->w - 1 - x; break;
                case 8: ox = y; oy = j->w - 1 - x; break;
                }
                uint8_t *o = im->rgb + ((size_t)oy * im->w + ox) * 3;
                o[0] = (uint8_t)r; o[1] = (uint8_t)g; o[2] = (uint8_t)b;
            }
        }
        free(rows); free(tmp);
        if (!ok) { free(im->rgb); im->rgb = NULL; }
    }
    if (!ok) im_fail(err, cap, e2[0] ? e2 : j->why ? j->why : "invalid JPEG");
    for (int i = 0; i < 4; i++) { free(j->c[i].co); free(j->c[i].px); }
    free(j);
    return ok;
}

/* ---- qualquer formato aceito; NULL em err quando deu certo ---- */
static const char *img_kind(const uint8_t *f, size_t n) {
    if (n >= 8 && !memcmp(f, "\x89PNG\r\n\x1a\n", 8)) return "png";
    if (n >= 3 && f[0] == 0xFF && f[1] == 0xD8 && f[2] == 0xFF) return "jpeg";
    if (n >= 2 && f[0] == 'B' && f[1] == 'M') return "bmp";
    if (n >= 12 && !memcmp(f, "RIFF", 4) && !memcmp(f + 8, "WEBP", 4)) return "webp";
    if (n >= 4 && !memcmp(f, "GIF8", 4)) return "gif";
    return NULL;
}
static int img_decode(const uint8_t *f, size_t n, Img *im, int dims_only, char *err, size_t cap) {
    memset(im, 0, sizeof *im);
    if (n > IM_MAX_FILE) return im_fail(err, cap, "image file is too large (limit 64 MB)");
    const char *k = img_kind(f, n);
    if (k && *k == 'p') return png_decode(f, n, im, dims_only, err, cap);
    if (k && *k == 'j') return jpeg_decode(f, n, im, dims_only, err, cap);
    if (k && *k == 'b') return bmp_decode(f, n, im, dims_only, err, cap);
    if (k) { snprintf(err, cap, "%s images are not supported (PNG, JPEG and BMP are); convert it first", k); return 0; }
    return im_fail(err, cap, "not an image file this reader knows (PNG, JPEG, BMP)");
}
