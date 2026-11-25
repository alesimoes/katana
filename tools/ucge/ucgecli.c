/* ucgecli: o nucleo do UCGE compilado sozinho (docs/ucge.md).
 *   gcc -O1 -Wall -I. tools/ucge/ucgecli.c -o /tmp/ucgecli -lm
 *   ucgecli check <spec>              le, valida e escreve os erros (linha: mensagem) ou "ok"
 *   ucgecli gen <alvo> <spec> [--no-comments] [--width N] [--tests]    gera o codigo (alvo: python)
 *   ucgecli run <alvo> <spec>         gera com o harness de testes, roda em subprocesso com limites e conta passou/falhou
 *   ucgecli stats <spec>              resumo da UIR usada
 *   ucgecli selftest                  testes internos da tabela de tipos, unificacao e do pretty-printer
 *   ucgecli infer <spec> [--lets] [--uses] [--check]   inferencia de tipos (ucgeinfer.c): erros e o relatorio de tipos e certeza
 *   --infer em check/gen/run/stats liga a inferencia depois do validador */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include "src/coder/ucge.c"
#include "src/coder/ucgereg.c"
#include "src/coder/ucgecheck.c"
#include "src/coder/ucgetype.c"
#include "src/coder/ucgeinfer.c"
#include "src/coder/ucgedoc.c"
#include "src/coder/ucgepy.c"
#include "src/coder/ucgedep.c"
#include "src/coder/ucgeinc.c"

