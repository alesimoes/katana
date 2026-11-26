/* ---------- fontes da web: registro, roteador, leitores, verificador e entrega (docs/web-sources.md) ----------
 * Modulo comum ao WebSearch e ao WebFetch:
 *   registro    ONDE: as fontes embutidas (agent/websources.inc) mais ~/.katana/sources.txt do dono (mesma id troca, -id remove)
 *   roteador    quais 2 a 4 fontes servem a uma pergunta: assunto, idioma, pais, se pede dado recente, entidades (regras, sem modelo)
 *   leitores    COMO ler cada resposta: json (com o mapa de campos da fonte), feed (RSS/Atom), html (leitores de agent/web.c), sitemap
 *   verificador tira patrocinados e o que nao cita a pergunta; ordena por palavras x confianca x frescor; o embedding so reordena
 *   entrega     a linha [search: ok|partial|empty, sources: a, b (c failed: motivo), n results] e a origem de cada resultado
 * A busca geral (leitores html) so entra quando ha menos de 2 fontes especificas ou todas voltaram vazias; depois dela, a consulta
 * reduzida as palavras informativas. Fonte que falha e NOMEADA na linha de estado, nunca escondida. Nada aqui foi medido. */
#include "websources.inc"

enum { WT_CODE = 1, WT_AI = 2, WT_NEWS = 4, WT_WORLD = 8, WT_ENC = 16, WT_PAPERS = 32, WT_QA = 64, WT_DOCS = 128, WT_GENERAL = 256 };
static const char *const g_wt_names[] = {"code", "ai", "news", "world-news", "encyclopedia", "papers", "q-and-a", "docs", "general", NULL};
enum { WK_JSON, WK_FEED, WK_HTML, WK_SITEMAP };
static const char *const g_wk_names[] = {"json", "feed", "html", "sitemap", NULL};
enum { WF_HOURS, WF_DAYS, WF_YEARS };
static const char *const g_wf_names[] = {"hours", "days", "years", NULL};

#define WS_MAXSRC 64
#define WS_PICK 6                  /* fontes consultadas numa rodada (2 a 4 especificas + a busca geral) */
#define WS_PER 8                   /* resultados por fonte */
#define WS_FEED_SCAN 80            /* entradas lidas de um feed antes do filtro por palavras */
#define WS_TMO 8                   /* segundos por fonte (curl --max-time) */
#define WS_BUDGET 14.0             /* segundos da busca inteira, com os passos de troca de rota */
#define WS_COS_LOW 0.30f           /* cosseno abaixo disso, sem nenhuma palavra da pergunta: o resultado cai (palpite, nao medido) */
typedef struct {
    char id[40]; unsigned topics; char country[4], lang[4]; int kind; char tmpl[700]; int fresh, trust, rpm; char map[900];
} WsSource;
typedef struct { char id[40]; double t0, until; int n; } WsRate;
static WsSource g_wsr[WS_MAXSRC];
static int g_nws, g_ws_loaded, g_ws_bad;
static time_t g_ws_mtime;
static WsRate g_ws_rate[WS_MAXSRC];
static int g_nws_rate;

/* ---------- registro ---------- */
static void ws_trim(char *s) {
    size_t n = strlen(s), a = 0;
    while (a < n && isspace((unsigned char)s[a])) a++;
    while (n > a && isspace((unsigned char)s[n - 1])) n--;
    memmove(s, s + a, n - a); s[n - a] = 0;
}
static int ws_name_ix(const char *const *names, const char *s) { for (int i = 0; names[i]; i++) if (!strcmp(names[i], s)) return i; return -1; }
/* uma linha do registro; 1 fonte valida, 0 comentario/vazia, -1 invalida; *remove = 1 para "-id" */
static int ws_parse_line(const char *line, WsSource *s, int *remove) {
    char buf[2600], *f[10]; int nf = 0;
    *remove = 0;
    snprintf(buf, sizeof buf, "%s", line);
    char *h = strchr(buf, '#');
    if (h && (h == buf || isspace((unsigned char)h[-1]))) *h = 0;        /* '#' no meio de um endereco nao e comentario */
    ws_trim(buf);
    if (!buf[0]) return 0;
    if (buf[0] == '-' && !strchr(buf, '|')) { memset(s, 0, sizeof *s); snprintf(s->id, sizeof s->id, "%s", buf + 1); ws_trim(s->id); *remove = 1; return s->id[0] ? 1 : -1; }
    char *p = buf;
    while (nf < 10) { f[nf++] = p; char *b = strchr(p, '|'); if (!b || nf == 10) break; *b = 0; p = b + 1; }
    if (nf < 9) return -1;
    for (int i = 0; i < nf; i++) ws_trim(f[i]);
    memset(s, 0, sizeof *s);
    if (!f[0][0] || strlen(f[0]) >= sizeof s->id) return -1;
    snprintf(s->id, sizeof s->id, "%s", f[0]);
    for (char *t = f[1]; *t; ) {
        char w[32]; size_t l = strcspn(t, ","); snprintf(w, sizeof w, "%.*s", (int)(l < 31 ? l : 31), t); ws_trim(w);
        const int k = ws_name_ix(g_wt_names, w);
        if (k < 0) return -1;
        s->topics |= 1u << k;
        t += l; if (*t == ',') t++;
    }
    snprintf(s->country, sizeof s->country, "%.3s", f[2]); snprintf(s->lang, sizeof s->lang, "%.3s", f[3]);
    if ((s->kind = ws_name_ix(g_wk_names, f[4])) < 0) return -1;
    if (ci_ncmp(f[5], "http://", 7) && ci_ncmp(f[5], "https://", 8)) return -1;
    if (strlen(f[5]) >= sizeof s->tmpl) return -1;
    snprintf(s->tmpl, sizeof s->tmpl, "%s", f[5]);
    if ((s->fresh = ws_name_ix(g_wf_names, f[6])) < 0) return -1;
    s->trust = atoi(f[7]); s->rpm = atoi(f[8]);
    if (s->trust < 1 || s->trust > 5 || s->rpm < 1 || !s->topics) return -1;
    if (nf == 10) snprintf(s->map, sizeof s->map, " %.896s ", f[9]);
    return 1;
}
static void ws_apply(const char *text) {
    const char *p = text;
    while (*p) {
        const size_t l = strcspn(p, "\n");
        char line[2600]; snprintf(line, sizeof line, "%.*s", (int)(l < 2599 ? l : 2599), p);
        p += l; if (*p) p++;
        WsSource s; int rm = 0; const int r = ws_parse_line(line, &s, &rm);
        if (r < 0) { g_ws_bad++; continue; }
        if (r == 0) continue;
        int at = -1;
        for (int i = 0; i < g_nws; i++) if (!strcmp(g_wsr[i].id, s.id)) at = i;
        if (rm) { if (at >= 0) { memmove(&g_wsr[at], &g_wsr[at + 1], sizeof g_wsr[0] * (size_t)(g_nws - at - 1)); g_nws--; } continue; }
        if (at >= 0) g_wsr[at] = s;
        else if (g_nws < WS_MAXSRC) g_wsr[g_nws++] = s;
    }
}
static void ws_owner_path(char *out, size_t cap) {
    const char *e = getenv("KATANA_SOURCES");
    if (e && *e) { snprintf(out, cap, "%s", e); return; }
    char h[1024]; agent_home(h, sizeof h); snprintf(out, cap, "%s/.katana/sources.txt", h);
}
/* carrega (ou recarrega, se o arquivo do dono mudou) o registro */
static void ws_load(void) {
    char path[1300]; struct stat st;
    ws_owner_path(path, sizeof path);
    const time_t mt = stat(path, &st) == 0 ? st.st_mtime : 0;
    if (g_ws_loaded && mt == g_ws_mtime) return;
    g_nws = 0; g_ws_bad = 0;
    ws_apply(g_ws_builtin);
    if (mt) {
        FILE *f = fopen(path, "rb");
        if (f) {
            char *b = (char *)malloc(262145); size_t n = b ? fread(b, 1, 262144, f) : 0;
            fclose(f);
            if (b) { b[n] = 0; ws_apply(b); free(b); }
        }
        if (g_ws_bad) { char m[160]; snprintf(m, sizeof m, "%d invalid line(s) ignored", g_ws_bad); agent_log("sources", path, m); }
    }
    g_ws_mtime = mt; g_ws_loaded = 1;
}
static WsRate *ws_rate(const char *id) {
    for (int i = 0; i < g_nws_rate; i++) if (!strcmp(g_ws_rate[i].id, id)) return &g_ws_rate[i];
    if (g_nws_rate >= WS_MAXSRC) return NULL;
    WsRate *r = &g_ws_rate[g_nws_rate++]; memset(r, 0, sizeof *r); snprintf(r->id, sizeof r->id, "%s", id);
    return r;
}
/* pode gastar uma requisicao agora? (limite por minuto e pausa depois de um 429); conta a requisicao quando sim */
static int ws_rate_take(const WsSource *s, char *why, size_t cap) {
    WsRate *r = ws_rate(s->id);
    const double t = now_sec();
    if (!r) return 1;
    if (r->until > t) { snprintf(why, cap, "paused %.0f s after the site limited requests", r->until - t); return 0; }
    if (t - r->t0 >= 60.0) { r->t0 = t; r->n = 0; }
    if (r->n >= s->rpm) { snprintf(why, cap, "limit of %d requests per minute reached", s->rpm); return 0; }
    r->n++;
    return 1;
}
static void ws_rate_pause(const WsSource *s, double sec) { WsRate *r = ws_rate(s->id); if (r) r->until = now_sec() + sec; }

