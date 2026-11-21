/* ---------- agente: conversa com ferramentas ----------
 * O modelo pode pedir ferramentas no formato <tool_call>{"name":..,"arguments":{..}}</tool_call>
 * (o do Qwen); o motor executa e devolve o resultado em <tool_response>. Ler, listar e buscar
 * arquivos dentro da pasta atual e livre; fora dela pede confirmacao uma vez por pasta raiz.
 * Gravar arquivo e executar comando so acontecem depois de o usuario confirmar a cada vez.
 * Tambem ha busca e leitura na web (curl do sistema) e memoria persistente. */
#define TOOL_OUT 3000
#define TOOL_WEB 12288      /* limite do texto de uma pagina ou busca devolvido ao modelo */

/* texto de um token, como print_token, mas num buffer; devolve o novo tamanho */
static size_t tok_append(const Tokenizer *tk, int id, char *out, size_t len, size_t cap) {
    const uint8_t *t = (const uint8_t *)tk->tok[id];
    int n = tk->toklen[id], i = 0;
    while (i < n && len + 4 < cap) {
        if (tk->spm) {
            if (i + 2 < n && t[i] == 0xE2 && t[i + 1] == 0x96 && t[i + 2] == 0x81) { out[len++] = ' '; i += 3; }
            else out[len++] = (char)t[i++];
            continue;
        }
        int cp = t[i], adv = 1;
        if (cp >= 0xC0 && cp < 0xE0 && i + 1 < n) { cp = ((cp & 0x1F) << 6) | (t[i + 1] & 0x3F); adv = 2; }
        if (adv <= 2 && cp < 512 && tk->cp2byte[cp] >= 0) out[len++] = (char)tk->cp2byte[cp];
        else { memcpy(out + len, t + i, adv); len += adv; }
        i += adv;
    }
    out[len] = 0;
    return len;
}

/* valor de texto da chave `key` num objeto JSON (primeira ocorrencia); 0 se nao achar */
static int json_str(const char *js, const char *end, const char *key, char *out, size_t cap) {
    char pat[64];
    size_t n = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *q = strstr(js, pat);
    if (!q || q >= end) return 0;
    q += strlen(pat);
    while (q < end && (*q == ' ' || *q == ':' || *q == '\n' || *q == '\t' || *q == '\r')) q++;
    if (q >= end || *q != '"') return 0;
    for (q++; q < end && *q != '"' && n + 4 < cap; q++) {
        if (*q != '\\') { out[n++] = *q; continue; }
        if (++q >= end) break;
        switch (*q) {
            case 'n': out[n++] = '\n'; break;
            case 't': out[n++] = '\t'; break;
            case 'r': out[n++] = '\r'; break;
            case 'u': {     /* \uXXXX -> UTF-8 (sem pares substitutos) */
                unsigned cp = 0;
                if (q + 4 < end) { char h[5] = { q[1], q[2], q[3], q[4], 0 }; cp = (unsigned)strtoul(h, NULL, 16); q += 4; }
                if (cp < 0x80) out[n++] = (char)cp;
                else if (cp < 0x800) { out[n++] = (char)(0xC0 | (cp >> 6)); out[n++] = (char)(0x80 | (cp & 63)); }
                else { out[n++] = (char)(0xE0 | (cp >> 12)); out[n++] = (char)(0x80 | ((cp >> 6) & 63)); out[n++] = (char)(0x80 | (cp & 63)); }
                break;
            }
            default: out[n++] = *q; break;
        }
    }
    out[n] = 0;
    return 1;
}

/* so caminhos relativos dentro da pasta atual */
static int path_ok(const char *p) {
    if (!*p || p[0] == '/' || p[0] == '\\' || strchr(p, ':') || strstr(p, "..")) return 0;
    return 1;
}

static int g_trust = 1;      /* a pasta de trabalho e confiavel (ver trust_init); 0 = toda acao pergunta antes */

/* pergunta sim/nao ao usuario. Na interface (KATANA_UI): marcador "ASK tipo<TAB>o que<TAB>detalhe" e a resposta e a linha
 * "\x01ANS 1" ou "\x01ANS 0". No terminal: no teclado. Sem teclado nem interface: nega. */
