/* ---------- uioe/conn_file.c: os conectores de arquivos e documentos (docs/uioe.md 3.8.2, fase U3) ----------
 *   file.grep      uma palavra ou frase LITERAL nas linhas dos arquivos da raiz (sem indice: o degrau 5 da escada)
 *   file.glob      arquivos pelo padrao do nome (os mesmos padroes da ferramenta Glob: ct_gmatch)
 *   place.resolve  a pasta citada que o Interpretar ja achou existe e e legivel
 *   docs.search    linhas dos documentos do projeto (README, docs/, KATANA.md, AGENTS.md...) que casam com os termos
 *   docs.select    o seletor do docsel (agent/docsel.c); precisa do tokenizador do modelo, so existe dentro do agente
 * Cada um preenche o UioeResult e nunca imprime. Arquivos ignorados: os de `sf_skip_dir` (node_modules, build...), pastas que comecam por
 * ponto, binarios (um NUL nos primeiros 512 bytes), mais de 1 MB, e os que parecem segredo (.env, chaves, certificados): o nome do arquivo
 * pode aparecer, o texto nunca. A varredura de arquivos nao respeita .gitignore (a ferramenta Grep usa o git quando pode; aqui nao ha
 * processo filho): o limite diz isso.
 * Decisoes do implementador: sem indice a varredura para em 3000 arquivos ou no prazo do conector; o resultado e UR_PARTIAL e o limite
 * diz quantos arquivos foram vistos. docs.search pontua cada linha por IDF dos termos (termos em mais de 30% das linhas sao ignorados,
 * como o docsel) e por prefixo comum de pelo menos 4 letras ("instalo" casa "install" e "instalar"). */
