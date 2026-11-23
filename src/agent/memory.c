/* ---------- agent/memory.c: memoria do agente (remember, recall, katana memory) ----------
 * Arquivo: ~/.katana/memoria.txt (KATANA_MEM muda o caminho), sempre 0600. O formato ANTIGO continua valido e e o formato dos fatos vivos:
 *     AAAA-MM-DD<TAB>fato                      fato vivo (uma linha por fato; e tudo o que o katana antigo entende)
 * Linhas novas, SEM tabulacao (o katana antigo as ignora, entao a migracao e silenciosa e reversivel: `katana memory export` devolve so os vivos):
 *     #sup ID DATA DATA_DA_TROCA ID_NOVO fato  fato substituido por outro sobre o mesmo assunto (historico, nao apagado)
 *     #del ID DATA DATA_DO_ESQUECIMENTO fato   fato esquecido (katana memory forget); fica no historico ate `forget --purge`
 *     #pin ID                                  fato fixo: entra no prompt de sistema mesmo fora dos mais recentes
 * ID (identidade estavel) = FNV-1a de 32 bits do texto do fato, 8 hex; nao e gravado nos fatos vivos (arquivo antigo funciona sem migrar).
 * Assunto: os termos de conteudo do fato (tokenizador de coder/text.c, sem palavras funcionais, pt e en); dois fatos vivos sao do mesmo
 * assunto quando dividem >= 2 termos e >= 60% dos termos do menor. remember de um fato do mesmo assunto SUBSTITUI o anterior (que vira #sup).
 * Prompt de sistema: mem_tail(max) devolve os `max` ultimos fatos vivos no formato "- data: fato" (identico ao de antes); fatos fixos
 * (#pin), se houver, entram no lugar dos mais antigos desse conjunto. Sem #pin a saida e a mesma de sempre.
 * memory_search(query, max, out, cap): BM25 simples (tokens pt/en) + recencia, fato substituido penalizado; usada por recall e pelo UIOE. */
