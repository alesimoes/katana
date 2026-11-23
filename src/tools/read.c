/* ---------- ferramenta Read (ver docs/tools.md) ----------
 * Linhas com o numero na frente, faixa por offset/limit, tamanho limitado pelo cap, aviso "PARTIAL view" quando
 * o trecho pedido nao coube. Tambem guarda aqui os auxiliares de arquivo que Write, Edit e NotebookEdit usam. */

/* ---- buffer que cresce (resultados de tamanho livre) ---- */
typedef struct { char *p; size_t n, cap; } CtBuf;
static void ct_buf_add(CtBuf *b, const char *s, size_t l) {
    if (b->n + l + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < b->n + l + 1) nc *= 2;
        b->p = (char *)realloc(b->p, nc); b->cap = nc;
    }
    memcpy(b->p + b->n, s, l); b->n += l; b->p[b->n] = 0;
}
static void ct_buf_str(CtBuf *b, const char *s) { ct_buf_add(b, s, strlen(s)); }

/* ---- auxiliares de arquivo ---- */
/* caminho absoluto de file_path: relativo vale a partir da pasta de trabalho do agente; aceita ~ */
static int ct_f_abs(const char *in, char *abs, size_t cap, char *err, size_t ecap) {
    if (!in || !*in) { snprintf(err, ecap, "error: file_path is empty"); return 0; }
    return agent_write_path(in, abs, cap, err, ecap);
}
/* le o arquivo inteiro (memoria nova, com um zero no fim); NULL com a mensagem em err */
static char *ct_f_slurp(const char *abs, size_t *n, char *err, size_t ecap) {
    struct stat st;
    if (stat(abs, &st) != 0) { snprintf(err, ecap, "error: cannot read %s (%s)", abs, strerror(errno)); return NULL; }
    if (st.st_size > 100000000LL) { snprintf(err, ecap, "error: %s is too large (%lld bytes)", abs, (long long)st.st_size); return NULL; }
    FILE *f = fopen(abs, "rb");
    if (!f) { snprintf(err, ecap, "error: cannot open %s (%s)%s", abs, strerror(errno), (errno == EACCES || errno == EPERM) ? "; use Bash with sudo if you must read it as root" : ""); return NULL; }
    char *b = (char *)malloc((size_t)st.st_size + 1);
    if (!b) { fclose(f); snprintf(err, ecap, "error: out of memory"); return NULL; }
    *n = fread(b, 1, (size_t)st.st_size, f);
    fclose(f);
    b[*n] = 0;
    return b;
}
static int ct_utf8_ok(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        const unsigned char c = s[i];
        size_t l = c < 0x80 ? 1 : (c >= 0xC2 && c < 0xE0) ? 2 : (c >= 0xE0 && c < 0xF0) ? 3 : (c >= 0xF0 && c < 0xF5) ? 4 : 0;
        if (!l || i + l > n) return 0;
        for (size_t k = 1; k < l; k++) if ((s[i + k] & 0xC0) != 0x80) return 0;
        if (c == 0xE0 && s[i + 1] < 0xA0) return 0;
        if (c == 0xED && s[i + 1] >= 0xA0) return 0;
        if (c == 0xF0 && s[i + 1] < 0x90) return 0;
        if (c == 0xF4 && s[i + 1] >= 0x90) return 0;
        i += l;
    }
    return 1;
}
static long long ct_count_lines(const char *b, size_t n) {
    long long t = 0;
    for (size_t i = 0; i < n; i++) if (b[i] == '\n') t++;
    if (n && b[n - 1] != '\n') t++;
    return t;
}
static int ct_ext_is(const char *path, const char *ext) {
    const char *b = strrchr(path, '/'), *d = strrchr(path, '.');
    if (!d || (b && d < b)) return 0;
    return ci_ncmp(d, ext, strlen(ext) + 1) == 0;
}

