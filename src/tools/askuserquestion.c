/* ---------- AskUserQuestion ----------
 * Pergunta pelo mesmo caminho do ask_user (agent/ask.c): no terminal, opcoes numeradas; na interface (KATANA_UI), o
 * marcador CHOICE e a resposta na linha "\x01ANS texto". Uma pergunta por vez; as descricoes das opcoes vao no texto da
 * pergunta (o mecanismo so leva os rotulos). Escolha multipla: o usuario digita os rotulos separados por virgula. */
#define CT_Q_MAX 4
#define CT_OPT_MAX 4
typedef struct { char label[80]; char desc[240]; } CtOpt;
typedef struct { char question[600]; char header[40]; CtOpt opt[CT_OPT_MAX]; int nopt; int multi; } CtQuestion;

/* escreve s dentro de um texto JSON (com aspas) em dst; devolve o tamanho ou -1 se nao couber */
static int ct_json_put(char *dst, size_t cap, size_t *o, const char *s) {
    for (; *s; s++) {
        char tmp[8]; int n;
        if (*s == '"' || *s == '\\') { tmp[0] = '\\'; tmp[1] = *s; n = 2; }
        else if (*s == '\n') { tmp[0] = '\\'; tmp[1] = 'n'; n = 2; }
        else if ((unsigned char)*s < 32) { tmp[0] = ' '; n = 1; }
        else { tmp[0] = *s; n = 1; }
        if (*o + (size_t)n + 1 >= cap) return -1;
        memcpy(dst + *o, tmp, (size_t)n); *o += (size_t)n;
    }
    dst[*o] = 0;
    return 0;
}
static int ct_ieq(const char *a, const char *b) { for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0; return !*a && !*b; }
static void ct_trim(char *s) {
    char *p = s; while (*p == ' ' || *p == '\t') p++;
    memmove(s, p, strlen(p) + 1);
    size_t l = strlen(s); while (l && (s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = 0;
}
/* faz uma pergunta ao usuario com os rotulos como opcoes. Devolve 1 e a resposta em ans, ou 0 e o texto de erro/cancelamento
   (para o modelo) em ans */
static int ct_ask(const char *question, char (*labels)[80], int n, char *ans, size_t cap) {
    static char js[4000];
    size_t o = 0;
    js[0] = 0;
    char opts[800]; size_t oo = 0;
    opts[0] = 0;
    for (int i = 0; i < n; i++) {
        char l[80]; snprintf(l, sizeof l, "%s", labels[i]);
        for (char *p = l; *p; p++) if (*p == '|') *p = '/';
        oo += (size_t)snprintf(opts + oo, sizeof opts - oo, "%s%s", i ? "|" : "", l);
        if (oo >= sizeof opts - 90) break;
    }
    o += (size_t)snprintf(js, sizeof js, "{\"question\": \"");
    ct_json_put(js, sizeof js, &o, question);
    o += (size_t)snprintf(js + o, sizeof js - o, "\", \"options\": \"");
    ct_json_put(js, sizeof js, &o, opts);
    snprintf(js + o, sizeof js - o, "\"}");
    static char res[4000];
    tool_ask_user(js, js + strlen(js), res, sizeof res);
    static const char P[] = "the user answered: ";
    if (strncmp(res, P, sizeof P - 1) == 0) { snprintf(ans, cap, "%.1400s", res + sizeof P - 1); return 1; }
    snprintf(ans, cap, "%.1400s", res);
    return 0;
}

/* opcoes a partir do valor JSON v (texto "a|b", ou lista de textos, ou lista de {label, description}); devolve quantas */
static int ct_opts_parse(const char *v, const char *ve, CtOpt *opt, int *toomany) {
    int n = 0;
    *toomany = 0;
    if (*v == '"') {
        char *s = ct_unquote(v, ve, NULL);
        for (char *p = s; *p; ) {
            char *e = strchr(p, '|');
            if (e) *e = 0;
            ct_trim(p);
            if (*p) { if (n == CT_OPT_MAX) { (*toomany)++; } else { memset(&opt[n], 0, sizeof opt[n]); ct_cpy(opt[n].label, sizeof opt[n].label, p); n++; } }
            if (!e) break;
            p = e + 1;
        }
        free(s);
        return n;
    }
    if (*v != '[') return 0;
    const char *p = v + 1;
    for (;;) {
        while (p < ve && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= ve || *p == ']') break;
        const char *e = ct_skip(p, ve);
        if (!e) break;
        if (n == CT_OPT_MAX) { (*toomany)++; p = e; continue; }
        memset(&opt[n], 0, sizeof opt[n]);
        if (*p == '"') { char *s = ct_unquote(p, e, NULL); ct_cpy(opt[n].label, sizeof opt[n].label, s); free(s); n++; }
        else if (*p == '{') {
            char *obj = (char *)malloc((size_t)(e - p) + 1);
            memcpy(obj, p, (size_t)(e - p)); obj[e - p] = 0;
            char *l = ct_str(obj, obj + (e - p), "label"), *d = ct_str(obj, obj + (e - p), "description");
            ct_cpy(opt[n].label, sizeof opt[n].label, l); ct_cpy(opt[n].desc, sizeof opt[n].desc, d);
            free(l); free(d); free(obj);
            n++;
        }
        else { p = e; continue; }
        if (!opt[n - 1].label[0]) n--;
        p = e;
    }
    return n;
}

static const char CT_ASKUSERQUESTION_DESC[] =
    "Ask the user 1 to 4 multiple-choice questions when you need a decision or a preference you cannot work out yourself. "
    "Each question has 2 to 4 options (a label, and a description when it helps); header (a very short label) and multiSelect are optional. The user can always type a free answer instead. "
    "Do not use it to ask for plan approval (use ExitPlanMode) and do not ask what you can find out by reading files.";
static const char CT_ASKUSERQUESTION_PROPS[] =
    "\"questions\": {\"type\": \"array\", \"description\": \"1 to 4 questions\", \"items\": {\"type\": \"object\", \"properties\": {"
    "\"question\": {\"type\": \"string\", \"description\": \"The complete question, ending with a question mark\"}, "
    "\"header\": {\"type\": \"string\", \"description\": \"Very short label (max 12 characters)\"}, "
    "\"options\": {\"type\": \"array\", \"description\": \"2 to 4 choices\", \"items\": {\"type\": \"object\", \"properties\": {"
    "\"label\": {\"type\": \"string\", \"description\": \"Short choice text (1 to 5 words)\"}, "
    "\"description\": {\"type\": \"string\", \"description\": \"What this choice means or implies\"}}, \"required\": [\"label\"]}}, "
    "\"multiSelect\": {\"type\": \"boolean\", \"description\": \"Allow choosing several options\"}}, "
    "\"required\": [\"question\", \"options\"]}}";
static const char CT_ASKUSERQUESTION_REQ[] = "\"questions\"";

/* um item de questions (objeto JSON em obj..oe) ou, na forma antiga, os argumentos todos */
static int ct_q_parse(const char *obj, const char *oe, int idx, CtQuestion *q, char *err, size_t ecap) {
    char *qs = ct_str(obj, oe, "question"), *h = ct_str(obj, oe, "header");
    const char *v, *ve;
    int r = 0;
    memset(q, 0, sizeof *q);
    if (!qs || !qs[0]) { snprintf(err, ecap, "error: questions[%d].question is missing or empty", idx); goto done; }
    ct_cpy(q->question, sizeof q->question, qs);
    ct_cpy(q->header, sizeof q->header, h);
    if (!ct_find(obj, oe, "options", &v, &ve)) { snprintf(err, ecap, "error: questions[%d].options is missing; give 2 to 4 options, each {label, description}", idx); goto done; }
    int many;
    q->nopt = ct_opts_parse(v, ve, q->opt, &many);
    if (q->nopt < 2) { snprintf(err, ecap, "error: questions[%d].options has %d usable option(s); it needs 2 to 4, each with a non-empty label", idx, q->nopt); goto done; }
    if (many) { snprintf(err, ecap, "error: questions[%d].options has %d options; the maximum is %d (the user can always type another answer)", idx, q->nopt + many, CT_OPT_MAX); goto done; }
    for (int i = 0; i < q->nopt; i++) for (int j = 0; j < i; j++)
        if (ct_ieq(q->opt[i].label, q->opt[j].label)) { snprintf(err, ecap, "error: questions[%d].options has the label \"%.60s\" twice; labels must be different", idx, q->opt[i].label); goto done; }
    q->multi = ct_bool(obj, oe, "multiSelect");
    r = 1;
done:
    free(qs); free(h);
    return r;
}

static void ct_askuserquestion(const char *js, const char *end, char *out, size_t cap) {
    CtQuestion *qs = (CtQuestion *)calloc(CT_Q_MAX, sizeof *qs);
    int nq = 0;
    const char *a, *b;
    char err[300];
    if (ct_find(js, end, "questions", &a, &b)) {
        if (*a != '[') { snprintf(out, cap, "error: questions must be an array of 1 to 4 objects {question, options}"); goto done; }
        const char *p = a + 1;
        for (;;) {
            while (p < b && (isspace((unsigned char)*p) || *p == ',')) p++;
            if (p >= b || *p == ']') break;
            const char *e = ct_skip(p, b);
            if (!e || *p != '{') { snprintf(out, cap, "error: questions[%d] must be an object {question, options}", nq); goto done; }
            if (nq == CT_Q_MAX) { snprintf(out, cap, "error: too many questions; ask at most %d at a time", CT_Q_MAX); goto done; }
            char *obj = (char *)malloc((size_t)(e - p) + 1);
            memcpy(obj, p, (size_t)(e - p)); obj[e - p] = 0;
            const int ok = ct_q_parse(obj, obj + (e - p), nq, &qs[nq], err, sizeof err);
            free(obj);
            if (!ok) { snprintf(out, cap, "%s", err); goto done; }
            nq++;
            p = e;
        }
        if (!nq) { snprintf(out, cap, "error: questions is empty; give 1 to 4 questions"); goto done; }
    } else if (ct_has(js, end, "question")) {            /* forma antiga: question + options */
        if (!ct_q_parse(js, end, 0, &qs[0], err, sizeof err)) { snprintf(out, cap, "%s", err); goto done; }
        nq = 1;
    } else { snprintf(out, cap, "error: questions is required: an array of 1 to 4 objects {question, options}"); goto done; }

    static char acc[8000];
    size_t o = (size_t)snprintf(acc, sizeof acc, "User answers:\n");
    for (int i = 0; i < nq; i++) {
        const CtQuestion *q = &qs[i];
        static char text[1500], ans[2000], lab[CT_OPT_MAX][80];
        size_t t = (size_t)snprintf(text, sizeof text, "%s%s%s%s", q->header[0] ? "[" : "", q->header, q->header[0] ? "] " : "", q->question);
        t += (size_t)snprintf(text + t, sizeof text - t, " Options:");
        for (int k = 0; k < q->nopt; k++) {
            snprintf(lab[k], sizeof lab[k], "%s", q->opt[k].label);
            t += (size_t)snprintf(text + t, sizeof text - t, " (%d) %s%s%s%s", k + 1, q->opt[k].label, q->opt[k].desc[0] ? " = " : "", q->opt[k].desc, k + 1 < q->nopt ? ";" : ".");
            if (t >= sizeof text - 200) break;
        }
        if (q->multi) snprintf(text + t, sizeof text - t, " You may choose several: type the labels separated by commas.");
        if (!ct_ask(text, lab, q->nopt, ans, sizeof ans)) {
            if (strncmp(ans, "error:", 6) == 0 || i == 0) snprintf(out, cap, "%s", ans);
            else snprintf(out, cap, "%s%d. cancelled: the user did not answer the remaining questions. Do not ask again; choose the safest reasonable option and continue.", acc, i + 1);
            goto done;
        }
        /* interpreta a resposta: rotulos conhecidos (um, ou varios separados por virgula) ou texto livre */
        char chosen[1200] = "";
        size_t cl = 0;
        int nm = 0, nparts = 0;
        char tmp[2000]; snprintf(tmp, sizeof tmp, "%s", ans);
        char whole[2000]; snprintf(whole, sizeof whole, "%s", ans); ct_trim(whole);
        int hit = -1;
        for (int k = 0; k < q->nopt; k++) if (ct_ieq(whole, q->opt[k].label)) hit = k;
        if (hit >= 0) { cl = (size_t)snprintf(chosen, sizeof chosen, "%s", q->opt[hit].label); nm = 1; nparts = 1; }
        else if (q->multi) {
            for (char *p = tmp; ; ) {
                char *e = strpbrk(p, ",;");
                if (e) *e = 0;
                ct_trim(p);
                if (*p) {
                    nparts++;
                    for (int k = 0; k < q->nopt; k++) if (ct_ieq(p, q->opt[k].label)) { cl += (size_t)snprintf(chosen + cl, sizeof chosen - cl, "%s%s", nm ? ", " : "", q->opt[k].label); nm++; break; }
                }
                if (!e) break;
                p = e + 1;
            }
        }
        if (nm && nm == nparts) o += (size_t)snprintf(acc + o, sizeof acc - o, "%d. %s -> %s\n", i + 1, q->question, chosen);
        else o += (size_t)snprintf(acc + o, sizeof acc - o, "%d. %s -> (free text) %s\n", i + 1, q->question, whole);
        if (o >= sizeof acc - 100) break;
    }
    snprintf(out, cap, "%s", acc);
    if (strlen(acc) >= cap && cap) out[cap - 1] = 0;
done:
    free(qs);
}
