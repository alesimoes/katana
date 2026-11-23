/* ---------- base das ferramentas de src/tools/ (ver docs/tools.md) ----------
 * Contrato de cada ferramenta:
 *     static void ct_nome(const char *js, const char *end, char *out, size_t cap);
 * js..end e o texto com os argumentos (um objeto JSON, sozinho ou depois de "arguments":). O resultado vai em out
 * (texto terminado em zero, no maximo cap bytes); erro comeca com "error: ". Nada e impresso na tela pela ferramenta.
 *
 * Argumentos: ct_str/ct_int/ct_bool/ct_has leem so as chaves do PRIMEIRO nivel do objeto (um valor de texto que
 * contenha "path" nao confunde a busca, ao contrario de json_str).
 * Arquivos vistos: ct_seen_mark/ct_seen guardam o que ja foi lido nesta conversa (Edit e Write consultam). */

/* pula um valor JSON a partir de p; devolve o ponteiro logo depois dele (ou NULL se o texto acabar no meio) */
static const char *ct_skip(const char *p, const char *end) {
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end) return NULL;
    if (*p == '"') {
        for (p++; p < end; p++) { if (*p == '\\') { p++; continue; } if (*p == '"') return p + 1; }
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; p < end; p++) {
            if (*p == '"') { for (p++; p < end && *p != '"'; p++) if (*p == '\\') p++; if (p >= end) return NULL; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
        }
        return NULL;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) p++;
    return p;
}
/* inicio do objeto dos argumentos: depois de "arguments" se houver, senao a primeira chave aberta */
static const char *ct_obj(const char *js, const char *end) {
    const char *a = strstr(js, "\"arguments\"");
    const char *p = (a && a < end) ? a + 11 : js;
    while (p < end && *p != '{') p++;
    return p < end ? p : NULL;
}
/* valor da chave key no primeiro nivel: *vs..*ve (com as aspas, se for texto). 1 se achou */
static int ct_find(const char *js, const char *end, const char *key, const char **vs, const char **ve) {
    const char *p = ct_obj(js, end);
    if (!p) return 0;
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
        if (!e) e = end;
        if ((size_t)(ke - 1 - ks) == kl && memcmp(ks, key, kl) == 0) { *vs = v; *ve = e; return 1; }
        p = e;
    }
}
static int ct_has(const char *js, const char *end, const char *key) { const char *a, *b; return ct_find(js, end, key, &a, &b); }
/* texto JSON entre aspas (s aponta a aspa de abrir) decodificado em memoria nova; trata \uXXXX com pares substitutos */
static char *ct_unquote(const char *s, const char *e, size_t *len) {
    char *o = (char *)malloc((size_t)(e - s) + 4); size_t n = 0;
    for (const char *q = s + 1; q < e && *q != '"'; q++) {
        if (*q != '\\') { o[n++] = *q; continue; }
        if (++q >= e) break;
        switch (*q) {
            case 'n': o[n++] = '\n'; break;
            case 't': o[n++] = '\t'; break;
            case 'r': o[n++] = '\r'; break;
            case 'b': o[n++] = '\b'; break;
            case 'f': o[n++] = '\f'; break;
            case 'u': {
                unsigned cp = 0;
                if (q + 4 < e) { char h[5] = { q[1], q[2], q[3], q[4], 0 }; cp = (unsigned)strtoul(h, NULL, 16); q += 4; }
                if (cp >= 0xD800 && cp < 0xDC00 && q + 6 < e && q[1] == '\\' && q[2] == 'u') {
                    char h[5] = { q[3], q[4], q[5], q[6], 0 }; const unsigned lo = (unsigned)strtoul(h, NULL, 16);
                    if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); q += 6; }
                }
                if (cp < 0x80) o[n++] = (char)cp;
                else if (cp < 0x800) { o[n++] = (char)(0xC0 | (cp >> 6)); o[n++] = (char)(0x80 | (cp & 63)); }
                else if (cp < 0x10000) { o[n++] = (char)(0xE0 | (cp >> 12)); o[n++] = (char)(0x80 | ((cp >> 6) & 63)); o[n++] = (char)(0x80 | (cp & 63)); }
                else { o[n++] = (char)(0xF0 | (cp >> 18)); o[n++] = (char)(0x80 | ((cp >> 12) & 63)); o[n++] = (char)(0x80 | ((cp >> 6) & 63)); o[n++] = (char)(0x80 | (cp & 63)); }
                break;
            }
            default: o[n++] = *q; break;
        }
    }
    o[n] = 0;
    if (len) *len = n;
    return o;
}
/* valor de texto da chave, em memoria nova (free depois); NULL se a chave nao existe ou nao e texto.
   Numero ou booleano escrito sem aspas vem como o proprio texto (modelos locais erram o tipo as vezes). */