/* ---- as linhas [offset, offset+limit) de b, com numero e tab, no maximo o que cabe em cap ---- */
#define CT_LINE_MAX 2000
static long long g_rdl_first, g_rdl_last;      /* faixa entregue pela ultima chamada de ct_rd_lines */
static void ct_rd_lines(const char *b, size_t n, long long offset, long long limit, int numbered, char *out, size_t cap, int *partial) {
    const long long total = ct_count_lines(b, n);
    size_t room = cap > 700 ? cap - 600 : 0;
    if (room > 100000) room = 100000;                  /* cerca de 25 mil tokens por chamada */
    if (room < 300) { snprintf(out, cap, "error: no room left in the context for a file view; continue after the context is compacted"); *partial = 1; return; }
    size_t lmax = room / 2 < CT_LINE_MAX ? room / 2 : CT_LINE_MAX;
    size_t s = 0, o = 0;
    long long ln = 1;
    while (ln < offset && s < n) { const char *nl = (const char *)memchr(b + s, '\n', n - s); if (!nl) { s = n; break; } s = (size_t)(nl - b) + 1; ln++; }
    long long shown = 0, first = offset, last = offset - 1;
    int by_size = 0;
    while (s < n && (!limit || shown < limit)) {
        const char *nl = (const char *)memchr(b + s, '\n', n - s);
        size_t le = nl ? (size_t)(nl - b) : n, nx = nl ? le + 1 : n;
        if (le > s && b[le - 1] == '\r') le--;
        size_t ll = le - s, cut = ll;
        if (cut > lmax) { cut = lmax; while (cut > 0 && ((unsigned char)b[s + cut] & 0xC0) == 0x80) cut--; }
        char tail[64] = "";
        if (cut < ll) snprintf(tail, sizeof tail, "... [line truncated, %zu bytes]", ll);
        const size_t need = (numbered ? 8 : 0) + cut + strlen(tail) + 1;
        if (o + need > room) { by_size = 1; break; }
        if (numbered) o += (size_t)snprintf(out + o, cap - o, "%6lld\t", offset + shown);
        memcpy(out + o, b + s, cut); o += cut;
        memcpy(out + o, tail, strlen(tail)); o += strlen(tail);
        out[o++] = '\n';
        out[o] = 0;
        s = nx; shown++; last = offset + shown - 1;
    }
    out[o] = 0;
    g_rdl_first = first; g_rdl_last = last;
    const int more = s < n;
    *partial = !(first == 1 && !more);
    if (more) {
        const long long next = last + 1;
        snprintf(out + o, cap - o, "[PARTIAL view: returned lines %lld-%lld of %lld total (stopped by the %s). To continue call Read again with offset=%lld%s.]",
                 first, last, total, by_size ? "size limit for one result" : "limit argument", next, by_size ? " and a smaller limit if needed" : "");
    } else if (first > 1) {
        snprintf(out + o, cap - o, "[lines %lld-%lld of %lld total returned; this is the end of the file]", first, last, total);
    } else if (o > 0 && out[o - 1] == '\n') out[--o] = 0;
}

/* ---- mapa de um arquivo grande (Read sem offset e sem limit) ----
 * Indice de simbolos com numero de linha, por extensao, com casadores simples escritos a mao. Serve so para o modelo saber
 * onde ler; nao e um analisador. KATANA_READ_OUTLINE=0 desliga; KATANA_READ_OUTLINE_LINES muda o limiar (200 linhas ou 8 KB). */
