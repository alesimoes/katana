/* ---------- coder/keys.c: chaves de texto (ligacoes que nao passam por simbolo, import nem chamada) ----------
 * Alem dos simbolos, cada arquivo emite CHAVES: literais de texto (ou nomes de variavel de ambiente) que funcionam como chave de ligacao entre
 * arquivos e processos: valor de um campo discriminador de mensagem (type: "register"), nome de evento (emit/on), rota (app.get, fetch, pathname ===),
 * variavel de ambiente (setenv/getenv/process.env.X/export X=), chave de configuracao, subcomando (strcmp(argv[1], "coder")).
 * Arquivos de CONTRATO (OpenAPI, AsyncAPI, .proto, GraphQL, JSON Schema, enum/uniao tipado) tambem definem chaves: papel DEFINE, classe contract (bloco "CONTRATOS" abaixo).
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
typedef struct { CP *c; CoderFile *f; int cap; char swfield[48]; int swcls; int max, contract; } CK;      /* max = teto de chaves do arquivo (600; 5000 em contrato); contract = o arquivo e um contrato */
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
    if (f->nkey >= K->max || !ck_valid(cls, s, l, disc)) return;
    int line = cp_line(K->c, off);
    for (int i = f->nkey - 1; i >= 0 && i >= f->nkey - 12; i--) if (f->key[i].line == line && f->key[i].role == role && (int)strlen(f->key[i].key) == l && !memcmp(f->key[i].key, s, (size_t)l)) return;
    if (f->nkey >= K->cap) { K->cap = K->cap ? K->cap * 2 : 32; f->key = (CoderKey *)realloc(f->key, sizeof(CoderKey) * (size_t)K->cap); }
    CoderKey *k = &f->key[f->nkey++]; k->key = cp_dup(s, (size_t)l); k->line = line; k->role = role; k->cls = cls; k->meth = meth; k->col = 0; k->sub = 0;
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

/* ================= CONTRATOS como fonte de chaves (papel DEFINE, classe contract) =================
 * Arquivos de contrato definem chaves que o codigo produz/consome por texto igual: OpenAPI/Swagger (yaml e json: rotas por metodo, operationId, nomes de schemas, discriminator.mapping),
 * AsyncAPI (channels, operations, messages), Protocol Buffers (service/rpc como rota "/pacote.Servico/Metodo", `option (google.api.http)`, nomes de message), GraphQL (campos de
 * Query/Mutation/Subscription como "Query.campo"; as consultas dos clientes sao lidas das strings), JSON Schema ($id, definitions/$defs, $ref para outro arquivo) e declaracoes
 * tipadas de mensagens (enum e uniao de literais em TypeScript, Enum/Literal em Python, const tipada em Go) cujo valor e o discriminador. A chave de uma rota e a MESMA do extrator de
 * literais (ck_route_norm: parametros cortam o caminho), entao produtor, consumidor e contrato caem na mesma chave; o `flow` (flow.c) liga os tres lados.
 * Tolerante: nada aqui trava; arquivo de contrato acima de 6 MB nao e lido, 40000 eventos de estrutura e 5000 chaves por arquivo, string acima de 4000 bytes aborta a leitura. */
#define CT_D 10
enum { CT_OAS = 1, CT_ASY = 2, CT_JSCH = 4 };
typedef struct { int depth; const char *k[CT_D]; int kl[CT_D]; const char *v; int vl; int isval; size_t off; } CtEv;
typedef void (*CtFn)(void *ctx, const CtEv *e);

static int ct_idc(int ch) { return isalnum(ch) || ch == '_' || ch == '$'; }
static int ct_valid(int sub, const char *s, int l) {
    if (l < 2 || l > 90) return 0;
    int digits = 1;
    for (int i = 0; i < l; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch <= 32 || ch >= 127 || strchr("\"'`\\<>|;,&()[]{}^!?=", ch)) return 0;
        if (!isdigit(ch)) digits = 0;
        if (sub == CKS_ROUTE || sub == CKS_RPC) { if (!(isalnum(ch) || strchr("/_-.~%@+", ch))) return 0; }
        else if (!(isalnum(ch) || strchr("_.:/-@#$+*", ch))) return 0;
    }
    if (digits) return 0;
    if (sub == CKS_ROUTE) return s[0] == '/' && l >= 2;
    if (sub == CKS_OP) { if (l < 3 || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0; for (int i = 0; i < l; i++) if (s[i] == '/' || s[i] == ':') return 0; }
    if (sub == CKS_FILE) return l >= 4;
    return 1;
}
/* uma chave de contrato (role DEFINE) ou uma referencia a contrato (role CONSUME/PRODUCE: nome de operacao num cliente gerado, arquivo de contrato citado) */
static void ck_cadd(CK *K, int role, int sub, int meth, size_t off, const char *s, int l) {
    CoderFile *f = K->f;
    if (f->nkey >= K->max || !ct_valid(sub, s, l)) return;
    int line = cp_line(K->c, off);
    for (int i = f->nkey - 1; i >= 0 && i >= f->nkey - 24; i--) if (f->key[i].line == line && f->key[i].role == role && f->key[i].sub == sub && f->key[i].meth == meth && (int)strlen(f->key[i].key) == l && !memcmp(f->key[i].key, s, (size_t)l)) return;
    if (f->nkey >= K->cap) { K->cap = K->cap ? K->cap * 2 : 32; f->key = (CoderKey *)realloc(f->key, sizeof(CoderKey) * (size_t)K->cap); }
    CoderKey *k = &f->key[f->nkey++]; k->key = cp_dup(s, (size_t)l); k->line = line; k->role = role; k->cls = CKC_CONTRACT; k->meth = meth; k->col = 0; k->sub = sub;
}
/* valor discriminador declarado num contrato tipado (enum/uniao): a chave e a mesma de uma mensagem (classe msg), entao so os textos validos como chave de mensagem entram */
static void ck_cadd_enum(CK *K, size_t off, const char *s, int l, int disc) {
    if (!ck_valid(CKC_MSG, s, l, disc)) return;
    ck_cadd(K, CKR_DEFINE, CKS_ENUM, 0, off, s, l);
}
/* variantes de caixa de um nome de operacao: o original, camelCase/lowerFirst e snake_case (geradores de cliente trocam a convencao) */
static void ct_case_variants(const char *s, int l, char out[3][100], int *n) {
    *n = 0; if (l <= 0 || l > 90) return;
    memcpy(out[0], s, (size_t)l); out[0][l] = 0; *n = 1;
    char b[100]; int w = 0; int up = 0, lo = 0, us = 0;
    for (int i = 0; i < l; i++) { if (isupper((unsigned char)s[i])) up = 1; if (islower((unsigned char)s[i])) lo = 1; if (s[i] == '_' || s[i] == '-') us = 1; }
    if (us) { int cap = 0; for (int i = 0; i < l && w < 98; i++) { if (s[i] == '_' || s[i] == '-') { cap = 1; continue; } { int first = w == 0; b[w++] = (char)(cap ? toupper((unsigned char)s[i]) : (first ? tolower((unsigned char)s[i]) : s[i])); } cap = 0; } }
    else if (isupper((unsigned char)s[0]) && lo) { memcpy(b, s, (size_t)l); w = l; b[0] = (char)tolower((unsigned char)b[0]); }
    if (w > 0) { b[w] = 0; if (strcmp(b, out[0])) { memcpy(out[*n], b, (size_t)w + 1); (*n)++; } }
    if (up && lo && !us) { w = 0; for (int i = 0; i < l && w < 96; i++) { if (isupper((unsigned char)s[i]) && i > 0 && (islower((unsigned char)s[i - 1]) || isdigit((unsigned char)s[i - 1]))) b[w++] = '_'; b[w++] = (char)tolower((unsigned char)s[i]); } b[w] = 0; if (w > 0 && strcmp(b, out[0]) && (*n < 2 || strcmp(b, out[1]))) { memcpy(out[*n], b, (size_t)w + 1); (*n)++; } }
}
static void ct_ops(CK *K, int role, size_t off, const char *s, int l) {
    char v[3][100]; int n; ct_case_variants(s, l, v, &n);
    for (int i = 0; i < n; i++) ck_cadd(K, role, CKS_OP, 0, off, v[i], (int)strlen(v[i]));
}
static const char *ct_base(const char *p, int l, int *bl) {
    int e = l; for (int i = 0; i < l; i++) if (p[i] == '?' || p[i] == '#') { e = i; break; }
    int b = e; while (b > 0 && p[b - 1] != '/' && p[b - 1] != '\\') b--;
    *bl = e - b; return p + b;
}
/* nome de arquivo de contrato? (.proto, .graphql, .gql, .graphqls, .schema.json, openapi/swagger/asyncapi em yaml/yml/json) */
static int ct_fname(const char *b, int l) {
    if (l < 5 || l > 80) return 0;
    char lo[84]; for (int i = 0; i < l; i++) lo[i] = (char)tolower((unsigned char)b[i]); lo[l] = 0;
    for (int i = 0; i < l; i++) if (!(isalnum((unsigned char)b[i]) || strchr("._-@", b[i]))) return 0;
    if (l > 6 && !strcmp(lo + l - 6, ".proto")) return 1;
    if ((l > 8 && !strcmp(lo + l - 8, ".graphql")) || (l > 4 && !strcmp(lo + l - 4, ".gql")) || (l > 9 && !strcmp(lo + l - 9, ".graphqls"))) return 1;
    if (l > 12 && !strcmp(lo + l - 12, ".schema.json")) return 1;
    int yj = (l > 5 && !strcmp(lo + l - 5, ".yaml")) || (l > 4 && !strcmp(lo + l - 4, ".yml")) || (l > 5 && !strcmp(lo + l - 5, ".json"));
    return yj && (strstr(lo, "openapi") || strstr(lo, "swagger") || strstr(lo, "asyncapi"));
}
/* "/pacote.Servico/Metodo" (caminho gRPC): um ponto no servico e inicial maiuscula no ultimo componente do servico e no metodo */
static int ct_grpc_path(const char *s, int l) {
    if (l < 6 || l > 90 || s[0] != '/') return 0;
    const char *sl = (const char *)memchr(s + 1, '/', (size_t)l - 1); if (!sl || sl - s < 4 || memchr(sl + 1, '/', (size_t)(s + l - sl - 1))) return 0;
    const char *dot = NULL; for (const char *p = s + 1; p < sl; p++) { if (*p == '.') dot = p; else if (!(isalnum((unsigned char)*p) || *p == '_')) return 0; }
    if (!dot || dot + 1 >= sl || !isupper((unsigned char)dot[1]) || !isupper((unsigned char)sl[1])) return 0;
    for (const char *p = sl + 1; p < s + l; p++) if (!(isalnum((unsigned char)*p) || *p == '_')) return 0;
    return 1;
}

