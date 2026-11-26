/* ---------- memoria de fluxos (docs/flows.md, secoes 4 e 5) ----------
 * Quatro pecas que usam o estado do fluxo (agent/flow.c). Nenhuma escreve texto de treino; todas nascem desligadas, menos o minerador
 * (um comando offline) e o detector de falta de progresso (so conta).
 *  1. Minerador (`katana flow learn`): le o registro de acoes e tira duas tabelas, sem modelo: licoes "falha seguida de sucesso"
 *     (ferramenta, assunto, assinatura do erro -> a chamada que deu certo logo depois) e transicoes entre ferramentas com o desfecho.
 *  2. Licoes no laco (KATANA_FLOW_LESSONS=N): uma falha cuja chave ja tem licao vista N vezes ou mais recebe, na linha `next`, a forma
 *     que resolveu antes.
 *  3. Recuperacao guiada pelo estado (KATANA_FLOW_RETRIEVE=1): o ESTADO diz o que falta (as linhas em volta do erro; onde um nome do
 *     erro aparece; qual resultado da busca abrir) e o motor busca so isso, sem pergunta em linguagem natural.
 *  4. Sonda de conjunto fechado (KATANA_FLOW_RERANK=1): os candidatos entram depois do prefixo ja em cache, o motor le os logitos dos
 *     numeros "1".."9" numa unica passada, desconta a mesma passada com candidatos vazios (o vies do modelo por cada numero) e volta o
 *     contexto ao ponto de antes. Nao gera texto nem deixa rastro na conversa. */

/* ---- chave de uma falha: ferramenta, assunto (programa do comando, senao a ferramenta), assinatura do erro (1a linha, numeros trocados por #) ---- */
static void fm_subject(const char *name, const char *js, const char *end, char *o, size_t cap) {
    snprintf(o, cap, "%s", name);
    if (strcmp(name, "Bash") && strcmp(name, "PowerShell")) return;
    char *cmd = ct_str(js, end, "command"); if (!cmd) return;
    const char *p = cmd;
    for (;;) {      /* pula "cd X &&", VAR=valor, sudo */
        while (*p == ' ') p++;
        if (!strncmp(p, "cd ", 3)) { const char *a = strstr(p, "&&"); const char *b = strchr(p, ';'); const char *n = a && (!b || a < b) ? a + 2 : b ? b + 1 : NULL; if (!n) break; p = n; continue; }
        if (!strncmp(p, "sudo ", 5)) { p += 5; continue; }
        const char *eq = p; while (*eq && (isalnum((unsigned char)*eq) || *eq == '_')) eq++;
        if (*eq == '=' && eq > p) { while (*eq && *eq != ' ') eq++; p = eq; continue; }
        break;
    }
    size_t n = 0; const char *b = p; for (const char *q = p; *q && *q != ' ' && *q != '\n'; q++) if (*q == '/') b = q + 1;
    while (b[n] && b[n] != ' ' && b[n] != '\n' && n + 1 < cap && n < 40) n++;
    if (n) snprintf(o, cap, "%.*s", (int)n, b);
    free(cmd);
}
static void fm_errsig(const char *r, char *o, size_t cap) {
    static const char *const kw[] = {"error", "Error", "ERROR", "failed", "FAILED", "not found", "No such", "denied", "unsupported", "invalid", "cannot", "usage:", NULL};
    const char *best = NULL;
    for (const char *p = r; *p && !best; ) { const char *e = strchr(p, '\n'); const size_t l = e ? (size_t)(e - p) : strlen(p);
        for (int i = 0; kw[i] && !best; i++) { const char *h = strstr(p, kw[i]); if (h && h < p + l) best = p; }
        if (!e) break; p = e + 1; }
    if (!best) best = r;
    size_t n = 0; int lastd = 0;
    for (const char *q = best; *q && *q != '\n' && n + 1 < cap && n < 90; q++) {
        if (isdigit((unsigned char)*q)) { if (!lastd) o[n++] = '#'; lastd = 1; continue; }
        lastd = 0; o[n++] = (*q == '\t') ? ' ' : (char)tolower((unsigned char)*q);
    }
    while (n && o[n - 1] == ' ') n--;
    o[n] = 0;
}

