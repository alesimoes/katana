/* ---------- docsel: o motor escolhe, nos .md que o KATANA.md aponta, as linhas que importam para o pedido ----------
 * O KATANA.md aponta (bloco docs:) para documentos; cada documento pode apontar para outros (link markdown ou caminho .md na linha).
 * Os documentos funcionam como indices: um grafo pequeno. A cada pedido o motor carrega o grafo (ate DS_MAXDOC arquivos e tres
 * saltos), tokeniza cada linha e o pedido com o tokenizador do proprio modelo, pesa os tokens pela raridade no grafo (idf) e entrega
 * ao agente so as linhas que casam, com arquivo e numero da linha. Uma linha que aponta para outro documento e casa com o pedido
 * da relevancia ao documento apontado. Nada aqui usa o modelo para escolher: sao milissegundos de CPU.
 * KATANA_DOCSEL=0 desliga; KATANA_DOCSEL_TOKENS muda o orcamento (padrao 600 tokens, contados como 4 bytes cada). */

#define DS_MAXDOC 12
#define DS_MAXLN 2400
#define DS_MAXDEPTH 3
typedef struct { int doc, no; char *orig; int *ids; int nid; int ptr; double sc; int mc; } DsLine;
typedef struct { char rel[500]; int depth, root; } DsDoc;      /* root: arquivo do projeto (KATANA.md, AGENTS.md): ja esta no prompt, so aponta */
typedef struct { DsDoc d[DS_MAXDOC]; int nd; DsLine *l; int nl; } DsGraph;

