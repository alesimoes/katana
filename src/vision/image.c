/* ---------- visao: imagens na conversa ----------
 * No texto a imagem e a marca <|katana_image:HASH|> (32 digitos do SHA-256 do arquivo). O tokenizador troca a marca por
 * <|vision_start|>, um id por vetor da imagem e <|vision_end|>. Os ids da imagem ficam acima do vocabulario:
 * VIS_BASE | lugar da imagem na tabela | indice do vetor; o mesmo arquivo recebe sempre o mesmo lugar, entao comparar ids
 * compara conteudo dentro do processo. Em disco (cache de prefixo) quem entra na chave e o hash, nunca o lugar.
 * Os vetores so sao calculados quando alguma passada pede (vis_row): retomar uma sessao com o KV em disco nao recalcula.
 * O projetor e mapeado na primeira imagem e solto em seguida (KATANA_VISION_KEEP=1 mantem).
 * Posicoes (RoPE de tres eixos): texto avanca os tres juntos; a imagem usa (inicio, inicio + linha, inicio + coluna) e o
 * texto seguinte continua de inicio + maior lado. A frequencia i da cabeca usa o eixo i mod 3. vis_rp guarda os tres
 * numeros de cada posicao do cache; so existe depois da primeira imagem (sem imagem, nada muda no caminho de texto). */
static uint64_t pfx_hash(uint64_t h, const void *p, size_t n);      /* agent/prefix_cache.c */
#define VIS_TAG "<|katana_image:"
#define VIS_TAG_N 15
#define VIS_MARK_N (VIS_TAG_N + 32 + 2)
#define VIS_MAX 8192                         /* imagens por processo */
#define VIS_IDX 65536                        /* vetores por imagem */
#define VIS_EMB_KEEP 4                       /* imagens com os vetores na memoria */
typedef struct { char hex[33]; uint8_t full[32]; Img px; int ow, oh, gw, gh, nt; float *emb; uint64_t used; } VisImg;
static struct {
    int on, start, end, ps, mg, outd, budget, n, store, nemb, warned;
    char path[1200]; int64_t psize;
    VisImg *v; Vit *vit; uint64_t tick; volatile int lock;
    char pend[32][33]; int npend;            /* imagens que a ferramenta em curso produziu: so estas passam pelo defang */
    Buf next;                                /* marcas para a proxima mensagem do usuario (--image) */
    int32_t *nxt, *st; int ctx;              /* por posicao do cache: proxima posicao de texto e inicio da imagem */
    double t_enc; long n_enc, tok_enc;
} g_vis;
static void vis_lock(void) { while (__sync_lock_test_and_set(&g_vis.lock, 1)) CPU_RELAX(); }
static void vis_unlock(void) { __sync_lock_release(&g_vis.lock); }

/* memoria residente do processo em MB (total e so a anonima) */
static void vis_rss(double *tot, double *anon) {
    *tot = *anon = 0;
#ifndef _WIN32
    FILE *f = fopen("/proc/self/status", "r"); char l[200]; long k;
    if (!f) return;
    while (fgets(l, sizeof l, f)) { if (sscanf(l, "VmRSS: %ld", &k) == 1) *tot = k / 1024.0; else if (sscanf(l, "RssAnon: %ld", &k) == 1) *anon = k / 1024.0; }
    fclose(f);
#endif
}

/* ---- tabela de imagens ---- */
static int vis_find(const char *hex) { for (int i = 0; i < g_vis.n; i++) if (!memcmp(g_vis.v[i].hex, hex, 32)) return i; return -1; }
/* pixels que o orcamento de tokens permite; menor imagem = 64 tokens, como a referencia */
static double vis_max_px(void) { const double s = (double)g_vis.ps * g_vis.mg; return (g_vis.budget > 64 ? g_vis.budget : 64) * s * s; }
static int vis_target(int w, int h, int *ow, int *oh) {
    const int s = g_vis.ps * g_vis.mg;
    return vr_size(h, w, s, 64.0 * s * s, vis_max_px(), oh, ow);
}
static void vis_store_path(char *out, size_t cap, const char *hex) {
    const char *over = getenv("KATANA_CACHE_DIR");
    if (over && *over) snprintf(out, cap, "%s/img-%.32s.bin", over, hex);
    else { char home[700]; agent_home(home, sizeof home); snprintf(out, cap, "%s/.katana/cache/img-%.32s.bin", home, hex); }
}
/* guarda o arquivo original para a conversa poder ser retomada noutro processo; ficam as 64 mais recentes */
static void vis_store_put(const char *hex, const uint8_t *f, size_t n) {
    char p[1100], t[1140], dir[1100];
    vis_store_path(p, sizeof p, hex);
    snprintf(dir, sizeof dir, "%s", p);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; char *s2 = strrchr(dir, '/'); if (s2) { *s2 = 0; MKDIR(dir); *s2 = '/'; } MKDIR(dir); }
    struct stat st;
    if (stat(p, &st) == 0 && (size_t)st.st_size == n) return;
    snprintf(t, sizeof t, "%s.%ld.tmp", p, (long)getpid());
