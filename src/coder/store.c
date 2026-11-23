/* ---------- armazenamento generico em segmentos imutaveis (modulo coder; reaproveitavel por outros modulos) ----------
 * Este arquivo NAO conhece tipos de codigo: so tipos C basicos e os tipos proprios do armazenamento. Entra em katana.c ANTES de coder/types.c
 * para o compilador recusar qualquer dependencia de tipos de codigo. Documentos de referencia: docs/code-intelligence.md 2.4 e 2.9.
 *
 * PRINCIPIO: o armazenamento e um CACHE derivado da arvore de trabalho. Precisa ser nunca errado e nunca pela metade, nao durar ate a
 * ultima atualizacao. Por isso: segmentos imutaveis + um MANIFESTO publicado por troca atomica (escrever tmp, fsync, rename). Sem WAL,
 * sem arvores B+. Escritor unico por lock de arquivo; leitores sem lock (varios processos ao mesmo tempo).
 *
 * LAYOUT EM DISCO (<pasta>/store/):
 *   MANIFEST        unico arquivo mutavel; trocado inteiro por rename
 *   LOCK            lock advisory do escritor (leitores nunca tomam)
 *   seg/NNNNNN.cbs  segmentos imutaveis (id decimal de 6 digitos)
 *   tmp/            arquivos em escrita; tudo que sobra aqui apos uma queda e lixo
 *   .gitignore      "*" (so dentro de store/)
 *
 * FORMATO (versao 1; tudo little-endian, campos lidos e escritos um a um, NUNCA fwrite/mmap de struct):
 *   segmento: cabecalho de 64 bytes
 *       0 u32 magic "KSEG" (0x4745534B)   4 u32 versao do formato   8 u64 id do segmento   16 u32 nsecoes   20 u32 reservado
 *       24 u64 tamanho total do arquivo   32 u64 checksum da tabela de secoes   40..55 reservado   56 u64 checksum dos bytes 0..55
 *     tabela de secoes (nsecoes * 40 bytes), a partir de 64:
 *       0 u32 tipo  4 u32 versao da secao  8 u64 offset  16 u64 tamanho  24 u64 checksum dos dados  32 u32 nregistros  36 u32 reservado
 *     dados das secoes, cada uma alinhada em 8 bytes. O significado do conteudo e de quem registrou o tipo (enum StoreSecType).
 *   manifesto: u32 magic "KMAN" (0x4E414D4B), u32 versao do formato, u64 geracao, u64 proximo id de segmento, u32 nsegmentos,
 *       u32 tamanho da raiz, raiz (bytes, preenchida ate multiplo de 8), nsegmentos * { u64 id, u64 tamanho, u64 checksum da tabela,
 *       u32 nsecoes, u32 contagem }, e no fim u64 checksum de tudo antes.
 *   Segmentos listados do mais antigo ao mais novo. A resposta vigente de uma chave e a do segmento mais novo que a cita (a ela ou a um
 *   tombstone, que e a marca de removido); o que e mais antigo fica mascarado ate a compactacao.
 *   Ids de 32 bits sao LOCAIS ao segmento; onde a identidade precisa sobreviver usa-se a chave estavel de 64 bits (store_hash64 do conteudo).
 *
 * Metas de desenho para 100 mil arquivos (nao medidas em campanha): indexacao a frio limitada pela leitura dos arquivos; catalogo de
 * ~40 bytes por arquivo mais o caminho (poucos MB); leitores mapeiam sem copiar, memoria residente proporcional ao que e tocado. */
#ifndef _WIN32
#include <sys/file.h>
#endif

#define STORE_FORMAT 1
#define STORE_SEG_MAGIC 0x4745534Bu
#define STORE_MAN_MAGIC 0x4E414D4Bu
#define STORE_HDR 64
#define STORE_SECENT 40
#define STORE_MAXSEC 64

/* tipos de secao, registrados por NUMERO; cada faixa pertence a um modulo */
enum StoreSecType {
    STS_NONE = 0,
    STS_DOCS = 1,           /* catalogo de documentos (este arquivo e storedocs.c) */
    STS_TOMBS = 2,          /* tombstones: caminhos removidos (tabela de strings ordenada) */
    STS_DIRS = 3,           /* pastas do rastreador: estado, contagens, resumo (coder/crawl.c) */
    STS_CRAWL = 4,          /* um registro por segmento: andamento do rastreador, publicado na mesma troca atomica do manifesto (coder/crawl.c) */
    STS_CAT_LAST = 0xFF,    /* 0x001..0x0FF: catalogo e secoes genericas */
    STS_LEX_BASE = 0x100,   /* 0x100..0x1FF: lexico e postings (busca) */
    STS_SYM_BASE = 0x200,   /* 0x200..0x2FF: simbolos, referencias, relacoes (analisadores) */
    STS_KB_BASE = 0x300     /* 0x300..0x3FF: conhecimento (base) */
};