#define UF_MAXFILES 3000
#define UF_MAXSIZE (1 << 20)
typedef struct { char **p; int n, cap, visited, full; double t_end; } UfList;
static void uf_free(UfList *L) { for (int i = 0; i < L->n; i++) free(L->p[i]); free(L->p); memset(L, 0, sizeof *L); }
/* caminho que parece segredo: o texto nunca entra na saida */
static int uioe_secret_path(const char *rel) {
    const char *b = strrchr(rel, '/'); b = b ? b + 1 : rel;
    static const char *const nm[] = {".env", "id_rsa", "id_ed25519", "credentials", "secrets", "secret", ".netrc", ".npmrc", ".pypirc", NULL};
    for (int i = 0; nm[i]; i++) if (!strncasecmp(b, nm[i], strlen(nm[i]))) return 1;
    static const char *const ex[] = {".pem", ".key", ".p12", ".pfx", ".crt", ".cer", ".jks", ".keystore", NULL};
    const size_t bl = strlen(b);
    for (int i = 0; ex[i]; i++) { const size_t el = strlen(ex[i]); if (bl > el && !strcasecmp(b + bl - el, ex[i])) return 1; }
    if (strcasestr(b, ".env.") && !strcasestr(b, ".example") && !strcasestr(b, ".sample") && !strcasestr(b, ".template")) return 1;
    return 0;
}
/* mascara um valor depois de nome de segredo (token=..., password: ...) e prefixos conhecidos de chave, numa linha */
static void uioe_mask_line(char *s) {
    static const char *const kw[] = {"api_key", "apikey", "api-key", "authorization", "password", "passwd", "secret", "token", "senha", NULL};
    for (int k = 0; kw[k]; k++) {
        for (char *p = s; (p = strcasestr(p, kw[k])); p += strlen(kw[k])) {
            char *q = p + strlen(kw[k]); while (*q == '"' || *q == '\'' || *q == ' ') q++;
            if (*q != '=' && *q != ':') continue;
            q++; while (*q == ' ' || *q == '"' || *q == '\'') q++;
            size_t l = 0; while (q[l] && q[l] != ' ' && q[l] != '"' && q[l] != '\'' && q[l] != ',' && q[l] != ';' && q[l] != ')') l++;
            if (l >= 8) { memcpy(q, "[masked]", 8); if (l > 8) memmove(q + 8, q + l, strlen(q + l) + 1); }
        }
    }
    static const char *const pf[] = {"sk-", "ghp_", "gho_", "github_pat_", "AKIA", "xoxb-", "xoxp-", NULL};
    for (int k = 0; pf[k]; k++) {
        char *p = strstr(s, pf[k]); if (!p) continue;
        const size_t pl = strlen(pf[k]); size_t l = 0; while (isalnum((unsigned char)p[pl + l]) || p[pl + l] == '_' || p[pl + l] == '-') l++;
        if (l >= 16) { memcpy(p, "[masked]", 8); memmove(p + 8, p + pl + l, strlen(p + pl + l) + 1); }
    }
}
#ifndef _WIN32
static int uf_text_ext(const char *name) {
    const char *dot = strrchr(name, '.'); if (!dot || !dot[1]) return 1;
    static const char *const bin[] = {"png", "jpg", "jpeg", "gif", "ico", "webp", "pdf", "zip", "gz", "tar", "xz", "bz2", "7z", "rar", "exe", "dll", "so", "dylib", "o", "a", "class", "jar", "gguf", "bin", "pyc", "woff", "woff2", "ttf", "otf", "mp3", "mp4", "wav", "ogg", "mov", "avi", "svg", "lock", "apk", "obj", "pdb", "ilk", "lib", "exp", "res", "db", "sqlite", NULL};
    char e[12]; size_t l = strlen(dot + 1); if (l >= sizeof e) return 1;
    for (size_t i = 0; i < l; i++) e[i] = (char)tolower((unsigned char)dot[1 + i]); e[l] = 0;
    for (int i = 0; bin[i]; i++) if (!strcmp(e, bin[i])) return 0;
    return 1;
}
static void uf_walk(UfList *L, const char *base, const char *rel, int depth, int (*keep)(void *, const char *, const char *), void *ctx) {
    char dir[4200]; snprintf(dir, sizeof dir, "%s%s%s", base, *rel ? "/" : "", rel);
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && !L->full) {
        if (e->d_name[0] == '.') continue;
        if (++L->visited > UF_MAXFILES * 4 || L->n >= UF_MAXFILES || (L->t_end > 0 && now_sec() > L->t_end)) { L->full = 1; break; }
        char r2[4200], full[8500]; snprintf(r2, sizeof r2, "%s%s%s", rel, *rel ? "/" : "", e->d_name); snprintf(full, sizeof full, "%s/%s", base, r2);
        struct stat st; if (lstat(full, &st) != 0 || S_ISLNK(st.st_mode)) continue;
        if (S_ISDIR(st.st_mode)) { if (!sf_skip_dir(e->d_name) && depth < 12) uf_walk(L, base, r2, depth + 1, keep, ctx); }
        else if (S_ISREG(st.st_mode) && st.st_size <= UF_MAXSIZE && uf_text_ext(e->d_name) && (!keep || keep(ctx, r2, e->d_name))) {
            if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 256; L->p = (char **)realloc(L->p, sizeof(char *) * (size_t)L->cap); }
            L->p[L->n++] = strdup(r2);
        }
    }
    closedir(d);
}
static int uf_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static char *uf_slurp(const char *full, size_t *n) {
    FILE *f = fopen(full, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > UF_MAXSIZE) { fclose(f); return NULL; }
    char *b = (char *)malloc((size_t)sz + 1); if (!b) { fclose(f); return NULL; }
    *n = fread(b, 1, (size_t)sz, f); fclose(f); b[*n] = 0;
    for (size_t i = 0; i < *n && i < 512; i++) if (!b[i]) { free(b); return NULL; }       /* binario */
    return b;
}
static int uf_root_is_home(const char *root) { char h[1100]; agent_home(h, sizeof h); return !strcmp(root, "/") || !strcmp(root, h); }
static int uf_limit_ms(const UioeCall *q, int dflt) { return q->deadline_ms > 0 && q->deadline_ms < dflt ? q->deadline_ms : dflt; }

