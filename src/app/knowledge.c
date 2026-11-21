/* ---------- katana knowledge: notas de texto a partir das sessoes anteriores de um agente de programacao ----------
 * Sem modelo: so extracao. katana knowledge build <raiz|pasta|arquivo.jsonl> [--out PASTA] [--force] [--replace REGEX=TEXTO]...
 * [--cut-matching REGEX] le as transcricoes com o leitor de transcript.c (tr_run, uma chamada por arquivo) e grava, por
 * projeto (o cwd gravado nos registros), uma nota Markdown por sessao: para cada pedido do usuario, o pedido, o fim do
 * episodio (a ultima fala do modelo), os arquivos gravados ou editados, os comandos que importaram e os erros que
 * foram vencidos. O agente busca nas notas com Grep/Glob/Read; katana knowledge search e stats servem ao dono e aos testes.
 * Mascara chaves (8-4-4-4-12 hex), valores depois de token/password/secret/api_key/Authorization e prefixos conhecidos.
 * Incremental: .src/<hash do caminho> guarda o nome da nota; transcricao mais velha que esse marcador e pulada (--force refaz).
 * katana knowledge corpus: as notas viram pares pergunta -> resposta para o treino (so extracao, sem modelo). */

#ifdef _WIN32
static int kn_main(int argc, char **argv) { fprintf(stderr, "knowledge: so no Linux por enquanto\n"); return 1; }
#else

#define KN_REQ_MAX 800
#define KN_OUT_MAX 1500
#define KN_CMDS 12
#define KN_PROBS 5
#define KN_FILES 30

typedef struct { char id[96]; int kind; char *cmd, *desc, *path, *err; int done, fail; } KnCall;
typedef struct {
    Rx *cut;
    Buf body; int nep; TrOpt *o;
    char cwd[512], ts[40], title[160], first[512];
} KnSess;

