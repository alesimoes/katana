/* ---------- API no formato da OpenAI: GET /v1/models, POST /v1/chat/completions ----------
 * Para que clientes que ja falam esse formato (editores, interfaces web, bibliotecas) usem o katana sem adaptacao.
 * Conversa em texto, com ou sem streaming (SSE), stop, max_tokens/max_completion_tokens, temperature, top_p, seed
 * e response_format (a mesma gramatica JSON do /api/chat). Modelos que raciocinam (<think>, ou o canal de
 * pensamento do Gemma 4) devolvem o raciocinio em reasoning_content, separado da resposta.
 * Ferramentas do cliente (tools): as declaracoes e o historico (tool_calls do assistente, mensagens "tool") vao no
 * formato nativo do modelo, com os mesmos escritores do agente e do import (ChatML com chamada em XML ou JSON,
 * Gemma 4, GLM); a chamada que o modelo escreve e lida pelos mesmos leitores do agente e volta em
 * message.tool_calls, com os argumentos convertidos para o tipo declarado no schema. tool_choice required ou uma
 * funcao: o turno do assistente ja comeca com a abertura da chamada (sem gramatica). Imagens: partes image_url com data: URI, quando ha projetor; n = 1.
 * Uma geracao por vez (srv_lock); as outras esperam na fila. */

#define OA_MAX_STOP 4
#define OA_MAX_TOOLS 128
#define OA_MAX_CALLS 32
#define OA_JCAP 98304

/* s sistema, u usuario, a assistente, t resultado de ferramenta */
typedef struct { char role; char *content; char *calls; char *tool_id; char *name; } OaMsg;
typedef struct {
    OaMsg msg[SRV_MAX_MSGS]; int n;
    int max_tokens, stream, usage, rf, has_seed;
    unsigned seed;
    float temp, top_p;
    SmpCfg sp;                                   /* min_p, penalidades, mirostat, DRY, logit_bias, top_k (engine/sampler.c) */
    char *stop[OA_MAX_STOP]; int nstop;
    char *rf_schema;
    char model[128];
    char *tools[OA_MAX_TOOLS]; int ntools;      /* objeto "function" de cada ferramenta (texto JSON) */
    int choice;                                 /* tool_choice: 0 auto, 1 none, 2 required, 3 uma funcao */
    char choice_name[128];
    int parallel;                               /* parallel_tool_calls (0 = so a primeira chamada) */
} OaReq;

static void oa_free(OaReq *r) {
    for (int i = 0; i < r->n; i++) { free(r->msg[i].content); free(r->msg[i].calls); free(r->msg[i].tool_id); free(r->msg[i].name); }
    for (int i = 0; i < r->nstop; i++) free(r->stop[i]);
    for (int i = 0; i < r->ntools; i++) free(r->tools[i]);
    free(r->rf_schema);
}

/* escritores e leitores do agente e do import usados aqui (definidos depois, em importer.c) */
static void im_result(const Config *c, Buf *out, const char *name, const char *res, int first);
static void im_think(const Config *c, Buf *out, const char *reason);
static char *im_args(const char *v);

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
    int after_img = 0;
    while ((e = gs_elem(q, &el))) {
        if (*el == '{') {
            char k[32], ty[32] = ""; const char *v, *q2 = el + 1, *e2, *txt = NULL, *img = NULL;
            while ((e2 = gs_member(q2, k, sizeof k, &v))) {
                if (!strcmp(k, "type") && *v == '"') { size_t n = 0; for (const char *w = v + 1; *w && *w != '"' && n < 31; w++) ty[n++] = *w; ty[n] = 0; }
                else if (!strcmp(k, "text") && *v == '"') txt = v;
                else if (!strcmp(k, "image_url")) img = v;
                q2 = e2;
            }
            if (!strcmp(ty, "image_url") && img) {      /* {"url": "data:image/png;base64,..."} ou a string direto */
                static __thread char ve[240];
                const char *u = NULL, *v3, *q3 = img + 1, *e3; char k3[32];
                if (*img == '"') u = img;
                else if (*img == '{') while ((e3 = gs_member(q3, k3, sizeof k3, &v3))) { if (!strcmp(k3, "url") && *v3 == '"') u = v3; q3 = e3; }
                if (!g_vis.on) { *why = "this model has no vision loaded (no projector file next to it)"; return 0; }
                if (!u || strncmp(u + 1, "data:image/", 11)) { *why = "image_url must be a data: URI (data:image/png;base64,...); remote addresses are not fetched"; return 0; }
                const char *c = strchr(u + 1, ','), *ue = gs_skipv(u);
                if (!c || !ue || c > ue || c - u < 8 || strncmp(c - 7, ";base64", 7)) { *why = "image_url: the data: URI must be base64"; return 0; }
                const int vk = vis_add_b64(c + 1, (size_t)(ue - 1 - (c + 1)), ve, sizeof ve);
                if (vk < 0) { *why = ve; return 0; }
                char mk[80]; vis_mark(vk, mk, sizeof mk);
                buf_str(out, mk); after_img = 1;      /* o formato do modelo nao separa imagem e texto */
                q = e;
                continue;
            }
            if (strcmp(ty, "text") != 0) { *why = "only text and image_url content is supported"; return 0; }
            if (txt) { if (out->n && !after_img) buf_str(out, "\n"); after_img = 0; const char *p = txt; if (!js_string(&p, out)) return 0; }
        } else if (*el == '"') { const char *p = el; if (!js_string(&p, out)) return 0; }
        q = e;
    }
    return 1;
}

static int oa_num(const char *v, double *out) {
    if (!(*v == '-' || (*v >= '0' && *v <= '9'))) return 0;
    *out = strtod(v, NULL); return 1;
}

/* copia do valor JSON que comeca em v */
static char *oa_dup(const char *v) {
    const char *e = gs_skipv(v);
    if (!e) return NULL;
    char *o = xalloc((size_t)(e - v) + 1, 1);
    memcpy(o, v, (size_t)(e - v));
    return o;
}

