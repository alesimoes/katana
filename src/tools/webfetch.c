/* ---------- ferramenta WebFetch (ver docs/tools.md) ----------
 * url (obrigatorio) e prompt (o que extrair). O papel do modelo pequeno que responde o prompt fica com o digest do
 * agente (depois do despacho); aqui a ferramenta devolve o texto da pagina que cabe em cap, com um cabecalho de uma
 * linha, e quando a pagina nao cabe escolhe os trechos que citam as palavras do prompt.
 * Extras do katana, opcionais: offset, find, links, render (passam para o web_fetch antigo). */
static const char CT_WEBFETCH_DESC[] =
    "Fetches a web page and returns its content as text (HTML converted, at most the first 100000 characters read). "
    "Give the url and a prompt saying what you want from the page; when the page is long, the passages that mention the "
    "words of the prompt are returned first and the result says how much was not read. http:// is upgraded to https://. "
    "Local addresses (localhost, hosts without a dot) are refused: use curl through Bash for local servers. If the URL "
    "redirects to another host the redirect is reported and you must fetch the new URL yourself. Without offset the "
    "result is short (about 4000 bytes): the passages that match the prompt (or, without a prompt, the page title and "
    "your last search), with the page length and how to read more; pages are cached for 24 hours. Optional extras: offset (continue a long page), find (only passages with a word), links (list the "
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


/* ---------- cache em disco (entre sessoes) e leitura de varias paginas ao mesmo tempo ----------
 * Pasta: $KATANA_CACHE_DIR/web ou ~/.katana/cache/web. Um arquivo por chave (hash FNV-1a do texto): buscas valem 6 horas,
 * paginas 24 horas; passando de 50 MB os mais antigos saem (uma vez por processo). KATANA_WEB_CACHE=0 desliga.
 * Arquivo: "KW1 <chave>\n" e depois o conteudo. Pagina: conteudo = "<tn> <ln> <nlinks>\n" + texto + links. */
#ifndef _WIN32
#include <poll.h>
#include <errno.h>
#endif
#define CT_WC_BUSCA 21600
#define CT_WC_PAGINA 86400
#define CT_WC_MAX (50L * 1024 * 1024)
#define CT_WF_DEF 4000             /* resposta padrao do WebFetch (sem offset): ate isto de trechos */
static char g_ct_lastq[400];       /* ultima consulta do WebSearch: dica para escolher trechos de uma pagina */