/* ---------- buffer de bytes e primitivas little-endian ---------- */
typedef struct { uint8_t *p; size_t n, cap; } StoreBuf;
static void sb_reserve(StoreBuf *b, size_t extra) {
    if (b->n + extra <= b->cap) return;
    size_t c = b->cap ? b->cap : 256;
    while (c < b->n + extra) c *= 2;
    b->p = (uint8_t *)realloc(b->p, c);
    if (!b->p) { fprintf(stderr, "store: sem memoria\n"); exit(1); }
    b->cap = c;
}
static void sb_free(StoreBuf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }
static void sb_bytes(StoreBuf *b, const void *s, size_t n) { sb_reserve(b, n); if (n) memcpy(b->p + b->n, s, n); b->n += n; }
static void sb_u8(StoreBuf *b, uint8_t v) { sb_reserve(b, 1); b->p[b->n++] = v; }
static void sb_u32(StoreBuf *b, uint32_t v) { sb_reserve(b, 4); for (int i = 0; i < 4; i++) b->p[b->n++] = (uint8_t)(v >> (8 * i)); }
static void sb_u64(StoreBuf *b, uint64_t v) { sb_reserve(b, 8); for (int i = 0; i < 8; i++) b->p[b->n++] = (uint8_t)(v >> (8 * i)); }
static void sb_varint(StoreBuf *b, uint64_t v) { while (v >= 0x80) { sb_u8(b, (uint8_t)(v | 0x80)); v >>= 7; } sb_u8(b, (uint8_t)v); }
static void sb_pad8(StoreBuf *b) { while (b->n & 7) sb_u8(b, 0); }
static uint32_t st_get_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t st_get_u64(const uint8_t *p) { return (uint64_t)st_get_u32(p) | (uint64_t)st_get_u32(p + 4) << 32; }
static void st_put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void st_put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
/* varint de 7 bits com continuacao; devolve o ponteiro seguinte, ou NULL se o dado acabou/estourou */
static const uint8_t *st_get_varint(const uint8_t *p, const uint8_t *end, uint64_t *out) {
    uint64_t v = 0; int sh = 0;
    while (p < end && sh < 64) { uint8_t c = *p++; v |= (uint64_t)(c & 0x7F) << sh; if (!(c & 0x80)) { *out = v; return p; } sh += 7; }
    return NULL;
}
/* lista ascendente de inteiros: varint(n) e n deltas varint (o primeiro e relativo a 0) */
static void st_put_deltas(StoreBuf *b, const uint32_t *v, uint32_t n) {
    sb_varint(b, n);
    uint32_t prev = 0;
    for (uint32_t i = 0; i < n; i++) { sb_varint(b, v[i] - prev); prev = v[i]; }
}
/* devolve quantos valores gravou em out (ate max) ou -1 se o dado esta truncado; *next recebe o ponteiro seguinte (pode ser NULL) */
static int64_t st_get_deltas(const uint8_t *p, const uint8_t *end, uint32_t *out, uint32_t max, const uint8_t **next) {
    uint64_t n, d;
    p = st_get_varint(p, end, &n);
    if (!p || n > max) return -1;
    uint32_t prev = 0;
    for (uint64_t i = 0; i < n; i++) { p = st_get_varint(p, end, &d); if (!p) return -1; prev += (uint32_t)d; out[i] = prev; }
    if (next) *next = p;
    return (int64_t)n;
}
/* hash/checksum de 64 bits, 8 bytes por vez (chave estavel de conteudo e checksum de secao) */
static uint64_t store_hash64(const void *data, size_t n, uint64_t seed) {
    const uint8_t *s = (const uint8_t *)data;
    uint64_t h = seed ^ ((uint64_t)n * 0x9E3779B97F4A7C15ull);
    while (n >= 8) {
        h ^= st_get_u64(s) * 0xFF51AFD7ED558CCDull; h = ((h << 31) | (h >> 33)) * 0xC4CEB9FE1A85EC53ull;
        s += 8; n -= 8;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) v |= (uint64_t)s[i] << (8 * i);
    h ^= v * 0xFF51AFD7ED558CCDull; h = ((h << 31) | (h >> 33)) * 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 33;
    return h;
}

/* ---------- tabelas de registros fixos (busca binaria) e de strings ordenadas ----------
 * tabela fixa: u32 n, u32 largura, n*largura bytes. strings: u32 n, u32 off[n+1] (relativos ao fim da tabela de offsets), bytes. */
typedef struct { uint32_t type, version, nrec; const uint8_t *data; uint64_t size, checksum; } StoreSection;
typedef struct { const uint8_t *rec; uint32_t n, w; } StoreTable;
typedef struct { const uint8_t *off, *pool; uint32_t n; uint64_t poolsize; } StoreStrs;
static int store_table_open(const StoreSection *s, StoreTable *t) {
    t->rec = NULL; t->n = t->w = 0;
    if (!s || s->size < 8) return 0;
    t->n = st_get_u32(s->data); t->w = st_get_u32(s->data + 4);
    if ((uint64_t)t->n * t->w + 8 > s->size) { t->n = 0; return 0; }
    t->rec = s->data + 8;
    return 1;
}
static const uint8_t *store_table_at(const StoreTable *t, uint32_t i) { return t->rec + (size_t)i * t->w; }
/* primeiro indice cujo registro nao e menor que a chave (cmp(rec,key) < 0 = rec menor); t->n se nenhum */
static uint32_t store_table_lower(const StoreTable *t, const void *key, int (*cmp)(const uint8_t *rec, const void *key)) {
    uint32_t lo = 0, hi = t->n;
    while (lo < hi) { uint32_t m = lo + (hi - lo) / 2; if (cmp(store_table_at(t, m), key) < 0) lo = m + 1; else hi = m; }
    return lo;
}
/* indice do registro igual a chave, ou -1 */
static int64_t store_table_find(const StoreTable *t, const void *key, int (*cmp)(const uint8_t *rec, const void *key)) {
    uint32_t i = store_table_lower(t, key, cmp);
    return i < t->n && cmp(store_table_at(t, i), key) == 0 ? (int64_t)i : -1;
}
static void store_table_begin(StoreBuf *b, uint32_t n, uint32_t w) { sb_u32(b, n); sb_u32(b, w); }
static int store_strs_open(const StoreSection *s, StoreStrs *t) {
    memset(t, 0, sizeof *t);
    if (!s || s->size < 8) return 0;
    uint32_t n = st_get_u32(s->data);
    uint64_t head = 4 + 4 * ((uint64_t)n + 1);
    if (head > s->size) return 0;
    t->n = n; t->off = s->data + 4; t->pool = s->data + head; t->poolsize = s->size - head;
    if (st_get_u32(t->off + 4 * (size_t)n) > t->poolsize) { t->n = 0; return 0; }
    return 1;
}
static const char *store_strs_get(const StoreStrs *t, uint32_t i, uint32_t *len) {
    uint32_t a = st_get_u32(t->off + 4 * (size_t)i), b = st_get_u32(t->off + 4 * ((size_t)i + 1));
    *len = b - a; return (const char *)t->pool + a;
}
static int st_cmp_bytes(const char *a, size_t al, const char *b, size_t bl) {
    int c = memcmp(a, b, al < bl ? al : bl);
    return c ? c : (al < bl ? -1 : al > bl ? 1 : 0);
}
static int64_t store_strs_find(const StoreStrs *t, const char *key, size_t klen) {
    uint32_t lo = 0, hi = t->n;
    while (lo < hi) {
        uint32_t m = lo + (hi - lo) / 2, l; const char *s = store_strs_get(t, m, &l);
        int c = st_cmp_bytes(s, l, key, klen);
        if (c == 0) return m;
        if (c < 0) lo = m + 1; else hi = m;
    }
    return -1;
}
/* serializa strings JA ORDENADAS por bytes */
static void store_strs_build(StoreBuf *b, const char **s, const uint32_t *len, uint32_t n) {
    sb_u32(b, n);
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i++) { sb_u32(b, o); o += len[i]; }
    sb_u32(b, o);
    for (uint32_t i = 0; i < n; i++) sb_bytes(b, s[i], len[i]);
}