/* valor de uma chave do mapa (" chave=valor "); 0 se nao ha */
static int ws_map(const WsSource *s, const char *key, char *out, size_t cap) {
    char pat[40]; snprintf(pat, sizeof pat, " %s=", key);
    const char *p = strstr(s->map, pat);
    out[0] = 0;
    if (!p) return 0;
    p += strlen(pat);
    const size_t l = strcspn(p, " ");
    snprintf(out, cap, "%.*s", (int)l, p);
    return out[0] != 0;
}

/* ---------- texto: dobra de acentos, palavras da pergunta, casamento ---------- */
/* minusculas ASCII e letras latinas acentuadas (UTF-8 de 2 bytes C3 xx) sem acento */
static size_t ws_fold(const char *s, char *o, size_t cap) {
    static const char m[] = "aaaaaaaceeeeiiiidnooooo/ouuuuyty";      /* C3 A0..BF (e C3 80..9F, mesmas letras) */
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n + 1 < cap; p++) {
        if (*p == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) { o[n++] = m[(p[1] - 0x80) & 31]; p++; }
        else if (*p < 0x80) o[n++] = (char)tolower(*p);
        else o[n++] = ' ';
    }
    o[n] = 0;
    return n;
}
static const char *const g_ws_stop[] = {"a", "o", "as", "os", "e", "de", "do", "da", "dos", "das", "em", "no", "na", "nos", "nas", "um", "uma", "uns", "umas", "que", "qual", "quais",
    "como", "para", "pra", "por", "com", "sem", "se", "ao", "aos", "ou", "mais", "menos", "sobre", "entre", "ate", "esta", "este", "esse", "essa", "isso", "isto", "eh", "sao", "foi",
    "ser", "tem", "ter", "ha", "me", "meu", "minha", "voce", "eu", "nao", "sim", "the", "of", "and", "or", "to", "in", "on", "for", "with", "by", "is", "are", "was", "were", "be",
    "an", "at", "from", "about", "what", "which", "who", "how", "why", "when", "where", "does", "do", "did", "can", "i", "my", "me", "it", "its", "this", "that", "these", "those",
    "quem", "quando", "onde", "porque", "pesquise", "busque", "procure", "search", "find", "web", "internet", "please", "favor", NULL};
/* palavras gerais de noticia: entram na classificacao mas nao filtram as entradas de um feed */
static const char *const g_ws_newsgen[] = {"noticia", "noticias", "news", "ultimas", "ultima", "latest", "hoje", "today", "ontem", "yesterday", "agora", "now", "recentes", "recente",
    "recent", "manchetes", "headlines", "brasil", "brazil", "mundo", "world", "semana", "week", NULL};
static int ws_inlist(const char *const *l, const char *w) { for (int i = 0; l[i]; i++) if (!strcmp(l[i], w)) return 1; return 0; }
/* palavras informativas (dobradas, sem as vazias); devolve quantas */
static int ws_words(const char *q, char w[][32], int max) {
    char f[1200]; ws_fold(q, f, sizeof f);
    int n = 0;
    for (char *p = f; *p && n < max; ) {
        while (*p && !(isalnum((unsigned char)*p) || *p == '+' || *p == '#')) p++;
        char *b = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '+' || *p == '#' || ((*p == '.' || *p == '-' || *p == '_') && isalnum((unsigned char)p[1])))) p++;
        const size_t l = (size_t)(p - b);
        if (!l || l > 31) continue;
        char t[32]; memcpy(t, b, l); t[l] = 0;
        if ((l < 2 && !isdigit((unsigned char)t[0])) || ws_inlist(g_ws_stop, t)) continue;
        int dup = 0; for (int i = 0; i < n; i++) if (!strcmp(w[i], t)) dup = 1;
        if (!dup) { memcpy(w[n], t, l + 1); n++; }
    }
    return n;
}
/* a palavra w aparece no texto dobrado t no comeco de uma palavra (prefixo vale para palavras de 4 letras ou mais) */
static int ws_has(const char *t, const char *w) {
    const size_t l = strlen(w);
    for (const char *p = t; (p = strstr(p, w)); p++) {
        if (p > t && isalnum((unsigned char)p[-1])) continue;
        if (l >= 4 || !isalnum((unsigned char)p[l])) return 1;
    }
    return 0;
}

/* ---------- classificacao da pergunta (assunto, idioma, pais, recencia, entidades) ---------- */
typedef struct { unsigned topics; char lang[4], country[4]; int recent; char ent[6][80]; int nent; char w[16][32]; int nw; char why[200]; } WsAsk;
static const char *const g_wsk_code[] = {"code", "codigo", "repo", "repository", "repositorio", "library", "biblioteca", "lib", "function", "funcao", "compile", "compilar", "compiler",
    "compilador", "api", "sdk", "npm", "pip", "crate", "cargo", "package", "pacote", "git", "python", "rust", "javascript", "typescript", "java", "golang", "c++", "linux", "kernel",
    "script", "bash", "shell", "framework", "github", "source", "plugin", "cli", "build", "debug", "segfault", "exception", "stack", "error", "erro", "bug", "install", "instalar",
    "docker", "json", "regex", "sql", "database", "banco", "http", "server", "servidor", "cuda", "vulkan", "shader", "opengl", "gpu", "driver", "makefile", "cmake", "gcc", "clang", NULL};
static const char *const g_wsk_ai[] = {"llm", "llms", "model", "models", "modelo", "modelos", "weights", "pesos", "gguf", "safetensors", "transformer", "transformers", "embedding",
    "embeddings", "quantization", "quantizacao", "quantized", "fine", "tuning", "finetune", "lora", "checkpoint", "dataset", "datasets", "benchmark", "inference", "inferencia", "token",
    "tokens", "tokenizer", "attention", "moe", "experts", "neural", "ia", "ai", "machine", "learning", "aprendizado", "diffusion", "agent", "agents", "agente", "agentes", "rag", NULL};
static const char *const g_wsk_news[] = {"noticia", "noticias", "news", "hoje", "ontem", "today", "yesterday", "eleicao", "eleicoes", "election", "governo", "government", "presidente",
    "president", "ministro", "minister", "congresso", "senado", "camara", "stf", "policia", "police", "economia", "economy", "inflacao", "inflation", "dolar", "juros", "pib", "mercado",
    "bolsa", "greve", "acidente", "chuva", "enchente", "temporal", "jogo", "campeonato", "manchetes", "headlines", "ultimas", "aconteceu", "happened", "crise", "crisis", NULL};
static const char *const g_wsk_world[] = {"mundo", "world", "internacional", "international", "guerra", "war", "onu", "un", "otan", "nato", "europa", "europe", "china", "russia", "ucrania",
    "ukraine", "israel", "gaza", "eua", "usa", "estados", "global", "cupula", "summit", "diplomacia", "sancoes", "sanctions", NULL};
static const char *const g_wsk_enc[] = {"historia", "history", "biografia", "biography", "definicao", "definition", "significado", "meaning", "capital", "populacao", "population",
    "fundado", "founded", "nasceu", "born", "morreu", "died", "pais", "country", "cidade", "city", "conceito", "concept", "teoria", "theory", NULL};
static const char *const g_wsk_papers[] = {"paper", "papers", "artigo", "artigos", "preprint", "estudo", "study", "studies", "pesquisa", "research", "survey", "abstract", "arxiv",
    "citation", "citacao", "publicacao", "publication", "journal", "revista", "tese", "thesis", NULL};
static const char *const g_wsk_qa[] = {"erro", "error", "exception", "segfault", "crash", "falha", "fails", "failing", "warning", "undefined", "traceback", "fix", "consertar", "resolver",
    "solve", "problema", "problem", "issue", "stackoverflow", NULL};
static const char *const g_wsk_docs[] = {"documentation", "documentacao", "docs", "manual", "reference", "referencia", "tutorial", "guide", "guia", "spec", "specification",
    "especificacao", "changelog", "release", "notes", NULL};
static const char *const g_wsk_recent[] = {"hoje", "ontem", "agora", "atual", "atualmente", "recente", "recentes", "ultima", "ultimas", "ultimo", "ultimos", "noticia", "noticias", "news",
    "latest", "today", "yesterday", "current", "currently", "recent", "now", "semana", "week", "cotacao", "preco", "price", "placar", "score", "resultado", NULL};
static const char *const g_wsk_pt[] = {"de", "do", "da", "que", "como", "para", "nao", "com", "uma", "em", "os", "as", "qual", "quais", "sobre", "noticias", "ultimas", "voce", "isso",
    "esta", "sao", "pra", "tem", "foi", "hoje", "ontem", NULL};