/* ---- licoes carregadas (~/.katana/flow-lessons.tsv: vezes, ferramenta, assunto, assinatura, forma que deu certo) ---- */
#define FM_MAX 512
typedef struct { int n; char tool[24], subj[44], sig[96], fix[320]; } FmLesson;
static FmLesson *g_fm; static int g_fm_n, g_fm_loaded;
static void fm_path(char *p, size_t cap, const char *file) { char h[1024]; agent_home(h, sizeof h); snprintf(p, cap, "%s/.katana/%s", h, file); }
static void fm_load(void) {
    if (g_fm_loaded) return;
    g_fm_loaded = 1;
    char p[1200]; fm_path(p, sizeof p, "flow-lessons.tsv");
    FILE *f = fopen(p, "rb"); if (!f) return;
    g_fm = (FmLesson *)calloc(FM_MAX, sizeof *g_fm); if (!g_fm) { fclose(f); return; }
    char ln[1200];
    while (g_fm_n < FM_MAX && fgets(ln, sizeof ln, f)) {
        char *c[5]; int k = 0; c[0] = ln;
        for (char *q = ln; *q && k < 4; q++) if (*q == '\t') { *q = 0; c[++k] = q + 1; }
        if (k != 4) continue;
        c[4][strcspn(c[4], "\r\n")] = 0;
        FmLesson *l = &g_fm[g_fm_n++];
        l->n = atoi(c[0]); snprintf(l->tool, sizeof l->tool, "%s", c[1]); snprintf(l->subj, sizeof l->subj, "%s", c[2]); snprintf(l->sig, sizeof l->sig, "%s", c[3]); snprintf(l->fix, sizeof l->fix, "%s", c[4]);
    }
    fclose(f);
}
static int fm_lessons_min(void) { const char *e = getenv("KATANA_FLOW_LESSONS"); return e ? atoi(e) : 0; }      /* 0 = desligado; N = so licao vista N vezes ou mais */
static int flow_retrieve(const char *name, const char *result, char *out, size_t cap);
/* chamado por flow_after quando a chamada falhou: o que a memoria sabe. Devolve quantos bytes escreveu */
static size_t flow_memory_note(const char *name, const char *js, const char *end, const char *result, int failed, char *out, size_t cap) {
    const int mn = fm_lessons_min();
    if (!failed || mn <= 0 || cap < 200) return 0;
    fm_load();
    char subj[44], sig[96]; fm_subject(name, js, end, subj, sizeof subj); fm_errsig(result, sig, sizeof sig);
    for (int i = 0; i < g_fm_n; i++) {
        const FmLesson *l = &g_fm[i];
        if (l->n < mn || strcmp(l->tool, name) || strcmp(l->subj, subj) || strcmp(l->sig, sig)) continue;
        fl_log("lesson\t%s\t%s\t%s", name, subj, sig);
        return (size_t)snprintf(out, cap, "a failure like this one was solved %d time(s) before on this machine by: %.300s. ", l->n, l->fix);
    }
    return 0;
}

