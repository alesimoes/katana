/* ============================ servico de embeddings ===============================
 * O modelo de embeddings (embed.c) como servico do motor: as ferramentas (Read, ToolSearch, Bash, ...) pedem vetores sem saber
 * onde esta o modelo nem se ja foram calculados.
 *
 *   es_available()                         ha um modelo de embeddings nesta maquina? (so olha o disco; nao carrega nada)
 *   es_vec(texto, tarefa, titulo, out)     vetor de ES_DIM floats, normalizado (cache em disco por conteudo)
 *   es_rank(consulta, trechos, n, ...)     semelhanca da consulta com cada trecho, com orcamento de tempo
 *
 * Regras (docs/embedding-usage.md): o embedding so reordena e sugere; nunca decide, nunca exclui. Sem modelo, sem tempo ou com
 * KATANA_EMBED=0 as ferramentas seguem com o caminho antigo, sem erro. O modelo carrega na primeira chamada (cerca de 0,5 s) e
 * fica na memoria; uma chamada por vez (o encoder nao e reentrante: um mutex serializa).
 *
 * Cache: ~/.katana/cache/embed-<impressao do modelo>.vec, so de anexar. Registro = chave (8 bytes) + ES_DIM floats. A chave e o
 * hash do modelo, da tarefa, do titulo e do texto: mudou o modelo ou o texto, e outro registro. Um registro cortado no fim do
 * arquivo (queda no meio da gravacao) e ignorado. Matryoshka: os vetores guardados tem ES_DIM dimensoes (256). */

#define ES_DIM 256
#define ES_REC (8 + ES_DIM * 4)
#define ES_MAXCHARS 6000                                   /* um trecho maior que isso e cortado antes de virar vetor (custo) */

static const char *models_dir(void);                       /* app/catalog.c */

static EmbModel *g_es_m;
static int g_es_state;                                     /* 0 ainda nao tentou carregar, 1 pronto, -1 sem modelo ou falhou */
static pthread_mutex_t g_es_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_es_fp;                                   /* impressao do modelo (nome, tamanho, data) */
static char g_es_model_path[4096], g_es_cache_path[4096];
static uint64_t *g_es_keys;                                /* chaves do cache, em ordem de gravacao */
static float *g_es_vecs;                                   /* g_es_n * ES_DIM */
static size_t g_es_n, g_es_cap;
static uint32_t *g_es_tab; static uint32_t g_es_tabcap;    /* tabela aberta: indice+1 em g_es_keys (0 = vazio) */
static FILE *g_es_cf;
static long g_es_computed, g_es_hits;                      /* vetores calculados e vindos do cache, desde o inicio (observacao) */

static uint64_t es_fnv64(uint64_t h, const void *p, size_t n) {
    const uint8_t *s = p;
    for (size_t i = 0; i < n; i++) { h ^= s[i]; h *= 1099511628211ull; }
    return h;
}

static int es_find_model(char *out, size_t cap) {
    const char *off = getenv("KATANA_EMBED");
    if (off && (!strcmp(off, "0") || !strcmp(off, "off"))) return 0;
    struct stat st;
    const char *e = getenv("KATANA_EMBED_MODEL");
    if (e && e[0]) { snprintf(out, cap, "%s", e); return stat(out, &st) == 0; }
    static const char *names[] = { "embeddinggemma-2/embeddinggemma-2-q8_0.gguf", "embeddinggemma-2/embeddinggemma-2-f32.gguf" };
    for (int i = 0; i < 2; i++) { snprintf(out, cap, "%s/%s", models_dir(), names[i]); if (stat(out, &st) == 0) return 1; }
    return 0;
}

/* ha um modelo de embeddings? So o disco: nao carrega nem calcula nada */
static int es_available(void) {
    if (g_es_state == 1) return 1;
    if (g_es_state == -1) return 0;
    char p[4096];
    return es_find_model(p, sizeof p);
}

