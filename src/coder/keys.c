/* ---------- coder/keys.c: chaves de texto (ligacoes que nao passam por simbolo, import nem chamada) ----------
 * Alem dos simbolos, cada arquivo emite CHAVES: literais de texto (ou nomes de variavel de ambiente) que funcionam como chave de ligacao entre
 * arquivos e processos: valor de um campo discriminador de mensagem (type: "register"), nome de evento (emit/on), rota (app.get, fetch, pathname ===),
 * variavel de ambiente (setenv/getenv/process.env.X/export X=), chave de configuracao, subcomando (strcmp(argv[1], "coder")).
 * Cada chave tem um PAPEL (PRODUCE: quem envia, emite, escreve, define, chama a rota; CONSUME: quem recebe, trata, le, registra a rota), uma
 * CLASSE (msg, event, route, env, config, cmd, other), um metodo HTTP quando houver, e a linha. Nao ha resolucao: a ligacao e o MESMO TEXTO em
 * arquivos diferentes (symquery/flow.c calcula a confianca). Aditivo: nao mexe em simbolos nem em referencias.
 *
 * O que nao pega (conhecido): chave montada em tempo de execucao (concatenacao, template com variavel no meio); desestruturacao
 * `const { A } = process.env`; mapas de despacho `{ register: fn }`; eventos de DOM/streams (data, close, click...) sao ignorados de proposito;
 * chaves genericas (type, id, name, data, error...) so valem como VALOR de campo discriminador; texto em f-string de Python e interpolacao de
 * shell so enxergam o literal; Rust: strings cruas r#".."#; linguagens lispy e de colunas (Clojure, Haskell, Fortran...) nao sao varridas.
 * Tolerancia: nada aqui trava; teto de 600 chaves por arquivo e 4000 bytes por literal. */
static const char *const CK_NOISE = " type id name data error ok true false text string object number integer boolean array null undefined none utf8 utf-8 value key default "
    "result status url path content index list dict str int bool json w r rb wb a ab get post put delete patch head options function class self this src dst en pt yes no on off auto all any "
    "user assistant system tool role file dir app main test foo bar x y z i n s t e v ";
static const char *const CK_SCHEMA = " string object number integer boolean array null undefined function ";
static const char *const CK_HTTPV = " GET POST PUT DELETE PATCH HEAD OPTIONS CONNECT TRACE ";
static const char *const CK_EVNOISE = " data end error close open click load exit connect connection drain finish readable ready unload keydown keyup keypress change input submit mousedown mouseup "
    "mousemove mouseover mouseout resize scroll focus blur drag drop dragover pointerdown pointerup pointermove uncaughtException unhandledRejection SIGINT SIGTERM SIGHUP message listening "
    "upgrade request response timeout abort cancel complete success failure progress readystatechange DOMContentLoaded beforeunload visibilitychange online offline storage popstate hashchange ";
static const char *const CK_SHBUILTIN = " HOME PATH PWD USER SHELL TERM LANG IFS OPTARG OPTIND RANDOM LINENO UID PPID HOSTNAME TMPDIR EDITOR PS1 DISPLAY OLDPWD SECONDS BASH BASH_SOURCE "
    "FUNCNAME EUID GROUPS LOGNAME MAIL PAGER TZ LC_ALL TEMP TMP USERNAME PIPESTATUS REPLY ";
typedef struct { CP *c; CoderFile *f; int cap; char swfield[48]; int swcls; } CK;
enum { CKF_NONE = 0, CKF_DISC, CKF_ROUTE, CKF_ARGV };

static int ck_in(const char *w, const char *list) {
    char b[80]; size_t l = strlen(w); if (!l || l > 70) return 0;
    snprintf(b, sizeof b, " %s ", w); return strstr(list, b) != NULL;
}
static int ck_isid(int ch) { return isalnum(ch) || ch == '_'; }
/* texto da chave valido? cls decide o alfabeto; disc = o valor de um campo discriminador (libera as chaves genericas) */
static int ck_valid(int cls, const char *s, int l, int disc) {
    if (l < 2 || l > (cls == CKC_ROUTE ? 90 : 60)) return 0;
    char zb[100]; memcpy(zb, s, (size_t)l); zb[l] = 0; s = zb;          /* o literal vem do texto do arquivo, sem NUL */
    int digits = 1;
    for (int i = 0; i < l; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch <= 32 || ch >= 127 || strchr("\"'`\\<>|;,&()", ch)) return 0;
        if (!isdigit(ch)) digits = 0;
        if (cls == CKC_ENV) { if (!(isalpha(ch) || ch == '_' || (i && isdigit(ch)))) return 0; }
        else if (cls == CKC_ROUTE) { if (!(isalnum(ch) || strchr("/_-.~%@+:", ch))) return 0; }
        else if (!(isalnum(ch) || strchr("_.:/-@#*+", ch))) return 0;
    }
    if (digits) return 0;
    if (cls == CKC_ENV) return !ck_in(s, " HOME PATH USERPROFILE APPDATA LOCALAPPDATA TEMP TMP TMPDIR SystemRoot COMSPEC SHELL USER USERNAME LANG TERM PWD XDG_CONFIG_HOME XDG_CACHE_HOME XDG_RUNTIME_DIR NO_COLOR COLUMNS LINES DISPLAY EDITOR PAGER LOGNAME HOSTNAME OLDPWD ");
    if (cls == CKC_ROUTE) return s[0] == '/';
    if (cls != CKC_ENV && (s[0] == '-' || s[0] == '.' || s[0] == '/' || s[0] == '#' || s[0] == '*' || s[0] == ':' || s[0] == '@' || s[0] == '+')) return 0;
    if (ck_in(s, CK_SCHEMA) || ck_in(s, CK_HTTPV)) return 0;
    if (cls != CKC_ENV && cls != CKC_ROUTE) {
        if (!disc && ck_in(s, CK_NOISE)) return 0;
        if (cls != CKC_MSG && cls != CKC_EVENT && strchr(s, '/')) return 0;
        const char *sl = strchr(s, '/'); if (sl && sl > s) { int alllow = 1; for (int i = 0; i < l; i++) if (isupper((unsigned char)s[i])) alllow = 0; if (alllow && !strchr(sl + 1, '/') && (!strncmp(s, "text/", 5) || !strncmp(s, "image/", 6) || !strncmp(s, "application/", 12) || !strncmp(s, "audio/", 6) || !strncmp(s, "video/", 6))) return 0; }
    }
    if (cls == CKC_EVENT && ck_in(s, CK_EVNOISE)) return 0;
    return 1;
}
static void ck_add(CK *K, int role, int cls, int meth, size_t off, const char *s, int l, int disc) {
    CoderFile *f = K->f;
    if (f->nkey >= 600 || !ck_valid(cls, s, l, disc)) return;
    int line = cp_line(K->c, off);
    for (int i = f->nkey - 1; i >= 0 && i >= f->nkey - 12; i--) if (f->key[i].line == line && f->key[i].role == role && (int)strlen(f->key[i].key) == l && !memcmp(f->key[i].key, s, (size_t)l)) return;
    if (f->nkey >= K->cap) { K->cap = K->cap ? K->cap * 2 : 32; f->key = (CoderKey *)realloc(f->key, sizeof(CoderKey) * (size_t)K->cap); }
    CoderKey *k = &f->key[f->nkey++]; k->key = cp_dup(s, (size_t)l); k->line = line; k->role = role; k->cls = cls; k->meth = meth; k->col = 0;
}

