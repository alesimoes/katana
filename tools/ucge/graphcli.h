/* graphcli.h: comandos de linha do modulo de grafos do UCGE (docs/ucge.md, "UIR-G"), incluido por ucgecli.c depois de slurp() e load().
 *   ucgecli ir <spec>                       CFG em SSA por funcao (blocos, dominadores, phi), mais o verificador de invariantes
 *   ucgecli flow <spec>                     relatorio das analises (codigo morto, variaveis, retornos, lacos, chamadas) com a certeza de cada achado
 *   ucgecli opt <spec> [--all] [--passes N] otimiza e diz o que mudou
 *   ucgecli optgen <alvo> <spec> [opcoes]   otimiza e gera o codigo do alvo (as mesmas opcoes de gen mais as de opt)
 *   ucgecli irexec <spec> <fn> <fase> args  interpretador da IR (so escalares): fase pre, ssa ou out; confere SSA e saida de SSA contra o Python */
#include "src/coder/ucgecfg.c"
#include "src/coder/ucgeflow.c"
#include "src/coder/ucgeopt.c"
static int graph_cli(int argc, char **argv) {
    Cgx g;
    if (!strcmp(argv[1], "ir") && argc >= 3) {
        if (!load(&g, argv[2], 1)) { cgx_free(&g); return 1; }
        CgxIr *ir = cgx_ir_build(&g); CgxB o = {0}; int bad = 0;
        cgx_ir_dump(ir, &o); fputs(o.p ? o.p : "", stdout);
        for (int i = 0; i < ir->nfn; i++) { char m[300]; if (cgx_ir_verify_fn(&ir->fn[i], m, sizeof m)) { printf("VERIFY FAIL %s: %s\n", ir->fn[i].name, m); bad++; } }
        printf("verify: %s\n", bad ? "FAILED" : "ok");
        free(o.p); cgx_ir_free(ir); cgx_free(&g); return bad != 0;
    }
    if (!strcmp(argv[1], "flow") && argc >= 3) {
        if (!load(&g, argv[2], 1)) { cgx_free(&g); return 1; }
        CgxIr *ir = cgx_ir_build_ex(&g, 1, argc > 3 && !strcmp(argv[3], "--globals")); CgxFlowReport r; cgx_flow_run(ir, &r);
        CgxB o = {0}; cgx_report_str(&r, &o); fputs(o.p ? o.p : "", stdout);
        int *ord = (int *)malloc(sizeof(int) * (size_t)(ir->nfn + 1)); const int n = cgx_cg_order(ir, ord);
        printf("generation order (callees first):"); for (int i = 0; i < n; i++) printf(" %s", ir->fn[ord[i]].name); printf("\n"); free(ord);
        printf("functions %d, blocks %d, instructions %d, phi %d, loops %d, unreachable blocks %d, pure-or-total functions %d, recursive %d; worklist visits: reach %ld avail %ld definite %ld, sccp evals %ld; findings: %d warnings\n",
               r.nfn, r.nblk, r.nins, r.nphi, r.nloop, r.nunreach, r.npure, r.nrec, r.visits_reach, r.visits_avail, r.visits_da, r.sccp_evals, cgx_report_warnings(&r));
        free(o.p); cgx_report_free(&r); cgx_ir_free(ir); cgx_free(&g); return 0;
    }
    if ((!strcmp(argv[1], "opt") || !strcmp(argv[1], "optgen")) && argc >= 3) {
        const int gen = !strcmp(argv[1], "optgen"); const char *target = gen ? argv[2] : NULL; const char *path = gen ? argv[3] : argv[2];
        if (gen && argc < 4) { fprintf(stderr, "uso: ucgecli optgen <alvo> <spec>\n"); return 2; }
        CgxOptOpts oo = {CGX_O_DEFAULT, 0, 0}; CgxGenOpts go = {88, 1, 4}; int recheck = 0;
        for (int i = gen ? 4 : 3; i < argc; i++) {
            if (!strcmp(argv[i], "--recheck")) recheck = 1; else if (!strcmp(argv[i], "--all")) oo.passes = CGX_O_ALL; else if (!strcmp(argv[i], "--passes") && i + 1 < argc) oo.passes = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--no-comments")) go.comments = 0; else if (!strcmp(argv[i], "--width") && i + 1 < argc) go.width = atoi(argv[++i]);
        }
        if (!load(&g, path, 1)) { printf("errors\n"); cgx_free(&g); return 1; }
        CgxOptReport rep; cgx_opt_run(&g, &oo, &rep);
        CgxB o = {0}; cgx_opt_report_str(&rep, &o);
        if (recheck) { g.nerr = g.nerr_all = 0; if (!cgx_check(&g)) { for (int i = 0; i < g.nerr; i++) printf("recheck line %d: %s\n", g.err[i].line, g.err[i].msg); printf("RECHECK FAILED\n"); free(o.p); cgx_free(&g); return 1; } }
        if (!gen) { fputs(o.p, stdout); free(o.p); cgx_free(&g); return 0; }
        const CgxBackend *be = cgx_backend(target); if (!be) { fprintf(stderr, "alvo desconhecido: %s\n", target); cgx_free(&g); return 2; }
        CgxB code = {0}; const int ok = be->gen(&g, &go, &code, NULL);
        for (int i = 0; i < g.nerr; i++) printf("line %d: %s\n", g.err[i].line, g.err[i].msg);
        if (ok) fputs(code.p ? code.p : "", stdout);
        fprintf(stderr, "%s", o.p); free(o.p); free(code.p); cgx_free(&g); return !ok;
    }
    return -1;
}