static int agent_ask_kind(const char *kind, const char *what, const char *detail) {
    if (getenv("KATANA_UI")) {
        char w[400], d[600], l[256];
        size_t n = 0;
        for (const char *q = what; *q && n < sizeof w - 1; q++) w[n++] = (*q == '\n' || *q == '\r' || *q == '\t' || *q == 1) ? ' ' : *q;
        w[n] = 0; n = 0;
        for (const char *q = detail; *q && n < sizeof d - 1; q++) d[n++] = (*q == '\n' || *q == '\r' || *q == '\t' || *q == 1) ? ' ' : *q;
        d[n] = 0;
        printf("\x01" "ASK %s\t%s\t%s\n", kind, w, d); fflush(stdout);
        if (!read_line(l, sizeof l)) return 0;
        return strncmp(l, "\x01" "ANS 1", 5) == 0;
    }
    if (!K_ISATTY(0)) return 0;
    char ans[64];
    fprintf(stderr, "\n[%s]\n%s\n[allow? y/N] ", what, detail);
    fflush(stderr);
    if (!fgets(ans, sizeof ans, stdin)) return 0;
    return ans[0] == 'y' || ans[0] == 'Y' || ans[0] == 's' || ans[0] == 'S';
}
static int agent_confirm(const char *what, const char *detail) { return agent_ask_kind("action", what, detail); }

/* gravar e executar: sem confirmacao por decisao do dono (a acao aparece no terminal e vai para o
 * agente.log); KATANA_CONFIRM=1 volta a pedir a cada vez */
static int agent_act(const char *what, const char *detail) {
    const char *e = getenv("KATANA_CONFIRM");
    if (!g_trust || (e && atoi(e) != 0)) return agent_confirm(what, detail);   /* pasta sem confianca: pergunta sempre */
    size_t dl = strcspn(detail, "\n");
    if (!g_con_rich) fprintf(stderr, "\n[o modelo vai %s: %.*s%s]\n", what, (int)(dl < 200 ? dl : 200), detail, dl < strlen(detail) ? " ..." : "");     /* o console rico ja mostra a linha da ferramenta */
    return 1;
}

/* ---- ferramentas de fora do modelo: internet, arquivos da maquina e memoria ----
 * O motor de inferencia continua local; so estas ferramentas saem da pasta atual ou da maquina.
 * Todo resultado e DADO (embrulhado com marcador no historico), nunca instrucao. */
static size_t g_budget = 12000;           /* bytes que o contexto ainda comporta para um resultado de ferramenta */
static int g_web = 1;
static int g_code;                        /* katana code: agente em modo de programacao (prompt e ferramentas enxutos) */
static int g_tool_glm;                    /* o modelo chama ferramentas no formato do GLM (<arg_key>/<arg_value>) */
static int g_tool_xml;                    /* o modelo chama ferramentas em XML (<function=..><parameter=..>) */                     /* KATANA_WEB=0 desliga web_search e web_fetch */
static char g_roots[32][1024];            /* pastas raiz fora da pasta atual ja liberadas pelo usuario */
static int g_nroots;

#ifdef _WIN32
#define CURL_BIN "curl.exe"
#define CURL_Q "\""
#define CURL_NUL "NUL"
#else
#define CURL_BIN "curl"
#define CURL_Q "'"
#define CURL_NUL "/dev/null"
#endif

static int ci_ncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}

static void agent_home(char *out, size_t cap) {
#ifdef _WIN32
    const char *h = getenv("USERPROFILE");
#else
    const char *h = getenv("HOME");
#endif
    snprintf(out, cap, "%s", h && *h ? h : ".");
}

