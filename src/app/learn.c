/* ---------- katana export e katana learn: o katana aprende com o proprio uso ----------
 * katana export: sessoes do agente (.ksess com o bloco KSX1) -> dados de treino (JSONL com "text" e "train_spans"),
 * o mesmo formato do katana train. Com --bench (resultados do codebench), so as sessoes cujos testes escondidos
 * passaram.
 *
 * katana learn: o laco de auto-aperfeicoamento, tudo local. Em cada rodada:
 *   1. coleta: o modelo atual (base + melhor especializacao ate agora) resolve as tarefas de treino do codebench
 *      varias vezes, com temperatura (amostras diferentes); opcional: um professor resolve as mesmas tarefas;
 *   2. filtro: so as sessoes que passaram nos testes escondidos viram dados (rejeicao por verificacao);
 *   3. treino: katana train em todos os dados acumulados, com validacao separada e o melhor passo;
 *   4. prova: o candidato resolve as tarefas de avaliacao (que nunca entram nos dados); so vira o novo melhor se
 *      resolver mais que o atual.
 * As sessoes rodam com uma pasta pessoal vazia (HOME/USERPROFILE): a memoria do agente e os servidores MCP do dono
 * nao entram nos dados. O codebench (tools/codebench/run.py) e o juiz: roda cada tarefa numa pasta temporaria. */

#ifdef _WIN32
#include <process.h>
#include <io.h>
#endif

/* ---- leitura de sessao ---- */
typedef struct { char *text; size_t n; int exact; int nm; char *role; int32_t *off; char arch[64]; char *tch; int ntch; } KsxSess;

static int ksx_rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }
static int ksx_skip_str(FILE *f) { int32_t l; return ksx_rd(f, &l, 4) && l >= 0 && l < (1 << 28) && fseek(f, l, SEEK_CUR) == 0; }
static char *ksx_str(FILE *f, size_t *len) {
    int32_t l;
    if (!ksx_rd(f, &l, 4) || l < 0 || l >= (1 << 28)) return NULL;
    char *s = xalloc((size_t)l + 1, 1);
    if (!ksx_rd(f, s, (size_t)l)) { free(s); return NULL; }
    s[l] = 0;
    if (len) *len = (size_t)l;
    return s;
}
static int ksx_skip_msgs(FILE *f) {
    int32_t n;
    if (!ksx_rd(f, &n, 4) || n < 0 || n > 1000000) return 0;
    for (int i = 0; i < n; i++) { if (fgetc(f) == EOF || !ksx_skip_str(f)) return 0; }
    return 1;
}
static void ksx_free(KsxSess *s) { free(s->text); free(s->role); free(s->off); free(s->tch); memset(s, 0, sizeof *s); }
/* falas do professor nesta sessao (<sessao>.teacher: indice da mensagem por linha, gravado pelo modo professor) */
static void ksx_teacher(const char *path, KsxSess *s) {
    char p[4400]; snprintf(p, sizeof p, "%s.teacher", path);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    s->tch = xalloc((size_t)s->nm + 1, 1);
    char line[512];
    while (fgets(line, sizeof line, f)) { const int i = atoi(line); if (i >= 0 && i < s->nm && s->role[i] == 'a' && !s->tch[i]) { s->tch[i] = 1; s->ntch++; } }
    fclose(f);
}

static int ksx_read(const char *path, KsxSess *s, const char **why) {
    memset(s, 0, sizeof *s);
    FILE *f = fopen(path, "rb");
    if (!f) { *why = "cannot open"; return 0; }
    uint64_t mg, fp; int64_t t; int32_t nl, ne, nid;
    int ok = ksx_rd(f, &mg, 8) && mg == SESS_MAGIC && ksx_rd(f, &fp, 8) && ksx_rd(f, &t, 8) && ksx_rd(f, &nl, 4) && ksx_rd(f, &ne, 4)
             && nl >= 0 && ne >= 0 && fseek(f, (long)nl * ne * 4, SEEK_CUR) == 0 && ksx_skip_msgs(f)
             && ksx_rd(f, &nid, 4) && nid >= 0 && fseek(f, (long)nid * 4, SEEK_CUR) == 0 && ksx_skip_msgs(f);
    if (!ok) { fclose(f); *why = "not a katana session"; return 0; }
    uint32_t em;
    if (!ksx_rd(f, &em, 4) || em != SESS_EXT_MAGIC) { fclose(f); *why = "written by an older katana (no exact text)"; return 0; }
    int32_t fmt, think, ex, n;
    char *arch = ksx_str(f, NULL);
    ok = arch && ksx_rd(f, &fmt, 4) && ksx_rd(f, &think, 4) && ksx_rd(f, &ex, 4) && ksx_skip_str(f);
    if (arch) { snprintf(s->arch, sizeof s->arch, "%s", arch); free(arch); }
    if (ok) s->text = ksx_str(f, &s->n);
    ok = ok && s->text && ksx_rd(f, &n, 4) && n >= 0 && n <= 1000000;
    if (ok) {
        s->nm = n; s->exact = ex;
        s->role = xalloc((size_t)n + 1, 1); s->off = xalloc((size_t)n * 2 + 1, sizeof(int32_t));
        for (int i = 0; i < n && ok; i++) { const int r = fgetc(f); s->role[i] = (char)r; ok = r != EOF && ksx_rd(f, s->off + 2 * i, 8); }
    }
    fclose(f);
    if (!ok) { ksx_free(s); *why = "session file is cut"; return 0; }
    return 1;
}

