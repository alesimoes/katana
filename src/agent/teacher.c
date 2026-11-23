/* ---------- modo professor: um modelo maior responde no lugar do modelo local, dentro do mesmo agente ----------
 * Com o professor ligado (/teacher on, ou KATANA_TEACHER=1), cada fala do agente vem de um PROGRAMA EXTERNO (plugin)
 * em vez de ser gerada aqui. O katana nao sabe quem esta do outro lado: ele so fala o protocolo de docs/teacher-protocol.md
 * (uma linha JSON no stdin do programa; linhas JSON no stdout). Todo o resto e o agente de sempre: as ferramentas
 * rodam nesta maquina pelo mesmo despacho, a conversa aparece na interface do mesmo jeito e a sessao e gravada no
 * mesmo arquivo.
 *
 * A resposta do professor e reescrita na sintaxe do modelo local (bloco de raciocinio, texto, chamadas de
 * ferramenta no formato do aluno) e entra no historico e no cache KV como se o aluno a tivesse escrito. Por isso:
 *   - a sessao gravada (.ksess, bloco KSX1) ja e dado de treino no formato exato do aluno; as falas do professor
 *     ficam anotadas em <sessao>.teacher e o katana export --teacher treina so nelas;
 *   - o professor pode ser ligado no meio de uma conversa: ele recebe a conversa inteira (a lista de mensagens e
 *     refeita a cada pedido a partir do historico da sessao), inclusive as tentativas do modelo local;
 *   - desligado o professor, o modelo local continua do ponto em que a conversa esta.
 *
 * Qual programa: KATANA_TEACHER_CMD (linha de comando para o sh), ou a primeira linha de ~/.katana/teacher.cmd, ou o
 * executavel ~/.katana/plugins/teacher/run. Opcoes enviadas ao programa: KATANA_TEACHER_MODEL, KATANA_TEACHER_EFFORT
 * (ou /teacher <nome>) e KATANA_TEACHER_MAX_TOKENS; o resto da configuracao (endereco, chave) e do plugin, que herda o
 * ambiente. O que sai da maquina depende do plugin: o prompt de sistema do agente (com a memoria salva e o mapa de
 * pastas), a conversa e os resultados das ferramentas sao entregues a ele. */

static int g_teacher;                                   /* 1: as falas do agente vem do professor */
static char g_tch_model[128], g_tch_effort[16];         /* opcoes repassadas ao programa (vazio: ele escolhe) */
static long g_tch_in, g_tch_out, g_tch_cread;           /* tokens somados nesta execucao (o que o programa informar) */
/* dado opaco devolvido pelo programa junto com a fala (p.ex. blocos de raciocinio assinados): volta igual nas mensagens seguintes */
typedef struct { int idx; char *txt; char *opaque; } TchRaw;
static TchRaw *g_tch_raw; static int g_tch_nraw;

static void js_escape(Buf *b, const char *s, size_t n);
static void im_call(const Config *c, Buf *out, const char *name, const char *args);
static void im_think(const Config *c, Buf *out, const char *reason);

static void tch_jstr(Buf *o, const char *s, size_t n) { buf_str(o, "\""); js_escape(o, s, n); buf_str(o, "\""); }

/* linha de comando do programa do professor; 0 = nenhum configurado */
static int tch_cmdline(char *out, size_t cap) {
    const char *e = getenv("KATANA_TEACHER_CMD");
    if (e && *e) { snprintf(out, cap, "%s", e); return 1; }
    char h[1024], p[1200];
    agent_home(h, sizeof h);
    snprintf(p, sizeof p, "%s/.katana/teacher.cmd", h);
    FILE *f = fopen(p, "rb");
    if (f) {
        size_t n = fread(out, 1, cap - 1, f); fclose(f); out[n] = 0;
        char *nl = strchr(out, '\n'); if (nl) *nl = 0;
        n = strlen(out);
        while (n && (unsigned char)out[n - 1] <= ' ') out[--n] = 0;
        if (n) return 1;
    }
#ifndef _WIN32
    snprintf(p, sizeof p, "%s/.katana/plugins/teacher/run", h);
    if (access(p, X_OK) == 0) { char q[1300]; shq(p, q, sizeof q); snprintf(out, cap, "%s", q); return 1; }
#endif
    return 0;
}

