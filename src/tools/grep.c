/* ---------- ferramenta Grep (ver docs/tools.md) ----------
 * Motor proprio (src/tools/regex.c); nao chama programa externo de busca (uma so sintaxe, a que a descricao promete).
 * Lista de arquivos: "git ls-files" (rastreados + nao rastreados que nao estao no .gitignore) quando a pasta esta num
 * repositorio; senao uma caminhada que aplica os .gitignore da pasta, das pastas acima dela ate a pasta de trabalho e
 * das subpastas (com !, /, ** e pasta/), pulando tambem node_modules, .venv etc. Um arquivo passado em path sempre e lido.
 * Tolerancia: padrao invalido vira texto simples; zero resultados tenta texto simples, caixa ignorada e sem filtros e
 * diz ao modelo o que achou; nomes de argumentos parecidos (include, ignore_case, context, literal...) sao aceitos. */
static const char CT_GREP_DESC[] =
    "Search file contents with a regular expression. Syntax: literals and . ^ $ \\A \\z \\Z \\b \\B \\< \\>; classes "
    "[abc] [^a-z] [[:alpha:]], \\d (0-9) \\w \\s \\h \\v and their negations \\D \\W \\S, Unicode \\p{L} \\p{Lu} \\p{Ll} "
    "\\p{N} \\p{P} \\p{Z} and \\P{..}; escapes \\t \\n \\r \\xHH \\x{HHHH} \\uHHHH; groups (...) (?:...) (?<name>...) "
    "(?>...); alternation a|b; quantifiers * + ? {n} {n,} {n,m}, lazy with a trailing ? (*?) and possessive with a "
    "trailing + (*+); lookahead (?=...) (?!...), lookbehind (?<=...) (?<!...) of bounded length; backreferences \\1 and "
    "\\k<name>; flags (?i) (?m) (?s) (?x) and (?i:...); \\Q...\\E for literal text. A { that is not a repetition is a "
    "literal. Examples: \"fn\\s+\\w+\\(\" finds function definitions; \"(?i)todo|fixme\" finds notes in any case. Set "
    "literal=true to search plain text (an invalid pattern is searched as plain text anyway). output_mode: "
    "\"files_with_matches\" (default; only paths, newest first), \"content\" (matching lines as path:line:text, with "
    "-A/-B/-C context lines; for code use content with -n and -C 2), \"count\" (matching lines per file and a total). `glob` (e.g. \"*.{ts,tsx}\") and `type` "
    "(c, cpp, h, py, js, ts, rust, go, java, sh, md, json, yaml, html, css, qml, ...) filter files. `path` is a folder "
    "or one file (default: the current folder). Binary files, .git and gitignored files are skipped (a file given as "
    "`path` is always searched). Lines are matched one by one; with multiline=true the pattern may span lines (. matches "
    "newlines, ^ and $ match at every line). Output is capped by head_limit (default 250, 0 = no limit) and skipped by "
    "offset. Use this instead of grep in Bash.";
static const char CT_GREP_PROPS[] =
    "\"pattern\": {\"type\": \"string\", \"description\": \"The regular expression to search for\"}, "
    "\"path\": {\"type\": \"string\", \"description\": \"File or folder to search in. Defaults to the current working folder\"}, "
    "\"glob\": {\"type\": \"string\", \"description\": \"Only search files whose name matches this glob, e.g. *.js or *.{ts,tsx}\"}, "
    "\"type\": {\"type\": \"string\", \"description\": \"Only search this kind of file: c, cpp, h, py, js, ts, rust, go, java, sh, md, json, yaml, html, css, qml\"}, "
    "\"output_mode\": {\"type\": \"string\", \"enum\": [\"content\", \"files_with_matches\", \"count\"], \"description\": \"files_with_matches (default) lists paths; content shows lines; count shows matching lines per file\"}, "
    "\"-i\": {\"type\": \"boolean\", \"description\": \"Case insensitive search\"}, "
    "\"-n\": {\"type\": \"boolean\", \"description\": \"Show line numbers (content mode only; default true)\"}, "
    "\"-A\": {\"type\": \"integer\", \"description\": \"Lines of context after each match (content mode)\"}, "
    "\"-B\": {\"type\": \"integer\", \"description\": \"Lines of context before each match (content mode)\"}, "
    "\"-C\": {\"type\": \"integer\", \"description\": \"Lines of context before and after each match (content mode)\"}, "
    "\"context\": {\"type\": \"integer\", \"description\": \"Same as -C\"}, "
    "\"multiline\": {\"type\": \"boolean\", \"description\": \"Let the pattern match across line boundaries (default false)\"}, "
    "\"literal\": {\"type\": \"boolean\", \"description\": \"Search the pattern as plain text, not as a regular expression\"}, "
    "\"head_limit\": {\"type\": \"integer\", \"description\": \"Return at most this many entries (default 250; 0 = no limit)\"}, "
    "\"offset\": {\"type\": \"integer\", \"description\": \"Skip this many entries before returning (for paging)\"}";