/* sessao do agente (ChatML) -> uma linha {"messages": [...]} para o im_line do katana import, que troca as chamadas do
   primeiro conjunto de ferramentas pelas do segundo e monta o texto com o prompt de sistema atual. Cada fala do modelo
   e desmontada: bloco de raciocinio, texto e chamadas (XML ou JSON); cada mensagem de resultados vira uma mensagem
   "tool" por resultado, sem o embrulho (o im_line embrulha de novo, com o nome novo). only_teacher: as outras falas
   do modelo levam "train": false. As trocas de o (--replace) valem em todos os textos. */
static void ksx_jtext(TrOpt *o, Buf *l, const char *s, size_t n, int trim) {
    Buf t = {0};
    buf_add(&t, s, n);
    if (trim) tr_trim(&t);
    tr_subst(o, &t);
    js_escape(l, tr_s(&t), t.n);
    free(t.p);
}
static int ksx_messages(const char *path, TrOpt *o, int only_teacher, Buf *l, const char **why) {
    KsxSess s;
    if (!ksx_read(path, &s, why)) return 0;
    if (strstr(s.text, "<|turn>") || strstr(s.text, "<|assistant|>") || !strstr(s.text, "<|im_start|>")) { ksx_free(&s); *why = "only ChatML sessions are converted"; return 0; }
    ksx_teacher(path, &s);
    static char js[1 << 20];
    int nmsg = 0;
    buf_str(l, "{\"messages\": [");
    for (int i = 0; i < s.nm; i++) {
        if (s.off[2 * i] < 0 || (size_t)s.off[2 * i + 1] > s.n || s.off[2 * i + 1] <= s.off[2 * i]) continue;
        const size_t n = (size_t)(s.off[2 * i + 1] - s.off[2 * i]);
        char *m = xalloc(n + 1, 1);
        memcpy(m, s.text + s.off[2 * i], n);
        const char *p = js_ws(m);
        if (s.role[i] == 'a') {
            buf_str(l, nmsg++ ? ", {\"role\": \"assistant\"" : "{\"role\": \"assistant\"");
            if (!strncmp(p, "<think>", 7)) {
                const char *e = strstr(p, "</think>");
                if (e) {
                    Buf r = {0}; buf_add(&r, p + 7, (size_t)(e - p - 7)); tr_trim(&r);
                    if (r.n) { buf_str(l, ", \"reasoning_content\": \""); ksx_jtext(o, l, r.p, r.n, 0); buf_str(l, "\""); }
                    free(r.p);
                    p = e + 8;
                }
            }
            const char *tc = strstr(p, "<tool_call>");
            buf_str(l, ", \"content\": \"");
            ksx_jtext(o, l, p, tc ? (size_t)(tc - p) : strlen(p), 1);
            buf_str(l, "\"");
            int nc = 0;
            for (; tc; tc = strstr(tc + 11, "<tool_call>")) {
                const char *te = strstr(tc, "</tool_call>");
                if (!te) te = tc + strlen(tc);
                char name[128] = "";
                Buf fn = {0};
                if (tool_xml_parse(tc, te, name, sizeof name, js, sizeof js) && name[0]) {
                    const char *ob = strchr(js, '{');
                    buf_str(&fn, "{\"name\": \""); js_escape(&fn, name, strlen(name)); buf_str(&fn, "\", \"arguments\": ");
                    if (ob) tr_json_subst(o, ob, &fn); else buf_str(&fn, "{}");
                    buf_str(&fn, "}");
                } else {                                   /* chamada em JSON: {"name": .., "arguments": {..}} */
                    const char *ob = js_ws(tc + 11);
                    if (*ob == '{' && gs_skipv(ob) && gs_skipv(ob) <= te) tr_json_subst(o, ob, &fn);
                }
                if (fn.n) {
                    buf_str(l, nc++ ? ", " : ", \"tool_calls\": [");
                    buf_str(l, "{\"type\": \"function\", \"function\": "); buf_add(l, fn.p, fn.n); buf_str(l, "}");
                }
                free(fn.p);
            }
            if (nc) buf_str(l, "]");
            if (only_teacher && !(s.tch && s.tch[i])) buf_str(l, ", \"train\": false");
            buf_str(l, "}");
        } else if (!strncmp(p, "<tool_response>", 15)) {
            const char *rest = p;
            for (const char *q = p; q && !strncmp(js_ws(q), "<tool_response>", 15); ) {
                q = js_ws(q) + 15;
                const char *close = strstr(q, "</tool_response>");
                const char *end = close ? close : q + strlen(q);
                char name[128] = "tool";
                const char *body = q, *bend = end;
                if (*body == '\n') body++;
                static const char H[] = "[resultado da ferramenta ", F[] = "\n[fim do resultado da ferramenta ";
                if (!strncmp(body, H, sizeof H - 1)) {
                    const char *nm = body + sizeof H - 1, *ne = nm;
                    while (*ne && *ne != ',' && *ne != ']' && *ne != '\n') ne++;
                    snprintf(name, sizeof name, "%.*s", (int)(ne - nm < 120 ? ne - nm : 120), nm);
                    const char *nl = strchr(body, '\n');
                    body = nl && nl < end ? nl + 1 : end;
                    for (const char *f = end - (sizeof F - 1); f >= body - 1 && f >= q; f--) if (!strncmp(f, F, sizeof F - 1)) { bend = f; break; }
                    if (bend < body) bend = body;
                }
                buf_str(l, nmsg++ ? ", {\"role\": \"tool\", \"name\": \"" : "{\"role\": \"tool\", \"name\": \"");
                js_escape(l, name, strlen(name));
                buf_str(l, "\", \"content\": \"");
                ksx_jtext(o, l, body, (size_t)(bend - body), 0);
                buf_str(l, "\"}");
                rest = q = close ? close + 16 : NULL;
                if (!q) rest = end;
            }
            const char *r2 = js_ws(rest);
            if (*r2) { buf_str(l, ", {\"role\": \"user\", \"content\": \""); ksx_jtext(o, l, r2, strlen(r2), 1); buf_str(l, "\"}"); nmsg++; }
        } else {
            buf_str(l, nmsg++ ? ", {\"role\": \"user\", \"content\": \"" : "{\"role\": \"user\", \"content\": \"");
            ksx_jtext(o, l, m, n, 1);
            buf_str(l, "\"}");
        }
        free(m);
    }
    buf_str(l, "]}");
    ksx_free(&s);
    if (!nmsg) { *why = "the session has no messages"; return 0; }
    return 1;
}