static const char *kn_s(const Buf *b) { return b->p ? b->p : ""; }
static void kn_cpy(char *d, const char *s, size_t cap) { size_t n = strlen(s); if (n >= cap) n = cap - 1; memcpy(d, s, n); d[n] = 0; }
static char *kn_str(const char *v) {                       /* valor JSON de texto -> texto no heap ("" se nao e texto) */
    Buf b = {0};
    if (v && *v == '"') { const char *p = v; js_string(&p, &b); }
    return b.p ? b.p : strdup("");
}
static int kn_ieq(const char *a, const char *b, size_t n) { for (size_t i = 0; i < n; i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0; return 1; }
static int kn_hexn(const char *s, int n) { for (int i = 0; i < n; i++) if (!isxdigit((unsigned char)s[i])) return 0; return 1; }
static int kn_uuid(const char *s) {
    return kn_hexn(s, 8) && s[8] == '-' && kn_hexn(s + 9, 4) && s[13] == '-' && kn_hexn(s + 14, 4) && s[18] == '-' && kn_hexn(s + 19, 4) && s[23] == '-' && kn_hexn(s + 24, 12);
}
static int kn_tokch(int c) { return isalnum(c) || c == '_' || c == '-'; }
static int kn_run(const char *s, int minlen) { int n = 0; while (kn_tokch((unsigned char)s[n]) || s[n] == '.') n++; return n >= minlen ? n : 0; }

/* ---------- mascara de segredos ---------- */
static void kn_mask(const char *s, Buf *out) {
    static const char *const kw[] = { "api_key", "apikey", "api-key", "authorization", "password", "passwd", "secret", "token", "senha" };
    const size_t n = strlen(s);
    size_t i = 0;
    while (i < n) {
        const unsigned char c = (unsigned char)s[i];
        const int bound = i == 0 || !isalnum((unsigned char)s[i - 1]);
        if (bound && isxdigit(c) && kn_uuid(s + i)) { buf_str(out, "[key]"); i += 36; continue; }
        if (bound) {                                        /* prefixos conhecidos de chaves */
            int m = 0;
            if (!strncmp(s + i, "sk-", 3)) m = kn_run(s + i + 3, 20) ? 3 + kn_run(s + i + 3, 20) : 0;
            else if (!strncmp(s + i, "ghp_", 4) || !strncmp(s + i, "gho_", 4) || !strncmp(s + i, "ghs_", 4)) m = kn_run(s + i + 4, 20) ? 4 + kn_run(s + i + 4, 20) : 0;
            else if (!strncmp(s + i, "github_pat_", 11)) m = kn_run(s + i + 11, 20) ? 11 + kn_run(s + i + 11, 20) : 0;
            else if (!strncmp(s + i, "AKIA", 4) && kn_run(s + i + 4, 16)) m = 4 + kn_run(s + i + 4, 16);
            else if (!strncmp(s + i, "xox", 3) && strchr("bpas", s[i + 3]) && s[i + 4] == '-') m = 5 + kn_run(s + i + 5, 1);
            else if (!strncmp(s + i, "eyJ", 3) && kn_run(s + i, 30)) m = kn_run(s + i, 30);
            if (m) { buf_str(out, "[masked]"); i += (size_t)m; continue; }
        }
        if (bound && (i == 0 || !strchr("/._-", s[i - 1]))) {            /* sequencia aleatoria longa (letras maiusculas, minusculas e digitos) */
            size_t r = 0; int up = 0, lo = 0, di = 0;
            while (isalnum((unsigned char)s[i + r])) { up |= isupper((unsigned char)s[i + r]) != 0; lo |= islower((unsigned char)s[i + r]) != 0; di |= isdigit((unsigned char)s[i + r]) != 0; r++; }
            if (r >= 32 && up && lo && di && !strchr("/._-", s[i + r] ? s[i + r] : ' ')) { buf_str(out, "[masked]"); i += r; continue; }
        }
        int hit = 0;
        if (i == 0 || !isalpha((unsigned char)s[i - 1])) for (size_t k = 0; k < sizeof kw / sizeof kw[0]; k++) {
            const size_t L = strlen(kw[k]);
            if (i + L > n || !kn_ieq(s + i, kw[k], L)) continue;
            size_t j = i + L;
            while (isalnum((unsigned char)s[j]) || s[j] == '_') j++;       /* token_value, secrets, ... */
            size_t q = j;
            while (s[q] == ' ' || s[q] == '\t') q++;
            const int space_only = q > j;
            if (s[q] == '"' || s[q] == '\'') q++;
            while (s[q] == ' ' || s[q] == '\t') q++;
            const int sep = s[q] == ':' || s[q] == '=';
            if (sep) { q++; while (s[q] == ' ' || s[q] == '\t') q++; }
            else if (!(space_only && ((i >= 2 && s[i - 1] == '-' && s[i - 2] == '-') || (i >= 1 && s[i - 1] == '-')))) {
                /* sem separador: so mascara um valor com cara de chave logo depois */
                size_t v = j; while (s[v] == ' ') v++;
                if (!(space_only && kn_run(s + v, 20))) continue;
                q = v;
            }
            if (kn_ieq(s + q, "bearer ", 7)) q += 7; else if (kn_ieq(s + q, "basic ", 6)) q += 6;
            if (s[q] == '"' || s[q] == '\'') q++;
            size_t e = q;
            while (s[e] && !strchr(" \t\r\n\"'`,;)}&\\", s[e])) e++;
            if (e == q || s[q] == '$' || s[q] == '<' || s[q] == '[' || s[q] == '{' || s[q] == '(' || (e - q > 0 && !strncmp(s + q, "[masked]", 8))) continue;
            buf_add(out, s + i, q - i); buf_str(out, "[masked]");
            i = e; hit = 1; break;
        }
        if (hit) continue;
        buf_add(out, s + i, 1); i++;
    }
}

/* corta em cap bytes sem partir um caractere UTF-8 */
static void kn_clip(Buf *b, size_t cap) {
    if (b->n <= cap) return;
    size_t a = cap;
    while (a > 0 && ((unsigned char)b->p[a] & 0xC0) == 0x80) a--;
    b->n = a; b->p[a] = 0;
    buf_str(b, " [...]");
}
static void kn_trim(Buf *b) {
    if (!b->p) return;
    size_t a = 0, e = b->n;
    while (a < e && strchr(" \t\r\n", b->p[a])) a++;
    while (e > a && strchr(" \t\r\n", b->p[e - 1])) e--;
    memmove(b->p, b->p + a, e - a); b->n = e - a; b->p[b->n] = 0;
}

/* texto -> out: linhas que casam com --cut-matching saem, segredos mascarados, aparado e cortado em cap */
static void kn_text(const KnSess *S, const char *raw, size_t cap, Buf *out) {
    Buf t = {0};
    for (const char *p = raw; *p; ) {
        const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (!(S->cut && l && rx_search(S->cut, p, (long)l, 0, NULL) == 1)) { buf_add(&t, p, l); buf_str(&t, "\n"); }
        p += l; if (*p) p++;
    }
    Buf m = {0};
    kn_mask(kn_s(&t), &m);
    free(t.p);
    kn_trim(&m); kn_clip(&m, cap);
    if (m.p) { buf_add(out, m.p, m.n); free(m.p); }
}
/* uma linha so (comandos, caminhos) */
static void kn_line(const KnSess *S, const char *raw, size_t cap, Buf *out) {
    Buf t = {0};
    kn_text(S, raw, cap, &t);
    for (size_t i = 0; i < t.n; i++) if (t.p[i] == '\n' || t.p[i] == '\r' || t.p[i] == '\t') t.p[i] = ' ';
    if (t.p) buf_add(out, t.p, t.n);
    free(t.p);
}
static void kn_quote(Buf *out, const char *txt) {
    for (const char *p = txt; *p; ) {
        const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
        buf_str(out, l ? "> " : ">"); buf_add(out, p, l); buf_str(out, "\n");
        p += l; if (*p) p++;
    }
}

/* ---------- um episodio ---------- */
static int kn_failed(const char *t) {
    t = js_ws(t);
    return !strncmp(t, "Exit code", 9) || !strncmp(t, "<tool_use_error>", 16) || kn_ieq(t, "error", 5);
}
/* primeira palavra do comando, sem "cd X &&" nem VAR=valor na frente */
static void kn_first(const char *c, char *w, size_t cap) {
    for (;;) {
        c = js_ws(c);
        if (!strncmp(c, "cd ", 3)) { const char *a = strstr(c, "&&"); if (a) { c = a + 2; continue; } }
        const char *q = c; while (isalnum((unsigned char)*q) || *q == '_') q++;
        if (q > c && *q == '=') { while (*q && *q != ' ') q++; if (*q) { c = q; continue; } }
        break;
    }
    size_t n = 0; while (c[n] && !strchr(" \t\n;|&", c[n]) && n + 1 < cap) { w[n] = c[n]; n++; }
    w[n] = 0;
}
static int kn_noise(const char *w) {
    static const char *const nz[] = { "ls", "cd", "pwd", "cat", "echo", "head", "tail", "sleep", "wc", "true", "which", "clear", "date", "tree", "file", "stat", "du", "df" };
    for (size_t i = 0; i < sizeof nz / sizeof nz[0]; i++) if (!strcmp(w, nz[i])) return 1;
    return 0;
}

static void kn_add_path(char ***v, int *n, const char *p) {
    for (int i = 0; i < *n; i++) if (!strcmp((*v)[i], p)) return;
    *v = realloc(*v, (size_t)(*n + 1) * sizeof(char *)); (*v)[(*n)++] = strdup(p);
}

/* uma linha {"messages": [...]} do leitor -> um episodio em S->body */
static void kn_emit(void *ud, const char *line) {
    KnSess *S = (KnSess *)ud;
    if (!S->cwd[0]) kn_cpy(S->cwd, S->o->ep_cwd, sizeof S->cwd);       /* o leitor guarda o cwd e a hora de quando o episodio abriu */
    if (!S->ts[0]) kn_cpy(S->ts, S->o->ep_ts, sizeof S->ts);
    const char *top = js_ws(line), *v, *e, *msgs = NULL; char k[64];
    for (const char *p = top + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) if (!strcmp(k, "messages")) msgs = v;
    if (!msgs) return;
    char *req = NULL, *last = NULL;
    KnCall *C = NULL; int nc = 0;
    const char *el, *ee;
    for (const char *p = msgs + 1; (ee = gs_elem(p, &el)); p = ee) {
        if (*el != '{') continue;
        char role[16] = "", tid[96] = ""; const char *content = NULL, *calls = NULL, *x, *xe;
        for (const char *q = el + 1; (xe = gs_member(q, k, sizeof k, &x)); q = xe) {
            if (!strcmp(k, "role")) { char *r = kn_str(x); kn_cpy(role, r, sizeof role); free(r); }
            else if (!strcmp(k, "tool_call_id")) { char *r = kn_str(x); kn_cpy(tid, r, sizeof tid); free(r); }
            else if (!strcmp(k, "content")) content = x;
            else if (!strcmp(k, "tool_calls") && *x == '[') calls = x;
        }
        if (!strcmp(role, "user")) { if (!req) req = kn_str(content); continue; }
        if (!strcmp(role, "assistant")) {
            char *t = kn_str(content);
            if (*js_ws(t)) { free(last); last = t; } else free(t);
            if (!calls) continue;
            const char *ce, *cel;
            for (const char *q = calls + 1; (ce = gs_elem(q, &cel)); q = ce) {
                if (*cel != '{') continue;
                KnCall c; memset(&c, 0, sizeof c); char nm[64] = "";
                for (const char *q2 = cel + 1; (xe = gs_member(q2, k, sizeof k, &x)); q2 = xe) {
                    if (!strcmp(k, "id")) { char *r = kn_str(x); kn_cpy(c.id, r, sizeof c.id); free(r); }
                    else if (!strcmp(k, "function") && *x == '{') {
                        const char *y, *ye;
                        for (const char *q3 = x + 1; (ye = gs_member(q3, k, sizeof k, &y)); q3 = ye) {
                            if (!strcmp(k, "name")) { char *r = kn_str(y); kn_cpy(nm, r, sizeof nm); free(r); }
                            else if (!strcmp(k, "arguments") && *y == '{') {
                                const char *z, *ze; char kk[64];
                                for (const char *q4 = y + 1; (ze = gs_member(q4, kk, sizeof kk, &z)); q4 = ze) {
                                    if (!strcmp(kk, "command")) { free(c.cmd); c.cmd = kn_str(z); }
                                    else if (!strcmp(kk, "description")) { free(c.desc); c.desc = kn_str(z); }
                                    else if (!strcmp(kk, "file_path") || !strcmp(kk, "notebook_path")) { free(c.path); c.path = kn_str(z); }
                                }
                            }
                        }
                    }
                }
                c.kind = !strcmp(nm, "Bash") || !strcmp(nm, "PowerShell") ? 1 : (!strcmp(nm, "Write") || !strcmp(nm, "Edit") || !strcmp(nm, "MultiEdit") || !strcmp(nm, "NotebookEdit")) ? 2 : 0;
                if (!c.kind) { free(c.cmd); free(c.desc); free(c.path); continue; }
                C = realloc(C, (size_t)(nc + 1) * sizeof(KnCall)); C[nc++] = c;
            }
            continue;
        }
        if (!strcmp(role, "tool")) {
            char *t = kn_str(content);
            for (int i = 0; i < nc; i++) if (!strcmp(C[i].id, tid) && !C[i].done) {
                C[i].done = 1; C[i].fail = kn_failed(t);
                if (C[i].fail) {                                    /* primeira linha com texto do erro */
                    const char *l = t; while (*l && (isspace((unsigned char)*l) || !strncmp(l, "Exit code", 9) || !strncmp(l, "<tool_use_error>", 16))) {
                        if (!strncmp(l, "Exit code", 9)) { while (*l && *l != '\n') l++; } else if (!strncmp(l, "<tool_use_error>", 16)) l += 16; else l++; }
                    size_t n = 0; while (l[n] && l[n] != '\n') n++;
                    C[i].err = strndup(l, n > 160 ? 160 : n);
                }
                break;
            }
            free(t);
        }
    }
    if (!req || !*js_ws(req) || !strncmp(js_ws(req), "This session is being continued", 31)) goto done;
    {
        Buf b = {0}, rq = {0};
        S->nep++;
        char hd[48]; snprintf(hd, sizeof hd, "## Episode %d\n", S->nep); buf_str(&b, hd);
        buf_str(&b, "**Request**\n"); kn_text(S, req, KN_REQ_MAX, &rq); kn_quote(&b, kn_s(&rq));
        if (!S->title[0]) {                                         /* titulo e nome da nota: o primeiro pedido */
            Buf ln = {0}; kn_line(S, req, 100, &ln); kn_cpy(S->title, kn_s(&ln), sizeof S->title); free(ln.p);
            kn_cpy(S->first, req, sizeof S->first);
        }
        free(rq.p);
        if (last) { Buf o = {0}; kn_text(S, last, KN_OUT_MAX, &o); if (o.n) { buf_str(&b, "**Outcome**\n"); kn_quote(&b, kn_s(&o)); } free(o.p); }
        char **files = NULL; int nf = 0;
        for (int i = 0; i < nc; i++) if (C[i].kind == 2 && C[i].done && !C[i].fail && C[i].path && *C[i].path && nf < KN_FILES) kn_add_path(&files, &nf, C[i].path);
        if (nf) { buf_str(&b, "**Files written or edited**\n"); for (int i = 0; i < nf; i++) { buf_str(&b, "- "); kn_line(S, files[i], 300, &b); buf_str(&b, "\n"); free(files[i]); } }
        free(files);
        /* comandos que importaram: sem falha; os com descricao primeiro, depois os que nao sao ruido */
        char *sel = calloc((size_t)nc + 1, 1); int ns = 0;
        for (int pass = 0; pass < 2; pass++) for (int i = 0; i < nc && ns < KN_CMDS; i++) {
            if (C[i].kind != 1 || !C[i].done || C[i].fail || !C[i].cmd || !*js_ws(C[i].cmd) || sel[i]) continue;
            char w[64]; kn_first(C[i].cmd, w, sizeof w);
            if (pass == 0 ? !(C[i].desc && *C[i].desc) : kn_noise(w)) continue;
            sel[i] = 1; ns++;
        }
        if (ns) {
            buf_str(&b, "**Commands**\n");
            for (int i = 0; i < nc; i++) if (sel[i]) {
                buf_str(&b, "- ");
                if (C[i].desc && *C[i].desc) { kn_line(S, C[i].desc, 120, &b); buf_str(&b, ": "); }
                kn_line(S, C[i].cmd, 200, &b); buf_str(&b, "\n");
            }
        }
        free(sel);
        /* erros vencidos: comando que falhou e, depois, outro com a mesma primeira palavra que deu certo */
        char *used = calloc((size_t)nc + 1, 1); int np = 0; Buf pb = {0};
        for (int i = 0; i < nc && np < KN_PROBS; i++) {
            if (C[i].kind != 1 || !C[i].done || !C[i].fail || !C[i].cmd) continue;
            char w[64], w2[64]; kn_first(C[i].cmd, w, sizeof w);
            if (!w[0]) continue;
            for (int j = i + 1; j < nc; j++) {
                if (C[j].kind != 1 || !C[j].done || C[j].fail || used[j] || !C[j].cmd) continue;
                kn_first(C[j].cmd, w2, sizeof w2);
                if (strcmp(w, w2)) continue;
                used[j] = 1; np++;
                buf_str(&pb, "- `"); kn_line(S, C[i].cmd, 140, &pb); buf_str(&pb, "` failed");
                if (C[i].err && *C[i].err) { buf_str(&pb, " ("); kn_line(S, C[i].err, 120, &pb); buf_str(&pb, ")"); }
                buf_str(&pb, " -> `"); kn_line(S, C[j].cmd, 140, &pb); buf_str(&pb, "` worked\n");
                break;
            }
        }
        free(used);
        if (np) { buf_str(&b, "**Problems and fixes**\n"); buf_add(&b, pb.p, pb.n); }
        free(pb.p);
        buf_str(&b, "\n");
        buf_add(&S->body, b.p, b.n);
        free(b.p);
    }
done:
    for (int i = 0; i < nc; i++) { free(C[i].cmd); free(C[i].desc); free(C[i].path); free(C[i].err); }
    free(C); free(req); free(last);
}

/* ---------- arquivos e pastas ---------- */
static void kn_mkdirs(const char *path) {
    char p[4096]; kn_cpy(p, path, sizeof p);
    for (char *c = p + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(p, 0755); *c = '/'; }
    mkdir(p, 0755);
}
static void kn_base(char *out, size_t cap) {                /* pasta das notas: KATANA_KNOWLEDGE ou ~/.katana/knowledge */
    const char *e = getenv("KATANA_KNOWLEDGE"), *h = getenv("HOME");
    if (e && *e) kn_cpy(out, e, cap);
    else snprintf(out, cap, "%s/.katana/knowledge", h ? h : ".");
}
static void kn_slug(const char *s, char *out, size_t cap, size_t maxc, int keepdots) {
    size_t n = 0; int dash = 0;
    for (; *s && n < maxc && n + 1 < cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if (isalnum(c) && c < 128) { out[n++] = (char)tolower(c); dash = 0; }
        else if (keepdots && (c == '.' || c == '_')) { out[n++] = (char)c; dash = 0; }
        else if (!dash && n) { out[n++] = '-'; dash = 1; }
    }
    while (n && out[n - 1] == '-') n--;
    out[n] = 0;
}
/* ultimo componente de um caminho (aceita / e \) */
static const char *kn_leaf(const char *p) {
    const char *r = p; size_t n = strlen(p);
    while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\')) n--;
    for (size_t i = 0; i < n; i++) if (p[i] == '/' || p[i] == '\\') r = p + i + 1;
    return r;
}
static void kn_project(const char *cwd, char *out, size_t cap) {
    char tmp[256]; size_t n = strlen(kn_leaf(cwd)); while (n && (kn_leaf(cwd)[n - 1] == '/' || kn_leaf(cwd)[n - 1] == '\\')) n--;
    snprintf(tmp, sizeof tmp, "%.*s", (int)(n < 200 ? n : 200), kn_leaf(cwd));
    kn_slug(tmp, out, cap, 60, 1);
    if (!out[0]) kn_cpy(out, "unknown", cap);
}
static int kn_cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int kn_cmpstr_desc(const void *a, const void *b) { return strcmp(*(char *const *)b, *(char *const *)a); }
static char *kn_slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *s = xalloc((size_t)n + 1, 1);
    size_t r = fread(s, 1, (size_t)n, f); s[r] = 0; fclose(f);
    if (len) *len = r;
    return s;
}
/* nomes dos itens de uma pasta (sem . e ..; sem os que comecam com ponto), ordenados */
static int kn_ls(const char *dir, char ***v, int dirs_only) {
    int n = 0, cap = 0; *v = NULL;
    DIR *d = opendir(dir); if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[4096]; struct stat st; snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (stat(p, &st) != 0 || (S_ISDIR(st.st_mode) != (dirs_only != 0))) continue;
        if (n == cap) { cap = cap ? cap * 2 : 32; *v = realloc(*v, (size_t)cap * sizeof(char *)); }
        (*v)[n++] = strdup(de->d_name);
    }
    closedir(d);
    if (n) qsort(*v, (size_t)n, sizeof(char *), kn_cmpstr);
    return n;
}
static void kn_free(char **v, int n) { for (int i = 0; i < n; i++) free(v[i]); free(v); }
static int kn_md(const char *name) { size_t l = strlen(name); return l > 3 && !strcmp(name + l - 3, ".md") && strcmp(name, "INDEX.md") != 0; }

