/* ---------- formato "brief" do KATANA.md ----------
 * Texto curto, um fato por linha, chaves no inicio da linha (gramatica completa em docs/extending.md):
 *   # nome: o que e        build: / test: / run: / lint: / done: (um valor)
 *   layout: rules: never: ask: style: docs: (blocos de linhas recuadas)
 * O motor usa o que entende: never e ask com padrao entre crases sao impostos nas chamadas de Bash, Write e Edit;
 * test e done geram uma nota no fim do pedido que alterou arquivos sem rodar a verificacao. Arquivo em prosa comum
 * continua valendo como texto. katana brief check [arquivo] confere o formato; katana brief show mostra o que foi lido. */
static const char *const BR_SINGLE[] = {"build", "test", "run", "lint", "done", NULL};
static const char *const BR_BLOCK[] = {"layout", "rules", "never", "ask", "style", "docs", NULL};
enum { BRB_LAYOUT, BRB_RULES, BRB_NEVER, BRB_ASK, BRB_STYLE, BRB_DOCS };
typedef struct { char key[12], label[24], val[400]; } BrSingle;
typedef struct { int blk; char text[300], pat[200]; } BrItem;
typedef struct { char header[300]; BrSingle s[24]; int ns; BrItem it[160]; int ni; int known; } Brief;