#define MEM_MAXFILE (16 * 1024 * 1024)
typedef struct { int kind; char id[9]; char date[40]; char sdate[40]; char by[9]; char *fact; char *raw; int pinned; } MemE;   /* kind: 0 vivo, 1 #sup, 2 #del, 4 linha estranha, 5 descartada */
typedef struct { MemE *e; int n, cap; char *buf; int trunc; } MemDb;
typedef struct { int idx; double score; } MemHit;

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
static void mem_today(char *d, size_t cap) { const time_t t = time(NULL); strftime(d, cap, "%Y-%m-%d", localtime(&t)); }
static void mem_id(const char *s, char *id) { unsigned h = 2166136261u; for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; } snprintf(id, 9, "%08x", h); }
static long mem_days(const char *d) {        /* dias desde 1970 de AAAA-MM-DD; -1 se nao for data */
    int y, m, dd;
    if (sscanf(d, "%4d-%2d-%2d", &y, &m, &dd) != 3 || y < 1970 || m < 1 || m > 12 || dd < 1 || dd > 31) return -1;
    if (m <= 2) y--;
    const int era = y / 400, yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + dd - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long)era * 146097 + doe - 719468;
}
static MemE *mem_add(MemDb *db) {
    if (db->n == db->cap) { db->cap = db->cap ? db->cap * 2 : 256; db->e = (MemE *)realloc(db->e, (size_t)db->cap * sizeof(MemE)); }
    MemE *x = &db->e[db->n++]; memset(x, 0, sizeof *x); return x;
}
static char *mem_word(char **p) {           /* proxima palavra separada por espaco (termina em \0), ou NULL */
    while (**p == ' ') (*p)++;
    if (!**p) return NULL;
    char *w = *p; while (**p && **p != ' ') (*p)++;
    if (**p) *(*p)++ = 0;
    return w;
}
static void mem_free(MemDb *db) { free(db->e); free(db->buf); memset(db, 0, sizeof *db); }
/* carrega o arquivo inteiro (ate 16 MB; maior que isso le o fim e marca trunc). -1 se nao existe */
static int mem_load(MemDb *db) {
    memset(db, 0, sizeof *db);
    char p[1200]; mem_path(p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    const long skip = sz > MEM_MAXFILE ? sz - MEM_MAXFILE : 0;
    db->trunc = skip > 0;
    fseek(f, skip, SEEK_SET);
    db->buf = (char *)malloc((size_t)(sz - skip) + 1);
    if (!db->buf) { fclose(f); return -1; }
    const size_t n = fread(db->buf, 1, (size_t)(sz - skip), f);
    db->buf[n] = 0;
    fclose(f);
    for (char *l = db->buf; *l; ) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        MemE *x = mem_add(db);
        char *tab = strchr(l, '\t');
        if (tab) {
            x->kind = 0; *tab = 0;
            snprintf(x->date, sizeof x->date, "%s", l);
            x->fact = tab + 1; mem_id(x->fact, x->id);
        } else if (!strncmp(l, "#sup ", 5) || !strncmp(l, "#del ", 5)) {
            char *q = l + 5; const int sup = l[1] == 's';
            char *id = mem_word(&q), *d = mem_word(&q), *sd = mem_word(&q), *by = sup ? mem_word(&q) : (char *)"";
            if (id && d && sd && by && strlen(id) == 8) {
                x->kind = sup ? 1 : 2; snprintf(x->id, sizeof x->id, "%s", id); snprintf(x->date, sizeof x->date, "%s", d);
                snprintf(x->sdate, sizeof x->sdate, "%s", sd); snprintf(x->by, sizeof x->by, "%.8s", by);
                while (*q == ' ') q++;
                x->fact = q;
            } else { x->kind = 4; x->raw = l; }
        } else if (!strncmp(l, "#pin ", 5)) {
            x->kind = 5; snprintf(x->id, sizeof x->id, "%.8s", l + 5);       /* marcador: aplicado abaixo e nao regravado como entrada */
        } else { x->kind = 4; x->raw = l; }
        if (!e) { if (!*l && x->kind == 4) db->n--; break; }
        l = e + 1;
    }
    for (int i = 0; i < db->n; i++) if (db->e[i].kind == 5 && db->e[i].id[0]) {
        for (int j = 0; j < db->n; j++) if (db->e[j].kind == 0 && !strcmp(db->e[j].id, db->e[i].id)) db->e[j].pinned = 1;
    }
    return 0;
}
static void mem_chmod(const char *p) {
#ifndef _WIN32
    chmod(p, 0600);
#else
    (void)p;
#endif
}
static void mem_mkdir_of(const char *p) {
    char dir[1200]; snprintf(dir, sizeof dir, "%s", p);
    char *sl = strrchr(dir, '/'); char *bs = strrchr(dir, '\\');
    if (bs && (!sl || bs > sl)) sl = bs;
    if (!sl) return;
    *sl = 0;
#ifdef _WIN32
    CreateDirectoryA(dir, NULL);
#else
    mkdir(dir, 0700);
#endif
}
/* regrava o arquivo inteiro (temporario + rename), 0600. 0 = ok */
static int mem_save(const MemDb *db) {
    if (db->trunc) return -1;                       /* nunca regrava um arquivo lido pela metade */
    char p[1200], t[1210]; mem_path(p, sizeof p); snprintf(t, sizeof t, "%s.tmp", p);
    mem_mkdir_of(p);
    FILE *f = fopen(t, "wb");
    if (!f) return -1;
    for (int i = 0; i < db->n; i++) {
        const MemE *x = &db->e[i];
        if (x->kind == 0) fprintf(f, "%s\t%s\n", x->date, x->fact);
        else if (x->kind == 1) fprintf(f, "#sup %s %s %s %s %s\n", x->id, x->date, x->sdate, x->by, x->fact);
        else if (x->kind == 2) fprintf(f, "#del %s %s %s %s\n", x->id, x->date, x->sdate, x->fact);
        else if (x->kind == 4) fprintf(f, "%s\n", x->raw);
    }
    for (int i = 0; i < db->n; i++) if (db->e[i].kind == 0 && db->e[i].pinned) fprintf(f, "#pin %s\n", db->e[i].id);
    if (fclose(f) != 0) { remove(t); return -1; }
    mem_chmod(t);
#ifdef _WIN32
    remove(p);
#endif
    if (rename(t, p) != 0) { remove(t); return -1; }
    mem_chmod(p);
    return 0;
}
static void mem_clean_date(char *d) { for (; *d; d++) if (*d == ' ' || *d == '\t') *d = '_'; }