static const char CT_GREP_REQ[] = "\"pattern\"";

#ifndef _WIN32
/* tipos de arquivo: nome -> extensoes separadas por virgula */
static const char *const g_ct_types[][2] = {
    {"c", "c,h"}, {"cpp", "cpp,cc,cxx,hpp,hh,hxx,h"}, {"h", "h,hpp,hh,hxx"}, {"py", "py,pyi"}, {"js", "js,jsx,mjs,cjs"},
    {"ts", "ts,tsx,mts,cts"}, {"rust", "rs"}, {"rs", "rs"}, {"go", "go"}, {"java", "java"}, {"sh", "sh,bash,zsh"},
    {"md", "md,markdown"}, {"json", "json"}, {"yaml", "yaml,yml"}, {"html", "html,htm"}, {"css", "css,scss"},
    {"qml", "qml"}, {"toml", "toml"}, {"txt", "txt"}, {"xml", "xml"}, {"sql", "sql"}, {"rb", "rb"}, {"php", "php"},
    {"cs", "cs"}, {"kt", "kt,kts"}, {"swift", "swift"}, {"lua", "lua"},
};
static const char *ct_type_exts(const char *t) {
    for (size_t i = 0; i < sizeof g_ct_types / sizeof g_ct_types[0]; i++) if (strcmp(g_ct_types[i][0], t) == 0) return g_ct_types[i][1];
    return NULL;
}
static int ct_ext_in(const char *name, const char *exts) {
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return 0;
    dot++;
    const size_t dl = strlen(dot);
    for (const char *p = exts; *p; ) {
        const char *c = strchr(p, ','); const size_t l = c ? (size_t)(c - p) : strlen(p);
        if (l == dl && memcmp(p, dot, l) == 0) return 1;
        if (!c) break;
        p = c + 1;
    }
    return 0;
}

/* filtros dos arquivos (glob, tipo) */
typedef struct { const char *glob, *exts; } CtGrepFilter;
static int ct_grep_keep(const CtGrepFilter *f, const char *rel, const char *name) {
    if (f->exts && !ct_ext_in(name, f->exts)) return 0;
    if (f->glob) {
        if (strchr(f->glob, '/')) { if (!ct_gmatch(f->glob, rel, rel)) return 0; }
        else if (!ct_gmatch(f->glob, name, name)) return 0;
    }
    return 1;
}

/* ---- regras de .gitignore (fora de repositorio git) ----
 * Cada regra guarda a pasta do .gitignore de onde veio; vale a ultima que casar (! desfaz). Pasta ignorada nao e
 * visitada, entao um arquivo dentro dela nao volta com !, como no git. */
typedef struct { char *pat, *base; int neg, dir, anch; } CtGiRule;
typedef struct { CtGiRule *v; int n, cap; } CtGiSet;
static void ct_gi_file(CtGiSet *s, const char *dir) {
    char gp[4300];
    snprintf(gp, sizeof gp, "%s/.gitignore", dir);
    FILE *fp = fopen(gp, "rb");
    if (!fp) return;
    char line[2048];
    while (fgets(line, sizeof line, fp)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        while (l && line[l - 1] == ' ' && !(l > 1 && line[l - 2] == '\\')) line[--l] = 0;
        if (l > 1 && line[l - 1] == ' ' && line[l - 2] == '\\') { line[l - 2] = ' '; line[--l] = 0; }
        char *p = line;
        if (!*p || *p == '#') continue;
        int neg = 0, isd = 0, anch = 0;
        if (*p == '!') { neg = 1; p++; }
        else if (*p == '\\' && (p[1] == '#' || p[1] == '!')) p++;
        l = strlen(p);
        if (l && p[l - 1] == '/') { p[--l] = 0; isd = 1; }
        if (!l) continue;
        if (strchr(p, '/')) anch = 1;
        if (*p == '/') p++;
        if (!*p) continue;
        if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 64; s->v = (CtGiRule *)realloc(s->v, sizeof(CtGiRule) * (size_t)s->cap); }
        s->v[s->n].pat = strdup(p); s->v[s->n].base = strdup(dir); s->v[s->n].neg = neg;
        s->v[s->n].dir = isd; s->v[s->n].anch = anch;
        s->n++;
    }
    fclose(fp);
}
static void ct_gi_trunc(CtGiSet *s, int n) {
    while (s->n > n) { s->n--; free(s->v[s->n].pat); free(s->v[s->n].base); }
}
static int ct_gi_ignored(const CtGiSet *s, const char *abs, int isdir) {
    int ign = 0;
    for (int i = 0; i < s->n; i++) {
        const CtGiRule *r = &s->v[i];
        if (r->dir && !isdir) continue;
        const size_t bl = strlen(r->base);
        if (strncmp(abs, r->base, bl) != 0 || abs[bl] != '/') continue;
        const char *rel = abs + bl + 1;
        int m;
        if (r->anch) m = ct_gmatch(r->pat, rel, rel);
        else { const char *nm = strrchr(rel, '/'); nm = nm ? nm + 1 : rel; m = ct_gmatch(r->pat, nm, nm); }
        if (m) ign = !r->neg;
    }
    return ign;
}
/* caminhada com .gitignore; base e a pasta buscada (para o caminho relativo do glob) */
static void ct_gwalk(CtFiles *f, const char *base, const char *dir, int depth, CtGiSet *gs, const CtGrepFilter *flt) {
    const int n0 = gs->n;
    ct_gi_file(gs, dir);
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (f->full) break;
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            if (++f->visited > CT_SCAN_MAX) { f->full = 1; break; }
            char full[4400];
            if ((size_t)snprintf(full, sizeof full, "%s/%s", dir, e->d_name) >= sizeof full) continue;
            struct stat st;
            if (lstat(full, &st) != 0) continue;
            if (S_ISLNK(st.st_mode) && (stat(full, &st) != 0 || S_ISDIR(st.st_mode))) continue;
            if (S_ISDIR(st.st_mode)) {
                if (strcmp(e->d_name, ".git") == 0 || sf_skip_dir(e->d_name) || depth >= 40) continue;
                if (ct_gi_ignored(gs, full, 1)) continue;
                ct_gwalk(f, base, full, depth + 1, gs, flt);
            } else if (S_ISREG(st.st_mode)) {
                if (ct_gi_ignored(gs, full, 0)) continue;
                const char *rel = full + strlen(base) + 1;
                if (!ct_grep_keep(flt, rel, e->d_name)) continue;
                if (f->n >= CT_KEEP_MAX) { f->full = 1; break; }
                ct_files_add(f, full, (long long)st.st_mtime);
            }
        }
        closedir(d);
    }
    ct_gi_trunc(gs, n0);
}

