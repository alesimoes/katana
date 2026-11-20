/* ---------- API no formato da OpenAI: GET /v1/models, POST /v1/chat/completions ----------
 * Para que clientes que ja falam esse formato (editores, interfaces web, bibliotecas) usem o katana sem adaptacao.
 * Conversa em texto, com ou sem streaming (SSE), stop, max_tokens/max_completion_tokens, temperature, top_p, seed
 * e response_format (a mesma gramatica JSON do /api/chat). Modelos que raciocinam (<think>) devolvem o raciocinio
 * em reasoning_content, separado da resposta. Ainda sem chamada de ferramentas (tools) e sem imagens: a requisicao
 * que pede isso recebe 400 com o motivo. Uma geracao por vez (srv_lock); as outras esperam na fila. */

#define OA_MAX_STOP 4

typedef struct { char role; char *content; } OaMsg;        /* s sistema, u usuario (e ferramenta), a assistente */
typedef struct {
    OaMsg msg[SRV_MAX_MSGS]; int n;
    int max_tokens, stream, usage, rf, has_seed;
    unsigned seed;
    float temp, top_p;
    char *stop[OA_MAX_STOP]; int nstop;
    char *rf_schema;
    char model[128];
} OaReq;

static void oa_free(OaReq *r) {
    for (int i = 0; i < r->n; i++) free(r->msg[i].content);
    for (int i = 0; i < r->nstop; i++) free(r->stop[i]);
    free(r->rf_schema);
}

static void oa_error(sock_t s, int code, const char *msg, const char *type) {
    Buf b = {0};
    buf_str(&b, "{\"error\":{\"message\":\""); js_escape(&b, msg, strlen(msg));
    buf_str(&b, "\",\"type\":\""); buf_str(&b, type); buf_str(&b, "\",\"code\":null}}");
    http_reply(s, code, "application/json; charset=utf-8", b.p, b.n);
    free(b.p);
}

/* texto de "content": string, ou lista de partes [{"type":"text","text":...}]; 0 se tiver parte que nao e texto */
static int oa_content(const char *val, Buf *out, const char **why) {
    if (*val == '"') { const char *p = val; return js_string(&p, out); }
    if (!strncmp(val, "null", 4)) return 1;
    if (*val != '[') { *why = "content must be a string or a list of text parts"; return 0; }
    const char *q = val + 1, *el, *e;
    while ((e = gs_elem(q, &el))) {
        if (*el == '{') {
            char k[32], ty[32] = ""; const char *v, *q2 = el + 1, *e2, *txt = NULL;
            while ((e2 = gs_member(q2, k, sizeof k, &v))) {
                if (!strcmp(k, "type") && *v == '"') { size_t n = 0; for (const char *w = v + 1; *w && *w != '"' && n < 31; w++) ty[n++] = *w; ty[n] = 0; }
                else if (!strcmp(k, "text") && *v == '"') txt = v;
                q2 = e2;
            }
            if (strcmp(ty, "text") != 0) { *why = "only text content is supported (no images or audio)"; return 0; }
            if (txt) { if (out->n) buf_str(out, "\n"); const char *p = txt; if (!js_string(&p, out)) return 0; }
        } else if (*el == '"') { const char *p = el; if (!js_string(&p, out)) return 0; }
        q = e;
    }
    return 1;
}

static int oa_num(const char *v, double *out) {
    if (!(*v == '-' || (*v >= '0' && *v <= '9'))) return 0;
    *out = strtod(v, NULL); return 1;
}

