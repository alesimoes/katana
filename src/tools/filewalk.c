/* ---------- tools/filewalk.c: apoio de arquivos ----------
 * Guarda o que o Grep e o UIOE usam: casamento de padroes (* ** ? [..] {a,b}), lista de arquivos com data e texto crescente (CtSb). */
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
            snprintf(out, cap, "error: path does not exist: %s. Check the spelling, or list the parent folder with Bash ls to see what is there.", path);
        else snprintf(out, cap, "%s", err);
        return 0;
    }
    if (stat(res, st) != 0) { snprintf(out, cap, "error: path does not exist: %s", path); return 0; }
    if (want_dir_only && !S_ISDIR(st->st_mode)) { snprintf(out, cap, "error: path is not a directory: %s. Pass the containing folder.", path); return 0; }
    return 1;
}

#endif
