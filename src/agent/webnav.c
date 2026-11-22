/* ---------- navegacao na web: web_fetch com memoria da pagina ----------
 * Uma pagina buscada fica guardada (texto limpo e links, as 8 ultimas). Em cima disso o modelo navega sem baixar de
 * novo: offset continua uma pagina longa, find devolve so os trechos que citam um termo, links=true lista para onde a
 * pagina leva (URLs absolutas). O texto vem do conteudo principal (<main> ou <article>) quando a pagina marca um,
 * com o titulo na primeira linha; menus e rodapes ficam de fora. */
#define WN_PAGES 8
#define WN_LINKS_MAX 300
#define WN_LINKS_BYTES 60000
typedef struct { char url[1024]; char *txt, *links; size_t tn, ln; int nlinks; unsigned long use; } WnPage;
static WnPage g_wn[WN_PAGES];
static unsigned long g_wn_clock;

static int json_int(const char *js, const char *end, const char *key, int def);
static int json_bool(const char *js, const char *end, const char *key);
static int shq(const char *s, char *o, size_t cap);
#ifndef _WIN32
static char *run_collect(const char *cmd, int tmo, size_t maxb, int *rc);
#endif

/* primeira ocorrencia de `needle` em [h, h+n) sem diferenciar maiusculas (ASCII); NULL se nao houver */
static const char *wn_cifind(const char *h, size_t n, const char *needle) {
    const size_t nl = strlen(needle);
    if (!nl || n < nl) return NULL;
    for (size_t i = 0; i + nl <= n; i++)
        if (tolower((unsigned char)h[i]) == tolower((unsigned char)needle[0]) && ci_ncmp(h + i, needle, nl) == 0) return h + i;
    return NULL;
}
/* abertura de <tag ...> a partir de p; exige que depois do nome venha espaco, > ou / */
static const char *wn_tag(const char *p, const char *e, const char *tag) {
    const size_t tl = strlen(tag);
    for (const char *q = p; (q = wn_cifind(q, (size_t)(e - q), tag)); q += tl) {
        const char c = q + tl < e ? q[tl] : 0;
        if (c == ' ' || c == '>' || c == '/' || c == '\n' || c == '\t' || c == '\r') return q;
    }
    return NULL;
}
static const char *wn_last(const char *p, const char *e, const char *s) {
    const char *r = NULL;
    for (const char *q = p; (q = wn_cifind(q, (size_t)(e - q), s)); q++) r = q;
    return r;
}
/* trecho do HTML com o conteudo principal: de <main> a </main> ou do primeiro <article> ao ultimo </article> */
static int wn_main(const char *raw, size_t n, const char **s, const char **e) {
    const char *end = raw + n, *a, *b;
    if ((a = wn_tag(raw, end, "<main")) && (b = wn_last(a, end, "</main")) && b - a > 2000) { *s = a; *e = b; return 1; }
    if ((a = wn_tag(raw, end, "<article")) && (b = wn_last(a, end, "</article")) && b - a > 2000) { *s = a; *e = b; return 1; }
    return 0;
}