#ifndef _WIN32
    const int fd = open(t, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    FILE *o = fd >= 0 ? fdopen(fd, "wb") : NULL;
#else
    FILE *o = fopen(t, "wb");
#endif
    if (!o) return;
    const int ok = fwrite(f, 1, n, o) == n;
    if (fclose(o) != 0 || !ok) { remove(t); return; }
    remove(p);
    if (rename(t, p) != 0) { remove(t); return; }
#ifndef _WIN32
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de; struct { char name[300]; long t; } *e = xalloc(512, sizeof *e); int ne = 0;
    while ((de = readdir(d)) && ne < 512) {
        char fp[1500];
        if (strncmp(de->d_name, "img-", 4) != 0 || strlen(de->d_name) > 200) continue;
        snprintf(fp, sizeof fp, "%s/%s", dir, de->d_name);
        if (stat(fp, &st) != 0) continue;
        snprintf(e[ne].name, sizeof e[ne].name, "%s", de->d_name); e[ne++].t = (long)st.st_mtime;
    }
    closedir(d);
    while (ne > 64) {
        int old = 0; char fp[1500];
        for (int i = 1; i < ne; i++) if (e[i].t < e[old].t) old = i;
        snprintf(fp, sizeof fp, "%s/%s", dir, e[old].name); remove(fp);
        e[old] = e[--ne];
    }
    free(e);
#endif
}

