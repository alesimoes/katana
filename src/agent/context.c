/* ---------- compactacao do contexto do agente (formato ChatML) ----------
 * O agente guarda as mensagens que ja estao no cache KV (role 'u' usuario, 'a' assistente, 't' resultados de
 * ferramenta, que vao como mensagem do usuario). Perto do fim do contexto, em camadas: (1) troca o corpo dos
 * resultados de ferramenta antigos por um aviso curto; (2) resume os turnos antigos com o proprio modelo;
 * (3) encolhe tambem os resultados do turno atual (menos o ultimo). Depois recalcula o cache do zero
 * (o cache KV e linear e o DeltaNet so continua do fim), aproveitando o prefixo de sistema do disco. */
typedef struct { char role; char *s; } AMsg;
typedef struct { AMsg *v; int n, cap; } AHist;
static int g_ah_g4;       /* 1: historico no formato Gemma 4 (turnos <|turn>role ... <turn|>) */
static void ah_push(AHist *h, char role, const char *s, size_t n) {
    if (h->n == h->cap) { h->cap = h->cap * 2 + 16; h->v = realloc(h->v, sizeof(AMsg) * (size_t)h->cap); if (!h->v) die("sem memoria"); }
    char *p = xalloc(n + 1, 1); memcpy(p, s, n); p[n] = 0;
    h->v[h->n].role = role; h->v[h->n].s = p; h->n++;
}
static void ah_clear(AHist *h) { for (int i = 0; i < h->n; i++) free(h->v[i].s); h->n = 0; }
/* texto ChatML das mensagens; termina no fim da ultima, sem o fecho (como o cache KV) */
static void ah_render(const AHist *h, const char *sys, Buf *o) {
    buf_clear(o);
    if (g_ah_g4) {      /* gemma4: mensagens ja no texto exato do cache (u: so o pedido; a: resposta; t: resposta de ferramenta) */
        if (sys[0] || g4_think()) { buf_str(o, "<|turn>system\n"); if (g4_think()) buf_str(o, "<|think|>\n"); buf_str(o, sys); buf_str(o, "<turn|>\n"); }
        for (int i = 0; i < h->n; i++) {
            const char r = h->v[i].role;
            if (r == 'u') {
                if (i) buf_str(o, "<turn|>\n");
                buf_str(o, "<|turn>user\n"); buf_str(o, h->v[i].s); buf_str(o, "<turn|>\n<|turn>model\n"); buf_str(o, g4_answer_pre());
            } else {
                buf_str(o, h->v[i].s);
                if (r == 't' && g4_think()) buf_str(o, "<|channel>thought\n");
            }
        }
        return;
    }
    if (sys[0]) { buf_str(o, tf_sys()); buf_str(o, sys); buf_str(o, tf_end()); }     /* sem prompt de sistema: sem o bloco (o agente sempre tem um) */
    else if (g_glm) buf_str(o, "[gMASK]<sop>");
    for (int i = 0; i < h->n; i++) {
        if (i) buf_str(o, tf_end());
        buf_str(o, h->v[i].role == 'a' ? tf_asst() : h->v[i].role == 't' ? tf_tool() : tf_user());
        buf_str(o, h->v[i].s);
    }
}
static int ah_count(const Model *m, const char *t) {
    Ids ids = {0};
    if (m->cfg.add_bos && m->cfg.bos >= 0) ids_push(&ids, m->cfg.bos);
    encode(&m->tk, t, &ids);
    const int n = ids.n; free(ids.ids); return n;
}
/* troca o corpo de cada resultado de ferramenta da mensagem por "[resultado de X omitido: N bytes]"; 1 se mudou */
static int ah_shrink_tool_g4(AMsg *mg) {
    const char *K = "<|tool_response>response:", *E = "<tool_response|>";
    if (mg->role != 't' || !strstr(mg->s, K) || strstr(mg->s, "[resultado de ")) return 0;
    Buf o = {0};
    for (const char *q = strstr(mg->s, K); q; q = strstr(q, K)) {
        const char *nm = q + strlen(K);
        size_t nl = strcspn(nm, "{\n");
        const char *e = strstr(nm, E);
        const size_t nb = e ? (size_t)(e - q) : strlen(q);
        char t[320];
        snprintf(t, sizeof t, "<|tool_response>response:%.*s{value:" G4Q "[resultado de %.*s omitido para economizar contexto: %zu bytes]" G4Q "}<tool_response|>",
                 (int)(nl < 100 ? nl : 100), nm, (int)(nl < 100 ? nl : 100), nm, nb);
        buf_str(&o, t);
        q = e ? e + strlen(E) : q + strlen(q);
    }
    free(mg->s); mg->s = o.p ? o.p : xalloc(1, 1);
    return 1;
}
/* encolhe o corpo de chamadas de ferramenta grandes (codigo de write_file) numa mensagem do assistente ja executada */
static int ah_shrink_call(AMsg *mg);
/* tokens de contexto a partir dos quais o agente compacta sozinho: o prompt de sistema mais 6000 de conversa
   (fixo em 6000; o desempenho cai com o contexto cheio, entao resume cedo), no maximo 60% do contexto */
