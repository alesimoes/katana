/* ---------- ferramentas CronCreate, CronDelete, CronList, ScheduleWakeup (prompts agendados, so durante a sessao) ----------
 * O modelo agenda um prompt; quando a hora chega e o agente esta ocioso (esperando o usuario), o prompt e entregue ao
 * modelo como se fosse uma mensagem nova do usuario, marcada como vinda do agendamento. Nada persiste depois da sessao
 * e nada sai da maquina. Horas locais (TZ da maquina); o relogio vem de ct_sched_now() (KATANA_SCHED_NOW, em segundos
 * unix, torna os testes deterministas).
 *
 * Lado da entrega (o laco do agente chama; src/agent/loop.c nao e editado aqui):
 *   static int ct_sched_due(double now, char *prompt, size_t cap, char *label, size_t lcap);
 *       devolve 1 e preenche prompt e label ("cron <id>" ou "wakeup") de UMA tarefa vencida (a mais antiga),
 *       avancando-a (recorrente: proximo horario a partir de now) ou removendo-a (unica, wakeup, recorrente
 *       vencida pelos 7 dias). 0 se nada venceu. Tarefa que passou da hora com o agente ocupado dispara uma vez
 *       so (sem recuperar execucoes perdidas). Chamar de novo para a proxima tarefa vencida.
 *   static double ct_sched_next(double now);
 *       segundos ate a proxima tarefa (0 se ja venceu), ou numero negativo se nao ha nenhuma: serve de tempo maximo
 *       de espera pela linha do usuario.
 *   static void ct_sched_reset(void);   sessao nova: apaga tudo.
 *   static double ct_sched_now(void);   relogio (unix, fracionario).
 * Gancho de teste: KATANA_SCHED_TEST=<unix> faz CronList anexar o resultado de chamar ct_sched_due repetidamente nesse
 * instante (uma linha "delivered [label] prompt" por tarefa entregue, ou "nothing due"). */

#define CT_SCHED_MAX 50
#define CT_SCHED_EXPIRE (7 * 86400)
typedef struct {
    char id[9]; char expr[336]; char *prompt;
    unsigned long long min; unsigned hour, dom, mon, dow;     /* conjuntos em bits */
    int dom_star, dow_star, recurring;
    double created, next;
} CtCron;
static CtCron g_ct_cron[CT_SCHED_MAX];
static int g_ct_cron_n;
static struct { int on; double at; char *prompt; } g_ct_wake;

static double ct_sched_now(void) {
    const char *e = getenv("KATANA_SCHED_NOW");
    if (e && *e) return atof(e);
    return (double)time(NULL);
}
static void ct_sc_lt(time_t t, struct tm *tm) {
#ifdef _WIN32
    localtime_s(tm, &t);
#else
    localtime_r(&t, tm);
#endif
}
static void ct_sc_fmt(double t, char *b, size_t n) {
    struct tm tm; ct_sc_lt((time_t)t, &tm);
    if (!strftime(b, n, "%Y-%m-%d %H:%M %Z", &tm)) snprintf(b, n, "?");
}
static void ct_sc_app(char *out, size_t cap, const char *fmt, ...) {
    size_t l = strlen(out);
    if (l + 1 >= cap) return;
    va_list ap; va_start(ap, fmt); vsnprintf(out + l, cap - l, fmt, ap); va_end(ap);
}
static int ct_sc_num(const char **q) {
    int v = 0;
    while (isdigit((unsigned char)**q)) { if (v < 100000) v = v * 10 + (**q - '0'); (*q)++; }
    return v;
}

