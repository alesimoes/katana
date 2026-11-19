/* ---------- ferramentas de programacao: edit_file, glob, download_file, install_package, git, read_document ----------
 * Todas devolvem texto curto; erros comecam com "error: ". As que gravam ou executam passam por agent_act
 * (KATANA_CONFIRM=1 pede confirmacao). As que dependem de shell so existem fora do Windows. */

/* escreve o texto em out, cortado (sem partir UTF-8) no limite do contexto, com aviso de truncamento */
static void tx_emit(const char *txt, size_t tn, char *out, size_t cap) {
    size_t lim = cap > 400 ? cap - 400 : cap;
    if (lim > 12000) lim = 12000;
    if (lim > g_budget) lim = g_budget;
    if (tn <= lim) snprintf(out, cap, "%s", txt);
    else {
        while (lim > 0 && ((unsigned char)txt[lim] & 0xC0) == 0x80) lim--;
        snprintf(out, cap, "%.*s\n[truncated: showing %zu of %zu bytes]", (int)lim, txt, lim, tn);
    }
}

/* json: valor booleano (true, "true" ou 1) da chave */
static int json_bool(const char *js, const char *end, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *q = strstr(js, pat);
    if (!q || q >= end) return 0;
    q += strlen(pat);
    while (q < end && (*q == ' ' || *q == ':' || *q == '\n' || *q == '\t' || *q == '\r')) q++;
    if (q < end && *q == '"') q++;
    return q < end && (strncmp(q, "true", 4) == 0 || *q == '1');
}

/* ---- sha256 (so libc) ---- */
typedef struct { uint32_t h[8]; uint64_t n; unsigned char b[64]; size_t bl; } Sha2;
static void sha2_block(Sha2 *s, const unsigned char *p) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
        0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
        0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
        0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    uint32_t w[64], a, b, c, d, e, f, g, h;