/* ---- rota: tira esquema, host e porta; template literal vira o trecho literal; parametros (:id, {id}, ${x}) cortam o caminho ---- */
static int ck_route_norm(const char *s, int l, char *o, int cap, int allow_noslash) {
    int i = 0;
    if (l > 400) return 0;
    const char *sch = NULL; for (int k = 0; k + 2 < l && k < 12; k++) if (s[k] == ':' && s[k + 1] == '/' && s[k + 2] == '/') { sch = s + k; break; }
    if (sch) i = (int)(sch - s) + 3; else if (l > 1 && s[0] == '/' && s[1] == '/') i = 2;
    if (sch || i == 2 || (l > 2 && s[0] == '$' && s[1] == '{')) {
        if (s[i] == '$' && i + 1 < l && s[i + 1] == '{') { while (i < l && s[i] != '}') i++; if (i < l) i++; }
        while (i < l && s[i] != '/' && s[i] != '?' && s[i] != '#') { if (s[i] == '$' && i + 1 < l && s[i + 1] == '{') { while (i < l && s[i] != '}') i++; } i++; }
    }
    if (i >= l) return 0;
    int w = 0; int slash = s[i] == '/';
    if (!slash) { if (!allow_noslash) return 0; o[w++] = '/'; }
    for (; i < l && w + 2 < cap; i++) {
        char ch = s[i];
        if (ch == '?' || ch == '#' || ch == ' ' || ch == '"' || ch == '\'') break;
        if (ch == '$' && i + 1 < l && s[i + 1] == '{') { if (w > 0 && o[w - 1] != '/') while (w > 0 && o[w - 1] != '/') w--; break; }
        if ((ch == ':' || ch == '{' || ch == '<' || ch == '[' || ch == '*') && w > 0 && o[w - 1] == '/') break;
        if (!(isalnum((unsigned char)ch) || strchr("/_-.~%@+", ch))) return 0;
        o[w++] = ch;
    }
    while (w > 1 && o[w - 1] == '/') w--;
    o[w] = 0;
    if (w < 2) return 0;
    static const char *const FS[] = {"/usr", "/etc", "/tmp", "/home", "/var", "/proc", "/dev", "/bin", "/opt", "/mnt", "/sys", "/lib", "/sbin", "/run", "/root", NULL};
    for (int k = 0; FS[k]; k++) { size_t fl = strlen(FS[k]); if (!strncmp(o, FS[k], fl) && (o[fl] == 0 || o[fl] == '/')) return 0; }
    return w;
}
static int ck_meth_of(const char *fn) {
    char lo[24]; size_t l = strlen(fn); if (l >= sizeof lo) return CKM_NONE;
    for (size_t i = 0; i <= l; i++) lo[i] = (char)tolower((unsigned char)fn[i]);
    if (!strcmp(lo, "get") || !strcmp(lo, "getmapping")) return CKM_GET;
    if (!strcmp(lo, "post") || !strcmp(lo, "postmapping")) return CKM_POST;
    if (!strcmp(lo, "put") || !strcmp(lo, "putmapping")) return CKM_PUT;
    if (!strcmp(lo, "delete") || !strcmp(lo, "del") || !strcmp(lo, "deletemapping")) return CKM_DELETE;
    if (!strcmp(lo, "patch") || !strcmp(lo, "patchmapping")) return CKM_PATCH;
    if (!strcmp(lo, "head") || !strcmp(lo, "options")) return CKM_OTHER;
    return CKM_NONE;
}
static int ck_meth_word(const char *s, int l) {
    char b[16]; if (l < 3 || l > 8) return CKM_NONE; memcpy(b, s, (size_t)l); b[l] = 0; return ck_meth_of(b);
}

