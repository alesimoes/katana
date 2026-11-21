/* ---------- ferramenta Grep (nomes e argumentos do Claude Code; ver docs/claude-tools.md) ----------
 * Motor proprio com regcomp (POSIX estendido); nao chama rg (decisao: uma so sintaxe, a que a descricao promete, e o
 * mesmo resultado com ou sem rg instalado). Lista de arquivos: "git ls-files" (rastreados + nao rastreados que nao
 * estao no .gitignore) quando a pasta esta num repositorio; senao uma caminhada que le o .gitignore da pasta
 * (padroes simples: sem negacao '!'), pulando tambem node_modules, .venv etc. Um arquivo passado em path sempre e lido. */
static const char CT_GREP_DESC[] =
    "Search file contents with a regular expression. The pattern uses POSIX extended syntax (like grep -E), plus "
    "\\d \\D \\w \\W \\s \\S \\b \\t \\n. It does NOT support lookahead/lookbehind, lazy quantifiers (*?), (?i) flags or "
    "named groups; escape literal braces and parentheses, e.g. \"func main\\(\\) \\{\". output_mode: "
    "\"files_with_matches\" (default; only paths, newest first), \"content\" (matching lines as path:line:text, with "
    "-A/-B/-C context lines), \"count\" (matching lines per file and a total). `glob` (e.g. \"*.{ts,tsx}\") and `type` "
    "(c, cpp, h, py, js, ts, rust, go, java, sh, md, json, yaml, html, css, qml, ...) filter files. `path` is a folder "
    "or one file (default: the current folder). Binary files, .git and gitignored files are skipped (a file given as "
    "`path` is always searched). Lines are matched one by one; with multiline=true the pattern may span lines (. matches "
    "newlines, ^ and $ only at the start and end of the file). Output is capped by head_limit (default 250, 0 = no "
    "limit) and skipped by offset. Use this instead of grep in Bash.";
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

/* filtros dos arquivos (glob, tipo, .gitignore simples) */
typedef struct { const char *glob, *exts; char **gi; int ngi; char *gibuf; } CtGrepFilter;

/* ignorado pelo .gitignore lido? rel e o caminho relativo a pasta */
static int ct_gi_ignored(const CtGrepFilter *f, const char *rel) {
    for (int k = 0; k < f->ngi; k++) {
        const char *pat = f->gi[k];
        const int dironly = pat[0] == '\\';        /* marca interna: padrao so de pasta */
        if (dironly) pat++;
        const int anchored = strchr(pat, '/') != NULL;
        if (pat[0] == '/') pat++;
        for (const char *c = rel; *c; ) {
            const char *sl = strchr(c, '/');
            const int last = sl == NULL;
            const size_t cl = last ? strlen(c) : (size_t)(sl - c);
            if (!(dironly && last)) {
                char cand[4200], nm[1100];
                const size_t pre = (size_t)(c - rel) + cl;
                if (pre < sizeof cand && cl < sizeof nm) {
                    memcpy(cand, rel, pre); cand[pre] = 0;
                    memcpy(nm, c, cl); nm[cl] = 0;
                    if (anchored ? ct_gmatch(pat, cand, cand) : ct_gmatch(pat, nm, nm)) return 1;
                }
            }
            if (last) break;
            c = sl + 1;
        }
    }
    return 0;
}
static int ct_grep_keep(void *ctx, const char *rel, const char *name) {
    const CtGrepFilter *f = (const CtGrepFilter *)ctx;
    if (f->exts && !ct_ext_in(name, f->exts)) return 0;
    if (f->glob) {
        if (strchr(f->glob, '/')) { if (!ct_gmatch(f->glob, rel, rel)) return 0; }
        else if (!ct_gmatch(f->glob, name, name)) return 0;
    }
    if (f->ngi && ct_gi_ignored(f, rel)) return 0;
    return 1;
}
static void ct_gi_load(CtGrepFilter *f, const char *base) {
    char gp[4300];
    snprintf(gp, sizeof gp, "%s/.gitignore", base);
    FILE *fp = fopen(gp, "rb");
    if (!fp) return;
    char *buf = (char *)malloc(1 << 20);
    const size_t n = fread(buf, 1, (1 << 20) - 1, fp);
    fclose(fp);
    buf[n] = 0;
    f->gibuf = buf;
    f->gi = (char **)calloc(4096, sizeof(char *));
    for (char *line = strtok(buf, "\n"); line && f->ngi < 4096; line = strtok(NULL, "\n")) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = 0;
        if (!l || line[0] == '#' || line[0] == '!') continue;
        int dir = 0;
        if (line[l - 1] == '/') { line[--l] = 0; dir = 1; }
        if (!l) continue;
        if (dir) {      /* guarda a marca de "so pasta" como um '\' na frente (a copia cabe: o texto original e maior) */
            memmove(line + 1, line, l + 1);
            line[0] = '\\';
            /* o proximo strtok continua depois do terminador original; como andamos um byte, reposiciona */
            f->gi[f->ngi++] = line;
            line[l + 1] = 0;
            continue;
        }
        f->gi[f->ngi++] = line;
    }
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

