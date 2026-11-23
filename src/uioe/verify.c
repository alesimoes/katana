/* ---------- uioe/verify.c: a verificacao (docs/uioe.md 3.6.1, fase U3) ----------
 * Tudo que vai para a saida passa por aqui: o arquivo e lido de volta e o que o no afirma tem de ocorrer na linha citada (mais ou menos
 * 3 linhas, a tolerancia de sy_verify). Resultados:
 *   UV_OK          a linha diz o que o no diz; +4 na confianca quando o conector ainda nao tinha reconferido (cver = 0)
 *   UV_MOVED       a linha andou (o arquivo foi editado depois do indice): a linha e corrigida e a nota diz quantas linhas
 *   UV_STALE       o texto nao esta mais no arquivo (nem em outra linha dele): -25, "line not verified: the file changed since it was indexed"
 *   UV_GONE        o arquivo nao existe mais; UV_UNREADABLE: nao abre. O no e descartado da saida (dropped) e vira uma linha de limites.
 * "Arquivo mudado": tamanho e mtime contra o registro do indice (a visao aberta), quando ha visao; sem indice nao se sabe e a nota diz isso.
 * Limites do documento: no maximo 40 faixas relidas, cada uma ate 200 linhas, em 150 ms; o resto sai "not reread".
 * Decisoes do implementador: a leitura e propria (um unico arquivo em cache por UioeCtx), nao a da visao, para o conector e a
 * verificacao serem independentes; nos de memoria, web e historico (U8/U9) nao sao verificaveis e nao passam por aqui. */
