/* ---------- transcricoes de um agente de programacao -> conversas no formato de mensagens (para o katana import) ----------
 * Entrada: arquivo .jsonl (ou pasta, lida com as subpastas) em que cada linha e um registro
 * {"type": "user"|"assistant", "message": {"role", "content"}, ...}. O conteudo e um texto ou uma lista de blocos:
 * "text", "thinking", "tool_use" {id, name, input} e "tool_result" {tool_use_id, content}. Os outros tipos de registro
 * sao ignorados. As ferramentas precisam ter os nomes e os argumentos do agente (src/tools/).
 *
 * Saida: uma conversa por episodio, entregue como uma linha {"messages": [...]} a quem chamou (o import_main, que a passa
 * ao mesmo im_line das outras entradas). Um episodio comeca num pedido do usuario (ou num resumo de contexto) e vai ate
 * o pedido seguinte; o treino corta tudo em janelas de poucos tokens, entao um episodio por pedido nao perde nada e
 * cada exemplo comeca do prompt de sistema. `whole`: um arquivo = uma conversa.
 *
 * O que sai e o que fica:
 * - avisos automaticos dentro das mensagens (<system-reminder>..</system-reminder>) sao tirados; registros marcados como
 *   meta, mensagens de erro da interface e ecos de comandos locais sao ignorados; um registro repetido em outro arquivo
 *   (sessao retomada) entra uma vez;
 * - uma fala cujas chamadas ficaram sem resultado (interrompida) encerra o episodio e nao entra, a nao ser que a chamada
 *   seja a que encerra o turno (o relatorio final de um sub-agente);
 * - resultado de ferramenta maior que max_result bytes fica com o comeco e o fim (o agente tambem corta); imagens num
 *   resultado viram "[image]";
 * - trocas (REGEX=TEXTO, na ordem dada) valem em todos os textos: pedidos, falas, argumentos das chamadas e resultados.
 *   Servem para caminhos e nomes da maquina de origem, que assim continuam coerentes entre a chamada e o resultado;
 * - corte (REGEX, sem diferenca de caixa): o episodio acaba antes da primeira fala do modelo (texto, raciocinio ou
 *   chamadas) que casa; o que veio antes fica, e um episodio que casa logo na primeira fala some. */

typedef struct { Rx *re; long *sp; const char *to; } TrRep;
typedef struct { char role; Buf text, reason, calls; char id[96]; char **cid; char *cend; int ncid; } TrMsg;
typedef struct { TrMsg *v; int n, cap; char **ans; int nans, capans; int cut, open; } TrEp;
typedef struct {
    int whole; long max_result;
    TrRep rep[32]; int nrep;
    Rx *cut;
    void (*emit)(void *ud, const char *line); void *ud;
    long nfiles, nep, nout, nturn, ncall, ncut, ndrop;
    uint64_t *seen; size_t seen_cap, seen_n;
    TrEp ep;
} TrOpt;

static void tr_cpy(char *d, const char *s, size_t cap) { size_t n = strlen(s); if (n >= cap) n = cap - 1; memcpy(d, s, n); d[n] = 0; }
static const char *tr_s(const Buf *b) { return b->p ? b->p : ""; }

/* registro ja visto? (conjunto de hashes de 64 bits do uuid) */
static int tr_seen(TrOpt *o, const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    if (!h) h = 1;
    if ((o->seen_n + 1) * 2 > o->seen_cap) {
        const size_t nc = o->seen_cap ? o->seen_cap * 2 : 4096;
        uint64_t *nv = xalloc(nc, sizeof(uint64_t));
        for (size_t i = 0; i < o->seen_cap; i++) if (o->seen[i]) { size_t j = o->seen[i] & (nc - 1); while (nv[j]) j = (j + 1) & (nc - 1); nv[j] = o->seen[i]; }
        free(o->seen); o->seen = nv; o->seen_cap = nc;
    }
    size_t j = h & (o->seen_cap - 1);
    while (o->seen[j]) { if (o->seen[j] == h) return 1; j = (j + 1) & (o->seen_cap - 1); }
    o->seen[j] = h; o->seen_n++;
    return 0;
}