/* lista de arquivos de uma pasta: git se possivel. *how recebe o nome do metodo usado */
static void ct_grep_collect(CtFiles *f, const char *base, CtGrepFilter *flt, const char **how) {
    char q[8400], cmd[9000];
    ct_shq(base, q, sizeof q);
    snprintf(cmd, sizeof cmd, "git -C %s -c core.quotepath=off ls-files -z --cached --others --exclude-standard 2>/dev/null | tr '\\000' '\\n'", q);
    int rc = 0;
    char *o = run_collect(cmd, 60, 32u << 20, &rc);
    if (o && rc == 0 && o[0]) {
        *how = "git ls-files";
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
    *how = ".gitignore walk";
    ct_gi_load(flt, base);
    ct_walk(f, base, "", 0, 1, ct_grep_keep, flt);
}

/* ---- busca ---- */
typedef struct {
    regex_t re;
    int mode;               /* 0 arquivos, 1 conteudo, 2 contagem */
    int A, B, num, multi;
    long offset, limit, ent, emitted;
    int more, stop, any_out;
    long long total;        /* linhas (ou casamentos) em todos os arquivos */
    int files_matched;
    CtSb sb;
} CtGrep;

static void ct_gemit(CtGrep *g, const char *text, size_t tl) {
    if (g->stop) return;
    if (g->ent++ < g->offset) return;
    if (g->limit && g->emitted >= g->limit) { g->more = 1; if (g->mode == 1) g->stop = 1; return; }
    ct_sb_add(&g->sb, text, tl); ct_sb_add(&g->sb, "\n", 1);
    g->emitted++; g->any_out = 1;
}

/* barra o padrao: \d \D \t \n viram o equivalente POSIX fora de colchetes; o resto passa */
static char *ct_re_translate(const char *p) {
    char *o = (char *)malloc(strlen(p) * 4 + 8);
    size_t n = 0;
    for (; *p; p++) {
        if (*p == '[') {
            o[n++] = *p++;
            if (*p == '^') o[n++] = *p++;
            if (*p == ']') o[n++] = *p++;
            while (*p && *p != ']') {
                if (p[0] == '[' && (p[1] == ':' || p[1] == '.' || p[1] == '=')) {
                    const char d = p[1];
                    o[n++] = *p++; o[n++] = *p++;
                    while (*p && !(p[0] == d && p[1] == ']')) o[n++] = *p++;
                    if (*p) { o[n++] = *p++; o[n++] = *p++; }
                    continue;
                }
                o[n++] = *p++;
            }
            if (!*p) break;
            o[n++] = ']';
            continue;
        }
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'd': memcpy(o + n, "[0-9]", 5); n += 5; break;
                case 'D': memcpy(o + n, "[^0-9]", 6); n += 6; break;
                case 't': o[n++] = '\t'; break;
                case 'n': o[n++] = '\n'; break;
                default: o[n++] = '\\'; o[n++] = *p; break;
            }
            continue;
        }
        if (*p == '{') {        /* so e intervalo se for {n}, {n,} ou {n,m}; senao e uma chave literal (codigo tem muitas) */
            const char *q = p + 1;
            while (isdigit((unsigned char)*q)) q++;
            const int digits = q > p + 1;
            if (digits && *q == ',') { q++; while (isdigit((unsigned char)*q)) q++; }
            if (!(digits && *q == '}')) { o[n++] = '\\'; o[n++] = '{'; continue; }
        }
        o[n++] = *p;
    }
    o[n] = 0;
    return o;
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
    long nl = 0;
    for (size_t i = 0; i < n; i++) if (buf[i] == '\n') nl++;
    if (n && buf[n - 1] != '\n') nl++;
    size_t *ls = (size_t *)malloc(sizeof(size_t) * (size_t)(nl + 2));
    size_t *le = (size_t *)malloc(sizeof(size_t) * (size_t)(nl + 2));
    long k = 0;
    for (size_t i = 0; i < n; ) {
        size_t e = i;
        while (e < n && buf[e] != '\n') e++;
        ls[k] = i; le[k] = e - i; k++;
        i = e + 1;
    }
    unsigned char *show = (unsigned char *)calloc((size_t)nl + 1, 1);     /* 1 casamento, 2 contexto */
    long nmatch = 0;
    int ok_file = 0;
    if (!g->multi) {
        for (long i = 0; i < nl; i++) {
            const char saved = buf[ls[i] + le[i]];
            buf[ls[i] + le[i]] = 0;
            const int hit = regexec(&g->re, buf + ls[i], 0, NULL, 0) == 0;
            buf[ls[i] + le[i]] = saved;
            if (hit) { show[i] = 1; nmatch++; if (g->mode == 0) break; }
        }
    } else {
        regmatch_t pm[1];
        size_t p = 0;
        while (p <= n) {
            if (regexec(&g->re, buf + p, 1, pm, p ? REG_NOTBOL : 0) != 0) break;
            const size_t so = p + (size_t)pm[0].rm_so, eo = p + (size_t)pm[0].rm_eo;
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
    ok_file = nmatch > 0;
    if (ok_file) {
        const char *disp = ct_display(abs, cwd);
        g->files_matched++;
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
#endif

static void ct_grep(const char *js, const char *end, char *out, size_t cap) {
#ifdef _WIN32
    (void)js; (void)end; snprintf(out, cap, "error: not available on Windows yet");
#else
    if (cap < 600) { if (cap) out[0] = 0; return; }
    char *pat = ct_str(js, end, "pattern"), *path = ct_str(js, end, "path"), *glob = ct_str(js, end, "glob");
    char *type = ct_str(js, end, "type"), *mode = ct_str(js, end, "output_mode"), *tr = NULL;
    CtGrep g; memset(&g, 0, sizeof g);
    CtFiles files; memset(&files, 0, sizeof files);
    CtGrepFilter flt; memset(&flt, 0, sizeof flt);
    char res[4200], cwd[4200];
    struct stat st;
    int compiled = 0;
    if (!pat || !*pat) { snprintf(out, cap, "error: missing `pattern`. Example: {\"pattern\": \"TODO\", \"output_mode\": \"content\"}"); goto done; }
    if (!mode || !*mode || strcmp(mode, "files_with_matches") == 0) g.mode = 0;
    else if (strcmp(mode, "content") == 0) g.mode = 1;
    else if (strcmp(mode, "count") == 0) g.mode = 2;
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
    tr = ct_re_translate(pat);
    {
        const int rc = regcomp(&g.re, tr, REG_EXTENDED | (ct_bool(js, end, "-i") ? REG_ICASE : 0));
        if (rc) {
            char eb[200];
            regerror(rc, &g.re, eb, sizeof eb);
            snprintf(out, cap, "error: invalid regular expression: %s. The syntax is POSIX extended (like grep -E): no lookahead/lookbehind, no lazy quantifiers; escape literal ( ) { } [ ] . * + ? | with a backslash.", eb);
            goto done;
        }
        compiled = 1;
    }
    if (!ct_dir_arg(path, 0, res, sizeof res, &st, out, cap)) goto done;
    if (!agent_resolve(".", cwd, sizeof cwd)) cwd[0] = 0;
    if (S_ISDIR(st.st_mode)) {
        const char *how = "";
        ct_grep_collect(&files, res, &flt, &how);
        qsort(files.v, (size_t)files.n, sizeof(CtFile), g.mode == 0 ? ct_cmp_mt : ct_cmp_path);
    } else ct_files_add(&files, res, (long long)st.st_mtime);
    for (int i = 0; i < files.n && !g.stop; i++) ct_grep_file(&g, files.v[i].path, cwd);
    {
        char head[200] = "", tail[420] = "";
        if (g.files_matched == 0) { snprintf(out, cap, "No matches found"); goto done; }
        if (g.emitted == 0 && g.ent > 0) { snprintf(out, cap, "No entries at this offset (the result has %ld entries; offset=%ld).", g.ent, g.offset); goto done; }
        if (g.mode == 0) snprintf(head, sizeof head, "Found %d file%s\n", g.files_matched, g.files_matched == 1 ? "" : "s");
        else if (g.mode == 2) snprintf(tail, sizeof tail, "Found %lld total match%s across %d file%s.", g.total, g.total == 1 ? "" : "es", g.files_matched, g.files_matched == 1 ? "" : "s");
        char note[300] = "";
        if (g.more) snprintf(note, sizeof note, "(Output limited to %ld entries starting at offset %ld; more results exist. Use offset=%ld to continue, or narrow with path, glob or type.)",
                             g.limit, g.offset, g.offset + g.limit);
        size_t budget = cap - 1 - strlen(head) - strlen(tail) - strlen(note) - 200;
        int cut = 0;
        if (g.sb.n > budget) {
            size_t b = budget;
            while (b > 0 && g.sb.p[b - 1] != '\n') b--;
            g.sb.n = b; if (g.sb.p) g.sb.p[b] = 0; cut = 1;
        }
        size_t n = (size_t)snprintf(out, cap, "%s%s", head, g.sb.p ? g.sb.p : "");
        if (cut) n += (size_t)snprintf(out + n, cap - n, "(Output truncated at the size limit. Narrow with path, glob or type, use head_limit and offset to page, or search with a more specific pattern.)\n");
        if (note[0]) n += (size_t)snprintf(out + n, cap - n, "%s\n", note);
        if (tail[0]) n += (size_t)snprintf(out + n, cap - n, "%s\n", tail);
        while (n > 0 && out[n - 1] == '\n') out[--n] = 0;
    }
done:
    if (compiled) regfree(&g.re);
    free(g.sb.p); free(tr); free(flt.gibuf); free(flt.gi);
    ct_files_free(&files);
    free(pat); free(path); free(glob); free(type); free(mode);
#endif
}