#define OL_MAX 60
static int ol_starts(const char *p, const char *w) { return strncmp(p, w, strlen(w)) == 0; }
static int ol_word(const char *p, const char *w) { const size_t l = strlen(w); return strncmp(p, w, l) == 0 && !isalnum((unsigned char)p[l]) && p[l] != '_'; }
static int ol_has_word(const char *p, const char *const *ws) { for (int i = 0; ws[i]; i++) if (ol_word(p, ws[i])) return 1; return 0; }
/* pula modificadores (export, public, static, async...) */
static const char *ol_skip_mods(const char *p, const char *const *mods) {
    for (int again = 1; again;) {
        again = 0;
        for (int i = 0; mods[i]; i++) if (ol_word(p, mods[i])) { p += strlen(mods[i]); while (*p == ' ') p++; again = 1; break; }
    }
    return p;
}
/* 1 se a linha (t, sem o fim de linha, com ind espacos tirados da frente) e um simbolo da linguagem ext; *kind 'd' = #define */
static int ol_match(const char *ext, const char *t, int ind, char *kind) {
    *kind = 's';
    if (!*t) return 0;
    static const char *const ctl[] = {"if", "for", "while", "switch", "return", "else", "do", "case", "goto", "sizeof", "defined", NULL};
    const size_t L = strlen(t);
    if (!strcmp(ext, "c") || !strcmp(ext, "h") || !strcmp(ext, "cpp") || !strcmp(ext, "hpp") || !strcmp(ext, "cc")) {
        if (ind) return 0;
        if (ol_starts(t, "#define ")) { *kind = 'd'; return 1; }
        if (*t == '#' || *t == '}' || *t == '/' || *t == '*' || *t == ')' || *t == '{') return 0;
        if (ol_has_word(t, ctl)) return 0;
        if (ol_word(t, "struct") || ol_word(t, "enum") || ol_word(t, "union") || ol_word(t, "class") || ol_word(t, "namespace") || ol_starts(t, "typedef struct") || ol_starts(t, "typedef enum"))
            return t[L - 1] != ';' || strchr(t, '{') != NULL;
        if (!strchr(t, '(')) return 0;
        if (t[L - 1] == ';' || t[L - 1] == ',') return 0;
        return t[L - 1] == '{' || t[L - 1] == ')' || strstr(t, ") {") || strstr(t, "){");
    }
    if (!strcmp(ext, "js") || !strcmp(ext, "mjs") || !strcmp(ext, "cjs") || !strcmp(ext, "ts") || !strcmp(ext, "tsx")) {
        if (ind > 2) return 0;
        static const char *const mods[] = {"export", "default", "async", "declare", "abstract", NULL};
        const char *q = ol_skip_mods(t, mods);
        if (ind == 0) {
            if (ol_word(q, "function") || ol_word(q, "class") || ol_word(q, "interface") || ol_word(q, "enum")) return 1;
            if (ol_word(q, "type") && strchr(q, '=')) return 1;
            if (ol_word(q, "const") || ol_word(q, "let") || ol_word(q, "var"))
                return strstr(q, "=>") || strstr(q, "function") || strstr(q, "class ") || strstr(q, "= {") || strstr(q, "= [") || strstr(q, "= new ") || strstr(q, "require(");
            return 0;
        }
        if (isalpha((unsigned char)*q) && t[L - 1] == '{' && !ol_has_word(q, ctl) && !ol_word(q, "catch") && !ol_word(q, "try")) {
            const char *a = q; while (isalnum((unsigned char)*a) || *a == '_') a++;      /* metodo de classe: nome(...) { */
            return a > q && *a == '(';
        }
        return 0;
    }
    if (!strcmp(ext, "py")) {
        if (ind > 4) return 0;
        static const char *const mods[] = {"async", NULL};
        const char *q = ol_skip_mods(t, mods);
        return ol_word(q, "def") || ol_word(q, "class");
    }
    if (!strcmp(ext, "kt") || !strcmp(ext, "java")) {
        if (ind > 4) return 0;
        static const char *const mods[] = {"public", "private", "protected", "internal", "static", "final", "abstract", "open", "override", "suspend", "data", "sealed", "inline", "default", "synchronized", NULL};
        const char *q = ol_skip_mods(t, mods);
        if (ol_word(q, "fun") || ol_word(q, "class") || ol_word(q, "interface") || ol_word(q, "object") || ol_word(q, "enum") || ol_word(q, "record")) return 1;
        if (!strcmp(ext, "java") && q != t && strchr(q, '(') && (t[L - 1] == '{' || t[L - 1] == ')') && !ol_has_word(q, ctl) && !strchr(q, '=')) return 1;
        return 0;
    }
    if (!strcmp(ext, "go")) return ind == 0 && (ol_word(t, "func") || (ol_word(t, "type") && (strstr(t, " struct") || strstr(t, " interface"))));
    if (!strcmp(ext, "rs")) {
        if (ind > 4) return 0;
        static const char *const mods[] = {"pub", "async", "unsafe", "const", "extern", NULL};
        const char *q = ol_skip_mods(t, mods);
        return ol_word(q, "fn") || ol_word(q, "struct") || ol_word(q, "enum") || ol_word(q, "trait") || ol_word(q, "impl") || ol_word(q, "mod");
    }
    if (!strcmp(ext, "sh")) {
        if (ind) return 0;
        if (ol_word(t, "function")) return 1;
        if (isalpha((unsigned char)*t) || *t == '_') {
            const char *a = t; while (isalnum((unsigned char)*a) || *a == '_' || *a == '-' || *a == ':') a++;
            const char *z = a; while (*z == ' ') z++;
            return a > t && z[0] == '(' && z[1] == ')';
        }
        if (ol_starts(t, "# ---") || ol_starts(t, "# ===") || ol_starts(t, "####") || ol_starts(t, "## ")) {      /* cabecalho de bloco: precisa ter texto */
            for (const char *a = t; *a; a++) if (isalpha((unsigned char)*a)) return 1;
        }
        return 0;
    }
    if (!strcmp(ext, "ps1")) return ind == 0 && (ol_word(t, "function") || ol_word(t, "filter") || ol_word(t, "class"));
    if (!strcmp(ext, "md")) return ind == 0 && *t == '#' && t[strspn(t, "#")] == ' ';
    if (!strcmp(ext, "qml")) {
        if (ind > 8) return 0;
        if (ol_word(t, "function") || ol_word(t, "component") || ol_word(t, "signal")) return 1;
        if (ind <= 4 && isupper((unsigned char)*t)) { const char *a = t; while (isalnum((unsigned char)*a) || *a == '.') a++; while (*a == ' ') a++; return *a == '{'; }
        return 0;
    }
    return 0;
}
/* extensoes que tem mapa; devolve o nome em ext (minusculo) */
static int ol_ext(const char *path, char *ext, size_t cap) {
    static const char *const ok[] = {"c", "h", "cpp", "hpp", "cc", "js", "mjs", "cjs", "ts", "tsx", "py", "kt", "java", "go", "rs", "sh", "md", "qml", "ps1", NULL};
    const char *b = strrchr(path, '/'), *d = strrchr(path, '.');
    if (!d || (b && d < b)) return 0;
    size_t i = 0;
    for (d++; *d && i + 1 < cap; d++) ext[i++] = (char)tolower((unsigned char)*d);
    ext[i] = 0;
    for (int k = 0; ok[k]; k++) if (!strcmp(ext, ok[k])) return 1;
    return 0;
}
/* limiar: 1 se o arquivo e grande para o Read sem faixa */
static int ol_big(long long lines, size_t bytes) {
    const char *e = getenv("KATANA_READ_OUTLINE_LINES");
    long long tl = 200, tb = 8192;
    if (e && atoll(e) > 0) { tl = atoll(e); tb = tl * 40; }
    return lines > tl || (long long)bytes > tb;
}
static int ol_on(void) { const char *e = getenv("KATANA_READ_OUTLINE"); return !(e && atoi(e) == 0); }
/* monta o texto do mapa em out (vazio se nao achou simbolo) */
static void ct_outline(const char *ext, const char *b, size_t n, char *out, size_t cap) {
    static long long el[4096]; static char et[4096][110];
    int ne = 0, fence = 0, nd = 0;
    const int md = !strcmp(ext, "md");
    long long ln = 0;
    out[0] = 0;
    for (size_t s = 0; s < n && ne < 4096;) {
        const char *nl = (const char *)memchr(b + s, '\n', n - s);
        size_t le = nl ? (size_t)(nl - b) : n;
        const size_t nx = nl ? le + 1 : n;
        if (le > s && b[le - 1] == '\r') le--;
        ln++;
        char t[400]; size_t tl = le - s; if (tl >= sizeof t) tl = sizeof t - 1;
        memcpy(t, b + s, tl); t[tl] = 0;
        s = nx;
        int ind = 0; while (t[ind] == ' ') ind++;
        if (t[ind] == '\t') continue;
        if (md && !strncmp(t, "```", 3)) { fence = !fence; continue; }
        if (md && fence) continue;
        char k;
        if (!ol_match(ext, t + ind, ind, &k)) continue;
        if (k == 'd' && ++nd > 12) continue;                     /* so os primeiros #define */
        size_t w = strlen(t + ind);
        while (w && (t[ind + w - 1] == ' ' || t[ind + w - 1] == '{')) w--;
        if (w > 100) { w = 100; while (w && ((unsigned char)t[ind + w] & 0xC0) == 0x80) w--; }
        el[ne] = ln; memcpy(et[ne], t + ind, w); et[ne][w] = 0; ne++;
    }
    if (!ne) return;
    const int step = (ne + OL_MAX - 1) / OL_MAX;
    size_t o = (size_t)snprintf(out, cap, "[map: %d symbol%s%s; line, text]\n", ne, ne == 1 ? "" : "s", step > 1 ? ", an even sample is shown" : "");
    for (int i = 0; i < ne && o + 160 < cap; i += step) o += (size_t)snprintf(out + o, cap - o, "%6lld  %s\n", el[i], et[i]);
}