#define ROR2(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
    for (int i = 0; i < 16; i++) w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) | ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ROR2(w[i-15], 7) ^ ROR2(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR2(w[i-2], 17) ^ ROR2(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t t1 = h + (ROR2(e, 6) ^ ROR2(e, 11) ^ ROR2(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        const uint32_t t2 = (ROR2(a, 2) ^ ROR2(a, 13) ^ ROR2(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
#undef ROR2
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void sha2_init(Sha2 *s) {
    static const uint32_t I[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memcpy(s->h, I, sizeof I); s->n = 0; s->bl = 0;
}
static void sha2_add(Sha2 *s, const unsigned char *p, size_t n) {
    s->n += n;
    while (n) {
        size_t k = 64 - s->bl; if (k > n) k = n;
        memcpy(s->b + s->bl, p, k); s->bl += k; p += k; n -= k;
        if (s->bl == 64) { sha2_block(s, s->b); s->bl = 0; }
    }
}
static void sha2_hex(Sha2 *s, char *hex) {      /* hex: 65 bytes */
    const uint64_t bits = s->n * 8;
    unsigned char pad = 0x80;
    sha2_add(s, &pad, 1);
    pad = 0;
    while (s->bl != 56) sha2_add(s, &pad, 1);
    unsigned char lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha2_add(s, lb, 8);
    for (int i = 0; i < 8; i++) snprintf(hex + 8 * i, 9, "%08x", s->h[i]);
}
static int sha2_file(const char *path, char *hex) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    Sha2 s; sha2_init(&s);
    static unsigned char buf[65536];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) sha2_add(&s, buf, r);
    fclose(f);
    sha2_hex(&s, hex);
    return 1;
}

#ifndef _WIN32
/* argumento entre aspas simples para o shell; 0 se nao couber */
static int shq(const char *s, char *o, size_t cap) {
    size_t n = 0;
    if (n + 2 >= cap) return 0;
    o[n++] = '\'';
    for (; *s; s++) {
        if (*s == '\'') { if (n + 5 >= cap) return 0; memcpy(o + n, "'\\''", 4); n += 4; }
        else { if (n + 2 >= cap) return 0; o[n++] = *s; }
    }
    o[n++] = '\'';
    o[n] = 0;
    return 1;
}

/* executa o comando (com 2>&1 dentro dele) e devolve a saida (ate maxb bytes) num buffer novo; *rc = codigo de saida */
static char *run_capture(const char *cmd, size_t maxb, int *rc, size_t *nout) {
    FILE *f = popen(cmd, "r");
    char *b = (char *)malloc(maxb + 1);
    if (!f || !b) { if (f) pclose(f); free(b); *rc = -1; *nout = 0; return NULL; }
    size_t n = 0, r;
    while (n < maxb && (r = fread(b + n, 1, maxb - n, f)) > 0) n += r;
    char sink[4096];
    while (fread(sink, 1, sizeof sink, f) > 0) ;
    const int st = pclose(f);
    *rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    b[n] = 0; *nout = n;
    return b;
}

/* inicio das ultimas `nl` linhas de s (no maximo maxb bytes) */
static const char *tail_lines(const char *s, size_t n, int nl, size_t maxb) {
    const char *e = s + n;
    while (e > s && (e[-1] == '\n' || e[-1] == '\r')) e--;
    const char *p = e;
    int k = 0;
    while (p > s && (size_t)(e - p) < maxb) { p--; if (*p == '\n' && ++k >= nl) { p++; break; } }
    return p;
}
#endif

/* ---- edit_file ---- */
static void diff_lines(char *o, size_t cap, size_t *n, char sign, const char *s) {
    int k = 0;
    const char *p = s;
    while (*p && k < 6 && *n + 200 < cap) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        *n += (size_t)snprintf(o + *n, cap - *n, "%c %.*s%s\n", sign, (int)(l < 150 ? l : 150), p, l > 150 ? "..." : "");
        k++;
        if (!e) { p += l; break; }
        p = e + 1;
    }
    if (*p && *n + 60 < cap) {
        int more = 1;
        for (const char *q = p; *q; q++) if (*q == '\n') more++;
        *n += (size_t)snprintf(o + *n, cap - *n, "%c ... (+%d more lines)\n", sign, more);
    }
}

static int json_has(const char *js, const char *end, const char *key);
static int root_write_file(const char *dest, const char *data, size_t n, int append, char *err, size_t ecap);
static char *root_read_file(const char *path, size_t *n);
static void tool_edit_file(const char *js, const char *end, char *out, size_t cap) {
    static char path[4096], eold[65536], enew[65536], wp[4300], werr[400];
    if (!json_str(js, end, "path", path, sizeof path)) { snprintf(out, cap, "error: missing path"); return; }
    if (!agent_write_path(path, wp, sizeof wp, werr, sizeof werr)) { snprintf(out, cap, "%s", werr); return; }
    snprintf(path, sizeof path, "%s", wp);
    if (!json_str(js, end, "old", eold, sizeof eold) || !eold[0]) { snprintf(out, cap, "error: missing or empty old"); return; }
    if (!json_str(js, end, "new", enew, sizeof enew)) enew[0] = 0;
    if (strlen(eold) + 8 >= sizeof eold || strlen(enew) + 8 >= sizeof enew) { snprintf(out, cap, "error: old/new too long (limit %d bytes); edit smaller pieces", (int)sizeof eold - 8); return; }
    const int all = json_bool(js, end, "replace_all");
    const int esudo = json_has(js, end, "sudo") && json_bool(js, end, "sudo");
    if (strcmp(eold, enew) == 0) { snprintf(out, cap, "error: old and new are identical"); return; }
    FILE *f = fopen(path, "rb");
    char *b = NULL;
    size_t rd = 0;
    if (!f && (errno == EACCES || errno == EPERM)) {
        if (!esudo) { snprintf(out, cap, "error: permission denied reading %s. To edit it as root call edit_file again with sudo=true (the user types the password in a dialog)", path); return; }
        b = root_read_file(path, &rd);          /* sudo=true: le como root (modal de senha) */
        if (!b) { snprintf(out, cap, "error: cannot read %s (permission denied, and sudo was not available)", path); return; }
    } else {
        if (!f) { snprintf(out, cap, "error: cannot open %s", path); return; }
        fseek(f, 0, SEEK_END);
        const long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz < 0 || sz > 8000000) { fclose(f); snprintf(out, cap, "error: file too large (max 8 MB)"); return; }
        b = (char *)malloc((size_t)sz + 1);
        if (!b) { fclose(f); snprintf(out, cap, "error: out of memory"); return; }
        rd = fread(b, 1, (size_t)sz, f);
        fclose(f);
        b[rd] = 0;
    }
    if (memchr(b, 0, rd)) { free(b); snprintf(out, cap, "error: %s looks like a binary file", path); return; }
    const size_t ol = strlen(eold), nl = strlen(enew);
    int cnt = 0;
    const char *first = NULL;
    for (const char *p = strstr(b, eold); p; p = strstr(p + ol, eold)) { if (!cnt) first = p; cnt++; }
    if (cnt == 0) { free(b); snprintf(out, cap, "error: old text not found in %s (it must match exactly, including spaces and line breaks)", path); return; }
    if (cnt > 1 && !all) { free(b); snprintf(out, cap, "error: old text appears %d times in %s; add more surrounding context to make it unique, or set replace_all", cnt, path); return; }
    int line = 1;
    for (const char *p = b; p < first; p++) if (*p == '\n') line++;
    char what[4200];
    snprintf(what, sizeof what, "editar o arquivo %s (%d troca%s)", path, all ? cnt : 1, (all ? cnt : 1) > 1 ? "s" : "");
    if (!agent_act(what, eold)) { free(b); snprintf(out, cap, "error: the user denied this action"); return; }
    char *nb = (char *)malloc(rd + (size_t)cnt * (nl > ol ? nl - ol : 0) + 1);
    if (!nb) { free(b); snprintf(out, cap, "error: out of memory"); return; }
    size_t o = 0;
    const char *p = b;
    int done = 0;
    for (const char *q = strstr(b, eold); q && (all || !done); q = strstr(p, eold)) {
        memcpy(nb + o, p, (size_t)(q - p)); o += (size_t)(q - p);
        memcpy(nb + o, enew, nl); o += nl;
        p = q + ol;
        done++;
    }
    const size_t rest = rd - (size_t)(p - b);
    memcpy(nb + o, p, rest); o += rest;
    f = fopen(path, "wb");
    if (!f && (errno == EACCES || errno == EPERM) && esudo) {          /* sudo=true: grava como root (modal de senha) */
        char rerr[400];
        if (!root_write_file(path, nb, o, 0, rerr, sizeof rerr)) { free(b); free(nb); snprintf(out, cap, "%s", rerr); return; }
    } else {
        if (!f) { free(b); free(nb); snprintf(out, cap, "error: cannot write %s (%s)%s", path, strerror(errno), errno == EACCES || errno == EPERM ? ". To write it as root call edit_file again with sudo=true" : ""); return; }
        fwrite(nb, 1, o, f);
        fclose(f);
    }
    size_t n = (size_t)snprintf(out, cap, "ok: edited %s (%d replacement%s, first at line %d)\n", path, done, done > 1 ? "s" : "", line);
    const size_t dc = cap > 2400 ? 2400 : cap;
    diff_lines(out, dc, &n, '-', eold);
    diff_lines(out, dc, &n, '+', enew);
    free(b); free(nb);
}

/* ---- glob: * (nao cruza /), ** (cruza), ? ---- */
static int glob_match(const char *p, const char *s, const char *s0) {
    while (*p) {
        if (p[0] == '*' && p[1] == '*') {
            const char *r = p + 2;
            const int slash = *r == '/';
            if (slash) r++;
            for (const char *t = s; ; t++) {
                if ((!slash || t == s0 || t[-1] == '/') && glob_match(r, t, s0)) return 1;
                if (!*t) break;
            }
            return 0;
        }
        if (*p == '*') {
            for (const char *t = s; ; t++) { if (glob_match(p + 1, t, s0)) return 1; if (!*t || *t == '/') break; }
            return 0;
        }
        if (*p == '?') { if (!*s || *s == '/') return 0; p++; s++; continue; }
        if (*p != *s) return 0;
        p++; s++;
    }
    return !*s;
}

#define GLOB_MAX 200
typedef struct { const char *pat; int slash; char *hit[GLOB_MAX]; int n, visited, full; } GlobSt;

#ifndef _WIN32
static void glob_walk(GlobSt *g, const char *disp, const char *rel, int depth) {
    DIR *d = opendir(*disp ? disp : ".");
    if (!d) return;
    struct dirent *e;
    char full[4200], rl[4200];
    while ((e = readdir(d)) != NULL) {
        if (g->full || g->visited > 100000) break;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        g->visited++;
        snprintf(full, sizeof full, "%s%s%s", disp, (!*disp || disp[strlen(disp) - 1] == '/') ? "" : "/", e->d_name);
        snprintf(rl, sizeof rl, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 20 && !sf_skip_dir(e->d_name)) glob_walk(g, full, rl, depth + 1); }
        else if (S_ISREG(st.st_mode)) {
            const char *m = g->slash ? rl : e->d_name;
            if (glob_match(g->pat, m, m)) {
                if (g->n >= GLOB_MAX) { g->full = 1; break; }
                g->hit[g->n++] = strdup(full);
            }
        }
    }
    closedir(d);
}
static int glob_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
#endif

static void tool_glob(const char *pattern, const char *root, char *out, size_t cap) {
#ifdef _WIN32
    (void)pattern; (void)root; snprintf(out, cap, "error: glob is not available on Windows yet");
#else
    char res[4200], err[1300];
    if (!pattern[0]) { snprintf(out, cap, "error: empty pattern"); return; }
    if (!agent_read_access(root, 1, res, sizeof res, err, sizeof err)) { snprintf(out, cap, "%s", err); return; }
    const int rel = root[0] != '/' && root[0] != '\\' && root[0] != '~' && !strchr(root, ':');
    char base[4200];
    snprintf(base, sizeof base, "%s", rel ? root : res);
    size_t bl = strlen(base);
    while (bl > 1 && base[bl - 1] == '/') base[--bl] = 0;
    if (strcmp(base, ".") == 0) base[0] = 0;
    const char *pat = pattern;
    while (pat[0] == '.' && pat[1] == '/') pat += 2;
    GlobSt *g = (GlobSt *)calloc(1, sizeof *g);
    if (!g) { snprintf(out, cap, "error: out of memory"); return; }
    g->pat = pat; g->slash = strchr(pat, '/') != NULL;
    glob_walk(g, base, "", 0);
    qsort(g->hit, (size_t)g->n, sizeof g->hit[0], glob_cmp);
    size_t n = 0;
    out[0] = 0;
    const size_t lim = (cap < 8000 ? cap : 8000) - 300;
    int shown = 0;
    for (int i = 0; i < g->n; i++) {
        const size_t l = strlen(g->hit[i]);
        if (n + l + 2 >= lim) break;
        n += (size_t)snprintf(out + n, cap - n, "%s\n", g->hit[i]);
        shown++;
    }
    if (!g->n) snprintf(out, cap, "no files match %s under %s (%d entries visited)", pattern, base[0] ? base : ".", g->visited);
    else if (shown < g->n || g->full) snprintf(out + n, cap - n, "[%s: showing %d files; refine the pattern or the root]", g->full ? "result limit reached" : "output limit", shown);
    for (int i = 0; i < g->n; i++) free(g->hit[i]);
    free(g);
#endif
}

/* ---- download_file ---- */
static void tool_download_file(const char *url, const char *path, char *out, size_t cap) {
#ifdef _WIN32
    (void)url; (void)path; snprintf(out, cap, "error: download_file is not available on Windows yet");
#else
    char safe[4400], qu[4600], qp[4300], part[4200], cmd[10000], hex[65], dwp[4300], dwerr[400];
    if (!agent_write_path(path, dwp, sizeof dwp, dwerr, sizeof dwerr)) { snprintf(out, cap, "%s", dwerr); return; }
    path = dwp;
    if (!url_safe(url, safe, sizeof safe)) { snprintf(out, cap, "error: invalid URL (only http and https; no control characters)"); return; }
    long mb = 2048;
    const char *e = getenv("KATANA_DL_MAX_MB");
    if (e && atol(e) > 0) mb = atol(e);
    snprintf(part, sizeof part, "%s.part", path);
    if (!shq(safe, qu, sizeof qu) || !shq(part, qp, sizeof qp)) { snprintf(out, cap, "error: URL or path too long"); return; }
    char what[8700];
    snprintf(what, sizeof what, "baixar %s para %s (limite %ld MB)", safe, path, mb);
    if (!agent_act(what, safe)) { snprintf(out, cap, "error: the user denied this action"); return; }
    snprintf(cmd, sizeof cmd,
        "(ulimit -f %ld 2>/dev/null; " CURL_BIN " -sS -L --fail --max-time 3600 --max-redirs 5 --max-filesize %ld "
        "--proto '=http,https' --proto-redir '=http,https' -A 'Mozilla/5.0 (compatible; katana-agent)' -o %s -- %s) 2>&1 </dev/null",
        mb * 2048L, mb * 1048576L, qp, qu);
    int rc; size_t n;
    char *o = run_capture(cmd, 4000, &rc, &n);
    struct stat st;
    const int have = stat(part, &st) == 0;
    if (rc != 0 || !have) {
        if (have) remove(part);
        const char *m = o ? tail_lines(o, n, 3, 300) : "";
        snprintf(out, cap, "error: download failed (curl exit code %d%s): %s", rc, rc == 63 || rc == 153 || rc == 152 ? ", file larger than KATANA_DL_MAX_MB" : "", m);
        free(o); return;
    }
    free(o);
    if ((long long)st.st_size > (long long)mb * 1048576LL) { remove(part); snprintf(out, cap, "error: file larger than KATANA_DL_MAX_MB=%ld", mb); return; }
    if (rename(part, path) != 0) { remove(part); snprintf(out, cap, "error: cannot write %s", path); return; }
    if (!sha2_file(path, hex)) strcpy(hex, "(unavailable)");
    snprintf(out, cap, "ok: downloaded %lld bytes (%.2f MB) to %s\nsha256: %s", (long long)st.st_size, (double)st.st_size / 1048576.0, path, hex);
#endif
}

/* ---- install_package ---- */
static int sudo_prepare(const char *cmd, int force);
static int sudo_ready(void);
static const char *sudo_prefix(void);
static char *run_collect(const char *cmd, int tmo, size_t maxb, int *rc);
static void tool_install_package(const char *mgr, const char *name, char *out, size_t cap) {
#ifdef _WIN32
    (void)mgr; (void)name; snprintf(out, cap, "error: install_package is not available on Windows yet");
#else
    char qn[600], cmd[2000], chk[200];
    int rc; size_t n;
    if (strcmp(mgr, "pip") && strcmp(mgr, "npm") && strcmp(mgr, "apt") && strcmp(mgr, "cargo")) { snprintf(out, cap, "error: manager must be one of pip, npm, apt, cargo"); return; }
    if (!name[0] || strlen(name) > 200 || name[0] == '-') { snprintf(out, cap, "error: invalid package name"); return; }
    for (const char *s = name; *s; s++) if (!isalnum((unsigned char)*s) && !strchr("._+-@/=:<>~^[],", *s)) { snprintf(out, cap, "error: invalid character in package name"); return; }
    shq(name, qn, sizeof qn);
    const char *bin = strcmp(mgr, "pip") == 0 ? "python3" : strcmp(mgr, "apt") == 0 ? "apt-get" : mgr;
    snprintf(chk, sizeof chk, "command -v %s >/dev/null 2>&1", bin);
    free(run_capture(chk, 10, &rc, &n));
    if (rc != 0) { snprintf(out, cap, "error: %s is not installed on this machine", bin); return; }
    const char *how = "";
    if (strcmp(mgr, "pip") == 0) {
        if (getenv("VIRTUAL_ENV") && *getenv("VIRTUAL_ENV")) { snprintf(cmd, sizeof cmd, "python3 -m pip install --disable-pip-version-check -- %s", qn); how = "(active venv)"; }
        else if (access(".venv/bin/python", X_OK) == 0) { snprintf(cmd, sizeof cmd, ".venv/bin/python -m pip install --disable-pip-version-check -- %s", qn); how = "(in ./.venv)"; }
        else { snprintf(cmd, sizeof cmd, "python3 -m pip install --user --disable-pip-version-check -- %s", qn); how = "(--user)"; }
    } else if (strcmp(mgr, "npm") == 0) snprintf(cmd, sizeof cmd, "npm install --no-fund --no-audit %s", qn);
    else if (strcmp(mgr, "apt") == 0) {
        if (!sudo_ready()) { snprintf(out, cap, "error: apt needs sudo and the password was not given"); return; }     /* abre o modal de senha */
        snprintf(cmd, sizeof cmd, "DEBIAN_FRONTEND=noninteractive %s apt-get install -y -- %s", sudo_prefix(), qn);
    }
    else snprintf(cmd, sizeof cmd, "cargo install %s", qn);
    char what[400];
    snprintf(what, sizeof what, "instalar o pacote %s com %s %s", name, mgr, how);
    if (!agent_act(what, cmd)) { snprintf(out, cap, "error: the user denied this action"); return; }
    strncat(cmd, " 2>&1 </dev/null", sizeof cmd - strlen(cmd) - 1);
    char *o;
    if (strcmp(mgr, "apt") == 0) { o = run_collect(cmd, 1800, 200000, &rc); n = o ? strlen(o) : 0; }   /* com o ambiente do askpass */
    else o = run_capture(cmd, 200000, &rc, &n);
    if (!o) { snprintf(out, cap, "error: cannot run %s", mgr); return; }
    if (strcmp(mgr, "apt") == 0 && (strstr(o, "a password is required") || strstr(o, "terminal is required") || strstr(o, "no tty present"))) {
        snprintf(out, cap, "error: sudo asks for a password, so apt cannot run unattended. Ask the user to run: sudo apt-get install %s", name);
        free(o); return;
    }
    const char *t = tail_lines(o, n, 12, 1500);
    size_t w = (size_t)snprintf(out, cap, rc == 0 ? "ok: %s install %s %s finished\n" : "error: %s install %s %s failed\n", mgr, name, how);
    w += (size_t)snprintf(out + w, cap - w, "%.*s", (int)(o + n - t), t);
    if (strstr(o, "externally-managed-environment"))
        w += (size_t)snprintf(out + w, cap - w, "\n[hint: create a virtual environment first (run_command: python3 -m venv .venv) and call install_package again; it will use ./.venv]");
    if (rc != 0) snprintf(out + w, cap - w, "\n[exit code %d]", rc);
    free(o);
#endif
}

/* ---- git: subconjunto seguro ---- */
static int git_tokens(const char *s, char (*tk)[512], int max) {      /* separa por espacos, com aspas simples/duplas; -1 se invalido */
    int n = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '\n') s++;
        if (!*s) break;
        if (n >= max) return -1;
        size_t l = 0; char q = 0;
        for (; *s && (q || (*s != ' ' && *s != '\t' && *s != '\n')); s++) {
            if (q) { if (*s == q) { q = 0; continue; } if (q == '"' && *s == '\\' && (s[1] == '"' || s[1] == '\\')) s++; }
            else if (*s == '"' || *s == '\'') { q = *s; continue; }
            if (l + 1 >= 512) return -1;
            tk[n][l++] = *s;
        }
        if (q) return -1;
        tk[n][l] = 0;
        n++;
    }
    return n;
}

