/* ---------- uioe/conn_web.c: o conector web.search (docs/uioe.md 3.8.2; docs/web-sources.md 4) ----------
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