/* registra a imagem (decodifica, reduz ao orcamento); devolve o lugar ou -1 com a mensagem em err */
static int vis_add(const uint8_t *f, size_t n, char *err, size_t cap) {
    if (!g_vis.on) { im_fail(err, cap, "this model has no vision loaded"); return -1; }
    char hx[65];
    if (n < 16 || n > IM_MAX_FILE) { im_fail(err, cap, n < 16 ? "the image is empty" : "image file is too large (limit 64 MB)"); return -1; }
    sha_hex(f, n, hx);
    vis_lock();
    int k = vis_find(hx);
    vis_unlock();
    if (k >= 0) return k;
    Img im; int ow, oh;
    if (!img_decode(f, n, &im, 0, err, cap)) return -1;
    const int w0 = im.w, h0 = im.h;
    if (!vis_target(im.w, im.h, &ow, &oh)) { free(im.rgb); im_fail(err, cap, "the image is too thin (one side more than 200 times the other)"); return -1; }
    const int nt = (ow / (g_vis.ps * g_vis.mg)) * (oh / (g_vis.ps * g_vis.mg));
    if (nt < 1 || nt > VIS_IDX) { free(im.rgb); im_fail(err, cap, "the image needs too many tokens"); return -1; }
    if (!vr_resize(&im, ow, oh)) { free(im.rgb); im_fail(err, cap, "out of memory for the image"); return -1; }
    if (g_vis.store) vis_store_put(hx, f, n);
    vis_lock();
    k = vis_find(hx);
    if (k < 0 && g_vis.n < VIS_MAX) {
        if (!g_vis.v) g_vis.v = xalloc(VIS_MAX, sizeof(VisImg));      /* tamanho fixo: os ponteiros nao mudam */
        VisImg *v = &g_vis.v[g_vis.n];
        memset(v, 0, sizeof *v);
        memcpy(v->hex, hx, 32);
        for (int i = 0; i < 32; i++) { unsigned b; sscanf(hx + 2 * i, "%2x", &b); v->full[i] = (uint8_t)b; }
        v->px = im; v->ow = w0; v->oh = h0; v->gw = ow / (g_vis.ps * g_vis.mg); v->gh = oh / (g_vis.ps * g_vis.mg); v->nt = nt;
        im.rgb = NULL;
        k = g_vis.n++;
    }
    vis_unlock();
    free(im.rgb);
    if (k < 0) im_fail(err, cap, "too many images in this process");
    return k;
}
static void vis_mark(int k, char *out, size_t cap) { snprintf(out, cap, VIS_TAG "%s|>", g_vis.v[k].hex); }
/* a ferramenta em curso produziu esta imagem */
static void vis_pend(int k) { if (g_vis.npend < 32) memcpy(g_vis.pend[g_vis.npend++], g_vis.v[k].hex, 33); }
static uint8_t *vis_slurp(const char *path, size_t *n, char *err, size_t cap) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) { snprintf(err, cap, "cannot read %.500s", path); return NULL; }
    if ((size_t)st.st_size > IM_MAX_FILE) { im_fail(err, cap, "image file is too large (limit 64 MB)"); return NULL; }
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, cap, "cannot open %.500s", path); return NULL; }
    uint8_t *b = malloc((size_t)st.st_size + 1);
    *n = b ? fread(b, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    if (!b) im_fail(err, cap, "out of memory for the image");
    return b;
}
static int vis_add_file(const char *path, char *err, size_t cap) {
    size_t n; uint8_t *b = vis_slurp(path, &n, err, cap);
    if (!b) return -1;
    const int k = vis_add(b, n, err, cap);
    free(b);
    return k;
}
/* base64 (texto com ou sem quebras, ate a aspa ou o fim) */
static int vis_b64v(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1; }
static int vis_add_b64(const char *s, size_t len, char *err, size_t cap) {
    if (len / 4 * 3 > IM_MAX_FILE) { im_fail(err, cap, "image file is too large (limit 64 MB)"); return -1; }
    uint8_t *b = malloc(len / 4 * 3 + 8); size_t n = 0; uint32_t acc = 0; int bits = 0, bad = 0;
    if (!b) { im_fail(err, cap, "out of memory for the image"); return -1; }
    for (size_t i = 0; i < len; i++) {
        const int c = (unsigned char)s[i], v = vis_b64v(c);
        if (c == '=') break;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        if (c == '\\' && i + 1 < len) { if (s[i + 1] == '/') continue; if (s[i + 1] == 'n' || s[i + 1] == 'r') { i++; continue; } }      /* JSON cru */
        if (v < 0) { bad = 1; break; }
        acc = acc << 6 | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; b[n++] = (uint8_t)(acc >> bits); }
    }
    int k = -1;
    if (bad) im_fail(err, cap, "the image data is not valid base64"); else k = vis_add(b, n, err, cap);
    free(b);
    return k;
}
/* linha que acompanha a imagem no resultado de uma ferramenta */
static void vis_note(int k, const char *name, char *out, size_t cap) {
    const VisImg *v = &g_vis.v[k];
    char mk[80]; vis_mark(k, mk, sizeof mk);
    if (v->px.w != v->ow || v->px.h != v->oh)
        snprintf(out, cap, "[image %.300s: %dx%d pixels, shown to you at %dx%d, %d tokens]\n%s", name, v->ow, v->oh, v->px.w, v->px.h, v->nt, mk);
    else snprintf(out, cap, "[image %.300s: %dx%d pixels, %d tokens]\n%s", name, v->ow, v->oh, v->nt, mk);
}

