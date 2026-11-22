/* ---------- katana curriculum pick: a escolha das conversas por cobertura ---------- */
/* katana curriculum pick <reserva.jsonl> <saida.jsonl> [--n N]: escolhe conversas por cobertura, nao por contagem.
   Ordem: (1) cada ferramenta da tabela de cobertura ate o seu minimo, 15 conversas de .ps1, 25 com ferramentas MCP e 8 de cada familia; (2) uma conversa de cada sequencia de ferramentas
   (familia + chamadas na ordem, com os pedidos), das menores para as maiores; (3) celulas ainda nao vistas; (4) o resto, na
   ordem do arquivo. AskUserQuestion fica entre 4,5% e 5% das escolhidas. A saida mantem a ordem da reserva. */
typedef struct { char *line; uint64_t sig, cell; unsigned long long tools; int ask, taken, ps1, mcp, fam, dep, real; uint64_t ffirst; size_t len; } CuPk;
static int cu_pk_cmp(const void *a, const void *b) { const CuPk *x = *(CuPk *const *)a, *y = *(CuPk *const *)b; return x->len < y->len ? -1 : x->len > y->len ? 1 : x < y ? -1 : 1; }
static int cu_pick_main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: katana curriculum pick <reserva.jsonl> <saida.jsonl> [--n N]\n"); return 1; }
    long n = 400;
    for (int i = 2; i + 1 < argc; i += 2) if (!strcmp(argv[i], "--n")) n = atol(argv[i + 1]); else { fprintf(stderr, "curriculum pick: opcao desconhecida %s\n", argv[i]); return 1; }
    FILE *f = fopen(argv[0], "rb");
    if (!f) { fprintf(stderr, "curriculum pick: nao abri %s\n", argv[0]); return 1; }
    CuPk *v = NULL; long nv = 0, cv = 0; char *line = NULL; size_t cap = 0;
    while (cu_slurp_line(f, &line, &cap)) {
        if (!*line) continue;
        if (nv == cv) { cv = cv ? cv * 2 : 1024; v = realloc(v, sizeof *v * (size_t)cv); }
        CuPk *k = &v[nv++]; memset(k, 0, sizeof *k);
        k->line = strdup(line); k->len = strlen(line);
        { const char *u0 = strstr(line, "{\"role\": \"user\""); if (u0) k->len -= (size_t)(u0 - line); }      /* o prompt de sistema nao conta no tamanho: senao as conversas com o prompt inteiro nunca seriam escolhidas */
        const char *fam = cu_jstr(cu_jget(line, "family")), *cell = cu_jstr(cu_jget(line, "cell"));
        uint64_t h = 1469598103934665603ull;      /* a sequencia nao leva a familia: duas familias com as mesmas chamadas contam como uma sequencia so */
        k->dep = strstr(line, "\"dep\": 1, \"scn\"") != NULL; k->real = strstr(line, "\"mode\": 2, \"decl\"") != NULL;
        for (const char *q = line; *q; ) {
            const char *a = strstr(q, "\"function\": {\"name\": \""), *u = strstr(q, "{\"role\": \"user\"");
            if (!a && !u) break;
            if (u && (!a || u < a)) { h = cu_fnv("|", 1, h); q = u + 15; continue; }
            a += 22;
            const size_t l = strcspn(a, "\""); char nm[96]; snprintf(nm, sizeof nm, "%.*s", (int)(l < 95 ? l : 95), a);
            const char *shown = strstr(nm, "__") ? "mcp" : nm;
            h = cu_fnv(shown, strlen(shown), h); h = cu_fnv(">", 1, h);
            for (int ci = 0; ci < CU_N(g_cu_cov); ci++) if (!strcmp(nm, g_cu_cov[ci].name)) k->tools |= 1ull << ci;
            if (!strcmp(nm, "AskUserQuestion")) k->ask = 1;
            q = a + l;
        }
        if (fam && !strcmp(fam, "x-ps1")) k->ps1 = 1;
        k->fam = fam ? cu_fam_ix(fam) : -1; k->mcp = strstr(line, "__") && strstr(line, "[external content from MCP tool ") != NULL;
        k->sig = h; k->cell = cell && *cell ? cu_fnv(cell, strlen(cell), 99) : 0;
        {   const char *a1 = strstr(line, "\"function\": {\"name\": \""); const size_t l1 = a1 ? strcspn(a1 + 22, "\"") : 0;      /* familia + primeira ferramenta */
            k->ffirst = cu_fnv(a1 ? a1 + 22 : "(answer)", a1 ? l1 : 8, cu_fnv(fam ? fam : "", fam ? strlen(fam) : 0, 7)); }
        cu_arena_free();
    }
    fclose(f); free(line);
    uint64_t *sigs = calloc((size_t)nv + 1, sizeof *sigs), *cells = calloc((size_t)nv + 1, sizeof *cells); long nsig = 0, ncell = 0, taken = 0, nask = 0, ndep = 0, nreal = 0;
    long *sigc = calloc((size_t)nv + 1, sizeof *sigc); long sigcap = n / 110 > 3 ? n / 110 : 3;      /* teto de conversas por sequencia: as 20 mais comuns ficam abaixo de um quarto do arquivo */
    const long depmin = n * 22 / 100, realmin = n * 10 / 100 + 1, realmax = n * 12 / 100;      /* o prompt inteiro custa umas cinco janelas por conversa: fica perto do minimo pedido (10%) */
    long cnt[CU_N(g_cu_cov)]; memset(cnt, 0, sizeof cnt);
    const long askmin = n * 45 / 1000 > 1 ? n * 45 / 1000 : 1, askmax = n * 5 / 100 > askmin ? n * 5 / 100 : askmin;