/* texto -> UTF-8 valido (bytes invalidos viram U+FFFD) com o mapa de posicoes antigas para novas */
static char *ksx_clean(const char *s, size_t n, size_t *on, size_t **map) {
    char *o = xalloc(n * 3 + 8, 1); size_t *mp = xalloc(n + 1, sizeof(size_t)), k = 0;
    for (size_t i = 0; i < n; ) {
        mp[i] = k;
        const size_t q = (unsigned char)s[i] < 0x80 ? 1 : utf8_seq((const unsigned char *)s + i, n - i);
        if (!q) { memcpy(o + k, "\xEF\xBF\xBD", 3); k += 3; i++; continue; }
        for (size_t j = 1; j < q; j++) mp[i + j] = k;
        memcpy(o + k, s + i, q); k += q; i += q;
    }
    mp[n] = k; o[k] = 0;
    *on = k; *map = mp;
    return o;
}

static const char *const KSX_STOPS[] = { "<|im_end|>", "<|tool_response>", "<turn|>", "<|observation|>", "<|user|>" };
static const char *ksx_eot(const char *text) {
    if (strstr(text, "<|turn>")) return "<turn|>";
    if (strstr(text, "<|assistant|>")) return "<|user|>";
    return "<|im_end|>";
}

/* uma sessao -> uma linha de dados; 0 se nao tem fala do modelo */
static int ksx_line(const KsxSess *s, const char *source, FILE *out, int only_teacher) {
    size_t n; size_t *map;
    char *t = ksx_clean(s->text, s->n, &n, &map);
    long *sp = xalloc((size_t)s->nm * 2 + 2, sizeof(long)); int ns = 0;
    for (int i = 0; i < s->nm; i++) {
        if (s->role[i] != 'a' || s->off[2 * i] < 0 || (size_t)s->off[2 * i + 1] > s->n) continue;
        if (only_teacher && !(s->tch && s->tch[i])) continue;      /* so as falas do professor contam para a perda */
        if (s->off[2 * i + 1] == s->off[2 * i]) continue;           /* fala vazia (interrompida) */
        const size_t a = map[s->off[2 * i]], b = map[s->off[2 * i + 1]];
        size_t end = b;
        int hit = 0;
        for (size_t k = 0; k < sizeof KSX_STOPS / sizeof KSX_STOPS[0] && !hit; k++) {
            const size_t l = strlen(KSX_STOPS[k]);
            if (b + l <= n && !memcmp(t + b, KSX_STOPS[k], l)) { end = b + l; hit = 1; }
        }
        if (!hit && i == s->nm - 1 && b == n) {           /* a ultima fala nao tem o fim de turno: acrescenta */
            const char *eot = ksx_eot(t);
            const size_t l = strlen(eot);
            t = realloc(t, n + l + 1); memcpy(t + n, eot, l + 1); n += l; end = n;
        }
        sp[2 * ns] = im_chars(t, a); sp[2 * ns + 1] = im_chars(t, end); ns++;
    }
    if (!ns) { free(t); free(map); free(sp); return 0; }
    Buf o = {0};
    buf_str(&o, "{\"text\": \"");
    js_escape(&o, t, n);
    buf_str(&o, "\", \"train_spans\": [");
    for (int i = 0; i < ns; i++) { char x[64]; snprintf(x, sizeof x, "%s[%ld, %ld]", i ? ", " : "", sp[2 * i], sp[2 * i + 1]); buf_str(&o, x); }
    buf_str(&o, "], \"source\": \"");
    js_escape(&o, source, strlen(source));
    buf_str(&o, s->exact ? "\", \"exact\": true}\n" : "\", \"exact\": false}\n");
    io_write(out, o.p, o.n, g_im_outpath);
    free(o.p); free(t); free(map); free(sp);
    return 1;
}

