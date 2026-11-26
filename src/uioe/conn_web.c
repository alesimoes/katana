/* ---------- uioe/conn_web.c: os conectores web.search e web.fetch (docs/uioe.md 3.8.2; docs/web-sources.md 4) ----------
 * Roda o roteador de fontes da web (agent/websrc.c: classifica a pergunta, escolhe 2 a 4 fontes, consulta em paralelo, verifica) e
 * devolve cada resultado como um no UN_FACT:
 *   name = a fonte (id do registro), sig = a data do resultado, text = titulo e resumo, how = o endereco e a linha de estado.
 * Conteudo de fora da maquina: confianca baixa (20 + 8 por ponto de confianca da fonte, no maximo 60), faixa 4, nunca verificado (UV_NONE).
 * Continua sob a politica do registro (uioe_av_web: so com KATANA_UIOE_WEB=1); fonte que falhou vai para `limits`, nomeada. */
#define CW_MAX 6
static int cw_search(UioeCtx *cx, const UioeCall *q, UioeResult *r) {
    (void)cx; snprintf(r->source, sizeof r->source, "web.search");
    if (!q->arg[0]) { r->status = UR_EMPTY; snprintf(r->notfound, sizeof r->notfound, "the request has no text to search for"); return r->status; }
    char *buf = (char *)malloc(24000);
    if (!buf) { r->status = UR_EMPTY; snprintf(r->notfound, sizeof r->notfound, "out of memory"); return r->status; }
    snprintf(r->sent, sizeof r->sent, "search terms \"%.150s\" to the sources of the router", q->arg);      /* exatamente o texto que sai: os termos, nunca o pedido */
    ws_search(q->arg, buf, 24000);
    const size_t sl = strcspn(buf, "\n");
    snprintf(r->limits, sizeof r->limits, "%.*s", (int)(sl < 399 ? sl : 399), buf);        /* a linha [search: ...] com as fontes que falharam */
    const int lim = q->limit > 0 && q->limit < CW_MAX ? q->limit : CW_MAX;
    int n = 0;
    for (const char *p = buf + sl; *p && n < lim; ) {
        while (*p == '\n') p++;
        const char *dot = p; while (isdigit((unsigned char)*dot)) dot++;
        const size_t ll = strcspn(p, "\n");
        if (dot == p || dot[0] != '.' || dot[1] != ' ') { p += ll; continue; }
        char title[300], url[700], snip[300], src[160];
        snprintf(title, sizeof title, "%.*s", (int)(ll - (size_t)(dot + 2 - p) < 299 ? ll - (size_t)(dot + 2 - p) : 299), dot + 2);
        p += ll;
        const char *lines[3] = {0}; char *outs[3] = {url, snip, src}; const size_t caps[3] = {sizeof url, sizeof snip, sizeof src};
        for (int k = 0; k < 3; k++) {
            outs[k][0] = 0;
            if (*p != '\n' || strncmp(p + 1, "   ", 3)) break;
            lines[k] = p + 4; const size_t l = strcspn(lines[k], "\n");
            snprintf(outs[k], caps[k], "%.*s", (int)(l < caps[k] - 1 ? l : caps[k] - 1), lines[k]);
            p = lines[k] + l;
        }
        UioeNode *x = ur_node(r); if (!x) break;
        x->kind = UN_FACT;
        char id[100] = "", date[40] = "";
        if (!strncmp(src, "(sources: ", 10)) {
            const char *s = src + 10; const size_t l = strcspn(s, ",;)");
            snprintf(id, sizeof id, "%.*s", (int)(l < 99 ? l : 99), s);
            const char *d = strstr(src, "date "); if (d) snprintf(date, sizeof date, "%.10s", d + 5);
        }
        snprintf(x->name, sizeof x->name, "%s", id[0] ? id : "web");
        snprintf(x->sig, sizeof x->sig, "%s", date);
        snprintf(x->cls, sizeof x->cls, "web");
        char t[600]; snprintf(t, sizeof t, "%s: %s", title, snip);
        ur_set_text(x, t);
        int trust = 3;
        ws_load();
        for (int i = 0; i < g_nws; i++) if (!strcmp(g_wsr[i].id, id)) trust = g_wsr[i].trust;
        x->conf = 20 + 8 * trust; if (x->conf > 60) x->conf = 60;
        x->tier = 4; x->step = 3; x->relevance = 40 - n * 5; x->ver = UV_NONE;
        snprintf(x->how, sizeof x->how, "web result %.150s", url);
        n++;
    }
    free(buf);
    r->total = n; r->status = r->nnode ? UR_OK : UR_EMPTY;
    if (!r->nnode) snprintf(r->notfound, sizeof r->notfound, "no web source returned a result that mentions the request");
    return r->status;
}

