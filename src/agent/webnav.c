/* ---------- navegacao na web: WebFetch com memoria da pagina ----------
 * Uma pagina buscada fica guardada (texto limpo e links, as 8 ultimas). Em cima disso o modelo navega sem baixar de
 * novo: offset continua uma pagina longa, find devolve so os trechos que citam um termo, links=true lista para onde a
 * pagina leva (URLs absolutas). O texto vem do conteudo principal (<main> ou <article>) quando a pagina marca um,
 * com o titulo na primeira linha; menus e rodapes ficam de fora. */
#define WN_PAGES 8
#define WN_LINKS_MAX 300
#define WN_LINKS_BYTES 60000
enum { WQ_OK = 0, WQ_SHELL = 1, WQ_EMPTY = 2 };      /* veredito do verificador de qualidade (wn_quality) */
typedef struct { char url[1024]; char *txt, *links; size_t tn, ln; int nlinks; unsigned long use; int verdict, quality; char route[24], tried[160]; } WnPage;
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
    for (int i = 0; i < WN_PAGES; i++) if (g_wn[i].txt && g_wn[i].verdict == WQ_OK && strcmp(g_wn[i].url, url) == 0) { g_wn[i].use = ++g_wn_clock; return &g_wn[i]; }
    return NULL;
}
/* a copia guardada do endereco com qualquer veredito (so quem acabou de ler a pagina usa; wn_get nunca serve shell nem empty) */
static WnPage *wn_peek(const char *url) {
    for (int i = 0; i < WN_PAGES; i++) if (g_wn[i].txt && strcmp(g_wn[i].url, url) == 0) return &g_wn[i];
    return NULL;
}
static int g_wn_no_search;     /* 1: o passo "outra fonte" da escada nao busca no roteador (o UIOE de antes do modelo so deixa sair o endereco do pedido; uioe/conn_web.c) */
static int g_wn_render;     /* 1: WebFetch pediu render=true (a pagina depois de o JavaScript rodar) */
static double g_wn_deadline;                /* hora (now_sec) em que a escada de rotas para de abrir rotas novas; 0: 60 s a partir da chamada */
#define WN_BUDGET 60.0                      /* tempo total de um WebFetch somando as rotas (docs/web-design.md 3.3) */
static char g_wn_errst[12], g_wn_errroute[16];   /* estado e rota da ultima falha de wn_load (login, blocked, error, empty); vazio se nao falhou */
static size_t g_wn_returned;                /* bytes do texto da pagina que o ultimo tool_web_fetch entregou */

/* a pagina como um navegador a ve: Chrome ou Chromium instalado, sem janela, perfil temporario, devolve o DOM depois
   de o JavaScript rodar. NULL sem navegador ou em falha. Nao faz login nem clica: so carrega e le. tmo: segundos no maximo */
static char *wn_render(const char *url, size_t max, long *n, int tmo) {
#ifndef _WIN32
    static char q[2200], cmd[3400];
    int rc = 0;
    if (!shq(url, q, sizeof q)) return NULL;
    snprintf(cmd, sizeof cmd, "for b in google-chrome google-chrome-stable chromium chromium-browser microsoft-edge brave-browser; do "
             "command -v $b >/dev/null 2>&1 || continue; d=$(mktemp -d) || exit 1; "
             "timeout %d $b --headless=new --disable-gpu --no-first-run --no-default-browser-check --disable-extensions --user-data-dir=\"$d\" "
             "--virtual-time-budget=8000 --dump-dom %s 2>/dev/null; r=$?; rm -rf \"$d\"; exit $r; done; exit 127", tmo, q);
    fprintf(stderr, "%s[web: abrindo a pagina num navegador sem janela para rodar o JavaScript...]%s\n", CE(C_DIM), CE(C_RESET));
    char *o = run_collect(cmd, tmo + 10, max, &rc);
    if (!o) return NULL;
    if (rc != 0 || strlen(o) < 200) { free(o); return NULL; }
    *n = (long)strlen(o);
    return o;
#else
    (void)url; (void)max; (void)n; (void)tmo; return NULL;
#endif
}
/* guarda texto e links ja prontos (de uma conversao ou do cache em disco); toma posse de txt e lk. O veredito comeca em ok
   e a rota em vazio: quem sabe mais (wn_ingest, o cache em disco) preenche */
static WnPage *wn_put(const char *url, char *txt, size_t tn, char *lk, size_t ln, int nl) {
    int v = 0;
    for (int i = 0; i < WN_PAGES; i++) { if (!g_wn[i].txt) { v = i; break; } if (g_wn[i].use < g_wn[v].use) v = i; }
    for (int i = 0; i < WN_PAGES; i++) if (g_wn[i].txt && strcmp(g_wn[i].url, url) == 0) v = i;      /* recarga: troca a copia guardada (uma shell e trocada por uma leitura melhor) */
    free(g_wn[v].txt); free(g_wn[v].links);
    snprintf(g_wn[v].url, sizeof g_wn[v].url, "%s", url);
    g_wn[v].txt = txt; g_wn[v].tn = tn; g_wn[v].links = lk; g_wn[v].ln = ln; g_wn[v].nlinks = nl; g_wn[v].use = ++g_wn_clock;
    g_wn[v].verdict = WQ_OK; g_wn[v].quality = 0; g_wn[v].route[0] = 0; g_wn[v].tried[0] = 0;
    return &g_wn[v];
}