/* aspas simples para o shell */
static void ct_shq(const char *s, char *o, size_t cap) {
    size_t n = 0;
    if (n + 1 < cap) o[n++] = '\'';
    for (; *s && n + 5 < cap; s++) {
        if (*s == '\'') { memcpy(o + n, "'\\''", 4); n += 4; } else o[n++] = *s;
    }
    if (n + 1 < cap) o[n++] = '\'';
    o[n] = 0;
}

/* lista de arquivos de uma pasta: git se possivel, senao caminhada com .gitignore */
static void ct_grep_collect(CtFiles *f, const char *base, const char *cwd, const CtGrepFilter *flt) {
    char q[8400], cmd[9000];
    ct_shq(base, q, sizeof q);
    snprintf(cmd, sizeof cmd, "git -C %s -c core.quotepath=off ls-files -z --cached --others --exclude-standard 2>/dev/null | tr '\\000' '\\n'", q);
    int rc = 0;
    char *o = run_collect(cmd, 60, 32u << 20, &rc);
    if (o && rc == 0 && o[0]) {
        for (char *line = strtok(o, "\n"); line; line = strtok(NULL, "\n")) {
            const char *name = strrchr(line, '/'); name = name ? name + 1 : line;
            if (!ct_grep_keep(flt, line, name)) continue;
            char full[8500]; struct stat st;
            snprintf(full, sizeof full, "%s/%s", base, line);
            if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            if (f->n >= CT_KEEP_MAX) { f->full = 1; break; }
            ct_files_add(f, full, (long long)st.st_mtime);
        }
        free(o);
        return;
    }
    free(o);
    CtGiSet gs; memset(&gs, 0, sizeof gs);
    /* .gitignore das pastas acima (da pasta de trabalho ate o pai da pasta buscada) */
    const size_t cl = strlen(cwd);
    if (cl && strcmp(cwd, base) != 0 && strncmp(base, cwd, cl) == 0 && base[cl] == '/') {
        char anc[4400];
        snprintf(anc, sizeof anc, "%s", cwd);
        ct_gi_file(&gs, anc);
        for (const char *p = base + cl + 1; ; ) {
            const char *sl = strchr(p, '/');
            if (!sl) break;
            const size_t l = (size_t)(sl - base);
            if (l >= sizeof anc) break;
            memcpy(anc, base, l); anc[l] = 0;
            ct_gi_file(&gs, anc);
            p = sl + 1;
        }
    }
    ct_gwalk(f, base, base, 0, &gs, flt);
    ct_gi_trunc(&gs, 0);
    free(gs.v);
}

/* ---- busca ---- */
typedef struct {
    Rx *re;
    long *spans;
    int mode;               /* 0 arquivos, 1 conteudo, 2 contagem */
    int A, B, num, multi;
    long offset, limit, ent, emitted;
    int more, stop, any_out;
    long long total;        /* linhas (ou casamentos) em todos os arquivos */
    int files_matched;
    int slow; char slowat[4400];
    int track; char ext[16][16]; int extn[16], next;    /* extensoes dos arquivos com resultado (para a dica de filtro) */
    CtSb sb;
} CtGrep;

