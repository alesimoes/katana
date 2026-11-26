/* ---------- o bloco `resume`: tres projecoes do estado do trabalho (docs/flow-resume.md secoes 7 e 9) ----------
 * A linha `next` (cada volta), o bloco `resume` (na reentrada) e a capsula (dentro de um resumo, ou no lugar de turnos velhos)
 * sao o mesmo estado em tres tamanhos. Rotulos fixos, em ordem fixa, uma linha cada; a linha que nao tem o que dizer some.
 * O bloco nunca diz o proximo passo quando ha fluxo aberto: a linha `next` vem depois e continua a ultima coisa que o modelo le. */
#define RZ_MAX_DEF 1200
static int rz_max(void) { const char *e = getenv("KATANA_RESUME_MAX"); int v = e && *e ? atoi(e) : RZ_MAX_DEF; return v < 400 ? (v ? 400 : RZ_MAX_DEF) : v > 3000 ? 3000 : v; }
static int rz_idle_min(void) { const char *e = getenv("KATANA_RESUME_IDLE_MIN"); const int v = e && *e ? atoi(e) : 30; return v > 0 ? v : 30; }
RZ_UNUSED static int rz_flag(const char *name) { const char *e = getenv(name); return e && atoi(e) > 0; }
static int rz_grace(void) { return g_rz.grace; }