/* aplica as trocas no texto de b */
static void tr_subst(TrOpt *o, Buf *b) {
    for (int r = 0; r < o->nrep && b->n; r++) {
        Buf out = {0};
        long pos = 0, n = (long)b->n; int any = 0;
        while (pos <= n && rx_search(o->rep[r].re, b->p, n, pos, o->rep[r].sp) == 1) {
            const long a = o->rep[r].sp[0], e = o->rep[r].sp[1];
            buf_add(&out, b->p + pos, (size_t)(a - pos));
            buf_str(&out, o->rep[r].to);
            any = 1;
            if (e > a) pos = e;
            else { if (a < n) buf_add(&out, b->p + a, 1); pos = a + 1; }     /* casou vazio: anda um byte */
        }
        if (!any) { free(out.p); continue; }
        if (pos < n) buf_add(&out, b->p + pos, (size_t)(n - pos));
        free(b->p); *b = out;
        if (!b->p) { b->n = 0; }
    }
}

/* copia o valor JSON v para out, com as trocas aplicadas em cada texto (as chaves ficam como estao) */
static void tr_json_subst(TrOpt *o, const char *v, Buf *out) {
    const char *e = gs_skipv(v), *x, *xe;
    if (!e) { buf_str(out, "null"); return; }
    if (*v == '"') {
        Buf s = {0}; const char *p = v;
        if (!js_string(&p, &s)) { buf_add(out, v, (size_t)(e - v)); free(s.p); return; }
        tr_subst(o, &s);
        buf_str(out, "\""); js_escape(out, tr_s(&s), s.n); buf_str(out, "\"");
        free(s.p);
    } else if (*v == '{') {
        char k[256]; int first = 1;
        buf_str(out, "{");
        for (const char *p = v + 1; (xe = gs_member(p, k, sizeof k, &x)); p = xe) {
            if (!first) buf_str(out, ", ");
            first = 0;
            buf_str(out, "\""); js_escape(out, k, strlen(k)); buf_str(out, "\": ");
            tr_json_subst(o, x, out);
        }
        buf_str(out, "}");
    } else if (*v == '[') {
        int first = 1;
        buf_str(out, "[");
        for (const char *p = v + 1; (xe = gs_elem(p, &x)); p = xe) {
            if (!first) buf_str(out, ", ");
            first = 0;
            tr_json_subst(o, x, out);
        }
        buf_str(out, "]");
    } else buf_add(out, v, (size_t)(e - v));
}

/* texto longo: comeco (2/3) e fim (1/3), sem partir um caractere UTF-8 */
static void tr_clip(Buf *b, long cap) {
    if (cap <= 0 || (long)b->n <= cap) return;
    size_t a = (size_t)cap * 2 / 3, t = b->n - (size_t)cap / 3;
    while (a > 0 && ((unsigned char)b->p[a] & 0xC0) == 0x80) a--;
    while (t < b->n && ((unsigned char)b->p[t] & 0xC0) == 0x80) t++;
    Buf out = {0}; char note[64];
    buf_add(&out, b->p, a);
    snprintf(note, sizeof note, "\n[... %zu bytes omitted ...]\n", t - a);
    buf_str(&out, note);
    buf_add(&out, b->p + t, b->n - t);
    free(b->p); *b = out;
}

static void tr_trim(Buf *b) {
    if (!b->p) return;
    size_t a = 0, e = b->n;
    while (a < e && strchr(" \t\r\n", b->p[a])) a++;
    while (e > a && strchr(" \t\r\n", b->p[e - 1])) e--;
    memmove(b->p, b->p + a, e - a); b->n = e - a; b->p[b->n] = 0;
}

/* valor JSON de texto -> dst (cortado em cap) */
static void tr_sv(const char *v, char *dst, size_t cap) {
    dst[0] = 0;
    if (!v || *v != '"') return;
    Buf s = {0}; const char *p = v;
    if (js_string(&p, &s) && s.p) tr_cpy(dst, s.p, cap);
    free(s.p);
}