/* ---- caderno .ipynb como texto (a funcao esta em notebookedit.c) ---- */
static int ct_nb_render(const char *b, size_t n, CtBuf *o, char *err, size_t ecap);

/* ---- PDF ---- */
#ifndef _WIN32
static int ct_pdf_pages(const char *pages, int *a, int *z) {
    int x = 0, y = 0;
    if (sscanf(pages, "%d-%d", &x, &y) == 2) { }
    else if (sscanf(pages, "%d", &x) == 1 && !strchr(pages, '-')) y = x;
    else return 0;
    if (x < 1 || y < x) return 0;
    *a = x; *z = y;
    return 1;
}
static void ct_read_pdf(const char *abs, const char *pages, long long offset, long long limit, char *out, size_t cap) {
    char q[4400], cmd[4800], er[300]; int rc; size_t n;
    (void)er;
    free(run_capture("command -v pdftotext >/dev/null 2>&1", 10, &rc, &n));
    if (rc != 0) { snprintf(out, cap, "error: reading PDFs needs pdftotext, which is not installed (apt install poppler-utils). Tell the user, or try Bash with another tool."); return; }
    if (!shq(abs, q, sizeof q)) { snprintf(out, cap, "error: path too long"); return; }
    int a = 0, z = 0;
    if (pages && *pages) {
        if (!ct_pdf_pages(pages, &a, &z)) { snprintf(out, cap, "error: pages must look like \"3\" or \"1-5\" (first page is 1)"); return; }
        if (z - a + 1 > 20) { snprintf(out, cap, "error: at most 20 pages per request; ask for a smaller range such as \"%d-%d\"", a, a + 19); return; }
        snprintf(cmd, sizeof cmd, "pdftotext -layout -q -f %d -l %d %s - 2>&1 </dev/null", a, z, q);
    } else {
        snprintf(cmd, sizeof cmd, "pdfinfo %s 2>/dev/null </dev/null", q);
        char *info = run_capture(cmd, 20000, &rc, &n);
        int np = 0;
        if (info && rc == 0) { const char *p = strstr(info, "Pages:"); if (p) np = atoi(p + 6); }
        free(info);
        if (np > 10) { snprintf(out, cap, "error: this PDF has %d pages; pass the pages argument with a range of at most 20 pages, for example pages=\"1-10\"", np); return; }
        snprintf(cmd, sizeof cmd, "pdftotext -layout -q %s - 2>&1 </dev/null", q);
    }
    char *t = run_capture(cmd, 4000000, &rc, &n);
    if (!t) { snprintf(out, cap, "error: cannot run pdftotext"); return; }
    if (rc != 0) snprintf(out, cap, "error: pdftotext failed (exit code %d): %.200s", rc, t);
    else if (!n) snprintf(out, cap, "(the PDF has no extractable text; it may be a scanned image, and this model cannot look at images)");
    else { int part; ct_rd_lines(t, n, offset, limit, 0, out, cap, &part); }
    free(t);
}
#endif

