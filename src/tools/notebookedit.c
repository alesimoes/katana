/* ---------- ferramenta NotebookEdit (nomes e argumentos do Claude Code; ver docs/claude-tools.md) ----------
 * O .ipynb e JSON. Nao se reserializa o caderno: o texto original fica byte a byte e so a celula tocada (ou a
 * insercao/remocao) e emendada, com o mesmo recuo do arquivo. Tambem aqui: ct_nb_render, que Read usa para mostrar as celulas. */
typedef struct { size_t s, e; } CtSpan;
typedef struct { size_t s, e; char *t; } CtSplice;

/* valor da chave key no objeto que comeca em obj (aponta a '{'); ks (opcional) recebe o inicio da chave. 1 se achou */
static int ct_jf(const char *obj, const char *end, const char *key, const char **vs, const char **ve, const char **ks) {
    const char *p = obj;
    if (p >= end || *p != '{') return 0;
    const size_t kl = strlen(key);
    p++;
    for (;;) {
        while (p < end && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= end || *p != '"') return 0;
        const char *k0 = p, *ke = ct_skip(p, end);
        if (!ke) return 0;
        p = ke;
        while (p < end && (isspace((unsigned char)*p) || *p == ':')) p++;
        const char *v = p, *e = ct_skip(p, end);
        if (!e) return 0;
        if ((size_t)(ke - 1 - (k0 + 1)) == kl && memcmp(k0 + 1, key, kl) == 0) { *vs = v; *ve = e; if (ks) *ks = k0; return 1; }
        p = e;
    }
}
/* texto de um valor JSON: texto, ou lista de textos (campo source e text do nbformat), concatenado */
static void ct_nb_text(const char *vs, const char *ve, CtBuf *o) {
    if (vs >= ve) return;
    if (*vs == '"') { size_t l; char *t = ct_unquote(vs, ve, &l); ct_buf_add(o, t, l); free(t); return; }
    if (*vs != '[') return;
    const char *p = vs + 1;
    for (;;) {
        while (p < ve && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= ve || *p == ']') return;
        const char *e = ct_skip(p, ve);
        if (!e) return;
        if (*p == '"') { size_t l; char *t = ct_unquote(p, e, &l); ct_buf_add(o, t, l); free(t); }
        p = e;
    }
}
/* as celulas do caderno: cells recebe os trechos (memoria nova), *arr_s o '[' e *arr_e o ']' da lista; 1 se achou */
static int ct_nb_cells(const char *b, size_t n, CtSpan **cells, int *nc, size_t *arr_s, size_t *arr_e, const char **cks, char *err, size_t ecap) {
    const char *end = b + n, *p = b;
    while (p < end && isspace((unsigned char)*p)) p++;
    const char *vs, *ve;
    if (p >= end || *p != '{' || !ct_jf(p, end, "cells", &vs, &ve, cks) || *vs != '[') { snprintf(err, ecap, "error: this is not a valid notebook (.ipynb): no \"cells\" list found at the top level"); return 0; }
    int cap = 16, c = 0;
    CtSpan *a = (CtSpan *)malloc(sizeof(CtSpan) * (size_t)cap);
    const char *q = vs + 1;
    for (;;) {
        while (q < ve && (isspace((unsigned char)*q) || *q == ',')) q++;
        if (q >= ve || *q == ']') break;
        const char *e = ct_skip(q, ve);
        if (!e || *q != '{') { free(a); snprintf(err, ecap, "error: the notebook JSON is malformed inside \"cells\""); return 0; }
        if (c == cap) { cap *= 2; a = (CtSpan *)realloc(a, sizeof(CtSpan) * (size_t)cap); }
        a[c].s = (size_t)(q - b); a[c].e = (size_t)(e - b); c++;
        q = e;
    }
    *cells = a; *nc = c; *arr_s = (size_t)(vs - b); *arr_e = (size_t)(ve - 1 - b);
    return 1;
}
/* id da celula; sem o campo id vale cell-N (N de zero) */
static void ct_nb_id(const char *b, const CtSpan *c, int idx, char *id, size_t cap, int *has) {
    const char *vs, *ve;
    *has = 0;
    if (ct_jf(b + c->s, b + c->e, "id", &vs, &ve, NULL) && *vs == '"') {
        size_t l; char *t = ct_unquote(vs, ve, &l);
        snprintf(id, cap, "%s", t); free(t); *has = 1;
        return;
    }
    snprintf(id, cap, "cell-%d", idx);
}
static void ct_nb_type(const char *b, const CtSpan *c, char *ty, size_t cap) {
    const char *vs, *ve;
    snprintf(ty, cap, "code");
    if (ct_jf(b + c->s, b + c->e, "cell_type", &vs, &ve, NULL) && *vs == '"') { size_t l; char *t = ct_unquote(vs, ve, &l); snprintf(ty, cap, "%s", t); free(t); }
}

