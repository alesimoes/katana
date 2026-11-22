/* ---------- modo professor: um modelo maior responde no lugar do modelo local, dentro do mesmo agente ----------
 * Com o professor ligado (/teacher on, ou KATANA_TEACHER=1), cada fala do agente vem da API de mensagens da
 * Anthropic (Claude) em vez de ser gerada aqui. Todo o resto e o agente de sempre: as ferramentas rodam nesta
 * maquina pelo mesmo despacho, a conversa aparece na interface do mesmo jeito e a sessao e gravada no mesmo arquivo.
 *
 * A resposta do professor e reescrita na sintaxe do modelo local (bloco de raciocinio, texto, chamadas de
 * ferramenta no formato do aluno) e entra no historico e no cache KV como se o aluno a tivesse escrito. Por isso:
 *   - a sessao gravada (.ksess, bloco KSX1) ja e dado de treino no formato exato do aluno; as falas do professor
 *     ficam anotadas em <sessao>.teacher e o katana export --teacher treina so nelas;
 *   - o professor pode ser ligado no meio de uma conversa: ele recebe a conversa inteira (a lista de mensagens e
 *     refeita a cada pedido a partir do historico da sessao), inclusive as tentativas do modelo local;
 *   - desligado o professor, o modelo local continua do ponto em que a conversa esta.
 *
 * Duas formas de falar com o professor (KATANA_TEACHER_VIA=api|cli, ou /teacher api, /teacher cli):
 *   - api: a API de mensagens, com chave;
 *   - cli: o Claude Code instalado nesta maquina (claude -p), com a sessao em que o dono ja esta logado, sem chave.
 *     Cada fala e uma chamada sem ferramentas proprias: ele recebe o prompt, as declaracoes das ferramentas do katana
 *     e a conversa, e devolve a proxima fala num objeto com esquema fixo (texto e chamadas). Sem chave gravada, e
 *     esta a forma usada.
 *
 * Rede: o pedido sai pelo curl do sistema. A chave vem de ANTHROPIC_API_KEY ou do arquivo ~/.katana/teacher.key
 * (so do dono) e vai num arquivo de cabecalho temporario com permissao so do dono, nunca na linha de comando.
 * O que sai da maquina: o prompt de sistema do agente (com a memoria salva e o mapa de pastas), a conversa e os
 * resultados das ferramentas. Variaveis: KATANA_TEACHER_MODEL (padrao claude-opus-5-5), KATANA_TEACHER_EFFORT
 * (low, medium, high, xhigh, max; padrao high), KATANA_TEACHER_URL (padrao https://api.anthropic.com),
 * KATANA_TEACHER_FALLBACK=0 (sem o modelo substituto quando o pedido e recusado). */

static int g_teacher;                                   /* 1: as falas do agente vem do professor */
static int g_tch_cli;                                   /* 1: pelo Claude Code desta maquina (claude -p); 0: pela API com chave */
static int g_tch_via;                                   /* escolha pedida: 0 automatica, 1 api, 2 cli */
static char g_tch_model[128] = "claude-opus-5-5", g_tch_url[512] = "https://api.anthropic.com", g_tch_effort[16] = "high";
static long g_tch_in, g_tch_out, g_tch_cread;           /* tokens somados nesta execucao */
/* conteudo cru das respostas desta execucao (com os blocos de raciocinio assinados): volta igual nos pedidos seguintes */
typedef struct { int idx; char *txt; char *raw; } TchRaw;
static TchRaw *g_tch_raw; static int g_tch_nraw;

static void js_escape(Buf *b, const char *s, size_t n);
static void im_call(const Config *c, Buf *out, const char *name, const char *args);
static void im_think(const Config *c, Buf *out, const char *reason);

static void tch_jstr(Buf *o, const char *s, size_t n) { buf_str(o, "\""); js_escape(o, s, n); buf_str(o, "\""); }

static int tch_key(char *out, size_t cap) {
    const char *e = getenv("ANTHROPIC_API_KEY");
    if (e && *e) { snprintf(out, cap, "%s", e); return 1; }
    char h[1024], p[1200];
    agent_home(h, sizeof h); snprintf(p, sizeof p, "%s/.katana/teacher.key", h);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    size_t n = fread(out, 1, cap - 1, f); fclose(f); out[n] = 0;
    while (n && (unsigned char)out[n - 1] <= ' ') out[--n] = 0;
    return n > 0;
}

static const char *tch_cli_prog(void) { const char *e = getenv("KATANA_TEACHER_CLI"); return e && *e ? e : "claude"; }
static int tch_cli_ok(void) {
#ifdef _WIN32
    return 0;
#else
    char q[1200], cmd[1300]; int rc = 1;
    shq(tch_cli_prog(), q, sizeof q); snprintf(cmd, sizeof cmd, "command -v %s", q);
    char *o = run_collect(cmd, 10, 512, &rc);
    free(o);
    return rc == 0;
#endif
}
static void tch_ui(void) { if (getenv("KATANA_UI")) { printf("\x01" "TEACHER %d %s %s\n", g_teacher, g_tch_model, g_tch_cli ? "(Claude Code)" : "(API)"); fflush(stdout); } }
static void tch_set(int on) {
    char key[600];
    if (on) {
        const int has = tch_key(key, sizeof key);
        memset(key, 0, sizeof key);
        g_tch_cli = g_tch_via == 2 || (g_tch_via == 0 && !has);
        if (g_tch_cli && !tch_cli_ok()) {
            fprintf(stderr, "%s[teacher: no API key (ANTHROPIC_API_KEY or ~/.katana/teacher.key) and the %s program was not found]%s\n", CE(C_DIM), tch_cli_prog(), CE(C_RESET));
            if (getenv("KATANA_UI")) { printf("\x01" "TEACHERR 0 no API key and no Claude Code on this machine\n"); fflush(stdout); }
            on = 0;
        } else if (!g_tch_cli && !has) { fprintf(stderr, "%s[teacher: no API key. Set ANTHROPIC_API_KEY or write the key to ~/.katana/teacher.key]%s\n", CE(C_DIM), CE(C_RESET)); on = 0; }
    }
    g_teacher = on;
    if (on && g_tch_cli) fprintf(stderr, "%s[teacher on: %s answers in place of the local model, through the Claude Code session of this machine (%s -p); the conversation and the tool results are sent out]%s\n", CE(C_DIM), g_tch_model, tch_cli_prog(), CE(C_RESET));
    else if (on) fprintf(stderr, "%s[teacher on: %s answers in place of the local model; the conversation and the tool results are sent to %s]%s\n", CE(C_DIM), g_tch_model, g_tch_url, CE(C_RESET));
    else fprintf(stderr, "%s[teacher off: the local model answers]%s\n", CE(C_DIM), CE(C_RESET));
    tch_ui();
}
static void tch_init(void) {
    const char *e;
    if ((e = getenv("KATANA_TEACHER_MODEL")) && *e) snprintf(g_tch_model, sizeof g_tch_model, "%s", e);
    if ((e = getenv("KATANA_TEACHER_URL")) && *e) snprintf(g_tch_url, sizeof g_tch_url, "%s", e);
    if ((e = getenv("KATANA_TEACHER_EFFORT")) && *e) snprintf(g_tch_effort, sizeof g_tch_effort, "%s", e);
    size_t ul = strlen(g_tch_url);
    while (ul && g_tch_url[ul - 1] == '/') g_tch_url[--ul] = 0;
    if ((e = getenv("KATANA_TEACHER_VIA")) && *e) g_tch_via = !strcmp(e, "cli") ? 2 : !strcmp(e, "api") ? 1 : 0;
    if ((e = getenv("KATANA_TEACHER")) && *e && strcmp(e, "0") != 0) tch_set(1);
}
/* comando /teacher [on|off|modelo]; 1 = era o comando */
static int tch_cmd(const char *line) {
    if (strncmp(line, "/teacher", 8) != 0 || (line[8] && line[8] != ' ')) return 0;
    const char *a = line + 8;
    while (*a == ' ') a++;
    if (!*a) tch_set(!g_teacher);
    else if (!strcmp(a, "on")) tch_set(1);
    else if (!strcmp(a, "off")) tch_set(0);
    else if (!strcmp(a, "cli") || !strcmp(a, "api")) { g_tch_via = a[0] == 'c' ? 2 : 1; tch_set(1); }
    else if (!strcmp(a, "status")) { fprintf(stderr, "[teacher %s: %s, %ld + %ld tokens in this run (%ld read from the cache)]\n", g_teacher ? "on" : "off", g_tch_model, g_tch_in, g_tch_out, g_tch_cread); tch_ui(); }
    else { snprintf(g_tch_model, sizeof g_tch_model, "%s", a); tch_set(1); }
    return 1;
}

