/* ---------- ferramenta Glob (ver docs/tools.md) ----------
 * Tambem guarda o que o Grep reaproveita: casamento de padroes (* ** ? [..] {a,b}), lista de arquivos com data,
 * caminhada de pastas e texto crescente (CtSb). */
static const char CT_GLOB_DESC[] =
    "Find files by name pattern. Supports ** (any depth), * and ? (within one path segment), [abc] / [a-z] and "
    "{a,b} alternatives, e.g. \"**/*.{ts,tsx}\" or \"src/**/test_*.c\". A pattern without a slash and without a "
    "leading **/ only matches files directly inside `path`; start it with **/ to match at any depth. Returns "
    "absolute file paths sorted by modification time (newest first), at most 100 (the result says how many more "
    "matched). Gitignored files are NOT skipped; the .git folder is. Use this to locate files by name; use Grep to "
    "search file contents.";
static const char CT_GLOB_PROPS[] =
    "\"pattern\": {\"type\": \"string\", \"description\": \"The glob pattern to match files against\"}, "
    "\"path\": {\"type\": \"string\", \"description\": \"The folder to search in. Defaults to the current working folder. Omit it for the default; do not pass \\\"undefined\\\" or \\\"null\\\".\"}";
static const char CT_GLOB_REQ[] = "\"pattern\"";

/* ---- texto crescente ---- */
typedef struct { char *p; size_t n, cap; } CtSb;
static void ct_sb_add(CtSb *b, const char *s, size_t l) {
    if (b->n + l + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->n + l + 1) nc *= 2;
        b->p = (char *)realloc(b->p, nc); b->cap = nc;
    }
    memcpy(b->p + b->n, s, l); b->n += l; b->p[b->n] = 0;
}
static void ct_sb_puts(CtSb *b, const char *s) { ct_sb_add(b, s, strlen(s)); }

/* ---- casamento de padroes ----
 * * nao cruza /, ** cruza (e "**" + "/" casa zero ou mais pastas), ? um caractere, [..] classe (com ! ou ^), {a,b} alternativas,
 * \x caractere literal. s0 e o inicio do texto (para saber se um ** comeca num limite de pasta). */
static int ct_gmatch(const char *p, const char *s, const char *s0) {
    while (*p) {
        if (p[0] == '*' && p[1] == '*') {
            const char *r = p + 2;
            const int slash = *r == '/';
            if (slash) r++;
            for (const char *t = s; ; t++) {
                if ((!slash || t == s0 || t[-1] == '/') && ct_gmatch(r, t, s0)) return 1;
                if (!*t) break;
            }
            return 0;
        }
        if (*p == '*') {
            for (const char *t = s; ; t++) { if (ct_gmatch(p + 1, t, s0)) return 1; if (!*t || *t == '/') break; }
            return 0;
        }
        if (*p == '?') { if (!*s || *s == '/') return 0; p++; s++; continue; }
        if (*p == '[') {
            const char *q = p + 1;
            int neg = 0, ok = 0;
            if (*q == '!' || *q == '^') { neg = 1; q++; }
            const char *first = q;
            while (*q && (*q != ']' || q == first)) {
                if (q[1] == '-' && q[2] && q[2] != ']') { if ((unsigned char)*s >= (unsigned char)q[0] && (unsigned char)*s <= (unsigned char)q[2]) ok = 1; q += 3; }
                else { if (*s == *q) ok = 1; q++; }
            }
            if (!*q) { if (*s != '[') return 0; p++; s++; continue; }     /* sem fechar: '[' literal */
            if (!*s || *s == '/' || ok == neg) return 0;
            p = q + 1; s++;
            continue;
        }
        if (*p == '{') {
            int depth = 0; const char *q = p;
            for (; *q; q++) { if (*q == '\\' && q[1]) { q++; continue; } if (*q == '{') depth++; else if (*q == '}' && --depth == 0) break; }
            if (*q == '}') {
                const char *rest = q + 1, *a = p + 1;
                int d2 = 0;
                for (const char *t = p + 1; t <= q; t++) {
                    if (*t == '\\' && t[1]) { t++; continue; }
                    if (*t == '{') d2++;
                    else if (*t == '}' && d2 > 0) d2--;
                    if ((*t == ',' && d2 == 0) || t == q) {
                        char tmp[1100];
                        const size_t al = (size_t)(t - a), rl = strlen(rest);
                        if (al + rl < sizeof tmp) {
                            memcpy(tmp, a, al); memcpy(tmp + al, rest, rl + 1);
                            if (ct_gmatch(tmp, s, s0)) return 1;
                        }
                        a = t + 1;
                    }
                }
                return 0;
            }
        }
        if (*p == '\\' && p[1]) p++;
        if (*p != *s) return 0;
        p++; s++;
    }
    return !*s;
}

