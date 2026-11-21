/* ---------- ferramenta WebFetch (nomes e argumentos do Claude Code; ver docs/claude-tools.md) ----------
 * url (obrigatorio) e prompt (o que extrair). O papel do modelo pequeno que responde o prompt fica com o digest do
 * agente (depois do despacho); aqui a ferramenta devolve o texto da pagina que cabe em cap, com um cabecalho de uma
 * linha, e quando a pagina nao cabe escolhe os trechos que citam as palavras do prompt.
 * Extras do katana, opcionais: offset, find, links, render (passam para o web_fetch antigo). */
static const char CT_WEBFETCH_DESC[] =
    "Fetches a web page and returns its content as text (HTML converted, at most the first 100000 characters read). "
    "Give the url and a prompt saying what you want from the page; when the page is long, the passages that mention the "
    "words of the prompt are returned first and the result says how much was not read. http:// is upgraded to https://. "
    "Local addresses (localhost, hosts without a dot) are refused: use curl through Bash for local servers. If the URL "
    "redirects to another host the redirect is reported and you must fetch the new URL yourself. Results are cached for "
    "15 minutes. Optional extras: offset (continue a long page), find (only passages with a word), links (list the "
    "links of the page), render (run the page's JavaScript in a browser).";
static const char CT_WEBFETCH_PROPS[] =
    "\"url\": {\"type\": \"string\", \"description\": \"The URL to fetch (http:// is upgraded to https://).\"}, "
    "\"prompt\": {\"type\": \"string\", \"description\": \"What to extract or answer from the page.\"}, "
    "\"offset\": {\"type\": \"integer\", \"description\": \"Optional: byte offset in the page text, to continue a long page.\"}, "
    "\"find\": {\"type\": \"string\", \"description\": \"Optional: return only the passages that mention this word.\"}, "
    "\"links\": {\"type\": \"boolean\", \"description\": \"Optional: list the links of the page instead of its text.\"}, "
    "\"render\": {\"type\": \"boolean\", \"description\": \"Optional: run the page's JavaScript in a headless browser first.\"}";
static const char CT_WEBFETCH_REQ[] = "\"url\", \"prompt\"";

#define CT_WF_READ 100000          /* no maximo isto do texto da pagina e lido */
#define CT_WF_TTL 900              /* a pagina guardada vale 15 minutos */

/* host (minusculo, sem usuario nem porta) de uma URL http(s); 0 se nao for uma URL */
static int ct_web_host(const char *url, char *host, size_t cap) {
    const char *p = strstr(url, "://");
    if (!p) return 0;
    p += 3;
    size_t n = strcspn(p, "/?#"), s = 0;
    const char *at = (const char *)memchr(p, '@', n);
    if (at) s = (size_t)(at - p) + 1;
    size_t e = n;
    if (p[s] == '[') { const char *rb = (const char *)memchr(p + s, ']', n - s); e = rb ? (size_t)(rb - p) + 1 : n; }
    else { const char *c = (const char *)memchr(p + s, ':', n - s); if (c) e = (size_t)(c - p); }
    if (e <= s || e - s + 1 > cap) return 0;
    for (size_t i = s; i < e; i++) host[i - s] = (char)tolower((unsigned char)p[i]);
    host[e - s] = 0;
    return 1;
}