/* linha "Chave: valor" do cabecalho de uma nota */
static int kn_hdr(const char *txt, const char *key, char *out, size_t cap) {
    char pat[40]; snprintf(pat, sizeof pat, "\n%s: ", key);
    const char *p = strstr(txt, pat);
    if (!p) return 0;
    p += strlen(pat);
    size_t n = strcspn(p, "\n"); if (n >= cap) n = cap - 1;
    memcpy(out, p, n); out[n] = 0;
    return 1;
}

/* INDEX.md de cada projeto e o geral, a partir das notas que estao na pasta */
static void kn_indexes(const char *base) {
    char **pj; int np = kn_ls(base, &pj, 1);
    Buf top = {0}; char path[4096];
    buf_str(&top, "# Knowledge from earlier sessions\n\nOne folder per project; each folder has an INDEX.md and one note per session. Search the notes with Grep.\n\n");
    for (int i = 0; i < np; i++) {
        char dir[4096]; snprintf(dir, sizeof dir, "%s/%s", base, pj[i]);
        char **nt; int nn = kn_ls(dir, &nt, 0), cnt = 0;
        if (nn) qsort(nt, (size_t)nn, sizeof(char *), kn_cmpstr_desc);
        Buf ix = {0}, lines = {0}; char cwd[512] = "", newest[16] = "", name[200];
        for (int j = 0; j < nn; j++) {
            if (!kn_md(nt[j])) continue;
            snprintf(path, sizeof path, "%s/%s", dir, nt[j]);
            char *t = kn_slurp(path, NULL); if (!t) continue;
            char date[16] = "", eps[16] = "", cw[512] = "", title[200] = "";
            const char *nl = strchr(t, '\n'); size_t tl = nl ? (size_t)(nl - t) : strlen(t);
            if (tl > 2) { snprintf(title, sizeof title, "%.*s", (int)(tl - 2 < 150 ? tl - 2 : 150), t + 2); }
            kn_hdr(t, "Date", date, sizeof date); kn_hdr(t, "Episodes", eps, sizeof eps); kn_hdr(t, "Project", cw, sizeof cw);
            if (!cwd[0]) kn_cpy(cwd, cw, sizeof cwd);
            if (!newest[0]) kn_cpy(newest, date, sizeof newest);
            buf_str(&lines, "- "); buf_str(&lines, date); buf_str(&lines, " [");
            buf_str(&lines, title); buf_str(&lines, "]("); buf_str(&lines, nt[j]); buf_str(&lines, "), "); buf_str(&lines, eps); buf_str(&lines, " episodes\n");
            cnt++; free(t);
        }
        kn_free(nt, nn);
        if (!cnt) { free(lines.p); free(ix.p); continue; }
        char hd[64]; snprintf(hd, sizeof hd, "Notes: %d\n\n", cnt);
        buf_str(&ix, "# Project "); buf_str(&ix, pj[i]); buf_str(&ix, "\n\nFolder: "); buf_str(&ix, cwd); buf_str(&ix, "\n"); buf_str(&ix, hd); buf_add(&ix, lines.p, lines.n);
        snprintf(path, sizeof path, "%s/INDEX.md", dir);
        FILE *f = fopen(path, "wb"); if (f) { fwrite(ix.p, 1, ix.n, f); fclose(f); }
        snprintf(name, sizeof name, "%s", pj[i]);
        buf_str(&top, "- ["); buf_str(&top, name); buf_str(&top, "]("); buf_str(&top, name); buf_str(&top, "/INDEX.md): ");
        buf_str(&top, cwd); snprintf(hd, sizeof hd, ", %d notes, newest ", cnt); buf_str(&top, hd); buf_str(&top, newest); buf_str(&top, "\n");
        free(lines.p); free(ix.p);
    }
    kn_free(pj, np);
    snprintf(path, sizeof path, "%s/INDEX.md", base);
    FILE *f = fopen(path, "wb"); if (f) { fwrite(top.p, 1, top.n, f); fclose(f); }
    free(top.p);
}