/* ---- contexto da chamada que envolve um literal ---- */
typedef struct { char fn[40], qual[40]; int argi, deco, has, nested; const char *a0; int a0l; } CkCall;
static int ck_ident_start(const char *p, int e) { int s = e; while (s > 0 && (ck_isid((unsigned char)p[s - 1]) || p[s - 1] == '$' || ((unsigned char)p[s - 1] & 0x80))) s--; return s; }
static void ck_call(const char *p, int n, CkCall *o) {
    memset(o, 0, sizeof *o);
    int depth = 0, commas = 0, i = n, k = -1;
    while (i > 0) {
        char ch = p[i - 1];
        if (ch == ')' || ch == ']' || ch == '}') depth++;
        else if (ch == '(' || ch == '[' || ch == '{') {
            if (depth == 0) { if (ch == '(') { k = i - 1; break; } o->nested = 1; commas = 0; }
            else depth--;
        } else if (ch == ',' && depth == 0) commas++;
        else if (ch == ';' && depth == 0) break;
        i--;
    }
    if (k < 0) return;
    o->argi = commas;
    int e = k; while (e > 0 && (p[e - 1] == ' ' || p[e - 1] == '\t')) e--;
    if (e > 0 && p[e - 1] == '>') { int d = 0; while (e > 0) { if (p[e - 1] == '>') d++; else if (p[e - 1] == '<' && --d == 0) { e--; break; } e--; } }
    int s = ck_ident_start(p, e);
    if (s == e) return;
    int fl = e - s; if (fl > 39) fl = 39; memcpy(o->fn, p + s, (size_t)fl); o->fn[fl] = 0; o->has = 1;
    int q = s;
    if (q >= 2 && ((p[q - 1] == '.' ) || (p[q - 1] == ':' && p[q - 2] == ':') || (p[q - 1] == '>' && p[q - 2] == '-'))) {
        int qe = p[q - 1] == '.' ? q - 1 : q - 2; int qs = ck_ident_start(p, qe); int ql = qe - qs; if (ql > 39) ql = 39;
        if (ql > 0) { memcpy(o->qual, p + qs, (size_t)ql); o->qual[ql] = 0; }
        q = qs;
    }
    { int b = q; while (b > 0 && (p[b - 1] == '.' || ck_isid((unsigned char)p[b - 1]))) b--; q = b; }
    if (q > 0 && (p[q - 1] == '@' || (q >= 2 && p[q - 1] == '[' && p[q - 2] == '#'))) o->deco = 1;
    /* texto do primeiro argumento: de k+1 ate a primeira virgula do nivel */
    int a = k + 1, d2 = 0, z = a;
    for (; z < n; z++) { char ch = p[z]; if (ch == '(' || ch == '[' || ch == '{') d2++; else if (ch == ')' || ch == ']' || ch == '}') d2--; else if (ch == ',' && d2 == 0) break; }
    o->a0 = p + a; o->a0l = z - a;
}
/* campo antes do literal: "campo:", "campo =", "\"campo\":", "x[\"campo\"] =", "campo =>". sep: ':' '=' '>'  */
static int ck_field(const char *p, int n, char *name, size_t cap, int *sep) {
    int i = n; name[0] = 0; *sep = 0;
    while (i > 0 && isspace((unsigned char)p[i - 1])) i--;
    if (i >= 1 && p[i - 1] == ':' && !(i >= 2 && p[i - 2] == ':')) { *sep = ':'; i--; }
    else if (i >= 2 && p[i - 1] == '>' && p[i - 2] == '=') { *sep = '>'; i -= 2; }
    else if (i >= 1 && p[i - 1] == '=' && !(i >= 2 && strchr("=!<>+-*/&|%^~", p[i - 2]))) { *sep = '='; i--; }
    else return 0;
    while (i > 0 && isspace((unsigned char)p[i - 1])) i--;
    if (i > 0 && (p[i - 1] == '?' || p[i - 1] == '!')) i--;
    if (i > 0 && (p[i - 1] == '"' || p[i - 1] == '\'' || p[i - 1] == '`')) { char q = p[i - 1]; int e = i - 1, s = e; while (s > 0 && p[s - 1] != q && e - s < 40) s--; if (s > 0 && p[s - 1] == q) { int l = e - s; if (l > 0 && (size_t)l < cap) { memcpy(name, p + s, (size_t)l); name[l] = 0; return 1; } } return 0; }
    if (i > 0 && p[i - 1] == ']') {
        int e = i - 1, s = e; while (s > 0 && p[s - 1] != '[' && e - s < 60) s--;
        if (s > 0 && p[s - 1] == '[') { const char *b = p + s; int l = e - s; if (l >= 3 && (b[0] == '"' || b[0] == '\'') && b[l - 1] == b[0]) { l -= 2; if ((size_t)l < cap) { memcpy(name, b + 1, (size_t)l); name[l] = 0; return 1; } } }
        return 0;
    }
    int s = ck_ident_start(p, i); int l = i - s; if (l <= 0 || (size_t)l >= cap) return 0;
    memcpy(name, p + s, (size_t)l); name[l] = 0; return 1;
}
static int ck_fieldclass(const char *name) {
    char b[48]; int w = 0;
    for (const char *s = name; *s && w < 46; s++) if (*s != '_' && *s != '-') b[w++] = (char)tolower((unsigned char)*s);
    b[w] = 0; const char *x = b; if (!strncmp(x, "get", 3) && w > 3) x += 3;
    if (ck_in(x, " type kind event eventtype eventname evt action cmd command op opcode msgtype messagetype msg message topic channel verb subtype intent rpc method signal act cmdtype ")) return CKF_DISC;
    if (ck_in(x, " pathname url path route uri endpoint originalurl requrl href routepath requestpath urlpath ")) return CKF_ROUTE;
    if (ck_in(x, " argv args arg argument arguments cmdline subcmd subcommand ")) return CKF_ARGV;
    return CKF_NONE;
}
/* campo do operando que termina em p[i) (lado esquerdo de uma comparacao) */
static int ck_operand_back(const char *p, int i, char *name, size_t cap) {
    name[0] = 0;
    while (i > 0 && isspace((unsigned char)p[i - 1])) i--;
    if (i > 0 && p[i - 1] == ')') {
        int d = 0, s = i; while (s > 0) { if (p[s - 1] == ')') d++; else if (p[s - 1] == '(' && --d == 0) { s--; break; } s--; }
        const char *a = p + s + 1; int l = i - 2 - s;
        for (int k = 0; k < l; k++) if ((a[k] == '"' || a[k] == '\'') && k + 1 < l) { int e = k + 1; while (e < l && a[e] != a[k]) e++; int ll = e - k - 1; if (ll > 0 && (size_t)ll < cap) { memcpy(name, a + k + 1, (size_t)ll); name[ll] = 0; return 1; } break; }
        int id = ck_ident_start(p, s); int l2 = s - id; if (l2 > 0 && (size_t)l2 < cap) { memcpy(name, p + id, (size_t)l2); name[l2] = 0; return 1; }
        return 0;
    }
    if (i > 0 && p[i - 1] == ']') {
        int e = i - 1, s = e; while (s > 0 && p[s - 1] != '[' && e - s < 60) s--;
        if (s > 0) { const char *b = p + s; int l = e - s;
            if (l >= 3 && (b[0] == '"' || b[0] == '\'') && b[l - 1] == b[0]) { l -= 2; if ((size_t)l < cap) { memcpy(name, b + 1, (size_t)l); name[l] = 0; return 1; } }
            int id = ck_ident_start(p, s - 1); int l2 = s - 1 - id; if (l2 > 0 && (size_t)l2 < cap) { memcpy(name, p + id, (size_t)l2); name[l2] = 0; return 1; } }
        return 0;
    }
    int s = ck_ident_start(p, i); int l = i - s; if (l <= 0 || (size_t)l >= cap) return 0;
    memcpy(name, p + s, (size_t)l); name[l] = 0; return 1;
}
/* lado direito: "literal == campo.x" */
static int ck_operand_fwd(const char *p, int n, char *name, size_t cap) {
    int i = 0; name[0] = 0; while (i < n && isspace((unsigned char)p[i])) i++;
    int last = -1, ll = 0;
    while (i < n) {
        int s = i; while (i < n && (ck_isid((unsigned char)p[i]) || p[i] == '$')) i++;
        if (i > s) { last = s; ll = i - s; }
        if (i + 1 < n && p[i] == '.' ) { i++; continue; }
        if (i + 2 < n && p[i] == '-' && p[i + 1] == '>') { i += 2; continue; }
        if (i < n && p[i] == '[') { int e = i + 1; while (e < n && p[e] != ']' && e - i < 60) e++; if (e < n && e - i >= 4 && (p[i + 1] == '"' || p[i + 1] == '\'')) { last = i + 2; ll = e - i - 3; } i = e + 1; if (i < n && p[i] == '.') { i++; continue; } break; }
        if (i < n && p[i] == '(') { int d = 0, e = i; while (e < n) { if (p[e] == '(') d++; else if (p[e] == ')' && --d == 0) break; e++; } i = e + 1; if (i < n && p[i] == '.') { i++; continue; } break; }
        break;
    }
    if (last < 0 || ll <= 0 || (size_t)ll >= cap) return 0;
    memcpy(name, p + last, (size_t)ll); name[ll] = 0; return 1;
}
static int ck_after_case(const char *p, int n) {          /* ... case "a", "b": o literal atual segue `case` (e literais ja listados) */
    int i = n; int guard = 0;
    while (guard++ < 24) {
        while (i > 0 && isspace((unsigned char)p[i - 1])) i--;
        if (i >= 4 && !strncmp(p + i - 4, "case", 4) && (i == 4 || !ck_isid((unsigned char)p[i - 5]))) return 1;
        if (i > 0 && p[i - 1] == ',') { i--; while (i > 0 && isspace((unsigned char)p[i - 1])) i--; if (i > 0 && (p[i - 1] == '"' || p[i - 1] == '\'')) { char q = p[i - 1]; i--; while (i > 0 && p[i - 1] != q) i--; if (i > 0) i--; continue; } }
        return 0;
    }
    return 0;
}
static int ck_second_arg_handler(const char *post, int n) {
    int i = 0; while (i < n && isspace((unsigned char)post[i])) i++;
    if (i >= n || post[i] != ',') return 0;
    i++; while (i < n && isspace((unsigned char)post[i])) i++;
    if (i >= n || post[i] == '{' || post[i] == '[' || post[i] == '"' || post[i] == '\'' || post[i] == ')') return 0;
    if (isalpha((unsigned char)post[i]) || post[i] == '_') { int s = i; while (i < n && (ck_isid((unsigned char)post[i]))) i++; int w = i; while (i < n && post[i] == ' ') i++; if (i < n && post[i] == '=' && !(i + 1 < n && (post[i + 1] == '=' || post[i + 1] == '>')) && w - s < 20) return 0; }
    return 1;
}