/* string JSON em v -> texto num buffer fixo; 0 se nao for string */
static int oa_str(const char *v, char *out, size_t cap) {
    out[0] = 0;
    if (!v || *v != '"') return 0;
    Buf b = {0}; const char *p = v;
    const int ok = js_string(&p, &b);
    if (ok) snprintf(out, cap, "%s", b.p ? b.p : "");
    free(b.p);
    return ok;
}

/* nome da ferramenta: letras, digitos, _ . - (1 a 64), como pedem os clientes */
static int oa_name_ok(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 64) return 0;
    for (size_t i = 0; i < n; i++) if (!(isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '.' || s[i] == '-')) return 0;
    return 1;
}

/* nome e argumentos de um item de tool_calls ({"id":..,"type":"function","function":{"name":..,"arguments":".."}}) */
static int oa_call_item(const char *el, char *name, size_t ncap, char *id, size_t icap, const char **args) {
    if (*el != '{') return 0;
    const char *fn = g4_js_get(el, "function");
    if (id) oa_str(g4_js_get(el, "id"), id, icap);
    if (!fn || *fn != '{') return 0;
    if (!oa_str(g4_js_get(fn, "name"), name, ncap) || !name[0]) return 0;
    if (args) *args = g4_js_get(fn, "arguments");
    return 1;
}

/* nome da ferramenta de uma mensagem "tool": pelo tool_call_id nas chamadas anteriores, ou pelo name */
static int oa_tool_msg_name(const OaReq *r, int i, char *name, size_t cap) {
    name[0] = 0;
    if (r->msg[i].tool_id) {
        for (int j = i - 1; j >= 0; j--) {
            if (r->msg[j].role != 'a' || !r->msg[j].calls) continue;
            const char *q = r->msg[j].calls + 1, *el, *e;
            while ((e = gs_elem(q, &el))) {
                char nm[128], id[128];
                if (oa_call_item(el, nm, sizeof nm, id, sizeof id, NULL) && !strcmp(id, r->msg[i].tool_id)) { snprintf(name, cap, "%s", nm); return 1; }
                q = e;
            }
        }
        return 0;
    }
    if (r->msg[i].name) { snprintf(name, cap, "%s", r->msg[i].name); return 1; }
    return 0;
}

/* le a requisicao; devolve NULL se esta boa, senao a mensagem do erro */
static const char *oa_parse(const char *js, OaReq *r) {
    const char *p = js_ws(js), *chk = p;
    r->parallel = 1;
    if (*p != '{' || !js_skip(&chk, 0)) return "invalid JSON body";
    char k[64]; const char *v, *q = p + 1, *e, *why = NULL;
    while ((e = gs_member(q, k, sizeof k, &v))) {
        double d;
        if (!strcmp(k, "messages") && *v == '[') {
            const char *q2 = v + 1, *el, *e2;
            while ((e2 = gs_elem(q2, &el))) {
                if (*el != '{') return "each message must be an object";
                if (r->n >= SRV_MAX_MSGS) return "too many messages";
                char k2[32], role[32] = ""; const char *v2, *q3 = el + 1, *e3, *cv = NULL, *tcv = NULL, *idv = NULL, *nmv = NULL;
                while ((e3 = gs_member(q3, k2, sizeof k2, &v2))) {
                    if (!strcmp(k2, "role") && *v2 == '"') { size_t n = 0; for (const char *w = v2 + 1; *w && *w != '"' && n < 31; w++) role[n++] = *w; role[n] = 0; }
                    else if (!strcmp(k2, "content")) cv = v2;
                    else if (!strcmp(k2, "tool_calls") && *v2 == '[' && gs_ws(v2 + 1)[0] != ']') tcv = v2;
                    else if (!strcmp(k2, "tool_call_id") && *v2 == '"') idv = v2;
                    else if (!strcmp(k2, "name") && *v2 == '"') nmv = v2;
                    q3 = e3;
                }
                char rc = !strcmp(role, "system") || !strcmp(role, "developer") ? 's' : !strcmp(role, "user") ? 'u' :
                          !strcmp(role, "assistant") ? 'a' : !strcmp(role, "tool") || !strcmp(role, "function") ? 't' : 0;
                if (!rc) return "unknown message role";
                if (tcv && rc != 'a') return "only assistant messages can carry tool_calls";
                if (rc == 't' && !idv && !nmv) return "a tool message needs tool_call_id";
                OaMsg *m = &r->msg[r->n];
                memset(m, 0, sizeof *m);
                m->role = rc;
                if (tcv) {
                    const char *q4 = tcv + 1, *el4, *e4;
                    while ((e4 = gs_elem(q4, &el4))) {
                        char nm[128];
                        if (!oa_call_item(el4, nm, sizeof nm, NULL, 0, NULL)) return "each tool_calls item needs function.name";
                        q4 = e4;
                    }
                    m->calls = oa_dup(tcv);
                }
                if (idv) { Buf b = {0}; const char *pp = idv; js_string(&pp, &b); m->tool_id = b.p ? b.p : xalloc(1, 1); }
                if (nmv) { Buf b = {0}; const char *pp = nmv; js_string(&pp, &b); m->name = b.p ? b.p : xalloc(1, 1); }
                Buf c = {0};
                r->n++;                                          /* ja conta: oa_free libera o que foi lido */
                if (cv && !oa_content(cv, &c, &why)) { free(c.p); m->content = xalloc(1, 1); return why ? why : "invalid message content"; }
                m->content = c.p ? c.p : xalloc(1, 1);
                q2 = e2;
            }
        } else if ((!strcmp(k, "max_tokens") || !strcmp(k, "max_completion_tokens")) && oa_num(v, &d)) r->max_tokens = (int)d;
        else if (!strcmp(k, "temperature") && oa_num(v, &d)) r->temp = (float)d;
        else if (!strcmp(k, "top_p") && oa_num(v, &d)) r->top_p = (float)d;
        else if (!strcmp(k, "seed") && oa_num(v, &d)) { r->seed = (unsigned)(long long)d; r->has_seed = 1; }
        else if (smp_json_field(&r->sp, k, v)) { }
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
        } else if (!strcmp(k, "tools")) {
            if (!strncmp(v, "null", 4)) { q = e; continue; }
            if (*v != '[') return "malformed tools: tools must be a list";
            const char *q2 = v + 1, *el, *e2;
            while ((e2 = gs_elem(q2, &el))) {
                if (*el != '{') return "malformed tools: each tool must be an object";
                char ty[32], nm[128];
                if (!oa_str(g4_js_get(el, "type"), ty, sizeof ty) || strcmp(ty, "function") != 0) return "malformed tools: only tools of type \"function\" are supported";
                const char *fn = g4_js_get(el, "function");
                if (!fn || *fn != '{') return "malformed tools: each tool needs a function object";
                if (!oa_str(g4_js_get(fn, "name"), nm, sizeof nm) || !oa_name_ok(nm)) return "malformed tools: function.name is required (1 to 64 letters, digits, _ . or -)";
                const char *pa = g4_js_get(fn, "parameters");
                if (pa && *pa != '{') return "malformed tools: function.parameters must be a JSON Schema object";
                for (int i = 0; i < r->ntools; i++) { char o[128]; oa_str(g4_js_get(r->tools[i], "name"), o, sizeof o); if (!strcmp(o, nm)) return "malformed tools: two tools with the same name"; }
                if (r->ntools >= OA_MAX_TOOLS) return "too many tools (at most 128)";
                r->tools[r->ntools++] = oa_dup(fn);
                q2 = e2;
            }
        } else if (!strcmp(k, "tool_choice")) {
            char t[32];
            if (*v == '"') {
                oa_str(v, t, sizeof t);
                r->choice = !strcmp(t, "auto") ? 0 : !strcmp(t, "none") ? 1 : !strcmp(t, "required") ? 2 : -1;
                if (r->choice < 0) return "tool_choice must be \"auto\", \"none\", \"required\" or {\"type\":\"function\",\"function\":{\"name\":...}}";
            } else if (*v == '{') {
                const char *fn = g4_js_get(v, "function");
                if (!fn || !oa_str(g4_js_get(fn, "name"), r->choice_name, sizeof r->choice_name) || !r->choice_name[0]) return "tool_choice: function.name is required";
                r->choice = 3;
            } else if (strncmp(v, "null", 4) != 0) return "tool_choice must be a string or an object";
        } else if (!strcmp(k, "parallel_tool_calls")) r->parallel = strncmp(v, "false", 5) != 0;
        q = e;
    }
    if (!r->n) return "messages is required";
    if (r->choice >= 2 && !r->ntools) return "tool_choice required or a function needs tools";
    if (r->choice == 3) {
        int found = 0;
        for (int i = 0; i < r->ntools; i++) { char o[128]; oa_str(g4_js_get(r->tools[i], "name"), o, sizeof o); if (!strcmp(o, r->choice_name)) found = 1; }
        if (!found) return "tool_choice names a function that is not in tools";
    }
    for (int i = 0; i < r->n; i++) {                   /* cada resultado responde a uma chamada anterior */
        if (r->msg[i].role != 't') continue;
        char nm[128];
        if (!oa_tool_msg_name(r, i, nm, sizeof nm)) return r->msg[i].tool_id ? "a tool message has a tool_call_id that matches no earlier tool call" : "a tool message needs tool_call_id";
    }
    return NULL;
}