/* ---------- verificador de qualidade (docs/web-design.md 3.2): deterministico, sem modelo ----------
 * Olha o texto que saiu da pagina e, quando ha, o HTML de onde veio: bytes de texto (sem a linha do titulo), raiz de aplicacao
 * vazia (id=app/root/__next/__nuxt sem filhos), <noscript> pedindo JavaScript, razao texto/HTML, texto so de linhas repetidas.
 * Veredito: ok, shell (a pagina existe mas o conteudo nao carregou) ou empty (nenhum texto). Os limites sao palpites ainda nao
 * medidos (docs/web-design.md secao 5). */
typedef struct { int verdict, score, root, noscript, boiler, html; size_t body; } WnQual;

/* raiz de aplicacao de uma pagina de JavaScript sem filhos: <div id="app"></div> */
static int wn_empty_root(const char *raw, size_t n) {
    static const char *const ids[] = {"id=\"app\"", "id=\"root\"", "id=\"__next\"", "id=\"__nuxt\"", "id='app'", "id='root'", NULL};
    for (int i = 0; ids[i]; i++) {
        const char *p = wn_cifind(raw, n, ids[i]);
        if (!p) continue;
        const size_t room = (size_t)(raw + n - p);
        const char *gt = (const char *)memchr(p, '>', room < 300 ? room : 300);
        if (!gt) continue;
        gt++;
        while (gt < raw + n && isspace((unsigned char)*gt)) gt++;
        if (gt + 1 < raw + n && gt[0] == '<' && gt[1] == '/') return 1;
    }
    return 0;
}
/* um <noscript> que pede JavaScript */
static int wn_noscript_js(const char *raw, size_t n) {
    const char *end = raw + n;
    int seen = 0;
    for (const char *q = raw; seen < 8 && (q = wn_tag(q, end, "<noscript")); q += 9, seen++) {
        size_t l = (size_t)(end - q);
        if (l > 800) l = 800;
        const char *e = wn_cifind(q, l, "</noscript");
        if (e) l = (size_t)(e - q);
        if (wn_cifind(q, l, "javascript") || wn_cifind(q, l, "enable js")) return 1;
    }
    return 0;
}
/* raw: o HTML de onde veio o texto (NULL se o texto ja era texto); txt/tn: o texto convertido */
static void wn_quality(const char *raw, size_t rawn, const char *txt, size_t tn, WnQual *q) {
    memset(q, 0, sizeof *q);
    const char *body = txt;
    size_t bl = tn;
    if (tn && txt[0] == '#') { const char *nl = (const char *)memchr(txt, '\n', tn); body = nl ? nl + 1 : txt + tn; bl = (size_t)(txt + tn - body); }
    for (size_t i = 0; i < bl; i++) if (!isspace((unsigned char)body[i])) q->body++;
    q->html = raw && rawn && memchr(raw, '<', rawn < 2048 ? rawn : 2048) != NULL;
    if (q->html) { q->root = wn_empty_root(raw, rawn); q->noscript = wn_noscript_js(raw, rawn); }
    if (q->html && bl > 400) {                               /* texto que e so a mesma linha varias vezes (menu, aviso de cookies) */
        enum { ML = 1500 };
        static uint64_t h[ML]; static unsigned lenv[ML];
        int nlines = 0;
        size_t sum = 0, rep = 0;
        for (size_t i = 0; i < bl && nlines < ML; ) {
            size_t j = i; uint64_t x = 1469598103934665603ULL;
            while (j < bl && body[j] != '\n') { x ^= (unsigned char)body[j]; x *= 1099511628211ULL; j++; }
            if (j - i >= 12) { h[nlines] = x; lenv[nlines] = (unsigned)(j - i); sum += j - i; nlines++; }
            i = j + 1;
        }
        for (int a = 0; a < nlines; a++) { int c = 0; for (int b = 0; b < nlines; b++) if (h[b] == h[a]) c++; if (c >= 4) rep += lenv[a]; }
        if (sum > 0 && rep * 2 > sum) q->boiler = 1;
    }
    if (q->body == 0) q->verdict = WQ_EMPTY;
    else if (!q->html) q->verdict = WQ_OK;                    /* texto puro (markdown, llms.txt): qualquer coisa que nao seja vazia */
    else if (q->body < 150) q->verdict = WQ_SHELL;
    else if (q->body < 600 && (q->root || q->noscript)) q->verdict = WQ_SHELL;
    else if (q->boiler) q->verdict = WQ_SHELL;
    else if (rawn > 30000 && q->body * 200 < rawn && q->body < 1500) q->verdict = WQ_SHELL;      /* menos de 0,5% do HTML e texto */
    int s = (int)(q->body / 40);
    if (s > 70) s = 70;
    s += 30 - 15 * (q->root + q->noscript + q->boiler);
    if (s < 0) s = 0;
    if (q->verdict != WQ_OK && s > 40) s = 40;
    if (q->verdict == WQ_EMPTY) s = 0;
    q->score = s;
}