static int cp_trig(Model *m, const char *sys) {
    static size_t ls = (size_t)-1; static unsigned long lh; static int lt, lc;
    unsigned long h = 5381; for (const char *q = sys; *q; q++) h = h * 33 + (unsigned char)*q;
    if (strlen(sys) != ls || h != lh) { ls = strlen(sys); lh = h; lt = ah_count(m, sys); }
    const int base = 6000;      /* constante do motor (desempenho), nao e opcao do usuario */
    int t = lt + base;
    lc = m->cfg.ctx * 60 / 100;
    return t > lc ? lc : t;
}
static int ah_shrink_call(AMsg *mg) {
    if (g_ah_g4 || mg->role != 'a') return 0;
    const char *K = "<tool_call>", *E = "</tool_call>";
    int ch = 0;
    Buf o = {0};
    const char *q = mg->s;
    for (const char *a = strstr(q, K); a; a = strstr(q, K)) {
        const char *e = strstr(a, E);
        if (!e || (size_t)(e - a) < 1500 || strstr(a, "[... ")) { buf_add(&o, q, (size_t)(a - q) + strlen(K)); q = a + strlen(K); continue; }
        size_t keep = 300; while (keep && ((unsigned char)a[strlen(K) + keep] & 0xC0) == 0x80) keep--;
        buf_add(&o, q, (size_t)(a - q) + strlen(K) + keep);
        char t[128]; snprintf(t, sizeof t, "\n[... %zu bytes of this call omitted to save context; it was already executed ...]\n", (size_t)(e - a) - strlen(K) - keep);
        buf_str(&o, t);
        q = e; ch++;
    }
    if (!ch) { free(o.p); return 0; }
    buf_str(&o, q);
    free(mg->s); mg->s = o.p;
    return 1;
}
static int ah_shrink_tool(AMsg *mg) {
    if (g_ah_g4) return ah_shrink_tool_g4(mg);
    const char *K = "<tool_response>\n[resultado da ferramenta ";
    if (mg->role != 't' || !strstr(mg->s, K)) return 0;
    Buf o = {0};
    for (const char *q = strstr(mg->s, K); q; q = strstr(q, K)) {
        const char *nm = q + strlen(K);
        size_t nl = strcspn(nm, ",\n");
        const char *e = strstr(nm, "</tool_response>");
        const size_t nb = e ? (size_t)(e - q) : strlen(q);
        char t[256];
        snprintf(t, sizeof t, "%s<tool_response>\n[resultado de %.*s omitido para economizar contexto: %zu bytes]\n</tool_response>", o.n ? "\n" : "", (int)(nl < 100 ? nl : 100), nm, nb);
        buf_str(&o, t);
        q = e ? e + 16 : q + strlen(q);
    }
    free(mg->s); mg->s = o.p ? o.p : xalloc(1, 1);
    return 1;
}
/* acrescenta ate cap bytes de s em o, cortando numa fronteira de UTF-8 */
static void ah_cut(Buf *o, const char *s, size_t cap) {
    size_t n = strlen(s);
    if (n > cap) { n = cap; while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--; }
    buf_add(o, s, n);
    if (n < strlen(s)) buf_str(o, " [...]");
}
/* resume as mensagens [0, upto) com o proprio modelo (sem ferramentas, gulosa); devolve texto alocado ou NULL */
static char *ah_summary(Model *m, const AHist *h, int upto) {
    const Config *c = &m->cfg;
    const Tokenizer *tk = &m->tk;
    const int im_end = map_get(&tk->vocab, "<|im_end|>", 10);
    Buf tx = {0};
    for (int i = 0; i < upto; i++) {
        const char *s = h->v[i].s;
        if (h->v[i].role == 'a') {          /* sem o raciocinio */
            const char *e = strstr(s, g_ah_g4 ? "<channel|>" : "</think>");
            if (e) s = e + (g_ah_g4 ? 10 : 8);
            while (*s == '\n') s++;
            buf_str(&tx, "Assistente: ");
        } else buf_str(&tx, h->v[i].role == 't' ? "Resultados de ferramenta: " : "Usuario: ");
        ah_cut(&tx, s, h->v[i].role == 't' ? 1500 : 4000);
        buf_str(&tx, "\n\n");
    }
    if (!tx.p) return NULL;
    /* a transcricao precisa caber (cerca de 3 bytes por token): comeco e, principalmente, o fim */
    const long room = ((long)c->ctx - 900) * 3;
    if (room < 2000) { free(tx.p); return NULL; }
    if ((long)tx.n > room) {
        size_t a = (size_t)room / 4, b = (size_t)room - a;
        while (a && ((unsigned char)tx.p[a] & 0xC0) == 0x80) a--;
        size_t bs = tx.n - b;
        while (bs < tx.n && ((unsigned char)tx.p[bs] & 0xC0) == 0x80) bs++;
        Buf t2 = {0};
        buf_add(&t2, tx.p, a); buf_str(&t2, "\n[...]\n"); buf_add(&t2, tx.p + bs, tx.n - bs);
        free(tx.p); tx = t2;
    }
    Buf pr = {0};
    buf_str(&pr, g_ah_g4 ? "<|turn>system\nVoce resume conversas de forma fiel e curta.<turn|>\n<|turn>user\n"
                         : g_glm ? "[gMASK]<sop><|system|>Voce resume conversas de forma fiel e curta.<|user|>"
                         : "<|im_start|>system\nVoce resume conversas de forma fiel e curta.<|im_end|>\n<|im_start|>user\n");
    buf_str(&pr, "Resuma a conversa abaixo em no maximo 15 linhas, no idioma da conversa: o que o usuario quer, fatos e decisoes, "
                 "arquivos, comandos e ferramentas usados com os resultados importantes, e o que ainda falta fazer. Nao invente nada.\n\n<conversa>\n");
    buf_str(&pr, tx.p); free(tx.p);
    if (g_ah_g4) { buf_str(&pr, "</conversa><turn|>\n<|turn>model\n"); buf_str(&pr, g4_answer_pre()); }
    else {
        buf_str(&pr, "</conversa>"); buf_str(&pr, tf_end()); buf_str(&pr, tf_asst());
        buf_str(&pr, think_off(c));
    }
    Ids ids = {0};
    if (c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
    encode(tk, pr.p, &ids);
    free(pr.p);
    if (ids.n + 64 >= c->ctx) { free(ids.ids); return NULL; }
    float *lg = forward_seq(m, ids.ids, ids.n, 0, 1);
    int pos = ids.n;
    free(ids.ids);
    Buf out = {0};
    char tmp[256];
    for (int g = 0; g < 500 && pos < c->ctx - 1 && !g_stop; g++) {
        const int t = sample(lg, c->vocab, 0.0f);
        if (t == c->eos || t == im_end || (tk->toktype && tk->toktype[t] == 3)) break;
        const size_t tl = tok_append(tk, t, tmp, 0, sizeof tmp - 1);
        buf_add(&out, tmp, tl);
        lg = forward(m, t, pos++, 1);
    }
    if (!out.p) return NULL;
    char *s = out.p; while (*s == '\n' || *s == ' ') s++;
    size_t l = strlen(s); while (l && (s[l - 1] == '\n' || s[l - 1] == ' ')) l--;
    if (!l) { free(out.p); return NULL; }
    char *r = xalloc(l + 1, 1); memcpy(r, s, l); r[l] = 0;
    free(out.p);
    return r;
}
/* resumo "ao vivo": em vez de reler a conversa inteira num prompt novo (caro: o texto todo passa de novo pelo modelo),
 * aproveita o cache KV que ja guarda a conversa. Processa so o texto pendente (resultado da ultima ferramenta) com um
 * pedido de resumo no fim e gera o resumo dali. O cache fica sujo com esse pedido; quem chama recalcula em seguida.
 * So ChatML. Devolve texto alocado ou NULL (sem espaco, modelo chamou ferramenta, resposta vazia). */
#define CP_NOTE "[katana: context compaction] Pause the task for this one message and do NOT call any tool. Write a faithful summary of " \
    "the conversation so far, in the language of the conversation, in at most 25 lines: 1) what the user asked, keeping exact requirements, " \
    "names and numbers; 2) what is already done: every file created or changed, with its path and what it contains now, and the commands run " \
    "with their important results; 3) the last error or failing test, quoted exactly; 4) what is left to do, in order. Do not invent anything. " \
    "After this message the older messages are removed and you continue the task from your summary."
static char *ah_summary_live(Model *m, int pos, const char *pending) {
    const Config *c = &m->cfg;
    const Tokenizer *tk = &m->tk;
    const int im_end = map_get(&tk->vocab, "<|im_end|>", 10);
    char K[64]; snprintf(K, sizeof K, "%s%s", tf_end(), tf_asst());
    const char *cut = NULL;
    for (const char *q = strstr(pending, K); q; q = strstr(q + 1, K)) cut = q;      /* a abertura do turno do assistente, no fim */
    if (!cut) return NULL;
    Buf pr = {0};
    buf_add(&pr, pending, (size_t)(cut - pending));
    buf_str(&pr, "\n\n" CP_NOTE); buf_str(&pr, K);
    buf_str(&pr, think_off(c));
    Ids ids = {0};
    encode(tk, pr.p, &ids);
    free(pr.p);
    if (pos + ids.n + 700 >= c->ctx) { free(ids.ids); return NULL; }
    float *lg = forward_seq(m, ids.ids, ids.n, pos, 1);
    pos += ids.n;
    free(ids.ids);
    Buf out = {0};
    char tmp[256];
    for (int g = 0; g < 600 && pos < c->ctx - 1 && !g_stop; g++) {
        const int t = sample(lg, c->vocab, 0.0f);
        if (t == c->eos || t == im_end || (tk->toktype && tk->toktype[t] == 3)) break;
        const size_t tl = tok_append(tk, t, tmp, 0, sizeof tmp - 1);
        buf_add(&out, tmp, tl);
        lg = forward(m, t, pos++, 1);
    }
    if (!out.p) return NULL;
    char *sp = out.p; while (*sp == '\n' || *sp == ' ') sp++;
    size_t l = strlen(sp); while (l && (sp[l - 1] == '\n' || sp[l - 1] == ' ')) l--;
    if (l < 40 || strstr(sp, "<tool_call>")) { free(out.p); return NULL; }
    char *r = xalloc(l + 1, 1); memcpy(r, sp, l); r[l] = 0;
    free(out.p);
    return r;
}
#define CP_SUM_HEAD "[Summary of the conversation so far, written by you to save context; the older messages were removed]\n"
#define CP_REQ_HEAD "\n\n[The user's current request, still in progress. Continue from the summary and do not redo what is done]\n"
static int g_cp_nolive;    /* 1: o cache KV da conversa nao e confiavel (subagente nao devolveu): sem resumo ao vivo */
static int g_cp_floor;     /* depois de compactar, a proxima compactacao automatica so acima disto (evita recalcular a cada rodada) */

/* compacta o historico e recalcula o cache com historico + pending (texto do turno atual ainda fora do cache,
   que comeca pelo fecho da mensagem anterior). force: resume mesmo abaixo do alvo. Devolve o novo pos e os
   logits do fim em *lg_out, ou -1 se nem compactado cabe. */
static int agent_compact(Model *m, AHist *h, const char *sys, const char *pending, int before, int force, float **lg_out) {
    g_rd_epoch++;       /* notas de leitura que estavam no contexto podem sair: a proxima leitura do arquivo traz de volta */
    const Config *c = &m->cfg;
    const int trig = cp_trig(m, sys);
    const int sys_tok = ah_count(m, sys);
    const int target = sys_tok + (trig - sys_tok) * 55 / 100;   /* depois de resumir os turnos antigos, mira nesse tamanho */
    Buf r = {0};
    int last_u = -1;
    for (int i = h->n - 1; i >= 0; i--) if (h->v[i].role == 'u') { last_u = i; break; }
    if (last_u < 0) last_u = h->n;
    int shr = 0, summ = 0;
    for (int i = 0; i < last_u; i++) shr += ah_shrink_tool(&h->v[i]);          /* camada 1 */
    ah_render(h, sys, &r); buf_str(&r, pending);
    int n = ah_count(m, r.p);
    /* /compact (force, sem texto pendente): resume a conversa inteira, inclusive o ultimo turno; no automatico o
       ultimo pedido do usuario e o que veio depois ficam como estao */
    const int upto = (force && !*pending) ? h->n : last_u;
    /* camada 2 ao vivo (automatico, ChatML): o proprio modelo resume a conversa a partir do cache que ja existe; ficam o
       resumo, o pedido em andamento e a ultima chamada com o resultado pendente. Cobre o caso de um pedido so com
       dezenas de rodadas de ferramenta, em que nao ha turno antigo para resumir. */
    if (!force && !g_ah_g4 && !g_cp_nolive && n > target && *pending && last_u < h->n && h->n - last_u >= 4 && h->v[h->n - 1].role == 'a') {
        fprintf(stderr, "%s[compactando: o modelo resume o trabalho feito ate aqui...]%s\n", CE(C_DIM), CE(C_RESET));
        char *sm = ah_summary_live(m, before, pending);
        if (sm) {
            const char *req = h->v[last_u].s, *rq = strstr(req, CP_REQ_HEAD);
            if (rq) req = rq + strlen(CP_REQ_HEAD);                 /* pedido original dentro de um resumo anterior */
            AHist nh = {0};
            Buf u = {0};
            buf_str(&u, CP_SUM_HEAD); buf_str(&u, sm); if (g_todo[0]) { buf_str(&u, "\n\n[Your todo list]\n"); buf_str(&u, g_todo); } buf_str(&u, CP_REQ_HEAD); buf_str(&u, req);
            fprintf(stderr, "%s[resumo]\n%s%s\n", CE(C_DIM), sm, CE(C_RESET));
            ah_push(&nh, 'u', u.p, u.n);
            ah_push(&nh, 'a', h->v[h->n - 1].s, strlen(h->v[h->n - 1].s));
            ah_clear(h); free(h->v); *h = nh;
            free(u.p); free(sm);
            summ = 1;
            ah_render(h, sys, &r); buf_str(&r, pending);
            n = ah_count(m, r.p);
        }
    }
    if ((n > target || force) && upto > 0 && !summ) {                            /* camada 2 */
        fprintf(stderr, "%s[compactando: resumindo %d mensagens...]%s\n", CE(C_DIM), upto, CE(C_RESET));
        char *sm = ah_summary(m, h, upto);
        if (sm) {
            AHist nh = {0};
            Buf u = {0};
            buf_str(&u, "[Resumo da conversa anterior, feito para economizar contexto]\n"); buf_str(&u, sm); if (g_todo[0]) { buf_str(&u, "\n\n[Your todo list]\n"); buf_str(&u, g_todo); }
            fprintf(stderr, "%s[resumo]\n%s%s\n", CE(C_DIM), sm, CE(C_RESET));
            ah_push(&nh, 'u', u.p, u.n);
            ah_push(&nh, 'a', "Entendido. Sigo a partir do resumo.", 35);
            for (int i = upto; i < h->n; i++) ah_push(&nh, h->v[i].role, h->v[i].s, strlen(h->v[i].s));
            ah_clear(h); free(h->v); *h = nh;
            free(u.p); free(sm);
            summ = 1;
            ah_render(h, sys, &r); buf_str(&r, pending);
            n = ah_count(m, r.p);
        }
    }
    if (n + 64 >= trig) {                                                   /* camada 3: resultados e codigo do turno atual */
        int lt = -1;
        for (int i = h->n - 1; i >= 0; i--) if (h->v[i].role == 't') { lt = i; break; }
        for (int i = 0; i < h->n; i++) { if (i != lt || !*pending) shr += ah_shrink_tool(&h->v[i]); if (i < h->n - 1) shr += ah_shrink_call(&h->v[i]); }
        ah_render(h, sys, &r); buf_str(&r, pending);
        n = ah_count(m, r.p);
    }
    if (n + 64 >= c->ctx) { free(r.p); return -1; }
    Ids ids = {0};
    if (c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
    encode(&m->tk, r.p, &ids);
    free(r.p);
    const int np = pfx_prepare(m, &ids, sys);
    *lg_out = forward_seq(m, ids.ids + np, ids.n - np, np, 1);
    const int after = ids.n;
    free(ids.ids);
    g_cp_floor = after + 2000;
    fprintf(stderr, "%s[contexto compactado: %d -> %d tokens de %d (%d resultados de ferramenta encolhidos%s)]%s\n",
            CE(C_DIM), before, after, c->ctx, shr, summ ? ", turnos antigos resumidos" : "", CE(C_RESET));
    if (getenv("KATANA_UI")) { printf("\x01" "COMPACT %d %d\n", before, after); fflush(stdout); }
    return after;
}