/* ferramentas que o prompt do agente so cita numa linha (sem declaracao): o professor precisa da declaracao para
   chamar. delegate fica de fora: e um subagente do modelo local, e o professor tem contexto de sobra. */
static const char *const TCH_EXTRA[][2] = {
    {"todo", "{\"name\": \"todo\", \"description\": \"Your own task list for a long job: one step per line; call again with [x] in front of each finished step\", \"input_schema\": {\"type\": \"object\", \"properties\": {\"steps\": {\"type\": \"string\"}}, \"required\": [\"steps\"]}}"},
    {"git", "{\"name\": \"git\", \"description\": \"Run git with these arguments (no shell)\", \"input_schema\": {\"type\": \"object\", \"properties\": {\"args\": {\"type\": \"string\"}}, \"required\": [\"args\"]}}"},
    {"terminal", "{\"name\": \"terminal\", \"description\": \"A shell session that keeps its state. action: open, send, read, list, close, interrupt\", \"input_schema\": {\"type\": \"object\", \"properties\": {\"action\": {\"type\": \"string\"}, \"name\": {\"type\": \"string\"}, \"text\": {\"type\": \"string\"}, \"enter\": {\"type\": \"boolean\"}, \"lines\": {\"type\": \"integer\"}, \"wait_ms\": {\"type\": \"integer\"}}, \"required\": [\"action\"]}}"},
    {"glob", "{\"name\": \"glob\", \"description\": \"List files matching a pattern under a folder\", \"input_schema\": {\"type\": \"object\", \"properties\": {\"pattern\": {\"type\": \"string\"}, \"root\": {\"type\": \"string\"}}, \"required\": [\"pattern\"]}}"},
    {"recall", "{\"name\": \"recall\", \"description\": \"Search the facts saved about the user\", \"input_schema\": {\"type\": \"object\", \"properties\": {\"query\": {\"type\": \"string\"}}, \"required\": [\"query\"]}}"},
    {"download_file", "{\"name\": \"download_file\", \"description\": \"Download a URL to a file\", \"input_schema\": {\"type\": \"object\", \"properties\": {\"url\": {\"type\": \"string\"}, \"path\": {\"type\": \"string\"}}, \"required\": [\"url\", \"path\"]}}"},
    {"install_package", "{\"name\": \"install_package\", \"description\": \"Install a package. manager: pip, npm, apt or cargo\", \"input_schema\": {\"type\": \"object\", \"properties\": {\"manager\": {\"type\": \"string\"}, \"name\": {\"type\": \"string\"}}, \"required\": [\"manager\", \"name\"]}}"},
    {"read_log", "{\"name\": \"read_log\", \"description\": \"Read a log. source: agent, commands, journal, dmesg, syslog or a file path\", \"input_schema\": {\"type\": \"object\", \"properties\": {\"source\": {\"type\": \"string\"}, \"lines\": {\"type\": \"integer\"}, \"filter\": {\"type\": \"string\"}, \"unit\": {\"type\": \"string\"}}, \"required\": [\"source\"]}}"},
    {"job", "{\"name\": \"job\", \"description\": \"Background commands. action: read, list or stop\", \"input_schema\": {\"type\": \"object\", \"properties\": {\"id\": {\"type\": \"integer\"}, \"action\": {\"type\": \"string\"}}}}"},
    {"howto", "{\"name\": \"howto\", \"description\": \"Recipes for using the tools\", \"input_schema\": {\"type\": \"object\", \"properties\": {\"topic\": {\"type\": \"string\"}}, \"required\": [\"topic\"]}}"},
};

/* prompt do agente no formato com as declaracoes em JSON (o do Gemma 4 declara de outro jeito: monta o generico) */
static const char *tch_base(const char *sys, int g4) {
    if (!g4) return sys;
    static char alt[32000]; static unsigned long seen;
    unsigned long h = 5381;
    for (const char *p = sys; *p; p++) h = h * 33 + (unsigned char)*p;
    if (h != seen || !alt[0]) {
        const int x = g_tool_xml, g = g_tool_glm;
        g_tool_xml = 0; g_tool_glm = 0;
        agent_build_sys(alt, sizeof alt, ""); mcp_sys(alt, sizeof alt);
        g_tool_xml = x; g_tool_glm = g; seen = h;
    }
    return alt;
}

/* declaracoes do prompt ({"type": "function", "function": {name, description, parameters}} por linha) -> tools da API */
static void tch_tools(const char *sys, Buf *o) {
    const char *a = strstr(sys, "<tools>\n"), *b = a ? strstr(a, "\n</tools>") : NULL;
    int n = 0;
    buf_str(o, "[");
    if (a && b) for (const char *p = a + 8; p < b; ) {
        const char *e = memchr(p, '\n', (size_t)(b - p));
        if (!e) e = b;
        const char *q = js_ws(p), *nv = NULL, *dv = NULL, *pv = NULL;
        char k[64]; const char *v, *me;
        if (*q == '{') for (const char *m = q + 1; (me = gs_member(m, k, sizeof k, &v)); m = me) {
            if (strcmp(k, "function") || *v != '{') continue;
            char k2[64]; const char *v2, *e2;
            for (const char *m2 = v + 1; (e2 = gs_member(m2, k2, sizeof k2, &v2)); m2 = e2) {
                if (!strcmp(k2, "name")) nv = v2; else if (!strcmp(k2, "description")) dv = v2; else if (!strcmp(k2, "parameters")) pv = v2;
            }
        }
        const char *ne = nv ? gs_skipv(nv) : NULL, *de = dv ? gs_skipv(dv) : NULL, *pe = pv ? gs_skipv(pv) : NULL;
        if (ne && *nv == '"' && strncmp(nv, "\"delegate\"", 10) != 0) {
            buf_str(o, n++ ? ", {\"name\": " : "{\"name\": "); buf_add(o, nv, (size_t)(ne - nv));
            if (de && *dv == '"') { buf_str(o, ", \"description\": "); buf_add(o, dv, (size_t)(de - dv)); }
            buf_str(o, ", \"input_schema\": ");
            if (pe && *pv == '{') buf_add(o, pv, (size_t)(pe - pv)); else buf_str(o, "{\"type\": \"object\", \"properties\": {}}");
            buf_str(o, "}");
        }
        p = e + 1;
    }
    for (size_t i = 0; i < sizeof TCH_EXTRA / sizeof TCH_EXTRA[0]; i++) {
        char pat[160]; snprintf(pat, sizeof pat, "\"name\": \"%s\"", TCH_EXTRA[i][0]);
        if (a && b && strstr(a, pat) && strstr(a, pat) < b) continue;          /* ja declarada no prompt */
        if (!launch_tool_allowed(TCH_EXTRA[i][0])) continue;                   /* --allowed-tools: o professor tambem so ve as permitidas */
        buf_str(o, n++ ? ", " : ""); buf_str(o, TCH_EXTRA[i][1]);
    }
    /* ferramentas dos servidores MCP ligados: o prompt as mostra em cartoes, fora do bloco <tools>, entao entram aqui */
    for (int i = 0; i < g_nmcp; i++) {
        if (!g_mcp[i].alive) continue;
        for (int j = 0; j < g_mcp[i].ntools; j++) {
            const McpTool *t = &g_mcp[i].tools[j];
            char fn[260]; mcp_fname(&g_mcp[i], t->name, fn, sizeof fn);
            if (!launch_tool_allowed(fn)) continue;
            buf_str(o, n++ ? ", {\"name\": " : "{\"name\": "); tch_jstr(o, fn, strlen(fn));
            if (t->desc && t->desc[0]) { buf_str(o, ", \"description\": "); tch_jstr(o, t->desc, strlen(t->desc)); }
            buf_str(o, ", \"input_schema\": ");
            buf_str(o, t->schema && t->schema[0] == '{' ? t->schema : "{\"type\": \"object\", \"properties\": {}}");
            buf_str(o, "}");
        }
    }
    buf_str(o, "]");
}