static void tch_ui(void) { if (getenv("KATANA_UI")) { printf("\x01" "TEACHER %d %s\n", g_teacher, g_tch_model[0] ? g_tch_model : "teacher"); fflush(stdout); } }
static void tch_set(int on) {
    char cmd[2048] = "";
    if (on && !tch_cmdline(cmd, sizeof cmd)) {
        fprintf(stderr, "%s[teacher: no teacher program configured. Set KATANA_TEACHER_CMD, write the command to ~/.katana/teacher.cmd, or install an executable at ~/.katana/plugins/teacher/run (see docs/teacher-protocol.md)]%s\n", CE(C_DIM), CE(C_RESET));
        if (getenv("KATANA_UI")) { printf("\x01" "TEACHERR 0 no teacher program configured\n"); fflush(stdout); }
        on = 0;
    }
    g_teacher = on;
    if (on) fprintf(stderr, "%s[teacher on: the teacher program answers in place of the local model; the conversation and the tool results are handed to it (%.200s)]%s\n", CE(C_DIM), cmd, CE(C_RESET));
    else fprintf(stderr, "%s[teacher off: the local model answers]%s\n", CE(C_DIM), CE(C_RESET));
    tch_ui();
}
static void tch_init(void) {
    const char *e;
    if ((e = getenv("KATANA_TEACHER_MODEL")) && *e) snprintf(g_tch_model, sizeof g_tch_model, "%s", e);
    if ((e = getenv("KATANA_TEACHER_EFFORT")) && *e) snprintf(g_tch_effort, sizeof g_tch_effort, "%s", e);
    if ((e = getenv("KATANA_TEACHER")) && *e && strcmp(e, "0") != 0) tch_set(1);
}
/* comando /teacher [on|off|status|nome-do-modelo]; 1 = era o comando */
static int tch_cmd(const char *line) {
    if (strncmp(line, "/teacher", 8) != 0 || (line[8] && line[8] != ' ')) return 0;
    const char *a = line + 8;
    while (*a == ' ') a++;
    if (!*a) tch_set(!g_teacher);
    else if (!strcmp(a, "on")) tch_set(1);
    else if (!strcmp(a, "off")) tch_set(0);
    else if (!strcmp(a, "status")) { fprintf(stderr, "[teacher %s: %s, %ld + %ld tokens in this run (%ld cached)]\n", g_teacher ? "on" : "off", g_tch_model[0] ? g_tch_model : "default model", g_tch_in, g_tch_out, g_tch_cread); tch_ui(); }
    else { snprintf(g_tch_model, sizeof g_tch_model, "%s", a); tch_set(1); }      /* o nome e so uma opcao repassada ao programa */
    return 1;
}

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
        if (ne && *nv == '"') {
            buf_str(o, n++ ? ", {\"name\": " : "{\"name\": "); buf_add(o, nv, (size_t)(ne - nv));
            if (de && *dv == '"') { buf_str(o, ", \"description\": "); buf_add(o, dv, (size_t)(de - dv)); }
            buf_str(o, ", \"parameters\": ");
            if (pe && *pv == '{') buf_add(o, pv, (size_t)(pe - pv)); else buf_str(o, "{\"type\": \"object\", \"properties\": {}}");
            buf_str(o, "}");
        }
        p = e + 1;
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
            buf_str(o, ", \"parameters\": ");
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
        buf_str(o, nb++ ? ", " : ""); buf_str(o, "{\"type\": \"tool_call\", \"id\": \""); buf_str(o, ids[*nid]); buf_str(o, "\", \"name\": ");
        tch_jstr(o, name, strlen(name)); buf_str(o, ", \"arguments\": ");
        if (oe) buf_add(o, ob, (size_t)(oe - ob)); else buf_str(o, "{}");
        buf_str(o, "}");
        (*nid)++;
    }
    return nb;
}

