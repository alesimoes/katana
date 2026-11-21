/* ---------- Workflow: varios subagentes em sequencia a partir de uma descricao em JSON ----------
 * Nao ha interpretador de JavaScript (sem dependencias): aceita um subconjunto declarativo com os mesmos nomes
 *     {"name": ..., "description": ..., "phases": [{"title": ..., "agents": [{"label": ..., "prompt": ...}], "then": "...{{results}}..."}]}
 * Cada prompt roda como subagente (o mesmo caminho do Agent, via g_ct_sub_run), um depois do outro. Substituicoes:
 * {{args}} em todo lugar; {{results}} nos prompts dos agentes = resultado da fase anterior, no "then" = respostas
 * dos agentes desta fase. Agente que falha fica anotado e a execucao segue. Teto de agentes por execucao:
 * KATANA_WORKFLOW_MAX_AGENTS (padrao 10). Os subagentes so leem (delegate liga o modo plano). */
static const char CT_WORKFLOW_DESC[] = "Run several sub-agents from one workflow description and get one consolidated result. "
    "Here this is a SEQUENTIAL JSON subset (no JavaScript, nothing runs in the background): pass script (inline JSON), scriptPath (a .json file) "
    "or name (.katana/workflows/<name>.json), plus optional args. Form: {\"name\":..., \"description\":..., \"phases\":[{\"title\":..., "
    "\"agents\":[{\"label\":..., \"prompt\":...}], \"then\": \"prompt that combines {{results}}\"}]}. In agent prompts {{results}} is the previous "
    "phase's result; in then it is this phase's answers; {{args}} is args. Sub-agents only read. At most 10 agents per run";
static const char CT_WORKFLOW_PROPS[] = "\"script\": {\"type\": \"string\", \"description\": \"The workflow as inline JSON\"}, "
    "\"scriptPath\": {\"type\": \"string\", \"description\": \"Path of a JSON workflow file\"}, "
    "\"name\": {\"type\": \"string\", \"description\": \"Name of a saved workflow in .katana/workflows/ or ~/.katana/workflows/\"}, "
    "\"args\": {\"description\": \"Value substituted for {{args}}\"}";
static const char CT_WORKFLOW_REQ[] = "";

/* chave do primeiro nivel de um objeto que comeca em p (sem a busca de "arguments" de ct_find) */
static int wf_key(const char *p, const char *end, const char *key, const char **vs, const char **ve) {
    while (p < end && *p != '{') p++;
    if (p >= end) return 0;
    const size_t kl = strlen(key);
    p++;
    for (;;) {
        while (p < end && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= end || *p != '"') return 0;
        const char *ks = p + 1, *ke = ct_skip(p, end);
        if (!ke) return 0;
        p = ke;
        while (p < end && (isspace((unsigned char)*p) || *p == ':')) p++;
        const char *v = p, *e = ct_skip(p, end);
        if (!e) return 0;
        if ((size_t)(ke - 1 - ks) == kl && memcmp(ks, key, kl) == 0) { *vs = v; *ve = e; return 1; }
        p = e;
    }
}
static char *wf_str(const char *p, const char *end, const char *key) {
    const char *a, *b;
    if (!wf_key(p, end, key, &a, &b) || *a != '"') return NULL;
    return ct_unquote(a, b, NULL);
}
/* proximo elemento de um array: *it aponta dentro do array; devolve inicio e fim do elemento */
static int wf_next(const char **it, const char *end, const char **es, const char **ee) {
    const char *p = *it;
    while (p < end && (isspace((unsigned char)*p) || *p == ',' || *p == '[')) p++;
    if (p >= end || *p == ']') return 0;
    const char *e = ct_skip(p, end);
    if (!e) return 0;
    *es = p; *ee = e; *it = e;
    return 1;
}
/* troca {{args}} e {{results}} em memoria nova */
static char *wf_subst(const char *t, const char *args, const char *results) {
    Buf b = {0};
    for (const char *p = t; *p;) {
        if (strncmp(p, "{{args}}", 8) == 0) { buf_add(&b, args, strlen(args)); p += 8; }
        else if (strncmp(p, "{{results}}", 11) == 0) { buf_add(&b, results, strlen(results)); p += 11; }
        else { buf_add(&b, p, 1); p++; }
    }
    buf_add(&b, "", 1);
    return b.p;
}
static int wf_is_js(const char *s) {
    return strstr(s, "export const meta") || strstr(s, "export default") || strstr(s, "await ") || strstr(s, "await(") ||
           strstr(s, "agent(") || strstr(s, "parallel(") || strstr(s, "pipeline(");
}
static const char WF_FORM[] = "Workflow here accepts only a sequential JSON form (no JavaScript, no background runs). Example:\n"
    "{\"name\": \"review\", \"phases\": [\n"
    "  {\"title\": \"find\", \"agents\": [{\"label\": \"api\", \"prompt\": \"List the HTTP handlers in src/app/server.c for {{args}}\"},\n"
    "                               {\"label\": \"docs\", \"prompt\": \"Find where docs/ describe {{args}}\"}]},\n"
    "  {\"title\": \"compare\", \"agents\": [{\"label\": \"diff\", \"prompt\": \"Compare these findings: {{results}}\"}],\n"
    "   \"then\": \"Write one report from: {{results}}\"}]}\n"
    "agent(...) becomes an entry in agents, pipeline(...) becomes consecutive phases, parallel(...) becomes several agents in one phase (they run one after another)";