/* ---- segredos: chaves de API, tokens, senhas nao sao gravados ---- */
static int mem_tokch(int c) { return isalnum(c) || c == '_' || c == '-' || c == '+' || c == '=' || c == '/' || c == '.'; }
static const char *mem_secret(const char *t) {   /* NULL se nao parece segredo, senao o tipo */
    static const struct { const char *pre; int min; const char *what; } P[] = {
        {"sk-", 20, "an API key"}, {"ghp_", 20, "a GitHub token"}, {"gho_", 20, "a GitHub token"}, {"ghs_", 20, "a GitHub token"}, {"ghu_", 20, "a GitHub token"},
        {"github_pat_", 20, "a GitHub token"}, {"glpat-", 20, "a GitLab token"}, {"hf_", 30, "an access token"}, {"npm_", 30, "an access token"},
        {"AIza", 35, "an API key"}, {"AKIA", 20, "a cloud access key"}, {"ASIA", 20, "a cloud access key"}, {"xoxb-", 15, "a chat token"}, {"xoxp-", 15, "a chat token"},
        {"xoxa-", 15, "a chat token"}, {"xoxr-", 15, "a chat token"}, {"xoxs-", 15, "a chat token"}, {NULL, 0, NULL}};
    if (strstr(t, "-----BEGIN") && strstr(t, "PRIVATE KEY")) return "a private key";
    for (const char *s = t; *s; ) {
        while (*s && !mem_tokch((unsigned char)*s)) s++;
        const char *b = s; while (*s && mem_tokch((unsigned char)*s)) s++;
        const int l = (int)(s - b);
        if (l < 8) continue;
        for (int i = 0; P[i].pre; i++) if (l >= P[i].min && !strncmp(b, P[i].pre, strlen(P[i].pre))) return P[i].what;
        if (l >= 30 && !strncmp(b, "eyJ", 3)) { int dots = 0; for (int i = 0; i < l; i++) dots += b[i] == '.'; if (dots == 2) return "a JWT"; }
        if (l >= 32) {                               /* sequencia longa e misturada: letras e digitos, nao so hexadecimal minusculo (hash de commit) */
            int dig = 0, let = 0, nonhex = 0, slash = 0, dot = 0;
            for (int i = 0; i < l; i++) { const int c = (unsigned char)b[i]; dig += isdigit(c) != 0; let += isalpha(c) != 0; slash += c == '/'; dot += c == '.';
                if (!(isdigit(c) || (c >= 'a' && c <= 'f'))) nonhex = 1; }
            if (dig && let && nonhex && !slash && !dot) return "a long random string (key or token)";
        }
    }
    char lc[600]; size_t n = 0;
    for (const char *s = t; *s && n < sizeof lc - 1; s++) lc[n++] = (char)tolower((unsigned char)*s);
    lc[n] = 0;
    static const char *const K[] = {"password", "passwd", "passphrase", "senha", "secret", "segredo", "token", "api key", "api_key", "apikey", "api-key", "chave de api", "private key", "chave privada", "pwd", NULL};
    for (int k = 0; K[k]; k++) {
        const size_t kl = strlen(K[k]);
        for (const char *s = lc; (s = strstr(s, K[k])) != NULL; s += kl) {
            const char *q = s + kl;
            while (*q == ' ') q++;
            int ok = 0;
            if (*q == ':' || *q == '=') { ok = 1; q++; }
            else if (!strncmp(q, "is ", 3)) { ok = 1; q += 3; }
            else if (!strncmp(q, "e ", 2)) { ok = 1; q += 2; }
            else if (!strncmp(q, "eh ", 3)) { ok = 1; q += 3; }
            else if (!strncmp(q, "= ", 2)) { ok = 1; q += 2; }
            if (!ok) continue;
            while (*q == ' ' || *q == '"' || *q == '\'' || *q == '`') q++;
            const char *v = q; while (*q && *q != ' ' && *q != '"' && *q != '\'' && *q != '`' && *q != ',') q++;
            const int vl = (int)(q - v);
            int mixed = 0; for (int i = 0; i < vl; i++) if (!isalpha((unsigned char)v[i])) mixed = 1;
            if (vl >= 6 && (mixed || vl >= 10)) return "a password, token or key";
        }
    }
    return NULL;
}