static void ct_gemit(CtGrep *g, const char *text, size_t tl) {
    if (g->stop) return;
    if (g->ent++ < g->offset) return;
    if (g->limit && g->emitted >= g->limit) { g->more = 1; if (g->mode == 1) g->stop = 1; return; }
    ct_sb_add(&g->sb, text, tl); ct_sb_add(&g->sb, "\n", 1);
    g->emitted++; g->any_out = 1;
}

static void ct_gline(CtGrep *g, const char *disp, const char *buf, const size_t *ls, long i, size_t nlen, char sep) {
    char head[4400];
    size_t hl;
    if (g->num) hl = (size_t)snprintf(head, sizeof head, "%s%c%ld%c", disp, sep, i + 1, sep);
    else hl = (size_t)snprintf(head, sizeof head, "%s%c", disp, sep);
    if (hl >= sizeof head) hl = sizeof head - 1;
    size_t l = nlen;
    while (l && buf[ls[i] + l - 1] == '\r') l--;
    const int cut = l > 500;
    if (cut) l = 500;
    char *t = (char *)malloc(hl + l + 4);
    memcpy(t, head, hl); memcpy(t + hl, buf + ls[i], l);
    size_t tl = hl + l;
    if (cut) { memcpy(t + tl, "...", 3); tl += 3; }
    ct_gemit(g, t, tl);
    free(t);
}

static void ct_grep_track(CtGrep *g, const char *abs) {
    const char *nm = strrchr(abs, '/'); nm = nm ? nm + 1 : abs;
    const char *dot = strrchr(nm, '.');
    char e[16];
    snprintf(e, sizeof e, "%s", dot && dot != nm ? dot : "(no extension)");
    for (int i = 0; i < g->next; i++) if (strcmp(g->ext[i], e) == 0) { g->extn[i]++; return; }
    if (g->next < 16) { snprintf(g->ext[g->next], 16, "%s", e); g->extn[g->next++] = 1; }
}