/* ---- leitor de estrutura: YAML (por indentacao) e JSON (por chaves) geram os mesmos eventos: caminho de chaves + valor escalar ---- */
static void ct_emit(CtFn fn, void *ctx, const char *const *k, const int *kl, int np, const char *v, int vl, int isval, size_t off) {
    if (np < 1 || np > CT_D) return;
    CtEv e; e.depth = np - 1; for (int i = 0; i < np; i++) { e.k[i] = k[i]; e.kl[i] = kl[i]; } e.v = v; e.vl = vl; e.isval = isval; e.off = off; fn(ctx, &e);
}
static void ct_yaml_walk(const char *t, size_t n, CtFn fn, void *ctx) {
    struct { int ind; const char *k; int kl; } st[64]; int sp = 0; const char *ks[64]; int kls[64];
    int blockskip = -1; long nev = 0;
    for (size_t a = 0; a < n && nev < 40000; ) {
        size_t e = a; while (e < n && t[e] != '\n') e++;
        size_t s = a; int ind = 0; while (s < e && (t[s] == ' ' || t[s] == '\t')) { s++; ind++; }
        size_t le = e; while (le > s && (t[le - 1] == ' ' || t[le - 1] == '\r' || t[le - 1] == '\t')) le--;
        if (blockskip >= 0) { if (s == le || ind > blockskip) { a = e + 1; continue; } blockskip = -1; }
        if (s < le && t[s] != '#' && !(le - s >= 3 && !memcmp(t + s, "---", 3)) && !(le - s >= 3 && !memcmp(t + s, "...", 3))) {
            size_t p = s; int col = ind;
            while (p < le && t[p] == '-' && (p + 1 == le || t[p + 1] == ' ')) {
                int vind = ind + (int)(p - s);
                if (sp > 0 && st[sp - 1].ind == vind && st[sp - 1].k[0] != '-') vind++;          /* "- item" alinhado com a chave pai */
                while (sp > 0 && st[sp - 1].ind >= vind) sp--;
                if (sp < 63) { st[sp].ind = vind; st[sp].k = "-"; st[sp].kl = 1; sp++; }
                p++; while (p < le && t[p] == ' ') p++; col = ind + (int)(p - s);
            }
            if (p < le) {
                size_t kb = p, ke = p, cpos = 0; int ok = 0;
                if (t[p] == '"' || t[p] == '\'') { char q = t[p]; kb = p + 1; ke = kb; while (ke < le && t[ke] != q) ke++; if (ke < le) { cpos = ke + 1; while (cpos < le && t[cpos] == ' ') cpos++; if (cpos < le && t[cpos] == ':' && (cpos + 1 == le || t[cpos + 1] == ' ')) ok = 1; } }
                else if (t[p] != '{' && t[p] != '[') { for (size_t x = p; x < le; x++) if (t[x] == ':' && (x + 1 == le || t[x + 1] == ' ')) { ke = x; cpos = x; ok = 1; break; } else if (t[x] == ' ' && x + 1 < le && t[x + 1] == '#') break; }
                if (ok && ke > kb && ke - kb < 200) {
                    while (sp > 0 && st[sp - 1].ind >= col) sp--;
                    if (sp < 63) { st[sp].ind = col; st[sp].k = t + kb; st[sp].kl = (int)(ke - kb); sp++; }
                    size_t vb = cpos + 1; while (vb < le && t[vb] == ' ') vb++;
                    size_t ve = le; for (size_t x = vb; x + 1 < le; x++) if (t[x] == ' ' && t[x + 1] == '#') { ve = x; while (ve > vb && t[ve - 1] == ' ') ve--; break; }
                    int isval = vb < ve;
                    if (isval && (t[vb] == '"' || t[vb] == '\'') && ve - vb >= 2 && t[ve - 1] == t[vb]) { vb++; ve--; }
                    if (isval && (t[vb] == '|' || t[vb] == '>') && ve - vb <= 3) { blockskip = col; isval = 0; }
                    for (int i = 0; i < sp && i < 64; i++) { ks[i] = st[i].k; kls[i] = st[i].kl; }
                    ct_emit(fn, ctx, ks, kls, sp, t + vb, (int)(ve - vb), isval, kb); nev++;
                }
            }
        }
        a = e + 1;
    }
}
static void ct_json_walk(const char *t, size_t n, CtFn fn, void *ctx) {
    const char *P[CT_D + 2]; int PL[CT_D + 2]; int np = 0;
    struct { char ty; int base; } fr[64]; int nf = 0; long nev = 0; size_t i = 0;
    fr[nf].ty = '{'; fr[nf].base = 0; nf++;
    while (i < n && nev < 40000) {
        char c = t[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == ',') { i++; continue; }
        if (c == '{' || c == '[') {
            int base;
            if (nf > 0 && fr[nf - 1].ty == '[') { base = fr[nf - 1].base; if (base < CT_D) { P[base] = "-"; PL[base] = 1; } np = base + 1; base = np; } else base = np;
            if (nf >= 63) return;
            fr[nf].ty = c; fr[nf].base = base; nf++;
            i++; continue;
        }
        if (c == '}' || c == ']') { if (nf > 1) nf--; np = fr[nf - 1].base; i++; continue; }
        if (c == '"') {
            size_t j = i + 1; while (j < n && t[j] != '"' && j - i < 4000) { if (t[j] == '\\') j++; else if (t[j] == '\n') return; j++; }
            if (j >= n || t[j] != '"' || j - i >= 4000) return;
            size_t k = j + 1; while (k < n && (t[k] == ' ' || t[k] == '\t' || t[k] == '\r' || t[k] == '\n')) k++;
            if (nf > 0 && fr[nf - 1].ty == '{' && k < n && t[k] == ':') {
                int b = fr[nf - 1].base; if (b < CT_D) { P[b] = t + i + 1; PL[b] = (int)(j - i - 1); } np = b + 1;
                k++; while (k < n && (t[k] == ' ' || t[k] == '\t' || t[k] == '\r' || t[k] == '\n')) k++;
                if (k >= n) return;
                if (t[k] == '{' || t[k] == '[') { ct_emit(fn, ctx, P, PL, np, "", 0, 0, i + 1); nev++; i = k; continue; }
                if (t[k] == '"') { size_t j2 = k + 1; while (j2 < n && t[j2] != '"' && j2 - k < 4000) { if (t[j2] == '\\') j2++; else if (t[j2] == '\n') return; j2++; }
                    if (j2 >= n || t[j2] != '"' || j2 - k >= 4000) return;
                    ct_emit(fn, ctx, P, PL, np, t + k + 1, (int)(j2 - k - 1), 1, i + 1); nev++; i = j2 + 1; continue; }
                size_t j2 = k; while (j2 < n && t[j2] != ',' && t[j2] != '}' && t[j2] != ']' && t[j2] != '\n') j2++;
                size_t ve = j2; while (ve > k && (t[ve - 1] == ' ' || t[ve - 1] == '\r' || t[ve - 1] == '\t')) ve--;
                ct_emit(fn, ctx, P, PL, np, t + k, (int)(ve - k), 1, i + 1); nev++; i = j2; continue;
            }
            i = j + 1; continue;
        }
        i++;
    }
}
static void ct_unesc(const char *s, int l, char *o, int cap) { int w = 0; for (int i = 0; i < l && w < cap - 1; i++) { if (s[i] == '\\' && i + 1 < l && (s[i + 1] == '/' || s[i + 1] == '"')) i++; o[w++] = s[i]; } o[w] = 0; }
static int ct_keyis(const CtEv *e, int d, const char *w) { size_t l = strlen(w); return d <= e->depth && (size_t)e->kl[d] == l && !memcmp(e->k[d], w, l); }