/* ---- assunto e busca ---- */
static int mem_terms(const char *text, char (*out)[48], int max, int *total) {   /* termos de conteudo distintos; total = tokens com repeticao */
    CoderTok tk[160];
    const int nt = coder_tokens(text, tk, 160);
    int n = 0; if (total) *total = 0;
    for (int i = 0; i < nt; i++) {
        if (tk[i].stop || !tk[i].norm[0]) continue;
        if (total) (*total)++;
        int dup = 0;
        for (int j = 0; j < n; j++) if (!strcmp(out[j], tk[i].norm)) { dup = 1; break; }
        if (!dup && n < max) { snprintf(out[n], 48, "%.47s", tk[i].norm); n++; }
    }
    return n;
}
static int mem_similar(const MemDb *db, const char *fact) {   /* indice do fato vivo do mesmo assunto, ou -1 */
    static char A[64][48], B[64][48];
    const int na = mem_terms(fact, A, 64, NULL);
    int best = -1; double bs = 0;
    for (int i = 0; i < db->n; i++) {
        if (db->e[i].kind != 0) continue;
        const int nb = mem_terms(db->e[i].fact, B, 64, NULL);
        int sh = 0;
        for (int a = 0; a < na; a++) for (int b = 0; b < nb; b++) if (!strcmp(A[a], B[b])) { sh++; break; }
        const int mn = na < nb ? na : nb;
        if (sh < 2 || mn == 0) continue;
        const double r = (double)sh / mn;
        if (r >= 0.6 && r >= bs) { bs = r; best = i; }
    }
    return best;
}
/* classifica os fatos contra a consulta; devolve quantos acertos (ate maxhits), do mais relevante ao menos */
static int mem_rank(const MemDb *db, const char *query, int incold, MemHit *hits, int maxhits) {
    static char Q[16][48];
    const int nq = mem_terms(query, Q, 16, NULL);
    char raw[16][40]; int nr = 0;
    for (const char *s = query; *s && nr < 16; ) {         /* palavras cruas (2+ letras), minusculas: acha o que a normalizacao nao junta */
        while (*s && !isalnum((unsigned char)*s) && (unsigned char)*s < 0x80) s++;
        size_t l = 0; char w[40];
        while (*s && (isalnum((unsigned char)*s) || (unsigned char)*s >= 0x80) && l < 39) w[l++] = (char)tolower((unsigned char)*s++);
        while (*s && (isalnum((unsigned char)*s) || (unsigned char)*s >= 0x80)) s++;
        w[l] = 0;
        if (l >= 2) { memcpy(raw[nr], w, l + 1); nr++; }
    }
    if (!nq && !nr) return 0;
    const int N = db->n;
    int *docs = (int *)malloc((size_t)(N + 1) * sizeof(int)), nd = 0;
    int *tf = (int *)calloc((size_t)(N + 1) * 16, sizeof(int)), *dl = (int *)calloc((size_t)N + 1, sizeof(int));
    if (!docs || !tf || !dl) { free(docs); free(tf); free(dl); return 0; }
    int df[16] = {0}; double avg = 0;
    for (int i = 0; i < N; i++) {
        const MemE *x = &db->e[i];
        if (!(x->kind == 0 || (incold && x->kind == 1))) continue;
        CoderTok tk[160]; const int nt = coder_tokens(x->fact, tk, 160);
        int len = 0;
        for (int t = 0; t < nt; t++) {
            if (tk[t].stop || !tk[t].norm[0]) continue;
            len++;
            for (int q = 0; q < nq; q++) if (!strcmp(tk[t].norm, Q[q])) tf[nd * 16 + q]++;
        }
        for (int q = 0; q < nq; q++) if (tf[nd * 16 + q]) df[q]++;
        dl[nd] = len; avg += len; docs[nd++] = i;
    }
    if (nd) avg /= nd;
    if (avg < 1) avg = 1;
    const long now = (long)(time(NULL) / 86400);
    int nh = 0;
    double *sc = (double *)calloc((size_t)nd + 1, sizeof(double));
    for (int d = 0; d < nd && sc; d++) {
        const MemE *x = &db->e[docs[d]];
        double s = 0;
        for (int q = 0; q < nq; q++) {
            const double f = tf[d * 16 + q]; if (f <= 0) continue;
            const double idf = log(1.0 + (nd - df[q] + 0.5) / (df[q] + 0.5));
            s += idf * (f * 2.2) / (f + 1.2 * (0.25 + 0.75 * dl[d] / avg));
        }
        const size_t fl = strlen(x->fact);
        for (int r = 0; r < nr; r++) if (ci_find(x->fact, fl, raw[r], strlen(raw[r]))) s += 0.3;
        if (s <= 0) continue;
        const long dd = mem_days(x->date); const double age = dd >= 0 && now > dd ? (double)(now - dd) : 0;
        s *= 0.75 + 0.5 / (1.0 + age / 365.0);               /* recencia: ate +25% para fato de hoje, -25% (assintota) para fato muito antigo */
        if (x->kind == 1) s *= 0.4;                           /* substituido: ainda aparece, bem abaixo */
        sc[d] = s;
    }
    for (int pick = 0; pick < maxhits && sc; pick++) {
        int best = -1;
        for (int d = 0; d < nd; d++) if (sc[d] > 0 && (best < 0 || sc[d] >= sc[best])) best = d;       /* empate: o mais recente (indice maior) */
        if (best < 0) break;
        hits[nh].idx = docs[best]; hits[nh].score = sc[best]; nh++; sc[best] = 0;
    }
    free(sc); free(docs); free(tf); free(dl);
    return nh;
}