/* HTML ou texto para o texto limpo e os links; txt tem RAW+600 bytes, lk WN_LINKS_BYTES. plain: o conteudo ja e texto (um
   gemeo markdown), nao passa pelo conversor de HTML. Devolve os bytes de texto */
static size_t wn_convert(const char *url, const char *raw, long n, int plain, char *txt, char *lk, size_t *ln, int *nl) {
    size_t tn = 0;
    const size_t probe = (size_t)n < 2048 ? (size_t)n : 2048;
    *ln = 0; *nl = 0; lk[0] = 0; txt[0] = 0;
    if (!plain && memchr(raw, '<', probe)) {
        const char *s, *e, *t0 = wn_tag(raw, raw + n, "<title"), *t1 = t0 ? wn_cifind(t0, (size_t)(raw + n - t0), "</title") : NULL;
        char title[300] = "";
        if (t0 && t1 && t1 - t0 < 2000) { const char *g = (const char *)memchr(t0, '>', (size_t)(t1 - t0)); if (g) html_snip(g + 1, t1, title, sizeof title); }
        size_t hd = title[0] ? (size_t)snprintf(txt, 600, "# %s\n\n", title) : 0;
        g_html_fold = 1;
        if (wn_main(raw, (size_t)n, &s, &e)) tn = html_to_text(s, (size_t)(e - s), txt + hd);
        if (tn < 400) { hd = 0; tn = html_to_text(raw, (size_t)n, txt); }  /* sem conteudo principal marcado (ou vazio): a pagina inteira, que ja traz o titulo */
        g_html_fold = 0;
        tn += hd;
        *nl = wn_links(raw, (size_t)n, url, lk, WN_LINKS_BYTES, ln);
    } else { memcpy(txt, raw, (size_t)n); txt[n] = 0; tn = (size_t)n; }
    return tn;
}

/* ---------- gemeos legiveis por maquina (docs/web-design.md 3.3, passo 3) ---------- */
/* href de <link rel="alternate" type="text/markdown"> no HTML */
static int wn_alt_link(const char *raw, size_t n, const char *base, char *out, size_t cap) {
    const char *end = raw + n;
    int seen = 0;
    for (const char *a = raw; seen < 40 && (a = wn_tag(a, end, "<link")); a += 5, seen++) {
        const char *gt = (const char *)memchr(a, '>', (size_t)(end - a));
        if (!gt) break;
        const size_t l = (size_t)(gt - a);
        if (!wn_cifind(a, l, "alternate") || !wn_cifind(a, l, "text/markdown")) continue;
        const char *h = wn_cifind(a, l, "href");
        if (!h) continue;
        h += 4;
        while (h < gt && (*h == ' ' || *h == '=')) h++;
        char q = 0, href[900];
        size_t hl = 0;
        if (h < gt && (*h == '"' || *h == '\'')) q = *h++;
        while (h < gt && hl + 1 < sizeof href && (q ? *h != q : (*h != ' ' && *h != '>'))) href[hl++] = *h++;
        href[hl] = 0;
        if (wn_resolve(base, href, out, cap)) return 1;
    }
    return 0;
}
/* cabecalho Link: <...>; rel="alternate"; type="text/markdown" (uma requisicao HEAD curta) */
static int wn_link_header(const char *url, char *out, size_t cap) {
#ifndef _WIN32
    char safe[2300], q[2500], cmd[3000];
    int rc = 0, found = 0;
    if (!url_safe(url, safe, sizeof safe) || !shq(safe, q, sizeof q)) return 0;
    snprintf(cmd, sizeof cmd, CURL_BIN " -sSI -L --max-time 6 --max-redirs 5 -A 'Mozilla/5.0 (compatible; katana-agent)' -- %s 2>/dev/null", q);
    char *h = run_collect(cmd, 10, 30000, &rc);
    if (!h) return 0;
    for (char *p = h; *p && !found; ) {
        char *e = strchr(p, '\n');
        const size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l > 6 && ci_ncmp(p, "link:", 5) == 0) {
            const char *line_end = p + l;
            for (const char *lt = (const char *)memchr(p, '<', l); lt && !found; ) {
                const char *gt = (const char *)memchr(lt, '>', (size_t)(line_end - lt));
                if (!gt) break;
                const char *nx = (const char *)memchr(gt, '<', (size_t)(line_end - gt));
                const size_t seg = (size_t)((nx ? nx : line_end) - gt);
                if (wn_cifind(gt, seg, "alternate") && wn_cifind(gt, seg, "text/markdown") && gt - lt < 900) {
                    char href[900];
                    snprintf(href, sizeof href, "%.*s", (int)(gt - lt - 1), lt + 1);
                    found = wn_resolve(url, href, out, cap);
                }
                lt = nx;
            }
        }
        p = e ? e + 1 : p + l;
    }
    free(h);
    return found;