/* ---------- web.fetch (docs/uioe.md 3.8.2, fase U9) ----------
 * Le UMA pagina pelo leitor da ferramenta WebFetch (ct_webfetch em tools/webfetch.c): a memoria de paginas de agent/webnav.c e o cache em disco valem como
 * sempre, e as passagens devolvidas sao as do leitor, no maximo 4000 bytes (CT_WF_DEF). A pagina entra como UM no UN_FACT de classe "page":
 *   name = a hora da busca (YYYY-MM-DD HH:MM, hora local), sig = a URL lida, text = uma linha das passagens (o no guarda 199 bytes; as passagens inteiras ficam
 *   na memoria de paginas e o modelo as recebe de volta com WebFetch da mesma URL, sem nova rede), how = a proveniencia.
 * Confianca baixa (conteudo de fora da maquina), faixa 4, nunca verificado (UV_NONE).
 * Quem pode chamar: q->depth = 1 quando o endereco foi digitado pelo usuario no pedido (vale mesmo com KATANA_UIOE_WEB=0: ele deu o endereco); q->depth = 0
 * quando o endereco veio de um resultado de busca, e entao so com KATANA_UIOE_WEB=1 (politica 3.8.6). q->arg = a URL, q->arg2 = as palavras que escolhem as
 * passagens (ficam na maquina: so a URL sai). O passo "outra fonte" da escada do leitor (busca no roteador com palavras tiradas do endereco) fica DESLIGADO
 * aqui (g_wn_no_search): o que sai e a URL. A escada ainda pode pedir ao mesmo site a mesma pagina em outra forma (copia markdown, render) e, para o endereco de
 * uma fonte registrada, o leitor dessa fonte; isso o `sent:` nao lista (ver docs/uioe.md, U9). */