/* caderno como texto para o Read: id, tipo, fonte e saidas de texto de cada celula */
static int ct_nb_render(const char *b, size_t n, CtBuf *o, char *err, size_t ecap) {
    CtSpan *cs; int nc; size_t as, ae; const char *cks;
    if (!ct_nb_cells(b, n, &cs, &nc, &as, &ae, &cks, err, ecap)) return 0;
    char h[96], id[300], ty[40];
    snprintf(h, sizeof h, "Jupyter notebook with %d cell%s. Edit it with NotebookEdit, using the cell ids below.\n", nc, nc == 1 ? "" : "s");
    ct_buf_str(o, h);
    for (int i = 0; i < nc; i++) {
        int has;
        ct_nb_id(b, &cs[i], i, id, sizeof id, &has);
        ct_nb_type(b, &cs[i], ty, sizeof ty);
        const char *c0 = b + cs[i].s, *c1 = b + cs[i].e, *vs, *ve;
        snprintf(h, sizeof h, "\n<cell %d id=\"", i); ct_buf_str(o, h);
        ct_buf_str(o, id);
        ct_buf_str(o, "\" type=\""); ct_buf_str(o, ty); ct_buf_str(o, "\">\n");
        if (ct_jf(c0, c1, "source", &vs, &ve, NULL)) {
            CtBuf s = {0}; ct_nb_text(vs, ve, &s);
            if (s.n) { ct_buf_add(o, s.p, s.n); if (s.p[s.n - 1] != '\n') ct_buf_str(o, "\n"); }
            free(s.p);
        }
        if (ct_jf(c0, c1, "outputs", &vs, &ve, NULL) && *vs == '[') {
            const char *p = vs + 1;
            for (;;) {
                while (p < ve && (isspace((unsigned char)*p) || *p == ',')) p++;
                if (p >= ve || *p == ']') break;
                const char *e = ct_skip(p, ve);
                if (!e) break;
                const char *xs, *xe;
                CtBuf t = {0};
                char ot[40] = "";
                if (ct_jf(p, e, "output_type", &xs, &xe, NULL) && *xs == '"') { size_t l; char *q = ct_unquote(xs, xe, &l); snprintf(ot, sizeof ot, "%s", q); free(q); }
                if (strcmp(ot, "stream") == 0) { if (ct_jf(p, e, "text", &xs, &xe, NULL)) ct_nb_text(xs, xe, &t); }
                else if (strcmp(ot, "error") == 0) {
                    CtBuf a = {0}, v = {0};
                    if (ct_jf(p, e, "ename", &xs, &xe, NULL)) ct_nb_text(xs, xe, &a);
                    if (ct_jf(p, e, "evalue", &xs, &xe, NULL)) ct_nb_text(xs, xe, &v);
                    ct_buf_str(&t, a.p ? a.p : "error"); ct_buf_str(&t, ": "); ct_buf_str(&t, v.p ? v.p : "");
                    free(a.p); free(v.p);
                } else if (ct_jf(p, e, "data", &xs, &xe, NULL) && *xs == '{') {
                    const char *ys, *ye;
                    if (ct_jf(xs, xe, "text/plain", &ys, &ye, NULL)) ct_nb_text(ys, ye, &t);
                    if (ct_jf(xs, xe, "image/png", &ys, &ye, NULL) || ct_jf(xs, xe, "image/jpeg", &ys, &ye, NULL)) ct_buf_str(&t, "\n[an image output is omitted: this model cannot look at images]");
                }
                if (t.n) {
                    ct_buf_str(o, "[output]\n"); ct_buf_add(o, t.p, t.n);
                    if (t.p[t.n - 1] != '\n') ct_buf_str(o, "\n");
                }
                free(t.p);
                p = e;
            }
        }
        ct_buf_str(o, "</cell>\n");
    }
    free(cs);
    return 1;
}