static int uioe_secret_path(const char *rel);             /* conn_file.c */
static void uioe_mask_line(char *s);
#define UV_MAXRANGES 40
#define UV_MS 150
static void uv_free(UioeCtx *cx) { free(cx->vbuf); free(cx->vln); cx->vbuf = NULL; cx->vln = NULL; cx->vpath[0] = 0; cx->vn = 0; cx->vnl = 0; cx->vok = 0; }
/* carrega raiz/rel; 1 ok, 0 nao existe, -1 nao abre/grande demais */
static int uv_load(UioeCtx *cx, const char *rel) {
    if (cx->vpath[0] && !strcmp(cx->vpath, rel)) return cx->vok;
    uv_free(cx); snprintf(cx->vpath, sizeof cx->vpath, "%s", rel);
    char full[2600]; if (rel[0] == '/') snprintf(full, sizeof full, "%s", rel); else snprintf(full, sizeof full, "%s/%s", cx->root, rel);
    struct stat st;
    if (stat(full, &st) != 0) { cx->vok = 0; return 0; }
    cx->vsize = (uint64_t)st.st_size;
#ifdef _WIN32
    cx->vmtime = (int64_t)st.st_mtime * 1000000000ll;
#else
    cx->vmtime = (int64_t)st.st_mtim.tv_sec * 1000000000ll + st.st_mtim.tv_nsec;
#endif
    if (!S_ISREG(st.st_mode) || st.st_size > (8 << 20)) { cx->vok = -1; return -1; }
    FILE *f = fopen(full, "rb"); if (!f) { cx->vok = -1; return -1; }
    cx->vbuf = (char *)malloc((size_t)st.st_size + 1);
    const size_t got = cx->vbuf ? fread(cx->vbuf, 1, (size_t)st.st_size, f) : 0; fclose(f);
    if (!cx->vbuf) { cx->vok = -1; return -1; }
    cx->vn = got; cx->vbuf[got] = 0;
    int nl = 1; for (size_t i = 0; i < got; i++) if (cx->vbuf[i] == '\n') nl++;
    cx->vln = (size_t *)malloc(sizeof(size_t) * (size_t)nl); if (!cx->vln) { cx->vok = -1; return -1; }
    int q = 0; cx->vln[q++] = 0; for (size_t i = 0; i < got; i++) if (cx->vbuf[i] == '\n') cx->vln[q++] = i + 1;
    cx->vnl = nl; cx->vok = 1; return 1;
}
/* texto da linha (1-based), sem espacos nas pontas e sem o fim de linha */
static int uv_line(const UioeCtx *cx, int line, char *o, size_t cap) {
    o[0] = 0; if (cx->vok != 1 || line < 1 || line > cx->vnl) return 0;
    size_t a = cx->vln[line - 1], b = a; while (b < cx->vn && cx->vbuf[b] != '\n' && cx->vbuf[b] != '\r') b++;
    while (a < b && (cx->vbuf[a] == ' ' || cx->vbuf[a] == '\t')) a++;
    while (b > a && (cx->vbuf[b - 1] == ' ' || cx->vbuf[b - 1] == '\t')) b--;
    size_t l = b - a; if (l + 1 > cap) { l = cap - 1; while (l > 0 && ((unsigned char)cx->vbuf[a + l] & 0xC0) == 0x80) l--; }
    memcpy(o, cx->vbuf + a, l); o[l] = 0; return 1;
}
static int uv_isword(const char *s) { for (; *s; s++) if (!(isalnum((unsigned char)*s) || *s == '_')) return 0; return 1; }
/* a linha contem a chave: como palavra inteira se a chave e um identificador, senao como texto */
static int uv_hit(const char *line, const char *key) {
    const size_t kl = strlen(key); if (!kl) return 1;
    const int word = uv_isword(key);
    for (const char *p = line; (p = strstr(p, key)); p++) {
        if (!word) return 1;
        const int pre = p == line || !(isalnum((unsigned char)p[-1]) || p[-1] == '_'), post = !(isalnum((unsigned char)p[kl]) || p[kl] == '_');
        if (pre && post) return 1;
    }
    return 0;
}
static int uv_find(const UioeCtx *cx, const char *key, int from, int to, int near) {
    char t[700]; int best = -1, bd = 1 << 30;
    if (from < 1) from = 1; if (to > cx->vnl) to = cx->vnl;
    for (int l = from; l <= to; l++) if (uv_line(cx, l, t, sizeof t) && uv_hit(t, key)) { const int d = abs(l - near); if (d < bd) { bd = d; best = l; } }
    return best;
}
/* o arquivo mudou depois do indice? 1 sim, 0 nao, -1 nao se sabe (sem visao ou arquivo fora do indice) */
static int uv_changed(const UioeCtx *cx, const char *rel) {
    if (cx->view_state != 1) return -1;
    StoreLiveDoc *e = store_live_find((StoreLive *)&cx->view.L, rel, strlen(rel));
    if (!e || e->dead) return -1;
    return e->d.size != cx->vsize || e->d.mtime != cx->vmtime;
}
/* verifica UM no (relendo o arquivo) e ajusta confianca e linha. Devolve o ver. */
static int uioe_verify_node(UioeCtx *cx, UioeNode *n) {
    if (n->kind == UN_FACT || !n->path[0]) { n->ver = UV_SKIPPED; return n->ver; }
    n->vnote[0] = 0;
    if (n->kind == UN_FOLDER) {          /* pasta: so existe ou nao */
        char full[2600]; struct stat st; if (n->path[0] == '/') snprintf(full, sizeof full, "%s", n->path); else snprintf(full, sizeof full, "%s/%s", cx->root, n->path);
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) { n->ver = UV_GONE; snprintf(n->vnote, sizeof n->vnote, "the folder no longer exists"); n->dropped = 1; } else n->ver = UV_OK;
        return n->ver;
    }
    const int ld = uv_load(cx, n->path);
    if (ld == 0) { n->ver = UV_GONE; snprintf(n->vnote, sizeof n->vnote, "the file no longer exists"); n->dropped = 1; return n->ver; }
    if (ld < 0) { n->ver = UV_UNREADABLE; snprintf(n->vnote, sizeof n->vnote, "the file cannot be read"); n->dropped = 1; return n->ver; }
    const int chg = uv_changed(cx, n->path);
    if (n->kind == UN_FILE || n->kind == UN_FOLDER) { n->ver = UV_OK; return n->ver; }
    const char *key = n->vkey[0] ? n->vkey : n->name;
    int line = n->line0 > 0 ? n->line0 : 1, found = -1;
    if (!key[0]) found = line <= cx->vnl ? line : -1;
    else { found = uv_find(cx, key, line - 3, line + 3, line); }
    if (found < 0 && key[0]) found = uv_find(cx, key, 1, cx->vnl, line);            /* o arquivo foi editado: procura o texto no arquivo todo */
    char t[700];
    if (found < 0) {
        n->ver = UV_STALE; n->conf -= 25; if (n->conf < 1) n->conf = 1;
        snprintf(n->vnote, sizeof n->vnote, chg == 1 ? "line not verified: the file changed since it was indexed" : "line not verified: the text is not in the file");
        return n->ver;
    }
    if (found != line) { const int d = found - line; n->line0 += d; n->line1 += d; n->vline = found; n->ver = UV_MOVED; snprintf(n->vnote, sizeof n->vnote, "line corrected: moved %+d line(s) since indexed", d); }
    else { n->vline = found; n->ver = UV_OK; if (chg == 1) snprintf(n->vnote, sizeof n->vnote, "the file changed since it was indexed, the line still says it"); else if (chg < 0) snprintf(n->vnote, sizeof n->vnote, "freshness against the index not known"); }
    if (n->cver <= 0) { n->conf += 4; if (n->conf > 99) n->conf = 99; }
    if (uv_line(cx, found, t, sizeof t)) { snprintf(n->text, sizeof n->text, "%.190s", t); uioe_mask_line(n->text); if (n->secret) n->text[0] = 0; }
    return n->ver;
}
/* a faixa de linhas lida de volta para citar (trecho completo de um no de simbolo, ate maxl linhas), ja na verificacao */
static int uioe_range_text(UioeCtx *cx, const char *rel, int l0, int l1, int maxl, char *out, size_t cap) {
    out[0] = 0; if (uv_load(cx, rel) != 1) return 0;
    size_t o = 0; char t[400];
    for (int l = l0; l <= l1 && l < l0 + maxl; l++) { if (!uv_line(cx, l, t, sizeof t)) break; o += (size_t)snprintf(out + o, cap - o, "%s%d: %s", o ? "\n" : "", l, t); if (o + 420 > cap) break; }
    return 1;
}
/* verifica os nos ids[0..n) na ordem de prioridade dada, dentro do orcamento; agrupa por arquivo para nao reler. Devolve quantos foram relidos. */
static int uioe_verify_list(UioeCtx *cx, UioeGraph *g, const int *ids, int n, UioeRecall *rc) {
    const double t0 = now_sec(); int done = 0, take = 0, order[UV_MAXRANGES];
    for (int i = 0; i < n && take < UV_MAXRANGES; i++) { UioeNode *x = &g->n[ids[i]]; if (x->ver != UV_NONE && x->ver != UV_SKIPPED) continue; if (x->kind == UN_FACT) continue; order[take++] = ids[i]; }
    /* a escolha dos 40 foi por prioridade; a leitura e por arquivo, para cada arquivo ser lido uma vez (insercao estavel) */
    for (int i = 1; i < take; i++) { const int x = order[i]; int j = i; while (j > 0 && strcmp(g->n[order[j - 1]].path, g->n[x].path) > 0) { order[j] = order[j - 1]; j--; } order[j] = x; }
    for (int i = 0; i < take; i++) {
        if (!cx->det && (now_sec() - t0) * 1000.0 > UV_MS) break;
        UioeNode *x = &g->n[order[i]]; const int v = uioe_verify_node(cx, x); done++;
        if (v == UV_OK) rc->nverified++; else if (v == UV_MOVED) { rc->nverified++; rc->nmoved++; } else if (v == UV_STALE) rc->nstale++;
    }
    for (int i = 0; i < n; i++) { const UioeNode *x = &g->n[ids[i]]; if ((x->ver == UV_NONE) && x->kind != UN_FACT) rc->nnotreread++; }
    return done;
}