static void es_tab_put(uint64_t key, size_t idx) {
    const uint32_t m = g_es_tabcap - 1;
    uint32_t h = (uint32_t)(key ^ (key >> 29)) & m;
    while (g_es_tab[h]) h = (h + 1) & m;
    g_es_tab[h] = (uint32_t)idx + 1;
}
static int es_tab_find(uint64_t key) {
    if (!g_es_tabcap) return -1;
    const uint32_t m = g_es_tabcap - 1;
    uint32_t h = (uint32_t)(key ^ (key >> 29)) & m;
    while (g_es_tab[h]) { if (g_es_keys[g_es_tab[h] - 1] == key) return (int)(g_es_tab[h] - 1); h = (h + 1) & m; }
    return -1;
}
static void es_grow(void) {
    if (g_es_n < g_es_cap) return;
    g_es_cap = g_es_cap ? g_es_cap * 2 : 4096;
    g_es_keys = realloc(g_es_keys, g_es_cap * sizeof *g_es_keys);
    g_es_vecs = realloc(g_es_vecs, g_es_cap * ES_DIM * sizeof(float));
    if (!g_es_keys || !g_es_vecs) die("sem memoria");
    if (g_es_cap * 4 > g_es_tabcap) {                      /* tabela com no maximo um quarto cheia */
        free(g_es_tab);
        g_es_tabcap = (uint32_t)(g_es_cap * 4);
        g_es_tab = xalloc(g_es_tabcap, sizeof *g_es_tab);
        for (size_t i = 0; i < g_es_n; i++) es_tab_put(g_es_keys[i], i);
    }
}
static void es_store(uint64_t key, const float *v) {
    es_grow();
    g_es_keys[g_es_n] = key;
    memcpy(g_es_vecs + g_es_n * ES_DIM, v, ES_DIM * sizeof(float));
    es_tab_put(key, g_es_n);
    g_es_n++;
}

/* carrega o modelo e o cache (com g_es_mu tomado). 0 = pronto */
static int es_load_locked(void) {
    if (g_es_state == 1) return 0;
    if (g_es_state == -1) return -1;
    g_es_state = -1;
    if (!es_find_model(g_es_model_path, sizeof g_es_model_path)) return -1;
    struct stat st;
    if (stat(g_es_model_path, &st) != 0) return -1;
    g_es_m = xalloc(1, sizeof *g_es_m);
    if (emb_load(g_es_m, g_es_model_path) != 0) { free(g_es_m); g_es_m = NULL; return -1; }
    const char *bn = strrchr(g_es_model_path, '/'); bn = bn ? bn + 1 : g_es_model_path;
    g_es_fp = es_fnv64(14695981039346656037ull, bn, strlen(bn));
    g_es_fp = es_fnv64(g_es_fp, &st.st_size, sizeof st.st_size);
    g_es_fp = es_fnv64(g_es_fp, &st.st_mtime, sizeof st.st_mtime);
    const char *home = getenv("HOME");
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/.katana", home ? home : "."); mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/.katana/cache", home ? home : "."); mkdir(dir, 0755);
    snprintf(g_es_cache_path, sizeof g_es_cache_path, "%s/embed-%016llx.vec", dir, (unsigned long long)g_es_fp);
    FILE *f = fopen(g_es_cache_path, "rb");
    if (f) {
        uint8_t rec[ES_REC];
        while (fread(rec, 1, ES_REC, f) == ES_REC) {       /* um registro cortado no fim nao e lido */
            uint64_t key; memcpy(&key, rec, 8);
            if (es_tab_find(key) >= 0) continue;
            float v[ES_DIM]; memcpy(v, rec + 8, sizeof v);
            es_store(key, v);
        }
        fclose(f);
    }
    g_es_cf = fopen(g_es_cache_path, "ab");                /* sem permissao de gravar: segue so com a memoria */
    g_es_state = 1;
    return 0;
}

static uint64_t es_key(const char *text, size_t n, int task, const char *title) {
    uint64_t h = es_fnv64(g_es_fp, &task, sizeof task);
    const int dim = ES_DIM;
    h = es_fnv64(h, &dim, sizeof dim);
    if (title) h = es_fnv64(h, title, strlen(title));
    h = es_fnv64(h, "\0", 1);
    return es_fnv64(h, text, n);
}