/* le a requisicao; devolve NULL se esta boa, senao a mensagem do erro */
static const char *oa_parse(const char *js, OaReq *r) {
    const char *p = js_ws(js), *chk = p;
    if (*p != '{' || !js_skip(&chk, 0)) return "invalid JSON body";
    char k[64]; const char *v, *q = p + 1, *e, *why = NULL;
    while ((e = gs_member(q, k, sizeof k, &v))) {
        double d;
        if (!strcmp(k, "messages") && *v == '[') {
            const char *q2 = v + 1, *el, *e2;
            while ((e2 = gs_elem(q2, &el))) {
                if (*el != '{') return "each message must be an object";
                if (r->n >= SRV_MAX_MSGS) return "too many messages";
                char k2[32], role[32] = ""; const char *v2, *q3 = el + 1, *e3, *cv = NULL;
                int tool_calls = 0;
                while ((e3 = gs_member(q3, k2, sizeof k2, &v2))) {
                    if (!strcmp(k2, "role") && *v2 == '"') { size_t n = 0; for (const char *w = v2 + 1; *w && *w != '"' && n < 31; w++) role[n++] = *w; role[n] = 0; }
                    else if (!strcmp(k2, "content")) cv = v2;
                    else if (!strcmp(k2, "tool_calls") && *v2 == '[' && gs_ws(v2 + 1)[0] != ']') tool_calls = 1;
                    q3 = e3;
                }
                if (tool_calls) return "tool calls are not supported yet";
                char rc = !strcmp(role, "system") || !strcmp(role, "developer") ? 's' : !strcmp(role, "user") ? 'u' :
                          !strcmp(role, "assistant") ? 'a' : !strcmp(role, "tool") || !strcmp(role, "function") ? 'u' : 0;
                if (!rc) return "unknown message role";
                Buf c = {0};
                if (cv && !oa_content(cv, &c, &why)) { free(c.p); return why ? why : "invalid message content"; }
                r->msg[r->n].role = rc; r->msg[r->n].content = c.p ? c.p : xalloc(1, 1); r->n++;
                q2 = e2;
            }
        } else if ((!strcmp(k, "max_tokens") || !strcmp(k, "max_completion_tokens")) && oa_num(v, &d)) r->max_tokens = (int)d;
        else if (!strcmp(k, "temperature") && oa_num(v, &d)) r->temp = (float)d;
        else if (!strcmp(k, "top_p") && oa_num(v, &d)) r->top_p = (float)d;
        else if (!strcmp(k, "seed") && oa_num(v, &d)) { r->seed = (unsigned)(long long)d; r->has_seed = 1; }
        else if (!strcmp(k, "n") && oa_num(v, &d)) { if ((int)d != 1) return "only n=1 is supported"; }
        else if (!strcmp(k, "stream")) r->stream = !strncmp(v, "true", 4);
        else if (!strcmp(k, "stream_options") && *v == '{') {
            char k2[32]; const char *v2, *q2 = v + 1, *e2;
            while ((e2 = gs_member(q2, k2, sizeof k2, &v2))) { if (!strcmp(k2, "include_usage")) r->usage = !strncmp(v2, "true", 4); q2 = e2; }
        } else if (!strcmp(k, "model") && *v == '"') {
            Buf m = {0}; const char *pp = v;
            if (js_string(&pp, &m) && m.p) snprintf(r->model, sizeof r->model, "%s", m.p);
            free(m.p);
        } else if (!strcmp(k, "stop")) {
            if (*v == '"' || *v == '[') {
                const char *q2 = *v == '[' ? v + 1 : NULL, *el = v, *e2 = NULL;
                for (;;) {
                    if (q2) { if (!(e2 = gs_elem(q2, &el))) break; }
                    if (*el == '"') {
                        Buf sb = {0}; const char *pp = el;
                        if (!js_string(&pp, &sb)) { free(sb.p); return "invalid stop"; }
                        if (sb.n && r->nstop < OA_MAX_STOP) r->stop[r->nstop++] = sb.p; else free(sb.p);
                    }
                    if (!q2) break;
                    q2 = e2;
                }
            }
        } else if (!strcmp(k, "response_format") && *v == '{') {
            char k2[64], ty[32] = ""; const char *v2, *q2 = v + 1, *e2;
            while ((e2 = gs_member(q2, k2, sizeof k2, &v2))) {
                if (!strcmp(k2, "type") && *v2 == '"') { size_t n = 0; for (const char *w = v2 + 1; *w && *w != '"' && n < 31; w++) ty[n++] = *w; ty[n] = 0; }
                else if (!strcmp(k2, "json_schema") && *v2 == '{') {
                    char k3[64]; const char *v3, *q3 = v2 + 1, *e3;
                    while ((e3 = gs_member(q3, k3, sizeof k3, &v3))) {
                        if (!strcmp(k3, "schema") && *v3 == '{') { const char *ve = gs_skipv(v3); if (ve) { free(r->rf_schema); r->rf_schema = xalloc((size_t)(ve - v3) + 1, 1); memcpy(r->rf_schema, v3, (size_t)(ve - v3)); } }
                        q3 = e3;
                    }
                }
                q2 = e2;
            }
            r->rf = !strcmp(ty, "json_object") ? 1 : !strcmp(ty, "json_schema") ? 2 : 0;
            if (r->rf == 2 && !r->rf_schema) return "response_format json_schema needs json_schema.schema";
        } else if (!strcmp(k, "tools") && *v == '[' && gs_ws(v + 1)[0] != ']') return "tools are not supported yet";
        q = e;
    }
    if (!r->n) return "messages is required";
    return NULL;
}