/* API estruturada para o UIOE (conn_memory.c): ate `max` fatos VIVOS (nunca substituidos nem esquecidos), do mais relevante ao menos, com id, data e texto.
 * Fato que parece segredo (mem_secret) nunca sai daqui, mesmo que alguem o tenha gravado a mao no arquivo. Devolve quantos; *nsecret conta os omitidos. */
typedef struct { char id[9]; char date[40]; char fact[420]; int pinned; } MemRec;
static int memory_query(const char *query, int max, MemRec *out, int *nsecret) {
    if (nsecret) *nsecret = 0;
    MemDb db;
    if (mem_load(&db) != 0) return 0;
    MemHit h[32]; if (max > 16) max = 16;
    const int n = mem_rank(&db, query, 0, h, 32);
    int got = 0;
    for (int i = 0; i < n && got < max; i++) {
        const MemE *x = &db.e[h[i].idx];
        if (mem_secret(x->fact)) { if (nsecret) (*nsecret)++; continue; }
        MemRec *r = &out[got++];
        snprintf(r->id, sizeof r->id, "%s", x->id[0] ? x->id : "");
        if (!r->id[0]) mem_id(x->fact, r->id);
        snprintf(r->date, sizeof r->date, "%s", x->date); snprintf(r->fact, sizeof r->fact, "%.419s", x->fact); r->pinned = x->pinned;
    }
    mem_free(&db);
    return got;
}
/* API para o UIOE e para recall: ate `max` fatos no formato "- data: fato" (substituidos marcados), do mais relevante ao menos; devolve quantos */
static int memory_search(const char *query, int max, char *out, size_t cap) {
    out[0] = 0;
    MemDb db;
    if (mem_load(&db) != 0) return 0;
    MemHit h[32]; if (max > 32) max = 32;
    const int n = mem_rank(&db, query, 1, h, max);
    size_t o = 0; int shown = 0;
    for (int i = 0; i < n; i++) {
        const MemE *x = &db.e[h[i].idx];
        if (o + 700 >= cap) break;
        o += (size_t)snprintf(out + o, cap - o, "- %s: %.400s%s\n", x->date, x->fact, x->kind == 1 ? " [superseded]" : "");
        shown++;
    }
    mem_free(&db);
    return shown;
}

