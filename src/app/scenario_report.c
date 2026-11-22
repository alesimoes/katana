/* ---------- katana curriculum variety | matrix | show: as medidas e a leitura de um arquivo de curriculo ----------
 * variety <treino.jsonl> [validacao.jsonl] [-v]   variedade (sequencias de ferramentas, formas de pedido, segundo pedido dependente,
 *                                                 perguntas, prompt inteiro) e, com dois arquivos, o que a validacao repete do treino
 * matrix <arquivo.jsonl>                          celulas (tarefa x obstaculo), pilhas, convencoes, ferramentas por familia
 * show <arquivo.jsonl> [indice|familia|trecho da celula] [--sys]   as conversas em texto corrido
 * Definicoes (as mesmas nos tres): sequencia = ferramentas das falas treinadas, na ordem; MCP vira "mcp", TodoWrite so com done vira
 * "done"; "|" separa pedidos e "." e uma fala sem chamada. Pedido = mensagem do usuario que nao e nota do motor ("[katana: "). */

typedef struct { char *seq; char first[64], fam[40], shape[128], cell[240]; uint64_t *turn; char *tkind; char **ttext; int nturn; uint64_t *req; int nreq; int ask, real, dep, lang, mode; } CuRv;
static void cu_rv_free(CuRv *v) { free(v->seq); free(v->turn); free(v->tkind); free(v->req); memset(v, 0, sizeof *v); }
/* pedido mascarado: trechos entre crases, caminhos, nomes de arquivo e numeros viram marcas; caixa e espacos nao contam */
static uint64_t cu_rv_mask(const char *t) {
    static const char *const ext[] = {"py", "js", "sh", "c", "h", "pl", "md", "conf", "json", "txt", "ps1", "log", "csv", "ipynb"};
    Buf b = {0}; buf_str(&b, "");
    for (const char *p = t; *p; ) {
        if (*p == '`') { const char *e = strchr(p + 1, '`'); if (e) { buf_str(&b, "<C>"); p = e + 1; continue; } }
        if (!isspace((unsigned char)*p) && (p == t || isspace((unsigned char)p[-1]) || b.n == 0 || 1)) {      /* palavra com barra: caminho */
            size_t l = 0; int slash = 0; while (p[l] && !isspace((unsigned char)p[l]) && p[l] != '`') { if (p[l] == '/' && p[l + 1] && !isspace((unsigned char)p[l + 1])) slash = 1; l++; }
            if (slash && (p == t || isspace((unsigned char)p[-1]) || p[-1] == '`')) { buf_str(&b, "<P>"); p += l; continue; }
        }
        if (isalnum((unsigned char)*p) || *p == '_' || *p == '-') {      /* nome.ext */
            size_t l = 0; while (isalnum((unsigned char)p[l]) || p[l] == '_' || p[l] == '-') l++;
            if (p[l] == '.' && (p == t || !(isalnum((unsigned char)p[-1]) || p[-1] == '_'))) {
                size_t el = 0; while (isalnum((unsigned char)p[l + 1 + el])) el++;
                int hit = 0; for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++) hit |= strlen(ext[i]) == el && !strncmp(ext[i], p + l + 1, el);
                if (hit && p[l + 1 + el] != '_') { buf_str(&b, "<P>"); p += l + 1 + el; continue; }
            }
        }
        if (isdigit((unsigned char)*p)) { while (isdigit((unsigned char)*p)) p++; buf_str(&b, "#"); continue; }
        if (isspace((unsigned char)*p)) { while (isspace((unsigned char)*p)) p++; if (b.n && *p) buf_str(&b, " "); continue; }
        const char c = (char)tolower((unsigned char)*p); buf_add(&b, &c, 1); p++;
    }
    const uint64_t h = cu_fnv(b.p, strlen(b.p), 1469598103934665603ull);
    free(b.p);
    return h;
}
static void cu_rv_scan(const char *line, CuRv *v) {
    memset(v, 0, sizeof *v);
    Buf sq = {0}; buf_str(&sq, "");
    const char *fam = cu_jstr(cu_jget(line, "family")), *shape = cu_jstr(cu_jget(line, "shape")), *cell = cu_jstr(cu_jget(line, "cell")), *dep = cu_jget(line, "dep"), *sc = cu_jget(line, "scn");
    snprintf(v->fam, sizeof v->fam, "%s", fam ? fam : "?"); snprintf(v->shape, sizeof v->shape, "%s", shape && *shape ? shape : "plain"); snprintf(v->cell, sizeof v->cell, "%s", cell ? cell : "");
    v->dep = dep && *dep == '1'; v->lang = -1; v->mode = -1;
    if (sc) { const char *lg = cu_jget(sc, "lang"), *md = cu_jget(sc, "mode"); if (lg) v->lang = atoi(lg); if (md) v->mode = atoi(md); }
    const char *msgs = cu_jget(line, "messages"), *el, *e; int first = 1;
    if (!msgs || *msgs != '[') { v->seq = sq.p; return; }
    for (const char *p = msgs + 1; (e = gs_elem(p, &el)); p = e) {
        const char *role = cu_jstr(cu_jget(el, "role")), *content = cu_jstr(cu_jget(el, "content")), *tc = cu_jget(el, "tool_calls"), *tr = cu_jget(el, "train");
        if (!role) continue;
        if (!content) content = "";
        if (!strcmp(role, "system")) { v->real = (strstr(content, "# Tools") || strstr(content, "<tools>")) && strlen(content) > 6000; continue; }
        if (!strcmp(role, "user")) {
            if (!strncmp(content, "[katana: ", 9)) continue;
            if (sq.n) buf_str(&sq, sq.n ? ">|" : "|");
            v->req = realloc(v->req, sizeof *v->req * (size_t)(v->nreq + 1)); v->req[v->nreq++] = cu_rv_mask(content);
            continue;
        }
        if (strcmp(role, "assistant") || (tr && *tr == 'f')) continue;
        uint64_t h = cu_fnv(content, strlen(content), 1469598103934665603ull);
        Buf tx = {0}; buf_str(&tx, content);
        char kind = 'f'; int allempty = 1;      /* f fala final, s ToolSearch, p plano escrito, n chamada sem argumento livre, c outra chamada */
        if (!tc || *tc != '[') { buf_str(&sq, sq.n ? ">." : "."); if (first) { snprintf(v->first, sizeof v->first, "(answer)"); first = 0; } }
        else {
            const char *c1, *ce;
            for (const char *q = tc + 1; (ce = gs_elem(q, &c1)); q = ce) {
                const char *fnv = cu_jget(c1, "function"), *name = fnv ? cu_jstr(cu_jget(fnv, "name")) : NULL, *args = fnv ? cu_jget(fnv, "arguments") : NULL;
                if (!name) continue;
                const char *ae = args ? gs_skipv(args) : NULL; const size_t al = args && ae ? (size_t)(ae - args) : 0;
                h = cu_fnv("\n", 1, h); h = cu_fnv(name, strlen(name), h); h = cu_fnv(" ", 1, h); if (al) h = cu_fnv(args, al, h);
                const char *arg0 = al ? cu_fmt("%.*s", (int)al, args) : "";
                buf_str(&tx, cu_fmt(" // %s %.110s", name, arg0));
                const char *shown = strstr(name, "__") ? "mcp" : !strcmp(name, "TodoWrite") && strstr(arg0, "\"done\"") && !strstr(arg0, "\"todos\"") ? "done" : name;
                buf_str(&sq, sq.n ? ">" : ""); buf_str(&sq, shown);
                if (first) { snprintf(v->first, sizeof v->first, "%s", strstr(name, "__") ? "mcp" : name); first = 0; }
                if (!strcmp(name, "AskUserQuestion")) v->ask = 1;
                if (!strcmp(name, "ToolSearch")) kind = 's';
                else if (!strcmp(name, "TodoWrite") && strstr(arg0, "\"todos\"")) { if (kind != 's') kind = 'p'; }
                else if (strcmp(shown, "done") && strcmp(arg0, "{}")) allempty = 0;
            }
            if (kind == 'f') kind = allempty ? 'n' : 'c';
        }
        v->tkind = realloc(v->tkind, (size_t)(v->nturn + 1)); v->tkind[v->nturn] = kind;
        v->ttext = realloc(v->ttext, sizeof(char *) * (size_t)(v->nturn + 1)); if (tx.n > 170) tx.p[170] = 0; v->ttext[v->nturn] = tx.p;
        v->turn = realloc(v->turn, sizeof *v->turn * (size_t)(v->nturn + 1)); v->turn[v->nturn++] = h;
    }
    v->seq = sq.p;
}
static int cu_u64_cmp(const void *a, const void *b) { const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
typedef struct { char *k; long n; } CuCnt;
typedef struct { CuCnt *v; long n, cap; } CuCnts;
static void cu_cnt_add(CuCnts *c, const char *k) {
    for (long i = 0; i < c->n; i++) if (!strcmp(c->v[i].k, k)) { c->v[i].n++; return; }
    if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 256; c->v = realloc(c->v, sizeof *c->v * (size_t)c->cap); }
    c->v[c->n].k = strdup(k); c->v[c->n++].n = 1;
}
static int cu_cnt_cmp(const void *a, const void *b) { const CuCnt *x = a, *y = b; return x->n > y->n ? -1 : x->n < y->n ? 1 : strcmp(x->k, y->k); }
static int cu_cnt_name(const void *a, const void *b) { return strcmp(((const CuCnt *)a)->k, ((const CuCnt *)b)->k); }
static void cu_cnt_print(const char *label, CuCnts *c, int byname) {
    qsort(c->v, (size_t)c->n, sizeof *c->v, byname ? cu_cnt_name : cu_cnt_cmp);
    printf("%s", label); for (long i = 0; i < c->n; i++) printf("%s%s %ld", i ? ", " : "", c->v[i].k, c->v[i].n); printf("\n");
}
typedef struct { CuRv *v; long n; CuCnts seqs; uint64_t *turns, *reqs; long nturns, nreqs; } CuFile;
static int cu_file_load(const char *path, CuFile *f) {
    memset(f, 0, sizeof *f);
    FILE *in = fopen(path, "rb"); if (!in) { fprintf(stderr, "curriculum: nao abri %s\n", path); return 0; }
    char *line = NULL; size_t cap = 0; long cv = 0;
    while (cu_slurp_line(in, &line, &cap)) {
        if (!*line) continue;
        if (f->n == cv) { cv = cv ? cv * 2 : 512; f->v = realloc(f->v, sizeof *f->v * (size_t)cv); }
        cu_rv_scan(line, &f->v[f->n++]);
        cu_arena_free();
    }
    fclose(in); free(line);
    for (long i = 0; i < f->n; i++) { f->nturns += f->v[i].nturn; f->nreqs += f->v[i].nreq; cu_cnt_add(&f->seqs, f->v[i].seq); }
    f->turns = malloc(sizeof(uint64_t) * (size_t)(f->nturns + 1)); f->reqs = malloc(sizeof(uint64_t) * (size_t)(f->nreqs + 1));
    long a = 0, b = 0;
    for (long i = 0; i < f->n; i++) { for (int k = 0; k < f->v[i].nturn; k++) f->turns[a++] = f->v[i].turn[k]; for (int k = 0; k < f->v[i].nreq; k++) f->reqs[b++] = f->v[i].req[k]; }
    qsort(f->turns, (size_t)a, sizeof(uint64_t), cu_u64_cmp); qsort(f->reqs, (size_t)b, sizeof(uint64_t), cu_u64_cmp);
    return 1;
}
static void cu_variety_report(const char *name, CuFile *f, int verbose) {
    const long n = f->n;
    qsort(f->seqs.v, (size_t)f->seqs.n, sizeof *f->seqs.v, cu_cnt_cmp);
    long top20 = 0; for (long i = 0; i < f->seqs.n && i < 20; i++) top20 += f->seqs.v[i].n;
    long multi = 0, dep = 0, ask = 0, real = 0, lang[2] = {0, 0};
    CuCnts shapes = {0}, fams = {0};
    for (long i = 0; i < n; i++) {
        const CuRv *v = &f->v[i];
        multi += v->nreq >= 2; dep += v->dep; ask += v->ask; real += v->real; if (v->lang == 0 || v->lang == 1) lang[v->lang]++;
        cu_cnt_add(&fams, v->fam);
        char sh[128]; snprintf(sh, sizeof sh, "%s", v->shape);
        for (char *t = strtok(sh, "+"); t; t = strtok(NULL, "+")) cu_cnt_add(&shapes, t);
    }
    qsort(fams.v, (size_t)fams.n, sizeof *fams.v, cu_cnt_name);
    Buf one_seq = {0}, one_first = {0}; buf_str(&one_seq, ""); buf_str(&one_first, ""); int n1s = 0, n1f = 0;
    Buf detail = {0}; buf_str(&detail, "");
    for (long k = 0; k < fams.n; k++) {      /* por familia: sequencias distintas e primeiras ferramentas distintas */
        CuCnts sq = {0}, ft = {0};
        for (long i = 0; i < n; i++) if (!strcmp(f->v[i].fam, fams.v[k].k)) { cu_cnt_add(&sq, f->v[i].seq); cu_cnt_add(&ft, f->v[i].first); }
        if (sq.n == 1) { buf_str(&one_seq, cu_fmt("%s%s", n1s++ ? ", " : "", fams.v[k].k)); }
        if (ft.n == 1) { buf_str(&one_first, cu_fmt("%s%s", n1f++ ? ", " : "", fams.v[k].k)); }
        qsort(ft.v, (size_t)ft.n, sizeof *ft.v, cu_cnt_name);
        buf_str(&detail, cu_fmt("   %-16s conv %3ld  seqs %3ld  first tools %ld:", fams.v[k].k, fams.v[k].n, sq.n, ft.n));
        for (long i = 0; i < ft.n; i++) buf_str(&detail, cu_fmt(" %s", ft.v[i].k));
        buf_str(&detail, "\n");
        for (long i = 0; i < sq.n; i++) free(sq.v[i].k);
        for (long i = 0; i < ft.n; i++) free(ft.v[i].k);
        free(sq.v); free(ft.v); cu_arena_free();
    }
    printf("== %s: %ld conversations, %ld trained turns, %ld requests\n", name, n, f->nturns, f->nreqs);
    printf("distinct tool sequences: %ld\n", f->seqs.n);
    printf("conversations in the 20 most common tool sequences: %ld of %ld (%.1f%%)\n", top20, n, 100.0 * (double)top20 / (double)(n ? n : 1));
    printf("families with one sequence: %d [%s]\n", n1s, one_seq.p);
    printf("families with one first tool: %d [%s]\n", n1f, one_first.p);
    printf("conversations with two or more requests: %ld (%.1f%%); with a second request that depends on the first (line flag dep): %ld (%.1f%%)\n", multi, 100.0 * (double)multi / (double)(n ? n : 1), dep, 100.0 * (double)dep / (double)(n ? n : 1));
    printf("conversations with AskUserQuestion: %ld (%.1f%%)\n", ask, 100.0 * (double)ask / (double)(n ? n : 1));
    printf("conversations with the real full prompt (tools section present): %ld (%.1f%%)\n", real, 100.0 * (double)real / (double)(n ? n : 1));
    cu_cnt_print("request shapes (line flag shape; a conversation may carry several): ", &shapes, 0);
    printf("language of the scenario: en %ld, pt %ld\n", lang[0], lang[1]);
    if (verbose) {
        printf("%s", detail.p);
        for (long i = 0; i < f->seqs.n && i < 20; i++) printf("   %4ld  %.170s\n", f->seqs.v[i].n, f->seqs.v[i].k);
    }
    free(one_seq.p); free(one_first.p); free(detail.p);
}
static int cu_variety_main(int argc, char **argv) {
    const char *a[2] = {NULL, NULL}; int na = 0, verbose = 0;
    for (int i = 0; i < argc; i++) { if (!strcmp(argv[i], "-v")) verbose = 1; else if (na < 2) a[na++] = argv[i]; }
    if (!na) { fprintf(stderr, "uso: katana curriculum variety <treino.jsonl> [validacao.jsonl] [-v]\n"); return 1; }
    static CuFile tr, ho;
    if (!cu_file_load(a[0], &tr)) return 1;
    cu_variety_report("train", &tr, verbose);
    if (na < 2) return 0;
    if (!cu_file_load(a[1], &ho)) return 1;
    cu_variety_report("validation", &ho, verbose);
    long req_eq = 0, turn_eq = 0, unseen = 0;
    for (long i = 0; i < ho.nreqs; i++) req_eq += bsearch(&ho.reqs[i], tr.reqs, (size_t)tr.nreqs, sizeof(uint64_t), cu_u64_cmp) != NULL;
    for (long i = 0; i < ho.nturns; i++) turn_eq += bsearch(&ho.turns[i], tr.turns, (size_t)tr.nturns, sizeof(uint64_t), cu_u64_cmp) != NULL;
    for (long i = 0; i < ho.n; i++) { int seen = 0; for (long k = 0; k < tr.seqs.n && !seen; k++) seen = !strcmp(tr.seqs.v[k].k, ho.v[i].seq); unseen += !seen; }
    printf("== validation against train\n");
    printf("validation requests equal to a training request (paths, file names, backticked text and digits masked; case and spacing ignored): %ld of %ld (%.1f%%)\n", req_eq, ho.nreqs, 100.0 * (double)req_eq / (double)(ho.nreqs ? ho.nreqs : 1));
    printf("validation trained turns equal to a training trained turn (exact text and calls): %ld of %ld (%.1f%%)\n", turn_eq, ho.nturns, 100.0 * (double)turn_eq / (double)(ho.nturns ? ho.nturns : 1));
    {   long byk[5] = {0, 0, 0, 0, 0}, allk[5] = {0, 0, 0, 0, 0}; CuCnts byf = {0}, rept = {0}; static const char kk[] = "fspnc";
        for (long i = 0; i < ho.n; i++) for (int k = 0; k < ho.v[i].nturn; k++) {
            const int ki = (int)(strchr(kk, ho.v[i].tkind[k]) - kk); allk[ki]++;
            if (bsearch(&ho.v[i].turn[k], tr.turns, (size_t)tr.nturns, sizeof(uint64_t), cu_u64_cmp)) { byk[ki]++; cu_cnt_add(&byf, ho.v[i].fam); if (verbose) cu_cnt_add(&rept, ho.v[i].ttext[k]); }
        }
        printf("  the repeated turns by kind (repeated of all of that kind): final answer %ld of %ld, ToolSearch %ld of %ld, written plan %ld of %ld, call with no free argument %ld of %ld, other call %ld of %ld\n",
               byk[0], allk[0], byk[1], allk[1], byk[2], allk[2], byk[3], allk[3], byk[4], allk[4]);
        if (verbose) { cu_cnt_print("  the repeated turns by family: ", &byf, 0); qsort(rept.v, (size_t)rept.n, sizeof *rept.v, cu_cnt_cmp); for (long i = 0; i < rept.n && i < 25; i++) printf("   %4ld  %s\n", rept.v[i].n, rept.v[i].k); } }
    printf("validation conversations whose tool sequence the training file does not have: %ld of %ld (%.1f%%)\n", unseen, ho.n, 100.0 * (double)unseen / (double)(ho.n ? ho.n : 1));
    return 0;
}
/* matrix: celulas tarefa x obstaculo das familias de tarefa, as outras celulas, e as ferramentas (conversas com chamada treinada, familias, erros do motor) */
static int cu_matrix_main(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "uso: katana curriculum matrix <arquivo.jsonl>\n"); return 1; }
    FILE *in = fopen(argv[0], "rb"); if (!in) { fprintf(stderr, "curriculum matrix: nao abri %s\n", argv[0]); return 1; }
    static const char *const tasks[] = {"fix", "add", "rename", "find", "explain", "run", "config", "clean", "investigate"};
    CuCnts cells = {0}, stacks = {0}, conv = {0}, fu = {0}, pre = {0}, other = {0}, fams = {0}, tools = {0}, toolfam = {0}, toolerr = {0}, untr = {0}, modes = {0};
    char *line = NULL; size_t cap = 0; long n = 0;
    while (cu_slurp_line(in, &line, &cap)) {
        if (!*line) continue;
        n++;
        const char *fam = cu_jstr(cu_jget(line, "family")), *cell = cu_jstr(cu_jget(line, "cell")), *sc = cu_jget(line, "scn");
        cu_cnt_add(&fams, fam ? fam : "?");
        if (sc) { const char *md = cu_jget(sc, "mode"), *lg = cu_jget(sc, "lang"); cu_cnt_add(&modes, cu_fmt("%s/%s", md && *md == '0' ? "full" : md && *md == '1' ? "blocks" : "real", lg && *lg == '1' ? "pt" : "en")); }
        char *p[8]; int np = 0; char *cp = cu_fmt("%s", cell ? cell : "");
        for (char *t = strtok(cp, "/"); t && np < 8; t = strtok(NULL, "/")) p[np++] = t;
        int ist = 0; if (np >= 7) for (int i = 0; i < CU_N(tasks); i++) ist |= !strcmp(p[1], tasks[i]);
        if (ist) { cu_cnt_add(&cells, cu_fmt("%s x %s", p[1], p[2])); cu_cnt_add(&stacks, p[0]); cu_cnt_add(&conv, p[6]); cu_cnt_add(&fu, p[5]); cu_cnt_add(&pre, p[4]); }
        else if (np >= 2) cu_cnt_add(&other, cu_fmt("%s/%s", p[0], p[1]));
        const char *msgs = cu_jget(line, "messages"), *el, *e;
        char seen[64][64]; int ns = 0; char idn[40][2][64]; int nid = 0;
        for (const char *q0 = msgs ? msgs + 1 : ""; msgs && (e = gs_elem(q0, &el)); q0 = e) {
            const char *role = cu_jstr(cu_jget(el, "role")), *tc = cu_jget(el, "tool_calls"), *tr = cu_jget(el, "train");
            if (role && !strcmp(role, "assistant") && tc && *tc == '[') {
                const char *c1, *ce; const int train = !(tr && *tr == 'f');
                for (const char *q = tc + 1; (ce = gs_elem(q, &c1)); q = ce) {
                    const char *fnv = cu_jget(c1, "function"), *name = fnv ? cu_jstr(cu_jget(fnv, "name")) : NULL, *id = cu_jstr(cu_jget(c1, "id"));
                    if (!name) continue;
                    const char *shown = strstr(name, "__") ? "MCP tools" : name;
                    if (id) { snprintf(idn[nid % 40][0], 64, "%s", id); snprintf(idn[nid % 40][1], 64, "%s", shown); nid++; }
                    if (!train) { cu_cnt_add(&untr, shown); continue; }
                    int dup = 0; for (int i = 0; i < ns; i++) dup |= !strcmp(seen[i], shown);
                    if (!dup && ns < 64) snprintf(seen[ns++], 64, "%s", shown);
                }
            } else if (role && !strcmp(role, "tool")) {
                const char *id = cu_jstr(cu_jget(el, "tool_call_id")), *content = cu_jstr(cu_jget(el, "content"));
                if (id && content && !strncmp(content, "error:", 6)) for (int i = 0; i < nid && i < 40; i++) if (!strcmp(idn[i][0], id)) { cu_cnt_add(&toolerr, idn[i][1]); break; }
            }
        }
        for (int i = 0; i < ns; i++) { cu_cnt_add(&tools, seen[i]); const char *k = cu_fmt("%s\t%s", seen[i], fam ? fam : "?"); int had = 0; for (long j = 0; j < toolfam.n; j++) had |= !strcmp(toolfam.v[j].k, k); if (!had) cu_cnt_add(&toolfam, k); }
        cu_arena_free();
    }
    fclose(in); free(line);
    printf("conversations: %ld\n", n);
    cu_cnt_print("prompt mode / language: ", &modes, 1);
    cu_cnt_print("cells, task x obstacle: ", &cells, 1);
    cu_cnt_print("stacks: ", &stacks, 1); cu_cnt_print("conventions: ", &conv, 1); cu_cnt_print("request before: ", &pre, 1); cu_cnt_print("second request: ", &fu, 1);
    cu_cnt_print("other cells: ", &other, 1);
    printf("tools: conversations with a trained call | families | results that are engine errors | untrained calls kept as context\n");
    qsort(tools.v, (size_t)tools.n, sizeof *tools.v, cu_cnt_cmp);
    for (long i = 0; i < tools.n; i++) {
        long nf = 0, ne = 0, nu = 0; const size_t l = strlen(tools.v[i].k);
        for (long j = 0; j < toolfam.n; j++) nf += !strncmp(toolfam.v[j].k, tools.v[i].k, l) && toolfam.v[j].k[l] == '\t';
        for (long j = 0; j < toolerr.n; j++) if (!strcmp(toolerr.v[j].k, tools.v[i].k)) ne = toolerr.v[j].n;
        for (long j = 0; j < untr.n; j++) if (!strcmp(untr.v[j].k, tools.v[i].k)) nu = untr.v[j].n;
        printf("  %-22s %4ld | %2ld | %3ld | %3ld\n", tools.v[i].k, tools.v[i].n, nf, ne, nu);
    }
    cu_cnt_print("families: ", &fams, 1);
    return 0;
}
/* show: as conversas em texto corrido; o seletor e o indice da linha (a partir de 0), a familia ou um trecho da celula */
static int cu_show_main(int argc, char **argv) {
    const char *path = NULL, *sel = NULL; int showsys = 0;
    for (int i = 0; i < argc; i++) { if (!strcmp(argv[i], "--sys")) showsys = 1; else if (!path) path = argv[i]; else if (!sel) sel = argv[i]; }
    if (!path) { fprintf(stderr, "uso: katana curriculum show <arquivo.jsonl> [indice|familia|trecho da celula] [--sys]\n"); return 1; }
    FILE *in = fopen(path, "rb"); if (!in) { fprintf(stderr, "curriculum show: nao abri %s\n", path); return 1; }
    char *line = NULL; size_t cap = 0; long i = -1;
    while (cu_slurp_line(in, &line, &cap)) {
        if (!*line) continue;
        i++;
        const char *fam = cu_jstr(cu_jget(line, "family")), *cell = cu_jstr(cu_jget(line, "cell")), *seq = cu_jstr(cu_jget(line, "seq")), *shape = cu_jstr(cu_jget(line, "shape"));
        if (sel && (isdigit((unsigned char)sel[0]) ? atol(sel) != i : !((fam && !strcmp(sel, fam)) || (cell && strstr(cell, sel))))) { cu_arena_free(); continue; }
        printf("============================== %ld %s %s | shape: %s | seq: %s\n", i, fam ? fam : "?", cell ? cell : "", shape ? shape : "", seq ? seq : "");
        const char *msgs = cu_jget(line, "messages"), *el, *e;
        for (const char *q0 = msgs ? msgs + 1 : ""; msgs && (e = gs_elem(q0, &el)); q0 = e) {
            const char *role = cu_jstr(cu_jget(el, "role")), *content = cu_jstr(cu_jget(el, "content")), *tc = cu_jget(el, "tool_calls"), *tr = cu_jget(el, "train");
            if (!role) continue;
            if (!content) content = "";
            if (!strcmp(role, "system")) { printf("[system %zu chars]\n", strlen(content)); if (showsys) printf("%s\n", content); }
            else if (!strcmp(role, "user")) printf("USER: %s\n", content);
            else if (!strcmp(role, "assistant")) {
                const char *nt = tr && *tr == 'f' ? " (not trained)" : "";
                if (*content) printf("ASSISTANT%s: %s\n", nt, content);
                const char *c1, *ce;
                for (const char *q = tc && *tc == '[' ? tc + 1 : ""; tc && *tc == '[' && (ce = gs_elem(q, &c1)); q = ce) {
                    const char *fnv = cu_jget(c1, "function"), *name = fnv ? cu_jstr(cu_jget(fnv, "name")) : NULL, *args = fnv ? cu_jget(fnv, "arguments") : NULL;
                    const char *ae = args ? gs_skipv(args) : NULL;
                    printf("  CALL%s %s %.*s\n", nt, name ? name : "?", (int)(args && ae ? ae - args : 0), args ? args : "");
                }
            } else {
                const char *name = cu_jstr(cu_jget(el, "name")); const size_t l = strlen(content);
                if (l < 1500) printf("  RESULT[%s]: %s\n", name ? name : "?", content); else printf("  RESULT[%s]: %.1500s ...[%zu chars]\n", name ? name : "?", content, l);
            }
        }
        cu_arena_free();
    }
    fclose(in); free(line);
    return 0;
}
