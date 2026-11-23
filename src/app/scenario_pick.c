/* ---------- katana curriculum pick: a escolha das conversas por cobertura ---------- */
/* katana curriculum pick <reserva.jsonl> <saida.jsonl> [--n N]: escolhe conversas por cobertura, nao por contagem.
   Ordem: (1) cada ferramenta da tabela de cobertura ate o seu minimo, 15 conversas de .ps1, 25 com ferramentas MCP e 8 de cada familia; (2) uma conversa de cada sequencia de ferramentas
   (familia + chamadas na ordem, com os pedidos), das menores para as maiores; (3) celulas ainda nao vistas; (4) o resto, na
   ordem do arquivo. AskUserQuestion fica entre 4,5% e 5% das escolhidas. A saida mantem a ordem da reserva. */
typedef struct { char *line; uint64_t sig, cell; unsigned long long tools; int ask, taken, ps1, mcp, fam, dep, real, blk; uint64_t ffirst; size_t len; } CuPk;
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
        k->fam = fam ? cu_fam_ix(fam) : -1; k->blk = cell && strstr(cell, "/blk1") != NULL; k->mcp = strstr(line, "__") && strstr(line, "[external content from MCP tool ") != NULL;
        k->sig = h; k->cell = cell && *cell ? cu_fnv(cell, strlen(cell), 99) : 0;
        {   const char *a1 = strstr(line, "\"function\": {\"name\": \""); const size_t l1 = a1 ? strcspn(a1 + 22, "\"") : 0;      /* familia + primeira ferramenta */
            k->ffirst = cu_fnv(a1 ? a1 + 22 : "(answer)", a1 ? l1 : 8, cu_fnv(fam ? fam : "", fam ? strlen(fam) : 0, 7)); }
        cu_arena_free();
    }
    fclose(f); free(line);
    uint64_t *sigs = calloc((size_t)nv + 1, sizeof *sigs), *cells = calloc((size_t)nv + 1, sizeof *cells); long nsig = 0, ncell = 0, taken = 0, nask = 0, ndep = 0, nreal = 0;
    long *sigc = calloc((size_t)nv + 1, sizeof *sigc); long sigcap = n / 150 > 3 ? n / 150 : 3;      /* teto de conversas por sequencia: as 20 mais comuns ficam abaixo de um quarto do arquivo */
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
    /* A ordem da escolha. Cada passo prefere uma conversa cuja sequencia de ferramentas ainda nao entrou; o teto por sequencia (sigcap)
       so cede quando um minimo nao fecha sem isso.
       (a) prompt inteiro ate 10%; (b) cada familia com cada primeira ferramenta que a reserva tem;
       (c) situacoes: cada ferramenta rara com cinco situacoes distintas, cada conceito do katana com tres, cada situacao de .ps1 uma vez;
       (d) minimos das ferramentas raras (5), 15 conversas de .ps1, 25 com MCP, 8 por familia;
       (e) segundo pedido que depende do primeiro ate 22%; (f) minimo de cada forma de pedido;
       (g) uma conversa de cada sequencia ainda nao vista, das menores para as maiores; (h) celulas nao vistas;
       (i) minimos das ferramentas declaradas (25) e de AskUserQuestion; (j) o resto, com o teto subindo so se faltar conversa. */