/* registro de acoes: ~/.katana/agente.log (KATANA_LOG muda o caminho); uma linha: data, ferramenta, argumentos, resultado */
static void agent_log(const char *name, const char *args, const char *res) {
    char p[1200], dir[1200], a[400], r[200];
    const char *e = getenv("KATANA_LOG");
    if (e && *e) snprintf(p, sizeof p, "%s", e);
    else {
        char h[1024];
        agent_home(h, sizeof h);
#ifdef _WIN32
        snprintf(p, sizeof p, "%s\\.katana\\agente.log", h);
#else
        snprintf(p, sizeof p, "%s/.katana/agente.log", h);
#endif
    }
    snprintf(dir, sizeof dir, "%s", p);
    char *sl = strrchr(dir, '/'); char *bs = strrchr(dir, '\\');
    if (bs && (!sl || bs > sl)) sl = bs;
    if (sl) {
        *sl = 0;
#ifdef _WIN32
        CreateDirectoryA(dir, NULL);
#else
        mkdir(dir, 0700);
#endif
    }
    size_t n = 0;
    for (const char *q = args; *q && n < sizeof a - 1; q++) a[n++] = (*q == '\n' || *q == '\r' || *q == '\t') ? ' ' : *q;
    a[n] = 0;
    n = 0;
    const char *ec = strstr(res, "[exit code ");
    if (ec) res = ec;
    for (const char *q = res; *q && n < sizeof r - 1 && !(ec && *q == ']'); q++) r[n++] = (*q == '\n' || *q == '\r' || *q == '\t') ? ' ' : *q;
    if (ec && n < sizeof r - 1) r[n++] = ']';
    r[n] = 0;
    FILE *f = fopen(p, "ab");
    if (!f) return;
    char ts[32];
    const time_t t = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(f, "%s\t%s\t%s\t%s\n", ts, name, a, r);
    fclose(f);
}

/* caminho absoluto canonico (aceita ~); 0 se nao existir */
static int agent_resolve(const char *p, char *out, size_t cap) {
    char tmp[4200];
    if (p[0] == '~' && (p[1] == 0 || p[1] == '/' || p[1] == '\\')) {
        char h[1024]; agent_home(h, sizeof h);
        snprintf(tmp, sizeof tmp, "%s%s", h, p + 1);
    } else snprintf(tmp, sizeof tmp, "%s", p);
#ifdef _WIN32
    char full[4200];
    if (!_fullpath(full, tmp, sizeof full)) return 0;
    if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) return 0;
    snprintf(out, cap, "%s", full);
    return 1;
#else
    char rp[4200];
    if (!realpath(tmp, rp)) return 0;
    snprintf(out, cap, "%s", rp);
    return 1;
#endif
}

static int path_under(const char *base, const char *p) {
    const size_t n = strlen(base);
    if (strncmp(base, p, n) != 0) return 0;
    if (n > 0 && (base[n - 1] == '/' || base[n - 1] == '\\')) return 1;
    return p[n] == 0 || p[n] == '/' || p[n] == '\\';
}

/* leitura de um caminho: dentro da pasta atual e livre; fora dela pede confirmacao uma vez por
 * pasta raiz (a propria pasta, ou a pasta que contem o arquivo) e lembra na sessao */
static int agent_read_access(const char *p, int is_dir, char *res, size_t cap, char *err, size_t ecap) {
    char cwd[4200];
    if (!*p) { snprintf(err, ecap, "error: empty path"); return 0; }
    if (!agent_resolve(p, res, cap)) { snprintf(err, ecap, "error: cannot open %s", p); return 0; }
    if (!agent_resolve(".", cwd, sizeof cwd)) cwd[0] = 0;
    if (cwd[0] && path_under(cwd, res)) return 1;
    char root[1024];
    snprintf(root, sizeof root, "%s", res);
    if (!is_dir) {
        struct stat st;
        if (stat(res, &st) == 0 && S_ISDIR(st.st_mode)) is_dir = 1;
    }
    if (!is_dir) {
        char *s = strrchr(root, '/');
        char *b = strrchr(root, '\\');
        if (b && (!s || b > s)) s = b;
        if (s && s != root) *s = 0;
    }
    for (int i = 0; i < g_nroots; i++) if (path_under(g_roots[i], res)) return 1;
    if (!agent_act("ler fora da pasta atual (vale para toda a pasta e subpastas nesta sessao)", root)) {
        snprintf(err, ecap, "error: the user denied access to %s", root);
        return 0;
    }
    if (g_nroots < 32) snprintf(g_roots[g_nroots++], sizeof g_roots[0], "%s", root);
    return 1;
}

