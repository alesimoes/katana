/* ---------- coder/maint.c: manutencao do indice e ignorados transparentes (fase 8 de docs/code-index-lifecycle.md: D.10, F.5, F.6, G, H) ----------
 * Subcomandos: katana coder list | forget | gc | ignored | include | exclude. Ganchos do rastreador: maint_on_crawl_start/end (chamados por crawl.c).
 *   - ignorados: a varredura (st_scan_walk, storedocs.c) diz a um IgnSink cada pasta/arquivo deixado de fora com o MOTIVO; a "passada de contagem"
 *     (ign_scan) e essa mesma varredura sem listar arquivos, mais readdir+lstat dentro de cada pasta ignorada (nada e lido; teto de 200 mil entradas ou
 *     1 s por pasta; .git/.hg/.svn/.katana nunca contam). O resultado e o arquivo `ignored` ao lado do store (texto, 0600). Roda ao fim de `katana coder
 *     index` e ao fim de uma rodada do rastreador (prioridade mais baixa); antes disso `status` diz "not counted yet". Refeita por pasta so se o mtime
 *     da pasta mudou. API de leitura para a fase 6 (frase do NOT FOUND e Symbols tree): ign_reg_load, ign_reg_largest, ign_reg_find_dir, ign_reg_free.
 *   - armazenamento: versao do formato (diferente e menor = apaga e reconstroi, mais nova = nao mexe), dano (quarentena store.bad-<hora>, uma guardada) e disco
 *     cheio, tudo em maint_store_prepare/maint_disk_ok, chamados por store_catalog_update; gc diario automatico e conferencia de checksums a cada
 *     7 dias no rastreador.
 * Decisoes do implementador (o documento deixava abertas):
 *   - as raizes com init tambem tem registro (~/.katana/coder/inits/<chave>.root, roots.c); sem ele `list` nao as acharia. Raizes iniciadas por versoes
 *     anteriores so aparecem depois da primeira consulta delas.
 *   - o registro de ignorados fica em arquivo proprio e nao em secoes do store: o rastreador (crawl.c) nao e desta fase e nao grava STS_RULES; a
 *     varredura sincrona e a passada de contagem usam as mesmas regras (ign_scan = st_scan_walk), o rastreador ainda usa as suas (ver relatorio).
 *   - o rastreador nao diz a maint.c a raiz que indexa: ela e lida de /proc/self/cmdline (so Linux).
 *   - gc nunca toca em raiz com init: nem o store, nem as copias store.bad-*. */
#ifndef _WIN32
#include <sys/statvfs.h>
#ifdef __linux__
#include <sys/vfs.h>
#endif
/* g_maint_rebuilt (0, 1 = formato diferente apagado, 3 = dano: quarentena; a fase 6 mostra "[index was damaged and is being rebuilt]") e g_maint_quarantine: declarados em storedocs.c */
static int cw_is_running(const char *dir, long *pid);          /* crawl.c */
static int coder_crawl_main(int argc, char **argv);            /* crawl.c */

/* ---------- utilidades ---------- */
static uint64_t mt_dirbytes(const char *p) {
    struct stat st; if (lstat(p, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (uint64_t)st.st_size;
    uint64_t t = 0; DIR *d = opendir(p); if (!d) return 0;
    struct dirent *de; while ((de = readdir(d))) { if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue; char c[4700]; snprintf(c, sizeof c, "%s/%s", p, de->d_name); t += mt_dirbytes(c); }
    closedir(d); return t;
}
static const char *mt_ago(time_t t, char *o, size_t cap) {
    long s = (long)(time(NULL) - t); if (s < 0) s = 0;
    if (s < 90) snprintf(o, cap, "just now"); else if (s < 5400) snprintf(o, cap, "%ld min ago", s / 60); else if (s < 172800) snprintf(o, cap, "%ld h ago", s / 3600); else snprintf(o, cap, "%ld d ago", s / 86400);
    return o;
}
static const char *mt_hum(uint64_t b, char *o, size_t cap) {
    if (b >= (1ull << 30)) snprintf(o, cap, "%.2f GiB", b / 1073741824.0); else if (b >= (1u << 20)) snprintf(o, cap, "%.1f MiB", b / 1048576.0); else if (b >= 1024) snprintf(o, cap, "%.1f KiB", b / 1024.0); else snprintf(o, cap, "%llu B", (unsigned long long)b);
    return o;
}
static void mt_commas(long long v, char *o, size_t cap) {
    char t[40]; snprintf(t, sizeof t, "%lld", v); int l = (int)strlen(t), k = 0; char r[60]; int n = 0;
    for (int i = l - 1; i >= 0; i--) { r[n++] = t[i]; if (++k % 3 == 0 && i > 0 && t[i - 1] != '-') r[n++] = ','; }
    for (int i = 0; i < n; i++) o[i] = r[n - 1 - i];
    o[n] = 0; (void)cap;
}
static void mt_jstr(const char *s) {
    putchar('"');
    for (; *s; s++) { unsigned char c = (unsigned char)*s; if (c == '"' || c == '\\') { putchar('\\'); putchar(c); } else if (c < 32) printf("\\u%04x", c); else putchar(c); }
    putchar('"');
}
static void mt_dirname(const char *p, char *o, size_t cap) { snprintf(o, cap, "%s", p); size_t l = strlen(o); while (l > 1 && o[l - 1] == '/') o[--l] = 0; char *sl = strrchr(o, '/'); if (sl && sl != o) *sl = 0; else if (sl) o[1] = 0; }
/* aviso sobre o sistema de arquivos do armazenamento (F.3): permissoes ignoradas, rede ou FUSE */
static const char *mt_fs_note(const char *path) {
#ifdef __linux__
    char p[4500]; snprintf(p, sizeof p, "%s", path);
    struct statfs s; struct stat st;
    while (stat(p, &st) != 0) { char *sl = strrchr(p, '/'); if (!sl || sl == p) return NULL; *sl = 0; }
    if (statfs(p, &s) != 0) return NULL;
    switch ((unsigned long)s.f_type) {
        case 0x7366746eul: case 0x5346544eul: return "this filesystem (ntfs) does not enforce file modes: the index files are readable by whoever can read the disk; keep the store in the home index or do not run init here";
        case 0x4d44ul: case 0x65735546ul: return "FUSE or FAT filesystem: modes are not enforced and locks or mapping may not be reliable";
        case 0x6969ul: case 0x517bul: case 0xff534d42ul: case 0xfe534d42ul: return "network filesystem: locks and mapping are not trusted here; the index is better kept in the home index";
        default: return NULL;
    }
#else
    (void)path; return NULL;
#endif
}

/* ---------- versao do formato, dano e disco (F.6, G) ---------- */
/* 0 = ok ou vazio, 1 = formato diferente do deste binario e menor (apaga e reconstroi, sem aviso), 2 = mais novo que este binario (nao mexe), 3 = danificado (quarentena e reconstroi) */
static int mt_store_class(const char *dir, char *err, size_t ecap) {
    char mp[4600]; snprintf(mp, sizeof mp, "%s/MANIFEST", dir); struct stat sv;
    if (stat(mp, &sv) != 0) return 0;
    uint8_t *mb = NULL; size_t ml = 0;
    if (st_read_file(mp, &mb, &ml) != 0) { snprintf(err, ecap, "MANIFEST unreadable"); return 3; }
    if (ml < 8 || st_get_u32(mb) != STORE_MAN_MAGIC) { free(mb); snprintf(err, ecap, "MANIFEST: bad magic number"); return 3; }
    const uint32_t v = st_get_u32(mb + 4); free(mb);
    if (v > STORE_FORMAT) { snprintf(err, ecap, "format %u is newer than this binary (%d)", v, STORE_FORMAT); return 2; }
    if (v < STORE_FORMAT) { snprintf(err, ecap, "format %u is not %d", v, STORE_FORMAT); return 1; }
    for (int a = 0; a < 3; a++) {                      /* uma compactacao concorrente pode estar trocando segmentos: tenta de novo antes de condenar */
        StoreReader *r; if (store_open(dir, &r, err, ecap) == 0) { store_close(r); return 0; }
        if (strstr(err, "mais novo")) return 2;
        usleep(100000);
    }
    return 3;
}
/* antes de escrever: formato diferente ou dano = reconstroi (quarentena store.bad-<hora>, uma so guardada; formato diferente e apagado), mais novo = deixa quieto.
 * 0 = segue, 1 = outro processo escreve, -1 = nao mexer (err). Pega o LOCK do store para nao trocar a pasta debaixo de um escritor */
static int mt_prepare(const char *dir, char *err, size_t ecap, int force) {
    char e2[300] = ""; int c = force ? force : mt_store_class(dir, e2, sizeof e2);
    if (c == 0) return 0;
    if (c == 2) { snprintf(err, ecap, "the index in %s was written by a newer katana (%s); it was left untouched", dir, e2); return -1; }
    char lp[4700]; snprintf(lp, sizeof lp, "%s/LOCK", dir);
    int fd = open(lp, O_RDWR | O_CREAT, 0600); if (fd < 0) { snprintf(err, ecap, "LOCK: %s", strerror(errno)); return -1; }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); snprintf(err, ecap, "another process is writing the index"); return 1; }
    c = force ? force : mt_store_class(dir, e2, sizeof e2);
    if (c == 0) { flock(fd, LOCK_UN); close(fd); return 0; }
    char par[4500], base[300]; mt_dirname(dir, par, sizeof par);
    { const char *sl = strrchr(dir, '/'); snprintf(base, sizeof base, "%s", sl ? sl + 1 : dir); }
    if (c == 3) {
        DIR *d = opendir(par);                         /* uma copia danificada so: as anteriores saem */
        if (d) { struct dirent *de; const size_t bl = strlen(base);
            while ((de = readdir(d))) if (!strncmp(de->d_name, base, bl) && !strncmp(de->d_name + bl, ".bad-", 5)) { char o[4700]; snprintf(o, sizeof o, "%s/%s", par, de->d_name); rt_rmtree(o); }
            closedir(d); }
        snprintf(g_maint_quarantine, sizeof g_maint_quarantine, "%s/%s.bad-%lld", par, base, (long long)time(NULL));
        if (rename(dir, g_maint_quarantine) != 0) { rt_rmtree(dir); g_maint_quarantine[0] = 0; }
    } else rt_rmtree(dir);
    flock(fd, LOCK_UN); close(fd);
    st_mkdirs(dir);
    { char gi[4700]; snprintf(gi, sizeof gi, "%s/.gitignore", dir); FILE *f = fopen(gi, "w"); if (f) { fputs("*\n", f); fclose(f); chmod(gi, 0600); } }
    g_maint_rebuilt = c; g_maint_rebuilds++;
    if (c == 3) fprintf(stderr, "[index was damaged and is being rebuilt]\n");
    return 0;
}
static int maint_store_prepare(const char *dir, char *err, size_t ecap) { return mt_prepare(dir, err, ecap, 0); }
static void maint_force_rebuild(const char *dir) { char err[300]; mt_prepare(dir, err, sizeof err, 3); }      /* conferencia de checksums achou dano */
/* espaco livre para gravar: 2 vezes o estimado mais KATANA_INDEX_MIN_FREE_MB (padrao 64 MiB). 1 = ok */
static int maint_disk_ok(const char *dir, uint64_t need, char *err, size_t ecap) {
    char p[4500]; snprintf(p, sizeof p, "%s", dir); struct stat st; struct statvfs v;
    while (stat(p, &st) != 0) { char *sl = strrchr(p, '/'); if (!sl || sl == p) return 1; *sl = 0; }
    if (statvfs(p, &v) != 0) return 1;
    const char *e = getenv("KATANA_INDEX_MIN_FREE_MB"); const uint64_t minfree = (e && *e ? strtoull(e, NULL, 10) : 64ull) << 20;
    const uint64_t avail = (uint64_t)v.f_bavail * (uint64_t)v.f_frsize;
    if (avail < need * 2 + minfree) { snprintf(err, ecap, "paused: disk full (%llu MiB free, the index needs about %llu MiB plus %llu MiB of margin); the previous index stays valid",
                                                (unsigned long long)(avail >> 20), (unsigned long long)((need * 2) >> 20), (unsigned long long)(minfree >> 20)); return 0; }
    return 1;
}