/* ---- tokenizador: marca em s -> ids; devolve quantos bytes a marca ocupa (0 = nao e marca) ---- */
static int vis_parse(const char *s, size_t n) {
    if (n < VIS_MARK_N || memcmp(s, VIS_TAG, VIS_TAG_N) || s[VIS_TAG_N + 32] != '|' || s[VIS_TAG_N + 33] != '>') return 0;
    for (int i = 0; i < 32; i++) { const char c = s[VIS_TAG_N + i]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0; }
    return 1;
}
static int vis_by_mark(const char *s) {
    const char *hex = s + VIS_TAG_N;
    vis_lock(); int k = vis_find(hex); vis_unlock();
    if (k < 0 && g_vis.store) {              /* sessao retomada: o arquivo guardado */
        char p[1100], err[200]; char h33[33];
        memcpy(h33, hex, 32); h33[32] = 0;
        vis_store_path(p, sizeof p, h33);
        size_t n; uint8_t *b = vis_slurp(p, &n, err, sizeof err);
        if (b) {
            char hx[65]; sha_hex(b, n, hx);
            if (!memcmp(hx, hex, 32)) k = vis_add(b, n, err, sizeof err);
            free(b);
        }
    }
    return k;
}
static int vis_encode_mark(const char *s, int n, Ids *out) {
    if (!vis_parse(s, (size_t)n)) return 0;
    const int k = vis_by_mark(s);
    if (k < 0) return VIS_MARK_N;            /* imagem que nao existe mais (sessao retomada): a marca some, a linha "[image CAMINHO ...]" antes dela fica como aviso */
    ids_push(out, g_vis.start);
    for (int i = 0; i < g_vis.v[k].nt; i++) ids_push(out, VIS_BASE | k << 16 | i);
    ids_push(out, g_vis.end);
    return VIS_MARK_N;
}
static int vis_tok(int id, int *idx) {
    if (id < VIS_BASE) return -1;
    const int k = (id - VIS_BASE) >> 16;
    *idx = id & 0xFFFF;
    return k < g_vis.n && *idx < g_vis.v[k].nt ? k : -1;
}
/* peso em bytes das imagens de um trecho, na moeda da compactacao (4 bytes por token) */
static size_t vis_weight(const char *a, const char *e) {
    size_t w = 0;
    if (!g_vis.on) return 0;
    for (const char *p = a; p && p < e && (p = strstr(p, VIS_TAG)) && p < e; p++)
        if (vis_parse(p, strlen(p))) { const int k = vis_by_mark(p); if (k >= 0) w += (size_t)g_vis.v[k].nt * 4; }
    return w;
}
/* a marca em p passa pelo defang? so a imagem que a propria ferramenta acabou de registrar */
static int vis_defang_ok(const char *p) {
    if (!g_vis.npend || !vis_parse(p, strlen(p))) return 0;
    for (int i = 0; i < g_vis.npend; i++) if (!memcmp(g_vis.pend[i], p + VIS_TAG_N, 32)) return 1;
    return 0;
}
/* identidade em disco dos ids de imagem: hash do arquivo, grade e projetor no lugar do lugar na tabela */
static uint64_t vis_ids_hash(uint64_t h, const int *ids, int n) {
    for (int i = 0; i < n; i++) {
        int x; const int k = ids[i] >= VIS_BASE ? vis_tok(ids[i], &x) : -1;
        if (k < 0) { h = pfx_hash(h, &ids[i], sizeof(int)); continue; }
        const VisImg *v = &g_vis.v[k];
        const int32_t g[3] = { v->gw, v->gh, x };
        if (x == 0) { h = pfx_hash(h, v->full, 32); h = pfx_hash(h, &g_vis.psize, sizeof g_vis.psize); }
        h = pfx_hash(h, g, sizeof g);
    }
    return h;
}