/* ---- um literal ---- */
static void ck_string(CK *K, size_t qpos, const char *s, int l, size_t endpos) {
    const char *t = K->c->t; size_t n = K->c->n;
    size_t p0 = qpos > 240 ? qpos - 240 : 0; const char *pre = t + p0; int pl = (int)(qpos - p0);
    const char *post = t + endpos; int ql = (int)(n - endpos); if (ql > 260) ql = 260;
    if (l < 2 || l > 400) return;
    const int L = K->c->lang;
    CkCall C; ck_call(pre, pl, &C);
    const int direct = C.has && !C.nested;
    char fld[48], opn[48]; int sep = 0;
    /* ---- ambiente ---- */
    if (direct && C.argi == 0 && (ck_in(C.fn, " getenv secure_getenv Getenv GetEnv getEnv GetEnvironmentVariable GetEnvironmentVariableA GetEnvironmentVariableW LookupEnv env_get getenv_s ") ||
        (ck_in(C.fn, " get fetch getdefault pop ") && ck_in(C.qual, " environ env ENV Env ")) || (ck_in(C.fn, " var var_os ") && !strcmp(C.qual, "env")))) { ck_add(K, CKR_CONSUME, CKC_ENV, 0, qpos, s, l, 0); return; }
    if (direct && C.argi == 0 && (ck_in(C.fn, " setenv putenv _putenv Setenv SetEnv setEnv SetEnvironmentVariable SetEnvironmentVariableA SetEnvironmentVariableW set_var putEnv ") ||
        (!strcmp(C.fn, "setdefault") && ck_in(C.qual, " environ env ENV")) || (!strcmp(C.fn, "env") && C.qual[0]) || (!strcmp(C.fn, "putenv") && !strcmp(C.qual, "os")))) {
        int kl = l; for (int i = 0; i < l; i++) if (s[i] == '=') { kl = i; break; }
        ck_add(K, CKR_PRODUCE, CKC_ENV, 0, qpos, s, kl, 0); return;
    }
    { int i = pl; while (i > 0 && isspace((unsigned char)pre[i - 1])) i--;
      if (i > 1 && pre[i - 1] == '[') { int e = i - 1, b = ck_ident_start(pre, e); char id[24]; int il = e - b; if (il > 0 && il < 20) { memcpy(id, pre + b, (size_t)il); id[il] = 0;
          if (ck_in(id, " environ env ENV _ENV _SERVER ")) { int j = 0; while (j < ql && isspace((unsigned char)post[j])) j++; if (j < ql && post[j] == ']') { j++; while (j < ql && post[j] == ' ') j++; int w = j < ql && post[j] == '=' && !(j + 1 < ql && post[j + 1] == '='); ck_add(K, w ? CKR_PRODUCE : CKR_CONSUME, CKC_ENV, 0, qpos, s, l, 0); return; } } } } }
    if (l > 4 && isupper((unsigned char)s[0]) && L != CL_SH) { int i = 0; while (i < l && (isupper((unsigned char)s[i]) || isdigit((unsigned char)s[i]) || s[i] == '_')) i++; if (i >= 3 && i < l && s[i] == '=' ) { ck_add(K, CKR_PRODUCE, CKC_ENV, 0, qpos, s, i, 0); return; } }
    /* ---- rotas ---- */
    {
        char rk[100]; int rl = 0, role = 0, meth = CKM_NONE;
        const int decoev = C.has && C.deco && ck_in(C.fn, " KafkaListener RabbitListener JmsListener SqsListener SubscribeMessage MessageMapping EventListener EventPattern MessagePattern StreamListener ");
        int decovalue = 0;
        if (C.has && C.deco && C.nested) { if (ck_field(pre, pl, fld, sizeof fld, &sep) && (!strcmp(fld, "value") || !strcmp(fld, "path"))) decovalue = 1; }
        if (C.has && (C.deco ? (!C.nested || decovalue) && ck_in(C.fn, " GetMapping PostMapping PutMapping DeleteMapping PatchMapping RequestMapping Path GET POST PUT DELETE PATCH Route Get Post Put Delete Patch Head Options All get post put delete patch route head options websocket ws ") : direct && C.argi <= 1) ) {
            const int isclientfn = ck_in(C.fn, " fetch request urlopen Request ajax WebSocket EventSource getForObject postForObject Head curl wget NewRequest PostForm open WebSocketClient ");
            const int isverb = ck_in(C.fn, " get post put delete del patch head options Get Post Put Delete Patch Head Options ");
            const int isupverb = ck_in(C.fn, " GET POST PUT DELETE PATCH HEAD OPTIONS Any ");
            const int issrvonly = ck_in(C.fn, " use all route Route HandleFunc Handle handle Static static websocket ws ");
            const int argok = C.argi == 0 || ((!strcmp(C.fn, "NewRequest") || !strcmp(C.fn, "open")) && C.argi == 1);
            if (C.deco) role = CKR_CONSUME;
            else if (!argok) role = 0;
            else if (isclientfn) role = CKR_PRODUCE;
            else if (issrvonly) role = CKR_CONSUME;
            else if (isupverb) role = ck_in(C.qual, " http client axios ") ? CKR_PRODUCE : CKR_CONSUME;
            else if (isverb) {
                if (ck_in(C.qual, " axios http https requests client session httpx ky got superagent request urllib rest restTemplate webClient httpClient HttpClient $ jQuery fetch ")) role = CKR_PRODUCE;
                else if (ck_in(C.qual, " app router route routes server srv mux r e g group bp blueprint fastify hono koa rt engine web api v1 v2 r1 rg ")) role = ck_second_arg_handler(post, ql) || ck_in(C.qual, " app router route routes server srv mux bp blueprint fastify hono koa ") ? CKR_CONSUME : CKR_PRODUCE;
                else role = ck_second_arg_handler(post, ql) ? CKR_CONSUME : CKR_PRODUCE;
            }
            if (role) {
                rl = ck_route_norm(s, l, rk, sizeof rk, C.deco);
                if (rl) {
                    meth = ck_meth_of(C.fn);
                    if (!strcmp(C.fn, "NewRequest") || !strcmp(C.fn, "open")) { char *q = (char *)memchr(C.a0, '"', (size_t)C.a0l); if (!q) q = (char *)memchr(C.a0, '\'', (size_t)C.a0l); if (q) { int e = 1; while (q + e < C.a0 + C.a0l && q[e] != q[0]) e++; meth = ck_meth_word(q + 1, e - 1); } }
                    else if (role == CKR_PRODUCE && !strcmp(C.fn, "fetch")) { meth = CKM_GET; for (int i = 0; i + 8 < ql; i++) if (!strncmp(post + i, "method", 6)) { int j = i + 6; while (j < ql && (post[j] == ' ' || post[j] == ':' || post[j] == '=' || post[j] == '"' || post[j] == '\'')) j++; int e = j; while (e < ql && isalpha((unsigned char)post[e])) e++; int m = ck_meth_word(post + j, e - j); if (m) meth = m; break; } }
                    else if (!strcmp(C.fn, "curl") || !strcmp(C.fn, "wget")) meth = CKM_NONE;
                    ck_add(K, role, CKC_ROUTE, meth, qpos, rk, rl, 0); return;
                }
            }
        }
        if (decoev && C.argi == 0 + 0 && !strchr(s, ' ')) { ck_add(K, CKR_CONSUME, CKC_EVENT, 0, qpos, s, l, 0); return; }
        if (decoev) { ck_add(K, CKR_CONSUME, CKC_EVENT, 0, qpos, s, l, 0); return; }
    }
    /* ---- eventos, configuracao, comandos externos ---- */
    if (direct && C.argi == 0) {
        if (ck_in(C.fn, " on once addListener prependListener addEventListener addEventHandler subscribe handle handleOnce listen consume onEvent add_listener on_event psubscribe ")) { ck_add(K, CKR_CONSUME, CKC_EVENT, 0, qpos, s, l, 0); return; }
        if (ck_in(C.fn, " emit send publish dispatch trigger fire broadcast produce sendMessage postMessage invoke notify emitEvent send_json sendJSON Publish Emit Send Trigger sendToQueue send_message pubsub ") || (!strcmp(C.fn, "write") && ck_in(C.qual, " ws socket sock conn client stream pipe port channel ch "))) { ck_add(K, CKR_PRODUCE, CKC_EVENT, 0, qpos, s, l, 0); return; }
        if (ck_in(C.fn, " getProperty getString getInt getBoolean getLong getDouble GetString GetInt GetBool GetInt64 getSection get_str get_int ") || (ck_in(C.fn, " get ") && ck_in(C.qual, " config conf cfg settings props properties prefs viper options opts ")) ) { ck_add(K, CKR_CONSUME, CKC_CONFIG, 0, qpos, s, l, 0); return; }
        if (ck_in(C.fn, " setProperty Set set_option ") || (ck_in(C.fn, " set put ") && ck_in(C.qual, " config conf cfg settings props properties prefs viper options opts "))) { ck_add(K, CKR_PRODUCE, CKC_CONFIG, 0, qpos, s, l, 0); return; }
        if (!strcmp(C.fn, "Value") && C.deco && l > 3 && s[0] == '$' && s[1] == '{') { int e = l - 1; if (s[e] == '}') { int c = 2; while (c < e && s[c] != ':') c++; ck_add(K, CKR_CONSUME, CKC_CONFIG, 0, qpos, s + 2, c - 2, 0); return; } }
        if (ck_in(C.fn, " exec execSync spawn spawnSync execFile execFileSync system popen run call check_output check_call Popen getoutput shell Command ShellExecute runCommand sh ")) {
            char w[3][48]; int nw = 0, i = 0;
            if (ck_in(C.fn, " spawn spawnSync execFile execFileSync Popen Command run call check_output check_call ") && !memchr(s, ' ', (size_t)l)) {          /* programa e subcomando em literais vizinhos: ("katana", ["coder", ...]) */
                memcpy(w[0], s, (size_t)(l < 47 ? l : 47)); w[0][l < 47 ? l : 47] = 0; nw = 1;
                int j = 0; while (j < ql && (isspace((unsigned char)post[j]) || post[j] == ',' || post[j] == '[' || post[j] == '(')) j++;
                if (j < ql && (post[j] == '"' || post[j] == '\'')) { int e = j + 1; while (e < ql && post[e] != post[j] && e - j < 40) e++; int sl = e - j - 1; if (sl > 0 && sl < 47) { memcpy(w[1], post + j + 1, (size_t)sl); w[1][sl] = 0; nw = 2; } }
            } else {
                while (i < l && nw < 2) { while (i < l && s[i] == ' ') i++; int b = i; while (i < l && s[i] != ' ') i++; int wl = i - b; if (wl <= 0 || wl > 46) break; memcpy(w[nw], s + b, (size_t)wl); w[nw][wl] = 0; nw++; }
            }
            if (nw == 2) { const char *pg = strrchr(w[0], '/'); pg = pg ? pg + 1 : w[0];
                int ok = !ck_in(pg, " git npm npx node python python3 pip bash sh cd ls echo make cp mv rm mkdir cat grep sed awk curl wget sudo tar chmod docker cargo go java mvn gradle apt apt-get brew yarn pnpm powershell cmd dotnet ") && islower((unsigned char)w[1][0]);
                for (const char *x = w[1]; *x && ok; x++) if (!(islower((unsigned char)*x) || isdigit((unsigned char)*x) || *x == '-')) ok = 0;
                if (ok) { ck_add(K, CKR_PRODUCE, CKC_CMD, 0, qpos, w[1], (int)strlen(w[1]), 0); return; } }
        }
    }
    /* ---- comparacoes e despacho ---- */
    {
        int fc = CKF_NONE, role = 0, cls = CKC_OTHER, disc = 0, found = 0;
        if (ck_field(pre, pl, fld, sizeof fld, &sep)) {
            fc = ck_fieldclass(fld);
            if (fc == CKF_DISC && !(sep == '=' && 0)) { ck_add(K, CKR_PRODUCE, CKC_MSG, 0, qpos, s, l, 1); return; }
            if (fc == CKF_ROUTE && sep != '=') { char rk[100]; int rl = ck_route_norm(s, l, rk, sizeof rk, 0); if (rl) { ck_add(K, CKR_PRODUCE, CKC_ROUTE, 0, qpos, rk, rl, 0); return; } }
        }
        int i = pl; while (i > 0 && isspace((unsigned char)pre[i - 1])) i--;
        if (i >= 2 && (!strncmp(pre + i - 2, "==", 2) || !strncmp(pre + i - 2, "!=", 2))) {
            int e = i - 2; if (e > 0 && (pre[e - 1] == '=' || pre[e - 1] == '!')) e--;
            if (!(i >= 2 && pre[i - 2] == '=' && i >= 3 && strchr("<>", pre[i - 3]))) { if (ck_operand_back(pre, e, opn, sizeof opn)) { fc = ck_fieldclass(opn); found = 1; } else found = 1; }
        } else if (C.has && C.argi >= 0 && ck_in(C.fn, " equals equalsIgnoreCase eq strcmp strcasecmp strncmp strncasecmp stricmp strnicmp streq g_strcmp0 Equals EqualsIgnoreCase startsWith endsWith StartsWith EndsWith compare ")) {
            found = 1;
            if (!strncmp(C.fn, "str", 3) || !strcmp(C.fn, "stricmp") || !strcmp(C.fn, "g_strcmp0") || !strcmp(C.fn, "compare")) { if (C.argi == 1 && C.a0l > 0 && C.a0l < 100) { char tmp[110]; memcpy(tmp, C.a0, (size_t)C.a0l); tmp[C.a0l] = 0; ck_operand_back(tmp, C.a0l, opn, sizeof opn); fc = ck_fieldclass(opn); if (fc == CKF_NONE && strstr(tmp, "argv")) fc = CKF_ARGV; } }
            else { int e = (int)(C.a0 - pre); (void)e; /* equals(): o operando e o qualificador */ int p2 = pl; int d = 0; while (p2 > 0) { if (pre[p2 - 1] == '(') { if (--d < 0) break; } else if (pre[p2 - 1] == ')') d++; p2--; }
                if (p2 > 0) { int fe = p2 - 1; while (fe > 0 && pre[fe - 1] != '.' ) fe--; if (fe > 1) { ck_operand_back(pre, fe - 1, opn, sizeof opn); fc = ck_fieldclass(opn); } } }
        } else if (ck_after_case(pre, pl)) { found = 1; if (K->swfield[0]) fc = ck_fieldclass(K->swfield); }
        else {
            int j = 0; while (j < ql && isspace((unsigned char)post[j])) j++;
            if (j + 8 < ql && post[j] == '.' && !strncmp(post + j, ".equals", 7)) { while (j < ql && post[j] != '(') j++; if (j < ql && ck_operand_fwd(post + j + 1, ql - j - 1, opn, sizeof opn)) fc = ck_fieldclass(opn); found = 1; }
            else if (j + 1 < ql && ((post[j] == '=' && post[j + 1] == '=') || (post[j] == '!' && post[j + 1] == '='))) { j += 2; if (j < ql && post[j] == '=') j++; if (ck_operand_fwd(post + j, ql - j, opn, sizeof opn)) { fc = ck_fieldclass(opn); } found = 1; }
            else if (L == CL_RS && j + 1 < ql && ((post[j] == '=' && post[j + 1] == '>') || post[j] == '|')) { found = 1; if (K->swfield[0]) fc = ck_fieldclass(K->swfield); }
            else if (L == CL_KT && j + 1 < ql && post[j] == '-' && post[j + 1] == '>') { found = 1; }
        }
        if (found) {
            role = CKR_CONSUME;
            if (fc == CKF_DISC) { cls = CKC_MSG; disc = 1; }
            else if (fc == CKF_ROUTE) { char rk[100]; int rl = ck_route_norm(s, l, rk, sizeof rk, 0); if (rl) { ck_add(K, role, CKC_ROUTE, 0, qpos, rk, rl, 0); return; } return; }
            else if (fc == CKF_ARGV) cls = CKC_CMD;
            ck_add(K, role, cls, 0, qpos, s, l, disc); return;
        }
        if (fc == CKF_ROUTE && C.has && ck_in(C.fn, " startsWith endsWith ")) { char rk[100]; int rl = ck_route_norm(s, l, rk, sizeof rk, 0); if (rl) ck_add(K, CKR_CONSUME, CKC_ROUTE, 0, qpos, rk, rl, 0); return; }
    }
    /* ---- tabela de comandos em C/C++: {"nome", ...} ---- */
    if ((L == CL_C || L == CL_CPP) && l >= 3 && l <= 24 && islower((unsigned char)s[0])) {
        int i = pl; while (i > 0 && isspace((unsigned char)pre[i - 1])) i--;
        int j = 0; while (j < ql && isspace((unsigned char)post[j])) j++;
        if (i > 0 && pre[i - 1] == '{' && j < ql && post[j] == ',') { int ok = 1; for (int k = 0; k < l; k++) if (!(islower((unsigned char)s[k]) || isdigit((unsigned char)s[k]) || s[k] == '-' || s[k] == '_')) ok = 0; if (ok) { ck_add(K, CKR_CONSUME, CKC_CMD, 0, qpos, s, l, 0); return; } }
    }
    /* ---- URL solta: so as que apontam para a propria maquina (localhost) ou sao websocket ---- */
    { const char *sc = (const char *)memchr(s, ':', (size_t)(l < 12 ? l : 12));
      if (sc && sc + 2 < s + l && sc[1] == '/' && sc[2] == '/') { int local = strstr(sc + 3, "localhost") == sc + 3 || strstr(sc + 3, "127.0.0.1") == sc + 3 || strstr(sc + 3, "0.0.0.0") == sc + 3 || strstr(sc + 3, "[::1]") == sc + 3 || sc[3] == '$' || !strncmp(s, "ws", 2);
        char rk[100]; int rl = local ? ck_route_norm(s, l, rk, sizeof rk, 0) : 0; if (rl) ck_add(K, CKR_PRODUCE, CKC_ROUTE, 0, qpos, rk, rl, 0); } }
}