/* URL absoluta de href em relacao a base; 0 se nao for um link de pagina */
static int wn_resolve(const char *base, const char *href, char *out, size_t cap) {
    while (*href == ' ' || *href == '\n' || *href == '\t') href++;
    if (!*href || *href == '#' || ci_ncmp(href, "javascript:", 11) == 0 || ci_ncmp(href, "mailto:", 7) == 0 || ci_ncmp(href, "tel:", 4) == 0 || ci_ncmp(href, "data:", 5) == 0) return 0;
    char b[1024];
    snprintf(b, sizeof b, "%s", base);
    b[strcspn(b, "#?")] = 0;
    const char *sch = strstr(b, "://");
    if (!sch) return 0;
    const char *host = sch + 3;
    const size_t hostend = (size_t)(host - b) + strcspn(host, "/");
    if (ci_ncmp(href, "http://", 7) == 0 || ci_ncmp(href, "https://", 8) == 0) snprintf(out, cap, "%s", href);
    else if (href[0] == '/' && href[1] == '/') snprintf(out, cap, "%.*s:%s", (int)(sch - b), b, href);
    else if (href[0] == '/') snprintf(out, cap, "%.*s%s", (int)hostend, b, href);
    else if (strchr(href, ':') && strcspn(href, ":") < strcspn(href, "/?#")) return 0;      /* outro esquema */
    else {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", b);
        char *sl = strrchr(dir + hostend, '/');
        if (sl) sl[1] = 0; else { const size_t l = strlen(dir); if (l + 2 < sizeof dir) { dir[l] = '/'; dir[l + 1] = 0; } }
        while (strncmp(href, "./", 2) == 0) href += 2;
        while (strncmp(href, "../", 3) == 0) {
            href += 3;
            const size_t l = strlen(dir);
            if (l > hostend + 1) { dir[l - 1] = 0; char *u = strrchr(dir + hostend, '/'); if (u) u[1] = 0; else { dir[l - 1] = '/'; } }
        }
        snprintf(out, cap, "%s%s", dir, href);
    }
    out[strcspn(out, " \n\r\t\"'<>")] = 0;
    for (char *q = out; (q = strstr(q, "&amp;")); q++) memmove(q + 1, q + 5, strlen(q + 5) + 1);
    return out[0] != 0;
}

/* links da pagina: linhas "texto<TAB>url"; sem repetidos; devolve quantos */
static int wn_links(const char *raw, size_t n, const char *base, char *out, size_t cap, size_t *len) {
    const char *end = raw + n;
    size_t o = 0;
    int k = 0;
    out[0] = 0;
    for (const char *a = raw; k < WN_LINKS_MAX && (a = wn_tag(a, end, "<a")); a += 2) {
        const char *gt = (const char *)memchr(a, '>', (size_t)(end - a));
        if (!gt) break;
        const char *h = wn_cifind(a, (size_t)(gt - a), "href");
        if (!h) continue;
        h += 4;
        while (h < gt && (*h == ' ' || *h == '=')) h++;
        char q = 0, href[900], url[1024], text[160], line[1300];
        if (h < gt && (*h == '"' || *h == '\'')) q = *h++;
        size_t hl = 0;
        while (h < gt && hl + 1 < sizeof href && (q ? *h != q : (*h != ' ' && *h != '>'))) href[hl++] = *h++;
        href[hl] = 0;
        if (!wn_resolve(base, href, url, sizeof url)) continue;
        const char *ce = wn_cifind(gt, (size_t)(end - gt), "</a");
        if (!ce || ce - gt > 3000) continue;
        html_snip(gt + 1, ce, text, sizeof text);
        if (!text[0]) continue;
        snprintf(line, sizeof line, "\t%s\n", url);
        if (strstr(out, line)) continue;                 /* mesma URL ja listada */
        const int w = snprintf(line, sizeof line, "%s\t%s\n", text, url);
        if (o + (size_t)w + 1 >= cap) break;
        memcpy(out + o, line, (size_t)w + 1); o += (size_t)w;
        k++;
    }
    *len = o;
    return k;
}

static WnPage *wn_get(const char *url) {
    for (int i = 0; i < WN_PAGES; i++) if (g_wn[i].txt && strcmp(g_wn[i].url, url) == 0) { g_wn[i].use = ++g_wn_clock; return &g_wn[i]; }
    return NULL;
}
static int g_wn_render;     /* 1: web_fetch pediu render=true (a pagina depois de o JavaScript rodar) */
/* a pagina como um navegador a ve: Chrome ou Chromium instalado, sem janela, perfil temporario, devolve o DOM depois
   de o JavaScript rodar. NULL sem navegador ou em falha. Nao faz login nem clica: so carrega e le. */