/* ---------- build ---------- */
static int kn_isdir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

static int kn_build(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "uso: katana knowledge build <raiz|pasta|arquivo.jsonl> [--out PASTA] [--force] [--replace REGEX=TEXTO]... [--cut-matching REGEX]\n"); return 1; }
    const char *in = argv[0]; char base[4096]; int force = 0;
    kn_base(base, sizeof base);
    TrOpt tro; memset(&tro, 0, sizeof tro);
    tro.max_result = 4000;
    KnSess S; memset(&S, 0, sizeof S);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) kn_cpy(base, argv[++i], sizeof base);
        else if (!strcmp(argv[i], "--force")) force = 1;
        else if (!strcmp(argv[i], "--replace") && i + 1 < argc) { if (!tr_add_rep(&tro, argv[++i])) return 1; }
        else if (!strcmp(argv[i], "--cut-matching") && i + 1 < argc) {
            char err[400]; const char *pat = argv[++i];
            S.cut = rx_compile(pat, strlen(pat), RX_I, err, sizeof err);
            if (!S.cut) { fprintf(stderr, "knowledge: --cut-matching %s: %s\n", pat, err); return 1; }
        }
        else { fprintf(stderr, "knowledge: opcao desconhecida: %s\n", argv[i]); return 1; }
    }
    tro.emit = kn_emit; tro.ud = &S; S.o = &tro;
    char **files = NULL; int nfiles = 0, cap = 0;
    if (kn_isdir(in)) { tr_walk(in, &files, &nfiles, &cap); if (nfiles) qsort(files, (size_t)nfiles, sizeof(char *), kn_cmpstr); }
    else { files = malloc(sizeof(char *)); files[0] = strdup(in); nfiles = 1; }
    char srcdir[4200]; snprintf(srcdir, sizeof srcdir, "%s/.src", base);
    kn_mkdirs(srcdir);
    long wrote = 0, skipped = 0, empty = 0, eps = 0, subs = 0;
    const time_t t0 = time(NULL);
    for (int fi = 0; fi < nfiles; fi++) {
        const char *fp = files[fi], *bn = kn_leaf(fp);
        struct stat st, ss;
        if (stat(fp, &st) != 0) continue;
        if (strstr(fp, "/subagents/")) { subs++; continue; }          /* sub-agentes: o relatorio final ja esta no resultado da sessao */
        uint64_t hp = 1469598103934665603ULL; for (const char *c = fp; *c; c++) { hp ^= (unsigned char)*c; hp *= 1099511628211ULL; }
        char stamp[4400]; snprintf(stamp, sizeof stamp, "%s/%016llx", srcdir, (unsigned long long)hp);     /* marcador por caminho: dois arquivos com o mesmo nome em pastas diferentes */
        char *old = NULL;
        if (stat(stamp, &ss) == 0) {
            if (!force && ss.st_mtime > st.st_mtime) { skipped++; continue; }
            old = kn_slurp(stamp, NULL);
        }
        if (fi && fi % 100 == 0) fprintf(stderr, "knowledge: %d de %d transcricoes (%lds)\n", fi, nfiles, (long)(time(NULL) - t0));
        free(S.body.p); memset(&S.body, 0, sizeof S.body); S.nep = 0; S.cwd[0] = S.ts[0] = S.title[0] = S.first[0] = 0;
        tr_run(&tro, fp);
        if (old && *old != '-') { char *nl = strchr(old, '\n'); if (nl) *nl = 0; char op[4400]; snprintf(op, sizeof op, "%s/%s", base, old); remove(op); }
        free(old);
        eps += S.nep;
        if (!S.nep) {
            empty++;
            FILE *f = fopen(stamp, "wb"); if (f) { fputs("-\n", f); fclose(f); }
            continue;
        }
        if (!S.cwd[0]) { const char *par = strrchr(fp, '/'); char tmp[512] = ""; if (par) snprintf(tmp, sizeof tmp, "%.*s", (int)(par - fp), fp); kn_cpy(S.cwd, kn_leaf(tmp), sizeof S.cwd); }
        char proj[80]; kn_project(S.cwd, proj, sizeof proj);
        char date[16] = "";
        if (strlen(S.ts) >= 10 && isdigit((unsigned char)S.ts[0]) && S.ts[4] == '-') { memcpy(date, S.ts, 10); date[10] = 0; }
        else { struct tm tm; time_t m = st.st_mtime; localtime_r(&m, &tm); strftime(date, sizeof date, "%Y-%m-%d", &tm); }
        char slug[64]; kn_slug(S.first, slug, sizeof slug, 40, 0); if (!slug[0]) kn_cpy(slug, "session", sizeof slug);
        uint64_t h = 1469598103934665603ULL; for (const char *c = bn; *c; c++) { h ^= (unsigned char)*c; h *= 1099511628211ULL; }
        char pdir[4200], npath[4400], rel[300];
        snprintf(pdir, sizeof pdir, "%s/%s", base, proj);
        kn_mkdirs(pdir);
        snprintf(rel, sizeof rel, "%s/%s-%s-%06x.md", proj, date, slug, (unsigned)(h & 0xFFFFFF));
        snprintf(npath, sizeof npath, "%s/%s", base, rel);
        Buf hd = {0}, sn = {0}; char num[32];
        kn_mask(S.title, &sn);
        buf_str(&hd, "# "); buf_str(&hd, kn_s(&sn)); buf_str(&hd, "\n\nProject: ");
        kn_line(&S, S.cwd, 300, &hd); buf_str(&hd, "\nDate: "); buf_str(&hd, date);
        buf_str(&hd, "\nSession: "); buf_str(&hd, bn);
        snprintf(num, sizeof num, "\nEpisodes: %d\n\n", S.nep); buf_str(&hd, num);
        FILE *f = fopen(npath, "wb");
        if (f) { fwrite(hd.p, 1, hd.n, f); fwrite(S.body.p, 1, S.body.n, f); fclose(f); wrote++; }
        free(hd.p); free(sn.p);
        f = fopen(stamp, "wb"); if (f) { fprintf(f, "%s\n", rel); fclose(f); }
    }
    for (int i = 0; i < nfiles; i++) free(files[i]);
    free(files);
    kn_indexes(base);
    printf("knowledge: %d transcricoes (%ld de sub-agentes ignoradas), %ld notas escritas, %ld ja atualizadas, %ld sem pedido, %ld episodios, %lds; pasta %s\n",
           nfiles, subs, wrote, skipped, empty, eps, (long)(time(NULL) - t0), base);
    return 0;
}