static int br_index(const char *const *names, const char *k, size_t n) {
    for (int i = 0; names[i]; i++) if (strlen(names[i]) == n && strncmp(names[i], k, n) == 0) return i;
    return -1;
}
/* glob de shell sobre o texto inteiro: * qualquer sequencia, ? um caractere, [abc] conjunto */
static int br_glob(const char *p, const char *s) {
    for (; *p; p++, s++) {
        if (*p == '*') { while (p[1] == '*') p++; if (!p[1]) return 1; for (; *s; s++) if (br_glob(p + 1, s)) return 1; return br_glob(p + 1, s); }
        if (!*s) return 0;
        if (*p == '?') continue;
        if (*p == '[') {
            const char *q = p + 1; int neg = *q == '!' || *q == '^', hit = 0;
            if (neg) q++;
            for (int first = 1; *q && (*q != ']' || first); q++, first = 0) {
                if (q[1] == '-' && q[2] && q[2] != ']') { if ((unsigned char)*s >= (unsigned char)*q && (unsigned char)*s <= (unsigned char)q[2]) hit = 1; q += 2; }
                else if (*q == *s) hit = 1;
            }
            if (!*q || hit == neg) return 0;
            p = q; continue;
        }
        if (*p == '\\' && p[1]) p++;
        if (*p != *s) return 0;
    }
    return *s == 0;
}
static int br_glob_ok(const char *p) {     /* o padrao "compila": colchetes fechados */
    for (; *p; p++) {
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '[') { const char *q = p + 1; if (*q == '!' || *q == '^') q++; if (*q == ']') q++; while (*q && *q != ']') q++; if (!*q) return 0; p = q; }
    }
    return 1;
}
static const char *const BR_HEDGE[] = {"try to", "maybe", "if possible", "should probably", "perhaps", "probably", "tente ", "talvez", "se possivel", "provavelmente", "de preferencia", NULL};
static int br_hedge(const char *t) {
    for (int i = 0; BR_HEDGE[i]; i++) for (const char *h = t; *h; h++) if (strncasecmp(h, BR_HEDGE[i], strlen(BR_HEDGE[i])) == 0) return i + 1;
    return 0;
}
static void br_trim(char *s) {
    size_t n = strlen(s), a = 0;
    while (a < n && (s[a] == ' ' || s[a] == '\t')) a++;
    while (n > a && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
    memmove(s, s + a, n - a); s[n - a] = 0;
}
#define BR_FIND(...) do { if (find) { char _m[500]; snprintf(_m, sizeof _m, __VA_ARGS__); mcp_bputs(find, _m); mcp_bputs(find, "\n"); } } while (0)
/* le o texto; acrescenta os achados do lint a find (se nao for NULL) com a numeracao de linhas; devolve o numero de erros.
   b recebe o que foi entendido (b->known = 0: texto comum). Chamar com o mesmo b para varias camadas as mescla. */
static int br_parse(const char *txt, Brief *b, McpB *find, const char *fname) {
    int errs = 0, lineno = 0, cur = -1, first = 1, known = 0, nrules = 0, nitems[6] = {0};
    char seen[96][120]; int nseen = 0;
    char fn[100]; snprintf(fn, sizeof fn, "%s", fname ? fname : "file");
#define ERR(...) do { errs++; if (find) { char _m[500]; snprintf(_m, sizeof _m, __VA_ARGS__); char _l[620]; snprintf(_l, sizeof _l, "%s:%d: error: %s\n", fn, lineno, _m); mcp_bputs(find, _l); } } while (0)
#define WARN(...) do { if (find) { char _m[500]; snprintf(_m, sizeof _m, __VA_ARGS__); char _l[620]; snprintf(_l, sizeof _l, "%s:%d: warning: %s\n", fn, lineno, _m); mcp_bputs(find, _l); } } while (0)
    for (const char *l = txt; *l;) {
        const size_t n = strcspn(l, "\n");
        char ln[1200]; snprintf(ln, sizeof ln, "%.*s", (int)(n < 1199 ? n : 1199), l);
        l += n; if (*l) l++;
        lineno++;
        const int indented = ln[0] == ' ' || ln[0] == '\t';
        char t[1200]; snprintf(t, sizeof t, "%s", ln); br_trim(t);
        if (!t[0]) continue;
        if (!indented && t[0] == '#') {
            if (first) {
                first = 0; known = 1;
                snprintf(b->header, sizeof b->header, "%s", t + 1 + (t[1] == ' '));
                const char *w = strchr(b->header, ':'); int words = 0;
                if (w) { for (const char *c = w + 1; *c; c++) if (*c != ' ' && (c[-1] == ' ' || c == w + 1)) words++; if (words > 12) WARN("the header says what it is in %d words; 12 at most", words); }
                else WARN("the header should be '# name: what it is'");
            }
            continue;
        }
        first = 0;
        if (!indented) {
            size_t kl = 0;
            while (islower((unsigned char)t[kl])) kl++;
            char label[24] = "";
            size_t ke = kl;
            if (kl && t[ke] == '[') { const char *c = strchr(t + ke, ']'); if (c && c - (t + ke) < 23) { snprintf(label, sizeof label, "%.*s", (int)(c - (t + ke) - 1), t + ke + 1); ke = (size_t)(c - t) + 1; } }
            if (kl && t[ke] == ':') {
                const int si = br_index(BR_SINGLE, t, kl), bi = br_index(BR_BLOCK, t, kl);
                char *v = t + ke + 1; while (*v == ' ') v++;
                if (si >= 0) {
                    known = 1; cur = -1;
                    if (!*v) ERR("'%s:' needs a value on the same line", BR_SINGLE[si]);
                    else {
                        int k = -1;
                        for (int i = 0; i < b->ns; i++) if (strcmp(b->s[i].key, BR_SINGLE[si]) == 0 && strcmp(b->s[i].label, label) == 0) k = i;
                        if (k < 0 && b->ns < 24) k = b->ns++;
                        if (k >= 0) { snprintf(b->s[k].key, sizeof b->s[k].key, "%s", BR_SINGLE[si]); snprintf(b->s[k].label, sizeof b->s[k].label, "%s", label); snprintf(b->s[k].val, sizeof b->s[k].val, "%s", v); }
                        if (br_hedge(v)) WARN("hedging word in '%s:' (say what to do, not what to try)", BR_SINGLE[si]);
                        if (strcmp(BR_SINGLE[si], "done") != 0 && strcmp(BR_SINGLE[si], "run") != 0) {
                            char w1[200]; size_t wl = strcspn(v, " \t");
                            snprintf(w1, sizeof w1, "%.*s", (int)(wl < 199 ? wl : 199), v);
                            if (w1[0] && !strchr(w1, '=') && !strchr(w1, '$') && !strchr(w1, '`') && strcmp(w1, "cd") != 0 && strcmp(w1, ".") != 0 && strcmp(w1, "source") != 0 && strcmp(w1, "export") != 0 && !mcp_which(w1))
                                WARN("'%s:' runs '%s', which is not on PATH or not executable here", BR_SINGLE[si], w1);
                        }
                    }
                } else if (bi >= 0) {
                    known = 1; cur = bi;
                    if (*v) ERR("'%s:' takes indented lines under it, not a value", BR_BLOCK[bi]);
                } else { cur = -1; ERR("unknown key '%.*s:'", (int)kl, t); }
                continue;
            }
            cur = -1;
            WARN("prose line outside any key (write one fact per line under a key): %.60s", t);
            continue;
        }
        if (cur < 0) { WARN("indented line outside a block: %.60s", t); continue; }
        BrItem *it = b->ni < 160 ? &b->it[b->ni] : NULL;
        char text[300] = "", pat[200] = "";
        if (cur == BRB_LAYOUT || cur == BRB_DOCS) {
            const char *sp = strstr(t, "  ");
            if (!sp) { WARN("'%s' lines are '<path>  <text>' with two spaces between: %.50s", BR_BLOCK[cur], t); snprintf(text, sizeof text, "%s", t); }
            else { snprintf(text, sizeof text, "%s", t); }
        } else {
            char *s = t;
            if (s[0] == '-' && s[1] == ' ') s += 2; else WARN("items of '%s' start with '- ': %.50s", BR_BLOCK[cur], t);
            while (*s == ' ') s++;
            snprintf(text, sizeof text, "%s", s);
            if (cur == BRB_NEVER || cur == BRB_ASK) {
                char *e = text + strlen(text);
                if (e > text && e[-1] == '`') {
                    char *st = e - 2; while (st > text && *st != '`') st--;
                    if (*st == '`' && st < e - 1) { snprintf(pat, sizeof pat, "%.*s", (int)(e - 2 - st), st + 1); *st = 0; br_trim(text); }
                }
                if (pat[0] && !br_glob_ok(pat)) ERR("the pattern `%s` does not compile (unclosed [)", pat);
                if (!pat[0]) WARN("'%s' item without a `pattern` is guidance only: the engine cannot enforce it", BR_BLOCK[cur]);
            }
            if (cur == BRB_RULES && strlen(text) > 100) ERR("rule is %zu characters; 100 at most", strlen(text));
            if (br_hedge(text)) WARN("hedging word in: %.60s", text);
            char key[120]; snprintf(key, sizeof key, "%d%.100s", cur == BRB_RULES || cur == BRB_STYLE ? 0 : cur, text);
            for (int i = 0; i < nseen; i++) if (strcmp(seen[i], key) == 0) { WARN("duplicate item: %.60s", text); break; }
            if (nseen < 96) snprintf(seen[nseen++], sizeof seen[0], "%s", key);
        }
        nitems[cur]++;
        if (cur == BRB_LAYOUT && nitems[cur] == 13) ERR("layout has more than 12 lines");
        if (cur == BRB_STYLE && nitems[cur] == 7) ERR("style has more than 6 lines");
        nrules++;
        if (it) { it->blk = cur; snprintf(it->text, sizeof it->text, "%s", text); snprintf(it->pat, sizeof it->pat, "%s", pat); b->ni++; }
    }
    (void)nrules;
    if (known) b->known = 1;
#undef ERR
#undef WARN
    return errs;
}
static const char *br_single(const Brief *b, const char *key, const char *label) {     /* a camada mais especifica ganha (a ultima lida) */
    for (int i = b->ns - 1; i >= 0; i--) if (strcmp(b->s[i].key, key) == 0 && strcmp(b->s[i].label, label ? label : "") == 0) return b->s[i].val;
    return NULL;
}
/* as camadas existentes mescladas, do geral ao especifico (chaves de um valor: a mais especifica vence; blocos somam) */
static void br_load(Brief *b) {
    InstrDoc dv[3];
    memset(b, 0, sizeof *b);
    const int n = instr_layers(dv);
    for (int i = 0; i < n; i++) { br_parse(dv[i].txt, b, NULL, NULL); free(dv[i].txt); }
}
static Brief g_br; static char g_br_cwd[4200]; static int g_br_req = -1, g_br_ok;
static const Brief *br_cur(void) {       /* relido uma vez por pedido ou quando a pasta muda */
    char cwd[4200];
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    if (!g_br_ok || g_br_req != g_undo_req || strcmp(cwd, g_br_cwd) != 0) { br_load(&g_br); snprintf(g_br_cwd, sizeof g_br_cwd, "%s", cwd); g_br_req = g_undo_req; g_br_ok = 1; }
    return &g_br;
}
/* ---- imposicao de never e ask ---- */
static int br_match_cmd(const char *pat, const char *cmd) {      /* a linha toda ou cada trecho separado por && || ; | e quebras de linha */
    if (br_glob(pat, cmd)) return 1;
    char seg[4200]; size_t k = 0;
    for (const char *c = cmd;; c++) {
        const int sep = !*c || *c == ';' || *c == '|' || *c == '&' || *c == '\n';
        if (!sep && k + 1 < sizeof seg) { seg[k++] = *c; continue; }
        seg[k] = 0; br_trim(seg);
        if (seg[0] && br_glob(pat, seg)) return 1;
        k = 0;
        if (!*c) break;
    }
    return 0;
}
static int br_pathlike(const char *pat) { return strchr(pat, '/') || strncmp(pat, "*.", 2) == 0; }
static int br_match_path(const char *pat, const char *file) {
    char cwd[4200], rel[4400];
    if (br_glob(pat, file)) return 1;
    if (getcwd(cwd, sizeof cwd)) {
        const size_t cl = strlen(cwd);
        if (strncmp(file, cwd, cl) == 0 && file[cl] == '/') { snprintf(rel, sizeof rel, "%s", file + cl + 1); if (br_glob(pat, rel)) return 1; }
        else if (file[0] != '/') { snprintf(rel, sizeof rel, "%s/%s", cwd, file); if (br_glob(pat, rel)) return 1; }
    }
    const char *base = strrchr(file, '/');
    return base && pat[0] == '*' && pat[1] == '.' && br_glob(pat, base + 1);
}
static int g_br_changed, g_br_ran, g_br_noted, g_br_exnoted, g_br_seen_req = -1;
static void br_sync(void) { if (g_br_seen_req != g_undo_req) { g_br_seen_req = g_undo_req; g_br_changed = g_br_ran = g_br_noted = g_br_exnoted = 0; } }
static int brief_guard_subject(int bash, const char *subj, char *out, size_t cap);
/* antes de Bash/Write/Edit: 1 se a regra do projeto recusa (out recebe o motivo) */
static int brief_guard(const char *name, const char *js, const char *end, char *out, size_t cap) {
    const int bash = strcmp(name, "Bash") == 0, wr = strcmp(name, "Write") == 0 || strcmp(name, "Edit") == 0 || strcmp(name, "NotebookEdit") == 0;
    if (!bash && !wr) return 0;
    char *subj = ct_str(js, end, bash ? "command" : "file_path");
    if (!subj && wr) subj = ct_str(js, end, "notebook_path");
    if (!subj) return 0;
    const int r = brief_guard_subject(bash, subj, out, cap);
    free(subj);
    return r;
}
static int brief_guard_subject(int bash, const char *subj, char *out, size_t cap) {
    const Brief *b = br_cur();
    if (!b->ni) return 0;
    int refused = 0;
    for (int pass = 0; pass < 2 && !refused; pass++) {      /* never primeiro, depois ask */
        const int want = pass == 0 ? BRB_NEVER : BRB_ASK;
        for (int i = 0; i < b->ni && !refused; i++) {
            const BrItem *it = &b->it[i];
            if (it->blk != want || !it->pat[0]) continue;
            const int hit = bash ? br_match_cmd(it->pat, subj) : br_pathlike(it->pat) && br_match_path(it->pat, subj);
            if (!hit) continue;
            if (want == BRB_NEVER) { snprintf(out, cap, "error: refused by the project rule (never): %s `%s`. Do something else; do not retry this.", it->text, it->pat); refused = 1; }
            else {
                char what[500]; static char det[9000];
                snprintf(what, sizeof what, "project rule (ask): %.300s", it->text);
                snprintf(det, sizeof det, "%s: %s", bash ? "command" : "file", subj);       /* quem aprova ve o comando inteiro */
                if (!agent_confirm(what, det)) { snprintf(out, cap, "error: the project rule (ask) says this needs the user's yes, and it was not given (or no one could be asked): %s `%s`", it->text, it->pat); refused = 1; }
            }
        }
    }
    return refused;
}
/* o comando de done: entre crases, ou a linha toda quando comeca como um comando; 0 = e so texto */
static int br_done_cmd(const char *dn, char *out, size_t cap) {
    if (!dn) return 0;
    const char *a = strchr(dn, '`'), *z = a ? strchr(a + 1, '`') : NULL;
    if (a && z && z > a + 1) { snprintf(out, cap, "%.*s", (int)(z - a - 1), a + 1); return 1; }
    static const char *const RUN[] = { "sh ", "bash ", "make", "python ", "python3 ", "npm ", "pnpm ", "yarn ", "npx ", "node ", "cargo ", "go test", "go build", "ctest ", "cmake ", "mix ", "zig ", "pytest", "./", "katana ", NULL };
    while (*dn == ' ') dn++;
    for (int i = 0; RUN[i]; i++) if (strncmp(dn, RUN[i], strlen(RUN[i])) == 0) {
        if (!strcmp(RUN[i], "make") && dn[4] && strncmp(dn + 4, " test", 5) && strncmp(dn + 4, " check", 6) && strncmp(dn + 4, " verify", 7)) continue;
        snprintf(out, cap, "%s", dn);
        size_t l = strlen(out); while (l && (out[l - 1] == '\n' || out[l - 1] == ' ' || out[l - 1] == '.')) out[--l] = 0;
        return 1;
    }
    return 0;
}
/* depois da ferramenta: registra que arquivos mudaram e se a verificacao do projeto rodou neste pedido */
static void brief_track(const char *name, const char *js, const char *end, const char *res) {
    br_sync();
    if (strncmp(res, "error:", 6) == 0) { if (strcmp(name, "Bash") != 0) return; }
    if (strcmp(name, "Write") == 0 || strcmp(name, "Edit") == 0 || strcmp(name, "NotebookEdit") == 0) {
        /* skill, uso de MCP e o proprio KATANA.md nao sao codigo do projeto: nao cobram o teste (decisao do dono, 2026-11-22) */
        char *fp = ct_str(js, end, strcmp(name, "NotebookEdit") == 0 ? "notebook_path" : "file_path");
        const char *bn = fp ? strrchr(fp, '/') : NULL; bn = bn ? bn + 1 : fp;
        const int conf = fp && (strstr(fp, "/.katana/") || !strncmp(fp, ".katana/", 8) || !strcmp(bn, "SKILL.md") || !strcmp(bn, "KATANA.md"));
        free(fp);
        if (!conf) { g_br_changed = 1; g_br_ran = 0; }       /* mudou depois de verificar: a verificacao nao vale mais */
        return;
    }
    if (strcmp(name, "Bash") != 0) return;
    const Brief *b = br_cur();
    char *cmd = ct_str(js, end, "command");
    if (!cmd) return;
    const char *tc = br_single(b, "test", ""), *dn = br_single(b, "done", "");
    char dc[300];
    if (br_done_cmd(dn, dc, sizeof dc)) { if (strstr(cmd, dc)) g_br_ran = 1; }       /* done: com comando proprio: e ele que vale */
    else {
        if (tc && *tc && strstr(cmd, tc)) g_br_ran = 1;
        for (int i = 0; i < b->ns; i++) if (strcmp(b->s[i].key, "test") == 0 && strstr(cmd, b->s[i].val)) g_br_ran = 1;
    }
    free(cmd);
}
/* o pedido traz exemplos de saida ou formato? (palavras nos dois idiomas, setas, trechos entre crases) */
static int br_has_example(const char *req) {
    static const char *const W[] = { "exemplo", "example", "for instance", "por ex", "e assim", "\xc3\xa9 assim", "like this", "saida", "sa\xc3\xad" "da", "output", "imprim", "print", "escreve no std", "must be", "tem que dar", "deve dar", "returns", "retorna", "->", "=>", NULL };
    if (!req) return 0;
    char *l = strdup(req); if (!l) return 0;
    for (char *p = l; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    int hit = 0;
    for (int i = 0; W[i] && !hit; i++) hit = strstr(l, W[i]) != NULL;
    free(l);
    return hit;
}
/* nota (uma vez por pedido): arquivos mudaram e o pedido tem exemplos; o modelo conferia o proprio trabalho contra ele mesmo e
   nao contra o que foi pedido (tres falhas iguais na prova de 21/11/2026). KATANA_EXAMPLE_NOTE=0 desliga. */
static int brief_example_note(const char *req, char *out, size_t cap) {
    br_sync();
    const char *e = getenv("KATANA_EXAMPLE_NOTE");
    if ((e && !atoi(e)) || g_br_exnoted || !g_br_changed || !br_has_example(req)) return 0;
    g_br_exnoted = 1;
    KNOTE(FINISH_EXAMPLE, out, cap);      /* sem o eco do pedido (cortado em 2026-11-23): ele ja esta na conversa */
    return 1;
}
/* nota (uma vez por pedido) para o fim do pedido: arquivos mudaram e a verificacao do projeto nao rodou. 0 = nada a dizer.
   Chamada pelo laco no ponto em que o pedido termina (ver o relatorio). */
static int brief_end_note(char *out, size_t cap) {
    br_sync();
    if (!g_br_changed || g_br_ran || g_br_noted) return 0;
    const Brief *b = br_cur();
    const char *tc = br_single(b, "test", ""), *dn = br_single(b, "done", "");
    if (!tc && !dn) return 0;
    g_br_noted = 1;
    char dc[300];
    if (br_done_cmd(dn, dc, sizeof dc) && !(tc && strstr(tc, dc))) tc = dc;      /* o test: ja contem o comando de done: */
    if (tc) KNOTE(FINISH_TEST, out, cap, tc);
    else KNOTE(FINISH_DONE, out, cap, dn);
    return 1;
}

/* katana brief check [arquivo] | show [arquivo] */
static int brief_cli(int argc, char **argv) {
    const char *sub = argc > 0 ? argv[0] : "check";
    if (strcmp(sub, "check") && strcmp(sub, "show")) { fprintf(stderr, "uso: katana brief check [arquivo] | show [arquivo]\n"); return 2; }
    char path[4400] = "";
    if (strcmp(sub, "show") == 0 && argc == 1) {        /* sem arquivo: as camadas mescladas, como o motor as usa */
        Brief *b = (Brief *)calloc(1, sizeof *b);
        br_load(b);
        if (!b->known) { printf("no brief keys in the layers (global, project, local): the files are plain text or absent\n"); free(b); return 0; }
        if (b->header[0]) printf("header: %s\n", b->header);
        for (int i = 0; i < b->ns; i++) printf("%s%s%s%s: %s\n", b->s[i].key, b->s[i].label[0] ? "[" : "", b->s[i].label, b->s[i].label[0] ? "]" : "", b->s[i].val);
        for (int k = 0; BR_BLOCK[k]; k++) {
            int c = 0;
            for (int i = 0; i < b->ni; i++) if (b->it[i].blk == k) { if (!c++) printf("%s:\n", BR_BLOCK[k]); printf("  %s%s%s%s\n", b->it[i].text, b->it[i].pat[0] ? " `" : "", b->it[i].pat, b->it[i].pat[0] ? "`" : ""); }
        }
        free(b); return 0;
    }
    if (argc > 1) snprintf(path, sizeof path, "%s", argv[1]);
    else { char d[4200]; if (!proj_dir(d, sizeof d) || !proj_file_in(d, path, sizeof path)) { fprintf(stderr, "no KATANA.md or AGENTS.md here or above\n"); return 2; } }
    char *txt = instr_read(path, 99);       /* sem inclusoes */
    if (!txt) { fprintf(stderr, "cannot read %s (missing, empty or binary)\n", path); return 2; }
    Brief *b = (Brief *)calloc(1, sizeof *b);
    McpB f = {0};
    mcp_bput(&f, "", 0);
    const char *bn = strrchr(path, '/'); bn = bn ? bn + 1 : path;
    const int errs = br_parse(txt, b, &f, bn);
    int rc = 0;
    if (strcmp(sub, "show") == 0) {
        if (!b->known) printf("%s: not in the brief format; the engine passes it to the model as plain text (%zu characters)\n", path, strlen(txt));
        else {
            printf("file: %s\n", path);
            if (b->header[0]) printf("header: %s\n", b->header);
            for (int i = 0; i < b->ns; i++) printf("%s%s%s%s: %s\n", b->s[i].key, b->s[i].label[0] ? "[" : "", b->s[i].label, b->s[i].label[0] ? "]" : "", b->s[i].val);
            for (int k = 0; BR_BLOCK[k]; k++) {
                int c = 0;
                for (int i = 0; i < b->ni; i++) if (b->it[i].blk == k) { if (!c++) printf("%s:\n", BR_BLOCK[k]); printf("  %s%s%s%s\n", b->it[i].text, b->it[i].pat[0] ? " `" : "", b->it[i].pat, b->it[i].pat[0] ? "`" : ""); }
            }
            printf("enforced by the engine:\n");
            int e = 0;
            for (int i = 0; i < b->ni; i++) if ((b->it[i].blk == BRB_NEVER || b->it[i].blk == BRB_ASK) && b->it[i].pat[0]) {
                printf("  %s `%s` on %s\n", b->it[i].blk == BRB_NEVER ? "never (refuse)" : "ask (user's yes)", b->it[i].pat, br_pathlike(b->it[i].pat) ? "Bash command lines and file paths of Write/Edit" : "Bash command lines"); e++;
            }
            if (br_single(b, "test", "")) { printf("  end-of-request note unless `%s` runs\n", br_single(b, "test", "")); e++; }
            else if (br_single(b, "done", "")) { printf("  end-of-request note about done: after changing files\n"); e++; }
            if (!e) printf("  (nothing)\n");
        }
    } else {
        if (!b->known) { printf("%s: no brief keys found; it is accepted as plain text (nothing is enforced)\n", path); }
        else printf("%s", f.p);
        const size_t sz = strlen(txt);
        if (sz > 1500) printf("%s: warning: %zu characters; the recommended budget is 1500\n", path, sz);
        else if (b->known) printf("%s: %zu characters (budget 1500)\n", path, sz);
        if (b->known) printf("%s: %d error(s)\n", path, errs);
        rc = errs ? 1 : 0;
    }
    free(f.p); free(b); free(txt);
    return rc;
}
