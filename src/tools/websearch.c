/* ---------- ferramenta WebSearch (ver docs/tools.md) ----------
 * query (ou queries: ate 4, em paralelo), read (0 a 5 paginas lidas ao mesmo tempo, com os trechos que citam as
 * palavras da busca), allowed_domains, blocked_domains (nao os dois). Usa a busca do katana (tool_web_search), filtra
 * pelo host e devolve UM resultado de cerca de 4000 bytes: a lista e os melhores trechos das paginas lidas. */
static const char CT_WEBSEARCH_DESC[] =
    "Searches the web and returns results as a numbered list with title, URL and a short summary, followed by the "
    "passages of the top pages that match your words (read=3 by default: the pages are downloaded at the same time, so "
    "you usually do not need WebFetch afterwards; read=0 gives only the list). Use it for current information or "
    "anything the model may not know. To research several angles in one call give queries (up to 4 searches run at "
    "once, results merged, duplicates removed) instead of query. A page listed here is kept: WebFetch of the same URL "
    "is instant and continues with offset. The whole result is about 4000 bytes. allowed_domains keeps only results "
    "from those sites, blocked_domains drops results from those sites (a domain also covers its subdomains). Give one "
    "of them, never both.";
static const char CT_WEBSEARCH_PROPS[] =
    "\"query\": {\"type\": \"string\", \"description\": \"The search query.\"}, "
    "\"queries\": {\"type\": \"array\", \"items\": {\"type\": \"string\"}, \"description\": \"Instead of query: up to 4 searches run at once, results merged.\"}, "
    "\"read\": {\"type\": \"integer\", \"description\": \"Pages to read for passages, 0 to 5 (default 3; 0 = list only).\"}, "
    "\"allowed_domains\": {\"type\": \"array\", \"items\": {\"type\": \"string\"}, \"description\": \"Only include results from these domains.\"}, "
    "\"blocked_domains\": {\"type\": \"array\", \"items\": {\"type\": \"string\"}, \"description\": \"Never include results from these domains.\"}";
static const char CT_WEBSEARCH_REQ[] = "";

#define CT_WS_MAXD 32
/* lista de dominios de um argumento (array de textos, ou um texto so): normaliza (minusculo, sem esquema, sem caminho,
   sem porta, sem "*." nem pontos na frente) e devolve quantos */
static int ct_ws_domains(const char *js, const char *end, const char *key, char d[][100]) {
    const char *a, *b;
    int n = 0;
    if (!ct_find(js, end, key, &a, &b)) return 0;
    const char *p = (*a == '[') ? a + 1 : a;
    while (p < b && n < CT_WS_MAXD) {
        while (p < b && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= b || *p == ']') break;
        const char *e = ct_skip(p, b);
        if (!e) break;
        if (*p == '"') {
            char *s = ct_unquote(p, e, NULL), *q = s;
            while (isspace((unsigned char)*q)) q++;
            const char *sc = strstr(q, "://");
            if (sc) q = (char *)sc + 3;
            q[strcspn(q, "/?#")] = 0;
            char *c = strchr(q, ':'); if (c) *c = 0;
            while (*q == '.' || *q == '*') q++;
            for (char *t = q; *t; t++) *t = (char)tolower((unsigned char)*t);
            size_t l = strlen(q);
            while (l > 0 && (q[l - 1] == '.' || isspace((unsigned char)q[l - 1]))) q[--l] = 0;
            if (*q && l < 100) { snprintf(d[n], 100, "%s", q); n++; }
            free(s);
        }
        if (*a != '[') break;
        p = e;
    }
    return n;
}
/* o host e o dominio ou um subdominio dele */
static int ct_ws_match(const char *host, const char *dom) {
    const size_t hl = strlen(host), dl = strlen(dom);
    if (hl == dl) return strcmp(host, dom) == 0;
    return hl > dl && host[hl - dl - 1] == '.' && strcmp(host + hl - dl, dom) == 0;
}
/* inicio de entrada: "<digitos>. " no comeco de uma linha */
static int ct_ws_entry(const char *s, const char **body) {
    const char *q = s;
    while (isdigit((unsigned char)*q)) q++;
    if (q == s || q[0] != '.' || q[1] != ' ') return 0;
    if (body) *body = q + 2;
    return 1;
}
/* Funcao pura: texto de resultados ("N. titulo\n   url\n   resumo\n..." como a busca devolve) filtrado por host.
   allow/block: listas de dominios (so uma delas nao vazia). Escreve em out ate cap bytes, so entradas inteiras,
   renumeradas. *total = entradas na entrada; *kept = entradas mantidas. */