/* dado opaco guardado desta execucao para a mensagem idx (mesmo texto): vira o primeiro bloco da fala */
static const char *tch_opaque_for(int idx, const char *txt) {
    for (int i = g_tch_nraw - 1; i >= 0; i--)
        if (g_tch_raw[i].idx == idx && g_tch_raw[i].opaque && !strcmp(g_tch_raw[i].txt, txt)) return g_tch_raw[i].opaque;
    return NULL;
}

/* mensagens neutras a partir da conversa inteira da sessao: {"role": "user"|"assistant", "content": [blocos]} com blocos
   text, tool_call (id, name, arguments), tool_result (id, content) e opaque (data). Regras garantidas aqui: papeis
   alternados (falas seguidas do mesmo papel viram uma), todo tool_call com o seu tool_result logo depois, nenhuma fala vazia */
static void tch_messages(const Config *c, const AHist *h, Buf *o) {
    const int g4 = c->chat_fmt == 8;
    const char *ro = g4 ? "<|tool_response>" : "<tool_response>\n", *rc = g4 ? "<tool_response|>" : "\n</tool_response>";
    static char ids[64][40];
    int nid = 0, n = 0; char last = 0;
#define TCH_OPEN(role) do { if (last != (role)) { if (last) buf_str(o, "]}, "); buf_str(o, (role) == 'a' ? "{\"role\": \"assistant\", \"content\": [" : "{\"role\": \"user\", \"content\": ["); last = (role); n = 0; } } while (0)
#define TCH_RESULT(id, s, l) do { buf_str(o, n++ ? ", " : ""); buf_str(o, "{\"type\": \"tool_result\", \"id\": \""); buf_str(o, id); buf_str(o, "\", \"content\": "); tch_jstr(o, s, l); buf_str(o, "}"); } while (0)
#define TCH_TEXT(s, l) do { buf_str(o, n++ ? ", " : ""); buf_str(o, "{\"type\": \"text\", \"text\": "); tch_jstr(o, s, l); buf_str(o, "}"); } while (0)
    for (int i = 0; i < h->n; i++) {
        const char r = h->v[i].role, *s = h->v[i].s;
        if (r == 'a') {
            Buf b = {0}; static char nids[64][40]; int nn = 0;
            int nb = tch_asst_blocks(c, s, i, &b, nids, &nn);
            if (!nb) { free(b.p); continue; }                    /* fala vazia (interrompida): nao vai */
            if (last == 'a' || !last) {                          /* duas falas seguidas do modelo, ou a conversa comeca por ele */
                TCH_OPEN('u');
                for (int j = 0; j < nid; j++) TCH_RESULT(ids[j], "(no result recorded)", 20);
                if (!nid) TCH_TEXT("(continue)", 10);
                nid = 0;
            }
            TCH_OPEN('a');
            const char *op = tch_opaque_for(i, s);
            if (op) { buf_str(o, "{\"type\": \"opaque\", \"data\": "); buf_str(o, op); buf_str(o, "}, "); }
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
    fprintf(f, "%d\t%zu\t%s\n", idx, len, g_tch_model[0] ? g_tch_model : "teacher");
    fclose(f);
}

#ifndef _WIN32
/* ---- saida do programa do professor, linha a linha, enquanto ele escreve ----
 * Eventos (um objeto JSON por linha): thinking e text (pedacos, impressos conforme chegam), progress (bytes ja
 * escritos de chamadas longas), done (a fala inteira) e error. Linhas que nao sao JSON sao ignoradas. */
static struct { Buf line, think, text, done, err; int think_open, think_closed, has_done, has_err, text_n; size_t shown; double mark; int midline; } g_tp;
static void tp_reset(void) {
    buf_clear(&g_tp.line); buf_clear(&g_tp.think); buf_clear(&g_tp.text); buf_clear(&g_tp.done); buf_clear(&g_tp.err);
    g_tp.think_open = g_tp.think_closed = g_tp.has_done = g_tp.has_err = g_tp.text_n = 0; g_tp.shown = 0; g_tp.mark = now_sec(); g_tp.midline = 0;
}
static void tp_show(const char *s, size_t n) { if (!n) return; fwrite(s, 1, n, stdout); fflush(stdout); g_tp.shown += n; g_tp.midline = s[n - 1] != '\n'; }
static void tp_close_think(void) { if (g_tp.think_open && !g_tp.think_closed) { fprintf(stdout, "%s\n", CE(C_RESET)); fflush(stdout); g_tp.think_closed = 1; g_tp.midline = 0; } }
static void tp_event(const char *js) {
    const char *q = js_ws(js);
    if (*q != '{') return;
    char k[64], ev[24] = ""; const char *v, *e, *tv = NULL, *mv = NULL, *bv = NULL;
    for (const char *p = q + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) {
        if (!strcmp(k, "event") && *v == '"') { size_t n = 0; for (const char *w = v + 1; *w && *w != '"' && n < sizeof ev - 1; w++) ev[n++] = *w; ev[n] = 0; }
        else if (!strcmp(k, "text") && *v == '"') tv = v; else if (!strcmp(k, "message") && *v == '"') mv = v; else if (!strcmp(k, "bytes")) bv = v;
    }
    Buf t = {0};
    if (!strcmp(ev, "thinking") && tv) {
        const char *pp = tv; js_string(&pp, &t);
        if (t.p && t.n) {
            if (!g_tp.think_open) { fprintf(stdout, "%s[raciocinio]\n", CE(C_DIM)); g_tp.think_open = 1; g_tp.shown++; }
            buf_add(&g_tp.think, t.p, t.n); tp_show(t.p, t.n);
        }
    } else if (!strcmp(ev, "text") && tv) {
        const char *pp = tv; js_string(&pp, &t);
        if (t.p && t.n) { tp_close_think(); buf_add(&g_tp.text, t.p, t.n); tp_show(t.p, t.n); }
    } else if (!strcmp(ev, "progress") && bv) {
        const double now = now_sec();
        if (now - g_tp.mark >= 1.0 && getenv("KATANA_UI")) {      /* escrevendo chamadas longas: mostra o quanto ja chegou */
            g_tp.mark = now; tp_close_think();
            if (g_tp.midline) { fputc('\n', stdout); g_tp.midline = 0; }
            printf("\x01" "TEACHW %ld\n", atol(bv)); fflush(stdout);
        }
    } else if (!strcmp(ev, "done")) { buf_clear(&g_tp.done); buf_str(&g_tp.done, js); g_tp.has_done = 1; }
    else if (!strcmp(ev, "error")) { if (mv) { const char *pp = mv; js_string(&pp, &t); if (t.p) { buf_clear(&g_tp.err); buf_add(&g_tp.err, t.p, t.n); } } g_tp.has_err = 1; }
    free(t.p);
}
static void tp_tap(const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (b[i] != '\n') { buf_add(&g_tp.line, b + i, 1); continue; }
        if (g_tp.line.n && g_tp.line.p[g_tp.line.n - 1] == '\r') g_tp.line.p[--g_tp.line.n] = 0;
        if (g_tp.line.n) tp_event(g_tp.line.p);
        buf_clear(&g_tp.line);
    }
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
    *cut = 0;
    char pcmd[2048];
    if (!tch_cmdline(pcmd, sizeof pcmd)) { fprintf(stderr, "[teacher: no teacher program configured]\n"); return 0; }
    const char *base = tch_base(sys, g4);
    const double t0 = now_sec();

    /* pedido: uma linha JSON no stdin do programa */
    Buf req = {0}, tsys = {0};
    tch_sys(base, &tsys);
    const char *mt = getenv("KATANA_TEACHER_MAX_TOKENS");
    buf_str(&req, "{\"protocol\": 1, \"system\": "); tch_jstr(&req, tsys.p ? tsys.p : "", tsys.n);
    buf_str(&req, ", \"tools\": "); tch_tools(base, &req);
    buf_str(&req, ", \"messages\": ["); tch_messages(c, slog, &req); buf_str(&req, "], \"options\": {\"max_tokens\": ");
    { char t[32]; snprintf(t, sizeof t, "%d", mt && atoi(mt) > 0 ? atoi(mt) : 32000); buf_str(&req, t); }
    if (g_tch_model[0]) { buf_str(&req, ", \"model\": "); tch_jstr(&req, g_tch_model, strlen(g_tch_model)); }
    if (g_tch_effort[0]) { buf_str(&req, ", \"effort\": "); tch_jstr(&req, g_tch_effort, strlen(g_tch_effort)); }
    buf_str(&req, "}}\n");
    free(tsys.p);
    if (getenv("KATANA_TEACHER_DUMP")) { FILE *fd2 = fopen(getenv("KATANA_TEACHER_DUMP"), "wb"); if (fd2) { fwrite(req.p, 1, req.n, fd2); fclose(fd2); } }

    char dir[] = "/tmp/katana_teacher_XXXXXX";
    if (!mkdtemp(dir)) { free(req.p); fprintf(stderr, "[teacher: no temporary folder]\n"); return 0; }
    char fi[80], fe[80], qi[200], qe[200], qd[200], qc[2300];
    snprintf(fi, sizeof fi, "%s/in", dir); snprintf(fe, sizeof fe, "%s/err", dir);
    int ok = 0;
    { const int fd = open(fi, O_WRONLY | O_CREAT | O_TRUNC, 0600); if (fd >= 0) { ok = write(fd, req.p, req.n) == (ssize_t)req.n; close(fd); } }
    free(req.p);
    shq(fi, qi, sizeof qi); shq(fe, qe, sizeof qe); shq(dir, qd, sizeof qd); shq(pcmd, qc, sizeof qc);
    static char cmd[5000];
    snprintf(cmd, sizeof cmd, "cd %s && sh -c %s < %s 2> %s", qd, qc, qi, qe);
    int rc = 1;
    tp_reset();
    if (ok) { g_cmd_tap = tp_tap; char *o = run_collect(cmd, 910, 2048, &rc); g_cmd_tap = NULL; free(o); }
    if (g_tp.line.n) tp_event(g_tp.line.p);             /* ultima linha sem quebra no fim */
    tp_close_think();
    Buf serr = {0};
    { FILE *fr = fopen(fe, "rb"); if (fr) { char tmp[8192]; size_t n = fread(tmp, 1, sizeof tmp, fr); fclose(fr); buf_add(&serr, tmp, n); } }
    unlink(fi); unlink(fe); rmdir(dir);
    if (g_stop) { free(serr.p); return 0; }
    if (!ok || !g_tp.has_done) {
        const char *m = g_tp.has_err && g_tp.err.n ? g_tp.err.p : serr.n ? serr.p : !ok ? "could not write the request" : rc == 124 ? "timeout" : "the teacher program ended without a done event";
        fprintf(stderr, "[teacher: %.400s]\n", m);
        if (getenv("KATANA_UI")) { printf("\x01" "TEACHERR %d %.300s\n", rc, m); fflush(stdout); }
        free(serr.p);
        return 0;
    }
    free(serr.p);

    /* o evento done: text, thinking, tool_calls [{id, name, arguments}], stop, usage {input, output, cached}, opaque */
    char stop[40] = "", k[64]; const char *v, *e;
    const char *tcv = NULL, *opv = NULL, *tv = NULL, *thv = NULL;
    long tin = 0, tout = 0, tread = 0;
    for (const char *p = js_ws(g_tp.done.p) + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) {
        if (!strcmp(k, "text") && *v == '"') tv = v; else if (!strcmp(k, "thinking") && *v == '"') thv = v;
        else if (!strcmp(k, "tool_calls") && *v == '[') tcv = v; else if (!strcmp(k, "opaque")) opv = v;
        else if (!strcmp(k, "stop") && *v == '"') { size_t n = 0; for (const char *w = v + 1; *w && *w != '"' && n < sizeof stop - 1; w++) stop[n++] = *w; stop[n] = 0; }
        else if (!strcmp(k, "usage") && *v == '{') {
            char k2[64]; const char *v2, *e2;
            for (const char *p2 = v + 1; (e2 = gs_member(p2, k2, sizeof k2, &v2)); p2 = e2) {
                if (!strcmp(k2, "input")) tin = atol(v2); else if (!strcmp(k2, "output")) tout = atol(v2); else if (!strcmp(k2, "cached")) tread = atol(v2);
            }
        }
    }
    g_tch_in += tin; g_tch_out += tout; g_tch_cread += tread;

    Buf reason = {0}, text = {0}, a = {0};
    if (thv) { const char *pp = thv; js_string(&pp, &reason); } else if (g_tp.think.n) buf_add(&reason, g_tp.think.p, g_tp.think.n);
    if (tv) { const char *pp = tv; js_string(&pp, &text); } else if (g_tp.text.n) buf_add(&text, g_tp.text.p, g_tp.text.n);
    typedef struct { char name[128]; const char *in, *ine; } TU;
    TU tu[64]; int ntu = 0;
    const char *el, *ee;
    if (tcv) for (const char *q = tcv + 1; ntu < 64 && (ee = gs_elem(q, &el)); q = ee) {
        if (*el != '{') continue;
        const char *nv = NULL, *av = NULL;
        for (const char *p = el + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) { if (!strcmp(k, "name") && *v == '"') nv = v; else if (!strcmp(k, "arguments") && *v == '{') av = v; }
        if (!nv) continue;
        Buf t = {0}; const char *pp = nv; js_string(&pp, &t);
        snprintf(tu[ntu].name, sizeof tu[ntu].name, "%s", t.p ? t.p : ""); free(t.p);
        tu[ntu].in = av; tu[ntu].ine = av ? gs_skipv(av) : NULL;
        if (tu[ntu].name[0]) ntu++;
    }
    if (!strcmp(stop, "max_tokens")) { *cut = 1; if (ntu) ntu--; }      /* a ultima chamada pode ter vindo pela metade */
    if (!strcmp(stop, "refusal")) fprintf(stderr, "%s[teacher: the request was declined (stop refusal)]%s\n", CE(C_DIM), CE(C_RESET));
    while (text.n && (unsigned char)text.p[text.n - 1] <= ' ') text.p[--text.n] = 0;
    while (reason.n && (unsigned char)reason.p[reason.n - 1] <= ' ') reason.p[--reason.n] = 0;
    if (!text.n && !ntu) { free(reason.p); free(text.p); if (strcmp(stop, "refusal")) fprintf(stderr, "[teacher: empty answer (stop %s)]\n", stop); return 0; }

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
    fprintf(stderr, "%s[teacher %s: %ld tokens in (%ld cached), %ld out, %.1f s]%s\n", CE(C_DIM), g_tch_model[0] ? g_tch_model : "default", tin, tread, tout, now_sec() - t0, CE(C_RESET));
    if (g_tp.shown > 0) { con_puts(a.p + think_n + text.n); fflush(stdout); }      /* raciocinio e texto ja apareceram enquanto chegavam: so o resto (chamadas) */
    else tch_print(reason.n ? reason.p : NULL, a.p + think_n);

    /* guarda o dado opaco para as mensagens seguintes desta execucao e anota a fala do professor na sessao */
    if (opv && *opv != 'n' && gs_skipv(opv)) {
        const char *oe = gs_skipv(opv);
        g_tch_raw = realloc(g_tch_raw, sizeof(TchRaw) * (size_t)(g_tch_nraw + 1));
        if (!g_tch_raw) die("sem memoria");
        TchRaw *t = &g_tch_raw[g_tch_nraw++];
        t->idx = slog->n;
        const size_t hs = g4 ? skip : 0;             /* o historico do gemma4 guarda a fala sem o comeco do prompt */
        t->txt = xalloc(a.n - hs + 1, 1); memcpy(t->txt, a.p + hs, a.n - hs);
        t->opaque = xalloc((size_t)(oe - opv) + 1, 1); memcpy(t->opaque, opv, (size_t)(oe - opv));
    }
    tch_mark(sess, slog->n, a.n);
    free(reason.p); free(text.p); free(a.p);
    return 1;
#endif
}