/* ---- sonda de conjunto fechado: ordena ate 9 candidatos pela resposta do proprio modelo, no prefixo em cache ---- */
static int fm_rerank_on(void) { const char *e = getenv("KATANA_FLOW_RERANK"); return e && atoi(e) > 0; }
/* uma passada: logitos dos numeros depois de `text`, a partir de mpos. 0 = falhou */
static int fm_probe_pass(Model *m, int mpos, const char *text, const int *dig, int n, float *score) {
    Ids ids = {0};
    encode(&m->tk, text, &ids);
    if (ids.n <= 0 || mpos + ids.n + 8 >= m->cfg.ctx) { free(ids.ids); return 0; }
    const float *lg = forward_seq(m, ids.ids, ids.n, mpos, 1);
    free(ids.ids);
    if (!lg) return 0;
    for (int i = 0; i < n; i++) score[i] = lg[dig[i]];
    return 1;
}
/* order recebe os indices dos candidatos, do melhor para o pior. 0 = sonda indisponivel (a ordem lexical de quem chamou fica) */
static int fm_probe_rank(const char *question, const char *const *item, int n, int *order) {
    Model *m = g_agent_m;
    if (!fm_rerank_on() || !m || g_in_sub || n < 2 || n > 9 || m->cfg.chat_fmt != 0) return 0;
    int dig[9];
    for (int i = 0; i < n; i++) {      /* cada numero precisa ser um token so */
        char d[2] = {(char)('1' + i), 0}; Ids one = {0}; encode(&m->tk, d, &one);
        const int ok = one.n == 1; if (ok) dig[i] = one.ids[0];
        free(one.ids);
        if (!ok) return 0;
    }
    int ngl, esz, backend;
    if (!pfx_geom(m, &ngl, &esz, &backend)) return 0;
    const int mpos = g_agent_pos;
    const uint64_t key = 0x464C4F5700000000ULL ^ ((uint64_t)getpid() << 20) ^ (uint64_t)mpos;
    static char text[9000], base[3000];
    size_t o = (size_t)snprintf(text, sizeof text, "<|im_end|>\n<|im_start|>user\nWhich ONE of these is the most useful for: %.400s\n", question);
    size_t b = (size_t)snprintf(base, sizeof base, "<|im_end|>\n<|im_start|>user\nWhich ONE of these is the most useful for: %.400s\n", question);
    for (int i = 0; i < n; i++) {
        o += (size_t)snprintf(text + o, sizeof text - o, "[%d] %.600s\n", i + 1, item[i]);
        b += (size_t)snprintf(base + b, sizeof base - b, "[%d] (empty)\n", i + 1);
        if (o + 800 >= sizeof text) return 0;
    }
    snprintf(text + o, sizeof text - o, "Answer with its number only.<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n[");
    snprintf(base + b, sizeof base - b, "Answer with its number only.<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n[");
    if (!pfx_save(m, key, mpos, ngl, esz, backend)) return 0;
    snap_hold();
    float s1[9], s0[9];
    const double t0 = now_sec();
    int ok = fm_probe_pass(m, mpos, text, dig, n, s1);
    int back = pfx_load(m, key, mpos, ngl, esz, backend);
    if (ok && back) { ok = fm_probe_pass(m, mpos, base, dig, n, s0); back = pfx_load(m, key, mpos, ngl, esz, backend); } else ok = 0;
    snap_release(back, mpos);
    { char p[1000]; pfx_path(p, sizeof p, key, 0); remove(p); }
    if (!back) g_ctx_lost = 1;                          /* o laco recalcula a conversa */
    if (!ok) return 0;
    for (int i = 0; i < n; i++) { s1[i] -= s0[i]; order[i] = i; }      /* desconta o vies do modelo por cada numero */
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (s1[order[j]] > s1[order[i]]) { const int t = order[i]; order[i] = order[j]; order[j] = t; }
    if (s1[order[0]] - s1[order[n - 1]] < 0.5f) { fl_log("probe\tno signal\t%d candidates\t%.1fs", n, now_sec() - t0); return 0; }      /* a sonda pode responder "sem sinal" */
    fl_log("probe\tranked\t%d candidates, best %d (margin %.2f)\t%.1fs", n, order[0] + 1, s1[order[0]] - s1[order[1]], now_sec() - t0);
    return 1;
}