static char *wn_render(const char *url, size_t max, long *n) {
#ifndef _WIN32
    static char q[2200], cmd[3400];
    int rc = 0;
    if (!shq(url, q, sizeof q)) return NULL;
    snprintf(cmd, sizeof cmd, "for b in google-chrome google-chrome-stable chromium chromium-browser microsoft-edge brave-browser; do "
             "command -v $b >/dev/null 2>&1 || continue; d=$(mktemp -d) || exit 1; "
             "timeout 40 $b --headless=new --disable-gpu --no-first-run --no-default-browser-check --disable-extensions --user-data-dir=\"$d\" "
             "--virtual-time-budget=8000 --dump-dom %s 2>/dev/null; r=$?; rm -rf \"$d\"; exit $r; done; exit 127", q);
    fprintf(stderr, "%s[web: abrindo a pagina num navegador sem janela para rodar o JavaScript...]%s\n", CE(C_DIM), CE(C_RESET));
    char *o = run_collect(cmd, 50, max, &rc);
    if (!o) return NULL;
    if (rc != 0 || strlen(o) < 200) { free(o); return NULL; }
    *n = (long)strlen(o);
    return o;
#else
    (void)url; (void)max; (void)n; return NULL;
#endif
}
/* guarda texto e links ja prontos (de uma conversao ou do cache em disco); toma posse de txt e lk */
static WnPage *wn_put(const char *url, char *txt, size_t tn, char *lk, size_t ln, int nl) {
    int v = 0;
    for (int i = 0; i < WN_PAGES; i++) { if (!g_wn[i].txt) { v = i; break; } if (g_wn[i].use < g_wn[v].use) v = i; }
    for (int i = 0; i < WN_PAGES; i++) if (g_wn[i].txt && strcmp(g_wn[i].url, url) == 0) v = i;      /* recarga: troca a copia guardada */
    free(g_wn[v].txt); free(g_wn[v].links);
    snprintf(g_wn[v].url, sizeof g_wn[v].url, "%s", url);
    g_wn[v].txt = txt; g_wn[v].tn = tn; g_wn[v].links = lk; g_wn[v].ln = ln; g_wn[v].nlinks = nl; g_wn[v].use = ++g_wn_clock;
    return &g_wn[v];
}
static WnPage *wn_ingest(const char *url, char *raw, long n, int rendered, int allow_render, char *err, size_t ecap);
static int ct_set2(void);      /* agent/prompt.c: 1 = ferramentas atuais (Read, Bash...), 0 = conjunto antigo */
/* baixa a pagina, converte e guarda; em erro escreve a mensagem em err e devolve NULL */
static WnPage *wn_load(const char *url, char *err, size_t ecap) {
    const size_t RAW = 2500000;
    char *raw = (char *)malloc(RAW + 1);
    int rc;
    const int two = ct_set2();      /* as mensagens citam as ferramentas do conjunto em uso */
    if (!raw) { snprintf(err, ecap, "error: out of memory"); return NULL; }
    long n = curl_get(url, raw, RAW, &rc);
    int rendered = 0;
    if (n >= 0 && g_curl_http >= 400) {      /* a pagina de erro do site nao e o conteudo pedido: vira erro, nao texto */
        const int c = g_curl_http;
        snprintf(err, ecap, "error: HTTP %d (%s) from %.300s: %s. Nothing from this page can be used. %s", c, http_reason(c), url,
                 c == 404 || c == 410 ? "there is no page at this address" : c == 401 || c == 403 ? "the site refuses this request (login or permission needed)"
                 : c == 429 ? "the site is limiting requests" : c >= 500 ? "the site failed" : "the site rejected the request",
                 c == 404 || c == 410 ? (two ? "Check the address, or find the right one with WebSearch" : "Check the address, or find the right one with web_search")
                 : c >= 500 || c == 429 ? "Try again later or use another source" : "Try another source");
        free(raw); return NULL;
    }
    if (n != -2 && (n <= 0 || g_wn_render)) {     /* pedido explicito, ou o curl nao trouxe nada (bloqueio a clientes sem navegador) */
        long rn = 0; char *r = wn_render(url, RAW, &rn);
        if (r) { memcpy(raw, r, (size_t)rn); raw[rn] = 0; n = rn; rendered = 1; free(r); }
    }
    if (n <= 0) {
        if (n == -2) snprintf(err, ecap, "error: invalid URL (only http and https; no control characters)");
        else if (n < 0) snprintf(err, ecap, "error: cannot run curl");
        else snprintf(err, ecap, "error: no content received (curl exit code %d: network failure, timeout, blocked or file too large). Try another source from %s, or curl -sSL -D- <url> with %s to see the HTTP status", rc, two ? "WebSearch" : "web_search", two ? "Bash" : "run_command");
        free(raw); return NULL;
    }
    return wn_ingest(url, raw, n, rendered, 1, err, ecap);
}
/* converte o HTML baixado (raw, de tamanho RAW+1, n bytes; toma posse) e guarda a pagina. allow_render 0: nunca abre o
   navegador (usado nas leituras em paralelo da busca) */