/* ---------- stats ---------- */
static int kn_corpus_stats(const char *path);
static int kn_stats(int argc, char **argv) {
    char base[4096]; kn_base(base, sizeof base);
    if (argc >= 1) { struct stat st; if (stat(argv[0], &st) == 0 && S_ISREG(st.st_mode)) return kn_corpus_stats(argv[0]); kn_cpy(base, argv[0], sizeof base); }
    char **pj; int np = kn_ls(base, &pj, 1);
    long notes = 0, eps = 0, bytes = 0, nproj = 0; time_t newest = 0; char newest_path[4400] = "";
    for (int i = 0; i < np; i++) {
        char dir[4200]; snprintf(dir, sizeof dir, "%s/%s", base, pj[i]);
        char **nt; int nn = kn_ls(dir, &nt, 0), c = 0;
        for (int j = 0; j < nn; j++) {
            if (!kn_md(nt[j])) continue;
            char path[4400]; struct stat st; snprintf(path, sizeof path, "%s/%s", dir, nt[j]);
            if (stat(path, &st) != 0) continue;
            char *t = kn_slurp(path, NULL); if (!t) continue;
            for (const char *p = t; (p = strstr(p, "\n## Episode ")); p++) eps++;
            free(t);
            c++; notes++; bytes += (long)st.st_size;
            if (st.st_mtime > newest) { newest = st.st_mtime; kn_cpy(newest_path, path, sizeof newest_path); }
        }
        if (c) nproj++;
        kn_free(nt, nn);
    }
    kn_free(pj, np);
    printf("pasta: %s\nprojetos: %ld\nnotas: %ld\nepisodios: %ld\nbytes: %ld\n", base, nproj, notes, eps, bytes);
    if (newest) { char d[32]; struct tm tm; localtime_r(&newest, &tm); strftime(d, sizeof d, "%Y-%m-%d %H:%M", &tm); printf("nota mais nova: %s (%s)\n", newest_path, d); }
    return 0;
}

/* ---------- search ---------- */
typedef struct { char *path, *req, *out; char date[12]; int all, cnt, ep; } KnHit;
static int kn_cmphit(const void *a, const void *b) {
    const KnHit *x = a, *y = b;
    if (x->all != y->all) return y->all - x->all;
    if (x->cnt != y->cnt) return y->cnt - x->cnt;
    int d = strcmp(y->date, x->date);
    return d ? d : strcmp(x->path, y->path);
}
static void kn_lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }
static int kn_search(int argc, char **argv) {
    char base[4096]; kn_base(base, sizeof base);
    const char *words[16]; int nw = 0, limit = 10; const char *project = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--project") && i + 1 < argc) project = argv[++i];
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dir") && i + 1 < argc) kn_cpy(base, argv[++i], sizeof base);
        else if (nw < 16) { char *w = strdup(argv[i]); kn_lower(w); words[nw++] = w; }
    }
    if (!nw) { fprintf(stderr, "uso: katana knowledge search <palavras...> [--project NOME] [--limit N] [--dir PASTA]\n"); return 1; }
    char pl[200] = ""; if (project) { kn_cpy(pl, project, sizeof pl); kn_lower(pl); }
    char **pj; int np = kn_ls(base, &pj, 1);
    KnHit *H = NULL; int nh = 0;
    for (int i = 0; i < np; i++) {
        if (project) { char lo[200]; kn_cpy(lo, pj[i], sizeof lo); kn_lower(lo); if (!strstr(lo, pl)) continue; }
        char dir[4200]; snprintf(dir, sizeof dir, "%s/%s", base, pj[i]);
        char **nt; int nn = kn_ls(dir, &nt, 0);
        for (int j = 0; j < nn; j++) {
            if (!kn_md(nt[j])) continue;
            char path[4400]; snprintf(path, sizeof path, "%s/%s", dir, nt[j]);
            char *t = kn_slurp(path, NULL); if (!t) continue;
            char *lc = strdup(t); kn_lower(lc);
            char *blk = strstr(t, "\n## Episode ");
            int ep = 0;
            while (blk) {
                char *next = strstr(blk + 1, "\n## Episode ");
                size_t off = (size_t)(blk - t), len = next ? (size_t)(next - blk) : strlen(blk);
                ep++;
                char save = lc[off + len]; lc[off + len] = 0;
                int cnt = 0;
                for (int w = 0; w < nw; w++) if (strstr(lc + off, words[w])) cnt++;
                lc[off + len] = save;
                if (cnt) {
                    KnHit h; memset(&h, 0, sizeof h);
                    h.path = strdup(path); h.cnt = cnt; h.all = cnt == nw; h.ep = ep;
                    kn_cpy(h.date, nt[j], sizeof h.date); h.date[10] = 0;
                    const char *r = strstr(blk, "**Request**\n> ");
                    if (r && (size_t)(r - blk) < len) { r += 14; size_t l = strcspn(r, "\n"); h.req = strndup(r, l > 200 ? 200 : l); } else h.req = strdup("");
                    Buf ob = {0}; const char *o = strstr(blk, "**Outcome**\n");
                    if (o && (size_t)(o - blk) < len) {
                        for (o += 12; *o == '>' && ob.n < 300; ) {
                            o++; if (*o == ' ') o++;
                            size_t l = strcspn(o, "\n"); if (ob.n) buf_str(&ob, " "); buf_add(&ob, o, l); o += l; if (*o) o++;
                        }
                    }
                    if (ob.n > 300) { size_t a = 300; while (a && ((unsigned char)ob.p[a] & 0xC0) == 0x80) a--; ob.p[a] = 0; }
                    h.out = ob.p ? ob.p : strdup("");
                    H = realloc(H, (size_t)(nh + 1) * sizeof h); H[nh++] = h;
                }
                blk = next;
            }
            free(lc); free(t);
        }
        kn_free(nt, nn);
    }
    kn_free(pj, np);
    if (nh) qsort(H, (size_t)nh, sizeof(KnHit), kn_cmphit);
    for (int i = 0; i < nh && i < limit; i++)
        printf("%s (episode %d, %d of %d words)\n  request: %s\n  outcome: %s\n", H[i].path, H[i].ep, H[i].cnt, nw, H[i].req, H[i].out);
    if (!nh) printf("nada encontrado em %s\n", base);
    return 0;
}