#else
    (void)url; (void)out; (void)cap; return 0;
#endif
}
typedef struct { char url[1100]; const char *route; } WnTwin;
/* enderecos de gemeos que se adivinham pelo endereco: <endereco>.md, index.md do diretorio, /llms.txt do site */
static int wn_twin_guess(const char *url, WnTwin *t, int max) {
    const char *sch = strstr(url, "://");
    if (!sch) return 0;
    const char *host = sch + 3, *path = host + strcspn(host, "/?#");
    const size_t olen = (size_t)(path - url), plen = strcspn(path, "?#");
    int n = 0;
    char c[1100];
    if (plen > 0 && path[plen - 1] != '/') {                          /* a pagina como arquivo .md */
        size_t base = plen;
        if (plen > 5 && ci_ncmp(path + plen - 5, ".html", 5) == 0) base = plen - 5;
        else if (plen > 4 && ci_ncmp(path + plen - 4, ".htm", 4) == 0) base = plen - 4;
        snprintf(c, sizeof c, "%.*s%.*s.md", (int)olen, url, (int)base, path);
        if (n < max && strcmp(c, url) != 0) { snprintf(t[n].url, sizeof t[n].url, "%s", c); t[n++].route = "md-suffix"; }
    }
    {                                                                   /* index.md do diretorio */
        const char *ls = NULL;
        for (size_t k = 0; k < plen; k++) if (path[k] == '/') ls = path + k;
        c[0] = 0;
        if (plen == 0) snprintf(c, sizeof c, "%.*s/index.md", (int)olen, url);
        else if (path[plen - 1] == '/') snprintf(c, sizeof c, "%.*s%.*sindex.md", (int)olen, url, (int)plen, path);
        else if (!ls || !memchr(ls, '.', plen - (size_t)(ls - path))) snprintf(c, sizeof c, "%.*s%.*s/index.md", (int)olen, url, (int)plen, path);
        if (c[0] && n < max && strcmp(c, url) != 0) { snprintf(t[n].url, sizeof t[n].url, "%s", c); t[n++].route = "md-index"; }
    }
    snprintf(c, sizeof c, "%.*s/llms.txt", (int)olen, url);              /* indice do site para modelos */
    if (n < max && strcmp(c, url) != 0) { snprintf(t[n].url, sizeof t[n].url, "%s", c); t[n++].route = "llms-txt"; }
    return n;
}
/* baixa um gemeo; 1 se veio texto que nao e HTML */
static int wn_twin_get(const char *url, char *buf, size_t cap, double dl, long *n) {
    const double left = dl - now_sec();
    int rc = 0;
    if (left < 2.0) return 0;
    g_curl_tmo = left < 8.0 ? (int)left : 8;
    const long k = curl_get(url, buf, cap, &rc);
    g_curl_tmo = 0;
    if (k <= 0 || g_curl_http >= 400) return 0;
    const char *p = buf;
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (*p == '<') return 0;                                            /* uma pagina de erro ou a propria pagina em HTML */
    *n = k;
    return 1;
}

