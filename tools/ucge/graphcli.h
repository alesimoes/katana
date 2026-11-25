/* graphcli.h: comandos de linha do modulo de grafos do UCGE (docs/ucge.md, "UIR-G"), incluido por ucgecli.c depois de slurp() e load().
 *   ucgecli ir <spec>                       CFG em SSA por funcao (blocos, dominadores, phi), mais o verificador de invariantes
 *   ucgecli flow <spec>                     relatorio das analises (codigo morto, variaveis, retornos, lacos, chamadas) com a certeza de cada achado
 *   ucgecli opt <spec> [--all] [--passes N] otimiza e diz o que mudou
 *   ucgecli optgen <alvo> <spec> [opcoes]   otimiza e gera o codigo do alvo (as mesmas opcoes de gen mais as de opt)
 *   ucgecli irexec <spec> <fn> <fase> args  interpretador da IR (so escalares): fase pre, ssa ou out; confere SSA e saida de SSA contra o Python */
#include "src/coder/ucgecfg.c"
#include "src/coder/ucgeflow.c"
#include "src/coder/ucgeopt.c"

/* ---- interpretador da IR (so escalares): executa o CFG antes da SSA, em SSA (phi pela aresta de chegada) e depois da saida de SSA ---- */
typedef struct { CgxIr *ir; long steps; int depth; } Ix;
enum { IXR_OK, IXR_EXC, IXR_UNSUP };
typedef struct { CgxCv v; char msg[160]; int sys; } IxRes;
static CgxCv ix_get(const CgxFnIr *f, const CgxCv *vals, const CgxCv *vars, int x, int *bad) {
    CgxCv u; memset(&u, 0, sizeof u); u.k = CV_TOP;
    if (x == CGX_NIL) { *bad = 1; return u; }
    if (CGX_ISVAR(x)) { (void)f; return vars[CGX_VARIDX(x)]; }
    if (x == 0) { *bad = 1; return u; }
    return vals[x];
}
static int ix_call(Ix *X, int fi, const CgxCv *args, int na, IxRes *res);
static int ix_run(Ix *X, const CgxFnIr *f, const CgxCv *args, int na, IxRes *res) {
    memset(res, 0, sizeof *res);
    CgxCv *vals = (CgxCv *)calloc((size_t)f->nval + 1, sizeof(CgxCv)), *vars = (CgxCv *)calloc((size_t)f->nvar + 1, sizeof(CgxCv));
    for (int i = 0; i <= f->nval; i++) { vals[i].k = CV_TOP; }
    for (int i = 0; i <= f->nvar; i++) { vars[i].k = CV_TOP; }
    int b = f->entry, prev = -1, rc = IXR_UNSUP; CgxCv pend; memset(&pend, 0, sizeof pend); int pend_sys = 0; char pendmsg[160], excmsg[160]; pendmsg[0] = excmsg[0] = 0;
    for (;;) {
        const CgxBB *bb = &f->bb[b];
        /* phis em paralelo */
        { CgxCv tmp[64]; int ti[64], nt = 0;
          for (int k = 0; k < bb->ins.n && nt < 64; k++) { const CgxIns *x = &f->ins[bb->ins.p[k]]; if (x->op != IO_PHI) break;
              const int slot = prev >= 0 ? cgx_dg_pslot(&f->g, prev, b) : -1; if (slot < 0) { rc = IXR_UNSUP; goto done; }
              int bad = 0; tmp[nt] = ix_get(f, vals, vars, x->args[slot], &bad); if (bad) { rc = IXR_UNSUP; goto done; } ti[nt++] = x->dst; }
          for (int k = 0; k < nt; k++) vals[ti[k]] = tmp[k]; }
        int jumped = 0;
        for (int k = 0; k < bb->ins.n && !jumped; k++) {
            const CgxIns *x = &f->ins[bb->ins.p[k]]; int bad = 0; CgxCv r; memset(&r, 0, sizeof r); int raise = 0; const char *rm = NULL; int rsys = 0;
            if (++X->steps > 400000) { rc = IXR_UNSUP; goto done; }
            #define GETA ix_get(f, vals, vars, x->a, &bad)
            switch (x->op) {
                case IO_PHI: continue;
                case IO_PARAM: if (x->sub < 0) { rc = IXR_UNSUP; goto done; } if (x->sub < na) r = args[x->sub]; else { const CgxNode *d = f->fn->par[x->sub].def; if (!d || !cgx_is_lit(d)) { rc = IXR_UNSUP; goto done; } r = cgx_cv_of_node(d); } break;
                case IO_CONST: r = cgx_cv_of_node(x->node); if (r.k < CV_INT) { rc = IXR_UNSUP; goto done; } break;
                case IO_GLOBAL: if (x->callee && x->callee->a && cgx_is_lit(x->callee->a)) r = cgx_cv_of_node(x->callee->a); else { rc = IXR_UNSUP; goto done; } break;
                case IO_COPY: r = GETA; break;
                case IO_BIN: {
                    CgxCv a = GETA, c = ix_get(f, vals, vars, x->b, &bad); if (bad) break;
                    if ((x->sub == CGO_DIV || x->sub == CGO_IDIV || x->sub == CGO_MOD) && ((c.k == CV_INT && c.iv == 0) || (c.k == CV_FLT && c.dv == 0.0))) { raise = 1; rm = "division by zero"; rsys = 1; break; }
                    if (!cgx_cv_bin(X->ir->g, x->sub, &a, &c, &r)) { rc = IXR_UNSUP; goto done; }
                    break;
                }
                case IO_UN: { CgxCv a = GETA; if (bad) break; if (!cgx_cv_un(x->sub, &a, &r)) { rc = IXR_UNSUP; goto done; } break; }
                case IO_CALL: {
                    CgxCv av[8]; if (x->nargs > 8) { rc = IXR_UNSUP; goto done; }
                    for (int j = 0; j < x->nargs; j++) av[j] = ix_get(f, vals, vars, x->args[j], &bad);
                    if (bad) break;
                    if (x->sub && !x->callee) {
                        if (x->sub == CGB_ERRMSG) { if (av[0].k != CV_STR || av[0].iv == 1) { rc = IXR_UNSUP; goto done; } r = av[0]; r.iv = 0; break; }
                        if (!cgx_cv_builtin(X->ir->g, x->sub, av, x->nargs, &r)) { rc = IXR_UNSUP; goto done; }
                    } else if (x->callee && x->callee->k == CGN_FN && x->callee->id > 0 && X->ir->fnid[x->callee->id] >= 0) {
                        IxRes cr; const int cc = ix_call(X, X->ir->fnid[x->callee->id], av, x->nargs, &cr);
                        if (cc == IXR_UNSUP) { rc = IXR_UNSUP; goto done; }
                        if (cc == IXR_EXC) { raise = 1; snprintf(excmsg, sizeof excmsg, "%s", cr.msg); rm = excmsg; rsys = cr.sys; } else r = cr.v;
                    } else { rc = IXR_UNSUP; goto done; }
                    break;
                }
                case IO_CATCH: r = pend; r.iv = pend_sys; break;
                case IO_BR: {
                    CgxCv c = GETA; if (bad || c.k != CV_BOOL) { rc = IXR_UNSUP; goto done; }
                    int tgt = -1; for (int e = f->g.sp[b]; e < f->g.sp[b + 1]; e++) if ((f->ekind[e] == EK_TRUE) == (c.iv != 0) && f->ekind[e] != EK_EXC) tgt = f->g.s[e];
                    if (tgt < 0) { rc = IXR_UNSUP; goto done; }
                    prev = b; b = tgt; jumped = 1; continue;
                }
                case IO_JMP: { int tgt = -1; for (int e = f->g.sp[b]; e < f->g.sp[b + 1]; e++) if (f->ekind[e] != EK_EXC) tgt = f->g.s[e]; if (tgt < 0) { rc = IXR_UNSUP; goto done; } prev = b; b = tgt; jumped = 1; continue; }
                case IO_RET: { if (x->a == CGX_NIL) { rc = IXR_UNSUP; goto done; } CgxCv a = GETA; if (bad) { rc = IXR_UNSUP; goto done; } res->v = a; rc = IXR_OK; goto done; }
                case IO_THROW: {
                    if (x->flags & IF_RETHROW) { raise = 1; snprintf(excmsg, sizeof excmsg, "%s", pend.s ? pend.s : ""); rm = excmsg; rsys = pend_sys; break; }
                    CgxCv a = GETA; if (bad || a.k != CV_STR) { rc = IXR_UNSUP; goto done; } raise = 1; rm = a.s; rsys = 0; break;
                }
                default: rc = IXR_UNSUP; goto done;
            }
            #undef GETA
            if (bad) { rc = IXR_UNSUP; goto done; }
            if (raise) {
                int tgt = -1; for (int e = f->g.sp[b]; e < f->g.sp[b + 1]; e++) if (f->ekind[e] == EK_EXC) tgt = f->g.s[e];
                if (tgt < 0) { snprintf(res->msg, sizeof res->msg, "%s", rm ? rm : ""); res->sys = rsys; rc = IXR_EXC; goto done; }
                snprintf(pendmsg, sizeof pendmsg, "%s", rm ? rm : ""); memset(&pend, 0, sizeof pend); pend.k = CV_STR; pend.s = pendmsg; pend_sys = rsys; prev = b; b = tgt; jumped = 1; continue;
            }
            if (x->dst != CGX_NIL) { if (CGX_ISVAR(x->dst)) vars[CGX_VARIDX(x->dst)] = r; else if (x->dst > 0) vals[x->dst] = r; }
            if (x->d2 != CGX_NIL) { rc = IXR_UNSUP; goto done; }
        }
        if (!jumped) { rc = IXR_UNSUP; goto done; }
    }
done:
    free(vals); free(vars);
    return rc;
}
static int ix_call(Ix *X, int fi, const CgxCv *args, int na, IxRes *res) {
    if (++X->depth > 60) { X->depth--; return IXR_UNSUP; }
    const int rc = ix_run(X, &X->ir->fn[fi], args, na, res); X->depth--; return rc;
}
static int irexec_cli(const char *path, const char *phase) {
    Cgx g; if (!load(&g, path, 1)) { cgx_free(&g); return 1; }
    const int pre = !strcmp(phase, "pre"), out = !strcmp(phase, "out");
    CgxIr *ir = cgx_ir_build_ex(&g, pre ? 0 : 1, 0);
    if (out) for (int i = 0; i < ir->nfn; i++) cgx_ssa_out(&ir->fn[i]);
    Ix X = {ir, 0, 0}; char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        char *tok[10]; int nt = 0; for (char *p = strtok(line, " \r\n"); p && nt < 10; p = strtok(NULL, " \r\n")) tok[nt++] = p;
        if (!nt) continue;
        int fi = -1; for (int i = 0; i < ir->nfn; i++) if (!ir->fn[i].cls && !strcmp(ir->fn[i].name, tok[0])) fi = i;
        if (fi < 0) { printf("U nofn\n"); continue; }
        CgxCv av[9]; int na = nt - 1;
        for (int i = 0; i < na; i++) {
            memset(&av[i], 0, sizeof av[i]); const char *t = tok[i + 1];
            if (!strcmp(t, "true") || !strcmp(t, "false")) { av[i].k = CV_BOOL; av[i].iv = t[0] == 't'; }
            else if (t[0] == 's' && t[1] == ':') { av[i].k = CV_STR; av[i].s = t + 2; }
            else if (strpbrk(t, ".eE")) { av[i].k = CV_FLT; av[i].dv = atof(t); }
            else { av[i].k = CV_INT; av[i].iv = atoll(t); }
        }
        X.steps = 0; X.depth = 0; X.ir->nfn = ir->nfn; IxRes r; const int rc = ix_call(&X, fi, av, na, &r);
        if (rc == IXR_UNSUP) printf("U\n");
        else if (rc == IXR_EXC) { if (r.sys) printf("E ZeroDivisionError\n"); else printf("E Exception %s\n", r.msg); }
        else if (r.v.k == CV_INT) printf("R int %lld\n", r.v.iv);
        else if (r.v.k == CV_FLT) printf("R float %.17g\n", r.v.dv);
        else if (r.v.k == CV_BOOL) printf("R bool %s\n", r.v.iv ? "true" : "false");
        else if (r.v.k == CV_STR) printf("R str %s\n", r.v.s);
        else printf("U\n");
    }
    cgx_ir_free(ir); cgx_free(&g); return 0;
}
static int graph_cli(int argc, char **argv) {
    if (!strcmp(argv[1], "irexec") && argc >= 4) return irexec_cli(argv[2], argv[3]);
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
        CgxOptOpts oo = {CGX_O_DEFAULT, 0, 0}; CgxGenOpts go = {88, 1, 4}; int recheck = 0, tests = 0;
        for (int i = gen ? 4 : 3; i < argc; i++) {
            if (!strcmp(argv[i], "--recheck")) recheck = 1; else if (!strcmp(argv[i], "--tests")) tests = 1; else if (!strcmp(argv[i], "--all")) oo.passes = CGX_O_ALL; else if (!strcmp(argv[i], "--passes") && i + 1 < argc) oo.passes = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--no-comments")) go.comments = 0; else if (!strcmp(argv[i], "--width") && i + 1 < argc) go.width = atoi(argv[++i]);
        }
        if (!load(&g, path, 1)) { printf("errors\n"); cgx_free(&g); return 1; }
        CgxOptReport rep; cgx_opt_run(&g, &oo, &rep);
        CgxB o = {0}; cgx_opt_report_str(&rep, &o);
        if (recheck) { g.nerr = g.nerr_all = 0; if (!cgx_check(&g)) { for (int i = 0; i < g.nerr; i++) printf("recheck line %d: %s\n", g.err[i].line, g.err[i].msg); printf("RECHECK FAILED\n"); free(o.p); cgx_free(&g); return 1; } }
        if (!gen) { fputs(o.p, stdout); free(o.p); cgx_free(&g); return 0; }
        const CgxBackend *be = cgx_backend(target); if (!be) { fprintf(stderr, "alvo desconhecido: %s\n", target); cgx_free(&g); return 2; }
        CgxB code = {0}, tb = {0}; const int ok = be->gen(&g, &go, &code, tests ? &tb : NULL);
        for (int i = 0; i < g.nerr; i++) printf("line %d: %s\n", g.err[i].line, g.err[i].msg);
        if (ok) { fputs(code.p ? code.p : "", stdout); if (tb.p) { printf("\n"); fputs(tb.p, stdout); } }
        fprintf(stderr, "%s", o.p); free(o.p); free(code.p); free(tb.p); cgx_free(&g); return !ok;
    }
    return -1;
}