static void ct_grep_file(CtGrep *g, const char *abs, const char *cwd) {
    if (g->stop) return;
    struct stat st;
    if (stat(abs, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (32 << 20)) return;
    FILE *fp = fopen(abs, "rb");
    if (!fp) return;
    char *buf = (char *)malloc((size_t)st.st_size + 2);
    const size_t n = fread(buf, 1, (size_t)st.st_size, fp);
    fclose(fp);
    buf[n] = 0;
    if (memchr(buf, 0, n < 8000 ? n : 8000)) { free(buf); return; }      /* binario */
    const Rx *re = g->re;
    if (re->reqlen && !rx_memmem(buf, n, re->req, (size_t)re->reqlen)) { free(buf); return; }   /* trecho obrigatorio ausente */
    long nl = 0;
    for (size_t i = 0; i < n; i++) if (buf[i] == '\n') nl++;
    if (n && buf[n - 1] != '\n') nl++;
    size_t *ls = (size_t *)malloc(sizeof(size_t) * (size_t)(nl + 2));
    size_t *le = (size_t *)malloc(sizeof(size_t) * (size_t)(nl + 2));
    long k = 0;
    for (size_t i = 0; i < n; ) {
        size_t e = i;
        const char *q = (const char *)memchr(buf + i, '\n', n - i);
        e = q ? (size_t)(q - buf) : n;
        ls[k] = i; le[k] = e - i; k++;
        i = e + 1;
    }
    unsigned char *show = (unsigned char *)calloc((size_t)nl + 1, 1);     /* 1 casamento, 2 contexto */
    long nmatch = 0;
    const char *disp = ct_display(abs, cwd);
    if (!g->multi) {
        size_t hit = 0; int havehit = 0;
        for (long i = 0; i < nl; i++) {
            if (re->reqlen) {       /* pula direto para a proxima linha que contem o trecho obrigatorio */
                if (!havehit || hit < ls[i]) {
                    const char *p = rx_memmem(buf + ls[i], n - ls[i], re->req, (size_t)re->reqlen);
                    if (!p) break;
                    hit = (size_t)(p - buf); havehit = 1;
                }
                if (hit >= ls[i] + le[i]) continue;
            }
            size_t len = le[i];
            if (len && buf[ls[i] + len - 1] == '\r') len--;
            const int r = rx_search(g->re, buf + ls[i], (long)len, 0, NULL);
            if (r < 0) { g->slow = 1; g->stop = 1; snprintf(g->slowat, sizeof g->slowat, "%s:%ld", disp, i + 1); break; }
            if (r) { show[i] = 1; nmatch++; if (g->mode == 0) break; }
        }
    } else {
        size_t p = 0;
        while (p <= n) {
            const int r = rx_search(g->re, buf, (long)n, (long)p, g->spans);
            if (r < 0) { g->slow = 1; g->stop = 1; snprintf(g->slowat, sizeof g->slowat, "%s", disp); break; }
            if (!r) break;
            const size_t so = (size_t)g->spans[0], eo = (size_t)g->spans[1];
            nmatch++;
            long a = 0, b = nl - 1, lo;     /* linha do inicio: ultima com ls <= so */
            while (a < b) { const long m = (a + b + 1) / 2; if (ls[m] <= so) a = m; else b = m - 1; }
            lo = a;
            long hi = lo;
            if (eo > so) { a = lo; b = nl - 1; while (a < b) { const long m = (a + b + 1) / 2; if (ls[m] <= eo - 1) a = m; else b = m - 1; } hi = a; }
            if (nl) for (long i = lo; i <= hi; i++) show[i] = 1;
            if (g->mode == 0) break;
            p = eo > so ? eo : so + 1;
        }
    }
    if (nmatch > 0 && !g->slow) {
        g->files_matched++;
        if (g->track) ct_grep_track(g, abs);
        if (g->mode == 0) { g->total++; ct_gemit(g, disp, strlen(disp)); }
        else if (g->mode == 2) {
            long cnt = 0;
            if (g->multi) cnt = nmatch; else for (long i = 0; i < nl; i++) cnt += show[i] == 1;
            g->total += cnt;
            char t[4400];
            const int tl = snprintf(t, sizeof t, "%s:%ld", disp, cnt);
            ct_gemit(g, t, (size_t)(tl < (int)sizeof t ? tl : (int)sizeof t - 1));
        } else {
            long cnt = 0;
            for (long i = 0; i < nl; i++) if (show[i] == 1) cnt++;
            g->total += g->multi ? nmatch : cnt;
            if (g->A || g->B) {
                for (long i = 0; i < nl; i++) if (show[i] == 1) {
                    for (long j = i - g->B < 0 ? 0 : i - g->B; j <= i + g->A && j < nl; j++) if (!show[j]) show[j] = 2;
                }
            }
            long prev = -2;
            for (long i = 0; i < nl && !g->stop; i++) {
                if (!show[i]) continue;
                if ((g->A || g->B) && ((prev >= 0 && prev != i - 1) || (prev == -2 && g->any_out))) ct_gemit(g, "--", 2);
                ct_gline(g, disp, buf, ls, i, le[i], show[i] == 1 ? ':' : '-');
                prev = i;
            }
        }
    }
    free(show); free(ls); free(le); free(buf);
}

static void ct_grep_all(CtGrep *g, const CtFiles *files, const char *cwd) {
    free(g->spans);
    g->spans = (long *)malloc(sizeof(long) * (size_t)(2 * g->re->ngroups));
    for (int i = 0; i < files->n && !g->stop; i++) ct_grep_file(g, files->v[i].path, cwd);
}
/* conta resultados (linhas) com outro padrao, sem guardar texto; -1 se estourou o limite de passos */
static long long ct_grep_count(Rx *re, const CtFiles *files, const char *cwd, int multi, CtGrep *keep) {
    CtGrep h; memset(&h, 0, sizeof h);
    h.re = re; h.mode = 2; h.multi = multi; h.track = 1;
    ct_grep_all(&h, files, cwd);
    free(h.spans); free(h.sb.p);
    if (keep) { h.sb.p = NULL; h.spans = NULL; *keep = h; }
    return h.slow ? -1 : h.total;
}

/* procura sob dir entradas (arquivo ou pasta) com o nome name; guarda a primeira em first */
static void ct_find_name(const char *dir, const char *name, int depth, int *found, char *first, size_t fcap, long *visits) {
    if (*found > 1 || depth > 30) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && *found <= 1) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (++*visits > 200000) break;
        char full[4400];
        if ((size_t)snprintf(full, sizeof full, "%s/%s", dir, e->d_name) >= sizeof full) continue;
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (strcmp(e->d_name, name) == 0) { if (++*found == 1) snprintf(first, fcap, "%s", full); }
        if (S_ISDIR(st.st_mode) && strcmp(e->d_name, ".git") != 0 && !sf_skip_dir(e->d_name)) ct_find_name(full, name, depth + 1, found, first, fcap, visits);
    }
    closedir(d);
}

static int ct_has_meta(const char *p) { return strpbrk(p, ".*+?()[]{}|^$\\") != NULL; }
/* o padrao parece uma frase que descreve o que se procura (varias palavras, sem metacaracteres): nao e texto que aparece nos arquivos */
static int ct_grep_prose(const char *p) {
    int words = 0, letters = 0; size_t n = strlen(p);
    if (n < 12 || ct_has_meta(p)) return 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)p[i];
        if (c == ' ') { if (i && p[i - 1] != ' ') words++; }
        else if (isalpha(c) || c >= 0x80) letters++;
        else if (c != '-' && c != '_' && c != '\'' && c != '"' && !isdigit(c)) return 0;
    }
    if (n && p[n - 1] != ' ') words++;
    return words >= 3 && letters * 10 >= (int)n * 7;
}