/* um campo do cron: listas, intervalos, passos. 0 ok; senao escreve o motivo em why */
static int ct_sc_field(const char *s, int lo, int hi, int dow, unsigned long long *set, char *why, size_t wn) {
    *set = 0;
    const char *p = s;
    for (;;) {
        int a, b, step = 1;
        const char *q = p;
        if (*q == '*') { a = lo; b = dow ? 6 : hi; q++; }
        else {
            if (!isdigit((unsigned char)*q)) { snprintf(why, wn, *q ? "unexpected '%c'" : "empty item", *q); return 1; }
            a = ct_sc_num(&q); b = a;
            if (*q == '-') {
                q++;
                if (!isdigit((unsigned char)*q)) { snprintf(why, wn, "incomplete range"); return 1; }
                b = ct_sc_num(&q);
            }
        }
        if (*q == '/') {
            q++;
            if (!isdigit((unsigned char)*q)) { snprintf(why, wn, "incomplete step"); return 1; }
            step = ct_sc_num(&q);
            if (step < 1) { snprintf(why, wn, "step must be at least 1"); return 1; }
        }
        if (*q && *q != ',') { snprintf(why, wn, "unexpected '%c'", *q); return 1; }
        if (a < lo || b > hi || a > b) { snprintf(why, wn, "value out of range %d-%d", lo, hi); return 1; }
        for (int v = a; v <= b; v += step) *set |= 1ULL << (dow && v == 7 ? 0 : v);
        if (*q == 0) break;
        p = q + 1;
    }
    return 0;
}
/* 0 ok; senao erro pronto para o modelo em err */
static int ct_sc_parse(const char *expr, CtCron *c, char *err, size_t en) {
    static const char *names[5] = { "minute", "hour", "day-of-month", "month", "day-of-week" };
    static const int lo[5] = { 0, 0, 1, 1, 0 }, hi[5] = { 59, 23, 31, 12, 7 };
    char f[5][64]; int n = 0; const char *p = expr;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        size_t l = strcspn(p, " \t");
        if (n == 5) { n = 6; break; }
        if (l >= sizeof f[0]) { snprintf(err, en, "error: invalid cron expression: %s field is too long", names[n]); return 1; }
        memcpy(f[n], p, l); f[n][l] = 0; n++; p += l;
    }
    if (n != 5) { snprintf(err, en, "error: invalid cron expression '%s': expected exactly 5 fields (minute hour day-of-month month day-of-week), found %s", expr, n > 5 ? "more" : n == 0 ? "none" : n == 1 ? "1" : n == 2 ? "2" : n == 3 ? "3" : "4"); return 1; }
    unsigned long long s[5]; char why[96];
    for (int i = 0; i < 5; i++)
        if (ct_sc_field(f[i], lo[i], hi[i], i == 4, &s[i], why, sizeof why)) {
            snprintf(err, en, "error: invalid cron expression: bad %s field '%s' (%s; allowed %d-%d, with *, lists a,b, ranges a-b and steps */n)", names[i], f[i], why, lo[i], hi[i]);
            return 1;
        }
    c->min = s[0]; c->hour = (unsigned)s[1]; c->dom = (unsigned)s[2]; c->mon = (unsigned)s[3]; c->dow = (unsigned)s[4];
    c->dom_star = f[2][0] == '*'; c->dow_star = f[4][0] == '*';
    snprintf(c->expr, sizeof c->expr, "%s %s %s %s %s", f[0], f[1], f[2], f[3], f[4]);
    return 0;
}
static int ct_sc_day_ok(const CtCron *c, const struct tm *tm) {
    if (!((c->mon >> (tm->tm_mon + 1)) & 1)) return 0;
    const int d = (c->dom >> tm->tm_mday) & 1, w = (c->dow >> tm->tm_wday) & 1;
    if (c->dom_star || c->dow_star) return d && w;
    return d || w;                  /* os dois restritos: qualquer um serve */
}
/* primeiro minuto estritamente depois de after que casa; 0 se nao houver em 5 anos. Pula dia e hora que nao casam. */
static double ct_sc_next(const CtCron *c, double after) {
    time_t t = (time_t)floor(after / 60.0) * 60 + 60;
    const time_t lim = t + (time_t)5 * 366 * 86400;
    while (t < lim) {
        struct tm tm; ct_sc_lt(t, &tm);
        if (!ct_sc_day_ok(c, &tm)) {
            tm.tm_mday++; tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0; tm.tm_isdst = -1;
            const time_t n = mktime(&tm);
            t = n > t ? n : t + 3600;
        } else if (!((c->hour >> tm.tm_hour) & 1)) {
            t += 3600 - (time_t)(tm.tm_min * 60 + tm.tm_sec);
        } else if (!((c->min >> tm.tm_min) & 1)) {
            t += 60;
        } else return (double)t;
    }
    return 0;
}
static void ct_sc_free(int i) {
    free(g_ct_cron[i].prompt);
    memmove(&g_ct_cron[i], &g_ct_cron[i + 1], sizeof(CtCron) * (size_t)(g_ct_cron_n - i - 1));
    g_ct_cron_n--;
}
static void ct_sc_wake_clear(void) { free(g_ct_wake.prompt); g_ct_wake.prompt = NULL; g_ct_wake.on = 0; }
static void ct_sched_reset(void) {
    while (g_ct_cron_n) ct_sc_free(0);
    ct_sc_wake_clear();
}
static void ct_sc_expire(double now) {
    for (int i = g_ct_cron_n - 1; i >= 0; i--)
        if (g_ct_cron[i].recurring && now >= g_ct_cron[i].created + CT_SCHED_EXPIRE) ct_sc_free(i);
}
static void ct_sc_copy(char *d, size_t n, const char *s) { if (n) snprintf(d, n, "%s", s ? s : ""); }