static WnPage *wn_ingest(const char *url, char *raw, long n, int rendered, int allow_render, char *err, size_t ecap) {
    const size_t RAW = 2500000;
    char *txt = (char *)malloc(RAW + 600), *lk = (char *)malloc(WN_LINKS_BYTES);
    if (!txt || !lk) { free(raw); free(txt); free(lk); snprintf(err, ecap, "error: out of memory"); return NULL; }
    if (n >= 4 && memcmp(raw, "%PDF", 4) == 0) {
        snprintf(err, ecap, "error: this URL is a PDF, not a page. Save it with download_file and read it with %s", ct_set2() ? "Read" : "read_document");
        free(raw); free(txt); free(lk); return NULL;
    }
    size_t tn = 0, ln = 0;
    int nl = 0;
    const size_t probe = (size_t)n < 2048 ? (size_t)n : 2048;
again:
    if (memchr(raw, '<', probe)) {
        const char *s, *e, *t0 = wn_tag(raw, raw + n, "<title"), *t1 = t0 ? wn_cifind(t0, (size_t)(raw + n - t0), "</title") : NULL;
        char title[300] = "";
        if (t0 && t1 && t1 - t0 < 2000) { const char *g = (const char *)memchr(t0, '>', (size_t)(t1 - t0)); if (g) html_snip(g + 1, t1, title, sizeof title); }
        size_t hd = title[0] ? (size_t)snprintf(txt, 600, "# %s\n\n", title) : 0;
        g_html_fold = 1;
        if (wn_main(raw, (size_t)n, &s, &e)) tn = html_to_text(s, (size_t)(e - s), txt + hd);
        if (tn < 400) { hd = 0; tn = html_to_text(raw, (size_t)n, txt); }  /* sem conteudo principal marcado (ou vazio): a pagina inteira, que ja traz o titulo */
        g_html_fold = 0;
        tn += hd;
        if (allow_render && !rendered && tn < 500 && wn_cifind(raw, (size_t)n, "<script")) {     /* quase sem texto e com scripts: a pagina se monta no navegador */
            long rn = 0; char *r = wn_render(url, RAW, &rn);
            rendered = 1;
            if (r) { memcpy(raw, r, (size_t)rn); raw[rn] = 0; n = rn; free(r); tn = 0; goto again; }
        }
        nl = wn_links(raw, (size_t)n, url, lk, WN_LINKS_BYTES, &ln);
    } else { memcpy(txt, raw, (size_t)n); txt[n] = 0; tn = (size_t)n; lk[0] = 0; }
    free(raw);
    return wn_put(url, txt, tn, lk, ln, nl);
}