/* hora em que cada URL foi baixada (para a validade de 15 minutos) */
typedef struct { char url[1024]; time_t t; } CtWfAt;
static CtWfAt g_ct_wfat[8];
static int g_ct_wfat_n;
static time_t *ct_wf_at(const char *url, int create) {
    for (int i = 0; i < g_ct_wfat_n; i++) if (strcmp(g_ct_wfat[i].url, url) == 0) return &g_ct_wfat[i].t;
    if (!create) return NULL;
    if (g_ct_wfat_n == 8) { memmove(g_ct_wfat, g_ct_wfat + 1, sizeof g_ct_wfat[0] * 7); g_ct_wfat_n = 7; }
    const int i = g_ct_wfat_n++;
    snprintf(g_ct_wfat[i].url, sizeof g_ct_wfat[i].url, "%s", url);
    g_ct_wfat[i].t = 0;
    return &g_ct_wfat[i].t;
}
/* descarta a copia guardada se for mais velha que 15 minutos (ou de origem desconhecida) */
static void ct_wf_expire(const char *url) {
    for (int i = 0; i < WN_PAGES; i++) {
        if (!g_wn[i].txt || strcmp(g_wn[i].url, url) != 0) continue;
        time_t *t = ct_wf_at(url, 0);
        if (!t || *t == 0 || time(NULL) - *t > CT_WF_TTL) {
            free(g_wn[i].txt); free(g_wn[i].links);
            g_wn[i].txt = NULL; g_wn[i].links = NULL; g_wn[i].tn = g_wn[i].ln = 0; g_wn[i].url[0] = 0;
        }
    }
}

/* palavras significativas do prompt (minusculas, 4 letras ou mais, fora as comuns); devolve quantas */
static int ct_wf_words(const char *prompt, char w[][32], int max) {
    static const char *stop[] = { "this", "that", "with", "from", "what", "which", "about", "page", "tell", "give", "please",
        "list", "extract", "summarize", "summary", "there", "their", "them", "have", "does", "into", "return", "find",
        "show", "whats", "also", "only", "each", "main", "most", NULL };
    int n = 0;
    for (const char *p = prompt; *p && n < max; ) {
        while (*p && !isalnum((unsigned char)*p) && !((unsigned char)*p & 0x80)) p++;
        char t[32]; int l = 0;
        while (*p && (isalnum((unsigned char)*p) || ((unsigned char)*p & 0x80)) && l < 31) t[l++] = (char)tolower((unsigned char)*p++);
        while (*p && (isalnum((unsigned char)*p) || ((unsigned char)*p & 0x80))) p++;
        t[l] = 0;
        if (l < 4) continue;
        int skip = 0;
        for (int k = 0; stop[k]; k++) if (strcmp(stop[k], t) == 0) skip = 1;
        for (int k = 0; k < n; k++) if (strcmp(w[k], t) == 0) skip = 1;
        if (!skip) memcpy(w[n++], t, (size_t)l + 1);
    }
    return n;
}

/* escolhe, dentro de [txt, txt+n), os blocos (separados por linha em branco) que mais citam as palavras e os copia em
   ordem original para out (ate lim bytes). Devolve os bytes escritos; *used recebe quantos bytes do texto entraram */
static size_t ct_wf_pick(const char *txt, size_t n, char w[][32], int nw, char *out, size_t lim, size_t *used) {
    enum { MAXB = 4000 };
    static size_t bs[MAXB], be[MAXB]; static int sc[MAXB]; static char sel[MAXB];
    int nb = 0;
    for (size_t i = 0; i < n && nb < MAXB; ) {                   /* blocos; os muito longos sao partidos */
        size_t j = i;
        while (j < n && !(txt[j] == '\n' && j + 1 < n && txt[j + 1] == '\n') && j - i < 1500) j++;
        if (j < n && txt[j] != '\n') { size_t k = j; while (k > i + 700 && txt[k] != '\n' && txt[k] != ' ') k--; if (k > i + 700) j = k; }
        bs[nb] = i; be[nb] = j; nb++;
        while (j < n && (txt[j] == '\n' || txt[j] == ' ')) j++;
        i = j;
    }
    for (int b = 0; b < nb; b++) {
        sc[b] = 0; sel[b] = 0;
        for (int k = 0; k < nw; k++) if (wn_cifind(txt + bs[b], be[b] - bs[b], w[k])) sc[b]++;
    }
    size_t o = 0;
    if (nb > 0 && txt[0] == '#' && be[0] - bs[0] < 300 && be[0] - bs[0] + 2 < lim) { sel[0] = 1; o += be[0] - bs[0] + 2; }       /* titulo */
    for (;;) {
        int best = -1;
        for (int b = 0; b < nb; b++) if (!sel[b] && sc[b] > 0 && (best < 0 || sc[b] > sc[best])) best = b;
        if (best < 0) break;
        if (o + (be[best] - bs[best]) + 2 > lim) { sc[best] = 0; continue; }       /* esse nao cabe; tenta os menores */
        sel[best] = 1; o += be[best] - bs[best] + 2;
    }
    size_t w_ = 0, u = 0;
    for (int b = 0; b < nb; b++) if (sel[b]) {
        w_ += (size_t)snprintf(out + w_, lim + 8 - w_, "%.*s\n\n", (int)(be[b] - bs[b]), txt + bs[b]);
        u += be[b] - bs[b];
    }
    *used = u;
    return w_;
}