/* ---------- arquivos e pastas ---------- */
#ifdef _WIN32
#define STORE_PATHSEP "\\"
#else
#define STORE_PATHSEP "/"
#endif
/* pastas e arquivos do indice so para o dono (0700/0600): o lexico e as assinaturas de simbolos carregam texto dos arquivos do usuario */
#ifdef _WIN32
#define ST_MKDIR(p) MKDIR(p)
#else
#define ST_MKDIR(p) mkdir((p), 0700)
#endif
static int st_mkdirs(const char *path) {
    char d[2048]; snprintf(d, sizeof d, "%s", path);
    for (char *q = d + 1; *q; q++) if (*q == '/') { *q = 0; ST_MKDIR(d); *q = '/'; }
    ST_MKDIR(d);
    struct stat st; return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}
static int st_read_file(const char *path, uint8_t **buf, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return -1; }
    uint8_t *b = (uint8_t *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return -1; }
    size_t got = n ? fread(b, 1, (size_t)n, f) : 0;
    fclose(f);
    if (got != (size_t)n) { free(b); return -1; }
    *buf = b; *len = (size_t)n; return 0;
}
static void st_fsync_dir(const char *dir) {
#ifndef _WIN32
    int fd = open(dir, O_RDONLY);
    if (fd >= 0) { fsync(fd); close(fd); }
#else
    (void)dir;
#endif
}
/* grava um arquivo inteiro com fsync */
static int st_write_file(const char *path, const void *data, size_t n) {
#ifndef _WIN32
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    const uint8_t *p = (const uint8_t *)data; size_t left = n;
    while (left) { ssize_t w = write(fd, p, left); if (w <= 0) { close(fd); return -1; } p += w; left -= (size_t)w; }
    int ok = fsync(fd) == 0; close(fd);
    return ok ? 0 : -1;
#else
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, n, f) == n; fclose(f);
    return ok ? 0 : -1;
#endif
}

/* ---------- onde fica o armazenamento ----------
 * A raiz e decidida por roots.c (index_resolve): pasta com init (marcador ROOT) tem o store dentro dela, <raiz>/.katana/coder/store, e cobre a
 * subarvore; o resto vai para ~/.katana/coder/<hash do caminho>/store. Pasta sem init nunca ganha .katana. Leitura (mk=0) so calcula o caminho;
 * mk=1 cria a pasta (0700) e, na home, registra a raiz. O store tem um .gitignore "*" so dentro de store/. */
static void store_locate(const char *project, const char *module, char *out, size_t cap, int mk) {
    (void)module; IndexLoc L; index_resolve(project, mk, &L);
    if (mk) {
        if (st_mkdirs(L.store) != 0 && L.init) { rt_home_store(L.root, L.store, sizeof L.store); st_mkdirs(L.store); }      /* pasta com init que nao aceita escrita: o store vai para a home */
        struct stat sv; char gi[4500]; snprintf(gi, sizeof gi, "%s/.gitignore", L.store);
        if (stat(gi, &sv) != 0) { FILE *f = fopen(gi, "w"); if (f) { fputs("*\n", f); fclose(f); chmod(gi, 0600); } }
    }
    snprintf(out, cap, "%s", L.store);
}

/* ---------- leitor ---------- */
typedef struct {
    uint64_t id, size, tabsum; uint32_t nsec, count;
    const uint8_t *base; int mapped;
    StoreSection *sec;
} StoreSeg;
typedef struct { uint64_t id, size, tabsum; uint32_t nsec, count; } StoreManEnt;
typedef struct { uint64_t gen, next_id; char root[1024]; uint32_t nseg; StoreManEnt *ent; } StoreManifest;
typedef struct {
    char dir[4400]; uint64_t gen, next_id; int nseg; StoreSeg *seg; StoreManifest man;
    uint64_t man_ino, man_mtime, man_size;
} StoreReader;