static int git_has(char (*tk)[512], int n, const char *a, const char *b, const char *c) {
    for (int i = 1; i < n; i++) if ((a && strcmp(tk[i], a) == 0) || (b && strcmp(tk[i], b) == 0) || (c && strcmp(tk[i], c) == 0)) return 1;
    return 0;
}

static void tool_git_in(const char *args, char *out, size_t cap) {
#ifdef _WIN32
    (void)args; snprintf(out, cap, "error: git is not available on Windows yet");
#else
    static char tk[40][512];
    static char cmd[24000];
    const int n = git_tokens(args, tk, 40);
    if (n < 0) { snprintf(out, cap, "error: could not parse args (unbalanced quotes or too many arguments)"); return; }
    if (n == 0) { snprintf(out, cap, "error: missing git subcommand (status, diff, log, show, add, commit, branch, checkout, switch, clone, pull, fetch, push)"); return; }
    const char *sub = tk[0];
    const int unsafe = getenv("KATANA_GIT_UNSAFE") && atoi(getenv("KATANA_GIT_UNSAFE")) != 0;
    static const char *const ok_sub[] = { "status", "diff", "log", "show", "add", "commit", "branch", "checkout", "switch", "clone", "pull", "fetch", "push", NULL };
    static const char *const un_sub[] = { "reset", "clean", "rebase", "merge", "stash", "restore", "rm", "tag", "revert", NULL };
    int allowed = 0, risky = 0;
    for (int i = 0; ok_sub[i]; i++) if (strcmp(sub, ok_sub[i]) == 0) allowed = 1;
    for (int i = 0; un_sub[i]; i++) if (strcmp(sub, un_sub[i]) == 0) risky = 1;
    if (!allowed && !risky) { snprintf(out, cap, "error: git subcommand '%s' is not allowed (allowed: status, diff, log, show, add, commit, branch, checkout, switch, clone, pull, fetch, push)", sub); return; }
    const int netsub = strcmp(sub, "clone") == 0 || strcmp(sub, "pull") == 0 || strcmp(sub, "fetch") == 0 || strcmp(sub, "push") == 0;
    for (int i = 0; i < n; i++) {
        const char *t = tk[i];
        if (strncmp(t, "--upload-pack", 13) == 0 || strncmp(t, "--receive-pack", 14) == 0 || strncmp(t, "--exec", 6) == 0 ||
            strncmp(t, "--output", 8) == 0 || strncmp(t, "--config", 8) == 0 || strncmp(t, "--git-dir", 9) == 0 ||
            strncmp(t, "ext::", 5) == 0 || strncmp(t, "fd::", 4) == 0 || (i > 0 && netsub && (strcmp(t, "-u") == 0 || strcmp(t, "-c") == 0))) {
            snprintf(out, cap, "error: refused: option '%s' is not allowed in git", t); return;
        }
    }
    const char *why = NULL;
    if (risky) why = strcmp(sub, "reset") == 0 && git_has(tk, n, "--hard", NULL, NULL) ? "reset --hard" : sub;
    else if (strcmp(sub, "push") == 0) {
        if (git_has(tk, n, "--force", "-f", "--mirror") || git_has(tk, n, "--delete", NULL, NULL)) why = "push --force";
        for (int i = 1; i < n; i++) if (tk[i][0] == '+' || strncmp(tk[i], "--force", 7) == 0) why = "push --force";
    }
    else if (strcmp(sub, "branch") == 0 && git_has(tk, n, "-D", "--force", "-f")) why = "branch -D";
    else if (strcmp(sub, "checkout") == 0 && (git_has(tk, n, "-f", "--force", ".") || git_has(tk, n, "-B", NULL, NULL))) why = "checkout that discards changes";
    if (why && !unsafe) { snprintf(out, cap, "error: refused: git %s can destroy work; the user must set KATANA_GIT_UNSAFE=1 to allow it", why); return; }
    int w = snprintf(cmd, sizeof cmd, "GIT_TERMINAL_PROMPT=0 GIT_PAGER=cat GIT_EDITOR=true git --no-pager");
    for (int i = 0; i < n; i++) {
        char q[1100];
        if (!shq(tk[i], q, sizeof q) || w + (int)strlen(q) + 40 >= (int)sizeof cmd) { snprintf(out, cap, "error: arguments too long"); return; }
        w += snprintf(cmd + w, sizeof cmd - (size_t)w, " %s", q);
    }
    snprintf(cmd + w, sizeof cmd - (size_t)w, " 2>&1 </dev/null");
    const int readonly = strcmp(sub, "status") == 0 || strcmp(sub, "diff") == 0 || strcmp(sub, "log") == 0 || strcmp(sub, "show") == 0;
    if (!readonly) { char what[200]; snprintf(what, sizeof what, "executar git %s", sub); if (!agent_act(what, args)) { snprintf(out, cap, "error: the user denied this action"); return; } }
    int rc; size_t on;
    char *o = run_capture(cmd, 200000, &rc, &on);
    if (!o) { snprintf(out, cap, "error: cannot run git"); return; }
    if (rc == 127) { snprintf(out, cap, "error: git is not installed on this machine"); free(o); return; }
    if (!on) snprintf(out, cap, rc ? "[exit code %d]" : "ok (no output)", rc);
    else {
        tx_emit(o, on, out, cap > 120 ? cap - 60 : cap);
        if (rc) { const size_t l = strlen(out); snprintf(out + l, cap - l, "\n[exit code %d]", rc); }
    }
    free(o);
#endif
}