/* abre um bloco de raciocinio no inicio da resposta? (o texto gerado comeca dentro dele) */
static int oa_think_open(const Config *c) {
    const char *tp = think_pre(c);
    return !strcmp(tp, "<think>\n") || !strcmp(tp, "<think>");
}

/* texto da conversa no formato do modelo, terminando no inicio da resposta do assistente */
static const char *oa_prompt(const Config *c, const OaReq *r, Buf *text) {
    Buf sys = {0}, user = {0};
    int i = 0, first = 1;
    for (; i < r->n && r->msg[i].role == 's'; i++) { if (sys.n) buf_str(&sys, "\n\n"); buf_str(&sys, r->msg[i].content); }
    if (r->msg[r->n - 1].role != 'u') { free(sys.p); return "the last message must be from the user (or a tool result)"; }
    while (i < r->n) {
        buf_clear(&user);
        for (; i < r->n && r->msg[i].role != 'a'; i++) {       /* usuario seguidos (e sistema no meio) viram um turno so */
            if (user.n) buf_str(&user, "\n\n");
            buf_str(&user, r->msg[i].content);
        }
        const size_t cap = user.n + sys.n + 4096;
        char *piece = xalloc(cap, 1);
        turn_text(c, piece, cap, sys.p ? sys.p : "", user.p ? user.p : "", first);
        buf_str(text, piece);
        free(piece);
        first = 0;
        if (i < r->n && r->msg[i].role == 'a') {                /* resposta anterior: raciocinio vazio, como o modelo preve */
            if (oa_think_open(c)) buf_str(text, c->glm_args ? "</think>" : "\n</think>\n\n");
            for (; i < r->n && r->msg[i].role == 'a'; i++) buf_str(text, r->msg[i].content);
        }
    }
    free(sys.p); free(user.p);
    return NULL;
}

/* saida da geracao: separa raciocinio e resposta, procura os stop e manda (streaming) ou junta (resposta unica) */
typedef struct {
    sock_t s; int stream, alive, think, stopped;
    char **stop; int nstop; size_t maxstop;
    Buf hold, content, reason, ev;
    const char *id, *model; long created;
} OaOut;

static void oa_chunk(OaOut *o, const char *field, const char *txt, size_t n, const char *finish) {
    if (!o->alive) return;
    if (field && !n && strcmp(field, "role") != 0) return;        /* nada de pedaco vazio */
    if (!o->stream) { if (n) buf_add(!strcmp(field, "content") ? &o->content : &o->reason, txt, n); return; }
    Buf *b = &o->ev; char hd[400];
    buf_clear(b);
    snprintf(hd, sizeof hd, "{\"id\":\"%s\",\"object\":\"chat.completion.chunk\",\"created\":%ld,\"model\":\"", o->id, o->created);
    buf_str(b, hd); js_escape(b, o->model, strlen(o->model));
    buf_str(b, "\",\"choices\":[{\"index\":0,\"delta\":{");
    if (field) { buf_str(b, "\""); buf_str(b, field); buf_str(b, "\":\""); js_escape(b, txt, n); buf_str(b, "\""); }
    buf_str(b, "},\"finish_reason\":");
    if (finish) { buf_str(b, "\""); buf_str(b, finish); buf_str(b, "\""); } else buf_str(b, "null");
    buf_str(b, "}]}");
    o->alive = sse_send(o->s, b->p, b->n);
}

