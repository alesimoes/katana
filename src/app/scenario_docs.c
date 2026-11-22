/* ---------- x-docs: o KATANA.md aponta para um indice de documentos, um documento por modulo ("tela") ----------
 * O projeto de verdade tem docs/index.md (o mapa) e docs/modules/<modulo>.md (o que o modulo e, os arquivos, as funcoes, uma regra).
 * O motor (ds_select, o mesmo codigo do laco) escolhe o trecho certo e o entrega junto do pedido. Tres situacoes:
 *   hit    o pedido usa as palavras do indice; o motor entrega o documento do modulo certo; o modelo responde dele, sem chamada;
 *   miss   o pedido diz a mesma coisa com outras palavras; o motor nao acha nada; o modelo le o indice, depois o documento do
 *          modulo que o indice descreve, e responde (o motor nao e inteligente: pode falhar, e o modelo faz o caminho sozinho);
 *   wrong  o motor entrega o documento de OUTRO modulo; o modelo percebe que o trecho nao e do que foi pedido e vai pelo indice.
 * O tokenizador vem do vocabulario de um modelo (KATANA_VOCAB_MODEL ou o primeiro .gguf da pasta de modelos); sem ele a familia nao gera. */

static const struct { const char *name, *what_en, *what_pt, *alt_en, *alt_pt, *fact_en, *fact_pt; } g_x_dp[] = {
    {"core", "the basic arithmetic that every other module reuses", "a aritmetica basica que os outros modulos reaproveitam", "the shared helpers at the bottom of everything", "os auxiliares comuns que ficam embaixo de tudo", "Every function takes and returns whole numbers.", "Toda funcao recebe e devolve numeros inteiros."},
    {"calc", "calculations on amounts", "calculos sobre valores", "sums and operations on quantities", "somas e operacoes com quantidades", "Results are rounded down.", "Os resultados sao arredondados para baixo."},
    {"rules", "business rules that decide what is allowed", "regras de negocio que decidem o que e permitido", "the checks that say yes or no to a request", "as verificacoes que dizem sim ou nao a um pedido", "A rule returns 1 when it holds and 0 when it does not.", "Uma regra devolve 1 quando vale e 0 quando nao vale."},
    {"tally", "counting and totalling items", "contagem e totais de itens", "adding up how many there are", "somar quantos existem", "Totals never include cancelled items.", "Os totais nunca incluem itens cancelados."},
    {"ledger", "the record of money coming in and going out", "o registro do dinheiro que entra e sai", "the books where every payment is written down", "o livro onde cada pagamento e anotado", "Money is stored in cents.", "O dinheiro e guardado em centavos."},
    {"metrics", "indicators and averages for the reports", "indicadores e medias dos relatorios", "the numbers management keeps an eye on", "os numeros que a gerencia acompanha", "Averages are rounded down to a whole number.", "As medias sao arredondadas para baixo."},
    {"convert", "conversion between units", "conversao entre unidades", "turning one measure into another", "transformar uma medida em outra", "Conversions go through the base unit first.", "A conversao passa antes pela unidade base."},
    {"pricing", "prices, discounts and taxes", "precos, descontos e impostos", "working out what a customer pays", "descobrir quanto o cliente paga", "A discount never takes a price below zero.", "Um desconto nunca deixa o preco abaixo de zero."},
    {"layout", "positions and sizes on the page", "posicoes e tamanhos na pagina", "where each thing sits on the screen", "onde cada coisa fica na tela", "Sizes are in whole pixels.", "Os tamanhos sao em pixels inteiros."},
    {"timing", "durations and schedules", "duracoes e agendamentos", "how long things take and when they run", "quanto as coisas demoram e quando rodam", "Durations are in seconds.", "As duracoes sao em segundos."},
    {"stock", "inventory levels and reorders", "niveis de estoque e reposicao", "how much is left on the shelf", "quanto sobra na prateleira", "Stock never goes below zero.", "O estoque nunca fica abaixo de zero."},
    {"limits", "quotas and ceilings", "cotas e tetos", "the most anyone is allowed to use", "o maximo que alguem pode usar", "A limit of zero means blocked.", "Um limite zero significa bloqueado."},
    {"shapes", "areas and perimeters of figures", "areas e perimetros de figuras", "how big a drawn figure is", "o tamanho de uma figura desenhada", "Lengths are in whole units.", "Os comprimentos sao em unidades inteiras."},
    {"report", "summaries shown to the user", "resumos mostrados ao usuario", "the figures that end up in the final printout", "os valores que saem na impressao final", "A report line shows at most three values.", "Uma linha de relatorio mostra no maximo tres valores."},
    {"rates", "percentages and interest", "percentuais e juros", "how much extra gets added over time", "quanto a mais e acrescentado com o tempo", "A rate of 15 means fifteen percent.", "Uma taxa de 15 significa quinze por cento."},
    {"counts", "occurrence counters", "contadores de ocorrencias", "how many times something happened", "quantas vezes algo aconteceu", "Counters only grow.", "Os contadores so crescem."},
};
static int x_dp_ix(const char *name) { for (int i = 0; i < (int)CU_N(g_x_dp); i++) if (!strcmp(g_x_dp[i].name, name)) return i; cu_die("modulo sem documento: %s", name); return 0; }
static const char *x_dp_what(int i) { return L(g_x_dp[i].what_en, g_x_dp[i].what_pt); }
static const char *x_dp_doc(const char *name) { return cu_fmt("docs/modules/%s.md", name); }
static void x_docs_write(const XProj *p) {
    const Scn *s = p->s;
    Buf ix = {0};
    buf_str(&ix, L("# Documentation index\n\nOne document per module. Find the module by what it does.\n\n", "# Indice da documentacao\n\nUm documento por modulo. Ache o modulo pelo que ele faz.\n\n"));
    for (int m = 0; m < p->nmod; m++) {
        const int d = x_dp_ix(p->mod[m].name);
        buf_str(&ix, cu_fmt("- %s: [%s](modules/%s.md)\n", x_dp_what(d), p->mod[m].name, p->mod[m].name));
        Buf b = {0};
        buf_str(&b, cu_fmt(L("# Module %s\n\n## What it is\n\n%s. %s\n\n## Files\n\n- %s: the code\n- %s: its test cases\n\n## Functions\n\n", "# Modulo %s\n\n## O que e\n\n%s. %s\n\n## Arquivos\n\n- %s: o codigo\n- %s: os casos de teste\n\n## Funcoes\n\n"),
                           p->mod[m].name, x_dp_what(d), L(g_x_dp[d].fact_en, g_x_dp[d].fact_pt), x_src(p, m), p->testrel));
        for (int i = 0; i < p->mod[m].nfn; i++) buf_str(&b, cu_fmt("- `%s(%s)`: %s\n", p->mod[m].fn[i].name, x_arglist(p->mod[m].fn[i].f, "", ", "), L(p->mod[m].fn[i].f->en, p->mod[m].fn[i].f->pt)));
        x_put(s, x_dp_doc(p->mod[m].name), cu_keep(b.p), 0);
    }
    x_put(s, "docs/index.md", cu_keep(ix.p), 0);
}