static TrMsg *tr_push(TrEp *ep, char role) {
    if (ep->n == ep->cap) { ep->cap = ep->cap ? ep->cap * 2 : 32; ep->v = realloc(ep->v, (size_t)ep->cap * sizeof(TrMsg)); }
    TrMsg *m = &ep->v[ep->n++];
    memset(m, 0, sizeof *m);
    m->role = role;
    return m;
}
static int tr_answered(const TrEp *ep, const char *id) { for (int i = 0; i < ep->nans; i++) if (!strcmp(ep->ans[i], id)) return 1; return 0; }
static void tr_ep_clear(TrEp *ep) {
    for (int i = 0; i < ep->n; i++) {
        TrMsg *m = &ep->v[i];
        free(m->text.p); free(m->reason.p); free(m->calls.p);
        for (int k = 0; k < m->ncid; k++) free(m->cid[k]);
        free(m->cid); free(m->cend);
    }
    for (int i = 0; i < ep->nans; i++) free(ep->ans[i]);
    free(ep->v); free(ep->ans);
    memset(ep, 0, sizeof *ep);
}
static int tr_hit(Rx *re, const Buf *b) { return b->n && rx_search(re, b->p, (long)b->n, 0, NULL) == 1; }

/* fecha o episodio aberto: valida, corta e entrega a linha de mensagens */
static void tr_flush(TrOpt *o) {
    TrEp *ep = &o->ep;
    if (!ep->open) return;
    o->nep++;
    int *keep = xalloc((size_t)ep->n + 1, sizeof(int)), nk = 0;
    for (int i = 0; i < ep->n; i++) {
        TrMsg *m = &ep->v[i];
        if (m->role == 'a') {
            int miss = 0, ends = 1;
            for (int k = 0; k < m->ncid; k++) if (!tr_answered(ep, m->cid[k])) { miss++; if (!m->cend[k]) ends = 0; }
            if (miss) { if (ends) keep[nk++] = i; break; }       /* chamada sem resultado: acaba aqui */
            if (!m->text.n && !m->ncid) continue;                 /* so raciocinio vazio: nada a aprender */
        }
        keep[nk++] = i;
    }
    if (o->cut) for (int j = 0; j < nk; j++) {
        TrMsg *m = &ep->v[keep[j]];
        if (m->role == 'a' && (tr_hit(o->cut, &m->text) || tr_hit(o->cut, &m->reason) || tr_hit(o->cut, &m->calls))) {
            nk = j; o->ncut++;
            while (nk && ep->v[keep[nk - 1]].role != 'a') nk--;
            if (!nk) o->ndrop++;
            break;
        }
    }
    while (nk && ep->v[keep[nk - 1]].role != 'a') nk--;
    if (nk) {
        Buf l = {0};
        buf_str(&l, "{\"messages\": [");
        for (int j = 0; j < nk; j++) {
            TrMsg *m = &ep->v[keep[j]];
            if (j) buf_str(&l, ", ");
            if (m->role == 't') { buf_str(&l, "{\"role\": \"tool\", \"tool_call_id\": \""); js_escape(&l, m->id, strlen(m->id)); buf_str(&l, "\", \"content\": \""); }
            else buf_str(&l, m->role == 'a' ? "{\"role\": \"assistant\", \"content\": \"" : "{\"role\": \"user\", \"content\": \"");
            js_escape(&l, tr_s(&m->text), m->text.n);
            buf_str(&l, "\"");
            if (m->role == 'a') {
                if (m->reason.n) { buf_str(&l, ", \"reasoning_content\": \""); js_escape(&l, m->reason.p, m->reason.n); buf_str(&l, "\""); }
                if (m->ncid) { buf_str(&l, ", \"tool_calls\": ["); buf_add(&l, m->calls.p, m->calls.n); buf_str(&l, "]"); }
                o->nturn++; o->ncall += m->ncid;
            }
            buf_str(&l, "}");
        }
        buf_str(&l, "]}");
        o->nout++;
        o->emit(o->ud, l.p);
        free(l.p);
    }
    free(keep);
    tr_ep_clear(ep);
}