/* ---- escrita: caminho relativo ou absoluto, dentro da pasta de trabalho (ou de uma pasta que o usuario liberou) ---- */
#ifndef _WIN32
static char g_wroots[32][1024];            /* pastas fora da pasta de trabalho liberadas para escrita nesta sessao */
static int g_nwroots;

/* resolve . e .. de um caminho absoluto, no lugar */
static void path_norm(char *p) {
    char out[4300]; size_t n = 0;
    out[0] = 0;
    const char *q = p;
    while (*q) {
        while (*q == '/') q++;
        if (!*q) break;
        const size_t l = strcspn(q, "/");
        if (l == 1 && q[0] == '.') { q += l; continue; }
        if (l == 2 && q[0] == '.' && q[1] == '.') { while (n > 0 && out[n - 1] != '/') n--; if (n > 0) n--; out[n] = 0; q += l; continue; }
        if (n + l + 2 >= sizeof out) break;
        out[n++] = '/'; memcpy(out + n, q, l); n += l; out[n] = 0;
        q += l;
    }
    snprintf(p, 4300, "%s", n ? out : "/");
}

/* caminho final (absoluto, com symlinks resolvidos nas partes que existem). 0 = recusado, com a mensagem em err */
static int agent_write_path(const char *in, char *res, size_t cap, char *err, size_t ecap) {
    char abs[4300], cwd[4200], rc[4200], head[4300], tail[4300], base[4300];
    if (!*in) { snprintf(err, ecap, "error: empty path"); return 0; }
    if (!getcwd(cwd, sizeof cwd)) { snprintf(err, ecap, "error: cannot read the working folder"); return 0; }
    if (!realpath(cwd, rc)) snprintf(rc, sizeof rc, "%s", cwd);
    if (in[0] == '~' && (in[1] == 0 || in[1] == '/')) { char h[1024]; agent_home(h, sizeof h); snprintf(abs, sizeof abs, "%s%s", h, in + 1); }
    else if (in[0] == '/') snprintf(abs, sizeof abs, "%s", in);
    else snprintf(abs, sizeof abs, "%s/%s", rc, in);
    path_norm(abs);
    snprintf(head, sizeof head, "%s", abs);
    tail[0] = 0;
    for (;;) {                                          /* o maior prefixo que existe, sem symlinks */
        if (realpath(head, base)) break;
        char *s = strrchr(head, '/');
        if (!s) { snprintf(err, ecap, "error: invalid path"); return 0; }
        char t2[4300];
        snprintf(t2, sizeof t2, "%s%s", s, tail);
        snprintf(tail, sizeof tail, "%s", t2);
        if (s == head) { head[1] = 0; if (!realpath(head, base)) { snprintf(err, ecap, "error: invalid path"); return 0; } break; }
        *s = 0;
    }
    if (strcmp(base, "/") == 0) snprintf(res, cap, "%s", tail[0] ? tail : "/");
    else snprintf(res, cap, "%s%s", base, tail);
    return 1;     /* o pedido do usuario ja e a autorizacao: qualquer pasta; so o sistema pode negar (ai entra o sudo) */
}

/* mkdir -p do diretorio que contem o arquivo */
static void mkdir_parents(const char *file) {
    char d[4300];
    snprintf(d, sizeof d, "%s", file);
    for (char *q = d + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(d, 0755); *q = '/'; }
}

/* pasta de trabalho do agente: so informa a pasta atual para a interface (marcador CWD). Nao pergunta nada: o pedido do usuario ja e a
 * autorizacao. KATANA_TRUST=0 volta a perguntar antes de cada acao (como KATANA_CONFIRM=1). */
static void trust_init(void) {
    char cwd[4200], rc[4200];
    if (!getcwd(cwd, sizeof cwd) || !realpath(cwd, rc)) snprintf(rc, sizeof rc, "?");
    const char *e = getenv("KATANA_TRUST");
    g_trust = !(e && *e && atoi(e) == 0);
    if (getenv("KATANA_UI")) { printf("\x01" "CWD %s\t%d\n", rc, g_trust); fflush(stdout); }
    else fprintf(stderr, "[working folder: %s]\n", rc);
}
#else
static int agent_write_path(const char *in, char *res, size_t cap, char *err, size_t ecap) {
    if (!path_ok(in)) { snprintf(err, ecap, "error: invalid path (only relative paths inside the current directory)"); return 0; }
    snprintf(res, cap, "%s", in);
    return 1;
}
static void mkdir_parents(const char *file) { (void)file; }
static void trust_init(void) {}
#endif