/* git em outra pasta: "-C <pasta> subcomando ..." (como o git); a pasta atual volta ao fim */
static void tool_git(const char *args, char *out, size_t cap) {
#ifdef _WIN32
    tool_git_in(args, out, cap);
#else
    const char *a = args;
    while (*a == ' ' || *a == '\t') a++;
    if (strncmp(a, "-C", 2) != 0 || (a[2] != ' ' && a[2] != '\t')) { tool_git_in(args, out, cap); return; }
    a += 2;
    while (*a == ' ' || *a == '\t') a++;
    char dir[4300]; size_t n = 0;
    const char q = (*a == '"' || *a == '\'') ? *a : 0;
    if (q) a++;
    while (*a && n < sizeof dir - 1 && (q ? *a != q : (*a != ' ' && *a != '\t'))) dir[n++] = *a++;
    dir[n] = 0;
    if (q && *a == q) a++;
    if (dir[0] == '~' && (dir[1] == 0 || dir[1] == '/')) { char h[1024], t2[4300]; agent_home(h, sizeof h); snprintf(t2, sizeof t2, "%s%s", h, dir + 1); snprintf(dir, sizeof dir, "%s", t2); }
    char old[4300];
    if (!getcwd(old, sizeof old)) old[0] = 0;
    if (chdir(dir) != 0) { snprintf(out, cap, "error: cannot open the folder %s", dir); return; }
    while (*a == ' ' || *a == '\t') a++;
    tool_git_in(a, out, cap);
    if (old[0] && chdir(old) != 0) {}
#endif
}