/* gancho de teste do motor (KATANA_REGEX_TEST=1): casa pattern contra subject e mostra os trechos */
static void ct_regex_test(const char *js, const char *end, const char *pat, char *out, size_t cap) {
    const char *a, *b;
    size_t sl = 0;
    char *subj = NULL;
    if (ct_find(js, end, "subject", &a, &b) && *a == '"') subj = ct_unquote(a, b, &sl);
    if (!subj) { snprintf(out, cap, "error: subject must be a string"); return; }
    const int fl = (ct_bool(js, end, "-i") ? RX_I : 0) | (ct_bool(js, end, "multiline") ? RX_M | RX_S : 0) | (ct_bool(js, end, "literal") ? RX_LITERAL : 0);
    char err[400];
    Rx *re = rx_compile(pat, strlen(pat), fl, err, sizeof err);
    if (!re) { snprintf(out, cap, "error: %s", err); free(subj); return; }
    long *sp = (long *)malloc(sizeof(long) * (size_t)(2 * re->ngroups));
    const long st = (long)ct_int(js, end, "start", 0);
    const int r = rx_search(re, subj, (long)sl, st, sp);
    if (r < 0) snprintf(out, cap, "error: pattern too slow on this input; simplify it");
    else if (r == 0) snprintf(out, cap, "no match");
    else {
        size_t n = 0;
        for (int i = 0; i < re->ngroups && n + 64 < cap; i++) {
            if (sp[2 * i] < 0 || sp[2 * i + 1] < 0) n += (size_t)snprintf(out + n, cap - n, "%s%d:-", i ? " " : "", i);
            else n += (size_t)snprintf(out + n, cap - n, "%s%d:%ld-%ld", i ? " " : "", i, sp[2 * i], sp[2 * i + 1]);
        }
        for (int k = 0; k < re->nnames && n + 64 < cap; k++) {
            const int i = re->nmi[k];
            if (sp[2 * i] < 0 || sp[2 * i + 1] < 0) n += (size_t)snprintf(out + n, cap - n, " %s=-", re->nm[k]);
            else n += (size_t)snprintf(out + n, cap - n, " %s=%ld-%ld", re->nm[k], sp[2 * i], sp[2 * i + 1]);
        }
    }
    free(sp); rx_free(re); free(subj);
}

/* monta o texto do resultado de uma busca com resultados */
static void ct_grep_format(CtGrep *g, const char *note, char *out, size_t cap) {
    char head[200] = "", tail[420] = "", lim[300] = "";
    size_t n = 0;
    if (note && note[0]) n = (size_t)snprintf(out, cap, "%s\n", note);
    if (n >= cap) n = cap - 1;
    if (g->emitted == 0 && g->ent > 0) { snprintf(out + n, cap - n, "No entries at this offset (the result has %ld entries; offset=%ld).", g->ent, g->offset); return; }
    if (g->mode == 0) snprintf(head, sizeof head, "Found %d file%s\n", g->files_matched, g->files_matched == 1 ? "" : "s");
    else if (g->mode == 2) snprintf(tail, sizeof tail, "Found %lld total match%s across %d file%s.", g->total, g->total == 1 ? "" : "es", g->files_matched, g->files_matched == 1 ? "" : "s");
    if (g->more) snprintf(lim, sizeof lim, "(Output limited to %ld entries starting at offset %ld; more results exist. Use offset=%ld to continue, or narrow with path, glob or type.)",
                          g->limit, g->offset, g->offset + g->limit);
    const size_t fixed = n + strlen(head) + strlen(tail) + strlen(lim) + 200;
    size_t budget = cap > fixed ? cap - 1 - fixed : 0;
    int cut = 0;
    if (g->sb.n > budget) {
        size_t b = budget;
        while (b > 0 && g->sb.p[b - 1] != '\n') b--;
        g->sb.n = b; if (g->sb.p) g->sb.p[b] = 0; cut = 1;
    }
    n += (size_t)snprintf(out + n, cap - n, "%s%s", head, g->sb.p ? g->sb.p : "");
    if (cut) n += (size_t)snprintf(out + n, cap - n, "(Output truncated at the size limit. Narrow with path, glob or type, use head_limit and offset to page, or search with a more specific pattern.)\n");
    if (lim[0]) n += (size_t)snprintf(out + n, cap - n, "%s\n", lim);
    if (tail[0]) n += (size_t)snprintf(out + n, cap - n, "%s\n", tail);
    while (n > 0 && out[n - 1] == '\n') out[--n] = 0;
}
#endif