/* ---------- registro de ignorados ---------- */
typedef struct { char *rel, *rule; int reason, isdir, state; int64_t files; uint64_t bytes; int64_t mtime; } IgnRec;      /* state: 0 nao contada, 1 contada, 2 passou do teto (>= files) */
typedef struct { IgnRec *r; int n, cap; int has; int64_t at; uint64_t tf[IGR_N], tb[IGR_N]; int ndirs_uncounted, nfiles_listed; } IgnReg;
#define IGN_FILE_CAP 20000
static void ign_reg_free(IgnReg *R) { for (int i = 0; i < R->n; i++) { free(R->r[i].rel); free(R->r[i].rule); } free(R->r); memset(R, 0, sizeof *R); }
static IgnRec *ign_reg_push(IgnReg *R) {
    if (R->n == R->cap) { R->cap = R->cap ? R->cap * 2 : 64; R->r = (IgnRec *)realloc(R->r, sizeof *R->r * (size_t)R->cap); }
    IgnRec *x = &R->r[R->n++]; memset(x, 0, sizeof *x); return x;
}
static void ign_reg_path(const char *storedir, char *out, size_t cap) { char par[4500]; mt_dirname(storedir, par, sizeof par); snprintf(out, cap, "%s/ignored", par); }
/* carrega o registro ao lado do store; 0 se nao ha (ainda nao contado) */
static int ign_reg_load(const char *storedir, IgnReg *R) {
    memset(R, 0, sizeof *R);
    char f[4600]; ign_reg_path(storedir, f, sizeof f);
    FILE *F = fopen(f, "rb"); if (!F) return 0;
    char ln[9000]; if (!fgets(ln, sizeof ln, F) || strncmp(ln, "katana-ignored 1", 16) != 0) { fclose(F); return 0; }
    R->has = 1;
    while (fgets(ln, sizeof ln, F)) {
        ln[strcspn(ln, "\r\n")] = 0;
        char *fld[9]; int nf = 0; char *p = ln; fld[nf++] = p;
        while (nf < 9 && (p = strchr(p, '\t'))) { *p++ = 0; fld[nf++] = p; }
        if (!strncmp(ln, "at ", 3)) { R->at = atoll(ln + 3); continue; }
        if (ln[0] == 'D' && nf >= 8) { IgnRec *x = ign_reg_push(R); x->isdir = 1; x->reason = atoi(fld[1]); x->state = atoi(fld[2]); x->files = atoll(fld[3]); x->bytes = strtoull(fld[4], NULL, 10); x->mtime = atoll(fld[5]); x->rel = strdup(fld[6]); x->rule = strdup(fld[7]);
            if (x->reason > 0 && x->reason < IGR_N) { if (x->state) { R->tf[x->reason] += (uint64_t)x->files; R->tb[x->reason] += x->bytes; } else R->ndirs_uncounted++; } }
        else if (ln[0] == 'F' && nf >= 5) { IgnRec *x = ign_reg_push(R); x->reason = atoi(fld[1]); x->bytes = strtoull(fld[2], NULL, 10); x->files = 1; x->state = 1; x->rel = strdup(fld[3]); x->rule = strdup(fld[4]); R->nfiles_listed++;
            if (x->reason > 0 && x->reason < IGR_N) { R->tf[x->reason]++; R->tb[x->reason] += x->bytes; } }
        else if (ln[0] == 'T' && nf >= 4) { int rs = atoi(fld[1]); if (rs > 0 && rs < IGR_N) { R->tf[rs] += (uint64_t)atoll(fld[2]); R->tb[rs] += strtoull(fld[3], NULL, 10); } }
    }
    fclose(F); return 1;
}
static int ign_reg_find_dir(const IgnReg *R, const char *rel) { for (int i = 0; i < R->n; i++) if (R->r[i].isdir && !strcmp(R->r[i].rel, rel)) return i; return -1; }
/* as `n` maiores pastas ignoradas debaixo de `prefix` (relativo a raiz; "" = tudo), para o NOT FOUND da fase 6; pastas nao contadas ficam por ultimo; devolve quantas */
static int ign_reg_largest(const IgnReg *R, const char *prefix, int n, int *idx) {
    int got = 0; const size_t pl = strlen(prefix);
    for (int i = 0; i < R->n; i++) {
        const IgnRec *x = &R->r[i]; if (!x->isdir || x->reason == IGR_SECRET) continue;
        if (pl && (strncmp(x->rel, prefix, pl) != 0 || (x->rel[pl] != '/' && x->rel[pl]))) continue;
        int at = got; if (got < n) got++; else if (x->files <= R->r[idx[n - 1]].files) continue; else at = n - 1;
        while (at > 0 && R->r[idx[at - 1]].files < x->files) { idx[at] = idx[at - 1]; at--; }
        idx[at] = i;
    }
    return got;
}