static int ct_sched_due(double now, char *prompt, size_t cap, char *label, size_t lcap) {
    ct_sc_expire(now);
    int best = -1; double bt = 0;
    for (int i = 0; i < g_ct_cron_n; i++)
        if (g_ct_cron[i].next <= now && (best < 0 || g_ct_cron[i].next < bt)) { best = i; bt = g_ct_cron[i].next; }
    if (g_ct_wake.on && g_ct_wake.at <= now && (best < 0 || g_ct_wake.at < bt)) {
        ct_sc_copy(prompt, cap, g_ct_wake.prompt); ct_sc_copy(label, lcap, "wakeup");
        ct_sc_wake_clear();
        return 1;
    }
    if (best < 0) return 0;
    CtCron *c = &g_ct_cron[best];
    ct_sc_copy(prompt, cap, c->prompt);
    char lb[32]; snprintf(lb, sizeof lb, "cron %s", c->id); ct_sc_copy(label, lcap, lb);
    if (c->recurring) {
        const double nx = ct_sc_next(c, now);
        if (nx > 0) c->next = nx; else ct_sc_free(best);
    } else ct_sc_free(best);
    return 1;
}
static double ct_sched_next(double now) {
    double m = -1;
    for (int i = 0; i < g_ct_cron_n; i++) if (m < 0 || g_ct_cron[i].next < m) m = g_ct_cron[i].next;
    if (g_ct_wake.on && (m < 0 || g_ct_wake.at < m)) m = g_ct_wake.at;
    if (m < 0) return -1;
    return m > now ? m - now : 0;
}

/* ---- CronCreate ---- */
static const char CT_CRONCREATE_DESC[] =
    "Schedule a prompt to be delivered to you later, as if the user had typed it (marked as coming from the schedule). "
    "The schedule only lives in this session: nothing persists after it ends. `cron` is a standard 5-field expression "
    "(minute hour day-of-month month day-of-week) in the machine's LOCAL time, supporting *, lists (1,15), ranges (1-5), "
    "steps (*/10, 0-30/5) and day-of-week 0-7 (0 and 7 are Sunday). When both day-of-month and day-of-week are restricted, "
    "either may match. `recurring` defaults to true; with false the task fires once at the next match and is removed. "
    "At most 50 tasks; recurring tasks expire automatically 7 days after creation. Returns the task id and next fire time. "
    "Prompts are delivered only while the agent is idle, so a late prompt fires once, never several times.";
static const char CT_CRONCREATE_PROPS[] =
    "\"cron\": {\"type\": \"string\", \"description\": \"5-field cron expression in local time, e.g. \\\"*/5 * * * *\\\" or \\\"30 14 * * 1-5\\\".\"}, "
    "\"prompt\": {\"type\": \"string\", \"description\": \"The prompt to deliver each time the task fires.\"}, "
    "\"recurring\": {\"type\": \"boolean\", \"description\": \"true (default) fires on every match until deleted or expired after 7 days; false fires once and is removed.\"}";
