/* ---------- x-script: trabalho repetitivo em UM Bash ----------
 * Em vez de uma chamada por arquivo, por funcao ou por teste, o modelo escreve um laco de shell ou poucas linhas de Python que
 * conferem o que esperam e imprimem um resumo, e roda tudo numa chamada so. O script e escrito aqui para rodar de verdade no
 * projeto do cenario (cada pilha tem o seu jeito de achar as funcoes e de conferir a sintaxe) e o resultado e o do motor.
 * Quatro situacoes: contar funcoes por modulo; dizer, funcao por funcao, se os testes passam; trocar um nome em varios arquivos
 * com a contagem conferida (e desfazer); conferir que todos os arquivos de codigo passam na checagem de sintaxe. */
enum { XSC_COUNT, XSC_PERFN, XSC_REPLACE, XSC_SYNTAX, XSC_N };
static const char *const g_xsc_names[XSC_N] = {"count", "per-function", "replace", "syntax"};
static const char *xsc_defrx(int st) { return st == X_PY ? "^def " : st == X_JS ? "^function " : st == X_SH ? "^[a-z_]+\\(\\) \\{" : st == X_C ? "^int [a-z_]+\\(" : "^sub "; }
static const char *xsc_sed(int st) {
    return st == X_PY ? "s/^def \\([a-z_]*\\)(.*/\\1/p" : st == X_JS ? "s/^function \\([a-z_]*\\)(.*/\\1/p" : st == X_SH ? "s/^\\([a-z_]*\\)() {.*/\\1/p" : st == X_C ? "s/^int \\([a-z_]*\\)(.*/\\1/p" : "s/^sub \\([a-z_]*\\) {.*/\\1/p";
}
static const char *xsc_glob(const XProj *p) { return cu_fmt("%s[a-z]*.%s", x_srcdir(p), g_x_ext[p->st]); }
static const char *xsc_check(int st) {
    return st == X_PY ? "python3 -c 'import ast,sys; ast.parse(open(sys.argv[1]).read())'" : st == X_JS ? "node --check" : st == X_SH ? "sh -n" : st == X_C ? "gcc -fsyntax-only -Isrc" : "perl -c";
}
/* a linha "chave: valor" do resultado, ou NULL */
static const char *xsc_line(const char *r, const char *pre) {
    for (const char *l = r; *l; ) { const size_t k = strcspn(l, "\n"); if (!strncmp(l, pre, strlen(pre))) return cu_fmt("%.*s", (int)k, l); l += k; if (*l) l++; }
    return NULL;
}
/* o script de troca de nome: confere que a contagem e a esperada antes de dar o trabalho por feito */
static const char *xsc_rename_script(const char *old, const char *nw, int want, int nf, const char *const *files) {
    Buf b = {0}; buf_str(&b, "python3 - <<'EOF'\nimport re\n");
    buf_str(&b, cu_fmt("old, new, want = %s, %s, %d\nfiles = [", cu_q(old), cu_q(nw), want));
    for (int i = 0; i < nf; i++) buf_str(&b, cu_fmt("%s%s", i ? ", " : "", cu_q(files[i])));
    buf_str(&b, "]\ntotal = 0\nfor f in files:\n    text = open(f).read()\n    n = len(re.findall(r'\\b' + old + r'\\b', text))\n    open(f, 'w').write(re.sub(r'\\b' + old + r'\\b', new, text))\n"
                "    print(f + ': ' + str(n))\n    total += n\nassert total == want, 'expected %d, replaced %d' % (want, total)\nprint('total: ' + str(total))\nEOF");
    return cu_keep(b.p);
}
static void fam_x_script(FILE *f, Scn *s) {
    int sit, v, stk, fu;
    for (int t = 0; ; t++) {
        if (t > 400) cu_die("sem celula de script");
        sit = cu_n(XSC_N); v = cu_n(3); stk = CU_PA(((const int[]){X_PY, X_PY, X_JS, X_JS, X_SH, X_SH, X_C, X_PL})); fu = cu_pct(45);
        if (x_held(200 + sit, v, stk, fu, 0) == (g_cu_hold != 0)) break;
    }
    static XProj P; XProj *p = &P;
    x_proj(p, s, stk, 3, 2 + cu_n(2));
    p->brief = cu_pct(45); p->readme = !p->brief && cu_pct(40); p->knows = p->brief;
    int nbad = 0, bm = 0; const char *broken = NULL;
    if (sit == XSC_PERFN) {
        nbad = cu_pct(20) ? 0 : cu_pct(70) ? 1 : 2;
        for (int k = 0; k < nbad; k++) { XFi *b; do b = &p->mod[cu_n(p->nmod)].fn[0]; while (b->num != b->f->num); b->num = b->f->bad; }
    }
    if (sit == XSC_SYNTAX && cu_pct(40)) {
        bm = cu_n(p->nmod);
        broken = cu_fmt("%s%s", x_mod_text(p, bm), p->st == X_PY ? "\n\ndef broken(:\n" : p->st == X_JS ? "\nfunction broken( {\n" : p->st == X_SH ? "\nbroken() {\n" : p->st == X_C ? "\nint broken( {\n" : "\nsub broken {\n");
        x_extra(p, x_src(p, bm), broken, 0);
    }
    XFi *rf = NULL; const char *nw = NULL;
    if (sit == XSC_REPLACE) { rf = &p->mod[cu_n(p->nmod)].fn[cu_n(2)]; nw = cu_fmt(XV("%s_v2", "%s_int", "%s_of"), rf->name); }
    x_begin("x-script", s, p, cu_pct(15) ? 2 : 0, cu_pct(12) ? 1 : 0);
    if (!broken && !nbad) x6_selftest(p);
    x_cell("script/%s/v%d/%s/fu%d/conv%d", g_xsc_names[sit], v, g_x_stid[p->st], fu, p->brief ? 2 : p->readme ? 1 : 0);
    const char *say = XV(L("One script for all of them, instead of one call each.", "Um script para todos, em vez de uma chamada para cada um."), L("This is repetitive: one Bash with a loop.", "Isso e repetitivo: um Bash so, com um laco."), L("A single script will do the whole job and print a summary.", "Um script so faz o trabalho todo e imprime um resumo."));
    if (sit == XSC_COUNT) {
        x_user(cu_fmt(v == 0 ? L("how many functions does each module of %s define, and how many in total?", "quantas funcoes cada modulo do %s define, e quantas no total?") : v == 1 ? L("count the functions per module in %s and give me the total", "conta as funcoes por modulo no %s e me da o total") : L("I want a table: module and number of functions, plus the total (%s)", "quero uma tabela: modulo e numero de funcoes, mais o total (%s)"), p->name));
        const char *cmd = cu_fmt("for f in %s; do echo \"$f: $(grep -c -E '%s' \"$f\")\"; done | awk -F': ' '{print; s += $2} END {print \"total: \" s}'", xsc_glob(p), xsc_defrx(p->st));
        const char *r = cu_fmt("%s", x_bash(say, cmd, L("Count the functions of every module", "Conta as funcoes de cada modulo"), 0));
        int tot = 0; Buf o = {0}; buf_str(&o, "");
        for (int m = 0; m < p->nmod; m++) {
            const char *ln = xsc_line(r, cu_fmt("%s: ", x_src(p, m)));
            if (!ln || atoi(ln + strlen(x_src(p, m)) + 2) != p->mod[m].nfn) cu_die("a contagem do script nao bate com o projeto (%s)", g_x.cell);
            tot += p->mod[m].nfn;
        }
        const char *tl = xsc_line(r, "total: ");
        if (!tl || atoi(tl + 7) != tot) cu_die("o total do script nao bate");
        for (const char *l = r; *l; ) { const size_t k = strcspn(l, "\n"); if (strncmp(l, "total: ", 7) && k) buf_str(&o, cu_fmt("%s%.*s", o.n ? ", " : "", (int)k, l)); l += k; if (*l) l++; }
        x_final(cu_fmt(L("Functions per module: %s. Total: %d.", "Funcoes por modulo: %s. Total: %d."), o.p, tot)); free(o.p);
        if (fu) {
            x_user(XV(L("do the tests still pass?", "os testes continuam passando?"), L("now run the tests", "agora roda os testes")));
            x_find_testcmd(p); const char *o2 = x_test(p, NULL); const char *t = x_totals(o2); if (!t) cu_die("sem totais");
            x_final(cu_fmt(L("`%s` ends with %s.", "O `%s` termina com %s."), p->testcmd, t));
        }
    } else if (sit == XSC_PERFN) {
        x_user(cu_fmt(v == 0 ? L("for each function of %s tell me if its tests pass, one line per function", "para cada funcao do %s me diz se os testes dela passam, uma linha por funcao") : v == 1 ? L("which functions of %s are failing? list every function with ok or FAIL", "quais funcoes do %s estao falhando? lista cada funcao com ok ou FAIL") : L("give me a per-function pass/fail summary of %s", "me da um resumo de passa/falha por funcao do %s"), p->name));
        const char *cmd = cu_fmt("out=$(%s 2>&1); for fn in $(sed -n '%s' %s); do if printf '%%s\\n' \"$out\" | grep -q \"FAIL $fn(\"; then echo \"$fn: FAIL\"; else echo \"$fn: ok\"; fi; done", p->testcmd, xsc_sed(p->st), xsc_glob(p));
        const char *r = cu_fmt("%s", x_bash(say, cmd, L("Run the tests once and report each function", "Roda os testes uma vez e relata cada funcao"), 0));
        Buf bad = {0}; buf_str(&bad, ""); int nf = 0, tot = 0;
        for (int m = 0; m < p->nmod; m++) for (int i = 0; i < p->mod[m].nfn; i++) {
            const XFi *fi = &p->mod[m].fn[i]; const int isbad = fi->num != fi->f->num;
            const char *ln = xsc_line(r, cu_fmt("%s: ", fi->name));
            if (!ln || !!strcmp(ln + strlen(fi->name) + 2, isbad ? "FAIL" : "ok")) cu_die("o script nao deu %s para %s (%s)", isbad ? "FAIL" : "ok", fi->name, g_x.cell);
            if (isbad) { buf_str(&bad, cu_fmt("%s`%s`", nf ? ", " : "", fi->name)); nf++; }
            tot++;
        }
        if (nf != nbad) cu_die("quantas falham: %d, esperado %d", nf, nbad);
        if (!nf) x_final(cu_fmt(L("All %d functions pass: every line says ok. I only ran the tests; nothing was changed.", "As %d funcoes passam: todas as linhas dizem ok. So rodei os testes; nada foi alterado."), tot));
        else x_final(cu_fmt(L("%d of %d functions fail: %s. The other %d pass. I only ran the tests; nothing was changed.", "%d de %d funcoes falham: %s. As outras %d passam. So rodei os testes; nada foi alterado."), nf, tot, bad.p, tot - nf));
        free(bad.p);
        if (fu && nf) {
            x_dep();
            x_user(XV(L("show me what the first failing case says", "me mostra o que o primeiro caso que falha diz"), L("what is the first failure, exactly?", "qual e a primeira falha, exatamente?")));
            const char *o2 = x_test(p, NULL); const char *fl = x_line_with(o2, "FAIL ");
            if (!fl) cu_die("sem linha de falha");
            x_final(cu_fmt(L("The first failure is: `%s`.", "A primeira falha e: `%s`."), fl));
        }
    } else if (sit == XSC_REPLACE) {
        const char *old = rf->name;
        x_user(cu_fmt(v == 0 ? L("rename `%s` to `%s` everywhere in %s, and make sure no occurrence is missed", "renomeia `%s` para `%s` em todo o %s, e garante que nenhuma ocorrencia fique de fora") : v == 1 ? L("replace `%s` with `%s` in all the files of %s and check the count", "troca `%s` por `%s` em todos os arquivos do %s e confere a contagem") : L("`%s` must be called `%s` in every file of %s; count what you change", "`%s` tem de se chamar `%s` em todo arquivo do %s; conta o que mudar"), old, nw, p->name));
        const char *cr = cu_fmt("%s", x_call(NULL, "Grep", cu_fmt("{\"pattern\": %s, \"output_mode\": \"count\"}", cu_q(cu_fmt("\\b%s\\b", old))), 0));
        const char *files[8]; int nf = 0, want = 0;
        for (const char *l = cr; *l; ) { const size_t k = strcspn(l, "\n"); const char *co = memchr(l, ':', k); if (co && nf < 8 && isdigit((unsigned char)co[1]) && strncmp(l, "Found", 5)) { files[nf++] = cu_fmt("%.*s", (int)(co - l), l); want += atoi(co + 1); } l += k; if (*l) l++; }
        if (nf < 2) cu_die("o Grep achou o nome em menos de dois arquivos");
        const char *r = cu_fmt("%s", x_bash(L("Counted: the same replacement in every file, with the total checked, in one script.", "Contei: a mesma troca em cada arquivo, com o total conferido, num script so."), xsc_rename_script(old, nw, want, nf, files), L("Rename in every file and check the count", "Troca o nome em cada arquivo e confere a contagem"), 0));
        const char *tl = xsc_line(r, "total: ");
        if (!tl || atoi(tl + 7) != want || strstr(r, "Traceback")) cu_die("o script de troca nao fechou a contagem (%s)", g_x.cell);
        const char *o2 = x_test(p, NULL); const char *t = x_totals(o2);
        if (!t || !strstr(t, " 0 failed")) cu_die("os testes nao passam depois da troca");
        x_final(cu_fmt(L("Renamed `%s` to `%s`: %d occurrences in %d files, the count matched the search. `%s` ends with %s.", "Troquei `%s` por `%s`: %d ocorrencias em %d arquivos, a contagem bateu com a busca. O `%s` termina com %s."), old, nw, want, nf, p->testcmd, t));
        if (fu) {
            x_dep(); x_shape_tag("goal");
            x_user(XV(L("actually, put the old name back", "na verdade, volta o nome antigo"), L("on second thought, undo that rename", "pensando melhor, desfaz essa troca")));
            const char *r2 = cu_fmt("%s", x_bash(L("The same script the other way round, with the same count.", "O mesmo script ao contrario, com a mesma contagem."), xsc_rename_script(nw, old, want, nf, files), L("Undo the rename and check the count", "Desfaz a troca e confere a contagem"), 0));
            const char *tl2 = xsc_line(r2, "total: ");
            if (!tl2 || atoi(tl2 + 7) != want || strstr(r2, "Traceback")) cu_die("o script de desfazer nao fechou a contagem");
            const char *o3 = x_test(p, NULL); const char *t3 = x_totals(o3);
            if (!t3 || !strstr(t3, " 0 failed")) cu_die("os testes nao passam depois de desfazer");
            x_final(cu_fmt(L("`%s` is back: %d occurrences in %d files. `%s` ends with %s.", "O `%s` voltou: %d ocorrencias em %d arquivos. O `%s` termina com %s."), old, want, nf, p->testcmd, t3));
        }
    } else {
        x_user(cu_fmt(v == 0 ? L("check that every source file of %s still parses, one line per file", "confere se todo arquivo de codigo do %s ainda passa na checagem de sintaxe, uma linha por arquivo") : v == 1 ? L("does any source file in %s have a syntax error? check them all", "algum arquivo de codigo do %s tem erro de sintaxe? confere todos") : L("run a syntax check over all the modules of %s and summarise", "roda uma checagem de sintaxe em todos os modulos do %s e resume"), p->name));
        const char *cmd = cu_fmt("for f in %s; do if %s \"$f\" >/dev/null 2>&1; then echo \"ok $f\"; else echo \"BAD $f\"; fi; done", xsc_glob(p), xsc_check(p->st));
        const char *r = cu_fmt("%s", x_bash(say, cmd, L("Syntax-check every module", "Checa a sintaxe de cada modulo"), 0));
        for (int m = 0; m < p->nmod; m++) {
            const int isbad = broken && m == bm;
            if (!xsc_line(r, cu_fmt("%s %s", isbad ? "BAD" : "ok", x_src(p, m)))) cu_die("o resultado nao tem %s para %s (%s) %s", isbad ? "BAD" : "ok", x_src(p, m), g_x.cell, r);
        }
        if (!broken) x_final(cu_fmt(L("All %d source files parse: every line says ok. I did not change anything.", "Os %d arquivos de codigo passam na checagem: todas as linhas dizem ok. Nao mudei nada."), p->nmod));
        else x_final(cu_fmt(L("%s does not parse; the other %d files do. I did not change anything.", "O %s nao passa na checagem; os outros %d arquivos passam. Nao mudei nada."), x_src(p, bm), p->nmod - 1));
        if (fu && !broken) {
            x_user(XV(L("now run the tests", "agora roda os testes"), L("and do the tests pass?", "e os testes passam?")));
            x_find_testcmd(p); const char *o2 = x_test(p, NULL); const char *t = x_totals(o2); if (!t) cu_die("sem totais");
            x_final(cu_fmt(L("`%s` ends with %s.", "O `%s` termina com %s."), p->testcmd, t));
        }
    }
    x_end(f);
}