static void store_seg_path(const char *dir, uint64_t id, char *out, size_t cap) { snprintf(out, cap, "%s/seg/%06llu.cbs", dir, (unsigned long long)id); }
static void store_man_encode(StoreBuf *b, const StoreManifest *m) {
    sb_u32(b, STORE_MAN_MAGIC); sb_u32(b, STORE_FORMAT); sb_u64(b, m->gen); sb_u64(b, m->next_id); sb_u32(b, m->nseg);
    uint32_t rl = (uint32_t)strlen(m->root); sb_u32(b, rl); sb_bytes(b, m->root, rl); sb_pad8(b);
    for (uint32_t i = 0; i < m->nseg; i++) { sb_u64(b, m->ent[i].id); sb_u64(b, m->ent[i].size); sb_u64(b, m->ent[i].tabsum); sb_u32(b, m->ent[i].nsec); sb_u32(b, m->ent[i].count); }
    sb_u64(b, store_hash64(b->p, b->n, 0));
}
static int store_man_parse(const uint8_t *p, size_t n, StoreManifest *m, char *err, size_t ecap) {
    memset(m, 0, sizeof *m);
    if (n < 40) { snprintf(err, ecap, "manifesto truncado"); return -1; }
    if (st_get_u32(p) != STORE_MAN_MAGIC) { snprintf(err, ecap, "manifesto: numero magico invalido"); return -1; }
    if (st_get_u32(p + 4) > STORE_FORMAT) { snprintf(err, ecap, "manifesto: formato %u mais novo que este binario (%d)", st_get_u32(p + 4), STORE_FORMAT); return -1; }
    if (st_get_u32(p + 4) < STORE_FORMAT) { snprintf(err, ecap, "manifesto: formato %u mais antigo que este binario (%d); sera reconstruido", st_get_u32(p + 4), STORE_FORMAT); return -1; }
    if (store_hash64(p, n - 8, 0) != st_get_u64(p + n - 8)) { snprintf(err, ecap, "manifesto: checksum nao confere"); return -1; }
    m->gen = st_get_u64(p + 8); m->next_id = st_get_u64(p + 16); m->nseg = st_get_u32(p + 24);
    uint32_t rl = st_get_u32(p + 28); size_t at = 32;
    uint64_t rp = ((uint64_t)rl + 7) & ~7ull;
    if (at + rp + (uint64_t)m->nseg * 32 + 8 != n || rl >= sizeof m->root) { snprintf(err, ecap, "manifesto: tamanho inconsistente"); m->nseg = 0; return -1; }
    memcpy(m->root, p + at, rl); m->root[rl] = 0; at += (size_t)rp;
    m->ent = (StoreManEnt *)calloc(m->nseg ? m->nseg : 1, sizeof *m->ent);
    for (uint32_t i = 0; i < m->nseg; i++, at += 32) {
        m->ent[i].id = st_get_u64(p + at); m->ent[i].size = st_get_u64(p + at + 8); m->ent[i].tabsum = st_get_u64(p + at + 16);
        m->ent[i].nsec = st_get_u32(p + at + 24); m->ent[i].count = st_get_u32(p + at + 28);
    }
    return 0;
}
static void store_seg_unmap(StoreSeg *s) {
    if (!s->base) return;
#ifndef _WIN32
    if (s->mapped) munmap((void *)s->base, (size_t)s->size); else
#endif
    free((void *)s->base);
    s->base = NULL; free(s->sec); s->sec = NULL;
}
/* mapeia um segmento (mmap; se falhar, le para a memoria) e valida cabecalho e limites; os checksums dos dados so em store_seg_verify */
static int store_seg_map(const char *dir, const StoreManEnt *e, StoreSeg *s, char *err, size_t ecap) {
    char path[4600]; store_seg_path(dir, e->id, path, sizeof path);
    memset(s, 0, sizeof *s);
    size_t len = 0; const uint8_t *base = NULL;
#ifndef _WIN32
    int fd = open(path, O_RDONLY);
    if (fd < 0) { snprintf(err, ecap, "segmento %llu: %s", (unsigned long long)e->id, strerror(errno)); return errno == ENOENT ? 1 : -1; }
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
        len = (size_t)st.st_size;
        void *m = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
        if (m != MAP_FAILED) { base = (const uint8_t *)m; s->mapped = 1; }
    }
    close(fd);