static int ct_wc_on(void) { const char *e = getenv("KATANA_WEB_CACHE"); return !(e && atoi(e) == 0 && *e); }
static int ct_web_local_ok(void) { const char *e = getenv("KATANA_WEB_ALLOW_LOCAL"); return e && *e && atoi(e) != 0; }   /* gancho de teste: servidor local */
#ifndef _WIN32
static void ct_wc_path(char kind, const char *key, char *out, size_t cap) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (const char *p = key; *p; p++) { h ^= (unsigned char)*p; h *= 0x100000001b3ULL; }
    char dir[900];
    const char *over = getenv("KATANA_CACHE_DIR");
    if (over && *over) snprintf(dir, sizeof dir, "%s/web", over);
    else { char home[700]; agent_home(home, sizeof home); snprintf(dir, sizeof dir, "%s/.katana/cache/web", home); }
    snprintf(out, cap, "%s/%c-%016llx", dir, kind, (unsigned long long)h);
}
/* conteudo guardado (malloc, terminado em zero) se existir, a chave bater e tiver menos de ttl segundos; NULL senao */
static char *ct_wc_read(char kind, const char *key, int ttl, size_t *len) {
    if (!ct_wc_on() || strlen(key) > 1100) return NULL;
    char p[1100];
    ct_wc_path(kind, key, p, sizeof p);
    struct stat st;
    if (stat(p, &st) != 0 || time(NULL) - st.st_mtime > ttl || st.st_size < 8 || st.st_size > 20L * 1024 * 1024) return NULL;
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    char *b = (char *)malloc((size_t)st.st_size + 1);
    if (!b) { fclose(f); return NULL; }
    const size_t n = fread(b, 1, (size_t)st.st_size, f);
    fclose(f);
    b[n] = 0;
    char hd[1200];
    const int hl = snprintf(hd, sizeof hd, "KW1 %s\n", key);
    if (n < (size_t)hl || memcmp(b, hd, (size_t)hl) != 0) { free(b); return NULL; }
    memmove(b, b + hl, n - (size_t)hl + 1);
    if (len) *len = n - (size_t)hl;
    return b;
}
/* tira os arquivos mais antigos quando a pasta passa do limite */
static void ct_wc_trim(const char *dir) {
    typedef struct { char name[64]; time_t t; long sz; } E;
    E *e = (E *)malloc(sizeof(E) * 4096);
    if (!e) return;
    int n = 0;
    long total = 0;
    DIR *d = opendir(dir);
    if (!d) { free(e); return; }
    struct dirent *de;
    while ((de = readdir(d)) && n < 4096) {
        if (de->d_name[0] == '.' || strlen(de->d_name) > 60) continue;
        char p[1200]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (stat(p, &st) != 0) continue;
        snprintf(e[n].name, sizeof e[n].name, "%s", de->d_name); e[n].t = st.st_mtime; e[n].sz = (long)st.st_size;
        total += e[n].sz; n++;
    }
    closedir(d);
    while (total > CT_WC_MAX * 4 / 5 && n > 0) {         /* ate 40 MB: apaga o mais velho de cada vez */
        int o = 0;
        for (int i = 1; i < n; i++) if (e[i].t < e[o].t) o = i;
        char p[1200]; snprintf(p, sizeof p, "%s/%s", dir, e[o].name);
        remove(p);
        total -= e[o].sz; e[o] = e[--n];
    }
    free(e);
}
static void ct_wc_write(char kind, const char *key, const char *a, size_t an, const char *b, size_t bn) {
    static int trimmed;
    if (!ct_wc_on() || strlen(key) > 1100 || an + bn > 20L * 1024 * 1024) return;
    char p[1100], tmp[1200];
    ct_wc_path(kind, key, p, sizeof p);
    mkdir_parents(p);
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", p, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fprintf(f, "KW1 %s\n", key);
    if (an) fwrite(a, 1, an, f);
    if (bn) fwrite(b, 1, bn, f);
    if (fclose(f) != 0 || rename(tmp, p) != 0) { remove(tmp); return; }
    if (!trimmed) { trimmed = 1; char dir[1100]; snprintf(dir, sizeof dir, "%s", p); char *s = strrchr(dir, '/'); if (s) { *s = 0; ct_wc_trim(dir); } }
}
#else
static char *ct_wc_read(char kind, const char *key, int ttl, size_t *len) { (void)kind; (void)key; (void)ttl; (void)len; return NULL; }
static void ct_wc_write(char kind, const char *key, const char *a, size_t an, const char *b, size_t bn) { (void)kind; (void)key; (void)a; (void)an; (void)b; (void)bn; }
#endif
/* pagina do disco para a memoria das paginas (wn_put); 0 se nao ha copia valida */
static WnPage *ct_wc_page_load(const char *url) {
    size_t len = 0;
    char *b = ct_wc_read('p', url, CT_WC_PAGINA, &len);
    if (!b) return NULL;
    size_t tn = 0, ln = 0; int nl = 0, hl = 0;
    if (sscanf(b, "%zu %zu %d%n", &tn, &ln, &nl, &hl) != 3 || b[hl] != '\n' || (size_t)hl + 1 + tn + ln > len) { free(b); return NULL; }
    char *txt = (char *)malloc(tn + 1), *lk = (char *)malloc(WN_LINKS_BYTES > ln + 1 ? WN_LINKS_BYTES : ln + 1);
    if (!txt || !lk) { free(b); free(txt); free(lk); return NULL; }
    memcpy(txt, b + hl + 1, tn); txt[tn] = 0;
    memcpy(lk, b + hl + 1 + tn, ln); lk[ln] = 0;
    free(b);
    WnPage *pg = wn_put(url, txt, tn, lk, ln, nl);
    *ct_wf_at(url, 1) = time(NULL);
    return pg;
}
static void ct_wc_page_store(const WnPage *pg) {
    if (!ct_wc_on() || !pg->txt) return;
    char hd[80];
    const int hl = snprintf(hd, sizeof hd, "%zu %zu %d\n", pg->tn, pg->ln, pg->nlinks);
    size_t an = (size_t)hl + pg->tn;
    char *a = (char *)malloc(an + 1);
    if (!a) return;
    memcpy(a, hd, (size_t)hl); memcpy(a + hl, pg->txt, pg->tn);
    ct_wc_write('p', pg->url, a, an, pg->links ? pg->links : "", pg->links ? pg->ln : 0);
    free(a);
}

