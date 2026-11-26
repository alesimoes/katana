/* ---------- web: URL segura, curl, HTML para texto, leitores de paginas de resultado (a busca fica em agent/websrc.c) ---------- */
/* ---- URL segura para a linha de comando: so http/https; tudo fora de um conjunto seguro vira %XX ---- */
static int url_safe(const char *u, char *out, size_t cap) {
    size_t n = 0;
    int ok = 0;
    if (strlen(u) > 2000) return 0;
    if (ci_ncmp(u, "http://", 7) == 0 || ci_ncmp(u, "https://", 8) == 0) ok = 1;
    if (!ok) return 0;
    for (const unsigned char *s = (const unsigned char *)u; *s; s++) {
        const unsigned char c = *s;
        if (c < 0x20 || c == 0x7f) return 0;
        if (n + 4 >= cap) return 0;
        if (isalnum(c) || strchr("-._~:/?#[]@+,=&;", c)) out[n++] = (char)c;
        else if (c == '%' && isxdigit(s[1]) && isxdigit(s[2])) out[n++] = '%';
        else n += (size_t)snprintf(out + n, cap - n, "%%%02X", c);
    }
    out[n] = 0;
    return 1;
}

/* percent-encode total (para valores de consulta) */
static void url_enc(const char *s, char *out, size_t cap) {
    size_t n = 0;
    for (; *s && n + 4 < cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') out[n++] = (char)c;
        else if (c == ' ') out[n++] = '+';
        else n += (size_t)snprintf(out + n, cap - n, "%%%02X", c);
    }
    out[n] = 0;
}

static void url_dec(const char *s, const char *end, char *out, size_t cap) {
    size_t n = 0;
    for (; s < end && n + 2 < cap; s++) {
        if (*s == '%' && s + 2 < end && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            out[n++] = (char)strtol(h, NULL, 16);
            s += 2;
        } else if (*s == '+') out[n++] = ' ';
        else out[n++] = *s;
    }
    out[n] = 0;
}

/* baixa uma URL com o curl do sistema; devolve bytes lidos (ate cap-1), -1 se nao deu para executar,
 * -2 se a URL for recusada */
#define CURL_STATUS_MARK "\nKATANA-HTTP-STATUS:"
static int g_curl_want_status;      /* 1: o curl escreve o codigo HTTP depois do corpo (so curl_get pede) */
static int g_curl_http;             /* codigo HTTP da ultima curl_get; 0 se nao deu para saber */
static int g_curl_tmo;              /* segundos maximos da proxima curl_get (0: 20); a escada de rotas do WebFetch encurta quando o tempo total esta acabando */
static FILE *curl_open(const char *url, int max_time) {
    char safe[4400], cmd[7000];
    if (!url_safe(url, safe, sizeof safe)) return NULL;
    snprintf(cmd, sizeof cmd,
        CURL_BIN " -sS -L --max-time %d --max-redirs 5 --max-filesize 3000000 --compressed "
        "--proto " CURL_Q "=http,https" CURL_Q " --proto-redir " CURL_Q "=http,https" CURL_Q " %s"
        "-A " CURL_Q "Mozilla/5.0 (compatible; katana-agent)" CURL_Q " -- " CURL_Q "%s" CURL_Q " 2>" CURL_NUL, max_time,
        g_curl_want_status ? "-w " CURL_Q "\\nKATANA-HTTP-STATUS:%{http_code}" CURL_Q " " : "", safe);
#ifdef _WIN32
    return _popen(cmd, "rb");
#else
    return popen(cmd, "r");
#endif
}

/* le a saida do curl (ate cap-1 bytes; cap 0 so descarta), fecha, e devolve os bytes lidos */
static long curl_finish(FILE *f, char *buf, size_t cap, int *rc) {
    size_t n = 0, r;
    while (cap > 0 && n + 1 < cap && (r = fread(buf + n, 1, cap - 1 - n, f)) > 0) n += r;
    if (cap > 0) buf[n] = 0;
    char sink[4096];
    while (fread(sink, 1, sizeof sink, f) > 0) ;
#ifdef _WIN32
    const int c = _pclose(f);
#else
    const int c = pclose(f);
#endif
    if (rc) *rc = c;
    return (long)n;
}

/* o codigo HTTP fica em g_curl_http e sai do fim do corpo. Uma pagina de erro (404, 500...) vem com corpo: quem chama
   decide o que fazer com ela olhando g_curl_http */