#endif
    if (!base) {
        uint8_t *b; if (st_read_file(path, &b, &len) != 0) { snprintf(err, ecap, "segmento %llu: nao foi possivel ler", (unsigned long long)e->id); return -1; }
        base = b; s->mapped = 0;
    }
    s->base = base; s->size = len;
    #define SEGFAIL(...) do { snprintf(err, ecap, __VA_ARGS__); store_seg_unmap(s); return -1; } while (0)
    if (len < STORE_HDR || st_get_u32(base) != STORE_SEG_MAGIC) SEGFAIL("segmento %llu: numero magico invalido", (unsigned long long)e->id);
    if (st_get_u32(base + 4) > STORE_FORMAT) SEGFAIL("segmento %llu: formato mais novo que este binario", (unsigned long long)e->id);
    if (store_hash64(base, 56, 0) != st_get_u64(base + 56)) SEGFAIL("segmento %llu: checksum do cabecalho nao confere", (unsigned long long)e->id);
    s->id = st_get_u64(base + 8); s->nsec = st_get_u32(base + 16); s->tabsum = st_get_u64(base + 32);
    if (s->id != e->id || st_get_u64(base + 24) != len || len != e->size) SEGFAIL("segmento %llu: id ou tamanho diferem do manifesto", (unsigned long long)e->id);
    if (s->nsec > STORE_MAXSEC || STORE_HDR + (uint64_t)s->nsec * STORE_SECENT > len) SEGFAIL("segmento %llu: tabela de secoes fora dos limites", (unsigned long long)e->id);
    if (s->tabsum != e->tabsum || store_hash64(base + STORE_HDR, (size_t)s->nsec * STORE_SECENT, 0) != s->tabsum) SEGFAIL("segmento %llu: checksum da tabela de secoes nao confere", (unsigned long long)e->id);
    s->count = e->count;
    s->sec = (StoreSection *)calloc(s->nsec ? s->nsec : 1, sizeof(StoreSection));
    for (uint32_t i = 0; i < s->nsec; i++) {
        const uint8_t *t = base + STORE_HDR + (size_t)i * STORE_SECENT;
        uint64_t off = st_get_u64(t + 8), sz = st_get_u64(t + 16);
        if (off > len || sz > len - off) SEGFAIL("segmento %llu: secao %u fora dos limites", (unsigned long long)e->id, i);
        s->sec[i].type = st_get_u32(t); s->sec[i].version = st_get_u32(t + 4); s->sec[i].data = base + off; s->sec[i].size = sz;
        s->sec[i].checksum = st_get_u64(t + 24); s->sec[i].nrec = st_get_u32(t + 32);
    }
    #undef SEGFAIL
    return 0;
}
/* confere o checksum de dados de todas as secoes; devolve o numero de secoes ruins (mensagem do primeiro em err) */
static int store_seg_verify(const StoreSeg *s, char *err, size_t ecap) {
    int bad = 0;
    for (uint32_t i = 0; i < s->nsec; i++)
        if (store_hash64(s->sec[i].data, (size_t)s->sec[i].size, 0) != s->sec[i].checksum) {
            if (!bad) snprintf(err, ecap, "segmento %llu: secao tipo %u (indice %u) com checksum errado", (unsigned long long)s->id, s->sec[i].type, i);
            bad++;
        }
    return bad;
}
static const StoreSection *store_seg_section(const StoreSeg *s, uint32_t type) {
    for (uint32_t i = 0; i < s->nsec; i++) if (s->sec[i].type == type) return &s->sec[i];
    return NULL;
}
/* iteracao de secoes de um tipo em todos os segmentos: *seg e *idx comecam em 0; devolve a secao (e o segmento em *segout) ou NULL no fim.
 * A ordem e do mais antigo ao mais novo. */
static const StoreSection *store_next_section(const StoreReader *r, uint32_t type, int *seg, uint32_t *idx, const StoreSeg **segout) {
    for (; *seg < r->nseg; (*seg)++, *idx = 0)
        for (; *idx < r->seg[*seg].nsec; (*idx)++)
            if (r->seg[*seg].sec[*idx].type == type) { if (segout) *segout = &r->seg[*seg]; return &r->seg[*seg].sec[(*idx)++]; }
    return NULL;
}
static void store_man_stat(const char *dir, uint64_t *ino, uint64_t *mt, uint64_t *sz) {
    char p[4600]; struct stat st; snprintf(p, sizeof p, "%s/MANIFEST", dir);
    if (stat(p, &st) != 0) { *ino = *mt = *sz = 0; return; }
    *ino = (uint64_t)st.st_ino; *sz = (uint64_t)st.st_size;
#ifdef _WIN32
    *mt = (uint64_t)st.st_mtime;
#else
    *mt = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
#endif
}
static void store_close(StoreReader *r) {
    if (!r) return;
    for (int i = 0; i < r->nseg; i++) store_seg_unmap(&r->seg[i]);
    free(r->seg); free(r->man.ent); free(r);
}
/* abre o estado publicado: le o manifesto, mapeia os segmentos. Sem manifesto = armazenamento vazio (nseg 0, gen 0). Devolve 0, ou -1 com err.
 * Se um segmento sumir (compactacao concorrente), le o manifesto de novo. Nunca toma lock. */
static int store_open(const char *dir, StoreReader **out, char *err, size_t ecap) {
    *out = NULL;
    for (int attempt = 0; attempt < 6; attempt++) {
        StoreReader *r = (StoreReader *)calloc(1, sizeof *r);
        snprintf(r->dir, sizeof r->dir, "%s", dir);
        store_man_stat(dir, &r->man_ino, &r->man_mtime, &r->man_size);
        char mp[4600]; snprintf(mp, sizeof mp, "%s/MANIFEST", dir);
        uint8_t *mb; size_t ml;
        if (st_read_file(mp, &mb, &ml) != 0) { if (errno == ENOENT || access(mp, F_OK) != 0) { *out = r; return 0; } snprintf(err, ecap, "MANIFEST ilegivel"); free(r); return -1; }
        int rc = store_man_parse(mb, ml, &r->man, err, ecap); free(mb);
        if (rc != 0) { store_close(r); return -1; }
        r->gen = r->man.gen; r->next_id = r->man.next_id; r->nseg = (int)r->man.nseg;
        r->seg = (StoreSeg *)calloc(r->nseg ? r->nseg : 1, sizeof(StoreSeg));
        int again = 0, mapped = 0;
        for (int i = 0; i < r->nseg; i++) {
            int c = store_seg_map(dir, &r->man.ent[i], &r->seg[i], err, ecap);
            if (c == 1) { again = 1; break; }
            if (c != 0) { r->nseg = mapped; store_close(r); return -1; }
            mapped++;
        }
        if (!again) { *out = r; return 0; }
        r->nseg = mapped; store_close(r);
#ifndef _WIN32
        usleep(20000);
#endif
    }
    snprintf(err, ecap, "segmentos mudando sem parar (compactacao concorrente?)");
    return -1;
}
/* 1 se o manifesto publicado mudou desde que este leitor abriu (um stat); chame store_close + store_open para atualizar */
static int store_stale(const StoreReader *r) {
    uint64_t i, m, s; store_man_stat(r->dir, &i, &m, &s);
    return i != r->man_ino || m != r->man_mtime || s != r->man_size;
}
static uint64_t store_disk_bytes(const StoreReader *r) { uint64_t t = 0; for (int i = 0; i < r->nseg; i++) t += r->seg[i].size; return t; }