/* ---- OpenAPI / Swagger / AsyncAPI / JSON Schema: semantica sobre os eventos ---- */
typedef struct { CK *K; int pass; int has_prefix; int jsch; char prefix[104]; char cpath[200]; } CtS;          /* jsch: so JSON Schema (sem openapi/asyncapi): o `title` de topo e o nome do tipo */
static void ct_set_prefix(CtS *S, const char *v, int vl) {
    char b[200]; ct_unesc(v, vl, b, sizeof b); char *p = b;
    char *h = strstr(p, "://"); if (h) { p = h + 3; while (*p && *p != '/') p++; } else if (p[0] == '/' && p[1] == '/') { p += 2; while (*p && *p != '/') p++; }
    char *q = strpbrk(p, "{?#"); if (q) *q = 0;
    size_t l = strlen(p); while (l > 0 && p[l - 1] == '/') p[--l] = 0;
    if (p[0] == '/' && l >= 2 && l < sizeof S->prefix) { memcpy(S->prefix, p, l + 1); S->has_prefix = 1; }
}
static void ct_route_def(CtS *S, const char *path, int meth, size_t off) {
    char rk[100]; int rl = ck_route_norm(path, (int)strlen(path), rk, sizeof rk, 0);
    if (rl) ck_cadd(S->K, CKR_DEFINE, CKS_ROUTE, meth, off, rk, rl);
    if (S->prefix[0]) { char full[320]; snprintf(full, sizeof full, "%s%s", S->prefix, path); rl = ck_route_norm(full, (int)strlen(full), rk, sizeof rk, 0); if (rl) ck_cadd(S->K, CKR_DEFINE, CKS_ROUTE, meth, off, rk, rl); }
}
/* nome de canal/topico: corta no primeiro parametro ({id}, +, #) e tira separador sobrando */
static void ct_channel(CtS *S, const char *v, int vl, size_t off) {
    char b[200]; ct_unesc(v, vl, b, sizeof b); char *q = strpbrk(b, "{+#"); if (q) *q = 0;
    size_t l = strlen(b); while (l > 0 && (b[l - 1] == '/' || b[l - 1] == '.')) b[--l] = 0;
    if (l >= 2) ck_cadd(S->K, CKR_DEFINE, CKS_MSG, 0, off, b, (int)l);
}
static void ct_sem(void *ctx, const CtEv *e) {
    CtS *S = (CtS *)ctx; const int d = e->depth; CK *K = S->K;
    if (S->pass == 0) {
        if (e->isval && !S->has_prefix) { if (d == 2 && ct_keyis(e, 0, "servers") && ct_keyis(e, 1, "-") && ct_keyis(e, 2, "url")) ct_set_prefix(S, e->v, e->vl); else if (d == 0 && ct_keyis(e, 0, "basePath")) ct_set_prefix(S, e->v, e->vl); }
        return;
    }
    char buf[200];
    if (d == 0 && !e->isval) { }
    if (d == 1 && ct_keyis(e, 0, "paths")) { ct_unesc(e->k[1], e->kl[1], S->cpath, sizeof S->cpath); return; }
    if (d == 2 && ct_keyis(e, 0, "paths") && S->cpath[0] == '/') {
        char m[12]; int ml = e->kl[2] < 11 ? e->kl[2] : 11; memcpy(m, e->k[2], (size_t)ml); m[ml] = 0;
        int meth = ck_meth_of(m); const int ismeth = meth != CKM_NONE || !strcasecmp(m, "trace");
        if (ismeth) ct_route_def(S, S->cpath, meth, e->off);
        return;
    }
    if (d == 3 && e->isval && ct_keyis(e, 0, "paths") && (ct_keyis(e, 3, "operationId"))) { ct_ops(K, CKR_DEFINE, e->off, e->v, e->vl); return; }
    if (d == 2 && ct_keyis(e, 0, "components") && !e->isval) {
        if (ct_keyis(e, 1, "schemas")) { ct_unesc(e->k[2], e->kl[2], buf, sizeof buf); ck_cadd(K, CKR_DEFINE, CKS_SCHEMA, 0, e->off, buf, (int)strlen(buf)); }
        else if (ct_keyis(e, 1, "messages")) { ct_unesc(e->k[2], e->kl[2], buf, sizeof buf); ck_cadd(K, CKR_DEFINE, CKS_MSG, 0, e->off, buf, (int)strlen(buf)); }
        return;
    }
    if (d == 1 && (ct_keyis(e, 0, "definitions") || ct_keyis(e, 0, "$defs")) && !e->isval) { ct_unesc(e->k[1], e->kl[1], buf, sizeof buf); ck_cadd(K, CKR_DEFINE, CKS_SCHEMA, 0, e->off, buf, (int)strlen(buf)); return; }
    if (d >= 2 && ct_keyis(e, d - 1, "mapping") && ct_keyis(e, d - 2, "discriminator")) { ct_unesc(e->k[d], e->kl[d], buf, sizeof buf); ck_cadd_enum(K, e->off, buf, (int)strlen(buf), 1); return; }
    if (d == 1 && ct_keyis(e, 0, "channels") && !e->isval) { ct_channel(S, e->k[1], e->kl[1], e->off); return; }
    if (d == 2 && e->isval && ct_keyis(e, 0, "channels") && ct_keyis(e, 2, "address")) { ct_channel(S, e->v, e->vl, e->off); return; }
    if (d == 3 && e->isval && ct_keyis(e, 0, "channels") && ct_keyis(e, 3, "operationId") && (ct_keyis(e, 2, "publish") || ct_keyis(e, 2, "subscribe"))) { ct_ops(K, CKR_DEFINE, e->off, e->v, e->vl); return; }
    if (d == 1 && ct_keyis(e, 0, "operations") && !e->isval) { ct_unesc(e->k[1], e->kl[1], buf, sizeof buf); ct_ops(K, CKR_DEFINE, e->off, buf, (int)strlen(buf)); return; }
    if (e->isval && d >= 2 && (ct_keyis(e, d, "name") || ct_keyis(e, d, "messageId")) && ((ct_keyis(e, d - 1, "message") && !ct_keyis(e, 0, "paths")) || (d == 3 && ct_keyis(e, 0, "components") && ct_keyis(e, 1, "messages")))) { ct_unesc(e->v, e->vl, buf, sizeof buf); ck_cadd(K, CKR_DEFINE, CKS_MSG, 0, e->off, buf, (int)strlen(buf)); return; }
    if (d == 0 && e->isval && S->jsch && ct_keyis(e, 0, "title")) { ct_unesc(e->v, e->vl, buf, sizeof buf); ck_cadd(K, CKR_DEFINE, CKS_SCHEMA, 0, e->off, buf, (int)strlen(buf)); return; }
    if (d == 0 && e->isval && ct_keyis(e, 0, "$id")) { ct_unesc(e->v, e->vl, buf, sizeof buf); ck_cadd(K, CKR_DEFINE, CKS_SCHEMA, 0, e->off, buf, (int)strlen(buf)); int bl; const char *b = ct_base(buf, (int)strlen(buf), &bl); if (bl >= 4 && bl < (int)strlen(buf)) ck_cadd(K, CKR_DEFINE, CKS_FILE, 0, e->off, b, bl); return; }
    if (e->isval && ct_keyis(e, d, "$ref") && e->vl > 0 && e->v[0] != '#') {          /* referencia a outro arquivo de contrato: o nome do arquivo */
        ct_unesc(e->v, e->vl, buf, sizeof buf); char *hs = strchr(buf, '#'); if (hs) *hs = 0;
        int bl; const char *b = ct_base(buf, (int)strlen(buf), &bl); if (bl >= 4 && strchr(b, '.')) ck_cadd(K, CKR_CONSUME, CKS_FILE, 0, e->off, b, bl);
    }
}
/* o que o texto parece (so olha o comeco): bits CT_* */
static int ct_kind_of(const char *path, const char *t, size_t n) {
    size_t m = n < 6000 ? n : 6000; int k = 0;
    for (size_t i = 0; i < m; i++) {
        if (t[i] == '"' || t[i] == '\'' || i == 0 || t[i - 1] == '\n') {
            size_t j = i + ((t[i] == '"' || t[i] == '\'') ? 1 : 0);
            if (j + 8 <= m && !strncmp(t + j, "openapi", 7) && (t[j + 7] == ':' || t[j + 7] == '"' || t[j + 7] == '\'')) k |= CT_OAS;
            else if (j + 8 <= m && !strncmp(t + j, "swagger", 7) && (t[j + 7] == ':' || t[j + 7] == '"' || t[j + 7] == '\'')) k |= CT_OAS;
            else if (j + 9 <= m && !strncmp(t + j, "asyncapi", 8) && (t[j + 8] == ':' || t[j + 8] == '"' || t[j + 8] == '\'')) k |= CT_ASY;
            else if (j + 8 <= m && (!strncmp(t + j, "$schema", 7) || !strncmp(t + j, "$id", 3)) && (t[j + 7] == ':' || t[j + 7] == '"' || t[j + 3] == '"' || t[j + 3] == ':' || t[j + 3] == '\'' || t[j + 7] == '\'')) k |= CT_JSCH;
        }
    }
    int bl; const char *b = ct_base(path, (int)strlen(path), &bl);
    if (bl > 12 && !strncasecmp(b + bl - 12, ".schema.json", 12)) k |= CT_JSCH;
    return k;
}
static int ck_json_is_contract(const char *t, size_t n) {
    if (n > 6u << 20) return 0;
    size_t i = 0; while (i < n && isspace((unsigned char)t[i])) i++;
    if (i >= n || t[i] != '{') return 0;
    return ct_kind_of("", t, n) != 0;
}
static void ck_contract_doc(CK *K, int kind) {
    CtS S; memset(&S, 0, sizeof S); S.K = K; const int json = K->c->lang == CL_JSON; S.jsch = (kind & CT_JSCH) && !(kind & (CT_OAS | CT_ASY));
    for (S.pass = 0; S.pass < 2; S.pass++) { S.cpath[0] = 0; if (json) ct_json_walk(K->c->t, K->c->n, ct_sem, &S); else ct_yaml_walk(K->c->t, K->c->n, ct_sem, &S); }
}

