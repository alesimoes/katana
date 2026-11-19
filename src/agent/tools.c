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
    fprintf(stderr, "\n[o modelo vai %s: %.*s%s]\n", what, (int)(dl < 200 ? dl : 200), detail, dl < strlen(detail) ? " ..." : "");
    return 1;
}

/* ---- ferramentas de fora do modelo: internet, arquivos da maquina e memoria ----
 * O motor de inferencia continua local; so estas ferramentas saem da pasta atual ou da maquina.
 * Todo resultado e DADO (embrulhado com marcador no historico), nunca instrucao. */
static size_t g_budget = 12000;           /* bytes que o contexto ainda comporta para um resultado de ferramenta */
static int g_web = 1;
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

/* ---- URL segura para a linha de comando: so http/https; tudo fora de um conjunto seguro vira %XX ---- */
static int url_safe(const char *u, char *out, size_t cap) {
    size_t n = 0;
    int ok = 0;
    if (strlen(u) > 2000) return 0;
    if (ci_ncmp(u, "http://", 7) == 0 || ci_ncmp(u, "https://", 8) == 0) ok = 1;
    if (!ok) return 0;
    for (const unsigned char *s = (const unsigned char *)u; *s; s++) {
        const unsigned char c = *s;
        if (c < 0x20 || c == 0x7f) return 0;
        if (n + 4 >= cap) return 0;
        if (isalnum(c) || strchr("-._~:/?#[]@+,=&;", c)) out[n++] = (char)c;
        else if (c == '%' && isxdigit(s[1]) && isxdigit(s[2])) out[n++] = '%';
        else n += (size_t)snprintf(out + n, cap - n, "%%%02X", c);
    }
    out[n] = 0;
    return 1;
}

/* percent-encode total (para valores de consulta) */
static void url_enc(const char *s, char *out, size_t cap) {
    size_t n = 0;
    for (; *s && n + 4 < cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') out[n++] = (char)c;
        else if (c == ' ') out[n++] = '+';
        else n += (size_t)snprintf(out + n, cap - n, "%%%02X", c);
    }
    out[n] = 0;
}

static void url_dec(const char *s, const char *end, char *out, size_t cap) {
    size_t n = 0;
    for (; s < end && n + 2 < cap; s++) {
        if (*s == '%' && s + 2 < end && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            out[n++] = (char)strtol(h, NULL, 16);
            s += 2;
        } else if (*s == '+') out[n++] = ' ';
        else out[n++] = *s;
    }
    out[n] = 0;
}

/* baixa uma URL com o curl do sistema; devolve bytes lidos (ate cap-1), -1 se nao deu para executar,
 * -2 se a URL for recusada */
static FILE *curl_open(const char *url, int max_time) {
    char safe[4400], cmd[7000];
    if (!url_safe(url, safe, sizeof safe)) return NULL;
    snprintf(cmd, sizeof cmd,
        CURL_BIN " -sS -L --max-time %d --max-redirs 5 --max-filesize 3000000 --compressed "
        "--proto " CURL_Q "=http,https" CURL_Q " --proto-redir " CURL_Q "=http,https" CURL_Q " "
        "-A " CURL_Q "Mozilla/5.0 (compatible; katana-agent)" CURL_Q " -- " CURL_Q "%s" CURL_Q " 2>" CURL_NUL, max_time, safe);
#ifdef _WIN32
    return _popen(cmd, "rb");
#else
    return popen(cmd, "r");
#endif
}

/* le a saida do curl (ate cap-1 bytes; cap 0 so descarta), fecha, e devolve os bytes lidos */
static long curl_finish(FILE *f, char *buf, size_t cap, int *rc) {
    size_t n = 0, r;
    while (cap > 0 && n + 1 < cap && (r = fread(buf + n, 1, cap - 1 - n, f)) > 0) n += r;
    if (cap > 0) buf[n] = 0;
    char sink[4096];
    while (fread(sink, 1, sizeof sink, f) > 0) ;
#ifdef _WIN32
    const int c = _pclose(f);
#else
    const int c = pclose(f);
#endif
    if (rc) *rc = c;
    return (long)n;
}