typedef struct { IgnReg R; const IgnReg *prev; size_t cap_entries; double cap_s; } IgnScan;
static int64_t mt_mtime(const char *p) { struct stat st; return lstat(p, &st) == 0 ? (int64_t)st.st_mtime : 0; }
/* readdir + lstat, nada lido; para no teto de entradas ou de tempo (state 2 = "pelo menos") */
static void mt_count(const char *p, double t0, double cap_s, size_t cap_n, size_t *n, uint64_t *files, uint64_t *bytes, int *capped) {
    DIR *d = opendir(p); if (!d) return;
    struct dirent *de;
    while (!*capped && (de = readdir(d))) {
        const char *nm = de->d_name; if (!strcmp(nm, ".") || !strcmp(nm, "..") || !strcmp(nm, ".git") || !strcmp(nm, ".hg") || !strcmp(nm, ".svn") || !strcmp(nm, ".katana")) continue;
        if (++*n > cap_n || now_sec() - t0 > cap_s) { *capped = 1; break; }
        char c[4700]; snprintf(c, sizeof c, "%s/%s", p, nm); struct stat st; if (lstat(c, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) mt_count(c, t0, cap_s, cap_n, n, files, bytes, capped);
        else { (*files)++; if (S_ISREG(st.st_mode)) *bytes += (uint64_t)st.st_size; }
    }
    closedir(d);
}
static void ign_sink_fn(void *ctx, const char *rel, int isdir, int reason, const char *rule, uint64_t size, const char *abs) {
    IgnScan *S = (IgnScan *)ctx; IgnReg *R = &S->R;
    if (strchr(rel, '\t') || strchr(rel, '\n')) return;
    if (isdir) {
        IgnRec *x = ign_reg_push(R); x->isdir = 1; x->reason = reason; x->rel = strdup(rel); x->rule = strdup(rule); x->mtime = mt_mtime(abs);
        if (S->prev) for (int i = 0; i < S->prev->n; i++) { const IgnRec *o = &S->prev->r[i]; if (o->isdir && o->state && o->mtime == x->mtime && o->reason == reason && !strcmp(o->rel, rel)) { x->state = o->state; x->files = o->files; x->bytes = o->bytes; break; } }
        if (!x->state) { size_t n = 0; uint64_t f = 0, b = 0; int capped = 0; mt_count(abs, now_sec(), S->cap_s, S->cap_entries, &n, &f, &b, &capped); x->files = (int64_t)f; x->bytes = b; x->state = capped ? 2 : 1; }
        R->tf[reason] += (uint64_t)x->files; R->tb[reason] += x->bytes;
    } else {
        R->tf[reason]++; R->tb[reason] += size;
        if (R->nfiles_listed < IGN_FILE_CAP) { IgnRec *x = ign_reg_push(R); x->reason = reason; x->bytes = size; x->files = 1; x->state = 1; x->rel = strdup(rel); x->rule = strdup(rule); R->nfiles_listed++; }
    }
}
/* grava o registro (tmp e rename, 0600). Os totais de arquivos vao em linhas T para valer mesmo acima do teto de linhas F */
static int ign_reg_save(const char *storedir, const char *root, const IgnReg *R) {
    char f[4600], t[4700]; ign_reg_path(storedir, f, sizeof f); snprintf(t, sizeof t, "%s.%ld.tmp", f, (long)getpid());
    FILE *O = fopen(t, "wb"); if (!O) return -1;
    fprintf(O, "katana-ignored 1\nroot %s\nat %lld\n", root, (long long)time(NULL));
    uint64_t lf[IGR_N] = {0}, lb[IGR_N] = {0};
    for (int i = 0; i < R->n; i++) { const IgnRec *x = &R->r[i];
        if (x->isdir) fprintf(O, "D\t%d\t%d\t%lld\t%llu\t%lld\t%s\t%s\n", x->reason, x->state, (long long)x->files, (unsigned long long)x->bytes, (long long)x->mtime, x->rel, x->rule);
        else { fprintf(O, "F\t%d\t%llu\t%s\t%s\n", x->reason, (unsigned long long)x->bytes, x->rel, x->rule); lf[x->reason]++; lb[x->reason] += x->bytes; } }
    for (int k = 1; k < IGR_N; k++) {          /* total de arquivos = os de cada pasta (D) mais os arquivos soltos; em T so os soltos */
        uint64_t df = 0, db = 0; for (int i = 0; i < R->n; i++) if (R->r[i].isdir && R->r[i].reason == k && R->r[i].state) { df += (uint64_t)R->r[i].files; db += R->r[i].bytes; }
        const uint64_t tf = R->tf[k] - df, tb = R->tb[k] - db;
        if (tf > lf[k]) fprintf(O, "T\t%d\t%llu\t%llu\n", k, (unsigned long long)(tf - lf[k]), (unsigned long long)(tb - lb[k])); }
    fclose(O); chmod(t, 0600);
    if (rename(t, f) != 0) { unlink(t); return -1; }
    ign_hide_in_repo(storedir);
    return 0;
}
/* a passada de contagem: varre a raiz com as mesmas regras do indice e conta o que ficou de fora. persist = 0 calcula sem gravar (pasta sem armazenamento) */
static int ign_scan(const char *root, const char *storedir, IgnReg *out, int persist) {
    IgnReg prev; ign_reg_load(storedir, &prev);
    IgnScan S; memset(&S, 0, sizeof S); S.prev = prev.has ? &prev : NULL; S.cap_entries = 200000; S.cap_s = 1.0;
    { const char *e = getenv("KATANA_IGNORED_COUNT_MS"); if (e && *e) S.cap_s = atof(e) / 1000.0; }
    IgnOvr ovr; ign_ovr_load(&ovr, storedir);
    StoreScan sc; memset(&sc, 0, sizeof sc); sc.ovr = &ovr; sc.nolist = 1;
    IgnSink sk; memset(&sk, 0, sizeof sk); sk.ctx = &S; sk.fn = ign_sink_fn; sk.maxsz = 2u << 20; sc.sink = &sk;
    char path[4096], rr[4096]; if (!realpath(root, rr)) { ign_reg_free(&prev); ign_ovr_free(&ovr); return -1; }
    snprintf(path, sizeof path, "%s", rr); size_t rl = strlen(rr);
    st_scan_walk(&sc, path, rl, rl + (rl > 1 ? 1 : 0));
    ign_ovr_free(&ovr); ign_reg_free(&prev);
    S.R.has = 1; S.R.at = (int64_t)time(NULL);
    int rc = 0; if (persist) rc = ign_reg_save(storedir, rr, &S.R);
    if (out) *out = S.R; else ign_reg_free(&S.R);
    return rc;
}


/* ---------- katana coder status: bloco dos ignorados (chamado por coder_cmd_status) ---------- */
static void ign_status_print(const char *storedir) {
    IgnReg R; IgnOvr O; char a[40], b[40];
    if (!ign_reg_load(storedir, &R)) printf("ignored: not counted yet (the counting pass runs after an index or a crawl; `katana coder ignored` runs it now)\n");
    else {
        uint64_t tf = 0, tb = 0; for (int k = 1; k < IGR_N; k++) { tf += R.tf[k]; tb += R.tb[k]; }
        char ago[40]; mt_ago((time_t)R.at, ago, sizeof ago);
        mt_commas((long long)tf, a, sizeof a);
        printf("ignored (left out of the index, counted %s): %s files, %s%s\n", ago, a, mt_hum(tb, b, sizeof b), R.ndirs_uncounted ? "; some folders not counted yet" : "");
        for (int k = 1; k < IGR_N; k++) if (R.tf[k]) { mt_commas((long long)R.tf[k], a, sizeof a); printf("  %-17s %10s files  %10s\n", g_ign_rname[k], a, mt_hum(R.tb[k], b, sizeof b)); }
        printf("  details and how to change them: katana coder ignored\n");
    }
    ign_reg_free(&R);
    ign_ovr_load(&O, storedir); int ni = 0, ne = 0, ns = 0; for (int i = 0; i < O.n; i++) { if (O.e[i].kind == IGO_EXCLUDE) ne++; else if (O.e[i].scoped) ns++; else ni++; }
    if (O.n) { char f[4600]; ign_ovr_path(storedir, f, sizeof f); printf("overrides: %d include, %d exclude, %d scoped (%s)\n", ni, ne, ns, f); }
    ign_ovr_free(&O);
    const char *fn = mt_fs_note(storedir); if (fn) printf("note: %s\n", fn);
}

/* ---------- katana coder ignored [pasta] [--files] [--json] ---------- */
typedef struct { char label[300], rule[420], worth[80]; int reason, isdir, state; int64_t files; uint64_t bytes; } IgnGrp;
static int mt_cmp_grp(const void *a, const void *b) { const IgnGrp *x = (const IgnGrp *)a, *y = (const IgnGrp *)b; if (x->files != y->files) return x->files < y->files ? 1 : -1; return strcmp(x->label, y->label); }
static int coder_cmd_ignored(int argc, char **argv) {
    char proj[4096] = "."; int files = 0, json = 0;
    for (int i = 0; i < argc; i++) { if (!strcmp(argv[i], "--files")) files = 1; else if (!strcmp(argv[i], "--json")) json = 1; else if (argv[i][0] != '-') snprintf(proj, sizeof proj, "%s", argv[i]); }
    struct stat s0; if (stat(proj, &s0) != 0 || !S_ISDIR(s0.st_mode)) { fprintf(stderr, "ignored: %s is not a folder\n", proj); return 2; }
    IndexLoc loc; index_resolve(proj, 0, &loc);
    { char h[1000]; agent_home(h, sizeof h); if (!loc.init && (!strcmp(loc.root, "/") || !strcmp(loc.root, h))) { fprintf(stderr, "ignored: %s is not an index root (home or /); run it inside a project folder\n", loc.root); return 2; } }
    char par[4500]; mt_dirname(loc.store, par, sizeof par);
    IgnReg R; memset(&R, 0, sizeof R); ign_scan(loc.root, loc.store, &R, rt_isdir(par)); if (!R.has) { fprintf(stderr, "ignored: cannot scan %s\n", loc.root); return 1; }
    /* o que o indice ja catalogou: arquivos lidos e binarios que so se reconhecem pelo conteudo (NUL) */
    long long indexed = -1, nulbin = 0, secsniff = 0; uint64_t nulbytes = 0, secbytes = 0; IgnGrp nulg; memset(&nulg, 0, sizeof nulg);
    { StoreReader *r; char er[300]; if (store_open(loc.store, &r, er, sizeof er) == 0) {
        if (r->nseg) {
            StoreLive L; const StoreSeg **sg = (const StoreSeg **)malloc(sizeof *sg * (size_t)(r->nseg + 1)); for (int i = 0; i < r->nseg; i++) sg[i] = &r->seg[i];
            store_live_build(&L, sg, r->nseg); free(sg); indexed = 0;
            const size_t pl = loc.prefix[0] ? strlen(loc.prefix) : 0;
            for (int i = 0; i < L.n; i++) { const StoreLiveDoc *e = &L.e[i]; if (e->dead) continue;
                if (pl && !(e->d.plen > pl && !memcmp(e->d.path, loc.prefix, pl) && e->d.path[pl] == '/')) continue;
                if (!(e->d.flags & SDF_SKIPMASK)) indexed++;
                else if ((e->d.flags & SDF_SKIP_SECRET) && pl == 0) { char tmp[4200]; snprintf(tmp, sizeof tmp, "%.*s", (int)e->d.plen, e->d.path); if (!ign_secret_name(tmp)) { secsniff++; secbytes += e->d.size; } }
                else if ((e->d.flags & SDF_SKIP_BINARY) && !(e->d.flags & SDF_SKIP_SECRET)) { char tmp[4200]; snprintf(tmp, sizeof tmp, "%.*s", (int)e->d.plen, e->d.path); if (!st_binary_ext(tmp)) { nulbin++; nulbytes += e->d.size; } } }
            store_live_free(&L); }
        store_close(r); } }
    IgnGrp *G = (IgnGrp *)calloc((size_t)R.n + 8, sizeof *G); int ng = 0; const size_t pl = loc.prefix[0] ? strlen(loc.prefix) : 0;
    struct { char ext[16]; int n; } hist[24]; int nh = 0; IgnGrp *bg = NULL; uint64_t listed_f[IGR_N] = {0}, listed_b[IGR_N] = {0};
    for (int i = 0; i < R.n; i++) { const IgnRec *x = &R.r[i];
        if (pl && !(strlen(x->rel) > pl && !memcmp(x->rel, loc.prefix, pl) && x->rel[pl] == '/')) continue;
        const char *rel = pl ? x->rel + pl + 1 : x->rel;
        if (x->isdir) { IgnGrp *g = &G[ng++]; snprintf(g->label, sizeof g->label, "%s/", rel); snprintf(g->rule, sizeof g->rule, "%s", x->rule); g->reason = x->reason; g->isdir = 1; g->state = x->state; g->files = x->state ? x->files : -1; g->bytes = x->bytes;
            if (x->reason == IGR_GITIGNORE || x->reason == IGR_KATANAIGNORE) { const char *nv = ign_never_path(x->rel); if (nv) snprintf(g->worth, sizeof g->worth, "usually worth indexing: under %s/", nv); } continue; }
        listed_f[x->reason]++; listed_b[x->reason] += x->bytes;
        if (x->reason == IGR_TOOBIG) { IgnGrp *g = &G[ng++]; snprintf(g->label, sizeof g->label, "%s", rel); snprintf(g->rule, sizeof g->rule, "%s", x->rule); g->reason = x->reason; g->files = 1; g->bytes = x->bytes; g->state = 1; continue; }
        if (x->reason == IGR_BINARY) {
            if (!bg) { bg = &G[ng++]; snprintf(bg->label, sizeof bg->label, "(files)"); bg->reason = IGR_BINARY; bg->state = 1; }
            bg->files++; bg->bytes += x->bytes; const char *dot = strchr(x->rule, '.'); char ex[16] = "?"; if (dot) { snprintf(ex, sizeof ex, "%.14s", dot); char *p = strchr(ex, ')'); if (p) *p = 0; }
            int k = 0; while (k < nh && strcmp(hist[k].ext, ex)) k++; if (k == nh && nh < 24) { snprintf(hist[nh].ext, sizeof hist[nh].ext, "%s", ex); hist[nh++].n = 0; } if (k < nh) hist[k].n++; continue; }
        int k = 0; for (; k < ng; k++) if (!G[k].isdir && G[k].reason == x->reason && G[k].reason != IGR_TOOBIG && G[k].reason != IGR_BINARY && !strcmp(G[k].rule, x->rule)) break;
        if (k == ng) { IgnGrp *g = &G[ng++]; snprintf(g->label, sizeof g->label, "(files)"); snprintf(g->rule, sizeof g->rule, "%s", x->rule); g->reason = x->reason; g->state = 1; }
        G[k].files++; G[k].bytes += x->bytes;
    }
    if (secsniff) { IgnGrp *g = &G[ng++]; snprintf(g->label, sizeof g->label, "(files)"); snprintf(g->rule, sizeof g->rule, "secret (private key recognized by its content; never read)"); g->reason = IGR_SECRET; g->state = 1; g->files = secsniff; g->bytes = secbytes; }
    if (nulbin) { if (!bg) { bg = &G[ng++]; snprintf(bg->label, sizeof bg->label, "(files)"); bg->reason = IGR_BINARY; bg->state = 1; } /* binarios por conteudo entram no mesmo grupo */ bg->files += nulbin; bg->bytes += nulbytes; }
    if (bg) { char b[300] = ""; int used[24] = {0}; for (int t = 0; t < 3; t++) { int best = -1; for (int k = 0; k < nh; k++) if (!used[k] && (best < 0 || hist[k].n > hist[best].n)) best = k; if (best < 0) break; used[best] = 1; size_t l = strlen(b); snprintf(b + l, sizeof b - l, "%s%s %d", l ? ", " : "", hist[best].ext, hist[best].n); }
        if (nulbin) { size_t l = strlen(b); snprintf(b + l, sizeof b - l, "%s%lld by content (NUL byte)", l ? ", " : "", nulbin); }
        snprintf(bg->rule, sizeof bg->rule, "binary (%s)", b); }
    if (!pl) for (int k = 1; k < IGR_N; k++) if (!bg || k != IGR_BINARY) { const int64_t more = (int64_t)R.tf[k] - (int64_t)listed_f[k]; /* arquivos alem do teto de registros, ou dentro de pastas contadas */
        int64_t indir = 0; for (int i = 0; i < R.n; i++) if (R.r[i].isdir && R.r[i].reason == k && R.r[i].state) indir += R.r[i].files;
        if (more - indir > 0) { IgnGrp *g = &G[ng++]; snprintf(g->label, sizeof g->label, "(files)"); snprintf(g->rule, sizeof g->rule, "%s (more than %d listed)", g_ign_rname[k], IGN_FILE_CAP); g->reason = k; g->files = more - indir; g->bytes = R.tb[k] - listed_b[k]; g->state = 1; } }
    qsort(G, (size_t)ng, sizeof *G, mt_cmp_grp);
    long long totf = 0; uint64_t totb = 0; int uncounted = 0; for (int i = 0; i < ng; i++) { if (G[i].files > 0) { totf += G[i].files; totb += G[i].bytes; } if (G[i].isdir && G[i].state == 0) uncounted++; }
    const char *where = loc.init ? "init" : "home index";
    char t1[60], t2[60], t3[60];
    if (json) {
        printf("{\"root\":"); mt_jstr(loc.root); printf(",\"prefix\":"); mt_jstr(loc.prefix); printf(",\"source\":\"%s\",\"indexed\":%lld,\"ignored\":[", where, indexed);
        for (int i = 0; i < ng; i++) { printf("%s{\"path\":", i ? "," : ""); mt_jstr(G[i].label); printf(",\"kind\":\"%s\",\"reason\":\"%s\",\"rule\":", G[i].isdir ? "dir" : "files", g_ign_rname[G[i].reason]); mt_jstr(G[i].rule);
            printf(",\"files\":%lld,\"bytes\":%llu,\"counted\":%s,\"capped\":%s,\"worth_indexing\":%s}", G[i].files < 0 ? -1LL : (long long)G[i].files, (unsigned long long)G[i].bytes, G[i].state ? "true" : "false", G[i].state == 2 ? "true" : "false", G[i].worth[0] ? "true" : "false"); }
        printf("],\"totals\":{\"files\":%lld,\"bytes\":%llu,\"folders_not_counted\":%d,\"by_reason\":{", totf, (unsigned long long)totb, uncounted);
        { int first = 1; for (int k = 1; k < IGR_N; k++) if (R.tf[k]) { printf("%s\"%s\":{\"files\":%llu,\"bytes\":%llu}", first ? "" : ",", g_ign_rname[k], (unsigned long long)R.tf[k], (unsigned long long)R.tb[k]); first = 0; } }
        printf("}}");
        if (files) { printf(",\"files\":["); int first = 1; for (int i = 0; i < R.n; i++) if (!R.r[i].isdir) { printf("%s{\"path\":", first ? "" : ","); mt_jstr(R.r[i].rel); printf(",\"reason\":\"%s\",\"rule\":", g_ign_rname[R.r[i].reason]); mt_jstr(R.r[i].rule); printf(",\"bytes\":%llu}", (unsigned long long)R.r[i].bytes); first = 0; } printf("]"); }
        printf("}\n");
    } else {
        char cur[4200]; rt_real(proj, cur, sizeof cur);
        printf("ignored under %s (root: %s at %s)\n", cur, loc.init ? "init" : "home index", loc.root);
        size_t lw = 8; for (int i = 0; i < ng; i++) { size_t l = strlen(G[i].label); if (l > lw) lw = l; } if (lw > 34) lw = 34;
        for (int i = 0; i < ng; i++) { const IgnGrp *g = &G[i];
            if (g->files < 0) printf("  %9s %10s  %-*s  %s     %s\n", "not counted", "yet", (int)lw, g->label, g->rule, g->worth);
            else { mt_commas((long long)g->files, t1, sizeof t1); printf("  %s%9s %s %10s  %-*s  %s%s%s\n", g->state == 2 ? ">=" : "  ", t1, g->files == 1 ? "file " : "files", mt_hum(g->bytes, t2, sizeof t2), (int)lw, g->label, g->rule, g->worth[0] ? "     " : "", g->worth); } }
        if (!ng) printf("  (nothing is being left out)\n");
        mt_commas(totf, t1, sizeof t1); printf("total: %s files, %s not in the index%s", t1, mt_hum(totb, t2, sizeof t2), uncounted ? " (some folders not counted yet)" : "");
        if (indexed >= 0) { mt_commas(indexed, t3, sizeof t3); printf("; %s files indexed", t3); } printf("\n");
        const char *ex = "node_modules/express"; for (int i = 0; i < ng; i++) if (G[i].isdir) { snprintf(t3, sizeof t3, "%s", G[i].label); size_t l = strlen(t3); if (l && t3[l - 1] == '/') t3[l - 1] = 0; ex = NULL; break; }
        printf("to index something that is ignored:   katana coder include %s\n", ex ? ex : t3);
        printf("                                      or a line  !%s/  in .katanaignore\n", ex ? "node_modules/express" : t3);
        printf("to ignore something that is indexed:  katana coder exclude <path>   or a line in .katanaignore\n");
        if (files) { int shown = 0, total = 0; for (int i = 0; i < R.n; i++) if (!R.r[i].isdir) total++;
            printf("files left out one by one:\n");
            for (int i = 0; i < R.n && shown < 300; i++) if (!R.r[i].isdir) { mt_hum(R.r[i].bytes, t2, sizeof t2); printf("  %-48s %10s  %s\n", R.r[i].rel, t2, R.r[i].rule); shown++; }
            if (total > shown) printf("  (+%d more; use --json)\n", total - shown); }
        const char *fn = mt_fs_note(loc.store); (void)fn;
    }
    free(G); ign_reg_free(&R);
    return 0;
}

/* ---------- katana coder include | exclude <caminho> ---------- */
static int ign_cmd_set(int argc, char **argv, const char *kind) {
    char arg[4096] = ""; for (int i = 0; i < argc; i++) if (argv[i][0] != '-') snprintf(arg, sizeof arg, "%s", argv[i]);
    if (!arg[0]) { fprintf(stderr, "usage: katana coder %s <path>   (a file or folder under an index root; run it from the project folder)\n", kind); return 2; }
    char cwd[4096], ab[4200]; if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
    if (arg[0] == '/') snprintf(ab, sizeof ab, "%s", arg); else snprintf(ab, sizeof ab, "%s/%s", cwd, arg);
    { char rp[4200]; if (realpath(ab, rp)) snprintf(ab, sizeof ab, "%s", rp);
      else { fprintf(stderr, "%s: %s does not exist\n", kind, arg); return 2; } }
    IndexLoc L; index_resolve(cwd, 0, &L);
    size_t rl = strlen(L.root);
    int under = !strncmp(ab, L.root, rl) && (ab[rl] == '/' || (rl == 1));
    if (!under) { index_resolve(ab, 0, &L); rl = strlen(L.root); under = (L.init || L.registered) && !strncmp(ab, L.root, rl) && ab[rl] == '/'; }
    if (!under) { fprintf(stderr, "%s: %s is not inside a known index root (run `katana init` in the project, or run this command from the project folder)\n", kind, ab); return 2; }
    const char *rel = ab + rl + (rl > 1 ? 1 : 0);
    if (!*rel) { fprintf(stderr, "%s: %s is the root itself\n", kind, ab); return 2; }
    for (const char *p = rel; *p; ) { char c[256]; size_t n = strcspn(p, "/"); if (n && n < sizeof c) { memcpy(c, p, n); c[n] = 0; const IgnHeavy *h = ign_heavy(c);
            if (h && !ign_group_includable(h->group)) { fprintf(stderr, "%s: %s is never indexed (%s)\n", kind, rel, g_ign_gwhy[h->group]); return 2; } }
        p += n; if (*p == '/') p++; }
    if (!strcmp(kind, "include") && ign_secret_name(rel)) { fprintf(stderr, "include: %s matches the secret file table (%s); its content is never read\n", rel, ign_secret_name(rel)); return 2; }
    IndexLoc W; index_resolve(L.root, 1, &W);                        /* grava: raiz da home passa a existir no registro */
    if (ign_ovr_set(W.store, kind, rel) != 0) { fprintf(stderr, "%s: cannot write the overrides file next to %s\n", kind, W.store); return 1; }
    char of[4600]; ign_ovr_path(W.store, of, sizeof of);
    printf("%s: %s recorded in %s\n", kind, rel, of);
    char *ia[2] = {W.root, NULL};
    const int rc = coder_cmd_index(1, ia);                            /* a varredura sincrona ja le os overrides: a pasta entra (ou sai) agora */
    if (rc == 0) printf("%s: done; `katana coder ignored` shows what is still left out\n", kind);
    return rc;
}
static int coder_cmd_include(int argc, char **argv) { return ign_cmd_set(argc, argv, "include"); }
static int coder_cmd_exclude(int argc, char **argv) { return ign_cmd_set(argc, argv, "exclude"); }
/* inclusao restrita por `path` (fase 6 chama quando o path de Find/Symbols cai dentro de pasta ignorada): grava `scoped <rel>`; 0 = gravado */
static int ign_scoped_add(const char *project_dir, const char *rel_in_root) {
    IndexLoc L; index_resolve(project_dir, 1, &L);
    return ign_ovr_set(L.store, "scoped", rel_in_root);
}

/* ---------- raizes conhecidas: list, forget, gc ---------- */
typedef struct { char key[24]; char root[4096]; int init; long long created; time_t lastuse; char store[4400]; int gone; } MtRoot;
static int mt_load_roots(MtRoot **out) {
    MtRoot *v = NULL; int n = 0, cap = 0;
    for (int pass = 0; pass < 2; pass++) {
        char d[900]; if (pass == 0) rt_reg_dir(d, sizeof d); else rt_inits_dir(d, sizeof d);
        DIR *D = opendir(d); if (!D) continue; struct dirent *de;
        while ((de = readdir(D))) {
            const size_t l = strlen(de->d_name); if (l != 21 || strcmp(de->d_name + 16, ".root")) continue;
            char f[1100], ln[4300] = ""; snprintf(f, sizeof f, "%s/%s", d, de->d_name); struct stat st; if (stat(f, &st) != 0) continue;
            FILE *F = fopen(f, "r"); if (!F) continue; if (!fgets(ln, sizeof ln, F)) ln[0] = 0;
            long long created = 0; char l2[200]; while (fgets(l2, sizeof l2, F)) if (!strncmp(l2, "created ", 8)) created = atoll(l2 + 8);
            fclose(F); ln[strcspn(ln, "\r\n")] = 0; if (!ln[0]) continue;
            if (n == cap) { cap = cap ? cap * 2 : 16; v = (MtRoot *)realloc(v, sizeof *v * (size_t)cap); }
            MtRoot *r = &v[n++]; memset(r, 0, sizeof *r); snprintf(r->key, sizeof r->key, "%.16s", de->d_name); snprintf(r->root, sizeof r->root, "%s", ln); r->init = pass; r->created = created ? created : (long long)st.st_mtime; r->lastuse = st.st_mtime;
            struct stat rs; r->gone = stat(r->root, &rs) != 0;
            if (pass) { snprintf(r->store, sizeof r->store, "%s/.katana/coder/store", strcmp(ln, "/") ? ln : ""); struct stat sv; if (stat(r->store, &sv) != 0) rt_home_store(ln, r->store, sizeof r->store); }
            else rt_home_store(ln, r->store, sizeof r->store);
        }
        closedir(D);
    }
    *out = v; return n;
}
static int mt_files_in(const char *store) {          /* arquivos vigentes do catalogo; -1 se nao ha indice */
    StoreReader *r; char er[200]; if (store_open(store, &r, er, sizeof er) != 0) return -1;
    int n = -1;
    if (r->nseg) { StoreLive L; const StoreSeg **sg = (const StoreSeg **)malloc(sizeof *sg * (size_t)(r->nseg + 1)); for (int i = 0; i < r->nseg; i++) sg[i] = &r->seg[i]; store_live_build(&L, sg, r->nseg); free(sg); n = 0; for (int i = 0; i < L.n; i++) if (!L.e[i].dead) n++; store_live_free(&L); }
    store_close(r); return n;
}
static int coder_cmd_list(int argc, char **argv) {
    int json = 0; for (int i = 0; i < argc; i++) if (!strcmp(argv[i], "--json")) json = 1;
    MtRoot *v; const int n = mt_load_roots(&v); uint64_t homebytes = 0; char a[40], b[40], c[40];
    const char *e = getenv("KATANA_INDEX_MAX_MB"); const uint64_t maxmb = e && *e ? strtoull(e, NULL, 10) : 2048;
    if (json) printf("{\"roots\":[");
    else if (!n) printf("no roots are known yet (an init'ed folder or a visited folder registers itself the first time it is indexed)\n");
    for (int i = 0; i < n; i++) {
        const MtRoot *r = &v[i]; const uint64_t bytes = mt_dirbytes(r->store); if (!r->init) homebytes += bytes;
        const int files = mt_files_in(r->store); long pid = 0; const int run = files >= 0 && cw_is_running(r->store, &pid);
        mt_ago((time_t)r->created, a, sizeof a); mt_ago(r->lastuse, b, sizeof b); mt_hum(bytes, c, sizeof c);
        if (json) { printf("%s{\"root\":", i ? "," : ""); mt_jstr(r->root); printf(",\"kind\":\"%s\",\"store\":", r->init ? "init" : "home"); mt_jstr(r->store);
            printf(",\"bytes\":%llu,\"files\":%d,\"created\":%lld,\"last_use\":%lld,\"crawler\":%s,\"root_exists\":%s}", (unsigned long long)bytes, files, r->created, (long long)r->lastuse, run ? "true" : "false", r->gone ? "false" : "true"); }
        else { char fb[40]; if (files < 0) snprintf(fb, sizeof fb, "no index yet"); else snprintf(fb, sizeof fb, "%d files", files);
            printf("%-5s %s%s\n      store %s, %s, %s; created %s, last used %s; crawler %s\n", r->init ? "init" : "home", r->root, r->gone ? "  [folder is gone]" : "", r->store, c, fb, a, b, run ? "running" : "not running"); }
    }
    if (json) printf("],\"home_bytes\":%llu,\"home_limit_mb\":%llu}\n", (unsigned long long)homebytes, (unsigned long long)maxmb);
    else if (n) printf("home index: %s in total (limit %llu MiB, KATANA_INDEX_MAX_MB; `katana coder gc` applies it)\n", mt_hum(homebytes, c, sizeof c), (unsigned long long)maxmb);
    free(v); return 0;
}
static void mt_stop_crawler(const MtRoot *r) { long pid = 0; if (cw_is_running(r->store, &pid)) { char *av[3] = {(char *)"--stop", (char *)r->root, NULL}; coder_crawl_main(2, av); usleep(300000); } }
/* tira um root da home inteiro: crawler, store, registro, estado e soquete */
static void mt_drop_home(const MtRoot *r) {
    char h[800], p[1100]; rt_home(h, sizeof h);
    mt_stop_crawler(r);
    snprintf(p, sizeof p, "%s/.katana/coder/%s", h, r->key); rt_rmtree(p);
    rt_unregister(r->root);
    snprintf(p, sizeof p, "%s/.katana/run/coder-%s.state", h, r->key); unlink(p);
    snprintf(p, sizeof p, "%s/.katana/run/coder-%s.sock", h, r->key); unlink(p);
}
static int coder_cmd_forget(int argc, char **argv) {
    char proj[4096] = "."; for (int i = 0; i < argc; i++) if (argv[i][0] != '-') snprintf(proj, sizeof proj, "%s", argv[i]);
    IndexLoc L; index_resolve(proj, 0, &L);
    if (L.init) { fprintf(stderr, "forget: %s is an initialized folder; its index belongs to the folder. Use `katana init --remove` there\n", L.root); return 2; }
    char rp[4200]; rt_real(proj, rp, sizeof rp);
    if (!L.registered) { fprintf(stderr, "forget: %s has no index in the home index (see `katana coder list`)\n", rp); return 1; }
    if (strcmp(rp, L.root) != 0) { fprintf(stderr, "forget: %s is covered by the home root %s; forget that root instead (`katana coder forget %s`)\n", rp, L.root, L.root); return 2; }
    MtRoot r; memset(&r, 0, sizeof r); rt_key(L.root, r.key, sizeof r.key); snprintf(r.root, sizeof r.root, "%s", L.root); snprintf(r.store, sizeof r.store, "%s", L.store);
    const uint64_t bytes = mt_dirbytes(L.store); char b[40];
    mt_drop_home(&r);
    printf("forget: %s removed from the home index (%s freed; nothing was inside the folder)\n", L.root, mt_hum(bytes, b, sizeof b));
    return 0;
}
static int mt_has_manifest(const char *store) { char m[4500]; snprintf(m, sizeof m, "%s/MANIFEST", store); struct stat st; return stat(m, &st) == 0; }
/* gc da home (F.5): raizes sumidas ha 30 dias, absorvidas, pastas orfas, copias store.bad-* de mais de 7 dias, e o teto de tamanho. Nunca toca em raiz com init.
 * Devolve quantas coisas removeu (ou removeria, com dry) */
static int mt_gc(int dry, int quiet, uint64_t maxmb) {
    MtRoot *v; const int n = mt_load_roots(&v); int acted = 0; uint64_t freed = 0; char b[40]; const time_t now = time(NULL);
    int *del = (int *)calloc((size_t)n + 1, sizeof *del); const char **why = (const char **)calloc((size_t)n + 1, sizeof *why); char (*why2)[300] = (char (*)[300])calloc((size_t)n + 1, 300);
    for (int i = 0; i < n; i++) { const MtRoot *r = &v[i]; if (r->init) continue;
        if (r->gone && now - r->lastuse > 30 * 86400) { del[i] = 1; why[i] = "the folder has not existed for more than 30 days"; continue; }
        char mk[4200];
        if (index_marker_root(r->root, mk, sizeof mk)) { del[i] = 1; snprintf(why2[i], 300, "absorbed by the init at %s", mk); why[i] = why2[i]; continue; }
        for (int j = 0; j < n; j++) { const MtRoot *o = &v[j]; if (j == i || o->init) continue; const size_t ol = strlen(o->root);
            if (!strncmp(r->root, o->root, ol) && r->root[ol] == '/' && mt_has_manifest(o->store)) { del[i] = 1; snprintf(why2[i], 300, "absorbed by the home root %s", o->root); why[i] = why2[i]; break; } } }
    /* teto de tamanho: das restantes, as de uso mais antigo (parado ha 14 dias ou mais) saem ate caber */
    uint64_t total = 0; uint64_t *sz = (uint64_t *)calloc((size_t)n + 1, sizeof *sz);
    for (int i = 0; i < n; i++) if (!v[i].init) { sz[i] = mt_dirbytes(v[i].store); if (!del[i]) total += sz[i]; }
    while (total > (maxmb << 20)) {
        int pick = -1;
        for (int i = 0; i < n; i++) if (!v[i].init && !del[i] && now - v[i].lastuse >= 14 * 86400 && (pick < 0 || v[i].lastuse < v[pick].lastuse)) pick = i;
        if (pick < 0) break;
        del[pick] = 1; why[pick] = "least recently used, unused for 14 days or more, and the home index is above its size limit"; total -= sz[pick]; }
    for (int i = 0; i < n; i++) if (del[i]) { acted++; freed += sz[i]; if (!quiet) printf("%s home root %s: %s (%s)\n", dry ? "would remove" : "removed", v[i].root, why[i], mt_hum(sz[i], b, sizeof b)); if (!dry) mt_drop_home(&v[i]); }
    /* registro de raiz com init cuja pasta sumiu ha 30 dias: so a entrada do registro sai (o store e da pasta, nunca e tocado) */
    for (int i = 0; i < n; i++) if (v[i].init && v[i].gone && now - v[i].lastuse > 30 * 86400) {
        acted++; if (!quiet) printf("%s registry entry of the initialized folder %s (the folder has not existed for more than 30 days)\n", dry ? "would remove" : "removed", v[i].root);
        if (!dry) { char f[1000]; rt_init_file(v[i].root, f, sizeof f); unlink(f); } }
    /* pastas de store sem registro (orfas) e copias de quarentena velhas (so das raizes da home) */
    { char h[800], d[900]; rt_home(h, sizeof h); snprintf(d, sizeof d, "%s/.katana/coder", h); DIR *D = opendir(d);
      if (D) { struct dirent *de; while ((de = readdir(D))) { const char *nm = de->d_name; int hex = strlen(nm) == 16; for (int k = 0; hex && k < 16; k++) if (!isxdigit((unsigned char)nm[k])) hex = 0; if (!hex) continue;
            int at = -1; for (int i = 0; i < n; i++) if (!strcmp(v[i].key, nm)) at = i;
            char p[1100]; snprintf(p, sizeof p, "%s/%s", d, nm);
            if (at < 0) { const uint64_t by = mt_dirbytes(p); acted++; freed += by; if (!quiet) printf("%s orphan store folder %s (no registry entry; %s)\n", dry ? "would remove" : "removed", p, mt_hum(by, b, sizeof b)); if (!dry) rt_rmtree(p); continue; }
            if (v[at].init) continue;                      /* raiz com init: nunca */
            DIR *E = opendir(p); if (E) { struct dirent *e2; while ((e2 = readdir(E))) if (!strncmp(e2->d_name, "store.bad-", 10)) { char q[1400]; snprintf(q, sizeof q, "%s/%s", p, e2->d_name); struct stat st; if (stat(q, &st) == 0 && now - st.st_mtime > 7 * 86400) { const uint64_t by = mt_dirbytes(q); acted++; freed += by; if (!quiet) printf("%s quarantined copy %s (%s)\n", dry ? "would remove" : "removed", q, mt_hum(by, b, sizeof b)); if (!dry) rt_rmtree(q); } } closedir(E); } }
        closedir(D); } }
    if (!quiet) printf("gc: %s%d item(s), %s%s; home index now %s of %llu MiB allowed\n", dry ? "dry run, " : "", acted, mt_hum(freed, b, sizeof b), dry ? " would be freed" : " freed", mt_hum(total, (char[40]){0}, 40), (unsigned long long)maxmb);
    if (!dry) { char h[800], sp[1000]; rt_home(h, sizeof h); snprintf(sp, sizeof sp, "%s/.katana/coder/gc.stamp", h); FILE *f = fopen(sp, "w"); if (f) { fprintf(f, "%lld\n", (long long)now); fclose(f); chmod(sp, 0600); } }
    free(del); free(why); free(why2); free(sz); free(v); return acted;
}
static int coder_cmd_gc(int argc, char **argv) {
    int dry = 0; const char *e = getenv("KATANA_INDEX_MAX_MB"); uint64_t maxmb = e && *e ? strtoull(e, NULL, 10) : 2048;
    for (int i = 0; i < argc; i++) { if (!strcmp(argv[i], "--dry-run")) dry = 1; else if (!strcmp(argv[i], "--max-mb") && i + 1 < argc) maxmb = strtoull(argv[++i], NULL, 10); }
    mt_gc(dry, 0, maxmb); return 0;
}

/* chamado ao fim de `katana coder index` (storedocs.c): a passada de contagem dos ignorados */
static void maint_after_index(const char *root, const char *dir) { char par[4500]; mt_dirname(dir, par, sizeof par); if (rt_isdir(par)) ign_scan(root, dir, NULL, 1); }

/* ---------- ganchos do rastreador (crawl.c) ---------- */
static int mt_crawl_target(char *root, size_t rc, char *dir, size_t dc) {         /* raiz e store que este processo `katana coder crawl` indexa, lidos da linha de comando */
    char buf[16384]; FILE *f = fopen("/proc/self/cmdline", "rb"); if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    const char *av[96]; int ac = 0; for (size_t i = 0; i < n && ac < 96; ) { av[ac++] = buf + i; i += strlen(buf + i) + 1; }
    int k = -1; for (int i = 0; i + 1 < ac; i++) if (!strcmp(av[i], "coder") && !strcmp(av[i + 1], "crawl")) { k = i + 2; break; }
    if (k < 0) return 0;
    const char *folder = "."; for (int i = k; i < ac; i++) { if (!strcmp(av[i], "--prio")) { i++; continue; } if (!strcmp(av[i], "--changed")) break; if (av[i][0] != '-') { folder = av[i]; break; } }
    IndexLoc L; index_resolve(folder, 0, &L); snprintf(root, rc, "%s", L.root); snprintf(dir, dc, "%s", L.store); return 1;
}
static int mt_verify_quiet(const char *dir) {         /* confere todos os checksums do store; 0 = ok */
    StoreReader *r; char er[300]; if (store_open(dir, &r, er, sizeof er) != 0) return -1;
    int bad = 0;
    for (int i = 0; i < r->nseg && !bad; i++) { char e2[300] = "";
        if (store_seg_verify(&r->seg[i], e2, sizeof e2)) { bad = 1; break; }
        const StoreSection *sd = store_seg_section(&r->seg[i], STS_DOCS);
        if (sd && cstore_check_docs(sd, e2, sizeof e2) != 0) bad = 1;
        else if (coder_search_verify_seg(&r->seg[i], e2, sizeof e2) != 0 || coder_sym_verify_seg(&r->seg[i], e2, sizeof e2) != 0 || coder_crawl_verify_seg(&r->seg[i], e2, sizeof e2) != 0 || coder_key_verify_seg(&r->seg[i], e2, sizeof e2) != 0) bad = 1; }
    store_close(r); return bad;
}
static void maint_on_crawl_start(void) {
    char root[4200], dir[4400]; if (!mt_crawl_target(root, sizeof root, dir, sizeof dir)) return;
    { char h[800], sp[1000]; rt_home(h, sizeof h); snprintf(sp, sizeof sp, "%s/.katana/coder/gc.stamp", h); struct stat st;      /* gc diario automatico (so a home) */
      if (stat(sp, &st) != 0 || time(NULL) - st.st_mtime > 86400) { const char *e = getenv("KATANA_INDEX_MAX_MB"); const int a = mt_gc(0, 1, e && *e ? strtoull(e, NULL, 10) : 2048); if (a) fprintf(stderr, "[index: daily clean-up removed %d item(s) from the home index]\n", a); } }
    char err[300] = ""; maint_store_prepare(dir, err, sizeof err);              /* formato diferente ou dano: reconstroi antes de o rastreador abrir o store */
    for (int i = 0; i < 5 && !maint_disk_ok(dir, 0, err, sizeof err); i++) { fprintf(stderr, "[index: %s]\n", err); sleep(60); }       /* disco cheio: pausa e tenta de novo a cada 60 s */
}
static void maint_on_crawl_end(void) {
    char root[4200], dir[4400]; if (!mt_crawl_target(root, sizeof root, dir, sizeof dir)) return;
    char par[4500]; mt_dirname(dir, par, sizeof par);
    if (rt_isdir(par)) ign_scan(root, dir, NULL, 1);                            /* passada de contagem: a de menor prioridade, depois do N3 */
    char sp[4600]; struct stat st; snprintf(sp, sizeof sp, "%s/scrub.stamp", par);            /* conferencia de checksums a cada 7 dias */
    if (stat(sp, &st) != 0 || time(NULL) - st.st_mtime > 7 * 86400) {
        const int bad = mt_verify_quiet(dir);
        FILE *f = fopen(sp, "w"); if (f) { fprintf(f, "%lld\n", (long long)time(NULL)); fclose(f); chmod(sp, 0600); }
        if (bad > 0) { fprintf(stderr, "[index: the checksum scrub found damage in %s; it will be rebuilt]\n", dir); maint_force_rebuild(dir); }
    }
}

/* ---------- fase 6: o que o modelo ve sobre as pastas ignoradas (J.3, J.5) e a inclusao restrita por `path` (D.10) ----------
 * g_q_dir: a pasta consultada pela ferramenta em andamento (a `path` ja resolvida; vazio = nenhuma). As visoes de leitura (search.c, symquery.c) a usam para
 * decidir quais pastas de inclusao restrita enxergam (store_live_hide_scoped): a consulta ve uma delas so se g_q_dir cai dentro dela. */
static char g_q_dir[4200];
static char g_scope_note[500];
static int index_force_update(const char *root);                 /* indexctl.c */
/* caminho como o modelo o usa em `path`: relativo a pasta de trabalho quando esta debaixo dela, senao absoluto */
static void ign_disp(const char *abs, char *out, size_t cap) {
    char cwd[4096]; size_t cl;
    if (getcwd(cwd, sizeof cwd) && (cl = strlen(cwd)) > 1) {
        if (!strcmp(abs, cwd)) { snprintf(out, cap, "."); return; }
        if (!strncmp(abs, cwd, cl) && abs[cl] == '/') { snprintf(out, cap, "%s", abs + cl + 1); return; }
    }
    snprintf(out, cap, "%s", abs);
}
/* prefixo da consulta relativo a raiz do indice: a pasta consultada (g_q_dir) quando esta debaixo da raiz, senao o prefixo que a resolucao deu */
static void ign_qprefix(const IndexLoc *loc, char *out, size_t cap) {
    const size_t rl = strlen(loc->root);
    if (g_q_dir[0] && !strncmp(g_q_dir, loc->root, rl) && (rl == 1 || g_q_dir[rl] == '/' || !g_q_dir[rl])) {
        const char *r = g_q_dir + rl; if (*r == '/') r++;
        snprintf(out, cap, "%s", r); return;
    }
    snprintf(out, cap, "%s", loc->prefix);
}
/* esconde da visao as pastas de inclusao restrita que esta consulta nao pediu; chamar ANTES de store_live_rebase. Devolve quantos documentos escondeu */
static int ign_hide_for_view(const IndexLoc *loc, StoreLive *L) {
    IgnOvr O; ign_ovr_load(&O, loc->store); int n = 0;
    if (O.n) { char qp[2100]; ign_qprefix(loc, qp, sizeof qp); n = store_live_hide_scoped(L, &O, qp); }
    ign_ovr_free(&O); return n;
}
/* parte da chave de cache de uma visao: a pasta de inclusao restrita que contem a consulta ("" = nenhuma); o resto da visao e igual */
static void ign_vis_key(const IndexLoc *loc, char *out, size_t cap) {
    out[0] = 0; IgnOvr O; ign_ovr_load(&O, loc->store);
    if (O.n) {
        char qp[2100]; ign_qprefix(loc, qp, sizeof qp); const size_t ql = strlen(qp);
        for (int k = 0; k < O.n; k++) { const IgnOvrEnt *x = &O.e[k]; if (x->scoped && ql >= x->pl && !memcmp(qp, x->p, x->pl) && (ql == x->pl || qp[x->pl] == '/')) { snprintf(out, cap, "%.200s", x->p); break; } }
    }
    ign_ovr_free(&O);
}
static void ign_scope_leave(void) { g_q_dir[0] = 0; }
/* o registro de ignorados da raiz: o que o ultimo `index`/rastreador gravou; se ainda nao existe (indice criado por uma ferramenta, que nao faz a passada de contagem),
 * faz a passada agora (so readdir e lstat; grava ao lado do store quando da) e usa o resultado. 0 = nao ha como saber */
static int ign_reg_get(const IndexLoc *loc, IgnReg *R) {
    if (ign_reg_load(loc->store, R)) return 1;
    char par[4500]; mt_dirname(loc->store, par, sizeof par);
    struct stat sm; char man[4500]; snprintf(man, sizeof man, "%s/MANIFEST", loc->store); if (stat(man, &sm) != 0) return 0;       /* sem indice nao se conta nada */
    if (ign_scan(loc->root, loc->store, R, rt_isdir(par)) != 0) { ign_reg_free(R); return 0; }
    return R->has;
}
/* a ferramenta vai consultar `dir`: guarda-a como pasta consultada e, se ela esta DENTRO de uma pasta ignorada (registro de ignorados), faz a inclusao
 * restrita dela (grava `scoped <pasta>` no arquivo overrides e atualiza o indice agora), para que consultas com esse path a vejam sem poluir a busca do projeto.
 * Nao inclui o que o usuario excluiu, pastas de segredo, raizes aninhadas nem pastas com mais de KATANA_INDEX_INCLUDE_MAX_FILES arquivos. g_scope_note guarda a
 * frase do que aconteceu (uma linha), tomada por ign_scope_note_take. allow_add = 0 so registra a pasta consultada (comandos de linha de comando). */
static void ign_scope_enter(const char *dir, int allow_add) {
    g_scope_note[0] = 0; g_q_dir[0] = 0;
    char rp[4200]; if (!dir || !*dir || !realpath(dir, rp)) return;
    snprintf(g_q_dir, sizeof g_q_dir, "%s", rp);
    if (!allow_add) return;
    IndexLoc loc; index_resolve(rp, 0, &loc);
    if (!loc.prefix[0]) return;
    { IgnOvr O; ign_ovr_load(&O, loc.store); const int ost = ign_ovr_state(&O, loc.prefix, 1); ign_ovr_free(&O);
      if (ost == IGO_INCLUDE || ost == IGO_EXCLUDE) return; }
    IgnReg R; if (!ign_reg_get(&loc, &R)) return;          /* sem o registro nao ha como saber que e ignorada (nomes de pasta pesada podem ter sido reincluidos) */
    const IgnRec *rec = NULL; char sub[2100]; const char *p = loc.prefix;
    for (size_t pos = 0; ; ) {
        const char *sl = strchr(p + pos, '/'); const size_t end = sl ? (size_t)(sl - p) : strlen(p); if (!end || end >= sizeof sub) break;
        memcpy(sub, p, end); sub[end] = 0;
        const int ix = ign_reg_find_dir(&R, sub);
        if (ix >= 0) { rec = &R.r[ix]; break; }
        if (!sl) break;
        pos = end + 1;
    }
    if (!rec || rec->reason == IGR_SECRET || rec->reason == IGR_NESTED || rec->reason == IGR_CHECKOUT || rec->reason == IGR_EXCLUDED) { ign_reg_free(&R); return; }
    char why[160]; snprintf(why, sizeof why, "%.150s", rec->rule); char shown[4300]; ign_disp(rp, shown, sizeof shown);
    ign_reg_free(&R);
    long long cap = 50000; { const char *e = getenv("KATANA_INDEX_INCLUDE_MAX_FILES"); if (e && *e && atoll(e) > 0) cap = atoll(e); }
    size_t n = 0; uint64_t f = 0, b = 0; int capped = 0; mt_count(rp, now_sec(), 2.0, (size_t)cap * 2 + 1000, &n, &f, &b, &capped);      /* readdir e lstat so: quantos arquivos a pasta tem */
    if (capped || (long long)f > cap) { snprintf(g_scope_note, sizeof g_scope_note, "[index: %s/ is in an ignored folder and has more than %lld files, so it was not included; set path to a smaller folder inside it]", shown, cap); return; }
    if (ign_scoped_add(rp, loc.prefix) != 0) return;
    index_force_update(loc.root);
    snprintf(g_scope_note, sizeof g_scope_note, "[index: %s/ was ignored (%s); it is now indexed for queries whose path is inside it]", shown, why);
}
static int ign_scope_note_take(char *out, size_t cap) {
    if (!g_scope_note[0]) { if (cap) out[0] = 0; return 0; }
    snprintf(out, cap, "%s\n", g_scope_note); g_scope_note[0] = 0; return 1;
}
/* a frase do NOT FOUND sobre onde o indice nao olhou: as (ate 3) maiores pastas ignoradas debaixo da consulta (`proj` + `pre`); nada dentro delas e lido.
 * Devolve 1 e escreve "N ignored folders could hold it: a/, b/, c/. To search one, set `path` to it."; 0 se nao ha pasta ignorada na consulta */
static int ign_notfound_note(const char *proj, const char *pre, char *out, size_t cap) {
    if (cap) out[0] = 0;
    IndexLoc loc; index_resolve(proj, 0, &loc); IgnReg R; if (!ign_reg_get(&loc, &R)) return 0;
    char scope[4300]; size_t sl = 0; scope[0] = 0;
    if (loc.prefix[0]) sl = (size_t)snprintf(scope, sizeof scope, "%s", loc.prefix);
    if (pre && *pre) { const char *q = pre; while (q[0] == '.' && q[1] == '/') q += 2; sl += (size_t)snprintf(scope + sl, sizeof scope - sl, "%s%s", sl ? "/" : "", q); }
    while (sl > 0 && scope[sl - 1] == '/') scope[--sl] = 0;
    int idx[3]; const int got = ign_reg_largest(&R, scope, 3, idx);
    if (!got) { ign_reg_free(&R); return 0; }
    int total = 0; const size_t pl = strlen(scope);
    for (int i = 0; i < R.n; i++) { const IgnRec *x = &R.r[i]; if (!x->isdir || x->reason == IGR_SECRET) continue; if (pl && (strncmp(x->rel, scope, pl) != 0 || (x->rel[pl] != '/' && x->rel[pl]))) continue; total++; }
    char names[600] = "";
    for (int i = 0; i < got; i++) {
        char ab[4400], sh[4400]; snprintf(ab, sizeof ab, "%s/%s", strcmp(loc.root, "/") ? loc.root : "", R.r[idx[i]].rel); ign_disp(ab, sh, sizeof sh);
        size_t l = strlen(names); snprintf(names + l, sizeof names - l, "%s%.150s/", i ? ", " : "", sh);
    }
    snprintf(out, cap, "%d ignored folder%s could hold it: %s%s. To search one, set `path` to it.", total, total == 1 ? "" : "s", names, total > got ? " and others" : "");
    ign_reg_free(&R); return 1;
}
#else
static int maint_store_prepare(const char *dir, char *err, size_t ecap) { (void)dir; (void)err; (void)ecap; return 0; }
static int maint_disk_ok(const char *dir, uint64_t need, char *err, size_t ecap) { (void)dir; (void)need; (void)err; (void)ecap; return 1; }
static void maint_on_crawl_start(void) {}
static void maint_on_crawl_end(void) {}
static void ign_status_print(const char *storedir) { (void)storedir; }
static void maint_after_index(const char *root, const char *dir) { (void)root; (void)dir; }
static int coder_cmd_list(int argc, char **argv) { (void)argc; (void)argv; fprintf(stderr, "list: not available on this platform\n"); return 2; }
static int coder_cmd_forget(int argc, char **argv) { (void)argc; (void)argv; fprintf(stderr, "forget: not available on this platform\n"); return 2; }
static int coder_cmd_gc(int argc, char **argv) { (void)argc; (void)argv; fprintf(stderr, "gc: not available on this platform\n"); return 2; }
static int coder_cmd_ignored(int argc, char **argv) { (void)argc; (void)argv; fprintf(stderr, "ignored: not available on this platform\n"); return 2; }
static int coder_cmd_include(int argc, char **argv) { (void)argc; (void)argv; fprintf(stderr, "include: not available on this platform\n"); return 2; }
static int coder_cmd_exclude(int argc, char **argv) { (void)argc; (void)argv; fprintf(stderr, "exclude: not available on this platform\n"); return 2; }
static char g_q_dir[4200];
static void ign_scope_enter(const char *dir, int allow_add) { (void)dir; (void)allow_add; }
static int ign_hide_for_view(const IndexLoc *loc, StoreLive *L) { (void)loc; (void)L; return 0; }
static void ign_scope_leave(void) {}
static int ign_scope_note_take(char *out, size_t cap) { if (cap) out[0] = 0; return 0; }
static int ign_notfound_note(const char *proj, const char *pre, char *out, size_t cap) { (void)proj; (void)pre; if (cap) out[0] = 0; return 0; }
#endif