static void ct_ws_filter(const char *text, char allow[][100], int na, char block[][100], int nb, char *out, size_t cap, int *total, int *kept) {
    size_t o = 0;
    int tot = 0, k = 0;
    const char *p = text, *body;
    out[0] = 0;
    while (*p && !ct_ws_entry(p, &body)) { const char *nl = strchr(p, '\n'); if (!nl) { p = p + strlen(p); break; } p = nl + 1; }
    while (*p) {
        const char *e = strchr(p, '\n');                         /* fim da entrada: a proxima linha que comeca outra */
        while (e) {
            e++;
            if (ct_ws_entry(e, NULL)) break;
            e = strchr(e, '\n');
        }
        if (!e) e = p + strlen(p);
        ct_ws_entry(p, &body);
        tot++;
        char host[300] = "";
        const char *l1 = (const char *)memchr(p, '\n', (size_t)(e - p));
        if (l1) {
            const char *u = l1 + 1;
            while (u < e && (*u == ' ' || *u == '\t')) u++;
            char url[1100]; size_t ul = strcspn(u, "\n"); if (ul > sizeof url - 1) ul = sizeof url - 1;
            memcpy(url, u, ul); url[ul] = 0;
            if (!ct_web_host(url, host, sizeof host)) host[0] = 0;
        }
        int keep = 1;
        if (na > 0) { keep = 0; for (int i = 0; i < na; i++) if (host[0] && ct_ws_match(host, allow[i])) keep = 1; }
        for (int i = 0; i < nb; i++) if (host[0] && ct_ws_match(host, block[i])) keep = 0;
        if (keep) {
            const size_t blen = (size_t)(e - body);
            char num[16]; const int nl = snprintf(num, sizeof num, "%d. ", k + 1);
            if (o + (size_t)nl + blen + 2 <= cap) {
                memcpy(out + o, num, (size_t)nl); o += (size_t)nl;
                memcpy(out + o, body, blen); o += blen;
                if (out[o - 1] != '\n') out[o++] = '\n';
                out[o] = 0;
                k++;
            }
        }
        p = e;
    }
    *total = tot; *kept = k;
}


#define CT_WS_BUDGET 4000          /* bytes do resultado todo (cerca de 1100 tokens) */
#define CT_WS_MAXQ 4
#define CT_WS_MAXH 40
typedef struct { char title[300], url[1000], snip[300]; double score; int idx; } CtWsHit;