static void ct_grep(const char *js, const char *end, char *out, size_t cap) {
#ifdef _WIN32
    (void)js; (void)end; snprintf(out, cap, "error: not available on Windows yet");
#else
    if (cap < 600) { if (cap) out[0] = 0; return; }
    char *pat = ct_str(js, end, "pattern"), *path = ct_str(js, end, "path"), *glob = ct_str(js, end, "glob");
    char *type = ct_str(js, end, "type"), *mode = ct_str(js, end, "output_mode"), *rxs = ct_str(js, end, "regex");
    CtGrep g; memset(&g, 0, sizeof g);
    CtFiles files; memset(&files, 0, sizeof files);
    CtGrepFilter flt; memset(&flt, 0, sizeof flt);
    char res[4200], cwd[4200], note[900] = "", err[400];
    struct stat st;
    Rx *re = NULL;
    /* nomes alternativos que os modelos usam */
    if ((!pat || !*pat) && rxs && *rxs && strcmp(rxs, "true") != 0 && strcmp(rxs, "false") != 0) { free(pat); pat = rxs; rxs = NULL; }
    if (!glob) glob = ct_str(js, end, "include");
    const int icase = ct_bool(js, end, "-i") || ct_bool(js, end, "case_insensitive") || ct_bool(js, end, "ignore_case");
    const int literal = (rxs && strcmp(rxs, "false") == 0) || ct_bool(js, end, "fixed_strings") || ct_bool(js, end, "literal");
    if (!pat || !*pat) { snprintf(out, cap, "error: missing `pattern`. Example: {\"pattern\": \"TODO\", \"output_mode\": \"content\"}"); goto done; }
    if (getenv("KATANA_REGEX_TEST") && strcmp(getenv("KATANA_REGEX_TEST"), "1") == 0 && !ct_has(js, end, "path") && ct_has(js, end, "subject")) {
        ct_regex_test(js, end, pat, out, cap);
        goto done;
    }
    if (!mode || !*mode || !strcmp(mode, "files_with_matches") || !strcmp(mode, "files") || !strcmp(mode, "file") ||
        !strcmp(mode, "filenames") || !strcmp(mode, "paths") || !strcmp(mode, "files_only")) g.mode = 0;
    else if (!strcmp(mode, "content") || !strcmp(mode, "lines") || !strcmp(mode, "matches") || !strcmp(mode, "match") || !strcmp(mode, "text")) g.mode = 1;
    else if (!strcmp(mode, "count") || !strcmp(mode, "counts") || !strcmp(mode, "count_matches")) g.mode = 2;
    else { snprintf(out, cap, "error: output_mode must be \"files_with_matches\", \"content\" or \"count\", not \"%s\"", mode); goto done; }
    if (type && *type) {
        flt.exts = ct_type_exts(type);
        if (!flt.exts) { snprintf(out, cap, "error: unknown type \"%s\". Known types: c, cpp, h, py, js, ts, rust, go, java, sh, md, json, yaml, html, css, qml, toml, txt, xml, sql, rb, php, cs, kt, swift, lua. For anything else use `glob`.", type); goto done; }
    }
    if (glob && *glob) flt.glob = glob;
    g.multi = ct_bool(js, end, "multiline");
    g.num = ct_has(js, end, "-n") ? ct_bool(js, end, "-n") : 1;
    g.A = (int)ct_int(js, end, "-A", 0); g.B = (int)ct_int(js, end, "-B", 0);
    {
        long long c = ct_int(js, end, "-C", 0);
        if (!c) c = ct_int(js, end, "context", 0);
        if (c > g.A) g.A = (int)c;
        if (c > g.B) g.B = (int)c;
        if (g.A < 0) g.A = 0;
        if (g.B < 0) g.B = 0;
        if (g.A > 50) g.A = 50;
        if (g.B > 50) g.B = 50;
    }
    g.limit = (long)ct_int(js, end, "head_limit", 250);
    if (g.limit < 0) g.limit = 250;
    g.offset = (long)ct_int(js, end, "offset", 0);
    if (g.offset < 0) g.offset = 0;
    const int baseflags = (icase ? RX_I : 0) | (g.multi ? RX_M | RX_S : 0);
    int aslit = literal;
    re = rx_compile(pat, strlen(pat), baseflags | (literal ? RX_LITERAL : 0), err, sizeof err);
    if (!re) {
        snprintf(note, sizeof note, "[pattern is not a valid regular expression (%s); searched for it as plain text]", err);
        re = rx_compile(pat, strlen(pat), baseflags | RX_LITERAL, err, sizeof err);
        aslit = 1;
    }
    if (!ct_dir_arg(path, 0, res, sizeof res, &st, out, cap)) {
        if (strncmp(out, "error: path does not exist", 26) == 0 && path && *path) {     /* sugere o unico lugar com esse nome */
            const char *bn = path + strlen(path);
            while (bn > path && bn[-1] == '/') bn--;
            const char *bs = bn;
            while (bs > path && bs[-1] != '/') bs--;
            char name[1100], first[4400] = "", root[4200];
            snprintf(name, sizeof name, "%.*s", (int)(bn - bs), bs);
            int found = 0; long visits = 0;
            if (name[0] && agent_resolve(".", root, sizeof root)) ct_find_name(root, name, 0, &found, first, sizeof first, &visits);
            if (found == 1) {
                const size_t l = strlen(out);
                snprintf(out + l, cap - l, " There is exactly one \"%s\" under the working folder: %s", name, ct_display(first, root));
            }
        }
        goto done;
    }
    if (!agent_resolve(".", cwd, sizeof cwd)) cwd[0] = 0;
    const int isdir = S_ISDIR(st.st_mode);
    if (isdir) {
        ct_grep_collect(&files, res, cwd, &flt);
        qsort(files.v, (size_t)files.n, sizeof(CtFile), g.mode == 0 ? ct_cmp_mt : ct_cmp_path);
    } else ct_files_add(&files, res, (long long)st.st_mtime);
    if (isdir && files.n == 0) {
        const char *shown = path && *path && strcmp(path, "undefined") && strcmp(path, "null") ? path : ".";
        if (flt.glob || flt.exts) {
            CtGrepFilter none; memset(&none, 0, sizeof none);
            CtFiles all; memset(&all, 0, sizeof all);
            ct_grep_collect(&all, res, cwd, &none);
            snprintf(out, cap, "%s%sNo files were searched: none of the %d file%s under %s passes the %s filter. Change or drop it.",
                     note, note[0] ? "\n" : "", all.n, all.n == 1 ? "" : "s", shown, flt.glob && flt.exts ? "`glob` and `type`" : flt.glob ? "`glob`" : "`type`");
            ct_files_free(&all);
        } else snprintf(out, cap, "%s%sNo files were searched: %s has no files that are not ignored (.git, .gitignore entries, node_modules and similar folders are skipped).",
                        note, note[0] ? "\n" : "", shown);
        goto done;
    }
    g.re = re;
    ct_grep_all(&g, &files, cwd);
    if (g.slow) { snprintf(out, cap, "error: pattern too slow on this input; simplify it (it needed more than %ld steps at %s). Avoid nested quantifiers such as (a+)+ and long .* chains.", RX_STEPS, g.slowat); goto done; }
    if (g.files_matched == 0) {
        /* 1: o padrao tem metacaracteres e, como texto simples, acha algo */
        if (!aslit && ct_has_meta(pat)) {
            Rx *lit = rx_compile(pat, strlen(pat), baseflags | RX_LITERAL, err, sizeof err);
            CtGrep h; memset(&h, 0, sizeof h);
            h.re = lit; h.mode = g.mode; h.A = g.A; h.B = g.B; h.num = g.num; h.multi = g.multi; h.limit = g.limit; h.offset = g.offset;
            ct_grep_all(&h, &files, cwd);
            if (h.files_matched > 0 && !h.slow) {
                ct_grep_format(&h, "[the regular expression matched nothing; searched for the pattern as plain text instead (escape . * + ? ( ) [ ] { } | ^ $ \\ with a backslash to match them literally, or pass \"literal\": true)]", out, cap);
                free(h.sb.p); free(h.spans); rx_free(lit);
                goto done;
            }
            free(h.sb.p); free(h.spans); rx_free(lit);
        }
        /* 2: acha ignorando a caixa */
        if (!icase) {
            Rx *ci = rx_compile(pat, strlen(pat), baseflags | RX_I | (aslit ? RX_LITERAL : 0), err, sizeof err);
            const long long c = ci ? ct_grep_count(ci, &files, cwd, g.multi, NULL) : 0;
            rx_free(ci);
            if (c > 0) { snprintf(out, cap, "%s%sNo matches found; %lld match%s when ignoring case: pass \"-i\": true", note, note[0] ? "\n" : "", c, c == 1 ? "" : "es"); goto done; }
        }
        /* 3: acha sem os filtros glob/type */
        if (isdir && (flt.glob || flt.exts)) {
            CtGrepFilter none; memset(&none, 0, sizeof none);
            CtFiles all; memset(&all, 0, sizeof all);
            ct_grep_collect(&all, res, cwd, &none);
            CtGrep h;
            const long long c = ct_grep_count(re, &all, cwd, g.multi, &h);
            ct_files_free(&all);
            if (c > 0) {
                char kinds[400] = ""; size_t kl = 0;
                for (int i = 0; i < h.next && kl + 40 < sizeof kinds; i++) kl += (size_t)snprintf(kinds + kl, sizeof kinds - kl, "%s%s (%d file%s)", i ? ", " : "", h.ext[i], h.extn[i], h.extn[i] == 1 ? "" : "s");
                snprintf(out, cap, "%s%sNo matches found in the files selected by %s; without that filter there %s %lld match%s in %d file%s: %s. Change or drop the filter.",
                         note, note[0] ? "\n" : "", flt.glob && flt.exts ? "`glob` and `type`" : flt.glob ? "`glob`" : "`type`",
                         c == 1 ? "is" : "are", c, c == 1 ? "" : "es", h.files_matched, h.files_matched == 1 ? "" : "s", kinds);
                goto done;
            }
        }
        snprintf(out, cap, "%s%sNo matches found%s", note, note[0] ? "\n" : "",
                 ct_grep_prose(pat) && !aslit ? ". This looks like a description of what you want, not text that appears in the files. To search by meaning use Find with query=\"your description\"; for exact text pass a word or a pattern." : "");
        goto done;
    }
    ct_grep_format(&g, note, out, cap);
done:
    rx_free(re);
    free(g.sb.p); free(g.spans);
    ct_files_free(&files);
    free(pat); free(path); free(glob); free(type); free(mode); free(rxs);
#endif
}
