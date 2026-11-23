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
static char g_intent_user[800], g_intent_find[400], g_intent_grep[200];
static void intent_set_user(const char *line) {
    while (line[0] == '[') { const char *nl = strchr(line, '\n'); if (!nl) break; line = nl + 1; }       /* pula as notas do katana no comeco */
    snprintf(g_intent_user, sizeof g_intent_user, "%s", line); g_intent_find[0] = 0; g_intent_grep[0] = 0;
}
/* o que o modelo procura: a ultima consulta do Find, o ultimo padrao do Grep e o pedido do usuario deste turno, juntos (so os termos; o rd_best_windows ignora palavras vazias) */
static const char *intent_get(void) {
    static char buf[1500];
    snprintf(buf, sizeof buf, "%s %s %s", g_intent_find, g_intent_grep, g_intent_user);
    for (char *p = buf; *p; p++) if (*p == '|' || *p == '\\') *p = ' ';      /* padrao de regex vira palavras */
    const char *q = buf; while (*q == ' ') q++;
    if (*q) return q;
    const char *e = getenv("KATANA_INTENT"); return e ? e : "";
}
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
typedef struct { char path[1024]; long long size, mt, cov_end, last_off, last_end, last_n, total; int nreads; } RdCur;      /* nreads: leituras deste arquivo neste pedido */
static int g_rd_toomany;      /* a ultima leitura foi a RD_FORCE_READS-esima do mesmo arquivo: o laco encerra o pedido (zera em rc_reset) */
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
    RdSeen *r = &g_rs[g_rs_n++]; snprintf(r->path, sizeof r->path, "%.1023s", abs); r->a = (int)a; r->b = (int)(b > 1000000000 ? 1000000000 : b); r->how = how;
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
/* ---- leituras de pedidos ANTERIORES da mesma conversa: o que o Read entregou continua no contexto do modelo (ate o arquivo mudar no
 * disco, o resultado sair do contexto pela compactacao ou a conversa acabar). Fica separado do estado do pedido (g_rs, g_rc) de proposito:
 * o Find e o UIOE so olham o pedido atual. rc_reset (pedido novo) absorve o que o pedido leu; rd_forget_* esquecem. ---- */