/* ---- search_files ---- */
#ifndef _WIN32
#include <regex.h>
#endif
typedef struct { const char *pat; char *out; size_t cap, n; int hits, files; void *re; } SFState;      /* re: expressao regular compilada (regex=true) ou NULL */
static int ci_find(const char *h, size_t hl, const char *needle_lc, size_t nl);
static int sf_match(SFState *s, char *ln, size_t l, size_t nl) {
#ifndef _WIN32
    if (s->re) { const char sv = ln[l]; ln[l] = 0; const int r = regexec((regex_t *)s->re, ln, 0, NULL, 0) == 0; ln[l] = sv; return r; }
#endif
    return ci_find(ln, l, s->pat, nl);
}
#define SF_MAXHITS 40
#define SF_MAXFILES 30000
#define SF_MAXSIZE 1500000

static int ci_find(const char *h, size_t hl, const char *needle_lc, size_t nl) {
    if (nl == 0 || hl < nl) return 0;
    for (size_t i = 0; i + nl <= hl; i++) {
        size_t j = 0;
        while (j < nl && tolower((unsigned char)h[i + j]) == (unsigned char)needle_lc[j]) j++;
        if (j == nl) return 1;
    }
    return 0;
}

static void sf_file(SFState *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > SF_MAXSIZE) { fclose(f); return; }
    char *b = (char *)malloc((size_t)sz + 1);
    if (!b) { fclose(f); return; }
    const size_t got = fread(b, 1, (size_t)sz, f);
    fclose(f);
    b[got] = 0;
    s->files++;
    if (memchr(b, 0, got < 1024 ? got : 1024)) { free(b); return; }        /* binario */
    const size_t nl = strlen(s->pat);
    int line = 1;
    for (size_t i = 0; i < got && s->hits < SF_MAXHITS; ) {
        size_t e = i;
        while (e < got && b[e] != '\n') e++;
        if (sf_match(s, b + i, e - i, nl)) {
            size_t a = i, l = e - i;
            while (l > 0 && (b[a] == ' ' || b[a] == '\t')) { a++; l--; }
            while (l > 0 && (b[a + l - 1] == '\r' || b[a + l - 1] == ' ')) l--;
            if (l > 160) { l = 160; while (l > 0 && ((unsigned char)b[a + l] & 0xC0) == 0x80) l--; }
            if (s->n + l + strlen(path) + 32 < s->cap) {
                s->n += (size_t)snprintf(s->out + s->n, s->cap - s->n, "%s:%d:%.*s\n", path, line, (int)l, b + a);
                s->hits++;
            } else s->hits = SF_MAXHITS;
        }
        i = e + 1; line++;
    }
    free(b);
}

static int sf_skip_dir(const char *n) {
    return strcmp(n, ".git") == 0 || strcmp(n, "node_modules") == 0 || strcmp(n, ".svn") == 0 ||
           strcmp(n, ".hg") == 0 || strcmp(n, "__pycache__") == 0 || strcmp(n, ".cache") == 0 ||
           strcmp(n, ".venv") == 0 || strcmp(n, "venv") == 0 || strcmp(n, "proc") == 0;
}

static void sf_walk(SFState *s, const char *dir, int depth) {
    char full[4200];
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    snprintf(full, sizeof full, "%s\\*", dir);
    HANDLE h = FindFirstFileA(full, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (s->hits >= SF_MAXHITS || s->files >= SF_MAXFILES) break;
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        snprintf(full, sizeof full, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 12 && !sf_skip_dir(fd.cFileName)) sf_walk(s, full, depth + 1); }
        else sf_file(s, full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (s->hits >= SF_MAXHITS || s->files >= SF_MAXFILES) break;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        struct stat st;
        snprintf(full, sizeof full, "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", e->d_name);
        if (lstat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 12 && !sf_skip_dir(e->d_name)) sf_walk(s, full, depth + 1); }
        else if (S_ISREG(st.st_mode)) sf_file(s, full);
    }
    closedir(d);