/* ---- lista de arquivos com data ---- */
typedef struct { char *path; long long mt; } CtFile;
typedef struct { CtFile *v; int n, cap; int visited, full; } CtFiles;
static void ct_files_add(CtFiles *f, const char *path, long long mt) {
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 256; f->v = (CtFile *)realloc(f->v, sizeof(CtFile) * (size_t)f->cap); }
    f->v[f->n].path = strdup(path); f->v[f->n].mt = mt; f->n++;
}
static void ct_files_free(CtFiles *f) { for (int i = 0; i < f->n; i++) free(f->v[i].path); free(f->v); memset(f, 0, sizeof *f); }
static int ct_cmp_mt(const void *a, const void *b) {
    const CtFile *x = (const CtFile *)a, *y = (const CtFile *)b;
    if (x->mt != y->mt) return x->mt < y->mt ? 1 : -1;
    return strcmp(x->path, y->path);
}
static int ct_cmp_path(const void *a, const void *b) { return strcmp(((const CtFile *)a)->path, ((const CtFile *)b)->path); }

#ifndef _WIN32
#define CT_SCAN_MAX 1000000     /* entradas visitadas, no maximo, numa busca */
#define CT_KEEP_MAX 200000      /* arquivos guardados, no maximo */
/* anda pela pasta base (absoluta). keep(ctx, rel, name) decide se o arquivo entra; skip_noise=1 pula tambem node_modules etc.
   Nao segue links para pastas. */
static void ct_walk(CtFiles *f, const char *base, const char *rel, int depth, int skip_noise,
                    int (*keep)(void *, const char *, const char *), void *ctx) {
    char dir[4200];
    snprintf(dir, sizeof dir, "%s%s%s", base, *rel ? "/" : "", rel);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (f->full) break;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (++f->visited > CT_SCAN_MAX) { f->full = 1; break; }
        char r2[4200], full[8500];
        snprintf(r2, sizeof r2, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        snprintf(full, sizeof full, "%s/%s", base, r2);
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISLNK(st.st_mode) && (stat(full, &st) != 0 || S_ISDIR(st.st_mode))) continue;
        if (S_ISDIR(st.st_mode)) {
            if (strcmp(e->d_name, ".git") == 0 || (skip_noise && sf_skip_dir(e->d_name)) || depth >= 40) continue;
            ct_walk(f, base, r2, depth + 1, skip_noise, keep, ctx);
        } else if (S_ISREG(st.st_mode)) {
            if (keep && !keep(ctx, r2, e->d_name)) continue;
            if (f->n >= CT_KEEP_MAX) { f->full = 1; break; }
            ct_files_add(f, full, (long long)st.st_mtime);
        }
    }
    closedir(d);
}

/* caminho para mostrar: relativo a pasta atual se estiver dentro dela, senao o absoluto */
static const char *ct_display(const char *abs, const char *cwd) {
    if (cwd[0] && path_under(cwd, abs) && abs[strlen(cwd)] == '/') return abs + strlen(cwd) + 1;
    return abs;
}

