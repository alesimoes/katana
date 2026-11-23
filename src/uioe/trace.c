/* ---------- uioe/trace.c: o traco (~/.katana/uioe.log) e o buffer de texto do UIOE ----------
 * Um registro legivel por pedido (formato de docs/uioe.md 3.11.1). KATANA_UIOE_LOG muda o caminho; KATANA_UIOE_LOG=0 desliga.
 * Rotacao: passando de 8 MB o arquivo vira uioe.log.1 (o anterior e sobrescrito). Arquivo criado com modo 0600.
 * Nao feito em U0: mascarar formatos de segredo no texto do pedido (a funcao de mascara das sessoes esta em app/knowledge.c, depois
 * desta parte no arquivo unico). O arquivo e 0600, e o texto do pedido ja esta no registro de sessoes com o mesmo modo. */
typedef struct { char *p; size_t n, cap; } UioeBuf;
static void uioe_bfree(UioeBuf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }
static void uioe_bvprintf(UioeBuf *b, const char *fmt, va_list ap) {
    for (;;) {
        va_list cp; va_copy(cp, ap);
        const size_t room = b->cap > b->n ? b->cap - b->n : 0;
        const int k = vsnprintf(b->p ? b->p + b->n : NULL, room, fmt, cp); va_end(cp);
        if (k < 0) return;
        if ((size_t)k < room) { b->n += (size_t)k; return; }
        size_t nc = b->cap ? b->cap * 2 : 4096; while (nc < b->n + (size_t)k + 1) nc *= 2;
        char *q = (char *)realloc(b->p, nc); if (!q) return;
        b->p = q; b->cap = nc;
    }
}
static void uioe_bprintf(UioeBuf *b, const char *fmt, ...) { va_list ap; va_start(ap, fmt); uioe_bvprintf(b, fmt, ap); va_end(ap); }

/* caminho do traco; "" quando desligado */
static void uioe_trace_path(char *out, size_t cap) {
    const char *e = getenv("KATANA_UIOE_LOG");
    if (e && !strcmp(e, "0")) { out[0] = 0; return; }
    if (e && *e) { snprintf(out, cap, "%s", e); return; }
    char h[1024]; agent_home(h, sizeof h);
    snprintf(out, cap, "%s/.katana/uioe.log", h);
}
static void uioe_mkdir_parent(const char *path) {
    char d[1300]; snprintf(d, sizeof d, "%s", path);
    for (char *p = d + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(d, 0700); *p = '/'; }
}
/* anexa um registro; devolve 1 se gravou */
static int uioe_trace_write(const char *rec) {
    char p[1300]; uioe_trace_path(p, sizeof p);
    if (!p[0]) return 0;
    uioe_mkdir_parent(p);
    struct stat st;
    if (stat(p, &st) == 0 && st.st_size > (8 << 20)) { char o[1320]; snprintf(o, sizeof o, "%s.1", p); rename(p, o); }
    const int fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return 0;
    const size_t n = strlen(rec); const ssize_t w = write(fd, rec, n);
    close(fd);
    return w == (ssize_t)n;
}
static uint64_t uioe_new_id(UioeSession *s) {
    uint64_t h = 1469598103934665603ULL ^ (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 20) ^ (++s->seq * 0x9e3779b97f4a7c15ULL);
    h ^= h >> 29; h *= 0xbf58476d1ce4e5b9ULL; h ^= h >> 32;
    return h & 0xffff;
}

/* ---- as linhas do registro (usadas tambem por `katana uioe ask`) ---- */
static void uioe_fmt_ents(UioeBuf *b, const UioeAsk *a, const char *indent) {
    for (int i = 0; i < a->nent; i++) {
        const UioeEntity *e = &a->ent[i];
        uioe_bprintf(b, "%s%-11s \"%s\"%s%s%s%s  [%s%s, %d]\n", indent, g_uioe_entname[(int)e->kind], e->text,
                     e->resolved[0] ? " -> " : "", e->resolved, "", "", e->how, e->guess ? "; guess" : "", e->conf);
    }
}
static void uioe_fmt_trace(UioeBuf *b, const UioeCtx *c, const UioeRequest *r) {
    const UioeAsk *a = &r->ask; const UioeClass *k = &r->cls; const UioeRoute *rt = &r->route;
    char ts[32]; struct tm tm; localtime_r(&c->now, &tm); strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    static const char *const org[] = {"request", "tool", "cli"};
    uioe_bprintf(b, "== %s req %04llx origin=%s session=- root=%s\n", ts, (unsigned long long)r->id, org[a->origin % 3], a->root);
    uioe_bprintf(b, "ask     lang=%s gap=n/a ambiguous=%s   class domain=%s kind=%s sure=%d rule=R%d (%s) retrieve=%d (C%d)\n", a->lang, a->ambiguous ? "yes" : "no",
                 g_uioe_domname[k->domain[0]], g_uioe_kindname[k->kind], k->sure, k->rule_kind, k->why_kind, k->retrieve, k->rule_retrieve);
    uioe_bprintf(b, "        text: %.300s\n", a->text);
    if (a->nent) { uioe_bprintf(b, "        entities:\n"); uioe_fmt_ents(b, a, "          "); }
    if (a->nterms) { uioe_bprintf(b, "        terms:"); for (int i = 0; i < a->nterms; i++) uioe_bprintf(b, " %s>%s", a->terms[i], a->norm[i]); uioe_bprintf(b, "\n"); }
    if (a->ndropped) { uioe_bprintf(b, "        dropped:"); for (int i = 0; i < a->ndropped; i++) uioe_bprintf(b, " %s", a->dropped[i]); uioe_bprintf(b, "\n"); }
    uioe_bprintf(b, "route   pipeline %s confidence %d before retrieval; deadline %d ms ceiling %d tokens; %d step(s)\n", rt->pipeline, rt->confidence, rt->deadline_ms, rt->token_ceiling, rt->nstep);
    for (int i = 0; i < rt->nstep; i++) {
        const UioeStep *s = &rt->step[i];
        uioe_bprintf(b, "step    %d %-9s %-14s %.80s%s\n", s->n, s->label, s->conn[0] ? s->conn : "(none)", s->input, s->status ? " [not run]" : "");
    }
    for (int i = 0; i < rt->nsig; i++) uioe_bprintf(b, "signal  %s\n", rt->signals[i]);
    if (rt->rule) uioe_bprintf(b, "gate    %s\n", rt->decision_note); else uioe_bprintf(b, "gate    pending: %s\n", rt->decision_note);
    uioe_bprintf(b, "        sent out: %s\n", rt->sends_out[0] ? rt->sends_out : "nothing");
}