#define PK_SEEN(arr, cnt_, x) ({ int s_ = 0; for (long z_ = 0; z_ < cnt_; z_++) if (arr[z_] == (x)) { s_ = 1; break; } s_; })
#define PK_SIGN(k) ({ long c_ = 0; for (long z_ = 0; z_ < nsig; z_++) if (sigs[z_] == (k)->sig) { c_ = sigc[z_]; break; } c_; })
#define PK_TAKE(k) do { (k)->taken = 1; taken++; nask += (k)->ask; ndep += (k)->dep; nreal += (k)->real; if (!PK_SEEN(sigs, nsig, (k)->sig)) sigs[nsig++] = (k)->sig; for (long z_ = 0; z_ < nsig; z_++) if (sigs[z_] == (k)->sig) sigc[z_]++; if ((k)->cell && !PK_SEEN(cells, ncell, (k)->cell)) cells[ncell++] = (k)->cell; \
        for (int c_ = 0; c_ < CU_N(g_cu_cov); c_++) if ((k)->tools & (1ull << c_)) cnt[c_]++; } while (0)
#define PK_OK2(k) (!(k)->taken && taken < n && !((k)->ask && nask >= askmax) && !((k)->real && nreal >= realmax))      /* sem o teto por sequencia: para os minimos */
#define PK_OK(k) (PK_OK2(k) && PK_SIGN(k) < sigcap)
    CuPk **bylen = malloc(sizeof *bylen * (size_t)(nv + 1));
    for (long i = 0; i < nv; i++) bylen[i] = &v[i];
    qsort(bylen, (size_t)nv, sizeof *bylen, cu_pk_cmp);
    /* (0) cotas: situacoes das familias de tabela, formas de pedido, segundo pedido dependente, prompt inteiro. Vem antes dos minimos por
       ferramenta porque esses, sozinhos, encheriam um arquivo pequeno */
    for (int pass = 0; pass < 2; pass++) for (long i = 0; i < nv && nreal < realmin; i++) { CuPk *k = bylen[i]; if (k->real && PK_OK(k) && (pass || !PK_SEEN(sigs, nsig, k->sig))) PK_TAKE(k); }   /* prompt inteiro: 11% a 14% */
    {   uint64_t *ff = calloc((size_t)nv + 1, sizeof *ff); long nff = 0;      /* cada familia com cada primeira ferramenta que a reserva tem */
        for (long i = 0; i < nv; i++) { CuPk *k = bylen[i]; if (PK_SEEN(ff, nff, k->ffirst)) continue; if (k->taken || PK_OK(k)) { if (!k->taken) PK_TAKE(k); ff[nff++] = k->ffirst; } }
        free(ff); }
    for (long i = 0; i < nv; i++) {      /* cada situacao (celula) das ferramentas raras, dos scripts .ps1 e dos conceitos do katana entra pelo menos uma vez */
        CuPk *k = bylen[i];
        if (!k->cell || !PK_OK2(k) || PK_SEEN(cells, ncell, k->cell)) continue;
        if (strstr(k->line, "\"family\": \"x-rare\"") || strstr(k->line, "\"family\": \"x-ps1\"") || strstr(k->line, "\"family\": \"x-concept\"")) PK_TAKE(k);
    }
    {   /* cada forma de pedido com um minimo (% do arquivo): as formas raras nao podem sumir na escolha */
        static const struct { const char *tag; int pct; } shp[] = {{"two", 5}, {"wrong", 6}, {"paste", 4}, {"goal", 4}, {"midway", 2}, {"typo", 8}, {"mix", 6}, {"ctx", 12}, {"polite", 8}, {"blunt", 6}, {"formal", 5}};
        for (int si = 0; si < CU_N(shp); si++) {
            long have = 0; const long need = n * shp[si].pct / 100;
#define PK_HAS(k) ({ int h_ = 0; const char *sh_ = strstr((k)->line, "\"shape\": \""); if (sh_) { sh_ += 10; const char *e_ = strchr(sh_, '"'); for (const char *q_ = sh_; e_ && (q_ = strstr(q_, shp[si].tag)) && q_ < e_; q_++) if ((q_ == sh_ || q_[-1] == '+') && (q_[strlen(shp[si].tag)] == '+' || q_[strlen(shp[si].tag)] == '"')) { h_ = 1; break; } } h_; })
            for (long i = 0; i < nv; i++) if (v[i].taken && PK_HAS(&v[i])) have++;
            for (int pass = 0; pass < 2; pass++) for (long i = 0; i < nv && have < need; i++) { CuPk *k = bylen[i]; if (PK_OK(k) && (pass || !PK_SEEN(sigs, nsig, k->sig)) && PK_HAS(k)) { PK_TAKE(k); have++; } }
        }
    }
    for (int ci = 0; ci < CU_N(g_cu_cov); ci++) {      /* (1) minimos por ferramenta: primeiro com sequencia nova, depois qualquer uma */
        const int isask = !strcmp(g_cu_cov[ci].name, "AskUserQuestion");
        const long need = isask ? askmin : g_cu_cov[ci].tier == 'A' ? 25 : 5;
        for (int pass = 0; pass < 2; pass++) for (long i = 0; i < nv && cnt[ci] < need; i++) {
            CuPk *k = bylen[i];
            if (!(k->tools & (1ull << ci)) || !PK_OK2(k) || (!pass && PK_SEEN(sigs, nsig, k->sig))) continue;
            PK_TAKE(k);
        }
    }
    { long np = 0; for (long i = 0; i < nv && np < 15; i++) { CuPk *k = bylen[i]; if (k->ps1 && k->taken) np++; }
      for (long i = 0; i < nv && np < 15; i++) { CuPk *k = &v[i]; if (k->ps1 && PK_OK2(k)) { PK_TAKE(k); np++; } } }      /* 15 conversas de scripts .ps1 */
    { long nm = 0; for (long i = 0; i < nv; i++) nm += v[i].taken && v[i].mcp;
      for (long i = 0; i < nv && nm < 25; i++) { CuPk *k = &v[i]; if (k->mcp && PK_OK(k)) { PK_TAKE(k); nm++; } } }      /* 25 conversas com ferramentas MCP */
    for (int fi = 0; fi < CU_NFAM; fi++) {      /* cada familia da reserva com pelo menos 8 */
        long nf = 0; for (long i = 0; i < nv; i++) nf += v[i].taken && v[i].fam == fi;
        for (long i = 0; i < nv && nf < 8; i++) { CuPk *k = &v[i]; if (k->fam == fi && PK_OK2(k)) { PK_TAKE(k); nf++; } }
    }
    for (int pass = 0; pass < 2; pass++) for (long i = 0; i < nv && ndep < depmin; i++) { CuPk *k = bylen[i]; if (k->dep && PK_OK(k) && (pass || !PK_SEEN(sigs, nsig, k->sig))) PK_TAKE(k); }      /* segundo pedido que depende do primeiro: 22% */
    for (long i = 0; i < nv; i++) { CuPk *k = bylen[i]; if (PK_OK(k) && !PK_SEEN(sigs, nsig, k->sig)) PK_TAKE(k); }                 /* (2) */
    for (long i = 0; i < nv; i++) { CuPk *k = bylen[i]; if (PK_OK(k) && k->cell && !PK_SEEN(cells, ncell, k->cell)) PK_TAKE(k); }   /* (3) */
    for (; taken < n && sigcap < n; sigcap++) for (long i = 0; i < nv; i++) { CuPk *k = &v[i]; if (PK_OK(k)) PK_TAKE(k); }           /* (4): o teto so sobe se a reserva nao encher o arquivo */
    FILE *o = fopen(argv[1], "wb");
    if (!o) { fprintf(stderr, "curriculum pick: nao gravei %s\n", argv[1]); return 1; }
    for (long i = 0; i < nv; i++) if (v[i].taken) { fputs(v[i].line, o); fputc('\n', o); }
    fclose(o);
    printf("curriculum pick: %ld de %ld conversas em %s; %ld sequencias de ferramentas (teto %ld por sequencia), %ld celulas, %ld com AskUserQuestion, %ld com segundo pedido dependente, %ld com o prompt inteiro\n", taken, nv, argv[1], nsig, sigcap, ncell, nask, ndep, nreal);
    return 0;
}