/* resolve e confere path; devolve 1 e res absoluto, ou 0 com a mensagem em out */
static int ct_dir_arg(const char *path, int want_dir_only, char *res, size_t rcap, struct stat *st, char *out, size_t cap) {
    char err[1300];
    if (!path || !*path || strcmp(path, "undefined") == 0 || strcmp(path, "null") == 0) path = ".";
    if (!agent_read_access(path, 1, res, rcap, err, sizeof err)) {
        struct stat t;
        if (strncmp(err, "error: cannot open", 18) == 0 && stat(path, &t) != 0)
            snprintf(out, cap, "error: path does not exist: %s. Check the spelling, or call Glob on a parent folder to see what is there.", path);
        else snprintf(out, cap, "%s", err);
        return 0;
    }
    if (stat(res, st) != 0) { snprintf(out, cap, "error: path does not exist: %s", path); return 0; }
    if (want_dir_only && !S_ISDIR(st->st_mode)) { snprintf(out, cap, "error: path is not a directory: %s. Pass the containing folder in `path` and the file name in `pattern`.", path); return 0; }
    return 1;
}

static int ct_glob_keep(void *ctx, const char *rel, const char *name) {
    (void)name;
    return ct_gmatch((const char *)ctx, rel, rel);
}
#endif

static void ct_glob(const char *js, const char *end, char *out, size_t cap) {
#ifdef _WIN32
    (void)js; (void)end; snprintf(out, cap, "error: not available on Windows yet");
#else
    if (cap < 300) { if (cap) out[0] = 0; return; }
    char *pat = ct_str(js, end, "pattern"), *path = ct_str(js, end, "path");
    char res[4200];
    struct stat st;
    if (!pat || !*pat) { snprintf(out, cap, "error: missing `pattern`. Example: {\"pattern\": \"**/*.c\"}"); goto done; }
    if ((!path || !*path) && ct_cwd_is_home() && strncmp(pat, "**/", 3) == 0) {
        snprintf(out, cap, "error: the working folder is your home folder, not a project, so a search of everything in it would only return files of other programs. "
                 "Set `path` to the folder of the project the user means (the user may have named it: find where it is with Bash, for example find / -maxdepth 4 -type d -name NAME 2>/dev/null), or use a narrower pattern.");
        goto done;
    }
    if (!ct_dir_arg(path, 1, res, sizeof res, &st, out, cap)) goto done;
    {
        const char *p = pat;
        while (p[0] == '.' && p[1] == '/') p += 2;
        if (p[0] == '/') {      /* padrao absoluto: vale se estiver dentro de path */
            const size_t bl = strlen(res);
            if (strncmp(p, res, bl) == 0 && p[bl] == '/') p += bl + 1;
            else { snprintf(out, cap, "error: absolute pattern is outside the search folder %s. Use a relative pattern, or set `path`.", res); goto done; }
        }
        CtFiles f; memset(&f, 0, sizeof f);
        ct_walk(&f, res, "", 0, 0, ct_glob_keep, (void *)p);
        qsort(f.v, (size_t)f.n, sizeof(CtFile), ct_cmp_mt);
        if (f.n == 0) snprintf(out, cap, "No files found");
        else {
            CtSb sb = {0};
            int shown = 0;
            const size_t lim = cap - 260;
            for (int i = 0; i < f.n && i < 100; i++) {
                const size_t l = strlen(f.v[i].path);
                if (sb.n + l + 1 > lim) break;
                ct_sb_add(&sb, f.v[i].path, l); ct_sb_add(&sb, "\n", 1);
                shown++;
            }
            if (shown < f.n) {
                char tail[240];
                snprintf(tail, sizeof tail, "(Results are truncated: showing %d of %d%s matching files. Use a more specific `pattern` or `path` to narrow the search.)",
                         shown, f.n, f.full ? "+" : "");
                ct_sb_puts(&sb, tail);
            } else if (sb.n) sb.p[--sb.n] = 0;
            snprintf(out, cap, "%s", sb.p ? sb.p : "");
            free(sb.p);
        }
        ct_files_free(&f);
    }
done:
    free(pat); free(path);
#endif
}