/* ---- Protocol Buffers ---- */
static size_t ct_skipws(const char *t, size_t n, size_t i) {
    while (i < n) {
        if (isspace((unsigned char)t[i])) i++;
        else if (t[i] == '/' && i + 1 < n && t[i + 1] == '/') { while (i < n && t[i] != '\n') i++; }
        else if (t[i] == '/' && i + 1 < n && t[i + 1] == '*') { i += 2; while (i + 1 < n && !(t[i] == '*' && t[i + 1] == '/')) i++; i += 2; }
        else break;
    }
    return i < n ? i : n;
}
static int ct_word(const char *t, size_t n, size_t i, size_t *e) { size_t j = i; while (j < n && (ct_idc((unsigned char)t[j]) || t[j] == '.')) j++; *e = j; return (int)(j - i); }
static void ct_rpc_def(CK *K, const char *pkg, const char *svc, const char *m, int ml, size_t off) {
    char key[100];
    if (pkg[0]) snprintf(key, sizeof key, "/%s.%s/%.*s", pkg, svc, ml, m); else snprintf(key, sizeof key, "/%s/%.*s", svc, ml, m);
    ck_cadd(K, CKR_DEFINE, CKS_RPC, 0, off, key, (int)strlen(key));
    if (pkg[0]) { snprintf(key, sizeof key, "%s/%.*s", svc, ml, m); ck_cadd(K, CKR_DEFINE, CKS_RPC, 0, off, key, (int)strlen(key)); }
    if (ml >= 4) ct_ops(K, CKR_DEFINE, off, m, ml);
}
static void ck_proto(CK *K) {
    const char *t = K->c->t; size_t n = K->c->n, i = 0; char pkg[80] = "", svc[60] = ""; int depth = 0, svcdepth = -1;
    while (i < n && K->f->nkey < K->max) {
        i = ct_skipws(t, n, i); if (i >= n) break;
        char c = t[i];
        if (c == '"' || c == '\'') { i++; while (i < n && t[i] != c && t[i] != '\n') { if (t[i] == '\\') i++; i++; } i++; continue; }
        if (c == '{') { depth++; i++; continue; }
        if (c == '}') { depth--; if (svcdepth >= 0 && depth < svcdepth) svcdepth = -1; i++; continue; }
        if (!(isalpha((unsigned char)c) || c == '_')) { i++; continue; }
        size_t we; int wl = ct_word(t, n, i, &we); char w[24]; int wc = wl < 23 ? wl : 23; memcpy(w, t + i, (size_t)wc); w[wc] = 0; size_t wpos = i; i = we;
        if (!strcmp(w, "package")) { size_t b = ct_skipws(t, n, i), e; int l = ct_word(t, n, b, &e); if (l > 0 && l < 79) { memcpy(pkg, t + b, (size_t)l); pkg[l] = 0; } i = e; }
        else if (!strcmp(w, "import")) { size_t b = ct_skipws(t, n, i), e; int l = ct_word(t, n, b, &e); if (l == 6 || l == 4) { if (!strncmp(t + b, "public", 6) || !strncmp(t + b, "weak", 4)) b = ct_skipws(t, n, e); }
            if (b < n && (t[b] == '"' || t[b] == '\'')) { size_t x = b + 1; while (x < n && t[x] != t[b] && t[x] != '\n') x++; if (x < n && t[x] == t[b]) { int bl; const char *bn = ct_base(t + b + 1, (int)(x - b - 1), &bl); if (ct_fname(bn, bl)) ck_cadd(K, CKR_CONSUME, CKS_FILE, 0, b + 1, bn, bl); i = x + 1; } } }
        else if (!strcmp(w, "service")) { size_t b = ct_skipws(t, n, i), e; int l = ct_word(t, n, b, &e); if (l > 0 && l < 59) { memcpy(svc, t + b, (size_t)l); svc[l] = 0; size_t o = ct_skipws(t, n, e); if (o < n && t[o] == '{') { depth++; svcdepth = depth; i = o + 1; } else i = e; } }
        else if (!strcmp(w, "rpc") && svcdepth >= 0 && depth == svcdepth) {
            size_t b = ct_skipws(t, n, i), e; int l = ct_word(t, n, b, &e); if (l <= 0) continue; char m[80]; int ml = l < 79 ? l : 79; memcpy(m, t + b, (size_t)ml); m[ml] = 0;
            size_t x = ct_skipws(t, n, e); int par = 0; while (x < n && par < 4 && t[x] != '{' && t[x] != ';') { if (t[x] == '(') par++; else if (t[x] == ')') par++; x++; }
            ct_rpc_def(K, pkg, svc, m, ml, b);
            x = ct_skipws(t, n, x);
            if (x < n && t[x] == '{') {          /* corpo do rpc: option (google.api.http) = { get: "/v1/x" ... } */
                size_t y = x + 1; int d2 = 1;
                while (y < n && d2 > 0 && K->f->nkey < K->max) {
                    y = ct_skipws(t, n, y); if (y >= n) break;
                    if (t[y] == '{') { d2++; y++; } else if (t[y] == '}') { d2--; y++; }
                    else if (t[y] == '"' || t[y] == '\'') { char q = t[y]; y++; while (y < n && t[y] != q && t[y] != '\n') y++; y++; }
                    else if (isalpha((unsigned char)t[y])) { size_t ye; int yl = ct_word(t, n, y, &ye); int meth = yl <= 6 ? ck_meth_word(t + y, yl) : CKM_NONE; size_t z = ct_skipws(t, n, ye);
                        if (meth && meth != CKM_OTHER && z < n && t[z] == ':') { z = ct_skipws(t, n, z + 1); if (z < n && (t[z] == '"' || t[z] == '\'')) { size_t q = z + 1; while (q < n && t[q] != t[z] && t[q] != '\n') q++; char rk[100]; int rl = ck_route_norm(t + z + 1, (int)(q - z - 1), rk, sizeof rk, 0); if (rl) ck_cadd(K, CKR_DEFINE, CKS_ROUTE, meth, z + 1, rk, rl); y = q + 1; continue; } }
                        y = ye; }
                    else y++;
                }
                i = y;
            } else i = x;
        }
        else if (!strcmp(w, "message") && svcdepth < 0) {
            size_t b = ct_skipws(t, n, i), e; int l = ct_word(t, n, b, &e);
            if (l >= 3 && l < 60 && isupper((unsigned char)t[b]) && !memchr(t + b, '.', (size_t)l)) { size_t o = ct_skipws(t, n, e); if (o < n && t[o] == '{') { char nm[100]; memcpy(nm, t + b, (size_t)l); nm[l] = 0; ck_cadd(K, CKR_DEFINE, CKS_MSG, 0, b, nm, l); if (pkg[0]) { char q[160]; snprintf(q, sizeof q, "%s.%s", pkg, nm); ck_cadd(K, CKR_DEFINE, CKS_MSG, 0, b, q, (int)strlen(q)); } } }
        }
        (void)wpos;
    }
}