static void ct_workflow(const char *js, const char *end, char *out, size_t cap) {
    char *src = NULL, *args = NULL;
    char err[600];
    {   /* fonte: script, scriptPath ou name */
        const char *a, *b;
        char *sp = ct_str(js, end, "scriptPath"), *nm = ct_str(js, end, "name");
        if (ct_find(js, end, "script", &a, &b)) {
            if (*a == '"') src = ct_unquote(a, b, NULL);
            else if (*a == '{') { src = (char *)malloc((size_t)(b - a) + 1); memcpy(src, a, (size_t)(b - a)); src[b - a] = 0; }
        } else if (sp || nm) {
            char path[1200];
            if (sp) snprintf(path, sizeof path, "%s", sp);
            else {
                snprintf(path, sizeof path, ".katana/workflows/%s.json", nm);
                struct stat st;
                if (stat(path, &st) != 0 && getenv("HOME")) snprintf(path, sizeof path, "%s/.katana/workflows/%s.json", getenv("HOME"), nm);
            }
            FILE *f = fopen(path, "rb");
            if (!f) { snprintf(out, cap, "error: cannot read workflow %s (%s). %s", path, strerror(errno), sp ? "Check scriptPath" : "Saved workflows are .katana/workflows/<name>.json"); free(sp); free(nm); return; }
            Buf b2 = {0}; char t[4096]; size_t n;
            while ((n = fread(t, 1, sizeof t, f)) > 0) buf_add(&b2, t, n);
            fclose(f); buf_add(&b2, "", 1);
            src = b2.p;
        }
        free(sp); free(nm);
        if (ct_find(js, end, "args", &a, &b)) {
            if (*a == '"') args = ct_unquote(a, b, NULL);
            else { args = (char *)malloc((size_t)(b - a) + 1); memcpy(args, a, (size_t)(b - a)); args[b - a] = 0; }
        }
        if (!args) args = strdup("");
    }
    if (!src) { snprintf(out, cap, "error: give script (inline JSON), scriptPath or name.\n%s", WF_FORM); free(args); return; }
    const char *s = src;
    while (isspace((unsigned char)*s)) s++;
    const char *se = s + strlen(s);
    if (*s != '{') {
        if (wf_is_js(s)) snprintf(out, cap, "error: this is JavaScript; katana has no JavaScript interpreter. %s", WF_FORM);
        else snprintf(out, cap, "error: script is not a JSON object. %s", WF_FORM);
        free(src); free(args); return;
    }
    const char *ps, *pe;
    if (!ct_skip(s, se) || !wf_key(s, se, "phases", &ps, &pe) || *ps != '[') {
        snprintf(out, cap, "error: the workflow needs a \"phases\" array. %s", WF_FORM); free(src); free(args); return;
    }
    /* conta e valida antes de rodar */
    long maxa = 10;
    { const char *e = getenv("KATANA_WORKFLOW_MAX_AGENTS"); if (e && atol(e) > 0) maxa = atol(e); }
    int total = 0, nph = 0;
    err[0] = 0;
    {
        const char *it = ps + 1, *es, *ee;
        while (wf_next(&it, pe, &es, &ee)) {
            nph++;
            const char *as, *ae;
            if (*es != '{' || !wf_key(es, ee, "agents", &as, &ae) || *as != '[') { snprintf(err, sizeof err, "phase %d has no \"agents\" array", nph); break; }
            const char *it2 = as + 1, *xs, *xe; int na = 0;
            while (wf_next(&it2, ae, &xs, &xe)) {
                na++;
                char *pr = *xs == '{' ? wf_str(xs, xe, "prompt") : NULL;
                if (!pr || !*pr) { snprintf(err, sizeof err, "agent %d of phase %d has no \"prompt\" text", na, nph); free(pr); break; }
                free(pr); total++;
            }
            if (err[0]) break;
            if (!na) { snprintf(err, sizeof err, "phase %d has no agents", nph); break; }
            char *th = wf_str(es, ee, "then");
            if (th && *th) total++;
            free(th);
        }
    }
    if (!err[0] && !nph) snprintf(err, sizeof err, "phases is empty");
    if (err[0]) { snprintf(out, cap, "error: %s. %s", err, WF_FORM); free(src); free(args); return; }
    if (total > maxa) {
        snprintf(out, cap, "error: this workflow needs %d sub-agent runs (agents plus then steps); the limit is %ld per run "
                 "(KATANA_WORKFLOW_MAX_AGENTS). Split it or merge prompts", total, maxa);
        free(src); free(args); return;
    }
    char *wname = wf_str(s, se, "name");
    /* roda */
    const size_t AC = 3400;
    char *ans = (char *)malloc(AC + 1);
    char *prev = strdup("");
    Buf lines = {0};
    int nok = 0, nfail = 0, ph = 0;
    const double tw = now_sec();
    const char *it = ps + 1, *es, *ee;
    while (wf_next(&it, pe, &es, &ee)) {
        ph++;
        char *title = wf_str(es, ee, "title");
        const char *as, *ae;
        wf_key(es, ee, "agents", &as, &ae);
        Buf res = {0};
        const char *it2 = as + 1, *xs, *xe; int na = 0;
        while (wf_next(&it2, ae, &xs, &xe)) {
            na++;
            char *lab = wf_str(xs, xe, "label"), *pr = wf_str(xs, xe, "prompt");
            char lb[120];
            snprintf(lb, sizeof lb, "%s", lab && *lab ? lab : "");
            if (!lb[0]) snprintf(lb, sizeof lb, "phase%d-agent%d", ph, na);
            char *full = wf_subst(pr, args, prev);
            const double t0 = now_sec();
            const int ok = ct_sub_call(full, ans, AC);
            const double dt = now_sec() - t0;
            ct_agents_note(full, ok ? ans : "error");
            char ln[400];
            if (ok) snprintf(ln, sizeof ln, "- %s%s%s: ok, %.0f s\n", title ? title : "", title ? " / " : "", lb, dt);
            else snprintf(ln, sizeof ln, "- %s%s%s: failed, %.0f s (%.200s)\n", title ? title : "", title ? " / " : "", lb, dt, ans);
            buf_add(&lines, ln, strlen(ln));
            if (ok) nok++; else nfail++;
            char hd[200];
            snprintf(hd, sizeof hd, "%s### %s%s\n", res.n ? "\n" : "", lb, ok ? "" : " (failed)");
            buf_add(&res, hd, strlen(hd));
            buf_add(&res, ok ? ans : "(no answer)", strlen(ok ? ans : "(no answer)"));
            buf_add(&res, "\n", 1);
            free(full); free(lab); free(pr);
        }
        buf_add(&res, "", 1);
        char *th = wf_str(es, ee, "then");
        free(prev);
        if (th && *th) {
            char *full = wf_subst(th, args, res.p);
            const double t0 = now_sec();
            const int ok = ct_sub_call(full, ans, AC);
            const double dt = now_sec() - t0;
            ct_agents_note(full, ok ? ans : "error");
            char ln[400];
            snprintf(ln, sizeof ln, "- %s%sthen: %s, %.0f s\n", title ? title : "", title ? " / " : "", ok ? "ok" : "failed", dt);
            buf_add(&lines, ln, strlen(ln));
            if (ok) nok++; else nfail++;
            prev = ok ? strdup(ans) : strdup(res.p);      /* sem then valido, fica a juncao das respostas */
            free(full);
        } else prev = strdup(res.p);
        free(th); free(res.p); free(title);
    }
    buf_add(&lines, "", 1);
    /* resultado: relatorio da ultima fase e uma linha por agente (as linhas vao inteiras; o relatorio e cortado se preciso) */
    const size_t ll = strlen(lines.p);
    char head[300];
    snprintf(head, sizeof head, "[workflow %s: %d phases, %d sub-agent runs in sequence, %d ok, %d failed, %.0f s]\n",
             wname ? wname : "(unnamed)", ph, nok + nfail, nok, nfail, now_sec() - tw);
    const size_t hl = strlen(head);
    long room = (long)cap - (long)hl - (long)ll - 40;
    const size_t pl = strlen(prev);
    if (room < 0) room = 0;
    snprintf(out, cap, "%s%.*s%s\n[runs]\n%s", head, (int)((long)pl > room ? room : (long)pl), prev, (long)pl > room ? "\n[report cut]" : "", lines.p);
    { size_t l = strlen(out); if (l && out[l - 1] == '\n') out[l - 1] = 0; }
    free(lines.p); free(prev); free(ans); free(wname); free(src); free(args);
}
