/* ---------- ferramenta WebSearch (nomes e argumentos do Claude Code; ver docs/claude-tools.md) ----------
 * query, allowed_domains, blocked_domains (nao os dois). Usa a busca do katana (tool_web_search) e filtra pelo host. */
static const char CT_WEBSEARCH_DESC[] =
    "Searches the web and returns results as a numbered list with title, URL and a short summary. Use it for current "
    "information or anything the model may not know; then read the best result with WebFetch. allowed_domains keeps "
    "only results from those sites, blocked_domains drops results from those sites (a domain also covers its "
    "subdomains). Give one of them, never both.";
static const char CT_WEBSEARCH_PROPS[] =
    "\"query\": {\"type\": \"string\", \"description\": \"The search query.\"}, "
    "\"allowed_domains\": {\"type\": \"array\", \"items\": {\"type\": \"string\"}, \"description\": \"Only include results from these domains.\"}, "
    "\"blocked_domains\": {\"type\": \"array\", \"items\": {\"type\": \"string\"}, \"description\": \"Never include results from these domains.\"}";
static const char CT_WEBSEARCH_REQ[] = "\"query\"";

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

static void ct_websearch(const char *js, const char *end, char *out, size_t cap) {
    static char allow[CT_WS_MAXD][100], block[CT_WS_MAXD][100];
    if (!g_web || (getenv("KATANA_WEB") && strcmp(getenv("KATANA_WEB"), "0") == 0)) { snprintf(out, cap, "error: internet tools are disabled (KATANA_WEB=0)"); return; }
    char *query = ct_str(js, end, "query");
    if (!query || !*query) { free(query); snprintf(out, cap, "error: missing query. Call WebSearch with query (the search text), optionally allowed_domains or blocked_domains"); return; }
    const int na = ct_ws_domains(js, end, "allowed_domains", allow), nb = ct_ws_domains(js, end, "blocked_domains", block);
    if (na > 0 && nb > 0) { free(query); snprintf(out, cap, "error: give allowed_domains or blocked_domains, not both. Use allowed_domains to keep only some sites, or blocked_domains to drop some sites"); return; }
    const size_t RB = 40000;
    char *raw = (char *)malloc(RB), *tmp = (char *)malloc(cap);
    if (!raw || !tmp) { free(raw); free(tmp); free(query); snprintf(out, cap, "error: out of memory"); return; }
    const char *fix = getenv("KATANA_WEBSEARCH_FIXTURE");      /* gancho de teste: resultados fixos em vez de buscar */
    if (fix) snprintf(raw, RB, "%s", fix);
    else tool_web_search(query, raw, RB);
    if (strncmp(raw, "error:", 6) == 0) { snprintf(out, cap, "%s", raw); goto done; }
    {
        int tot = 0, kept = 0;
        const char *by = na ? "allowed_domains" : "blocked_domains";
        ct_ws_filter(raw, allow, na, block, nb, tmp, cap > 200 ? cap - 200 : cap / 2, &tot, &kept);
        if (tot == 0) snprintf(out, cap, "no results for \"%s\". Try different words", query);
        else if (kept == 0 && (na || nb)) snprintf(out, cap, "no results left after filtering: all %d results were removed by %s. Search again without the filter or with other domains", tot, by);
        else if (kept == 0) snprintf(out, cap, "no results for \"%s\". Try different words", query);
        else {
            snprintf(out, cap, "%s", tmp);
            const size_t l = strlen(out);
            if (kept < tot && (na || nb)) snprintf(out + l, cap - l, "[%d of %d results kept by %s]", kept, tot, by);
            else if (kept < tot) snprintf(out + l, cap - l, "[%d of %d results shown]", kept, tot);
        }
    }
done:
    free(raw); free(tmp); free(query);
}