static const char CT_READ_DESC[] =
    "Reads a file from the local filesystem. file_path should be an absolute path. By default it returns up to 2000 lines from the start; "
    "use offset (first line, 1-based) and limit (number of lines) to read a part of a long file. Each line comes back as its line number, a tab, "
    "then the text. A large source file read without offset and limit comes back as a map of its symbols with line numbers plus its first 40 lines: read the part you need with offset and limit, or search with Grep. If the range does not fit in one result, a PARTIAL view notice tells which lines were returned and how to continue. "
    "Also reads PDFs as text (pass pages like \"1-5\", at most 20 pages at a time) and Jupyter notebooks (all cells with their text outputs). "
    "It cannot show images (this model has no vision). It cannot list directories: use Bash with ls. "
    "Read a file before you Edit it or overwrite it with Write.";
static const char CT_READ_PROPS[] =
    "\"file_path\": {\"type\": \"string\", \"description\": \"The absolute path of the file to read\"}, "
    "\"offset\": {\"type\": \"integer\", \"description\": \"The line number to start reading from (1-based). Only for files too long to read at once\"}, "
    "\"limit\": {\"type\": \"integer\", \"description\": \"The number of lines to read. Only for files too long to read at once\"}, "
    "\"pages\": {\"type\": \"string\", \"description\": \"Page range for PDF files, for example \\\"1-5\\\" (at most 20 pages per request)\"}";
