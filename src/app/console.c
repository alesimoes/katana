/* ---------- console: cores, Ctrl+C, comandos de barra ----------
 * Cores ANSI discretas so quando o terminal e interativo e NO_COLOR nao esta definido.
 * Ctrl+C durante a geracao so interrompe a resposta; fora dela encerra o programa. */
#ifdef _WIN32
#include <io.h>
#define K_ISATTY _isatty
#define K_FILENO _fileno
#else
#define K_ISATTY isatty
#define K_FILENO fileno
#endif
#include <signal.h>

typedef struct { char *p; size_t n, cap; } Buf;
static void buf_add(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2 + 256;
        b->p = realloc(b->p, b->cap);
        if (!b->p) die("sem memoria");
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void buf_str(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
static void buf_clear(Buf *b) { b->n = 0; if (b->p) b->p[0] = 0; }

static size_t tok_append(const Tokenizer *tk, int id, char *out, size_t len, size_t cap);

static int col_out, col_err;
static volatile sig_atomic_t g_stop, g_busy;
#define CE(code) (col_err ? (code) : "")
#define CO(code) (col_out ? (code) : "")
#define C_DIM   "\033[2m"
#define C_CYAN  "\033[36m"
#define C_RESET "\033[0m"

static void on_sigint(int s) {
    signal(SIGINT, on_sigint);
    if (g_busy) g_stop = 1;
    else _Exit(130);
}

static void console_init(void) {
    const int nc = getenv("NO_COLOR") != NULL && getenv("NO_COLOR")[0] != 0;
    col_out = !nc && K_ISATTY(K_FILENO(stdout));
    col_err = !nc && K_ISATTY(K_FILENO(stderr));
#ifdef _WIN32
    {   /* liga as sequencias ANSI no console do Windows */
        DWORD mode;
        HANDLE h[2] = { GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE) };
        for (int i = 0; i < 2; i++)
            if (GetConsoleMode(h[i], &mode)) SetConsoleMode(h[i], mode | 0x0004);
    }
#endif
    signal(SIGINT, on_sigint);
}

/* nomes dos comandos em ingles; os nomes antigos em portugues continuam aceitos como apelidos (reescreve a linha no lugar) */
static void cmd_norm(char *line, size_t cap) {
    static const char *const M[][2] = { {"novo", "new"}, {"sistema", "system"}, {"salvar", "save"}, {"ajuda", "help"}, {"sair", "exit"},
        {"quit", "exit"}, {"compactar", "compact"}, {"sessao", "session"}, {"retomar", "resume"} };
    if (line[0] != '/') return;
    const size_t w = strcspn(line + 1, " \t");
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++) {
        if (strlen(M[i][0]) != w || strncmp(line + 1, M[i][0], w) != 0) continue;
        const size_t tl = strlen(M[i][1]), rest = strlen(line + 1 + w);
        if (1 + tl + rest + 1 > cap) return;
        memmove(line + 1 + tl, line + 1 + w, rest + 1);
        memcpy(line + 1, M[i][1], tl);
        return;
    }
}