/* manda o que ja e certo; guarda o fim que ainda pode ser o comeco de "</think>" ou de um stop */
static void oa_feed(OaOut *o, const char *txt, size_t n, int final) {
    buf_add(&o->hold, txt, n);
    for (;;) {
        if (o->stopped) return;
        const char *h = o->hold.p ? o->hold.p : "";
        size_t keep;
        if (o->think) {
            const char *e = strstr(h, "</think>");
            if (e) {
                oa_chunk(o, "reasoning_content", h, (size_t)(e - h), NULL);
                size_t skip = (size_t)(e - h) + 8;
                while (skip < o->hold.n && (h[skip] == '\n' || h[skip] == ' ')) skip++;     /* quebras entre o raciocinio e a resposta */
                memmove(o->hold.p, h + skip, o->hold.n - skip); o->hold.n -= skip; o->hold.p[o->hold.n] = 0;
                o->think = 0;
                continue;
            }
            keep = final ? 0 : 7;
            if (keep > o->hold.n) keep = o->hold.n;
            size_t send = utf8_complete(h, o->hold.n - keep);
            oa_chunk(o, "reasoning_content", h, send, NULL);
            memmove(o->hold.p, h + send, o->hold.n - send); o->hold.n -= send; o->hold.p[o->hold.n] = 0;
            return;
        }
        size_t cut = o->hold.n; int hit = 0;
        for (int i = 0; i < o->nstop; i++) {
            const char *f = strstr(h, o->stop[i]);
            if (f && (size_t)(f - h) < cut) { cut = (size_t)(f - h); hit = 1; }
        }
        if (hit) { oa_chunk(o, "content", h, cut, NULL); o->stopped = 1; o->hold.n = 0; return; }
        keep = final || !o->maxstop ? 0 : o->maxstop - 1;
        if (keep > o->hold.n) keep = o->hold.n;
        size_t send = utf8_complete(h, o->hold.n - keep);
        oa_chunk(o, "content", h, send, NULL);
        memmove(o->hold.p, h + send, o->hold.n - send); o->hold.n -= send; o->hold.p[o->hold.n] = 0;
        return;
    }
}