/* ---------- a proxima rota, calculada pelo motor, com enderecos reais (docs/web-design.md 3.1) ---------- */
/* hospedagens de codigo: a pagina de um projeto costuma ligar o repositorio dele */
static int wn_repo_of(const char *links, const char *page_url, char *repo, size_t rcap, char *raw_readme, size_t qcap) {
    static const char *const hosts[] = {"github.com", "gitlab.com", "codeberg.org", NULL};
    static const char *const skip[] = {"sponsors", "orgs", "features", "about", "login", "join", "pricing", "topics", "marketplace", "apps", "settings", "collections", "explore", "enterprise", "customer-stories", "readme", NULL};
    for (const char *p = links; *p; ) {
        const char *e = strchr(p, '\n'), *tab = e ? (const char *)memchr(p, '\t', (size_t)(e - p)) : NULL;
        if (!e) break;
        if (tab) {
            const char *u = tab + 1;
            for (int h = 0; hosts[h]; h++) {
                char pre[64];
                snprintf(pre, sizeof pre, "https://%s/", hosts[h]);
                if (ci_ncmp(u, pre, strlen(pre)) != 0 || strstr(page_url, pre)) continue;
                const char *o = u + strlen(pre), *os = (const char *)memchr(o, '/', (size_t)(e - o));
                if (!os) continue;
                const size_t ol = (size_t)(os - o);
                const char *r = os + 1;
                size_t rl = 0;
                while (r + rl < e && r[rl] != '/' && r[rl] != '?' && r[rl] != '#' && r[rl] != '"') rl++;
                if (!ol || !rl) continue;
                int sk = 0;
                for (int k = 0; skip[k]; k++) if (strlen(skip[k]) == ol && ci_ncmp(o, skip[k], ol) == 0) sk = 1;
                if (sk) continue;
                snprintf(repo, rcap, "https://%s/%.*s/%.*s", hosts[h], (int)ol, o, (int)rl, r);
                if (h == 0) snprintf(raw_readme, qcap, "https://raw.githubusercontent.com/%.*s/%.*s/HEAD/README.md", (int)ol, o, (int)rl, r);
                else raw_readme[0] = 0;
                return 1;
            }
        }
        p = e + 1;
    }
    return 0;
}
/* consulta de busca para a mesma coisa em outra fonte: o titulo da pagina, ou as palavras do fim do endereco */
static void wn_query(const char *url, const WnPage *pg, char *q, size_t cap) {
    size_t n = 0;
    q[0] = 0;
    if (pg && pg->txt && pg->tn && pg->txt[0] == '#') {
        const char *t = pg->txt + 1;
        while (*t == ' ') t++;
        for (; *t && *t != '\n' && n + 2 < cap && n < 100; t++) q[n++] = (*t == '"' || *t == '\\' || (unsigned char)*t < 0x20) ? ' ' : *t;
        q[n] = 0;
    }
    if (n >= 4) return;
    const char *sch = strstr(url, "://"), *host = sch ? sch + 3 : url, *path = host + strcspn(host, "/?#");
    size_t pl = strcspn(path, "?#");
    while (pl > 1 && path[pl - 1] == '/') pl--;
    const char *a = path + pl;
    int segs = 0;
    while (a > path && segs < 2) { a--; if (*a == '/') segs++; }
    n = 0;
    for (const char *p = a; p < path + pl && n + 2 < cap && n < 100; p++) q[n++] = (*p == '/' || *p == '-' || *p == '_' || *p == '.' || *p == '+' || *p == '"') ? ' ' : *p;
    q[n] = 0;
    while (n > 0 && q[n - 1] == ' ') q[--n] = 0;
    { size_t z = 0; while (q[z] == ' ') z++; if (z) memmove(q, q + z, n - z + 1), n -= z; }     /* o "/" do primeiro segmento nao fica como espaco no comeco */
    if (n < 4) snprintf(q, cap, "%.*s", (int)strcspn(host, "/?#"), host);
}
static int wn_tried_has(const char *tried, const char *route) {
    const size_t l = strlen(route);
    for (const char *p = tried; (p = strstr(p, route)); p += l) if ((p == tried || p[-1] == ' ') && (p[l] == 0 || p[l] == ',')) return 1;
    return 0;
}
static WnPage *ws_fetch_page(const char *url);                       /* agent/websrc.c */
static int ws_other_sources(const char *q, const char *url, char *o, size_t cap);
/* KATANA_WEB_OFFLINE_ROUTER=1: o passo "outra fonte" da escada nao consulta o roteador (testes sem rede); sem a chave, comportamento normal */
static int wn_offline_router(void) { const char *e = getenv("KATANA_WEB_OFFLINE_ROUTER"); return e && e[0] == '1'; }
/* a linha "[next: ...]" para um resultado que nao e ok. state: shell, empty, blocked, login, error. pg pode ser NULL (falha antes de haver pagina) */
static void wn_next(const char *url, const char *state, const WnPage *pg, const char *tried, char *o, size_t cap) {
    size_t n = 0;
    char q[160], repo[300] = "", rawr[400] = "";
    const int page = strcmp(state, "shell") == 0 || strcmp(state, "empty") == 0;
    n += (size_t)snprintf(o + n, cap - n, "[next: %s. Do not fetch this address again.",
                          !strcmp(state, "shell") ? "this page is an empty shell: its content is built by JavaScript and did not load"
                          : !strcmp(state, "empty") ? "no readable text came back"
                          : !strcmp(state, "blocked") ? "the site refuses this client"
                          : !strcmp(state, "login") ? "the page needs a login" : "the request failed");
    if (tried && tried[0]) n += (size_t)snprintf(o + n, cap - n, " Routes already tried: %s.", tried);
    if (page) {
        WnTwin t[4];
        const int nt = wn_twin_guess(url, t, 4);
        for (int i = 0; i < nt; i++) if (!tried || !wn_tried_has(tried, t[i].route)) { n += (size_t)snprintf(o + n, cap - n, " Same page as markdown, not tried: %s.", t[i].url); break; }
    }
    if (pg && pg->links && wn_repo_of(pg->links, url, repo, sizeof repo, rawr, sizeof rawr)) {
        if (rawr[0]) n += (size_t)snprintf(o + n, cap - n, " The page links the project repository %s (README: %s).", repo, rawr);
        else n += (size_t)snprintf(o + n, cap - n, " The page links the project repository %s.", repo);
    }
    wn_query(url, pg, q, sizeof q);
    {   /* passo "outra fonte" da escada: o roteador de fontes busca o mesmo assunto, se ainda ha tempo */
        char alt[1200];
        const double left = g_wn_deadline > 0 ? g_wn_deadline - now_sec() : 20.0;
        if (!g_wn_no_search && !wn_offline_router() && left > 8.0 && ws_other_sources(q, url, alt, sizeof alt) > 0) { snprintf(o + n, cap - n, " Other sources found by the router for \"%s\": %s.]", q, alt); return; }
    }
    snprintf(o + n, cap - n, " Another source: WebSearch with query \"%s\" and read a different site.]", q);
}
/* estado de um resultado entregue: shell e empty vem do verificador; partial e a pagina boa entregue so em parte */
static const char *wn_state_name(const WnPage *pg, size_t returned) {
    if (pg->verdict == WQ_EMPTY) return "empty";
    if (pg->verdict == WQ_SHELL) return "shell";
    return returned < pg->tn ? "partial" : "ok";
}
/* poe `head` e uma quebra de linha no comeco de out (que tem cap bytes); corta o fim do corpo se for preciso */
static void wn_front(char *out, size_t cap, const char *head) {
    const size_t hl = strlen(head), ol = strlen(out);
    if (hl + 2 >= cap) return;
    size_t keep = ol;
    if (keep + hl + 2 > cap) keep = cap - hl - 2;
    memmove(out + hl + 1, out, keep);
    memcpy(out, head, hl);
    out[hl] = '\n';
    out[hl + 1 + keep] = 0;
}
/* contrato do WebFetch: a primeira linha diz o estado; se nao for ok, a linha seguinte diz a proxima rota */
static void wn_prefix(char *out, size_t cap, const WnPage *pg, const char *url, size_t returned) {
    char head[1700];
    const char *st = wn_state_name(pg, returned);
    int n = snprintf(head, sizeof head, "[web: %s, via %s, %zu of %zu bytes of text, quality %d]", st, pg->route[0] ? pg->route : "plain", returned, pg->tn, pg->quality);
    if (strcmp(st, "shell") == 0 || strcmp(st, "empty") == 0) { head[n++] = '\n'; wn_next(url, st, pg, pg->tried, head + n, sizeof head - (size_t)n); }
    wn_front(out, cap, head);
}
/* o mesmo para uma falha: estado de g_wn_errst (vazio: erro sem relacao com a rede, so a linha de estado) */
static void wn_prefix_err(char *out, size_t cap, const char *url) {
    char head[1700];
    const char *st = g_wn_errst[0] ? g_wn_errst : "error";
    int n = snprintf(head, sizeof head, "[web: %s, via %s, 0 of 0 bytes of text, quality 0]", st, g_wn_errroute[0] ? g_wn_errroute : "plain");
    if (g_wn_errst[0]) { head[n++] = '\n'; wn_next(url, st, NULL, g_wn_errroute, head + n, sizeof head - (size_t)n); }
    wn_front(out, cap, head);
}