/* comandos comuns; devolve 0 = nao e comando, 1 = tratado, 2 = sair. *reset = 1 recomeca a conversa */
static int console_cmd(const char *line, Buf *tr, char *sys, size_t syscap, int *reset) {
    if (line[0] != '/') return 0;
    if (strcmp(line, "/exit") == 0) return 2;
    if (strcmp(line, "/new") == 0) {
        *reset = 1; buf_clear(tr);
        fprintf(stderr, "%s[new conversation]%s\n", CE(C_DIM), CE(C_RESET));
        return 1;
    }
    if (strncmp(line, "/system", 7) == 0 && (line[7] == 0 || line[7] == ' ')) {
        const char *t = line + 7;
        while (*t == ' ') t++;
        snprintf(sys, syscap, "%s", t);
        *reset = 1; buf_clear(tr);
        fprintf(stderr, "%s[system prompt %s; new conversation]%s\n", CE(C_DIM), *t ? "set" : "removed", CE(C_RESET));
        return 1;
    }
    if (strncmp(line, "/save", 5) == 0 && (line[5] == 0 || line[5] == ' ')) {
        const char *t = line + 5;
        while (*t == ' ') t++;
        if (!*t) { fprintf(stderr, "usage: /save <file>\n"); return 1; }
        FILE *f = fopen(t, "wb");
        if (!f) { fprintf(stderr, "[could not write %s]\n", t); return 1; }
        if (sys[0]) fprintf(f, "System: %s\n\n", sys);
        if (tr->n) fwrite(tr->p, 1, tr->n, f);
        fclose(f);
        fprintf(stderr, "%s[conversation saved to %s]%s\n", CE(C_DIM), t, CE(C_RESET));
        return 1;
    }
    if (strcmp(line, "/help") == 0) {
        fprintf(stderr,
            "  /new              clears the conversation\n"
            "  /compact          (agent) summarizes and shrinks the old context to free space\n"
            "  /system <text>    sets the system prompt (and restarts the conversation)\n"
            "  /save <file>      writes the conversation to a text file\n"
            "  /plan             (agent) plan mode: read-only until you approve with /go (/plan off leaves it)\n"
            "  /undo             (agent) puts back the files changed by the last request that changed any\n"
            "  /cd <folder>      (agent) changes the working folder (asks to trust it the first time)\n"
            "  /help             this list\n"
            "  /exit             quits (also Ctrl+D)\n"
            "  Ctrl+C            interrupts the current answer\n");
        return 1;
    }
    fprintf(stderr, "[unknown command; /help lists the commands]\n");
    return 1;
}

/* gera a resposta em streaming a partir dos logits lg; para no fim de turno (eos, token de controle
 * ou stop2), no limite de tokens, no contexto cheio ou em Ctrl+C. Acumula o texto em cap (se nao NULL) */