/* abre um bloco de raciocinio no inicio da resposta? (o texto gerado comeca dentro dele) */
static int oa_think_open(const Config *c) {
    const char *tp = think_pre(c);
    return !strcmp(tp, "<think>\n") || !strcmp(tp, "<think>");
}

/* texto da conversa no formato do modelo, terminando no inicio da resposta do assistente (conversa sem ferramentas) */
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

/* ---- ferramentas do cliente no formato nativo do modelo ---- */

/* formatos com chamada de ferramenta: ChatML (XML ou JSON), Gemma 4, GLM 4.5+ */
static int oa_fmt_tools(const Config *c) { return c->chat_fmt == 0 || c->chat_fmt == 8 || (c->chat_fmt == 10 && c->glm_args); }
/* os escritores do agente leem o formato destas globais (as mesmas para o modelo inteiro) */
static void oa_fmt_globals(const Config *c) {
    const int g4 = c->chat_fmt == 8, glm = c->chat_fmt == 10 && c->glm_args;
    g_ah_g4 = g4; g_glm = glm; g_tool_glm = glm; g_tool_xml = c->tool_xml;
}
/* a requisicao precisa do caminho com ferramentas: declara ferramentas ou traz chamadas/resultados no historico */
static int oa_uses_tools(const OaReq *r) {
    if (r->ntools) return 1;
    for (int i = 0; i < r->n; i++) if (r->msg[i].role == 't' || r->msg[i].calls) return 1;
    return 0;
}