/* ---------- escada de rotas (docs/web-design.md 3.3, passos 1 a 3) ----------
 * 1 requisicao simples (ja feita por quem chama, raw); 2 navegador sem janela, so se o verificador reprovou; 3 gemeos legiveis por
 * maquina, so se continua reprovado. Cada passo so roda se o anterior reprovou; fica a melhor leitura e a rota que a deu. O tempo
 * total respeita g_wn_deadline (a escada nao abre rota nova sem tempo para ela). */
typedef struct { char *txt, *lk; size_t tn, ln; int nl; WnQual q; } WnTry;
static int wn_try_make(WnTry *t, const char *url, const char *raw, long n, int plain) {
    t->txt = (char *)malloc(2500000 + 600); t->lk = (char *)malloc(WN_LINKS_BYTES);
    if (!t->txt || !t->lk) { free(t->txt); free(t->lk); t->txt = t->lk = NULL; return 0; }
    t->tn = wn_convert(url, raw, n, plain, t->txt, t->lk, &t->ln, &t->nl);
    wn_quality(plain ? NULL : raw, (size_t)n, t->txt, t->tn, &t->q);
    return 1;
}
static void wn_try_free(WnTry *t) { free(t->txt); free(t->lk); t->txt = t->lk = NULL; }
/* qual leitura serve: veredito melhor primeiro (ok antes de shell antes de empty), depois mais texto */
static int wn_better(const WnTry *a, const WnTry *b) { return a->q.verdict != b->q.verdict ? a->q.verdict < b->q.verdict : a->q.body > b->q.body; }
static void wn_add(char *s, size_t cap, const char *t) { const size_t l = strlen(s); snprintf(s + l, cap - l, "%s%s", l ? ", " : "", t); }
/* tenta um gemeo em twin_url; se for texto ok e melhor que cur, ele vira o texto (os links ficam os do HTML) */
static int wn_try_twin(const char *url, const char *twin_url, char *buf, double dl, WnTry *cur) {
    long k = 0;
    if (!wn_twin_get(twin_url, buf, 600000, dl, &k)) return 0;
    WnTry t3;
    if (!wn_try_make(&t3, url, buf, k, 1)) return 0;
    if (t3.q.verdict == WQ_OK && t3.q.body >= 150 && wn_better(&t3, cur)) {
        free(t3.lk); t3.lk = cur->lk; t3.ln = cur->ln; t3.nl = cur->nl;
        free(cur->txt);
        *cur = t3;
        return 1;
    }
    wn_try_free(&t3);
    return 0;
}