/* ---- ferramentas ---- */
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
    const char *sec = mem_secret(f);
    if (sec) { snprintf(out, cap, "error: not saved: the fact looks like it contains %s. Secrets are never stored in memory; save the fact without the value", sec); return; }
    char today[16]; mem_today(today, sizeof today);
    MemDb db;
    const int have = mem_load(&db) == 0;
    int same = -1;
    if (have) for (int i = 0; i < db.n; i++) if (db.e[i].kind == 0 && !strcmp(db.e[i].fact, f)) { same = i; break; }
    if (same >= 0) {
        if (!strcmp(db.e[same].date, today)) { mem_free(&db); snprintf(out, cap, "ok: already in memory"); return; }
        MemE moved = db.e[same];                       /* mesmo fato em outro dia: renova a data (vai para o fim) */
        snprintf(moved.date, sizeof moved.date, "%s", today);
        memmove(&db.e[same], &db.e[same + 1], (size_t)(db.n - same - 1) * sizeof(MemE));
        db.e[db.n - 1] = moved;
        const int r = mem_save(&db); mem_free(&db);
        snprintf(out, cap, r == 0 ? "ok: already in memory (date refreshed)" : "ok: already in memory");
        return;
    }
    const int sim = have ? mem_similar(&db, f) : -1;
    if (sim >= 0 && !db.trunc) {
        MemE *o = &db.e[sim];
        char nid[9]; mem_id(f, nid);
        char oldtxt[120]; snprintf(oldtxt, sizeof oldtxt, "%.100s", o->fact);
        char oldid[9]; snprintf(oldid, sizeof oldid, "%s", o->id);
        const int pin = o->pinned;
        char *copy = strdup(o->fact);                  /* o texto vive no buffer lido; a copia sobrevive a mem_free */
        o->kind = 1; snprintf(o->by, sizeof o->by, "%s", nid); snprintf(o->sdate, sizeof o->sdate, "%s", today); mem_clean_date(o->date);
        o->fact = copy; o->pinned = 0;
        MemE *x = mem_add(&db);                         /* mem_add pode mover o vetor: o ponteiro `o` nao e mais usado */
        x->kind = 0; snprintf(x->date, sizeof x->date, "%s", today); x->fact = strdup(f); snprintf(x->id, sizeof x->id, "%s", nid); x->pinned = pin;
        const int r = mem_save(&db);
        mem_free(&db);                                  /* (a copia e o texto novo ficam alocados: processo curto, vazamento pequeno e deliberado) */
        if (r != 0) { snprintf(out, cap, "error: cannot write memory"); return; }
        snprintf(out, cap, "ok: updated memory (replaced the earlier fact on the same subject, kept as superseded [%s]: %s)", oldid, oldtxt);
        return;
    }
    mem_free(&db);
    char p[1200];
    mem_path(p, sizeof p);
    mem_mkdir_of(p);
    int nl = 0;
    FILE *r = fopen(p, "rb");
    if (r) { if (fseek(r, -1, SEEK_END) == 0) nl = fgetc(r) != '\n'; fclose(r); }
    FILE *fp = fopen(p, "ab");
    if (!fp) { snprintf(out, cap, "error: cannot write %s", p); return; }
    fprintf(fp, "%s%s\t%s\n", nl ? "\n" : "", today, f);
    fclose(fp);
    mem_chmod(p);
    snprintf(out, cap, "ok: saved to memory");
}

/* ultimos `max` fatos vivos, do mais antigo ao mais novo, no formato "- data: fato"; fatos fixos (#pin) ocupam o lugar dos mais antigos */
static size_t mem_tail(int max, char *out, size_t cap) {
    MemDb db; size_t o = 0;
    out[0] = 0;
    if (mem_load(&db) != 0) return 0;
    int *live = (int *)malloc((size_t)(db.n + 1) * sizeof(int)), nl = 0;
    char *pick = (char *)calloc((size_t)db.n + 1, 1);
    if (!live || !pick) { free(live); free(pick); mem_free(&db); return 0; }
    for (int i = 0; i < db.n; i++) if (db.e[i].kind == 0) live[nl++] = i;
    int left = max;
    for (int k = nl - 1; k >= 0 && left > max - max / 2; k--) if (db.e[live[k]].pinned) { pick[k] = 1; left--; }       /* fixos: ate metade do conjunto */
    for (int k = nl - 1; k >= 0 && left > 0; k--) if (!pick[k]) { pick[k] = 1; left--; }
    for (int k = 0; k < nl; k++) {
        if (!pick[k]) continue;
        if (o + 700 >= cap) break;
        const MemE *x = &db.e[live[k]];
        o += (size_t)snprintf(out + o, cap - o, "- %s: %.400s\n", x->date, x->fact);
    }
    free(live); free(pick); mem_free(&db);
    return o;
}

static void tool_recall(const char *query, char *out, size_t cap) {
    MemDb db;
    if (mem_load(&db) != 0) { snprintf(out, cap, "memory is empty"); return; }
    MemHit h[8];
    const int n = mem_rank(&db, query, 0, h, 8);
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        const MemE *x = &db.e[h[i].idx];
        if (o + 700 < cap) o += (size_t)snprintf(out + o, cap - o, "- %s: %.400s\n", x->date, x->fact);
    }
    if (!out[0]) snprintf(out, cap, "no matching facts in memory");
    mem_free(&db);
}