/* ---- edicao ---- */
static int ct_nb_col(const char *b, const char *p) {          /* coluna de p se so ha espacos antes dele na linha; senao -1 */
    int c = 0;
    while (p > b && p[-1] != '\n') { p--; if (*p != ' ' && *p != '\t') return -1; c++; }
    return c;
}
static void ct_nb_esc(CtBuf *o, const char *s, size_t l) {
    ct_buf_str(o, "\"");
    for (size_t i = 0; i < l; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '"') ct_buf_str(o, "\\\"");
        else if (c == '\\') ct_buf_str(o, "\\\\");
        else if (c == '\n') ct_buf_str(o, "\\n");
        else if (c == '\r') ct_buf_str(o, "\\r");
        else if (c == '\t') ct_buf_str(o, "\\t");
        else if (c < 0x20) { char u[8]; snprintf(u, sizeof u, "\\u%04x", c); ct_buf_str(o, u); }
        else ct_buf_add(o, s + i, 1);
    }
    ct_buf_str(o, "\"");
}
static void ct_nb_sp(CtBuf *o, int k) { for (int i = 0; i < k; i++) ct_buf_str(o, " "); }
/* lista de linhas do nbformat (cada linha guarda o seu \n); ind e a coluna dos itens, ou -1 para uma linha so */
static void ct_nb_src(CtBuf *o, const char *s, int ind, int close_ind) {
    const size_t n = strlen(s);
    if (!n) { ct_buf_str(o, "[]"); return; }
    ct_buf_str(o, "[");
    size_t i = 0; int first = 1;
    while (i < n) {
        size_t j = i;
        while (j < n && s[j] != '\n') j++;
        if (j < n) j++;
        if (!first) ct_buf_str(o, ind >= 0 ? "," : ", ");
        if (ind >= 0) { ct_buf_str(o, "\n"); ct_nb_sp(o, ind); }
        ct_nb_esc(o, s + i, j - i);
        first = 0; i = j;
    }
    if (ind >= 0) { ct_buf_str(o, "\n"); ct_nb_sp(o, close_ind); }
    ct_buf_str(o, "]");
}
/* celula nova (ci: coluna do "{", -1 = tudo numa linha; u: largura do recuo) */
static void ct_nb_build(CtBuf *o, const char *type, const char *id, const char *meta, const char *src, int ci, int u) {
    const int code = strcmp(type, "code") == 0;
    const char *nl = ci >= 0 ? "\n" : "";
    const int fi = ci >= 0 ? ci + u : 0, si = ci >= 0 ? ci + 2 * u : -1;
    const char *sep = ci >= 0 ? ",\n" : ", ";
    ct_buf_str(o, "{"); ct_buf_str(o, nl);
    if (ci >= 0) ct_nb_sp(o, fi);
    ct_buf_str(o, "\"cell_type\": "); ct_nb_esc(o, type, strlen(type));
    if (code) { ct_buf_str(o, sep); if (ci >= 0) ct_nb_sp(o, fi); ct_buf_str(o, "\"execution_count\": null"); }
    if (id) { ct_buf_str(o, sep); if (ci >= 0) ct_nb_sp(o, fi); ct_buf_str(o, "\"id\": "); ct_nb_esc(o, id, strlen(id)); }
    ct_buf_str(o, sep); if (ci >= 0) ct_nb_sp(o, fi);
    ct_buf_str(o, "\"metadata\": "); ct_buf_str(o, meta && *meta ? meta : "{}");
    if (code) { ct_buf_str(o, sep); if (ci >= 0) ct_nb_sp(o, fi); ct_buf_str(o, "\"outputs\": []"); }
    ct_buf_str(o, sep); if (ci >= 0) ct_nb_sp(o, fi);
    ct_buf_str(o, "\"source\": "); ct_nb_src(o, src, si, fi);
    ct_buf_str(o, nl); if (ci >= 0) ct_nb_sp(o, ci);
    ct_buf_str(o, "}");
}
static int ct_nb_splice_cmp(const void *a, const void *b) { const CtSplice *x = (const CtSplice *)a, *y = (const CtSplice *)b; return x->s < y->s ? -1 : x->s > y->s; }
/* aplica as emendas (sem sobrepor) ao texto original */
static char *ct_nb_apply(const char *b, size_t n, CtSplice *sp, int ns, size_t *outn) {
    qsort(sp, (size_t)ns, sizeof *sp, ct_nb_splice_cmp);
    CtBuf o = {0};
    size_t pos = 0;
    for (int i = 0; i < ns; i++) { ct_buf_add(&o, b + pos, sp[i].s - pos); ct_buf_str(&o, sp[i].t); pos = sp[i].e; }
    ct_buf_add(&o, b + pos, n - pos);
    *outn = o.n;
    return o.p;
}