/* converte o HTML baixado (raw, de tamanho RAW+1, n bytes; toma posse), roda a escada e guarda a pagina. allow_render 0: nunca abre
   o navegador nem busca gemeos (so a leitura simples) */
static WnPage *wn_ingest(const char *url, char *raw, long n, int rendered, int allow_render, char *err, size_t ecap) {
    const size_t RAW = 2500000;
    const double dl = g_wn_deadline > 0 ? g_wn_deadline : now_sec() + WN_BUDGET;
    if (n >= 4 && memcmp(raw, "%PDF", 4) == 0) {
        snprintf(err, ecap, "error: this URL is a PDF, not a page. Save it with Bash (curl -o) and read it with Read");
        snprintf(g_wn_errst, sizeof g_wn_errst, "error"); snprintf(g_wn_errroute, sizeof g_wn_errroute, "plain");
        free(raw); return NULL;
    }
    WnTry cur;
    if (!wn_try_make(&cur, url, raw, n, 0)) { free(raw); snprintf(err, ecap, "error: out of memory"); snprintf(g_wn_errst, sizeof g_wn_errst, "error"); return NULL; }
    char route[24], tried[160] = "", alt[1100] = "";
    snprintf(route, sizeof route, "%s", rendered ? "browser" : "plain");
    wn_add(tried, sizeof tried, route);
    if (cur.q.html) wn_alt_link(raw, (size_t)n, url, alt, sizeof alt);
    if (allow_render && cur.q.verdict != WQ_OK && cur.q.html && !rendered && wn_cifind(raw, (size_t)n, "<script")) {      /* passo 2: a pagina se monta no navegador */
        const int rt = (int)(dl - now_sec()) - 12;
        if (rt >= 8) {
            long rn = 0;
            char *r = wn_render(url, RAW, &rn, rt > 35 ? 35 : rt);
            wn_add(tried, sizeof tried, "browser");
            if (r) {
                WnTry t2;
                if (wn_try_make(&t2, url, r, rn, 0)) {
                    if (!alt[0]) wn_alt_link(r, (size_t)rn, url, alt, sizeof alt);
                    if (wn_better(&t2, &cur)) { wn_try_free(&cur); cur = t2; snprintf(route, sizeof route, "browser"); }
                    else wn_try_free(&t2);
                }
                free(r);
            }
        }
    }
    free(raw);
    if (allow_render && cur.q.verdict != WQ_OK) {                    /* passo 3: gemeos legiveis por maquina */
        WnTwin tw[4];
        int nt = 0, hdr_done = 0, adopted = 0;
        char *buf = (char *)malloc(600001);
        if (alt[0]) { snprintf(tw[0].url, sizeof tw[0].url, "%s", alt); tw[0].route = "md-link"; nt = 1; }
        nt += wn_twin_guess(url, tw + nt, 4 - nt);
        for (int i = 0; buf && i <= nt && !adopted; i++) {
            if (!hdr_done && (i == nt || strcmp(tw[i].route, "md-link") != 0)) {      /* o cabecalho Link vem depois do link do HTML e antes dos palpites */
                char hdr[1100];
                hdr_done = 1;
                if (dl - now_sec() > 4.0 && wn_link_header(url, hdr, sizeof hdr) && strcmp(hdr, alt) != 0) {
                    wn_add(tried, sizeof tried, "md-header");
                    if (wn_try_twin(url, hdr, buf, dl, &cur)) { snprintf(route, sizeof route, "md-header"); adopted = 1; break; }
                }
            }
            if (i == nt || dl - now_sec() < 2.0) break;
            wn_add(tried, sizeof tried, tw[i].route);
            if (wn_try_twin(url, tw[i].url, buf, dl, &cur)) { snprintf(route, sizeof route, "%s", tw[i].route); adopted = 1; }
        }
        free(buf);
    }
    WnPage *pg = wn_put(url, cur.txt, cur.tn, cur.lk, cur.ln, cur.nl);
    pg->verdict = cur.q.verdict; pg->quality = cur.q.score;
    snprintf(pg->route, sizeof pg->route, "%s", route);
    snprintf(pg->tried, sizeof pg->tried, "%s", tried);
    return pg;
}