static const char *think_pre(const Config *c);
static int go_reason(void);
static int gen_console(Model *m, float *lg, int *pos, int max_tokens, float temp, int stop2, Buf *cap) {
    const Config *c = &m->cfg;
    const Tokenizer *tk = &m->tk;
    int gen = 0;
    char tmp[256];
    if (!g_agent_sys) gr_env_begin(tk, c->vocab, stop2);       /* KATANA_JSON: so fora do agente */
    /* raciocinio (<think> aberto no prompt): aparece apagado e separado da resposta */
    int thinking = c->think && strncmp(think_pre(c), "<think>", 7) == 0 && !strstr(think_pre(c), "</think>"), skipnl = 0, skiplabel = 0;
    /* harmony (gpt-oss): canal atual (1 analise, 2 final, 3 comentario), lendo nome do canal, dentro do corpo, cabecalho de analise impresso */
    int go_ch = go_reason() ? 1 : 2, go_rdch = 0, go_body = c->chat_fmt == 9, go_an = 0;
    if (thinking) { fprintf(stdout, "%s[raciocinio]\n", CE(C_DIM)); fflush(stdout); }
    while (gen < max_tokens && *pos < c->ctx - 1 && !g_stop) {
        g_gr.left = max_tokens - gen;
        const int t = sample_c(lg, c->vocab, temp, c->eos);
        if (t == c->eos || t == stop2) break;
        if (c->chat_fmt == 9) { if (t == 200012) break; }   /* <|call|>: chamada de ferramenta */
        else if (tk->toktype && tk->toktype[t] == 3) break;
        const int tl = tok_append(tk, t, tmp, 0, sizeof tmp - 1);
        tmp[tl] = 0;
        if (c->chat_fmt == 9) {         /* harmony: estrutura em canais (<|channel|>nome<|message|> ... <|end|>) */
            if (t == 200005) { go_rdch = 1; go_body = 0; }          /* <|channel|> */
            else if (go_rdch) { if (t == 200008) { go_rdch = 0; go_body = 1; } else go_ch = t == 35644 ? 1 : t == 17196 ? 2 : 3; }  /* nome: analysis/final/outro */
            else if (t == 200008) go_body = 1;                      /* <|message|> */
            else if (t == 200007 || t == 200006) go_body = 0;       /* <|end|> / <|start|> */
            else if (go_body) {
                if (go_ch == 2) { if (go_an) { fprintf(stdout, "%s\n", CE(C_RESET)); go_an = 0; } print_token(tk, t, stdout); if (cap && !g_agent_sys) buf_add(cap, tmp, tl); }
                else if (go_ch == 1 && go_reason()) { if (!go_an) { fprintf(stdout, "%s[raciocinio]\n", CE(C_DIM)); go_an = 1; } print_token(tk, t, stdout); }
            }
            if (cap && g_agent_sys) buf_add(cap, tmp, tl);    /* agente: texto cru para achar a chamada de ferramenta */
            fflush(stdout);
            gen++;
            lg = forward(m, t, (*pos)++, 1);
            continue;
        }
        if (c->chat_fmt == 8 && !thinking && strcmp(tmp, "<|channel>") == 0) {   /* gemma4: canal de raciocinio */
            thinking = 1; skiplabel = 1;
            fprintf(stdout, "%s[raciocinio]\n", CE(C_DIM));
        } else if (skiplabel) {         /* nome do canal ("thought") ate a quebra de linha */
            if (strchr(tmp, '\n')) skiplabel = 0;
        } else if (thinking && (strstr(tmp, "</think>") || (c->chat_fmt == 8 && strcmp(tmp, "<channel|>") == 0))) {
            thinking = 0;
            fprintf(stdout, "%s\n", CE(C_RESET));
            skipnl = 1;
        } else if (skipnl && strspn(tmp, "\n") == (size_t)tl) {
            /* quebras de linha logo depois do raciocinio: nao imprime */
        } else { skipnl = 0; print_token(tk, t, stdout); }
        fflush(stdout);
        if (cap) buf_add(cap, tmp, tl);
        if (g_gr.on) { gr_accept(t); if (g_gr.tool && gr_done()) gr_off(); }
        else if (g_agent_sys && cap && cap->n >= 10 && !c->glm_args) {      /* GLM: a chamada nao e JSON nem <function=> */
            /* o modelo abriu uma chamada de ferramenta: JSON valido com ferramenta e argumentos do schema (XML: nome da funcao) */
            const int xml = cap->n >= 10 && strcmp(cap->p + cap->n - 10, "<function=") == 0;
            if (xml || (cap->n >= 11 && strcmp(cap->p + cap->n - 11, "<tool_call>") == 0)) {
                gr_prepare(tk, c->vocab);
                g_gr.stop2 = stop2;
                if (gr_tools(g_agent_sys, xml)) gr_start();
            }
        }
        gen++;
        lg = forward(m, t, (*pos)++, 1);
    }
    gr_off();
    if (thinking) fprintf(stdout, "%s", CE(C_RESET));
    if (g_stop) fprintf(stderr, "\n%s[interrompido]%s", CE(C_DIM), CE(C_RESET));
    return gen;
}

static int g_no_ready;      /* 1: o proximo prompt nao manda READY (comando silencioso, ex.: /sessao) */
static void prompt_user(void) {
    fprintf(stderr, "\n%syou>%s ", CE(C_CYAN), CE(C_RESET));
    fflush(stderr);
    if (g_no_ready) { g_no_ready = 0; return; }
    if (getenv("KATANA_UI")) { fputs("\x01" "READY\n", stdout); fflush(stdout); }   /* camada de interface: fim do turno */
}

static void stats_line(int in_n, double in_s, int gen, double gen_s, int pos, int ctx) {
    fprintf(stderr, "%s[entrada: %d tokens, %.1f tok/s | resposta: %d tokens, %.1f tok/s | contexto %d de %d (%d%%)]%s\n",
            CE(C_DIM), in_n, in_n / (in_s + 1e-9), gen, gen / (gen_s + 1e-9), pos, ctx, 100 * pos / ctx, CE(C_RESET));
}