/* ---- GraphQL: esquema (campos de Query/Mutation/Subscription) e operacoes de cliente (campos da raiz) ---- */
static size_t ct_gql_ws(const char *t, size_t n, size_t i) {
    while (i < n) {
        if (isspace((unsigned char)t[i]) || t[i] == ',') i++;
        else if (t[i] == '#') { while (i < n && t[i] != '\n') i++; }
        else if (t[i] == '"') { if (i + 2 < n && t[i + 1] == '"' && t[i + 2] == '"') { i += 3; while (i + 2 < n && !(t[i] == '"' && t[i + 1] == '"' && t[i + 2] == '"')) i++; i += 3; } else { i++; while (i < n && t[i] != '"' && t[i] != '\n') { if (t[i] == '\\') i++; i++; } i++; } }
        else break;
    }
    return i < n ? i : n;
}
static size_t ct_gql_bal(const char *t, size_t n, size_t i, char op, char cl) {          /* t[i] == op; devolve a posicao depois do fechamento */
    int d = 0;
    while (i < n) {
        char c = t[i];
        if (c == '"') { size_t j = ct_gql_ws(t, n, i); if (j > i) { i = j; continue; } i++; continue; }
        if (c == '#') { while (i < n && t[i] != '\n') i++; continue; }
        if (c == op) d++; else if (c == cl && --d == 0) return i + 1;
        i++;
    }
    return n;
}
static const char *const CT_GQL_ROOT[] = {"Query", "Mutation", "Subscription"};
/* selecao `{ ... }` na raiz de uma operacao de cliente: um CONSUME por campo ("Query.orders") */
static size_t ct_gql_sel(CK *K, const char *t, size_t n, size_t i, size_t off0, const char *root) {
    i++;
    while (i < n && K->f->nkey < K->max) {
        i = ct_gql_ws(t, n, i); if (i >= n) break;
        char c = t[i];
        if (c == '}') return i + 1;
        if (c == '.') { while (i < n && t[i] == '.') i++; i = ct_gql_ws(t, n, i); size_t e; int l = ct_word(t, n, i, &e); if (l == 2 && !strncmp(t + i, "on", 2)) { i = ct_gql_ws(t, n, e); l = ct_word(t, n, i, &e); } i = e; i = ct_gql_ws(t, n, i); while (i < n && t[i] == '@') { size_t x; ct_word(t, n, i + 1, &x); i = ct_gql_ws(t, n, x); if (i < n && t[i] == '(') i = ct_gql_ws(t, n, ct_gql_bal(t, n, i, '(', ')')); } if (i < n && t[i] == '{') i = ct_gql_bal(t, n, i, '{', '}'); continue; }
        if (!(isalpha((unsigned char)c) || c == '_')) { i++; continue; }
        size_t e; int l = ct_word(t, n, i, &e); size_t fb = i; int fl = l; size_t x = ct_gql_ws(t, n, e);
        if (x < n && t[x] == ':') { x = ct_gql_ws(t, n, x + 1); size_t e2; int l2 = ct_word(t, n, x, &e2); if (l2 > 0) { fb = x; fl = l2; e = e2; } }
        char key[100]; if (fl > 0 && fl < 80 && !memchr(t + fb, '.', (size_t)fl)) { snprintf(key, sizeof key, "%s.%.*s", root, fl, t + fb); ck_cadd(K, CKR_CONSUME, CKS_GQL, 0, off0 + fb, key, (int)strlen(key)); }
        i = ct_gql_ws(t, n, e);
        if (i < n && t[i] == '(') i = ct_gql_ws(t, n, ct_gql_bal(t, n, i, '(', ')'));
        while (i < n && t[i] == '@') { size_t y; ct_word(t, n, i + 1, &y); i = ct_gql_ws(t, n, y); if (i < n && t[i] == '(') i = ct_gql_ws(t, n, ct_gql_bal(t, n, i, '(', ')')); }
        if (i < n && t[i] == '{') i = ct_gql_bal(t, n, i, '{', '}');
    }
    return n;
}
/* corpo `{ ... }` de um tipo raiz do esquema: um DEFINE por campo ("Query.orders") */
static size_t ct_gql_fields(CK *K, const char *t, size_t n, size_t i, size_t off0, const char *root) {
    i++;
    while (i < n && K->f->nkey < K->max) {
        i = ct_gql_ws(t, n, i); if (i >= n) break;
        char c = t[i];
        if (c == '}') return i + 1;
        if (c == '@') { size_t y; ct_word(t, n, i + 1, &y); i = ct_gql_ws(t, n, y); if (i < n && t[i] == '(') i = ct_gql_bal(t, n, i, '(', ')'); continue; }
        if (!(isalpha((unsigned char)c) || c == '_')) { i++; continue; }
        size_t e; int l = ct_word(t, n, i, &e); size_t x = ct_gql_ws(t, n, e);
        if (x < n && (t[x] == '(' || t[x] == ':')) {
            if (l < 80 && !memchr(t + i, '.', (size_t)l)) { char key[100]; snprintf(key, sizeof key, "%s.%.*s", root, l, t + i); ck_cadd(K, CKR_DEFINE, CKS_GQL, 0, off0 + i, key, (int)strlen(key)); }
            if (t[x] == '(') x = ct_gql_ws(t, n, ct_gql_bal(t, n, x, '(', ')'));
            if (x < n && t[x] == ':') { x++; x = ct_gql_ws(t, n, x); while (x < n && (ct_idc((unsigned char)t[x]) || t[x] == '[' || t[x] == ']' || t[x] == '!')) x++; }
            i = x; continue;
        }
        i = e;
    }
    return n;
}
/* documento GraphQL: esquema (schema_ok) e/ou operacoes de cliente. rootmap: nome do tipo raiz -> 0 Query, 1 Mutation, 2 Subscription */
static void ck_gql_parse(CK *K, const char *t, size_t n, size_t off0, int schema_ok) {
    char rootname[3][48] = {"Query", "Mutation", "Subscription"}; size_t i = 0; int first = 1;
    if (schema_ok) {          /* schema { query: RootQuery ... } antes dos tipos (ou depois: a leitura abaixo e so uma passada, entao so vale se vier antes) */
        for (size_t p = 0; p + 8 < n && p < 200000; p++) if ((p == 0 || !ct_idc((unsigned char)t[p - 1])) && !strncmp(t + p, "schema", 6) && !ct_idc((unsigned char)t[p + 6])) {
            size_t b = ct_gql_ws(t, n, p + 6); if (b >= n || t[b] != '{') continue; size_t e = ct_gql_bal(t, n, b, '{', '}'); size_t q = b + 1;
            while (q < e) { size_t we; int l = ct_word(t, n, q, &we); if (l > 0) { int ri = l == 5 && !strncmp(t + q, "query", 5) ? 0 : l == 8 && !strncmp(t + q, "mutation", 8) ? 1 : l == 12 && !strncmp(t + q, "subscription", 12) ? 2 : -1;
                    if (ri >= 0) { size_t c = ct_gql_ws(t, n, we); if (c < e && t[c] == ':') { c = ct_gql_ws(t, n, c + 1); size_t ce; int cl = ct_word(t, n, c, &ce); if (cl > 0 && cl < 47) { memcpy(rootname[ri], t + c, (size_t)cl); rootname[ri][cl] = 0; } we = ce; } } q = we; } else q++; }
            break; }
    }
    while (i < n && K->f->nkey < K->max) {
        i = ct_gql_ws(t, n, i); if (i >= n) break;
        char c = t[i];
        if (c == '{') { if (schema_ok || first) i = ct_gql_sel(K, t, n, i, off0, "Query"); else i = ct_gql_bal(t, n, i, '{', '}'); first = 0; continue; }          /* `{ ... }` sem palavra: consulta anonima (so no inicio de uma string de codigo; em arquivo .graphql, em qualquer lugar) */
        if (!(isalpha((unsigned char)c) || c == '_')) { i++; continue; }
        first = 0; size_t e; int l = ct_word(t, n, i, &e); char w[24]; int wc = l < 23 ? l : 23; memcpy(w, t + i, (size_t)wc); w[wc] = 0; i = e;
        int op = !strcmp(w, "query") ? 0 : !strcmp(w, "mutation") ? 1 : !strcmp(w, "subscription") ? 2 : -1;
        if (op >= 0) {
            size_t x = ct_gql_ws(t, n, i); size_t ne; if (x < n && isalpha((unsigned char)t[x])) { ct_word(t, n, x, &ne); x = ct_gql_ws(t, n, ne); }
            if (x < n && t[x] == '(') x = ct_gql_ws(t, n, ct_gql_bal(t, n, x, '(', ')'));
            while (x < n && t[x] == '@') { size_t y; ct_word(t, n, x + 1, &y); x = ct_gql_ws(t, n, y); if (x < n && t[x] == '(') x = ct_gql_ws(t, n, ct_gql_bal(t, n, x, '(', ')')); }
            if (x < n && t[x] == '{') { i = ct_gql_sel(K, t, n, x, off0, CT_GQL_ROOT[op]); continue; }
            i = x; continue;          /* "query failed: {x}" e prosa, nao uma operacao */
        }
        if (schema_ok && (!strcmp(w, "type") || !strcmp(w, "extend"))) {
            size_t x = ct_gql_ws(t, n, i);
            if (!strcmp(w, "extend")) { size_t e2; int l2 = ct_word(t, n, x, &e2); if (l2 == 4 && !strncmp(t + x, "type", 4)) x = ct_gql_ws(t, n, e2); else continue; }
            size_t ne; int nl = ct_word(t, n, x, &ne); if (nl <= 0) continue;
            int ri = -1; for (int r = 0; r < 3; r++) if ((int)strlen(rootname[r]) == nl && !strncmp(t + x, rootname[r], (size_t)nl)) ri = r;
            if (ri < 0) for (int r = 0; r < 3; r++) if ((int)strlen(CT_GQL_ROOT[r]) == nl && !strncmp(t + x, CT_GQL_ROOT[r], (size_t)nl)) ri = r;
            size_t y = ct_gql_ws(t, n, ne);
            while (y < n && t[y] != '{' && t[y] != '\n' && y - ne < 400) y++;           /* implements A & B @dir */
            y = ct_gql_ws(t, n, y);
            if (y < n && t[y] == '{') { i = ri >= 0 ? ct_gql_fields(K, t, n, y, off0, CT_GQL_ROOT[ri]) : ct_gql_bal(t, n, y, '{', '}'); }
            else i = ne;
            continue;
        }
        if (!strcmp(w, "fragment") || !strcmp(w, "input") || !strcmp(w, "interface") || !strcmp(w, "enum") || !strcmp(w, "schema")) { size_t y = i; while (y < n && t[y] != '{' && y - i < 400) y++; if (y < n) i = ct_gql_bal(t, n, y, '{', '}'); continue; }
    }
}
/* uma string de codigo e uma operacao GraphQL de cliente? (comeca por query/mutation/subscription, ou `{` quando vem de uma marca gql/graphql) */
static void ck_gql_try(CK *K, size_t cb, size_t ce) {
    const char *t = K->c->t; if (ce - cb < 12 || ce - cb > 4000) return;
    size_t i = cb; while (i < ce && isspace((unsigned char)t[i])) i++;
    int tagged = 0; { size_t b = cb > 0 ? cb - 1 : 0; while (b > 0 && (t[b] == '`' || t[b] == '"' || t[b] == '\'' || t[b] == '(' || isspace((unsigned char)t[b]))) b--; if (b >= 2 && ((!strncmp(t + b - 2, "gql", 3)) || (b >= 6 && !strncmp(t + b - 6, "graphql", 7)))) tagged = 1; }
    int ok = 0;
    if ((ce - i > 6 && !strncmp(t + i, "query", 5) && !ct_idc((unsigned char)t[i + 5])) || (ce - i > 9 && !strncmp(t + i, "mutation", 8) && !ct_idc((unsigned char)t[i + 8])) || (ce - i > 13 && !strncmp(t + i, "subscription", 12) && !ct_idc((unsigned char)t[i + 12]))) ok = 1;
    else if (tagged && t[i] == '{') ok = 1;
    if (!ok || !memchr(t + i, '{', ce - i)) return;
    ck_gql_parse(K, t + cb, ce - cb, cb, 0);
}