/* caminhos das sessoes que passaram, a partir dos resultados do codebench ({"session": ..., "passed": true}) */
static int ksx_bench(const char *path, char ***list) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int n = 0;
    char *line = NULL; size_t cap = 0;
    for (;;) {
        size_t k = 0; int ch;
        while ((ch = fgetc(f)) != EOF && ch != '\n') { if (k + 2 >= cap) { cap = cap * 2 + 4096; line = realloc(line, cap); } line[k++] = (char)ch; }
        if (line) line[k] = 0;
        if (k) {
            char key[64]; const char *v, *e, *q = js_ws(line);
            int passed = 0; char sess[2048] = "";
            if (*q == '{') for (q++; (e = gs_member(q, key, sizeof key, &v)); q = e) {
                if (!strcmp(key, "passed")) passed = !strncmp(v, "true", 4);
                else if (!strcmp(key, "session") && *v == '"') { const char *p = v; Buf b = {0}; if (js_string(&p, &b) && b.p) snprintf(sess, sizeof sess, "%s", b.p); free(b.p); }
            }
            if (passed && sess[0]) { *list = realloc(*list, sizeof(char *) * (size_t)(n + 1)); (*list)[n++] = strdup(sess); }
        }
        if (ch == EOF) break;
    }
    free(line); fclose(f);
    return n;
}

/* katana export <saida.jsonl> <sessoes ou pastas...> [--bench resultados.jsonl] [--inexact] [--append] */
static int export_main(int argc, char **argv) {
    const char *outp = NULL, *bench = NULL; int inexact = 0, append = 0, teacher = 0;
    char **paths = NULL; int np = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench = argv[++i];
        else if (!strcmp(argv[i], "--inexact")) inexact = 1;
        else if (!strcmp(argv[i], "--append")) append = 1;
        else if (!strcmp(argv[i], "--teacher")) teacher = 1;
        else if (!outp) outp = argv[i];
        else { paths = realloc(paths, sizeof(char *) * (size_t)(np + 1)); paths[np++] = strdup(argv[i]); }
    }
    if (!outp || (!np && !bench)) { fprintf(stderr, "uso: katana export <saida.jsonl> <sessoes.ksess ou pastas...> [--bench resultados.jsonl] [--inexact] [--append] [--teacher]\n"); return 1; }
    if (bench) {                                       /* com --bench: so as que passaram (e, se houver caminhos, so as listadas) */
        char **ok = NULL; const int n = ksx_bench(bench, &ok);
        if (n < 0) { fprintf(stderr, "export: nao abriu %s\n", bench); return 1; }
        for (int i = 0; i < np; i++) free(paths[i]);
        free(paths); paths = ok; np = n;
    } else {                                           /* pastas: todos os .ksess dentro */
        char **all = NULL; int na = 0;
        for (int i = 0; i < np; i++) {
            const size_t l = strlen(paths[i]);
            if (l > 6 && !strcmp(paths[i] + l - 6, ".ksess")) { all = realloc(all, sizeof(char *) * (size_t)(na + 1)); all[na++] = paths[i]; continue; }
#ifdef _WIN32
            char pat[2100]; WIN32_FIND_DATAA fd;
            snprintf(pat, sizeof pat, "%s\\*.ksess", paths[i]);
            HANDLE h = FindFirstFileA(pat, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do { char full[2400]; snprintf(full, sizeof full, "%s\\%s", paths[i], fd.cFileName); all = realloc(all, sizeof(char *) * (size_t)(na + 1)); all[na++] = strdup(full); } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
#else
            DIR *d = opendir(paths[i]);
            if (d) {
                struct dirent *de;
                while ((de = readdir(d))) {
                    const size_t k = strlen(de->d_name);
                    if (k > 6 && !strcmp(de->d_name + k - 6, ".ksess")) { char full[2400]; snprintf(full, sizeof full, "%s/%s", paths[i], de->d_name); all = realloc(all, sizeof(char *) * (size_t)(na + 1)); all[na++] = strdup(full); }
                }
                closedir(d);
            }
#endif
            free(paths[i]);
        }
        free(paths); paths = all; np = na;
    }
    FILE *out = fopen(outp, append ? "ab" : "wb");
    if (!out) { fprintf(stderr, "export: nao consegui gravar %s: %s\n", outp, strerror(errno)); return 1; }
    g_im_outpath = outp; g_io_msgs = 0;
    int wrote = 0, skipped = 0;
    for (int i = 0; i < np; i++) {
        KsxSess s; const char *why = NULL;
        if (!ksx_read(paths[i], &s, &why)) { fprintf(stderr, "export: %s: %s\n", paths[i], why); skipped++; free(paths[i]); continue; }
        if (teacher) ksx_teacher(paths[i], &s);
        if (teacher && !s.ntch) skipped++;                 /* --teacher: so sessoes com falas do professor; ele viu a conversa inteira, compactada ou nao */
        else if (teacher) { if (ksx_line(&s, paths[i], out, 1)) wrote++; else skipped++; }
        else if (!s.exact && !inexact) { fprintf(stderr, "export: %s: the agent compacted its context in this session (use --inexact to keep it)\n", paths[i]); skipped++; }
        else if (ksx_line(&s, paths[i], out, 0)) wrote++;
        else skipped++;
        ksx_free(&s); free(paths[i]);
    }
    free(paths);
    if (!io_close(out, outp) || g_io_msgs) { fprintf(stderr, "export: a gravacao de %s falhou; o arquivo esta incompleto\n", outp); return 4; }
    printf("export: %d sessoes -> %s (%d puladas)\n", wrote, outp, skipped);
    return 0;
}