static void tool_web_fetch(const char *js, const char *end, char *out, size_t cap) {
    static char url[1024], find[200];
    if (!json_str(js, end, "url", url, sizeof url) || !url[0]) { snprintf(out, cap, "error: missing url"); return; }
    if (!json_str(js, end, "find", find, sizeof find)) find[0] = 0;
    const int want_links = json_bool(js, end, "links");
    long off = json_int(js, end, "offset", 0);
    g_wn_render = json_bool(js, end, "render");
    WnPage *pg = g_wn_render ? NULL : wn_get(url);
    if (!pg) pg = wn_load(url, out, cap);
    g_wn_render = 0;
    if (!pg) return;
    size_t lim = cap > 600 ? cap - 600 : cap;
    if (lim > 12000) lim = 12000;
    if (lim > g_budget) lim = g_budget;
    size_t o = 0;
    if (want_links) {                                    /* para onde a pagina leva */
        int shown = 0, match = 0;
        o += (size_t)snprintf(out + o, cap - o, "Links on %s (%d found%s%s%s):\n", url, pg->nlinks, find[0] ? ", filter \"" : "", find, find[0] ? "\"" : "");
        for (const char *p = pg->links; *p; ) {
            const char *e = strchr(p, '\n'), *tab = (const char *)memchr(p, '\t', (size_t)(e - p));
            if (!find[0] || wn_cifind(p, (size_t)(e - p), find)) {
                match++;
                if (shown < 80 && o + (size_t)(e - p) + 8 < lim) { o += (size_t)snprintf(out + o, cap - o, "- %.*s: %.*s\n", (int)(tab - p), p, (int)(e - tab - 1), tab + 1); shown++; }
            }
            p = e + 1;
        }
        if (!match) o += (size_t)snprintf(out + o, cap - o, "(no links%s)\n", find[0] ? " match the filter; call again without find to see all" : " found on this page");
        else if (shown < match) snprintf(out + o, cap - o, "[%d of %d shown; narrow it with find=\"word\"]", shown, match);
        return;
    }
    if (find[0]) {                                       /* so os trechos que citam o termo */
        int hits = 0;
        const char *p = pg->txt, *te = pg->txt + pg->tn;
        while (hits < 6 && o + 900 < lim && (p = wn_cifind(p, (size_t)(te - p), find))) {
            const char *a = p - 350 > pg->txt ? p - 350 : pg->txt, *b = p + 450 < te ? p + 450 : te;
            while (a > pg->txt && a[-1] != '\n' && p - a < 500) a--;         /* comeca no inicio da linha quando esta perto */
            while (b < te && *b != '\n' && b - p < 600) b++;
            while (a < p && ((unsigned char)*a & 0xC0) == 0x80) a++;
            while (b < te && ((unsigned char)*b & 0xC0) == 0x80) b++;
            o += (size_t)snprintf(out + o, cap - o, "[passage at offset %zu]\n%.*s\n\n", (size_t)(a - pg->txt), (int)(b - a), a);
            hits++;
            p = b;
        }
        if (!hits) snprintf(out, cap, "no passage mentions \"%s\" in this page (%zu bytes of text). Try another word, read the page from the start (no find), or list its links (links=true)", find, pg->tn);
        else snprintf(out + o, cap - o, "[%d passage%s with \"%s\"; to read around one, call web_fetch with offset=<that offset>]", hits, hits > 1 ? "s" : "", find);
        return;
    }
    if (off < 0) off = 0;
    if ((size_t)off >= pg->tn && pg->tn) { snprintf(out, cap, "error: offset %ld is past the end of the page text (%zu bytes)", off, pg->tn); return; }
    const char *a = pg->txt + off, *te = pg->txt + pg->tn;
    while (a < te && ((unsigned char)*a & 0xC0) == 0x80) a++;
    const char *b = (size_t)(te - a) > lim ? a + lim : te;
    if (b < te) {
        const char *cut = b;
        while (cut > a + lim / 2 && *cut != '\n') cut--;            /* corta numa quebra de linha quando ha uma por perto */
        if (*cut == '\n') b = cut; else while (b > a && ((unsigned char)*b & 0xC0) == 0x80) b--;
    }
    o = (size_t)snprintf(out, cap, "%.*s", (int)(b - a), a);
    if (!pg->tn) snprintf(out, cap, "(the page has no readable text, even after running its JavaScript when a browser is installed; it probably needs a login. Try another source)");
    else if (b < te || off > 0)
        snprintf(out + o, cap - o, b < te ? "\n[page text bytes %zu-%zu of %zu. Continue: same url with offset=%zu. Jump to a subject: find=\"word\". See where the page leads: links=true]"
                                          : "\n[page text bytes %zu-%zu of %zu (end of page)]", (size_t)(a - pg->txt), (size_t)(b - pg->txt), pg->tn, (size_t)(b - pg->txt));
}