/* ---- file.grep ---- */
static int cf_grep(UioeCtx *cx, const UioeCall *q, UioeResult *r) {
    snprintf(r->source, sizeof r->source, "file.grep");
    if (!q->arg[0]) { r->status = UR_EMPTY; snprintf(r->notfound, sizeof r->notfound, "nothing to search for"); snprintf(r->limits, sizeof r->limits, "no pattern"); return r->status; }
    if (uf_root_is_home(cx->root)) { r->status = UR_UNAVAILABLE; snprintf(r->note, sizeof r->note, "the scope is the home folder, not a project: a scan would only return files of other programs"); snprintf(r->limits, sizeof r->limits, "not scanned: scope is the home folder"); return r->status; }
    UfList L = {0}; L.t_end = now_sec() + uf_limit_ms(q, 300) / 1000.0;
    uf_walk(&L, cx->root, "", 0, NULL, NULL);
    qsort(L.p, (size_t)L.n, sizeof *L.p, uf_cmp);
    const int word = uv_isword(q->arg), lim = q->limit > 0 ? q->limit : 8; int hits = 0, seen = 0, truncated = L.full;
    for (int i = 0; i < L.n && hits < lim; i++) {
        if (uioe_secret_path(L.p[i])) continue;
        char full[8600]; snprintf(full, sizeof full, "%s/%s", cx->root, L.p[i]); size_t n = 0; char *b = uf_slurp(full, &n); if (!b) continue; seen++;
        int ln = 1; for (char *p = b; p && *p && hits < lim; ln++) {
            char *e = strchr(p, '\n'); if (e) *e = 0;
            const char *m = strcasestr(p, q->arg);
            if (m && word) { const size_t kl = strlen(q->arg); int ok = 0; for (; m && !ok; m = strcasestr(m + 1, q->arg)) ok = (m == p || !(isalnum((unsigned char)m[-1]) || m[-1] == '_')) && !(isalnum((unsigned char)m[kl]) || m[kl] == '_'); m = ok ? p : NULL; }
            if (m) {
                UioeNode *nd = ur_node(r); if (!nd) break;
                nd->kind = UN_RANGE; snprintf(nd->path, sizeof nd->path, "%s", L.p[i]); nd->line0 = nd->line1 = ln; snprintf(nd->name, sizeof nd->name, "%.99s", q->arg);
                snprintf(nd->vkey, sizeof nd->vkey, "%.99s", q->arg); nd->conf = 85; nd->step = 1; nd->tier = q->tier ? q->tier : 4;
                char t[240]; snprintf(t, sizeof t, "%.230s", p); uioe_mask_line(t); ur_set_text(nd, t);
                snprintf(nd->how, sizeof nd->how, "literal text found by scanning the files (no index)"); hits++;
            }
            if (e) { *e = '\n'; p = e + 1; } else break;
        }
        free(b);
    }
    snprintf(r->limits, sizeof r->limits, "scanned %d of %d text files by reading them (the .gitignore is not applied; folders such as node_modules and build output are skipped)%s", seen, L.n, truncated ? "; the walk stopped at its file or time limit" : "");
    r->total = hits; r->status = hits ? (truncated ? UR_PARTIAL : UR_OK) : (truncated ? UR_PARTIAL : UR_EMPTY);
    if (!hits) snprintf(r->notfound, sizeof r->notfound, "no line with \"%.80s\" in the %d files scanned", q->arg, seen);
    uf_free(&L); return r->status;
}

/* ---- file.glob ---- */
static int cf_glob_keep(void *ctx, const char *rel, const char *name) { (void)name; return ct_gmatch((const char *)ctx, rel, rel); }
static int cf_glob(UioeCtx *cx, const UioeCall *q, UioeResult *r) {
    snprintf(r->source, sizeof r->source, "file.glob");
    if (!q->arg[0]) { r->status = UR_EMPTY; snprintf(r->limits, sizeof r->limits, "no pattern"); return r->status; }
    if (uf_root_is_home(cx->root)) { r->status = UR_UNAVAILABLE; snprintf(r->note, sizeof r->note, "the scope is the home folder, not a project"); snprintf(r->limits, sizeof r->limits, "not scanned: scope is the home folder"); return r->status; }
    UfList L = {0}; L.t_end = now_sec() + uf_limit_ms(q, 300) / 1000.0;
    uf_walk(&L, cx->root, "", 0, cf_glob_keep, (void *)q->arg);
    qsort(L.p, (size_t)L.n, sizeof *L.p, uf_cmp);
    const int lim = q->limit > 0 ? q->limit : 12;
    for (int i = 0; i < L.n && i < lim; i++) {
        UioeNode *nd = ur_node(r); if (!nd) break;
        nd->kind = UN_FILE; snprintf(nd->path, sizeof nd->path, "%s", L.p[i]); nd->line0 = nd->line1 = 1; nd->conf = 90; nd->step = 1; nd->tier = q->tier ? q->tier : 3;
        snprintf(nd->how, sizeof nd->how, "name matches %.60s", q->arg);
    }
    snprintf(r->limits, sizeof r->limits, "%d matching text file(s) found by walking the folder%s (the .gitignore is not applied; node_modules and build output are skipped)", L.n, L.full ? "; the walk stopped at its limit" : "");
    r->total = L.n; r->status = L.n ? (L.full ? UR_PARTIAL : UR_OK) : UR_EMPTY;
    if (!L.n) snprintf(r->notfound, sizeof r->notfound, "no file matches %.80s", q->arg);
    uf_free(&L); return r->status;
}