#endif
}

static void tool_search_files(const char *pattern, const char *root, int regex, char *out, size_t cap) {
    char lc[256], res[4200], err[1300];
    size_t i = 0;
    for (; pattern[i] && i < sizeof lc - 1; i++) lc[i] = (char)tolower((unsigned char)pattern[i]);
    lc[i] = 0;
    if (!lc[0]) { snprintf(out, cap, "error: empty pattern"); return; }
    if (!agent_read_access(root, 1, res, sizeof res, err, sizeof err)) { snprintf(out, cap, "%s", err); return; }
    SFState s = { lc, out, cap > 6000 ? 6000 : cap, 0, 0, 0, NULL };
#ifndef _WIN32
    regex_t re;
    if (regex) {        /* expressao regular estendida, sem diferenciar maiusculas */
        const int rc = regcomp(&re, pattern, REG_EXTENDED | REG_ICASE | REG_NOSUB);
        if (rc) { char eb[200]; regerror(rc, &re, eb, sizeof eb); snprintf(out, cap, "error: invalid regular expression (%s). POSIX extended syntax: use [0-9] and [[:space:]] instead of \\d and \\s", eb); return; }
        s.re = &re;
    }
#else
    (void)regex;
#endif
    out[0] = 0;
    struct stat st;
    if (stat(res, &st) == 0 && !S_ISDIR(st.st_mode)) sf_file(&s, res);
    else {
        const int rel = root[0] != '/' && root[0] != '\\' && root[0] != '~' && !strchr(root, ':');
        sf_walk(&s, rel ? root : res, 0);       /* dentro da pasta atual, mostra caminhos relativos */
    }
#ifndef _WIN32
    if (s.hits == 0 && !regex && (strchr(pattern, '|') || strstr(pattern, ".*") || strchr(pattern, '\\'))) {     /* parece expressao regular: tenta como uma */
        tool_search_files(pattern, root, 1, out, cap);
        if (strncmp(out, "no matches", 10) != 0 && strncmp(out, "error:", 6) != 0) return;
        snprintf(out, cap, "no matches in %d text files under %s (tried as plain text and as a regular expression)", s.files, res);
        return;
    }
#endif
    if (s.hits == 0) snprintf(out, cap, "no matches in %d text files under %s", s.files, res);
    else if (s.hits >= SF_MAXHITS) snprintf(out + s.n, cap - s.n, "[result limit reached; refine the pattern or the path]\n");
#ifndef _WIN32
    if (s.re) regfree(&re);
#endif
}

/* ---- memoria: ~/.katana/memoria.txt, uma linha por fato: AAAA-MM-DD<TAB>fato ---- */
static void mem_path(char *out, size_t cap) {
    const char *e = getenv("KATANA_MEM");
    if (e && *e) { snprintf(out, cap, "%s", e); return; }
    char h[1024];
    agent_home(h, sizeof h);
#ifdef _WIN32
    snprintf(out, cap, "%s\\.katana\\memoria.txt", h);
#else
    snprintf(out, cap, "%s/.katana/memoria.txt", h);
#endif
}