/* ---- katana memory ---- */
static int mem_match(const MemDb *db, const char *key, int kinds, int *found) {   /* entradas cujo id (ou prefixo de 4+) ou texto casam; devolve quantas */
    int n = 0; const size_t kl = strlen(key);
    for (int i = 0; i < db->n; i++) {
        const MemE *x = &db->e[i];
        if (!((1 << x->kind) & kinds)) continue;
        const int byid = kl >= 4 && kl <= 8 && !strncmp(x->id, key, kl);
        if (byid || (kl >= 3 && ci_find(x->fact, strlen(x->fact), key, kl))) { if (n < 64) found[n] = i; n++; }
    }
    return n;
}
static void mem_lowercase(char *d, const char *s, size_t cap) { size_t n = 0; for (; *s && n < cap - 1; s++) d[n++] = (char)tolower((unsigned char)*s); d[n] = 0; }
static int mem_pick1(const MemDb *db, const char *key, int kinds, int *idx) {   /* exatamente uma entrada; senao explica */
    int f[64]; char lk[256]; mem_lowercase(lk, key, sizeof lk);
    int n = mem_match(db, lk, kinds, f);
    if (n > 1) {                                       /* id exato vence texto */
        for (int i = 0; i < n && i < 64; i++) if (!strcmp(db->e[f[i]].id, lk)) { *idx = f[i]; return 1; }
    }
    if (n == 1) { *idx = f[0]; return 1; }
    if (n == 0) { fprintf(stderr, "memory: nothing matches '%s'\n", key); return 0; }
    fprintf(stderr, "memory: '%s' matches %d facts; use the id:\n", key, n);
    for (int i = 0; i < n && i < 12; i++) fprintf(stderr, "  %s  %s  %.100s\n", db->e[f[i]].id, db->e[f[i]].date, db->e[f[i]].fact);
    return 0;
}
static int memory_cli(int argc, char **argv) {
    const char *cmd = argc >= 1 ? argv[0] : "list";
    int all = 0, purge = 0; char key[600] = ""; size_t kn = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--all")) all = 1;
        else if (!strcmp(argv[i], "--purge")) purge = 1;
        else kn += (size_t)snprintf(key + kn, sizeof key - kn, "%s%s", kn ? " " : "", argv[i]);
    }
    if (!strcmp(cmd, "add") || !strcmp(cmd, "remember")) {
        if (!kn) { fprintf(stderr, "usage: katana memory add <fact>\n"); return 2; }
        char r[1024]; tool_remember(key, r, sizeof r); printf("%s\n", r); return strncmp(r, "error", 5) ? 0 : 1;
    }
    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        puts("katana memory <command>\n  list [--all]         the live facts (id, date); --all adds superseded and forgotten ones\n  show <id|text>       one fact with its history (versions it replaced, who replaced it)\n"
             "  search <query>       ranked search (relevance, recency; superseded facts penalized)\n  add <fact>           same as the remember tool (refuses secrets, updates a fact on the same subject)\n"
             "  forget <id|text> [--purge]  move a fact to the history as forgotten; --purge erases it and its old versions for good\n  pin <id|text> | unpin <id|text>  pinned facts stay in the system prompt\n"
             "  export [--all]       live facts in the old format (date TAB fact); --all prints the whole file\nFile: ~/.katana/memoria.txt (KATANA_MEM overrides), mode 0600.");
        return 0;
    }
    MemDb db;
    if (mem_load(&db) != 0) {
        if (!strcmp(cmd, "list") || !strcmp(cmd, "export") || !strcmp(cmd, "search")) { if (strcmp(cmd, "export")) puts("memory is empty"); return 0; }
        fprintf(stderr, "memory: no memory file yet\n"); return 1;
    }
    int rc = 0;
    if (!strcmp(cmd, "list") || !strcmp(cmd, "ls")) {
        int shown = 0;
        for (int i = 0; i < db.n; i++) {
            const MemE *x = &db.e[i];
            if (x->kind == 0) { printf("%s  %s  %s%s\n", x->id, x->date, x->pinned ? "[pinned] " : "", x->fact); shown++; }
            else if (all && x->kind == 1) { printf("%s  %s  [superseded %s by %s] %s\n", x->id, x->date, x->sdate, x->by, x->fact); shown++; }
            else if (all && x->kind == 2) { printf("%s  %s  [forgotten %s] %s\n", x->id, x->date, x->sdate, x->fact); shown++; }
        }
        if (!shown) puts("memory is empty");
    } else if (!strcmp(cmd, "search")) {
        MemHit h[32]; const int n = kn ? mem_rank(&db, key, 1, h, 20) : 0;
        if (!n) puts("no matching facts in memory");
        for (int i = 0; i < n; i++) { const MemE *x = &db.e[h[i].idx]; printf("%s  %s  score %.2f  %s%s\n", x->id, x->date, h[i].score, x->kind == 1 ? "[superseded] " : "", x->fact); }
    } else if (!strcmp(cmd, "show")) {
        int idx;
        if (!kn || !mem_pick1(&db, key, 0x7, &idx)) rc = 1;
        else {
            const MemE *x = &db.e[idx];
            printf("id:      %s\nstatus:  %s\ndate:    %s\nfact:    %s\n", x->id, x->kind == 0 ? (x->pinned ? "live, pinned" : "live") : x->kind == 1 ? "superseded" : "forgotten", x->date, x->fact);
            { char t[8][48]; const int nt = mem_terms(x->fact, t, 8, NULL); printf("subject: "); for (int i = 0; i < nt; i++) printf("%s%s", i ? " " : "", t[i]); printf("\n"); }
            if (x->kind == 1) printf("replaced on %s by %s\n", x->sdate, x->by);
            if (x->kind == 2) printf("forgotten on %s\n", x->sdate);
            for (int i = 0; i < db.n; i++) if (db.e[i].kind == 1 && !strcmp(db.e[i].by, x->id)) printf("replaced earlier: %s  %s  %s\n", db.e[i].id, db.e[i].date, db.e[i].fact);
        }
    } else if (!strcmp(cmd, "forget") || !strcmp(cmd, "rm")) {
        int idx;
        if (!kn || !mem_pick1(&db, key, purge ? 0x7 : 0x1, &idx)) rc = 1;
        else if (db.trunc) { fprintf(stderr, "memory: file too large to rewrite safely\n"); rc = 1; }
        else {
            char today[16]; mem_today(today, sizeof today);
            char id[9]; snprintf(id, sizeof id, "%s", db.e[idx].id);
            printf("forgot %s  %.100s\n", id, db.e[idx].fact);
            if (purge) {
                db.e[idx].kind = 5;
                for (int again = 1; again; ) { again = 0; for (int i = 0; i < db.n; i++) if (db.e[i].kind == 1) for (int j = 0; j < db.n; j++) if (db.e[j].kind == 5 && j != i && !strcmp(db.e[i].by, db.e[j].id)) { db.e[i].kind = 5; again = 1; break; } }
            } else if (db.e[idx].kind == 0) { db.e[idx].kind = 2; snprintf(db.e[idx].sdate, sizeof db.e[idx].sdate, "%s", today); mem_clean_date(db.e[idx].date); db.e[idx].pinned = 0; }
            if (mem_save(&db) != 0) { fprintf(stderr, "memory: cannot write\n"); rc = 1; }
        }
    } else if (!strcmp(cmd, "pin") || !strcmp(cmd, "unpin")) {
        int idx;
        if (!kn || !mem_pick1(&db, key, 0x1, &idx)) rc = 1;
        else { db.e[idx].pinned = !strcmp(cmd, "pin"); if (mem_save(&db) != 0) { fprintf(stderr, "memory: cannot write\n"); rc = 1; } else printf("%s %s\n", db.e[idx].pinned ? "pinned" : "unpinned", db.e[idx].id); }
    } else if (!strcmp(cmd, "export")) {
        if (all) { for (int i = 0; i < db.n; i++) { const MemE *x = &db.e[i];
            if (x->kind == 0) printf("%s\t%s\n", x->date, x->fact); else if (x->kind == 1) printf("#sup %s %s %s %s %s\n", x->id, x->date, x->sdate, x->by, x->fact);
            else if (x->kind == 2) printf("#del %s %s %s %s\n", x->id, x->date, x->sdate, x->fact); else if (x->kind == 4) printf("%s\n", x->raw); }
            for (int i = 0; i < db.n; i++) if (db.e[i].kind == 0 && db.e[i].pinned) printf("#pin %s\n", db.e[i].id); }
        else for (int i = 0; i < db.n; i++) if (db.e[i].kind == 0) printf("%s\t%s\n", db.e[i].date, db.e[i].fact);
    } else { fprintf(stderr, "memory: unknown command '%s' (katana memory help)\n", cmd); rc = 2; }
    mem_free(&db);
    return rc;
}