/* ---- place.resolve ---- */
static int cf_place(UioeCtx *cx, const UioeCall *q, UioeResult *r) {
    (void)cx; snprintf(r->source, sizeof r->source, "place.resolve");
    struct stat st;
    if (q->arg[0] && stat(q->arg, &st) == 0 && S_ISDIR(st.st_mode) && access(q->arg, R_OK | X_OK) == 0) {
        UioeNode *nd = ur_node(r); if (nd) { nd->kind = UN_FOLDER; snprintf(nd->path, sizeof nd->path, "%s", q->arg); nd->line0 = nd->line1 = 1; snprintf(nd->name, sizeof nd->name, "%.99s", q->arg); nd->conf = 80; nd->step = 1; nd->tier = 1; snprintf(nd->how, sizeof nd->how, "the cited folder exists and is readable"); }
        r->status = UR_OK; r->total = 1; snprintf(r->limits, sizeof r->limits, "only the folder's existence was checked; nothing inside it was searched");
    } else { r->status = UR_EMPTY; snprintf(r->notfound, sizeof r->notfound, "the cited folder does not exist or is not readable"); snprintf(r->limits, sizeof r->limits, "nothing searched"); }
    return r->status;
}

/* ---- docs.search ---- */
static int cf_doc_keep(void *ctx, const char *rel, const char *name) {
    (void)ctx; const char *dot = strrchr(name, '.'); if (!dot) return 0;
    if (strcasecmp(dot, ".md") && strcasecmp(dot, ".txt") && strcasecmp(dot, ".rst") && strcasecmp(dot, ".adoc") && strcasecmp(dot, ".markdown")) return 0;
    if (uioe_secret_path(rel)) return 0;
    const int depth = (int)(strchr(rel, '/') ? 1 : 0);
    if (!depth) return 1;                                              /* a raiz: todo documento */
    if (!strncmp(rel, "docs/", 5) || !strncmp(rel, "doc/", 4) || !strncmp(rel, "documentation/", 14)) return 1;
    return !strncasecmp(name, "readme", 6) || !strncasecmp(name, "install", 7) || !strncasecmp(name, "contributing", 12);      /* README de subpasta */
}
static int cf_prefix_match(const char *a, const char *b) {         /* prefixo comum de pelo menos 4 letras e de tamanho min-1 */
    const size_t la = strlen(a), lb = strlen(b), m = la < lb ? la : lb; if (m < 4) return la == lb && !strcmp(a, b);
    size_t c = 0; while (c < m && a[c] == b[c]) c++;
    const size_t M = la > lb ? la : lb;
    return c >= 4 && c + 1 >= m && M - m <= 3;       /* a palavra maior tem no maximo 3 letras a mais: "katana" nao casa "katana_ctx" */
}
typedef struct { char *path; int line; char *text; char *head; int hl; int nw; char (*w)[28]; } DocLine;
static int cf_docs(UioeCtx *cx, const UioeCall *q, UioeResult *r) {
    snprintf(r->source, sizeof r->source, "docs.search");
    char qt[420]; snprintf(qt, sizeof qt, "%s", q->arg);
    char qw[40][28]; int nq = 0;                                         /* termos da consulta, em minusculas */
    for (char *p = qt; *p && nq < 40; ) {
        while (*p && !isalnum((unsigned char)*p)) p++; if (!*p) break;
        char *s = p; while (*p && (isalnum((unsigned char)*p) || *p == '_')) { *p = (char)tolower((unsigned char)*p); p++; }
        const size_t l = (size_t)(p - s); if (l >= 3 && l < 28) { int dup = 0; for (int i = 0; i < nq; i++) dup |= !strncmp(qw[i], s, l) && !qw[i][l]; if (!dup) { memcpy(qw[nq], s, l); qw[nq][l] = 0; nq++; } }
    }
    if (!nq) { r->status = UR_EMPTY; snprintf(r->limits, sizeof r->limits, "no informative term to look for"); snprintf(r->notfound, sizeof r->notfound, "no term to search the documents for"); return r->status; }
    UfList L = {0}; L.t_end = now_sec() + uf_limit_ms(q, 300) / 1000.0;
    uf_walk(&L, cx->root, "", 0, cf_doc_keep, NULL);
    qsort(L.p, (size_t)L.n, sizeof *L.p, uf_cmp);
    if (L.n > 80) { for (int i = 80; i < L.n; i++) free(L.p[i]); L.n = 80; }
    int tot = 0, cap = 0; DocLine *D = NULL;
    for (int i = 0; i < L.n; i++) {
        char full[8600]; snprintf(full, sizeof full, "%s/%s", cx->root, L.p[i]); size_t n = 0; char *b = uf_slurp(full, &n); if (!b) continue;
        char head[120] = ""; int hl = 0, ln = 1;
        for (char *p = b; p && *p; ln++) {
            char *e = strchr(p, '\n'); if (e) *e = 0;
            const char *t = p; while (*t == ' ' || *t == '\t') t++;
            if (*t) {
                if (*t == '#') { const char *h = t; while (*h == '#' || *h == ' ') h++; snprintf(head, sizeof head, "%.110s", h); hl = ln; }
                if (tot == cap) { cap = cap ? cap * 2 : 512; D = (DocLine *)realloc(D, sizeof *D * (size_t)cap); }
                DocLine *d = &D[tot++]; d->path = strdup(L.p[i]); d->line = ln; d->text = strdup(t); d->head = strdup(head); d->hl = hl; d->nw = 0; d->w = NULL;
            }
            if (e) p = e + 1; else break;
        }
        free(b);
    }
    /* df de cada termo sobre as linhas */
    int df[40] = {0}; double idf[40]; int *mt = (int *)calloc((size_t)tot + 1, sizeof(int)); unsigned *mask = (unsigned *)calloc((size_t)tot + 1, sizeof(unsigned)); (void)mt;
    for (int i = 0; i < tot; i++) {
        char lw[500]; size_t o = 0; for (const char *p = D[i].text; *p && o < sizeof lw - 1; p++) lw[o++] = (char)tolower((unsigned char)*p); lw[o] = 0;
        for (char *p = lw; *p; ) {
            while (*p && !isalnum((unsigned char)*p)) p++; if (!*p) break;
            char *s = p; while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            const char sv = *p; *p = 0;
            for (int k = 0; k < nq; k++) if (!(mask[i] >> k & 1) && cf_prefix_match(qw[k], s)) { mask[i] |= 1u << k; df[k]++; }
            *p = sv;
        }
    }
    int ign[40] = {0}; int nuse = 0;
    for (int k = 0; k < nq; k++) { ign[k] = (tot >= 30 && df[k] * 10 > tot * 3) || df[k] == 0; idf[k] = log(1.0 + (double)tot / (df[k] + 0.5)); if (!ign[k]) nuse++; }
    typedef struct { int i; double sc; int mc; } Sc; Sc *S = (Sc *)calloc((size_t)tot + 1, sizeof *S); int ns = 0;
    for (int i = 0; i < tot; i++) {
        double sc = 0; int mc = 0; for (int k = 0; k < nq; k++) if (!ign[k] && (mask[i] >> k & 1)) { sc += idf[k]; mc++; }
        if (D[i].text[0] == '#') sc *= 1.2;
        if (mc && sc >= 1.5 && (mc >= 2 || sc >= 3.0 || nuse == 1)) { S[ns].i = i; S[ns].sc = sc; S[ns].mc = mc; ns++; }
    }
    for (int a = 1; a < ns; a++) { const Sc x = S[a]; int b = a; while (b > 0 && S[b - 1].sc < x.sc) { S[b] = S[b - 1]; b--; } S[b] = x; }
    const int lim = q->limit > 0 ? q->limit : 6;
    for (int a = 0; a < ns && a < lim; a++) {
        const DocLine *d = &D[S[a].i]; UioeNode *nd = ur_node(r); if (!nd) break;
        nd->kind = UN_DOCLINE; snprintf(nd->path, sizeof nd->path, "%s", d->path); nd->line0 = nd->line1 = d->line; snprintf(nd->name, sizeof nd->name, "%.99s", d->head);
        snprintf(nd->vkey, sizeof nd->vkey, "%.40s", d->text); char t[240]; snprintf(t, sizeof t, "%.200s", d->text); uioe_mask_line(t); ur_set_text(nd, t);
        nd->conf = 40 + 8 * S[a].mc; if (nd->conf > 70) nd->conf = 70; nd->step = 1; nd->tier = S[a].mc >= 2 ? 3 : 4;
        snprintf(nd->how, sizeof nd->how, "document line matching %d of the terms%s%s", S[a].mc, d->head[0] ? ", under heading: " : "", d->head);
    }
    snprintf(r->limits, sizeof r->limits, "%d document(s) read (README, docs/ and the root documents; %d lines)%s", L.n, tot, L.full ? "; the walk stopped at its limit" : "");
    r->total = ns; r->status = ns ? (L.full ? UR_PARTIAL : UR_OK) : UR_EMPTY;
    if (!ns) snprintf(r->notfound, sizeof r->notfound, "no document line matches the terms (%d document(s), %d lines read)", L.n, tot);
    for (int i = 0; i < tot; i++) { free(D[i].path); free(D[i].text); free(D[i].head); }
    free(D); free(mt); free(mask); free(S); uf_free(&L); return r->status;
}