static char *mem_read(size_t *n) {         /* arquivo inteiro (ate 1 MB, o fim) ou NULL */
    char p[1200];
    mem_path(p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    const long skip = sz > 1000000 ? sz - 1000000 : 0;
    fseek(f, skip, SEEK_SET);
    char *b = (char *)malloc((size_t)(sz - skip) + 1);
    if (!b) { fclose(f); return NULL; }
    *n = fread(b, 1, (size_t)(sz - skip), f);
    b[*n] = 0;
    fclose(f);
    return b;
}

static void tool_remember(const char *fact, char *out, size_t cap) {
    char f[520];
    size_t n = 0;
    for (const char *s = fact; *s && n < 500; s++) {
        const char c = (*s == '\n' || *s == '\r' || *s == '\t') ? ' ' : *s;
        if (c == ' ' && (n == 0 || f[n - 1] == ' ')) continue;
        f[n++] = c;
    }
    while (n > 0 && f[n - 1] == ' ') n--;
    f[n] = 0;
    if (n < 3) { snprintf(out, cap, "error: fact too short"); return; }
    size_t bn = 0;
    char *b = mem_read(&bn);
    if (b) {
        for (char *l = b; *l; ) {
            char *e = strchr(l, '\n');
            if (e) *e = 0;
            char *t = strchr(l, '\t');
            if (t && strcmp(t + 1, f) == 0) { free(b); snprintf(out, cap, "ok: already in memory"); return; }
            if (!e) break;
            l = e + 1;
        }
        free(b);
    }
    char p[1200], dir[1200];
    mem_path(p, sizeof p);
    snprintf(dir, sizeof dir, "%s", p);
    char *sl = strrchr(dir, '/'); char *bs = strrchr(dir, '\\');
    if (bs && (!sl || bs > sl)) sl = bs;
    if (sl) {
        *sl = 0;
#ifdef _WIN32
        CreateDirectoryA(dir, NULL);
#else
        mkdir(dir, 0700);
#endif
    }
    FILE *fp = fopen(p, "ab");
    if (!fp) { snprintf(out, cap, "error: cannot write %s", p); return; }
    char day[16];
    const time_t t = time(NULL);
    strftime(day, sizeof day, "%Y-%m-%d", localtime(&t));
    fprintf(fp, "%s\t%s\n", day, f);
    fclose(fp);
    snprintf(out, cap, "ok: saved to memory");
}

/* ultimos `max` fatos, do mais antigo ao mais novo, no formato "- data: fato" */
static size_t mem_tail(int max, char *out, size_t cap) {
    size_t bn = 0, o = 0;
    char *b = mem_read(&bn);
    out[0] = 0;
    if (!b) return 0;
    static char *ln[4096];
    int nl = 0;
    for (char *l = b; *l && nl < 4096; ) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        if (strchr(l, '\t')) ln[nl++] = l;
        if (!e) break;
        l = e + 1;
    }
    for (int i = nl > max ? nl - max : 0; i < nl; i++) {
        char *t = strchr(ln[i], '\t');
        *t = 0;
        if (o + 700 >= cap) break;
        o += (size_t)snprintf(out + o, cap - o, "- %s: %.400s\n", ln[i], t + 1);
    }
    free(b);
    return o;
}

static void tool_recall(const char *query, char *out, size_t cap) {
    size_t bn = 0, o = 0;
    char *b = mem_read(&bn);
    if (!b) { snprintf(out, cap, "memory is empty"); return; }
    char words[16][40]; int nw = 0;
    for (const char *s = query; *s && nw < 16; ) {         /* palavras da consulta (2+ letras), minusculas */
        while (*s && !isalnum((unsigned char)*s) && (unsigned char)*s < 0x80) s++;
        size_t l = 0; char w[40];
        while (*s && (isalnum((unsigned char)*s) || (unsigned char)*s >= 0x80) && l < 39) w[l++] = (char)tolower((unsigned char)*s++);
        while (*s && (isalnum((unsigned char)*s) || (unsigned char)*s >= 0x80)) s++;
        w[l] = 0;
        if (l >= 2) { memcpy(words[nw], w, l + 1); nw++; }
    }
    static char *ln[4096]; static int sc[4096];
    int nl = 0;
    for (char *l = b; *l && nl < 4096; ) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        if (strchr(l, '\t')) {
            int s = 0;
            const size_t L = strlen(l);
            for (int w = 0; w < nw; w++) if (ci_find(l, L, words[w], strlen(words[w]))) s++;
            ln[nl] = l; sc[nl] = s; nl++;
        }
        if (!e) break;
        l = e + 1;
    }
    out[0] = 0;
    for (int pick = 0; pick < 8; pick++) {                 /* maior pontuacao, empate pelo mais recente */
        int best = -1;
        for (int i = 0; i < nl; i++) if (sc[i] > 0 && (best < 0 || sc[i] >= sc[best])) best = i;
        if (best < 0) break;
        char *t = strchr(ln[best], '\t');
        *t = 0;
        if (o + 700 < cap) o += (size_t)snprintf(out + o, cap - o, "- %s: %.400s\n", ln[best], t + 1);
        sc[best] = 0;
    }
    if (!out[0]) snprintf(out, cap, "no matching facts in memory");
    free(b);
}

