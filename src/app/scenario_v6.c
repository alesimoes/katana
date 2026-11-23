/* ---------- curriculo v6: Find, varias chamadas na mesma fala, um script em uma chamada ----------
 * Tres familias do motor de cenarios (mesma maquina de scenario.c: projeto de verdade, ferramentas do motor, check refaz tudo):
 *   x-find6   navegar por significado com o Find (src/tools/find.c): o pedido descreve o conceito com outras palavras, o modelo
 *             faz uma consulta curta, le so o trecho em volta do candidato e responde com arquivo:linha; seguir uma referencia
 *             com mode=refs; quando o Find diz "No match" muda para o Grep com termos do codigo; nome exato conhecido vai direto
 *             ao Grep; verificar o candidato antes de afirmar e trocar de candidato quando era o errado.
 *   x-batch   leituras independentes como VARIAS chamadas na mesma fala (x_calls), e o passo seguinte usa os resultados juntos.
 *   x-script  trabalho repetitivo em UM Bash (laco de shell ou poucas linhas de Python que conferem o que esperam), rodado de
 *             verdade no projeto do cenario.
 * Os projetos levam um comentario de uma linha acima de cada funcao (p->cmt), em ingles, e as vezes uma funcao report_<f> que
 * chama a funcao de outro modulo (p->wrap): e dai que o Find tira "calls:" e o mode=refs mostra quem usa. */

/* ---- acervo: a funcao, como o usuario a descreve (outras palavras que as do codigo), a consulta curta que o modelo faz ao Find
   (palavras do comentario e do nome) e o termo de codigo que o modelo tenta no Grep ---- */