/* ---- katana learn ---- */
/* roda um programa com argumentos (sem shell, sem problema de aspas); stdout vai para outp (ou fica no terminal) */
static int lr_run(char *const *argv, const char *outp) {
    fflush(stdout); fflush(stderr);
#ifdef _WIN32
    /* o _spawnvp junta os argumentos com espaco: os que tem espaco ou aspas vao entre aspas */
    int n = 0; while (argv[n]) n++;
    char **q = xalloc((size_t)n + 1, sizeof(char *));
    for (int i = 0; i < n; i++) {
        const char *a = argv[i];
        if (*a && !strpbrk(a, " \t\"")) { q[i] = strdup(a); continue; }
        Buf b = {0}; buf_str(&b, "\"");
        for (const char *p = a; ; p++) {
            size_t bs = 0; while (*p == '\\') { bs++; p++; }
            if (!*p) { for (size_t k = 0; k < bs * 2; k++) buf_add(&b, "\\", 1); break; }
            if (*p == '"') { for (size_t k = 0; k < bs * 2 + 1; k++) buf_add(&b, "\\", 1); buf_add(&b, "\"", 1); }
            else { for (size_t k = 0; k < bs; k++) buf_add(&b, "\\", 1); buf_add(&b, p, 1); }
        }
        buf_str(&b, "\""); q[i] = b.p;
    }
    int saved = -1;
    if (outp) {
        FILE *f = fopen(outp, "wb");
        if (!f) { for (int i = 0; i < n; i++) free(q[i]); free(q); return -1; }
        saved = _dup(1); _dup2(_fileno(f), 1); fclose(f);
    }
    const intptr_t rc = _spawnvp(_P_WAIT, q[0], (const char *const *)q);
    if (outp) { fflush(stdout); _dup2(saved, 1); _close(saved); }
    for (int i = 0; i < n; i++) free(q[i]);
    free(q);
    return (int)rc;
#else
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (outp) { FILE *f = fopen(outp, "wb"); if (!f) _exit(127); dup2(fileno(f), 1); }
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}

/* caminho do proprio executavel (para chamar katana train, katana import) */
static void lr_self(char *out, size_t cap, const char *argv0) {
#ifdef _WIN32
    if (!GetModuleFileNameA(NULL, out, (DWORD)cap)) snprintf(out, cap, "%s", argv0);
#else
    const ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n > 0) out[n] = 0; else snprintf(out, cap, "%s", argv0);
#endif
}

static int lr_mkdir(const char *p) {
#ifdef _WIN32
    return CreateDirectoryA(p, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir(p, 0700) == 0 || errno == EEXIST;
#endif
}

/* le um arquivo de resultados do codebench: passou / rodou (as puladas, como noticias sem internet, nao contam) */
static void lr_score(const char *path, int *pass, int *runs) {
    *pass = *runs = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[16384];
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, "\"skipped\": true")) continue;
        if (!strstr(line, "\"passed\"")) continue;
        (*runs)++;
        if (strstr(line, "\"passed\": true")) (*pass)++;
    }
    fclose(f);
}

/* copia src para dst (mesmo conteudo); append: no fim de dst */
static long lr_copy(const char *src, const char *dst, int append) {
    FILE *a = fopen(src, "rb");
    if (!a) return -1;
    FILE *b = fopen(dst, append ? "ab" : "wb");
    if (!b) { fclose(a); return -1; }
    char buf[65536]; size_t n; long lines = 0;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) { for (size_t i = 0; i < n; i++) lines += buf[i] == '\n'; fwrite(buf, 1, n, b); }
    fclose(a); fclose(b);
    return lines;
}

/* lista as tarefas do codebench (pastas em tasks/), em ordem */
static int lr_tasks(const char *dir, char ***out) {
    int n = 0;
#ifdef _WIN32
    char pat[2100]; WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.cFileName[0] == '.' || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        *out = realloc(*out, sizeof(char *) * (size_t)(n + 1)); (*out)[n++] = strdup(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[2400]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (stat(p, &st) || !S_ISDIR(st.st_mode)) continue;
        *out = realloc(*out, sizeof(char *) * (size_t)(n + 1)); (*out)[n++] = strdup(de->d_name);
    }
    closedir(d);
#endif
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && strcmp((*out)[j - 1], (*out)[j]) > 0; j--) { char *t = (*out)[j]; (*out)[j] = (*out)[j - 1]; (*out)[j - 1] = t; }
    return n;
}