/* ---- declaracoes tipadas de mensagens: enum e uniao de literais (TypeScript), Enum/Literal (Python), const tipada (Go) ---- */
static int ct_discname(const char *nm, int l) {          /* o nome do tipo diz que e um discriminador? */
    char b[64]; int w = 0; for (int i = 0; i < l && w < 62; i++) b[w++] = (char)tolower((unsigned char)nm[i]); b[w] = 0;
    static const char *const W[] = {"type", "kind", "event", "action", "message", "msg", "command", "cmd", "topic", "channel", "signal", "intent", "opcode", "verb", NULL};
    for (int i = 0; W[i]; i++) if (strstr(b, W[i])) return 1;
    size_t bl = strlen(b); if ((bl >= 2 && !strcmp(b + bl - 2, "op")) || (bl >= 6 && !strcmp(b + bl - 6, "method"))) return 1;
    return 0;
}
static int ct_composite(const char *s, int l) { for (int i = 0; i < l; i++) if (strchr("._:/-", s[i])) return 1; return 0; }
static size_t ct_str_at(const char *t, size_t n, size_t i, size_t *b, size_t *e) {          /* literal '..' ou ".." em t[i]; devolve a posicao depois do fechamento, 0 se nao for */
    if (i >= n || (t[i] != '"' && t[i] != '\'')) return 0;
    char q = t[i]; size_t j = i + 1; while (j < n && t[j] != q && t[j] != '\n' && j - i < 200) { if (t[j] == '\\') j++; j++; }
    if (j >= n || t[j] != q) { return 0; }
    *b = i + 1; *e = j; return j + 1;
}
/* corpo de interface/tipo-objeto: campos discriminadores com tipo literal (`type: "a" | "b";`) */
static void ct_ts_disc_body(CK *K, const char *t, size_t n, size_t i, size_t end) {
    int d = 0;
    while (i < end && K->f->nkey < K->max) {
        char c = t[i];
        if (c == '{' || c == '(' || c == '[') { d++; i++; continue; }
        if (c == '}' || c == ')' || c == ']') { d--; i++; continue; }
        if (c == '"' || c == '\'' || c == '`') { size_t b, e, x = ct_str_at(t, n, i, &b, &e); if (x) i = x; else i++; continue; }
        if (d == 0 && (isalpha((unsigned char)c) || c == '_') && (i == 0 || !ct_idc((unsigned char)t[i - 1]))) {
            size_t e; int l = ct_word(t, n, i, &e); size_t x = e; while (x < end && t[x] == ' ') x++; if (x < end && t[x] == '?') x++; while (x < end && t[x] == ' ') x++;
            if (x < end && t[x] == ':' && l < 40) { char nm[48]; memcpy(nm, t + i, (size_t)l); nm[l] = 0;
                if (ck_fieldclass(nm) == CKF_DISC) { x++; for (;;) { while (x < end && (t[x] == ' ' || t[x] == '\t' || t[x] == '\n' || t[x] == '|')) x++; size_t b, e2, y = ct_str_at(t, n, x, &b, &e2); if (!y) break; ck_cadd_enum(K, b, t + b, (int)(e2 - b), 1); x = y; } e = x; } }
            i = e > i ? e : i + 1; continue;
        }
        i++;
    }
}
static void ck_typed_ts(CK *K) {
    const char *t = K->c->t; size_t n = K->c->n, i = 0;
    while (i < n && K->f->nkey < K->max) {
        char c = t[i];
        if (c == '"' || c == '\'') { size_t b, e, x = ct_str_at(t, n, i, &b, &e); i = x ? x : i + 1; continue; }
        if (c == '`') { i++; while (i < n && t[i] != '`') { if (t[i] == '\\') i++; i++; } i++; continue; }
        if (!((isalpha((unsigned char)c) || c == '_') && (i == 0 || (!ct_idc((unsigned char)t[i - 1]) && t[i - 1] != '.')))) { i++; continue; }
        size_t we; int wl = ct_word(t, n, i, &we); const char *w = t + i; i = we;
        const int isenum = wl == 4 && !memcmp(w, "enum", 4), istype = wl == 4 && !memcmp(w, "type", 4), isif = wl == 9 && !memcmp(w, "interface", 9);
        if (!isenum && !istype && !isif) continue;
        size_t b = i; while (b < n && (t[b] == ' ' || t[b] == '\t')) b++;
        size_t ne; int nl = ct_word(t, n, b, &ne); if (nl <= 0 || nl > 60 || !(isalpha((unsigned char)t[b]) || t[b] == '_')) continue;
        const int disc = ct_discname(t + b, nl); size_t x = ne;
        if (isenum || isif) {
            while (x < n && t[x] != '{' && t[x] != '\n' && x - ne < 300) x++;
            if (x >= n || t[x] != '{') continue;
            size_t be = x, d = 0; for (be = x; be < n && be - x < 60000; be++) { if (t[be] == '{') d++; else if (t[be] == '}' && --d == 0) break; else if (t[be] == '"' || t[be] == '\'') { size_t bb, ee, y = ct_str_at(t, n, be, &bb, &ee); if (y) be = y - 1; } }
            if (isif) ct_ts_disc_body(K, t, n, x + 1, be);
            else for (size_t p = x + 1; p < be; p++) { if (t[p] == '=') { size_t q = p + 1; while (q < be && isspace((unsigned char)t[q])) q++; size_t bb, ee, y = ct_str_at(t, n, q, &bb, &ee); if (y) { if (disc || ct_composite(t + bb, (int)(ee - bb))) ck_cadd_enum(K, bb, t + bb, (int)(ee - bb), disc); p = y - 1; } } }
            i = be < n ? be : n; continue;
        }
        /* type Nome<...> = "a" | "b" | { type: "c" } */
        if (x < n && t[x] == '<') { int d = 0; while (x < n && x - ne < 300) { if (t[x] == '<') d++; else if (t[x] == '>' && --d == 0) { x++; break; } x++; } }
        while (x < n && isspace((unsigned char)t[x])) x++;
        if (x >= n || t[x] != '=') continue;
        x++; size_t lits_b[40], lits_e[40]; int nlit = 0, nalt = 0, strict = 1;
        for (;;) {
            while (x < n && (isspace((unsigned char)t[x]) || t[x] == '|')) x++;
            if (x >= n) break;
            size_t bb, ee, y = ct_str_at(t, n, x, &bb, &ee);
            if (y) { if (nlit < 40) { lits_b[nlit] = bb; lits_e[nlit] = ee; nlit++; } nalt++; x = y; }
            else if (t[x] == '{') { size_t be = x; int d = 0; for (be = x; be < n && be - x < 20000; be++) { if (t[be] == '{') d++; else if (t[be] == '}' && --d == 0) break; else if (t[be] == '"' || t[be] == '\'') { size_t b2, e2, y2 = ct_str_at(t, n, be, &b2, &e2); if (y2) be = y2 - 1; } }
                ct_ts_disc_body(K, t, n, x + 1, be); strict = 0; nalt++; x = be + 1; }
            else { strict = 0; nalt++; while (x < n && t[x] != '|' && t[x] != ';' && t[x] != '\n') x++; }
            size_t z = x; while (z < n && (t[z] == ' ' || t[z] == '\t')) z++;
            size_t z2 = z; while (z2 < n && isspace((unsigned char)t[z2])) z2++;
            if (z < n && t[z] == '|') { x = z; continue; }
            if (z2 < n && t[z2] == '|' ) { x = z2; continue; }
            break;
        }
        if (nlit >= 2 && strict) for (int k = 0; k < nlit; k++) if (disc || ct_composite(t + lits_b[k], (int)(lits_e[k] - lits_b[k]))) ck_cadd_enum(K, lits_b[k], t + lits_b[k], (int)(lits_e[k] - lits_b[k]), disc);
        i = x > i ? x : i;
    }
}
static void ck_typed_py(CK *K) {
    const char *t = K->c->t; size_t n = K->c->n; int encl = -1; int encl_disc = 0;
    for (size_t a = 0; a < n && K->f->nkey < K->max; ) {
        size_t e = a; while (e < n && t[e] != '\n') e++;
        size_t s = a; int ind = 0; while (s < e && (t[s] == ' ' || t[s] == '\t')) { s++; ind++; }
        if (s < e && t[s] != '#') {
            if (encl >= 0 && ind <= encl) encl = -1;
            size_t we; int wl = ct_word(t, n, s, &we);
            if (wl == 5 && !memcmp(t + s, "class", 5) && we < e) {
                size_t b = we; while (b < e && t[b] == ' ') b++; size_t ne; int nl = ct_word(t, n, b, &ne);
                if (nl > 0 && ne < e && t[ne] == '(') { size_t cl = ne; while (cl < e && t[cl] != ')') cl++; char bases[160]; int bl = (int)(cl - ne) < 159 ? (int)(cl - ne) : 159; memcpy(bases, t + ne, (size_t)bl); bases[bl] = 0;
                    if (strstr(bases, "Enum")) { encl = ind; encl_disc = ct_discname(t + b, nl); } }
            } else if (encl >= 0 && wl > 0 && ind > encl) {
                size_t x = we; while (x < e && t[x] == ' ') x++; if (x < e && t[x] == ':') { x++; while (x < e && t[x] != '=') x++; } else if (!(x < e && t[x] == '=')) x = e;
                if (x < e && t[x] == '=' && !(x + 1 < e && t[x + 1] == '=')) { x++; while (x < e && t[x] == ' ') x++; size_t bb, ee, y = ct_str_at(t, n, x, &bb, &ee); if (y && y <= e + 1 && (encl_disc || ct_composite(t + bb, (int)(ee - bb)))) ck_cadd_enum(K, bb, t + bb, (int)(ee - bb), encl_disc); }
            } else if (wl > 0) {
                /* Nome = Literal["a", "b"]  /  Nome: TypeAlias = Literal[...]  /  campo: Literal["a"] com campo discriminador */
                const char *lit = NULL; for (size_t x = we; x + 8 < e; x++) if (!memcmp(t + x, "Literal[", 8) && (x == 0 || !ct_idc((unsigned char)t[x - 1]))) { lit = t + x + 8; break; }
                if (lit) {
                    char nm[64]; int nl2 = wl < 63 ? wl : 63; memcpy(nm, t + s, (size_t)nl2); nm[nl2] = 0;
                    const char *eq = (const char *)memchr(t + we, '=', (size_t)(lit - (t + we))); const int alias = eq != NULL;
                    const int isdisc = alias ? ct_discname(nm, nl2) : ck_fieldclass(nm) == CKF_DISC;
                    size_t x = (size_t)(lit - t); size_t endl = x; int d = 1; while (endl < n && endl - x < 600 && d > 0) { if (t[endl] == '[') d++; else if (t[endl] == ']') d--; endl++; }
                    int nlit = 0; for (size_t p = x; p < endl; p++) { size_t bb, ee, y = ct_str_at(t, n, p, &bb, &ee); if (y) { nlit++; p = y - 1; } }
                    for (size_t p = x; p < endl; p++) { size_t bb, ee, y = ct_str_at(t, n, p, &bb, &ee); if (y) { if (isdisc) ck_cadd_enum(K, bb, t + bb, (int)(ee - bb), 1); else if (alias && nlit >= 2 && ct_composite(t + bb, (int)(ee - bb))) ck_cadd_enum(K, bb, t + bb, (int)(ee - bb), 0); p = y - 1; } }
                }
            }
        }
        a = e + 1;
    }
}
static void ck_typed_go(CK *K) {
    const char *t = K->c->t; size_t n = K->c->n;
    for (size_t a = 0; a < n && K->f->nkey < K->max; ) {
        size_t e = a; while (e < n && t[e] != '\n') e++;
        size_t s = a; while (s < e && (t[s] == ' ' || t[s] == '\t')) s++;
        if (e - s > 8) {
            size_t p = s; if (!strncmp(t + p, "const ", 6)) { p += 6; while (p < e && t[p] == ' ') p++; }
            size_t ne; int nl = ct_word(t, n, p, &ne);
            if (nl > 0 && ne < e) { size_t q = ne; while (q < e && t[q] == ' ') q++; size_t te; int tl = ct_word(t, n, q, &te);
                if (tl > 0 && tl < 40 && isalpha((unsigned char)t[q]) && ct_discname(t + q, tl)) { size_t r = te; while (r < e && t[r] == ' ') r++;
                    if (r < e && t[r] == '=') { r++; while (r < e && t[r] == ' ') r++; size_t bb, ee, y = ct_str_at(t, n, r, &bb, &ee); if (y) ck_cadd_enum(K, bb, t + bb, (int)(ee - bb), 1); } } }
        }
        a = e + 1;
    }
}