/* le uma linha sem o fim de linha; 0 no fim da entrada */
static int read_line(char *line, size_t cap) {
    if (!fgets(line, (int)cap, stdin)) return 0;
    size_t ln = strlen(line);
    while (ln > 0 && (line[ln - 1] == '\n' || line[ln - 1] == '\r')) line[--ln] = 0;
    return 1;
}

/* ---------- conversa de varios turnos ----------
 * O processo fica vivo e o cache KV guarda a conversa: cada turno so processa o texto novo.
 * Num MoE isso tambem mantem na VRAM os especialistas do assunto. Comandos: /novo recomeca,
 * /sair termina. Quando o contexto enche, a conversa recomeca (mantendo o prompt de sistema). */
/* inicio da resposta do assistente em modelos que raciocinam (<think>): KATANA_THINK=0 desliga o
   raciocinio (bloco vazio, como o proprio modelo preve); padrao: raciocina */
/* bloco de raciocinio vazio (o modelo responde direto); GLM: so o fecho, como no template dele */
static const char *think_off(const Config *c) { return !c->think ? "" : c->glm_args ? "</think>" : "<think>\n\n</think>\n\n"; }
static const char *think_pre(const Config *c) {
    if (!c->think || c->chat_fmt == 8) return "";
    const char *e = getenv("KATANA_THINK");
    if (e && atoi(e) == 0) return think_off(c);
    return c->glm_args ? "<think>" : "<think>\n";
}
/* marcas de turno do agente: ChatML ou GLM (g_glm; no GLM o inicio de um turno ja fecha o anterior) */
static int g_glm;
static const char *tf_sys(void)  { return g_glm ? "[gMASK]<sop><|system|>" : "<|im_start|>system\n"; }
static const char *tf_user(void) { return g_glm ? "<|user|>" : "<|im_start|>user\n"; }
static const char *tf_asst(void) { return g_glm ? "<|assistant|>" : "<|im_start|>assistant\n"; }
static const char *tf_tool(void) { return g_glm ? "<|observation|>" : "<|im_start|>user\n"; }     /* resultados de ferramenta */
static const char *tf_end(void)  { return g_glm ? "" : "<|im_end|>\n"; }

/* texto de um turno do usuario no formato do modelo, terminando no inicio da resposta. first = 1 no primeiro turno
   (leva o prompt de sistema); nos seguintes, o texto comeca pelo fecho do turno anterior (o token de fim de turno
   nao entra no cache KV, entao o fecho vai aqui). Reproduz o chat_template do modelo. */
/* Gemma 4: raciocinio so com KATANA_THINK=1 (o template do modelo o deixa desligado por padrao). Ligado, o sistema
   abre com <|think|> e o modelo escreve <|channel>thought ... <channel|> antes da resposta; desligado, o inicio
   da resposta ja traz o canal vazio. */
static int g4_think(void) { const char *e = getenv("KATANA_THINK"); return e && atoi(e) != 0; }
static const char *g4_answer_pre(void) { return g4_think() ? "" : "<|channel>thought\n<channel|>"; }
/* gpt-oss (harmony): raciocinio desligado por padrao (KATANA_THINK=1 liga). Desligado, a resposta ja
   comeca no canal final, entao o modelo nao escreve o canal de analise. */
static int go_reason(void) { const char *e = getenv("KATANA_THINK"); return e && atoi(e) != 0; }
static const char *go_answer_pre(void) { return go_reason() ? "<|channel|>analysis<|message|>" : "<|channel|>final<|message|>"; }