static const char CT_CRONCREATE_REQ[] = "\"cron\", \"prompt\"";
static void ct_sc_newid(char *id) {
    static unsigned long long st;
    for (;;) {
        if (!st) {      /* com KATANA_SCHED_NOW (testes) a sequencia de ids e sempre a mesma; senao depende do relogio e do processo */
            const char *fx = getenv("KATANA_SCHED_NOW");
            st = (fx && *fx) ? 88172645463325252ULL ^ (unsigned long long)atof(fx)
                : ((unsigned long long)time(NULL) * 6364136223846793005ULL) ^ (unsigned long long)clock() ^ (unsigned long long)(size_t)&st ^ 88172645463325252ULL;
            if (!st) st = 1;
        }
        st ^= st << 13; st ^= st >> 7; st ^= st << 17;
        snprintf(id, 9, "%08x", (unsigned)(st >> 16));
        int dup = 0;
        for (int i = 0; i < g_ct_cron_n; i++) if (strcmp(g_ct_cron[i].id, id) == 0) dup = 1;
        if (!dup) return;
    }
}
static void ct_croncreate(const char *js, const char *end, char *out, size_t cap) {
    out[0] = 0;
    char *expr = ct_str(js, end, "cron"), *prompt = ct_str(js, end, "prompt");
    if (!expr || !*expr) { snprintf(out, cap, "error: CronCreate needs `cron` (5 fields: minute hour day-of-month month day-of-week)"); goto done; }
    if (!prompt || !*prompt) { snprintf(out, cap, "error: CronCreate needs a non-empty `prompt`"); goto done; }
    const int rec = ct_has(js, end, "recurring") ? ct_bool(js, end, "recurring") : 1;
    const double now = ct_sched_now();
    if (g_ct_cron_n >= CT_SCHED_MAX) { snprintf(out, cap, "error: too many scheduled tasks (maximum %d); use CronList and CronDelete to remove some", CT_SCHED_MAX); goto done; }
    CtCron c; memset(&c, 0, sizeof c);
    if (ct_sc_parse(expr, &c, out, cap)) goto done;
    c.next = ct_sc_next(&c, now);
    if (c.next <= 0) { snprintf(out, cap, "error: cron expression '%s' never matches a real date within 5 years (for example February 30); fix the day-of-month and month fields", c.expr); goto done; }
    c.recurring = rec; c.created = now; c.prompt = prompt; prompt = NULL;
    ct_sc_newid(c.id);
    g_ct_cron[g_ct_cron_n++] = c;
    char nb[48], eb[48]; ct_sc_fmt(c.next, nb, sizeof nb); ct_sc_fmt(now + CT_SCHED_EXPIRE, eb, sizeof eb);
    if (rec) snprintf(out, cap, "Scheduled recurring task %s (%s). Next fire: %s. Recurring tasks expire automatically 7 days after creation (%s).", c.id, c.expr, nb, eb);
    else snprintf(out, cap, "Scheduled one-shot task %s (%s). It fires once at %s and is then removed.", c.id, c.expr, nb);
done:
    free(expr); free(prompt);
}

/* ---- CronDelete ---- */
static const char CT_CRONDELETE_DESC[] = "Remove a scheduled task created with CronCreate, by its id (see CronList).";
static const char CT_CRONDELETE_PROPS[] = "\"id\": {\"type\": \"string\", \"description\": \"The task id returned by CronCreate.\"}";
static const char CT_CRONDELETE_REQ[] = "\"id\"";
static void ct_crondelete(const char *js, const char *end, char *out, size_t cap) {
    out[0] = 0;
    char *id = ct_str(js, end, "id");
    if (!id || !*id) { snprintf(out, cap, "error: CronDelete needs `id`"); free(id); return; }
    for (int i = 0; i < g_ct_cron_n; i++)
        if (strcmp(g_ct_cron[i].id, id) == 0) { ct_sc_free(i); snprintf(out, cap, "Deleted scheduled task %s", id); free(id); return; }
    snprintf(out, cap, "error: no scheduled task with id '%s'. Existing ids:", id);
    if (!g_ct_cron_n) ct_sc_app(out, cap, " (none)");
    for (int i = 0; i < g_ct_cron_n; i++) ct_sc_app(out, cap, "%s%s", i ? ", " : " ", g_ct_cron[i].id);
    free(id);
}