static const struct { const char *fn, *ask_en, *ask_pt, *q, *g; } g_x6_c[] = {
    {"discount", "the price a customer pays once a percentage is taken off", "o preco que o cliente paga depois de tirar uma porcentagem", "amount after percent discount", "discount"},
    {"with_tip", "the total of a restaurant check with the extra for the waiter", "o total da conta do restaurante com o extra do garcom", "bill plus percent tip", "tip"},
    {"add_tax", "how much a purchase costs once the government share is added", "quanto uma compra custa depois que a parte do governo e somada", "amount plus rate percent of tax", "tax"},
    {"interest", "how much extra a loan costs as the years go by", "quanto a mais um emprestimo custa com o passar dos anos", "simple interest rate percent years", "interest"},
    {"area", "how much floor a rectangular room covers", "quanto chao um comodo retangular cobre", "area rectangle width height", "area"},
    {"perimeter", "how long the fence around a rectangular lot is", "o tamanho da cerca em volta de um terreno retangular", "perimeter rectangle width height", "perimeter"},
    {"volume", "how much space a box takes up", "quanto espaco uma caixa ocupa", "volume of a box", "volume"},
    {"to_seconds", "turning a stopwatch reading into one number of seconds", "transformar a leitura de um cronometro em um numero de segundos", "minutes and seconds as seconds", "seconds"},
    {"stock_left", "what is still on the shelf after sales and goods sent back", "o que ainda sobra na prateleira depois das vendas e das mercadorias devolvidas", "stock after sales and returns", "stock"},
    {"padding", "how many filler bytes are needed to round a size up", "quantos bytes de enchimento faltam para arredondar um tamanho", "bytes needed to pad length multiple of eight", "pad"},
    {"shift_end", "the time of day a work shift finishes", "a hora do dia em que um turno de trabalho termina", "hour a shift ends 24-hour clock", "shift"},
    {"score", "the points a player earns for good and bad shots", "os pontos que um jogador ganha por tiros bons e ruins", "points per hit minus one per miss", "score"},
    {"points", "the standings points of a team in a league", "os pontos de um time na tabela do campeonato", "points per win and one per draw", "points"},
    {"fahrenheit", "converting a temperature to the scale used in the US", "converter uma temperatura para a escala usada nos EUA", "temperature in Fahrenheit", "fahrenheit"},
    {"frames", "how many pictures a video shows in a few seconds", "quantas imagens um video mostra em alguns segundos", "frames shown in seconds at fps", "frames"},
    {"dozens", "how many full boxes of twelve eggs can be filled", "quantas caixas cheias de doze ovos da para montar", "how many full dozens fit in items", "dozen"},
    {"checksum", "the little number that catches a wrongly typed pair of values", "o numero pequeno que pega um par de valores digitado errado", "small checksum of two values", "checksum"},
    {"next_slot", "which seat comes after the current one in a circular queue", "qual lugar vem depois do atual numa fila circular", "slot after index wrapping at count", "slot"},
    {"pack_weight", "how heavy a shipment is with its packaging", "quanto pesa uma remessa com a embalagem", "weight of items plus the box", "weight"},
    {"to_meters", "turning a distance in kilometres and metres into metres only", "transformar uma distancia em quilometros e metros so em metros", "kilometers and meters as meters", "meters"},
    {"midpoint", "the value halfway between two limits", "o valor que fica no meio de dois limites", "whole-number midpoint of low and high", "midpoint"},
    {"rows_needed", "how many lines of eight are needed to lay out all the items", "quantas linhas de oito sao precisas para encaixar todos os itens", "rows of eight needed for items", "rows"},
    {"gap", "the distance between two positions counting both ends", "a distancia entre duas posicoes contando as duas pontas", "distance from second to first counting both ends", "gap"},
    {"net", "what remains of a payment after the tax is taken", "o que sobra de um pagamento depois de tirar o imposto", "gross value minus the tax", "net"},
    {"half", "one of two equal parts of an amount", "uma de duas partes iguais de uma quantia", "half of the value rounded down", "half"},
    {"quarter", "one of four equal parts of an amount", "uma de quatro partes iguais de uma quantia", "a quarter of the value rounded down", "quarter"},
    {"tens", "how many complete groups of ten there are in a count", "quantos grupos completos de dez existem numa contagem", "how many full tens fit in the number", "tens"},
    {"leftover", "the crumbs left after filling equal groups", "as sobras depois de encher grupos iguais", "what is left after filling groups of size", "leftover"},
};
#define X6_NC CU_N(g_x6_c)
/* pares que se confundem: o alvo e o parecido (palavras em comum no comentario), com a consulta feita so com as palavras comuns */
static const struct { const char *a, *b, *q; } g_x6_p[] = {
    {"discount", "with_tip", "pct percent"}, {"with_tip", "add_tax", "percent amount"}, {"add_tax", "interest", "rate percent"}, {"area", "perimeter", "rectangle width height"},
    {"perimeter", "area", "rectangle width height"}, {"score", "points", "points per"}, {"dozens", "tens", "how many full fit"}, {"half", "quarter", "of the value rounded down"},
    {"interest", "add_tax", "rate percent"}, {"volume", "area", "width height"},
};
static int x6_ci(const char *fn) { for (int i = 0; i < (int)X6_NC; i++) if (!strcmp(g_x6_c[i].fn, fn)) return i; cu_die("sem descricao para %s", fn); return 0; }
/* poe a funcao do acervo no projeto (ou acha onde ela ja esta); not_mi: modulo onde ela nao pode cair */
static XFi *x6_place(XProj *p, const char *name, int not_mi, int *mo) {
    int m; XFi *e = x_fi(p, name, &m);
    if (e) { if (m == not_mi) cu_die("%s caiu no modulo proibido", name); if (mo) *mo = m; return e; }
    int idx = -1; for (int i = 0; i < (int)X_NFN; i++) if (!strcmp(g_x_fns[i].name, name)) idx = i;
    if (idx < 0) cu_die("funcao fora do acervo: %s", name);
    int mi = cu_n(p->nmod); if (mi == not_mi) mi = (mi + 1) % p->nmod;
    XMod *md = &p->mod[mi]; XFi *x = &md->fn[cu_n(md->nfn)];
    x->f = &g_x_fns[idx]; x->name = x->f->name; x->num = x->f->num; x_cases(x);
    if (mo) *mo = mi;
    return x;
}
/* so para eu conferir o projeto: KATANA_CU_RUNTEST=1 roda os testes dele e descarta o cenario se falharem */
static void x6_selftest(const XProj *p) {
    if (!getenv("KATANA_CU_RUNTEST")) return;
    const char *o = x_sh(p->s, p->testcmd);
    if (!strstr(o, " 0 failed")) cu_die("o projeto do cenario nao passa nos proprios testes: %.200s", o);
}
static void x6_base(XProj *p, const Scn *s, int st, int nfn) {
    x_proj(p, s, st, 3, nfn);
    p->cmt = 1;
    p->brief = cu_pct(45); p->readme = !p->brief && cu_pct(40); p->knows = p->brief;
}
static const char *x6_src_noslash(const XProj *p) { const char *d = x_srcdir(p); return cu_fmt("%.*s", (int)strlen(d) - 1, d); }
/* ---- le o que o Find devolveu: candidatos (arquivo, faixa de linhas, linhas que casam, calls:) ---- */
typedef struct { char path[200]; int a, b, nl, lines[8]; const char *calls; } X6Cand;
static int x6_parse(const char *out, X6Cand *c, int max) {
    int n = 0;
    for (const char *l = out; *l; ) {
        const size_t k = strcspn(l, "\n");
        if (isdigit((unsigned char)*l)) {
            const char *q = l; while (isdigit((unsigned char)*q)) q++;
            if (q[0] == '.' && q[1] == ' ' && n < max) {
                q += 2; const char *co = memchr(q, ':', k - (size_t)(q - l));
                if (co) { memset(&c[n], 0, sizeof c[n]); snprintf(c[n].path, sizeof c[n].path, "%.*s", (int)(co - q), q); c[n].a = atoi(co + 1); const char *d = strchr(co + 1, '-'); c[n].b = d ? atoi(d + 1) : c[n].a; n++; }
            }
        } else if (n && !strncmp(l, "   ", 3) && isdigit((unsigned char)l[3])) { if (c[n - 1].nl < 8) c[n - 1].lines[c[n - 1].nl++] = atoi(l + 3); }
        else if (n && !strncmp(l, "   calls: ", 10)) c[n - 1].calls = cu_fmt("%.*s", (int)k - 10, l + 10);
        l += k; if (*l) l++;
    }
    return n;
}
/* ate 4 palavras do pedido que nao aparecem em nenhum arquivo do projeto (o usuario nao usa as palavras do codigo) */
static const char *x6_foreign(const XProj *p, const char *ask) {
    Buf all = {0}; buf_str(&all, "");
    for (int m = 0; m < p->nmod; m++) { buf_str(&all, x_slurp(p->s, x_src(p, m))); if (p->st == X_C) buf_str(&all, x_slurp(p->s, x_hdr(p, m))); }
    buf_str(&all, x_slurp(p->s, p->testrel));
    if (p->readme) buf_str(&all, x_slurp(p->s, "README.md"));
    if (p->brief) buf_str(&all, x_slurp(p->s, "KATANA.md"));
    for (char *c = all.p; *c; c++) *c = (char)tolower((unsigned char)*c);
    Buf o = {0}; buf_str(&o, ""); int n = 0;
    for (const char *w = ask; *w && n < 4; ) {
        const size_t k = strcspn(w, " ,");
        if (k >= 4) {
            char t[40]; snprintf(t, sizeof t, "%.*s", (int)(k < 5 ? k : 5), w); for (char *c = t; *c; c++) *c = (char)tolower((unsigned char)*c);
            if (!strstr(all.p, t)) buf_str(&o, cu_fmt("%s%.*s", n++ ? " " : "", (int)k, w));
        }
        w += k; while (*w == ' ' || *w == ',') w++;
    }
    free(all.p);
    if (n < 2) cu_die("o pedido nao tem palavras que faltam no projeto");
    return cu_keep(o.p);
}
static int x6_cand(const X6Cand *c, int n, const char *path) { for (int i = 0; i < n; i++) if (!strcmp(c[i].path, path)) return i; return -1; }
/* numero (da coluna do Read) da linha que tem lit; 0 se nenhuma */
static int x6_line_of(const char *rd, const char *lit) {
    for (const char *l = rd; *l; ) {
        const size_t k = strcspn(l, "\n"); const char *tb = memchr(l, '\t', k);
        if (tb && tb - l == 6 && memmem(tb + 1, k - (size_t)(tb + 1 - l), lit, strlen(lit))) return atoi(l);
        l += k; if (*l) l++;
    }
    return 0;
}
static const char *x6_find(const char *say, const char *q, const char *path, int refs) {
    return x_call(say, "Find", cu_fmt("{\"query\": %s%s%s%s}", cu_q(q), path ? cu_fmt(", \"path\": %s", cu_q(path)) : "", refs ? ", \"mode\": \"refs\"" : "", ""), 0);
}
/* Read em volta do candidato: da linha antes da primeira que casou, dez linhas */
static const char *g_x6_rd = "";      /* o ultimo trecho lido em volta de um candidato */
static const char *x6_read_at(const XProj *p, const char *rel, const X6Cand *c, int *off, int last) {
    int ln = c->nl ? c->lines[last ? c->nl - 1 : 0] : c->a; *off = ln > 2 ? ln - 2 : 1;
    g_x6_rd = x_call(NULL, "Read", cu_fmt("{\"file_path\": %s, \"offset\": %d, \"limit\": 10}", cu_q(x_abs(p->s, rel)), *off), 0);
    return g_x6_rd;
}
static const char *x6_ask(int ci, int v) {
    const char *a = L(g_x6_c[ci].ask_en, g_x6_c[ci].ask_pt);
    switch (v % 3) {
    case 0: return cu_fmt(L("where does the code work out %s?", "onde o codigo calcula %s?"), a);
    case 1: return cu_fmt(L("which file takes care of this: %s?", "qual arquivo cuida disso: %s?"), a);
    default: return cu_fmt(L("I want to change this: %s. Where does it live?", "quero mudar isto: %s. Onde isso fica?"), a);
    }
}
static const char *x6_say(void) { return cu_pct(30) ? CU_PA(((const char *const[]){L("Describing it to Find first.", "Descrevo para o Find primeiro."), L("Let me ask the index.", "Vou perguntar ao indice."), L("A short query should do.", "Uma consulta curta deve bastar.")})) : NULL; }
/* a funcao que responde ao conceito: Find, confere o primeiro candidato, le o trecho, devolve a linha da definicao (*dl) */
static const char *x6_locate_by_concept(XProj *p, const XFi *fi, int mi, int ci, int *dl) {
    const char *src = x_src(p, mi);
    const char *r = x6_find(x6_say(), g_x6_c[ci].q, cu_pct(25) ? x6_src_noslash(p) : NULL, 0);
    X6Cand cs[12]; const int n = x6_parse(r, cs, 12);
    if (n < 1 || strcmp(cs[0].path, src)) cu_die("o Find nao pos %s em primeiro para \"%s\" (%s)", src, g_x6_c[ci].q, g_x.cell);
    int off; const char *rd = x6_read_at(p, src, &cs[0], &off, 0);
    *dl = x6_line_of(rd, x_deflit(p, fi->name));
    if (!*dl) cu_die("o trecho lido nao tem a definicao de %s", fi->name);
    return src;
}
enum { X6_CONCEPT, X6_REFS, X6_NOMATCH, X6_EXACT, X6_CONFUSE, X6_N };
static const char *const g_x6_names[X6_N] = {"concept", "refs", "no-match", "exact", "confuse"};
static void fam_x_find6(FILE *f, Scn *s) {
    int sit, v, stk, fu;
    for (int t = 0; ; t++) {
        if (t > 400) cu_die("sem celula de find6");
        sit = cu_n(X6_N); v = cu_n(3); stk = CU_PA(((const int[]){X_PY, X_JS, X_SH, X_C})); fu = cu_pct(45) ? 1 + cu_n(2) : 0;
        if (sit == X6_NOMATCH || sit == X6_EXACT || sit == X6_CONFUSE) fu = cu_pct(30) ? 1 : 0;
        if (x_held(180 + sit, v, stk, fu, 0) == (g_cu_hold != 0)) break;
    }
    static XProj P; XProj *p = &P;
    x6_base(p, s, stk, 2 + cu_n(2));
    int ci = cu_ix(X6_NC), mi = 0, di = 0; XFi *fi = NULL, *dc = NULL;
    const char *dname = NULL, *pq = NULL;
    g_x6_rd = "";
    if (sit == X6_CONFUSE) {      /* o alvo e o parecido, em modulos diferentes */
        const int pi = cu_ix(CU_N(g_x6_p)); ci = x6_ci(g_x6_p[pi].a); dname = g_x6_p[pi].b; pq = g_x6_p[pi].q;
        fi = x6_place(p, g_x6_p[pi].a, -1, &mi);
        dc = x6_place(p, dname, mi, &di);
        if (x_fi(p, g_x6_p[pi].a, &mi) != fi) cu_die("o alvo foi trocado");
    } else fi = x6_place(p, g_x6_c[ci].fn, -1, &mi);
    if (sit == X6_REFS) { p->wrap = 1; p->wf = fi; p->wfm = mi; p->wm = (mi + 1 + cu_n(p->nmod - 1)) % p->nmod; }
    XFi *ex = NULL; int exm = 0;      /* exact: uma funcao qualquer do projeto, pelo nome */
    if (sit == X6_EXACT) { do { exm = cu_n(p->nmod); ex = &p->mod[exm].fn[cu_n(p->mod[exm].nfn)]; } while (ex == fi); }
    x_begin("x-find6", s, p, cu_pct(15) ? 2 : 0, cu_pct(12) ? 1 : 0);
    x6_selftest(p);
    x_cell("find6/%s/v%d/%s/fu%d/conv%d", g_x6_names[sit], v, g_x_stid[p->st], fu, p->brief ? 2 : p->readme ? 1 : 0);
    const char *src = x_src(p, mi), *fn = fi->name, *askn = L(g_x6_c[ci].ask_en, g_x6_c[ci].ask_pt);
    int dl = 0;
    const char *what = L(fi->f->en, fi->f->pt);
    if (sit == X6_CONCEPT) {
        x_user(x6_ask(ci, v));
        x6_locate_by_concept(p, fi, mi, ci, &dl);
        x_final(cu_fmt(L("It is `%s`, at %s:%d: it computes %s.", "E a `%s`, em %s:%d: ela calcula %s."), fn, src, dl, what));
    } else if (sit == X6_REFS) {
        const char *wsrc = x_src(p, p->wm), *wn = cu_fmt("report_%s", fn);
        x_user(cu_fmt(v == 0 ? L("which function feeds the report with %s, and where does the real calculation live?", "qual funcao alimenta o relatorio com %s, e onde fica o calculo de verdade?")
                      : v == 1 ? L("the report shows %s. Which function is it, and what does it rely on?", "o relatorio mostra %s. Qual funcao e essa, e do que ela depende?")
                               : L("trace %s in the report back to where it is really computed", "rastreia %s no relatorio ate onde e calculado de verdade"), askn));
        const char *r = x6_find(x6_say(), cu_fmt("report %s", g_x6_c[ci].q), NULL, 0);
        X6Cand cs[12]; const int n = x6_parse(r, cs, 12);
        const int wi = x6_cand(cs, n, wsrc), di0 = x6_cand(cs, n, src);
        if (wi < 0 || wi > 2 || di0 < 0 || di0 > 2) cu_die("o Find nao trouxe o relatorio e a definicao entre os tres primeiros (%s)", g_x.cell);
        const int viacalls = cs[wi].calls && strstr(cs[wi].calls, fn);
        const char *rf = x6_find(viacalls ? L("The `calls:` line names it; following the name.", "A linha `calls:` cita o nome; vou seguir por ele.") : L("The definition lists the report as a user; following the name.", "A definicao lista o relatorio como usuario; vou seguir pelo nome."), fn, NULL, 1);
        if (!strstr(rf, src) || !strstr(rf, wn)) cu_die("o refs nao mostrou a definicao e o uso (%s)", g_x.cell);
        const int intests = strstr(rf, p->testrel) != NULL;
        int off; const char *rd = x6_read_at(p, wsrc, &cs[wi], &off, 1);
        const int ul = x6_line_of(rd, wn); if (!ul) cu_die("o trecho do relatorio nao tem %s", wn);
        int dline = 0;
        if (cu_pct(60)) { const char *rd2 = x_call(NULL, "Read", cu_fmt("{\"file_path\": %s, \"offset\": 1, \"limit\": 14}", cu_q(x_abs(s, src))), 0); dline = x6_line_of(rd2, x_deflit(p, fn)); if (!dline) cu_die("a definicao nao esta nas primeiras linhas"); }
        else { const char *sl = x_slurp(s, src); int ln = 1; for (const char *q = sl; *q && !dline; ) { const size_t k = strcspn(q, "\n"); if (memmem(q, k, x_deflit(p, fn), strlen(x_deflit(p, fn)))) dline = ln; q += k; if (*q) q++; ln++; } if (!dline || !strstr(rf, cu_fmt("%d", dline))) cu_die("o refs nao traz a linha da definicao"); }
        x_final(cu_fmt(L("`%s` (%s:%d) feeds the report and calls `%s`, which is really computed in %s:%d%s.", "A `%s` (%s:%d) alimenta o relatorio e chama a `%s`, que e calculada de verdade em %s:%d%s."), wn, wsrc, ul, fn, src, dline,
                       intests ? cu_fmt(L("; the test runner %s calls it too", "; o executor de testes %s tambem a chama"), p->testrel) : ""));
    } else if (sit == X6_NOMATCH) {
        x_user(x6_ask(ci, v));
        Buf q = {0}; buf_str(&q, x6_foreign(p, askn));
        const char *r = cu_fmt("%s", x6_find(NULL, q.p, NULL, 0));
        if (strncmp(r, "No match for", 12)) cu_die("o Find achou algo para as palavras do usuario (%s)", g_x.cell);
        const char *gr = x_call(L("The index has nothing for those words; searching for the code term.", "O indice nao tem nada com essas palavras; procuro pelo termo do codigo."), "Grep", cu_fmt("{\"pattern\": %s, \"-i\": true, \"output_mode\": \"content\"}", cu_q(g_x6_c[ci].g)), 0);
        const char *pre = cu_fmt("%s:", src), *lit = x_deflit(p, fn);
        for (const char *l = gr; *l && !dl; ) { const size_t k = strcspn(l, "\n"); if (!strncmp(l, pre, strlen(pre)) && memmem(l, k, lit, strlen(lit))) dl = atoi(l + strlen(pre)); l += k; if (*l) l++; }
        if (!dl) cu_die("o Grep nao mostrou a definicao de %s", fn);
        if (cu_pct(50)) { const char *rd = x_call(NULL, "Read", cu_fmt("{\"file_path\": %s, \"offset\": %d, \"limit\": 6}", cu_q(x_abs(s, src)), dl > 2 ? dl - 2 : 1), 0); if (!x6_line_of(rd, lit)) cu_die("a leitura nao confirma a definicao"); }
        x_final(cu_fmt(L("It is `%s`, at %s:%d: it computes %s. The words you used are not in the code, so the Find had nothing; the search for `%s` found it.", "E a `%s`, em %s:%d: ela calcula %s. As suas palavras nao estao no codigo, entao o Find nao achou nada; a busca por `%s` achou."), fn, src, dl, what, g_x6_c[ci].g));
        free(q.p);
    } else if (sit == X6_EXACT) {
        const char *esrc = x_src(p, exm);
        x_user(cu_fmt(v == 0 ? L("where is `%s` defined, and what does it return?", "onde `%s` e definida, e o que ela devolve?") : v == 1 ? L("which file has `%s` and what is its return expression?", "em que arquivo esta `%s` e qual a expressao que ela devolve?") : L("show me where `%s` lives and what it gives back", "me mostra onde `%s` fica e o que ela devolve"), ex->name));
        const char *r = x_call(L("An exact pattern: Grep.", "Um padrao exato: Grep."), "Grep", cu_fmt("{\"pattern\": %s, \"output_mode\": \"content\", \"-A\": 2}", cu_q(x_defpat(p, ex->name))), 0);
        const char *pre = cu_fmt("%s:", esrc), *lit = x_deflit(p, ex->name); int el = 0;
        for (const char *l = r; *l && !el; ) { const size_t k = strcspn(l, "\n"); if (!strncmp(l, pre, strlen(pre)) && memmem(l, k, lit, strlen(lit))) el = atoi(l + strlen(pre)); l += k; if (*l) l++; }
        const char *ex_e = x_expr(p->st, ex->f, ex->num);
        if (!el || !strstr(r, ex_e)) cu_die("o Grep nao mostrou a definicao e a expressao de %s", ex->name);
        x_final(cu_fmt(L("`%s` is defined in %s, line %d; it returns `%s`.", "A `%s` e definida em %s, linha %d; ela devolve `%s`."), ex->name, esrc, el, ex_e));
        if (fu) { x_user(x6_ask(ci, v + 1)); x6_locate_by_concept(p, fi, mi, ci, &dl); x_final(cu_fmt(L("That one is `%s`, at %s:%d: it computes %s.", "Essa e a `%s`, em %s:%d: ela calcula %s."), fn, src, dl, what)); fu = 0; }
    } else {      /* confuse: o primeiro candidato pode ser o parecido; le antes de afirmar */
        const char *dsrc = x_src(p, di);
        x_user(x6_ask(ci, v));
        const char *r = x6_find(x6_say(), pq, NULL, 0);
        X6Cand cs[12]; const int n = x6_parse(r, cs, 12);
        const int ti = x6_cand(cs, n, src), dix = x6_cand(cs, n, dsrc);
        if (ti < 0 || dix < 0 || (ti && dix)) cu_die("o Find nao trouxe o alvo e o parecido (%s)", g_x.cell);
        int off; const char *lit = x_deflit(p, fn);
        const char *rd = x6_read_at(p, ti ? dsrc : src, &cs[0], &off, 0);
        if (!ti) {      /* o primeiro era o alvo: a leitura confirma */
            dl = x6_line_of(rd, lit); if (!dl) cu_die("o primeiro candidato nao tem %s", fn);
            x_final(cu_fmt(L("Checked: the top candidate holds `%s`, at %s:%d: it computes %s.", "Conferi: o primeiro candidato tem a `%s`, em %s:%d: ela calcula %s."), fn, src, dl, what));
        } else {      /* o primeiro era o parecido: a leitura mostra outra funcao, e o certo e o segundo */
            const int dd = x6_line_of(rd, x_deflit(p, dname));
            if (!dd || x6_line_of(rd, lit)) cu_die("o primeiro candidato nao e so o parecido (%s)", g_x.cell);
            rd = x6_read_at(p, src, &cs[ti], &off, 0);
            dl = x6_line_of(rd, lit); if (!dl) cu_die("o segundo candidato nao tem %s", fn);
            x_final(cu_fmt(L("Not the first candidate: %s:%d is `%s`, which computes %s. The one you describe is `%s`, at %s:%d: it computes %s.", "Nao e o primeiro candidato: %s:%d e a `%s`, que calcula %s. A que voce descreve e a `%s`, em %s:%d: ela calcula %s."),
                           dsrc, dd, dname, L(dc->f->en, dc->f->pt), fn, src, dl, what));
        }
    }
    if (fu == 1 && sit != X6_EXACT) {      /* quem usa: o Find em mode=refs, que mostra cada uso com a funcao em volta */
        x_dep();
        x_user(XV(L("and who uses it?", "e quem usa ela?"), L("where else is it called from?", "de onde mais ela e chamada?"), L("which functions depend on it?", "quais funcoes dependem dela?")));
        const char *rf = cu_fmt("%s", x6_find(NULL, fn, NULL, 1));
        int nu = 0, nf = 0; const char *u = strstr(rf, "Used ");
        if (!u || sscanf(u, "Used %d times in %d files", &nu, &nf) != 2 || !strstr(rf, src)) cu_die("o refs nao deu os usos (%s)", g_x.cell);
        Buf fl = {0}; buf_str(&fl, ""); int k = 0;
        for (const char *l = u; *l; ) { const size_t m = strcspn(l, "\n"); const char *pa = memchr(l, ' ', m); if (m > 4 && l[m - 1] == ':' && l[0] != ' ' && pa && l[m - 2] == ')') buf_str(&fl, cu_fmt("%s%.*s", k++ ? ", " : "", (int)(pa - l), l)); l += m; if (*l) l++; }
        if (!k) cu_die("o refs nao listou arquivos");
        x_final(cu_fmt(L("`%s` is used %d time%s, in %s.", "A `%s` e usada %d vez%s, em %s."), fn, nu, nu == 1 ? "" : L("s", "es"), fl.p));
        free(fl.p);
    } else if (fu == 2 && (sit == X6_CONCEPT || sit == X6_CONFUSE)) {      /* o que ela devolve: a leitura de antes ja mostra a conta, sem chamada */
        const char *ee = x_expr(p->st, fi->f, fi->num);
        if (strstr(g_x6_rd, ee)) {
            x_dep();
            x_user(XV(L("what exactly does it return?", "o que exatamente ela devolve?"), L("show me its return expression", "me mostra a expressao que ela devolve"), L("what is the formula inside it?", "qual e a formula dentro dela?")));
            x_final(cu_fmt(L("It returns `%s`, as the part I read shows.", "Ela devolve `%s`, como mostra o trecho que li."), ee));
        }
    }
    x_end(f);
}
