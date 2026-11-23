/* ---------- x-batch: chamadas independentes na MESMA fala do modelo ----------
 * O formato do modelo aceita varias chamadas por fala. Quando duas ou tres leituras nao dependem uma da outra (dois ou tres Read,
 * o mesmo Grep em duas pastas, Find + Glob, dois Find), elas saem juntas (x_calls: cada uma passa pelo despacho do motor e tem o
 * seu resultado), e o passo seguinte usa os resultados juntos. Cinco situacoes; as leituras que dependem do resultado anterior
 * continuam em falas separadas. */
enum { XB_READS2, XB_READS3, XB_GREP2, XB_FINDGLOB, XB_FIND2, XB_N };
static const char *const g_xb_names[XB_N] = {"reads2", "reads3", "grep2", "find-glob", "find2"};
static const char *x6_rd_args(const Scn *s, const char *rel, int limit) {
    return limit ? cu_fmt("{\"file_path\": %s, \"offset\": 1, \"limit\": %d}", cu_q(x_abs(s, rel)), limit) : cu_fmt("{\"file_path\": %s}", cu_q(x_abs(s, rel)));
}
/* linhas de resultado de Grep em modo content */
static int x6_grep_lines(const char *r) { int n = 0; for (const char *l = r; *l; ) { const size_t k = strcspn(l, "\n"); if (k && *l != '[') n++; l += k; if (*l) l++; } return n; }
static void fam_x_batch(FILE *f, Scn *s) {
    int sit, v, stk, fu;
    for (int t = 0; ; t++) {
        if (t > 400) cu_die("sem celula de batch");
        sit = cu_n(XB_N); v = cu_n(3); stk = CU_PA(((const int[]){X_PY, X_JS, X_SH, X_C, X_PL})); fu = cu_pct(45);
        if ((sit == XB_FINDGLOB || sit == XB_FIND2) && stk == X_PL) continue;      /* o Find nao indexa Perl */
        if (x_held(190 + sit, v, stk, fu, 0) == (g_cu_hold != 0)) break;
    }
    static XProj P; XProj *p = &P;
    x6_base(p, s, stk, 2 + cu_n(2));
    int ci = cu_ix(X6_NC), mi = 0, mj = 0, cj = 0; XFi *fi = NULL, *fj = NULL;
    if (sit == XB_FINDGLOB || sit == XB_FIND2) {
        fi = x6_place(p, g_x6_c[ci].fn, -1, &mi);
        if (sit == XB_FIND2) { do cj = cu_ix(X6_NC); while (cj == ci); fj = x6_place(p, g_x6_c[cj].fn, mi, &mj); if (x_fi(p, g_x6_c[ci].fn, &mi) != fi) cu_die("o primeiro foi trocado"); }
    } else { mi = cu_n(p->nmod); fi = &p->mod[mi].fn[cu_n(p->mod[mi].nfn)]; }
    x_begin("x-batch", s, p, cu_pct(15) ? 2 : 0, cu_pct(12) ? 1 : 0);
    x6_selftest(p);
    x_cell("batch/%s/v%d/%s/fu%d/conv%d", g_xb_names[sit], v, g_x_stid[p->st], fu, p->brief ? 2 : p->readme ? 1 : 0);
    const char *td = p->st == X_JS ? "test" : "tests";
    if (sit == XB_READS2) {
        const int mj2 = (mi + 1 + cu_n(p->nmod - 1)) % p->nmod; const char *a = x_src(p, mi), *b = x_src(p, mj2);
        x_user(cu_fmt(v == 0 ? L("show me how %s and %s start", "me mostra como comecam %s e %s") : v == 1 ? L("compare %s with %s: what does each one define?", "compara %s com %s: o que cada um define?") : L("I need the first lines of %s and of %s", "preciso das primeiras linhas de %s e de %s"), a, b));
        const char *nm[2] = {"Read", "Read"}, *ar[2] = {x6_rd_args(s, a, 0), x6_rd_args(s, b, 0)}, *res[2];
        x_calls(L("Both reads are independent: one turn.", "As duas leituras sao independentes: uma fala so."), 2, nm, ar, res);
        Buf o = {0}; buf_str(&o, "");
        const XMod *ms[2] = {&p->mod[mi], &p->mod[mj2]}; const char *rel[2] = {a, b};
        for (int k = 0; k < 2; k++) {
            Buf fl = {0}; buf_str(&fl, "");
            for (int i = 0; i < ms[k]->nfn; i++) { if (!strstr(res[k], x_deflit(p, ms[k]->fn[i].name))) cu_die("a leitura nao tem %s", ms[k]->fn[i].name); buf_str(&fl, cu_fmt("%s`%s`", fl.n ? ", " : "", ms[k]->fn[i].name)); }
            buf_str(&o, cu_fmt("%s%s: %s.", k ? " " : "", rel[k], fl.p)); free(fl.p);
        }
        x_final(cu_fmt(L("%s Nothing was changed.", "%s Nada foi alterado."), o.p)); free(o.p);
        if (fu) {
            x_dep();
            x_user(XV(L("which of the two has more functions?", "qual dos dois tem mais funcoes?"), L("and which one defines more?", "e qual deles define mais?")));
            const int na = p->mod[mi].nfn, nb = p->mod[mj2].nfn;
            if (na == nb) x_final(cu_fmt(L("Neither: both define %d functions, as the reads above show.", "Nenhum: os dois definem %d funcoes, como as leituras acima mostram."), na));
            else x_final(cu_fmt(L("%s: it defines %d functions against %d in %s, as the reads above show.", "%s: ele define %d funcoes contra %d em %s, como as leituras acima mostram."), na > nb ? a : b, na > nb ? na : nb, na > nb ? nb : na, na > nb ? b : a));
        }
    } else if (sit == XB_READS3) {
        x_user(cu_fmt(v == 0 ? L("give me one line per module of %s", "me da uma linha por modulo do %s") : v == 1 ? L("what does each module of %s hold?", "o que cada modulo do %s guarda?") : L("list the modules of %s and what is in each", "lista os modulos do %s e o que tem em cada um"), p->name));
        const char *g = x_glob(cu_fmt("%s[a-z]*.%s", x_srcdir(p), g_x_ext[p->st]));
        for (int i = 0; i < p->nmod; i++) if (!strstr(g, x_src(p, i))) cu_die("o Glob nao achou %s", x_src(p, i));
        const char *nm[3] = {"Read", "Read", "Read"}, *ar[3], *res[3];
        for (int i = 0; i < p->nmod; i++) ar[i] = x6_rd_args(s, x_src(p, i), 0);
        x_calls(L("The files are independent: reading all of them in one turn.", "Os arquivos sao independentes: leio todos numa fala so."), p->nmod, nm, ar, res);
        Buf o = {0}; buf_str(&o, "");
        for (int k = 0; k < p->nmod; k++) {
            Buf fl = {0}; buf_str(&fl, "");
            for (int i = 0; i < p->mod[k].nfn; i++) { if (!strstr(res[k], x_deflit(p, p->mod[k].fn[i].name))) cu_die("a leitura nao tem %s", p->mod[k].fn[i].name); buf_str(&fl, cu_fmt("%s`%s`", fl.n ? ", " : "", p->mod[k].fn[i].name)); }
            buf_str(&o, cu_fmt("%s%s: %s.", k ? " " : "", x_src(p, k), fl.p)); free(fl.p);
        }
        x_final(o.p); free(o.p);
        if (fu) {
            int best = 0, tie = 0; for (int k = 1; k < p->nmod; k++) { if (p->mod[k].nfn > p->mod[best].nfn) { best = k; tie = 0; } else if (p->mod[k].nfn == p->mod[best].nfn) tie = 1; }
            if (!tie) { x_dep(); x_user(XV(L("which module has the most functions?", "qual modulo tem mais funcoes?"), L("and the biggest one?", "e o maior deles?"))); x_final(cu_fmt(L("%s, with %d functions; I counted them in the files I just read.", "%s, com %d funcoes; contei nos arquivos que acabei de ler."), x_src(p, best), p->mod[best].nfn)); }
        }
    } else if (sit == XB_GREP2) {
        x_user(cu_fmt(v == 0 ? L("is `%s` used outside the tests? check the source folder and the tests folder", "`%s` e usada fora dos testes? confere a pasta do fonte e a dos testes") : v == 1 ? L("count where `%s` shows up, in the source and in the tests", "conta onde `%s` aparece, no fonte e nos testes") : L("where do I see `%s`: in the code, in the tests, or both?", "onde eu vejo `%s`: no codigo, nos testes, ou nos dois?"), fi->name));
        const char *sd = x6_src_noslash(p);
        const char *nm[2] = {"Grep", "Grep"}, *ar[2] = {cu_fmt("{\"pattern\": %s, \"path\": %s, \"output_mode\": \"content\"}", cu_q(fi->name), cu_q(sd)), cu_fmt("{\"pattern\": %s, \"path\": %s, \"output_mode\": \"content\"}", cu_q(fi->name), cu_q(td))}, *res[2];
        x_calls(L("Two folders, two independent searches in one turn.", "Duas pastas, duas buscas independentes numa fala so."), 2, nm, ar, res);
        const int n1 = x6_grep_lines(res[0]), n2 = x6_grep_lines(res[1]);
        if (!n1 || !n2) cu_die("uma das buscas nao achou nada");
        x_final(cu_fmt(L("`%s` shows up %d time%s in %s/ (its definition and the places that use it) and %d time%s in %s/, in the test cases.", "A `%s` aparece %d vez%s em %s/ (a definicao e quem usa) e %d vez%s em %s/, nos casos de teste."), fi->name, n1, n1 == 1 ? "" : L("s", "es"), sd, n2, n2 == 1 ? "" : L("s", "es"), td));
        if (fu) {
            x_dep();
            x_user(XV(L("show me those lines in the tests", "me mostra essas linhas dos testes"), L("what do the test lines look like?", "como sao essas linhas dos testes?")));
            const char *r = x_grep(fi->name, "content", cu_fmt("%s/*", td));
            if (!x6_grep_lines(r)) cu_die("sem linhas de teste");
            x_final(cu_fmt(L("They are the test cases: %d lines, the first is `%.*s`.", "Sao os casos de teste: %d linhas, a primeira e `%.*s`."), x6_grep_lines(r), (int)strcspn(r, "\n"), r));
        }
    } else if (sit == XB_FINDGLOB) {
        x_user(cu_fmt(v == 0 ? L("where does the code work out %s, and which test file covers it?", "onde o codigo calcula %s, e qual arquivo de teste cobre isso?") : v == 1 ? L("find %s in the code and tell me which test file to run for it", "acha %s no codigo e me diz que arquivo de teste rodar pra isso") : L("%s: which file, and where are the tests?", "%s: qual arquivo, e onde ficam os testes?"), L(g_x6_c[ci].ask_en, g_x6_c[ci].ask_pt)));
        const char *nm[2] = {"Find", "Glob"}, *ar[2] = {cu_fmt("{\"query\": %s}", cu_q(g_x6_c[ci].q)), "{\"pattern\": \"**/test_*\"}"}, *res[2];
        x_calls(L("The search and the list of test files do not depend on each other.", "A busca e a lista dos arquivos de teste nao dependem uma da outra."), 2, nm, ar, res);
        X6Cand cs[12]; const int n = x6_parse(res[0], cs, 12);
        if (n < 1 || strcmp(cs[0].path, x_src(p, mi)) || !strstr(res[1], p->testrel)) cu_die("o Find ou o Glob nao trouxeram o esperado (%s)", g_x.cell);
        const int ln = cs[0].nl ? cs[0].lines[0] : cs[0].a;
        const char *nm2[2] = {"Read", "Grep"}, *ar2[2] = {cu_fmt("{\"file_path\": %s, \"offset\": %d, \"limit\": 10}", cu_q(x_abs(s, x_src(p, mi))), ln > 2 ? ln - 2 : 1), cu_fmt("{\"pattern\": %s, \"path\": %s, \"output_mode\": \"content\"}", cu_q(fi->name), cu_q(p->testrel))};
        x_calls(L("Now the definition and its test cases, again in one turn.", "Agora a definicao e os casos de teste dela, de novo numa fala so."), 2, nm2, ar2, res);
        const int dl = x6_line_of(res[0], x_deflit(p, fi->name)), nt = x6_grep_lines(res[1]);
        if (!dl || nt < 2) cu_die("a definicao ou os casos nao apareceram");
        x_final(cu_fmt(L("`%s` is at %s:%d (it computes %s). Its cases are in %s, which `%s` runs.", "A `%s` esta em %s:%d (ela calcula %s). Os casos dela estao em %s, que o `%s` roda."), fi->name, x_src(p, mi), dl, L(fi->f->en, fi->f->pt), p->testrel, p->testcmd));
        if (fu) {
            x_user(XV(L("now run the tests", "agora roda os testes"), L("ok, run them", "ok, roda eles")));
            x_find_testcmd(p); const char *o = x_test(p, NULL); const char *t = x_totals(o); if (!t) cu_die("sem totais");
            x_final(cu_fmt(L("`%s` ends with %s.", "O `%s` termina com %s."), p->testcmd, t));
        }
    } else {      /* find2: duas perguntas, dois Find na mesma fala, depois as duas leituras na mesma fala */
        x_user(cu_fmt(v == 0 ? L("where does the code work out %s, and where %s?", "onde o codigo calcula %s, e onde %s?") : v == 1 ? L("two things: which file handles %s, and which one handles %s?", "duas coisas: qual arquivo cuida de %s, e qual cuida de %s?") : L("I need two locations: for %s and for %s", "preciso de dois lugares: o de %s e o de %s"), L(g_x6_c[ci].ask_en, g_x6_c[ci].ask_pt), L(g_x6_c[cj].ask_en, g_x6_c[cj].ask_pt)));
        const char *nm[2] = {"Find", "Find"}, *ar[2] = {cu_fmt("{\"query\": %s}", cu_q(g_x6_c[ci].q)), cu_fmt("{\"query\": %s}", cu_q(g_x6_c[cj].q))}, *res[2];
        x_calls(L("Two separate questions: two searches in one turn.", "Duas perguntas separadas: duas buscas numa fala so."), 2, nm, ar, res);
        X6Cand ca[12], cb[12]; const int na = x6_parse(res[0], ca, 12), nb = x6_parse(res[1], cb, 12);
        if (na < 1 || nb < 1 || strcmp(ca[0].path, x_src(p, mi)) || strcmp(cb[0].path, x_src(p, mj))) cu_die("os dois Find nao trouxeram os arquivos esperados (%s)", g_x.cell);
        const int la = ca[0].nl ? ca[0].lines[0] : ca[0].a, lb = cb[0].nl ? cb[0].lines[0] : cb[0].a;
        const char *nm2[2] = {"Read", "Read"}, *ar2[2] = {cu_fmt("{\"file_path\": %s, \"offset\": %d, \"limit\": 10}", cu_q(x_abs(s, x_src(p, mi))), la > 2 ? la - 2 : 1), cu_fmt("{\"file_path\": %s, \"offset\": %d, \"limit\": 10}", cu_q(x_abs(s, x_src(p, mj))), lb > 2 ? lb - 2 : 1)};
        x_calls(L("Both candidates are clear: reading both snippets in one turn.", "Os dois candidatos estao claros: leio os dois trechos numa fala so."), 2, nm2, ar2, res);
        const int da = x6_line_of(res[0], x_deflit(p, fi->name)), db = x6_line_of(res[1], x_deflit(p, fj->name));
        if (!da || !db) cu_die("as leituras nao tem as definicoes");
        x_final(cu_fmt(L("The first is `%s`, at %s:%d. The second is `%s`, at %s:%d.", "A primeira e a `%s`, em %s:%d. A segunda e a `%s`, em %s:%d."), fi->name, x_src(p, mi), da, fj->name, x_src(p, mj), db));
    }
    x_end(f);
}