/* texto de um resultado de ferramenta: texto, ou lista de blocos (text, image, tool_reference) */
static void tr_result(const char *v, Buf *out) {
    if (!v) return;
    if (*v == '"') { const char *p = v; js_string(&p, out); return; }
    if (*v != '[') return;
    const char *el, *ee; int first = 1;
    for (const char *p = v + 1; (ee = gs_elem(p, &el)); p = ee) {
        if (*el != '{') continue;
        char k[64], ty[32] = ""; const char *x, *xe, *tx = NULL, *tn = NULL;
        for (const char *q = el + 1; (xe = gs_member(q, k, sizeof k, &x)); q = xe) {
            if (!strcmp(k, "type")) tr_sv(x, ty, sizeof ty);
            else if (!strcmp(k, "text")) tx = x;
            else if (!strcmp(k, "tool_name")) tn = x;
        }
        const char *src = !strcmp(ty, "text") ? tx : !strcmp(ty, "tool_reference") ? tn : NULL;
        if (!src && strcmp(ty, "image")) continue;
        if (!first) buf_str(out, "\n");
        first = 0;
        if (!src) buf_str(out, "[image]");
        else if (*src == '"') { const char *p2 = src; js_string(&p2, out); }
    }
}

/* um registro da transcricao */
static void tr_line(TrOpt *o, const char *line) {
    TrEp *ep = &o->ep;
    char k[64], type[24] = "", uuid[96] = ""; const char *v, *e, *msg = NULL;
    int meta = 0, apierr = 0, compact = 0, ends = 0;
    const char *q = js_ws(line);
    if (*q != '{') return;
    for (const char *p = q + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) {
        if (!strcmp(k, "type")) tr_sv(v, type, sizeof type);
        else if (!strcmp(k, "uuid")) tr_sv(v, uuid, sizeof uuid);
        else if (!strcmp(k, "message") && *v == '{') msg = v;
        else if (!strcmp(k, "isMeta")) meta = *v == 't';
        else if (!strcmp(k, "isApiErrorMessage")) apierr = *v == 't';
        else if (!strcmp(k, "isCompactSummary")) compact = *v == 't';
        else if (!strcmp(k, "toolEndsTurn")) ends = *v == 't';
    }
    const int asst = !strcmp(type, "assistant");
    if ((!asst && strcmp(type, "user")) || !msg) return;
    if (uuid[0] && tr_seen(o, uuid)) return;
    char mid[96] = "", model[64] = ""; const char *content = NULL;
    for (const char *p = msg + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) {
        if (!strcmp(k, "id")) tr_sv(v, mid, sizeof mid);
        else if (!strcmp(k, "model")) tr_sv(v, model, sizeof model);
        else if (!strcmp(k, "content")) content = v;
    }
    if (!content) return;
    const char *el, *ee;
    if (asst) {
        if (apierr || !strcmp(model, "<synthetic>") || !ep->open || ep->cut) return;
        if (!mid[0]) tr_cpy(mid, uuid, sizeof mid);
        TrMsg *m = NULL;
        for (int i = ep->n - 1; i >= 0 && !m; i--) if (ep->v[i].role == 'a' && !strcmp(ep->v[i].id, mid)) m = &ep->v[i];
        if (!m) { m = tr_push(ep, 'a'); tr_cpy(m->id, mid, sizeof m->id); }
        if (*content == '"') {
            Buf t = {0}; const char *p = content; js_string(&p, &t); tr_trim(&t); tr_subst(o, &t);
            if (t.n) { if (m->text.n) buf_str(&m->text, "\n\n"); buf_add(&m->text, t.p, t.n); }
            free(t.p);
            return;
        }
        if (*content != '[') return;
        for (const char *p = content + 1; (ee = gs_elem(p, &el)); p = ee) {
            if (*el != '{') continue;
            char ty[32] = "", cid[96] = "", nm[128] = ""; const char *x, *xe, *tx = NULL, *th = NULL, *in = NULL;
            for (const char *p2 = el + 1; (xe = gs_member(p2, k, sizeof k, &x)); p2 = xe) {
                if (!strcmp(k, "type")) tr_sv(x, ty, sizeof ty);
                else if (!strcmp(k, "text")) tx = x;
                else if (!strcmp(k, "thinking")) th = x;
                else if (!strcmp(k, "id")) tr_sv(x, cid, sizeof cid);
                else if (!strcmp(k, "name")) tr_sv(x, nm, sizeof nm);
                else if (!strcmp(k, "input") && *x == '{') in = x;
            }
            const int is_text = !strcmp(ty, "text"), is_think = !strcmp(ty, "thinking");
            if ((is_text && tx && *tx == '"') || (is_think && th && *th == '"')) {
                Buf t = {0}; const char *p3 = is_text ? tx : th; js_string(&p3, &t); tr_trim(&t); tr_subst(o, &t);
                Buf *dst = is_text ? &m->text : &m->reason;
                if (t.n) { if (dst->n) buf_str(dst, "\n\n"); buf_add(dst, t.p, t.n); }
                free(t.p);
            } else if (!strcmp(ty, "tool_use") && nm[0]) {
                if (m->ncid) buf_str(&m->calls, ", ");
                buf_str(&m->calls, "{\"id\": \""); js_escape(&m->calls, cid, strlen(cid));
                buf_str(&m->calls, "\", \"type\": \"function\", \"function\": {\"name\": \""); js_escape(&m->calls, nm, strlen(nm));
                buf_str(&m->calls, "\", \"arguments\": ");
                if (in) tr_json_subst(o, in, &m->calls); else buf_str(&m->calls, "{}");
                buf_str(&m->calls, "}}");
                m->cid = realloc(m->cid, (size_t)(m->ncid + 1) * sizeof(char *));
                m->cend = realloc(m->cend, (size_t)(m->ncid + 1));
                m->cid[m->ncid] = strdup(cid); m->cend[m->ncid] = (char)ends; m->ncid++;
            }
        }
        return;
    }
    /* registro do usuario: resultados de ferramenta, ou texto */
    Buf text = {0}; int has_res = 0;
    if (*content == '"') { const char *p = content; js_string(&p, &text); }
    else if (*content == '[') {
        for (const char *p = content + 1; (ee = gs_elem(p, &el)); p = ee) {
            if (*el != '{') continue;
            char ty[32] = "", tid[96] = ""; const char *x, *xe, *tx = NULL, *rc = NULL;
            for (const char *p2 = el + 1; (xe = gs_member(p2, k, sizeof k, &x)); p2 = xe) {
                if (!strcmp(k, "type")) tr_sv(x, ty, sizeof ty);
                else if (!strcmp(k, "text")) tx = x;
                else if (!strcmp(k, "tool_use_id")) tr_sv(x, tid, sizeof tid);
                else if (!strcmp(k, "content")) rc = x;
            }
            if (!strcmp(ty, "tool_result")) {
                has_res = 1;
                if (!ep->open || ep->cut) continue;
                if (ep->nans == ep->capans) { ep->capans = ep->capans ? ep->capans * 2 : 32; ep->ans = realloc(ep->ans, (size_t)ep->capans * sizeof(char *)); }
                ep->ans[ep->nans++] = strdup(tid);
                TrMsg *m = tr_push(ep, 't');
                tr_cpy(m->id, tid, sizeof m->id);
                tr_result(rc, &m->text);
                tr_clip(&m->text, o->max_result);
                tr_subst(o, &m->text);
            } else if (!strcmp(ty, "text") && tx && *tx == '"') {
                if (text.n) buf_str(&text, "\n\n");
                const char *p3 = tx; js_string(&p3, &text);
            }
        }
    }
    if (has_res || !text.n) { free(text.p); return; }
    {   /* interrupcao pelo usuario: nada mais entra neste episodio */
        char head[81]; tr_cpy(head, text.p, sizeof head);
        if (strstr(head, "[Request interrupted by user")) { if (ep->open) ep->cut = 1; free(text.p); return; }
    }
    static const char *const local[] = { "<local-command-", "<command-name>", "<command-message>", "<bash-input>", "<bash-stdout>", "<bash-stderr>" };
    const char *t0 = js_ws(text.p);
    int skip = meta;
    for (size_t i = 0; i < sizeof local / sizeof local[0]; i++) if (!strncmp(t0, local[i], strlen(local[i]))) skip = 1;
    if (skip) { free(text.p); return; }
    for (char *a; (a = strstr(text.p, "<system-reminder>")); ) {          /* avisos automaticos */
        char *b = strstr(a, "</system-reminder>");
        if (!b) break;
        b += strlen("</system-reminder>");
        while (*b && strchr(" \t\r\n", *b)) b++;
        memmove(a, b, strlen(b) + 1);
        text.n = strlen(text.p);
    }
    tr_trim(&text);
    if (!text.n) { free(text.p); return; }
    const int notice = !strncmp(text.p, "<task-notification>", 19) || !strncmp(text.p, "[SYSTEM NOTIFICATION", 20);   /* tarefa em segundo plano: continua o episodio */
    const int cont = ep->open && !ep->cut && (notice || o->whole) && !compact;
    if (!cont && notice) { free(text.p); return; }
    if (!cont) { tr_flush(o); ep->open = 1; }
    tr_subst(o, &text);
    TrMsg *m = tr_push(ep, 'u');
    m->text = text;
}