/* prompt de sistema do professor: o do agente sem o bloco que ensina a sintaxe de chamada do aluno */
static void tch_sys(const char *sys, Buf *o) {
    const char *t0 = strstr(sys, "# Tools\n"), *t1 = t0 ? strstr(t0, "\n# Environment") : NULL;
    if (!t0 || !t1) { buf_str(o, sys); return; }
    buf_add(o, sys, (size_t)(t0 - sys));
    buf_str(o, "# Tools\n\nCall the functions you are given to act. The conversation may contain earlier turns written by a smaller local model: check its claims instead of trusting them.\n");
    { McpB cb = {0}; mcp_bput(&cb, "", 0); mcpch_sys_text(&cb, 20000); if (cb.n) buf_add(o, cb.p, cb.n); free(cb.p); }      /* o bloco cortado levou as instrucoes dos canais */
    buf_str(o, t1);
}

/* fala do modelo no formato do aluno -> blocos da API (texto e tool_use); o raciocinio fica de fora.
   ids/nid: identificadores das chamadas, na ordem. Devolve o numero de blocos */
static int tch_asst_blocks(const Config *c, const char *s, int midx, Buf *o, char ids[][40], int *nid) {
    const int g4 = c->chat_fmt == 8, glm = c->chat_fmt == 10 && c->glm_args;
    const char *th = strstr(s, g4 ? "<channel|>" : "</think>");
    const char *body = th ? th + (g4 ? 10 : 8) : s;
    const int g4_native = g4 && strstr(body, "<|tool_call>") != NULL;
    const char *open_t = g4_native ? "<|tool_call>" : "<tool_call>", *close_t = g4_native ? "<tool_call|>" : "</tool_call>";
    const char *fc = strstr(body, open_t);
    const char *te = fc ? fc : body + strlen(body);
    int nb = 0;
    while (body < te && (unsigned char)*body <= ' ') body++;
    while (te > body && (unsigned char)te[-1] <= ' ') te--;
    if (te > body) { buf_str(o, "{\"type\": \"text\", \"text\": "); tch_jstr(o, body, (size_t)(te - body)); buf_str(o, "}"); nb++; }
    static char xjs[98304]; char name[128];
    for (const char *tc = fc; tc && *nid < 64; tc = strstr(tc + 1, open_t)) {
        const char *end = strstr(tc, close_t);
        if (!end) end = tc + strlen(tc);
        const char *ap = NULL;
        if (g4_native) { if (!g4_call_parse(tc, end, name, sizeof name, xjs, sizeof xjs)) continue; ap = xjs; }
        else if (glm && glm_call_parse(tc, end, name, sizeof name, xjs, sizeof xjs)) ap = xjs;
        else if (tool_xml_parse(tc, end, name, sizeof name, xjs, sizeof xjs)) ap = xjs;
        else {
            if (!json_str(tc, end, "name", name, sizeof name)) continue;
            const size_t l = (size_t)(end - tc) < sizeof xjs - 1 ? (size_t)(end - tc) : sizeof xjs - 1;
            memcpy(xjs, tc, l); xjs[l] = 0; ap = xjs;
        }
        const char *ar = strstr(ap, "\"arguments\""), *ob = ar ? strchr(ar + 11, '{') : NULL, *oe = ob ? gs_skipv(ob) : NULL;
        snprintf(ids[*nid], 40, "kt_%d_%d", midx, *nid);
        buf_str(o, nb++ ? ", " : ""); buf_str(o, "{\"type\": \"tool_use\", \"id\": \""); buf_str(o, ids[*nid]); buf_str(o, "\", \"name\": ");
        tch_jstr(o, name, strlen(name)); buf_str(o, ", \"input\": ");
        if (oe) buf_add(o, ob, (size_t)(oe - ob)); else buf_str(o, "{}");
        buf_str(o, "}");
        (*nid)++;
    }
    return nb;
}

/* resposta guardada desta execucao para a mensagem idx (mesmo texto): blocos crus e os ids das chamadas */
static int tch_raw_blocks(int idx, const char *txt, Buf *o, char ids[][40], int *nid) {
    for (int i = g_tch_nraw - 1; i >= 0; i--) {
        if (g_tch_raw[i].idx != idx || strcmp(g_tch_raw[i].txt, txt) != 0) continue;
        const char *r = js_ws(g_tch_raw[i].raw), *el, *ee;
        int nb = 0;
        if (*r != '[') return 0;
        for (const char *q = r + 1; (ee = gs_elem(q, &el)); q = ee) {
            const char *le = gs_skipv(el);
            if (*el != '{' || !le) continue;
            buf_str(o, nb++ ? ", " : ""); buf_add(o, el, (size_t)(le - el));
            char k[64]; const char *v, *me; int tu = 0; char id[40] = "";
            for (const char *m = el + 1; (me = gs_member(m, k, sizeof k, &v)); m = me) {
                if (!strcmp(k, "type")) tu = !strncmp(v, "\"tool_use\"", 10);
                else if (!strcmp(k, "id") && *v == '"') { size_t n = 0; for (const char *w = v + 1; *w && *w != '"' && n < 39; w++) id[n++] = *w; id[n] = 0; }
            }
            if (tu && id[0] && *nid < 64) snprintf(ids[(*nid)++], 40, "%s", id);
        }
        return nb;
    }
    return 0;
}

/* mensagens da API a partir da conversa inteira da sessao. Regras da API garantidas aqui: papeis alternados (falas
   seguidas do mesmo papel viram uma), todo tool_use com o seu tool_result logo depois, nenhuma fala vazia */
static void tch_messages(const Config *c, const AHist *h, Buf *o) {
    const int g4 = c->chat_fmt == 8;
    const char *ro = g4 ? "<|tool_response>" : "<tool_response>\n", *rc = g4 ? "<tool_response|>" : "\n</tool_response>";
    static char ids[64][40];
    int nid = 0, n = 0; char last = 0;
#define TCH_OPEN(role) do { if (last != (role)) { if (last) buf_str(o, "]}, "); buf_str(o, (role) == 'a' ? "{\"role\": \"assistant\", \"content\": [" : "{\"role\": \"user\", \"content\": ["); last = (role); n = 0; } } while (0)
#define TCH_RESULT(id, s, l) do { buf_str(o, n++ ? ", " : ""); buf_str(o, "{\"type\": \"tool_result\", \"tool_use_id\": \""); buf_str(o, id); buf_str(o, "\", \"content\": "); tch_jstr(o, s, l); buf_str(o, "}"); } while (0)
#define TCH_TEXT(s, l) do { buf_str(o, n++ ? ", " : ""); buf_str(o, "{\"type\": \"text\", \"text\": "); tch_jstr(o, s, l); buf_str(o, "}"); } while (0)
    for (int i = 0; i < h->n; i++) {
        const char r = h->v[i].role, *s = h->v[i].s;
        if (r == 'a') {
            Buf b = {0}; static char nids[64][40]; int nn = 0;
            int nb = tch_raw_blocks(i, s, &b, nids, &nn);
            if (!nb) nb = tch_asst_blocks(c, s, i, &b, nids, &nn);
            if (!nb) { free(b.p); continue; }                    /* fala vazia (interrompida): nao vai */
            if (last == 'a' || !last) {                          /* duas falas seguidas do modelo, ou a conversa comeca por ele */
                TCH_OPEN('u');
                for (int j = 0; j < nid; j++) TCH_RESULT(ids[j], "(no result recorded)", 20);
                if (!nid) TCH_TEXT("(continue)", 10);
                nid = 0;
            }
            TCH_OPEN('a');
            buf_add(o, b.p, b.n); n = nb; free(b.p);
            nid = nn; memcpy(ids, nids, sizeof ids);
        } else if (r == 't') {
            TCH_OPEN('u');
            int j = 0;
            for (const char *p = strstr(s, ro); p; p = strstr(p, ro)) {
                p += strlen(ro);
                const char *e = strstr(p, rc);
                if (!e) e = p + strlen(p);
                if (j < nid) TCH_RESULT(ids[j], p, (size_t)(e - p)); else TCH_TEXT(p, (size_t)(e - p));
                j++; p = e;
            }
            for (; j < nid; j++) TCH_RESULT(ids[j], "(no result recorded)", 20);
            if (!n) TCH_TEXT(*s ? s : "(empty)", *s ? strlen(s) : 7);
            nid = 0;
        } else {
            TCH_OPEN('u');
            for (int j = 0; j < nid; j++) TCH_RESULT(ids[j], "(not run: the user interrupted)", 31);
            nid = 0;
            TCH_TEXT(*s ? s : "(empty)", *s ? strlen(s) : 7);
        }
    }
    if (last == 'a' || !last) { TCH_OPEN('u'); for (int j = 0; j < nid; j++) TCH_RESULT(ids[j], "(no result recorded)", 20); if (!nid) TCH_TEXT("(continue)", 10); }
    buf_str(o, "]}");
#undef TCH_OPEN
#undef TCH_RESULT
#undef TCH_TEXT
}