static int lr_in(const char *list, const char *name) {           /* name esta na lista separada por virgulas? */
    const size_t l = strlen(name);
    for (const char *p = list; p && *p; ) {
        const char *e = strchr(p, ','); const size_t k = e ? (size_t)(e - p) : strlen(p);
        if (k == l && !strncmp(p, name, l)) return 1;
        p = e ? e + 1 : NULL;
    }
    return 0;
}

/* katana learn <modelo.gguf> [opcoes] [-- opcoes do katana train] */
static int learn_main(const char *argv0, int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "uso: katana learn <modelo.gguf> [--dir PASTA] [--rounds N] [--repeat K] [--tasks a,b] [--eval-tasks c,d] [--eval-repeat K] [--temp T] [--teacher URL --teacher-model M] [--extra dados.jsonl] [--python python3] [--bench tools/codebench] [--timeout s] [-- opcoes do katana train]\n"); return 1; }
    const char *model = argv[0], *tasks_opt = NULL, *eval_opt = NULL, *teacher = NULL, *tmodel = NULL, *tfmt = "qwen", *tkey = "TEACHER_API_KEY";
    const char *extra = NULL, *py = NULL, *bench = "tools/codebench", *timeout = "900", *temp = "0.7";
    char dir[1100]; snprintf(dir, sizeof dir, "%s.learn", model);
    int rounds = 3, repeat = 4, erepeat = 1, ntrain_args = 0;
    char **train_args = NULL;
    for (int i = 1; i < argc; i++) {
        const char *k = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(k, "--")) { train_args = argv + i + 1; ntrain_args = argc - i - 1; break; }
        if (!v) { fprintf(stderr, "learn: falta o valor de %s\n", k); return 1; }
        i++;
        if (!strcmp(k, "--dir")) snprintf(dir, sizeof dir, "%s", v);
        else if (!strcmp(k, "--rounds")) rounds = atoi(v);
        else if (!strcmp(k, "--repeat")) repeat = atoi(v);
        else if (!strcmp(k, "--eval-repeat")) erepeat = atoi(v);
        else if (!strcmp(k, "--tasks")) tasks_opt = v;
        else if (!strcmp(k, "--eval-tasks")) eval_opt = v;
        else if (!strcmp(k, "--temp")) temp = v;
        else if (!strcmp(k, "--teacher")) teacher = v;
        else if (!strcmp(k, "--teacher-model")) tmodel = v;
        else if (!strcmp(k, "--teacher-format")) tfmt = v;
        else if (!strcmp(k, "--teacher-key-env")) tkey = v;
        else if (!strcmp(k, "--extra")) extra = v;
        else if (!strcmp(k, "--python")) py = v;
        else if (!strcmp(k, "--bench")) bench = v;
        else if (!strcmp(k, "--timeout")) timeout = v;
        else { fprintf(stderr, "learn: opcao desconhecida %s\n", k); return 1; }
    }
    if (rounds < 1 || repeat < 1 || erepeat < 1) { fprintf(stderr, "learn: --rounds, --repeat e --eval-repeat precisam ser >= 1\n"); return 1; }
    if (teacher && !tmodel) { fprintf(stderr, "learn: --teacher precisa de --teacher-model\n"); return 1; }
#ifdef _WIN32
    if (!py) py = "python";
#else
    if (!py) py = "python3";
