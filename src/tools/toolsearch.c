/* ---------- ferramenta ToolSearch ----------
 * Carrega a definicao completa (esquema) de ferramentas anunciadas so pelo nome. Procura na tabela de src/tools/ e nas
 * ferramentas dos servidores MCP; cada resultado e uma linha de JSON no mesmo formato do prompt de sistema.
 *
 * A tabela g_ct e definida em table.c, que vem DEPOIS deste arquivo. Por isso a leitura passa por um acessor que
 * table.c deve definir (nao-static):
 *     int ct_tool_info(int i, const char **name, const char **desc, const char **props, const char **req)
 * devolve 1 e preenche os campos da i-esima ferramenta, ou 0 se i passou do fim. Enquanto ele nao existir (declaracao
 * fraca), so as ferramentas MCP sao pesquisadas. */
#if defined(__GNUC__) && !defined(_WIN32)
int ct_tool_info(int i, const char **name, const char **desc, const char **props, const char **req) __attribute__((weak));
#define TSR_HAVE_TABLE (ct_tool_info != NULL)
#else
#define ct_tool_info(i, n, d, p, r) 0
#define TSR_HAVE_TABLE 0
#endif

static const char CT_TOOLSEARCH_DESC[] =
    "Load the full definition (name, description, parameters) of tools that were announced by name only, so you can call them. "
    "Query forms: \"select:Name1,Name2\" loads exactly those tools; otherwise a keyword search over tool names and descriptions "
    "(a word written +word must appear in the name; the other words rank the results). Returns one JSON definition per line, "
    "or \"No matching tools\".";
static const char CT_TOOLSEARCH_PROPS[] =
    "\"query\": {\"type\": \"string\", \"description\": \"select:Name1,Name2 to load tools by exact name, or keywords (+word requires it in the name)\"}, "
    "\"max_results\": {\"type\": \"integer\", \"description\": \"Maximum number of results (default 5)\"}";
static const char CT_TOOLSEARCH_REQ[] = "\"query\"";

typedef struct { char *name, *desc, *schema; int score; int order; } TsrCand;   /* schema: JSON do objeto "parameters" */

static int tsr_ci_has(const char *h, const char *needle) {
    const size_t n = strlen(needle);
    if (!n) return 1;
    for (; *h; h++) if (strncasecmp(h, needle, n) == 0) return 1;
    return 0;
}
static void tsr_esc(McpB *b, const char *s) { mcp_besc(b, s); }

static void tsr_add(TsrCand **v, int *n, char *name, char *desc, char *schema) {
    *v = (TsrCand *)realloc(*v, sizeof(TsrCand) * (size_t)(*n + 1));
    (*v)[*n] = (TsrCand){ name, desc, schema, 0, *n };
    (*n)++;
}
/* todas as ferramentas conhecidas (copias em memoria nova) */
static int tsr_collect(TsrCand **v) {
    int n = 0;
    if (TSR_HAVE_TABLE) {
        const char *nm, *ds, *pr, *rq;
        for (int i = 0; ct_tool_info(i, &nm, &ds, &pr, &rq); i++) {
            McpB sc = {0};
            mcp_bputs(&sc, "{\"type\": \"object\", \"properties\": {"); mcp_bputs(&sc, pr);
            mcp_bputs(&sc, "}, \"required\": ["); mcp_bputs(&sc, rq); mcp_bputs(&sc, "]}");
            tsr_add(v, &n, strdup(nm), strdup(ds), sc.p);
        }
    }
    mcp_init(0);
    for (int i = 0; i < g_nmcp; i++) {
        if (!g_mcp[i].alive) continue;
        for (int j = 0; j < g_mcp[i].ntools; j++) {
            const McpTool *t = &g_mcp[i].tools[j];
            char fn[200], d[700];
            mcp_fname(&g_mcp[i], t->name, fn, sizeof fn);
            snprintf(d, sizeof d, "%.400s (MCP server %.47s)", t->desc, g_mcp[i].name);
            tsr_add(v, &n, strdup(fn), strdup(d), strdup(t->schema));
        }
    }
    return n;
}
static void tsr_free(TsrCand *v, int n) {
    for (int i = 0; i < n; i++) { free(v[i].name); free(v[i].desc); free(v[i].schema); }
    free(v);
}
static int tsr_cmp(const void *a, const void *b) {
    const TsrCand *x = (const TsrCand *)a, *y = (const TsrCand *)b;
    if (x->score != y->score) return y->score - x->score;
    return x->order - y->order;
}
/* uma linha de definicao no formato do prompt; 0 se nao couber no espaco que resta */
static int tsr_emit(const TsrCand *c, char *out, size_t cap, size_t *o) {
    McpB b = {0};
    mcp_bputs(&b, "{\"type\": \"function\", \"function\": {\"name\": "); tsr_esc(&b, c->name);
    mcp_bputs(&b, ", \"description\": "); tsr_esc(&b, c->desc);
    mcp_bputs(&b, ", \"parameters\": "); mcp_bputs(&b, c->schema); mcp_bputs(&b, "}}");
    const int fits = *o + b.n + 2 <= cap;
    if (fits) { memcpy(out + *o, b.p, b.n); *o += b.n; out[(*o)++] = '\n'; out[*o] = 0; }
    free(b.p);
    return fits;
}