/* ---- varredura de linguagens com sintaxe de string de codigo ---- */
static int ck_sq_char(int L) { return L == CL_C || L == CL_CPP || L == CL_JAVA || L == CL_GO || L == CL_RS || L == CL_CS || L == CL_KT || L == CL_SCALA || L == CL_OBJC || L == CL_ZIG || L == CL_SWIFT; }
static int ck_generic_lang(int L) {
    switch (L) { case CL_JS: case CL_TS: case CL_PY: case CL_KT: case CL_JAVA: case CL_GO: case CL_RS: case CL_C: case CL_CPP: case CL_QML: case CL_PS1: case CL_CS: case CL_SWIFT: case CL_PHP: case CL_RB:
        case CL_LUA: case CL_DART: case CL_SCALA: case CL_GROOVY: case CL_OBJC: case CL_ZIG: case CL_PERL: case CL_EX: case CL_JL: case CL_R: case CL_VUE: case CL_CMAKE: return 1; default: return 0; }
}
static void ck_generic(CK *K) {
    const char *t = K->c->t; size_t n = K->c->n; const int L = K->c->lang;
    const int sqstr = !ck_sq_char(L), bt = L == CL_JS || L == CL_TS || L == CL_VUE || L == CL_GO;
    size_t i = 0;
    while (i < n && K->f->nkey < 600) {
        unsigned char ch = (unsigned char)t[i];
        if (ch == '"' || (ch == '\'' && sqstr) || (ch == '`' && bt)) {
            const char q = (char)ch;
            if ((L == CL_PY || L == CL_KT || L == CL_SCALA || L == CL_DART || L == CL_GROOVY || L == CL_SWIFT || L == CL_JL || L == CL_RB) && i + 2 < n && t[i + 1] == q && t[i + 2] == q) {
                size_t j = i + 3; while (j + 2 < n && !(t[j] == q && t[j + 1] == q && t[j + 2] == q)) j++; i = j + 3; continue;
            }
            size_t j = i + 1; int ml = q == '`';
            while (j < n && t[j] != q && j - i < 4000) { if (t[j] == '\\' && q != '`') j++; else if (t[j] == '\n' && !ml) break; j++; }
            if (j >= n || t[j] != q || j - i >= 4000) { i++; continue; }
            if (j > i + 1) ck_string(K, i, t + i + 1, (int)(j - i - 1), j + 1);
            i = j + 1; continue;
        }
        if (ch == '\'' && !sqstr) {
            if (i + 1 < n && t[i + 1] == '\\') { size_t j = i + 2; while (j < n && t[j] != '\'' && j - i < 8) j++; i = j + 1; } else if (i + 2 < n && t[i + 2] == '\'') i += 3; else i++;
            continue;
        }
        if (cp_idc(ch) && (i == 0 || !cp_idc((unsigned char)t[i - 1]))) {
            size_t j = i; while (j < n && cp_idc((unsigned char)t[j])) j++;
            const size_t wl = j - i;
            if ((wl == 6 && !memcmp(t + i, "switch", 6)) || (wl == 5 && !memcmp(t + i, "match", 5)) || (wl == 4 && !memcmp(t + i, "when", 4))) {
                size_t a = j; while (a < n && t[a] == ' ') a++;
                size_t b; if (a < n && t[a] == '(') { int d = 0; b = a; while (b < n && b - a < 90) { if (t[b] == '(') d++; else if (t[b] == ')' && --d == 0) break; b++; } a++; }
                else { b = a; while (b < n && t[b] != '{' && t[b] != '\n' && b - a < 80) b++; }
                char tmp[100]; int tl = (int)(b - a); if (tl > 90) tl = 90; memcpy(tmp, t + a, (size_t)tl); tmp[tl] = 0;
                while (tl > 0 && (tmp[tl - 1] == ' ' || tmp[tl - 1] == ':')) tmp[--tl] = 0;
                K->swfield[0] = 0; if (tl > 0) ck_operand_back(tmp, tl, K->swfield, sizeof K->swfield);
            } else if (wl == 3 && !memcmp(t + i, "env", 3) && (L == CL_JS || L == CL_TS || L == CL_VUE) && i >= 2 && t[i - 1] == '.' && j + 1 < n && t[j] == '.') {
                size_t a = j + 1, b = a; while (b < n && ck_isid((unsigned char)t[b])) b++;
                int pw = (i >= 8 && !memcmp(t + i - 8, "process.", 8)) || (i >= 5 && !memcmp(t + i - 5, "meta.", 5)) || (i >= 4 && !memcmp(t + i - 4, "Bun.", 4));
                if (pw && b > a) { size_t z = b; while (z < n && (t[z] == ' ')) z++; int w = z < n && t[z] == '=' && !(z + 1 < n && (t[z + 1] == '=' || t[z + 1] == '>')); ck_add(K, w ? CKR_PRODUCE : CKR_CONSUME, CKC_ENV, 0, a, t + a, (int)(b - a), 0); }
            } else if (wl == 3 && !memcmp(t + i, "ENV", 3) && L == CL_CMAKE && j < n && t[j] == '{') {
                size_t a = j + 1, b = a; while (b < n && t[b] != '}' && b - a < 60) b++;
                if (b < n) ck_add(K, i > 0 && t[i - 1] == '$' ? CKR_CONSUME : CKR_PRODUCE, CKC_ENV, 0, a, t + a, (int)(b - a), 0);
            } else if (wl == 3 && !memcmp(t + i, "env", 3) && L == CL_PS1 && i > 0 && t[i - 1] == ':' ) { }
            i = j; continue;
        }
        if (L == CL_PS1 && ch == '$' && i + 4 < n && !strncasecmp(t + i, "$env:", 5)) {
            size_t a = i + 5, b = a; while (b < n && ck_isid((unsigned char)t[b])) b++;
            if (b > a) { size_t z = b; while (z < n && t[z] == ' ') z++; int w = z < n && t[z] == '=' && !(z + 1 < n && t[z + 1] == '='); ck_add(K, w ? CKR_PRODUCE : CKR_CONSUME, CKC_ENV, 0, a, t + a, (int)(b - a), 0); }
            i = b; continue;
        }
        i++;
    }
}