/* ---- vetores: calculados na primeira vez que o motor pede ---- */
static void vis_encode_image(VisImg *v) {
    char err[200] = "";
    const double t0 = now_sec();
    double r0, a0, r1, a1;
    vis_rss(&r0, &a0);
    if (!g_vis.vit) g_vis.vit = vit_load(g_vis.path, 0, err, sizeof err);
    int nt = 0;
    if (g_vis.vit && g_vis.vit->outd == g_vis.outd) v->emb = vit_encode(g_vis.vit, &v->px, &nt);
    vis_rss(&r1, &a1);
    if (v->emb && nt != v->nt) die("visao: numero de vetores inesperado");
    if (!v->emb) {                           /* projetor sumiu ou mudou depois da carga: a imagem vira zeros, com aviso */
        fprintf(stderr, "[vision: cannot encode the image (%s)]\n", err[0] ? err : "the projector changed");
        v->emb = vt_alloc((size_t)v->nt * g_vis.outd * 4);
        memset(v->emb, 0, (size_t)v->nt * g_vis.outd * 4);
    } else g_vis.nemb++;
    if (g_vis.vit && !(getenv("KATANA_VISION_KEEP") && atoi(getenv("KATANA_VISION_KEEP")))) { vit_free(g_vis.vit); g_vis.vit = NULL; }
    const double dt = now_sec() - t0;
    g_vis.t_enc += dt; g_vis.n_enc++; g_vis.tok_enc += v->nt;
    fprintf(stderr, "[vision: image %dx%d -> %dx%d, %d tokens, encoded in %.1f s (memory while encoding: %.0f MB, %.0f of them the projector file)]\n",
            v->ow, v->oh, v->px.w, v->px.h, v->nt, dt, r1 - r0, (r1 - r0) - (a1 - a0));
}
static void vis_row(const Model *m, float *out, int id) {
    int x; const int k = vis_tok(id, &x);
    if (k < 0) { memset(out, 0, (size_t)m->cfg.dim * 4); return; }      /* id que nao e imagem conhecida: nunca le fora da tabela */
    VisImg *v = &g_vis.v[k];
    if (!v->emb) {
        int live = 0, old = -1;
        for (int i = 0; i < g_vis.n; i++) if (g_vis.v[i].emb) { live++; if (old < 0 || g_vis.v[i].used < g_vis.v[old].used) old = i; }
        if (live >= VIS_EMB_KEEP && old >= 0) { vt_free(g_vis.v[old].emb); g_vis.v[old].emb = NULL; }
        vis_encode_image(v);
    }
    v->used = ++g_vis.tick;
    memcpy(out, v->emb + (size_t)x * g_vis.outd, (size_t)m->cfg.dim * 4);
}

/* ---- posicoes: chamada antes de cada passada com os ids que entram em pos0..pos0+n-1 ---- */
static void vis_seq(Model *m, const int *toks, int n, int pos0) {
    if (!vis_rp) {
        int any = 0;
        for (int i = 0; i < n && !any; i++) any = toks[i] >= VIS_BASE;
        if (!any) return;
        const int C = m->cfg.ctx;
        g_vis.nxt = xalloc((size_t)C, sizeof(int32_t)); g_vis.st = xalloc((size_t)C, sizeof(int32_t));
        int32_t *rp = xalloc((size_t)C * 3, sizeof(int32_t));
        for (int p = 0; p < C; p++) { rp[3 * p] = rp[3 * p + 1] = rp[3 * p + 2] = p; g_vis.nxt[p] = p + 1; }      /* o que ja esta no cache era texto */
        g_vis.ctx = C; vis_rp = rp;
    }
    for (int i = 0; i < n; i++) {
        const int p = pos0 + i;
        if (p < 0 || p >= g_vis.ctx) return;
        const int base = p ? g_vis.nxt[p - 1] : 0;
        int x; const int k = vis_tok(toks[i], &x);
        int32_t *r = vis_rp + 3 * (size_t)p;
        if (k < 0) { r[0] = r[1] = r[2] = base; g_vis.nxt[p] = base + 1; continue; }
        const VisImg *v = &g_vis.v[k];
        const int s = x == 0 || !p ? base : g_vis.st[p - 1];
        r[0] = s; r[1] = s + x / v->gw; r[2] = s + x % v->gw;
        g_vis.st[p] = s;
        g_vis.nxt[p] = x == v->nt - 1 ? s + (v->gw > v->gh ? v->gw : v->gh) : s;
    }
}

/* ---- ligar: projetor ao lado do modelo ----
   KATANA_MMPROJ=arquivo forca, 0 desliga. Procura na pasta do modelo um arquivo mmproj*.gguf cuja saida tem a largura do
   modelo e cujo nome casa com o dele: <modelo sem .gguf>.mmproj.gguf, ou general.basename do projetor dentro do nome do
   modelo (um projetor de outro modelo com a mesma largura nao serve). */