static const char CT_NOTEBOOKEDIT_DESC[] =
    "Edits a cell of a Jupyter notebook (.ipynb). notebook_path should be an absolute path. edit_mode is replace (default: puts new_source in the cell; "
    "a code cell also gets its outputs and execution count cleared), insert (adds a new cell after the cell with cell_id, or at the start when cell_id is "
    "left out; cell_type is required) or delete (removes the cell). Cells are named by their id; notebooks whose cells have no id field use cell-N, where N is "
    "the zero-based position of the cell (Read on the notebook shows the ids). Only the touched cell changes: the rest of the file stays byte for byte as it was.";
static const char CT_NOTEBOOKEDIT_PROPS[] =
    "\"notebook_path\": {\"type\": \"string\", \"description\": \"The absolute path of the .ipynb file\"}, "
    "\"cell_id\": {\"type\": \"string\", \"description\": \"The id of the cell to edit (or cell-N, N counted from zero, when the cell has no id). For insert, the new cell goes after it\"}, "
    "\"new_source\": {\"type\": \"string\", \"description\": \"The new source of the cell\"}, "
    "\"cell_type\": {\"type\": \"string\", \"enum\": [\"code\", \"markdown\"], \"description\": \"The type of the cell. Required for insert; for replace it changes the type\"}, "
    "\"edit_mode\": {\"type\": \"string\", \"enum\": [\"replace\", \"insert\", \"delete\"], \"description\": \"replace (default), insert or delete\"}";
static const char CT_NOTEBOOKEDIT_REQ[] = "\"notebook_path\", \"new_source\"";