static char *slurp(const char *path) {
    FILE *f = strcmp(path, "-") ? fopen(path, "rb") : stdin; if (!f) { perror(path); exit(2); }
    size_t cap = 1 << 16, n = 0; char *b = (char *)malloc(cap);
    for (;;) { size_t r = fread(b + n, 1, cap - n - 1, f); n += r; if (n + 1 < cap) break; cap *= 2; b = (char *)realloc(b, cap); }
    b[n] = 0; if (f != stdin) fclose(f); return b;
}
static int g_infer;
static int load(Cgx *g, const char *path, int show) {
    char *src = slurp(path); CgxTypes T; cgx_ty_init(&T);
    const int ok = cgx_parse(g, src) && cgx_check_ex(g, &T, g_infer); free(src); cgx_ty_free(&T);
    if (show) for (int i = 0; i < g->nerr; i++) printf("line %d: %s\n", g->err[i].line, g->err[i].msg);
    return ok;
}
#include "tools/ucge/graphcli.h"
static int selftest(void) {
    int bad = 0;
#define T(c) do { if (!(c)) { printf("FAIL selftest: %s (line %d)\n", #c, __LINE__); bad++; } } while (0)
    CgxTypes T; cgx_ty_init(&T);
    CgxTid I = cgx_ty_prim(&T, TYK_INT), F = cgx_ty_prim(&T, TYK_FLOAT), S = cgx_ty_prim(&T, TYK_STR), A = cgx_ty_prim(&T, TYK_ANY);
    T(cgx_ty_list(&T, I) == cgx_ty_list(&T, I));                       /* hash-consing */
    T(cgx_ty_list(&T, I) != cgx_ty_list(&T, S));
    CgxTid v = cgx_ty_var(&T);
    T(cgx_ty_unify(&T, cgx_ty_list(&T, v), cgx_ty_list(&T, I)) && cgx_ty_find(&T, v) == I);
    T(!cgx_ty_unify(&T, I, S));
    T(cgx_ty_unify(&T, A, S));                                          /* gradual */
    T(cgx_ty_assignable(&T, F, I) && !cgx_ty_assignable(&T, I, F));     /* int <: float */
    CgxTid w = cgx_ty_var(&T);
    T(!cgx_ty_unify(&T, w, cgx_ty_list(&T, w)));                        /* ocorrencia */
    CgxTid u1 = cgx_ty_union(&T, (CgxTid[]){S, I, S}, 3), u2 = cgx_ty_union(&T, (CgxTid[]){I, S}, 2);
    T(u1 == u2);
    T(cgx_ty_union(&T, (CgxTid[]){I, I}, 2) == I);
    /* let-polimorfismo: id : forall a. a -> a, usada em int e em str */
    T.level = 1; CgxTid a = cgx_ty_var(&T); CgxTid idt = cgx_ty_fn(&T, &a, 1, a); T.level = 0;
    int nq; CgxTid sch = cgx_ty_generalize(&T, idt, &nq);
    T(nq == 1);
    CgxTid i1 = cgx_ty_instantiate(&T, sch, nq), i2 = cgx_ty_instantiate(&T, sch, nq);
    T(cgx_ty_unify(&T, i1, cgx_ty_fn(&T, &I, 1, I)));
    T(cgx_ty_unify(&T, i2, cgx_ty_fn(&T, &S, 1, S)));
    CgxB sb = {0}; cgx_ty_str(&T, sch, NULL, &sb); T(!strcmp(sb.p, "('a) -> 'a")); free(sb.p);
    cgx_ty_free(&T);
    /* pretty-printer: o mesmo grupo cabe ou quebra conforme a largura */
    CgxDocs D = {0};
    CgxDoc *items[3] = {cgx_dtext(&D, "alpha"), cgx_dtext(&D, "beta"), cgx_dtext(&D, "gamma")};
    CgxDoc *in = cgx_dcatn(&D, items[0], cgx_dtext(&D, ","), cgx_dline(&D), items[1], cgx_dtext(&D, ","), cgx_dline(&D), items[2], NULL);
    CgxDoc *grp = cgx_dgroup(&D, cgx_dcatn(&D, cgx_dtext(&D, "f("), cgx_dnest(&D, 4, cgx_dcat(&D, cgx_dsoft(&D), in)), cgx_dsoft(&D), cgx_dtext(&D, ")"), NULL));
    CgxB o1 = {0}, o2 = {0}; cgx_render(grp, 80, &o1); cgx_render(grp, 12, &o2);
    T(!strcmp(o1.p, "f(alpha, beta, gamma)"));
    T(!strcmp(o2.p, "f(\n    alpha,\n    beta,\n    gamma\n)"));
    free(o1.p); free(o2.p); cgx_docs_free(&D);
    /* interning */
    Cgx g; memset(&g, 0, sizeof g);
    T(cgx_intern(&g, "abc") == 1 && cgx_intern(&g, "xyz") == 2 && cgx_intern(&g, "abc") == 1 && !strcmp(cgx_symname(&g, 2), "xyz"));
    for (int i = 0; i < 1000; i++) { char t[20]; snprintf(t, sizeof t, "n%d", i); cgx_intern(&g, t); }
    T(cgx_intern(&g, "n500") == 503 && g.nsyms == 1002);
    cgx_free(&g);
    printf(bad ? "selftest: %d FAILED\n" : "selftest: ok\n", bad);
    return bad != 0;
}
static int run_python(const char *code, const char *tests, int *pass, int *fail, char *detail, size_t dcap) {
    char path[] = "/tmp/ucgeXXXXXX.py"; int fd = mkstemps(path, 3); if (fd < 0) return -1;
    if (write(fd, code, strlen(code)) < 0 || write(fd, "\n\n", 2) < 0 || write(fd, tests, strlen(tests)) < 0) { close(fd); return -1; }
    close(fd);
    char cmd[400]; snprintf(cmd, sizeof cmd, "(ulimit -t 10; ulimit -v 4000000; timeout 10 python3 -I %s) 2>&1", path);
    FILE *p = popen(cmd, "r"); char line[512]; *pass = *fail = 0; detail[0] = 0;
    while (p && fgets(line, sizeof line, p)) {
        if (!strncmp(line, "test ", 5)) { if (strstr(line, ": ok")) (*pass)++; else { (*fail)++; if (strlen(detail) + strlen(line) + 1 < dcap) strcat(detail, line); } }
        else if (strstr(line, "Traceback") || strstr(line, "Error:") || strstr(line, "ulimit")) { (*fail)++; if (strlen(detail) + strlen(line) + 1 < dcap) strcat(detail, line); }
    }
    if (p) pclose(p);
    unlink(path);
    return 0;
}
/* cachebench N DIR: mede a geracao de N funcoes encadeadas (f_i chama f_{i-1}) sem cache, com cache frio, quente e depois de mudar uma funcao */
static char *bench_spec(int n, int changed) {
    CgxB b = {0};
    for (int i = 0; i < n; i++) {
        cgx_bf(&b, "fn f%d(a: int, b: int) -> int\n  let t = a * %d + b\n  if t > 100\n    t = t - %d\n", i, i + 2, changed == i ? 7 : 1);
        if (i) cgx_bf(&b, "  t = t + f%d(a, t)\n", i - 1);
        cgx_bf(&b, "  return t\n");
    }
    return b.p;
}
static double bench_run(int n, int changed, const CgxBackend *be, CgxCache *c, CgxCacheRep *rp) {
    char *src = bench_spec(n, changed); Cgx g; CgxGenOpts o = {88, 1, 4}; CgxB code = {0};
    if (!cgx_parse(&g, src) || !cgx_check(&g)) { printf("bench spec invalid: %s\n", g.nerr ? g.err[0].msg : "?"); exit(1); }
    const double t0 = cgx_now_ms();
    if (c) cgx_gen_cached(&g, be, &o, c, "", 0, &code, NULL, rp); else be->gen(&g, &o, &code, NULL);
    const double t = cgx_now_ms() - t0;
    free(code.p); free(src); cgx_free(&g); return t;
}
static int cachebench(int n, const char *dir) {
    const CgxBackend *be = cgx_backend("python"); CgxCache c; CgxCacheRep rp;
    cgx_cache_open(&c, dir); cgx_cache_clear(&c);
    const double t_nc = bench_run(n, -1, be, NULL, NULL);
    const double t_cold = bench_run(n, -1, be, &c, &rp); const int cm = rp.misses;
    const double t_warm = bench_run(n, -1, be, &c, &rp); const int wh = rp.hits;
    const double t_leaf = bench_run(n, n - 1, be, &c, &rp); const int lm = rp.misses;
    const double t_root = bench_run(n, 0, be, &c, &rp); const int rm = rp.misses;
    printf("cachebench %d functions\n  no cache          %9.2f ms\n  cold cache        %9.2f ms  (%d misses)\n  warm cache        %9.2f ms  (%d hits)\n  change last fn    %9.2f ms  (%d misses: it has no dependents)\n  change first fn   %9.2f ms  (%d misses: every function depends on it)\n",
           n, t_nc, t_cold, cm, t_warm, wh, t_leaf, lm, t_root, rm);
    return 0;
}
int main(int argc, char **argv) {
    Cgx g;
    if (argc < 2) { fprintf(stderr, "uso: ucgecli check|gen|run|stats|selftest ...\n"); return 2; }
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--ops")) {          /* --ops ARQUIVO: operacoes do usuario (docs/ucge.md, registro) */
        char e[300]; cgx_reg_init();
        if (!cgx_reg_load_file(argv[i + 1], e, sizeof e)) { fprintf(stderr, "ops: %s: %s\n", argv[i + 1], e); return 2; }
        for (int j = i; j + 2 < argc; j++) argv[j] = argv[j + 2];
        argc -= 2; i--;
    }
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--infer")) g_infer = 1;
    if (!strcmp(argv[1], "infer") && argc >= 3) {
        int flags = 0, chk = 0; for (int i = 3; i < argc; i++) { if (!strcmp(argv[i], "--lets")) flags |= 1; else if (!strcmp(argv[i], "--uses")) flags |= 2; else if (!strcmp(argv[i], "--check")) chk = 1; }
        char *src = slurp(argv[2]); CgxTypes T; cgx_ty_init(&T); CgxB rep = {0};
        int ok = cgx_parse(&g, src) && (!chk || cgx_check(&g)); free(src);
        if (ok) ok = cgx_infer_ex(&g, &T, flags, &rep);
        for (int i = 0; i < g.nerr; i++) printf("line %d: %s\n", g.err[i].line, g.err[i].msg);
        if (rep.p) fputs(rep.p, stdout);
        printf("%s\n", ok ? "ok" : "errors");
        free(rep.p); cgx_ty_free(&T); cgx_free(&g); return !ok;
    }
    if (!strcmp(argv[1], "dep") && argc >= 3) {                  /* dep SPEC [--no-rec]: grafo, ordem, ciclos */
        int nocheck = 0, norec = 0; for (int i = 3; i < argc; i++) { if (!strcmp(argv[i], "--nocheck")) nocheck = 1; if (!strcmp(argv[i], "--no-rec")) norec = 1; }
        if (nocheck) { char *src = slurp(argv[2]); const int ok = cgx_parse(&g, src); free(src); if (!ok) { for (int i = 0; i < g.nerr; i++) printf("line %d: %s\n", g.err[i].line, g.err[i].msg); cgx_free(&g); return 1; } }
        else if (!load(&g, argv[2], 1)) { cgx_free(&g); return 1; }
        CgxDep d; cgx_dep_init(&d, 1); cgx_dep_add_spec(&d, &g, "main"); CgxB r = {0}, m = {0}; int nrec = 0;
        cgx_dep_report(&d, &r); const int errs = cgx_dep_cycles(&d, !norec, &m, &nrec);
        fputs(r.p, stdout); fputs(m.p ? m.p : "", stdout); printf("cycle errors %d recursion groups %d\n", errs, nrec);
        free(r.p); free(m.p); cgx_dep_free(&d); cgx_free(&g); return errs != 0;
    }
    if (!strcmp(argv[1], "cache") && argc >= 3) {               /* cache clear|trim|path [dir] */
        CgxCache c; cgx_cache_open(&c, argc > 3 ? argv[3] : NULL);
        if (!strcmp(argv[2], "clear")) printf("removed %d\n", cgx_cache_clear(&c));
        else if (!strcmp(argv[2], "trim")) printf("size %lld bytes, evicted %d\n", cgx_cache_trim(&c), c.evicted);
        else if (!strcmp(argv[2], "path")) printf("%s%s\n", c.dir, c.enabled ? "" : " (unavailable)");
        return 0;
    }
    if (!strcmp(argv[1], "key") && argc >= 3) {                  /* key SPEC: chave de cada unidade (para ver o que muda) */
        if (!load(&g, argv[2], 1)) { cgx_free(&g); return 1; }
        CgxDep d; cgx_dep_init(&d, 1); cgx_dep_add_spec(&d, &g, "main");
        CgxGenOpts o = {88, 1, 4}; cgx_dep_keys(&d, cgx_cache_ctx(cgx_backend("python"), &o, ""));
        for (int i = 0; i < d.nn; i++) printf("%s %016llx\n", d.n[i].name, (unsigned long long)d.n[i].key);
        cgx_dep_free(&d); cgx_free(&g); return 0;
    }
    if (!strcmp(argv[1], "inc") && argc >= 5 && !strcmp(argv[2], "apply")) {      /* inc apply ARQUIVO ESPEC [--write] [--acts]: sem --write imprime o resultado */
        char *spec = slurp(argv[4]); int wr = 0, acts = 0; for (int i = 5; i < argc; i++) { if (!strcmp(argv[i], "--write")) wr = 1; if (!strcmp(argv[i], "--acts")) acts = 1; }
        CgxIncResult R; CgxIncActs A = {0}; char msg[1000];
        const int ok = cgx_inc_apply_file(argv[3], spec, cgx_inc_write_atomic, NULL, !wr, &R, &A, msg, sizeof msg);
        if (!ok) { fprintf(stderr, "error: %s\n", msg); cgx_inc_acts_free(&A); free(spec); return 1; }
        for (int i = 0; i < R.nrep; i++) fprintf(stderr, "%s\n", R.rep[i].msg);
        fprintf(stderr, R.changed ? "changed\n" : "no change\n");
        if (acts) for (int i = 0; i < A.n; i++) fprintf(stderr, "act %d %s\n", A.a[i].kind, A.a[i].symbol);
        if (!wr && R.changed) fwrite(R.text, 1, R.n, stdout);
        cgx_inc_result_free(&R); cgx_inc_acts_free(&A); free(spec); return 0;
    }
    if (!strcmp(argv[1], "inc") && argc >= 4 && (!strcmp(argv[2], "read") || !strcmp(argv[2], "uir"))) {     /* inc read|uir ARQUIVO.py */
        char *src = slurp(argv[3]); CgxInc ic; CgxIncUir u;
        if (!cgx_inc_parse(&ic, src, strlen(src))) { printf("line %d: %s\n", ic.errline, ic.err); cgx_inc_free(&ic); free(src); return 1; }
        cgx_inc_uir(&ic, &u);
        if (!strcmp(argv[2], "uir")) fputs(u.csp.out.p, stdout);
        else {
            for (int i = 1; i < u.g.prog->b1.n + 1 && i - 1 < u.g.prog->b1.n; i++) {
                const CgxNode *nd = u.g.prog->b1.p[i - 1]; size_t b0, b1; int l0, l1;
                if (cgx_inc_range(&ic, &u, nd, &b0, &b1, &l0, &l1)) printf("%-6s %-14s lines %d-%d bytes %zu-%zu\n", nd->k == CGN_FN ? "fn" : nd->k == CGN_CLASS ? "class" : nd->k == CGN_LET ? "const" : nd->k == CGN_RAW ? "raw" : "node", nd->s ? nd->s : (nd->defs ? nd->defs : ""), l0, l1, b0, b1);
            }
        }
        cgx_inc_uir_free(&u); cgx_inc_free(&ic); free(src); return 0;
    }
    if (!strcmp(argv[1], "cachebench") && argc >= 3) return cachebench(atoi(argv[2]), argc > 3 ? argv[3] : NULL);
    if (!strcmp(argv[1], "dump") && argc >= 3) { if (!load(&g, argv[2], 1)) { cgx_free(&g); return 1; } CgxB b = {0}; cgx_dump(&g, 1, &b); fputs(b.p ? b.p : "", stdout); free(b.p); cgx_free(&g); return 0; }
    if (!strcmp(argv[1], "ops")) { CgxB b = {0}; if (argc > 2 && !strcmp(argv[2], "--dump")) cgx_reg_dump(&b); else cgx_reg_list(&b); fputs(b.p ? b.p : "", stdout); free(b.p); return 0; }
    if (!strcmp(argv[1], "selftest")) return selftest();
    if (!strcmp(argv[1], "check") && argc >= 3) { const int ok = load(&g, argv[2], 1); printf("%s\n", ok ? "ok" : "errors"); cgx_free(&g); return !ok; }
    if (!strcmp(argv[1], "stats") && argc >= 3) {
        if (!load(&g, argv[2], 1)) { cgx_free(&g); return 1; }
        CgxStats st; cgx_stats(&g, &st);
        printf("functions %d classes %d tests %d raw %d level L%d ops: %s\n", st.nfunc, st.nclass, st.ntest, st.nraw, st.level, st.ops);
        cgx_free(&g); return 0;
    }
    if ((!strcmp(argv[1], "gen") || !strcmp(argv[1], "run")) && argc >= 4) {
        const CgxBackend *be = cgx_backend(argv[2]);
        if (!be) { fprintf(stderr, "alvo desconhecido: %s\n", argv[2]); return 2; }
        CgxGenOpts o = {88, 1, 4}; int tests = !strcmp(argv[1], "run");
        for (int i = 4; i < argc; i++) { if (!strcmp(argv[i], "--no-comments")) o.comments = 0; else if (!strcmp(argv[i], "--width") && i + 1 < argc) o.width = atoi(argv[++i]); else if (!strcmp(argv[i], "--tests")) tests = 1; }
        if (!load(&g, argv[3], 1)) { printf("errors\n"); cgx_free(&g); return 1; }
        for (int i = 4; i < argc; i++) if (!strcmp(argv[i], "--simp")) { const int c = cgx_reg_simplify(&g); fprintf(stderr, "simplified %d\n", c); }
        CgxB code = {0}, tb = {0}; int ok; const char *cdir = NULL, *flag = NULL;
        for (int i = 4; i < argc; i++) if (!strcmp(argv[i], "--cache")) { flag = "1"; if (i + 1 < argc && argv[i + 1][0] != '-') cdir = argv[i + 1]; }
        if (flag) {
            CgxCache c; CgxCacheRep rp; cgx_cache_open(&c, cdir);
            int topo = 0; for (int i = 4; i < argc; i++) if (!strcmp(argv[i], "--topo")) topo = 1;
            ok = cgx_gen_cached(&g, be, &o, &c, "", topo, &code, tests ? &tb : NULL, &rp);
            fprintf(stderr, "[cache] units %d hits %d misses %d corrupt %d dep %.2f ms gen %.2f ms total %.2f ms\n", rp.units, rp.hits, rp.misses, rp.corrupt, rp.ms_dep, rp.ms_gen, rp.ms_total);
        } else ok = be->gen(&g, &o, &code, tests ? &tb : NULL);
        for (int i = 0; i < g.nerr; i++) printf("line %d: %s\n", g.err[i].line, g.err[i].msg);
        if (!ok) { printf("errors\n"); free(code.p); free(tb.p); cgx_free(&g); return 1; }
        if (!strcmp(argv[1], "gen")) { fputs(code.p ? code.p : "", stdout); if (tb.p) { printf("\n"); fputs(tb.p, stdout); } }
        else {
            int pass = 0, fail = 0; char det[2000];
            if (!tb.p) { printf("no tests\n"); }
            else if (!strcmp(be->name, "python")) {
                run_python(code.p ? code.p : "", tb.p, &pass, &fail, det, sizeof det);
                printf("tests: %d passed, %d failed\n%s", pass, fail, det);
                free(code.p); free(tb.p); cgx_free(&g); return fail != 0;
            }
        }
        free(code.p); free(tb.p); cgx_free(&g); return 0;
    }
    { const int rc = graph_cli(argc, argv); if (rc >= 0) return rc; }
    fprintf(stderr, "uso: ucgecli check|gen|run|stats|selftest|ir|flow|opt|optgen ...\n");
    return 2;
}