/* ---------- corpus: as notas viram pares pergunta -> resposta para o treino ----------
 * So extracao: a resposta e o texto da nota (nada e inventado; campo vazio nao gera par). Cada fato sai com duas
 * formulacoes no treino (0 e 1) e uma terceira (2) so no arquivo --holdout, para medir depois se o modelo responde
 * a uma pergunta que nao viu. A pergunta leva sempre o nome do projeto. */
typedef struct { char *req, *out, *files, *cmds, *probs; } KnEp;
typedef struct { char date[12]; KnEp *e; int n; } KnNote;
typedef struct { int type, pt; char *x, *ans; } KnFact;

/* formulacoes: [tipo][0 ingles, 1 portugues][0, 1 treino; 2 so holdout]; {R} pedido, {P} projeto, {X} complemento */
static const char *const KN_Q[7][2][3] = {
    { { "What was done about \"{R}\" in the project {P}?", "In the project {P}, how did the request \"{R}\" end up?", "Summarize what happened with the request \"{R}\" in {P}." },
      { "O que foi feito sobre \"{R}\" no projeto {P}?", "No projeto {P}, como terminou o pedido \"{R}\"?", "Resuma o que aconteceu com o pedido \"{R}\" em {P}." } },
    { { "Which files were changed for \"{R}\" in {P}?", "In {P}, what files did the request \"{R}\" touch?", "List the files written or edited when we handled \"{R}\" in {P}." },
      { "Quais arquivos foram alterados para \"{R}\" no projeto {P}?", "No {P}, que arquivos o pedido \"{R}\" mexeu?", "Liste os arquivos gravados ou editados quando tratamos de \"{R}\" em {P}." } },
    { { "How do we {X} in {P}?", "In {P}, which command does this: {X}?", "What is the command to {X} in the project {P}?" },
      { "Como fazemos para {X} no projeto {P}?", "No {P}, qual comando faz isto: {X}?", "Qual e o comando para {X} no projeto {P}?" } },
    { { "In {P}, {X}. What worked?", "{X} (project {P}). What was the fix?", "Project {P}: {X}. How did we get past it?" },
      { "No {P}, {X}. O que funcionou?", "{X} (projeto {P}). Qual foi a correcao?", "Projeto {P}: {X}. Como resolvemos?" } },
    { { "What is {P}?", "Tell me about the project {P}.", "Give me an overview of {P}." },
      { "O que e o {P}?", "Fale do projeto {P}.", "Faca uma visao geral do {P}." } },
    { { "Where is {P}?", "In which folder does the project {P} live?", "What is the path of {P}?" },
      { "Onde fica o {P}?", "Em que pasta mora o projeto {P}?", "Qual e o caminho do {P}?" } },
    { { "What are the main files of {P}?", "Which files do we touch most in {P}?", "List the files of {P} that change the most." },
      { "Quais sao os arquivos principais do {P}?", "Quais arquivos mais mexemos no {P}?", "Liste os arquivos do {P} que mais mudam." } },
};

static int kn_is_pt(const char *t) {
    static const char *const sw[] = { "de", "do", "da", "que", "para", "com", "uma", "um", "na", "em", "por", "isso", "como", "qual", "voce", "pra", "fazer", "faca", "nao" };
    int hits = 0;
    for (const unsigned char *p = (const unsigned char *)t; *p; p++) if (*p == 0xC3 && p[1] >= 0xA0 && p[1] <= 0xBF) return 1;   /* acentos latinos */
    for (const char *p = t; *p; ) {
        while (*p && !isalpha((unsigned char)*p)) p++;
        char w[16]; size_t n = 0;
        while (isalpha((unsigned char)*p)) { if (n < 15) w[n++] = (char)tolower((unsigned char)*p); p++; }
        w[n] = 0;
        for (size_t i = 0; i < sizeof sw / sizeof sw[0]; i++) if (!strcmp(w, sw[i])) hits++;
    }
    return hits >= 2;
}

static void kn_fill(const char *tpl, const char *r, const char *p, const char *x, Buf *out) {
    for (const char *c = tpl; *c; c++) {
        if (c[0] == '{' && c[2] == '}' && (c[1] == 'R' || c[1] == 'P' || c[1] == 'X')) { buf_str(out, c[1] == 'R' ? r : c[1] == 'P' ? p : x); c += 2; }
        else buf_add(out, c, 1);
    }
}

/* primeira linha de um texto, ate cap bytes, sem partir UTF-8 */
static char *kn_first_line(const char *t, size_t cap) {
    size_t n = strcspn(t, "\n");
    if (n > cap) { n = cap; while (n && ((unsigned char)t[n] & 0xC0) == 0x80) n--; }
    char *s = strndup(t, n);
    for (size_t i = strlen(s); i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\r'); i--) s[i - 1] = 0;
    return s;
}

static void kn_sec_add(char **dst, const char *line, size_t l, int strip) {      /* acrescenta uma linha (sem o prefixo "> " ou "- ") */
    if (strip && l >= 1) { size_t sk = (line[0] == '>' || line[0] == '-') ? (l >= 2 && line[1] == ' ' ? 2 : 1) : 0; line += sk; l -= sk; }
    size_t o = *dst ? strlen(*dst) : 0;
    *dst = realloc(*dst, o + l + 2);
    if (o) (*dst)[o++] = '\n';
    memcpy(*dst + o, line, l); (*dst)[o + l] = 0;
}

/* nota -> episodios */
static void kn_parse_note(const char *t, KnNote *N) {
    memset(N, 0, sizeof *N);
    const char *blk = strstr(t, "\n## Episode ");
    while (blk) {
        const char *next = strstr(blk + 1, "\n## Episode "), *end = next ? next : blk + strlen(blk);
        KnEp e; memset(&e, 0, sizeof e);
        char **cur = NULL;
        for (const char *p = strchr(blk + 1, '\n'); p && p < end; ) {
            p++;
            const char *nl = memchr(p, '\n', (size_t)(end - p)); size_t l = nl ? (size_t)(nl - p) : (size_t)(end - p);
            if (!strncmp(p, "**Request**", 11)) cur = &e.req;
            else if (!strncmp(p, "**Outcome**", 11)) cur = &e.out;
            else if (!strncmp(p, "**Files written or edited**", 27)) cur = &e.files;
            else if (!strncmp(p, "**Commands**", 12)) cur = &e.cmds;
            else if (!strncmp(p, "**Problems and fixes**", 22)) cur = &e.probs;
            else if (cur && l) kn_sec_add(cur, p, l, 1);
            p = nl;
        }
        N->e = realloc(N->e, (size_t)(N->n + 1) * sizeof e); N->e[N->n++] = e;
        blk = next;
    }
}
static void kn_note_free(KnNote *N) { for (int i = 0; i < N->n; i++) { free(N->e[i].req); free(N->e[i].out); free(N->e[i].files); free(N->e[i].cmds); free(N->e[i].probs); } free(N->e); }

static uint64_t kn_norm_hash(const char *s) {          /* hash do texto so com letras e numeros, minusculo */
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) if (isalnum((unsigned char)*s)) { h ^= (unsigned char)tolower((unsigned char)*s); h *= 1099511628211ULL; }
    return h;
}