static void ct_notebookedit(const char *js, const char *end, char *out, size_t cap) {
    static char abs[4300], err[1400];
    char *fp = ct_str(js, end, "notebook_path"), *cid = ct_str(js, end, "cell_id"), *src = ct_str(js, end, "new_source");
    char *ctype = ct_str(js, end, "cell_type"), *mode = ct_str(js, end, "edit_mode"), *b = NULL, *nb = NULL;
    CtSpan *cs = NULL;
    CtSplice sp[8]; int ns = 0;
    do {
        if (!fp) { snprintf(out, cap, "error: missing notebook_path"); break; }
        const char *m = mode && *mode ? mode : "replace";
        const int rep = strcmp(m, "replace") == 0, ins = strcmp(m, "insert") == 0, del = strcmp(m, "delete") == 0;
        if (!rep && !ins && !del) { snprintf(out, cap, "error: edit_mode must be replace, insert or delete (got \"%s\")", m); break; }
        if (ctype && *ctype && strcmp(ctype, "code") != 0 && strcmp(ctype, "markdown") != 0 && strcmp(ctype, "raw") != 0) { snprintf(out, cap, "error: cell_type must be code or markdown (got \"%s\")", ctype); break; }
        if (ctype && !*ctype) { free(ctype); ctype = NULL; }
        if (cid && !*cid) { free(cid); cid = NULL; }
        if (!ct_ext_is(fp, ".ipynb")) { snprintf(out, cap, "error: %s is not a notebook (.ipynb); use Edit for other files", fp); break; }
        if (!del && !src) { snprintf(out, cap, "error: missing new_source (required for %s)", m); break; }
        if (ins && !ctype) { snprintf(out, cap, "error: cell_type (code or markdown) is required when edit_mode is insert"); break; }
        if (!ins && !cid) { snprintf(out, cap, "error: cell_id is required for %s; Read the notebook to see the ids (cell-N for cells without one)", m); break; }
        if (!ct_f_abs(fp, abs, sizeof abs, err, sizeof err)) { snprintf(out, cap, "%s", err); break; }
        struct stat st;
        if (stat(abs, &st) != 0) { snprintf(out, cap, "error: %s does not exist", abs); break; }
        if (S_ISDIR(st.st_mode)) { snprintf(out, cap, "error: %s is a directory", abs); break; }
        size_t n = 0;
        b = ct_f_slurp(abs, &n, err, sizeof err);
        if (!b) { snprintf(out, cap, "%s", err); break; }
        int nc; size_t as, ae; const char *cks;
        if (!ct_nb_cells(b, n, &cs, &nc, &as, &ae, &cks, err, sizeof err)) { snprintf(out, cap, "%s", err); break; }
        /* qual celula */
        int idx = -1;
        char id[300], ty[40];
        if (cid) {
            for (int i = 0; i < nc && idx < 0; i++) { int has; ct_nb_id(b, &cs[i], i, id, sizeof id, &has); if (strcmp(id, cid) == 0) idx = i; }
            if (idx < 0) {
                size_t w = (size_t)snprintf(out, cap, "error: no cell with id \"%s\" in %s (%d cells). Ids:", cid, abs, nc);
                for (int i = 0; i < nc && i < 20 && w + 320 < cap; i++) { int has; ct_nb_id(b, &cs[i], i, id, sizeof id, &has); w += (size_t)snprintf(out + w, cap - w, " %s", id); }
                if (nc > 20 && w + 8 < cap) snprintf(out + w, cap - w, " ...");
                break;
            }
        }
        /* recuo do arquivo: coluna da chave "cells" e da primeira celula */
        const int kc = ct_nb_col(b, cks), c0 = nc ? ct_nb_col(b, b + cs[0].s) : -1;
        const int u = (kc >= 0 && c0 > kc) ? c0 - kc : (kc >= 0 ? 1 : 1);
        char msg[600];
        if (rep) {
            ct_nb_id(b, &cs[idx], idx, id, sizeof id, &(int){0});
            ct_nb_type(b, &cs[idx], ty, sizeof ty);
            const char *cs0 = b + cs[idx].s, *ce0 = b + cs[idx].e, *vs, *ve, *ks;
            if (ctype && strcmp(ctype, ty) != 0) {          /* muda o tipo: refaz a celula com o mesmo id e os mesmos metadados */
                CtBuf c = {0};
                int has; char idn[300]; ct_nb_id(b, &cs[idx], idx, idn, sizeof idn, &has);
                const char *ms = NULL, *me = NULL; char *meta = NULL;
                if (ct_jf(cs0, ce0, "metadata", &ms, &me, NULL)) { meta = (char *)malloc((size_t)(me - ms) + 1); memcpy(meta, ms, (size_t)(me - ms)); meta[me - ms] = 0; }
                ct_nb_build(&c, ctype, has ? idn : NULL, meta, src, c0 >= 0 ? ct_nb_col(b, cs0) : -1, u);
                free(meta);
                sp[ns++] = (CtSplice){ cs[idx].s, cs[idx].e, c.p };
                snprintf(msg, sizeof msg, "Replaced the source of cell %s and changed its type from %s to %s", id, ty, ctype);
            } else {
                if (!ct_jf(cs0, ce0, "source", &vs, &ve, &ks)) { snprintf(out, cap, "error: cell %s has no source field", id); break; }
                CtBuf c = {0};
                const int kcol = ct_nb_col(b, ks);
                ct_nb_src(&c, src, kcol >= 0 ? kcol + u : -1, kcol >= 0 ? kcol : 0);
                sp[ns++] = (CtSplice){ (size_t)(vs - b), (size_t)(ve - b), c.p };
                if (strcmp(ty, "code") == 0) {
                    if (ct_jf(cs0, ce0, "outputs", &vs, &ve, NULL)) { char *t = strdup("[]"); sp[ns++] = (CtSplice){ (size_t)(vs - b), (size_t)(ve - b), t }; }
                    if (ct_jf(cs0, ce0, "execution_count", &vs, &ve, NULL)) { char *t = strdup("null"); sp[ns++] = (CtSplice){ (size_t)(vs - b), (size_t)(ve - b), t }; }
                }
                snprintf(msg, sizeof msg, "Replaced the source of cell %s (%s)%s", id, ty, strcmp(ty, "code") == 0 ? "; its outputs and execution count were cleared" : "");
            }
        } else if (del) {
            ct_nb_id(b, &cs[idx], idx, id, sizeof id, &(int){0});
            if (nc == 1) sp[ns++] = (CtSplice){ as + 1, ae, strdup("") };
            else if (idx + 1 < nc) sp[ns++] = (CtSplice){ cs[idx].s, cs[idx + 1].s, strdup("") };
            else sp[ns++] = (CtSplice){ cs[idx - 1].e, cs[idx].e, strdup("") };
            snprintf(msg, sizeof msg, "Deleted cell %s", id);
        } else {                                              /* insert */
            int ids = 0, minor = 0;
            for (int i = 0; i < nc && !ids; i++) { int has; ct_nb_id(b, &cs[i], i, id, sizeof id, &has); ids = has; }
            {   const char *p = b; while (p < b + n && isspace((unsigned char)*p)) p++;
                const char *vs, *ve;
                if (ct_jf(p, b + n, "nbformat_minor", &vs, &ve, NULL)) minor = atoi(vs);
                if (minor >= 5) ids = 1;
            }
            char nid[16] = "";
            if (ids) {
                static unsigned long long rs;
                if (!rs) rs = (unsigned long long)time(NULL) * 2654435761ull ^ (unsigned long long)getpid();
                for (;;) {
                    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
                    snprintf(nid, sizeof nid, "%08x", (unsigned)(rs >> 16));
                    int clash = 0;
                    for (int i = 0; i < nc; i++) { int has; ct_nb_id(b, &cs[i], i, id, sizeof id, &has); if (strcmp(id, nid) == 0) clash = 1; }
                    if (!clash) break;
                }
            }
            CtBuf c = {0};
            const int ci = nc ? c0 : (kc >= 0 ? kc + u : -1);
            ct_nb_build(&c, ctype, ids ? nid : NULL, NULL, src, ci, u);
            char *t;
            if (nc == 0) {
                CtBuf w = {0};
                if (ci >= 0) { ct_buf_str(&w, "\n"); ct_nb_sp(&w, ci); ct_buf_add(&w, c.p, c.n); ct_buf_str(&w, "\n"); ct_nb_sp(&w, kc); } else ct_buf_add(&w, c.p, c.n);
                free(c.p);
                sp[ns++] = (CtSplice){ as + 1, ae, w.p };
            } else {
                CtBuf w = {0};
                const char *sepb; size_t sepn;
                if (nc >= 2) { sepb = b + cs[0].e; sepn = cs[1].s - cs[0].e; }
                else { sepb = ci >= 0 ? ",\n" : ", "; sepn = strlen(sepb); }
                char *sepc = (char *)malloc(sepn + (nc >= 2 ? 0 : (size_t)(ci > 0 ? ci : 0)) + 1);
                memcpy(sepc, sepb, sepn);
                size_t sl = sepn;
                if (nc < 2 && ci > 0) { memset(sepc + sl, ' ', (size_t)ci); sl += (size_t)ci; }
                sepc[sl] = 0;
                if (!cid) { ct_buf_add(&w, c.p, c.n); ct_buf_add(&w, sepc, sl); sp[ns++] = (CtSplice){ cs[0].s, cs[0].s, w.p }; }
                else { ct_buf_add(&w, sepc, sl); ct_buf_add(&w, c.p, c.n); sp[ns++] = (CtSplice){ cs[idx].e, cs[idx].e, w.p }; }
                free(sepc); free(c.p);
            }
            t = NULL; (void)t;
            snprintf(msg, sizeof msg, "Inserted a new %s cell (id %s) %s", ctype, ids ? nid : "none; address it as cell-N by its position", cid ? "after the given cell" : "at the start");
        }
        size_t nn = 0;
        nb = ct_nb_apply(b, n, sp, ns, &nn);
        /* confere que o resultado ainda e um caderno com o numero certo de celulas */
        CtSpan *c2 = NULL; int nc2 = 0; size_t a2, e2; const char *k2;
        if (!ct_nb_cells(nb, nn, &c2, &nc2, &a2, &e2, &k2, err, sizeof err) || nc2 != nc + (ins ? 1 : del ? -1 : 0)) {
            free(c2);
            snprintf(out, cap, "error: the edit would have produced an invalid notebook; nothing was written. Check the notebook with Read.");
            break;
        }
        free(c2);
        char what[4400];
        snprintf(what, sizeof what, "editar o caderno %s (%s)", abs, m);
        if (!agent_act(what, src ? src : "")) { snprintf(out, cap, "error: the user denied this action"); break; }
        if (!ct_f_save(abs, nb, nn, out, cap)) break;
        snprintf(out, cap, "%s in %s. The notebook now has %d cell%s. Only that cell was touched; the rest of the file is unchanged.", msg, abs, nc2, nc2 == 1 ? "" : "s");
    } while (0);
    for (int i = 0; i < ns; i++) free(sp[i].t);
    free(cs); free(fp); free(cid); free(src); free(ctype); free(mode); free(b); free(nb);
}