static const char *const g_wsk_en[] = {"the", "of", "how", "what", "is", "to", "for", "with", "and", "latest", "which", "does", "are", "was", "news", "today", "why", "who", "can", NULL};
static const char *const g_wsk_br[] = {"brasil", "brazil", "brasileiro", "brasileira", "brasileiros", "paulo", "rio", "brasilia", "stf", "pix", "sus", "inss", "receita", "petrobras",
    "minas", "bahia", "parana", "pernambuco", "ceara", "nordeste", "amazonia", "reais", "lula", "planalto", NULL};
static const char *const g_wsk_us[] = {"eua", "usa", "america", "american", "americano", "washington", "york", "california", "texas", "congress", "senate", "biden", "trump", "fed", NULL};

static int ws_count(const char *const *list, char w[][32], int n) { int c = 0; for (int i = 0; i < n; i++) if (ws_inlist(list, w[i])) c++; return c; }
static void ws_classify(const char *q, WsAsk *a) {
    memset(a, 0, sizeof *a);
    char all[48][32];
    int na = 0;
    {   /* todas as palavras (com as vazias): idioma e pais olham tambem as palavras curtas */
        char f[1200]; ws_fold(q, f, sizeof f);
        for (char *p = f; *p && na < 48; ) {
            while (*p && !(isalnum((unsigned char)*p) || *p == '+')) p++;
            char *b = p; while (*p && (isalnum((unsigned char)*p) || *p == '+')) p++;
            const size_t l = (size_t)(p - b);
            if (l && l < 32) { memcpy(all[na], b, l); all[na][l] = 0; na++; }
        }
    }
    a->nw = ws_words(q, a->w, 16);
    int acc = 0;
    for (const unsigned char *p = (const unsigned char *)q; *p; p++) if (*p == 0xC3 && p[1] && strchr("\xA1\xA2\xA3\xA7\xA9\xAA\xAD\xB3\xB4\xB5\xBA", p[1])) acc++;
    const int pt = ws_count(g_wsk_pt, all, na) + acc * 2, en = ws_count(g_wsk_en, all, na);
    snprintf(a->lang, sizeof a->lang, "%s", pt > en ? "pt" : en > pt ? "en" : "*");
    const int br = ws_count(g_wsk_br, all, na), us = ws_count(g_wsk_us, all, na) + (strstr(q, "Estados Unidos") || strstr(q, "United States") ? 1 : 0);
    snprintf(a->country, sizeof a->country, "%s", br > us ? "br" : us > br ? "us" : !strcmp(a->lang, "pt") ? "br" : "*");
    static const struct { const char *const *l; unsigned t; } T[] = { {g_wsk_code, WT_CODE}, {g_wsk_ai, WT_AI}, {g_wsk_news, WT_NEWS}, {g_wsk_world, WT_WORLD}, {g_wsk_enc, WT_ENC},
        {g_wsk_papers, WT_PAPERS}, {g_wsk_qa, WT_QA}, {g_wsk_docs, WT_DOCS} };
    for (size_t i = 0; i < sizeof T / sizeof *T; i++) if (ws_count(T[i].l, all, na) > 0) a->topics |= T[i].t;
    a->recent = ws_count(g_wsk_recent, all, na) > 0;
    {   /* ano corrente ou o anterior citado: pede dado recente */
        const int y = 1970 + (int)(time(NULL) / 31556952);
        for (int i = 0; i < na; i++) { const int v = atoi(all[i]); if (strlen(all[i]) == 4 && (v == y || v == y - 1)) a->recent = 1; }
    }
    /* entidades: dono/nome (repositorio ou modelo), nome com versao, texto entre aspas, nomes proprios no meio da frase */
    const char *p = q;
    while (*p && a->nent < 6) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '"') { const char *e = strchr(p + 1, '"'); if (e && e > p + 1) { snprintf(a->ent[a->nent++], 80, "%.*s", (int)(e - p - 1 < 79 ? e - p - 1 : 79), p + 1); p = e + 1; continue; } }
        const char *b = p; while (*p && !isspace((unsigned char)*p)) p++;
        char t[80]; const size_t l = (size_t)(p - b); if (!l || l >= sizeof t) continue;
        memcpy(t, b, l); t[l] = 0;
        while (strlen(t) && strchr(",.;:?!()", t[strlen(t) - 1])) t[strlen(t) - 1] = 0;
        const char *sl = strchr(t, '/');
        int dig = 0, alp = 0; for (const char *c = t; *c; c++) { if (isdigit((unsigned char)*c)) dig = 1; else if (isalpha((unsigned char)*c)) alp = 1; }
        if (sl && sl > t && sl[1] && !strchr(sl + 1, '/') && !strstr(t, "://")) { snprintf(a->ent[a->nent++], 80, "%s", t); a->topics |= WT_CODE | WT_AI; }
        else if (dig && alp && strlen(t) >= 3) { snprintf(a->ent[a->nent++], 80, "%s", t); }
        else if (b != q && isupper((unsigned char)t[0]) && strlen(t) >= 3) { snprintf(a->ent[a->nent++], 80, "%s", t); }
    }
    if (a->topics & WT_QA) a->topics |= WT_CODE;
    if (!a->topics) a->topics = WT_GENERAL | WT_ENC;
    size_t o = 0;
    for (int i = 0; g_wt_names[i]; i++) if (a->topics & (1u << i)) o += (size_t)snprintf(a->why + o, sizeof a->why - o, "%s%s", o ? "," : "topics ", g_wt_names[i]);
    snprintf(a->why + o, sizeof a->why - o, "; lang %s; country %s%s", a->lang, a->country, a->recent ? "; recent" : "");
}

/* ---------- roteador ---------- */
static int ws_has_query(const WsSource *s) { return strstr(s->tmpl, "%s") != NULL; }
/* pontos de uma fonte para a pergunta; < 0 nao serve */
static int ws_fit(const WsSource *s, const WsAsk *a) {
    if (s->topics == WT_GENERAL) return -1;                          /* a busca geral entra so como ultimo passo */
    const unsigned m = s->topics & a->topics;
    if (!m) return -1;
    if (!ws_has_query(s) && !(m & (WT_NEWS | WT_WORLD))) return -1;   /* feed sem consulta so para noticia */
    int bits = 0; for (unsigned x = m; x; x &= x - 1) bits++;
    int sc = bits * 10 + s->trust * 2;
    if (strcmp(s->lang, "*")) { if (!strcmp(s->lang, a->lang)) sc += 4; else if (strcmp(a->lang, "*")) sc -= 6; } else sc += 1;
    if (strcmp(s->country, "*")) { if (!strcmp(s->country, a->country)) sc += 4; else if (strcmp(a->country, "*")) sc -= 8; else sc -= 2; }
    if (a->recent) sc += s->fresh == WF_HOURS ? 6 : s->fresh == WF_DAYS ? 2 : -4;
    if (!ws_has_query(s) && a->nw == 0) sc -= 2;
    return sc;
}
/* escolhe ate max fontes especificas (indices em g_wsr), melhores primeiro */
static int ws_route(const WsAsk *a, int *pick, int max) {
    int sc[WS_MAXSRC], n = 0;
    for (int i = 0; i < g_nws; i++) { const int f = ws_fit(&g_wsr[i], a); if (f > 0) { pick[n] = i; sc[n] = f; n++; if (n >= WS_MAXSRC) break; } }
    for (int i = 1; i < n; i++) { int x = pick[i], y = sc[i], j = i - 1; while (j >= 0 && sc[j] < y) { pick[j + 1] = pick[j]; sc[j + 1] = sc[j]; j--; } pick[j + 1] = x; sc[j + 1] = y; }
    int feeds = 0, k = 0;
    for (int i = 0; i < n && k < max; i++) {
        if (sc[i] < sc[0] - 12) break;                               /* bem abaixo da melhor: so ocuparia tempo e orcamento */                         /* no maximo 2 feeds sem consulta: o resto vem de fontes que buscam */
        if (!ws_has_query(&g_wsr[pick[i]]) && ++feeds > 2) continue;
        pick[k++] = pick[i];
    }
    return k;
}