static long curl_get(const char *url, char *buf, size_t cap, int *rc) {
    *rc = -1;
    FILE *f = curl_open(url, 20);
    if (!f) return -2;
    return curl_finish(f, buf, cap, rc);
}

/* entidade HTML em s (apos o '&'): escreve o texto em o e devolve quantos bytes de s consumiu; 0 se nao for entidade */
static int html_entity(const char *s, const char *end, char *o, int *ol) {
    static const struct { const char *n, *v; } T[] = {
        {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "},
        {"ndash", "\xe2\x80\x93"}, {"mdash", "\xe2\x80\x94"}, {"hellip", "..."}, {"laquo", "\xc2\xab"},
        {"raquo", "\xc2\xbb"}, {"copy", "(c)"}, {"reg", "(r)"}, {"middot", "\xc2\xb7"}, {"bull", "*"},
        {"lsquo", "'"}, {"rsquo", "'"}, {"ldquo", "\""}, {"rdquo", "\""},
        {"aacute", "\xc3\xa1"}, {"eacute", "\xc3\xa9"}, {"iacute", "\xc3\xad"}, {"oacute", "\xc3\xb3"},
        {"uacute", "\xc3\xba"}, {"agrave", "\xc3\xa0"}, {"acirc", "\xc3\xa2"}, {"ecirc", "\xc3\xaa"},
        {"ocirc", "\xc3\xb4"}, {"atilde", "\xc3\xa3"}, {"otilde", "\xc3\xb5"}, {"ccedil", "\xc3\xa7"},
        {"Aacute", "\xc3\x81"}, {"Eacute", "\xc3\x89"}, {"Iacute", "\xc3\x8d"}, {"Oacute", "\xc3\x93"},
        {"Uacute", "\xc3\x9a"}, {"Ccedil", "\xc3\x87"},
    };
    const char *e = s;
    while (e < end && e - s < 10 && *e != ';' && *e != '&' && *e != '<' && *e != ' ') e++;
    if (e >= end || *e != ';' || e == s) return 0;
    const int len = (int)(e - s);
    if (s[0] == '#') {
        unsigned long cp = (s[1] == 'x' || s[1] == 'X') ? strtoul(s + 2, NULL, 16) : strtoul(s + 1, NULL, 10);
        if (cp == 0xA0) cp = ' ';
        if (cp == 0) return 0;
        if (cp < 0x80) o[(*ol)++] = (char)cp;
        else if (cp < 0x800) { o[(*ol)++] = (char)(0xC0 | (cp >> 6)); o[(*ol)++] = (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { o[(*ol)++] = (char)(0xE0 | (cp >> 12)); o[(*ol)++] = (char)(0x80 | ((cp >> 6) & 63)); o[(*ol)++] = (char)(0x80 | (cp & 63)); }
        else { o[(*ol)++] = (char)(0xF0 | ((cp >> 18) & 7)); o[(*ol)++] = (char)(0x80 | ((cp >> 12) & 63)); o[(*ol)++] = (char)(0x80 | ((cp >> 6) & 63)); o[(*ol)++] = (char)(0x80 | (cp & 63)); }
        return len + 1;
    }
    for (size_t k = 0; k < sizeof T / sizeof T[0]; k++)
        if ((int)strlen(T[k].n) == len && strncmp(T[k].n, s, (size_t)len) == 0) {
            const int l = (int)strlen(T[k].v);
            memcpy(o + *ol, T[k].v, (size_t)l); *ol += l;
            return len + 1;
        }
    return 0;
}

static int tag_is(const char *name, int nl, const char *list) {   /* list: nomes separados por ',' */
    const char *p = list;
    while (*p) {
        const char *q = strchr(p, ',');
        const int l = q ? (int)(q - p) : (int)strlen(p);
        if (l == nl && strncmp(p, name, (size_t)nl) == 0) return 1;
        if (!q) break;
        p = q + 1;
    }
    return 0;
}

/* HTML -> texto: sem tags, scripts e estilos; entidades decodificadas; espacos colapsados.
 * `out` precisa de n+16 bytes. Devolve o tamanho. */
static size_t html_to_text(const char *in, size_t n, char *out) {
    size_t o = 0;
    int pend_sp = 0, nl = 0;
    for (size_t i = 0; i < n; ) {
        const char c = in[i];
        if (c == '<' && i + 1 < n && (isalpha((unsigned char)in[i + 1]) || in[i + 1] == '/' || in[i + 1] == '!')) {
            if (n - i >= 4 && strncmp(in + i, "<!--", 4) == 0) {
                const char *e = strstr(in + i + 4, "-->");
                i = e ? (size_t)(e - in) + 3 : n;
                continue;
            }
            size_t j = i + 1;
            const int closing = in[j] == '/';
            if (closing) j++;
            char name[16]; int nlen = 0;
            while (j < n && nlen < 15 && isalnum((unsigned char)in[j])) name[nlen++] = (char)tolower((unsigned char)in[j++]);
            name[nlen] = 0;
            char q = 0;                          /* fim da tag, respeitando aspas */
            while (j < n && !(in[j] == '>' && !q)) {
                if (q) { if (in[j] == q) q = 0; }
                else if (in[j] == '"' || in[j] == '\'') q = in[j];
                j++;
            }
            i = j < n ? j + 1 : n;
            if (!closing && tag_is(name, nlen, "script,style,noscript,svg,template")) {
                char endtag[24]; snprintf(endtag, sizeof endtag, "</%s", name);
                const size_t el = strlen(endtag);
                size_t k = i;
                while (k + el <= n) {            /* procura o fecho sem diferenciar maiusculas */
                    if (in[k] == '<' && in[k + 1] == '/' && ci_ncmp(in + k, endtag, el) == 0) break;
                    k++;
                }
                if (k + el > n) k = n;
                while (k < n && in[k] != '>') k++;
                i = k < n ? k + 1 : n;
                continue;
            }
            if (tag_is(name, nlen, "p,h1,h2,h3,h4,h5,h6,table,ul,ol,section,article,blockquote,pre,title")) { if (nl < 2) nl = 2; }
            else if (tag_is(name, nlen, "br,div,li,tr,header,footer,nav,hr,form,dd,dt,main,aside,option")) { if (nl < 1) nl = 1; }
            else if (tag_is(name, nlen, "td,th")) pend_sp = 1;
            continue;
        }
        char tmp[8]; int tl = 0;
        if (c == '&') {
            const int used = html_entity(in + i + 1, in + n, tmp, &tl);
            if (used) i += (size_t)used + 1;
            else { tmp[tl++] = '&'; i++; }
        } else { tmp[tl++] = c; i++; }
        if (tl == 1 && (tmp[0] == ' ' || tmp[0] == '\t' || tmp[0] == '\n' || tmp[0] == '\r' || tmp[0] == '\f')) { pend_sp = 1; continue; }
        if (o > 0) {
            if (nl) { for (int k = 0; k < nl; k++) out[o++] = '\n'; }
            else if (pend_sp && out[o - 1] != '\n') out[o++] = ' ';
        }
        nl = 0; pend_sp = 0;
        for (int k = 0; k < tl; k++) out[o++] = tmp[k];
    }
    out[o] = 0;
    return o;
}

/* texto limpo de um trecho HTML curto (titulo, resumo), numa linha so */
static void html_snip(const char *s, const char *e, char *out, size_t cap) {
    static char tmp[8300];
    size_t n = (size_t)(e - s);
    if (n > 8192) n = 8192;
    size_t o = html_to_text(s, n, tmp);
    if (o >= cap) { o = cap - 1; while (o > 0 && ((unsigned char)tmp[o] & 0xC0) == 0x80) o--; }
    memcpy(out, tmp, o);
    out[o] = 0;
    for (size_t i = 0; i < o; i++) if (out[i] == '\n') out[i] = ' ';
}

static void tool_web_fetch(const char *url, char *out, size_t cap) {
    const size_t RAW = 2500000;
    char *raw = (char *)malloc(RAW + 1), *txt = (char *)malloc(RAW + 64);
    int rc;
    if (!raw || !txt) { free(raw); free(txt); snprintf(out, cap, "error: out of memory"); return; }
    const long n = curl_get(url, raw, RAW, &rc);
    if (n == -2) snprintf(out, cap, "error: invalid URL (only http and https; no control characters)");
    else if (n < 0) snprintf(out, cap, "error: cannot run curl");
    else if (n == 0) snprintf(out, cap, "error: no content received (curl exit code %d: network failure, timeout, blocked or file too large)", rc);
    else {
        size_t tn;
        const size_t probe = (size_t)n < 2048 ? (size_t)n : 2048;
        if (memchr(raw, '<', probe)) tn = html_to_text(raw, (size_t)n, txt);
        else { memcpy(txt, raw, (size_t)n); txt[n] = 0; tn = (size_t)n; }
        size_t lim = cap > 400 ? cap - 400 : cap;
        if (lim > 12000) lim = 12000;
        if (lim > g_budget) lim = g_budget;
        if (tn <= lim) snprintf(out, cap, "%s", txt);
        else {
            while (lim > 0 && ((unsigned char)txt[lim] & 0xC0) == 0x80) lim--;
            snprintf(out, cap, "%.*s\n[truncated: showing %zu of %zu bytes of text]", (int)lim, txt, lim, tn);
        }
        if (!out[0]) snprintf(out, cap, "(the page has no readable text)");
    }
    free(raw); free(txt);
}

/* SearXNG local (KATANA_SEARX=http://127.0.0.1:8888): resultados em JSON {title,url,content}.
 * Devolve quantos resultados escreveu em out; 0 se falhou ou veio vazio. */
static int searx_parse(const char *raw, char *out, size_t cap) {
    static char title[400], url[1000], content[700];
    const char *r = strstr(raw, "\"results\"");
    const char *p = r ? strchr(r, '[') : NULL;
    int k = 0;
    size_t o = 0;
    out[0] = 0;
    while (p && k < 8) {
        const char *ob = strchr(p, '{');
        if (!ob) break;
        const char *q = ob;
        int depth = 0, instr = 0;
        for (; *q; q++) {                       /* fim do objeto, respeitando strings */
            if (instr) { if (*q == '\\' && q[1]) q++; else if (*q == '"') instr = 0; }
            else if (*q == '"') instr = 1;
            else if (*q == '{') depth++;
            else if (*q == '}' && --depth == 0) break;
        }
        if (!*q) break;
        title[0] = url[0] = content[0] = 0;
        json_str(ob, q, "title", title, sizeof title);
        json_str(ob, q, "url", url, sizeof url);
        json_str(ob, q, "content", content, sizeof content);
        if (url[0] && title[0]) {
            k++;
            o += (size_t)snprintf(out + o, cap - o, "%d. %s\n   %s\n   %s\n", k, title, url, content);
            if (o + 1800 > cap) break;
        }
        p = q + 1;
    }
    return k;
}

static int tool_web_search_searx(const char *base, const char *query, char *out, size_t cap) {
    const size_t RAW = 1500000;
    char *raw = (char *)malloc(RAW + 1);
    char q[1200], url[1700];
    int rc, k = 0;
    if (!raw) return 0;
    url_enc(query, q, sizeof q);
    size_t bl = strlen(base);
    while (bl > 0 && base[bl - 1] == '/') bl--;
    snprintf(url, sizeof url, "%.*s/search?q=%s&format=json", (int)(bl < 400 ? bl : 400), base, q);
    const long n = curl_get(url, raw, RAW, &rc);
    if (n > 0) k = searx_parse(raw, out, cap);
    free(raw);
    return k;
}

/* ---- meta-buscador embutido: consulta varias fontes sem chave de API, junta, remove duplicadas e ordena ----
 * Cada fonte tem seu proprio parser (funcoes src_*): se um site mudar o formato, so ele precisa de ajuste
 * e as outras seguem. Falhas vao para o registro (agente.log). */
typedef struct { char title[300], url[700], snip[450]; double score; unsigned src; } Hit;
#define MS_PER_SRC 10
#define MS_NSRC 5

static int hit_set(Hit *h, const char *title, const char *url, const char *snip) {
    if (!title[0] || !url[0] || strncmp(url, "http", 4) != 0) return 0;
    snprintf(h->title, sizeof h->title, "%s", title);
    snprintf(h->url, sizeof h->url, "%s", url);
    snprintf(h->snip, sizeof h->snip, "%s", snip);
    h->score = 0; h->src = 0;
    return 1;
}

/* DuckDuckGo (html.duckduckgo.com/html): links em //duckduckgo.com/l/?uddg=<url codificada> */
static int src_ddg(const char *raw, const char *arg, Hit *out, int max) {
    static char href[2000], title[300], snip[450];
    const char *end = raw + strlen(raw);
    const char *p = strstr(raw, "class=\"result__a\"");
    int k = 0;
    while (p && k < max) {
        const char *next = strstr(p + 10, "class=\"result__a\"");
        const char *lim = next ? next : end;
        const char *h = strstr(p, "href=\"");
        href[0] = title[0] = snip[0] = 0;
        if (h && h < lim) {
            h += 6;
            const char *he = strchr(h, '"');
            if (he && he < lim) {
                const char *u = NULL;
                for (const char *t = h; t + 5 < he; t++) if (strncmp(t, "uddg=", 5) == 0) { u = t + 5; break; }
                if (u) { const char *ue = u; while (ue < he && *ue != '&') ue++; url_dec(u, ue, href, sizeof href); }
                else {
                    char tmp[1000];
                    size_t l = (size_t)(he - h);
                    if (l >= sizeof tmp) l = sizeof tmp - 1;
                    memcpy(tmp, h, l); tmp[l] = 0;
                    snprintf(href, sizeof href, "%s%s", tmp[0] == '/' && tmp[1] == '/' ? "https:" : "", tmp);
                }
                const char *ts = strchr(he, '>');
                const char *te = ts ? strstr(ts, "</a>") : NULL;
                if (ts && te && te < lim) html_snip(ts + 1, te, title, sizeof title);
            }
        }
        const char *sp = strstr(p, "result__snippet");
        if (sp && sp < lim) {
            const char *ss = strchr(sp, '>');
            const char *se = ss ? strstr(ss, "</a>") : NULL;
            if (ss && se && se < lim) html_snip(ss + 1, se, snip, sizeof snip);
        }
        if (hit_set(&out[k], title, href, snip)) k++;
        p = next;
    }
    return k;
}

/* Mojeek (www.mojeek.com/search): <a class="title" href="URL">titulo</a> ... <p class="s">resumo</p>.
 * O site pode exigir um desafio de JavaScript (captcha) para clientes automaticos; nesse caso nao ha resultados. */
static int src_mojeek(const char *raw, const char *arg, Hit *out, int max) {
    static char title[300], snip[450], href[700];
    const char *end = raw + strlen(raw);
    const char *p = strstr(raw, "class=\"title\"");
    int k = 0;
    while (p && k < max) {
        const char *next = strstr(p + 10, "class=\"title\"");
        const char *lim = next ? next : end;
        title[0] = snip[0] = href[0] = 0;
        const char *a = p;                           /* href vem antes ou depois de class= */
        while (a > raw && *a != '<') a--;
        const char *h = strstr(a, "href=\"");
        const char *ae = strchr(a, '>');
        if (h && ae && h < ae) {
            h += 6;
            const char *he = strchr(h, '"');
            if (he && he < ae) {
                size_t l = (size_t)(he - h);
                if (l >= sizeof href) l = sizeof href - 1;
                memcpy(href, h, l); href[l] = 0;
            }
            const char *te = strstr(ae, "</a>");
            if (te && te < lim) html_snip(ae + 1, te, title, sizeof title);
        }
        const char *sp = strstr(p, "class=\"s\"");
        if (sp && sp < lim) {
            const char *ss = strchr(sp, '>');
            const char *se = ss ? strstr(ss, "</p>") : NULL;
            if (ss && se && se < lim) html_snip(ss + 1, se, snip, sizeof snip);
        }
        if (hit_set(&out[k], title, href, snip)) k++;
        p = next;
    }
    return k;
}

/* Wikipedia (api.php, list=search, JSON): arg = host (ex.: pt.wikipedia.org); URL montada a partir do titulo */
static int src_wiki(const char *raw, const char *arg, Hit *out, int max) {
    static char title[300], snip[450], url[700], enc[500];
    const char *r = strstr(raw, "\"search\":[");
    const char *p = r ? strchr(r, '[') : NULL;
    int k = 0;
    while (p && k < max) {
        const char *ob = strchr(p, '{');
        if (!ob) break;
        const char *q = ob;
        int depth = 0, instr = 0;
        for (; *q; q++) {
            if (instr) { if (*q == '\\' && q[1]) q++; else if (*q == '"') instr = 0; }
            else if (*q == '"') instr = 1;
            else if (*q == '{') depth++;
            else if (*q == '}' && --depth == 0) break;
        }
        if (!*q) break;
        static char rawsnip[900];
        title[0] = rawsnip[0] = snip[0] = 0;
        json_str(ob, q, "title", title, sizeof title);
        json_str(ob, q, "snippet", rawsnip, sizeof rawsnip);
        html_snip(rawsnip, rawsnip + strlen(rawsnip), snip, sizeof snip);
        size_t n = 0;                                 /* titulo -> caminho: espaco vira _ ; resto percent-encoded */
        for (const unsigned char *t = (const unsigned char *)title; *t && n + 4 < sizeof enc; t++) {
            if (*t == ' ') enc[n++] = '_';
            else if (isalnum(*t) || strchr("-._~()", *t)) enc[n++] = (char)*t;
            else n += (size_t)snprintf(enc + n, sizeof enc - n, "%%%02X", *t);
        }
        enc[n] = 0;
        snprintf(url, sizeof url, "https://%s/wiki/%s", arg, enc);
        if (hit_set(&out[k], title, url, snip)) k++;
        p = q + 1;
    }
    return k;
}

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return c == '-' ? 62 : 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

/* Bing (www.bing.com/search): <li class="b_algo"><h2><a href="...&u=a1<base64 da URL>">titulo</a></h2> ... <p>resumo</p> */
static int src_bing(const char *raw, const char *arg, Hit *out, int max) {
    static char title[300], snip[450], href[700];
    const char *end = raw + strlen(raw);
    const char *p = strstr(raw, "class=\"b_algo\"");
    int k = 0;
    while (p && k < max) {
        const char *next = strstr(p + 10, "class=\"b_algo\"");
        const char *lim = next ? next : end;
        title[0] = snip[0] = href[0] = 0;
        const char *h2 = strstr(p, "<h2");
        if (h2 && h2 < lim) {
            const char *h = strstr(h2, "href=\"");
            const char *ae = strchr(h2, '>');
            ae = ae ? strchr(ae + 1, '>') : NULL;      /* fim do <a ...> */
            if (h && ae && h < ae) {
                h += 6;
                const char *he = strchr(h, '"');
                const char *u = NULL;
                for (const char *t = h; he && t + 5 < he; t++) if (strncmp(t, "&amp;u=a1", 9) == 0 || strncmp(t, "&u=a1", 5) == 0) { u = strstr(t, "u=a1") + 4; break; }
                if (u) {                                /* URL real em base64 (variante url-safe, sem preenchimento) */
                    size_t n = 0; unsigned acc = 0; int bits = 0;
                    for (; u < he && *u != '&' && n + 2 < sizeof href; u++) {
                        const int v = b64_val((unsigned char)*u);
                        if (v < 0) break;
                        acc = (acc << 6) | (unsigned)v; bits += 6;
                        if (bits >= 8) { bits -= 8; href[n++] = (char)((acc >> bits) & 255); }
                    }
                    href[n] = 0;
                } else if (he && he - h < (long)sizeof href && strncmp(h, "http", 4) == 0 && !strstr(h, "bing.com/ck/")) {
                    memcpy(href, h, (size_t)(he - h)); href[he - h] = 0;
                }
                const char *te = strstr(ae, "</a>");
                if (te && te < lim) html_snip(ae + 1, te, title, sizeof title);
            }
        }
        const char *sp = strstr(p, "<p");
        if (sp && sp < lim) {
            const char *ss = strchr(sp, '>');
            const char *se = ss ? strstr(ss, "</p>") : NULL;
            if (ss && se && se < lim) html_snip(ss + 1, se, snip, sizeof snip);
        }
        if (hit_set(&out[k], title, href, snip)) k++;
        p = next;
    }
    return k;
}

typedef struct { const char *name, *fmt, *arg; int (*parse)(const char *, const char *, Hit *, int); } Source;
static const Source SOURCES[MS_NSRC] = {
    { "duckduckgo", "https://html.duckduckgo.com/html/?q=%s", "", src_ddg },
    { "bing", "https://www.bing.com/search?q=%s", "", src_bing },
    { "mojeek", "https://www.mojeek.com/search?q=%s", "", src_mojeek },
    { "wikipedia-pt", "https://pt.wikipedia.org/w/api.php?action=query&list=search&format=json&srlimit=5&srsearch=%s", "pt.wikipedia.org", src_wiki },
    { "wikipedia-en", "https://en.wikipedia.org/w/api.php?action=query&list=search&format=json&srlimit=5&srsearch=%s", "en.wikipedia.org", src_wiki },
};

/* chave de comparacao de URL: sem esquema, sem www., sem fragmento, sem barra final, minuscula */
static void url_key(const char *u, char *k, size_t cap) {
    size_t n = 0;
    if (ci_ncmp(u, "https://", 8) == 0) u += 8; else if (ci_ncmp(u, "http://", 7) == 0) u += 7;
    if (ci_ncmp(u, "www.", 4) == 0) u += 4;
    for (; *u && *u != '#' && n + 1 < cap; u++) k[n++] = (char)tolower((unsigned char)*u);
    while (n > 0 && k[n - 1] == '/') n--;
    k[n] = 0;
}

static int hit_cmp(const void *a, const void *b) {
    const double x = ((const Hit *)a)->score, y = ((const Hit *)b)->score;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void tool_web_search_meta(const char *query, char *out, size_t cap) {
    const size_t RAW = 1000000;
    static Hit all[MS_NSRC * MS_PER_SRC], per[MS_PER_SRC];
    static char keys[MS_NSRC * MS_PER_SRC][500];
    FILE *fp[MS_NSRC];
    char q[1200], url[1700], fails[400];
    int n = 0;
    fails[0] = 0;
    url_enc(query, q, sizeof q);
    for (int i = 0; i < MS_NSRC; i++) {          /* todas as fontes em paralelo: abre todos os curl antes de ler */
        snprintf(url, sizeof url, SOURCES[i].fmt, q);
        fp[i] = curl_open(url, 12);
    }
    char *raw = (char *)malloc(RAW + 1);
    if (!raw) { for (int i = 0; i < MS_NSRC; i++) if (fp[i]) curl_finish(fp[i], raw, 0, NULL); snprintf(out, cap, "error: out of memory"); return; }
    for (int i = 0; i < MS_NSRC; i++) {
        int rc = -1, got = 0;
        const long len = fp[i] ? curl_finish(fp[i], raw, RAW, &rc) : -1;
        if (len > 0) {
            const int k = SOURCES[i].parse(raw, SOURCES[i].arg, per, MS_PER_SRC);
            for (int j = 0; j < k; j++) {
                char key[500];
                url_key(per[j].url, key, sizeof key);
                const double pts = 1.0 / (j + 1) + 0.5;      /* posicao na fonte + bonus por aparecer */
                int d = -1;
                for (int t = 0; t < n; t++) if (strcmp(keys[t], key) == 0) { d = t; break; }
                if (d < 0) { d = n++; all[d] = per[j]; snprintf(keys[d], sizeof keys[d], "%s", key); }
                else if (strlen(per[j].snip) > strlen(all[d].snip)) snprintf(all[d].snip, sizeof all[d].snip, "%s", per[j].snip);
                all[d].score += pts;
                all[d].src |= 1u << i;
                got++;
            }
        }
        if (!got) {
            size_t l = strlen(fails);
            snprintf(fails + l, sizeof fails - l, "%s%s(%s)", l ? ", " : "", SOURCES[i].name,
                     len > 0 ? "sem resultados/formato mudou/bloqueio" : "falha de rede");
        }
    }
    free(raw);
    if (fails[0]) agent_log("web_search", query, fails);        /* fontes que falharam */
    qsort(all, (size_t)n, sizeof all[0], hit_cmp);              /* estavel o bastante: empate mantem a ordem de insercao na pratica */
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < n && i < 8; i++) {
        o += (size_t)snprintf(out + o, cap - o, "%d. %s\n   %s\n   %.250s\n   (sources:", i + 1, all[i].title, all[i].url, all[i].snip);
        for (int s = 0; s < MS_NSRC; s++) if (all[i].src & (1u << s)) o += (size_t)snprintf(out + o, cap - o, " %s", SOURCES[s].name);
        o += (size_t)snprintf(out + o, cap - o, ")\n");
        if (o + 1800 > cap) break;
    }
    if (!n) snprintf(out, cap, "error: no search source returned results (%s)", fails);
}

static void tool_web_search(const char *query, char *out, size_t cap) {
    const char *sx = getenv("KATANA_SEARX");        /* opcional: SearXNG proprio; senao usa o meta-buscador embutido */
    if (sx && *sx && tool_web_search_searx(sx, query, out, cap) > 0) return;
    tool_web_search_meta(query, out, cap);
}

/* ---- search_files ---- */
typedef struct { const char *pat; char *out; size_t cap, n; int hits, files; } SFState;
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
        if (ci_find(b + i, e - i, s->pat, nl)) {
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

static void tool_search_files(const char *pattern, const char *root, char *out, size_t cap) {
    char lc[256], res[4200], err[1300];
    size_t i = 0;
    for (; pattern[i] && i < sizeof lc - 1; i++) lc[i] = (char)tolower((unsigned char)pattern[i]);
    lc[i] = 0;
    if (!lc[0]) { snprintf(out, cap, "error: empty pattern"); return; }
    if (!agent_read_access(root, 1, res, sizeof res, err, sizeof err)) { snprintf(out, cap, "%s", err); return; }
    SFState s = { lc, out, cap > 6000 ? 6000 : cap, 0, 0, 0 };
    out[0] = 0;
    struct stat st;
    if (stat(res, &st) == 0 && !S_ISDIR(st.st_mode)) sf_file(&s, res);
    else {
        const int rel = root[0] != '/' && root[0] != '\\' && root[0] != '~' && !strchr(root, ':');
        sf_walk(&s, rel ? root : res, 0);       /* dentro da pasta atual, mostra caminhos relativos */
    }
    if (s.hits == 0) snprintf(out, cap, "no matches in %d text files under %s", s.files, res);
    else if (s.hits >= SF_MAXHITS) snprintf(out + s.n, cap - s.n, "[result limit reached; refine the pattern or the path]\n");
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