static void tr_file(TrOpt *o, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "import: nao abriu %s\n", path); return; }
    o->nfiles++;
    size_t lc = 1 << 20; char *line = xalloc(lc, 1);
    for (;;) {
        size_t n = 0; int ch;
        while ((ch = fgetc(f)) != EOF && ch != '\n') { if (n + 2 >= lc) { lc *= 2; line = realloc(line, lc); } line[n++] = (char)ch; }
        line[n] = 0;
        if (n) tr_line(o, line);
        if (ch == EOF) break;
    }
    tr_flush(o);
    fclose(f); free(line);
}

#ifndef _WIN32
static int tr_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void tr_walk(const char *dir, char ***v, int *n, int *cap) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char p[4096]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) tr_walk(p, v, n, cap);
        else { const size_t l = strlen(p); if (l > 6 && !strcmp(p + l - 6, ".jsonl")) {
            if (*n == *cap) { *cap = *cap ? *cap * 2 : 64; *v = realloc(*v, (size_t)*cap * sizeof(char *)); }
            (*v)[(*n)++] = strdup(p);
        } }
    }
    closedir(d);
}
#endif

/* a entrada e uma transcricao? pasta, ou arquivo cuja primeira linha e um registro com "type" e sem lista de mensagens */
static int tr_is(const char *path) {
#ifndef _WIN32
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 1;
#endif
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    Buf l = {0}; int ch, r = 0;
    while ((ch = fgetc(f)) != EOF) { if (ch == '\n') { if (l.n) break; continue; } char c1 = (char)ch; buf_add(&l, &c1, 1); }
    fclose(f);
    if (l.p) {
        char k[64]; const char *v, *e, *q = js_ws(l.p); int ty = 0, ms = 0;
        if (*q == '{') for (const char *p = q + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) {
            if (!strcmp(k, "type")) ty = 1;
            else if (!strcmp(k, "messages") || !strcmp(k, "conversations")) ms = 1;
        }
        r = ty && !ms;
    }
    free(l.p);
    return r;
}