/* lista de textos de um argumento (array, ou um texto so), cada um em memoria nova; devolve quantos */
static int ct_ws_strlist(const char *js, const char *end, const char *key, char **q, int max) {
    const char *a, *b;
    int n = 0;
    if (!ct_find(js, end, key, &a, &b)) return 0;
    const char *p = (*a == '[') ? a + 1 : a;
    while (p < b && n < max) {
        while (p < b && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= b || *p == ']') break;
        const char *e = ct_skip(p, b);
        if (!e) break;
        if (*p == '"') { q[n] = ct_unquote(p, e, NULL); if (q[n][0]) n++; else free(q[n]); }
        if (*a != '[') break;
        p = e;
    }
    return n;
}
static const char *ct_ws_nextline(const char *p) { const char *nl = strchr(p, '\n'); return nl ? nl + 1 : p + strlen(p); }
/* entradas "N. titulo\n   url\n   resumo" do texto filtrado */
static int ct_ws_parse(const char *text, CtWsHit *h, int max) {
    int n = 0;
    const char *p = text, *body;
    while (*p && n < max) {
        if (!ct_ws_entry(p, &body)) { p = ct_ws_nextline(p); continue; }
        CtWsHit *x = &h[n];
        memset(x, 0, sizeof *x);
        snprintf(x->title, sizeof x->title, "%.*s", (int)strcspn(body, "\n"), body);
        p = ct_ws_nextline(p);
        while (*p == ' ' || *p == '\t') p++;
        snprintf(x->url, sizeof x->url, "%.*s", (int)strcspn(p, "\n"), p);
        p = ct_ws_nextline(p);
        while (*p == ' ' || *p == '\t') p++;
        if (!ct_ws_entry(p, NULL) && strncmp(p, "(sources:", 9) != 0) snprintf(x->snip, sizeof x->snip, "%.*s", (int)strcspn(p, "\n"), p);
        if (strncmp(x->url, "http", 4) == 0) n++;
        while (*p && !ct_ws_entry(p, NULL)) p = ct_ws_nextline(p);
    }
    return n;
}
/* a URL pode ser lida: http(s), nao e PDF, nao e endereco local (a menos do gancho de teste) */
static int ct_ws_fetchable(const char *url, char *u2, size_t cap) {
    char host[300];
    if (ci_ncmp(url, "https://", 8) == 0) snprintf(u2, cap, "%s", url);
    else if (ci_ncmp(url, "http://", 7) == 0) { if (ct_web_local_ok()) snprintf(u2, cap, "%s", url); else snprintf(u2, cap, "https://%s", url + 7); }
    else return 0;
    if (strlen(u2) > 1000 || !ct_web_host(u2, host, sizeof host)) return 0;
    const size_t hl = strlen(host), ul = strcspn(u2, "?#");
    const int loop = strcmp(host, "localhost") == 0 || (hl > 10 && strcmp(host + hl - 10, ".localhost") == 0) || strncmp(host, "127.", 4) == 0;
    if ((loop || !strchr(host, '.')) && !ct_web_local_ok()) return 0;
    if (ul > 4 && ci_ncmp(u2 + ul - 4, ".pdf", 4) == 0) return 0;
    return 1;
}