/* ---------- compactacao: fusores registrados por tipo de secao ---------- */
typedef struct {
    const StoreSeg **seg; int nseg;     /* segmentos fundidos, do mais antigo ao mais novo */
    uint32_t **docmap;                  /* por segmento: indice antigo do documento -> novo, ou UINT32_MAX (morto); preenchido pelo fusor de STS_DOCS, que roda primeiro */
    int keep_tombs;                     /* 1 se existem segmentos mais antigos fora da fusao: os tombstones precisam continuar */
} StoreMergeCtx;
/* escreve a secao fundida em out e o numero de registros em *nrec; nrec 0 com out vazio = nao gravar a secao. Devolve 0 ou -1 */
typedef int (*StoreMergeFn)(StoreMergeCtx *c, StoreBuf *out, uint32_t *nrec);
static struct { uint32_t type, version; StoreMergeFn fn; } g_store_mergers[32];
static int g_store_nmerge;
static void store_register_merger(uint32_t type, uint32_t version, StoreMergeFn fn) {
    for (int i = 0; i < g_store_nmerge; i++) if (g_store_mergers[i].type == type) { g_store_mergers[i].version = version; g_store_mergers[i].fn = fn; return; }
    if (g_store_nmerge < 32) { g_store_mergers[g_store_nmerge].type = type; g_store_mergers[g_store_nmerge].version = version; g_store_mergers[g_store_nmerge++].fn = fn; }
}

/* ---------- escritor (unico, por lock de arquivo) ----------
 *   store_writer_begin    toma o lock, limpa orfaos e .tmp, abre o estado atual (w->base)
 *   store_writer_add_section / _buf   acrescenta uma secao ao segmento novo
 *   store_writer_drop     tira um segmento do manifesto novo (compactacao)
 *   store_writer_commit   grava o segmento (tmp, fsync, rename), publica o manifesto (tmp, fsync, rename), apaga os soltos
 *   store_writer_end      solta o lock */
typedef struct { uint32_t type, version, nrec; StoreBuf buf; } StorePending;
typedef struct {
    char dir[4400]; int lockfd; StoreReader *base;
    StorePending sec[STORE_MAXSEC]; int nsec;
    uint64_t drop[4096]; int ndrop;
    int orphans_removed;
} StoreWriter;

/* armazenamento criado por versao antiga (0755/0644): passa para 0700/0600 na proxima gravacao; so mexe se a pasta ou o LOCK tem bit de grupo/outros */
static void st_harden(const char *dir) {
#ifndef _WIN32
    struct stat sv; char p[4700];
    snprintf(p, sizeof p, "%s/LOCK", dir);
    const int dl = stat(dir, &sv) == 0 && (sv.st_mode & 077), fl = stat(p, &sv) == 0 && (sv.st_mode & 077);
    if (!dl && !fl) return;
    chmod(dir, 0700);
    const char *sub[2] = {"", "/seg"};
    for (int k = 0; k < 2; k++) {
        char d[4600]; snprintf(d, sizeof d, "%s%s", dir, sub[k]); if (k) chmod(d, 0700);
        DIR *dh = opendir(d); if (!dh) continue;
        struct dirent *e;
        while ((e = readdir(dh))) { if (e->d_name[0] == '.') continue; char f[4700]; snprintf(f, sizeof f, "%s/%s", d, e->d_name); if (stat(f, &sv) == 0 && S_ISREG(sv.st_mode)) chmod(f, 0600); }
        closedir(dh);
    }
    snprintf(p, sizeof p, "%s/tmp", dir); chmod(p, 0700);
#else
    (void)dir;
#endif
}
/* devolve 0 (ok), 1 (outro escritor ativo: use so leitura) ou -1 (erro, em err) */
static int store_writer_begin(const char *dir, StoreWriter **out, char *err, size_t ecap) {
    *out = NULL;
#ifdef _WIN32
    (void)dir; snprintf(err, ecap, "escrita no Windows: ainda nao"); return -1;
#else
    char p[4600];
    if (st_mkdirs(dir) != 0) { snprintf(err, ecap, "nao foi possivel criar %s", dir); return -1; }
    snprintf(p, sizeof p, "%s/seg", dir); st_mkdirs(p);
    snprintf(p, sizeof p, "%s/tmp", dir); st_mkdirs(p);
    st_harden(dir);
    snprintf(p, sizeof p, "%s/LOCK", dir);
    int fd = open(p, O_RDWR | O_CREAT, 0600);
    if (fd < 0) { snprintf(err, ecap, "LOCK: %s", strerror(errno)); return -1; }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); snprintf(err, ecap, "outro processo esta escrevendo"); return 1; }
    StoreWriter *w = (StoreWriter *)calloc(1, sizeof *w);
    snprintf(w->dir, sizeof w->dir, "%s", dir); w->lockfd = fd;
    if (store_open(dir, &w->base, err, ecap) != 0) { flock(fd, LOCK_UN); close(fd); free(w); return -1; }
    /* recuperacao: tudo em tmp/ e lixo; segmento que o manifesto nao cita e orfao (queda entre o rename do segmento e o do manifesto) */
    DIR *d;
    snprintf(p, sizeof p, "%s/tmp", dir);
    if ((d = opendir(p))) {
        struct dirent *e;
        while ((e = readdir(d))) if (e->d_name[0] != '.') { char f[4600]; snprintf(f, sizeof f, "%s/%s", p, e->d_name); if (unlink(f) == 0) w->orphans_removed++; }
        closedir(d);
    }
    snprintf(p, sizeof p, "%s/seg", dir);
    if ((d = opendir(p))) {
        struct dirent *e;
        while ((e = readdir(d))) {
            unsigned long long id; char ext[8];
            if (e->d_name[0] == '.') continue;
            int live = 0;
            if (sscanf(e->d_name, "%llu.%7s", &id, ext) == 2 && !strcmp(ext, "cbs"))
                for (int i = 0; i < w->base->nseg; i++) if (w->base->seg[i].id == id) live = 1;
            if (!live) { char f[4600]; snprintf(f, sizeof f, "%s/%s", p, e->d_name); if (unlink(f) == 0) w->orphans_removed++; }
        }
        closedir(d);
    }
    *out = w; return 0;