/* ---- cliente gerado: os nomes das funcoes/metodos sao nomes de operacao do contrato (CONSUME) ---- */
static int ct_is_generated(const char *path, const char *t, size_t n) {
    int bl; const char *b = ct_base(path, (int)strlen(path), &bl); char lo[160]; int w = 0; for (const char *p = path; *p && w < 150; p++) lo[w++] = (char)tolower((unsigned char)*p); lo[w] = 0;
    static const char *const P[] = {"/generated/", "/__generated__/", "/gen/", "/openapi-client/", "/api-client/", "/apiclient/", "/openapi_client/", "/swagger-client/", NULL};
    for (int i = 0; P[i]; i++) if (strstr(lo, P[i])) return 1;
    static const char *const S[] = {".pb.go", "_grpc.pb.go", "_pb2.py", "_pb2_grpc.py", ".pb.js", "_pb.js", "_pb.ts", "_grpc_pb.js", "grpc.java", "grpckt.kt", "grpc.kt", ".gen.go", ".gen.ts", ".generated.ts", NULL};
    for (int i = 0; S[i]; i++) { size_t sl = strlen(S[i]); if ((size_t)w >= sl && !strcmp(lo + w - sl, S[i])) return 1; }
    (void)b; (void)bl;
    size_t m = n < 700 ? n : 700; char h[704]; for (size_t i = 0; i < m; i++) h[i] = (char)tolower((unsigned char)t[i]); h[m] = 0;
    return strstr(h, "code generated") || strstr(h, "@generated") || strstr(h, "generated by") || strstr(h, "auto-generated") || strstr(h, "autogenerated") || strstr(h, "do not edit") || strstr(h, "automatically generated") || strstr(h, "openapi-generator") || strstr(h, "openapi generator");
}
static void ck_generated_ops(CK *K) {
    CoderFile *f = K->f; int add = 0;
    for (int i = 0; i < f->nsym && add < 400 && f->nkey < K->max; i++) {
        const CoderSym *y = &f->sym[i]; if (y->kind != CK_FUNC && y->kind != CK_METHOD) continue;
        int l = (int)strlen(y->name); if (l < 4 || l > 60 || y->name[0] == '_' || 0) continue;
        int before = f->nkey; ck_cadd(K, CKR_CONSUME, CKS_OP, 0, y->byte0, y->name, l);
        add += f->nkey - before;
    }
}
static int ck_key_line_cmp(const void *a, const void *b) { const CoderKey *x = (const CoderKey *)a, *y = (const CoderKey *)b; return x->line != y->line ? x->line - y->line : x->role - y->role; }
/* despacho dos extratores de contrato do arquivo; devolve 1 se o arquivo e um contrato (o ck_yaml deixa de tratar `/caminho:` como rota consumida) */
static int ck_contract_extract(CK *K) {
    CP *c = K->c; const int L = c->lang; int contract = 0;
    K->max = 5000;
    int bl; const char *bn = ct_base(c->path, (int)strlen(c->path), &bl);
    if (L == CL_PROTO) {
        size_t m = c->n < 6000 ? c->n : 6000; int isproto = bl > 6 && !strncasecmp(bn + bl - 6, ".proto", 6);
        for (size_t i = 0; i + 14 < m && !isproto; i++) if (!strncmp(c->t + i, "syntax", 6)) { size_t j = i + 6; while (j < m && (c->t[j] == ' ' || c->t[j] == '=')) j++; if (j + 6 < m && !strncmp(c->t + j + 1, "proto", 5)) isproto = 1; }
        if (isproto) { ck_proto(K); contract = 1; }
    } else if (L == CL_GQL) { ck_gql_parse(K, c->t, c->n, 0, 1); contract = 1; }
    else if ((L == CL_YAML || L == CL_JSON) && c->n < (6u << 20)) { int kind = ct_kind_of(c->path, c->t, c->n); if (kind) { ck_contract_doc(K, kind); contract = 1; } }
    if (contract && bl >= 4) ck_cadd(K, CKR_DEFINE, CKS_FILE, 0, 0, bn, bl);          /* o proprio arquivo: o codigo o cita pelo nome (loadSync('orders.proto'), require('./x.schema.json')) */
    if (contract && K->f->nkey > 1) qsort(K->f->key, (size_t)K->f->nkey, sizeof(CoderKey), ck_key_line_cmp);
    if (!contract) K->max = 600;
    return contract;
}