#define RP_MAX 192
typedef struct { char path[1024]; int a, b; long long size, mt; } RpRange;
typedef struct { char path[1024]; long long size, mt, last_off, last_end, last_n, total; } RpCur;
static RpRange g_rp[RP_MAX]; static int g_rp_n;
static RpCur g_rpc[RC_MAX]; static int g_rpc_n;
static void rp_drop_path(const char *abs) {
    for (int i = 0; i < g_rp_n; ) { if (!strcmp(g_rp[i].path, abs)) g_rp[i] = g_rp[--g_rp_n]; else i++; }
    for (int i = 0; i < g_rpc_n; ) { if (!strcmp(g_rpc[i].path, abs)) g_rpc[i] = g_rpc[--g_rpc_n]; else i++; }
}
static void rp_add(const char *abs, int a, int b, long long size, long long mt) {
    for (int i = 0; i < g_rp_n; i++) if (!strcmp(g_rp[i].path, abs) && g_rp[i].size == size && g_rp[i].mt == mt && a <= g_rp[i].b + 1 && b >= g_rp[i].a - 1) {
        if (a < g_rp[i].a) g_rp[i].a = a;
        if (b > g_rp[i].b) g_rp[i].b = b;
        return;      /* emenda com a faixa vizinha */
    }
    if (g_rp_n == RP_MAX) { memmove(g_rp, g_rp + 1, sizeof(RpRange) * (RP_MAX - 1)); g_rp_n--; }
    RpRange *r = &g_rp[g_rp_n++]; snprintf(r->path, sizeof r->path, "%.1023s", abs); r->a = a; r->b = b; r->size = size; r->mt = mt;
}
/* pedido novo: o que este pedido leu passa a valer como "ja entregue em pedido anterior" */
static void rp_absorb(void) {
    for (int i = 0; i < g_rc_n; i++) {
        const RdCur *c = &g_rc[i];
        for (int j = 0; j < g_rp_n; ) { if (!strcmp(g_rp[j].path, c->path) && (g_rp[j].size != c->size || g_rp[j].mt != c->mt)) g_rp[j] = g_rp[--g_rp_n]; else j++; }      /* arquivo mudou */
        for (int j = 0; j < g_rs_n; j++) if ((g_rs[j].how & 2) && !strcmp(g_rs[j].path, c->path)) rp_add(c->path, g_rs[j].a, g_rs[j].b, c->size, c->mt);
        int k = -1; for (int j = 0; j < g_rpc_n; j++) if (!strcmp(g_rpc[j].path, c->path)) k = j;
        if (k < 0) { if (g_rpc_n == RC_MAX) { memmove(g_rpc, g_rpc + 1, sizeof(RpCur) * (RC_MAX - 1)); g_rpc_n--; } k = g_rpc_n++; }
        RpCur *p = &g_rpc[k]; snprintf(p->path, sizeof p->path, "%.1023s", c->path);
        p->size = c->size; p->mt = c->mt; p->last_off = c->last_off; p->last_end = c->last_end; p->last_n = c->last_n; p->total = c->total;
    }
}
static void rc_clear(void) { g_rc_n = 0; g_rs_n = 0; g_rd_toomany = 0; }
static void rc_reset(void) { rp_absorb(); rc_clear(); }
static void rs_forget_read(const char *abs);
static void rd_forget_all(void) { g_rp_n = 0; g_rpc_n = 0; g_rc_n = 0; for (int i = 0; i < g_rs_n; ) { g_rs[i].how &= ~2; if (!g_rs[i].how) g_rs[i] = g_rs[--g_rs_n]; else i++; } }
/* o resultado de um Read de `key` (o caminho como o modelo o escreveu: absoluto ou relativo) saiu do contexto: o que foi entregue desse
 * arquivo deixa de valer, e uma releitura entrega o texto de novo */
static void rd_forget_key(const char *key) {
    if (!key || !*key) return;
    while (key[0] == '.' && key[1] == '/') key += 2;
    const size_t kl = strlen(key);
    if (!kl) return;
    static char list[64][1024]; int nl = 0;
    #define RD_MATCH(P) ({ const size_t pl = strlen(P); !strcmp(P, key) || (pl > kl && P[pl - kl - 1] == '/' && !strcmp(P + pl - kl, key)); })
    for (int i = 0; i < g_rp_n && nl < 64; i++) if (RD_MATCH(g_rp[i].path)) { int d = 0; for (int j = 0; j < nl; j++) if (!strcmp(list[j], g_rp[i].path)) d = 1; if (!d) snprintf(list[nl++], 1024, "%s", g_rp[i].path); }
    for (int i = 0; i < g_rpc_n && nl < 64; i++) if (RD_MATCH(g_rpc[i].path)) { int d = 0; for (int j = 0; j < nl; j++) if (!strcmp(list[j], g_rpc[i].path)) d = 1; if (!d) snprintf(list[nl++], 1024, "%s", g_rpc[i].path); }
    for (int i = 0; i < g_rc_n && nl < 64; i++) if (RD_MATCH(g_rc[i].path)) { int d = 0; for (int j = 0; j < nl; j++) if (!strcmp(list[j], g_rc[i].path)) d = 1; if (!d) snprintf(list[nl++], 1024, "%s", g_rc[i].path); }
    #undef RD_MATCH
    for (int j = 0; j < nl; j++) {
        rp_drop_path(list[j]); rs_forget_read(list[j]);
        for (int i = 0; i < g_rc_n; ) { if (!strcmp(g_rc[i].path, list[j])) g_rc[i] = g_rc[--g_rc_n]; else i++; }
    }
}
/* cobertura de [a, b] pelo que o Read ja entregou ao modelo, neste pedido (g_rs) e em pedidos anteriores da conversa (g_rp, so se o arquivo
 * e o mesmo: tamanho e data). Devolve quantas linhas; *first_new = primeira linha ainda nao entregue (b + 1 se nenhuma);
 * *prior = 1 se alguma linha so foi entregue em pedido anterior */