#endif
}
/* como store_writer_begin, mas espera ate timeout_ms enquanto outro processo escreve (o rastreador segura o lock so durante um lote) */
static int store_writer_begin_wait(const char *dir, StoreWriter **out, char *err, size_t ecap, int timeout_ms) {
    int rc = store_writer_begin(dir, out, err, ecap);
#ifndef _WIN32
    for (int waited = 0; rc == 1 && waited < timeout_ms; waited += 20) { usleep(20000); rc = store_writer_begin(dir, out, err, ecap); }
#else
    (void)timeout_ms;
#endif
    return rc;
}
static void store_writer_add_section_buf(StoreWriter *w, uint32_t type, uint32_t version, uint32_t nrec, StoreBuf *b) {   /* toma posse de b->p */
    if (w->nsec >= STORE_MAXSEC) { sb_free(b); return; }
    StorePending *s = &w->sec[w->nsec++];
    s->type = type; s->version = version; s->nrec = nrec; s->buf = *b; memset(b, 0, sizeof *b);
}
static void store_writer_add_section(StoreWriter *w, uint32_t type, uint32_t version, uint32_t nrec, const void *data, size_t len) {
    StoreBuf b = {0}; sb_bytes(&b, data, len); store_writer_add_section_buf(w, type, version, nrec, &b);
}
static void store_writer_set_root(StoreWriter *w, const char *root) { snprintf(w->base->man.root, sizeof w->base->man.root, "%s", root); }
static void store_writer_drop(StoreWriter *w, uint64_t seg_id) { if (w->ndrop < 4096) w->drop[w->ndrop++] = seg_id; }
/* grava o segmento novo (se houver secoes) e publica o manifesto novo. count = contagem de registros "principais" do segmento (informativa).
 * Devolve a nova geracao, 0 se nao havia nada a fazer, ou -1 com err. */
