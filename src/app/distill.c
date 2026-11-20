/* ---------- katana distill: um modelo professor maior trabalha com as ferramentas do proprio katana ----------
 * O professor e qualquer servidor com /chat/completions no formato da OpenAI e chamada de ferramentas (um modelo
 * aberto como Kimi, DeepSeek ou GLM, na API oficial ou num provedor que hospede os pesos abertos; ou um servidor
 * local). Ele recebe a tarefa, o prompt de sistema real do agente e a lista de ferramentas do katana; cada chamada
 * dele roda nesta maquina pelo proprio katana (agent_tool, o mesmo codigo do agente), na pasta atual. No fim a
 * sessao vira uma linha JSONL no formato de mensagens, com o prompt de sistema que o ALUNO ve; o katana import
 * converte para o template do aluno e o katana train especializa. O codebench (run.py --teacher) roda isto em
 * cada tarefa numa copia limpa do projeto e marca passed com os testes escondidos.
 *
 * Rede: o pedido sai pelo curl do sistema (como as ferramentas web do agente); a chave vem da variavel de ambiente
 * indicada (--key-env, padrao TEACHER_API_KEY) e vai num arquivo de cabecalho com permissao so do dono, nunca na
 * linha de comando. A memoria salva do usuario nao entra no prompt (HOME vazio enquanto o prompt e montado), e as
 * ferramentas que falam com o usuario ou gravam memoria (ask_user, remember) nao sao oferecidas ao professor. */

/* extrai do prompt do agente (formato json) as declaracoes de ferramenta e a versao do prompt para o professor */
static void ds_split_sys(const char *sys, Buf *tools, Buf *tsys) {
    const char *a = strstr(sys, "<tools>\n"), *b = a ? strstr(a, "\n</tools>") : NULL;
    buf_str(tools, "[");
    int first = 1;
    if (a && b) for (const char *p = a + 8; p < b; ) {
        const char *e = memchr(p, '\n', (size_t)(b - p));
        if (!e) e = b;
        char line[4096];
        const size_t n = (size_t)(e - p) < sizeof line - 1 ? (size_t)(e - p) : sizeof line - 1;
        memcpy(line, p, n); line[n] = 0;
        const int skip = !n || strstr(line, "\"name\": \"ask_user\"") || strstr(line, "\"name\": \"remember\"");
        if (!skip) { if (!first) buf_str(tools, ","); buf_add(tools, p, (size_t)(e - p)); first = 0; }
        p = e + 1;
    }
    buf_str(tools, "]");
    /* prompt do professor: sem o bloco que ensina a sintaxe de chamada do aluno (ele chama as funcoes direto) */
    const char *t0 = strstr(sys, "# Tools\n"), *t1 = b ? strstr(b, "</tool_call>") : NULL;
    if (t0 && t1) {
        t1 += 12;
        buf_add(tsys, sys, (size_t)(t0 - sys));
        buf_str(tsys, "# Tools\n\nUse the functions you are given to act.");
        buf_str(tsys, t1);
    } else buf_str(tsys, sys);
}

/* prompt de sistema do agente no formato pedido, com HOME vazio (sem a memoria salva do usuario) */
static void ds_agent_sys(const char *fmt, char *out, size_t cap) {
    char tmpl[] = "/tmp/katana_distill_XXXXXX";
    const char *h0 = getenv("HOME");
    char *old = h0 ? strdup(h0) : NULL;
#ifndef _WIN32
    if (mkdtemp(tmpl)) setenv("HOME", tmpl, 1);
#endif
    g_tool_xml = !strcmp(fmt, "qwen"); g_tool_glm = !strcmp(fmt, "glm");
    agent_build_sys(out, cap, "");
    if (!strcmp(fmt, "gemma4")) g4_agent_sys(out, cap);
#ifndef _WIN32
    if (old) setenv("HOME", old, 1); else unsetenv("HOME");
    rmdir(tmpl);
#endif
    free(old);
}