static void rz_app(char *o, size_t cap, const char *fmt, ...) {
    const size_t n = strlen(o); if (n + 2 >= cap) return;
    va_list ap; va_start(ap, fmt); vsnprintf(o + n, cap - n, fmt, ap); va_end(ap);
}
static void rz_dur(long long s, char *o, size_t cap) {
    if (s < 60) snprintf(o, cap, "a moment");
    else if (s < 3600) snprintf(o, cap, "%lldm", s / 60);
    else snprintf(o, cap, "%lldh%02lldm", s / 3600, (s % 3600) / 60);
}
/* uma linha rotulada, cortada em max bytes sem partir um caractere */
static void rz_lab(char *o, size_t cap, const char *label, const char *text, size_t max) {
    if (!text[0]) return;
    char t[700]; fc_cut(t, max + 1 < sizeof t ? max + 1 : sizeof t, text);
    rz_app(o, cap, "%s%s\n", label, t);
}
/* o primeiro passo aberto do plano: numero, total e texto */
static int rz_plan_open(int *k, int *n, char *txt, size_t cap) {
    int total = 0, first = -1; txt[0] = 0;
    for (const char *p = g_rz.s.todo; *p; ) {
        const char *e = strchr(p, '\n'); const size_t l = e ? (size_t)(e - p) : strlen(p);
        if (first < 0 && strncmp(p, "[x]", 3) != 0 && l > 0) { first = total; const char *t = l > 3 && p[0] == '[' ? p + 3 : p; while (*t == ' ') t++; char tmp[200]; snprintf(tmp, sizeof tmp, "%.*s", (int)(l - (size_t)(t - p) < 190 ? l - (size_t)(t - p) : 190), t); fc_cut(txt, cap, tmp); }
        if (l > 0) total++;
        if (!e) break;
        p = e + 1;
    }
    *n = total; *k = first + 1;
    return first >= 0;
}
static int rz_has_plan(void) { int k, n; char t[8]; return rz_plan_open(&k, &n, t, sizeof t); }
static int rz_has_work(void) {
    const RzState *s = &g_rz.s;
    if (s->depth > 0 || rz_has_plan() || g_rz.ndelta > 0 || s->open_tool[0]) return 1;
    for (int i = 0; i < RZ_NJOB; i++) if (s->job[i].id && (!s->job[i].told || s->job[i].status == RZJ_RUN || s->job[i].status == RZJ_WAIT || s->job[i].status == RZJ_DETACHED)) return 1;
    return 0;
}
static const char *rz_evtext(int i) { const RzState *s = &g_rz.s; return s->ev[(s->nev == RZ_EVID ? s->ev_w + i : i) % RZ_EVID].text; }
/* o corpo. form: 0 completo, 1 curto (reentrada dentro do processo), 2 capsula (so o estavel). lvl: quanto ja foi cortado para caber */
static void rz_body_lvl(char *o, size_t cap, int form, int lvl) {
    const RzState *s = &g_rz.s; o[0] = 0;
    char b[700];
    const int ndone = lvl >= 3 ? 0 : 3 - lvl, ndel = lvl >= 4 ? 3 : 6, nrep = lvl >= 6 ? 1 : lvl >= 5 ? 2 : 3, dec = lvl < 7;
    if (form != 1) rz_lab(o, cap, "goal: ", s->goal[0] ? s->goal : s->last_req, 200);
    {   /* was: a pilha de cima para baixo, depois o passo aberto do plano */
        b[0] = 0;
        for (int i = s->depth - 1; i >= 0; i--) rz_app(b, sizeof b, "%s%s %d/%d %s", i == s->depth - 1 ? "" : ", over ", FL_BOOK[s->stk[i].id].name, s->stk[i].st + 1, FL_BOOK[s->stk[i].id].nst, FL_BOOK[s->stk[i].id].st[s->stk[i].st].name);
        int k, n; char pt[200];
        if (rz_plan_open(&k, &n, pt, sizeof pt)) rz_app(b, sizeof b, "%splan step %d of %d: %s", b[0] ? "; " : "", k, n, pt);
        rz_lab(o, cap, "was: ", b, 240);
    }
    if (form != 1) {      /* done: as tres mais novas que nao envelheceram; depois o que foi feito e nao mostrado */
        b[0] = 0; int got = 0;
        const int cnt = form == 2 ? 8 : ndone;
        for (int i = s->nev - 1; i >= 0 && got < cnt; i--) {
            const RzEvid *e = &s->ev[(s->nev == RZ_EVID ? s->ev_w + i : i) % RZ_EVID];
            if (e->stale) continue;
            rz_app(b, sizeof b, "%s%s", got ? "; " : "", e->text); got++;
        }
        (void)rz_evtext;
        for (int i = 0; i < s->nfile; i++) if (s->file[i].op == RZF_MADE && !s->file[i].shown) rz_app(b, sizeof b, "%smade, not shown yet: %s", b[0] ? "; " : "", s->file[i].path);
        rz_lab(o, cap, "done: ", b, form == 2 ? 700 : 300);
    }
    if (form == 2) {      /* arquivos editados */
        b[0] = 0; int got = 0;
        for (int i = 0; i < s->nfile; i++) if (s->file[i].op == RZF_WRITE) { rz_app(b, sizeof b, "%s%s", got ? "; " : "edited: ", s->file[i].path); got++; }
        rz_lab(o, cap, "files: ", b, 500);
    }
    if (form != 2) {      /* changed: as frases da reconciliacao */
        b[0] = 0;
        for (int i = 0; i < g_rz.ndelta && i < ndel; i++) rz_app(b, sizeof b, "%s%s", i ? "; " : "", g_rz.delta[i]);
        const int rest = (g_rz.ndelta > ndel ? g_rz.ndelta - ndel : 0) + g_rz.more_delta;
        if (rest > 0) rz_app(b, sizeof b, "; and %d more change%s", rest, rest > 1 ? "s" : "");
        rz_lab(o, cap, "changed: ", b, 400);
        /* running: tarefas vivas, ou "none" se houve tarefa e nenhuma roda */
        b[0] = 0; int any = 0, live = 0;
        for (int i = 0; i < RZ_NJOB; i++) {
            const RzJob *j = &s->job[i]; if (!j->id) continue;
            any = 1;
            if (j->status == RZJ_RUN || j->status == RZJ_DETACHED) { rz_app(b, sizeof b, "%stask %d (`%.60s`) running, output in %.100s", live ? "; " : "", j->id, j->cmd, j->log); live++; }
            else if (j->status == RZJ_WAIT) { rz_app(b, sizeof b, "%stask %d (`%.60s`) waits at \"%.80s\"", live ? "; " : "", j->id, j->cmd, j->asked); live++; }
        }
        if (!live && any) snprintf(b, sizeof b, "none");
        rz_lab(o, cap, "running: ", b, 240);
    }
    if (form != 1) {      /* do not repeat: as mortas, as mais repetidas primeiro */
        b[0] = 0; int used[RZ_ATT] = {0};
        for (int r = 0; r < nrep; r++) {
            int best = -1;
            for (int i = 0; i < s->natt; i++) if (s->att[i].dead && !used[i] && (best < 0 || s->att[i].n > s->att[best].n)) best = i;
            if (best < 0) break;
            used[best] = 1;
            const RzAttempt *a = &s->att[best];
            if (!strcmp(a->tool, a->subj) || !a->subj[0]) rz_app(b, sizeof b, "%s%s -> %s (x%d)", r ? "; " : "", a->tool, a->cause, a->n);
            else rz_app(b, sizeof b, "%s%s %s -> %s (x%d)", r ? "; " : "", a->tool, a->subj, a->cause, a->n);
        }
        rz_lab(o, cap, "do not repeat: ", b, 300);
    }
    if (form != 1 || s->ask[0]) {
        b[0] = 0;
        if (s->ask[0] && form != 2) snprintf(b, sizeof b, "\"%.150s\" -> waiting for an answer", s->ask);
        else if (dec && s->ndec) { const RzDecision *d = &s->dec[(s->ndec == RZ_DEC ? s->dec_w + s->ndec - 1 : s->ndec - 1) % RZ_DEC]; snprintf(b, sizeof b, "\"%.100s\" -> the user answered: %.80s", d->q, d->a); }
        rz_lab(o, cap, "asked: ", b, 200);
    }
    if (form != 2 && !s->depth) {      /* sem fluxo aberto, o bloco tem a sua propria ultima linha */
        int k, n; char pt[200];
        if (rz_plan_open(&k, &n, pt, sizeof pt)) rz_app(o, cap, "next: %s\n", pt);
        else rz_app(o, cap, "next: tell the user in two sentences where this work stands and ask how to go on\n");
    }
    size_t l = strlen(o); while (l && o[l - 1] == '\n') o[--l] = 0;
}
/* o corpo, cortado por etapas (a ordem da tabela de 7.2) ate caber em cap bytes */
static void rz_body(char *o, size_t cap, int form) {
    size_t limit = form == 2 ? 2500 : form == 1 ? 500 : (size_t)rz_max();
    if (limit >= cap) limit = cap - 1;
    char *tmp = (char *)malloc(cap < 4000 ? 4000 : cap);
    if (!tmp) { o[0] = 0; return; }
    for (int lvl = 0; lvl <= 8; lvl++) {
        rz_body_lvl(tmp, cap < 4000 ? 4000 : cap, form, lvl);
        if (strlen(tmp) <= limit || lvl == 8) break;
    }
    if (strlen(tmp) > limit) { char *c = tmp + limit; while (c > tmp && ((unsigned char)*c & 0xC0) == 0x80) c--; *c = 0; }
    rz_cpy(o, cap, tmp); free(tmp);
}
static void rz_reason_text(int r, char *o, size_t cap) {
    const long long now = (long long)time(NULL); char d[32];
    if (r == RZR_SESSION) { rz_dur(now - (g_rz.t_saved ? g_rz.t_saved : now), d, sizeof d); snprintf(o, cap, "session reopened after %s", d); }
    else if (r == RZR_INTERRUPT) snprintf(o, cap, "you were interrupted");
    else if (r == RZR_IDLE) { rz_dur(g_rz.s.t_clean ? now - g_rz.s.t_clean : 0, d, sizeof d); snprintf(o, cap, "back after %s", d); }
    else if (r == RZR_RECALL) snprintf(o, cap, "back to earlier work");
    else if (r == RZR_TASK) snprintf(o, cap, "a background task ended");
    else snprintf(o, cap, "state of the work");
}
/* o bloco do pedido que segue a reentrada; 0 = nada a dizer (sem trabalho, sem bloco). Gasta a reentrada pendente */
static size_t rz_block(char *out, size_t cap) {
    out[0] = 0;
    const int reason = g_rz.reason;
    if (!rz_on() || !reason || reason == RZR_SUMMARY) { if (reason == RZR_SUMMARY) g_rz.reason = 0; return 0; }
    g_rz.reason = 0;
    if (!rz_has_work()) return 0;
    char body[3200], why[64];
    rz_body(body, sizeof body, reason == RZR_INTERRUPT || reason == RZR_IDLE || reason == RZR_TASK ? 1 : 0);
    if (!body[0]) return 0;
    rz_reason_text(reason, why, sizeof why);
    const size_t n = KNOTE(RESUME, out, cap, why, body);
    if (n >= cap) { out[0] = 0; return 0; }
    g_rz.enter_reason = reason;
    g_rz.probe_pending = rz_flag("KATANA_RESUME_PROBE");
    rz_ev('N', RZ_RNAME[reason], rz_i((long long)n), NULL);
    fl_log("resume\tenter\t%s\t%d", RZ_RNAME[reason], (int)n);
    g_rz.score_left = 3; g_rz.sc_on = g_rz.sc_dead = g_rz.sc_dup = 0; g_rz.sc_reason = reason;
    return n;
}
/* a capsula dentro de um resumo (context.c, loop.c): so o estavel; 1 se acrescentou algo */
static int rz_capsule(Buf *o) {
    if (!rz_on() || g_ct_depth > 1) return 0;
    char body[3200]; rz_body(body, sizeof body, 2);
    if (!body[0]) return 0;
    buf_str(o, KNF_RESUME_CAPSULE); buf_str(o, body);
    return 1;
}
/* ---- reidratacao (KATANA_RESUME_REHYDRATE=1, desligada): ao retomar a sessao, o contexto vem do estado e nao de turnos velhos, sem chamar o modelo.
 * Tudo antes do ultimo pedido do usuario vira duas mensagens: a capsula (usuario) e "Understood" (assistente); o ultimo pedido e o que veio
 * depois ficam inteiros. A conversa inteira (full) nao e tocada: e a mesma classe de mudanca da compactacao, nao um desfazer. A capsula e
 * deterministica, entao a segunda retomada da mesma sessao acha o KV em disco com a mesma chave. ---- */