static void turn_text(const Config *c, char *out, size_t n, const char *sys, const char *user, int first) {
    char sp[4096] = "";
    char *ut = NULL, *st = NULL;
    if (c->trim_msgs) {     /* o template aplica |trim as mensagens */
        while (*user == ' ' || (*user >= 9 && *user <= 13)) user++;
        size_t l = strlen(user);
        while (l && (user[l - 1] == ' ' || (user[l - 1] >= 9 && user[l - 1] <= 13))) l--;
        ut = xalloc(l + 1, 1); memcpy(ut, user, l); user = ut;
        if (sys) {
            while (*sys == ' ' || (*sys >= 9 && *sys <= 13)) sys++;
            l = strlen(sys);
            while (l && (sys[l - 1] == ' ' || (sys[l - 1] >= 9 && sys[l - 1] <= 13))) l--;
            st = xalloc(l + 1, 1); memcpy(st, sys, l); sys = st;
        }
    }
    int sp_done = 0;
    if (first && c->chat_fmt == 1 && c->llama_hdr) {      /* data do dia: KATANA_DATE fixa (testes) */
        char dt[64];
        const char *e = getenv("KATANA_DATE");
        if (e) snprintf(dt, sizeof dt, "%s", e);
        else { time_t t = time(NULL); strftime(dt, sizeof dt, "%d %b %Y", localtime(&t)); }
        snprintf(sp, sizeof sp, "<|start_header_id|>system<|end_header_id|>\n\nCutting Knowledge Date: December 2023\nToday Date: %s\n\n%s<|eot_id|>", dt, sys ? sys : "");
        sp_done = 1;
    }
    if (first && !(sys && *sys) && c->chat_fmt == 0 && c->def_sys) sys = c->def_sys;
    if (!sp_done && first && sys && *sys) switch (c->chat_fmt) {
        case 0:  snprintf(sp, sizeof sp, "<|im_start|>system\n%s<|im_end|>\n", sys); break;
        case 1:  snprintf(sp, sizeof sp, "<|start_header_id|>system<|end_header_id|>\n\n%s<|eot_id|>", sys); break;
        case 2:  snprintf(sp, sizeof sp, "<|system|>\n%s</s>\n", sys); break;
        case 3:  snprintf(sp, sizeof sp, "%s\n\n", sys); break;
        case 5:  snprintf(sp, sizeof sp, "%s\n\n", sys); break;
        case 6:  snprintf(sp, sizeof sp, "<|im_system|>system<|im_middle|>%s<|im_end|>", sys); break;
        case 7:  snprintf(sp, sizeof sp, "<|system|>\n%s\n", sys); break;
        case 8:  break;
        case 9:  break;
        case 10: snprintf(sp, sizeof sp, c->glm_args ? "<|system|>%s" : "<|system|>\n%s", sys); break;
        default: snprintf(sp, sizeof sp, "%s\n", sys); break;
    }
    if (c->chat_fmt == 8 && first && ((sys && *sys) || g4_think()))
        snprintf(sp, sizeof sp, "<|turn>system\n%s%s<turn|>\n", g4_think() ? "<|think|>\n" : "", sys ? sys : "");
    if (c->chat_fmt == 9 && first) {       /* harmony: mensagem de sistema (+ developer com o prompt do usuario) */
        const char *reason = go_reason() ? "high" : "low";
        char dev[3072] = "";
        if (sys && *sys) snprintf(dev, sizeof dev, "<|start|>developer<|message|># Instructions\n\n%s<|end|>", sys);
        snprintf(sp, sizeof sp, "<|start|>system<|message|>You are ChatGPT, a large language model trained by OpenAI.\n\nReasoning: %s\n\n# Valid channels: analysis, final. Channel must be included for every message.<|end|>%s", reason, dev);
    }
    switch (c->chat_fmt) {
        case 0:  snprintf(out, n, "%s%s<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n%s", first ? "" : "<|im_end|>\n", sp, user, think_pre(c)); break;
        case 1:  snprintf(out, n, "%s%s%s<|start_header_id|>user<|end_header_id|>\n\n%s<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n\n",
                          first ? "" : "<|eot_id|>", first && !c->add_bos ? "<|begin_of_text|>" : "", sp, user); break;
        case 2:  snprintf(out, n, "%s%s<|user|>\n%s</s>\n<|assistant|>\n", first ? "" : "</s>\n", sp, user); break;
        case 3:  snprintf(out, n, "%s[INST] %s%s [/INST]", first ? "" : "</s>", sp, user); break;
        case 5:  snprintf(out, n, "%s%sUser: %s\n\nAssistant:", first ? "" : c->eos_text, sp, user); break;     /* o template fecha a resposta com o eos */
        case 6:  snprintf(out, n, "%s%s<|im_user|>user<|im_middle|>%s<|im_end|><|im_assistant|>assistant<|im_middle|>", first ? "" : "<|im_end|>",
                          first && !*sp ? "<|im_system|>system<|im_middle|>You are a helpful assistant<|im_end|>" : sp, user); break;
        case 7:  snprintf(out, n, "%s%s%s<|user|>\n%s\n<|assistant|>\n", first ? c->bos_text : c->eos_text, first ? "" : "\n", sp, user); break;
        case 8:  snprintf(out, n, "%s%s<|turn>user\n%s<turn|>\n<|turn>model\n%s", first ? "" : "<turn|>\n", sp, user, g4_answer_pre()); break;
        case 9:  snprintf(out, n, "%s%s<|start|>user<|message|>%s<|end|><|start|>assistant%s", first ? "" : "<|end|>", sp, user, go_answer_pre()); break;
        case 10: if (c->glm_args) snprintf(out, n, "%s%s<|user|>%s<|assistant|>%s", first ? "[gMASK]<sop>" : "", sp, user, think_pre(c));
                 else snprintf(out, n, "%s%s<|user|>\n%s<|assistant|>", first ? "[gMASK]<sop>" : "", sp, user);
                 break;
        default: snprintf(out, n, "%s%sUser: %s\nAssistant:", first ? "" : "\n", sp, user); break;
    }
    free(ut); free(st);
}