static long curl_get(const char *url, char *buf, size_t cap, int *rc) {
    *rc = -1; g_curl_http = 0;
    g_curl_want_status = 1;
    FILE *f = curl_open(url, g_curl_tmo > 0 ? g_curl_tmo : 20);
    g_curl_want_status = 0;
    if (!f) return -2;
    long n = curl_finish(f, buf, cap, rc);
    const size_t ml = strlen(CURL_STATUS_MARK);
    for (long i = n - (long)ml; i >= 0 && i >= n - (long)ml - 3; i--)
        if (memcmp(buf + i, CURL_STATUS_MARK, ml) == 0) { g_curl_http = atoi(buf + i + ml); n = i; buf[n] = 0; break; }
    return n;
}
/* nome curto de um codigo HTTP de erro */
static const char *http_reason(int code) {
    switch (code) {
    case 400: return "Bad Request"; case 401: return "Unauthorized"; case 403: return "Forbidden"; case 404: return "Not Found";
    case 410: return "Gone"; case 429: return "Too Many Requests"; case 500: return "Internal Server Error";
    case 502: return "Bad Gateway"; case 503: return "Service Unavailable"; case 504: return "Gateway Timeout";
    default: return code >= 500 ? "server error" : "client error";
    }
}

/* entidade HTML em s (apos o '&'): escreve o texto em o e devolve quantos bytes de s consumiu; 0 se nao for entidade */
static int html_entity(const char *s, const char *end, char *o, int *ol) {
    static const struct { const char *n, *v; } T[] = {
        {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "},
        {"ndash", "\xe2\x80\x93"}, {"mdash", "\xe2\x80\x94"}, {"hellip", "..."}, {"laquo", "\xc2\xab"},
        {"raquo", "\xc2\xbb"}, {"copy", "(c)"}, {"reg", "(r)"}, {"middot", "\xc2\xb7"}, {"bull", "*"},
        {"lsquo", "'"}, {"rsquo", "'"}, {"ldquo", "\""}, {"rdquo", "\""},
        {"aacute", "\xc3\xa1"}, {"eacute", "\xc3\xa9"}, {"iacute", "\xc3\xad"}, {"oacute", "\xc3\xb3"},
        {"uacute", "\xc3\xba"}, {"agrave", "\xc3\xa0"}, {"acirc", "\xc3\xa2"}, {"ecirc", "\xc3\xaa"},
        {"ocirc", "\xc3\xb4"}, {"atilde", "\xc3\xa3"}, {"otilde", "\xc3\xb5"}, {"ccedil", "\xc3\xa7"},
        {"Aacute", "\xc3\x81"}, {"Eacute", "\xc3\x89"}, {"Iacute", "\xc3\x8d"}, {"Oacute", "\xc3\x93"},
        {"Uacute", "\xc3\x9a"}, {"Ccedil", "\xc3\x87"},
    };
    const char *e = s;
    while (e < end && e - s < 10 && *e != ';' && *e != '&' && *e != '<' && *e != ' ') e++;
    if (e >= end || *e != ';' || e == s) return 0;
    const int len = (int)(e - s);
    if (s[0] == '#') {
        unsigned long cp = (s[1] == 'x' || s[1] == 'X') ? strtoul(s + 2, NULL, 16) : strtoul(s + 1, NULL, 10);
        if (cp == 0xA0) cp = ' ';
        if (cp == 0) return 0;
        if (cp < 0x80) o[(*ol)++] = (char)cp;
        else if (cp < 0x800) { o[(*ol)++] = (char)(0xC0 | (cp >> 6)); o[(*ol)++] = (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { o[(*ol)++] = (char)(0xE0 | (cp >> 12)); o[(*ol)++] = (char)(0x80 | ((cp >> 6) & 63)); o[(*ol)++] = (char)(0x80 | (cp & 63)); }
        else { o[(*ol)++] = (char)(0xF0 | ((cp >> 18) & 7)); o[(*ol)++] = (char)(0x80 | ((cp >> 12) & 63)); o[(*ol)++] = (char)(0x80 | ((cp >> 6) & 63)); o[(*ol)++] = (char)(0x80 | (cp & 63)); }
        return len + 1;
    }
    for (size_t k = 0; k < sizeof T / sizeof T[0]; k++)
        if ((int)strlen(T[k].n) == len && strncmp(T[k].n, s, (size_t)len) == 0) {
            const int l = (int)strlen(T[k].v);
            memcpy(o + *ol, T[k].v, (size_t)l); *ol += l;
            return len + 1;
        }
    return 0;
}

static int tag_is(const char *name, int nl, const char *list) {   /* list: nomes separados por ',' */
    const char *p = list;
    while (*p) {
        const char *q = strchr(p, ',');
        const int l = q ? (int)(q - p) : (int)strlen(p);
        if (l == nl && strncmp(p, name, (size_t)nl) == 0) return 1;
        if (!q) break;
        p = q + 1;
    }
    return 0;
}

/* HTML -> texto: sem tags, scripts e estilos; entidades decodificadas; espacos colapsados.
 * `out` precisa de n+16 bytes. Devolve o tamanho. */
static int g_html_fold;      /* 1: sequencias longas de linhas que sao so link (menus, listas de idiomas) viram uma nota */
/* fim de uma linha de texto [ls, *o): conta a sequencia de linhas so de link e, quando ela acaba com 10 ou mais, troca por uma nota */
static void html_fold(char *out, size_t *o, size_t ls, size_t plain, int *run_n, size_t *run_s, int last) {
    const size_t ll = *o - ls;
    if (ll == 0 && !last) return;
    if (ll > 0 && plain == 0 && ll <= 60) { if (!(*run_n)++) *run_s = ls; if (!last) return; ls = *o; }
    if (*run_n >= 10) {
        char note[64];
        const size_t w = (size_t)snprintf(note, sizeof note, "[%d links omitted; WebFetch with links=true lists them]\n\n", *run_n);
        if (ls - *run_s > w) {
            memmove(out + *run_s + w, out + ls, *o - ls);
            memcpy(out + *run_s, note, w);
            *o = *run_s + w + (*o - ls);
        }
    }
    *run_n = 0;
}
static size_t html_to_text(const char *in, size_t n, char *out) {
    size_t o = 0, line_s = 0, line_plain = 0, run_s = 0;
    int pend_sp = 0, nl = 0, in_a = 0, run_n = 0;
    for (size_t i = 0; i < n; ) {
        const char c = in[i];
        if (c == '<' && i + 1 < n && (isalpha((unsigned char)in[i + 1]) || in[i + 1] == '/' || in[i + 1] == '!')) {
            if (n - i >= 4 && strncmp(in + i, "<!--", 4) == 0) {
                const char *e = strstr(in + i + 4, "-->");
                i = e ? (size_t)(e - in) + 3 : n;
                continue;
            }
            size_t j = i + 1;
            const int closing = in[j] == '/';
            if (closing) j++;
            char name[16]; int nlen = 0;
            while (j < n && nlen < 15 && isalnum((unsigned char)in[j])) name[nlen++] = (char)tolower((unsigned char)in[j++]);
            name[nlen] = 0;
            char q = 0;                          /* fim da tag, respeitando aspas */
            while (j < n && !(in[j] == '>' && !q)) {
                if (q) { if (in[j] == q) q = 0; }
                else if (in[j] == '"' || in[j] == '\'') q = in[j];
                j++;
            }
            i = j < n ? j + 1 : n;
            if (nlen == 1 && name[0] == 'a') in_a = !closing;
            if (!closing && tag_is(name, nlen, "script,style,noscript,svg,template,nav,select,button")) {
                char endtag[24]; snprintf(endtag, sizeof endtag, "</%s", name);
                const size_t el = strlen(endtag);
                size_t k = i;
                while (k + el <= n) {            /* procura o fecho sem diferenciar maiusculas */
                    if (in[k] == '<' && in[k + 1] == '/' && ci_ncmp(in + k, endtag, el) == 0) break;
                    k++;
                }
                if (k + el > n) k = n;
                while (k < n && in[k] != '>') k++;
                i = k < n ? k + 1 : n;
                continue;
            }
            if (tag_is(name, nlen, "p,h1,h2,h3,h4,h5,h6,table,ul,ol,section,article,blockquote,pre,title")) { if (nl < 2) nl = 2; }
            else if (tag_is(name, nlen, "br,div,li,tr,header,footer,nav,hr,form,dd,dt,main,aside,option")) { if (nl < 1) nl = 1; }
            else if (tag_is(name, nlen, "td,th")) pend_sp = 1;
            continue;
        }
        char tmp[8]; int tl = 0;
        if (c == '&') {
            const int used = html_entity(in + i + 1, in + n, tmp, &tl);
            if (used) i += (size_t)used + 1;
            else { tmp[tl++] = '&'; i++; }
        } else { tmp[tl++] = c; i++; }
        if (tl == 1 && (tmp[0] == ' ' || tmp[0] == '\t' || tmp[0] == '\n' || tmp[0] == '\r' || tmp[0] == '\f')) { pend_sp = 1; continue; }
        if (o > 0) {
            if (nl) {
                if (g_html_fold) html_fold(out, &o, line_s, line_plain, &run_n, &run_s, 0);
                for (int k = 0; k < nl; k++) out[o++] = '\n';
                line_s = o; line_plain = 0;
            }
            else if (pend_sp && out[o - 1] != '\n') out[o++] = ' ';
        }
        nl = 0; pend_sp = 0;
        for (int k = 0; k < tl; k++) out[o++] = tmp[k];
        if (!in_a) line_plain += (size_t)tl;
    }
    if (g_html_fold) html_fold(out, &o, line_s, line_plain, &run_n, &run_s, 1);
    out[o] = 0;
    return o;
}

/* texto limpo de um trecho HTML curto (titulo, resumo), numa linha so */
static void html_snip(const char *s, const char *e, char *out, size_t cap) {
    static char tmp[8300];
    size_t n = (size_t)(e - s);
    if (n > 8192) n = 8192;
    size_t o = html_to_text(s, n, tmp);
    if (o >= cap) { o = cap - 1; while (o > 0 && ((unsigned char)tmp[o] & 0xC0) == 0x80) o--; }
    memcpy(out, tmp, o);
    out[o] = 0;
    for (size_t i = 0; i < o; i++) if (out[i] == '\n') out[i] = ' ';
}

/* ---- leitores de paginas de resultado em HTML (kind html do registro de fontes, agent/websrc.c) ----
 * Cada leitor conhece um formato de marcacao, nao um site: o registro diz qual leitor cada fonte usa (parser=...).
 * Se um site mudar o formato, so o leitor dele precisa de ajuste e as outras fontes seguem. */
typedef struct { char title[300], url[700], snip[450], date[16]; double score, rel; long age; unsigned src; int words; float cos; } Hit;


static int hit_set(Hit *h, const char *title, const char *url, const char *snip) {
    if (!title[0] || !url[0] || strncmp(url, "http", 4) != 0) return 0;
    snprintf(h->title, sizeof h->title, "%s", title);
    snprintf(h->url, sizeof h->url, "%s", url);
    snprintf(h->snip, sizeof h->snip, "%s", snip);
    h->score = h->rel = 0; h->src = 0; h->date[0] = 0; h->age = -1; h->words = 0; h->cos = -2.0f;
    return 1;
}

/* leitor "result-a": <a class="result__a" href="...uddg=<url codificada>">titulo</a> ... class="result__snippet" */
static int hp_result_a(const char *raw, const char *arg, Hit *out, int max) {
    static char href[2000], title[300], snip[450];
    const char *end = raw + strlen(raw);
    const char *p = strstr(raw, "class=\"result__a\"");
    int k = 0;
    while (p && k < max) {
        const char *next = strstr(p + 10, "class=\"result__a\"");
        const char *lim = next ? next : end;
        const char *h = strstr(p, "href=\"");
        href[0] = title[0] = snip[0] = 0;
        if (h && h < lim) {
            h += 6;
            const char *he = strchr(h, '"');
            if (he && he < lim) {
                const char *u = NULL;
                for (const char *t = h; t + 5 < he; t++) if (strncmp(t, "uddg=", 5) == 0) { u = t + 5; break; }
                if (u) { const char *ue = u; while (ue < he && *ue != '&') ue++; url_dec(u, ue, href, sizeof href); }
                else {
                    char tmp[1000];
                    size_t l = (size_t)(he - h);
                    if (l >= sizeof tmp) l = sizeof tmp - 1;
                    memcpy(tmp, h, l); tmp[l] = 0;
                    snprintf(href, sizeof href, "%s%s", tmp[0] == '/' && tmp[1] == '/' ? "https:" : "", tmp);
                }
                const char *ts = strchr(he, '>');
                const char *te = ts ? strstr(ts, "</a>") : NULL;
                if (ts && te && te < lim) html_snip(ts + 1, te, title, sizeof title);
            }
        }
        const char *sp = strstr(p, "result__snippet");
        if (sp && sp < lim) {
            const char *ss = strchr(sp, '>');
            const char *se = ss ? strstr(ss, "</a>") : NULL;
            if (ss && se && se < lim) html_snip(ss + 1, se, snip, sizeof snip);
        }
        if (hit_set(&out[k], title, href, snip)) k++;
        p = next;
    }
    return k;
}

/* leitor "title-s": <a class="title" href="URL">titulo</a> ... <p class="s">resumo</p>.
 * O site pode exigir um desafio de JavaScript (captcha) para clientes automaticos; nesse caso nao ha resultados. */
static int hp_title_s(const char *raw, const char *arg, Hit *out, int max) {
    static char title[300], snip[450], href[700];
    const char *end = raw + strlen(raw);
    const char *p = strstr(raw, "class=\"title\"");
    int k = 0;
    while (p && k < max) {
        const char *next = strstr(p + 10, "class=\"title\"");
        const char *lim = next ? next : end;
        title[0] = snip[0] = href[0] = 0;
        const char *a = p;                           /* href vem antes ou depois de class= */
        while (a > raw && *a != '<') a--;
        const char *h = strstr(a, "href=\"");
        const char *ae = strchr(a, '>');
        if (h && ae && h < ae) {
            h += 6;
            const char *he = strchr(h, '"');
            if (he && he < ae) {
                size_t l = (size_t)(he - h);
                if (l >= sizeof href) l = sizeof href - 1;
                memcpy(href, h, l); href[l] = 0;
            }
            const char *te = strstr(ae, "</a>");
            if (te && te < lim) html_snip(ae + 1, te, title, sizeof title);
        }
        const char *sp = strstr(p, "class=\"s\"");
        if (sp && sp < lim) {
            const char *ss = strchr(sp, '>');
            const char *se = ss ? strstr(ss, "</p>") : NULL;
            if (ss && se && se < lim) html_snip(ss + 1, se, snip, sizeof snip);
        }
        if (hit_set(&out[k], title, href, snip)) k++;
        p = next;
    }
    return k;
}

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return c == '-' ? 62 : 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

/* leitor "b-algo": <li class="b_algo"><h2><a href="...&u=a1<base64 da URL>">titulo</a></h2> ... <p>resumo</p> */
static int hp_b_algo(const char *raw, const char *arg, Hit *out, int max) {
    static char title[300], snip[450], href[700];
    const char *end = raw + strlen(raw);
    const char *p = strstr(raw, "class=\"b_algo\"");
    int k = 0;
    while (p && k < max) {
        const char *next = strstr(p + 10, "class=\"b_algo\"");
        const char *lim = next ? next : end;
        title[0] = snip[0] = href[0] = 0;
        const char *h2 = strstr(p, "<h2");
        if (h2 && h2 < lim) {
            const char *h = strstr(h2, "href=\"");
            const char *ae = strchr(h2, '>');
            ae = ae ? strchr(ae + 1, '>') : NULL;      /* fim do <a ...> */
            if (h && ae && h < ae) {
                h += 6;
                const char *he = strchr(h, '"');
                const char *u = NULL;
                for (const char *t = h; he && t + 5 < he; t++) if (strncmp(t, "&amp;u=a1", 9) == 0 || strncmp(t, "&u=a1", 5) == 0) { u = strstr(t, "u=a1") + 4; break; }
                if (u) {                                /* URL real em base64 (variante url-safe, sem preenchimento) */
                    size_t n = 0; unsigned acc = 0; int bits = 0;
                    for (; u < he && *u != '&' && n + 2 < sizeof href; u++) {
                        const int v = b64_val((unsigned char)*u);
                        if (v < 0) break;
                        acc = (acc << 6) | (unsigned)v; bits += 6;
                        if (bits >= 8) { bits -= 8; href[n++] = (char)((acc >> bits) & 255); }
                    }
                    href[n] = 0;
                } else if (he && he - h < (long)sizeof href && strncmp(h, "http", 4) == 0 && !strstr(h, "/ck/a?")) {
                    memcpy(href, h, (size_t)(he - h)); href[he - h] = 0;
                }
                const char *te = strstr(ae, "</a>");
                if (te && te < lim) html_snip(ae + 1, te, title, sizeof title);
            }
        }
        const char *sp = strstr(p, "<p");
        if (sp && sp < lim) {
            const char *ss = strchr(sp, '>');
            const char *se = ss ? strstr(ss, "</p>") : NULL;
            if (ss && se && se < lim) html_snip(ss + 1, se, snip, sizeof snip);
        }
        if (hit_set(&out[k], title, href, snip)) k++;
        p = next;
    }
    return k;
}

/* chave de comparacao de URL: sem esquema, sem www., sem fragmento, sem barra final, minuscula */
static void url_key(const char *u, char *k, size_t cap) {
    size_t n = 0;
    if (ci_ncmp(u, "https://", 8) == 0) u += 8; else if (ci_ncmp(u, "http://", 7) == 0) u += 7;
    if (ci_ncmp(u, "www.", 4) == 0) u += 4;
    for (; *u && *u != '#' && n + 1 < cap; u++) k[n++] = (char)tolower((unsigned char)*u);
    while (n > 0 && k[n - 1] == '/') n--;
    k[n] = 0;
}