static void ct_toolsearch(const char *js, const char *end, char *out, size_t cap) {
    char *q = ct_str(js, end, "query");
    if (!q || !*q) { snprintf(out, cap, "error: query is required (select:Name1,Name2 or keywords)"); free(q); return; }
    long long max = ct_int(js, end, "max_results", 5);
    if (max < 1) max = 5;
    if (max > 50) max = 50;
    TsrCand *v = NULL; const int n = tsr_collect(&v);
    int *pick = (int *)calloc((size_t)(n ? n : 1), sizeof(int)); int np = 0;
    char *p = q;
    while (*p == ' ') p++;
    if (strncasecmp(p, "select:", 7) == 0) {
        for (char *t = strtok(p + 7, ", \t"); t; t = strtok(NULL, ", \t"))
            for (int i = 0; i < n; i++) if (strcasecmp(v[i].name, t) == 0 || strcasecmp(v[i].name + (strncmp(v[i].name, "mcp__", 5) == 0 ? 5 : 0), t + (strncmp(t, "mcp__", 5) == 0 ? 5 : 0)) == 0) {
                int dup = 0;
                for (int k = 0; k < np; k++) if (pick[k] == i) dup = 1;
                if (!dup) pick[np++] = i;
                break;
            }
    } else {
        char *req[16], *opt[16]; int nr = 0, no = 0;
        for (char *t = strtok(p, " \t\n"); t; t = strtok(NULL, " \t\n")) {
            if (*t == '+' && t[1] && nr < 16) req[nr++] = t + 1;
            else if (*t != '+' && no < 16) opt[no++] = t;
        }
        TsrCand *rank = (TsrCand *)malloc(sizeof(TsrCand) * (size_t)(n ? n : 1)); int nrk = 0;
        float *cs = NULL;
        if (no > 0 && n > 0 && es_available()) {
            char qq[1024]; size_t ql = 0; qq[0] = 0;      /* a consulta sem os marcadores +; o strtok ja cortou o texto, entao junta as palavras */
            for (int k = 0; k < nr && ql + 40 < sizeof qq; k++) ql += (size_t)snprintf(qq + ql, sizeof qq - ql, "%.100s ", req[k]);
            for (int k = 0; k < no && ql + 40 < sizeof qq; k++) ql += (size_t)snprintf(qq + ql, sizeof qq - ql, "%.100s ", opt[k]);
            const char **tp = (const char **)malloc(sizeof *tp * (size_t)n), **tt = (const char **)malloc(sizeof *tt * (size_t)n);
            for (int i = 0; i < n; i++) { tp[i] = v[i].desc; tt[i] = v[i].name; }
            cs = (float *)malloc(sizeof *cs * (size_t)n);
            const char *bm = getenv("KATANA_EMBED_TOOLS_MS");
            if (es_rank(qq, EMB_SEARCH, tp, tt, n, bm && atoi(bm) > 0 ? atof(bm) : 1500.0, cs) <= 0) { free(cs); cs = NULL; }
            free(tp); free(tt);
        }
        for (int i = 0; i < n; i++) {
            int ok = 1, sc = 0;
            for (int k = 0; k < nr; k++) if (!tsr_ci_has(v[i].name, req[k])) ok = 0; else sc += 4;
            if (!ok) continue;
            for (int k = 0; k < no; k++) {
                if (tsr_ci_has(v[i].name, opt[k])) sc += 4;
                if (tsr_ci_has(v[i].desc, opt[k])) sc += 1;
            }
            /* sentido (servico de embeddings): a consulta inteira contra a descricao da ferramenta; soma na nota por palavras, e uma
             * ferramenta sem palavra em comum entra se o cosseno for >= 0,65 (calibrado por analogia com a memoria: cosseno de textos sem relacao fica
             * em 0,5 a 0,68). So reordena e sugere: nada sai da lista. */
            int sem = 0;
            if (cs && cs[i] >= 0.3f) sem = (int)(cs[i] * 20.0f);
            if (sc <= 0 && !(cs && cs[i] >= 0.65f)) continue;
            rank[nrk] = v[i]; rank[nrk].score = sc * 2 + sem; rank[nrk].order = i; nrk++;
        }
        free(cs);
        qsort(rank, (size_t)nrk, sizeof(TsrCand), tsr_cmp);
        for (int k = 0; k < nrk; k++) pick[np++] = rank[k].order;
        free(rank);
    }
    size_t o = 0; int shown = 0;
    out[0] = 0;
    for (int k = 0; k < np && shown < max; k++) {
        if (!tsr_emit(&v[pick[k]], out, cap, &o)) break;
        shown++;
    }
    if (o && out[o - 1] == '\n') out[--o] = 0;
    if (!np) snprintf(out, cap, "No matching tools");
    else if (!shown) snprintf(out, cap, "error: the matching definition is too large for the result buffer; ask for fewer tools");
    free(pick); tsr_free(v, n); free(q);
}

/* Prepara os vetores das ferramentas da tabela (descricao, titulo = nome) no cache de embeddings, para a primeira busca ja achar tudo
 * pronto. Roda na linha de pre-carga (embsvc.c); so as ferramentas da tabela: as do MCP sobem sob demanda e nao se mexe nelas aqui. */
static void tsr_warm(void) {
    if (!TSR_HAVE_TABLE) return;
    const char *nm, *ds, *pr, *rq;
    float tmp[ES_DIM];
    for (int i = 0; ct_tool_info(i, &nm, &ds, &pr, &rq); i++) es_vec(ds, EMB_DOCUMENT, nm, tmp);
}