#define PK_NEW(k) (!PK_SEEN(sigs, nsig, (k)->sig))
    for (int pass = 0; pass < 2; pass++) for (long i = 0; i < nv && nreal < realmin; i++) { CuPk *k = bylen[i]; if (k->real && PK_OK(k) && (pass || PK_NEW(k))) PK_TAKE(k); }      /* (a) */
    {   uint64_t *ff = calloc((size_t)nv + 1, sizeof *ff); long nff = 0;      /* (b) */
        for (long i = 0; i < nv; i++) { CuPk *k = bylen[i]; if (PK_SEEN(ff, nff, k->ffirst)) continue; if (k->taken || PK_OK(k)) { if (!k->taken) PK_TAKE(k); ff[nff++] = k->ffirst; } }
        free(ff); }
    {   uint64_t conh[64], psh[64]; long conc[64], ncon = 0, nps = 0;      /* (c) */
        for (int pass = 0; pass < 3; pass++) for (long i = 0; i < nv; i++) {      /* passo 0: sequencia nova; 1: dentro do teto; 2: qualquer uma */
            CuPk *k = bylen[i];
            if (!k->cell || PK_SEEN(cells, ncell, k->cell) || !(pass == 2 ? PK_OK2(k) : PK_OK(k)) || (!pass && !PK_NEW(k))) continue;
            const char *cc;
            if ((cc = strstr(k->line, "\"cell\": \"ps1/"))) {
                cc += 13; const uint64_t ch = cu_fnv(cc, strcspn(cc, "/\""), 5);
                if (!PK_SEEN(psh, nps, ch) && nps < 64) { psh[nps++] = ch; PK_TAKE(k); }
            } else if (strstr(k->line, "\"family\": \"x-rare\"")) {
                int need = 0; for (int ci = 0; ci < CU_N(g_cu_cov); ci++) if ((k->tools & (1ull << ci)) && g_cu_cov[ci].tier != 'A' && cnt[ci] < 5) need = 1;
                if (need) PK_TAKE(k);
            } else if ((cc = strstr(k->line, "\"cell\": \"concept/"))) {
                cc += 17; const uint64_t ch = cu_fnv(cc, strcspn(cc, "/\""), 5);
                long ix = -1; for (long z = 0; z < ncon; z++) if (conh[z] == ch) ix = z;
                if (ix < 0 && ncon < 64) { ix = ncon; conh[ncon] = ch; conc[ncon++] = 0; }
                if (ix >= 0 && conc[ix] < 3) { PK_TAKE(k); conc[ix]++; }
            }
        }
    }
    for (int ci = 0; ci < CU_N(g_cu_cov); ci++) {      /* (d) */
        if (g_cu_cov[ci].tier == 'A') continue;
        for (int pass = 0; pass < 3; pass++) for (long i = 0; i < nv && cnt[ci] < 5; i++) {
            CuPk *k = bylen[i];
            if (!(k->tools & (1ull << ci)) || !(pass == 2 ? PK_OK2(k) : PK_OK(k)) || (!pass && !PK_NEW(k))) continue;
            PK_TAKE(k);
        }
    }
    { long np = 0; for (long i = 0; i < nv; i++) np += v[i].taken && v[i].ps1;
      for (int pass = 0; pass < 2; pass++) for (long i = 0; i < nv && np < 15; i++) { CuPk *k = bylen[i]; if (k->ps1 && (pass ? PK_OK2(k) : PK_OK(k))) { PK_TAKE(k); np++; } } }
    { long nm = 0; for (long i = 0; i < nv; i++) nm += v[i].taken && v[i].mcp;
      for (int pass = 0; pass < 2; pass++) for (long i = 0; i < nv && nm < 25; i++) { CuPk *k = bylen[i]; if (k->mcp && PK_OK(k) && (pass || PK_NEW(k))) { PK_TAKE(k); nm++; } } }
    for (int fi = 0; fi < CU_NFAM; fi++) {
        long nf = 0; for (long i = 0; i < nv; i++) nf += v[i].taken && v[i].fam == fi;
        for (int pass = 0; pass < 2; pass++) for (long i = 0; i < nv && nf < 8; i++) { CuPk *k = bylen[i]; if (k->fam == fi && PK_OK(k) && (pass || PK_NEW(k))) { PK_TAKE(k); nf++; } }
    }
    {   /* (d2) cota da familia x-ctx: 5% do arquivo, dois tercos com o bloco de contexto e um terco sem (a conversa sem bloco continua valida).
           Sem esta cota o teto por sequencia (poucas sequencias de ferramentas na familia) cortava a familia em ~1% */
        const int cf = cu_fam_ix("x-ctx"); const long need = n * 5 / 100, needb = need * 2 / 3;
        long nc = 0, nb = 0; for (long i = 0; i < nv; i++) if (v[i].taken && v[i].fam == cf) { nc++; nb += v[i].blk; }
        for (int st = 0; st < 3; st++) for (int pass = 0; pass < 2; pass++) for (long i = 0; i < nv; i++) {      /* 0: com bloco ate dois tercos; 1: sem bloco ate a cota; 2: o que faltar */
            CuPk *k = bylen[i]; if (k->fam != cf || k->taken || (st == 0 && (!k->blk || nb >= needb)) || (st == 1 && k->blk) || nc >= need) continue;
            if (!(pass ? PK_OK2(k) : PK_OK(k) && PK_NEW(k))) continue;
            PK_TAKE(k); nc++; nb += k->blk;
        }
    }
    for (int ci = 0; ci < CU_N(g_cu_cov); ci++) {      /* (i) */
        if (g_cu_cov[ci].tier != 'A') continue;
        const int isask = !strcmp(g_cu_cov[ci].name, "AskUserQuestion");
        const long need = isask ? askmin : n >= 500 ? 25 : n * 2 / 100 + 3;      /* o check cobra 2% do arquivo (25 a partir de 500 conversas) */
        for (int pass = 0; pass < 2; pass++) for (long i = 0; i < nv && cnt[ci] < need; i++) { CuPk *k = bylen[i]; if ((k->tools & (1ull << ci)) && (pass ? PK_OK2(k) : PK_OK(k) && PK_NEW(k))) PK_TAKE(k); }
    }
    for (int pass = 0; pass < 2; pass++) for (long i = 0; i < nv && ndep < depmin; i++) { CuPk *k = bylen[i]; if (k->dep && PK_OK(k) && (pass || PK_NEW(k))) PK_TAKE(k); }      /* (e) */
    {   static const struct { const char *tag; int pct; } shp[] = {{"two", 5}, {"wrong", 6}, {"paste", 4}, {"goal", 4}, {"midway", 2}, {"typo", 8}, {"mix", 6}, {"ctx", 12}, {"polite", 8}, {"blunt", 6}, {"formal", 5}};      /* (f) */
        for (int si = 0; si < CU_N(shp); si++) {
            long have = 0; const long need = n * shp[si].pct / 100;
#define PK_HAS(k) ({ int h_ = 0; const char *sh_ = strstr((k)->line, "\"shape\": \""); if (sh_) { sh_ += 10; const char *e_ = strchr(sh_, '"'); for (const char *q_ = sh_; e_ && (q_ = strstr(q_, shp[si].tag)) && q_ < e_; q_++) if ((q_ == sh_ || q_[-1] == '+') && (q_[strlen(shp[si].tag)] == '+' || q_[strlen(shp[si].tag)] == '"')) { h_ = 1; break; } } h_; })
            for (long i = 0; i < nv; i++) if (v[i].taken && PK_HAS(&v[i])) have++;
            for (int pass = 0; pass < 2; pass++) for (long i = 0; i < nv && have < need; i++) { CuPk *k = bylen[i]; if (PK_OK(k) && (pass || PK_NEW(k)) && PK_HAS(k)) { PK_TAKE(k); have++; } }
        }
    }
    for (long i = 0; i < nv; i++) { CuPk *k = bylen[i]; if (PK_OK(k) && PK_NEW(k)) PK_TAKE(k); }                                    /* (g) */
    for (long i = 0; i < nv; i++) { CuPk *k = bylen[i]; if (PK_OK(k) && k->cell && !PK_SEEN(cells, ncell, k->cell)) PK_TAKE(k); }   /* (h) */
    for (; taken < n && sigcap < n; sigcap++) for (long i = 0; i < nv; i++) { CuPk *k = &v[i]; if (PK_OK(k)) PK_TAKE(k); }           /* (j) */
    FILE *o = fopen(argv[1], "wb");
    if (!o) { fprintf(stderr, "curriculum pick: nao gravei %s\n", argv[1]); return 1; }
    for (long i = 0; i < nv; i++) if (v[i].taken) { fputs(v[i].line, o); fputc('\n', o); }
    fclose(o);
    printf("curriculum pick: %ld de %ld conversas em %s; %ld sequencias de ferramentas (teto %ld por sequencia), %ld celulas, %ld com AskUserQuestion, %ld com segundo pedido dependente, %ld com o prompt inteiro\n", taken, nv, argv[1], nsig, sigcap, ncell, nask, ndep, nreal);
    return 0;
}