/* ---- read_document: PDF (pdftotext), HTML e texto ---- */
static void tool_read_document(const char *path, char *out, size_t cap) {
    char res[4200], err[1300];
    if (!agent_read_access(path, 0, res, sizeof res, err, sizeof err)) { snprintf(out, cap, "%s", err); return; }
    struct stat st;
    if (stat(res, &st) != 0 || S_ISDIR(st.st_mode)) { snprintf(out, cap, "error: cannot open %s", path); return; }
    FILE *f = fopen(res, "rb");
    if (!f) { snprintf(out, cap, "error: cannot open %s", path); return; }
    unsigned char mg[8] = {0};
    const size_t mr = fread(mg, 1, 5, f);
    fclose(f);
    if (mr >= 4 && memcmp(mg, "%PDF", 4) == 0) {
#ifdef _WIN32
        snprintf(out, cap, "error: PDF reading is not available on Windows yet");
#else
        char q[4400], cmd[4600]; int rc; size_t n;
        free(run_capture("command -v pdftotext >/dev/null 2>&1", 10, &rc, &n));
        if (rc != 0) { snprintf(out, cap, "error: pdftotext is not installed (apt install poppler-utils)"); return; }
        if (!shq(res, q, sizeof q)) { snprintf(out, cap, "error: path too long"); return; }
        snprintf(cmd, sizeof cmd, "pdftotext -layout -q %s - 2>&1 </dev/null", q);
        char *o = run_capture(cmd, 400000, &rc, &n);
        if (!o) { snprintf(out, cap, "error: cannot run pdftotext"); return; }
        if (rc != 0) snprintf(out, cap, "error: pdftotext failed (exit code %d): %.200s", rc, o);
        else if (!n) snprintf(out, cap, "(the PDF has no extractable text; it may be a scanned image)");
        else tx_emit(o, n, out, cap);
        free(o);
#endif
        return;
    }
    if (st.st_size > 20000000) { snprintf(out, cap, "error: file too large (max 20 MB)"); return; }
    char *raw = (char *)malloc((size_t)st.st_size + 1), *txt = (char *)malloc((size_t)st.st_size + 64);
    if (!raw || !txt) { free(raw); free(txt); snprintf(out, cap, "error: out of memory"); return; }
    f = fopen(res, "rb");
    const size_t rd = f ? fread(raw, 1, (size_t)st.st_size, f) : 0;
    if (f) fclose(f);
    raw[rd] = 0;
    if (memchr(raw, 0, rd < 4096 ? rd : 4096)) snprintf(out, cap, "error: %s is a binary file (supported: PDF, HTML, text)", path);
    else {
        const size_t probe = rd < 4096 ? rd : 4096;
        size_t tn;
        const char *dot = strrchr(res, '.');
        int html = dot && (ci_ncmp(dot, ".html", 6) == 0 || ci_ncmp(dot, ".htm", 5) == 0 || ci_ncmp(dot, ".xhtml", 7) == 0);
        if (!html) for (size_t i = 0; i + 5 < probe; i++) if (raw[i] == '<' && (ci_ncmp(raw + i, "<html", 5) == 0 || ci_ncmp(raw + i, "<!doc", 5) == 0)) { html = 1; break; }
        if (html) tn = html_to_text(raw, rd, txt);
        else { memcpy(txt, raw, rd); txt[rd] = 0; tn = rd; }
        if (!tn) snprintf(out, cap, "(the document has no readable text)");
        else tx_emit(txt, tn, out, cap);
    }
    free(raw); free(txt);
}