/* ---------- JSON pequeno: pular valor, chave de objeto, caminho, texto ---------- */
static const char *jx_ws(const char *p, const char *e) { while (p < e && isspace((unsigned char)*p)) p++; return p; }
static const char *jx_skip(const char *p, const char *e) {
    p = jx_ws(p, e);
    if (p >= e) return NULL;
    if (*p == '"') { for (p++; p < e; p++) { if (*p == '\\') p++; else if (*p == '"') return p + 1; } return NULL; }
    if (*p == '{' || *p == '[') {
        int d = 0;
        for (; p < e; p++) {
            if (*p == '"') { for (p++; p < e && *p != '"'; p++) if (*p == '\\') p++; continue; }
            if (*p == '{' || *p == '[') d++;
            else if ((*p == '}' || *p == ']') && --d == 0) return p + 1;
        }
        return NULL;
    }
    while (p < e && !strchr(",}] \t\r\n", *p)) p++;
    return p;
}
/* membro `key` do objeto em o ("*" = o primeiro membro); devolve o comeco do valor ou NULL */
static const char *jx_key(const char *o, const char *e, const char *key) {
    o = jx_ws(o, e);
    if (o >= e || *o != '{') return NULL;
    const size_t kl = strlen(key);
    const char *p = o + 1;
    for (;;) {
        p = jx_ws(p, e);
        if (p >= e || *p == '}' || *p != '"') return NULL;
        const char *ks = p + 1, *ke = jx_skip(p, e);
        if (!ke) return NULL;
        const int hit = !strcmp(key, "*") || ((size_t)(ke - 1 - ks) == kl && !memcmp(ks, key, kl));
        p = jx_ws(ke, e);
        if (p >= e || *p != ':') return NULL;
        p = jx_ws(p + 1, e);
        if (hit) return p;
        p = jx_skip(p, e);
        if (!p) return NULL;
        p = jx_ws(p, e);
        if (p < e && *p == ',') p++;
    }
}
/* elemento i de um array */
static const char *jx_at(const char *a, const char *e, int i) {
    a = jx_ws(a, e);
    if (a >= e || *a != '[') return NULL;
    const char *p = jx_ws(a + 1, e);
    for (int k = 0; p < e && *p != ']'; k++) {
        if (k == i) return p;
        p = jx_skip(p, e);
        if (!p) return NULL;
        p = jx_ws(p, e);
        if (p < e && *p == ',') p = jx_ws(p + 1, e);
    }
    return NULL;
}
/* caminho com pontos ("query.search", "package.links.npm", "query.pages.*.extract"; "." ou "" = o proprio valor) */
static const char *jx_path(const char *v, const char *e, const char *path) {
    if (!path[0] || !strcmp(path, ".")) return jx_ws(v, e);
    char seg[80];
    while (v && *path) {
        const size_t l = strcspn(path, ".");
        snprintf(seg, sizeof seg, "%.*s", (int)(l < 79 ? l : 79), path);
        path += l; if (*path == '.') path++;
        v = jx_ws(v, e);
        if (v < e && *v == '[') v = jx_at(v, e, isdigit((unsigned char)seg[0]) ? atoi(seg) : 0);
        else v = jx_key(v, e, seg);
    }
    return v;
}
static void jx_utf8(unsigned cp, char *o, size_t *n, size_t cap) {
    if (*n + 5 >= cap) return;
    if (cp < 0x80) o[(*n)++] = (char)cp;
    else if (cp < 0x800) { o[(*n)++] = (char)(0xC0 | (cp >> 6)); o[(*n)++] = (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { o[(*n)++] = (char)(0xE0 | (cp >> 12)); o[(*n)++] = (char)(0x80 | ((cp >> 6) & 63)); o[(*n)++] = (char)(0x80 | (cp & 63)); }
    else { o[(*n)++] = (char)(0xF0 | (cp >> 18)); o[(*n)++] = (char)(0x80 | ((cp >> 12) & 63)); o[(*n)++] = (char)(0x80 | ((cp >> 6) & 63)); o[(*n)++] = (char)(0x80 | (cp & 63)); }
}
/* texto de um valor: string decodificada; numero e booleano como estao; null, objeto e array dao "" */
static void jx_text(const char *v, const char *e, char *o, size_t cap) {
    size_t n = 0;
    o[0] = 0;
    if (!v || (v = jx_ws(v, e)) >= e) return;
    if (*v == '"') {
        for (const char *p = v + 1; p < e && *p != '"' && n + 6 < cap; p++) {
            if (*p != '\\') { o[n++] = *p; continue; }
            if (++p >= e) break;
            switch (*p) {
            case 'n': o[n++] = '\n'; break; case 't': o[n++] = '\t'; break; case 'r': break;
            case 'b': case 'f': o[n++] = ' '; break;
            case 'u': {
                if (p + 4 >= e) break;
                char h[5] = {p[1], p[2], p[3], p[4], 0}; unsigned cp = (unsigned)strtoul(h, NULL, 16); p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && p + 6 < e && p[1] == '\\' && p[2] == 'u') {
                    char h2[5] = {p[3], p[4], p[5], p[6], 0}; const unsigned lo = (unsigned)strtoul(h2, NULL, 16);
                    if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                }
                jx_utf8(cp, o, &n, cap); break; }
            default: o[n++] = *p;
            }
        }
        o[n] = 0;
        return;
    }
    if (*v == '{' || *v == '[' || !strncmp(v, "null", 4)) return;
    const char *q = jx_skip(v, e);
    if (q) snprintf(o, cap, "%.*s", (int)(q - v < (long)cap - 1 ? q - v : (long)cap - 1), v);
}

/* ---------- datas ---------- */
static long ws_days_civil(int y, int m, int d) {          /* dias desde 1970-01-01 (calendario gregoriano) */
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400), doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1), doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
/* ISO (2026-11-25T10:00:00Z), RFC 822 (Wed, 25 Nov 2026 10:00:00 GMT) ou segundos desde 1970; 0 se nao entendeu */
static long long ws_date(const char *s) {
    static const char *const mon[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    while (*s == ' ') s++;
    int y = 0, mo = 0, d = 0, hh = 0, mi = 0, ss = 0;
    if (isdigit((unsigned char)s[0]) && strlen(s) >= 9 && strspn(s, "0123456789") >= 9) { const long long v = atoll(s); return v > 100000000LL ? v : 0; }
    if (sscanf(s, "%4d-%2d-%2d", &y, &mo, &d) == 3) { const char *t = s + 10; if (*t == 'T' || *t == ' ') sscanf(t + 1, "%2d:%2d:%2d", &hh, &mi, &ss); }
    else {
        const char *c = strchr(s, ','); c = c ? c + 1 : s;
        char m3[8] = "";
        if (sscanf(c, "%d %3s %d %d:%d:%d", &d, m3, &y, &hh, &mi, &ss) < 3) return 0;
        for (int i = 0; i < 12; i++) if (!ci_ncmp(m3, mon[i], 3)) mo = i + 1;
        if (y < 100) y += 2000;
    }
    if (y < 1990 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    return (long long)ws_days_civil(y, mo, d) * 86400 + hh * 3600 + mi * 60 + ss;
}
static void ws_hit_date(Hit *h, const char *raw) {
    const long long t = ws_date(raw);
    if (!t) return;
    const time_t tt = (time_t)t; struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    strftime(h->date, sizeof h->date, "%Y-%m-%d", &g);
    const long long now = (long long)time(NULL);
    h->age = now > t ? (long)((now - t) / 3600) : 0;
}

/* ---------- leitores ---------- */
/* {caminho} do item (JSON) ou {1} {2} (pedacos de um endereco) num modelo; enc: codifica o valor para endereco. 0 se algum ficou vazio */
static int ws_expand(const char *tmpl, const char *it, const char *e, char cap_[][300], int ncap, int enc, char *out, size_t cap) {
    size_t n = 0;
    for (const char *p = tmpl; *p && n + 1 < cap; ) {
        if (*p != '{') { out[n++] = *p++; continue; }
        const char *b = strchr(p, '}');
        if (!b) { out[n++] = *p++; continue; }
        char key[80], val[600]; snprintf(key, sizeof key, "%.*s", (int)(b - p - 1 < 79 ? b - p - 1 : 79), p + 1);
        p = b + 1;
        val[0] = 0;
        if (isdigit((unsigned char)key[0])) { const int k = atoi(key) - 1; if (k >= 0 && k < ncap) snprintf(val, sizeof val, "%s", cap_[k]); }
        else if (it) jx_text(jx_path(it, e, key), e, val, sizeof val);
        if (!val[0]) return 0;
        for (const unsigned char *v = (const unsigned char *)val; *v && n + 4 < cap; v++) {
            if (!enc || isalnum(*v) || strchr("-._~/:", *v)) out[n++] = (char)*v;
            else n += (size_t)snprintf(out + n, cap - n, "%%%02X", *v);
        }
    }
    out[n] = 0;
    return 1;
}
/* um campo do mapa num item: alternativas separadas por ',' (a primeira que der valor); em snip, partes com '+' viram "nome: valor" */
static void ws_field(const WsSource *s, const char *key, const char *it, const char *e, int enc, char *out, size_t cap) {
    char spec[400]; out[0] = 0;
    if (!ws_map(s, key, spec, sizeof spec)) return;
    for (char *a = spec; *a && !out[0]; ) {
        const size_t l = strcspn(a, ",");
        char alt[400]; snprintf(alt, sizeof alt, "%.*s", (int)l, a);
        a += l; if (*a == ',') a++;
        if (strchr(alt, '{')) { if (!ws_expand(alt, it, e, NULL, 0, enc, out, cap)) out[0] = 0; continue; }
        if (strchr(alt, '+')) {
            size_t n = 0;
            for (char *pp = alt; *pp; ) {
                const size_t pl = strcspn(pp, "+"); char part[80], v[200];
                snprintf(part, sizeof part, "%.*s", (int)(pl < 79 ? pl : 79), pp); pp += pl; if (*pp == '+') pp++;
                jx_text(jx_path(it, e, part), e, v, sizeof v);
                if (v[0] && n + 4 < cap) n += (size_t)snprintf(out + n, cap - n, "%s%s: %s", n ? ", " : "", part, v);
            }
            continue;
        }
        jx_text(jx_path(it, e, alt), e, out, cap);
    }
}
/* leitor json: items=<caminho do array>; title, url, snip, date = campos do mapa. -1 se a resposta nao tem o formato esperado */
static int ws_read_json(const WsSource *s, const char *raw, size_t n, Hit *out, int max) {
    char ip[200];
    if (!ws_map(s, "items", ip, sizeof ip)) snprintf(ip, sizeof ip, ".");
    const char *e = raw + n, *arr = jx_path(raw, e, ip);
    if (!arr || *jx_ws(arr, e) != '[') return -1;
    static char title[600], url[1400], snip[2000], date[80], t2[300], s2[450];
    int k = 0;
    for (int i = 0; k < max; i++) {
        const char *it = jx_at(arr, e, i);
        if (!it) break;
        const char *ie = jx_skip(it, e);
        if (!ie) break;
        ws_field(s, "title", it, ie, 0, title, sizeof title);
        ws_field(s, "url", it, ie, 1, url, sizeof url);
        ws_field(s, "snip", it, ie, 0, snip, sizeof snip);
        ws_field(s, "date", it, ie, 0, date, sizeof date);
        html_snip(title, title + strlen(title), t2, sizeof t2);       /* alguns campos vem com marcacao (<span class=...>) e entidades */
        html_snip(snip, snip + strlen(snip), s2, sizeof s2);
        if (hit_set(&out[k], t2, url, s2)) { if (date[0]) ws_hit_date(&out[k], date); k++; }
    }
    return k;
}
/* texto da tag (primeira ocorrencia entre s e e): <tag ...>texto</tag>, sem CDATA; o nome tem de terminar em '>' ou espaco */
static int ws_xml_tag(const char *s, const char *e, const char *tag, char *out, size_t cap) {
    const size_t tl = strlen(tag);
    out[0] = 0;
    for (const char *p = s; p && p < e; p++) {
        p = (const char *)memchr(p, '<', (size_t)(e - p));
        if (!p) return 0;
        if ((size_t)(e - p) < tl + 2 || strncmp(p + 1, tag, tl) || !(p[1 + tl] == '>' || p[1 + tl] == ' ' || p[1 + tl] == '/')) continue;
        const char *gt = (const char *)memchr(p, '>', (size_t)(e - p));
        if (!gt) return 0;
        if (gt[-1] == '/') { out[0] = 0; return 1; }
        char close[48]; snprintf(close, sizeof close, "</%s>", tag);
        const char *c = strstr(gt + 1, close);
        if (!c || c > e) return 0;
        const char *a = gt + 1, *b = c;
        while (a < b && isspace((unsigned char)*a)) a++;
        if (!strncmp(a, "<![CDATA[", 9)) { a += 9; const char *ce = strstr(a, "]]>"); if (ce && ce < b) b = ce; }
        static char tmp[8300], tmp2[8300];
        html_snip(a, b, tmp, sizeof tmp);
        if (strchr(tmp, '<') || strchr(tmp, '&')) { html_snip(tmp, tmp + strlen(tmp), tmp2, sizeof tmp2); snprintf(out, cap, "%s", tmp2); }   /* HTML escapado dentro do feed */
        else snprintf(out, cap, "%s", tmp);
        ws_trim(out);
        return 1;
    }
    return 0;
}
/* atributo href do primeiro <link ...> com rel ausente ou alternate (Atom) */
static int ws_atom_link(const char *s, const char *e, char *out, size_t cap) {
    out[0] = 0;
    for (const char *p = s; (p = strstr(p, "<link")) && p < e; p += 5) {
        const char *gt = strchr(p, '>');
        if (!gt || gt > e) break;
        char tag[1400]; snprintf(tag, sizeof tag, "%.*s", (int)(gt - p < 1399 ? gt - p : 1399), p);
        const char *h = strstr(tag, "href=\"");
        if (!h) continue;
        const char *r = strstr(tag, "rel=\"");
        if (r && strncmp(r + 5, "alternate", 9)) continue;
        h += 6; const size_t l = strcspn(h, "\"");
        char raw[1400]; snprintf(raw, sizeof raw, "%.*s", (int)l, h);
        html_snip(raw, raw + strlen(raw), out, cap);
        return out[0] != 0;
    }
    return 0;
}
/* ISO-8859-1/windows-1252 declarado no cabecalho: converte para UTF-8 (devolve memoria nova ou NULL se ja e UTF-8) */
static char *ws_latin1(const char *raw, size_t n, size_t *on) {
    const char *d = strstr(raw, "encoding=");
    if (!d || d - raw > 200 || !(ci_ncmp(d + 10, "iso-8859-1", 10) == 0 || ci_ncmp(d + 10, "windows-1252", 12) == 0)) return NULL;
    char *o = (char *)malloc(n * 2 + 1);
    if (!o) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)raw[i];
        if (c < 0x80) o[k++] = (char)c;
        else if (c < 0xA0) o[k++] = ' ';
        else { o[k++] = (char)(0xC0 | (c >> 6)); o[k++] = (char)(0x80 | (c & 63)); }
    }
    o[k] = 0; *on = k;
    return o;
}
/* leitor feed (RSS 0.9x/2.0 e Atom): <item> ou <entry>. -1 se nao e um feed */
static int ws_read_feed(const char *raw0, size_t n0, Hit *out, int max) {
    size_t n = n0; char *conv = ws_latin1(raw0, n0, &n);
    const char *raw = conv ? conv : raw0;
    const int atom = !strstr(raw, "<item") && strstr(raw, "<entry") != NULL;
    const char *open = atom ? "<entry" : "<item", *close = atom ? "</entry>" : "</item>";
    if (!strstr(raw, "<rss") && !strstr(raw, "<feed") && !strstr(raw, "<rdf") && !strstr(raw, "<channel")) { free(conv); return -1; }
    static char title[600], link[1400], snip[2400], date[120];
    int k = 0;
    for (const char *p = strstr(raw, open); p && k < max; p = strstr(p + 1, open)) {
        if (!(p[strlen(open)] == '>' || p[strlen(open)] == ' ')) continue;
        const char *ie = strstr(p, close);
        if (!ie) break;
        ws_xml_tag(p, ie, "title", title, sizeof title);
        if (atom || !ws_xml_tag(p, ie, "link", link, sizeof link) || !link[0]) ws_atom_link(p, ie, link, sizeof link);
        if (!ws_xml_tag(p, ie, "atom:subtitle", snip, sizeof snip) || !snip[0])
            if (!ws_xml_tag(p, ie, "description", snip, sizeof snip) || !snip[0])
                if (!ws_xml_tag(p, ie, "summary", snip, sizeof snip) || !snip[0]) ws_xml_tag(p, ie, "content", snip, sizeof snip);
        if (!ws_xml_tag(p, ie, "pubDate", date, sizeof date) || !date[0])
            if (!ws_xml_tag(p, ie, "published", date, sizeof date) || !date[0])
                if (!ws_xml_tag(p, ie, "updated", date, sizeof date) || !date[0]) ws_xml_tag(p, ie, "dc:date", date, sizeof date);
        for (char *t = snip; *t; t++) if (*t == '\n') *t = ' ';
        for (char *t = title; *t; t++) if (*t == '\n') *t = ' ';
        if (hit_set(&out[k], title, link, snip)) { if (date[0]) ws_hit_date(&out[k], date); k++; }
        p = ie;
    }
    free(conv);
    return k;
}
/* leitor sitemap: <loc>endereco</loc>; o titulo sai das palavras do caminho */
static int ws_read_sitemap(const char *raw, size_t n, Hit *out, int max) {
    const char *e = raw + n;
    if (!strstr(raw, "<urlset") && !strstr(raw, "<sitemapindex")) return -1;
    int k = 0;
    static char loc[1400], title[300], date[120];
    for (const char *p = strstr(raw, "<url>"); p && p < e && k < max; p = strstr(p + 5, "<url>")) {
        const char *ue = strstr(p, "</url>"); if (!ue) break;
        ws_xml_tag(p, ue, "loc", loc, sizeof loc);
        ws_xml_tag(p, ue, "lastmod", date, sizeof date);
        const char *path = strstr(loc, "://"); path = path ? path + 3 : loc; path += strcspn(path, "/");
        size_t t = 0;
        for (const char *c = path; *c && t + 1 < sizeof title; c++) title[t++] = strchr("/-_.", *c) ? ' ' : *c;
        title[t] = 0; ws_trim(title);
        if (!title[0]) snprintf(title, sizeof title, "%s", loc);
        if (hit_set(&out[k], title, loc, "")) { if (date[0]) ws_hit_date(&out[k], date); k++; }
        p = ue;
    }
    return k;
}
/* leitor html "links": para paginas de resultado sem leitor proprio, os links com texto de 12 caracteres ou mais para outros enderecos */
static int hp_links(const char *raw, const char *base, Hit *out, int max) {
    static char href[1400], abs_[1400], text[300];
    int k = 0;
    char bh[300] = ""; { const char *h = strstr(base, "://"); if (h) snprintf(bh, sizeof bh, "%.*s", (int)strcspn(h + 3, "/?#"), h + 3); }
    for (const char *p = strstr(raw, "<a "); p && k < max; p = strstr(p + 3, "<a ")) {
        const char *gt = strchr(p, '>'), *ce = gt ? strstr(gt, "</a>") : NULL;
        if (!gt || !ce) break;
        const char *h = strstr(p, "href=\"");
        if (!h || h > gt) continue;
        h += 6; const size_t l = strcspn(h, "\"");
        snprintf(href, sizeof href, "%.*s", (int)(l < sizeof href - 1 ? l : sizeof href - 1), h);
        if (!wn_resolve(base, href, abs_, sizeof abs_)) continue;
        html_snip(gt + 1, ce, text, sizeof text); ws_trim(text);
        if (strlen(text) < 12) continue;
        char hh[300] = ""; { const char *q = strstr(abs_, "://"); if (q) snprintf(hh, sizeof hh, "%.*s", (int)strcspn(q + 3, "/?#"), q + 3); }
        if (!strcmp(hh, bh)) continue;                                 /* navegacao do proprio site */
        int dup = 0; for (int i = 0; i < k; i++) if (!strcmp(out[i].url, abs_)) dup = 1;
        if (!dup && hit_set(&out[k], text, abs_, "")) k++;
    }
    return k;
}
static int ws_read_html(const WsSource *s, const char *raw, const char *url, Hit *out, int max) {
    char ps[40]; ws_map(s, "parser", ps, sizeof ps);
    if (!strcmp(ps, "result-a")) return hp_result_a(raw, "", out, max);
    if (!strcmp(ps, "b-algo")) return hp_b_algo(raw, "", out, max);
    if (!strcmp(ps, "title-s")) return hp_title_s(raw, "", out, max);
    return hp_links(raw, url, out, max);
}