static int rd_cover(const char *abs, long long size, long long mt, int a, int b, int *first_new, int *prior) {
    int ra[96], rb[96], rp[96], nr = 0, n = 0;
    if (first_new) *first_new = b + 1;
    if (prior) *prior = 0;
    if (b - a > 20000) b = a + 20000;
    for (int i = 0; i < g_rs_n && nr < 96; i++) if ((g_rs[i].how & 2) && !strcmp(g_rs[i].path, abs)) { ra[nr] = g_rs[i].a; rb[nr] = g_rs[i].b; rp[nr++] = 0; }
    for (int i = 0; i < g_rp_n && nr < 96; i++) if (g_rp[i].size == size && g_rp[i].mt == mt && !strcmp(g_rp[i].path, abs)) { ra[nr] = g_rp[i].a; rb[nr] = g_rp[i].b; rp[nr++] = 1; }
    if (!nr) return 0;
    for (int ln = a; ln <= b; ln++) {
        int c = 0, cur = 0;
        for (int i = 0; i < nr; i++) if (ra[i] <= ln && ln <= rb[i]) { c = 1; if (!rp[i]) cur = 1; }
        if (c) { n++; if (!cur && prior) *prior = 1; } else if (first_new && *first_new > b) *first_new = ln;
    }
    return n;
}
/* ultima leitura do arquivo (neste pedido ou, se nao houve, em pedido anterior): onde comecou, onde terminou, quantas linhas entregou */
static int rd_last(const RdCur *c, const char *abs, long long size, long long mt, long long *off, long long *end, long long *nl) {
    if (c) { *off = c->last_off; *end = c->last_end; *nl = c->last_n; return 1; }
    for (int i = 0; i < g_rpc_n; i++) if (!strcmp(g_rpc[i].path, abs) && g_rpc[i].size == size && g_rpc[i].mt == mt) { *off = g_rpc[i].last_off; *end = g_rpc[i].last_end; *nl = g_rpc[i].last_n; return 1; }
    return 0;
}
/* o arquivo mudou: o que o Read entregou dele deixou de valer */
static void rs_forget_read(const char *abs) {
    for (int i = 0; i < g_rs_n; ) {
        if (!strcmp(g_rs[i].path, abs)) { g_rs[i].how &= ~2; if (!g_rs[i].how) { g_rs[i] = g_rs[--g_rs_n]; continue; } }
        i++;
    }
}
static RdCur *rc_find(const char *abs, long long size, long long mt) {
    for (int i = 0; i < g_rc_n; i++) if (!strcmp(g_rc[i].path, abs)) {
        if (g_rc[i].size != size || g_rc[i].mt != mt) { rs_forget_read(abs); g_rc[i] = g_rc[--g_rc_n]; return NULL; }      /* arquivo mudou: recomeca */
        return &g_rc[i];
    }
    return NULL;
}
/* registra que as linhas [first, last] de um arquivo de total linhas foram entregues */
static void rc_note(const char *abs, long long size, long long mt, long long first, long long last, long long total) {
    if (strlen(abs) >= sizeof g_rc[0].path) { rs_note(abs, first, last, 2); return; }
    RdCur *c = rc_find(abs, size, mt);      /* antes do rs_note: arquivo mudado esquece o que foi lido dele */
    rs_note(abs, first, last, 2);
    if (!c) {
        if (g_rc_n == RC_MAX) memmove(g_rc, g_rc + 1, sizeof(RdCur) * (RC_MAX - 1)), g_rc_n--;
        c = &g_rc[g_rc_n++]; snprintf(c->path, sizeof c->path, "%s", abs); c->size = size; c->mt = mt; c->cov_end = 0; c->nreads = 1;
    }
    if (first <= c->cov_end + 1 && last > c->cov_end) c->cov_end = last;
    c->last_off = first; c->last_end = last; c->last_n = last - first + 1; c->total = total;
}
static void ct_seen_reset(void) { for (int i = 0; i < g_ct_seen_n; i++) free(g_ct_seen[i].path); g_ct_seen_n = 0; rd_forget_all(); rc_clear(); }
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