/* imprime a fala como o gerador local imprime: raciocinio separado, depois o resto */
static void tch_print(const char *reason, const char *rest) {
    if (reason && *reason) { fprintf(stdout, "%s[raciocinio]\n", CE(C_DIM)); fputs(reason, stdout); fprintf(stdout, "%s\n", CE(C_RESET)); }
    while (*rest == '\n') rest++;
    con_puts(rest);                      /* console rico: Markdown e sem o texto das chamadas de ferramenta */
    fflush(stdout);
}

/* anota que a mensagem idx da sessao veio do professor (katana export --teacher treina so nessas) */
static void tch_mark(const char *sess, int idx, size_t len) {
    if (!sess || !sess[0]) return;
    char p[4400]; snprintf(p, sizeof p, "%s.teacher", sess);
    FILE *f = fopen(p, "ab");
    if (!f) return;
    fprintf(f, "%d\t%zu\t%s\n", idx, len, g_tch_model);
    fclose(f);
}

#ifndef _WIN32
/* ---- texto do professor aparecendo enquanto ele escreve (caminho cli) ----
 * O programa sai em linhas JSON (stream-json). O objeto da resposta ({"text": ..., "tool_calls": ...}) chega em
 * pedacos (input_json_delta / partial_json); aqui eles sao juntados e o valor de "text" e impresso conforme chega,
 * na mesma fala. A ultima linha (type result) traz o objeto inteiro, que e o que vale. */