static char *ct_str(const char *js, const char *end, const char *key) {
    const char *a, *b;
    if (!ct_find(js, end, key, &a, &b)) return NULL;
    if (*a == '"') return ct_unquote(a, b, NULL);
    if (*a == '{' || *a == '[' || (b - a == 4 && memcmp(a, "null", 4) == 0)) return NULL;
    char *o = (char *)malloc((size_t)(b - a) + 1);
    memcpy(o, a, (size_t)(b - a)); o[b - a] = 0;
    return o;
}
/* valor inteiro (numero, ou numero entre aspas); def se nao houver */
static long long ct_int(const char *js, const char *end, const char *key, long long def) {
    const char *a, *b;
    if (!ct_find(js, end, key, &a, &b)) return def;
    if (*a == '"') a++;
    if (a >= b || !(isdigit((unsigned char)*a) || *a == '-')) return def;
    return atoll(a);
}
/* booleano: true, "true" ou 1 */
static int ct_bool(const char *js, const char *end, const char *key) {
    const char *a, *b;
    if (!ct_find(js, end, key, &a, &b)) return 0;
    if (*a == '"') a++;
    return (b - a >= 4 && memcmp(a, "true", 4) == 0) || *a == '1';
}

/* a pasta de trabalho e a home (ou /): nao e um projeto; buscar "tudo" nela devolve lixo de outros programas */
static int ct_cwd_is_home(void) {
    char cwd[1100], h[1100]; if (!getcwd(cwd, sizeof cwd)) return 0;
    agent_home(h, sizeof h);
    return !strcmp(cwd, "/") || !strcmp(cwd, h);
}
/* o que o modelo procura neste pedido: a ultima consulta do Find (ou o texto do usuario) serve para o Read mostrar a parte do arquivo que importa */
static char g_intent_user[800], g_intent_find[400];
static void intent_set_user(const char *line) {
    while (line[0] == '[') { const char *nl = strchr(line, '\n'); if (!nl) break; line = nl + 1; }       /* pula as notas do katana no comeco */
    snprintf(g_intent_user, sizeof g_intent_user, "%s", line); g_intent_find[0] = 0;
}
static const char *intent_get(void) { if (g_intent_find[0]) return g_intent_find; if (g_intent_user[0]) return g_intent_user; const char *e = getenv("KATANA_INTENT"); return e ? e : ""; }
/* ---- arquivos vistos nesta conversa ----
 * Read marca (inteiro ou so uma parte); Edit e Write consultam. Guarda tamanho e data para saber se o arquivo mudou
 * no disco depois da leitura. ct_seen_reset() a cada conversa nova. */
#define CT_SEEN_MAX 512
typedef struct { char *path; long long size, mt; int partial; } CtSeen;
static CtSeen g_ct_seen[CT_SEEN_MAX];
static int g_ct_seen_n;
/* ---- cursor de leitura por arquivo, neste pedido: ate que linha o modelo ja recebeu (contigua desde a 1) e onde comecou a
 * ultima leitura. Read sem offset, ou repetindo a ultima chamada, continua de onde parou em vez de entregar o mesmo trecho.
 * Zera a cada pedido (rc_reset em rr_reset) e a cada conversa. ---- */
#define RC_MAX 64
typedef struct { char path[1024]; long long size, mt, cov_end, last_off, total; } RdCur;
static RdCur g_rc[RC_MAX]; static int g_rc_n;
/* ---- faixas de linhas ja entregues ao modelo neste pedido (lidas pelo Read ou apontadas pelo Find): o Find nao repete o mesmo trecho e
 * prefere o que ainda nao foi visto. how: 1 mostrado pelo Find, 2 lido pelo Read. Zera junto com o cursor de leitura. ---- */