static int sess_msg_tokens(const Model *m, const AHist *h, const char *sys);      /* loop.c */
static void rz_rehydrate(const Model *m, AHist *hist, const char *sys) {
    if (!rz_on() || !g_rz.loaded || !rz_has_work()) return;
    int k = -1;
    for (int i = hist->n - 1; i >= 0; i--) if (hist->v[i].role == 'u') { k = i; break; }
    if (k <= 0) return;
    AHist pre = {0};
    for (int i = 0; i < k; i++) ah_push(&pre, hist->v[i].role, hist->v[i].s, strlen(hist->v[i].s));
    const int before = sess_msg_tokens(m, &pre, sys);
    ah_clear(&pre); free(pre.v);
    if (before <= 600) return;
    char body[3200]; rz_body(body, sizeof body, 2);
    if (!body[0]) return;
    Buf u = {0};
    buf_str(&u, KNF_RESUME_CAPSULE + 2); buf_str(&u, body);
    AHist nh = {0};
    ah_push(&nh, 'u', u.p, u.n); ah_push(&nh, 'a', "Understood. I continue from this state.", 39);
    free(u.p);
    for (int i = k; i < hist->n; i++) ah_push(&nh, hist->v[i].role, hist->v[i].s, strlen(hist->v[i].s));
    if (!g_ah_g4 && sess_msg_tokens(m, &nh, sys) > 3000) ah_mask(&nh, 0, 6000, 0);      /* o que sai fica em disco e volta com o Read, como em sess_compact */
    fl_log("resume\trehydrate\t%d messages -> %d", hist->n, nh.n);
    ah_clear(hist); free(hist->v); *hist = nh;
}
/* depois de uma compactacao que escreveu resumo: a proxima linha `next` sai de novo e o estado anota (loop.c) */
static void rz_after_compact(void) {
    if (!g_cp_summed) return;
    g_cp_summed = 0;
    g_fl.emitted_st = -1;
    g_rz.enter_reason = RZR_SUMMARY;
    if (rz_on() && g_ct_depth <= 1) rz_ev('N', "summary", "0", NULL);
}