static void ct_websearch(const char *js, const char *end, char *out, size_t cap) {
    static char allow[CT_WS_MAXD][100], block[CT_WS_MAXD][100];
    static CtWsHit hits[CT_WS_MAXH], one[CT_WS_MAXH];
    const double t0 = now_sec();
    if (!g_web || (getenv("KATANA_WEB") && strcmp(getenv("KATANA_WEB"), "0") == 0)) { snprintf(out, cap, "error: internet tools are disabled (KATANA_WEB=0)"); return; }
    char *qs[CT_WS_MAXQ] = {0};
    int nq = ct_ws_strlist(js, end, "queries", qs, CT_WS_MAXQ);
    if (nq == 0) { char *q = ct_str(js, end, "query"); if (q && *q) qs[nq++] = q; else free(q); }
    if (nq == 0) { snprintf(out, cap, "error: missing query. Call WebSearch with query (the search text) or queries (up to 4 searches), optionally read, allowed_domains or blocked_domains"); return; }
    int rd = (int)ct_int(js, end, "read", 3);
    if (rd < 0) rd = 0;
    if (rd > 5) rd = 5;
    const int na = ct_ws_domains(js, end, "allowed_domains", allow), nb = ct_ws_domains(js, end, "blocked_domains", block);
    const size_t RB = 40000;
    char *raw[CT_WS_MAXQ] = {0}, *tmp[CT_WS_MAXQ] = {0};
    size_t body = 0;
    int i, nfail = 0, nread = 0, sc_cached = 0, pg_cached = 0;
    double t_search = 0, t_pages = 0;
    if (na > 0 && nb > 0) { snprintf(out, cap, "error: give allowed_domains or blocked_domains, not both. Use allowed_domains to keep only some sites, or blocked_domains to drop some sites"); goto done; }
    const char *fix = getenv("KATANA_WEBSEARCH_FIXTURE");      /* gancho de teste: resultados fixos em vez de buscar */
    if (fix && !ct_web_local_ok()) rd = 0;                      /* e sem leitura de paginas, a menos que sejam locais */
    snprintf(g_ct_lastq, sizeof g_ct_lastq, "%s", qs[0]);
    for (i = 0; i < nq; i++) { raw[i] = (char *)malloc(RB); tmp[i] = (char *)malloc(cap); if (!raw[i] || !tmp[i]) { snprintf(out, cap, "error: out of memory"); goto done; } raw[i][0] = 0; }
    {   /* buscas: do cache, ou todas ao mesmo tempo */
        FILE *fp[CT_WS_MAXQ][MS_NSRC];
        int miss[CT_WS_MAXQ] = {0};
        const char *sx = getenv("KATANA_SEARX");
        for (i = 0; i < nq; i++) {
            size_t len = 0; char *c;
            if (fix) { snprintf(raw[i], RB, "%s", fix); continue; }
            if ((c = ct_wc_read('s', qs[i], CT_WC_BUSCA, &len))) { snprintf(raw[i], RB, "%s", c); free(c); sc_cached++; continue; }
            miss[i] = 1;
            if (!(sx && *sx)) tool_web_search_start(qs[i], fp[i]);
        }
        for (i = 0; i < nq; i++) if (miss[i]) {
            if (sx && *sx) tool_web_search(qs[i], raw[i], RB); else tool_web_search_collect(qs[i], fp[i], raw[i], RB);
            if (strncmp(raw[i], "1. ", 3) == 0) ct_wc_write('s', qs[i], raw[i], strlen(raw[i]), "", 0);
        }
    }
    t_search = now_sec() - t0;
    int tot = 0, kept = 0, okq = 0;
    const char *by = na ? "allowed_domains" : "blocked_domains";
    for (i = 0; i < nq; i++) {
        if (strncmp(raw[i], "error:", 6) == 0) { tmp[i][0] = 0; continue; }
        int t1 = 0, k1 = 0;
        ct_ws_filter(raw[i], allow, na, block, nb, tmp[i], cap > 200 ? cap - 200 : cap / 2, &t1, &k1);
        tot += t1; kept += k1; okq++;
    }
    if (okq == 0) { snprintf(out, cap, "%s", raw[0]); goto done; }          /* todas as buscas falharam: a mensagem da primeira */
    if (tot == 0) { snprintf(out, cap, nq == 1 ? "no results for \"%s\". Try different words" : "no results for \"%s\" and the other queries. Try different words", qs[0]); goto done; }
    if (kept == 0 && (na || nb)) { snprintf(out, cap, "no results left after filtering: all %d results were removed by %s. Search again without the filter or with other domains", tot, by); goto done; }
    if (kept == 0) { snprintf(out, cap, "no results for \"%s\". Try different words", qs[0]); goto done; }
    if (nq == 1 && rd == 0) {                                   /* uma consulta, so a lista: como sempre */
        snprintf(out, cap, "%s", tmp[0]);
        const size_t l = strlen(out);
        if (kept < tot && (na || nb)) snprintf(out + l, cap - l, "[%d of %d results kept by %s]", kept, tot, by);
        else if (kept < tot) snprintf(out + l, cap - l, "[%d of %d results shown]", kept, tot);
        body = strlen(out);
        goto foot;
    }
    {   /* junta as consultas: URL repetida soma pontos (posicao + bonus por aparecer) */
        int nh = 0;
        for (i = 0; i < nq; i++) {
            if (!tmp[i][0]) continue;
            const int k = ct_ws_parse(tmp[i], one, CT_WS_MAXH);
            for (int j = 0; j < k; j++) {
                char key[500], key2[500];
                url_key(one[j].url, key, sizeof key);
                int d = -1;
                for (int t = 0; t < nh; t++) { url_key(hits[t].url, key2, sizeof key2); if (strcmp(key, key2) == 0) { d = t; break; } }
                if (d < 0) { if (nh >= CT_WS_MAXH) continue; d = nh++; hits[d] = one[j]; hits[d].score = 0; hits[d].idx = d; }
                else if (strlen(one[j].snip) > strlen(hits[d].snip)) snprintf(hits[d].snip, sizeof hits[d].snip, "%s", one[j].snip);
                hits[d].score += 1.0 / (j + 1) + 0.5;
            }
        }
        for (int a = 1; a < nh; a++) {                           /* ordem por pontos; empate fica na ordem de chegada */
            CtWsHit x = hits[a]; int b = a - 1;
            while (b >= 0 && hits[b].score < x.score) { hits[b + 1] = hits[b]; b--; }
            hits[b + 1] = x;
        }
        size_t B = CT_WS_BUDGET;
        if (g_budget >= 500 && B > g_budget) B = g_budget;
        if (B + 300 > cap) B = cap > 600 ? cap - 300 : cap / 2;
        /* paginas a ler: as primeiras da lista que podem ser lidas */
        char urls[5][1100]; int sel[5], ns = 0;
        for (int j = 0; j < nh && j < 8 && ns < rd; j++) if (ct_ws_fetchable(hits[j].url, urls[ns], sizeof urls[ns])) sel[ns++] = j;
        WnPage *pgs[5] = {0};
        const double t2 = now_sec();
        {   /* na memoria, no disco, ou baixa as que faltam ao mesmo tempo */
            const char *dl[5]; int dj[5], nd = 0;
            for (int k = 0; k < ns; k++) {
                ct_wf_expire(urls[k]);
                if ((pgs[k] = wn_get(urls[k])) || (pgs[k] = ct_wc_page_load(urls[k]))) { pg_cached++; continue; }
                dl[nd] = urls[k]; dj[nd++] = k;
            }
            if (nd > 0) {
                CtWsGet g[5];
                ct_ws_get_all(dl, nd, g, 6, 10.0);
                for (int k = 0; k < nd; k++) {
                    if (g[k].n > 0) {
                        char err[300];
                        pgs[dj[k]] = wn_ingest(dl[k], g[k].buf, g[k].n, 0, 0, err, sizeof err);     /* toma posse do buffer */
                        g[k].buf = NULL;
                        if (pgs[dj[k]]) { *ct_wf_at(dl[k], 1) = time(NULL); ct_wc_page_store(pgs[dj[k]]); }
                    }
                    free(g[k].buf);
                }
            }
        }
        t_pages = now_sec() - t2;
        for (int k = 0; k < ns; k++) { if (pgs[k] && pgs[k]->tn) nread++; else { pgs[k] = NULL; nfail++; } }
        /* lista compacta */
        size_t o = 0;
        out[0] = 0;
        const int tl = rd > 0 ? 90 : 160, sl_ = rd > 0 ? 90 : 220, maxl = rd > 0 ? 6 : 8, lcap = rd > 0 ? 1500 : (int)B;
        int shown = 0;
        for (int j = 0; j < nh && j < maxl; j++) {
            char line[1700];
            const int ln = snprintf(line, sizeof line, "%d. %.*s\n   %s\n   %.*s\n", j + 1, tl, hits[j].title, hits[j].url, sl_, hits[j].snip);
            if (shown >= 3 && (int)o + ln > lcap) break;
            if (o + (size_t)ln + 1 > B) break;
            memcpy(out + o, line, (size_t)ln + 1); o += (size_t)ln; shown++;
        }
        if (nh > shown) o += (size_t)snprintf(out + o, cap - o, "[%d of %d results shown]\n", shown, nh);
        /* trechos das paginas lidas: o orcamento restante se reparte pela pontuacao dos trechos */
        char w[24][32]; int nw = 0;
        for (i = 0; i < nq && nw < 24; i++) {
            char t[24][32]; const int k = ct_wf_words(qs[i], t, 24);
            for (int a = 0; a < k && nw < 24; a++) { int dup = 0; for (int b = 0; b < nw; b++) if (strcmp(w[b], t[a]) == 0) dup = 1; if (!dup) memcpy(w[nw++], t[a], 32); }
        }
        const size_t RESERVE = 220;                              /* rodape e cabecalho */
        const long P = (long)B - (long)o - (long)RESERVE;
        g_ct_wf_query = qs[0]; g_ct_wf_budget_ms = 1000.0;      /* varias paginas: tempo curto por pagina; o cache cresce */
        if (nread > 0 && (nw > 0 || es_available()) && P >= 400) {
            char *first[5] = {0}; int score[5] = {0}, order[5], no = 0;
            for (int k = 0; k < ns; k++) if (pgs[k]) {
                const size_t avail = pgs[k]->tn < CT_WF_READ ? pgs[k]->tn : CT_WF_READ;
                size_t used = 0;
                char *b = (char *)malloc((size_t)P + 16);
                if (b && ct_wf_pick(pgs[k]->txt, avail, w, nw, b, (size_t)P, &used) > 0) score[k] = ct_wf_score(b, w, nw);
                free(b);
                if (score[k] > 0) order[no++] = k;
            }
            for (int a = 1; a < no; a++) { int x = order[a], b = a - 1; while (b >= 0 && score[order[b]] < score[x]) { order[b + 1] = order[b]; b--; } order[b + 1] = x; }
            long left = P; int sum = 0;
            for (int a = 0; a < no; a++) sum += score[order[a]];
            if (no > 0) o += (size_t)snprintf(out + o, cap - o, "\nPassages from the pages read (matching your words):\n");
            for (int a = 0; a < no && sum > 0; a++) {
                const int k = order[a];
                char hd[1200];
                const int hl = snprintf(hd, sizeof hd, "[%d] %s\n", sel[k] + 1, urls[k]);
                long share = left * score[k] / sum - hl;
                sum -= score[k];
                if (share < 200) { left -= 0; continue; }
                const size_t avail = pgs[k]->tn < CT_WF_READ ? pgs[k]->tn : CT_WF_READ;
                size_t used = 0;
                char *b = (char *)malloc((size_t)share + 16);
                if (!b) continue;
                size_t bl = ct_wf_pick(pgs[k]->txt, avail, w, nw, b, (size_t)share, &used);
                char *t = b;
                if (t[0] == '#') { char *nn = strstr(t, "\n\n"); if (nn) { t = nn + 2; bl -= (size_t)(t - b); } }          /* o titulo ja esta na lista */
                while (bl > 0 && (t[bl - 1] == '\n' || t[bl - 1] == ' ')) t[--bl] = 0;
                if (bl > 0 && o + (size_t)hl + bl + 4 < cap) {
                    o += (size_t)snprintf(out + o, cap - o, "%s%.*s\n\n", hd, (int)bl, t);
                    left -= hl + (long)bl + 2;
                }
                free(b);
            }
            for (int k = 0; k < 5; k++) free(first[k]);
        }
        body = strlen(out);
        while (body > 0 && out[body - 1] == '\n') out[--body] = 0;
        out[body] = 0;
    }
foot:
    {
        char f[400];
        const double net = now_sec() - t0;
        int fl = snprintf(f, sizeof f, "\n[web: search %.1f s%s", t_search, sc_cached ? " (cached)" : "");
        if (nread + nfail > 0) fl += snprintf(f + fl, sizeof f - fl, ", %d page%s read in %.1f s%s%s", nread, nread == 1 ? "" : "s", t_pages, pg_cached ? " (some cached)" : "", "");
        if (nfail) fl += snprintf(f + fl, sizeof f - fl, ", %d failed or slow", nfail);
        fl += snprintf(f + fl, sizeof f - fl, ", total %.1f s, %zu bytes returned", net, body);
        if (nread > 0) fl += snprintf(f + fl, sizeof f - fl, ". Pages are kept: WebFetch of a listed URL is instant, offset reads more");
        snprintf(f + fl, sizeof f - fl, "]");
        const size_t l = strlen(out);
        if (l + strlen(f) + 1 < cap) memcpy(out + l, f, strlen(f) + 1);
    }
done:
    for (i = 0; i < CT_WS_MAXQ; i++) { free(raw[i]); free(tmp[i]); free(qs[i]); }
}