static struct { Buf line, pj, res; size_t at; int state; size_t shown; double mark; int midline; } g_ts;     /* state: 0 procurando "text", 1 dentro do texto, 2 texto encerrado */
static void ts_reset(void) { buf_clear(&g_ts.line); buf_clear(&g_ts.pj); buf_clear(&g_ts.res); g_ts.at = 0; g_ts.state = 0; g_ts.shown = 0; g_ts.mark = now_sec(); g_ts.midline = 0; }
static void ts_utf8(unsigned cp) {
    char u[5]; int n = 0;
    if (cp < 0x80) u[n++] = (char)cp;
    else if (cp < 0x800) { u[n++] = (char)(0xC0 | (cp >> 6)); u[n++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { u[n++] = (char)(0xE0 | (cp >> 12)); u[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[n++] = (char)(0x80 | (cp & 0x3F)); }
    else { u[n++] = (char)(0xF0 | (cp >> 18)); u[n++] = (char)(0x80 | ((cp >> 12) & 0x3F)); u[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[n++] = (char)(0x80 | (cp & 0x3F)); }
    fwrite(u, 1, (size_t)n, stdout); g_ts.shown += (size_t)n;
}
static int ts_hex4(const char *p, unsigned *v) {
    *v = 0;
    for (int i = 0; i < 4; i++) { const char ch = p[i]; unsigned d; if (ch >= '0' && ch <= '9') d = (unsigned)(ch - '0'); else if (ch >= 'a' && ch <= 'f') d = (unsigned)(ch - 'a' + 10); else if (ch >= 'A' && ch <= 'F') d = (unsigned)(ch - 'A' + 10); else return 0; *v = *v * 16 + d; }
    return 1;
}
/* imprime o que ja da para imprimir do valor de "text" no JSON parcial juntado em g_ts.pj */
static void ts_emit(void) {
    const char *s = g_ts.pj.p; const size_t n = g_ts.pj.n;
    if (!s) return;
    if (g_ts.state == 0) {
        const char *k = strstr(s, "\"text\"");
        if (!k) return;
        const char *q = k + 6;
        while (*q == ' ' || *q == ':' || *q == '\n' || *q == '\t') q++;
        if (!*q) return;
        if (*q != '"') { g_ts.state = 2; return; }
        g_ts.at = (size_t)(q + 1 - s); g_ts.state = 1;
    }
    if (g_ts.state != 1) return;
    size_t i = g_ts.at;
    while (i < n) {
        const unsigned char ch = (unsigned char)s[i];
        if (ch == '"') { g_ts.state = 2; i++; break; }
        if (ch != '\\') {
            if (ch >= 0x80) {       /* caractere UTF-8 inteiro ou espera o resto */
                const size_t l = ch >= 0xF0 ? 4 : ch >= 0xE0 ? 3 : ch >= 0xC0 ? 2 : 1;
                if (i + l > n) break;
                fwrite(s + i, 1, l, stdout); g_ts.shown += l; i += l;
            } else { fputc(ch, stdout); g_ts.shown++; i++; }
            continue;
        }
        if (i + 1 >= n) break;
        const char e = s[i + 1];
        if (e == 'u') {
            unsigned cp, lo;
            if (i + 6 > n) break;
            if (!ts_hex4(s + i + 2, &cp)) { i += 2; continue; }
            if (cp >= 0xD800 && cp < 0xDC00) {         /* par substituto: precisa da segunda metade */
                if (i + 12 > n) break;
                if (s[i + 6] == '\\' && s[i + 7] == 'u' && ts_hex4(s + i + 8, &lo)) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 12; }
                else { cp = 0xFFFD; i += 6; }
            } else i += 6;
            ts_utf8(cp);
            continue;
        }
        const char o = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? 0 : e == 'b' || e == 'f' ? ' ' : e;
        if (o) { fputc(o, stdout); g_ts.shown++; }
        i += 2;
    }
    if (i > g_ts.at) g_ts.midline = s[i - 1] != '\n' && !(i >= 2 && s[i - 2] == '\\' && s[i - 1] == 'n');
    g_ts.at = i;
    fflush(stdout);
}
/* depois do texto o professor escreve as chamadas (um arquivo inteiro pode levar minutos): a interface mostra o
   quanto ja chegou, uma vez por segundo, em vez de parecer parada */
static void ts_progress(void) {
    if (g_ts.state != 2 && !(g_ts.state == 0 && g_ts.pj.n > 64)) return;
    const double t = now_sec();
    if (t - g_ts.mark < 1.0 || !getenv("KATANA_UI")) return;
    g_ts.mark = t;
    if (g_ts.midline) { fputc('\n', stdout); g_ts.midline = 0; }      /* o marcador precisa comecar a linha */
    printf("\x01" "TEACHW %zu\n", g_ts.pj.n); fflush(stdout);
}
static void ts_line(const char *l) {
    if (strstr(l, "\"type\":\"result\"") || strstr(l, "\"type\": \"result\"")) { buf_clear(&g_ts.res); buf_str(&g_ts.res, l); return; }
    if (!strstr(l, "input_json_delta")) return;
    const char *k = strstr(l, "\"partial_json\"");
    if (!k) return;
    k += 14;
    while (*k == ' ' || *k == ':') k++;
    if (*k != '"') return;
    Buf t = {0}; const char *pp = k;
    if (js_string(&pp, &t) && t.p) { buf_add(&g_ts.pj, t.p, t.n); ts_emit(); ts_progress(); }
    free(t.p);
}
static void ts_tap(const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (b[i] != '\n') { buf_add(&g_ts.line, b + i, 1); continue; }
        if (g_ts.line.n) ts_line(g_ts.line.p);
        buf_clear(&g_ts.line);
    }
}

/* ---- texto do professor aparecendo enquanto ele escreve (caminho api) ----
 * O pedido vai com stream: true e a resposta chega em eventos (linhas "data: {...}"). O raciocinio resumido e o texto
 * sao impressos conforme chegam; os blocos sao remontados (texto, raciocinio com a assinatura, chamadas) num objeto
 * igual ao da resposta sem stream, para o mesmo leitor e para voltarem iguais nos pedidos seguintes. Resposta que
 * nao e stream (erro HTTP) fica em other; a ultima linha, escrita pelo curl, traz o codigo HTTP. */
#define AS_BLOCKS 64
typedef struct { char type[24]; Buf start, acc, sig; } AsBlock;
static struct { Buf line, other, err; AsBlock b[AS_BLOCKS]; int nb, http, events; char stop[40]; long tin, tout, tread, twrite;
                int think_open, think_closed, text_blocks; size_t shown; double mark; int midline; } g_as;
static void as_reset(void) {
    buf_clear(&g_as.line); buf_clear(&g_as.other); buf_clear(&g_as.err);
    for (int i = 0; i < AS_BLOCKS; i++) { g_as.b[i].type[0] = 0; buf_clear(&g_as.b[i].start); buf_clear(&g_as.b[i].acc); buf_clear(&g_as.b[i].sig); }
    g_as.nb = 0; g_as.http = 0; g_as.events = 0; g_as.stop[0] = 0; g_as.tin = g_as.tout = g_as.tread = g_as.twrite = 0;
    g_as.think_open = g_as.think_closed = g_as.text_blocks = 0; g_as.shown = 0; g_as.mark = now_sec(); g_as.midline = 0;
}
static void as_word(const char *v, char *out, size_t cap) { size_t n = 0; if (*v == '"') for (const char *w = v + 1; *w && *w != '"' && n < cap - 1; w++) out[n++] = *w; out[n] = 0; }
static void as_show(const char *s, size_t n) { if (!n) return; fwrite(s, 1, n, stdout); fflush(stdout); g_as.shown += n; g_as.midline = s[n - 1] != '\n'; }
static void as_event(const char *js) {
    char k[64], type[40] = "", dtype[40] = ""; const char *v, *e, *cb = NULL, *delta = NULL, *msg = NULL, *usage = NULL, *error = NULL;
    int index = -1;
    const char *q = js_ws(js);
    if (*q != '{') return;
    for (const char *p = q + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) {
        if (!strcmp(k, "type")) as_word(v, type, sizeof type); else if (!strcmp(k, "index")) index = atoi(v);
        else if (!strcmp(k, "content_block") && *v == '{') cb = v; else if (!strcmp(k, "delta") && *v == '{') delta = v;
        else if (!strcmp(k, "message") && *v == '{') msg = v; else if (!strcmp(k, "usage") && *v == '{') usage = v;
        else if (!strcmp(k, "error") && *v == '{') error = v;
    }
    g_as.events++;
    if (msg) for (const char *p = msg + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) if (!strcmp(k, "usage") && *v == '{') usage = v;
    if (usage) for (const char *p = usage + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) {
        if (!strcmp(k, "input_tokens")) g_as.tin = atol(v); else if (!strcmp(k, "output_tokens")) g_as.tout = atol(v);
        else if (!strcmp(k, "cache_read_input_tokens")) g_as.tread = atol(v); else if (!strcmp(k, "cache_creation_input_tokens")) g_as.twrite = atol(v);
    }
    if (error) for (const char *p = error + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) if (!strcmp(k, "message") && *v == '"') { const char *pp = v; buf_clear(&g_as.err); js_string(&pp, &g_as.err); }
    if (!strcmp(type, "content_block_start") && cb && index >= 0 && index < AS_BLOCKS) {
        AsBlock *b = &g_as.b[index];
        const char *ce = gs_skipv(cb);
        if (ce) buf_add(&b->start, cb, (size_t)(ce - cb));
        for (const char *p = cb + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) if (!strcmp(k, "type")) as_word(v, b->type, sizeof b->type);
        if (index + 1 > g_as.nb) g_as.nb = index + 1;
        if (!strcmp(b->type, "text")) g_as.text_blocks++;
    } else if (!strcmp(type, "content_block_delta") && delta && index >= 0 && index < AS_BLOCKS) {
        AsBlock *b = &g_as.b[index];
        const char *tv = NULL, *thv = NULL, *sv = NULL, *pv = NULL;
        for (const char *p = delta + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) {
            if (!strcmp(k, "type")) as_word(v, dtype, sizeof dtype);
            else if (!strcmp(k, "text") && *v == '"') tv = v; else if (!strcmp(k, "thinking") && *v == '"') thv = v;
            else if (!strcmp(k, "signature") && *v == '"') sv = v; else if (!strcmp(k, "partial_json") && *v == '"') pv = v;
        }
        Buf t = {0};
        if (thv) {
            const char *pp = thv; js_string(&pp, &t);
            if (t.p && t.n) {
                if (!g_as.think_open) { fprintf(stdout, "%s[raciocinio]\n", CE(C_DIM)); g_as.think_open = 1; g_as.shown++; }
                buf_add(&b->acc, t.p, t.n); as_show(t.p, t.n);
            }
        } else if (tv) {
            const char *pp = tv; js_string(&pp, &t);
            if (t.p && t.n) {
                if (g_as.think_open && !g_as.think_closed) { fprintf(stdout, "%s\n", CE(C_RESET)); g_as.think_closed = 1; g_as.midline = 0; }
                if (!b->acc.n && g_as.text_blocks > 1) as_show("\n\n", 2);      /* outro bloco de texto: como a fala final junta */
                buf_add(&b->acc, t.p, t.n); as_show(t.p, t.n);
            }
        } else if (sv) { const char *pp = sv; js_string(&pp, &t); if (t.p) buf_add(&b->sig, t.p, t.n); }
        else if (pv) {
            const char *pp = pv; js_string(&pp, &t);
            if (t.p) buf_add(&b->acc, t.p, t.n);
            const double now = now_sec();          /* escrevendo uma chamada (um arquivo inteiro leva minutos): mostra o quanto ja chegou */
            if (now - g_as.mark >= 1.0 && getenv("KATANA_UI")) {
                g_as.mark = now;
                if (g_as.think_open && !g_as.think_closed) { fprintf(stdout, "%s\n", CE(C_RESET)); g_as.think_closed = 1; g_as.midline = 0; }
                if (g_as.midline) { fputc('\n', stdout); g_as.midline = 0; }
                size_t tot = 0; for (int i = 0; i < g_as.nb; i++) if (!strcmp(g_as.b[i].type, "tool_use")) tot += g_as.b[i].acc.n;
                printf("\x01" "TEACHW %zu\n", tot); fflush(stdout);
            }
        }
        free(t.p);
    } else if (!strcmp(type, "message_delta") && delta) {
        for (const char *p = delta + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) if (!strcmp(k, "stop_reason")) as_word(v, g_as.stop, sizeof g_as.stop);
    }
}
static void as_line(const char *l) {
    if (!strncmp(l, "data:", 5)) { as_event(l + 5); return; }
    if (!strncmp(l, "KATANA_HTTP ", 12)) { g_as.http = atoi(l + 12); return; }
    if (!strncmp(l, "event:", 6) || l[0] == ':' || !l[0] || l[0] == '\r') return;
    buf_str(&g_as.other, l); buf_str(&g_as.other, "\n");
}
static void as_tap(const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (b[i] != '\n') { buf_add(&g_as.line, b + i, 1); continue; }
        if (g_as.line.n && g_as.line.p[g_as.line.n - 1] == '\r') g_as.line.p[--g_as.line.n] = 0;
        as_line(g_as.line.p ? g_as.line.p : "");
        buf_clear(&g_as.line);
    }
}
/* fim do stream: fecha o raciocinio na tela e escreve em r o objeto da resposta (ou o corpo de erro). Devolve o HTTP */
static int as_finish(Buf *r) {
    if (g_as.line.n) as_line(g_as.line.p);
    if (g_as.think_open && !g_as.think_closed) { fprintf(stdout, "%s\n", CE(C_RESET)); fflush(stdout); g_as.think_closed = 1; }
    buf_clear(r);
    if (g_as.http != 200 || !g_as.events || g_as.err.n) {
        if (g_as.err.n) { buf_str(r, "{\"error\": {\"message\": "); tch_jstr(r, g_as.err.p, g_as.err.n); buf_str(r, "}}"); return g_as.http == 200 ? 0 : g_as.http; }
        if (g_as.other.n) buf_add(r, g_as.other.p, g_as.other.n);
        return g_as.http == 200 ? 0 : g_as.http;
    }
    buf_str(r, "{\"content\": [");
    int n = 0;
    for (int i = 0; i < g_as.nb; i++) {
        AsBlock *b = &g_as.b[i];
        if (!b->type[0]) continue;
        buf_str(r, n++ ? ", " : "");
        if (!strcmp(b->type, "text")) { buf_str(r, "{\"type\": \"text\", \"text\": "); tch_jstr(r, b->acc.p ? b->acc.p : "", b->acc.n); buf_str(r, "}"); }
        else if (!strcmp(b->type, "thinking")) {
            buf_str(r, "{\"type\": \"thinking\", \"thinking\": "); tch_jstr(r, b->acc.p ? b->acc.p : "", b->acc.n);
            buf_str(r, ", \"signature\": "); tch_jstr(r, b->sig.p ? b->sig.p : "", b->sig.n); buf_str(r, "}");
        } else if (!strcmp(b->type, "tool_use")) {     /* os campos do inicio do bloco (id, name...) e a entrada juntada dos pedacos */
            char k[64]; const char *v, *e; int m = 0;
            buf_str(r, "{");
            if (b->start.p) for (const char *p = b->start.p + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) {
                if (!strcmp(k, "input")) continue;
                const char *ve = gs_skipv(v);
                if (!ve) continue;
                buf_str(r, m++ ? ", \"" : "\""); buf_str(r, k); buf_str(r, "\": "); buf_add(r, v, (size_t)(ve - v));
            }
            buf_str(r, m ? ", \"input\": " : "\"input\": ");
            const char *in = b->acc.p ? js_ws(b->acc.p) : "";
            if (*in == '{' && gs_skipv(in)) buf_add(r, b->acc.p, b->acc.n); else buf_str(r, "{}");
            buf_str(r, "}");
        } else if (b->start.p) buf_add(r, b->start.p, b->start.n);
        else n--;
    }
    char t[300];
    snprintf(t, sizeof t, "], \"stop_reason\": \"%s\", \"usage\": {\"input_tokens\": %ld, \"output_tokens\": %ld, \"cache_read_input_tokens\": %ld, \"cache_creation_input_tokens\": %ld}}",
             g_as.stop, g_as.tin, g_as.tout, g_as.tread, g_as.twrite);
    buf_str(r, t);
    return 200;
}

/* uma fala pelo Claude Code desta maquina. Escreve em r um objeto no formato de resposta da API (content com text e
   tool_use, usage), para o mesmo leitor servir aos dois caminhos. Devolve 200, ou 0 com a mensagem de erro em r */
static int tch_cli_call(const Config *c, const char *base, const AHist *slog, Buf *r) {
    char dir[] = "/tmp/katana_teacher_XXXXXX";
    if (!mkdtemp(dir)) return 0;
    char fs[80], fp[80], fo[80], fe[80], qs[200], qp[200], qo[200], qe[200], qd[200], qprog[1200], qm[300], qef[60];
    snprintf(fs, sizeof fs, "%s/s", dir); snprintf(fp, sizeof fp, "%s/p", dir); snprintf(fo, sizeof fo, "%s/o", dir); snprintf(fe, sizeof fe, "%s/e", dir);
    Buf sysb = {0}, pr = {0};
    tch_sys(base, &sysb);
    buf_str(&sysb, "\n\n# Functions you can call\n");
    tch_tools(base, &sysb);
    buf_str(&sysb, "\n\n# How to answer\nYou write the next turn of this agent; you do not run anything yourself here. Answer with the structured object: "
                   "\"text\" is what you say to the user (it may be empty when you only call functions) and \"tool_calls\" lists the functions to call now, "
                   "each with its name and an \"arguments\" object. The agent runs them on the user's machine and their results come in the next message. "
                   "When the job is done, or you need the user, answer with text and an empty tool_calls.");
    buf_str(&pr, "The conversation so far, as a JSON array of messages (tool_use blocks are the calls made, tool_result blocks what they returned):\n[");
    tch_messages(c, slog, &pr);
    buf_str(&pr, "]\n\nWrite the next assistant turn.");
    int ok = 0;
    FILE *f = fopen(fs, "wb");
    if (f) { ok = fwrite(sysb.p, 1, sysb.n, f) == sysb.n; fclose(f); }
    f = ok ? fopen(fp, "wb") : NULL;
    if (f) { ok = fwrite(pr.p, 1, pr.n, f) == pr.n; fclose(f); } else ok = 0;
    if (getenv("KATANA_TEACHER_DUMP")) { FILE *fd2 = fopen(getenv("KATANA_TEACHER_DUMP"), "wb"); if (fd2) { fwrite(pr.p, 1, pr.n, fd2); fclose(fd2); } }
    free(sysb.p); free(pr.p);
    static const char *schema = "'{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"},\"tool_calls\":{\"type\":\"array\",\"items\":{\"type\":\"object\","
                                "\"properties\":{\"name\":{\"type\":\"string\"},\"arguments\":{\"type\":\"object\"}},\"required\":[\"name\",\"arguments\"]}}},\"required\":[\"text\",\"tool_calls\"]}'";
    shq(fs, qs, sizeof qs); shq(fp, qp, sizeof qp); shq(fo, qo, sizeof qo); shq(fe, qe, sizeof qe); shq(dir, qd, sizeof qd);
    shq(tch_cli_prog(), qprog, sizeof qprog); shq(g_tch_model, qm, sizeof qm); shq(g_tch_effort, qef, sizeof qef);
    static char cmd[6000];
    /* sem ferramentas proprias, sem MCP, sem configuracoes nem sessao gravada: so o modelo, com a conta ja logada */
    /* saida em linhas (stream-json) pelo cano: o texto aparece enquanto chega (ts_tap); a ultima linha e o resultado */
    snprintf(cmd, sizeof cmd, "cd %s && env -u CLAUDECODE -u CLAUDE_CODE_ENTRYPOINT %s -p --system-prompt-file %s --tools '' --json-schema %s --output-format stream-json --verbose --include-partial-messages "
             "--model %s --effort %s --no-session-persistence --strict-mcp-config --setting-sources '' < %s 2> %s", qd, qprog, qs, schema, qm, qef, qp, qe);
    (void)qo;
    int rc = 1;
    ts_reset();
    if (ok) { g_cmd_tap = ts_tap; char *o = run_collect(cmd, 910, 2048, &rc); g_cmd_tap = NULL; free(o); }
    if (g_ts.line.n) ts_line(g_ts.line.p);             /* ultima linha sem quebra no fim */
    Buf raw = {0}, err = {0};
    if (g_ts.res.n) buf_add(&raw, g_ts.res.p, g_ts.res.n);
    {
        FILE *fr = fopen(fe, "rb");
        if (fr) { char tmp[65536]; size_t n; while ((n = fread(tmp, 1, sizeof tmp, fr)) > 0) buf_add(&err, tmp, n); fclose(fr); }
    }
    unlink(fs); unlink(fp); unlink(fo); unlink(fe); rmdir(dir);
    /* saida: {"is_error":..,"result":"..","structured_output":{"text":..,"tool_calls":[{"name":..,"arguments":{..}}]},"usage":{..}} */
    const char *so = NULL, *us = NULL, *res = NULL, *q0 = raw.p ? js_ws(raw.p) : "";
    int is_err = 0; char k[64]; const char *v, *e;
    if (*q0 == '{') for (const char *p = q0 + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) {
        if (!strcmp(k, "structured_output") && *v == '{') so = v; else if (!strcmp(k, "usage") && *v == '{') us = v;
        else if (!strcmp(k, "result") && *v == '"') res = v; else if (!strcmp(k, "is_error")) is_err = !strncmp(v, "true", 4);
    }
    buf_clear(r);
    if (!ok || is_err || !so) {
        Buf m = {0};
        if (res) { const char *pp = res; js_string(&pp, &m); }
        buf_str(r, "{\"error\": {\"message\": ");
        const char *msg = m.p && m.n ? m.p : err.p && err.n ? err.p : rc == 124 ? "timeout" : "no answer from the claude program (is it logged in? run it once in a terminal)";
        tch_jstr(r, msg, strlen(msg) < 600 ? strlen(msg) : 600);
        buf_str(r, "}}");
        free(m.p); free(raw.p); free(err.p);
        return 0;
    }
    const char *tv = NULL, *tc = NULL;
    for (const char *p = so + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) { if (!strcmp(k, "text") && *v == '"') tv = v; else if (!strcmp(k, "tool_calls") && *v == '[') tc = v; }
    buf_str(r, "{\"content\": [");
    int nb = 0;
    if (tv) { const char *te = gs_skipv(tv); if (te && te - tv > 2) { buf_str(r, "{\"type\": \"text\", \"text\": "); buf_add(r, tv, (size_t)(te - tv)); buf_str(r, "}"); nb++; } }
    const char *el, *ee;
    if (tc) for (const char *q = tc + 1; (ee = gs_elem(q, &el)); q = ee) {
        if (*el != '{') continue;
        const char *nv = NULL, *av = NULL;
        for (const char *p = el + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) { if (!strcmp(k, "name") && *v == '"') nv = v; else if (!strcmp(k, "arguments") && *v == '{') av = v; }
        const char *ne = nv ? gs_skipv(nv) : NULL, *ae = av ? gs_skipv(av) : NULL;
        if (!ne) continue;
        char id[64]; snprintf(id, sizeof id, "cli_%d_%d", slog->n, nb);
        buf_str(r, nb++ ? ", " : ""); buf_str(r, "{\"type\": \"tool_use\", \"id\": \""); buf_str(r, id); buf_str(r, "\", \"name\": "); buf_add(r, nv, (size_t)(ne - nv));
        buf_str(r, ", \"input\": "); if (ae) buf_add(r, av, (size_t)(ae - av)); else buf_str(r, "{}");
        buf_str(r, "}");
    }
    buf_str(r, "], \"stop_reason\": \"end_turn\", \"usage\": ");
    const char *ue = us ? gs_skipv(us) : NULL;
    if (ue) buf_add(r, us, (size_t)(ue - us)); else buf_str(r, "{}");
    buf_str(r, "}");
    free(raw.p); free(err.p);
    return 200;
}
#endif