static int vis_basename_ok(const char *proj, const char *model) {
    VitRd r; size_t sz; const uint8_t *mp = vit_map(proj, &sz);
    int ok = 0;
    if (!mp) return 0;
    r.p = mp; r.end = mp + sz; r.bad = 0;
    const uint32_t magic = (uint32_t)vg_n(&r, 4); vg_n(&r, 4); vg_n(&r, 8);
    const uint64_t nk = vg_n(&r, 8);
    for (uint64_t i = 0; magic == 0x46554747 && i < nk && i < 100 && !r.bad; i++) {
        size_t kl, sl; const char *k = vg_str(&r, &kl);
        const uint32_t t = (uint32_t)vg_n(&r, 4);
        if (t == 8) {
            const char *s = vg_str(&r, &sl);
            if (vg_is(k, kl, "general.basename") && sl >= 4) for (const char *q = model; strlen(q) >= sl && !ok; q++) ok = ci_ncmp(q, s, (int)sl) == 0;
        } else if (t == 9) {
            const uint32_t et = (uint32_t)vg_n(&r, 4); const uint64_t n = vg_n(&r, 8);
            if (et == 8) for (uint64_t j = 0; j < n && !r.bad; j++) vg_str(&r, &sl);
            else { const int es = vg_size(et); if (es <= 0 || n > (uint64_t)(r.end - r.p) / (uint64_t)es) break; r.p += n * (uint64_t)es; }
        } else { const int es = vg_size(t); if (es <= 0) break; vg_n(&r, es > 8 ? 8 : es); }
    }
#ifndef _WIN32
    munmap((void *)mp, sz);
#endif
    return ok;
}
static void vis_init(Model *m, const char *model) {
    const Config *c = &m->cfg;
    const char *e = getenv("KATANA_MMPROJ");
    char path[1200] = "", err[200] = "";
    if (e && !strcmp(e, "0")) return;
    if (strcmp(c->arch, "qwen35moe")) { if (e && *e) fprintf(stderr, "[vision: off, the %s architecture has no vision support yet]\n", c->arch); return; }
    if (e && *e) snprintf(path, sizeof path, "%s", e);
    else {
#ifndef _WIN32
        char dir[1024]; const char *sl = strrchr(model, '/'), *base = sl ? sl + 1 : model;
        snprintf(dir, sizeof dir, "%.*s", sl ? (int)(sl - model) : 1, sl ? model : ".");
        size_t bl = strlen(base);
        if (bl > 5 && !strcmp(base + bl - 5, ".gguf")) bl -= 5;
        struct stat st;
        snprintf(path, sizeof path, "%s/%.*s.mmproj.gguf", dir, (int)bl, base);
        if (stat(path, &st) != 0) {
            path[0] = 0;
            DIR *d = opendir(dir); struct dirent *de;
            while (d && (de = readdir(d))) {
                const size_t l = strlen(de->d_name);
                if (strncmp(de->d_name, "mmproj", 6) || l < 11 || strcmp(de->d_name + l - 5, ".gguf")) continue;
                char p[1200]; snprintf(p, sizeof p, "%s/%.160s", dir, de->d_name);
                if (!vis_basename_ok(p, base)) continue;
                if (!path[0] || strcmp(p, path) < 0) snprintf(path, sizeof path, "%s", p);      /* escolha estavel */
            }
            if (d) closedir(d);
        }
#endif
    }
    if (!path[0]) return;
    Vit *v = vit_load(path, 1, err, sizeof err);
    if (!v) { fprintf(stderr, "[vision: off, %s: %s]\n", path, err); return; }
    g_vis.start = map_get(&m->tk.vocab, "<|vision_start|>", 16);
    g_vis.end = map_get(&m->tk.vocab, "<|vision_end|>", 14);
    if (v->outd != c->dim || g_vis.start < 0 || g_vis.end < 0) {
        fprintf(stderr, "[vision: off, %s does not fit this model (%d x %d)]\n", path, v->outd, c->dim);
        vit_free(v); return;
    }
    snprintf(g_vis.path, sizeof g_vis.path, "%s", path);
    g_vis.ps = v->ps; g_vis.mg = v->mg; g_vis.outd = v->outd; g_vis.psize = (int64_t)v->size;
    vit_free(v);
    const char *b = getenv("KATANA_IMAGE_TOKENS");
    g_vis.budget = b && atoi(b) > 0 ? atoi(b) : 1000;
    if (g_vis.budget > 16384) g_vis.budget = 16384;
    g_vis.on = 1; g_img_mark = vis_encode_mark;
    fprintf(stderr, "[vision: %s, up to %d tokens per image (KATANA_IMAGE_TOKENS)]\n", path, g_vis.budget);
}
/* imagens pedidas na linha de comando (--image, uma por linha em KATANA_IMAGE): marcas para a proxima mensagem */
static int vis_cli_images(void) {
    const char *e = getenv("KATANA_IMAGE");
    if (!e || !*e) return 1;
    char *l = strdup(e), *sv = l;
    int ok = 1;
    for (char *p = l, *q; p && *p; p = q) {
        q = strchr(p, '\n');
        if (q) *q++ = 0;
        char err[700] = "";
        const int k = g_vis.on ? vis_add_file(p, err, sizeof err) : -1;
        if (k < 0) { fprintf(stderr, "katana: --image %s: %s\n", p, g_vis.on ? err : "this model has no vision (no projector file next to it)"); ok = 0; continue; }
        char mk[80]; vis_mark(k, mk, sizeof mk);
        buf_str(&g_vis.next, mk);
        fprintf(stderr, "[image %s: %dx%d, %d tokens]\n", p, g_vis.v[k].ow, g_vis.v[k].oh, g_vis.v[k].nt);
    }
    free(sv);
    unsetenv("KATANA_IMAGE");
    return ok;
}
/* @image(CAMINHO) em qualquer mensagem do usuario: troca cada uso por "[image CAMINHO: LxA pixels, N tokens]" e a marca da imagem
   (o caminho fica no texto, a sessao guarda o caminho e o hash; os pixels ficam no armazenamento de imagens). Erro (arquivo ausente,
   formato nao suportado, sem projetor) vira uma linha "[image CAMINHO not attached: motivo]" no texto e na saida de erro; nunca aborta.
   Caminho absoluto, relativo a pasta de trabalho ou com ~/. Devolve quantas imagens entraram. */