static const char CT_READ_REQ[] = "\"file_path\"";

static void ct_read(const char *js, const char *end, char *out, size_t cap) {
    static char abs[4300], res[4300], err[1400];
    char *fp = ct_str(js, end, "file_path"), *pages = ct_str(js, end, "pages");
    if (!fp) { snprintf(out, cap, "error: missing file_path"); free(pages); return; }
    const int has_off = ct_has(js, end, "offset"), has_lim = ct_has(js, end, "limit");
    long long offset = ct_int(js, end, "offset", 1), limit = ct_int(js, end, "limit", 2000);
    if (offset < 1) offset = 1;
    if (limit < 1) limit = 2000;
    char *b = NULL;
    do {
        if (!ct_f_abs(fp, abs, sizeof abs, err, sizeof err)) { snprintf(out, cap, "%s", err); break; }
        struct stat st;
        if (stat(abs, &st) != 0) {
            char cwd[1200]; if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
            snprintf(out, cap, "error: %s does not exist (working folder: %s). Check the name with Glob or Bash ls.", abs, cwd);
            break;
        }
        if (S_ISDIR(st.st_mode)) { snprintf(out, cap, "error: %s is a directory, not a file. List it with Bash (ls -la %s).", abs, abs); break; }
        if (!agent_read_access(abs, 0, res, sizeof res, err, sizeof err)) { snprintf(out, cap, "%s", err); break; }
        static const char *img[] = { ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".tif", ".tiff", ".ico", ".heic", ".svgz", NULL };
        static const char *bin[] = { ".zip", ".gz", ".tgz", ".bz2", ".xz", ".7z", ".rar", ".tar", ".jar", ".xlsx", ".xls", ".docx", ".doc", ".pptx", ".ppt", ".odt", ".ods",
                                     ".exe", ".dll", ".so", ".o", ".a", ".bin", ".dylib", ".class", ".pyc", ".wasm", ".mp3", ".mp4", ".mkv", ".avi", ".mov", ".wav", ".flac", ".ogg",
                                     ".gguf", ".safetensors", ".pt", ".onnx", ".sqlite", ".db", ".iso", ".woff", ".woff2", ".ttf", ".otf", NULL };
        int hit = 0;
        for (int i = 0; img[i]; i++) if (ct_ext_is(res, img[i])) hit = 1;
        if (hit && g_vis.on) {
            char e2[300] = "";
            const int k = vis_add_file(res, e2, sizeof e2);
            if (k < 0) snprintf(out, cap, "error: cannot show %s: %s", res, e2);
            else { char nt[500]; const char *b0 = strrchr(res, '/'); vis_note(k, b0 ? b0 + 1 : res, nt, sizeof nt); snprintf(out, cap, "%s", nt); vis_pend(k); ct_seen_mark(res, 0); }
            break;
        }
        if (hit) { snprintf(out, cap, "error: %s is an image and this model has no vision input, so it cannot be looked at. If the text inside matters, try Bash with an OCR tool (tesseract) if installed; otherwise tell the user.", res); break; }
        for (int i = 0; bin[i]; i++) if (ct_ext_is(res, bin[i])) hit = 1;
        if (hit) { snprintf(out, cap, "error: %s is a binary format and cannot be read as text. Use Bash to inspect it (for example unzip -l, file, or a converter to text).", res); break; }
        if (ct_ext_is(res, ".pdf")) {
#ifdef _WIN32
            snprintf(out, cap, "error: not available on Windows yet (PDF reading)");
#else
            ct_read_pdf(res, pages, has_off ? offset : 1, has_lim ? limit : 0, out, cap);
#endif
            break;
        }
        if (st.st_size == 0) { snprintf(out, cap, "(the file %s exists but is empty)", res); ct_seen_mark(res, 0); break; }
        if (!has_off && !has_lim && st.st_size > 262144) {
            snprintf(out, cap, "error: %s is %lld bytes, too large to read whole. Read it in parts with offset and limit (for example offset=1, limit=200), or find what you need with Grep or Bash.", res, (long long)st.st_size);
            break;
        }
        size_t n = 0;
        b = ct_f_slurp(res, &n, err, sizeof err);
        if (!b) { snprintf(out, cap, "%s", err); break; }
        if (memchr(b, 0, n < 8192 ? n : 8192)) { snprintf(out, cap, "error: %s looks like a binary file (it has NUL bytes). Use Bash to inspect it (file, xxd, strings).", res); break; }
        int partial = 0;
        if (ct_ext_is(res, ".ipynb")) {
            CtBuf t = {0};
            if (!ct_nb_render(b, n, &t, err, sizeof err)) { snprintf(out, cap, "%s", err); free(t.p); break; }
            ct_rd_lines(t.p ? t.p : "", t.n, has_off ? offset : 1, has_lim ? limit : 0, 0, out, cap, &partial);
            free(t.p);
            break;                                     /* caderno: nao conta como visto (editar com NotebookEdit) */
        }
        const long long total = ct_count_lines(b, n);
        if (offset > total) {
            snprintf(out, cap, "(the file %s has %lld lines, which is fewer than the requested offset %lld; use an offset from 1 to %lld)", res, total, offset, total);
            ct_seen_mark(res, 1);
            break;
        }
        char cnote[260] = ""; long long eoff = offset;
        {   RdCur *c = rc_on() ? rc_find(res, (long long)st.st_size, (long long)st.st_mtime) : NULL;
            if (c && c->cov_end >= 1 && c->cov_end < total && (!has_off || offset == c->last_off)) {      /* ja entregue ate cov_end: segue dali */
                eoff = c->cov_end + 1;
                snprintf(cnote, sizeof cnote, "[katana: lines 1-%lld of %s were already given to you in this request; here is the rest, from line %lld]\n", c->cov_end, res, eoff);
            } }
        char oext[16];
        if (!cnote[0] && !has_off && !has_lim && ct_seen(res) < 2 && ol_on() && ol_ext(res, oext, sizeof oext) && ol_big(total, n)) {      /* arquivo grande sem faixa: mapa + comeco */
            static char mp[16000], hd[12000];
            ct_outline(oext, b, n, mp, sizeof mp);
            ct_rd_lines(b, n, 1, 40, 1, hd, sizeof hd, &partial);
            char *pm = strstr(hd, "[PARTIAL view");
            if (pm) *pm = 0;
            size_t hl = strlen(hd);
            while (hl && hd[hl - 1] == '\n') hd[--hl] = 0;
            snprintf(out, cap, "[%s: %lld lines, %zu bytes]\n%s[first 40 lines]\n%s\n[katana: file has %lld lines; this is its map and its start. "
                     "Read a part with offset and limit (40 to 80 lines), or search inside it with Grep (-n -C 2).]", res, total, n, mp, hd, total);
            ct_seen_mark(res, 1);
            rc_note(res, (long long)st.st_size, (long long)st.st_mtime, 1, 40, total);
            break;
        }
        if (!cnote[0] && !has_off && !has_lim && ct_seen(res) >= 2 && rd_delta(res, b, n, out, cap)) { ct_seen_mark(res, 0); break; }      /* ja lido inteiro nesta conversa */
        ct_rd_lines(b, n, eoff, limit, 1, out, cap, &partial);
        if (strncmp(out, "error:", 6) != 0) {
            if (g_rdl_last >= g_rdl_first) rc_note(res, (long long)st.st_size, (long long)st.st_mtime, g_rdl_first, g_rdl_last, total);
            if (cnote[0]) { const size_t ol = strlen(out), cl = strlen(cnote); if (ol + cl + 1 < cap) { memmove(out + cl, out, ol + 1); memcpy(out, cnote, cl); } } ct_seen_mark(res, partial); if (!partial) rd_delta_put(res, b, n); }
    } while (0);
    free(b); free(fp); free(pages);
}