/* ---- recuperacao guiada pelo estado: o estado do fluxo decide O QUE buscar ---- */
static int fm_retrieve_on(void) { const char *e = getenv("KATANA_FLOW_RETRIEVE"); return e && atoi(e) > 0; }
/* linhas a-b de um arquivo, numeradas, em out */
static size_t fm_lines(const char *path, int a, int b, char *out, size_t cap) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    char ln[600]; int no = 0; size_t o = 0;
    while (fgets(ln, sizeof ln, f) && no < b) { no++; if (no < a) continue; ln[strcspn(ln, "\r\n")] = 0; if (o + 220 >= cap) break; o += (size_t)snprintf(out + o, cap - o, "%6d  %.200s\n", no, ln); }
    fclose(f);
    return o;
}
/* o nome mais especifico da linha de erro: o identificador mais longo com _ ou maiuscula no meio, ou o mais longo */
static void fm_ident(const char *s, char *o, size_t cap) {
    static const char *const stop[] = {"error", "Error", "undefined", "reference", "expected", "cannot", "failed", "warning", "function", "declaration", "implicit", NULL};
    size_t bl = 0; int bs = 0; o[0] = 0;
    for (const char *p = s; *p; ) {
        if (!(isalpha((unsigned char)*p) || *p == '_')) { p++; continue; }
        const char *a = p; int sc = 0;
        while (isalnum((unsigned char)*p) || *p == '_') { if (*p == '_' || (p > a && isupper((unsigned char)*p))) sc = 1; p++; }
        const size_t l = (size_t)(p - a); if (l < 4 || l + 1 >= cap) continue;
        int skip = 0; for (int i = 0; stop[i]; i++) if (strlen(stop[i]) == l && !strncmp(stop[i], a, l)) skip = 1;
        if (skip) continue;
        if (sc > bs || (sc == bs && l > bl)) { bs = sc; bl = l; memcpy(o, a, l); o[l] = 0; }
    }
}
/* chamado por flow_after depois de o estado andar; devolve 1 se acrescentou evidencia ao resultado */
static int flow_retrieve(const char *name, const char *result, char *out, size_t cap) {
    (void)name;
    if (!fm_retrieve_on() || cap < 1200) return 0;
    const FlowPos *t = fl_top(); if (!t) return 0;
    const int need_code = (t->id == FL_DEBUG && t->st == 1) || (t->id == FL_CODE && t->st == 3);
    if (need_code && g_fl.errfile[0]) {      /* o erro aponta arquivo:linha que existe: as linhas em volta sao a evidencia, sem busca nenhuma */
        char path[300]; int line = 0;
        const char *a = strstr(g_fl.errfile, "at "); if (!a) return 0; a += 3;
        const char *c = strrchr(a, ':'); if (!c) return 0;
        snprintf(path, sizeof path, "%.*s", (int)(c - a), a); line = atoi(c + 1);
        size_t o = (size_t)snprintf(out, cap, "\n[katana: evidence for the next step, read by the engine: %s lines %d-%d]\n", path, line > 4 ? line - 4 : 1, line + 4);
        const size_t n = fm_lines(path, line > 4 ? line - 4 : 1, line + 4, out + o, cap - o);
        if (!n) { out[0] = 0; return 0; }
        fl_log("retrieve\tlines\t%s:%d", path, line);
        return 1;
    }
    if (need_code && g_fl.err[0]) {         /* sem arquivo:linha: onde o nome mais especifico do erro aparece (lexical), reordenado pela sonda se ligada */
        char id[80]; fm_ident(g_fl.err, id, sizeof id); if (!id[0]) return 0;
        static char js[300], gr[6000];
        snprintf(js, sizeof js, "{\"pattern\": \"%s\", \"literal\": true, \"output_mode\": \"content\", \"head_limit\": 9}", id);
        ct_grep(js, js + strlen(js), gr, sizeof gr);
        if (!strncmp(gr, "error:", 6) || !strchr(gr, ':')) return 0;
        const char *item[9]; static char buf[9][260]; int n = 0;
        for (const char *p = gr; *p && n < 9; ) { const char *e = strchr(p, '\n'); const size_t l = e ? (size_t)(e - p) : strlen(p); if (l > 6 && memchr(p, ':', l)) { snprintf(buf[n], sizeof buf[n], "%.*s", (int)(l < 250 ? l : 250), p); item[n] = buf[n]; n++; } if (!e) break; p = e + 1; }
        if (!n) return 0;
        int order[9]; for (int i = 0; i < n; i++) order[i] = i;
        char q[300]; snprintf(q, sizeof q, "finding where this error comes from: %.200s", g_fl.err);
        const int ranked = n > 1 ? fm_probe_rank(q, item, n, order) : 0;
        size_t o = (size_t)snprintf(out, cap, "\n[katana: evidence for the next step: where `%s` appears%s]\n", id, ranked ? ", best first by the model's own reading" : "");
        for (int i = 0; i < n && i < 4 && o + 270 < cap; i++) o += (size_t)snprintf(out + o, cap - o, "%s\n", item[order[i]]);
        fl_log("retrieve\tident\t%s\t%d hit(s)%s", id, n, ranked ? " ranked" : "");
        return 1;
    }
    if (t->id == FL_WEB && t->st == 1 && fm_rerank_on()) {      /* qual resultado abrir: os titulos da busca, ordenados pela sonda */
        const char *item[9]; static char buf[9][400]; int n = 0;
        for (const char *p = result; *p && n < 9; ) { const char *e = strchr(p, '\n'); const size_t l = e ? (size_t)(e - p) : strlen(p);
            if (l > 4 && isdigit((unsigned char)p[0]) && (p[1] == '.' || p[2] == '.')) { const char *u = e ? e + 1 : NULL; size_t ul = u ? strcspn(u, "\n") : 0; snprintf(buf[n], sizeof buf[n], "%.*s %.*s", (int)(l < 180 ? l : 180), p, (int)(ul < 180 ? ul : 180), u ? u : ""); item[n] = buf[n]; n++; }
            if (!e) break; p = e + 1; }
        int order[9];
        if (n < 2 || !fm_probe_rank("answering the user's question", item, n, order)) return 0;
        snprintf(out, cap, "\n[katana: of the results above, open this one first: %.300s]", item[order[0]]);
        fl_log("retrieve\tweb\t%d result(s)", n);
        return 1;
    }
    return 0;
}