/* ---- varias paginas ao mesmo tempo: todos os curl abrem juntos, a leitura espera todos com poll ---- */
typedef struct { char *buf; long n; FILE *f; int fd, done, rc; } CtWsGet;
#define CT_WS_RAW 2500000L
/* baixa urls[0..n) ao mesmo tempo; cada uma com no maximo per segundos, tudo com no maximo total. g[i].n > 0: baixou */
static void ct_ws_get_all(const char **urls, int n, CtWsGet *g, int per, double total) {
    const double t0 = now_sec();
    for (int i = 0; i < n; i++) {
        memset(&g[i], 0, sizeof g[i]);
        g[i].fd = -1;
        g[i].buf = (char *)malloc(CT_WS_RAW + 1);
        g[i].f = g[i].buf ? curl_open(urls[i], per) : NULL;
        if (g[i].f) { g[i].fd = fileno(g[i].f); } else g[i].done = 1;
    }
#ifndef _WIN32
    for (;;) {
        struct pollfd pf[8]; int map[8], np = 0;
        for (int i = 0; i < n && i < 8; i++) if (!g[i].done) { pf[np].fd = g[i].fd; pf[np].events = POLLIN; pf[np].revents = 0; map[np++] = i; }
        const double left = total - (now_sec() - t0);
        if (!np || left <= 0) break;
        if (poll(pf, (nfds_t)np, (int)(left * 1000) + 1) <= 0) break;
        for (int k = 0; k < np; k++) {
            if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            CtWsGet *p = &g[map[k]];
            const ssize_t r = read(p->fd, p->buf + p->n, (size_t)(CT_WS_RAW - p->n));
            if (r > 0) { p->n += (long)r; if (p->n >= CT_WS_RAW) p->done = 1; }
            else if (r == 0 || errno != EINTR) p->done = 1;
        }
    }
    for (int i = 0; i < n; i++) {
        if (!g[i].done && g[i].f) g[i].n = 0;           /* lenta demais: sem trechos */
        if (g[i].f) { g[i].rc = pclose(g[i].f); g[i].f = NULL; }
        if (g[i].rc != 0) g[i].n = 0;           /* curl falhou ou estourou o tempo: pagina cortada nao serve */
        if (g[i].buf) g[i].buf[g[i].n > 0 ? g[i].n : 0] = 0;
    }
#else
    for (int i = 0; i < n; i++) if (g[i].f) { g[i].n = curl_finish(g[i].f, g[i].buf, (size_t)CT_WS_RAW, &g[i].rc); g[i].f = NULL; }
#endif
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

/* quantas palavras distintas aparecem no texto (sem a linha de titulo "# ...") */
static int ct_wf_score(const char *t, char w[][32], int nw) {
    if (t[0] == '#') t += strcspn(t, "\n");
    const size_t n = strlen(t);
    int sc = 0;
    for (int k = 0; k < nw; k++) if (wn_cifind(t, n, w[k])) sc++;
    return sc;
}
/* ultima linha do resultado: tempo de rede e bytes devolvidos (o log do agente mostra onde o tempo vai; "[web:" tambem
   avisa o tool_digest de que o resultado ja foi enxugado) */
static void ct_web_foot(char *out, size_t cap, double net, int cached, size_t body) {
    const size_t l = strlen(out);
    if (l + 160 >= cap) return;
    if (cached) snprintf(out + l, cap - l, "\n[web: from cache, %zu bytes returned]", body);
    else snprintf(out + l, cap - l, "\n[web: network %.1f s, %zu bytes returned]", net, body);
}

static void ct_webfetch(const char *js, const char *end, char *out, size_t cap) {
    int okres = 0, from_cache = 0;
    double net = 0;
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
    if (ci_ncmp(url, "http://", 7) == 0 && !ct_web_local_ok()) { snprintf(u2, sizeof u2, "https://%s", url + 7); upgraded = 1; }
    else if (ci_ncmp(url, "https://", 8) == 0 || (ci_ncmp(url, "http://", 7) == 0 && ct_web_local_ok())) snprintf(u2, sizeof u2, "%s", url);   /* http local so no gancho de teste */
    else { snprintf(out, cap, "error: invalid URL \"%.200s\": it must start with http:// or https://", url); goto done; }
    if (!ct_web_host(u2, host, sizeof host)) { snprintf(out, cap, "error: invalid URL \"%.200s\": no host", url); goto done; }
    {
        const size_t hl = strlen(host);
        const int loop = strcmp(host, "localhost") == 0 || (hl > 10 && strcmp(host + hl - 10, ".localhost") == 0) || strncmp(host, "127.", 4) == 0;
        if ((loop || !strchr(host, '.')) && !ct_web_local_ok()) {
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
            const int rnd = ct_bool(js, end, "render");
            if (!rnd && !wn_get(u2) && ct_wc_page_load(u2)) from_cache = 1;
            const int had = wn_get(u2) != NULL;
            char *tmp = (char *)malloc(cap);
            if (!tmp) { snprintf(out, cap, "error: out of memory"); goto done; }
            const double t1 = now_sec();
            tool_web_fetch(j, j + n, tmp, cap > 400 ? cap - 400 : cap);
            net = now_sec() - t1;
            if (strncmp(tmp, "error:", 6) == 0) snprintf(out, cap, "%s%s", tmp, upgraded ? " (fetched after upgrading http to https)" : "");
            else {
                time_t *t = ct_wf_at(u2, 1); if (!*t) *t = time(NULL);
                snprintf(out, cap, "%s\n\n%s", hdr, tmp);
                WnPage *np = wn_get(u2);
                if (!had && np) ct_wc_page_store(np);
                okres = 1;
            }
            free(tmp);
            if (had) from_cache = 1;
            goto done;
        }
        ct_wf_expire(u2);
        WnPage *pg = wn_get(u2);
        if (pg) from_cache = 1;
        else if ((pg = ct_wc_page_load(u2))) from_cache = 1;
        if (!pg) {
            char err[1200];
            g_wn_render = 0;
            const double t1 = now_sec();
            pg = wn_load(u2, err, sizeof err);
            net = now_sec() - t1;
            if (!pg) { snprintf(out, cap, "%s%s", err, upgraded ? " (fetched after upgrading http to https)" : ""); goto done; }
            *ct_wf_at(u2, 1) = time(NULL);
            ct_wc_page_store(pg);
        }
        if (!pg->tn) { snprintf(out, cap, "%s\n\n(the page has no readable text, even after running its JavaScript when a browser is installed; it probably needs a login. Try another source)", hdr); goto done; }
        const size_t total = pg->tn, avail = total < CT_WF_READ ? total : CT_WF_READ;
        size_t lim = cap > 700 ? cap - 700 : cap / 2;
        if (g_budget >= 500 && lim > g_budget) lim = g_budget;
        if (lim > CT_WF_DEF) lim = CT_WF_DEF;                     /* resposta curta por padrao: mais so com offset */
        size_t o;
        if (avail <= lim) {
            snprintf(out, cap, "%s: returned all %zu bytes of the page text%s\n\n", hdr, avail, total > avail ? " (the first 100000 read; the rest was not read)" : "");
            o = strlen(out);
            memcpy(out + o, pg->txt, avail); out[o + avail] = 0;
            okres = 1;
            goto done;
        }
        char w[24][32];
        int nw = prompt ? ct_wf_words(prompt, w, 24) : 0;
        size_t used = 0, e;
        char *sel = (char *)malloc(lim + 16);
        if (!sel) { snprintf(out, cap, "error: out of memory"); goto done; }
        size_t sl = 0;
        const char *what = "the words of the prompt";
        if (nw > 0) sl = ct_wf_pick(pg->txt, avail, w, nw, sel, lim, &used);
        if (sl == 0 || ct_wf_score(sel, w, nw) == 0) {       /* nenhum trecho cita o prompt (so o titulo entrou) */                               /* sem prompt util: palavras do titulo da pagina e da ultima busca */
            char hint[800] = "";
            if (pg->txt[0] == '#') { size_t tl = strcspn(pg->txt, "\n"); if (tl > 300) tl = 300; snprintf(hint, sizeof hint, "%.*s ", (int)tl, pg->txt); }
            { const size_t hl = strlen(hint); snprintf(hint + hl, sizeof hint - hl, "%s", g_ct_lastq); }
            nw = ct_wf_words(hint, w, 24);
            what = "the words of the page title and of your last search";
            sl = 0; used = 0;
            if (nw > 0) sl = ct_wf_pick(pg->txt, avail, w, nw, sel, lim, &used);
            if (sl > 0 && ct_wf_score(sel, w, nw) == 0) sl = 0;
        }
        if (sl > 0) {
            snprintf(out, cap, "%s: returned %zu bytes of the %zu-byte page text, the passages that mention %s; %zu bytes were not returned. "
                     "Read more with offset=<n>, or ask for a word with find=\"word\".\n\n", hdr, used, total, what, total - used);
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
        okres = 1;
    }
done:
    if (okres) ct_web_foot(out, cap, net, from_cache, strlen(out));
    free(url); free(prompt); free(find);
}