/* ---- um literal ---- */
static void ck_string(CK *K, size_t qpos, const char *s, int l, size_t endpos) {
    const char *t = K->c->t; size_t n = K->c->n;
    size_t p0 = qpos > 240 ? qpos - 240 : 0; const char *pre = t + p0; int pl = (int)(qpos - p0);
    const char *post = t + endpos; int ql = (int)(n - endpos); if (ql > 260) ql = 260;
    if (l < 2 || l > 400) return;
    const int L = K->c->lang;
    { int bl; const char *bn = ct_base(s, l, &bl); if (ct_fname(bn, bl)) { ck_cadd(K, CKR_CONSUME, CKS_FILE, 0, qpos, bn, bl); return; } }          /* "orders.proto", "./x.schema.json", "openapi.yaml": o arquivo de contrato citado */
    if (ct_grpc_path(s, l)) { ck_add(K, CKR_PRODUCE, CKC_ROUTE, 0, qpos, s, l, 0); return; }          /* "/pacote.Servico/Metodo": caminho gRPC (cliente gerado, Invoke, grpc-web); a rota do .proto tem a mesma chave */
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
                size_t j = i + 3; while (j + 2 < n && !(t[j] == q && t[j + 1] == q && t[j + 2] == q)) j++; if (j + 2 < n) ck_gql_try(K, i + 3, j); i = j + 3; continue;
            }
            size_t j = i + 1; int ml = q == '`';
            while (j < n && t[j] != q && j - i < 4000) { if (t[j] == '\\' && q != '`') j++; else if (t[j] == '\n' && !ml) break; j++; }
            if (j >= n || t[j] != q || j - i >= 4000) { i++; continue; }
            if (j > i + 1) { ck_string(K, i, t + i + 1, (int)(j - i - 1), j + 1); if (j - i > 14) ck_gql_try(K, i + 1, j); }
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
                else if (!K->contract && t[s] == '/' && le > s + 1 && t[le - 1] == ':' && ind >= 1) { char rk[100]; int rl = ck_route_norm(t + s, (int)(le - 1 - s), rk, sizeof rk, 0); if (rl) ck_add(K, CKR_CONSUME, CKC_ROUTE, 0, s, rk, rl, 0); }
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
    CK K; memset(&K, 0, sizeof K); K.c = c; K.f = c->f; const int L = c->lang; K.max = 600;
    if (!c->t || c->n < 3) return;
    K.contract = ck_contract_extract(&K);          /* contratos: OpenAPI/AsyncAPI/JSON Schema (yaml, json), .proto, GraphQL: papel DEFINE */
    if (L == CL_JSON) return;
    if (L == CL_SH) ck_shell(&K);
    else if (L == CL_DOCKER) ck_docker(&K);
    else if (L == CL_YAML) ck_yaml(&K);
    else if (L == CL_PROPS) ck_props(&K, 0);
    else if (L == CL_TOML) ck_props(&K, 1);
    else if (ck_generic_lang(L)) {
        ck_generic(&K);
        if (L == CL_TS) ck_typed_ts(&K); else if (L == CL_PY) ck_typed_py(&K); else if (L == CL_GO) ck_typed_go(&K);
        if (ct_is_generated(c->path, c->raw ? c->raw : c->t, c->n)) ck_generated_ops(&K);
    }
}