/* uma fala do professor. slog ja tem a mensagem pendente (pedido do usuario ou resultados de ferramenta).
   Escreve em reply a fala na sintaxe do aluno (sem o comeco que o prompt ja abriu) e devolve 1; *cut = 1 quando
   a resposta parou no limite de tokens. 0 = sem resposta (erro, recusa ou interrompido) */
static int teacher_turn(const Config *c, const char *sys, const AHist *slog, const char *sess, Buf *reply, int *cut) {
#ifdef _WIN32
    (void)c; (void)sys; (void)slog; (void)sess; (void)reply; (void)cut;
    fprintf(stderr, "[teacher: Linux only for now]\n");
    return 0;
#else
    const int g4 = c->chat_fmt == 8;
    char key[600];
    *cut = 0;
    const char *base = tch_base(sys, g4);
    Buf r = {0};
    int http = 0;
    const double t0 = now_sec();
    if (g_tch_cli) { http = tch_cli_call(c, base, slog, &r); if (g_stop) { free(r.p); return 0; } goto parse; }
    if (!tch_key(key, sizeof key)) { fprintf(stderr, "[teacher: no API key]\n"); return 0; }
    const char *fb = getenv("KATANA_TEACHER_FALLBACK");
    const int fallback = !(fb && atoi(fb) == 0);
    Buf req = {0}, tsys = {0};
    tch_sys(base, &tsys);
    buf_str(&req, "{\"model\": "); tch_jstr(&req, g_tch_model, strlen(g_tch_model));
    buf_str(&req, ", \"stream\": true, \"max_tokens\": 32000, \"cache_control\": {\"type\": \"ephemeral\"}, \"thinking\": {\"type\": \"adaptive\", \"display\": \"summarized\"}, \"output_config\": {\"effort\": ");
    tch_jstr(&req, g_tch_effort, strlen(g_tch_effort)); buf_str(&req, "}");
    if (fallback) buf_str(&req, ", \"fallbacks\": \"default\"");
    buf_str(&req, ", \"system\": "); tch_jstr(&req, tsys.p ? tsys.p : "", tsys.n);
    buf_str(&req, ", \"tools\": "); tch_tools(base, &req);
    buf_str(&req, ", \"messages\": ["); tch_messages(c, slog, &req); buf_str(&req, "]}");
    free(tsys.p);

    char dir[] = "/tmp/katana_teacher_XXXXXX";
    if (!mkdtemp(dir)) { free(req.p); fprintf(stderr, "[teacher: no temporary folder]\n"); return 0; }
    char hdr[80], body[80], resp[80], qh[200], qb[200], qr[200], full[700], qf[1500], cmd[2400];
    snprintf(hdr, sizeof hdr, "%s/h", dir); snprintf(body, sizeof body, "%s/b", dir); snprintf(resp, sizeof resp, "%s/r", dir);
    int okf = 0;
    {
        const int fd = open(hdr, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            char h[1200];
            const int n = snprintf(h, sizeof h, "content-type: application/json\nanthropic-version: 2026-10-21\nx-api-key: %s\n%s", key, fallback ? "anthropic-beta: server-side-fallback-2026-07-01\n" : "");
            okf = write(fd, h, (size_t)n) == n;
            close(fd);
        }
        FILE *fbd = okf ? fopen(body, "wb") : NULL;
        if (fbd) { okf = fwrite(req.p, 1, req.n, fbd) == req.n; fclose(fbd); } else okf = 0;
    }
    memset(key, 0, sizeof key);
    if (getenv("KATANA_TEACHER_DUMP")) { FILE *fd2 = fopen(getenv("KATANA_TEACHER_DUMP"), "wb"); if (fd2) { fwrite(req.p, 1, req.n, fd2); fclose(fd2); } }
    free(req.p);
    shq(hdr, qh, sizeof qh); shq(body, qb, sizeof qb); shq(resp, qr, sizeof qr);
    snprintf(full, sizeof full, "%s/v1/messages", g_tch_url); shq(full, qf, sizeof qf);
    (void)qr;
    snprintf(cmd, sizeof cmd, "curl -sS -N --max-time 900 -w '\\nKATANA_HTTP %%{http_code}\\n' -H @%s --data-binary @%s %s", qh, qb, qf);
    for (int attempt = 0; okf && attempt < 4 && !g_stop; attempt++) {
        if (attempt) {          /* 429, 5xx ou rede: espera e tenta de novo */
            const int wait = attempt == 1 ? 3 : attempt == 2 ? 10 : 25;
            fprintf(stderr, "%s[teacher: HTTP %d, trying again in %d s]%s\n", CE(C_DIM), http, wait, CE(C_RESET));
            for (int w = 0; w < wait * 10 && !g_stop; w++) usleep(100000);
            if (g_stop) break;
        }
        int rcx = 0;
        as_reset();
        g_cmd_tap = as_tap;
        char *code = run_collect(cmd, 910, 4096, &rcx);
        g_cmd_tap = NULL;
        free(code);
        http = as_finish(&r);
        if (g_as.shown) break;          /* ja apareceu texto na tela: nao repete o pedido */
        if (http == 200 || (http >= 400 && http < 500 && http != 429 && http != 408 && http != 409)) break;
    }
    unlink(hdr); unlink(body); unlink(resp); rmdir(dir);
    if (g_stop) { free(r.p); return 0; }
    if (!okf) { free(r.p); fprintf(stderr, "[teacher: could not write the request]\n"); return 0; }

parse:;
    /* resposta: content, stop_reason, usage (ou error.message) */
    const char *cont = NULL, *q0 = r.p ? js_ws(r.p) : "";
    char stop[40] = "", k[64]; const char *v, *e;
    long tin = 0, tout = 0, tread = 0, twrite = 0;
    Buf err = {0};
    if (*q0 == '{') for (const char *p = q0 + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) {
        if (!strcmp(k, "content") && *v == '[') cont = v;
        else if (!strcmp(k, "stop_reason") && *v == '"') { size_t n = 0; for (const char *w = v + 1; *w && *w != '"' && n < sizeof stop - 1; w++) stop[n++] = *w; stop[n] = 0; }
        else if (!strcmp(k, "usage") && *v == '{') {
            char k2[64]; const char *v2, *e2;
            for (const char *p2 = v + 1; (e2 = gs_member(p2, k2, sizeof k2, &v2)); p2 = e2) {
                if (!strcmp(k2, "input_tokens")) tin = atol(v2); else if (!strcmp(k2, "output_tokens")) tout = atol(v2);
                else if (!strcmp(k2, "cache_read_input_tokens")) tread = atol(v2); else if (!strcmp(k2, "cache_creation_input_tokens")) twrite = atol(v2);
            }
        } else if (!strcmp(k, "error") && *v == '{') {
            char k2[64]; const char *v2, *e2;
            for (const char *p2 = v + 1; (e2 = gs_member(p2, k2, sizeof k2, &v2)); p2 = e2)
                if (!strcmp(k2, "message") && *v2 == '"') { const char *pp = v2; buf_clear(&err); js_string(&pp, &err); }
        }
    }
    if (http != 200 || !cont) {
        fprintf(stderr, "[teacher: HTTP %d: %.400s]\n", http, err.p ? err.p : r.p ? r.p : "no answer (network?)");
        if (getenv("KATANA_UI")) { printf("\x01" "TEACHERR %d %.300s\n", http, err.p ? err.p : "no answer"); fflush(stdout); }
        free(err.p); free(r.p);
        return 0;
    }
    free(err.p);
    g_tch_in += tin + tread + twrite; g_tch_out += tout; g_tch_cread += tread;

    /* blocos: thinking (resumo), text, tool_use */
    Buf reason = {0}, text = {0}, a = {0};
    const char *el, *ee;
    typedef struct { char name[128]; const char *in, *ine; } TU;
    TU tu[64]; int ntu = 0, lastkind = 0;
    for (const char *q = cont + 1; (ee = gs_elem(q, &el)); q = ee) {
        if (*el != '{') continue;
        char type[40] = ""; const char *tv = NULL, *thv = NULL, *nv = NULL, *iv = NULL;
        for (const char *p = el + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) {
            if (!strcmp(k, "type") && *v == '"') { size_t n = 0; for (const char *w = v + 1; *w && *w != '"' && n < sizeof type - 1; w++) type[n++] = *w; type[n] = 0; }
            else if (!strcmp(k, "text") && *v == '"') tv = v; else if (!strcmp(k, "thinking") && *v == '"') thv = v;
            else if (!strcmp(k, "name") && *v == '"') nv = v; else if (!strcmp(k, "input") && *v == '{') iv = v;
        }
        if (!strcmp(type, "thinking") && thv) { const char *pp = thv; Buf t = {0}; js_string(&pp, &t); if (t.p && t.n) { if (reason.n) buf_str(&reason, "\n"); buf_add(&reason, t.p, t.n); } free(t.p); }
        else if (!strcmp(type, "text") && tv) { const char *pp = tv; Buf t = {0}; js_string(&pp, &t); if (t.p && t.n) { if (text.n) buf_str(&text, "\n\n"); buf_add(&text, t.p, t.n); } free(t.p); lastkind = 1; }
        else if (!strcmp(type, "tool_use") && nv && ntu < 64) {
            const char *pp = nv; Buf t = {0}; js_string(&pp, &t);
            snprintf(tu[ntu].name, sizeof tu[ntu].name, "%s", t.p ? t.p : ""); free(t.p);
            tu[ntu].in = iv; tu[ntu].ine = iv ? gs_skipv(iv) : NULL;
            if (tu[ntu].name[0]) { ntu++; lastkind = 2; }
        }
    }
    int whole = 1;
    if (!strcmp(stop, "max_tokens")) { *cut = 1; if (lastkind == 2 && ntu) { ntu--; whole = 0; } }      /* a ultima chamada pode ter vindo pela metade */
    if (!strcmp(stop, "refusal")) fprintf(stderr, "%s[teacher: the request was declined (stop_reason refusal)]%s\n", CE(C_DIM), CE(C_RESET));
    while (text.n && (unsigned char)text.p[text.n - 1] <= ' ') text.p[--text.n] = 0;
    while (reason.n && (unsigned char)reason.p[reason.n - 1] <= ' ') reason.p[--reason.n] = 0;
    if (!text.n && !ntu) { free(reason.p); free(text.p); free(r.p); if (strcmp(stop, "refusal")) fprintf(stderr, "[teacher: empty answer (stop_reason %s)]\n", stop); return 0; }

    /* a fala na sintaxe do aluno, como o importador escreve */
    im_think(c, &a, reason.n ? reason.p : NULL);
    const size_t think_n = a.n;
    if (text.n) buf_add(&a, text.p, text.n);
    for (int i = 0; i < ntu; i++) {
        if (!g4) {
            const char tail = a.n ? a.p[a.n - 1] : '\n';
            if (i > 0) buf_str(&a, "\n"); else if (tail != '\n') buf_str(&a, c->chat_fmt == 10 ? "\n" : "\n\n");
        }
        char *args = NULL;
        if (tu[i].in && tu[i].ine) { const size_t l = (size_t)(tu[i].ine - tu[i].in); args = xalloc(l + 1, 1); memcpy(args, tu[i].in, l); }
        im_call(c, &a, tu[i].name, args ? args : "{}");
        free(args);
    }
    const char *pre = g4 ? g4_answer_pre() : think_pre(c);
    const size_t pl = strlen(pre);
    size_t skip = a.p && pl <= a.n && !strncmp(a.p, pre, pl) ? pl : 0;
    /* gemma4 com raciocinio: depois de resultados o prompt ja abriu o canal de pensamento; a fala so o continua */
    if (g4 && g4_think() && slog->n && slog->v[slog->n - 1].role == 't' && a.p && a.n >= 18 && !strncmp(a.p, "<|channel>thought\n", 18)) skip = 18;
    buf_clear(reply);
    buf_add(reply, a.p + skip, a.n - skip);
    fprintf(stderr, "%s[teacher %s: %ld tokens in (%ld from the cache), %ld out, %.1f s]%s\n", CE(C_DIM), g_tch_model, tin + tread + twrite, tread, tout, now_sec() - t0, CE(C_RESET));
#ifndef _WIN32
    if (g_tch_cli ? g_ts.shown > 0 : g_as.shown > 0) { con_puts(a.p + think_n + text.n); fflush(stdout); }      /* raciocinio e texto ja apareceram enquanto chegavam: so o resto (chamadas) */
    else
#endif
    tch_print(reason.n ? reason.p : NULL, a.p + think_n);

    /* guarda a resposta crua para os pedidos seguintes desta execucao e anota a fala do professor na sessao */
    if (whole && !g_tch_cli) {
        const char *ce = gs_skipv(cont);
        if (ce) {
            g_tch_raw = realloc(g_tch_raw, sizeof(TchRaw) * (size_t)(g_tch_nraw + 1));
            if (!g_tch_raw) die("sem memoria");
            TchRaw *t = &g_tch_raw[g_tch_nraw++];
            t->idx = slog->n;
            const size_t hs = g4 ? skip : 0;             /* o historico do gemma4 guarda a fala sem o comeco do prompt */
            t->txt = xalloc(a.n - hs + 1, 1); memcpy(t->txt, a.p + hs, a.n - hs);
            t->raw = xalloc((size_t)(ce - cont) + 1, 1); memcpy(t->raw, cont, (size_t)(ce - cont));
        }
    }
    tch_mark(sess, slog->n, a.n);
    free(reason.p); free(text.p); free(a.p); free(r.p);
    return 1;
#endif
}