/* le a transcricao (arquivo ou pasta) e entrega cada episodio por o->emit */
static void tr_run(TrOpt *o, const char *path) {
#ifndef _WIN32
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        char **v = NULL; int n = 0, cap = 0;
        tr_walk(path, &v, &n, &cap);
        if (n) qsort(v, (size_t)n, sizeof(char *), tr_cmp);
        for (int i = 0; i < n; i++) { tr_file(o, v[i]); free(v[i]); }
        free(v);
        return;
    }
#endif
    tr_file(o, path);
}

/* opcao REGEX=TEXTO; 0 se o padrao nao compila */
static int tr_add_rep(TrOpt *o, char *spec) {
    char *eq = strrchr(spec, '='), err[400];
    if (!eq || eq == spec || o->nrep == 32) { fprintf(stderr, "import: --replace espera REGEX=TEXTO (ate 32): %s\n", spec); return 0; }
    *eq = 0;
    Rx *re = rx_compile(spec, strlen(spec), 0, err, sizeof err);
    if (!re) { fprintf(stderr, "import: --replace %s: %s\n", spec, err); return 0; }
    o->rep[o->nrep].re = re; o->rep[o->nrep].sp = xalloc((size_t)(2 * re->ngroups), sizeof(long)); o->rep[o->nrep].to = eq + 1;
    o->nrep++;
    return 1;
}