#endif
    char self[2048]; lr_self(self, sizeof self, argv0);
    char runpy[2100], tdir[2100];
    snprintf(runpy, sizeof runpy, "%s/run.py", bench); snprintf(tdir, sizeof tdir, "%s/tasks", bench);
    /* tarefas: as de avaliacao nunca entram nos dados. Padrao: uma em cada quatro (em ordem alfabetica) avalia */
    char **all = NULL; const int na = lr_tasks(tdir, &all);
    if (!na) { fprintf(stderr, "learn: nenhuma tarefa em %s (use --bench)\n", tdir); return 1; }
    Buf tr = {0}, ev = {0};
    for (int i = 0; i < na; i++) {
        const int is_eval = eval_opt ? lr_in(eval_opt, all[i]) : i % 4 == 3;
        const int is_train = !is_eval && (!tasks_opt || lr_in(tasks_opt, all[i]));
        Buf *b = is_eval ? &ev : is_train ? &tr : NULL;
        if (!b) continue;
        if (b->n) buf_str(b, ",");
        buf_str(b, all[i]);
    }
    if (!tr.n || !ev.n) { fprintf(stderr, "learn: precisa de tarefas de treino e de avaliacao (treino: %s; avaliacao: %s)\n", tr.n ? tr.p : "-", ev.n ? ev.p : "-"); return 1; }
    if (!lr_mkdir(dir)) { fprintf(stderr, "learn: nao criou %s\n", dir); return 1; }
    char home[1200], data[1200], logp[1200], best[1200];
    snprintf(home, sizeof home, "%s/home", dir); lr_mkdir(home);
    snprintf(data, sizeof data, "%s/data.jsonl", dir); snprintf(logp, sizeof logp, "%s/learn.jsonl", dir); snprintf(best, sizeof best, "%s/best.kspec", dir);
    char env_home[1300], env_up[1300], env_temp[64];
    snprintf(env_home, sizeof env_home, "HOME=%s", home); snprintf(env_up, sizeof env_up, "USERPROFILE=%s", home);
    snprintf(env_temp, sizeof env_temp, "KATANA_TEMP=%s", temp);
    printf("learn: %s\n  tarefas de treino: %s\n  tarefas de avaliacao (nunca viram dados): %s\n  %d rodadas, %d tentativas por tarefa (temperatura %s), pasta %s\n",
           model, tr.p, ev.p, rounds, repeat, temp, dir);
    FILE *lg = fopen(logp, "ab");
    if (!lg) { fprintf(stderr, "learn: nao abriu %s\n", logp); return 1; }
    char rep_s[16], erep_s[16]; snprintf(rep_s, sizeof rep_s, "%d", repeat); snprintf(erep_s, sizeof erep_s, "%d", erepeat);
    /* rodada 0: o modelo base nas tarefas de avaliacao */
    char evp[1300]; snprintf(evp, sizeof evp, "%s/r0_eval.jsonl", dir);
    int champ_pass = 0, champ_runs = 0, have_best = 0;
    {
        remove(evp);
        char *av[] = { (char *)py, runpy, "--model", (char *)model, "--katana", self, "--tasks", ev.p, "--repeat", erep_s, "--timeout", (char *)timeout,
                       "--label", "base", "--out", evp, "--env", env_home, "--env", env_up, NULL };
        printf("\nrodada 0: modelo base nas tarefas de avaliacao\n");
        if (lr_run(av, NULL) != 0) fprintf(stderr, "learn: o codebench terminou com erro (seguindo com o que gravou)\n");
        lr_score(evp, &champ_pass, &champ_runs);
        printf("rodada 0: base resolveu %d de %d\n", champ_pass, champ_runs);
        fprintf(lg, "{\"round\": 0, \"eval_pass\": %d, \"eval_runs\": %d, \"spec\": null}\n", champ_pass, champ_runs); fflush(lg);
    }
    if (extra) { const long n = lr_copy(extra, data, 1); printf("dados extras: %ld linhas de %s\n", n, extra); }
    for (int r = 1; r <= rounds; r++) {
        char rd[1300], sess[1400], col[1400], rdata[1400], cand[1400], tlog[1400], tout[1400], env_spec[1500];
        snprintf(rd, sizeof rd, "%s/r%d", dir, r); lr_mkdir(rd);
        snprintf(sess, sizeof sess, "%s/sessions", rd); snprintf(col, sizeof col, "%s/collect.jsonl", rd);
        snprintf(rdata, sizeof rdata, "%s/data.jsonl", rd); snprintf(cand, sizeof cand, "%s/cand.kspec", rd);
        snprintf(tlog, sizeof tlog, "%s/train_log.jsonl", rd); snprintf(tout, sizeof tout, "%s/train_out.txt", rd);
        snprintf(env_spec, sizeof env_spec, "KATANA_SPEC=%s", have_best ? best : "");
        /* 1. coleta com o melhor modelo ate agora */
        printf("\nrodada %d: coleta (%s)\n", r, have_best ? "base + melhor especializacao" : "modelo base");
        remove(col);
        {
            char *av[] = { (char *)py, runpy, "--model", (char *)model, "--katana", self, "--tasks", tr.p, "--repeat", rep_s, "--timeout", (char *)timeout,
                           "--label", "collect", "--out", col, "--sessions", sess, "--env", env_home, "--env", env_up, "--env", env_temp, "--env", env_spec, NULL };
            if (lr_run(av, NULL) != 0) fprintf(stderr, "learn: o codebench terminou com erro (seguindo com o que gravou)\n");
        }
        int cpass = 0, cruns = 0; lr_score(col, &cpass, &cruns);
        /* 2. so as que passaram viram dados */
        remove(rdata);
        { char *av[] = { self, "export", rdata, "--bench", col, NULL }; lr_run(av, NULL); }
        long nnew = lr_copy(rdata, data, 1); if (nnew < 0) nnew = 0;
        long tpass = 0;
        if (teacher) {                                   /* professor: resolve as mesmas tarefas; as aprovadas viram dados */
            char tsess[1400], truns[1400], tdata[1400];
            snprintf(tsess, sizeof tsess, "%s/teacher.jsonl", rd); snprintf(truns, sizeof truns, "%s/teacher_runs.jsonl", rd); snprintf(tdata, sizeof tdata, "%s/teacher_data.jsonl", rd);
            remove(tsess); remove(truns);
            char *av[] = { (char *)py, runpy, "--teacher", (char *)teacher, "--teacher-model", (char *)tmodel, "--teacher-format", (char *)tfmt,
                           "--teacher-key-env", (char *)tkey, "--katana", self, "--tasks", tr.p, "--timeout", (char *)timeout, "--label", "teacher",
                           "--out", truns, "--distill-out", tsess, "--env", env_home, "--env", env_up, NULL };
            printf("rodada %d: professor %s\n", r, tmodel);
            if (lr_run(av, NULL) != 0) fprintf(stderr, "learn: o professor terminou com erro (seguindo com o que gravou)\n");
            char *ai[] = { self, "import", (char *)model, tsess, tdata, NULL };
            if (lr_run(ai, NULL) == 0) { tpass = lr_copy(tdata, data, 1); if (tpass < 0) tpass = 0; }
        }
        long ntot = 0; { FILE *f = fopen(data, "rb"); if (f) { int ch; while ((ch = fgetc(f)) != EOF) ntot += ch == '\n'; fclose(f); } }
        printf("rodada %d: o modelo resolveu %d de %d tentativas; %ld sessoes novas%s; %ld conversas nos dados\n", r, cpass, cruns, nnew,
               teacher ? " (mais as do professor)" : "", ntot);
        if (!ntot) {
            printf("rodada %d: sem dados para treinar ainda\n", r);
            fprintf(lg, "{\"round\": %d, \"collect_pass\": %d, \"collect_runs\": %d, \"data\": 0, \"accepted\": false}\n", r, cpass, cruns); fflush(lg);
            continue;
        }
        /* 3. treino em todos os dados acumulados (sempre a partir do modelo base) */
        {
            const int na2 = 8 + ntrain_args;
            char **av = xalloc((size_t)na2 + 1, sizeof(char *)); int k = 0;
            av[k++] = self; av[k++] = "train"; av[k++] = (char *)model; av[k++] = data; av[k++] = "--out"; av[k++] = cand; av[k++] = "--log"; av[k++] = tlog;
            for (int i = 0; i < ntrain_args; i++) av[k++] = train_args[i];
            av[k] = NULL;
            remove(tlog);
            printf("rodada %d: treino (saida em %s)\n", r, tout);
            const int rc = lr_run(av, tout);
            free(av);
            if (rc == 2) {        /* o treino recusou gravar: a validacao nao venceu a base (ou divergiu) */
                fprintf(stderr, "learn: rodada %d: o treino nao venceu o modelo base na validacao e nao gravou candidato (veja %s)\n", r, tout);
                fprintf(lg, "{\"round\": %d, \"train_refused\": true}\n", r); fflush(lg); continue;
            }
            if (rc != 0) { fprintf(stderr, "learn: katana train falhou (veja %s)\n", tout); fprintf(lg, "{\"round\": %d, \"train_failed\": true}\n", r); fflush(lg); continue; }
        }
        char res[4096] = "null";
        { FILE *f = fopen(tout, "rb"); char line[4096]; if (f) { while (fgets(line, sizeof line, f)) if (!strncmp(line, "resultado: ", 11)) { snprintf(res, sizeof res, "%s", line + 11); res[strcspn(res, "\r\n")] = 0; } fclose(f); } }
        /* 4. prova nas tarefas de avaliacao */
        char cev[1400], env_cand[1500]; snprintf(cev, sizeof cev, "%s/eval.jsonl", rd); snprintf(env_cand, sizeof env_cand, "KATANA_SPEC=%s", cand);
        remove(cev);
        {
            char *av[] = { (char *)py, runpy, "--model", (char *)model, "--katana", self, "--tasks", ev.p, "--repeat", erep_s, "--timeout", (char *)timeout,
                           "--label", "cand", "--out", cev, "--env", env_home, "--env", env_up, "--env", env_cand, NULL };
            printf("rodada %d: avaliacao do candidato\n", r);
            if (lr_run(av, NULL) != 0) fprintf(stderr, "learn: o codebench terminou com erro (seguindo com o que gravou)\n");
        }
        int ep = 0, er = 0; lr_score(cev, &ep, &er);
        const int accept = er > 0 && ep > champ_pass;
        printf("rodada %d: candidato resolveu %d de %d (melhor ate agora: %d de %d) -> %s\n", r, ep, er, champ_pass, champ_runs,
               accept ? "ACEITO, vira o novo melhor" : "mantem o anterior");
        if (accept) {
            if (lr_copy(cand, best, 0) < 0) { fprintf(stderr, "learn: nao copiou %s\n", cand); return 1; }
            champ_pass = ep; champ_runs = er; have_best = 1;
        }
        fprintf(lg, "{\"round\": %d, \"collect_pass\": %d, \"collect_runs\": %d, \"new_sessions\": %ld, \"teacher_sessions\": %ld, \"data\": %ld, "
                    "\"train\": %s, \"eval_pass\": %d, \"eval_runs\": %d, \"accepted\": %s}\n",
                r, cpass, cruns, nnew, tpass, ntot, res, ep, er, accept ? "true" : "false");
        fflush(lg);
    }
    fclose(lg);
    printf("\nlearn: %s\n", have_best ? "melhor especializacao em" : "nenhum candidato superou o modelo base; nada gravado em");
    printf("  %s (use KATANA_SPEC=%s, ou katana merge para um GGUF novo)\n  historico: %s\n", best, best, logp);
    for (int i = 0; i < na; i++) free(all[i]);
    free(all); free(tr.p); free(ev.p);
    return 0;
}