typedef struct { KnFact *v; int n, cap, lim; uint64_t *seen; int nseen, capseen; } KnFacts;
static void kn_fact(KnFacts *F, int type, int pt, const char *x, const char *ans) {
    if (!ans || !*ans || F->n * 2 >= F->lim) return;                 /* teto de pares do projeto: 2 por fato */
    const uint64_t h = kn_norm_hash(x) * 31 + (uint64_t)type;     /* mesma pergunta duas vezes: fica a mais recente (a primeira vista) */
    for (int i = 0; i < F->nseen; i++) if (F->seen[i] == h) return;
    if (F->nseen == F->capseen) { F->capseen = F->capseen ? F->capseen * 2 : 64; F->seen = realloc(F->seen, (size_t)F->capseen * sizeof(uint64_t)); }
    F->seen[F->nseen++] = h;
    if (F->n == F->cap) { F->cap = F->cap ? F->cap * 2 : 64; F->v = realloc(F->v, (size_t)F->cap * sizeof(KnFact)); }
    KnFact f = { type, pt, strdup(x), strdup(ans) };
    F->v[F->n++] = f;
}
static void kn_corpus_line(FILE *f, const char *proj, const char *q, const char *a) {
    Buf l = {0};
    buf_str(&l, "{\"messages\": [{\"role\": \"user\", \"content\": \""); js_escape(&l, q, strlen(q));
    buf_str(&l, "\"}, {\"role\": \"assistant\", \"content\": \""); js_escape(&l, a, strlen(a));
    buf_str(&l, "\"}], \"project\": \""); js_escape(&l, proj, strlen(proj)); buf_str(&l, "\"}\n");
    fwrite(l.p, 1, l.n, f); free(l.p);
}
static long kn_chars(const char *s) { long n = 0; for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++; return n; }
static void kn_budget(long pairs, long chars) {
    const double w = (double)chars / 3.3 / 512.0;
    printf("  janelas de 512 tokens (estimativa: caracteres / 3,3 / 512): %.0f; noites a 100-150 janelas por noite: %.1f a %.1f (estimativa)\n", w, w / 150.0, w / 100.0);
    (void)pairs;
}

typedef struct { char *path; int n; } KnPathCount;
static int kn_cmp_pc(const void *a, const void *b) { return ((const KnPathCount *)b)->n - ((const KnPathCount *)a)->n; }

static int kn_corpus(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: katana knowledge corpus <pasta das notas> <saida.jsonl> [--project NOME]... [--max-per-project N] [--holdout ARQ.jsonl]\n"); return 1; }
    const char *dir = argv[0], *outp = argv[1], *hold = NULL; const char *projs[32]; int np = 0, cap = 400;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--project") && i + 1 < argc && np < 32) projs[np++] = argv[++i];
        else if (!strcmp(argv[i], "--max-per-project") && i + 1 < argc) cap = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--holdout") && i + 1 < argc) hold = argv[++i];
        else { fprintf(stderr, "knowledge corpus: opcao desconhecida: %s\n", argv[i]); return 1; }
    }
    FILE *fo = fopen(outp, "wb"), *fh = hold ? fopen(hold, "wb") : NULL;
    if (!fo || (hold && !fh)) { fprintf(stderr, "knowledge corpus: nao gravou %s\n", fo ? hold : outp); return 1; }
    char **pj; int npj = kn_ls(dir, &pj, 1);
    long tp = 0, tc = 0, th = 0, nproj = 0;
    for (int i = 0; i < npj; i++) {
        if (np) { int ok = 0; char lo[200]; kn_cpy(lo, pj[i], sizeof lo); kn_lower(lo);
            for (int k = 0; k < np; k++) { char w[200]; kn_cpy(w, projs[k], sizeof w); kn_lower(w); if (strstr(lo, w)) ok = 1; }
            if (!ok) continue; }
        char pd[4200]; snprintf(pd, sizeof pd, "%s/%s", dir, pj[i]);
        char **nt; int nn = kn_ls(pd, &nt, 0);
        if (nn) qsort(nt, (size_t)nn, sizeof(char *), kn_cmpstr_desc);       /* o nome comeca pela data: mais recentes primeiro */
        KnNote *N = calloc((size_t)nn + 1, sizeof(KnNote)); int nnotes = 0; char cwd[512] = "";
        int ptc = 0, enc = 0;
        for (int j = 0; j < nn; j++) {
            if (!kn_md(nt[j])) continue;
            char path[4400]; snprintf(path, sizeof path, "%s/%s", pd, nt[j]);
            char *t = kn_slurp(path, NULL); if (!t) continue;
            kn_parse_note(t, &N[nnotes]);
            kn_cpy(N[nnotes].date, nt[j], sizeof N[nnotes].date); N[nnotes].date[10] = 0;
            if (!cwd[0]) kn_hdr(t, "Project", cwd, sizeof cwd);
            free(t);
            for (int k = 0; k < N[nnotes].n; k++) { if (N[nnotes].e[k].req && kn_is_pt(N[nnotes].e[k].req)) ptc++; else enc++; }
            nnotes++;
        }
        kn_free(nt, nn);
        if (!nnotes) { free(N); continue; }
        const int ppt = ptc > enc;
        KnFacts F; memset(&F, 0, sizeof F); F.lim = cap;
        /* visao geral do projeto: primeiros pedidos, pasta, arquivos mais mexidos */
        {
            Buf a = {0}; char nb[160];
            const char *first = N[nnotes - 1].date, *last = N[0].date;
            long total = 0; for (int j = 0; j < nnotes; j++) total += N[j].n;
            snprintf(nb, sizeof nb, ppt ? " Ha %d sessoes registradas, de %s a %s, com %ld pedidos." : " There are %d recorded sessions, from %s to %s, with %ld requests.", nnotes, first, last, total);
            buf_str(&a, pj[i]); buf_str(&a, ppt ? " e um projeto na pasta " : " is a project in the folder "); buf_str(&a, cwd); buf_str(&a, "."); buf_str(&a, nb);
            int shown = 0;
            for (int j = nnotes - 1; j >= 0 && shown < 3; j--) for (int k = 0; k < N[j].n && shown < 3; k++) {
                if (!N[j].e[k].req) continue;
                char *r = kn_first_line(N[j].e[k].req, 120);
                if (strlen(r) >= 12) { buf_str(&a, shown ? "; \"" : (ppt ? " Comecou com pedidos como: \"" : " It started with requests such as: \"")); buf_str(&a, r); buf_str(&a, "\""); shown++; }
                free(r);
            }
            if (shown) buf_str(&a, ".");
            kn_fact(&F, 4, ppt, "", kn_s(&a)); free(a.p);
            Buf w = {0}; buf_str(&w, pj[i]); buf_str(&w, ppt ? " fica em " : " is in "); buf_str(&w, cwd); buf_str(&w, ".");
            kn_fact(&F, 5, ppt, "", cwd[0] ? kn_s(&w) : ""); free(w.p);
            KnPathCount *pc = NULL; int npc = 0;
            for (int j = 0; j < nnotes; j++) for (int k = 0; k < N[j].n; k++) {
                if (!N[j].e[k].files) continue;
                for (const char *p = N[j].e[k].files; *p; ) {
                    size_t l = strcspn(p, "\n"); char *pa = strndup(p, l); int f = -1;
                    for (int q = 0; q < npc; q++) if (!strcmp(pc[q].path, pa)) f = q;
                    if (f < 0) { pc = realloc(pc, (size_t)(npc + 1) * sizeof *pc); pc[npc].path = pa; pc[npc++].n = 1; } else { pc[f].n++; free(pa); }
                    p += l; if (*p) p++;
                }
            }
            if (npc) qsort(pc, (size_t)npc, sizeof *pc, kn_cmp_pc);
            Buf m = {0};
            for (int q = 0; q < npc && q < 8; q++) { char b2[32]; snprintf(b2, sizeof b2, ppt ? " (%d vezes)" : " (%d times)", pc[q].n); buf_str(&m, q ? "\n- " : "- "); buf_str(&m, pc[q].path); buf_str(&m, b2); }
            if (m.n) { Buf mm = {0}; buf_str(&mm, ppt ? "Arquivos mais mexidos no " : "Files touched most often in "); buf_str(&mm, pj[i]); buf_str(&mm, ":\n"); buf_add(&mm, m.p, m.n); kn_fact(&F, 6, ppt, "", kn_s(&mm)); free(mm.p); }
            free(m.p);
            for (int q = 0; q < npc; q++) free(pc[q].path);
            free(pc);
        }
        /* episodios, dos mais recentes aos mais antigos, ate o teto de pares (2 por fato) */
        for (int j = 0; j < nnotes && F.n * 2 < cap; j++) for (int k = 0; k < N[j].n && F.n * 2 < cap; k++) {
            KnEp *e = &N[j].e[k];
            if (!e->req) continue;
            char *r = kn_first_line(e->req, 160);
            if (strlen(r) < 12) { free(r); continue; }
            const int pt = kn_is_pt(e->req);
            kn_fact(&F, 0, pt, r, e->out);
            if (e->files) { Buf a = {0}; buf_str(&a, pt ? "Arquivos alterados:\n" : "Files changed:\n"); for (const char *p = e->files; *p; ) { size_t l = strcspn(p, "\n"); buf_str(&a, "- "); buf_add(&a, p, l); buf_str(&a, "\n"); p += l; if (*p) p++; } kn_trim(&a); kn_fact(&F, 1, pt, r, kn_s(&a)); free(a.p); }
            if (e->cmds) for (const char *p = e->cmds; *p; ) {              /* "descricao: comando": so as que tem descricao */
                size_t l = strcspn(p, "\n"); char *ln = strndup(p, l); char *sep = strstr(ln, ": ");
                if (sep && sep > ln && sep - ln <= 120 && strcspn(ln, "\"`=|&$") >= (size_t)(sep - ln)) { *sep = 0; char d[130]; kn_cpy(d, ln, sizeof d); if (isupper((unsigned char)d[0])) d[0] = (char)tolower((unsigned char)d[0]); Buf a = {0}; buf_str(&a, "`"); buf_str(&a, sep + 2); buf_str(&a, "`"); kn_fact(&F, 2, pt, d, kn_s(&a)); free(a.p); }
                free(ln); p += l; if (*p) p++;
            }
            if (e->probs) for (const char *p = e->probs; *p; ) {              /* "`cmd` failed (erro) -> `cmd2` worked" */
                size_t l = strcspn(p, "\n"); char *ln = strndup(p, l);
                char *arrow = strstr(ln, " -> `"); char *w = arrow ? strstr(arrow, "` worked") : NULL;
                if (arrow && w && ln[0] == '`') {
                    *arrow = 0; *w = 0;
                    Buf x = {0}; char *fl = pt ? strstr(ln, "` failed") : NULL;
                    if (fl) { buf_add(&x, ln, (size_t)(fl - ln)); buf_str(&x, "` falhou"); buf_str(&x, fl + 8); } else buf_str(&x, ln);
                    Buf a = {0}; buf_str(&a, pt ? "Funcionou: `" : "What worked: `"); buf_str(&a, arrow + 5); buf_str(&a, "`");
                    kn_fact(&F, 3, pt, kn_s(&x), kn_s(&a)); free(x.p); free(a.p);
                }
                free(ln); p += l; if (*p) p++;
            }
            free(r);
        }
        long pairs = 0, chars = 0, hp = 0;
        for (int q = 0; q < F.n; q++) {
            KnFact *f = &F.v[q];
            for (int ph = 0; ph < 3; ph++) {
                Buf qb = {0};
                kn_fill(KN_Q[f->type][f->pt][ph], f->type < 4 && f->type != 2 && f->type != 3 ? f->x : "", pj[i], f->type == 2 || f->type == 3 ? f->x : "", &qb);
                if (ph < 2) { kn_corpus_line(fo, pj[i], kn_s(&qb), f->ans); pairs++; chars += kn_chars(f->ans); }
                else if (fh) { kn_corpus_line(fh, pj[i], kn_s(&qb), f->ans); hp++; }
                free(qb.p);
            }
            free(f->x); free(f->ans);
        }
        free(F.v); free(F.seen);
        for (int j = 0; j < nnotes; j++) kn_note_free(&N[j]);
        free(N);
        printf("  %s: %d fatos, %ld pares, %ld caracteres de resposta\n", pj[i], F.n, pairs, chars);
        tp += pairs; tc += chars; th += hp; nproj++;
    }
    kn_free(pj, npj);
    fclose(fo); if (fh) fclose(fh);
    printf("corpus: %ld projetos, %ld pares (%ld caracteres de resposta)%s; arquivo %s\n", nproj, tp, tc, hold ? "" : "", outp);
    if (hold) printf("holdout: %ld pares, uma formulacao que o treino nao tem, por fato; arquivo %s\n", th, hold);
    kn_budget(tp, tc);
    return 0;
}