/* ---------- consulta ---------- */
/* o endereco da fonte para a consulta: %s vira a consulta codificada (substituicao direta: o modelo pode ter outros %) */
static void ws_url(const WsSource *s, const char *q, char *out, size_t cap) {
    char enc[1200]; url_enc(q, enc, sizeof enc);
    const char *m = strstr(s->tmpl, "%s");
    if (!m) { snprintf(out, cap, "%s", s->tmpl); return; }
    snprintf(out, cap, "%.*s%s%s", (int)(m - s->tmpl), s->tmpl, enc, m + 2);
}
/* resultado patrocinado: link de anuncio do buscador (redirecionador y.js, parametros de anuncio) */
static int ws_sponsored(const char *u) {
    return strstr(u, "/y.js") || strstr(u, "ad_provider=") || strstr(u, "ad_domain=") || strstr(u, "/aclk?") || strstr(u, "/aclick?") || strstr(u, "&adurl=");
}
typedef struct {
    WsAsk a; char q[400];
    int src[WS_PICK], n; FILE *fp[WS_PICK]; int state[WS_PICK];      /* state: 0 aberto, 1 respondeu, 2 falhou, 3 nao consultada */
    char why[WS_PICK][100]; int got[WS_PICK];
    int general; double t0;
} WsRun;
static void ws_open(WsRun *r, int i, const char *q, int tmo) {
    char url[1600], why[100];
    const WsSource *s = &g_wsr[r->src[i]];
    r->fp[i] = NULL; r->got[i] = 0; r->why[i][0] = 0;
    if (!ws_rate_take(s, why, sizeof why)) { r->state[i] = 2; snprintf(r->why[i], sizeof r->why[i], "%s", why); return; }
    ws_url(s, q, url, sizeof url);
    g_curl_want_status = 1;
    r->fp[i] = curl_open(url, tmo);
    g_curl_want_status = 0;
    r->state[i] = r->fp[i] ? 0 : 2;
    if (!r->fp[i]) snprintf(r->why[i], sizeof r->why[i], "could not start the request");
}
/* fonte da busca geral (indice em g_wsr) para o passo k (0, 1, ...); a variavel KATANA_SEARX vira a primeira */
static int ws_general(int k) {
    int c = 0;
    for (int i = 0; i < g_nws; i++) if (g_wsr[i].topics & WT_GENERAL && g_wsr[i].kind == WK_HTML) { if (c == k) return i; c++; }
    return -1;
}
static void ws_add_searx(void) {
    const char *sx = getenv("KATANA_SEARX");
    if (!sx || !*sx || g_nws >= WS_MAXSRC) return;
    for (int i = 0; i < g_nws; i++) if (!strcmp(g_wsr[i].id, "own-metasearch")) return;
    char line[1200]; size_t bl = strlen(sx); while (bl > 0 && sx[bl - 1] == '/') bl--;
    snprintf(line, sizeof line, "own-metasearch | general | * | * | json | %.*s/search?q=%%s&format=json | days | 3 | 60 | items=results title=title url=url snip=content", (int)(bl < 600 ? bl : 600), sx);
    WsSource s; int rm; if (ws_parse_line(line, &s, &rm) == 1) { memmove(&g_wsr[1], &g_wsr[0], sizeof g_wsr[0] * (size_t)g_nws); g_wsr[0] = s; g_nws++; }
}
static int ws_start(const char *q, WsRun *r) {
    memset(r, 0, sizeof *r);
    ws_load(); ws_add_searx();
    snprintf(r->q, sizeof r->q, "%s", q);
    r->t0 = now_sec();
    ws_classify(q, &r->a);
    int pick[WS_MAXSRC];
    r->n = ws_route(&r->a, pick, 4);
    for (int i = 0; i < r->n; i++) r->src[i] = pick[i];
    if (r->n < 2) { const int g = ws_general(0); if (g >= 0) { r->src[r->n++] = g; r->general = 1; } }
    for (int i = 0; i < r->n; i++) ws_open(r, i, q, WS_TMO);
    return r->n;
}
/* le a fonte i aberta e acrescenta os resultados em all (ate cap); devolve quantos */
static int ws_collect(WsRun *r, int i, char *raw, size_t RAW, Hit *all, int na, int cap) {
    if (r->state[i] != 0 || !r->fp[i]) return 0;
    int rc = 0;
    long len = curl_finish(r->fp[i], raw, RAW, &rc);
    r->fp[i] = NULL;
    int http = 0;
    const size_t ml = strlen(CURL_STATUS_MARK);
    for (long k = len - (long)ml; k >= 0 && k >= len - (long)ml - 3; k--)
        if (memcmp(raw + k, CURL_STATUS_MARK, ml) == 0) { http = atoi(raw + k + ml); len = k; raw[len] = 0; break; }
    const WsSource *s = &g_wsr[r->src[i]];
    if (http >= 400) {
        r->state[i] = 2; snprintf(r->why[i], sizeof r->why[i], "HTTP %d", http);
        if (http == 429 || http == 403) ws_rate_pause(s, 120.0);
        return 0;
    }
    if (len <= 0) { r->state[i] = 2; snprintf(r->why[i], sizeof r->why[i], rc ? "network failure or timeout" : "empty answer"); return 0; }
    Hit *h = all + na;
    const int room = cap - na, scan = s->kind == WK_FEED && !ws_has_query(s) ? WS_FEED_SCAN : WS_PER;
    static Hit tmp[WS_FEED_SCAN];
    int k;
    char url[1600]; ws_url(s, r->q, url, sizeof url);
    if (s->kind == WK_JSON) k = ws_read_json(s, raw, (size_t)len, tmp, scan);
    else if (s->kind == WK_FEED) k = ws_read_feed(raw, (size_t)len, tmp, scan);
    else if (s->kind == WK_SITEMAP) k = ws_read_sitemap(raw, (size_t)len, tmp, scan);
    else k = ws_read_html(s, raw, url, tmp, scan);
    if (k < 0) { r->state[i] = 2; snprintf(r->why[i], sizeof r->why[i], "unexpected format"); return 0; }
    if (k == 0 && s->kind == WK_HTML) { r->state[i] = 2; snprintf(r->why[i], sizeof r->why[i], "no results, a challenge page or a changed format"); return 0; }
    r->state[i] = 1;
    /* feed sem consulta: o roteador filtra as entradas pelas palavras da pergunta (as gerais de noticia nao contam); sem palavra, as mais novas */
    if (s->kind != WK_JSON && !ws_has_query(s)) {
        char w[16][32]; int nw = 0;
        for (int j = 0; j < r->a.nw; j++) if (!ws_inlist(g_ws_newsgen, r->a.w[j])) memcpy(w[nw++], r->a.w[j], 32);
        int m = 0;
        for (int j = 0; j < k; j++) {
            int hit = nw == 0;
            if (!hit) { char f[1000], t[800]; snprintf(t, sizeof t, "%s %s", tmp[j].title, tmp[j].snip); ws_fold(t, f, sizeof f); for (int x = 0; x < nw; x++) if (ws_has(f, w[x])) hit = 1; }
            if (hit) tmp[m++] = tmp[j];
        }
        k = m;
        if (nw == 0) for (int a = 1; a < k; a++) { Hit x = tmp[a]; int b = a - 1; while (b >= 0 && (tmp[b].age < 0 ? 1L << 30 : tmp[b].age) > (x.age < 0 ? 1L << 30 : x.age)) { tmp[b + 1] = tmp[b]; b--; } tmp[b + 1] = x; }
        if (k > WS_PER) k = WS_PER;
    }
    int add = 0;
    for (int j = 0; j < k && add < room; j++) { h[add] = tmp[j]; h[add].src = 1u << i; h[add].rel = 1.0 / (j + 1); add++; }
    r->got[i] = add;
    return add;
}