#define RS_MAX 256
typedef struct { char path[1024]; int a, b, how; } RdSeen;
static RdSeen g_rs[RS_MAX]; static int g_rs_n;
static void rs_note(const char *abs, long long a, long long b, int how) {
    if (!abs || strlen(abs) >= sizeof g_rs[0].path || a < 1 || b < a) return;
    for (int i = 0; i < g_rs_n; i++) if (g_rs[i].a == a && g_rs[i].b == b && !strcmp(g_rs[i].path, abs)) { g_rs[i].how |= how; return; }
    if (g_rs_n == RS_MAX) { memmove(g_rs, g_rs + 1, sizeof(RdSeen) * (RS_MAX - 1)); g_rs_n--; }
    RdSeen *r = &g_rs[g_rs_n++]; snprintf(r->path, sizeof r->path, "%s", abs); r->a = (int)a; r->b = (int)(b > 1000000000 ? 1000000000 : b); r->how = how;
}
/* quantas linhas de [a, b] do arquivo ja foram entregues; *how recebe a uniao dos tipos */
static int rs_cover(const char *abs, int a, int b, int *how) {
    int n = 0; if (how) *how = 0;
    if (b - a > 4000) b = a + 4000;
    for (int ln = a; ln <= b; ln++) {
        int c = 0;
        for (int i = 0; i < g_rs_n && !c; i++) if (g_rs[i].a <= ln && ln <= g_rs[i].b && !strcmp(g_rs[i].path, abs)) { c = 1; if (how) *how |= g_rs[i].how; }
        n += c;
    }
    return n;
}
static void rc_reset(void) { g_rc_n = 0; g_rs_n = 0; }
static RdCur *rc_find(const char *abs, long long size, long long mt) {
    for (int i = 0; i < g_rc_n; i++) if (!strcmp(g_rc[i].path, abs)) {
        if (g_rc[i].size != size || g_rc[i].mt != mt) { g_rc[i] = g_rc[--g_rc_n]; return NULL; }      /* arquivo mudou: recomeca */
        return &g_rc[i];
    }
    return NULL;
}
/* registra que as linhas [first, last] de um arquivo de total linhas foram entregues */
static void rc_note(const char *abs, long long size, long long mt, long long first, long long last, long long total) {
    rs_note(abs, first, last, 2);
    if (strlen(abs) >= sizeof g_rc[0].path) return;
    RdCur *c = rc_find(abs, size, mt);
    if (!c) {
        if (g_rc_n == RC_MAX) memmove(g_rc, g_rc + 1, sizeof(RdCur) * (RC_MAX - 1)), g_rc_n--;
        c = &g_rc[g_rc_n++]; snprintf(c->path, sizeof c->path, "%s", abs); c->size = size; c->mt = mt; c->cov_end = 0;
    }
    if (first <= c->cov_end + 1 && last > c->cov_end) c->cov_end = last;
    c->last_off = first; c->total = total;
}
static void ct_seen_reset(void) { for (int i = 0; i < g_ct_seen_n; i++) free(g_ct_seen[i].path); g_ct_seen_n = 0; rc_reset(); }
static void ct_seen_mark(const char *abs, int partial) {
    struct stat st; long long sz = -1, mt = 0;
    if (stat(abs, &st) == 0) { sz = (long long)st.st_size; mt = (long long)st.st_mtime; }
    for (int i = 0; i < g_ct_seen_n; i++) if (strcmp(g_ct_seen[i].path, abs) == 0) {
        if (!partial || g_ct_seen[i].size != sz || g_ct_seen[i].mt != mt) g_ct_seen[i].partial = partial;     /* parte depois de inteiro: continua inteiro */
        g_ct_seen[i].size = sz; g_ct_seen[i].mt = mt;
        return;
    }
    if (g_ct_seen_n == CT_SEEN_MAX) { free(g_ct_seen[0].path); memmove(g_ct_seen, g_ct_seen + 1, sizeof(CtSeen) * (CT_SEEN_MAX - 1)); g_ct_seen_n--; }
    g_ct_seen[g_ct_seen_n++] = (CtSeen){ strdup(abs), sz, mt, partial };
}
/* 0 nunca visto; 1 visto so em parte; 2 visto inteiro; 3 visto inteiro mas mudou no disco depois */
static int ct_seen(const char *abs) {
    for (int i = 0; i < g_ct_seen_n; i++) if (strcmp(g_ct_seen[i].path, abs) == 0) {
        if (g_ct_seen[i].partial) return 1;
        struct stat st;
        if (stat(abs, &st) == 0 && ((long long)st.st_size != g_ct_seen[i].size || (long long)st.st_mtime != g_ct_seen[i].mt)) return 3;
        return 2;
    }
    return 0;
}