/* ---- CronList ---- */
static const char CT_CRONLIST_DESC[] = "List the scheduled tasks of this session: id, cron expression, recurring or one-shot, next fire time and the start of the prompt.";
static const char CT_CRONLIST_PROPS[] = "";
static const char CT_CRONLIST_REQ[] = "";
static void ct_cronlist(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    out[0] = 0;
    ct_sc_expire(ct_sched_now());
    if (!g_ct_cron_n) snprintf(out, cap, "No scheduled tasks");
    for (int i = 0; i < g_ct_cron_n; i++) {
        const CtCron *c = &g_ct_cron[i];
        char nb[48], pv[4 * 80 + 8]; ct_sc_fmt(c->next, nb, sizeof nb);
        const char *s = c->prompt; size_t n = 0, k = 0, ch = 0;
        for (; s[k]; k++) {                    /* 80 caracteres (UTF-8 inteiro); quebras de linha viram espaco */
            if (((unsigned char)s[k] & 0xC0) != 0x80) { if (ch == 80) break; ch++; }
            pv[n++] = (s[k] == '\n' || s[k] == '\r') ? ' ' : s[k];
        }
        pv[n] = 0;
        ct_sc_app(out, cap, "%s%s  %s  %s  next %s  %s%s", i ? "\n" : "", c->id, c->expr, c->recurring ? "recurring" : "one-shot", nb, pv, s[k] ? "..." : "");
    }
    const char *th = getenv("KATANA_SCHED_TEST");      /* gancho de teste: entrega o que venceu nesse instante */
    if (th && *th) {
        char p[4096], l[32]; int any = 0;
        while (ct_sched_due(atof(th), p, sizeof p, l, sizeof l)) { ct_sc_app(out, cap, "\ndelivered [%s] %s", l, p); any = 1; }
        if (!any) ct_sc_app(out, cap, "\nnothing due");
    }
}

/* ---- ScheduleWakeup ---- */
static const char CT_SCHEDULEWAKEUP_DESC[] =
    "Schedule ONE wakeup: after delaySeconds (clamped to 60..3600) `prompt` is delivered to you as if the user had typed it "
    "(marked as coming from the schedule), when the agent is idle. Only one wakeup is pending at a time: a new call replaces "
    "the pending one. With stop=true the pending wakeup is cancelled and the other fields are ignored. Use it to check back on "
    "something later (a build, a download). Lives only in this session.";
static const char CT_SCHEDULEWAKEUP_PROPS[] =
    "\"delaySeconds\": {\"type\": \"integer\", \"description\": \"Seconds until the wakeup, clamped to 60..3600.\"}, "
    "\"prompt\": {\"type\": \"string\", \"description\": \"The prompt delivered when the wakeup fires.\"}, "
    "\"reason\": {\"type\": \"string\", \"description\": \"Short reason for waking up, shown to the user.\"}, "
    "\"stop\": {\"type\": \"boolean\", \"description\": \"true cancels the pending wakeup (other fields ignored).\"}";
static const char CT_SCHEDULEWAKEUP_REQ[] = "";
static void ct_schedulewakeup(const char *js, const char *end, char *out, size_t cap) {
    out[0] = 0;
    if (ct_bool(js, end, "stop")) {
        const int had = g_ct_wake.on;
        ct_sc_wake_clear();
        snprintf(out, cap, "%s", had ? "Cancelled the pending wakeup" : "No pending wakeup to cancel");
        return;
    }
    char *prompt = ct_str(js, end, "prompt"), *reason = ct_str(js, end, "reason");
    if (!ct_has(js, end, "delaySeconds")) { snprintf(out, cap, "error: ScheduleWakeup needs `delaySeconds` (60..3600), or stop=true to cancel"); goto done; }
    if (!prompt || !*prompt) { snprintf(out, cap, "error: ScheduleWakeup needs a non-empty `prompt`"); goto done; }
    long long d = ct_int(js, end, "delaySeconds", 60);
    const int clamped = d < 60 || d > 3600;
    if (d < 60) d = 60;
    if (d > 3600) d = 3600;
    const double now = ct_sched_now();
    const int replaced = g_ct_wake.on;
    ct_sc_wake_clear();
    g_ct_wake.prompt = prompt; prompt = NULL; g_ct_wake.on = 1; g_ct_wake.at = now + (double)d;
    char fb[48]; ct_sc_fmt(g_ct_wake.at, fb, sizeof fb);
    snprintf(out, cap, "Wakeup scheduled in %lld seconds%s, at %s.", d, clamped ? " (clamped to 60..3600)" : "", fb);
    if (reason && *reason) ct_sc_app(out, cap, " Reason: %s.", reason);
    if (replaced) ct_sc_app(out, cap, " Replaced the previous pending wakeup.");
done:
    free(prompt); free(reason);
}