static int session(Model *m, const char *sys0, int max_tokens, float temp) {
    const Config *c = &m->cfg;
    const Tokenizer *tk = &m->tk;
    static char line[16384], text[24576], sys[3800];
    Buf tr = {0}, reply = {0};
    int pos = 0, first = 1;
    srand((unsigned)time(NULL));
    console_init();
    if (sys0) snprintf(sys, sizeof sys, "%s", sys0);
    fprintf(stderr, "%sChat started. /help lists the commands; Ctrl+C interrupts the answer.%s\n", CE(C_DIM), CE(C_RESET));
    for (;;) {
        prompt_user();
        if (!read_line(line, sizeof line)) break;
        if (line[0] == 0) continue;
        cmd_norm(line, sizeof line);
        int reset = 0;
        const int cmd = console_cmd(line, &tr, sys, sizeof sys, &reset);
        if (cmd == 2) break;
        if (reset) { pos = 0; first = 1; }
        if (cmd) continue;

        Ids ids = {0};
        for (int attempt = 0; attempt < 2; attempt++) {
            ids.n = 0;
            if (first && c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
            turn_text(c, text, sizeof text, sys, line, first);
            encode(tk, text, &ids);
            if (pos + ids.n + 64 < c->ctx) break;
            if (first) { fprintf(stderr, "[mensagem maior que o contexto]\n"); ids.n = 0; break; }
            fprintf(stderr, "[contexto cheio (%d de %d tokens): conversa recomecada]\n", pos, c->ctx);
            pos = 0; first = 1;
        }
        if (ids.n == 0) { free(ids.ids); continue; }

        g_stop = 0; g_busy = 1;
        const double t0 = now_sec();
        float *lg = forward_seq(m, ids.ids, ids.n, pos, 1);
        pos += ids.n;
        first = 0;
        const double t1 = now_sec();
        buf_clear(&reply);
        const int gen = gen_console(m, lg, &pos, max_tokens, temp, -1, &reply);
        const double t2 = now_sec();
        g_busy = 0;
        printf("\n"); fflush(stdout);
        stats_line(ids.n, t1 - t0, gen, t2 - t1, pos, c->ctx);
        if (getenv("KATANA_UI")) { printf("\x01" "TOKENS %d %d\n", pos, c->ctx); fflush(stdout); }
        buf_str(&tr, "Voce: "); buf_str(&tr, line);
        buf_str(&tr, "\nAssistente: "); buf_str(&tr, reply.p ? reply.p : "");
        buf_str(&tr, "\n\n");
        free(ids.ids);
    }
    moe_report();
    heat_save(m);
    return 0;
}