/* escreve s entre aspas JSON em o */
static size_t ct_wf_jq(const char *s, char *o, size_t cap) {
    size_t n = 0;
    o[n++] = '"';
    for (; *s && n + 8 < cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o[n++] = '\\'; o[n++] = (char)c; }
        else if (c < 0x20) n += (size_t)snprintf(o + n, cap - n, "\\u%04x", c);
        else o[n++] = (char)c;
    }
    o[n++] = '"'; o[n] = 0;
    return n;
}

static void ct_webfetch(const char *js, const char *end, char *out, size_t cap) {
    if (!g_web || (getenv("KATANA_WEB") && strcmp(getenv("KATANA_WEB"), "0") == 0)) { snprintf(out, cap, "error: internet tools are disabled (KATANA_WEB=0)"); return; }
    char *url = ct_str(js, end, "url");
    if (!url || !*url) { free(url); snprintf(out, cap, "error: missing url. Call WebFetch with url (the page) and prompt (what to extract from it)"); return; }
    char *prompt = ct_str(js, end, "prompt"), *find = ct_str(js, end, "find");
    char host[300], u2[1100], hdr[1400];
    int upgraded = 0;
    {   /* tira espacos das pontas */
        char *s = url; while (isspace((unsigned char)*s)) s++;
        size_t l = strlen(s); while (l > 0 && isspace((unsigned char)s[l - 1])) l--;
        s[l] = 0; memmove(url, s, l + 1);
    }
    if (strlen(url) > 1000) { snprintf(out, cap, "error: URL too long (limit 1000 characters)"); goto done; }
    if (ci_ncmp(url, "http://", 7) == 0) { snprintf(u2, sizeof u2, "https://%s", url + 7); upgraded = 1; }
    else if (ci_ncmp(url, "https://", 8) == 0) snprintf(u2, sizeof u2, "%s", url);
    else { snprintf(out, cap, "error: invalid URL \"%.200s\": it must start with http:// or https://", url); goto done; }
    if (!ct_web_host(u2, host, sizeof host)) { snprintf(out, cap, "error: invalid URL \"%.200s\": no host", url); goto done; }
    {
        const size_t hl = strlen(host);
        const int loop = strcmp(host, "localhost") == 0 || (hl > 10 && strcmp(host + hl - 10, ".localhost") == 0) || strncmp(host, "127.", 4) == 0;
        if (loop || !strchr(host, '.')) {
            snprintf(out, cap, "error: WebFetch refuses local or internal hosts (\"%.100s\"). For a local server use curl through Bash, for example: curl -sS %.200s", host, url);
            goto done;
        }
    }
    snprintf(hdr, sizeof hdr, "WebFetch %s%s", u2, upgraded ? " (upgraded from http to https)" : "");
    if (getenv("KATANA_WEBFETCH_DRY")) {                          /* gancho de teste: mostra o que seria buscado */
        snprintf(out, cap, "%s (dry run)", hdr);
        goto done;
    }
    {
        const int extras = ct_has(js, end, "offset") || (find && *find) || ct_bool(js, end, "links") || ct_bool(js, end, "render");
        if (extras) {                                             /* extras do katana: o web_fetch antigo faz */
            char j[2600]; size_t n = (size_t)snprintf(j, sizeof j, "{\"url\":");
            n += ct_wf_jq(u2, j + n, sizeof j - n);
            if (find && *find) { n += (size_t)snprintf(j + n, sizeof j - n, ",\"find\":"); n += ct_wf_jq(find, j + n, sizeof j - n); }
            n += (size_t)snprintf(j + n, sizeof j - n, ",\"offset\":%lld,\"links\":%s,\"render\":%s}", ct_int(js, end, "offset", 0),
                                  ct_bool(js, end, "links") ? "true" : "false", ct_bool(js, end, "render") ? "true" : "false");
            ct_wf_expire(u2);
            char *tmp = (char *)malloc(cap);
            if (!tmp) { snprintf(out, cap, "error: out of memory"); goto done; }
            tool_web_fetch(j, j + n, tmp, cap > 400 ? cap - 400 : cap);
            if (strncmp(tmp, "error:", 6) == 0) snprintf(out, cap, "%s%s", tmp, upgraded ? " (fetched after upgrading http to https)" : "");
            else { time_t *t = ct_wf_at(u2, 1); if (!*t) *t = time(NULL); snprintf(out, cap, "%s\n\n%s", hdr, tmp); }
            free(tmp);
            goto done;
        }
        ct_wf_expire(u2);
        WnPage *pg = wn_get(u2);
        if (!pg) {
            char err[1200];
            g_wn_render = 0;
            pg = wn_load(u2, err, sizeof err);
            if (!pg) { snprintf(out, cap, "%s%s", err, upgraded ? " (fetched after upgrading http to https)" : ""); goto done; }
            *ct_wf_at(u2, 1) = time(NULL);
        }
        if (!pg->tn) { snprintf(out, cap, "%s\n\n(the page has no readable text, even after running its JavaScript when a browser is installed; it probably needs a login. Try another source)", hdr); goto done; }
        const size_t total = pg->tn, avail = total < CT_WF_READ ? total : CT_WF_READ;
        size_t lim = cap > 700 ? cap - 700 : cap / 2;
        if (g_budget >= 500 && lim > g_budget) lim = g_budget;
        size_t o;
        if (avail <= lim) {
            snprintf(out, cap, "%s: returned all %zu bytes of the page text%s\n\n", hdr, avail, total > avail ? " (the first 100000 read; the rest was not read)" : "");
            o = strlen(out);
            memcpy(out + o, pg->txt, avail); out[o + avail] = 0;
            goto done;
        }
        char w[24][32];
        const int nw = prompt ? ct_wf_words(prompt, w, 24) : 0;
        size_t used = 0, e;
        char *sel = (char *)malloc(lim + 16);
        if (!sel) { snprintf(out, cap, "error: out of memory"); goto done; }
        size_t sl = 0;
        if (nw > 0) sl = ct_wf_pick(pg->txt, avail, w, nw, sel, lim, &used);
        if (sl > 0) {
            snprintf(out, cap, "%s: returned %zu bytes of the %zu-byte page text, the passages that mention the words of the prompt; %zu bytes were not returned. "
                     "Read more with offset=<n>, or ask for a word with find=\"word\".\n\n", hdr, used, total, total - used);
            o = strlen(out);
            memcpy(out + o, sel, sl); out[o + sl] = 0;
        } else {                                                  /* sem palavras no prompt ou nenhum trecho cita: o comeco da pagina */
            e = lim;
            while (e > lim / 2 && pg->txt[e] != '\n') e--;
            if (pg->txt[e] != '\n') e = lim;
            while (e > 0 && ((unsigned char)pg->txt[e] & 0xC0) == 0x80) e--;
            snprintf(out, cap, "%s: returned the first %zu of %zu bytes of the page text; %zu bytes were not returned. "
                     "Continue with offset=%zu, or ask for a word with find=\"word\".\n\n", hdr, e, total, total - e, e);
            o = strlen(out);
            memcpy(out + o, pg->txt, e); out[o + e] = 0;
        }
        free(sel);
    }
done:
    free(url); free(prompt); free(find);
}