static int vis_user_images(char *line, size_t cap) {
    static const char T[] = "@image(";
    if (!strstr(line, T)) return 0;
    char *o = malloc(cap + 1);
    if (!o) return 0;
    size_t w = 0; int n = 0;
    for (const char *p = line; *p; ) {
        const char *q = strncmp(p, T, 7) == 0 ? strchr(p + 7, ')') : NULL;
        if (!q) { if (w + 1 >= cap) break; o[w++] = *p++; continue; }
        char path[1100], err[700] = "", note[1500];
        const char *a = p + 7, *e = q;
        while (a < e && (*a == ' ' || *a == '\t')) a++;
        while (e > a && (e[-1] == ' ' || e[-1] == '\t')) e--;
        if (e - a >= 2 && (*a == '"' || *a == '\'') && e[-1] == *a) { a++; e--; }
        if (a == e) { if (w + 1 >= cap) break; o[w++] = *p++; continue; }      /* @image() vazio: fica como texto */
        const char *hm = getenv("HOME");
        if (*a == '~' && (a + 1 == e || a[1] == '/') && hm) snprintf(path, sizeof path, "%s%.*s", hm, (int)(e - a - 1), a + 1);
        else snprintf(path, sizeof path, "%.*s", (int)(e - a), a);
        int k = -1;
        if (!g_vis.on) snprintf(err, sizeof err, "this model has no vision loaded (no projector file next to it)");
        else k = vis_add_file(path, err, sizeof err);
        if (k < 0) {
            snprintf(note, sizeof note, "[image %.600s not attached: %.300s]", path, err);
            fprintf(stderr, "[image %s not attached: %s]\n", path, err);
        } else {
            char ab[4200];
            vis_note(k, path, note, sizeof note);
#ifndef _WIN32
            if (realpath(path, ab)) vis_note(k, ab, note, sizeof note);
#endif
            fprintf(stderr, "[image %s: %dx%d, %d tokens]\n", path, g_vis.v[k].ow, g_vis.v[k].oh, g_vis.v[k].nt);
            n++;
        }
        const size_t nl = strlen(note);
        if (w + nl + 1 >= cap) break;
        memcpy(o + w, note, nl); w += nl;
        p = q + 1;
    }
    o[w] = 0;
    snprintf(line, cap, "%s", o);
    free(o);
    return n;
}
/* poe as marcas pendentes na frente da mensagem do usuario */
static void vis_take(char *line, size_t cap) {
    if (!g_vis.next.n) return;
    const size_t l = strlen(line);
    if (g_vis.next.n + 1 + l + 1 < cap) { memmove(line + g_vis.next.n, line, l + 1); memcpy(line, g_vis.next.p, g_vis.next.n); }
    buf_clear(&g_vis.next);
}