/* JSON numa linha, com ": " e ", " entre os itens (como o tojson do template) */
static void oa_min(Buf *o, const char *v) {
    const char *e = gs_skipv(v);
    if (!e) return;
    for (const char *p = v; p < e; p++) {
        if (*p == '"') { const char *q = gs_skipv(p); if (!q) return; buf_add(o, p, (size_t)(q - p)); p = q - 1; continue; }
        if (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') continue;
        buf_add(o, p, 1);
        if (*p == ':' || *p == ',') buf_add(o, " ", 1);
    }
}

/* bloco "# Tools" do sistema, como o agente escreve (agent_build_sys): declaracoes em JSON e a instrucao do formato */
static void oa_tools_block(const OaReq *r, Buf *o) {
    buf_str(o, "# Tools\n\nYou may call one or more functions to assist with the user query.\n\n"
               "You are provided with function signatures within <tools></tools> XML tags:\n<tools>\n");
    for (int i = 0; i < r->ntools; i++) { buf_str(o, "{\"type\": \"function\", \"function\": "); oa_min(o, r->tools[i]); buf_str(o, "}\n"); }
    if (g_tool_glm) buf_str(o,
        "</tools>\n\nFor each function call, output the function name and arguments within the following XML format:\n"
        "<tool_call>{function-name}<arg_key>{arg-key-1}</arg_key><arg_value>{arg-value-1}</arg_value><arg_key>{arg-key-2}</arg_key><arg_value>{arg-value-2}</arg_value>...</tool_call>");
    else if (g_tool_xml) buf_str(o,
        "</tools>\n\nIf you choose to call a function ONLY reply in the following format with NO suffix:\n\n"
        "<tool_call>\n<function=example_function_name>\n<parameter=example_parameter_1>\nvalue_1\n</parameter>\n</function>\n</tool_call>\n\n"
        "<IMPORTANT>\nReminder:\n- Function calls MUST follow the specified format: an inner <function=...></function> block must be nested within <tool_call></tool_call> XML tags\n"
        "- Required parameters MUST be specified\n</IMPORTANT>");
    else buf_str(o,
        "</tools>\n\nFor each function call, return a json object with function name and arguments within <tool_call></tool_call> XML tags:\n"
        "<tool_call>\n{\"name\": <function-name>, \"arguments\": <args-json-object>}\n</tool_call>");
}

/* conversa com ferramentas no formato do modelo (o mesmo historico do agente: ah_render, im_call, im_result).
   forced: abertura da chamada ja escrita no prompt (tool_choice required ou funcao). think: 0 a resposta comeca
   fora do raciocinio, 1 dentro de <think>, 2 dentro do canal de pensamento do Gemma 4 */
static const char *oa_prompt_tools(const Config *c, const OaReq *r, Buf *text, Buf *forced, int *think) {
    *think = 0;
    if (!oa_fmt_tools(c)) return "tools are not supported for this model's chat format (ChatML, Gemma 4 and GLM are)";
    const char last = r->msg[r->n - 1].role;
    if (last != 'u' && last != 't') return "the last message must be from the user or a tool result";
    oa_fmt_globals(c);
    const int g4 = c->chat_fmt == 8, glm = g_glm;
    Buf sys = {0}, cur = {0};
    int i = 0;
    for (; i < r->n && r->msg[i].role == 's'; i++) { if (sys.n) buf_str(&sys, "\n\n"); buf_str(&sys, r->msg[i].content); }
    if (r->ntools && r->choice != 1) {                 /* tool_choice none: as ferramentas nao sao declaradas */
        Buf tb = {0};
        oa_tools_block(r, &tb);
        if (g4) {                                      /* Gemma 4: declaracoes nativas no fim do sistema */
            const size_t cap = sys.n + 2 * tb.n + 65536;
            char *s = xalloc(cap, 1);
            snprintf(s, cap, "%s%s%s", sys.p ? sys.p : "", sys.n ? "\n\n" : "", tb.p);
            g4_agent_sys(s, cap);
            buf_clear(&sys); buf_str(&sys, s);
            free(s);
        } else { if (sys.n) buf_str(&sys, "\n\n"); buf_str(&sys, tb.p); }
        free(tb.p);
    }
    AHist h = {0};
    char prev = 0;
    while (i < r->n) {
        const char ro = r->msg[i].role;
        buf_clear(&cur);
        if (ro == 'u' || ro == 's') {                  /* usuario seguidos (e sistema no meio) viram um turno so */
            for (; i < r->n && (r->msg[i].role == 'u' || r->msg[i].role == 's'); i++) { if (cur.n) buf_str(&cur, "\n\n"); buf_str(&cur, r->msg[i].content); }
            ah_push(&h, 'u', cur.p ? cur.p : "", cur.n);
        } else if (ro == 'a') {
            if (g4 && prev == 't') { if (g4_think()) buf_str(&cur, "<channel|>"); }   /* o canal ja foi aberto depois do resultado */
            else im_think(c, &cur, NULL);              /* raciocinio vazio nas falas anteriores */
            int ncall = 0;
            for (; i < r->n && r->msg[i].role == 'a'; i++) {
                buf_str(&cur, r->msg[i].content);
                if (!r->msg[i].calls) continue;
                const char *q = r->msg[i].calls + 1, *el, *e;
                while ((e = gs_elem(q, &el))) {
                    char nm[128]; const char *av = NULL;
                    if (oa_call_item(el, nm, sizeof nm, NULL, 0, &av)) {
                        char *args = av ? im_args(av) : NULL;
                        if (!g4) {                     /* separador antes da chamada, como o template escreve */
                            const char tail = cur.n ? cur.p[cur.n - 1] : '\n';
                            if (ncall > 0) buf_str(&cur, "\n");
                            else if (tail != '\n') buf_str(&cur, glm ? "\n" : "\n\n");
                        }
                        im_call(c, &cur, nm, args ? args : "{}");
                        free(args);
                        ncall++;
                    }
                    q = e;
                }
            }
            ah_push(&h, 'a', cur.p ? cur.p : "", cur.n);
        } else {                                       /* resultados seguidos: uma mensagem, como o agente embrulha */
            for (int k = 0; i < r->n && r->msg[i].role == 't'; i++, k++) {
                char nm[128];
                oa_tool_msg_name(r, i, nm, sizeof nm);
                im_result(c, &cur, nm, r->msg[i].content, k == 0);
            }
            ah_push(&h, 't', cur.p ? cur.p : "", cur.n);
        }
        prev = ro == 's' ? 'u' : ro;
    }
    ah_render(&h, sys.p ? sys.p : "", text);
    int topen = 0;
    if (!g4) {
        buf_str(text, tf_end()); buf_str(text, tf_asst()); buf_str(text, think_pre(c));
        topen = oa_think_open(c);
    } else if (last == 't' && g4_think()) topen = 2;   /* ah_render ja abriu o canal de pensamento */
    if (r->choice >= 2) {                              /* chamada obrigatoria: o turno ja comeca pela abertura dela */
        if (topen == 1) buf_str(text, glm ? "</think>" : "\n</think>\n\n");
        else if (topen == 2) buf_str(text, "<channel|>");
        else if (g4 && g4_think()) buf_str(text, "<|channel>thought\n<channel|>");
        topen = 0;
        const char *nm = r->choice == 3 ? r->choice_name : NULL;
        if (g4) { buf_str(forced, "<|tool_call>call:"); if (nm) { buf_str(forced, nm); buf_str(forced, "{"); } }
        else if (glm) { buf_str(forced, "<tool_call>"); if (nm) buf_str(forced, nm); }
        else if (c->tool_xml) { buf_str(forced, "<tool_call>\n<function="); if (nm) { buf_str(forced, nm); buf_str(forced, ">\n"); } }
        else { buf_str(forced, "<tool_call>\n{\"name\": \""); if (nm) { js_escape(forced, nm, strlen(nm)); buf_str(forced, "\", \"arguments\": "); } }
        buf_add(text, forced->p, forced->n);
    }
    *think = topen;
    ah_clear(&h); free(h.v); free(sys.p); free(cur.p);
    return NULL;
}

/* valor de texto -> o tipo do schema (integer, number, boolean, null, array, object); 0 = fica texto */
static int oa_typed_value(Buf *o, const char *v, const char *sch) {
    const char *tv = sch ? g4_js_get(sch, "type") : NULL;
    if (!tv) return 0;
    char types[6][16]; int nt = 0;
    if (*tv == '"') { oa_str(tv, types[0], sizeof types[0]); nt = 1; }
    else if (*tv == '[') { const char *q = tv + 1, *el, *e; while (nt < 6 && (e = gs_elem(q, &el))) { if (oa_str(el, types[nt], sizeof types[0])) nt++; q = e; } }
    for (int i = 0; i < nt; i++) if (!strcmp(types[i], "string")) return 0;   /* aceita texto: fica texto */
    Buf s = {0}; const char *pp = v;
    if (!js_string(&pp, &s)) { free(s.p); return 0; }
    const char *t = s.p ? s.p : "";
    while (*t == ' ' || *t == '\n' || *t == '\t' || *t == '\r') t++;
    size_t n = strlen(t);
    while (n && (t[n - 1] == ' ' || t[n - 1] == '\n' || t[n - 1] == '\t' || t[n - 1] == '\r')) n--;
    char *x = xalloc(n + 1, 1); memcpy(x, t, n);
    int ok = 0;
    for (int i = 0; i < nt && !ok; i++) {
        const char *ty = types[i];
        if (!strcmp(ty, "integer")) {
            size_t k = x[0] == '-' ? 1 : 0;
            if (x[k]) { ok = 1; for (; x[k]; k++) if (x[k] < '0' || x[k] > '9') ok = 0; }
            if (ok) buf_str(o, x);
        } else if (!strcmp(ty, "number")) {
            char *end = NULL;
            if (n && strspn(x, "+-.0123456789eE") == n) { strtod(x, &end); ok = end && !*end && x[0] != '+' && x[0] != '.'; }
            if (ok) buf_str(o, x);
        } else if (!strcmp(ty, "boolean")) {
            if (!strcmp(x, "true") || !strcmp(x, "True")) { buf_str(o, "true"); ok = 1; }
            else if (!strcmp(x, "false") || !strcmp(x, "False")) { buf_str(o, "false"); ok = 1; }
        } else if (!strcmp(ty, "null")) {
            if (!strcmp(x, "null") || !strcmp(x, "None")) { buf_str(o, "null"); ok = 1; }
        } else if ((!strcmp(ty, "array") && x[0] == '[') || (!strcmp(ty, "object") && x[0] == '{')) {
            const char *q = x;
            if (js_skip(&q, 0) && !*js_ws(q)) { oa_min(o, x); ok = 1; }
        }
    }
    free(x); free(s.p);
    return ok;
}

/* argumentos lidos da chamada (objeto JSON, valores em texto nos formatos XML e GLM) -> JSON compacto com os tipos
   declarados nas properties da ferramenta */
static char *oa_typed_args(const OaReq *r, const char *name, const char *obj) {
    const char *props = NULL;
    for (int i = 0; i < r->ntools; i++) {
        char nm[128];
        if (oa_str(g4_js_get(r->tools[i], "name"), nm, sizeof nm) && !strcmp(nm, name)) {
            const char *pa = g4_js_get(r->tools[i], "parameters");
            props = pa ? g4_js_get(pa, "properties") : NULL;
            if (props && *props != '{') props = NULL;
        }
    }
    const char *q = obj ? js_ws(obj) : "", *chk = q;
    if (*q != '{' || !js_skip(&chk, 0)) return strdup("{}");
    Buf o = {0};
    buf_str(&o, "{");
    char k[256]; const char *v, *e; int first = 1;
    for (const char *p = q + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) {
        if (!first) buf_str(&o, ", ");
        first = 0;
        buf_str(&o, "\""); js_escape(&o, k, strlen(k)); buf_str(&o, "\": ");
        const char *sch = props ? g4_js_get(props, k) : NULL;
        if (*v == '"' && sch && oa_typed_value(&o, v, sch)) continue;
        oa_min(&o, v);
    }
    buf_str(&o, "}");
    return o.p;
}

typedef struct { char name[128]; char *args; } OaCall;

/* chamadas no texto do modelo (a partir da abertura), com os leitores do agente; devolve quantas */
static int oa_calls_parse(const Config *c, const OaReq *r, const char *raw, OaCall *out, int max) {
    const int glm = c->chat_fmt == 10 && c->glm_args;
    const int g4n = strstr(raw, "<|tool_call>") != NULL;      /* Gemma 4 nativo; senao <tool_call> (tambem o reserva do Gemma) */
    const char *open_t = g4n ? "<|tool_call>" : "<tool_call>", *close_t = g4n ? "<tool_call|>" : "</tool_call>";
    char *js = xalloc(OA_JCAP, 1);
    int n = 0;
    for (const char *tc = strstr(raw, open_t); tc && n < max; tc = strstr(tc + 1, open_t)) {
        const char *end = strstr(tc, close_t);
        if (!end) end = raw + strlen(raw);
        char name[128] = ""; const char *obj = NULL; char *own = NULL;
        js[0] = 0;
        if (g4n) { if (!g4_call_parse(tc, end, name, sizeof name, js, OA_JCAP)) continue; obj = strchr(js, '{'); }
        else if (glm && glm_call_parse(tc, end, name, sizeof name, js, OA_JCAP)) obj = strchr(js, '{');
        else if (tool_xml_parse(tc, end, name, sizeof name, js, OA_JCAP)) obj = strchr(js, '{');
        else {                                         /* {"name": ..., "arguments": {...}} */
            const char *b = strchr(tc, '{');
            if (!b || b >= end) continue;
            const size_t bl = (size_t)(end - b);
            char *body = xalloc(bl + 1, 1); memcpy(body, b, bl);
            if (!oa_str(g4_js_get(body, "name"), name, sizeof name)) { free(body); continue; }
            const char *av = g4_js_get(body, "arguments");
            if (!av) av = g4_js_get(body, "parameters");
            if (av && *av == '"') own = im_args(av);
            else if (av) own = oa_dup(av);
            free(body);
            obj = own;
        }
        if (!name[0]) { free(own); continue; }
        snprintf(out[n].name, sizeof out[n].name, "%s", name);
        out[n].args = oa_typed_args(r, name, obj);
        free(own);
        n++;
    }
    free(js);
    return n;
}

/* saida da geracao: separa raciocinio e resposta, procura os stop e a abertura de chamada, e manda (streaming) ou
   junta (resposta unica) */
typedef struct {
    sock_t s; int stream, alive, think, stopped, lstrip;   /* lstrip: tira os espacos e quebras do comeco da resposta */
    int g4start, g4strip;                    /* Gemma 4: a resposta pode abrir o canal de pensamento; tira o "thought\n" */
    const char *tclose;                      /* fecho do raciocinio: "</think>" (padrao) ou "<channel|>" */
    int calls_on, in_calls;                  /* procura a abertura de chamada; depois dela tudo vai para calls */
    char **stop; int nstop; size_t maxstop;
    Buf hold, content, reason, ev, calls;
    const char *id, *model; long created;
} OaOut;

/* um pedaco do stream: delta e o miolo do objeto "delta" (JSON sem as chaves) */
static void oa_delta(OaOut *o, const char *delta, const char *finish) {
    if (!o->alive || !o->stream) return;
    Buf *b = &o->ev; char hd[400];
    buf_clear(b);
    snprintf(hd, sizeof hd, "{\"id\":\"%s\",\"object\":\"chat.completion.chunk\",\"created\":%ld,\"model\":\"", o->id, o->created);
    buf_str(b, hd); js_escape(b, o->model, strlen(o->model));
    buf_str(b, "\",\"choices\":[{\"index\":0,\"delta\":{"); buf_str(b, delta); buf_str(b, "},\"finish_reason\":");
    if (finish) { buf_str(b, "\""); buf_str(b, finish); buf_str(b, "\""); } else buf_str(b, "null");
    buf_str(b, "}]}");
    o->alive = sse_send(o->s, b->p, b->n);
}

static void oa_chunk(OaOut *o, const char *field, const char *txt, size_t n, const char *finish) {
    if (!o->alive) return;
    if (field && !n && strcmp(field, "role") != 0) return;        /* nada de pedaco vazio */
    if (!o->stream) { if (n) buf_add(!strcmp(field, "content") ? &o->content : &o->reason, txt, n); return; }
    Buf d = {0};
    if (field) { buf_str(&d, "\""); buf_str(&d, field); buf_str(&d, "\":\""); js_escape(&d, txt, n); buf_str(&d, "\""); }
    oa_delta(o, d.p ? d.p : "", finish);
    free(d.p);
}

static void oa_take(OaOut *o, size_t n) {          /* tira n bytes do comeco do que esta guardado */
    if (!n || !o->hold.p) return;
    memmove(o->hold.p, o->hold.p + n, o->hold.n - n); o->hold.n -= n; o->hold.p[o->hold.n] = 0;
}

/* manda o que ja e certo; guarda o fim que ainda pode ser o comeco do fecho do raciocinio, de um stop ou de uma
   chamada */
static void oa_feed(OaOut *o, const char *txt, size_t n, int final) {
    buf_add(&o->hold, txt, n);
    for (;;) {
        if (o->stopped) return;
        const char *h = o->hold.p ? o->hold.p : "";
        size_t keep;
        if (o->in_calls) { buf_add(&o->calls, h, o->hold.n); oa_take(o, o->hold.n); return; }
        if (o->g4start && !o->think) {                 /* Gemma 4 com raciocinio: a resposta abre <|channel>thought */
            static const char ch[] = "<|channel>";
            if (!final && o->hold.n < 10 && !strncmp(ch, h, o->hold.n)) return;
            o->g4start = 0;
            if (!strncmp(h, ch, 10)) { oa_take(o, 10); o->think = 1; o->tclose = "<channel|>"; o->g4strip = 1; continue; }
        }
        if (o->think) {
            if (o->g4strip) {                          /* nome do canal: "thought\n" */
                if (!final && o->hold.n < 8 && !strncmp("thought\n", h, o->hold.n)) return;
                o->g4strip = 0;
                oa_take(o, !strncmp(h, "thought\n", 8) ? 8 : !strncmp(h, "thought", 7) ? 7 : 0);
                continue;
            }
            const char *tc = o->tclose ? o->tclose : "</think>";
            const size_t tl = strlen(tc);
            const char *e = strstr(h, tc);
            if (e) {
                oa_chunk(o, "reasoning_content", h, (size_t)(e - h), NULL);
                oa_take(o, (size_t)(e - h) + tl);
                o->think = 0; o->lstrip = 1;                 /* as quebras entre o raciocinio e a resposta podem vir nos proximos tokens */
                continue;
            }
            keep = final ? 0 : tl - 1;
            if (keep > o->hold.n) keep = o->hold.n;
            size_t send = utf8_complete(h, o->hold.n - keep);
            oa_chunk(o, "reasoning_content", h, send, NULL);
            oa_take(o, send);
            return;
        }
        if (o->lstrip) {
            size_t sk = 0;
            while (sk < o->hold.n && (h[sk] == '\n' || h[sk] == ' ' || h[sk] == '\r')) sk++;
            oa_take(o, sk);
            if (!o->hold.n) return;
            o->lstrip = 0;
            h = o->hold.p;
        }
        size_t cut = o->hold.n; int hit = 0;
        for (int i = 0; i < o->nstop; i++) {
            const char *f = strstr(h, o->stop[i]);
            if (f && (size_t)(f - h) < cut) { cut = (size_t)(f - h); hit = 1; }
        }
        if (o->calls_on) {                             /* abertura de chamada antes de qualquer stop */
            const char *a = strstr(h, "<|tool_call>"), *b = strstr(h, "<tool_call>");
            const char *op = a && (!b || a < b) ? a : b;
            if (op && (size_t)(op - h) <= cut) {
                const size_t at = (size_t)(op - h);
                oa_chunk(o, "content", h, at, NULL);
                buf_add(&o->calls, op, o->hold.n - at);
                oa_take(o, o->hold.n);
                o->in_calls = 1;
                return;
            }
        }
        if (hit) { oa_chunk(o, "content", h, cut, NULL); o->stopped = 1; o->hold.n = 0; return; }
        keep = final || !o->maxstop ? 0 : o->maxstop - 1;
        if (!final && o->calls_on && keep < 11) keep = 11;   /* "<|tool_call>" tem 12 bytes */
        if (keep > o->hold.n) keep = o->hold.n;
        size_t send = utf8_complete(h, o->hold.n - keep);
        oa_chunk(o, "content", h, send, NULL);
        oa_take(o, send);
        return;
    }
}

static void oa_call_id(char *id, size_t cap) {
    snprintf(id, cap, "call_%08x%08x%08x", (unsigned)rand(), (unsigned)rand(), (unsigned)rand());
}

static void oa_chat(Model *m, sock_t s, const char *body) {
    const Config *c = &m->cfg;
    const Tokenizer *tk = &m->tk;
    OaReq *rq = xalloc(1, sizeof *rq);
    rq->max_tokens = -1; rq->temp = 0.7f; rq->top_p = -1.0f; rq->sp = g_smp;
    const char *err = oa_parse(body, rq);
    Buf text = {0}, forced = {0};
    int tpath = 0, tthink = 0;
    if (!err) {
        tpath = oa_uses_tools(rq);
        err = tpath ? oa_prompt_tools(c, rq, &text, &forced, &tthink) : oa_prompt(c, rq, &text);
    }
    if (err) { oa_error(s, 400, err, "invalid_request_error"); oa_free(rq); free(rq); free(text.p); free(forced.p); return; }
    Ids ids = {0};
    if (c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
    encode(tk, text.p ? text.p : "", &ids);
    if (getenv("KATANA_TEST_PROMPT_LOG")) fprintf(stderr, "[serve] prompt:\n%s\n[serve] fim do prompt\n", text.p ? text.p : "");
    free(text.p);
    if (ids.n + 16 >= c->ctx) {
        char b[200];
        snprintf(b, sizeof b, "the conversation is too long for the context (%d tokens of %d)", ids.n, c->ctx);
        oa_error(s, 400, b, "invalid_request_error");
        free(ids.ids); free(forced.p); oa_free(rq); free(rq); return;
    }
    int max_tokens = rq->max_tokens > 0 ? rq->max_tokens : c->ctx;
    if (max_tokens > c->ctx - ids.n - 1) max_tokens = c->ctx - ids.n - 1;
    float temp = rq->temp < 0 ? 0 : rq->temp > 2 ? 2 : rq->temp;
    /* gancho de teste: KATANA_TEST_RAW_OUTPUT e o texto cru que o "modelo" escreve (um modelo aleatorio nao chama
       ferramentas); o prompt e montado e tokenizado igual, so a geracao e trocada */
    const char *fake = getenv("KATANA_TEST_RAW_OUTPUT");

    srv_lock();
    if (rq->rf) {                                  /* saida estruturada: valida o schema antes de comecar */
        gr_prepare(tk, c->vocab);
        g_gr.stop2 = -1;
        const int ok = rq->rf == 2 ? gr_json_schema(rq->rf_schema) : (gr_json_any(), 1);
        gr_off();
        if (!ok) { srv_unlock(); oa_error(s, 400, "response_format: invalid or unsupported schema", "invalid_request_error"); free(ids.ids); free(forced.p); oa_free(rq); free(rq); return; }
    }
    const float top_p0 = g_top_p;
    if (rq->top_p > 0 && rq->top_p <= 1) g_top_p = rq->top_p;
    SmpScope spsv; smp_scope_begin(&spsv, &rq->sp, rq->has_seed, rq->seed, ids.ids, ids.n);

    /* reuso de prefixo, como no /api/chat */
    int common = 0;
    if (!fake) {
        while (common < ids.n && common < srv_cache.n && ids.ids[common] == srv_cache.ids[common]) common++;
        common = snap_rewind(m, ids.ids, srv_cache.n, common, ids.n);
        srv_cache.n = common;
    }

    char id[48];
    snprintf(id, sizeof id, "chatcmpl-%08x%08x", (unsigned)rand(), (unsigned)time(NULL));
    OaOut o = {0};
    o.s = s; o.stream = rq->stream; o.alive = 1;
    if (tpath) { o.think = tthink && !rq->rf; o.tclose = tthink == 2 ? "<channel|>" : "</think>"; }
    else o.think = oa_think_open(c) && !rq->rf;
    o.g4start = c->chat_fmt == 8 && !rq->rf && !forced.n;
    o.calls_on = tpath && rq->ntools && rq->choice != 1 && !rq->rf;
    o.stop = rq->stop; o.nstop = rq->nstop; o.id = id; o.created = (long)time(NULL);
    o.model = rq->model[0] ? rq->model : g_srv_name;
    for (int i = 0; i < rq->nstop; i++) if (strlen(rq->stop[i]) > o.maxstop) o.maxstop = strlen(rq->stop[i]);
    if (o.stream) {
        const char *hd = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\nCache-Control: no-store\r\n"
                         "X-Accel-Buffering: no\r\nConnection: close\r\n\r\n";
        o.alive = sock_send(s, hd, strlen(hd));
        if (o.alive) oa_chunk(&o, "role", "assistant", 9, NULL);
    }
    if (forced.n) oa_feed(&o, forced.p, forced.n, 0);     /* a abertura da chamada ja esta no prompt */

    const double t0 = now_sec();
    double t1 = t0;
    int gen = 0, natural = 0;
    Buf pend = {0};
    if (fake) {
        const size_t L = strlen(fake);
        for (size_t i = 0; i < L && o.alive && !o.stopped; i += 3) { oa_feed(&o, fake + i, L - i < 3 ? L - i : 3, 0); gen++; }
        natural = 1;
    } else {
        float *lg = snap_feed(m, ids.ids, ids.n, common);
        int pos = ids.n;
        for (int i = common; i < ids.n; i++) ids_push(&srv_cache, ids.ids[i]);
        t1 = now_sec();
        if (rq->rf) { if (rq->rf == 2) gr_json_schema(rq->rf_schema); else gr_json_any(); }
        /* Gemma 4 e GLM: o modelo para ao abrir a resposta da ferramenta (como no agente) */
        const int stop2 = c->chat_fmt == 8 ? map_get(&tk->vocab, "<|tool_response>", 16) : c->chat_fmt == 10 ? map_get(&tk->vocab, "<|observation|>", 15) : -1;
        char tmp[256];
        while (gen < max_tokens && pos < c->ctx - 1 && o.alive && !o.stopped) {
            g_gr.left = max_tokens - gen;
            const int t = sample_c(lg, c->vocab, temp, c->eos);
            if (t == c->eos || (stop2 >= 0 && t == stop2) || (tk->toktype && tk->toktype[t] == 3)) { natural = 1; break; }
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
    }
    if (pend.n) oa_feed(&o, pend.p, pend.n, 0);
    oa_feed(&o, "", 0, 1);
    const double t2 = now_sec();
    gr_off();
    g_top_p = top_p0;
    smp_scope_end(&spsv);
    srv_unlock();

    OaCall calls[OA_MAX_CALLS];
    int ncalls = 0;
    if (o.in_calls) {
        ncalls = oa_calls_parse(c, rq, o.calls.p ? o.calls.p : "", calls, OA_MAX_CALLS);
        if (!rq->parallel && ncalls > 1) { for (int i = 1; i < ncalls; i++) free(calls[i].args); ncalls = 1; }
        if (!ncalls && o.calls.n) oa_chunk(&o, "content", o.calls.p, o.calls.n, NULL);   /* nao virou chamada: e texto */
    }
    const char *finish = ncalls ? "tool_calls" : natural || o.stopped ? "stop" : "length";
    char cid[OA_MAX_CALLS][48];
    for (int i = 0; i < ncalls; i++) oa_call_id(cid[i], sizeof cid[i]);
    if (o.stream) {
        for (int i = 0; i < ncalls && o.alive; i++) {  /* uma chamada: id e nome, depois os argumentos */
            Buf d = {0}; char hd[200];
            snprintf(hd, sizeof hd, "\"tool_calls\":[{\"index\":%d,\"id\":\"%s\",\"type\":\"function\",\"function\":{\"name\":\"", i, cid[i]);
            buf_str(&d, hd); js_escape(&d, calls[i].name, strlen(calls[i].name)); buf_str(&d, "\",\"arguments\":\"\"}}]");
            oa_delta(&o, d.p, NULL);
            buf_clear(&d);
            snprintf(hd, sizeof hd, "\"tool_calls\":[{\"index\":%d,\"function\":{\"arguments\":\"", i);
            buf_str(&d, hd); js_escape(&d, calls[i].args, strlen(calls[i].args)); buf_str(&d, "\"}}]");
            oa_delta(&o, d.p, NULL);
            free(d.p);
        }
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
        buf_str(&b, "\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":");
        size_t cn = o.content.n;
        if (ncalls) while (cn && (o.content.p[cn - 1] == '\n' || o.content.p[cn - 1] == ' ' || o.content.p[cn - 1] == '\r')) cn--;
        if (ncalls && !cn) buf_str(&b, "null");
        else { buf_str(&b, "\""); js_escape(&b, o.content.p ? o.content.p : "", cn); buf_str(&b, "\""); }
        size_t rs = 0;
        while (rs < o.reason.n && (o.reason.p[rs] == '\n' || o.reason.p[rs] == ' ' || o.reason.p[rs] == '\r')) rs++;
        if (rs < o.reason.n) { buf_str(&b, ",\"reasoning_content\":\""); js_escape(&b, o.reason.p, o.reason.n); buf_str(&b, "\""); }
        if (ncalls) {
            buf_str(&b, ",\"tool_calls\":[");
            for (int i = 0; i < ncalls; i++) {
                snprintf(hd, sizeof hd, "%s{\"id\":\"%s\",\"type\":\"function\",\"function\":{\"name\":\"", i ? "," : "", cid[i]);
                buf_str(&b, hd); js_escape(&b, calls[i].name, strlen(calls[i].name));
                buf_str(&b, "\",\"arguments\":\""); js_escape(&b, calls[i].args, strlen(calls[i].args)); buf_str(&b, "\"}}");
            }
            buf_str(&b, "]");
        }
        snprintf(hd, sizeof hd, "},\"finish_reason\":\"%s\"}],\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":%d,\"total_tokens\":%d}}",
                 finish, ids.n, gen, ids.n + gen);
        buf_str(&b, hd);
        http_reply(s, 200, "application/json; charset=utf-8", b.p, b.n);
        free(b.p);
    }
    fprintf(stderr, "[serve] /v1 prompt %d tokens (%d reaproveitados, %.1f tok/s), resposta %d tokens, %.1f tok/s, %d chamada(s), fim %s%s\n",
            ids.n, common, (ids.n - common) / (t1 - t0 + 1e-9), gen, gen / (t2 - t1 + 1e-9), ncalls, finish, o.alive ? "" : " (cliente saiu)");
    for (int i = 0; i < ncalls; i++) free(calls[i].args);
    free(ids.ids); free(pend.p); free(forced.p); free(o.hold.p); free(o.content.p); free(o.reason.p); free(o.ev.p); free(o.calls.p);
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
