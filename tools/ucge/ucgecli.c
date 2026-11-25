/* ucgecli: o nucleo do UCGE compilado sozinho (docs/ucge.md).
 *   gcc -O1 -Wall -I. tools/ucge/ucgecli.c -o /tmp/ucgecli -lm
 *   ucgecli check <spec>              le, valida e escreve os erros (linha: mensagem) ou "ok"
 *   ucgecli gen <alvo> <spec> [--no-comments] [--width N] [--tests]    gera o codigo (alvo: python, js, ts)
 *   ucgecli run <alvo> <spec>         gera com o harness de testes, roda em subprocesso com limites e conta passou/falhou
 *   ucgecli stats <spec>              resumo da UIR usada
 *   ucgecli selftest                  testes internos da tabela de tipos, unificacao e do pretty-printer */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <unistd.h>
#include <sys/wait.h>
#include "src/coder/ucge.c"
#include "src/coder/ucgecheck.c"
#include "src/coder/ucgetype.c"
#include "src/coder/ucgedoc.c"
#include "src/coder/ucgepy.c"
#include "src/coder/ucgejs.c"

static char *slurp(const char *path) {
    FILE *f = strcmp(path, "-") ? fopen(path, "rb") : stdin; if (!f) { perror(path); exit(2); }
    size_t cap = 1 << 16, n = 0; char *b = (char *)malloc(cap);
    for (;;) { size_t r = fread(b + n, 1, cap - n - 1, f); n += r; if (n + 1 < cap) break; cap *= 2; b = (char *)realloc(b, cap); }
    b[n] = 0; if (f != stdin) fclose(f); return b;
}
static int load(Cgx *g, const char *path, int show) {
    char *src = slurp(path); const int ok = cgx_parse(g, src) && cgx_check(g); free(src);
    if (show) for (int i = 0; i < g->nerr; i++) printf("line %d: %s\n", g->err[i].line, g->err[i].msg);
    return ok;
}
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
/* JavaScript e TypeScript: node (v20+); o TypeScript passa antes pelo tsc que a variavel UCGE_TSC aponta (um tsc executavel), sem ele so gera */
static int run_node(const char *code, const char *tests, int ts, int *pass, int *fail, char *detail, size_t dcap) {
    char dir[] = "/tmp/ucgenodeXXXXXX"; if (!mkdtemp(dir)) return -1;
    char path[300]; snprintf(path, sizeof path, "%s/prog.%s", dir, ts ? "ts" : "js");
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    fputs(code, f); fputs("\n\n", f); fputs(tests, f); fclose(f);
    char cmd[1200];
    if (ts) {
        const char *tsc = getenv("UCGE_TSC"); if (!tsc) { snprintf(detail, dcap, "typescript: set UCGE_TSC to a tsc (tsc.js or executable) to run\n"); *fail = 1; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if (system(cmd)) {} return 0; }
        snprintf(cmd, sizeof cmd, "(cd %s && '%s' --strict --target es2022 --module commonjs --lib es2022,dom --skipLibCheck --outDir out prog.ts 2>&1 && ulimit -t 10 && timeout 10 node out/prog.js) 2>&1", dir, tsc);
    } else snprintf(cmd, sizeof cmd, "(cd %s && ulimit -t 10 && timeout 10 node prog.js) 2>&1", dir);
    FILE *p = popen(cmd, "r"); char line[512]; *pass = *fail = 0; detail[0] = 0;
    while (p && fgets(line, sizeof line, p)) {
        if (!strncmp(line, "test ", 5)) { if (strstr(line, ": ok")) (*pass)++; else { (*fail)++; if (strlen(detail) + strlen(line) + 1 < dcap) strcat(detail, line); } }
        else if (strstr(line, "Error") || strstr(line, "error TS") || strstr(line, "ulimit")) { (*fail)++; if (strlen(detail) + strlen(line) + 1 < dcap) strcat(detail, line); }
    }
    if (p) pclose(p);
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if (system(cmd)) {}
    return 0;
}
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: ucgecli check|gen|run|stats|selftest ...\n"); return 2; }
    if (!strcmp(argv[1], "selftest")) return selftest();
    Cgx g;
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
        CgxB code = {0}, tb = {0};
        const int ok = be->gen(&g, &o, &code, tests ? &tb : NULL);
        for (int i = 0; i < g.nerr; i++) printf("line %d: %s\n", g.err[i].line, g.err[i].msg);
        if (!ok) { printf("errors\n"); free(code.p); free(tb.p); cgx_free(&g); return 1; }
        if (!strcmp(argv[1], "gen")) { fputs(code.p ? code.p : "", stdout); if (tb.p) { printf("\n"); fputs(tb.p, stdout); } }
        else {
            int pass = 0, fail = 0; char det[2000];
            if (!tb.p) { printf("no tests\n"); }
            else if (!strcmp(be->name, "javascript") || !strcmp(be->name, "typescript")) {
                run_node(code.p ? code.p : "", tb.p, !strcmp(be->name, "typescript"), &pass, &fail, det, sizeof det);
                printf("tests: %d passed, %d failed\n%s", pass, fail, det);
                free(code.p); free(tb.p); cgx_free(&g); return fail != 0;
            }
            else if (!strcmp(be->name, "python")) {
                run_python(code.p ? code.p : "", tb.p, &pass, &fail, det, sizeof det);
                printf("tests: %d passed, %d failed\n%s", pass, fail, det);
                free(code.p); free(tb.p); cgx_free(&g); return fail != 0;
            }
        }
        free(code.p); free(tb.p); cgx_free(&g); return 0;
    }
    fprintf(stderr, "uso: ucgecli check|gen|run|stats|selftest ...\n");
    return 2;
}