/* ---- minerador: katana flow learn [arquivo] [--days N] [--print] ---- */
typedef struct { char *t, *tool, *args, *res; } FmRec;
static int fm_rec_failed(const char *r) {
    if (!strncmp(r, "error:", 6) || !strncmp(r, "Error:", 6)) return 1;
    const char *e = strstr(r, "Exit code "); if (e && e[10] >= '1' && e[10] <= '9') return 1;
    return strstr(r, "command not found") || strstr(r, "No such file or directory") || strstr(r, "Operation unsupported") || strstr(r, "unrecognized ") || strstr(r, "opcao desconhecida");
}
/* argumentos do registro -> a forma mostravel da chamada (o comando do Bash; senao os argumentos), com segredos mascarados */
static void fm_call_text(const FmRec *r, char *o, size_t cap) {
    char *cmd = ct_str(r->args, r->args + strlen(r->args), "command");
    snprintf(o, cap, "%.300s", cmd ? cmd : r->args);
    free(cmd);
    for (char *q = o; *q; q++) if (*q == '\t' || *q == '\n') *q = ' ';
    secret_mask(o);
}
/* forma dos argumentos: as chaves, em ordem alfabetica ("command,timeout") */
static void fm_shape(const char *args, char *o, size_t cap) {
    char k[12][28]; int n = 0;
    for (const char *p = args; *p && n < 12; p++) {
        if (*p != '"' || (p > args && p[-1] == '\\')) continue;
        const char *e = p + 1; while (*e && (isalnum((unsigned char)*e) || *e == '_' || *e == '-')) e++;
        if (*e != '"' || e == p + 1 || e - p > 26) continue;
        const char *c = e + 1; while (*c == ' ') c++;
        if (*c != ':') continue;
        char w[28]; snprintf(w, sizeof w, "%.*s", (int)(e - p - 1), p + 1);
        if (!strcmp(w, "arguments") || !strcmp(w, "description")) { p = c; continue; }
        int dup = 0; for (int i = 0; i < n; i++) if (!strcmp(k[i], w)) dup = 1;
        if (!dup) snprintf(k[n++], sizeof k[0], "%s", w);
        p = c;
    }
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (strcmp(k[j], k[i]) < 0) { char t[28]; memcpy(t, k[i], 28); memcpy(k[i], k[j], 28); memcpy(k[j], t, 28); }
    size_t q = 0; o[0] = 0;
    for (int i = 0; i < n && q + 30 < cap; i++) q += (size_t)snprintf(o + q, cap - q, "%s%s", i ? "," : "", k[i]);
}
/* parte das palavras de a que tambem esta em b (0 a 100): a correcao de um comando e o MESMO comando com algo mudado, nao outro comando do mesmo programa */
static int fm_overlap(const char *a, const char *b) {
    int tot = 0, hit = 0;
    for (const char *p = a; *p; ) {
        while (*p == ' ') p++;
        const char *e = p; while (*e && *e != ' ') e++;
        const size_t l = (size_t)(e - p);
        if (l >= 2 && l < 120) { char w[122]; w[0] = ' '; memcpy(w + 1, p, l); w[l + 1] = 0; tot++; const char *h = strstr(b, w + 1); if (h && (h == b || h[-1] == ' ' || h[-1] == '/') ) hit++; }
        p = e;
    }
    return tot ? hit * 100 / tot : 0;
}
static int fm_time_s(const char *t) { int h = 0, m = 0, s = 0, d = 0; if (strlen(t) >= 19) { d = atoi(t + 8); h = atoi(t + 11); m = atoi(t + 14); s = atoi(t + 17); } return ((d * 24 + h) * 60 + m) * 60 + s; }
static int flowmem_main(int argc, char **argv) {
    const char *cmd = argc ? argv[0] : "";
    if (!strcmp(cmd, "lessons")) {
        fm_load();
        for (int i = 0; i < g_fm_n; i++) printf("%3d  %s %s | %s\n       -> %s\n", g_fm[i].n, g_fm[i].tool, g_fm[i].subj, g_fm[i].sig, g_fm[i].fix);
        printf("%d lesson(s); used in the loop only with KATANA_FLOW_LESSONS=N (N = minimum times seen)\n", g_fm_n);
        return 0;
    }
    if (strcmp(cmd, "learn")) { fprintf(stderr, "usage: katana flow list | tools | classify \"<request>\" | sim <script> | learn [log] [--days N] [--print] | lessons\n"); return 2; }
    char path[1200] = ""; double days = 0; int print = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--days") && i + 1 < argc) days = atof(argv[++i]);
        else if (!strcmp(argv[i], "--print")) print = 1;
        else snprintf(path, sizeof path, "%s", argv[i]);
    }
    if (!path[0]) { const char *e = getenv("KATANA_LOG"); if (e && *e) snprintf(path, sizeof path, "%s", e); else fm_path(path, sizeof path, "agente.log"); }
    FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "flow learn: cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); const long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1); if (!buf) { fclose(f); return 1; }
    const size_t got = fread(buf, 1, (size_t)sz, f); buf[got] = 0; fclose(f);
    char cut[24] = "";
    if (days > 0) { const time_t t = time(NULL) - (time_t)(days * 86400.0); strftime(cut, sizeof cut, "%Y-%m-%d %H:%M:%S", localtime(&t)); }
    int nr = 0, capr = 4096; FmRec *rec = (FmRec *)malloc(sizeof *rec * (size_t)capr);
    for (char *p = buf; *p; ) {      /* um registro por linha: data \t ferramenta \t argumentos \t resultado */
        char *e = strchr(p, '\n'); if (e) *e = 0;
        if (isdigit((unsigned char)p[0]) && strlen(p) > 20 && p[4] == '-' && p[19] == '\t' && (!cut[0] || strncmp(p, cut, 19) >= 0)) {
            char *c1 = p + 19; *c1++ = 0; char *c2 = strchr(c1, '\t'); char *c3 = c2 ? strchr(c2 + 1, '\t') : NULL;
            if (c2 && c3) { *c2++ = 0; *c3++ = 0; if (nr == capr) { capr *= 2; rec = (FmRec *)realloc(rec, sizeof *rec * (size_t)capr); } rec[nr].t = p; rec[nr].tool = c1; rec[nr].args = c2; rec[nr].res = c3; nr++; }
        }
        if (!e) break; p = e + 1;
    }
    /* licoes: para cada falha, a primeira chamada da MESMA ferramenta e do MESMO assunto que deu certo nas 12 chamadas e 30 minutos seguintes */
    FmLesson *L = (FmLesson *)calloc(FM_MAX, sizeof *L); int nl = 0, pairs = 0;
    /* transicoes: (ferramenta anterior, desfecho) -> ferramenta seguinte, com quantas deram certo */
    typedef struct { char a[24], b[24]; int af, n, ok; } FmTr;
    FmTr *T = (FmTr *)calloc(1024, sizeof *T); int nt = 0;
    for (int i = 0; i < nr; i++) {
        const int fi = fm_rec_failed(rec[i].res);
        if (i + 1 < nr && fm_time_s(rec[i + 1].t) - fm_time_s(rec[i].t) < 1800 && fm_time_s(rec[i + 1].t) >= fm_time_s(rec[i].t)) {
            int k = -1; for (int j = 0; j < nt; j++) if (T[j].af == fi && !strcmp(T[j].a, rec[i].tool) && !strcmp(T[j].b, rec[i + 1].tool)) k = j;
            if (k < 0 && nt < 1024) { k = nt++; snprintf(T[k].a, sizeof T[k].a, "%.23s", rec[i].tool); snprintf(T[k].b, sizeof T[k].b, "%.23s", rec[i + 1].tool); T[k].af = fi; }
            if (k >= 0) { T[k].n++; T[k].ok += !fm_rec_failed(rec[i + 1].res); }
        }
        if (!fi || rec[i].args[0] != '"') continue;      /* so o registro com os argumentos em JSON (a linha crua da busca nao conta) */
        char subj[44], sig[96]; const char *ae = rec[i].args + strlen(rec[i].args);
        fm_subject(rec[i].tool, rec[i].args, ae, subj, sizeof subj); fm_errsig(rec[i].res, sig, sizeof sig);
        if (!sig[0]) continue;
        for (int j = i + 1; j < nr && j <= i + 12; j++) {
            if (fm_time_s(rec[j].t) - fm_time_s(rec[i].t) > 1800 || fm_time_s(rec[j].t) < fm_time_s(rec[i].t)) break;
            if (strcmp(rec[j].tool, rec[i].tool) || fm_rec_failed(rec[j].res) || !strcmp(rec[j].args, rec[i].args)) continue;
            char s2[44]; fm_subject(rec[j].tool, rec[j].args, rec[j].args + strlen(rec[j].args), s2, sizeof s2);
            if (strcmp(s2, subj)) continue;
            char fix[320], bad[320]; fm_call_text(&rec[j], fix, sizeof fix); fm_call_text(&rec[i], bad, sizeof bad);
            if (strstr(fix, "--help") || strstr(fix, " -h") || strstr(fix, "--version")) continue;      /* pedir ajuda da certo, mas nao e a correcao */
            if (!strcmp(rec[i].tool, "Bash") || !strcmp(rec[i].tool, "PowerShell")) { if (fm_overlap(bad, fix) < 50) continue; }      /* outro comando do mesmo programa nao e a correcao deste */
            else {      /* outras ferramentas: a licao e a FORMA dos argumentos que deu certo, e so quando ela mudou; os valores nao se repetem */
                char s1[200], s2b[200]; fm_shape(rec[i].args, s1, sizeof s1); fm_shape(rec[j].args, s2b, sizeof s2b);
                if (!s2b[0] || !strcmp(s1, s2b)) continue;
                snprintf(fix, sizeof fix, "calling %s with the arguments {%s} instead of {%s}", rec[i].tool, s2b, s1);
            }
            pairs++;
            int k = -1; for (int q = 0; q < nl; q++) if (!strcmp(L[q].tool, rec[i].tool) && !strcmp(L[q].subj, subj) && !strcmp(L[q].sig, sig)) k = q;
            if (k < 0 && nl < FM_MAX) { k = nl++; snprintf(L[k].tool, sizeof L[k].tool, "%.23s", rec[i].tool); snprintf(L[k].subj, sizeof L[k].subj, "%s", subj); snprintf(L[k].sig, sizeof L[k].sig, "%s", sig); }
            if (k >= 0) { L[k].n++; snprintf(L[k].fix, sizeof L[k].fix, "%s", fix); }      /* fica a forma mais recente */
            break;
        }
    }
    for (int i = 0; i < nl; i++) for (int j = i + 1; j < nl; j++) if (L[j].n > L[i].n) { const FmLesson t = L[i]; L[i] = L[j]; L[j] = t; }
    char out1[1200], out2[1200]; fm_path(out1, sizeof out1, "flow-lessons.tsv"); fm_path(out2, sizeof out2, "flow-transitions.tsv");
    FILE *o1 = print ? stdout : fopen(out1, "wb");
    if (o1) { for (int i = 0; i < nl; i++) fprintf(o1, "%d\t%s\t%s\t%s\t%s\n", L[i].n, L[i].tool, L[i].subj, L[i].sig, L[i].fix); if (!print) fclose(o1); }
    FILE *o2 = print ? NULL : fopen(out2, "wb");
    if (o2) { for (int i = 0; i < nt; i++) fprintf(o2, "%s\t%s\t%s\t%d\t%d\n", T[i].a, T[i].af ? "fail" : "ok", T[i].b, T[i].n, T[i].ok); fclose(o2); }
    fprintf(stderr, "flow learn: %d records, %d failure-then-success pairs, %d lesson(s)%s%s, %d transition(s)%s%s\n", nr, pairs, nl, print ? "" : " -> ", print ? "" : out1, nt, print ? "" : " -> ", print ? "" : out2);
    free(L); free(T); free(rec); free(buf);
    return 0;
}