/* ---------- katana flow state [arquivo]: o arquivo de estado impresso como bloco e como capsula (sem modelo) ---------- */
static int rz_cmd_state(const char *file) {
    memset(&g_rz, 0, sizeof g_rz);
    if (!rz_replay(file)) { fprintf(stderr, "flow state: no state in %s\n", file); return 1; }
    char body[3200];
    g_rz.t_saved = g_rz.s.t_event;
    rz_body(body, sizeof body, 0); printf("--- block (full form)\n%s\n", body);
    rz_body(body, sizeof body, 2); printf("--- capsule\n%s\n", body);
    return 0;
}
/* simulador (katana flow sim): R = reinicio; guarda o estado, limpa a memoria, le de volta e reconcilia */
static void rz_sim_begin(void) {
    char p[200]; snprintf(p, sizeof p, "/tmp/katana-flow-sim-%ld.kstate", (long)getpid());
    remove(p);
    if (!getenv("KATANA_RESUME_FILE")) setenv("KATANA_RESUME_FILE", p, 1);
    rz_close(); rz_open("", 0);
}
static void rz_sim_restart(void) {
    rz_checkpoint();
    rz_release(); memset(&g_rz.s, 0, sizeof g_rz.s);
    flow_reset();
    if (rz_open("", 0)) { g_rz.reason = RZR_SESSION; g_rz.grace = 1; }
}
static void rz_sim_end(void) {
    const char *p = getenv("KATANA_RESUME_FILE");
    rz_close();
    if (p && !strncmp(p, "/tmp/katana-flow-sim-", 21)) { char q[260]; remove(p); snprintf(q, sizeof q, "%.200s.lock", p); remove(q); unsetenv("KATANA_RESUME_FILE"); }
}