static int64_t store_writer_commit(StoreWriter *w, uint32_t count, char *err, size_t ecap) {
    if (!w->nsec && !w->ndrop) return 0;
    StoreManifest m; memset(&m, 0, sizeof m);
    StoreReader *b = w->base;
    m.gen = b->gen + 1; m.next_id = b->next_id ? b->next_id : 1;
    snprintf(m.root, sizeof m.root, "%s", b->man.root);
    m.ent = (StoreManEnt *)calloc((size_t)b->nseg + 1, sizeof *m.ent);
    for (int i = 0; i < b->nseg; i++) {
        int dropped = 0;
        for (int k = 0; k < w->ndrop; k++) if (w->drop[k] == b->seg[i].id) dropped = 1;
        if (!dropped) { m.ent[m.nseg].id = b->seg[i].id; m.ent[m.nseg].size = b->seg[i].size; m.ent[m.nseg].tabsum = b->seg[i].tabsum; m.ent[m.nseg].nsec = b->seg[i].nsec; m.ent[m.nseg].count = b->seg[i].count; m.nseg++; }
    }
    char tmp[4600], fin[4600];
    if (w->nsec) {
        StoreBuf s = {0};
        uint64_t id = m.next_id++;
        sb_u32(&s, STORE_SEG_MAGIC); sb_u32(&s, STORE_FORMAT); sb_u64(&s, id); sb_u32(&s, (uint32_t)w->nsec); sb_u32(&s, 0);
        sb_u64(&s, 0); sb_u64(&s, 0); while (s.n < 56) sb_u8(&s, 0); sb_u64(&s, 0);
        size_t tab = s.n; for (int i = 0; i < w->nsec * STORE_SECENT; i++) sb_u8(&s, 0);
        sb_pad8(&s);
        for (int i = 0; i < w->nsec; i++) {
            StorePending *p = &w->sec[i]; uint64_t off = s.n;
            sb_bytes(&s, p->buf.p, p->buf.n); sb_pad8(&s);
            uint8_t *t = s.p + tab + (size_t)i * STORE_SECENT;
            st_put_u32(t, p->type); st_put_u32(t + 4, p->version); st_put_u64(t + 8, off); st_put_u64(t + 16, p->buf.n);
            st_put_u64(t + 24, store_hash64(p->buf.p, p->buf.n, 0)); st_put_u32(t + 32, p->nrec); st_put_u32(t + 36, 0);
        }
        uint64_t tabsum = store_hash64(s.p + tab, (size_t)w->nsec * STORE_SECENT, 0);
        st_put_u64(s.p + 24, s.n); st_put_u64(s.p + 32, tabsum); st_put_u64(s.p + 56, store_hash64(s.p, 56, 0));
        snprintf(tmp, sizeof tmp, "%s/tmp/seg-%llu.%ld.tmp", w->dir, (unsigned long long)id, (long)getpid());
        store_seg_path(w->dir, id, fin, sizeof fin);
        if (st_write_file(tmp, s.p, s.n) != 0 || rename(tmp, fin) != 0) { snprintf(err, ecap, "nao foi possivel gravar o segmento %llu", (unsigned long long)id); unlink(tmp); sb_free(&s); free(m.ent); return -1; }
        snprintf(tmp, sizeof tmp, "%s/seg", w->dir); st_fsync_dir(tmp);
        m.ent[m.nseg].id = id; m.ent[m.nseg].size = s.n; m.ent[m.nseg].tabsum = tabsum; m.ent[m.nseg].nsec = (uint32_t)w->nsec; m.ent[m.nseg].count = count; m.nseg++;
        sb_free(&s);
    }
    /* gancho de teste de queda: KATANA_STORE_STALL=<segundos> espera aqui, com o segmento gravado e o manifesto ainda nao trocado */
    const char *stall = getenv("KATANA_STORE_STALL");
    if (stall && *stall) { fprintf(stderr, "[store] segmento gravado, manifesto ainda nao publicado (espera %ss)\n", stall); fflush(stderr);
#ifndef _WIN32
        sleep((unsigned)atoi(stall));
#endif
    }
    StoreBuf mb = {0}; store_man_encode(&mb, &m);
    snprintf(tmp, sizeof tmp, "%s/tmp/MANIFEST.%ld.tmp", w->dir, (long)getpid());
    snprintf(fin, sizeof fin, "%s/MANIFEST", w->dir);
    if (st_write_file(tmp, mb.p, mb.n) != 0 || rename(tmp, fin) != 0) { snprintf(err, ecap, "nao foi possivel publicar o manifesto"); unlink(tmp); sb_free(&mb); free(m.ent); return -1; }
    st_fsync_dir(w->dir);
    sb_free(&mb);
    for (int k = 0; k < w->ndrop; k++) { char sp[4600]; store_seg_path(w->dir, w->drop[k], sp, sizeof sp); unlink(sp); }   /* quem tem o arquivo mapeado continua lendo (POSIX) */
    int64_t gen = (int64_t)m.gen;
    /* o estado do escritor passa a ser o publicado */
    for (int i = 0; i < w->nsec; i++) sb_free(&w->sec[i].buf);
    w->nsec = 0; w->ndrop = 0;
    store_close(w->base); free(m.ent);
    w->base = NULL;
    if (store_open(w->dir, &w->base, err, ecap) != 0) return -1;
    return gen;
}
/* funde os segmentos [from, nseg) em um so, com os fusores registrados, e publica. Devolve a nova geracao, 0 se nada a fazer, -1 com err */
static int64_t store_writer_compact(StoreWriter *w, int from, char *err, size_t ecap) {
    StoreReader *b = w->base;
    if (from < 0) from = 0;
    if (b->nseg - from < 2) return 0;
    StoreMergeCtx c; memset(&c, 0, sizeof c);
    c.nseg = b->nseg - from; c.seg = (const StoreSeg **)calloc((size_t)c.nseg, sizeof *c.seg);
    c.docmap = (uint32_t **)calloc((size_t)c.nseg, sizeof *c.docmap);
    for (int i = 0; i < c.nseg; i++) c.seg[i] = &b->seg[from + i];
    c.keep_tombs = from > 0;
    uint32_t types[256]; int nt = 0;
    for (int i = 0; i < c.nseg; i++) for (uint32_t k = 0; k < c.seg[i]->nsec; k++) {
        uint32_t t = c.seg[i]->sec[k].type; int have = 0;
        for (int j = 0; j < nt; j++) if (types[j] == t) have = 1;
        if (!have && nt < 256) types[nt++] = t;
    }
    for (int i = 1; i < nt; i++) { uint32_t v = types[i]; int j = i - 1; while (j >= 0 && types[j] > v) { types[j + 1] = types[j]; j--; } types[j + 1] = v; }
    int64_t rc = 0; uint32_t count = 0;
    for (int i = 0; i < nt && rc == 0; i++) {
        StoreMergeFn fn = NULL; uint32_t ver = 0;
        for (int j = 0; j < g_store_nmerge; j++) if (g_store_mergers[j].type == types[i]) { fn = g_store_mergers[j].fn; ver = g_store_mergers[j].version; }
        if (!fn) { snprintf(err, ecap, "compactacao: nao ha fusor para secoes do tipo %u", types[i]); rc = -1; break; }
        StoreBuf out = {0}; uint32_t nrec = 0;
        if (fn(&c, &out, &nrec) != 0) { snprintf(err, ecap, "compactacao: o fusor do tipo %u falhou", types[i]); sb_free(&out); rc = -1; break; }
        if (types[i] == STS_DOCS) count = nrec;
        if (nrec == 0 && out.n == 0) { sb_free(&out); continue; }
        store_writer_add_section_buf(w, types[i], ver, nrec, &out);
    }
    if (rc == 0) {
        for (int i = 0; i < c.nseg; i++) store_writer_drop(w, c.seg[i]->id);
        rc = store_writer_commit(w, count, err, ecap);
    }
    for (int i = 0; i < c.nseg; i++) free(c.docmap[i]);
    free(c.docmap); free(c.seg);
    if (rc < 0) { for (int i = 0; i < w->nsec; i++) sb_free(&w->sec[i].buf); w->nsec = 0; w->ndrop = 0; }
    return rc;
}
static void store_writer_end(StoreWriter *w) {
    if (!w) return;
    for (int i = 0; i < w->nsec; i++) sb_free(&w->sec[i].buf);
#ifndef _WIN32
    flock(w->lockfd, LOCK_UN); close(w->lockfd);
#endif
    store_close(w->base); free(w);
}