/* vetor de um texto (com g_es_mu tomado e o modelo carregado). cached_only: so devolve se ja estiver no cache. 0 = ok, -1 = nao */
static int es_vec_locked(const char *text, int task, const char *title, float *out, int cached_only) {
    size_t n = strlen(text);
    if (n > ES_MAXCHARS) { n = ES_MAXCHARS; while (n > 0 && ((uint8_t)text[n] & 0xC0) == 0x80) n--; }   /* nao corta no meio de um caractere UTF-8 */
    const uint64_t key = es_key(text, n, task, title);
    const int at = es_tab_find(key);
    if (at >= 0) { memcpy(out, g_es_vecs + (size_t)at * ES_DIM, ES_DIM * sizeof(float)); g_es_hits++; return 0; }
    if (cached_only) return -1;
    char *cut = xalloc(n + 1, 1);
    memcpy(cut, text, n);
    const int d = embed_text(g_es_m, cut, task, title, ES_DIM, out);
    free(cut);
    if (d != ES_DIM) return -1;
    es_store(key, out);
    g_es_computed++;
    if (g_es_cf) {
        uint8_t rec[ES_REC];
        memcpy(rec, &key, 8); memcpy(rec + 8, out, ES_DIM * sizeof(float));
        if (fwrite(rec, 1, ES_REC, g_es_cf) == ES_REC) fflush(g_es_cf);
    }
    return 0;
}

/* vetor de um texto; 0 = ok (ES_DIM floats em out, normalizado), -1 = sem modelo ou falhou */
static int es_vec(const char *text, int task, const char *title, float *out) {
    pthread_mutex_lock(&g_es_mu);
    int r = es_load_locked() == 0 ? es_vec_locked(text, task, title, out, 0) : -1;
    pthread_mutex_unlock(&g_es_mu);
    return r;
}

static float es_dot(const float *a, const float *b) {
    float s = 0.0f;
    for (int i = 0; i < ES_DIM; i++) s += a[i] * b[i];
    return s;
}

/* Semelhanca (cosseno) da consulta com cada um dos n trechos. title[i] pode ser NULL (ou title inteiro NULL).
 * budget_ms: tempo maximo gasto CALCULANDO vetores de trechos (o que ja esta no cache e de graca; a consulta e sempre calculada).
 * Trecho sem vetor (acabou o tempo) fica com score -2. Devolve quantos trechos tem score, ou -1 se nao ha modelo ou a consulta falhou.
 * A ordem em que os trechos sao tentados e a dada: quem chama manda os mais provaveis primeiro. */
static int es_rank(const char *query, int qtask, const char *const *chunk, const char *const *title, int n, double budget_ms, float *score) {
    for (int i = 0; i < n; i++) score[i] = -2.0f;
    pthread_mutex_lock(&g_es_mu);
    if (es_load_locked() != 0) { pthread_mutex_unlock(&g_es_mu); return -1; }
    float q[ES_DIM], v[ES_DIM];
    int ok = es_vec_locked(query, qtask, NULL, q, 0) == 0, got = 0;
    if (ok) {
        const double t0 = emb_now();
        for (int i = 0; i < n; i++) {
            const int cached_only = emb_now() - t0 > budget_ms;
            if (es_vec_locked(chunk[i], EMB_DOCUMENT, title ? title[i] : NULL, v, cached_only) == 0) { score[i] = es_dot(q, v); got++; }
        }
    }
    pthread_mutex_unlock(&g_es_mu);
    if (ok && getenv("KATANA_EMBED_DEBUG"))      /* calibragem dos limiares: o cosseno de cada trecho contra a consulta */
        for (int i = 0; i < n; i++) {
            char t[72]; size_t k = 0;
            for (const char *s = chunk[i]; *s && k < sizeof t - 1; s++) t[k++] = (*s == '\n' || *s == '\t') ? ' ' : *s;
            t[k] = 0;
            fprintf(stderr, "[embed] cos %6.3f  %s\n", (double)score[i], t);
        }
    return ok ? got : -1;
}

/* Carrega o modelo e o cache numa linha propria, para o agente nao esperar na primeira pergunta. Sem modelo, nao faz nada. */
static void tsr_warm(void);                                 /* tools/toolsearch.c */
static void *es_preload_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_es_mu);
    const int ok = es_load_locked();
    pthread_mutex_unlock(&g_es_mu);
    if (ok == 0) tsr_warm();
    return NULL;
}
static void es_preload_async(void) {
    static int started;
    if (started || !es_available()) return;
    started = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, es_preload_thread, NULL) == 0) pthread_detach(t);
}