/* ---- docs.select (so dentro do agente: precisa do tokenizador) ---- */
static int cf_docsel(UioeCtx *cx, const UioeCall *q, UioeResult *r) {
    snprintf(r->source, sizeof r->source, "docs.select");
    if (!cx->tk) { r->status = UR_UNAVAILABLE; snprintf(r->note, sizeof r->note, "needs the model's tokenizer (only inside an agent session)"); snprintf(r->limits, sizeof r->limits, "docs.select not run: no tokenizer outside the agent"); return r->status; }
    char *out = (char *)malloc(8200); if (!out) { r->status = UR_ERROR; return r->status; }
    const int n = ds_select(cx->tk, q->arg, br_cur(), cx->root, out, 8200);
    if (n <= 0) { r->status = UR_EMPTY; snprintf(r->notfound, sizeof r->notfound, "the project documents that KATANA.md points to have nothing for this request"); snprintf(r->limits, sizeof r->limits, "only the documents KATANA.md or AGENTS.md point to"); free(out); return r->status; }
    for (char *p = strstr(out, "\n== "); p; p = strstr(p + 1, "\n== ")) {
        char path[400]; int a = 0, b = 0; if (sscanf(p + 4, "%399[^:]:%d-%d", path, &a, &b) < 3) continue;
        UioeNode *nd = ur_node(r); if (!nd) break;
        nd->kind = UN_DOCLINE; snprintf(nd->path, sizeof nd->path, "%s", path); nd->line0 = a; nd->line1 = b; snprintf(nd->name, sizeof nd->name, "selected by docsel");
        const char *t = strchr(p + 4, '\n'); if (t) { t++; char tmp[200]; snprintf(tmp, sizeof tmp, "%.190s", t); char *e = strchr(tmp, '\n'); if (e) *e = 0; ur_set_text(nd, tmp); snprintf(nd->vkey, sizeof nd->vkey, "%.40s", nd->text); }
        nd->conf = 70; nd->step = 1; nd->tier = 3; snprintf(nd->how, sizeof nd->how, "picked by the docsel selector from the documents KATANA.md points to");
    }
    free(out); r->total = r->nnode; r->status = r->nnode ? UR_OK : UR_EMPTY;
    snprintf(r->limits, sizeof r->limits, "only the documents KATANA.md or AGENTS.md point to");
    return r->status;
}
#else
/* Windows: sem varredura de pastas nem busca em documentos ainda (a ferramenta Grep tambem nao existe la); o resultado diz isso e o pipeline cai na escada */
static int cf_unavail(UioeResult *r, const char *id) { snprintf(r->source, sizeof r->source, "%s", id); r->status = UR_UNAVAILABLE; snprintf(r->note, sizeof r->note, "not available on this platform yet"); snprintf(r->limits, sizeof r->limits, "%s: not available on this platform", id); return r->status; }
static int cf_grep(UioeCtx *cx, const UioeCall *q, UioeResult *r) { (void)cx; (void)q; return cf_unavail(r, "file.grep"); }
static int cf_glob(UioeCtx *cx, const UioeCall *q, UioeResult *r) { (void)cx; (void)q; return cf_unavail(r, "file.glob"); }
static int cf_place(UioeCtx *cx, const UioeCall *q, UioeResult *r) { (void)cx; (void)q; return cf_unavail(r, "place.resolve"); }
static int cf_docs(UioeCtx *cx, const UioeCall *q, UioeResult *r) { (void)cx; (void)q; return cf_unavail(r, "docs.search"); }
static int cf_docsel(UioeCtx *cx, const UioeCall *q, UioeResult *r) { (void)cx; (void)q; return cf_unavail(r, "docs.select"); }
#endif