/* ---- shell, Dockerfile, YAML, properties, TOML: por linha ---- */
static void ck_dollar_reads(CK *K, size_t a, size_t e, int skip_single) {
    const char *t = K->c->t;
    int insq = 0;
    for (size_t i = a; i < e; i++) {
        if (skip_single && t[i] == '\'') insq = !insq;
        if (t[i] != '$' || insq || (i > a && t[i - 1] == '\\') || i + 1 >= e) continue;
        size_t s = i + 1; int br = 0; if (t[s] == '{') { br = 1; s++; if (s < e && t[s] == '{') continue; } else if (t[s] == '(') continue;
        size_t b = s; while (b < e && (isupper((unsigned char)t[b]) || isdigit((unsigned char)t[b]) || t[b] == '_')) b++;
        if (b - s < 3 || !(isupper((unsigned char)t[s]) || t[s] == '_')) continue;
        if (b < e && (islower((unsigned char)t[b]) || (!br && ck_isid((unsigned char)t[b])))) continue;
        char nm[64]; int nl = (int)(b - s); if (nl > 60) continue; memcpy(nm, t + s, (size_t)nl); nm[nl] = 0;
        if (ck_in(nm, CK_SHBUILTIN)) continue;
        ck_add(K, CKR_CONSUME, CKC_ENV, 0, s, nm, nl, 0);
    }
}
static void ck_urls_line(CK *K, size_t a, size_t e) {
    const char *t = K->c->t;
    for (size_t i = a; i + 7 < e; i++) {
        if (!((t[i] == 'h' && !strncmp(t + i, "http://", 7)) || (t[i] == 'w' && !strncmp(t + i, "ws://", 5)) || (t[i] == 'w' && !strncmp(t + i, "wss://", 6)))) continue;
        size_t b = i; while (b < e && !isspace((unsigned char)t[b]) && t[b] != '"' && t[b] != '\'' && t[b] != ')' && t[b] != '`') b++;
        const char *u = t + i; int ul = (int)(b - i); const char *h = strstr(u, "://"); h = h ? h + 3 : u;
        if (!(!strncmp(h, "localhost", 9) || !strncmp(h, "127.0.0.1", 9) || !strncmp(h, "0.0.0.0", 7) || h[0] == '$' || !strncmp(u, "ws", 2))) { i = b; continue; }
        char rk[100]; int rl = ck_route_norm(u, ul, rk, sizeof rk, 0);
        if (rl) { int meth = CKM_NONE; for (size_t z = a; z + 3 < i; z++) if (!strncmp(t + z, "-X ", 3)) { size_t m = z + 3; size_t me = m; while (me < e && isalpha((unsigned char)t[me])) me++; meth = ck_meth_word(t + m, (int)(me - m)); } ck_add(K, CKR_PRODUCE, CKC_ROUTE, meth, i, rk, rl, 0); }
        i = b;
    }
}
static void ck_shell(CK *K) {
    const char *t = K->c->t; size_t n = K->c->n; int incase = 0;
    for (size_t a = 0; a < n && K->f->nkey < 600; ) {
        size_t e = a; while (e < n && t[e] != '\n') e++;
        size_t s = a; while (s < e && (t[s] == ' ' || t[s] == '\t')) s++;
        if (s < e) {
            size_t p = s;
            if (e - s > 7 && (!strncmp(t + s, "export ", 7) || !strncmp(t + s, "readonly", 8) || !strncmp(t + s, "declare ", 8) || !strncmp(t + s, "local ", 6))) { while (p < e && t[p] != ' ') p++; while (p < e && t[p] == ' ') p++; if (p + 1 < e && t[p] == '-' ) { while (p < e && t[p] != ' ') p++; while (p < e && t[p] == ' ') p++; } }
            int lead = 1, any = 0;
            while (p < e) {
                size_t b = p; while (b < e && (ck_isid((unsigned char)t[b]))) b++;
                if (b > p && b < e && t[b] == '=' && (isalpha((unsigned char)t[p]) || t[p] == '_')) {
                    if (b - p >= 2 && !(b + 1 < e && t[b + 1] == '=')) ck_add(K, CKR_PRODUCE, CKC_ENV, 0, p, t + p, (int)(b - p), 0);
                    any = 1; size_t v = b + 1; int q = 0; while (v < e && (q || (t[v] != ' ' && t[v] != '\t'))) { if (t[v] == '"' || t[v] == '\'') q = q ? 0 : 1; v++; } p = v; while (p < e && (t[p] == ' ' || t[p] == '\t')) p++; continue;
                }
                break;
            }
            if (lead && p < e && !any && !strncmp(t + s, "export", 6)) { /* export NAME sem valor */ size_t b = p; while (b < e && ck_isid((unsigned char)t[b])) b++; if (b > p && b - p >= 2) ck_add(K, CKR_PRODUCE, CKC_ENV, 0, p, t + p, (int)(b - p), 0); }
            if (e - s >= 5 && !strncmp(t + s, "case ", 5) && strstr(t + s, " in")) incase++;
            else if (e - s >= 4 && !strncmp(t + s, "esac", 4)) { if (incase) incase--; }
            else if (incase) {
                size_t b = s; int ok = 1; size_t z = s;
                while (z < e && (ck_isid((unsigned char)t[z]) || t[z] == '-' || t[z] == '|')) z++;
                if (z > s && z < e && t[z] == ')' ) { for (size_t w = s; w < z && ok; ) { size_t x = w; while (x < z && t[x] != '|') x++; if (x - w >= 3 && islower((unsigned char)t[w])) ck_add(K, CKR_CONSUME, CKC_CMD, 0, w, t + w, (int)(x - w), 0); w = x + 1; } }
                (void)b;
            }
            if (p < e && (isalpha((unsigned char)t[p]) || t[p] == '.' || t[p] == '/')) {
                size_t b = p; while (b < e && !isspace((unsigned char)t[b]) && (ck_isid((unsigned char)t[b]) || t[b] == '.' || t[b] == '/' || t[b] == '-')) b++;
                if (b < e && !isspace((unsigned char)t[b])) b = e;
                size_t c0 = b; while (c0 < e && (t[c0] == ' ' || t[c0] == '\t')) c0++; size_t c1 = c0; while (c1 < e && (islower((unsigned char)t[c1]) || isdigit((unsigned char)t[c1]) || t[c1] == '-')) c1++;
                char pg[48]; int pgl = (int)(b - p); if (pgl > 0 && pgl < 47 && c1 - c0 >= 3 && c1 - c0 < 25 && (c1 == e || isspace((unsigned char)t[c1]) || t[c1] == ';')) {
                    memcpy(pg, t + p, (size_t)pgl); pg[pgl] = 0; const char *bn = strrchr(pg, '/'); bn = bn ? bn + 1 : pg;
                    if (!ck_in(bn, " git npm npx node python python3 pip bash sh cd ls echo make cp mv rm mkdir cat grep sed awk curl wget sudo tar chmod docker cargo go java mvn gradle apt apt-get brew yarn pnpm powershell cmd dotnet source export set unset if then else fi for while do done return exit test kill sleep read eval exec trap shift printf touch chown find sort head tail wc cut tr xargs tee diff patch ln systemctl service journalctl ssh scp rsync zip unzip gzip gunzip which type command alias unalias wait cmake gcc cc g++ clang ninja install ") && (isalpha((unsigned char)t[c0]))) ck_add(K, CKR_PRODUCE, CKC_CMD, 0, c0, t + c0, (int)(c1 - c0), 0);
                }
            }
            ck_dollar_reads(K, s, e, 1);
            ck_urls_line(K, s, e);
        }
        a = e + 1;
    }
}
static void ck_docker(CK *K) {
    const char *t = K->c->t; size_t n = K->c->n;
    for (size_t a = 0; a < n && K->f->nkey < 600; ) {
        size_t e = a; while (e < n && t[e] != '\n') e++;
        size_t s = a; while (s < e && (t[s] == ' ' || t[s] == '\t')) s++;
        int isenv = e - s > 4 && !strncasecmp(t + s, "ENV ", 4), isarg = e - s > 4 && !strncasecmp(t + s, "ARG ", 4);
        if (isenv || isarg) {
            size_t p = s + 4;
            while (p < e) {
                while (p < e && t[p] == ' ') p++;
                size_t b = p; while (b < e && ck_isid((unsigned char)t[b])) b++;
                if (b > p && b - p >= 2) ck_add(K, CKR_PRODUCE, CKC_ENV, 0, p, t + p, (int)(b - p), 0);
                if (b < e && t[b] == '=') { size_t v = b + 1; int q = 0; while (v < e && (q || t[v] != ' ')) { if (t[v] == '"') q = !q; v++; } p = v; continue; }
                break;
            }
            ck_dollar_reads(K, s + 4, e, 0);
        } else { ck_dollar_reads(K, s, e, 0); ck_urls_line(K, s, e); }
        a = e + 1;
    }
}
static void ck_yaml(CK *K) {
    const char *t = K->c->t; size_t n = K->c->n; int envind = -1;
    for (size_t a = 0; a < n && K->f->nkey < 600; ) {
        size_t e = a; while (e < n && t[e] != '\n') e++;
        size_t s = a; int ind = 0; while (s < e && (t[s] == ' ' || t[s] == '\t')) { s++; ind++; }
        if (s < e && t[s] != '#') {
            size_t le = e; while (le > s && (t[le - 1] == ' ' || t[le - 1] == '\r')) le--;
            if (envind >= 0 && ind <= envind) envind = -1;
            if (envind >= 0) {
                size_t p = s; if (t[p] == '-') { p++; while (p < le && t[p] == ' ') p++; }
                if (le - p > 5 && !strncmp(t + p, "name:", 5)) { size_t v = p + 5; while (v < le && t[v] == ' ') v++; if (v < le && (t[v] == '"' || t[v] == '\'')) v++; size_t b = v; while (b < le && ck_isid((unsigned char)t[b])) b++; if (b - v >= 2) ck_add(K, CKR_PRODUCE, CKC_ENV, 0, v, t + v, (int)(b - v), 0); }
                else { size_t b = p; while (b < le && ck_isid((unsigned char)t[b])) b++; if (b - p >= 2 && b < le && (t[b] == ':' || t[b] == '=') && (isupper((unsigned char)t[p]) || t[p] == '_')) ck_add(K, CKR_PRODUCE, CKC_ENV, 0, p, t + p, (int)(b - p), 0); }
            } else {
                size_t b = s; while (b < le && ck_isid((unsigned char)t[b])) b++;
                if (b < le && t[b] == ':' && b + 1 == le && ((b - s == 3 && !memcmp(t + s, "env", 3)) || (b - s == 11 && !memcmp(t + s, "environment", 11)) || (b - s == 9 && !memcmp(t + s, "variables", 9)))) envind = ind;
                else if (t[s] == '/' && le > s + 1 && t[le - 1] == ':' && ind >= 1) { char rk[100]; int rl = ck_route_norm(t + s, (int)(le - 1 - s), rk, sizeof rk, 0); if (rl) ck_add(K, CKR_CONSUME, CKC_ROUTE, 0, s, rk, rl, 0); }
            }
            ck_dollar_reads(K, s, le, 0);
        }
        a = e + 1;
    }
}
static void ck_props(CK *K, int toml) {
    const char *t = K->c->t; size_t n = K->c->n; char sect[60] = "";
    const char *base = strrchr(K->c->path, '/'); base = base ? base + 1 : K->c->path;
    const int dotenv = !strncmp(base, ".env", 4) || strstr(base, ".env") != NULL;
    for (size_t a = 0; a < n && K->f->nkey < 600; ) {
        size_t e = a; while (e < n && t[e] != '\n') e++;
        size_t s = a; while (s < e && (t[s] == ' ' || t[s] == '\t')) s++;
        size_t le = e; while (le > s && (t[le - 1] == ' ' || t[le - 1] == '\r')) le--;
        if (s < le && t[s] != '#' && t[s] != ';') {
            if (toml && t[s] == '[') { size_t b = s + 1; if (b < le && t[b] == '[') b++; size_t z = b; while (z < le && t[z] != ']') z++; int sl = (int)(z - b); if (sl > 0 && sl < 58) { memcpy(sect, t + b, (size_t)sl); sect[sl] = 0; } else sect[0] = 0; }
            else {
                size_t p = s; if (!strncmp(t + p, "export ", 7)) p += 7;
                size_t b = p; while (b < le && (ck_isid((unsigned char)t[b]) || t[b] == '.' || t[b] == '-')) b++;
                size_t q = b; while (q < le && t[q] == ' ') q++;
                if (b - p >= 2 && q < le && (t[q] == '=' || (!toml && t[q] == ':'))) {
                    int upper = 1; for (size_t z = p; z < b; z++) if (islower((unsigned char)t[z]) || t[z] == '.' || t[z] == '-') upper = 0;
                    if (upper || dotenv) { if (upper || !memchr(t + p, '.', b - p)) ck_add(K, CKR_PRODUCE, CKC_ENV, 0, p, t + p, (int)(b - p), 0); }
                    else if (memchr(t + p, '.', b - p) || sect[0]) { char kk[100]; if (toml && sect[0]) snprintf(kk, sizeof kk, "%s.%.*s", sect, (int)(b - p), t + p); else snprintf(kk, sizeof kk, "%.*s", (int)(b - p), t + p); ck_add(K, CKR_PRODUCE, CKC_CONFIG, 0, p, kk, (int)strlen(kk), 0); }
                    ck_dollar_reads(K, q, le, 0);
                }
            }
        }
        a = e + 1;
    }
}
static void ck_extract(CP *c) {
    CK K; memset(&K, 0, sizeof K); K.c = c; K.f = c->f; const int L = c->lang;
    if (!c->t || c->n < 3) return;
    if (L == CL_SH) ck_shell(&K);
    else if (L == CL_DOCKER) ck_docker(&K);
    else if (L == CL_YAML) ck_yaml(&K);
    else if (L == CL_PROPS) ck_props(&K, 0);
    else if (L == CL_TOML) ck_props(&K, 1);
    else if (ck_generic_lang(L)) ck_generic(&K);
}
