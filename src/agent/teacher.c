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
 * Rede: o pedido sai pelo curl do sistema. A chave vem de ANTHROPIC_API_KEY ou do arquivo ~/.katana/teacher.key
 * (so do dono) e vai num arquivo de cabecalho temporario com permissao so do dono, nunca na linha de comando.
 * O que sai da maquina: o prompt de sistema do agente (com a memoria salva e o mapa de pastas), a conversa e os
 * resultados das ferramentas. Variaveis: KATANA_TEACHER_MODEL (padrao claude-opus-5-5), KATANA_TEACHER_EFFORT
 * (low, medium, high, xhigh, max; padrao high), KATANA_TEACHER_URL (padrao https://api.anthropic.com),
 * KATANA_TEACHER_FALLBACK=0 (sem o modelo substituto quando o pedido e recusado). */

static int g_teacher;                                   /* 1: as falas do agente vem do professor */
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

static void tch_ui(void) { if (getenv("KATANA_UI")) { printf("\x01" "TEACHER %d %s\n", g_teacher, g_tch_model); fflush(stdout); } }
static void tch_set(int on) {
    char key[600];
    if (on && !tch_key(key, sizeof key)) {
        fprintf(stderr, "%s[teacher: no API key. Set ANTHROPIC_API_KEY or write the key to ~/.katana/teacher.key]%s\n", CE(C_DIM), CE(C_RESET));
        on = 0;
    }
    g_teacher = on;
    if (on) fprintf(stderr, "%s[teacher on: %s answers in place of the local model; the conversation and the tool results are sent to %s]%s\n", CE(C_DIM), g_tch_model, g_tch_url, CE(C_RESET));
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
        buf_str(o, n++ ? ", " : ""); buf_str(o, TCH_EXTRA[i][1]);
    }
    buf_str(o, "]");
}

/* prompt de sistema do professor: o do agente sem o bloco que ensina a sintaxe de chamada do aluno */
static void tch_sys(const char *sys, Buf *o) {
    const char *t0 = strstr(sys, "# Tools\n"), *t1 = t0 ? strstr(t0, "\n# Environment") : NULL;
    if (!t0 || !t1) { buf_str(o, sys); return; }
    buf_add(o, sys, (size_t)(t0 - sys));
    buf_str(o, "# Tools\n\nCall the functions you are given to act. The conversation may contain earlier turns written by a smaller local model: check its claims instead of trusting them.\n");
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
    fputs(rest, stdout);
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
    if (!tch_key(key, sizeof key)) { fprintf(stderr, "[teacher: no API key]\n"); return 0; }
    const char *base = tch_base(sys, g4);
    const char *fb = getenv("KATANA_TEACHER_FALLBACK");
    const int fallback = !(fb && atoi(fb) == 0);
    Buf req = {0}, tsys = {0};
    tch_sys(base, &tsys);
    buf_str(&req, "{\"model\": "); tch_jstr(&req, g_tch_model, strlen(g_tch_model));
    buf_str(&req, ", \"max_tokens\": 16000, \"cache_control\": {\"type\": \"ephemeral\"}, \"thinking\": {\"type\": \"adaptive\", \"display\": \"summarized\"}, \"output_config\": {\"effort\": ");
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
    snprintf(cmd, sizeof cmd, "curl -sS --max-time 900 -o %s -w '%%{http_code}' -H @%s --data-binary @%s %s", qr, qh, qb, qf);
    Buf r = {0};
    int http = 0;
    const double t0 = now_sec();
    for (int attempt = 0; okf && attempt < 4 && !g_stop; attempt++) {
        if (attempt) {          /* 429, 5xx ou rede: espera e tenta de novo */
            const int wait = attempt == 1 ? 3 : attempt == 2 ? 10 : 25;
            fprintf(stderr, "%s[teacher: HTTP %d, trying again in %d s]%s\n", CE(C_DIM), http, wait, CE(C_RESET));
            for (int w = 0; w < wait * 10 && !g_stop; w++) usleep(100000);
            if (g_stop) break;
        }
        int rcx = 0;
        char *code = run_collect(cmd, 910, 4096, &rcx);
        http = code ? atoi(code) : 0;
        free(code);
        buf_clear(&r);
        FILE *fr = fopen(resp, "rb");
        if (fr) { char tmp[65536]; size_t n; while ((n = fread(tmp, 1, sizeof tmp, fr)) > 0) buf_add(&r, tmp, n); fclose(fr); }
        if (http == 200 || (http >= 400 && http < 500 && http != 429 && http != 408 && http != 409)) break;
    }
    unlink(hdr); unlink(body); unlink(resp); rmdir(dir);
    if (g_stop) { free(r.p); return 0; }
    if (!okf) { free(r.p); fprintf(stderr, "[teacher: could not write the request]\n"); return 0; }

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
    const size_t skip = a.p && pl <= a.n && !strncmp(a.p, pre, pl) ? pl : 0;
    buf_clear(reply);
    buf_add(reply, a.p + skip, a.n - skip);
    fprintf(stderr, "%s[teacher %s: %ld tokens in (%ld from the cache), %ld out, %.1f s]%s\n", CE(C_DIM), g_tch_model, tin + tread + twrite, tread, tout, now_sec() - t0, CE(C_RESET));
    tch_print(reason.n ? reason.p : NULL, a.p + think_n);

    /* guarda a resposta crua para os pedidos seguintes desta execucao e anota a fala do professor na sessao */
    if (whole) {
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