static char *ds_read(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 200000) { fclose(f); return NULL; }
    char *b = malloc((size_t)st.st_size + 1);
    const size_t n = b ? fread(b, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    if (!b) return NULL;
    b[n] = 0;
    return b;
}
/* o texto vira ids distintos do tokenizador do modelo (minusculo; so pedacos com 3 ou mais letras/digitos contam) */
static int ds_ids(const Tokenizer *tk, const char *txt, int **out) {
    char *low = strdup(txt);
    for (char *p = low; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    Ids ids = {0};
    encode(tk, low, &ids);
    free(low);
    int *o = malloc(sizeof(int) * (size_t)(ids.n + 1)), n = 0;
    for (int i = 0; i < ids.n; i++) {
        const int id = ids.ids[i];
        if (id < 0 || id >= tk->n) continue;
        int al = 0; for (int k = 0; k < tk->toklen[id]; k++) al += isalnum((unsigned char)tk->tok[id][k]) || (unsigned char)tk->tok[id][k] >= 0x80;
        if (al < 3) continue;
        int dup = 0; for (int k = 0; k < n && !dup; k++) dup = o[k] == id;
        if (!dup) o[n++] = id;
    }
    free(ids.ids);
    *out = o;
    return n;
}
/* caminho .md numa linha: [texto](caminho.md) ou palavra que termina em .md; devolve o caminho dentro do projeto ou NULL */
static int ds_find_ptr(const DsGraph *g, int doc, const char *root, const char *line, char *rel, size_t cap) {
    for (const char *p = line; *p; p++) {
        if (!(p[0] == '.' && p[1] == 'm' && p[2] == 'd' && !isalnum((unsigned char)p[3]) && p[3] != '_')) continue;
        const char *s = p; while (s > line && !strchr(" \t()[]`\"'<>,;", s[-1])) s--;
        char cand[500]; snprintf(cand, sizeof cand, "%.*s", (int)(p + 3 - s), s);
        if (cand[0] == '/' || strstr(cand, "://") || strstr(cand, "..")) continue;
        char t[1200], rp[PATH_MAX], rr[PATH_MAX];
        const char *tries[2]; char near[700] = "";
        if (g->d[doc].rel[0]) { snprintf(near, sizeof near, "%s", g->d[doc].rel); char *sl = strrchr(near, '/'); if (sl) { sl[1] = 0; strncat(near, cand, sizeof near - strlen(near) - 1); } else near[0] = 0; }
        tries[0] = near[0] ? near : cand; tries[1] = cand;
        for (int k = 0; k < 2; k++) {
            snprintf(t, sizeof t, "%s/%s", root, tries[k]);
            if (!realpath(t, rp) || !realpath(root, rr)) continue;
            const size_t rl = strlen(rr);
            if (strncmp(rp, rr, rl) || rp[rl] != '/') continue;
            snprintf(rel, cap, "%s", rp + rl + 1);
            return 1;
        }
    }
    return 0;
}
static int ds_doc_index(DsGraph *g, const char *rel, int depth) {
    for (int i = 0; i < g->nd; i++) if (!strcmp(g->d[i].rel, rel)) return i;
    if (g->nd >= DS_MAXDOC || depth > DS_MAXDEPTH) return -1;
    snprintf(g->d[g->nd].rel, sizeof g->d[g->nd].rel, "%s", rel); g->d[g->nd].depth = depth;
    return g->nd++;
}
static void ds_free(DsGraph *g) { for (int i = 0; i < g->nl; i++) { free(g->l[i].orig); free(g->l[i].ids); } free(g->l); g->l = NULL; g->nl = 0; }
static void ds_add(DsGraph *g, const Tokenizer *tk, int doc, int no, const char *txt, int ptr, const char *head) {
    if (g->nl >= DS_MAXLN) return;
    if (!g->l) g->l = calloc(DS_MAXLN, sizeof(DsLine));
    DsLine *L = &g->l[g->nl++];
    L->doc = doc; L->no = no; L->orig = strdup(txt); L->ptr = ptr;
    char *both = head && head[0] ? malloc(strlen(txt) + strlen(head) + 2) : NULL;      /* o titulo da secao conta para o casamento */
    if (both) sprintf(both, "%s %s", head, txt);
    L->nid = ds_ids(tk, both ? both : txt, &L->ids);
    free(both);
}
/* carrega o grafo: as entradas do docs: do KATANA.md sao as raizes; cada documento lido pode apontar para mais */
static void ds_load(DsGraph *g, const Tokenizer *tk, const Brief *b, const char *root) {
    memset(g, 0, sizeof *g);
    static const char *const proj[] = {"KATANA.md", "AGENTS.md"};
    for (int k = 0; k < 2; k++) {
        char t[1200]; snprintf(t, sizeof t, "%s/%s", root, proj[k]);
        if (access(t, R_OK) == 0) { const int d = ds_doc_index(g, proj[k], 0); if (d >= 0) g->d[d].root = 1; }
    }
    for (int i = 0; i < b->ni; i++) {
        if (b->it[i].blk != BRB_DOCS) continue;
        char path[500]; snprintf(path, sizeof path, "%.*s", (int)strcspn(b->it[i].text, " \t"), b->it[i].text);
        char rel[PATH_MAX], t[1200];
        snprintf(t, sizeof t, "%s/%s", root, path);
        char rp[PATH_MAX], rr[PATH_MAX];
        if (path[0] == '/' || strstr(path, "..") || !realpath(t, rp) || !realpath(root, rr) || strncmp(rp, rr, strlen(rr)) || rp[strlen(rr)] != '/') continue;
        snprintf(rel, sizeof rel, "%s", rp + strlen(rr) + 1);
        const int d = ds_doc_index(g, rel, 1);
        if (d >= 0) ds_add(g, tk, -1, i + 1, b->it[i].text, d, NULL);
    }
    for (int d = 0; d < g->nd; d++) {
        char t[1200]; snprintf(t, sizeof t, "%s/%s", root, g->d[d].rel);
        char *txt = ds_read(t);
        if (!txt) continue;
        { char nm[600]; snprintf(nm, sizeof nm, "%s", g->d[d].rel); for (char *c = nm; *c; c++) if (*c == '/' || *c == '-' || *c == '_' || *c == '.') *c = ' ';
          ds_add(g, tk, d, 0, nm, -1, NULL); }      /* o nome do arquivo tambem diz do que o documento trata */
        int no = 0; char head[200] = "";
        for (char *ln = txt; ln && *ln; ) {
            char *e = strchr(ln, '\n'); if (e) *e = 0;
            no++;
            const char *s = ln; while (*s == ' ' || *s == '\t') s++;
            if (*s == '#') { const char *h = s; while (*h == '#') h++; while (*h == ' ') h++; snprintf(head, sizeof head, "%s", h); }
            if (*s && strlen(s) > 3 && strncmp(s, "```", 3)) {
                char rel[PATH_MAX]; int ptr = -1;
                if (ds_find_ptr(g, d, root, s, rel, sizeof rel)) ptr = ds_doc_index(g, rel, g->d[d].depth + 1);
                ds_add(g, tk, d, no, s, ptr, *s == '#' ? NULL : head);
            }
            ln = e ? e + 1 : NULL;
        }
        free(txt);
    }
}
/* escolhe as linhas e monta a nota; devolve quantas linhas entraram (0 = nada que valha mostrar) */
static int ds_select(const Tokenizer *tk, const char *req, const Brief *b, const char *root, char *out, size_t cap) {
    out[0] = 0;
    const char *e = getenv("KATANA_DOCSEL");
    if (e && !atoi(e)) return 0;
    int docs = 0; for (int i = 0; i < b->ni; i++) docs += b->it[i].blk == BRB_DOCS;
    { char t[1200]; int any = docs; static const char *const pf[] = {"KATANA.md", "AGENTS.md"};
      for (int k = 0; k < 2 && !any; k++) { snprintf(t, sizeof t, "%s/%s", root, pf[k]); any = access(t, R_OK) == 0; }
      if (!any || strlen(req) < 12) return 0; }
    DsGraph g; ds_load(&g, tk, b, root);
    int *df = calloc((size_t)tk->n, sizeof(int));
    for (int i = 0; i < g.nl; i++) for (int k = 0; k < g.l[i].nid; k++) df[g.l[i].ids[k]]++;
    char rq[2100]; snprintf(rq, sizeof rq, "%s", req);
    int *qi, nq = ds_ids(tk, rq, &qi);
    double idf[DS_MAXLN > 0 ? 400 : 1]; int qn = 0, qid[400];
    for (int k = 0; k < nq && qn < 400; k++) {
        const int d = df[qi[k]];
        if (!d || (g.nl >= 10 && d * 10 > g.nl * 3)) continue;      /* ausente do grafo ou comum demais (mais de 30% das linhas) */
        qid[qn] = qi[k]; idf[qn++] = log(1.0 + (double)g.nl / (d + 0.5));
    }
    for (int i = 0; i < g.nl; i++) {
        DsLine *L = &g.l[i];
        for (int k = 0; k < qn; k++) for (int j = 0; j < L->nid; j++) if (L->ids[j] == qid[k]) { L->sc += idf[k]; L->mc++; break; }
    }
    double pa[DS_MAXDOC] = {0}, nm[DS_MAXDOC] = {0};
    double act[DS_MAXDOC] = {0};      /* relevancia do documento: uma linha que casa e aponta para ele, ou o nome do arquivo casa com o pedido */
    for (int i = 0; i < g.nl; i++) {
        const DsLine *L = &g.l[i];
        if (L->ptr >= 0 && L->sc > pa[L->ptr]) pa[L->ptr] = L->sc;
        if (L->doc >= 0 && L->no == 0 && L->sc > nm[L->doc]) nm[L->doc] = L->sc;
    }
    for (int d = 0; d < g.nd; d++) act[d] = pa[d] + 1.5 * nm[d];      /* o indice que descreve a tela e o nome do arquivo somam: o nome da tela pesa mais que uma palavra rara de outra */
    { double mx = 0; for (int d = 0; d < g.nd; d++) if (!g.d[d].root && act[d] > mx) mx = act[d];      /* so os documentos que mais casam contam: o resto e ruido */
      for (int d = 0; d < g.nd; d++) if (act[d] < 0.6 * mx) act[d] = 0; }
    int nptr[DS_MAXDOC] = {0};      /* documento com varias linhas que apontam e um indice: dele so entram as linhas que casam */
    for (int i = 0; i < g.nl; i++) if (g.l[i].doc >= 0 && g.l[i].ptr >= 0) nptr[g.l[i].doc]++;
    size_t dbytes[DS_MAXDOC] = {0};
    for (int i = 0; i < g.nl; i++) if (g.l[i].doc >= 0 && g.l[i].no > 0) dbytes[g.l[i].doc] += strlen(g.l[i].orig) + 1;
    size_t budget = 600 * 4; { const char *bt = getenv("KATANA_DOCSEL_TOKENS"); if (bt && atoi(bt) > 0) budget = (size_t)atoi(bt) * 4; }
    /* blocos a entregar: um documento inteiro (a tela, o modulo que o pedido nomeia) quando cabe, ou as secoes que mais casam */
    typedef struct { int doc, a, b; double sc; int whole; } Blk;
    Blk blk[6]; int nb = 0;
    int wd = -1;
    for (int d = 0; d < g.nd; d++) if (!g.d[d].root && nptr[d] < 3 && act[d] >= 2.5 && dbytes[d] > 0 && dbytes[d] <= budget && (wd < 0 || act[d] > act[wd])) wd = d;
    if (wd >= 0) for (int d = 0; d < g.nd; d++) if (d != wd && !g.d[d].root && nptr[d] < 3 && act[d] * 1.25 > act[wd]) wd = -1;      /* dois documentos empatam: nao da para dizer qual e a tela */
    size_t left = budget;
    if (wd >= 0) { int a = -1, z = -1; for (int i = 0; i < g.nl; i++) if (g.l[i].doc == wd && g.l[i].no > 0) { if (a < 0) a = i; z = i + 1; }
        blk[nb++] = (Blk){wd, a, z, act[wd], 1}; left -= dbytes[wd]; }
    Blk cand[64]; int nc = 0;
    for (int i = 0; i < g.nl && nc < 64 && wd < 0; ) {      /* achou o documento da tela: so ele; senao, as secoes que mais casam */
        const DsLine *L = &g.l[i];
        if (L->doc < 0 || L->no == 0 || g.d[L->doc].root || nptr[L->doc] >= 3) { i++; continue; }      /* indices nao entram: servem para achar o documento */
        int z = i + 1; while (z < g.nl && g.l[z].doc == L->doc && strncmp(g.l[z].orig, "##", 2)) z++;      /* o titulo (#) fica na primeira secao */
        double mx = 0, sm = 0; int ok = 0;
        for (int k = i; k < z; k++) {
            const DsLine *M = &g.l[k];
            sm += M->sc; if (M->sc > mx) mx = M->sc;
            ok |= (M->mc >= 2 && M->sc >= 2.5) || (M->mc >= 1 && M->sc >= 3.5) || (M->mc >= 1 && act[L->doc] >= 3.0 && M->sc >= 1.5);
        }
        if (act[L->doc] <= 0 && mx < 5.0) ok = 0;      /* fora dos documentos que casam, so uma linha muito forte vale */
        if (ok) cand[nc++] = (Blk){L->doc, i, z, mx + 0.3 * (sm - mx) + 0.5 * act[L->doc], 0};
        i = z;
    }
    for (int r = 0; r < 3 && nb < 6 && left > 120; r++) {
        int best = -1; for (int c = 0; c < nc; c++) if (cand[c].sc > 0 && (best < 0 || cand[c].sc > cand[best].sc)) best = c;
        if (best < 0) break;
        Blk x = cand[best]; cand[best].sc = 0;
        size_t sz = 0; for (int k = x.a; k < x.b; k++) sz += strlen(g.l[k].orig) + 1;
        if (sz > left) { size_t acc = 0; int k = x.a; for (; k < x.b; k++) { acc += strlen(g.l[k].orig) + 1; if (acc > left) break; } x.b = k; sz = acc > left ? left : acc; }
        if (x.b <= x.a) continue;
        blk[nb++] = x; left -= sz < left ? sz : left;
    }
    int n = 0;
    if (nb) {
        for (int a = 0; a < nb; a++) for (int c = a + 1; c < nb; c++)      /* ordem do grafo: documento, depois posicao */
            if (blk[a].doc > blk[c].doc || (blk[a].doc == blk[c].doc && blk[a].a > blk[c].a)) { const Blk t = blk[a]; blk[a] = blk[c]; blk[c] = t; }
        size_t o = (size_t)snprintf(out, cap, "[katana: the engine picked this from the project documents that KATANA.md points to, because it matches this request. "
                                              "It is a selection: open the file if you need more.]");
        for (int a = 0; a < nb && o + 400 < cap; a++) {
            const Blk *B = &blk[a];
            o += (size_t)snprintf(out + o, cap - o, "\n== %s:%d-%d%s", g.d[B->doc].rel, g.l[B->a].no, g.l[B->b - 1].no, B->whole ? " (the whole document)" : "");
            for (int k = B->a; k < B->b && o + 300 < cap; k++) {
                if (g.l[k].ptr >= 0 && g.l[k].sc <= 0 && (nptr[B->doc] >= 3 || !B->whole)) continue;      /* linha de indice que nao casa */
                if (nptr[B->doc] >= 3 && g.l[k].ptr < 0) continue;
                o += (size_t)snprintf(out + o, cap - o, "\n%.300s", g.l[k].orig);
            }
            n++;
        }
    }
    free(qi); free(df); ds_free(&g);
    return n;
}