static void oa_chat(Model *m, sock_t s, const char *body) {
    const Config *c = &m->cfg;
    const Tokenizer *tk = &m->tk;
    OaReq *rq = xalloc(1, sizeof *rq);
    rq->max_tokens = -1; rq->temp = 0.7f; rq->top_p = -1.0f;
    const char *err = oa_parse(body, rq);
    Buf text = {0};
    if (!err) err = oa_prompt(c, rq, &text);
    if (err) { oa_error(s, 400, err, "invalid_request_error"); oa_free(rq); free(rq); free(text.p); return; }
    Ids ids = {0};
    if (c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
    encode(tk, text.p ? text.p : "", &ids);
    free(text.p);
    if (ids.n + 16 >= c->ctx) {
        char b[200];
        snprintf(b, sizeof b, "the conversation is too long for the context (%d tokens of %d)", ids.n, c->ctx);
        oa_error(s, 400, b, "invalid_request_error");
        free(ids.ids); oa_free(rq); free(rq); return;
    }
    int max_tokens = rq->max_tokens > 0 ? rq->max_tokens : c->ctx;
    if (max_tokens > c->ctx - ids.n - 1) max_tokens = c->ctx - ids.n - 1;
    float temp = rq->temp < 0 ? 0 : rq->temp > 2 ? 2 : rq->temp;

    srv_lock();
    if (rq->rf) {                                  /* saida estruturada: valida o schema antes de comecar */
        gr_prepare(tk, c->vocab);
        g_gr.stop2 = -1;
        const int ok = rq->rf == 2 ? gr_json_schema(rq->rf_schema) : (gr_json_any(), 1);
        gr_off();
        if (!ok) { srv_unlock(); oa_error(s, 400, "response_format: invalid or unsupported schema", "invalid_request_error"); free(ids.ids); oa_free(rq); free(rq); return; }
    }
    const float top_p0 = g_top_p;
    if (rq->top_p > 0 && rq->top_p <= 1) g_top_p = rq->top_p;
    if (rq->has_seed) srand(rq->seed);

    /* reuso de prefixo, como no /api/chat */
    int common = 0;
    while (common < ids.n && common < srv_cache.n && ids.ids[common] == srv_cache.ids[common]) common++;
    if (common >= ids.n) common = ids.n - 1;
    if (c->ssm_inner && common != srv_cache.n) common = 0;
    srv_cache.n = common;

    char id[48];
    snprintf(id, sizeof id, "chatcmpl-%08x%08x", (unsigned)rand(), (unsigned)time(NULL));
    OaOut o = {0};
    o.s = s; o.stream = rq->stream; o.alive = 1; o.think = oa_think_open(c) && !rq->rf;
    o.stop = rq->stop; o.nstop = rq->nstop; o.id = id; o.created = (long)time(NULL);
    o.model = rq->model[0] ? rq->model : g_srv_name;
    for (int i = 0; i < rq->nstop; i++) if (strlen(rq->stop[i]) > o.maxstop) o.maxstop = strlen(rq->stop[i]);
    if (o.stream) {
        const char *hd = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\nCache-Control: no-store\r\n"
                         "X-Accel-Buffering: no\r\nConnection: close\r\n\r\n";
        o.alive = sock_send(s, hd, strlen(hd));
        if (o.alive) oa_chunk(&o, "role", "assistant", 9, NULL);
    }

    const double t0 = now_sec();
    float *lg = forward_seq(m, ids.ids + common, ids.n - common, common, 1);
    int pos = ids.n;
    for (int i = common; i < ids.n; i++) ids_push(&srv_cache, ids.ids[i]);
    const double t1 = now_sec();
    if (rq->rf) { if (rq->rf == 2) gr_json_schema(rq->rf_schema); else gr_json_any(); }
    Buf pend = {0};
    char tmp[256];
    int gen = 0, natural = 0;
    while (gen < max_tokens && pos < c->ctx - 1 && o.alive && !o.stopped) {
        g_gr.left = max_tokens - gen;
        const int t = sample_c(lg, c->vocab, temp, c->eos);
        if (t == c->eos || (tk->toktype && tk->toktype[t] == 3)) { natural = 1; break; }
        gr_accept(t);
        buf_add(&pend, tmp, tok_bytes(tk, t, tmp, sizeof tmp));
        const size_t ok = utf8_complete(pend.p, pend.n);
        if (ok > 0) {
            oa_feed(&o, pend.p, ok, 0);
            memmove(pend.p, pend.p + ok, pend.n - ok); pend.n -= ok; pend.p[pend.n] = 0;
        }
        gen++;
        lg = forward(m, t, pos++, 1);
        ids_push(&srv_cache, t);
    }
    if (pend.n) oa_feed(&o, pend.p, pend.n, 0);
    oa_feed(&o, "", 0, 1);
    const double t2 = now_sec();
    gr_off();
    g_top_p = top_p0;
    srv_unlock();

    const char *finish = natural || o.stopped ? "stop" : "length";
    if (o.stream) {
        oa_chunk(&o, NULL, "", 0, finish);
        if (rq->usage && o.alive) {
            char u[512];
            const int k = snprintf(u, sizeof u, "{\"id\":\"%s\",\"object\":\"chat.completion.chunk\",\"created\":%ld,\"model\":\"%s\",\"choices\":[],"
                                   "\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":%d,\"total_tokens\":%d}}",
                                   id, o.created, g_srv_name, ids.n, gen, ids.n + gen);
            o.alive = sse_send(s, u, (size_t)k);
        }
        if (o.alive) o.alive = sse_send(s, "[DONE]", 6);
    } else {
        Buf b = {0}; char hd[400];
        snprintf(hd, sizeof hd, "{\"id\":\"%s\",\"object\":\"chat.completion\",\"created\":%ld,\"model\":\"", id, o.created);
        buf_str(&b, hd); js_escape(&b, o.model, strlen(o.model));
        buf_str(&b, "\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"");
        js_escape(&b, o.content.p ? o.content.p : "", o.content.n);
        buf_str(&b, "\"");
        if (o.reason.n) { buf_str(&b, ",\"reasoning_content\":\""); js_escape(&b, o.reason.p, o.reason.n); buf_str(&b, "\""); }
        snprintf(hd, sizeof hd, "},\"finish_reason\":\"%s\"}],\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":%d,\"total_tokens\":%d}}",
                 finish, ids.n, gen, ids.n + gen);
        buf_str(&b, hd);
        http_reply(s, 200, "application/json; charset=utf-8", b.p, b.n);
        free(b.p);
    }
    fprintf(stderr, "[serve] /v1 prompt %d tokens (%d reaproveitados, %.1f tok/s), resposta %d tokens, %.1f tok/s, fim %s%s\n",
            ids.n, common, (ids.n - common) / (t1 - t0 + 1e-9), gen, gen / (t2 - t1 + 1e-9), finish, o.alive ? "" : " (cliente saiu)");
    free(ids.ids); free(pend.p); free(o.hold.p); free(o.content.p); free(o.reason.p); free(o.ev.p);
    oa_free(rq); free(rq);
}

static void oa_models(sock_t s) {
    Buf b = {0}; char t[64];
    buf_str(&b, "{\"object\":\"list\",\"data\":[{\"id\":\"");
    js_escape(&b, g_srv_name, strlen(g_srv_name));
    snprintf(t, sizeof t, "\",\"object\":\"model\",\"created\":%ld,", (long)g_srv_started);
    buf_str(&b, t);
    buf_str(&b, "\"owned_by\":\"katana\"}]}");
    http_reply(s, 200, "application/json; charset=utf-8", b.p, b.n);
    free(b.p);
}