/* baixa a pagina (passo 1), roda a escada e guarda; em erro escreve a mensagem em err, o estado em g_wn_errst e devolve NULL */
static WnPage *wn_load_in(const char *url, char *err, size_t ecap) {
    const size_t RAW = 2500000;
    char *raw = (char *)malloc(RAW + 1);
    int rc;
    g_wn_errst[0] = 0; g_wn_errroute[0] = 0;
    if (!g_wn_render) { WnPage *sp = ws_fetch_page(url); if (sp) { free(raw); return sp; } }    /* endereco de uma fonte registrada: o leitor dela (docs/web-sources.md 4) */
    if (!raw) { snprintf(err, ecap, "error: out of memory"); snprintf(g_wn_errst, sizeof g_wn_errst, "error"); return NULL; }
    long n = curl_get(url, raw, RAW, &rc);
    int rendered = 0;
    if (n >= 0 && g_curl_http >= 400) {      /* a pagina de erro do site nao e o conteudo pedido: vira erro, nao texto */
        const int c = g_curl_http;
        snprintf(err, ecap, "error: HTTP %d (%s) from %.300s: %s.", c, http_reason(c), url,
                 c == 404 || c == 410 ? "there is no page at this address" : c == 401 || c == 403 ? "the site refuses this request (login or permission needed)"
                 : c == 429 ? "the site is limiting requests" : c >= 500 ? "the site failed" : "the site rejected the request");
        snprintf(g_wn_errst, sizeof g_wn_errst, "%s", c == 401 ? "login" : c == 403 || c == 429 ? "blocked" : "error");
        snprintf(g_wn_errroute, sizeof g_wn_errroute, "plain");
        free(raw); return NULL;
    }
    if (n != -2 && (n <= 0 || g_wn_render)) {     /* pedido explicito, ou o curl nao trouxe nada (bloqueio a clientes sem navegador) */
        int rt = (int)(g_wn_deadline - now_sec()) - 12;
        if (rt > 40) rt = 40;
        if (rt < 10) rt = 10;
        long rn = 0; char *r = wn_render(url, RAW, &rn, rt);
        if (r) { memcpy(raw, r, (size_t)rn); raw[rn] = 0; n = rn; rendered = 1; free(r); }
    }
    if (n <= 0) {
        if (n == -2) { snprintf(err, ecap, "error: invalid URL (only http and https; no control characters)"); snprintf(g_wn_errst, sizeof g_wn_errst, "error"); }
        else if (n < 0) { snprintf(err, ecap, "error: cannot run curl"); snprintf(g_wn_errst, sizeof g_wn_errst, "error"); }
        else {
            snprintf(err, ecap, "error: no content received (curl exit code %d: network failure, timeout, blocked or file too large). curl -sSL -D- <url> with Bash shows the HTTP status", rc);
            snprintf(g_wn_errst, sizeof g_wn_errst, "%s", rc == 0 ? "empty" : (rc == 6 || rc == 7 || rc == 28) ? "error" : "blocked");
        }
        snprintf(g_wn_errroute, sizeof g_wn_errroute, "%s", g_wn_render ? "browser" : "plain");
        free(raw); return NULL;
    }
    return wn_ingest(url, raw, n, rendered, 1, err, ecap);
}
static WnPage *wn_load(const char *url, char *err, size_t ecap) {
    g_wn_deadline = now_sec() + WN_BUDGET;
    WnPage *p = wn_load_in(url, err, ecap);
    g_wn_deadline = 0;
    return p;
}

static void tool_web_fetch(const char *js, const char *end, char *out, size_t cap) {
    static char url[1024], find[200];
    if (!json_str(js, end, "url", url, sizeof url) || !url[0]) { snprintf(out, cap, "error: missing url"); return; }
    {   char *h = strchr(url, '#'); if (h) *h = 0;      /* mesma pagina: sem fragmento e sem a barra final do caminho (a chave do cache e o endereco) */
        size_t ul = strlen(url); if (ul > 8 && url[ul - 1] == '/' && !strchr(url, '?') && strchr(url + 8, '/') != url + ul - 1) url[ul - 1] = 0; }
    if (!json_str(js, end, "find", find, sizeof find)) find[0] = 0;
    const int want_links = json_bool(js, end, "links");
    long off = json_int(js, end, "offset", 0);
    g_wn_render = json_bool(js, end, "render");
    g_wn_errst[0] = 0; g_wn_errroute[0] = 0; g_wn_returned = 0;
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
        g_wn_returned = pg->tn;
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
            hits++; g_wn_returned += (size_t)(b - a);
            p = b;
        }
        if (!hits) snprintf(out, cap, "no passage mentions \"%s\" in this page (%zu bytes of text). Try another word, read the page from the start (no find), or list its links (links=true)", find, pg->tn);
        else snprintf(out + o, cap - o, "[%d passage%s with \"%s\"; to read around one, call WebFetch with offset=<that offset>]", hits, hits > 1 ? "s" : "", find);
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
    g_wn_returned = (size_t)(b - a);
    if (!pg->tn) snprintf(out, cap, "(the page has no readable text)");
    else if (b < te || off > 0)
        snprintf(out + o, cap - o, b < te ? "\n[page text bytes %zu-%zu of %zu. Continue: same url with offset=%zu. Jump to a subject: find=\"word\". See where the page leads: links=true]"
                                          : "\n[page text bytes %zu-%zu of %zu (end of page)]", (size_t)(a - pg->txt), (size_t)(b - pg->txt), pg->tn, (size_t)(b - pg->txt));
}
