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
    "then the text. If the range does not fit in one result, a PARTIAL view notice tells which lines were returned and how to continue. "
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
        if (!has_off && !has_lim && ct_seen(res) >= 2 && rd_delta(res, b, n, out, cap)) { ct_seen_mark(res, 0); break; }      /* ja lido inteiro nesta conversa */
        ct_rd_lines(b, n, offset, limit, 1, out, cap, &partial);
        if (strncmp(out, "error:", 6) != 0) { ct_seen_mark(res, partial); if (!partial) rd_delta_put(res, b, n); }
    } while (0);
    free(b); free(fp); free(pages);
}