/* ---------- verificador e ordem ---------- */
static int hit_cmp(const void *a, const void *b) {
    const double x = ((const Hit *)a)->score, y = ((const Hit *)b)->score;
    return x < y ? 1 : x > y ? -1 : 0;
}
/* tira patrocinados e o que nao cita a pergunta; pontua por palavras + cosseno (so soma) x confianca x frescor; junta repetidos */
static int ws_verify(WsRun *r, Hit *h, int n, int *dropped, int *sponsored) {
    *dropped = *sponsored = 0;
    float *cs = NULL;
    int ncos = -1;
    if (n > 0 && es_available()) {
        const char **tx = (const char **)malloc(sizeof(char *) * (size_t)n);
        char *buf = (char *)malloc((size_t)n * 800);
        cs = (float *)malloc(sizeof(float) * (size_t)n);
        if (tx && buf && cs) {
            for (int i = 0; i < n; i++) { snprintf(buf + (size_t)i * 800, 800, "%s. %s", h[i].title, h[i].snip); tx[i] = buf + (size_t)i * 800; }
            ncos = es_rank(r->q, EMB_SEARCH, tx, NULL, n, 1500.0, cs);
        }
        free(tx); free(buf);
        if (ncos <= 0) { free(cs); cs = NULL; }
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
        Hit x = h[i];
        x.cos = cs ? cs[i] : -2.0f;
        if (ws_sponsored(x.url)) { (*sponsored)++; continue; }
        char t[800], f[1000]; snprintf(t, sizeof t, "%s %s %s", x.title, x.snip, x.url); ws_fold(t, f, sizeof f);
        x.words = 0;
        for (int j = 0; j < r->a.nw; j++) if (ws_has(f, r->a.w[j])) x.words++;
        int si = 0; for (int b = 0; b < WS_PICK; b++) if (x.src & (1u << b)) { si = b; break; }
        const WsSource *s = &g_wsr[r->src[si]];
        if (r->a.nw > 0 && x.words == 0) {
            /* sem nenhuma palavra: cai se o cosseno tambem e baixo; sem cosseno, cai so o que veio da busca geral (a API de uma fonte especifica ja casou a consulta) */
            if (x.cos > -1.5f ? x.cos < WS_COS_LOW : (s->topics & WT_GENERAL) != 0) { (*dropped)++; continue; }
        }
        double rel = (r->a.nw ? (double)x.words / r->a.nw : 0.5) + 0.3 * x.rel;
        if (x.cos > -1.5f) rel += 0.8 * x.cos;
        double fr = 1.0;
        if (r->a.recent) fr = x.age >= 0 ? 0.3 + 0.7 / (1.0 + x.age / 48.0) : 0.5;
        x.score = rel * (0.5 + 0.1 * s->trust) * fr;
        char k1[500], k2[500]; url_key(x.url, k1, sizeof k1);
        int d = -1;
        for (int j = 0; j < m; j++) { url_key(h[j].url, k2, sizeof k2); if (!strcmp(k1, k2)) { d = j; break; } }
        if (d >= 0) { h[d].src |= x.src; h[d].score += 0.5 * x.score; if (strlen(x.snip) > strlen(h[d].snip)) snprintf(h[d].snip, sizeof h[d].snip, "%s", x.snip); continue; }
        h[m++] = x;
    }
    free(cs);
    qsort(h, (size_t)m, sizeof h[0], hit_cmp);
    return m;
}