/* katana distill --url URL --model M [--key-env VAR] [--format qwen|json|gemma4|glm] [--max-steps N] [--timeout S]
                  [--out arquivo.jsonl] [--task NOME] "tarefa" */
static int distill_main(int argc, char **argv) {
    const char *url = NULL, *model = NULL, *keyenv = "TEACHER_API_KEY", *fmt = "qwen", *outp = "distill.jsonl", *task = "task", *instr = NULL;
    int steps = 40, tmo = 300;
    for (int i = 0; i < argc; i++) {
        if (i + 1 < argc && !strcmp(argv[i], "--url")) url = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--model")) model = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--key-env")) keyenv = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--format")) fmt = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--max-steps")) steps = atoi(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--timeout")) tmo = atoi(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--out")) outp = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--task")) task = argv[++i];
        else if (argv[i][0] != '-' || !argv[i][1]) instr = argv[i];
        else { fprintf(stderr, "opcao desconhecida: %s\n", argv[i]); return 1; }
    }
    if (!url || !model || !instr) {
        fprintf(stderr, "uso: katana distill --url URL --model MODELO [--key-env VAR] [--format qwen|json|gemma4|glm] [--max-steps N] [--out f.jsonl] [--task NOME] \"tarefa\"\n");
        return 1;
    }
#ifdef _WIN32
    fprintf(stderr, "distill: ainda so no Linux\n"); return 1;