static int cw_fetch(UioeCtx *cx, const UioeCall *q, UioeResult *r) {
    (void)cx; snprintf(r->source, sizeof r->source, "web.fetch");
    if (!q->arg[0]) { r->status = UR_EMPTY; snprintf(r->notfound, sizeof r->notfound, "no address to read"); return r->status; }
    if (!q->depth && !uioe_web_policy_on()) { r->status = UR_UNAVAILABLE; snprintf(r->note, sizeof r->note, "an address taken from search results is read only with KATANA_UIOE_WEB=1 (policy 3.8.6)"); return r->status; }
    char *buf = (char *)malloc(7000), *j = (char *)malloc(2400);
    if (!buf || !j) { free(buf); free(j); r->status = UR_EMPTY; snprintf(r->notfound, sizeof r->notfound, "out of memory"); return r->status; }
    size_t n = (size_t)snprintf(j, 2400, "{\"url\":"); n += ct_wf_jq(q->arg, j + n, 2400 - n);
    n += (size_t)snprintf(j + n, 2400 - n, ",\"prompt\":"); n += ct_wf_jq(q->arg2, j + n, 2400 - n); n += (size_t)snprintf(j + n, 2400 - n, "}");
    buf[0] = 0; g_wn_no_search = 1; ct_webfetch(j, j + n, buf, 7000); g_wn_no_search = 0; free(j);      /* sem o passo "outra fonte": nenhum termo derivado do endereco vai ao roteador de busca */
    /* o leitor poe linhas [web: ...] [next: ...] antes do cabecalho "WebFetch URL: ..."; a URL que de fato foi pedida (http vira https fora do gancho de teste) esta nele */
    char *hp = !strncmp(buf, "WebFetch ", 9) ? buf : strstr(buf, "\nWebFetch "); if (hp && hp != buf) hp++;
    if (!hp || !strncmp(buf, "error:", 6) || !strncmp(buf, "[web: error", 11)) {
        const char *m = strstr(buf, "error:"); if (!m) m = buf; const size_t l = strcspn(m, "\n"); const int off = strstr(buf, "disabled (KATANA_WEB=0)") != NULL;
        r->status = off ? UR_UNAVAILABLE : UR_EMPTY; snprintf(r->note, sizeof r->note, "%.*s", (int)(l < 190 ? l : 190), m);
        snprintf(r->notfound, sizeof r->notfound, "the page was not read: %.*s", (int)(l < 240 ? l : 240), m);
        if (!off) snprintf(r->sent, sizeof r->sent, "page address %.150s (the reader failed or refused it)", q->arg);
        free(buf); return r->status;
    }
    { const char *h = hp + 9; size_t l = strcspn(h, "\n"); const char *e = strstr(h, ": returned"); if (e && (size_t)(e - h) < l) l = (size_t)(e - h);
      e = strstr(h, " (upgraded"); if (e && (size_t)(e - h) < l) l = (size_t)(e - h);
      snprintf(r->sent, sizeof r->sent, "page address %.*s", (int)(l < 170 ? l : 170), h); }
    char *body = strstr(hp, "\n\n"); body = body ? body + 2 : hp;
    char *foot = strstr(body, "\n[web: "); const int cached = foot && !strncmp(foot + 8, "from cache", 10); if (foot) *foot = 0;
    size_t bl = strlen(body); if (bl > 4000) bl = 4000;                       /* passagens limitadas a 4000 bytes */
    body[bl] = 0;
    /* o trecho que o no guarda: a primeira linha de texto (nao titulo) que cita uma palavra do pedido; sem ela, a primeira linha de texto; sem nenhuma, o comeco */
    const char *pick = NULL, *firstt = NULL;
    for (const char *p = body; *p && !pick; ) {
        const size_t ll = strcspn(p, "\n");
        if (ll >= 40 && *p != '#') { if (!firstt) firstt = p;
            char ln[400]; snprintf(ln, sizeof ln, "%.*s", (int)(ll < 399 ? ll : 399), p);
            for (const char *w = q->arg2; *w && !pick; ) { while (*w == ' ') w++; const size_t wl = strcspn(w, " "); if (wl >= 4) { char wd[40]; snprintf(wd, sizeof wd, "%.*s", (int)(wl < 39 ? wl : 39), w); if (strcasestr(ln, wd)) pick = p; } w += wl; } }
        p += ll; while (*p == '\n') p++;
    }
    if (!pick) pick = firstt ? firstt : body;
    char ex[200]; size_t k = 0; int sp = 1;
    for (size_t i = 0; pick[i] && k + 1 < sizeof ex; i++) { const unsigned char c = (unsigned char)pick[i]; if (c <= ' ') { if (!sp) ex[k++] = ' '; sp = 1; } else { ex[k++] = (char)c; sp = 0; } }
    while (k > 0 && (ex[k - 1] == ' ' || ((unsigned char)ex[k - 1] & 0xC0) == 0x80)) k--;
    ex[k] = 0;
    UioeNode *x = ur_node(r);
    if (!x || !ex[0]) { free(buf); r->status = UR_EMPTY; snprintf(r->notfound, sizeof r->notfound, "the page has no readable text"); return r->status; }
    x->kind = UN_FACT; snprintf(x->cls, sizeof x->cls, "page");
    { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); strftime(x->name, sizeof x->name, "%Y-%m-%d %H:%M", &tm); }
    { const char *u = r->sent + 13; const size_t l = strcspn(u, " "); snprintf(x->sig, sizeof x->sig, "%.*s", (int)(l < 159 ? l : 159), u); }
    ur_set_text(x, ex);
    x->conf = q->depth ? 45 : 35; x->tier = 4; x->step = 3; x->relevance = 50; x->ver = UV_NONE;
    snprintf(x->how, sizeof x->how, "web page %s by web.fetch (%s; %zu bytes of passages, at most 4000)", cached ? "from the page memory" : "read from the network", q->depth ? "address typed by the user" : "best web.search result", bl);
    r->total = 1; r->status = UR_OK; free(buf);
    return r->status;
}