static const char *models_dir(void);      /* app/catalog.c */
/* o vocabulario do tokenizador: o do modelo de trabalho, so o vocabulario (sem os pesos) */
static Model g_x_vm; static int g_x_vok = -1;
static const Tokenizer *x_vocab(void) {
    if (g_x_vok < 0) {
        const char *path = getenv("KATANA_VOCAB_MODEL"); static char found[1200];
        if (!path) {
            DIR *d = opendir(models_dir());
            for (struct dirent *e; d && (e = readdir(d)); ) {
                const size_t l = strlen(e->d_name);
                if (l > 5 && !strcmp(e->d_name + l - 5, ".gguf") && strncmp(e->d_name, "mmproj", 6) && (!found[0] || strstr(e->d_name, "Qwen3.6"))) snprintf(found, sizeof found, "%s/%s", models_dir(), e->d_name);
            }
            if (d) closedir(d);
            path = found[0] ? found : NULL;
        }
        g_x_vok = 0;
        if (path) { g_vocab_only = 1; memset(&g_x_vm, 0, sizeof g_x_vm); load_model(&g_x_vm, path); g_x_vok = g_x_vm.tk.n > 0; }
        if (!g_x_vok) fprintf(stderr, "curriculum: x-docs precisa do vocabulario de um modelo (KATANA_VOCAB_MODEL=arquivo.gguf)\n");
    }
    return g_x_vok ? &g_x_vm.tk : NULL;
}
/* a nota que o motor entregaria junto deste pedido (NULL = nada) */
static const char *x_ds_note(const char *req) {
    const Tokenizer *tk = x_vocab(); if (!tk) cu_die("sem vocabulario para o x-docs");
    static char out[8000];
    char cw[2048]; if (!getcwd(cw, sizeof cw)) cu_die("sem pasta de trabalho");
    br_sync(); g_br_seen_req = -1;
    return ds_select(tk, req, br_cur(), cw, out, sizeof out) ? cu_fmt("%s", out) : NULL;
}
enum { XD_HIT, XD_MISS, XD_WRONG, XD_N };
static const char *const g_x_dnames[XD_N] = {"hit", "miss", "wrong"};
static void fam_x_docs(FILE *f, Scn *s) {
    int sit, v, stk;
    for (int t = 0; ; t++) {
        if (t > 400) cu_die("sem celula de docs");
        sit = cu_n(XD_N); v = cu_n(2); stk = CU_PA(((const int[]){X_PY, X_JS, X_SH, X_PL}));
        if (x_held(160 + sit, v, stk, 0, 0) == (g_cu_hold != 0)) break;
    }
    static XProj P; x_proj(&P, s, stk, 3, 2); XProj *p = &P;
    p->brief = 1; p->idx = 1; p->knows = 1;
    const int mi = cu_n(3), oi = (mi + 1 + cu_n(2)) % 3;      /* o modulo pedido e outro que o motor entrega no caso wrong */
    const int d = x_dp_ix(p->mod[mi].name), od = x_dp_ix(p->mod[oi].name);
    x_begin("x-docs", s, p, 0, 0);
    x_cell("docs/%s/v%d/%s", g_x_dnames[sit], v, g_x_stid[p->st]);
    const char *nm = p->mod[mi].name, *doc = x_dp_doc(nm);
    const char *phr = sit == XD_HIT ? x_dp_what(d) : L(g_x_dp[d].alt_en, g_x_dp[d].alt_pt);
    const char *req = v ? cu_fmt(L("I need to change %s: which file is it and what does it hold?", "preciso mexer em %s: qual e o arquivo e o que tem nele?"), phr)
                        : cu_fmt(L("where do I go to change %s?", "onde eu mexo para mudar %s?"), phr);
    x_user(req);
    XMsg *um = &g_x.m[g_x.n - 1];
    const char *note = x_ds_note(um->text), *other = NULL;
    if (sit == XD_HIT) {
        int only = note && strstr(note, doc) != NULL;
        for (int k = 0; k < 3 && only; k++) if (k != mi && strstr(note, x_dp_doc(p->mod[k].name))) only = 0;
        if (!only) cu_die("o motor nao entregou so o documento certo (%s)", g_x.cell);
        um->text = cu_fmt("%s\n\n%s", um->text, note);
    } else if (sit == XD_MISS) {
        if (note) cu_die("o motor entregou algo onde devia faltar (%s)", g_x.cell);
    } else {
        const char *rq = cu_fmt(L("I need to change %s: which file is it and what does it hold?", "preciso mexer em %s: qual e o arquivo e o que tem nele?"), x_dp_what(od));
        other = x_ds_note(rq);
        if (!other || !strstr(other, x_dp_doc(p->mod[oi].name)) || strstr(other, doc)) cu_die("nao deu para forjar o trecho do outro modulo (%s)", g_x.cell);
        if (note) cu_die("o pedido ja casaria com um documento (%s)", g_x.cell);
        um->text = cu_fmt("%s\n\n%s", um->text, other);
    }
    const char *fact = L(g_x_dp[d].fact_en, g_x_dp[d].fact_pt);
    const char *tail = cu_fmt(L("It is `%s` and its cases are in `%s`. %s", "E o `%s` e os casos dele ficam em `%s`. %s"), x_src(p, mi), p->testrel, fact);
    if (sit != XD_HIT) {
        const char *r = x_read(s, "docs/index.md", 0);
        if (!strstr(r, x_dp_doc(nm) + 5)) cu_die("o indice nao aponta o modulo");
        r = x_read(s, doc, 0);
        if (!strstr(r, x_src(p, mi))) cu_die("o documento do modulo nao traz o arquivo");
    }
    if (sit == XD_WRONG)
        x_final(cu_fmt(L("The passage attached to your message is about the `%s` module, not about %s, so I went through the index: that is the `%s` module (%s). %s", "O trecho que veio junto da sua mensagem e do modulo `%s`, e nao de %s, entao fui pelo indice: e o modulo `%s` (%s). %s"),
                       p->mod[oi].name, phr, nm, doc, tail));
    else if (sit == XD_MISS) x_final(cu_fmt(L("That is the `%s` module (%s, found through docs/index.md). %s", "E o modulo `%s` (%s, achado pelo docs/index.md). %s"), nm, doc, tail));
    else x_final(cu_fmt(L("That is the `%s` module, per %s. %s", "E o modulo `%s`, conforme o %s. %s"), nm, doc, tail));
    x_end(f);
}