/* ---------- entrega ---------- */
static size_t ws_state_line(const WsRun *r, int nres, char *o, size_t cap) {
    int fails = 0, oks = 0;
    for (int i = 0; i < r->n; i++) { if (r->state[i] == 2) fails++; else if (r->state[i] == 1) oks++; }
    size_t n = (size_t)snprintf(o, cap, "[search: %s, sources: ", nres == 0 ? "empty" : fails ? "partial" : "ok");
    int first = 1;
    for (int i = 0; i < r->n; i++) if (r->state[i] == 1) { n += (size_t)snprintf(o + n, cap - n, "%s%s", first ? "" : ", ", g_wsr[r->src[i]].id); first = 0; }
    if (!oks) n += (size_t)snprintf(o + n, cap - n, "none");
    if (fails) {
        n += (size_t)snprintf(o + n, cap - n, " (");
        int f1 = 1;
        for (int i = 0; i < r->n; i++) if (r->state[i] == 2) { n += (size_t)snprintf(o + n, cap - n, "%s%s failed: %s", f1 ? "" : "; ", g_wsr[r->src[i]].id, r->why[i]); f1 = 0; }
        n += (size_t)snprintf(o + n, cap - n, ")");
    }
    n += (size_t)snprintf(o + n, cap - n, ", %d result%s]\n", nres, nres == 1 ? "" : "s");
    return n < cap ? n : cap - 1;
}
/* fecha a rodada: le as fontes, verifica, troca de rota se tudo caiu (busca geral, depois a consulta reduzida) e escreve o texto.
   Formato: a linha de estado e depois "N. titulo\n   url\n   resumo\n   (sources: a, b; date AAAA-MM-DD)\n" */
static void ws_finish(WsRun *r, char *out, size_t cap) {
    const size_t RAW = 1500000;
    const int MAXH = WS_PICK * WS_PER;
    static Hit all[WS_PICK * WS_PER];
    char *raw = (char *)malloc(RAW + 1);
    out[0] = 0;
    if (!raw) { for (int i = 0; i < r->n; i++) if (r->fp[i]) curl_finish(r->fp[i], NULL, 0, NULL); snprintf(out, cap, "error: out of memory"); return; }
    int na = 0, m = 0, dropped = 0, spons = 0, tdrop = 0, tspons = 0, reduced = 0;
    for (int i = 0; i < r->n; i++) na += ws_collect(r, i, raw, RAW, all, na, MAXH);
    m = ws_verify(r, all, na, &dropped, &spons); tdrop += dropped; tspons += spons;
    /* troca de rota antes de o modelo ver a falha: a busca geral (as fontes gerais ainda nao usadas) e depois a primeira delas com a
       consulta reduzida as palavras informativas */
    int gen_used = r->general ? 1 : 0;
    for (int step = 0; m == 0 && step < 3 && now_sec() - r->t0 < WS_BUDGET - 4 && r->n < WS_PICK; step++) {
        char q2[400];
        int g;
        snprintf(q2, sizeof q2, "%s", r->q);
        if (step < 2) { g = ws_general(gen_used); if (g < 0) continue; gen_used++; }
        else {
            g = ws_general(0);
            if (g < 0 || r->a.nw < 2) break;
            size_t o = 0; q2[0] = 0;
            for (int j = 0; j < r->a.nw && j < 4; j++) o += (size_t)snprintf(q2 + o, sizeof q2 - o, "%s%s", j ? " " : "", r->a.w[j]);
            if (!strcmp(q2, r->q)) break;
            reduced = 1;
        }
        const int i = r->n++;
        r->src[i] = g; r->general = 1;
        const int tl = (int)(WS_BUDGET - (now_sec() - r->t0));
        ws_open(r, i, q2, tl < WS_TMO ? (tl < 3 ? 3 : tl) : WS_TMO);
        char qsave[400];
        snprintf(qsave, sizeof qsave, "%s", r->q);
        snprintf(r->q, sizeof r->q, "%s", q2);
        na = ws_collect(r, i, raw, RAW, all, 0, MAXH);
        m = ws_verify(r, all, na, &dropped, &spons); tdrop += dropped; tspons += spons;
        snprintf(r->q, sizeof r->q, "%s", qsave);
        if (m == 0) reduced = 0;
    }
    free(raw);
    size_t o = ws_state_line(r, m < 8 ? m : 8, out, cap);
    for (int i = 0; i < m && i < 8 && o + 1800 < cap; i++) {
        o += (size_t)snprintf(out + o, cap - o, "%d. %s\n   %s\n   %.250s\n   (sources:", i + 1, all[i].title, all[i].url, all[i].snip[0] ? all[i].snip : "-");
        int f1 = 1;
        for (int b = 0; b < r->n; b++) if (all[i].src & (1u << b)) { o += (size_t)snprintf(out + o, cap - o, "%s %s", f1 ? "" : ",", g_wsr[r->src[b]].id); f1 = 0; }
        if (all[i].date[0]) o += (size_t)snprintf(out + o, cap - o, "; date %s", all[i].date);
        o += (size_t)snprintf(out + o, cap - o, ")\n");
    }
    if (m == 0) {
        o += (size_t)snprintf(out + o, cap - o, "error: no source returned a result about \"%.200s\"", r->q);
        if (tdrop || tspons) o += (size_t)snprintf(out + o, cap - o, " (%d off-topic and %d sponsored results were dropped)", tdrop, tspons);
        o += (size_t)snprintf(out + o, cap - o, ". Try other words");
    } else if (reduced) o += (size_t)snprintf(out + o, cap - o, "(the general search was repeated with only the main words of the question)\n");
    char lg[700]; size_t l = 0;
    l += (size_t)snprintf(lg + l, sizeof lg - l, "%s; picked:", r->a.why);
    for (int i = 0; i < r->n; i++) l += (size_t)snprintf(lg + l, sizeof lg - l, " %s=%s", g_wsr[r->src[i]].id, r->state[i] == 1 ? "ok" : r->why[i]);
    snprintf(lg + l, sizeof lg - l, "; %d dropped, %d sponsored", tdrop, tspons);
    agent_log("WebSearch", r->q, lg);
}
/* busca completa numa chamada (WebSearch com uma consulta, conector do UIOE, passo "outra fonte" do WebFetch) */
static void ws_search(const char *q, char *out, size_t cap) {
    static WsRun r;
    ws_start(q, &r);
    ws_finish(&r, out, cap);
}