/* katana knowledge stats <corpus.jsonl>: pares por projeto, caracteres de resposta e janelas de treino */
static int kn_corpus_stats(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "knowledge: nao abriu %s\n", path); return 1; }
    char names[256][80]; long np[256] = {0}, nc[256] = {0}; int n = 0; long tp = 0, tc = 0;
    size_t lc = 1 << 16; char *line = xalloc(lc, 1);
    for (;;) {
        size_t len = 0; int ch;
        while ((ch = fgetc(f)) != EOF && ch != '\n') { if (len + 2 >= lc) { lc *= 2; line = realloc(line, lc); } line[len++] = (char)ch; }
        line[len] = 0;
        if (len) {
            char k[64], proj[80] = "?"; const char *v, *e; long ans = 0;
            const char *q = js_ws(line);
            if (*q == '{') for (const char *p = q + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) {
                if (!strcmp(k, "project")) { char *s = kn_str(v); kn_cpy(proj, s, sizeof proj); free(s); }
                else if (!strcmp(k, "messages") && *v == '[') {
                    const char *el, *ee;
                    for (const char *p2 = v + 1; (ee = gs_elem(p2, &el)); p2 = ee) {
                        char k2[32], role[16] = ""; const char *x, *xe, *c = NULL;
                        for (const char *p3 = el + 1; (xe = gs_member(p3, k2, sizeof k2, &x)); p3 = xe) {
                            if (!strcmp(k2, "role")) { char *s = kn_str(x); kn_cpy(role, s, sizeof role); free(s); }
                            else if (!strcmp(k2, "content")) c = x;
                        }
                        if (!strcmp(role, "assistant") && c) { char *s = kn_str(c); ans += kn_chars(s); free(s); }
                    }
                }
            }
            int id = -1; for (int i = 0; i < n; i++) if (!strcmp(names[i], proj)) id = i;
            if (id < 0 && n < 256) { kn_cpy(names[n], proj, sizeof names[n]); id = n++; }
            if (id >= 0) { np[id]++; nc[id] += ans; }
            tp++; tc += ans;
        }
        if (ch == EOF) break;
    }
    fclose(f); free(line);
    printf("corpus: %s\n", path);
    for (int i = 0; i < n; i++) printf("  %s: %ld pares, %ld caracteres de resposta\n", names[i], np[i], nc[i]);
    printf("total: %ld pares, %ld caracteres de resposta (o que o treino aprende)\n", tp, tc);
    kn_budget(tp, tc);
    return 0;
}
static int kn_main(int argc, char **argv) {
    if (argc >= 1 && !strcmp(argv[0], "build")) return kn_build(argc - 1, argv + 1);
    if (argc >= 1 && !strcmp(argv[0], "search")) return kn_search(argc - 1, argv + 1);
    if (argc >= 1 && !strcmp(argv[0], "stats")) return kn_stats(argc - 1, argv + 1);
    if (argc >= 1 && !strcmp(argv[0], "corpus")) return kn_corpus(argc - 1, argv + 1);
    fprintf(stderr, "uso: katana knowledge build <raiz|pasta|arquivo.jsonl> [--out PASTA] [--force] [--replace REGEX=TEXTO]... [--cut-matching REGEX]\n"
                    "     katana knowledge search <palavras...> [--project NOME] [--limit N]\n"
                    "     katana knowledge stats [PASTA|corpus.jsonl]\n"
                    "     katana knowledge corpus <pasta das notas> <saida.jsonl> [--project NOME]... [--max-per-project N] [--holdout ARQ.jsonl]\n"
                    "     pasta das notas: --out, ou KATANA_KNOWLEDGE, ou ~/.katana/knowledge\n");
    return 1;
}
#endif