#else
    g_web = !(getenv("KATANA_WEB") && strcmp(getenv("KATANA_WEB"), "0") == 0);
    static char sys_s[32000], sys_j[32000], res[TOOL_WEB + 512];
    ds_agent_sys(fmt, sys_s, sizeof sys_s);              /* o que o aluno ve */
    ds_agent_sys("json", sys_j, sizeof sys_j);           /* de onde saem as ferramentas e o prompt do professor */
    g_tool_xml = 0; g_tool_glm = 0;
    Buf tools = {0}, tsys = {0};
    ds_split_sys(sys_j, &tools, &tsys);
    char dir[] = "/tmp/katana_teacher_XXXXXX";
    if (!mkdtemp(dir)) { fprintf(stderr, "distill: sem pasta temporaria\n"); return 1; }
    char hdr[600], body[600], resp[600];
    snprintf(hdr, sizeof hdr, "%s/h", dir); snprintf(body, sizeof body, "%s/b", dir); snprintf(resp, sizeof resp, "%s/r", dir);
    {   /* cabecalhos (com a chave) num arquivo so do dono */
        const char *key = getenv(keyenv);
        const int fd = open(hdr, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) { fprintf(stderr, "distill: nao gravou o cabecalho\n"); return 1; }
        char h[4200];
        const int n = snprintf(h, sizeof h, "Content-Type: application/json\n%s%s%s", key && *key ? "Authorization: Bearer " : "", key && *key ? key : "", key && *key ? "\n" : "");
        if (write(fd, h, (size_t)n) != n) { close(fd); return 1; }
        close(fd);
    }
    /* mensagens em JSON (a do sistema e trocada pela do aluno no fim) */
    Buf msgs = {0};
    buf_str(&msgs, "{\"role\": \"system\", \"content\": \""); js_escape(&msgs, tsys.p, tsys.n); buf_str(&msgs, "\"}");
    buf_str(&msgs, ", {\"role\": \"user\", \"content\": \""); js_escape(&msgs, instr, strlen(instr)); buf_str(&msgs, "\"}");
    long pt = 0, ct = 0; int ncalls = 0, ok = 1;
    char qb[700], qh[700], qr[700];
    shq(body, qb, sizeof qb); shq(hdr, qh, sizeof qh); shq(resp, qr, sizeof qr);
    for (int step = 0; step < steps; step++) {
        Buf req = {0};
        buf_str(&req, "{\"model\": \""); js_escape(&req, model, strlen(model)); buf_str(&req, "\", \"temperature\": 0.6, \"tools\": ");
        buf_add(&req, tools.p, tools.n); buf_str(&req, ", \"messages\": ["); buf_add(&req, msgs.p, msgs.n); buf_str(&req, "]}");
        FILE *fb = fopen(body, "wb");
        if (!fb) { ok = 0; break; }
        fwrite(req.p, 1, req.n, fb); fclose(fb); free(req.p);
        char cmd[5000];
        const size_t ul = strlen(url);
        snprintf(cmd, sizeof cmd, "curl -sS --max-time %d -o %s -w '%%{http_code}' -H @%s --data-binary @%s ", tmo, qr, qh, qb);
        {
            char full[1400]; snprintf(full, sizeof full, "%.*s/chat/completions", (int)(ul && url[ul - 1] == '/' ? ul - 1 : ul), url);
            char qf[2900]; shq(full, qf, sizeof qf);
            strncat(cmd, qf, sizeof cmd - strlen(cmd) - 1);
        }
        int rc = 0;
        char *code = run_collect(cmd, tmo + 10, 4096, &rc);
        Buf r = {0};
        FILE *fr = fopen(resp, "rb");
        if (fr) { char tmp[65536]; size_t n; while ((n = fread(tmp, 1, sizeof tmp, fr)) > 0) buf_add(&r, tmp, n); fclose(fr); }
        if (rc != 0 || !code || strncmp(code, "200", 3) != 0 || !r.p) {
            fprintf(stderr, "distill: o professor respondeu %s (curl %d): %.300s\n", code ? code : "?", rc, r.p ? r.p : "");
            free(code); free(r.p); ok = 0; break;
        }
        free(code);
        /* choices[0].message, usage */
        const char *msg = NULL; char k[64]; const char *v, *e;
        for (const char *p = js_ws(r.p) + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) {
            if (!strcmp(k, "choices") && *v == '[') { const char *el; if (gs_elem(v + 1, &el) && *el == '{') { char k2[64]; const char *v2, *e2; for (const char *p2 = el + 1; (e2 = gs_member(p2, k2, sizeof k2, &v2)); p2 = e2) if (!strcmp(k2, "message")) msg = v2; } }
            else if (!strcmp(k, "usage") && *v == '{') { char k2[64]; const char *v2, *e2; for (const char *p2 = v + 1; (e2 = gs_member(p2, k2, sizeof k2, &v2)); p2 = e2) { if (!strcmp(k2, "prompt_tokens")) pt += atol(v2); else if (!strcmp(k2, "completion_tokens")) ct += atol(v2); } }
        }
        if (!msg || *msg != '{') { fprintf(stderr, "distill: resposta sem choices[0].message: %.300s\n", r.p); free(r.p); ok = 0; break; }
        const char *cv = NULL, *rv = NULL, *tc = NULL, *tce = NULL;
        for (const char *p = msg + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) {
            if (!strcmp(k, "content")) cv = v;
            else if (!strcmp(k, "reasoning_content") && *v == '"') rv = v;
            else if (!strcmp(k, "tool_calls") && *v == '[') { tc = v; tce = e; }
        }
        buf_str(&msgs, ", {\"role\": \"assistant\", \"content\": ");
        if (cv && *cv == '"') { const char *ce = gs_skipv(cv); buf_add(&msgs, cv, (size_t)(ce - cv)); } else buf_str(&msgs, "\"\"");
        if (rv) { const char *re = gs_skipv(rv); buf_str(&msgs, ", \"reasoning_content\": "); buf_add(&msgs, rv, (size_t)(re - rv)); }
        int n = 0;
        if (tc && js_ws(tc + 1)[0] != ']') { buf_str(&msgs, ", \"tool_calls\": "); buf_add(&msgs, tc, (size_t)(tce - tc)); }
        buf_str(&msgs, "}");
        /* executa as chamadas */
        if (tc) for (const char *q = tc + 1, *el, *ee; (ee = gs_elem(q, &el)); q = ee) {
            if (*el != '{') continue;
            char id[200] = "", name[128] = ""; Buf args = {0};
            char k2[64]; const char *v2, *e2;
            for (const char *p2 = el + 1; (e2 = gs_member(p2, k2, sizeof k2, &v2)); p2 = e2) {
                if (!strcmp(k2, "id") && *v2 == '"') { Buf b = {0}; const char *pp = v2; if (js_string(&pp, &b) && b.p) snprintf(id, sizeof id, "%s", b.p); free(b.p); }
                else if (!strcmp(k2, "function") && *v2 == '{') {
                    char k3[64]; const char *v3, *e3;
                    for (const char *p3 = v2 + 1; (e3 = gs_member(p3, k3, sizeof k3, &v3)); p3 = e3) {
                        if (!strcmp(k3, "name") && *v3 == '"') { Buf b = {0}; const char *pp = v3; if (js_string(&pp, &b) && b.p) snprintf(name, sizeof name, "%s", b.p); free(b.p); }
                        else if (!strcmp(k3, "arguments")) {
                            if (*v3 == '"') { const char *pp = v3; buf_clear(&args); js_string(&pp, &args); }
                            else if (*v3 == '{') { const char *ve = gs_skipv(v3); buf_clear(&args); buf_add(&args, v3, (size_t)(ve - v3)); }
                        }
                    }
                }
            }
            if (!args.p) buf_str(&args, "{}");
            if (!strcmp(name, "ask_user") || !strcmp(name, "remember") || !name[0])
                snprintf(res, sizeof res, "error: tool %s is not available here", name[0] ? name : "(sem nome)");
            else agent_tool(name, args.p, args.p + args.n, res, sizeof res);
            fprintf(stderr, "[professor: %s -> %zu bytes]\n", name, strlen(res));
            buf_str(&msgs, ", {\"role\": \"tool\", \"tool_call_id\": \""); js_escape(&msgs, id, strlen(id));
            buf_str(&msgs, "\", \"content\": \""); js_escape(&msgs, res, strlen(res)); buf_str(&msgs, "\"}");
            free(args.p); n++; ncalls++;
        }
        free(r.p);
        if (!n) break;                                   /* resposta sem chamada: o professor terminou */
    }
    unlink(hdr); unlink(body); unlink(resp); rmdir(dir);
    /* registro: o prompt de sistema do aluno no lugar do prompt do professor */
    const char *first_end = strstr(msgs.p, "\"}, {\"role\": \"user\"");
    FILE *fo = fopen(outp, "ab");
    if (!fo || !first_end) { fprintf(stderr, "distill: nao consegui gravar %s\n", outp); if (fo) fclose(fo); return 1; }
    Buf o = {0};
    buf_str(&o, "{\"task\": \""); js_escape(&o, task, strlen(task)); buf_str(&o, "\", \"teacher\": \""); js_escape(&o, model, strlen(model));
    char t[200]; snprintf(t, sizeof t, "\", \"passed\": null, \"complete\": %s, \"tool_calls\": %d, \"usage\": {\"prompt_tokens\": %ld, \"completion_tokens\": %ld}, \"messages\": [",
                          ok ? "true" : "false", ncalls, pt, ct);
    buf_str(&o, t);
    buf_str(&o, "{\"role\": \"system\", \"content\": \""); js_escape(&o, sys_s, strlen(sys_s));
    buf_str(&o, first_end);
    buf_str(&o, "]}\n");
    fwrite(o.p, 1, o.n, fo); fclose(fo);
    printf("distill: %d chamadas de ferramenta, %ld + %ld tokens do professor, sessao %s em %s\n", ncalls, pt, ct, ok ? "completa" : "interrompida", outp);
    free(o.p); free(msgs.p); free(tools.p); free(tsys.p);
    return ok ? 0 : 2;
#endif
}
