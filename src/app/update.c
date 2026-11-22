/* ---------- katana update: publica o katana desta copia de trabalho como o instalado ----------
 * katana update [--from PASTA] [--model M.gguf] [--rollback] [--list]
 * Compila (build.sh da pasta), confere (calltest, compacttest e as suites de tools/tooltest; com --model, o selftest dele) e so
 * entao troca o programa instalado, de uma vez (rename). Sessoes abertas seguem no programa antigo. Os 5 anteriores ficam em
 * ~/.katana/versions. Fontes: --from, ou update= no katana.cfg (~/.katana), ou a pasta atual se tiver build.sh e src/katana.c. */
#ifndef _WIN32
static int up_exec(char *const *av, const char *cwd, const char *home, double tmo, const char *what, int quiet) {
    CbRes r;
    char **e = cb_env_new();
    if (home) cb_env_set(&e, "HOME", home);
    const int rc = cb_exec(av, cwd, e, "", tmo, &r);
    cb_env_free(e);
    const int bad = rc != 0 || r.rc != 0 || r.timed_out;
    if (bad && !quiet) {
        fprintf(stderr, "update: %s falhou%s\n", what, r.timed_out ? " (tempo esgotado)" : "");
        const Buf *b = r.err.n > 1 ? &r.err : &r.out;
        if (b->p) fprintf(stderr, "%s\n", b->n > 1500 ? b->p + b->n - 1500 : b->p);
    }
    free(r.out.p); free(r.err.p);
    return !bad;
}
static int up_copy(const char *a, const char *b) {
    char t[4400]; snprintf(t, sizeof t, "%s.new", b);
    struct stat st;
    if (stat(a, &st) || !S_ISREG(st.st_mode)) return 0;
    unlink(t);
    FILE *f = fopen(a, "rb"), *g = fopen(t, "wb");
    int ok = f && g; char buf[65536]; size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, f)) > 0) ok = fwrite(buf, 1, n, g) == n;
    if (f) { ok = ok && !ferror(f); fclose(f); }
    if (g && fclose(g)) ok = 0;
    if (!ok || chmod(t, 0755) || rename(t, b)) { unlink(t); return 0; }
    return 1;
}
static int up_cmp(const void *a, const void *b) { return strcmp(*(char *const *)b, *(char *const *)a); }
/* versoes guardadas, da mais nova para a mais velha */
static int up_list(const char *dir, char **v, int max) {
    int n = 0; DIR *d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d)) && n < max;) if (!strncmp(e->d_name, "katana-", 7)) v[n++] = strdup(e->d_name);
    if (d) closedir(d);
    qsort(v, (size_t)n, sizeof *v, up_cmp);
    return n;
}
static int update_main(int argc, char **argv) {
    const char *home = getenv("HOME"), *from = NULL, *model = NULL; int back = 0, list = 0;
    char src[4200] = "", dst[4200], dir[1200], self[4200];
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--from") && i + 1 < argc) from = argv[++i];
        else if (!strcmp(argv[i], "--model") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "--rollback")) back = 1;
        else if (!strcmp(argv[i], "--list")) list = 1;
        else { fprintf(stderr, "uso: katana update [--from PASTA] [--model M.gguf] [--rollback] [--list]\n"); return 2; }
    }
    if (!home || !*home) { fprintf(stderr, "update: sem HOME\n"); return 1; }
    const ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1);
    if (l <= 0) { fprintf(stderr, "update: nao achei o proprio programa\n"); return 1; }
    self[l] = 0; snprintf(dst, sizeof dst, "%s", self);
    snprintf(dir, sizeof dir, "%s/.katana", home); mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/.katana/versions", home); mkdir(dir, 0755);
    char *v[200]; int n = up_list(dir, v, 200);
    if (list) { printf("instalado: katana %s (%s)\n", KATANA_BUILD, dst); for (int i = 0; i < n; i++) printf("  %s/%s\n", dir, v[i]); return 0; }
    if (back) {
        if (!n) { fprintf(stderr, "update: nao ha versao guardada\n"); return 1; }
        char a[1600]; snprintf(a, sizeof a, "%s/%s", dir, v[0]);
        if (!up_copy(a, dst)) { fprintf(stderr, "update: nao consegui trocar %s\n", dst); return 1; }
        unlink(a); printf("voltou para %s\n", v[0]); return 0;
    }
    if (from) snprintf(src, sizeof src, "%s", from);
    else {
        char cf[1200], ln[4200]; snprintf(cf, sizeof cf, "%s/.katana/katana.cfg", home);
        FILE *f = fopen(cf, "rb");
        while (f && fgets(ln, sizeof ln, f)) if (!strncmp(ln, "update=", 7)) { ln[strcspn(ln, "\r\n")] = 0; snprintf(src, sizeof src, "%s", ln + 7); }
        if (f) fclose(f);
        if (!src[0] && !access("build.sh", R_OK) && !access("src/katana.c", R_OK) && !getcwd(src, sizeof src)) src[0] = 0;
    }
    char chk[4400], bin[4400], rp[4200];
    snprintf(chk, sizeof chk, "%s/src/katana.c", src);
    if (!src[0] || !realpath(src, rp) || access(chk, R_OK)) { fprintf(stderr, "update: diga de onde: katana update --from PASTA (a pasta com build.sh e src/katana.c), ou update=PASTA em ~/.katana/katana.cfg\n"); return 1; }
    snprintf(bin, sizeof bin, "%s/katana", rp);
    if (strcmp(bin, dst) == 0) { fprintf(stderr, "update: este ja e o programa desta pasta; use o katana instalado\n"); return 1; }
    printf("compilando %s ...\n", rp); fflush(stdout);
    char *b1[] = { "sh", "build.sh", NULL };
    if (!up_exec(b1, rp, NULL, 900, "a compilacao", 0)) return 1;
    char tmp[] = "/tmp/katana-update-XXXXXX";
    if (!mkdtemp(tmp)) return 1;
    int ok = 1;
    char *c1[] = { bin, "calltest", NULL }, *c2[] = { bin, "compacttest", NULL };
    ok = up_exec(c1, rp, NULL, 120, "calltest", 0) && up_exec(c2, rp, tmp, 120, "compacttest", 0);
    static const char *const T[] = { "files", "tasks", "meta", "misc", "extend", "agents", NULL };
    for (int i = 0; T[i] && ok; i++) {
        char sh[4400]; snprintf(sh, sizeof sh, "%s/tools/tooltest/%s.sh", rp, T[i]);
        if (access(sh, R_OK)) continue;
        printf("conferindo %s ...\n", T[i]); fflush(stdout);
        char *a[] = { "nice", "-n", "10", "sh", sh, bin, NULL };
        ok = up_exec(a, rp, NULL, 600, T[i], 0);
    }
    if (ok && model) { printf("selftest do modelo ...\n"); fflush(stdout); char *a[] = { bin, "selftest", (char *)model, NULL }; ok = up_exec(a, rp, NULL, 1200, "selftest", 0); }
    { char cx[4400]; snprintf(cx, sizeof cx, "%s/.katana", tmp); cb_rmtree(tmp); }
    if (!ok) { fprintf(stderr, "update: nada foi trocado\n"); return 1; }
    char ts[40]; const time_t t = time(NULL); strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", localtime(&t));
    char kp[1600]; snprintf(kp, sizeof kp, "%s/katana-%s", dir, ts);
    if (!up_copy(dst, kp)) { fprintf(stderr, "update: nao consegui guardar a versao atual; nada foi trocado\n"); return 1; }
    for (int i = 4; i < n; i++) { char o[1600]; snprintf(o, sizeof o, "%s/%s", dir, v[i]); unlink(o); }
    if (!up_copy(bin, dst)) { fprintf(stderr, "update: nao consegui trocar %s\n", dst); return 1; }
    char *vs[] = { dst, "version", NULL }; CbRes r;
    char **e = cb_env_new();
    if (!cb_exec(vs, rp, e, "", 20, &r) && r.out.p) printf("instalado: %s", r.out.p);
    cb_env_free(e);
    return 0;
}
#else
static int update_main(int argc, char **argv) { (void)argc; (void)argv; fprintf(stderr, "update: so no Linux por enquanto\n"); return 1; }
#endif