/* katana flow resume-report [--days N]: as linhas `resume` do registro de fluxos, somadas por motivo (o que o modelo fez depois do bloco) */
static int rz_cmd_report(int argc, char **argv) {
    int days = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--days") && i + 1 < argc) days = atoi(argv[++i]);
    char h[1024], p[1200]; agent_home(h, sizeof h); snprintf(p, sizeof p, "%s/.katana/flow.log", h);
    FILE *f = fopen(p, "rb"); if (!f) { fprintf(stderr, "resume-report: no %s\n", p); return 1; }
    struct { int enter, score, calls, on_flow, dead, dup, answered; long bytes; } t[7]; memset(t, 0, sizeof t);
    const time_t cut = days > 0 ? time(NULL) - (time_t)days * 86400 : 0;
    static char ln[2000];
    while (fgets(ln, sizeof ln, f)) {
        char *tab[12]; int nt = 0; ln[strcspn(ln, "\r\n")] = 0;
        for (char *q = ln; nt < 12; ) { tab[nt++] = q; char *e = strchr(q, '\t'); if (!e) break; *e = 0; q = e + 1; }
        if (nt < 5 || strcmp(tab[2], "resume")) continue;
        if (cut) { struct tm tm; memset(&tm, 0, sizeof tm); if (sscanf(tab[0], "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) { tm.tm_year -= 1900; tm.tm_mon--; tm.tm_isdst = -1; if (mktime(&tm) < cut) continue; } }
        int r = 0; for (int i = 1; i < 7; i++) if (!strcmp(tab[4], RZ_RNAME[i])) r = i;
        if (!r) continue;
        if (!strcmp(tab[3], "enter")) { t[r].enter++; if (nt > 5) t[r].bytes += atol(tab[5]); }
        else if (!strcmp(tab[3], "score") && nt >= 10) {
            t[r].score++;
            t[r].calls += atoi(strchr(tab[5], '=') ? strchr(tab[5], '=') + 1 : "0"); t[r].on_flow += atoi(strchr(tab[6], '=') ? strchr(tab[6], '=') + 1 : "0");
            t[r].dead += atoi(strchr(tab[7], '=') ? strchr(tab[7], '=') + 1 : "0"); t[r].dup += atoi(strchr(tab[8], '=') ? strchr(tab[8], '=') + 1 : "0");
            t[r].answered += atoi(strchr(tab[9], '=') ? strchr(tab[9], '=') + 1 : "0");
        }
    }
    fclose(f);
    printf("reason     blocks  bytes  scored  calls  on_flow  dead  dup  answered\n");
    for (int r = 1; r < 7; r++) if (t[r].enter || t[r].score) printf("%-10s %6d %6ld %7d %6d %8d %5d %4d %9d\n", RZ_RNAME[r], t[r].enter, t[r].bytes, t[r].score, t[r].calls, t[r].on_flow, t[r].dead, t[r].dup, t[r].answered);
    return 0;
}

/* ---- sonda (KATANA_RESUME_PROBE=1, desligada; precisa tambem de KATANA_FLOW_RERANK=1): na primeira chamada depois da reentrada, o motor pergunta
 * ao proprio modelo "Which tool comes next?" com os candidatos do estado e alguns distratores (as ferramentas das tentativas mortas e da lista
 * `off`), por fm_probe_rank (flowmem.c): le os logitos, nao gera texto, nao deixa rastro na conversa. O topo esperado: orientado; um distrator:
 * desvio (uma clausula diz o passo do estado); margem pequena: sem sinal, nada muda. Custa o que docs/flows.md secao 4 diz da sonda. ---- */
static size_t rz_probe_clause(char *out, size_t cap) {
    out[0] = 0;
    if (!g_rz.probe_pending) return 0;
    g_rz.probe_pending = 0;
    const FlowPos *t = fl_top();
    if (!rz_on() || !rz_flag("KATANA_RESUME_PROBE") || !t || cap < 300) return 0;
    if (!fm_rerank_on()) { fl_log("resume\tprobe\tunavailable (KATANA_FLOW_RERANK is off)"); return 0; }
    const char *ex = FL_BOOK[t->id].st[t->st].expect;
    if (!ex) return 0;      /* qualquer ferramenta serve: nada a conferir */
    static char nm[10][40]; const char *item[10]; int n = 0;
    if (!ex[0]) snprintf(nm[n++], sizeof nm[0], "answer the user");
    else for (const char *p = ex; *p && n < 4; ) { const size_t l = strcspn(p, "|"); snprintf(nm[n++], sizeof nm[0], "%.*s", (int)(l < 39 ? l : 39), p); p += l; if (*p) p++; }
    const int nexp = n;
    for (int i = 0; i < g_rz.s.natt && n < nexp + 3; i++) {      /* distratores: ferramentas de tentativas mortas, depois a lista off */
        if (!g_rz.s.att[i].dead) continue;
        int dup = 0; for (int k = 0; k < n; k++) if (!strcmp(nm[k], g_rz.s.att[i].tool)) dup = 1;
        if (!dup) snprintf(nm[n++], sizeof nm[0], "%.39s", g_rz.s.att[i].tool);
    }
    const char *off = FL_BOOK[t->id].off;
    for (const char *p = off; p && *p && n < nexp + 3; ) {
        const size_t l = strcspn(p, "|"); char one[40]; snprintf(one, sizeof one, "%.*s", (int)(l < 39 ? l : 39), p); p += l; if (*p) p++;
        int dup = 0; for (int k = 0; k < n; k++) if (!strcmp(nm[k], one)) dup = 1;
        if (!dup) snprintf(nm[n++], sizeof nm[0], "%s", one);
    }
    if (n < 2) return 0;
    for (int i = 0; i < n; i++) item[i] = nm[i];
    int order[10];
    if (!fm_probe_rank("Which tool comes next?", item, n, order)) { fl_log("resume\tprobe\tnosignal"); return 0; }
    if (order[0] < nexp) { fl_log("resume\tprobe\toriented"); return 0; }
    fl_log("resume\tprobe\tdrift\t%s", item[order[0]]);
    char nx[600]; fl_next_text(nx, sizeof nx);
    return (size_t)snprintf(out, cap, "orientation check: the work stands at %s %d/%d %s (%.300s). ", FL_BOOK[t->id].name, t->st + 1, FL_BOOK[t->id].nst, FL_BOOK[t->id].st[t->st].name, nx);
}