/* ---------- WebFetch: endereco de uma fonte registrada usa o leitor dela ----------
 * page=<modelo com {1} {2}> casa o endereco pedido (cada {n} e um pedaco do caminho sem '/'); fetch=<modelo> e o endereco que a fonte
 * prefere servir (texto, markdown, feed ou JSON com fetchpath=). O endereco de um feed registrado e lido pelo leitor de feed. */
static int ws_page_match(const char *pat, const char *url, char cap_[][300], int *nc) {
    const char *p = pat, *u = url;
    *nc = 0;
    while (*p) {
        if (*p == '{') {
            const char *b = strchr(p, '}'); if (!b) return 0;
            const size_t l = strcspn(u, "/?#");
            if (!l || *nc >= 4) return 0;
            snprintf(cap_[(*nc)++], 300, "%.*s", (int)(l < 299 ? l : 299), u);
            u += l; p = b + 1; continue;
        }
        if (tolower((unsigned char)*p) != tolower((unsigned char)*u)) return 0;
        p++; u++;
    }
    if (*u == '/' && (!u[1] || u[1] == '?' || u[1] == '#')) u++;
    return !*u || *u == '?' || *u == '#';
}
/* texto de um feed para leitura: titulo, data, endereco e resumo de cada entrada */
static size_t ws_feed_text(const char *raw, size_t n, char *o, size_t cap) {
    static Hit h[40];
    const int k = ws_read_feed(raw, n, h, 40);
    size_t m = 0;
    for (int i = 0; i < k && m + 600 < cap; i++)
        m += (size_t)snprintf(o + m, cap - m, "%s%s\n%s%s%s\n%s\n\n", k == 1 ? "# " : "## ", h[i].title, h[i].date[0] ? h[i].date : "", h[i].date[0] ? "  " : "", h[i].url, h[i].snip);
    return k > 0 ? m : 0;
}
static WnPage *ws_fetch_page(const char *url) {
    ws_load();
    for (int i = 0; i < g_nws; i++) {
        const WsSource *s = &g_wsr[i];
        char pat[700], fmt[900], path[200], furl[1600], why[100];
        char cp[4][300]; int nc = 0;
        int feed_self = s->kind == WK_FEED && !ws_has_query(s) && !strcmp(s->tmpl, url);
        if (!feed_self) {
            if (!ws_map(s, "page", pat, sizeof pat) || !ws_map(s, "fetch", fmt, sizeof fmt)) continue;
            if (!ws_page_match(pat, url, cp, &nc)) continue;
            if (!ws_expand(fmt, NULL, NULL, cp, nc, 0, furl, sizeof furl)) continue;
        } else snprintf(furl, sizeof furl, "%s", url);
        if (!ws_rate_take(s, why, sizeof why)) return NULL;
        const size_t RAW = 2500000;
        char *raw = (char *)malloc(RAW + 1);
        if (!raw) return NULL;
        int rc; const int save = g_curl_tmo; g_curl_tmo = 12;
        const long n = curl_get(furl, raw, RAW, &rc);
        g_curl_tmo = save;
        if (n <= 0 || g_curl_http >= 400) { if (g_curl_http == 429) ws_rate_pause(s, 120.0); free(raw); return NULL; }
        char *txt = (char *)malloc((size_t)n * 2 + 4096);
        if (!txt) { free(raw); return NULL; }
        size_t tn = 0;
        if (ws_map(s, "fetchpath", path, sizeof path)) {
            const char *v = jx_path(raw, raw + n, path);
            char *t = (char *)malloc((size_t)n + 1);
            if (t) { jx_text(v, raw + n, t, (size_t)n + 1); tn = (size_t)snprintf(txt, (size_t)n * 2 + 4096, "%s", t); free(t); }
        } else if (s->kind == WK_FEED || strstr(raw, "<feed") || strstr(raw, "<rss")) tn = ws_feed_text(raw, (size_t)n, txt, (size_t)n * 2 + 4096);
        else if (!memchr(raw, '<', (size_t)(n < 200 ? n : 200)) || raw[0] == '#') { memcpy(txt, raw, (size_t)n); tn = (size_t)n; txt[tn] = 0; }   /* texto ou markdown */
        free(raw);
        if (tn < 150) { free(txt); return NULL; }                      /* pouco texto: a escada normal le a pagina */
        char *lk = (char *)malloc(WN_LINKS_BYTES);
        if (!lk) { free(txt); return NULL; }
        lk[0] = 0;
        WnPage *pg = wn_put(url, txt, tn, lk, 0, 0);
        pg->verdict = WQ_OK; pg->quality = 90;
        snprintf(pg->route, sizeof pg->route, "src:%.19s", s->id);
        snprintf(pg->tried, sizeof pg->tried, "src:%.19s", s->id);
        return pg;
    }
    return NULL;
}

/* passo "outra fonte" da escada do WebFetch: o roteador busca o mesmo assunto e devolve ate 3 enderecos de outros sites */
static int ws_other_sources(const char *q, const char *url, char *o, size_t cap) {
    char *buf = (char *)malloc(24000);
    if (!buf) return 0;
    ws_search(q, buf, 24000);
    char host[300] = ""; { const char *h = strstr(url, "://"); if (h) snprintf(host, sizeof host, "%.*s", (int)strcspn(h + 3, "/?#"), h + 3); }
    size_t n = 0; int k = 0;
    o[0] = 0;
    for (const char *p = buf; (p = strstr(p, "\n   http")) && k < 3; p++) {
        const char *u = p + 4; const size_t l = strcspn(u, "\n");
        char cand[1100]; snprintf(cand, sizeof cand, "%.*s", (int)(l < 1099 ? l : 1099), u);
        if (host[0] && strstr(cand, host)) continue;
        n += (size_t)snprintf(o + n, cap > n ? cap - n : 0, "%s%s", k ? " | " : "", cand);
        k++;
    }
    free(buf);
    return k;
}

/* katana sources: o registro carregado (embutido + arquivo do dono) e, com uma pergunta, a rota que o roteador escolhe */
static int ws_cli(int argc, char **argv) {
    ws_load(); ws_add_searx();
    char path[1300]; ws_owner_path(path, sizeof path);
    if (argc > 0) {
        char q[400] = ""; size_t n = 0;
        for (int i = 0; i < argc; i++) n += (size_t)snprintf(q + n, sizeof q - n, "%s%s", i ? " " : "", argv[i]);
        WsAsk a; ws_classify(q, &a);
        int pick[WS_MAXSRC]; const int k = ws_route(&a, pick, 4);
        printf("question: %s\n%s\nroute:", q, a.why);
        for (int i = 0; i < k; i++) printf(" %s", g_wsr[pick[i]].id);
        if (k < 2) printf("%s general search", k ? " +" : "");
        printf("\n");
        return 0;
    }
    printf("%d sources (built-in + %s%s)\n", g_nws, path, g_ws_mtime ? "" : ", not present");
    for (int i = 0; i < g_nws; i++) {
        const WsSource *s = &g_wsr[i]; char t[120] = ""; size_t o = 0;
        for (int b = 0; g_wt_names[b]; b++) if (s->topics & (1u << b)) o += (size_t)snprintf(t + o, sizeof t - o, "%s%s", o ? "," : "", g_wt_names[b]);
        printf("%-16s %-28s %-3s %-3s %-7s %-5s trust %d, %d/min  %s\n", s->id, t, s->country, s->lang, g_wk_names[s->kind], g_wf_names[s->fresh], s->trust, s->rpm, s->tmpl);
    }
    if (g_ws_bad) printf("%d invalid line(s) in %s were ignored\n", g_ws_bad, path);
    return 0;
}
