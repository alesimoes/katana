/*
 * katana.c - motor de inferencia em C puro para modelos abertos no formato GGUF.
 * Sem dependencias externas (so libc; AVX2 e OpenMP sao opcionais).
 *
 * Suporta: arquiteturas "llama" e "qwen2" (RMSNorm, RoPE, GQA, SwiGLU),
 *          tensores F32/F16/Q8_0/Q4_K/Q6_K (os quantizados sao usados direto do
 *          arquivo mapeado em memoria, sem conversao), tokenizador BPE lido do GGUF.
 *
 * Uso:
 *   katana info <modelo.gguf>
 *   katana selftest <modelo.gguf>                                    confere a matematica quantizada
 *   katana bench <modelo.gguf> [tokens_prompt] [tokens_geracao]      mede a velocidade (ja aquecido)
 *   katana run  <modelo.gguf> "texto" [max_tokens] [temperatura]     completa o texto
 *   katana chat <modelo.gguf> "pergunta" [max_tokens] [temperatura]  formato ChatML
 *
 * As multiplicacoes de matriz rodam na GPU (OpenCL) quando ha uma; KATANA_CPU=1 forca CPU.
 */

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#ifdef __AVX2__
#include <immintrin.h>
#endif
#ifdef _OPENMP
#include <omp.h>
#endif
#ifndef _WIN32
#define _putenv(s) putenv((char *)(s))
#endif
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#endif
/* camada Vulkan (backend opcional, KATANA_BACKEND=vulkan); o kvk.h deixa as macros G e F, removidas aqui */
#define KVK_IMPL
#include "vk/kvk.h"
#undef G
#undef F
#include "vk/shaders/mv_f32.h"
#include "vk/shaders/mv_q8.h"
#include "vk/shaders/mv_q4k.h"
#include "vk/shaders/mv_q5k.h"
#include "vk/shaders/mv_q6k.h"
#include "vk/shaders/rms_wg.h"
#include "vk/shaders/att1f_kv32.h"
#include "vk/shaders/att1f_kv16.h"
#include "vk/shaders/mm_t0_64x64_g0.h"
#include "vk/shaders/mm_t0_64x64_g1.h"
#include "vk/shaders/mm_t0_64x32w4_g0.h"
#include "vk/shaders/mm_t0_64x32_g1.h"
#include "vk/shaders/mm_t0_64x16w4_g0.h"
#include "vk/shaders/mm_t0_64x16_g1.h"
#include "vk/shaders/mm_t1_64x64_g0.h"
#include "vk/shaders/mm_t1_64x64_g1.h"
#include "vk/shaders/mm_t1_64x32w4_g0.h"
#include "vk/shaders/mm_t1_64x32_g1.h"
#include "vk/shaders/mm_t1_64x16w4_g0.h"
#include "vk/shaders/mm_t1_64x16_g1.h"
#include "vk/shaders/mm_t2_64x64_g0.h"
#include "vk/shaders/mm_t2_64x64_g1.h"
#include "vk/shaders/mm_t2_64x32w4_g0.h"
#include "vk/shaders/mm_t2_64x32_g1.h"
#include "vk/shaders/mm_t2_64x16w4_g0.h"
#include "vk/shaders/mm_t2_64x16_g1.h"
#include "vk/shaders/mm_t3_64x64_g0.h"
#include "vk/shaders/mm_t3_64x64_g1.h"
#include "vk/shaders/mm_t3_64x32w4_g0.h"
#include "vk/shaders/mm_t3_64x32_g1.h"
#include "vk/shaders/mm_t3_64x16w4_g0.h"
#include "vk/shaders/mm_t3_64x16_g1.h"
#include "vk/shaders/mm_t4_64x64_g0.h"
#include "vk/shaders/mm_t4_64x64_g1.h"
#include "vk/shaders/mm_t4_64x32w4_g0.h"
#include "vk/shaders/mm_t4_64x32_g1.h"
#include "vk/shaders/mm_t4_64x16w4_g0.h"
#include "vk/shaders/mm_t4_64x16_g1.h"
#include "vk/shaders/cvt.h"
#include "vk/shaders/rms_wg16.h"
#include "vk/shaders/gather.h"
#include "vk/shaders/qkv_post_kv32.h"
#include "vk/shaders/qkv_post_kv16.h"
#include "vk/shaders/att_flash_kv32.h"
#include "vk/shaders/att_flash_kv16.h"
#include "vk/shaders/att_scores_kv32.h"
#include "vk/shaders/att_scores_kv16.h"
#include "vk/shaders/att_softmax_kv32.h"
#include "vk/shaders/att_softmax_kv16.h"
#include "vk/shaders/att_mix_kv32.h"
#include "vk/shaders/att_mix_kv16.h"

#define MAX_CTX 4096
#define TOP_K 40

static void die(const char *msg) { fprintf(stderr, "Erro: %s\n", msg); exit(1); }

static void *xalloc(size_t n, size_t sz) {
    void *p = calloc(n ? n : 1, sz);
    if (!p) die("sem memoria");
    return p;
}

/* ================= pool de threads =================
 * Threads proprias, criadas uma vez. Entre um trabalho e o proximo elas giram em espera
 * curta (sem dormir: o proximo produto de matriz chega em microssegundos) e so dormem se
 * nada aparecer por ~1 ms, por exemplo enquanto a GPU trabalha.
 * O trabalho e repartido em blocos pequenos pegos por um contador atomico: se o sistema
 * tirar uma thread do nucleo, as outras continuam consumindo os blocos (com divisao fixa,
 * uma thread atrasada seguraria a matriz inteira).
 * KATANA_THREADS define o total (padrao: media entre nucleos fisicos e logicos). KATANA_PIN=1 fixa cada thread num
 * nucleo e KATANA_PRIO=0 tira a prioridade acima do normal. Medido numa maquina com 78% da
 * CPU ocupada por outros processos: a prioridade dobrou a vazao e fixar em nucleos piorou. */
typedef void (*PoolFn)(void *ctx, int i0, int i1);

#ifdef _WIN32
#define ATOM_INC(p) InterlockedIncrement(p)
#define ATOM_DEC(p) InterlockedDecrement(p)
#define ATOM_XADD(p, v) InterlockedExchangeAdd(p, v)
#define ATOM_SET(p, v) InterlockedExchange(p, v)
#define CPU_RELAX() YieldProcessor()
typedef LONG atom_t;
#else
#include <pthread.h>
#define ATOM_INC(p) __sync_add_and_fetch(p, 1)
#define ATOM_DEC(p) __sync_sub_and_fetch(p, 1)
#define ATOM_XADD(p, v) __sync_fetch_and_add(p, v)
#define ATOM_SET(p, v) __sync_lock_test_and_set(p, v)
#define CPU_RELAX() __builtin_ia32_pause()
typedef long atom_t;
#endif

static struct {
    int n;                              /* total de threads, contando a principal */
    volatile atom_t gen, open, active, next;
    PoolFn fn; void *ctx; int total, chunk;
    volatile atom_t st_jobs, st_main, st_work, st_sleep;   /* estatistica: trabalhos, blocos da principal e das outras, sonos */
#ifndef _WIN32
    pthread_mutex_t mu; pthread_cond_t cv;
#endif
} pool;

/* consome blocos ate acabar; devolve quantos fez */
static int pool_run(void) {
    int done = 0;
    for (;;) {
        const long i = (long)ATOM_XADD(&pool.next, pool.chunk);
        if (i >= pool.total) break;
        const long e = i + pool.chunk > pool.total ? pool.total : i + pool.chunk;
        pool.fn(pool.ctx, (int)i, (int)e);
        done++;
    }
    return done;
}

/* Protocolo: a principal so mexe nos campos do trabalho com open=0 e active=0; uma thread
   so le os campos depois de somar em active e ver open=1. Assim uma thread que acorda
   atrasada nunca le um trabalho pela metade, e a principal nao espera quem nao entrou. */
#ifdef _WIN32
static DWORD WINAPI pool_worker(LPVOID arg) {
#else
static void *pool_worker(void *arg) {
#endif
    atom_t last = 0;
    (void)arg;
    for (;;) {
        int spins = 0;
        while (pool.gen == last) {
            if (++spins < 20000) { CPU_RELAX(); continue; }
#ifdef _WIN32
            ATOM_INC(&pool.st_sleep);
            WaitOnAddress((volatile VOID *)&pool.gen, &last, sizeof last, INFINITE);
#else
            pthread_mutex_lock(&pool.mu);
            while (pool.gen == last) pthread_cond_wait(&pool.cv, &pool.mu);
            pthread_mutex_unlock(&pool.mu);
#endif
            spins = 0;
        }
        last = pool.gen;
        ATOM_INC(&pool.active);
        if (pool.open) ATOM_XADD(&pool.st_work, pool_run());
        ATOM_DEC(&pool.active);
    }
}

/* executa fn sobre [0, total) em blocos de `chunk`, usando todas as threads */
static void pool_for(PoolFn fn, void *ctx, int total, int chunk) {
    if (chunk < 1) chunk = 1;
    if (pool.n <= 1 || total <= chunk) { fn(ctx, 0, total); return; }
    pool.fn = fn; pool.ctx = ctx; pool.total = total; pool.chunk = chunk;
    ATOM_SET(&pool.next, 0);
    ATOM_SET(&pool.open, 1);
    ATOM_INC(&pool.gen);
#ifdef _WIN32
    WakeByAddressAll((PVOID)&pool.gen);
#else
    pthread_mutex_lock(&pool.mu); pthread_cond_broadcast(&pool.cv); pthread_mutex_unlock(&pool.mu);
#endif
    ATOM_INC(&pool.st_jobs);
    ATOM_XADD(&pool.st_main, pool_run());
    ATOM_SET(&pool.open, 0);
    while (pool.active) CPU_RELAX();
}

#ifdef _WIN32
/* pede ao Windows para nao reduzir a velocidade destas threads para economizar energia
   (em notebook ele faz isso com threads que julga de fundo). KATANA_THROTTLE=1 mantem o padrao. */
static void no_throttle(HANDLE h) {
    struct { ULONG Version, ControlMask, StateMask; } st = { 1, 1 /* EXECUTION_SPEED */, 0 };
    if (getenv("KATANA_THROTTLE") && atoi(getenv("KATANA_THROTTLE")) != 0) return;
    typedef BOOL (WINAPI *Fn)(HANDLE, int, LPVOID, DWORD);
    Fn f = (Fn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadInformation");
    if (f) f(h, 3 /* ThreadPowerThrottling */, &st, sizeof st);
}
#endif

static void pool_init(void) {
    int n = 0, pin = getenv("KATANA_PIN") && atoi(getenv("KATANA_PIN")) != 0;
    if (getenv("KATANA_THREADS")) n = atoi(getenv("KATANA_THREADS"));
    else if (getenv("OMP_NUM_THREADS")) n = atoi(getenv("OMP_NUM_THREADS"));
#ifdef _WIN32
    DWORD_PTR cores[64]; int nc = 0, nlog = 0;
    const int prio = !(getenv("KATANA_PRIO") && atoi(getenv("KATANA_PRIO")) == 0);
    DWORD len = 0;
    GetLogicalProcessorInformation(NULL, &len);
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info = len ? malloc(len) : NULL;
    if (info && GetLogicalProcessorInformation(info, &len))
        for (DWORD i = 0; i < len / sizeof *info && nc < 64; i++)
            if (info[i].Relationship == RelationProcessorCore) {
                cores[nc++] = info[i].ProcessorMask;
                for (DWORD_PTR mk = info[i].ProcessorMask; mk; mk &= mk - 1) nlog++;
            }
    free(info);
    /* padrao: meio caminho entre nucleos fisicos e logicos (medido: 12 de 8/16 rendeu mais) */
    if (n < 1) n = nc > 0 ? (nc + nlog) / 2 : 1;
    if (n > 64) n = 64;
    pool.n = n;
    for (int k = 1; k < n; k++) {
        HANDLE h = CreateThread(NULL, 0, pool_worker, NULL, 0, NULL);
        if (!h) { pool.n = k; break; }
        if (pin && nc > 1) SetThreadAffinityMask(h, cores[k % nc]);     /* um nucleo fisico por thread */
        if (prio) SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
        no_throttle(h);
        CloseHandle(h);
    }
    no_throttle(GetCurrentThread());
    /* prioridade acima do normal: com a maquina ocupada, uma thread nossa tirada do nucleo no
       meio de um bloco segura o trabalho inteiro ate voltar */
    if (prio) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#else
    if (n < 1) { n = (int)sysconf(_SC_NPROCESSORS_ONLN) / 2; if (n < 1) n = 1; }
    if (n > 64) n = 64;
    (void)pin;
    pthread_mutex_init(&pool.mu, NULL); pthread_cond_init(&pool.cv, NULL);
    pool.n = n;
    for (int k = 1; k < n; k++) {
        pthread_t t;
        if (pthread_create(&t, NULL, pool_worker, NULL) != 0) { pool.n = k; break; }
        pthread_detach(t);
    }
#endif
}

/* ================= mapa string -> int (hash aberto, FNV-1a) ================= */

typedef struct { const char **k; int *kl; int *v; uint32_t cap; } Map;

static uint32_t fnv(const char *s, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}
static void map_init(Map *m, uint32_t n) {
    uint32_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    m->cap = cap;
    m->k = xalloc(cap, sizeof(char *));
    m->kl = xalloc(cap, sizeof(int));
    m->v = xalloc(cap, sizeof(int));
}
static void map_put(Map *m, const char *s, int n, int v) {
    uint32_t i = fnv(s, n) & (m->cap - 1);
    while (m->k[i]) {
        if (m->kl[i] == n && memcmp(m->k[i], s, n) == 0) return; /* mantem o primeiro */
        i = (i + 1) & (m->cap - 1);
    }
    m->k[i] = s; m->kl[i] = n; m->v[i] = v;
}
static int map_get(const Map *m, const char *s, int n) {
    uint32_t i = fnv(s, n) & (m->cap - 1);
    while (m->k[i]) {
        if (m->kl[i] == n && memcmp(m->k[i], s, n) == 0) return m->v[i];
        i = (i + 1) & (m->cap - 1);
    }
    return -1;
}

/* ================= estruturas ================= */

typedef struct {
    int dim, n_layers, n_heads, n_kv, hd, hidden, ctx, vocab;
    float rope_base, eps;
    int bos, eos, add_bos;
    int rope_neox;      /* RoPE em metades (qwen2) em vez de pares adjacentes (llama) */
    int n_expert, n_used;   /* mistura de especialistas (MoE): quantos existem e quantos entram por token */
    int moe_norm;       /* renormaliza os pesos dos especialistas escolhidos (qwen3moe) */
    int qk_norm;        /* norma RMS em q e k: 1 = no vetor inteiro (olmoe), 2 = por cabeca (qwen3) */
    int hidden_exp;
    int hd_key;         /* largura de cada cabeca quando nao e dim/cabecas (qwen3moe) */
    int chat_fmt;       /* 0 = ChatML, 1 = Llama 3, 2 = Zephyr, 3 = [INST], 4 = texto simples */
    char arch[64];
} Config;

/* matriz de pesos: float32 (f) ou quantizada (q aponta direto para o arquivo mapeado) */
typedef struct {
    float *f; const uint8_t *q; uint32_t type; size_t stride;
    void *gbuf;         /* buffer na VRAM (pode ser compartilhado com outra matriz) */
    int gs, go, pair;   /* matrizes intercaladas no buffer: quantas, qual; par gate/up fundido */
} Mat;

typedef struct {
    float *attn_norm, *ffn_norm;
    float *bq, *bk, *bv, *bo;           /* bias da atencao (opcional; qwen2 usa) */
    Mat wq, wk, wv, wo, w_gate, w_up, w_down;
    float *q_norm, *k_norm;             /* norma de q e k (olmoe, qwen3) */
    Mat router;                         /* MoE: [n_expert][dim], escolhe os especialistas do token */
    Mat *eg, *eu, *ed;                  /* MoE: gate/up/down de cada especialista (fatias do tensor empilhado) */
} Layer;

typedef struct {
    char **tok; int *toklen; int *toktype; int n;
    int spm; float *score;      /* SentencePiece: funde pares pela pontuacao do token */
    Map vocab, merges;
    int *special; int n_special;
    int byte2cp[256]; int cp2byte[512];
} Tokenizer;

typedef struct {
    Config cfg;
    Tokenizer tk;
    Mat tok_emb, lm_head;
    float *out_norm;
    Layer *layers;
    float *kcache, *vcache;             /* [L][ctx][kv_dim] */
    int64_t fsize;                      /* tamanho do arquivo do modelo */
    float *x, *xb, *xb2, *q, *k, *v, *att, *hb, *hb2, *logits, *rope_freq;
    size_t n_params;
} Model;

/* ================= leitura do GGUF ================= */

typedef struct { const uint8_t *p, *end; } Rd;

static void rd_need(Rd *r, size_t n) { if ((size_t)(r->end - r->p) < n) die("GGUF truncado"); }
static uint32_t rd_u32(Rd *r) { uint32_t v; rd_need(r, 4); memcpy(&v, r->p, 4); r->p += 4; return v; }
static uint64_t rd_u64(Rd *r) { uint64_t v; rd_need(r, 8); memcpy(&v, r->p, 8); r->p += 8; return v; }
static const char *rd_str(Rd *r, int *len) {
    uint64_t n = rd_u64(r);
    rd_need(r, (size_t)n);
    const char *s = (const char *)r->p;
    r->p += n; *len = (int)n;
    return s;
}

static const int GG_SIZE[13] = {1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8};

static double rd_num(Rd *r, uint32_t t) {
    if (t > 12 || GG_SIZE[t] == 0) die("tipo numerico inesperado no GGUF");
    rd_need(r, GG_SIZE[t]);
    const uint8_t *p = r->p; r->p += GG_SIZE[t];
    switch (t) {
        case 0: case 7: return *p;
        case 1: return *(const int8_t *)p;
        case 2: { uint16_t v; memcpy(&v, p, 2); return v; }
        case 3: { int16_t v; memcpy(&v, p, 2); return v; }
        case 4: { uint32_t v; memcpy(&v, p, 4); return v; }
        case 5: { int32_t v; memcpy(&v, p, 4); return v; }
        case 6: { float v; memcpy(&v, p, 4); return v; }
        case 10: { uint64_t v; memcpy(&v, p, 8); return (double)v; }
        case 11: { int64_t v; memcpy(&v, p, 8); return (double)v; }
        default: { double v; memcpy(&v, p, 8); return v; }
    }
}

static void rd_skip(Rd *r, uint32_t t) {
    int len;
    if (t == 8) { rd_str(r, &len); return; }
    if (t == 9) {
        uint32_t et = rd_u32(r); uint64_t n = rd_u64(r);
        if (et != 8 && et != 9) {
            if (et > 12) die("tipo de array invalido");
            rd_need(r, (size_t)n * GG_SIZE[et]); r->p += n * GG_SIZE[et];
        } else for (uint64_t i = 0; i < n; i++) rd_skip(r, et);
        return;
    }
    rd_num(r, t);
}

static int key_ends(const char *k, int kl, const char *suf) {
    int sl = (int)strlen(suf);
    return kl >= sl && memcmp(k + kl - sl, suf, sl) == 0;
}
static int key_is(const char *k, int kl, const char *s) {
    return kl == (int)strlen(s) && memcmp(k, s, kl) == 0;
}

typedef struct {
    const char *name; int namelen;
    uint64_t dims[4]; uint32_t ndims, type; uint64_t offset;
} TInfo;

typedef struct { TInfo *t; uint64_t n; const uint8_t *data, *end; } Tensors;

static float f16_to_f32(uint16_t h) {
    uint32_t s = (h >> 15) & 1, m = h & 0x3ff, u;
    int e = (h >> 10) & 0x1f;
    if (e == 0) {
        if (m == 0) u = s << 31;
        else {
            e = 1;
            while (!(m & 0x400)) { m <<= 1; e--; }
            m &= 0x3ff;
            u = (s << 31) | ((uint32_t)(e + 112) << 23) | (m << 13);
        }
    } else if (e == 31) u = (s << 31) | 0x7f800000u | (m << 13);
    else u = (s << 31) | ((uint32_t)(e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &u, 4);
    return f;
}

/* tipos de tensor do GGUF */
enum { T_F32 = 0, T_F16 = 1, T_Q4_0 = 2, T_Q8_0 = 8, T_Q3_K = 11, T_Q4_K = 12, T_Q5_K = 13, T_Q6_K = 14 };

/* bytes ocupados por n elementos (0 = tipo nao suportado ou n desalinhado) */
static size_t qbytes(uint32_t type, size_t n) {
    switch (type) {
        case T_F32:  return n * 4;
        case T_F16:  return n * 2;
        case T_Q4_0: return n % 32 ? 0 : n / 32 * 18;     /* f16 d + 32 nibbles */
        case T_Q8_0: return n % 32 ? 0 : n / 32 * 34;     /* f16 d + 32 x int8 */
        case T_Q3_K: return n % 256 ? 0 : n / 256 * 110;  /* 32 bits altos, 64 de 2 bits, 12 escalas, f16 d */
        case T_Q5_K: return n % 256 ? 0 : n / 256 * 176;  /* f16 d, f16 dmin, 12 escalas, 32 bits altos, 128 nibbles */
        case T_Q4_K: return n % 256 ? 0 : n / 256 * 144;  /* f16 d, f16 dmin, 12 escalas, 128 nibbles */
        case T_Q6_K: return n % 256 ? 0 : n / 256 * 210;  /* 128 ql, 64 qh, 16 escalas, f16 d */
        default:     return 0;
    }
}

/* escala e minimo (6 bits cada) do sub-bloco j de um bloco Q4_K */
static void k4_scale_min(int j, const uint8_t *q, int *d, int *m) {
    if (j < 4) { *d = q[j] & 63; *m = q[j + 4] & 63; }
    else {
        *d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4);
        *m = (q[j + 4] >> 4) | ((q[j] >> 6) << 4);
    }
}

/* bloco Q3_K -> 256 valores inteiros (-4..3) e 16 escalas (-32..31) */
static void q3k_unpack(const uint8_t *src, int8_t *val, int8_t *sc) {
    const uint32_t km1 = 0x03030303u, km2 = 0x0f0f0f0fu;
    const uint8_t *hm = src, *q = src + 32;
    uint32_t aux[4];
    memcpy(aux, src + 96, 12);
    const uint32_t tmp = aux[2];
    aux[2] = ((aux[0] >> 4) & km2) | (((tmp >> 4) & km1) << 4);
    aux[3] = ((aux[1] >> 4) & km2) | (((tmp >> 6) & km1) << 4);
    aux[0] = (aux[0] & km2) | (((tmp >> 0) & km1) << 4);
    aux[1] = (aux[1] & km2) | (((tmp >> 2) & km1) << 4);
    for (int i = 0; i < 16; i++) sc[i] = (int8_t)(((const uint8_t *)aux)[i] - 32);
    uint8_t m = 1;
    for (int j = 0; j < 2; j++, q += 32) {
        for (int k = 0, shift = 0; k < 4; k++, shift += 2, m <<= 1) {
            for (int l = 0; l < 16; l++) *val++ = (int8_t)(((q[l] >> shift) & 3) - ((hm[l] & m) ? 0 : 4));
            for (int l = 0; l < 16; l++) *val++ = (int8_t)(((q[l + 16] >> shift) & 3) - ((hm[l + 16] & m) ? 0 : 4));
        }
    }
}

static void dequant(uint32_t type, const uint8_t *src, float *out, size_t n) {
    if (type == T_Q4_0) {
        for (size_t b = 0; b < n / 32; b++, src += 18, out += 32) {
            uint16_t h; memcpy(&h, src, 2);
            const float d = f16_to_f32(h);
            for (int j = 0; j < 16; j++) {
                out[j] = d * ((src[2 + j] & 0xF) - 8);
                out[j + 16] = d * ((src[2 + j] >> 4) - 8);
            }
        }
        return;
    }
    if (type == T_Q3_K) {
        for (size_t b = 0; b < n / 256; b++, src += 110, out += 256) {
            int8_t val[256], sc[16];
            uint16_t h; memcpy(&h, src + 108, 2);
            const float d = f16_to_f32(h);
            q3k_unpack(src, val, sc);
            for (int i = 0; i < 256; i++) out[i] = d * sc[i / 16] * val[i];
        }
        return;
    }
    if (type == T_Q5_K) {
        for (size_t b = 0; b < n / 256; b++, src += 176) {
            uint16_t h0, h1; memcpy(&h0, src, 2); memcpy(&h1, src + 2, 2);
            const float d = f16_to_f32(h0), dmin = f16_to_f32(h1);
            const uint8_t *sc = src + 4, *qh = src + 16, *q = src + 48;
            for (int j = 0; j < 8; j += 2, q += 32) {
                int s1, m1, s2, m2;
                k4_scale_min(j, sc, &s1, &m1); k4_scale_min(j + 1, sc, &s2, &m2);
                for (int l = 0; l < 32; l++) *out++ = d * s1 * ((q[l] & 0xF) + (((qh[l] >> j) & 1) << 4)) - dmin * m1;
                for (int l = 0; l < 32; l++) *out++ = d * s2 * ((q[l] >> 4) + (((qh[l] >> (j + 1)) & 1) << 4)) - dmin * m2;
            }
        }
        return;
    }
    if (type == T_F32) memcpy(out, src, n * 4);
    else if (type == T_F16) {
        for (size_t j = 0; j < n; j++) { uint16_t h; memcpy(&h, src + j * 2, 2); out[j] = f16_to_f32(h); }
    } else if (type == T_Q8_0) {
        for (size_t b = 0; b < n / 32; b++, src += 34, out += 32) {
            uint16_t h; memcpy(&h, src, 2);
            const float d = f16_to_f32(h);
            const int8_t *qs = (const int8_t *)(src + 2);
            for (int j = 0; j < 32; j++) out[j] = d * qs[j];
        }
    } else if (type == T_Q4_K) {
        for (size_t b = 0; b < n / 256; b++, src += 144) {
            uint16_t h0, h1; memcpy(&h0, src, 2); memcpy(&h1, src + 2, 2);
            const float d = f16_to_f32(h0), dmin = f16_to_f32(h1);
            const uint8_t *sc = src + 4, *q = src + 16;
            for (int j = 0; j < 8; j += 2, q += 32) {
                int s1, m1, s2, m2;
                k4_scale_min(j, sc, &s1, &m1); k4_scale_min(j + 1, sc, &s2, &m2);
                for (int l = 0; l < 32; l++) *out++ = d * s1 * (q[l] & 0xF) - dmin * m1;
                for (int l = 0; l < 32; l++) *out++ = d * s2 * (q[l] >> 4) - dmin * m2;
            }
        }
    } else if (type == T_Q6_K) {
        for (size_t b = 0; b < n / 256; b++, src += 210) {
            const uint8_t *ql = src, *qh = src + 128;
            const int8_t *sc = (const int8_t *)(src + 192);
            uint16_t h; memcpy(&h, src + 208, 2);
            const float d = f16_to_f32(h);
            for (int c = 0; c < 2; c++, out += 128, ql += 64, qh += 32, sc += 8) {
                for (int l = 0; l < 32; l++) {
                    const int is = l / 16;
                    const int q1 = (int)((ql[l] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
                    const int q2 = (int)((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
                    const int q3 = (int)((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32;
                    const int q4 = (int)((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32;
                    out[l]      = d * sc[is] * q1;
                    out[l + 32] = d * sc[is + 2] * q2;
                    out[l + 64] = d * sc[is + 4] * q3;
                    out[l + 96] = d * sc[is + 6] * q4;
                }
            }
        }
    }
}

/* devolve o tensor convertido para float32; NULL se nao existir e !required */
static float *get_tensor(const Tensors *ts, const char *name, size_t expect, int required, size_t *count) {
    int nl = (int)strlen(name);
    for (uint64_t i = 0; i < ts->n; i++) {
        const TInfo *t = &ts->t[i];
        if (t->namelen != nl || memcmp(t->name, name, nl) != 0) continue;
        size_t n = 1;
        for (uint32_t d = 0; d < t->ndims; d++) n *= (size_t)t->dims[d];
        if (n != expect) { fprintf(stderr, "Tensor %s: %zu elementos, esperava %zu\n", name, n, expect); exit(1); }
        const uint8_t *src = ts->data + t->offset;
        float *out = xalloc(n, sizeof(float));
        size_t bytes = qbytes(t->type, n);
        if (!bytes) {
            fprintf(stderr, "Tensor %s: quantizacao tipo %u nao suportada (use F32, F16, Q8_0, Q4_0, Q3_K, Q4_K, Q5_K ou Q6_K)\n", name, t->type);
            exit(1);
        }
        if (src + bytes > ts->end) die("tensor fora do arquivo");
        dequant(t->type, src, out, n);
        if (count) *count += n;
        return out;
    }
    if (required) { fprintf(stderr, "Tensor %s nao encontrado\n", name); exit(1); }
    return NULL;
}

/* matriz [rows][cols]: usa os dados quantizados direto do arquivo quando cada linha
   comeca num bloco inteiro; senao converte para float32 */
static Mat get_mat(const Tensors *ts, const char *name, size_t rows, size_t cols, int required, size_t *count) {
    Mat m = {0};
    int nl = (int)strlen(name);
    for (uint64_t i = 0; i < ts->n; i++) {
        const TInfo *t = &ts->t[i];
        if (t->namelen != nl || memcmp(t->name, name, nl) != 0) continue;
        if (t->type == T_F32 || t->type == T_F16) break;
        size_t n = 1, stride = qbytes(t->type, cols);
        if (!stride) break;
        for (uint32_t d = 0; d < t->ndims; d++) n *= (size_t)t->dims[d];
        if (n != rows * cols) { fprintf(stderr, "Tensor %s: tamanho inesperado\n", name); exit(1); }
        if (ts->data + t->offset + rows * stride > ts->end) die("tensor fora do arquivo");
        m.q = ts->data + t->offset;
        m.type = t->type; m.stride = stride;
        if (count) *count += n;
        return m;
    }
    m.f = get_tensor(ts, name, rows * cols, required, count);
    return m;
}

static void tokenizer_finish(Tokenizer *tk, const char **merges, int *mlen, int n_merges) {
    int n = 0;
    for (int b = 0; b < 256; b++) {
        if ((b >= 33 && b <= 126) || (b >= 161 && b <= 172) || b >= 174) tk->byte2cp[b] = b;
        else tk->byte2cp[b] = 256 + n++;
    }
    for (int i = 0; i < 512; i++) tk->cp2byte[i] = -1;
    for (int b = 0; b < 256; b++) tk->cp2byte[tk->byte2cp[b]] = b;

    map_init(&tk->vocab, tk->n);
    tk->special = xalloc(tk->n, sizeof(int));
    for (int i = 0; i < tk->n; i++) {
        map_put(&tk->vocab, tk->tok[i], tk->toklen[i], i);
        if (tk->toktype && (tk->toktype[i] == 3 || tk->toktype[i] == 4) && tk->toklen[i] > 0)
            tk->special[tk->n_special++] = i;
    }
    map_init(&tk->merges, n_merges);
    for (int i = 0; i < n_merges; i++) map_put(&tk->merges, merges[i], mlen[i], i);
}

static void gpu_setup(Model *m);
static void shards_open(const char *path, int64_t fsz);

/* Aquecimento dos especialistas (MoE): no Windows a primeira leitura de cada pagina do arquivo
   mapeado custa caro, e num MoE cada token toca especialistas novos. Algumas threads de baixa
   prioridade percorrem os tensores de especialistas logo na carga, para esse custo nao cair
   no caminho dos primeiros tokens. KATANA_WARM=0 desliga. */
#define WARM_T 4
typedef struct { const Model *m; int t; } WarmArg;
static volatile long warm_sink;
#ifdef _WIN32
static DWORD WINAPI warm_thread(LPVOID a) {
    const WarmArg *w = a;
    const Config *c = &w->m->cfg;
    const size_t d = c->dim, H = c->hidden, E = c->n_expert;
    long acc = 0;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (int l = w->t; l < c->n_layers; l += WARM_T) {
        const Layer *L = &w->m->layers[l];
        const Mat *ms[3] = { &L->eg[0], &L->eu[0], &L->ed[0] };
        for (int k = 0; k < 3; k++) {
            if (!ms[k]->q) continue;
            const size_t len = E * (k < 2 ? H : d) * ms[k]->stride;
            for (size_t o = 0; o < len; o += 4096) acc += ms[k]->q[o];
        }
    }
    warm_sink += acc;
    return 0;
}
#endif
static void warm_start(const Model *m) {
#ifdef _WIN32
    if (getenv("KATANA_WARM") && atoi(getenv("KATANA_WARM")) == 0) return;
    for (int t = 0; t < WARM_T; t++) {
        WarmArg *w = xalloc(1, sizeof(WarmArg));
        w->m = m; w->t = t;
        HANDLE h = CreateThread(NULL, 0, warm_thread, w, 0, NULL);
        if (h) CloseHandle(h);
    }
#else
    (void)m;
#endif
}
static void prefetch_setup(int64_t fsz);

/* mapeia o arquivo em memoria (somente leitura): o SO carrega as paginas sob demanda
   e o mapeamento fica vivo ate o processo terminar */
static const uint8_t *map_file(const char *path, int64_t *size) {
#ifdef _WIN32
    int wn = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *w = xalloc(wn, sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, wn);
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart == 0) return NULL;
    HANDLE mp = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mp) return NULL;
    *size = sz.QuadPart;
    return MapViewOfFile(mp, FILE_MAP_READ, 0, 0, 0);
#else
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size == 0) return NULL;
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    *size = st.st_size;
    return p == MAP_FAILED ? NULL : p;
#endif
}

static void load_model(Model *m, const char *path) {
    int64_t fsz = 0;
    const uint8_t *buf = map_file(path, &fsz);
    if (!buf) { fprintf(stderr, "Nao consegui abrir %s\n", path); exit(1); }

    Rd r = { buf, buf + fsz };
    if (rd_u32(&r) != 0x46554747u) die("nao e um arquivo GGUF");
    uint32_t ver = rd_u32(&r);
    if (ver < 2) die("versao de GGUF nao suportada");
    uint64_t n_tensors = rd_u64(&r), n_kv = rd_u64(&r);

    Config *c = &m->cfg;
    c->rope_base = 10000.0f; c->eps = 1e-5f; c->bos = -1; c->eos = -1; c->add_bos = -1;
    char *tmpl = NULL;
    uint64_t align = 32;
    const char **merges = NULL; int *mlen = NULL; int n_merges = 0;
    Tokenizer *tk = &m->tk;

    for (uint64_t i = 0; i < n_kv; i++) {
        int kl; const char *k = rd_str(&r, &kl);
        uint32_t t = rd_u32(&r);
        if (t == 8 && key_is(k, kl, "general.architecture")) {
            int sl; const char *s = rd_str(&r, &sl);
            if (sl > 63) sl = 63;
            memcpy(c->arch, s, sl);
        } else if (t == 9 && key_is(k, kl, "tokenizer.ggml.tokens")) {
            uint32_t et = rd_u32(&r); uint64_t n = rd_u64(&r);
            if (et != 8) die("tokens: tipo inesperado");
            tk->n = (int)n;
            tk->tok = xalloc(n, sizeof(char *)); tk->toklen = xalloc(n, sizeof(int));
            for (uint64_t j = 0; j < n; j++) {
                int sl; const char *s = rd_str(&r, &sl);
                tk->tok[j] = xalloc(sl + 1, 1); memcpy(tk->tok[j], s, sl); tk->toklen[j] = sl;
            }
        } else if (t == 8 && key_is(k, kl, "tokenizer.ggml.model")) {
            int sl; const char *sv = rd_str(&r, &sl);
            tk->spm = sl == 5 && memcmp(sv, "llama", 5) == 0;
        } else if (t == 8 && key_is(k, kl, "tokenizer.chat_template")) {
            int sl; const char *sv = rd_str(&r, &sl);
            tmpl = xalloc((size_t)sl + 1, 1); memcpy(tmpl, sv, sl);
        } else if (t == 9 && key_is(k, kl, "tokenizer.ggml.scores")) {
            uint32_t et = rd_u32(&r); uint64_t n = rd_u64(&r);
            tk->score = xalloc(n, sizeof(float));
            for (uint64_t j = 0; j < n; j++) tk->score[j] = (float)rd_num(&r, et);
        } else if (t == 9 && key_is(k, kl, "tokenizer.ggml.merges")) {
            uint32_t et = rd_u32(&r); uint64_t n = rd_u64(&r);
            if (et != 8) die("merges: tipo inesperado");
            n_merges = (int)n;
            merges = xalloc(n, sizeof(char *)); mlen = xalloc(n, sizeof(int));
            for (uint64_t j = 0; j < n; j++) {
                int sl; const char *s = rd_str(&r, &sl);
                char *cp = xalloc(sl + 1, 1); memcpy(cp, s, sl);
                merges[j] = cp; mlen[j] = sl;
            }
        } else if (t == 9 && key_is(k, kl, "tokenizer.ggml.token_type")) {
            uint32_t et = rd_u32(&r); uint64_t n = rd_u64(&r);
            tk->toktype = xalloc(n, sizeof(int));
            for (uint64_t j = 0; j < n; j++) tk->toktype[j] = (int)rd_num(&r, et);
        } else if (t != 8 && t != 9) {
            double v = rd_num(&r, t);
            if (key_ends(k, kl, ".block_count")) c->n_layers = (int)v;
            else if (key_ends(k, kl, ".context_length")) c->ctx = (int)v;
            else if (key_ends(k, kl, ".embedding_length")) c->dim = (int)v;
            else if (key_ends(k, kl, ".expert_feed_forward_length")) c->hidden_exp = (int)v;
            else if (key_ends(k, kl, ".feed_forward_length")) c->hidden = (int)v;
            else if (key_ends(k, kl, ".expert_count")) c->n_expert = (int)v;
            else if (key_ends(k, kl, ".expert_used_count")) c->n_used = (int)v;
            else if (key_ends(k, kl, ".attention.head_count")) c->n_heads = (int)v;
            else if (key_ends(k, kl, ".attention.head_count_kv")) c->n_kv = (int)v;
            else if (key_ends(k, kl, ".attention.key_length")) c->hd_key = (int)v;
            else if (key_ends(k, kl, ".rope.freq_base")) c->rope_base = (float)v;
            else if (key_ends(k, kl, ".layer_norm_rms_epsilon")) c->eps = (float)v;
            else if (key_is(k, kl, "tokenizer.ggml.bos_token_id")) c->bos = (int)v;
            else if (key_is(k, kl, "tokenizer.ggml.eos_token_id")) c->eos = (int)v;
            else if (key_is(k, kl, "tokenizer.ggml.add_bos_token")) c->add_bos = (int)v;
            else if (key_is(k, kl, "general.alignment")) align = (uint64_t)v;
        } else rd_skip(&r, t);
    }

    if (strcmp(c->arch, "qwen2") == 0) c->rope_neox = 1;
    else if (strcmp(c->arch, "olmoe") == 0) { c->rope_neox = 1; c->qk_norm = 1; }
    else if (strcmp(c->arch, "qwen3") == 0) { c->rope_neox = 1; c->qk_norm = 2; }
    else if (strcmp(c->arch, "qwen3moe") == 0) { c->rope_neox = 1; c->qk_norm = 2; c->moe_norm = 1; }
    else if (strcmp(c->arch, "llama") != 0)
        fprintf(stderr, "Aviso: arquitetura '%s' nao testada (so 'llama' e 'qwen2' por enquanto)\n", c->arch);
    if (getenv("KATANA_ROPE")) c->rope_neox = atoi(getenv("KATANA_ROPE"));
    if (c->n_expert) {
        if (c->hidden_exp) c->hidden = c->hidden_exp;      /* daqui em diante, hidden = largura de UM especialista */
        if (c->n_used < 1 || c->n_used > 64 || c->n_used > c->n_expert || c->n_expert > 1024) die("MoE: contagem de especialistas invalida");
    }
    if (!c->dim || !c->n_layers || !c->n_heads || !c->hidden || !tk->n) die("metadados incompletos no GGUF");
    if (!c->n_kv) c->n_kv = c->n_heads;
    c->vocab = tk->n;
    c->hd = c->hd_key > 0 ? c->hd_key : c->dim / c->n_heads;       /* q tem n_heads*hd, que pode passar de dim */
    if ((!c->hd_key && c->dim % c->n_heads) || c->hd % 2 || c->n_heads % c->n_kv) die("dimensoes de atencao invalidas");
    if (!c->ctx || c->ctx > MAX_CTX) c->ctx = MAX_CTX;
    if (getenv("KATANA_CTX")) {     /* contexto menor = cache KV menor (libera VRAM para mais camadas) */
        const int want = atoi(getenv("KATANA_CTX"));
        if (want >= 64 && want < c->ctx) c->ctx = want;
    }
    if (!merges && !tk->spm) die("GGUF sem merges BPE nem SentencePiece (tokenizador nao suportado)");
    if (tk->spm && !tk->score) tk->score = xalloc(tk->n, sizeof(float));
    if (c->add_bos < 0) c->add_bos = tk->spm;      /* SentencePiece comeca com BOS por padrao */
    tokenizer_finish(tk, merges, mlen, n_merges);
    /* formato de conversa: pelo template do proprio modelo; sem template, pelo vocabulario */
    if (tmpl && strstr(tmpl, "<|im_start|>")) c->chat_fmt = 0;
    else if (tmpl && strstr(tmpl, "<|start_header_id|>")) c->chat_fmt = 1;
    else if (tmpl && strstr(tmpl, "<|user|>")) c->chat_fmt = 2;
    else if (tmpl && strstr(tmpl, "[INST]")) c->chat_fmt = 3;
    else if (map_get(&tk->vocab, "<|im_start|>", 12) >= 0) c->chat_fmt = 0;
    else if (map_get(&tk->vocab, "<|start_header_id|>", 19) >= 0) c->chat_fmt = 1;
    else c->chat_fmt = 4;
    free(tmpl);

    Tensors ts;
    ts.n = n_tensors; ts.t = xalloc(n_tensors, sizeof(TInfo));
    for (uint64_t i = 0; i < n_tensors; i++) {
        TInfo *t = &ts.t[i];
        t->name = rd_str(&r, &t->namelen);
        t->ndims = rd_u32(&r);
        if (t->ndims > 4) die("tensor com dimensoes demais");
        for (uint32_t d = 0; d < t->ndims; d++) t->dims[d] = rd_u64(&r);
        t->type = rd_u32(&r);
        t->offset = rd_u64(&r);
    }
    uint64_t pos = (uint64_t)(r.p - buf);
    pos = (pos + align - 1) / align * align;
    ts.data = buf + pos; ts.end = buf + fsz;

    size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd, V = c->vocab;
    const size_t qd = (size_t)c->n_heads * c->hd, dq = qd > d ? qd : d;
    size_t *np = &m->n_params;
    m->tok_emb = get_mat(&ts, "token_embd.weight", V, d, 1, np);
    m->out_norm = get_tensor(&ts, "output_norm.weight", d, 1, np);
    m->lm_head = get_mat(&ts, "output.weight", V, d, 0, np);
    if (!m->lm_head.f && !m->lm_head.q) m->lm_head = m->tok_emb;   /* pesos amarrados */
    m->layers = xalloc(c->n_layers, sizeof(Layer));
    for (int l = 0; l < c->n_layers; l++) {
        Layer *L = &m->layers[l];
        char nm[96];
#define V1(field, suffix) snprintf(nm, sizeof nm, "blk.%d." suffix, l); L->field = get_tensor(&ts, nm, d, 1, np)
#define M2(field, suffix, rows, cols) snprintf(nm, sizeof nm, "blk.%d." suffix, l); L->field = get_mat(&ts, nm, rows, cols, 1, np)
        V1(attn_norm, "attn_norm.weight");
        M2(wq, "attn_q.weight", qd, d);
        M2(wk, "attn_k.weight", kvd, d);
        M2(wv, "attn_v.weight", kvd, d);
        M2(wo, "attn_output.weight", d, qd);
        V1(ffn_norm, "ffn_norm.weight");
        if (c->n_expert) {
            /* tensores empilhados: o especialista e ocupa a fatia e, com as linhas inteiras */
            const size_t E = c->n_expert;
            M2(router, "ffn_gate_inp.weight", E, d);
            Mat sg, su, sd;
            snprintf(nm, sizeof nm, "blk.%d.ffn_gate_exps.weight", l); sg = get_mat(&ts, nm, E * H, d, 1, np);
            snprintf(nm, sizeof nm, "blk.%d.ffn_up_exps.weight", l);   su = get_mat(&ts, nm, E * H, d, 1, np);
            snprintf(nm, sizeof nm, "blk.%d.ffn_down_exps.weight", l); sd = get_mat(&ts, nm, E * d, H, 1, np);
            L->eg = xalloc(E, sizeof(Mat)); L->eu = xalloc(E, sizeof(Mat)); L->ed = xalloc(E, sizeof(Mat));
            for (size_t e = 0; e < E; e++) {
                L->eg[e] = sg; L->eu[e] = su; L->ed[e] = sd;
                if (sg.q) L->eg[e].q += e * H * sg.stride; else L->eg[e].f += e * H * d;
                if (su.q) L->eu[e].q += e * H * su.stride; else L->eu[e].f += e * H * d;
                if (sd.q) L->ed[e].q += e * d * sd.stride; else L->ed[e].f += e * d * H;
            }
            L->w_gate = L->eg[0]; L->w_up = L->eu[0]; L->w_down = L->ed[0];    /* para os testes por matriz */
        } else {
            M2(w_gate, "ffn_gate.weight", H, d);
            M2(w_up, "ffn_up.weight", H, d);
            M2(w_down, "ffn_down.weight", d, H);
        }
        if (c->qk_norm) {
            snprintf(nm, sizeof nm, "blk.%d.attn_q_norm.weight", l); L->q_norm = get_tensor(&ts, nm, c->qk_norm == 1 ? d : (size_t)c->hd, 1, np);
            snprintf(nm, sizeof nm, "blk.%d.attn_k_norm.weight", l); L->k_norm = get_tensor(&ts, nm, c->qk_norm == 1 ? kvd : (size_t)c->hd, 1, np);
        }
        snprintf(nm, sizeof nm, "blk.%d.attn_q.bias", l);      L->bq = get_tensor(&ts, nm, qd, 0, np);
        snprintf(nm, sizeof nm, "blk.%d.attn_k.bias", l);      L->bk = get_tensor(&ts, nm, kvd, 0, np);
        snprintf(nm, sizeof nm, "blk.%d.attn_v.bias", l);      L->bv = get_tensor(&ts, nm, kvd, 0, np);
        snprintf(nm, sizeof nm, "blk.%d.attn_output.bias", l); L->bo = get_tensor(&ts, nm, d, 0, np);
#undef V1
#undef M2
    }
    free(ts.t);   /* o mapeamento do arquivo continua vivo: os pesos apontam para ele */

    size_t ctx = c->ctx, L = c->n_layers;
    m->kcache = xalloc(L * ctx * kvd, sizeof(float));
    m->vcache = xalloc(L * ctx * kvd, sizeof(float));
    m->x = xalloc(d, sizeof(float));   m->xb = xalloc(dq, sizeof(float));
    m->xb2 = xalloc(dq, sizeof(float)); m->q = xalloc(dq, sizeof(float));
    m->k = xalloc(kvd, sizeof(float)); m->v = xalloc(kvd, sizeof(float));
    m->att = xalloc(ctx, sizeof(float));
    m->hb = xalloc(H, sizeof(float));  m->hb2 = xalloc(H, sizeof(float));
    m->logits = xalloc(V, sizeof(float));
    m->rope_freq = xalloc(c->hd / 2, sizeof(float));
    for (int i = 0; i < c->hd / 2; i++)
        m->rope_freq[i] = powf(c->rope_base, -2.0f * i / c->hd);

    /* por ultimo: a GPU precisa de tudo acima ja preenchido (ex.: rope_freq) */
    m->fsize = fsz;
    shards_open(path, fsz);
    prefetch_setup(fsz);
    if (c->n_expert) warm_start(m);
    if (!getenv("KATANA_CPU")) gpu_setup(m);
}

/* ================= tokenizador BPE (estilo GPT-2) ================= */

typedef struct { int *ids; int n, cap; } Ids;

static void ids_push(Ids *o, int id) {
    if (o->n == o->cap) { o->cap = o->cap ? o->cap * 2 : 256; o->ids = realloc(o->ids, o->cap * sizeof(int)); if (!o->ids) die("sem memoria"); }
    o->ids[o->n++] = id;
}

/* aplica BPE em uma "palavra" (bytes crus) e empilha os ids */
static void bpe_word(const Tokenizer *tk, const uint8_t *s, int n, Ids *out) {
    char *buf = xalloc((size_t)n * 2 + 1, 1);
    int *st = xalloc(n, sizeof(int)), *ln = xalloc(n, sizeof(int));
    char *pair = xalloc((size_t)n * 2 + 2, 1);
    int bl = 0, ns = 0;

    for (int i = 0; i < n; i++) {   /* um simbolo por caractere unicode (bytes mapeados) */
        int start = bl, cp = tk->byte2cp[s[i]];
        if (cp < 0x80) buf[bl++] = (char)cp;
        else { buf[bl++] = (char)(0xC0 | (cp >> 6)); buf[bl++] = (char)(0x80 | (cp & 0x3F)); }
        /* bytes de continuacao UTF-8 viram simbolos proprios (BPE em nivel de byte) */
        st[ns] = start; ln[ns] = bl - start; ns++;
    }
    for (;;) {
        int best = -1, best_rank = 0x7fffffff;
        for (int i = 0; i + 1 < ns; i++) {
            int a = ln[i], b = ln[i + 1];
            memcpy(pair, buf + st[i], a); pair[a] = ' '; memcpy(pair + a + 1, buf + st[i + 1], b);
            int rk = map_get(&tk->merges, pair, a + 1 + b);
            if (rk >= 0 && rk < best_rank) { best_rank = rk; best = i; }
        }
        if (best < 0) break;
        ln[best] += ln[best + 1];
        for (int i = best + 1; i + 1 < ns; i++) { st[i] = st[i + 1]; ln[i] = ln[i + 1]; }
        ns--;
    }
    for (int i = 0; i < ns; i++) {
        int id = map_get(&tk->vocab, buf + st[i], ln[i]);
        ids_push(out, id >= 0 ? id : 0);
    }
    free(buf); free(st); free(ln); free(pair);
}

static int c_space(int c)  { return c == ' ' || (c >= 9 && c <= 13); }
static int c_digit(int c)  { return c >= '0' && c <= '9'; }
static int c_letter(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80; }
static int c_other(int c)  { return !c_space(c) && !c_digit(c) && !c_letter(c); }

/* pre-tokenizacao aproximada do regex do GPT-2 (digitos isolados, como no SmolLM) */
static void encode_plain(const Tokenizer *tk, const uint8_t *s, int n, Ids *out) {
    int i = 0;
    while (i < n) {
        int c = s[i], j, k;
        if (c_digit(c)) { bpe_word(tk, s + i, 1, out); i++; continue; }
        if (c == '\'' && i + 1 < n) {   /* contracoes do ingles */
            int len = 0, a = s[i + 1], b = i + 2 < n ? s[i + 2] : 0;
            if ((a == 'r' && b == 'e') || (a == 'v' && b == 'e') || (a == 'l' && b == 'l')) len = 3;
            else if (a == 's' || a == 't' || a == 'm' || a == 'd') len = 2;
            if (len) { bpe_word(tk, s + i, len, out); i += len; continue; }
        }
        j = (c == ' ' && i + 1 < n && (c_letter(s[i + 1]) || c_other(s[i + 1]))) ? i + 1 : i;
        if (c_letter(s[j])) {
            for (k = j; k < n && c_letter(s[k]); k++);
            bpe_word(tk, s + i, k - i, out); i = k; continue;
        }
        if (c_other(s[j])) {
            for (k = j; k < n && c_other(s[k]); k++);
            bpe_word(tk, s + i, k - i, out); i = k; continue;
        }
        for (k = i; k < n && c_space(s[k]); k++);
        if (k < n && k - i > 1) k--;    /* o ultimo espaco fica para a proxima palavra */
        bpe_word(tk, s + i, k - i, out); i = k;
    }
}

/* SentencePiece: espacos viram U+2581; parte de um simbolo por caractere e funde, a cada
   passo, o par vizinho cujo token resultante existe e tem a maior pontuacao. O que sobrar
   fora do vocabulario sai como tokens de byte (<0xNN>). */
static void spm_encode(const Tokenizer *tk, const uint8_t *s, int n, Ids *out, int prefix) {
    char *buf = xalloc((size_t)n * 3 + 4, 1);
    int bl = 0, ns = 0;
    if (prefix) { buf[bl++] = (char)0xE2; buf[bl++] = (char)0x96; buf[bl++] = (char)0x81; }
    for (int i = 0; i < n; i++) {
        if (s[i] == ' ') { buf[bl++] = (char)0xE2; buf[bl++] = (char)0x96; buf[bl++] = (char)0x81; }
        else buf[bl++] = (char)s[i];
    }
    int *st = xalloc(bl + 1, sizeof(int)), *ln = xalloc(bl + 1, sizeof(int));
    for (int i = 0; i < bl; ) {         /* um simbolo por caractere UTF-8 */
        const uint8_t ch = (uint8_t)buf[i];
        int l = ch < 0x80 ? 1 : ch < 0xE0 ? 2 : ch < 0xF0 ? 3 : 4;
        if (i + l > bl) l = bl - i;
        st[ns] = i; ln[ns] = l; ns++; i += l;
    }
    for (;;) {
        int best = -1; float bscore = -1e30f;
        for (int i = 0; i + 1 < ns; i++) {
            const int id = map_get(&tk->vocab, buf + st[i], ln[i] + ln[i + 1]);
            if (id >= 0 && tk->score[id] > bscore) { bscore = tk->score[id]; best = i; }
        }
        if (best < 0) break;
        ln[best] += ln[best + 1];
        for (int i = best + 1; i + 1 < ns; i++) { st[i] = st[i + 1]; ln[i] = ln[i + 1]; }
        ns--;
    }
    for (int i = 0; i < ns; i++) {
        const int id = map_get(&tk->vocab, buf + st[i], ln[i]);
        if (id >= 0) { ids_push(out, id); continue; }
        for (int j = 0; j < ln[i]; j++) {
            char bt[8];
            snprintf(bt, sizeof bt, "<0x%02X>", (uint8_t)buf[st[i] + j]);
            const int bid = map_get(&tk->vocab, bt, 6);
            ids_push(out, bid >= 0 ? bid : 0);
        }
    }
    free(buf); free(st); free(ln);
}

/* codifica texto, reconhecendo tokens especiais (ex.: <|im_start|>, </s>) */
static void encode(const Tokenizer *tk, const char *text, Ids *out) {
    const uint8_t *s = (const uint8_t *)text;
    int n = (int)strlen(text), start = 0, i = 0, prev_special = 1;
    while (i <= n) {
        int hit = -1;
        if (i < n && s[i] == '<') {
            for (int j = 0; j < tk->n_special; j++) {
                int id = tk->special[j], l = tk->toklen[id];
                if (l <= n - i && memcmp(s + i, tk->tok[id], l) == 0 && (hit < 0 || l > tk->toklen[hit])) hit = id;
            }
        }
        if (hit >= 0 || i == n) {
            if (i > start) {
                if (tk->spm) spm_encode(tk, s + start, i - start, out, prev_special);
                else encode_plain(tk, s + start, i - start, out);
                prev_special = 0;
            }
            if (hit < 0) break;
            ids_push(out, hit);
            prev_special = 1;
            i += tk->toklen[hit]; start = i;
        } else i++;
    }
}

/* escreve os bytes originais de um token */
static void print_token(const Tokenizer *tk, int id, FILE *f) {
    const uint8_t *s = (const uint8_t *)tk->tok[id];
    int n = tk->toklen[id], i = 0;
    if (tk->spm) {
        if (tk->toktype && tk->toktype[id] == 6 && n == 6 && s[0] == '<') {     /* token de byte <0xNN> */
            fputc((int)strtol((const char *)s + 3, NULL, 16), f);
            return;
        }
        while (i < n) {
            if (i + 2 < n && s[i] == 0xE2 && s[i + 1] == 0x96 && s[i + 2] == 0x81) { fputc(' ', f); i += 3; }
            else fputc(s[i++], f);
        }
        return;
    }
    while (i < n) {
        int cp = s[i], adv = 1;
        if (cp >= 0xC0 && cp < 0xE0 && i + 1 < n) { cp = ((cp & 0x1F) << 6) | (s[i + 1] & 0x3F); adv = 2; }
        if (adv <= 2 && cp < 512 && tk->cp2byte[cp] >= 0) fputc(tk->cp2byte[cp], f);
        else fwrite(s + i, 1, adv, f);
        i += adv;
    }
}

/* ================= matematica ================= */

static float dot(const float *a, const float *b, int n) {
    int j = 0;
    float s = 0.0f;
#ifdef __AVX2__
    __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
    for (; j + 16 <= n; j += 16) {
        acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + j), _mm256_loadu_ps(b + j), acc0);
        acc1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + j + 8), _mm256_loadu_ps(b + j + 8), acc1);
    }
    acc0 = _mm256_add_ps(acc0, acc1);
    __m128 lo = _mm_add_ps(_mm256_castps256_ps128(acc0), _mm256_extractf128_ps(acc0, 1));
    lo = _mm_hadd_ps(lo, lo); lo = _mm_hadd_ps(lo, lo);
    s = _mm_cvtss_f32(lo);
#endif
    for (; j < n; j++) s += a[j] * b[j];
    return s;
}

/* produto de uma linha Q8_0 (blocos: f16 escala + 32 x int8) por um vetor float */
static float dot_q8(const uint8_t *row, const float *x, int cols) {
    const int nb = cols / 32;
#ifdef __AVX2__
    __m256 acc = _mm256_setzero_ps();
    for (int b = 0; b < nb; b++, row += 34, x += 32) {
        uint16_t h; memcpy(&h, row, 2);
        const __m256 d = _mm256_set1_ps(_mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))));
        const uint8_t *qs = row + 2;
#define Q8(o) _mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_loadl_epi64((const __m128i *)(qs + (o)))))
        __m256 s = _mm256_mul_ps(Q8(0), _mm256_loadu_ps(x));
        s = _mm256_fmadd_ps(Q8(8), _mm256_loadu_ps(x + 8), s);
        s = _mm256_fmadd_ps(Q8(16), _mm256_loadu_ps(x + 16), s);
        s = _mm256_fmadd_ps(Q8(24), _mm256_loadu_ps(x + 24), s);
#undef Q8
        acc = _mm256_fmadd_ps(s, d, acc);
    }
    __m128 lo = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
    lo = _mm_hadd_ps(lo, lo); lo = _mm_hadd_ps(lo, lo);
    return _mm_cvtss_f32(lo);
#else
    float total = 0.0f;
    for (int b = 0; b < nb; b++, row += 34, x += 32) {
        uint16_t h; memcpy(&h, row, 2);
        const int8_t *qs = (const int8_t *)(row + 2);
        float s = 0.0f;
        for (int j = 0; j < 32; j++) s += qs[j] * x[j];
        total += s * f16_to_f32(h);
    }
    return total;
#endif
}

#ifdef __AVX2__
static float hsum256(__m256 v) {
    __m128 lo = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    lo = _mm_hadd_ps(lo, lo); lo = _mm_hadd_ps(lo, lo);
    return _mm_cvtss_f32(lo);
}
/* soma de 16 bytes (sem sinal / com sinal) vezes 16 floats, em 8 acumuladores */
static __m256 mul16_u8(__m128i v, const float *x) {
    __m256 s = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(v)), _mm256_loadu_ps(x));
    return _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_srli_si128(v, 8))), _mm256_loadu_ps(x + 8), s);
}
static __m256 mul16_i8(__m128i v, const float *x) {
    __m256 s = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(v)), _mm256_loadu_ps(x));
    return _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_srli_si128(v, 8))), _mm256_loadu_ps(x + 8), s);
}
#endif

/* linha Q4_K vezes vetor float. Cada peso vale d*sc*q - dmin*m, entao por sub-bloco de 32:
   soma = d*sc * SUM(q_i*x_i) - dmin*m * SUM(x_i); sx traz SUM(x_i) ja calculado por sub-bloco */
static float dot_q4k(const uint8_t *row, const float *x, const float *sx, int cols) {
#ifdef __AVX2__
    const __m256i m4 = _mm256_set1_epi8(0x0F);
    __m256 acc = _mm256_setzero_ps();
    float corr = 0.0f;
    for (int b = 0; b < cols / 256; b++, row += 144) {
        uint16_t h0, h1; memcpy(&h0, row, 2); memcpy(&h1, row + 2, 2);
        const float d = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h0)));
        const float dmin = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h1)));
        const uint8_t *sc = row + 4, *q = row + 16;
        for (int j = 0; j < 8; j += 2, q += 32, x += 64, sx += 2) {
            int s1, m1, s2, m2;
            k4_scale_min(j, sc, &s1, &m1); k4_scale_min(j + 1, sc, &s2, &m2);
            const __m256i v = _mm256_loadu_si256((const __m256i *)q);
            const __m256i lo = _mm256_and_si256(v, m4);
            const __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), m4);
            __m256 a = _mm256_add_ps(mul16_u8(_mm256_castsi256_si128(lo), x),
                                     mul16_u8(_mm256_extracti128_si256(lo, 1), x + 16));
            __m256 c = _mm256_add_ps(mul16_u8(_mm256_castsi256_si128(hi), x + 32),
                                     mul16_u8(_mm256_extracti128_si256(hi, 1), x + 48));
            acc = _mm256_fmadd_ps(a, _mm256_set1_ps(d * s1), acc);
            acc = _mm256_fmadd_ps(c, _mm256_set1_ps(d * s2), acc);
            corr += dmin * (m1 * sx[0] + m2 * sx[1]);
        }
    }
    return hsum256(acc) - corr;
#else
    float tmp[256], total = 0.0f;
    (void)sx;
    for (int b = 0; b < cols / 256; b++, row += 144, x += 256) {
        dequant(T_Q4_K, row, tmp, 256);
        total += dot(tmp, x, 256);
    }
    return total;
#endif
}

/* linha Q6_K vezes vetor float: pesos de 6 bits (-32..31), escala int8 por sub-bloco de 16 */
static float dot_q6k(const uint8_t *row, const float *x, int cols) {
#ifdef __AVX2__
    const __m256i m4 = _mm256_set1_epi8(0x0F), m2 = _mm256_set1_epi8(3), off = _mm256_set1_epi8(32);
    __m256 acc = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 210) {
        const uint8_t *ql = row, *qh = row + 128;
        const int8_t *sc = (const int8_t *)(row + 192);
        uint16_t h; memcpy(&h, row + 208, 2);
        const float d = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h)));
        for (int c = 0; c < 2; c++, ql += 64, qh += 32, sc += 8, x += 128) {
            const __m256i l0 = _mm256_loadu_si256((const __m256i *)ql);
            const __m256i l1 = _mm256_loadu_si256((const __m256i *)(ql + 32));
            const __m256i hv = _mm256_loadu_si256((const __m256i *)qh);
            __m256i q[4];
            q[0] = _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(hv, m2), 4));
            q[1] = _mm256_or_si256(_mm256_and_si256(l1, m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 2), m2), 4));
            q[2] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 4), m2), 4));
            q[3] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 6), m2), 4));
            for (int k = 0; k < 4; k++) {
                const __m256i v = _mm256_sub_epi8(q[k], off);
                acc = _mm256_fmadd_ps(mul16_i8(_mm256_castsi256_si128(v), x + 32 * k),
                                      _mm256_set1_ps(d * sc[2 * k]), acc);
                acc = _mm256_fmadd_ps(mul16_i8(_mm256_extracti128_si256(v, 1), x + 32 * k + 16),
                                      _mm256_set1_ps(d * sc[2 * k + 1]), acc);
            }
        }
    }
    return hsum256(acc);
#else
    float tmp[256], total = 0.0f;
    for (int b = 0; b < cols / 256; b++, row += 210, x += 256) {
        dequant(T_Q6_K, row, tmp, 256);
        total += dot(tmp, x, 256);
    }
    return total;
#endif
}

/* ---------- produtos inteiros na CPU ----------
 * As ativacoes sao quantizadas em 16 bits por bloco de 256 colunas (x ~ q * xs[bloco]).
 * Os pesos (4 ou 6 bits) sao alargados para 16 bits e um madd (s16 x s16 -> s32) faz 16
 * produtos de uma vez, sem converter peso nenhum para float; so a soma de cada sub-bloco
 * vira float para receber a escala. Com 16 bits o erro de quantizacao das ativacoes fica
 * na casa de 1e-5 (com 8 bits, medido, chegava a trocar o token escolhido).
 * cpu_exact=1 volta aos produtos em float (referencia exata, usada pelo selftest). */
typedef struct { int16_t *q; int32_t *bs32; float *xs; int cap; } XQ;     /* bs32: soma de cada 32 valores */
/* a entrada quantizada em uso: a global, ou outra escolhida por thread (xq_tls) quando varios
   produtos com entradas diferentes rodam no mesmo despacho do pool (especialistas do MoE) */
static XQ xq0;
#ifdef _MSC_VER
static __declspec(thread) XQ *xq_tls;
#else
static __thread XQ *xq_tls;
#endif
#define xq (*(xq_tls ? xq_tls : &xq0))
static int cpu_exact;

#ifdef __AVX2__
#define HAVE_INT_DOT 1
static void xq_prepare(const float *x, int cols) {
    if (cols > xq.cap) {
        free(xq.q); free(xq.bs32); free(xq.xs);
        xq.q = xalloc((size_t)cols + 64, sizeof(int16_t));
        xq.bs32 = xalloc((size_t)cols / 32 + 16, sizeof(int32_t));
        xq.xs = xalloc((size_t)cols / 256 + 1, sizeof(float));
        xq.cap = cols;
    }
    const __m256 absm = _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
    for (int b = 0; b < cols / 256; b++) {
        const float *xb = x + b * 256;
        int16_t *q = xq.q + b * 256;
        __m256 mx = _mm256_setzero_ps();
        for (int j = 0; j < 256; j += 8) mx = _mm256_max_ps(mx, _mm256_and_ps(_mm256_loadu_ps(xb + j), absm));
        float t[8], amax = 0.0f;
        _mm256_storeu_ps(t, mx);
        for (int j = 0; j < 8; j++) if (t[j] > amax) amax = t[j];
        if (amax == 0.0f) {
            memset(q, 0, 256 * sizeof(int16_t)); memset(xq.bs32 + b * 8, 0, 8 * sizeof(int32_t)); xq.xs[b] = 0.0f;
            continue;
        }
        const __m256 is = _mm256_set1_ps(32767.0f / amax);
        for (int sub = 0; sub < 8; sub++) {
            __m256i sum = _mm256_setzero_si256();
            for (int j = 0; j < 32; j += 16) {
                const __m256i a = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(xb + sub * 32 + j), is));
                const __m256i c = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(xb + sub * 32 + j + 8), is));
                sum = _mm256_add_epi32(sum, _mm256_add_epi32(a, c));
                _mm256_storeu_si256((__m256i *)(q + sub * 32 + j), _mm256_permute4x64_epi64(_mm256_packs_epi32(a, c), 0xD8));
            }
            __m128i s4 = _mm_add_epi32(_mm256_castsi256_si128(sum), _mm256_extracti128_si256(sum, 1));
            s4 = _mm_add_epi32(s4, _mm_srli_si128(s4, 8));
            s4 = _mm_add_epi32(s4, _mm_srli_si128(s4, 4));
            xq.bs32[b * 8 + sub] = _mm_cvtsi128_si32(s4);
        }
        xq.xs[b] = amax / 32767.0f;
    }
}

/* soma de 32 pesos u8 (em v) vezes 32 ativacoes s16: devolve 8 parciais s32 */
static __m256i mul32_u8_s16(__m256i v, const int16_t *x) {
    const __m256i a = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(v));
    const __m256i c = _mm256_cvtepu8_epi16(_mm256_extracti128_si256(v, 1));
    return _mm256_add_epi32(_mm256_madd_epi16(a, _mm256_loadu_si256((const __m256i *)x)),
                            _mm256_madd_epi16(c, _mm256_loadu_si256((const __m256i *)(x + 16))));
}

static float hsum128(__m128 v) {
    v = _mm_hadd_ps(v, v); v = _mm_hadd_ps(v, v);
    return _mm_cvtss_f32(v);
}

/* Q4_K inteiro. Por bloco de 256 pesos: as 8 escalas e os 8 minimos de 6 bits saem de 12
   bytes com meia duzia de operacoes de 32 bits (sem desvios). Cada peso vale
   d*sc*q - dmin*m: o termo dos minimos usa as somas de x por sub-bloco (bs32). */
static float dot_q4k_i(const uint8_t *row, int cols) {
    const __m256i m4 = _mm256_set1_epi8(0x0F);
    const uint32_t k1 = 0x3f3f3f3fu, k2 = 0x0f0f0f0fu, k3 = 0x03030303u;
    const int16_t *xp = xq.q;
    const int32_t *bs = xq.bs32;
    __m256 acc = _mm256_setzero_ps(), accm = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 144, xp += 256, bs += 8) {
        uint16_t h0, h1; uint32_t u[4];
        memcpy(&h0, row, 2); memcpy(&h1, row + 2, 2); memcpy(u, row + 4, 12);
        u[3] = ((u[2] >> 4) & k2) | (((u[1] >> 6) & k3) << 4);     /* minimos 4..7 */
        const uint32_t aux = u[1] & k1;                              /* minimos 0..3 */
        u[1] = (u[2] & k2) | (((u[0] >> 6) & k3) << 4);             /* escalas 4..7 */
        u[2] = aux;
        u[0] &= k1;                                                  /* escalas 0..3 */
        const float xs = xq.xs[b];
        const float d = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h0))) * xs;
        const float dmin = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h1))) * xs;
        const __m256i sc32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)u));
        const __m256i mn32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(u + 2)));
        const __m256 scf = _mm256_mul_ps(_mm256_cvtepi32_ps(sc32), _mm256_set1_ps(d));   /* d*xs*escala[j] */
        accm = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_mullo_epi32(mn32, _mm256_loadu_si256((const __m256i *)bs))),
                               _mm256_set1_ps(dmin), accm);
        const uint8_t *q = row + 16;
        for (int j = 0; j < 4; j++) {
            const __m256i v = _mm256_loadu_si256((const __m256i *)(q + 32 * j));
            const __m256i lo = _mm256_and_si256(v, m4), hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), m4);
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(mul32_u8_s16(lo, xp + 64 * j)),
                                  _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * j)), acc);
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(mul32_u8_s16(hi, xp + 64 * j + 32)),
                                  _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * j + 1)), acc);
        }
    }
    return hsum256(acc) - hsum256(accm);
}

/* Q6_K inteiro: pesos de 6 bits (-32..31), uma escala int8 para cada 16 pesos */
static float dot_q6k_i(const uint8_t *row, int cols) {
    const __m256i m4 = _mm256_set1_epi8(0x0F), m2 = _mm256_set1_epi8(3), off = _mm256_set1_epi16(32);
    const int16_t *xp = xq.q;
    __m256 acc = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 210, xp += 256) {
        const uint8_t *ql = row, *qh = row + 128;
        uint16_t h; memcpy(&h, row + 208, 2);
        const __m256 dv = _mm256_set1_ps(_mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))) * xq.xs[b]);
        const __m128i s8 = _mm_loadu_si128((const __m128i *)(row + 192));
        for (int c = 0; c < 2; c++, ql += 64, qh += 32) {
            /* 8 escalas deste meio bloco, ja multiplicadas por d*xs */
            const __m256 scf = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(c ? _mm_srli_si128(s8, 8) : s8)), dv);
            const __m256i l0 = _mm256_loadu_si256((const __m256i *)ql);
            const __m256i l1 = _mm256_loadu_si256((const __m256i *)(ql + 32));
            const __m256i hv = _mm256_loadu_si256((const __m256i *)qh);
            __m256i q[4];
            q[0] = _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(hv, m2), 4));
            q[1] = _mm256_or_si256(_mm256_and_si256(l1, m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 2), m2), 4));
            q[2] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 4), m2), 4));
            q[3] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4), _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 6), m2), 4));
            for (int k = 0; k < 4; k++) {
                const int16_t *x = xp + c * 128 + k * 32;
                const __m256i a = _mm256_sub_epi16(_mm256_cvtepu8_epi16(_mm256_castsi256_si128(q[k])), off);
                const __m256i e = _mm256_sub_epi16(_mm256_cvtepu8_epi16(_mm256_extracti128_si256(q[k], 1)), off);
                acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_madd_epi16(a, _mm256_loadu_si256((const __m256i *)x))),
                                      _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * k)), acc);
                acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_madd_epi16(e, _mm256_loadu_si256((const __m256i *)(x + 16)))),
                                      _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * k + 1)), acc);
            }
        }
    }
    return hsum256(acc);
}

/* Q5_K inteiro: como o Q4_K, com o quinto bit de cada peso vindo de uma mascara de 32 bytes */
static float dot_q5k_i(const uint8_t *row, int cols) {
    const __m256i m4 = _mm256_set1_epi8(0x0F), one = _mm256_set1_epi8(1);
    const uint32_t k1 = 0x3f3f3f3fu, k2 = 0x0f0f0f0fu, k3 = 0x03030303u;
    const int16_t *xp = xq.q;
    const int32_t *bs = xq.bs32;
    __m256 acc = _mm256_setzero_ps(), accm = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 176, xp += 256, bs += 8) {
        uint16_t h0, h1; uint32_t u[4];
        memcpy(&h0, row, 2); memcpy(&h1, row + 2, 2); memcpy(u, row + 4, 12);
        u[3] = ((u[2] >> 4) & k2) | (((u[1] >> 6) & k3) << 4);
        const uint32_t aux = u[1] & k1;
        u[1] = (u[2] & k2) | (((u[0] >> 6) & k3) << 4);
        u[2] = aux;
        u[0] &= k1;
        const float xs = xq.xs[b];
        const float d = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h0))) * xs;
        const float dmin = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h1))) * xs;
        const __m256i sc32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)u));
        const __m256i mn32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(u + 2)));
        const __m256 scf = _mm256_mul_ps(_mm256_cvtepi32_ps(sc32), _mm256_set1_ps(d));
        accm = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_mullo_epi32(mn32, _mm256_loadu_si256((const __m256i *)bs))),
                               _mm256_set1_ps(dmin), accm);
        const __m256i hv = _mm256_loadu_si256((const __m256i *)(row + 16));
        const uint8_t *q = row + 48;
        for (int j = 0; j < 4; j++) {
            const __m256i v = _mm256_loadu_si256((const __m256i *)(q + 32 * j));
            const __m256i lo = _mm256_or_si256(_mm256_and_si256(v, m4),
                               _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 2 * j), one), 4));
            const __m256i hi = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(v, 4), m4),
                               _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hv, 2 * j + 1), one), 4));
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(mul32_u8_s16(lo, xp + 64 * j)),
                                  _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * j)), acc);
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(mul32_u8_s16(hi, xp + 64 * j + 32)),
                                  _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * j + 1)), acc);
        }
    }
    return hsum256(acc) - hsum256(accm);
}

/* Q3_K inteiro: pesos de 3 bits (-4..3) = 2 bits + 1 bit alto numa mascara; 16 escalas de 6 bits */
static float dot_q3k_i(const uint8_t *row, int cols) {
    const __m256i m3 = _mm256_set1_epi8(3), one = _mm256_set1_epi8(1), four = _mm256_set1_epi16(4);
    const uint32_t km1 = 0x03030303u, km2 = 0x0f0f0f0fu;
    const int16_t *xp = xq.q;
    __m256 acc = _mm256_setzero_ps();
    for (int b = 0; b < cols / 256; b++, row += 110, xp += 256) {
        uint32_t aux[4]; uint16_t h;
        memcpy(aux, row + 96, 12); memcpy(&h, row + 108, 2);
        const uint32_t tmp = aux[2];
        aux[2] = ((aux[0] >> 4) & km2) | (((tmp >> 4) & km1) << 4);
        aux[3] = ((aux[1] >> 4) & km2) | (((tmp >> 6) & km1) << 4);
        aux[0] = (aux[0] & km2) | (((tmp >> 0) & km1) << 4);
        aux[1] = (aux[1] & km2) | (((tmp >> 2) & km1) << 4);
        const __m128i s8 = _mm_sub_epi8(_mm_loadu_si128((const __m128i *)aux), _mm_set1_epi8(32));
        const __m256 dv = _mm256_set1_ps(_mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))) * xq.xs[b]);
        const __m256i hm = _mm256_loadu_si256((const __m256i *)row);
        for (int j = 0; j < 2; j++) {
            const __m256 scf = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(j ? _mm_srli_si128(s8, 8) : s8)), dv);
            const __m256i qv = _mm256_loadu_si256((const __m256i *)(row + 32 + 32 * j));
            for (int k = 0; k < 4; k++) {
                const int16_t *x = xp + (j * 4 + k) * 32;
                const __m256i u = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(qv, 2 * k), m3),
                                  _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hm, 4 * j + k), one), 2));
                const __m256i a = _mm256_sub_epi16(_mm256_cvtepu8_epi16(_mm256_castsi256_si128(u)), four);
                const __m256i e = _mm256_sub_epi16(_mm256_cvtepu8_epi16(_mm256_extracti128_si256(u, 1)), four);
                acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_madd_epi16(a, _mm256_loadu_si256((const __m256i *)x))),
                                      _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * k)), acc);
                acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_madd_epi16(e, _mm256_loadu_si256((const __m256i *)(x + 16)))),
                                      _mm256_permutevar8x32_ps(scf, _mm256_set1_epi32(2 * k + 1)), acc);
            }
        }
    }
    return hsum256(acc);
}
#else
#define HAVE_INT_DOT 0
static float dot_q5k_i(const uint8_t *row, int cols) { (void)row; (void)cols; return 0.0f; }
static float dot_q3k_i(const uint8_t *row, int cols) { (void)row; (void)cols; return 0.0f; }
static void xq_prepare(const float *x, int cols) { (void)x; (void)cols; }
static float dot_q4k_i(const uint8_t *row, int cols) { (void)row; (void)cols; return 0.0f; }
static float dot_q6k_i(const uint8_t *row, int cols) { (void)row; (void)cols; return 0.0f; }
#endif

/* a matriz usa o produto inteiro? (so tipos K, e se nao estiver no modo exato) */
static int mat_int(const Mat *W) {
    return HAVE_INT_DOT && !cpu_exact && W->q &&
           (W->type == T_Q4_K || W->type == T_Q6_K || W->type == T_Q5_K || W->type == T_Q3_K);
}

static float dot_row(const Mat *W, size_t i, const float *x, const float *sx, int cols) {
    if (W->f) return dot(W->f + i * cols, x, cols);
    const uint8_t *row = W->q + i * W->stride;
    if (mat_int(W)) return W->type == T_Q4_K ? dot_q4k_i(row, cols) : W->type == T_Q6_K ? dot_q6k_i(row, cols)
                         : W->type == T_Q5_K ? dot_q5k_i(row, cols) : dot_q3k_i(row, cols);
    if (W->type == T_Q4_K) return dot_q4k(row, x, sx, cols);
    if (W->type == T_Q6_K) return dot_q6k(row, x, cols);
    if (W->type == T_Q8_0) return dot_q8(row, x, cols);
    {   /* demais tipos (Q4_0, Q3_K, Q5_K): dequantiza cada bloco e multiplica em float */
        const int unit = W->type == T_Q4_0 ? 32 : 256;
        const size_t ub = qbytes(W->type, unit);
        float tmp[256], total = 0.0f;
        for (int b = 0; b < cols / unit; b++) {
            dequant(W->type, row + b * ub, tmp, unit);
            total += dot(tmp, x + b * unit, unit);
        }
        return total;
    }
}

/* somas de x por sub-bloco de 32 (usadas pelo Q4_K) */
static void block_sums(float *sx, const float *x, int cols) {
    for (int b = 0; b < cols / 32; b++) {
        float s = 0.0f;
        for (int j = 0; j < 32; j++) s += x[b * 32 + j];
        sx[b] = s;
    }
}

#define MAX_SX 2048   /* suporta ate 65536 colunas */

/* ================= GPU: OpenCL carregado em tempo de execucao =================
 * Sem SDK: as funcoes vem do OpenCL.dll / libOpenCL.so do driver e os kernels sao
 * compilados pelo proprio driver. Os pesos sobem quantizados, como estao no arquivo,
 * e sao decodificados dentro do kernel.
 *
 * Dois modos:
 *   grafo completo - tudo cabe na VRAM: o token inteiro roda na GPU (pesos, cache KV,
 *                    atencao, normas); por token ha 1 escrita (embedding) e 1 leitura (logits).
 *   hibrido        - so parte cabe: as matrizes que couberam fazem o produto na GPU.
 * KATANA_CPU=1 desliga a GPU.
 */

static double now_sec(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

typedef void *clh;

static struct {
    int32_t (*GetPlatformIDs)(uint32_t, clh *, uint32_t *);
    int32_t (*GetDeviceIDs)(clh, uint64_t, uint32_t, clh *, uint32_t *);
    int32_t (*GetDeviceInfo)(clh, uint32_t, size_t, void *, size_t *);
    clh     (*CreateContext)(const intptr_t *, uint32_t, const clh *, void *, void *, int32_t *);
    clh     (*CreateCommandQueue)(clh, clh, uint64_t, int32_t *);
    clh     (*CreateProgramWithSource)(clh, uint32_t, const char **, const size_t *, int32_t *);
    int32_t (*BuildProgram)(clh, uint32_t, const clh *, const char *, void *, void *);
    int32_t (*GetProgramBuildInfo)(clh, clh, uint32_t, size_t, void *, size_t *);
    clh     (*CreateKernel)(clh, const char *, int32_t *);
    clh     (*CreateBuffer)(clh, uint64_t, size_t, void *, int32_t *);
    int32_t (*SetKernelArg)(clh, uint32_t, size_t, const void *);
    int32_t (*EnqueueNDRangeKernel)(clh, clh, uint32_t, const size_t *, const size_t *, const size_t *, uint32_t, const clh *, clh *);
    int32_t (*EnqueueReadBuffer)(clh, clh, uint32_t, size_t, size_t, void *, uint32_t, const clh *, clh *);
    int32_t (*EnqueueWriteBuffer)(clh, clh, uint32_t, size_t, size_t, const void *, uint32_t, const clh *, clh *);
    int32_t (*Finish)(clh);
    int32_t (*ReleaseMemObject)(clh);
    int32_t (*Flush)(clh);
    int32_t (*GetEventInfo)(clh, uint32_t, size_t, void *, size_t *);
    int32_t (*ReleaseEvent)(clh);
    int32_t (*EnqueueFillBuffer)(clh, clh, const void *, size_t, size_t, size_t, uint32_t, const clh *, clh *);
} cl;

#define GPU_BMAX 16     /* maior lote de tokens por passada */

/* um conjunto de kernels compilado para um tamanho de lote fixo (NB) */
typedef struct {
    int nb, nr;     /* tokens por passada; linhas da matriz por thread */
    clh k_f32, k_q8, k_q4k, k_q5k, k_q6k, k_bsum;
    clh kp_f32, kp_q8, kp_q4k, kp_q5k, kp_q6k;      /* mesmos kernels com 2*nr linhas por thread (par gate/up) */
    clh k_moe_sum, k_rmsh, k_att1f;
    clh k_rms, k_qkv, k_gather, k_att_scores, k_att_softmax, k_att_mix;
    clh k_mv3, k_att1;      /* fundidos da geracao; so existem com nb=1 e nr=1 */
} KSet;

typedef struct {
    int on, full, force_cpu, debug, test_mv, kv16, n_shard;
    int want_all;                                       /* devolver os logits dos 4 tokens do lote */
    clh blog4; float *logits_all;                       /* logits transpostos [vocab][4] */
    int cfg_rows, cfg_rowsb;                            /* linhas por thread: geracao e lote */
    size_t cfg_seg;                                     /* alvo de threads para segmentar colunas */
    clh dev;
    int n_gl;                                           /* camadas inteiras na GPU (as primeiras) */
    clh ctx, queue;
    KSet set[4]; int n_sets;                            /* nb decrescente; o ultimo e nb=1 */
    clh bpart, bsx;                                     /* parciais dos segmentos; somas de x */
    size_t seg_target, local;                           /* ajuste: KATANA_SEG, KATANA_LOCAL */
    clh xbuf, obuf;                                     /* modo hibrido */
    int vk, vk_nbuf;                                    /* backend Vulkan ativo; buffers de pesos criados */
    KvkBuf vxb, vob;                                    /* Vulkan: x e saida do modo hibrido */
    clh bx, bxb, bq, bk, bv, bhb, blog, batt, bfreq, bkc, bvc, bon;   /* grafo completo */
    clh *an, *fn, *lbq, *lbk, *lbv, *lbo;               /* normas e bias por camada */
    float *xT;                                          /* embeddings do lote, transpostos */
    char name[128];
    uint64_t vram, used;
    int n_up, n_total;
} Gpu;

static Gpu gpu;

/* fracao da VRAM que o motor pode ocupar (o resto fica para o driver e o sistema) */
static uint64_t gpu_budget(void) {
    /* 92%: medido com um 7B numa placa de 4 GB. A 95% cabia uma camada a mais, mas a geracao
       caia de 13,4 para 9,8 tokens/s (a placa cheia demais passa a usar memoria compartilhada). */
    int pct = getenv("KATANA_VRAM_PCT") ? atoi(getenv("KATANA_VRAM_PCT")) : 92;
    if (pct < 10 || pct > 99) pct = 92;
    return gpu.vram / 100 * (uint64_t)pct;
}
static int32_t gerr;
static struct { clh k; const char *name; double t; long calls; double elems; } prof[64];
static int n_prof;
static double prof_elems;   /* elementos de peso do produto que esta sendo enfileirado */

static const char *GPU_SRC =
/* O mesmo fonte e compilado uma vez por tamanho de lote (-DNB=1, 4, 16).
   Ativacoes ficam transpostas: X[coluna*NB + token]. Assim os NB valores de uma coluna
   sao um vetor so (FB) e cada peso decodificado faz UMA multiplicacao vetorial pelo lote:
   o peso e lido e decodificado uma vez para NB tokens. */
"#if NB == 1\n"
"#define FB float\n#define LOADB(p, k) ((p)[k])\n#define STOREB(v, p, k) (p)[k] = (v)\n"
"#elif NB == 4\n"
"#define FB float4\n#define LOADB(p, k) vload4(k, p)\n#define STOREB(v, p, k) vstore4(v, k, p)\n"
"#elif NB == 8\n"
"#define FB float8\n#define LOADB(p, k) vload8(k, p)\n#define STOREB(v, p, k) vstore8(v, k, p)\n"
"#else\n"
"#define FB float16\n#define LOADB(p, k) vload16(k, p)\n#define STOREB(v, p, k) vstore16(v, k, p)\n"
"#endif\n"
/* Pesos (layout nosso, nao o do arquivo): cada linha vira `wpr` palavras de 32 bits e as
   linhas sao entrelacadas de 32 em 32: palavra w da linha i em W[(i/32)*wpr*32 + w*32 + i%32].
   As 32 threads de um warp leem 32 palavras consecutivas a cada passo (acesso coalescido).
   Cada thread calcula NR linhas de uma vez (de NR grupos de 32 vizinhos): os valores de x
   sao carregados uma vez e reusados nas NR linhas (ladrilhamento em registradores).
   Um buffer pode conter varias matrizes intercaladas por grupo (gs = quantas, go = qual). */
"#if NB * NR > 32\n#define MAXSEG 4\n#else\n#define MAXSEG 8\n#endif\n"
/* cache KV em float32 ou, para economizar VRAM, em float16 */
"#if KV16\n#define KVT half\n#define KVL(p, i) vload_half(i, p)\n#define KVS(v, i, p) vstore_half(v, i, p)\n"
"#else\n#define KVT float\n#define KVL(p, i) ((p)[i])\n#define KVS(v, i, p) (p)[i] = (v)\n#endif\n"
"#define GIDX(r) (((i >> 5) * NR + (r)) * gs + go)\n"
"#define RPTR(r, wpr) (W + (size_t)GIDX(r) * (size_t)(wpr) * 32 + (i & 31))\n"
"#define RROW(r) ((((i >> 5) * NR + (r)) << 5) + (i & 31))\n"
/* fecho: bias opcional (bit 4) e modo: 0 grava, 1 soma (residual), 2 SwiGLU com out */
"#define FIN(t, row) { FB t_ = (t); if (mode & 4) t_ += bias[row]; int mm = mode & 3;\\\n"
"  if (mm == 1) t_ += LOADB(out, row);\\\n"
"  else if (mm == 2) { FB g_ = LOADB(out, row); t_ = g_ / (1.0f + exp(-g_)) * t_; }\\\n"
"  STOREB(t_, out, row); }\n"
/* modo 8 (par): as linhas pares do buffer sao o gate e as impares o up da mesma linha;
   a thread tem os dois produtos e ja grava silu(gate)*up */
"#if NR >= 2\n"
"#define PAIRS if (mode & 8) { for (int r = 0; r < NR; r += 2) {\\\n"
"    int row = ((((i >> 5) * NR + r) >> 1) << 5) + (i & 31);\\\n"
"    if (row < rows) { FB g_ = total[r]; STOREB(g_ / (1.0f + exp(-g_)) * total[r + 1], out, row); } }\\\n"
"    return; }\n"
"#else\n"
"#define PAIRS\n"
"#endif\n"
/* com segmentos de colunas (dimensao 1 do NDRange) as threads do mesmo grupo de trabalho
   (32 linhas x nseg segmentos) somam os parciais pela memoria local: um kernel so */
"#define FINISH \\\n"
"  if (nseg > 1) { int sg = get_global_id(1), lane = i & 31;\\\n"
"    for (int r = 0; r < NR; r++) lp[(r * MAXSEG + sg) * 32 + lane] = total[r];\\\n"
"    barrier(CLK_LOCAL_MEM_FENCE);\\\n"
"    if (sg != 0) return;\\\n"
"    for (int r = 0; r < NR; r++) { FB t = (FB)(0.0f);\\\n"
"      for (int q_ = 0; q_ < nseg; q_++) t += lp[(r * MAXSEG + q_) * 32 + lane];\\\n"
"      total[r] = t; } }\\\n"
"  PAIRS\\\n"
"  for (int r = 0; r < NR; r++) { int row = RROW(r); if (row < rows) FIN(total[r], row) }\n"
"#define MVARGS __global const uint *W, __global const float *x, __global float *out,\\\n"
"  __global const float *bias, __global const float *sx,\\\n"
"  int cols, int mode, int rows, int nseg, int gs, int go,\\\n"
"  __global const uint *mtab, int mtp, int mxrow, int morow, int mcol\n"
/* varias matrizes num lancamento (especialistas do MoE): as threads sao divididas em faixas de
   mtp; a faixa k_ tem 3 entradas em mtab: (inicio da matriz em W, em blocos de 32 palavras;
   indice do vetor de entrada; indice do vetor de saida). Antes: le x a partir
   de k_*mxrow e grava a partir de k_*morow. mtp = 0 desliga. */
"#define MOEPRE if (mtp > 0) { int k_ = i / mtp; i -= k_ * mtp;\\\n"
"  W += (size_t)mtab[6 * k_ + mcol] * 32; x += (size_t)mtab[6 * k_ + mcol + 1] * mxrow; out += (size_t)mtab[6 * k_ + mcol + 2] * morow; }\n"
"#define MVLOCAL __local FB lp[NR * MAXSEG * 32];\n"
"inline float h2f(uint h) {\n"
"  uint e = (h >> 10) & 31, m = h & 1023;\n"
"  float v = e == 0 ? ldexp((float)m, -24) : ldexp((float)(m | 1024), (int)e - 25);\n"
"  return (h & 0x8000) ? -v : v;\n"
"}\n"
"inline uint sb(const uint *hw, int k) { return (hw[k >> 2] >> ((k & 3) * 8)) & 255; }\n"
"inline void k4(int j, const uint *hw, int *d, int *m) {\n"
"  if (j < 4) { *d = sb(hw, j) & 63; *m = sb(hw, j + 4) & 63; }\n"
"  else { *d = (sb(hw, j + 4) & 0xF) | ((sb(hw, j - 4) >> 6) << 4); *m = (sb(hw, j + 4) >> 4) | ((sb(hw, j) >> 6) << 4); }\n"
"}\n"
"__kernel void mv_f32(MVARGS) {\n"
"  MVLOCAL\n"
"  int i = get_global_id(0);\n"
"  MOEPRE\n"
"  __global const uint *p[NR]; FB total[NR];\n"
"  for (int r = 0; r < NR; r++) { p[r] = RPTR(r, cols); total[r] = (FB)(0.0f); }\n"
"  for (int j = 0; j < cols; j++) {\n"
"    FB xv = LOADB(x, j);\n"
"    for (int r = 0; r < NR; r++) total[r] += as_float(p[r][(size_t)j * 32]) * xv;\n"
"  }\n"
"  FINISH\n"
"}\n"
/* Q8: 9 palavras por bloco de 32 pesos: escala float + 32 int8 */
"__kernel void mv_q8(MVARGS) {\n"
"  MVLOCAL\n"
"  int i = get_global_id(0);\n"
"  MOEPRE\n"
"  int nb = cols >> 5, bps = nb / nseg, b0 = get_global_id(1) * bps;\n"
"  __global const uint *p[NR]; FB total[NR];\n"
"  for (int r = 0; r < NR; r++) { p[r] = RPTR(r, nb * 9) + (size_t)b0 * (9 * 32); total[r] = (FB)(0.0f); }\n"
"  __global const float *xb = x + (size_t)b0 * 32 * NB;\n"
"  for (int b = 0; b < bps; b++, xb += 32 * NB) {\n"
"    FB a[NR];\n"
"    for (int r = 0; r < NR; r++) a[r] = (FB)(0.0f);\n"
"    for (int l = 0; l < 8; l++) {\n"
"      FB x0 = LOADB(xb, 4 * l), x1 = LOADB(xb, 4 * l + 1), x2 = LOADB(xb, 4 * l + 2), x3 = LOADB(xb, 4 * l + 3);\n"
"      for (int r = 0; r < NR; r++) {\n"
"        int w = (int)p[r][(1 + l) * 32];\n"
"        a[r] += (float)((w << 24) >> 24) * x0 + (float)((w << 16) >> 24) * x1\n"
"              + (float)((w << 8) >> 24) * x2 + (float)(w >> 24) * x3;\n"
"      }\n"
"    }\n"
"    for (int r = 0; r < NR; r++) { total[r] += as_float(p[r][0]) * a[r]; p[r] += 9 * 32; }\n"
"  }\n"
"  FINISH\n"
"}\n"
/* Q4_K: 36 palavras por bloco de 256 pesos (mesmos bytes do arquivo). Cada peso vale
   d*sc*q - dmin*m; o termo do minimo usa a soma de x por sub-bloco, igual para todas as
   linhas: calculada uma vez por thread quando NB=1, ou vinda do kernel bsum (sx) em lote. */
"__kernel void mv_q4k(MVARGS) {\n"
"  MVLOCAL\n"
"  int i = get_global_id(0);\n"
"  MOEPRE\n"
"  int nb = cols >> 8, bps = nb / nseg, b0 = get_global_id(1) * bps;\n"
"  __global const uint *p[NR]; FB total[NR];\n"
"  for (int r = 0; r < NR; r++) { p[r] = RPTR(r, nb * 36) + (size_t)b0 * (36 * 32); total[r] = (FB)(0.0f); }\n"
"  __global const float *xb = x + (size_t)b0 * 256 * NB;\n"
"  for (int b = 0; b < bps; b++) {\n"
"    float d[NR], dmin[NR]; uint hw[NR][3];\n"
"    for (int r = 0; r < NR; r++) {\n"
"      uint h0 = p[r][0];\n"
"      hw[r][0] = p[r][32]; hw[r][1] = p[r][64]; hw[r][2] = p[r][96];\n"
"      d[r] = h2f(h0 & 0xFFFF); dmin[r] = h2f(h0 >> 16);\n"
"    }\n"
"    for (int j = 0; j < 8; j += 2, xb += 64 * NB) {\n"
"      FB a[NR], c[NR];\n"
"      for (int r = 0; r < NR; r++) { a[r] = (FB)(0.0f); c[r] = (FB)(0.0f); }\n"
"#if NB == 1\n"
"      FB xa = 0.0f, xc = 0.0f;\n"
"#else\n"
"      FB xa = LOADB(sx, (b0 + b) * 8 + j), xc = LOADB(sx, (b0 + b) * 8 + j + 1);\n"
"#endif\n"
"      for (int l = 0; l < 8; l++) {\n"
"        FB x0 = LOADB(xb, 4 * l), x1 = LOADB(xb, 4 * l + 1), x2 = LOADB(xb, 4 * l + 2), x3 = LOADB(xb, 4 * l + 3);\n"
"        FB y0 = LOADB(xb, 32 + 4 * l), y1 = LOADB(xb, 33 + 4 * l), y2 = LOADB(xb, 34 + 4 * l), y3 = LOADB(xb, 35 + 4 * l);\n"
"#if NB == 1\n"
"        xa += x0 + x1 + x2 + x3; xc += y0 + y1 + y2 + y3;\n"
"#endif\n"
"        for (int r = 0; r < NR; r++) {\n"
"          uint w = p[r][(4 + j * 4 + l) * 32];\n"
"          a[r] += (float)(w & 15) * x0 + (float)((w >> 8) & 15) * x1 + (float)((w >> 16) & 15) * x2 + (float)((w >> 24) & 15) * x3;\n"
"          c[r] += (float)((w >> 4) & 15) * y0 + (float)((w >> 12) & 15) * y1 + (float)((w >> 20) & 15) * y2 + (float)(w >> 28) * y3;\n"
"        }\n"
"      }\n"
"      for (int r = 0; r < NR; r++) {\n"
"        int s1, m1, s2, m2;\n"
"        k4(j, hw[r], &s1, &m1); k4(j + 1, hw[r], &s2, &m2);\n"
"        total[r] += d[r] * ((float)s1 * a[r] + (float)s2 * c[r]) - dmin[r] * ((float)m1 * xa + (float)m2 * xc);\n"
"      }\n"
"    }\n"
"    for (int r = 0; r < NR; r++) p[r] += 36 * 32;\n"
"  }\n"
"  FINISH\n"
"}\n"
/* Q5_K: 44 palavras por bloco (mesmos bytes do arquivo): cabecalho do Q4_K, 8 palavras com o
   quinto bit de cada peso e 32 palavras de nibbles */
"__kernel void mv_q5k(MVARGS) {\n"
"  MVLOCAL\n"
"  int i = get_global_id(0);\n"
"  MOEPRE\n"
"  int nb = cols >> 8, bps = nb / nseg, b0 = get_global_id(1) * bps;\n"
"  __global const uint *p[NR]; FB total[NR];\n"
"  for (int r = 0; r < NR; r++) { p[r] = RPTR(r, nb * 44) + (size_t)b0 * (44 * 32); total[r] = (FB)(0.0f); }\n"
"  __global const float *xb = x + (size_t)b0 * 256 * NB;\n"
"  for (int b = 0; b < bps; b++) {\n"
"    float d[NR], dmin[NR]; uint hw[NR][3];\n"
"    for (int r = 0; r < NR; r++) {\n"
"      uint h0 = p[r][0];\n"
"      hw[r][0] = p[r][32]; hw[r][1] = p[r][64]; hw[r][2] = p[r][96];\n"
"      d[r] = h2f(h0 & 0xFFFF); dmin[r] = h2f(h0 >> 16);\n"
"    }\n"
"    for (int j = 0; j < 8; j += 2, xb += 64 * NB) {\n"
"      FB a[NR], c[NR];\n"
"      for (int r = 0; r < NR; r++) { a[r] = (FB)(0.0f); c[r] = (FB)(0.0f); }\n"
"#if NB == 1\n"
"      FB xa = 0.0f, xc = 0.0f;\n"
"#else\n"
"      FB xa = LOADB(sx, (b0 + b) * 8 + j), xc = LOADB(sx, (b0 + b) * 8 + j + 1);\n"
"#endif\n"
"      for (int l = 0; l < 8; l++) {\n"
"        FB x0 = LOADB(xb, 4 * l), x1 = LOADB(xb, 4 * l + 1), x2 = LOADB(xb, 4 * l + 2), x3 = LOADB(xb, 4 * l + 3);\n"
"        FB y0 = LOADB(xb, 32 + 4 * l), y1 = LOADB(xb, 33 + 4 * l), y2 = LOADB(xb, 34 + 4 * l), y3 = LOADB(xb, 35 + 4 * l);\n"
"#if NB == 1\n"
"        xa += x0 + x1 + x2 + x3; xc += y0 + y1 + y2 + y3;\n"
"#endif\n"
"        for (int r = 0; r < NR; r++) {\n"
"          uint w = p[r][(12 + j * 4 + l) * 32], hb = p[r][(4 + l) * 32] >> j;\n"
"          a[r] += (float)((w & 15) | ((hb & 1) << 4)) * x0 + (float)(((w >> 8) & 15) | (((hb >> 8) & 1) << 4)) * x1 + (float)(((w >> 16) & 15) | (((hb >> 16) & 1) << 4)) * x2 + (float)(((w >> 24) & 15) | (((hb >> 24) & 1) << 4)) * x3;\n"
"          c[r] += (float)(((w >> 4) & 15) | (((hb >> 1) & 1) << 4)) * y0 + (float)(((w >> 12) & 15) | (((hb >> 9) & 1) << 4)) * y1 + (float)(((w >> 20) & 15) | (((hb >> 17) & 1) << 4)) * y2 + (float)((w >> 28) | (((hb >> 25) & 1) << 4)) * y3;\n"
"        }\n"
"      }\n"
"      for (int r = 0; r < NR; r++) {\n"
"        int s1, m1, s2, m2;\n"
"        k4(j, hw[r], &s1, &m1); k4(j + 1, hw[r], &s2, &m2);\n"
"        total[r] += d[r] * ((float)s1 * a[r] + (float)s2 * c[r]) - dmin[r] * ((float)m1 * xa + (float)m2 * xc);\n"
"      }\n"
"    }\n"
"    for (int r = 0; r < NR; r++) p[r] += 44 * 32;\n"
"  }\n"
"  FINISH\n"
"}\n"
/* Q6 reempacotado: 53 palavras por bloco de 256 pesos:
   48 palavras de pesos (16 pesos de 6 bits a cada 3 palavras), 4 de escalas int8, 1 float d */
"#define QT(v, k) ((float)((int)((v) & 63) - 32) * xv[k])\n"
"__kernel void mv_q6k(MVARGS) {\n"
"  MVLOCAL\n"
"  int i = get_global_id(0);\n"
"  MOEPRE\n"
"  int nb = cols >> 8, bps = nb / nseg, b0 = get_global_id(1) * bps;\n"
"  __global const uint *p[NR]; FB total[NR];\n"
"  for (int r = 0; r < NR; r++) { p[r] = RPTR(r, nb * 53) + (size_t)b0 * (53 * 32); total[r] = (FB)(0.0f); }\n"
"  __global const float *xb = x + (size_t)b0 * 256 * NB;\n"
"  for (int b = 0; b < bps; b++) {\n"
"    FB acc[NR];\n"
"    for (int r = 0; r < NR; r++) acc[r] = (FB)(0.0f);\n"
"    for (int g = 0; g < 4; g++) {\n"
"      for (int k = 0; k < 4; k++, xb += 16 * NB) {\n"
"        FB xv[16];\n"
"        for (int u = 0; u < 16; u++) xv[u] = LOADB(xb, u);\n"
"        for (int r = 0; r < NR; r++) {\n"
"          __global const uint *q = p[r] + (g * 4 + k) * 3 * 32;\n"
"          uint w0 = q[0], w1 = q[32], w2 = q[64];\n"
"          int scw = (int)p[r][(48 + g) * 32];\n"
"          FB s = QT(w0, 0) + QT(w0 >> 6, 1) + QT(w0 >> 12, 2) + QT(w0 >> 18, 3) + QT(w0 >> 24, 4)\n"
"               + QT((w0 >> 30) | (w1 << 2), 5) + QT(w1 >> 4, 6) + QT(w1 >> 10, 7) + QT(w1 >> 16, 8)\n"
"               + QT(w1 >> 22, 9) + QT((w1 >> 28) | (w2 << 4), 10) + QT(w2 >> 2, 11) + QT(w2 >> 8, 12)\n"
"               + QT(w2 >> 14, 13) + QT(w2 >> 20, 14) + QT(w2 >> 26, 15);\n"
"          acc[r] += (float)((scw << (24 - 8 * k)) >> 24) * s;\n"
"        }\n"
"      }\n"
"    }\n"
"    for (int r = 0; r < NR; r++) { total[r] += as_float(p[r][52 * 32]) * acc[r]; p[r] += 53 * 32; }\n"
"  }\n"
"  FINISH\n"
"}\n"
/* ---- kernels fundidos da geracao (um token por vez: NB=1, NR=1) ----
   O custo fixo de lancar um kernel domina a geracao (um modelo de 135M e um de 1,1B geravam
   na mesma velocidade). Aqui q, k e v saem de um lancamento so, mesmo com tipos diferentes,
   e a atencao inteira (notas, softmax e mistura) de outro. */
/* soma dos quadrados de len valores do token b a partir de start (16 por passo com um token) */
"inline float sumsq(__global const float *p, int start, int len, int b) {\n"
"  float ss = 0.0f; int j = 0;\n"
"#if NB == 1\n"
"  float16 acc = (float16)(0.0f);\n"
"  for (; j + 16 <= len; j += 16) { float16 v = vload16(0, p + start + j); acc += v * v; }\n"
"  float8 a8 = acc.lo + acc.hi; float4 a4 = a8.lo + a8.hi; float2 a2 = a4.lo + a4.hi;\n"
"  ss = a2.x + a2.y;\n"
"#endif\n"
"  for (; j < len; j++) { float v = p[(size_t)(start + j) * NB + b]; ss += v * v; }\n"
"  return ss;\n"
"}\n"
"#if NB == 1 && NR == 1\n"
/* soma de uma linha Q4_K (five=0) ou Q5_K (five=1) sobre bps blocos */
"inline float rd_q45(__global const uint *p, __global const float *xb, int bps, int five) {\n"
"  const int wpb = five ? 44 : 36, q0 = five ? 12 : 4;\n"
"  float total = 0.0f;\n"
"  for (int b = 0; b < bps; b++, p += wpb * 32) {\n"
"    uint h0 = p[0], hw[3];\n"
"    hw[0] = p[32]; hw[1] = p[64]; hw[2] = p[96];\n"
"    float d = h2f(h0 & 0xFFFF), dmin = h2f(h0 >> 16);\n"
"    for (int j = 0; j < 8; j += 2, xb += 64) {\n"
"      int s1, m1, s2, m2;\n"
"      k4(j, hw, &s1, &m1); k4(j + 1, hw, &s2, &m2);\n"
"      float a = 0.0f, c = 0.0f, xa = 0.0f, xc = 0.0f;\n"
"      for (int l = 0; l < 8; l++) {\n"
"        uint w = p[(q0 + j * 4 + l) * 32], hb = five ? p[(4 + l) * 32] >> j : 0;\n"
"        float x0 = xb[4 * l], x1 = xb[4 * l + 1], x2 = xb[4 * l + 2], x3 = xb[4 * l + 3];\n"
"        float y0 = xb[32 + 4 * l], y1 = xb[33 + 4 * l], y2 = xb[34 + 4 * l], y3 = xb[35 + 4 * l];\n"
"        a += (float)((w & 15) | ((hb & 1) << 4)) * x0 + (float)(((w >> 8) & 15) | (((hb >> 8) & 1) << 4)) * x1\n"
"           + (float)(((w >> 16) & 15) | (((hb >> 16) & 1) << 4)) * x2 + (float)(((w >> 24) & 15) | (((hb >> 24) & 1) << 4)) * x3;\n"
"        c += (float)(((w >> 4) & 15) | (((hb >> 1) & 1) << 4)) * y0 + (float)(((w >> 12) & 15) | (((hb >> 9) & 1) << 4)) * y1\n"
"           + (float)(((w >> 20) & 15) | (((hb >> 17) & 1) << 4)) * y2 + (float)((w >> 28) | (((hb >> 25) & 1) << 4)) * y3;\n"
"        xa += x0 + x1 + x2 + x3; xc += y0 + y1 + y2 + y3;\n"
"      }\n"
"      total += d * ((float)s1 * a + (float)s2 * c) - dmin * ((float)m1 * xa + (float)m2 * xc);\n"
"    }\n"
"  }\n"
"  return total;\n"
"}\n"
"inline float rd_q6(__global const uint *p, __global const float *xb, int bps) {\n"
"  float total = 0.0f;\n"
"  for (int b = 0; b < bps; b++, p += 53 * 32) {\n"
"    float acc = 0.0f;\n"
"    for (int g = 0; g < 4; g++) {\n"
"      int scw = (int)p[(48 + g) * 32];\n"
"      for (int k = 0; k < 4; k++, xb += 16) {\n"
"        float xv[16];\n"
"        for (int u = 0; u < 16; u++) xv[u] = xb[u];\n"
"        __global const uint *q = p + (g * 4 + k) * 3 * 32;\n"
"        uint w0 = q[0], w1 = q[32], w2 = q[64];\n"
"        float s = QT(w0, 0) + QT(w0 >> 6, 1) + QT(w0 >> 12, 2) + QT(w0 >> 18, 3) + QT(w0 >> 24, 4)\n"
"                + QT((w0 >> 30) | (w1 << 2), 5) + QT(w1 >> 4, 6) + QT(w1 >> 10, 7) + QT(w1 >> 16, 8)\n"
"                + QT(w1 >> 22, 9) + QT((w1 >> 28) | (w2 << 4), 10) + QT(w2 >> 2, 11) + QT(w2 >> 8, 12)\n"
"                + QT(w2 >> 14, 13) + QT(w2 >> 20, 14) + QT(w2 >> 26, 15);\n"
"        acc += (float)((scw << (24 - 8 * k)) >> 24) * s;\n"
"      }\n"
"    }\n"
"    total += as_float(p[52 * 32]) * acc;\n"
"  }\n"
"  return total;\n"
"}\n"
/* tres matrizes de mesmas colunas (q, k, v) num lancamento: a thread descobre pela faixa de
   linhas qual matriz e, calcula o produto no tipo dela e grava no buffer de saida dela.
   kinds: 12 = Q4_K, 13 = Q5_K, 14 = 6 bits. rp0/rp1: linhas (arredondadas) de q e de k. */
"__kernel void mv3(__global const uint *W0, __global const uint *W1, __global const uint *W2,\n"
"    __global const float *x, __global float *o0, __global float *o1, __global float *o2,\n"
"    int k0, int k1, int k2, int rp0, int rp1, int r0, int r1, int r2, int cols, int nseg) {\n"
"  __local float lp[MAXSEG * 32];\n"
"  int i = get_global_id(0), sg = get_global_id(1), lane = i & 31;\n"
"  int which = i < rp0 ? 0 : i < rp0 + rp1 ? 1 : 2;\n"
"  int row = which == 0 ? i : which == 1 ? i - rp0 : i - rp0 - rp1;\n"
"  int kind = which == 0 ? k0 : which == 1 ? k1 : k2;\n"
"  int rows = which == 0 ? r0 : which == 1 ? r1 : r2;\n"
"  __global const uint *W = which == 0 ? W0 : which == 1 ? W1 : W2;\n"
"  __global float *out = which == 0 ? o0 : which == 1 ? o1 : o2;\n"
"  int nb = cols >> 8, bps = nb / nseg, b0 = sg * bps;\n"
"  int wpb = kind == 12 ? 36 : kind == 13 ? 44 : 53;\n"
"  __global const uint *p = W + (size_t)(row >> 5) * (size_t)(nb * wpb) * 32 + lane + (size_t)b0 * (wpb * 32);\n"
"  __global const float *xb = x + (size_t)b0 * 256;\n"
"  float total = kind == 14 ? rd_q6(p, xb, bps) : rd_q45(p, xb, bps, kind == 13);\n"
"  if (nseg > 1) {\n"
"    lp[sg * 32 + lane] = total;\n"
"    barrier(CLK_LOCAL_MEM_FENCE);\n"
"    if (sg != 0) return;\n"
"    total = 0.0f;\n"
"    for (int q_ = 0; q_ < nseg; q_++) total += lp[q_ * 32 + lane];\n"
"  }\n"
"  if (row < rows) out[row] = total;\n"
"}\n"
/* atencao de um token num lancamento: um grupo de trabalho por cabeca, hd threads cada.
   As threads repartem as posicoes para as notas, juntam maximo e soma pela memoria local
   (softmax) e depois cada uma mistura um elemento da saida. */
"__kernel void att1(__global const float *q, __global const KVT *kc, __global const KVT *vc,\n"
"    __global float *att, __global float *out, int base, int pos, int ctx, int hd, int kvd, int group, float scale) {\n"
"  __local float lm[256];\n"
"  int h = get_group_id(0), l = get_local_id(0), T = pos + 1;\n"
"  __global float *a = att + (size_t)h * ctx;\n"
"  __global const float *qh = q + h * hd;\n"
"  float mx = -3.0e38f;\n"
"  for (int t = l; t < T; t += hd) {\n"
"    __global const KVT *kh = kc + base + t * kvd + (h / group) * hd;\n"
"    float s = 0.0f;\n"
"    for (int j = 0; j < hd; j++) s += qh[j] * KVL(kh, j);\n"
"    s *= scale; a[t] = s; mx = max(mx, s);\n"
"  }\n"
"  lm[l] = mx;\n"
"  barrier(CLK_LOCAL_MEM_FENCE);\n"
"  float gm = lm[0];\n"
"  for (int k = 1; k < hd; k++) gm = max(gm, lm[k]);\n"
"  barrier(CLK_LOCAL_MEM_FENCE);\n"
"  float sum = 0.0f;\n"
"  for (int t = l; t < T; t += hd) { float e = exp(a[t] - gm); a[t] = e; sum += e; }\n"
"  lm[l] = sum;\n"
"  barrier(CLK_LOCAL_MEM_FENCE | CLK_GLOBAL_MEM_FENCE);\n"
"  float tot = 0.0f;\n"
"  for (int k = 0; k < hd; k++) tot += lm[k];\n"
"  __global const KVT *v = vc + base + (h / group) * hd + l;\n"
"  float s = 0.0f;\n"
"  for (int t = 0; t < T; t++) s += a[t] * KVL(v, t * kvd);\n"
"  out[h * hd + l] = s / tot;\n"
"}\n"
/* A atencao inteira de um token, da saida crua das projecoes ate a mistura: bias e norma
   opcionais de q e k, RoPE, gravacao de k e v no cache, notas, softmax e mistura. Um grupo de hd
   threads por cabeca. As threads 0..hd/2-1 giram um par de q; as hd/2..hd-1 giram um par de k
   da cabeca kv correspondente. A posicao atual usa k e v locais (o cache so e lido para t < pos),
   e so a primeira cabeca de cada grupo kv grava no cache. */
"__kernel void att1f(__global const float *qs, __global const float *ks_, __global const float *vs,\n"
"    __global const float *bq, __global const float *bk, __global const float *bv,\n"
"    __global KVT *kc, __global KVT *vc, __global float *att, __global float *out,\n"
"    __global const float *freq, __global const float *qn, __global const float *kn,\n"
"    int base, int pos, int ctx, int hd, int kvd, int group, int neox, int has_bias, int norm, int d,\n"
"    float scale, float eps) {\n"
"  __local float lm[256]; __local float lq[256]; __local float lk[256];\n"
"  int h = get_group_id(0), l = get_local_id(0), T = pos + 1, g = h / group, half_ = hd / 2;\n"
"  int off = base + pos * kvd + g * hd;\n"
"  __global float *a = att + (size_t)h * ctx;\n"
"  {\n"
"    int isk = l >= half_, i = isk ? l - half_ : l;\n"
"    int i0 = neox ? i : 2 * i, i1 = i0 + (neox ? half_ : 1);\n"
"    float ang = pos * freq[i], cs = cos(ang), sn = sin(ang);\n"
"    if (isk) {\n"
"      float x0 = ks_[g * hd + i0], x1 = ks_[g * hd + i1];\n"
"      if (has_bias & 2) { x0 += bk[g * hd + i0]; x1 += bk[g * hd + i1]; }\n"
"      if (norm) { int r0 = norm == 1 ? 0 : g * hd, len = norm == 1 ? kvd : hd;\n"
"        float inv = 1.0f / sqrt(sumsq(ks_, r0, len, 0) / (float)len + eps);\n"
"        x0 *= kn[g * hd + i0 - r0] * inv; x1 *= kn[g * hd + i1 - r0] * inv; }\n"
"      float y0 = x0 * cs - x1 * sn, y1 = x0 * sn + x1 * cs;\n"
"      lk[i0] = y0; lk[i1] = y1;\n"
"      if (h % group == 0) { KVS(y0, off + i0, kc); KVS(y1, off + i1, kc); }\n"
"    } else {\n"
"      float x0 = qs[h * hd + i0], x1 = qs[h * hd + i1];\n"
"      if (has_bias & 1) { x0 += bq[h * hd + i0]; x1 += bq[h * hd + i1]; }\n"
"      if (norm) { int r0 = norm == 1 ? 0 : h * hd, len = norm == 1 ? d : hd;\n"
"        float inv = 1.0f / sqrt(sumsq(qs, r0, len, 0) / (float)len + eps);\n"
"        x0 *= qn[h * hd + i0 - r0] * inv; x1 *= qn[h * hd + i1 - r0] * inv; }\n"
"      lq[i0] = x0 * cs - x1 * sn; lq[i1] = x0 * sn + x1 * cs;\n"
"    }\n"
"  }\n"
"  float vl = vs[g * hd + l] + ((has_bias & 4) ? bv[g * hd + l] : 0.0f);\n"
"  if (h % group == 0) KVS(vl, off + l, vc);\n"
"  barrier(CLK_LOCAL_MEM_FENCE);\n"
"  float mx = -3.0e38f;\n"
"  for (int t = l; t < T; t += hd) {\n"
"    float s = 0.0f;\n"
"    if (t == pos) { for (int j = 0; j < hd; j++) s += lq[j] * lk[j]; }\n"
"    else { __global const KVT *kh = kc + base + t * kvd + g * hd;\n"
"      for (int j = 0; j < hd; j++) s += lq[j] * KVL(kh, j); }\n"
"    s *= scale; a[t] = s; mx = max(mx, s);\n"
"  }\n"
"  lm[l] = mx;\n"
"  barrier(CLK_LOCAL_MEM_FENCE);\n"
"  float gm = lm[0];\n"
"  for (int k = 1; k < hd; k++) gm = max(gm, lm[k]);\n"
"  barrier(CLK_LOCAL_MEM_FENCE);\n"
"  float sum = 0.0f;\n"
"  for (int t = l; t < T; t += hd) { float e = exp(a[t] - gm); a[t] = e; sum += e; }\n"
"  lm[l] = sum;\n"
"  barrier(CLK_LOCAL_MEM_FENCE | CLK_GLOBAL_MEM_FENCE);\n"
"  float tot = 0.0f;\n"
"  for (int k = 0; k < hd; k++) tot += lm[k];\n"
"  __global const KVT *v = vc + base + g * hd + l;\n"
"  float s = a[pos] * vl;\n"
"  for (int t = 0; t < pos; t++) s += a[t] * KVL(v, t * kvd);\n"
"  out[h * hd + l] = s / tot;\n"
"}\n"
"#endif\n"
/* somas de x por sub-bloco de 32 colunas (termo do minimo do Q4_K em lote) */
"__kernel void bsum(__global const float *x, __global float *sx) {\n"
"  int i = get_global_id(0);\n"
"  FB s = (FB)(0.0f);\n"
"  for (int j = 0; j < 32; j++) s += LOADB(x, i * 32 + j);\n"
"  STOREB(s, sx, i);\n"
"}\n"
/* RMSNorm num kernel so: cada thread refaz a soma dos quadrados (n e pequeno e x esta no cache) */
"__kernel void rms(__global const float *x, __global const float *g, __global float *out, int n, float eps) {\n"
"  int i = get_global_id(0);\n"
"#if NB == 1\n"
"  FB ss = sumsq(x, 0, n, 0);\n"
"#else\n"
"  FB ss = (FB)(0.0f);\n"
"  for (int j = 0; j < n; j++) { FB v = LOADB(x, j); ss += v * v; }\n"
"#endif\n"
"  STOREB(g[i] * (LOADB(x, i) / sqrt(ss / (float)n + eps)), out, i);\n"
"}\n"
/* RMSNorm por cabeca (q e k do qwen3): cada bloco de hd valores e normalizado sozinho */
"__kernel void rmsh(__global const float *x, __global const float *g, __global float *out, int hd, float eps) {\n"
"  int i = get_global_id(0), h0 = (i / hd) * hd;\n"
"  FB ss = (FB)(0.0f);\n"
"  for (int j = 0; j < hd; j++) { FB v = LOADB(x, h0 + j); ss += v * v; }\n"
"  STOREB(g[i - h0] * (LOADB(x, i) / sqrt(ss / (float)hd + eps)), out, i);\n"
"}\n"
/* MoE: x += soma dos n especialistas, cada um com o seu peso (t tem um vetor por especialista) */
"__kernel void moe_sum(__global float *x, __global const float *t, __global const uint *meta, int woff, int K, int d, int B) {\n"
"  int g = get_global_id(0), i = g / B, b = g % B;\n"
"  float s_ = 0.0f;\n"
"  for (int k = 0; k < K; k++) s_ += as_float(meta[woff + b * K + k]) * t[(size_t)meta[woff + (B + b) * K + k] * d + i];\n"
"  x[(size_t)i * B + b] += s_;\n"
"}\n"
/* copia a coluna `off` de um buffer transposto para um vetor simples */
"__kernel void gather(__global const float *src, __global float *dst, int stride, int off) {\n"
"  int i = get_global_id(0);\n"
"  dst[i] = src[(size_t)i * stride + off];\n"
"}\n"
/* pos-projecao de q, k e v num kernel so: bias opcional, RoPE em q (no lugar) e em k
   (direto no cache KV) e copia de v para o cache; o token b do lote esta na posicao pos0+b */
"__kernel void qkv_post(__global float *q, __global const float *k, __global const float *v,\n"
"    __global const float *bq, __global const float *bk, __global const float *bv,\n"
"    __global KVT *kc, __global KVT *vc, __global const float *freq,\n"
"    int base, int pos0, int hd, int neox, int has_bias, int d, int kvd,\n"
"    __global const float *qs, __global const float *qn, __global const float *kn, int norm, float eps) {\n"
"  int gid = get_global_id(0), b = gid % NB, p = gid / NB, half_ = hd / 2;\n"
"  int pos = pos0 + b, off = base + pos * kvd;\n"
"  if (p >= d / 2 + kvd / 2) {\n"
"    p -= d / 2 + kvd / 2;\n"
"    KVS(v[p * NB + b] + ((has_bias & 4) ? bv[p] : 0.0f), off + p, vc);\n"
"    return;\n"
"  }\n"
"  int isk = p >= d / 2;\n"
"  if (isk) p -= d / 2;\n"
"  int h = p / half_, i = p % half_;\n"
"  int i0 = h * hd + (neox ? i : 2 * i), i1 = i0 + (neox ? half_ : 1);\n"
"  float ang = pos * freq[i], cs = cos(ang), sn = sin(ang);\n"
"  if (isk) {\n"
"    float a = k[i0 * NB + b], c = k[i1 * NB + b];\n"
"    if (has_bias & 2) { a += bk[i0]; c += bk[i1]; }\n"
"    if (norm) { int r0 = norm == 1 ? 0 : h * hd, len = norm == 1 ? kvd : hd;\n"
"      float inv = 1.0f / sqrt(sumsq(k, r0, len, b) / (float)len + eps);\n"
"      a *= kn[i0 - r0] * inv; c *= kn[i1 - r0] * inv; }\n"
"    KVS(a * cs - c * sn, off + i0, kc); KVS(a * sn + c * cs, off + i1, kc);\n"
"  } else {\n"
"    float a = qs[i0 * NB + b], c = qs[i1 * NB + b];\n"
"    if (has_bias & 1) { a += bq[i0]; c += bq[i1]; }\n"
"    if (norm) { int r0 = norm == 1 ? 0 : h * hd, len = norm == 1 ? d : hd;\n"
"      float inv = 1.0f / sqrt(sumsq(qs, r0, len, b) / (float)len + eps);\n"
"      a *= qn[i0 - r0] * inv; c *= qn[i1 - r0] * inv; }\n"
"    q[i0 * NB + b] = a * cs - c * sn; q[i1 * NB + b] = a * sn + c * cs;\n"
"  }\n"
"}\n"
/* atencao: notas (token x cabeca x posicao; so t <= posicao do token), softmax, mistura */
"__kernel void att_scores(__global const float *q, __global const KVT *kc, __global float *att,\n"
"    int base, int pos0, int tmax, int ctx, int hd, int kvd, int group, int nh, float scale) {\n"
"  int gid = get_global_id(0), t = gid % tmax, bh = gid / tmax, h = bh % nh, b = bh / nh;\n"
"  if (t > pos0 + b) return;\n"
"  __global const KVT *kh = kc + base + t * kvd + (h / group) * hd;\n"
"  float s = 0.0f;\n"
"  for (int j = 0; j < hd; j++) s += q[(h * hd + j) * NB + b] * KVL(kh, j);\n"
"  att[(size_t)bh * ctx + t] = s * scale;\n"
"}\n"
"__kernel void att_softmax(__global float *att, int pos0, int ctx, int nh) {\n"
"  int gid = get_global_id(0), T = pos0 + gid / nh + 1;\n"
"  __global float *a = att + (size_t)gid * ctx;\n"
"  float mx = a[0], sum = 0.0f;\n"
"  for (int t = 1; t < T; t++) mx = max(mx, a[t]);\n"
"  for (int t = 0; t < T; t++) { float e = exp(a[t] - mx); a[t] = e; sum += e; }\n"
"  float inv = 1.0f / sum;\n"
"  for (int t = 0; t < T; t++) a[t] *= inv;\n"
"}\n"
"__kernel void att_mix(__global const float *att, __global const KVT *vc, __global float *out,\n"
"    int base, int pos0, int ctx, int hd, int kvd, int group, int nh) {\n"
"  int gid = get_global_id(0), b = gid % NB, i = gid / NB, h = i / hd, j = i % hd;\n"
"  __global const float *a = att + (size_t)(b * nh + h) * ctx;\n"
"  __global const KVT *v = vc + base + (h / group) * hd + j;\n"
"  float s = 0.0f;\n"
"  for (int t = 0; t <= pos0 + b; t++) s += a[t] * KVL(v, t * kvd);\n"
"  out[i * NB + b] = s;\n"
"}\n";

static int cl_load(void) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA("OpenCL.dll");
    if (!h) return 0;
#define SYM(f) if ((*(void **)&cl.f = (void *)GetProcAddress(h, "cl" #f)) == NULL) return 0
#else
    void *h = dlopen("libOpenCL.so.1", RTLD_NOW);
    if (!h) h = dlopen("libOpenCL.so", RTLD_NOW);
    if (!h) return 0;
#define SYM(f) if ((*(void **)&cl.f = dlsym(h, "cl" #f)) == NULL) return 0
#endif
    SYM(GetPlatformIDs); SYM(GetDeviceIDs); SYM(GetDeviceInfo); SYM(CreateContext);
    SYM(CreateCommandQueue); SYM(CreateProgramWithSource); SYM(BuildProgram);
    SYM(GetProgramBuildInfo); SYM(CreateKernel); SYM(CreateBuffer); SYM(SetKernelArg);
    SYM(EnqueueNDRangeKernel); SYM(EnqueueReadBuffer); SYM(EnqueueWriteBuffer); SYM(Finish); SYM(ReleaseMemObject); SYM(Flush); SYM(EnqueueFillBuffer); SYM(GetEventInfo); SYM(ReleaseEvent);
#undef SYM
    return 1;
}

/* compila o fonte para um tamanho de lote e um numero de linhas por thread */
static clh gpu_compile(int nb, int nr, clh dev) {
    int32_t err = 0;
    char opts[160];
    snprintf(opts, sizeof opts, "-cl-mad-enable %s-DNB=%d -DNR=%d -DKV16=%d",
             getenv("KATANA_FASTMATH") && atoi(getenv("KATANA_FASTMATH")) ? "-cl-fast-relaxed-math " : "", nb, nr, gpu.kv16);
    clh prog = cl.CreateProgramWithSource(gpu.ctx, 1, &GPU_SRC, NULL, &err);
    if (err) return NULL;
    if (cl.BuildProgram(prog, 1, &dev, opts, NULL, NULL) != 0) {
        static char log[8192];
        cl.GetProgramBuildInfo(prog, dev, 0x1183, sizeof log - 1, log, NULL);
        fprintf(stderr, "GPU: falha ao compilar os kernels (lote %d, linhas %d):\n%s\n", nb, nr, log);
        return NULL;
    }
    return prog;
}

static int gpu_build_set(KSet *ks, int nb, int nr, clh dev) {
    int32_t err = 0;
    clh prog = gpu_compile(nb, nr, dev), prog2 = gpu_compile(nb, nr * 2, dev);
    if (!prog || !prog2) return 0;
    ks->nb = nb; ks->nr = nr;
#define KERN(pr, field, kname) ks->field = cl.CreateKernel(pr, kname, &err); if (err) return 0; \
    if (n_prof < 64) { prof[n_prof].k = ks->field; prof[n_prof++].name = kname; }
    KERN(prog, k_f32, "mv_f32"); KERN(prog, k_q8, "mv_q8"); KERN(prog, k_q4k, "mv_q4k"); KERN(prog, k_q5k, "mv_q5k"); KERN(prog, k_q6k, "mv_q6k");
    KERN(prog2, kp_f32, "mv_f32"); KERN(prog2, kp_q8, "mv_q8"); KERN(prog2, kp_q4k, "mv_q4k"); KERN(prog2, kp_q5k, "mv_q5k"); KERN(prog2, kp_q6k, "mv_q6k");
    KERN(prog, k_bsum, "bsum"); KERN(prog, k_rms, "rms"); KERN(prog, k_qkv, "qkv_post");
    KERN(prog, k_gather, "gather"); KERN(prog, k_att_scores, "att_scores"); KERN(prog, k_moe_sum, "moe_sum"); KERN(prog, k_rmsh, "rmsh");
    KERN(prog, k_att_softmax, "att_softmax"); KERN(prog, k_att_mix, "att_mix");
    {   /* modo de varias matrizes desligado por padrao em todos os kernels de produto */
        clh mk[10] = { ks->k_f32, ks->k_q8, ks->k_q4k, ks->k_q5k, ks->k_q6k, ks->kp_f32, ks->kp_q8, ks->kp_q4k, ks->kp_q5k, ks->kp_q6k };
        static clh dummy;
        int32_t z = 0, ed = 0;
        if (!dummy) dummy = cl.CreateBuffer(gpu.ctx, 4, 64, NULL, &ed);
        for (int i = 0; i < 10; i++) {
            cl.SetKernelArg(mk[i], 11, sizeof(clh), &dummy); cl.SetKernelArg(mk[i], 12, sizeof z, &z);
            cl.SetKernelArg(mk[i], 13, sizeof z, &z);    cl.SetKernelArg(mk[i], 14, sizeof z, &z);
            cl.SetKernelArg(mk[i], 15, sizeof z, &z);
        }
    }
    ks->k_mv3 = ks->k_att1 = ks->k_att1f = NULL;
    if (nb == 1 && nr == 1 && !getenv("KATANA_NOFUSE")) { KERN(prog, k_mv3, "mv3"); KERN(prog, k_att1, "att1"); KERN(prog, k_att1f, "att1f"); }
#undef KERN
    return 1;
}

/* escolhe a GPU (prefere placa dedicada), cria contexto, fila e kernels */
static int gpu_init(void) {
    if (!cl_load()) return 0;
    clh plats[8], devs[8], best = NULL;
    uint32_t np = 0;
    int best_score = -1;
    if (cl.GetPlatformIDs(8, plats, &np) != 0) return 0;
    for (uint32_t i = 0; i < np && i < 8; i++) {
        uint32_t nd = 0;
        if (cl.GetDeviceIDs(plats[i], 4 /* GPU */, 8, devs, &nd) != 0) continue;
        for (uint32_t j = 0; j < nd && j < 8; j++) {
            char vendor[128] = {0}, name[128] = {0};
            cl.GetDeviceInfo(devs[j], 0x102C, sizeof vendor - 1, vendor, NULL);
            cl.GetDeviceInfo(devs[j], 0x102B, sizeof name - 1, name, NULL);
            int score = (strstr(vendor, "NVIDIA") || strstr(vendor, "Advanced Micro") || strstr(vendor, "AMD")) ? 2 : 1;
            if (score > best_score) {
                best_score = score; best = devs[j];
                memcpy(gpu.name, name, sizeof gpu.name);
                cl.GetDeviceInfo(devs[j], 0x101F, sizeof gpu.vram, &gpu.vram, NULL);
            }
        }
    }
    if (!best) return 0;

    int32_t err = 0;
    gpu.ctx = cl.CreateContext(NULL, 1, &best, NULL, NULL, &err);
    if (err) return 0;
    gpu.queue = cl.CreateCommandQueue(gpu.ctx, best, 0, &err);
    if (err) return 0;
    gpu.debug = getenv("KATANA_DEBUG") != NULL;

    gpu.dev = best;
    return 1;
}

/* compila os conjuntos de kernels (depois de decidir o formato do cache KV).
   Lotes: KATANA_BATCH limita o maior (1 = sem lote). */
static int gpu_build_all(void) {
    static const int sizes[] = { 16, 4, 1 };
    int maxb = getenv("KATANA_BATCH") ? atoi(getenv("KATANA_BATCH")) : GPU_BMAX;
    gpu.n_sets = 0;
    for (int i = 0; i < 3; i++) {
        if (sizes[i] > maxb && sizes[i] != 1) continue;
        const char *ev = getenv(sizes[i] == 1 ? "KATANA_ROWS" : "KATANA_ROWSB");
        int nr = ev ? atoi(ev) : (sizes[i] == 1 ? gpu.cfg_rows : gpu.cfg_rowsb);
        if (nr != 1 && nr != 2) nr = 1;      /* o par gate/up usa 2*nr <= 4 */
        if (!gpu_build_set(&gpu.set[gpu.n_sets], sizes[i], nr, gpu.dev)) return 0;
        gpu.n_sets++;
    }
    return 1;
}

/* Especie do layout na VRAM. Q4_0 vira o layout de 8 bits e Q3_K o de 6 bits: as duas
   conversoes sao exatas (os mesmos valores inteiros e escalas), so ocupam mais VRAM. */
static uint32_t gpu_kind(const Mat *W) {
    if (!W->q) return T_F32;
    if (W->type == T_Q4_0) return T_Q8_0;
    if (W->type == T_Q3_K) return T_Q6_K;
    return W->type;
}

static size_t gpu_wpr(const Mat *W, size_t cols) {
    switch (gpu_kind(W)) {
        case T_F32:  return cols;
        case T_Q4_K: return cols / 256 * 36;
        case T_Q5_K: return cols / 256 * 44;
        case T_Q6_K: return cols / 256 * 53;
        default:     return cols / 32 * 9;
    }
}

/* bloco de 6 bits da GPU: 256 valores (0..63, com +32) em 48 palavras, 16 escalas int8, float d */
static void pack_q6p(const uint8_t *qv, const int8_t *sc, float d, uint32_t *o) {
    for (int g = 0; g < 16; g++) {          /* 16 pesos de 6 bits em 96 bits */
        uint64_t lo = 0, hi = 0;
        for (int k = 0; k < 16; k++) {
            const int bit = 6 * k;
            const uint64_t v = qv[g * 16 + k];
            if (bit < 64) { lo |= v << bit; if (bit > 58) hi |= v >> (64 - bit); }
            else hi |= v << (bit - 64);
        }
        o[g * 3] = (uint32_t)lo; o[g * 3 + 1] = (uint32_t)(lo >> 32); o[g * 3 + 2] = (uint32_t)hi;
    }
    memcpy(o + 48, sc, 16);
    memcpy(o + 52, &d, 4);
}

/* converte a linha r para as palavras do layout da GPU */
static void gpu_pack_row(const Mat *W, size_t r, size_t cols, uint32_t *o) {
    if (!W->q) { memcpy(o, W->f + r * cols, cols * 4); return; }
    const uint8_t *src = W->q + r * W->stride;
    uint16_t h;
    switch (W->type) {
    case T_Q4_K: memcpy(o, src, cols / 256 * 144); return;
    case T_Q5_K: memcpy(o, src, cols / 256 * 176); return;
    case T_Q8_0:
        for (size_t b = 0; b < cols / 32; b++, src += 34, o += 9) {
            memcpy(&h, src, 2);
            float d = f16_to_f32(h);
            memcpy(o, &d, 4); memcpy(o + 1, src + 2, 32);
        }
        return;
    case T_Q4_0:
        for (size_t b = 0; b < cols / 32; b++, src += 18, o += 9) {
            int8_t v[32];
            memcpy(&h, src, 2);
            float d = f16_to_f32(h);
            for (int j = 0; j < 16; j++) { v[j] = (int8_t)((src[2 + j] & 0xF) - 8); v[j + 16] = (int8_t)((src[2 + j] >> 4) - 8); }
            memcpy(o, &d, 4); memcpy(o + 1, v, 32);
        }
        return;
    case T_Q3_K:
        for (size_t b = 0; b < cols / 256; b++, src += 110, o += 53) {
            int8_t val[256], sc[16]; uint8_t qv[256];
            q3k_unpack(src, val, sc);
            for (int i = 0; i < 256; i++) qv[i] = (uint8_t)(val[i] + 32);
            memcpy(&h, src + 108, 2);
            pack_q6p(qv, sc, f16_to_f32(h), o);
        }
        return;
    default:    /* T_Q6_K */
        for (size_t b = 0; b < cols / 256; b++, src += 210, o += 53) {
            uint8_t qv[256];
            const uint8_t *ql = src, *qh = src + 128;
            for (int c = 0; c < 2; c++, ql += 64, qh += 32) {
                uint8_t *y = qv + c * 128;
                for (int l = 0; l < 32; l++) {
                    y[l]      = (uint8_t)((ql[l] & 0xF) | (((qh[l] >> 0) & 3) << 4));
                    y[l + 32] = (uint8_t)((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4));
                    y[l + 64] = (uint8_t)((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4));
                    y[l + 96] = (uint8_t)((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4));
                }
            }
            memcpy(&h, src + 208, 2);
            pack_q6p(qv, (const int8_t *)(src + 192), f16_to_f32(h), o);
        }
        return;
    }
}

/* ---------- SHA-256 (enderecamento e verificacao dos shards) ---------- */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; int n; } Sha;

static const uint32_t SHA_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(Sha *s, const uint8_t *p) {
    uint32_t w[64], a[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(a, s->h, sizeof a);
    for (int i = 0; i < 64; i++) {
        const uint32_t S1 = ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25), ch = (a[4] & a[5]) ^ (~a[4] & a[6]);
        const uint32_t t1 = a[7] + S1 + ch + SHA_K[i] + w[i];
        const uint32_t S0 = ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22), mj = (a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]);
        a[7] = a[6]; a[6] = a[5]; a[5] = a[4]; a[4] = a[3] + t1; a[3] = a[2]; a[2] = a[1]; a[1] = a[0]; a[0] = t1 + S0 + mj;
    }
    for (int i = 0; i < 8; i++) s->h[i] += a[i];
}
static void sha_hex(const void *data, size_t n, char *hex) {
    Sha s = { { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 }, 0, { 0 }, 0 };
    const uint8_t *p = data;
    uint8_t last[128] = { 0 };
    size_t i = 0;
    for (; i + 64 <= n; i += 64) sha_block(&s, p + i);
    const size_t r = n - i;
    memcpy(last, p + i, r);
    last[r] = 0x80;
    const size_t tot = r + 9 <= 64 ? 64 : 128;
    const uint64_t bits = (uint64_t)n * 8;
    for (int k = 0; k < 8; k++) last[tot - 1 - k] = (uint8_t)(bits >> (8 * k));
    sha_block(&s, last);
    if (tot == 128) sha_block(&s, last + 64);
    for (int k = 0; k < 8; k++) snprintf(hex + 8 * k, 9, "%08x", s.h[k]);
}

/* ---------- shards: buffers ja no layout da GPU, um arquivo cada, com hash no manifesto ----------
 * <modelo>.gguf.ktn/manifest.txt + <nome>.bin. `katana pack` gera, a carga usa se existir
 * (pula a conversao do GGUF) e `katana verify` confere os hashes. E a unidade que uma rede
 * p2p distribuiria: cada shard e autocontido e verificavel pelo hash. */
typedef struct { char name[32]; size_t bytes; char hex[65]; } Shard;
static struct { char dir[1024]; int use, n, cap; Shard *e; } shards;

static void shards_open(const char *path, int64_t fsz) {
    char mf[1100], line[256];
    long long origem = -1;
    snprintf(shards.dir, sizeof shards.dir, "%s.ktn", path);
    if (getenv("KATANA_NOSHARDS")) return;
    snprintf(mf, sizeof mf, "%s/manifest.txt", shards.dir);
    FILE *f = fopen(mf, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        Shard e; unsigned long long b;
        if (sscanf(line, "origem %lld", &origem) == 1) continue;
        if (sscanf(line, "%64s %llu %31s", e.hex, &b, e.name) == 3 && strlen(e.hex) == 64) {
            if (shards.n == shards.cap) { shards.cap = shards.cap ? shards.cap * 2 : 256; shards.e = realloc(shards.e, shards.cap * sizeof(Shard)); }
            e.bytes = (size_t)b;
            shards.e[shards.n++] = e;
        }
    }
    fclose(f);
    shards.use = origem == (long long)fsz && shards.n > 0;
    if (!shards.use) shards.n = 0;
}

/* le um shard inteiro; NULL se nao existir ou o tamanho nao bater */
static uint32_t *shard_read(const char *name, size_t bytes) {
    char fn[1100];
    int found = 0;
    for (int i = 0; i < shards.n; i++) if (strcmp(shards.e[i].name, name) == 0 && shards.e[i].bytes == bytes) found = 1;
    if (!found) return NULL;
    snprintf(fn, sizeof fn, "%s/%s.bin", shards.dir, name);
    FILE *f = fopen(fn, "rb");
    if (!f) return NULL;
    uint32_t *buf = malloc(bytes);
    if (buf && fread(buf, 1, bytes, f) != bytes) { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}

typedef struct { Mat **ms; int n; size_t rows, cols, wpr; uint32_t *buf; } PackJob;
static void pack_job(void *c, int g0, int g1) {
    const PackJob *j = c;
    uint32_t *tmp = malloc(j->wpr * 4);
    if (!tmp) die("sem memoria");
    for (int g = g0; g < g1; g++) {
        const Mat *W = j->ms[g % j->n];
        const size_t r0 = (size_t)(g / j->n) * 32;
        uint32_t *base = j->buf + (size_t)g * j->wpr * 32;
        for (size_t k = 0; k < 32 && r0 + k < j->rows; k++) {
            gpu_pack_row(W, r0 + k, j->cols, tmp);
            for (size_t w = 0; w < j->wpr; w++) base[w * 32 + k] = tmp[w];
        }
    }
    free(tmp);
}

/* monta o buffer de n matrizes de mesmo formato: layout entrelacado, intercaladas por grupo de 32 linhas */
static uint32_t *gpu_build_buf(Mat **ms, int n, size_t rows, size_t cols, size_t *bytes_out) {
    const size_t wpr = gpu_wpr(ms[0], cols), rowsP = (rows + 127) & ~(size_t)127;
    uint32_t *buf = calloc(rowsP * wpr * n, 4);
    *bytes_out = rowsP * wpr * 4 * n;
    if (!buf) return NULL;
    PackJob job = { ms, n, rows, cols, wpr, buf };
    pool_for(pack_job, &job, (int)(rowsP / 32) * n, 4);
    return buf;
}

/* envia n matrizes para um unico buffer na VRAM (do shard, se houver; senao convertendo o GGUF) */
#ifdef _WIN32
#define FSEEK64 _fseeki64
#else
#define FSEEK64 fseeko
#endif
/* estado do cache de especialistas (MoE) na VRAM; explicado junto de expert_ensure */
typedef struct { uint32_t *glu, *down; int slot, pending; } ExpSlot;  /* blobs mornos (RAM), vaga na VRAM (-1 = fora), pedido em curso */
/* fila circular de cargas em segundo plano: a thread carregadora le (ou converte) o especialista
   para o buffer do pedido; a thread principal instala na VRAM quando fica pronto */
#define LQ 64
typedef struct { int l, e; uint8_t *buf; volatile long done; } LoadReq;
static struct {
    clh bqn, bkn, *qn, *kn;         /* norma de q e k */
    clh *sg, *sd;                   /* por camada: vagas dos pares gate/up e dos down */
    clh bh, bt;                     /* saidas dos especialistas do lote */
    clh bmeta, bxn;                 /* metadados do lote (faixas, pesos, linhas) e entradas por token */
    uint32_t *meta; float *w; uint32_t *rowi; float *xn;
    ExpSlot *slot;                  /* [camada][especialista] */
    int *owner; uint64_t *stamp;    /* [camada][vaga]: quem ocupa e quando foi usado */
    int on, S;                      /* vagas por camada */
    size_t *bg, *bd;                /* por camada: bytes de um par gate/up e de um down */
    LoadReq rq[LQ];
    volatile long head;             /* proximo pedido a criar (thread principal) */
    long fin, fin_safe;             /* proximo a instalar; ate onde as copias ja terminaram na GPU */
    int loader;                     /* thread carregadora ligada */
    uint32_t *pre; int n_pre, i_pre;        /* especialistas quentes da sessao anterior, a pre-carregar */
    float swap_margin;
    Model *m;
    FILE **pf;                      /* por camada: arquivo com os especialistas ja no layout da GPU (pack) */
    uint8_t *stage;                 /* um especialista lido do arquivo, a caminho da VRAM */
    float *heat;                    /* [camada][especialista]: uso recente (decai a cada token) */
    float *cpu_out;                 /* saidas dos especialistas que rodaram na CPU neste token */
    int cpu_cold;                   /* especialista frio roda na CPU ate esquentar */
    uint64_t clock, hits, miss, evict, conv, cpu_runs, loads;
    double t_up, t_cpu, t_read;
} gx;
static void loader_start(Model *m, size_t maxper);
static void heat_load(const Model *m);
static void preload_feed(const Model *m);

static void *vk_wbuf(const uint32_t *host, size_t bytes);     /* Vulkan: buffer de pesos na VRAM */
static void gpu_upload_n(Mat **ms, int n, size_t rows, size_t cols, const char *name) {
    size_t bytes = ((rows + 127) & ~(size_t)127) * gpu_wpr(ms[0], cols) * 4 * n;
    gpu.n_total += n;
    if (gpu.used + bytes > gpu_budget()) return;
    uint32_t *buf = shards.use ? shard_read(name, bytes) : NULL;
    if (buf) gpu.n_shard++;
    else buf = gpu_build_buf(ms, n, rows, cols, &bytes);
    if (!buf) return;
    int32_t err = 0;
    clh b;
    if (gpu.vk) { b = vk_wbuf(buf, bytes); free(buf); if (!b) return; }
    else {
        b = cl.CreateBuffer(gpu.ctx, 4 | 32 /* READ_ONLY | COPY_HOST_PTR */, bytes, buf, &err);
        free(buf);
        if (err || !b) return;
    }
    for (int i = 0; i < n; i++) { ms[i]->gbuf = b; ms[i]->gs = n; ms[i]->go = i; }
    gpu.used += bytes; gpu.n_up += n;
}

static void gpu_upload(Mat *W, size_t rows, size_t cols, const char *name) { gpu_upload_n(&W, 1, rows, cols, name); }

/* gate e up tem o mesmo tipo? entao vao intercalados num buffer so (um kernel faz o SwiGLU) */
static int glu_pair(const Mat *gate, const Mat *up) {
    return (gate->q != NULL) == (up->q != NULL) && gate->type == up->type && !getenv("KATANA_NOPAIR");
}

static void gpu_upload_glu(Mat *gate, Mat *up, size_t rows, size_t cols, int l) {
    char nm[32];
    if (glu_pair(gate, up)) {
        Mat *ms[2] = { gate, up };
        snprintf(nm, sizeof nm, "L%03d.glu", l);
        gpu_upload_n(ms, 2, rows, cols, nm);
        gate->pair = gate->gbuf != NULL;
    } else {
        snprintf(nm, sizeof nm, "L%03d.gate", l); gpu_upload(gate, rows, cols, nm);
        snprintf(nm, sizeof nm, "L%03d.up", l);   gpu_upload(up, rows, cols, nm);
    }
}

/* buffer de leitura/escrita na VRAM; NULL se nao couber */
static clh gbuf_rw(size_t bytes) {
    int32_t err = 0;
    if (gpu.used + bytes > gpu_budget()) return NULL;
    clh b = cl.CreateBuffer(gpu.ctx, 1, bytes, NULL, &err);
    if (err) return NULL;
    gpu.used += bytes;
    return b;
}
/* vetor float32 constante (normas, bias); NULL se src for NULL */
static clh gbuf_f(const float *src, size_t n, int *fail) {
    int32_t err = 0;
    if (!src) return NULL;
    clh b = cl.CreateBuffer(gpu.ctx, 4 | 32, n * sizeof(float), (void *)src, &err);
    if (err || !b) { *fail = 1; return NULL; }
    gpu.used += n * sizeof(float);
    return b;
}

/* quantos segmentos de colunas (divisor de nb, ate 8 ou 4) usar para `threads` threads */
static int gpu_nseg(size_t threads, int nb, int lote_x_linhas) {
    const int maxseg = lote_x_linhas > 32 ? 4 : 8;      /* igual ao MAXSEG dos kernels */
    int best = 1;
    if (!gpu.seg_target || nb <= 1 || threads >= gpu.seg_target) return 1;
    for (int sg = 1; sg <= maxseg && sg <= nb; sg++) if (nb % sg == 0) {
        best = sg;
        if (threads * sg >= gpu.seg_target) break;
    }
    return best;
}


/* ================= backend Vulkan (so o produto matriz x vetor; o grafo ainda roda na CPU) ================= */

/* um buffer por matriz (ou por par gate/up); o handle fica em Mat.gbuf como ponteiro para KvkBuf */
static size_t gpu_mat_bytes(const Mat *W, size_t rows, size_t cols);
static void *vk_wbuf(const uint32_t *host, size_t bytes) {
    KvkBuf b = kvk_buf_create(bytes, KVK_USAGE_RW);
    if (!b.b) { fprintf(stderr, "Vulkan: buffer de %.1f MB falhou: %s\n", bytes / 1048576.0, kvk_error()); return NULL; }
    if (!kvk_buf_write(b, 0, host, bytes)) { fprintf(stderr, "Vulkan: envio falhou: %s\n", kvk_error()); kvk_buf_destroy(&b); return NULL; }
    KvkBuf *p = xalloc(1, sizeof(KvkBuf));
    *p = b;
    gpu.vk_nbuf++;
    return p;
}

static const uint32_t *vk_spv[5]; static size_t vk_spv_n[5];
static KvkPipe vk_pipe[5][9];      /* [tipo][nseg]: criados sob demanda */

static int vk_kind_idx(uint32_t kind) { return kind == T_F32 ? 0 : kind == T_Q8_0 ? 1 : kind == T_Q4_K ? 2 : kind == T_Q5_K ? 3 : 4; }

static KvkPipe vk_pipe2[5][9];     /* mesmos kernels com NR=2 (par gate/up) */

/* enfileira out = W * x (um token) sem esperar; mesmo layout, gs/go e nseg dos kernels OpenCL.
   mode: 0 grava, 1 soma, 2 SwiGLU com out, bit 4 = soma bias (buffer `bias`), pair = gate/up intercalados (grava silu(g)*u) */
static int vk_gmv_ex(const Mat *W, KvkBuf x, KvkBuf out, KvkBuf bias, int rows, int cols, int mode, int pair) {
    if (!vk_spv[0]) {
        vk_spv[0] = mv_f32_spv; vk_spv_n[0] = sizeof mv_f32_spv; vk_spv[1] = mv_q8_spv; vk_spv_n[1] = sizeof mv_q8_spv;
        vk_spv[2] = mv_q4k_spv; vk_spv_n[2] = sizeof mv_q4k_spv; vk_spv[3] = mv_q5k_spv; vk_spv_n[3] = sizeof mv_q5k_spv;
        vk_spv[4] = mv_q6k_spv; vk_spv_n[4] = sizeof mv_q6k_spv;
    }
    const uint32_t kind = gpu_kind(W);
    const int ki = vk_kind_idx(kind);
    const size_t rowsP = ((size_t)rows + 127) & ~(size_t)127;
    const int nb = kind == T_F32 ? 1 : kind == T_Q8_0 ? cols / 32 : cols / 256;
    const int nseg = gpu_nseg(rowsP, nb, pair ? 2 : 1);
    KvkPipe *pp = pair ? &vk_pipe2[ki][nseg] : &vk_pipe[ki][nseg];
    if (!pp->p) {
        const uint32_t ids[2] = { 0, 1 }, vals[2] = { pair ? 2u : 1u, (uint32_t)nseg };
        *pp = kvk_pipe_create(vk_spv[ki], vk_spv_n[ki], 4, 20, ids, vals, 2);
        if (!pp->p) { fprintf(stderr, "Vulkan: pipeline falhou: %s\n", kvk_error()); return 0; }
    }
    const KvkBuf bufs[4] = { *(const KvkBuf *)W->gbuf, x, out, bias };
    const int pc[5] = { cols, pair ? (mode | 8) : mode, rows, pair ? 1 : W->gs, pair ? 0 : W->go };
    return kvk_dispatch(*pp, bufs, 4, pc, sizeof pc, (uint32_t)(rowsP / 32), 1, 1);
}

static int vk_gmv(const Mat *W, KvkBuf x, KvkBuf out, int rows, int cols, int mode) {
    return vk_gmv_ex(W, x, out, x, rows, cols, mode, 0);
}

static void vk_matvec(float *out, const Mat *W, const float *x, int rows, int cols) {
    if (!kvk_buf_write_q(gpu.vxb, 0, x, (size_t)cols * sizeof(float)) || !vk_gmv(W, gpu.vxb, gpu.vob, rows, cols, 0)
        || !kvk_buf_read(gpu.vob, 0, out, (size_t)rows * sizeof(float))) die("falha na execucao em Vulkan");
}

/* ---------- grafo denso completo em Vulkan (um token por vez) ----------
   Mesma sequencia do gpu_forward_n do OpenCL: norma, q/k/v, atencao fundida (att1f), saida com residual,
   norma, gate/up com SwiGLU, down com residual; no fim norma final e vocabulario. A posicao do token vem
   de um buffer pequeno (meta), entao o mesmo grafo gravado serve para todos os tokens. */
static void mat_row(float *out, const Mat *W, int row, int cols);

typedef struct {
    int on, rec, kv16;
    KvkBuf x, xn, q, k, v, o, hb, lg, att, meta, freq, onorm, dummy;
    KvkBuf *an, *fn, *bq, *bk, *bv, *bo, *qn, *kn, *kc, *vc;    /* b==0: ausente */
    KvkPipe rms, att1f;
    KvkRec g[2];                    /* grafo gravado: [0] sem a projecao final, [1] com ela */
    int coop, sg32;                 /* produto cooperativo fp16 disponivel; subgrupo de 32 */
    int P;                          /* maior lote por passada em Vulkan (0 = so token a token) */
    int g_ok[2];
    double t_emb, t_sub, t_wait;    /* embedding+escrita, enfileirar/submeter, espera+leitura */
} VkG;
static VkG vkg;
#define VK_PASS_DEF 256
static int vk_batch_build(Model *m);
static int vk_emit_head(const Model *m);

static KvkBuf vk_wk(size_t bytes, int *fail) {
    KvkBuf b = kvk_buf_create(bytes < 16 ? 16 : bytes, KVK_USAGE_RW);
    if (!b.b) { *fail = 1; return b; }
    gpu.used += bytes;
    return b;
}

/* vetor float32 constante na VRAM; handle zerado se src for NULL */
static KvkBuf vk_vec(const float *src, size_t n, int *fail) {
    KvkBuf z; memset(&z, 0, sizeof z);
    if (!src) return z;
    KvkBuf b = kvk_buf_create(n * sizeof(float), KVK_USAGE_RW);
    if (!b.b || !kvk_buf_write_q(b, 0, src, n * sizeof(float))) { *fail = 1; return z; }
    gpu.used += n * sizeof(float);
    return b;
}

/* decide se o grafo completo cabe (pesos + vetores + cache KV + buffers de trabalho) e o tipo do cache */
static int vk_plan(const Model *m, uint64_t budget) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd, V = c->vocab, qd = (size_t)c->n_heads * c->hd;
    if (getenv("KATANA_VK_GRAPH") && atoi(getenv("KATANA_VK_GRAPH")) == 0) return 0;
    if (c->hd > 256 || c->hd % 2) { fprintf(stderr, "Vulkan: cabeca de %d nao suportada pelo grafo, grafo na CPU\n", c->hd); return 0; }
    size_t w = gpu_mat_bytes(&m->lm_head, V, d), pequenos = d * 4, kv32 = 0;
    for (int l = 0; l < c->n_layers; l++) {
        const Layer *L = &m->layers[l];
        w += gpu_mat_bytes(&L->wq, qd, d) + gpu_mat_bytes(&L->wk, kvd, d) + gpu_mat_bytes(&L->wv, kvd, d)
           + gpu_mat_bytes(&L->wo, d, qd) + gpu_mat_bytes(&L->w_gate, H, d) + gpu_mat_bytes(&L->w_up, H, d)
           + gpu_mat_bytes(&L->w_down, d, H);
        pequenos += (2 * d + qd + 2 * kvd + d + (c->qk_norm ? d + kvd : 0)) * 4;
        kv32 += 2 * (size_t)c->ctx * kvd * 4;
    }
    const size_t work = (d + 2 * (qd > d ? qd : d) + qd + 2 * kvd + H + V + (size_t)c->n_heads * c->ctx + 256) * 4;
    int kv16 = getenv("KATANA_KV16") ? atoi(getenv("KATANA_KV16")) != 0
             : gpu.used + w + pequenos + work + kv32 + ((size_t)256 << 20) > budget;
    const size_t need = w + pequenos + work + (kv16 ? kv32 / 2 : kv32);
    if (gpu.used + need > budget) {
        fprintf(stderr, "Vulkan: nao cabe (grafo completo precisa de %.0f MB, orcamento livre %.0f MB): grafo na CPU\n",
                need / 1048576.0, (budget > gpu.used ? budget - gpu.used : 0) / 1048576.0);
        return 0;
    }
    vkg.kv16 = kv16;
    /* lote do prompt: buffers de trabalho por token (ativacoes transpostas); o que sobrar do orcamento limita o lote */
    vkg.P = 0;
    if (vkg.coop && !(getenv("KATANA_VK_BATCH") && atoi(getenv("KATANA_VK_BATCH")) == 0)) {
        const size_t mx = d > qd ? (d > H ? d : H) : (qd > H ? qd : H);
        const size_t per = d * 4 + mx * 2 + 2 * qd * 4 + 2 * kvd * 4 + H * 2;
        const size_t left = budget - gpu.used - need;
        size_t P = getenv("KATANA_VK_PASS") ? (size_t)atoi(getenv("KATANA_VK_PASS")) : VK_PASS_DEF;
        if (P * per > left) P = left / per;
        if (P > 1024) P = 1024;
        vkg.P = P >= 2 ? (int)P : 0;
    }
    return 1;
}

/* cria vetores, cache KV, buffers de trabalho e pipelines do grafo; 0 se algo falhar */
static int vk_graph_build(Model *m) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd, V = c->vocab, qd = (size_t)c->n_heads * c->hd;
    const size_t NL = c->n_layers, dq = qd > d ? qd : d;
    int fail = 0;
    for (size_t l = 0; l < NL; l++) {
        const Layer *L = &m->layers[l];
        if (!L->wq.gbuf || !L->wk.gbuf || !L->wv.gbuf || !L->wo.gbuf || !L->w_gate.gbuf || !L->w_up.gbuf || !L->w_down.gbuf) return 0;
    }
    if (!m->lm_head.gbuf) return 0;
    vkg.x = vk_wk(d * 4, &fail); vkg.xn = vk_wk(dq * 4, &fail); vkg.q = vk_wk(dq * 4, &fail);
    vkg.k = vk_wk(kvd * 4, &fail); vkg.v = vk_wk(kvd * 4, &fail); vkg.o = vk_wk(qd * 4, &fail);
    vkg.hb = vk_wk(H * 4, &fail); vkg.lg = vk_wk((V + 128) * 4, &fail);
    vkg.att = vk_wk((size_t)c->n_heads * c->ctx * 4, &fail); vkg.meta = vk_wk(16, &fail); vkg.dummy = vk_wk(256, &fail);
    vkg.freq = vk_vec(m->rope_freq, c->hd / 2, &fail); vkg.onorm = vk_vec(m->out_norm, d, &fail);
    vkg.an = xalloc(NL, sizeof(KvkBuf)); vkg.fn = xalloc(NL, sizeof(KvkBuf)); vkg.bq = xalloc(NL, sizeof(KvkBuf));
    vkg.bk = xalloc(NL, sizeof(KvkBuf)); vkg.bv = xalloc(NL, sizeof(KvkBuf)); vkg.bo = xalloc(NL, sizeof(KvkBuf));
    vkg.qn = xalloc(NL, sizeof(KvkBuf)); vkg.kn = xalloc(NL, sizeof(KvkBuf));
    vkg.kc = xalloc(NL, sizeof(KvkBuf)); vkg.vc = xalloc(NL, sizeof(KvkBuf));
    const size_t kvb = (size_t)c->ctx * kvd * (vkg.kv16 ? 2 : 4);
    for (size_t l = 0; l < NL && !fail; l++) {
        const Layer *L = &m->layers[l];
        vkg.an[l] = vk_vec(L->attn_norm, d, &fail); vkg.fn[l] = vk_vec(L->ffn_norm, d, &fail);
        vkg.bq[l] = vk_vec(L->bq, qd, &fail); vkg.bk[l] = vk_vec(L->bk, kvd, &fail);
        vkg.bv[l] = vk_vec(L->bv, kvd, &fail); vkg.bo[l] = vk_vec(L->bo, d, &fail);
        if (c->qk_norm) {
            vkg.qn[l] = vk_vec(L->q_norm, c->qk_norm == 1 ? d : (size_t)c->hd, &fail);
            vkg.kn[l] = vk_vec(L->k_norm, c->qk_norm == 1 ? kvd : (size_t)c->hd, &fail);
        }
        vkg.kc[l] = vk_wk(kvb, &fail); vkg.vc[l] = vk_wk(kvb, &fail);
    }
    if (fail) { fprintf(stderr, "Vulkan: sem memoria para o grafo completo (%s)\n", kvk_error()); return 0; }
    kvk_wait();
    vkg.rms = kvk_pipe_create(rms_wg_spv, sizeof rms_wg_spv, 3, 12, NULL, NULL, 0);
    const uint32_t id0 = 0, hdv = (uint32_t)c->hd;
    vkg.att1f = vkg.kv16 ? kvk_pipe_create(att1f_kv16_spv, sizeof att1f_kv16_spv, 14, 44, &id0, &hdv, 1)
                         : kvk_pipe_create(att1f_kv32_spv, sizeof att1f_kv32_spv, 14, 44, &id0, &hdv, 1);
    if (!vkg.rms.p || !vkg.att1f.p) { fprintf(stderr, "Vulkan: pipeline do grafo falhou: %s\n", kvk_error()); return 0; }
    vkg.on = 1;
    vk_batch_build(m);
    return 1;
}

/* norma final + projecao para o vocabulario do vetor vkg.x (um token) */
static int vk_emit_head(const Model *m) {
    const Config *c = &m->cfg;
    const struct { int n; float eps; int B; } rp = { c->dim, c->eps, 1 };
    const KvkBuf rb[3] = { vkg.x, vkg.onorm, vkg.xn };
    int ok = kvk_dispatch(vkg.rms, rb, 3, &rp, sizeof rp, 1, 1, 1);
    ok &= vk_gmv_ex(&m->lm_head, vkg.xn, vkg.lg, vkg.dummy, c->vocab, c->dim, 0, 0);
    return ok;
}

/* enfileira (ao vivo ou na gravacao) todos os despachos de um token; head = norma final + vocabulario */
static int vk_emit(const Model *m, int head) {
    const Config *c = &m->cfg;
    const int d = c->dim, hd = c->hd, H = c->hidden, kvd = c->n_kv * hd, qd = c->n_heads * hd, nh = c->n_heads;
    const struct { int n; float eps; int B; } rp = { d, c->eps, 1 };
    int ok = 1;
    for (int l = 0; l < c->n_layers && ok; l++) {
        const Layer *L = &m->layers[l];
        const KvkBuf rb[3] = { vkg.x, vkg.an[l], vkg.xn };
        ok &= kvk_dispatch(vkg.rms, rb, 3, &rp, sizeof rp, 1, 1, 1);
        ok &= vk_gmv_ex(&L->wq, vkg.xn, vkg.q, vkg.dummy, qd, d, 0, 0);
        ok &= vk_gmv_ex(&L->wk, vkg.xn, vkg.k, vkg.dummy, kvd, d, 0, 0);
        ok &= vk_gmv_ex(&L->wv, vkg.xn, vkg.v, vkg.dummy, kvd, d, 0, 0);
#define OR_DUMMY(v) ((v).b ? (v) : vkg.dummy)
        const KvkBuf ab[14] = { vkg.q, vkg.k, vkg.v, OR_DUMMY(vkg.bq[l]), OR_DUMMY(vkg.bk[l]), OR_DUMMY(vkg.bv[l]),
                                vkg.kc[l], vkg.vc[l], vkg.att, vkg.o, vkg.freq, OR_DUMMY(vkg.qn[l]), OR_DUMMY(vkg.kn[l]), vkg.meta };
        const struct { int base, ctx, hd, kvd, group, neox, has_bias, norm, d; float scale, eps; } ap =
            { 0, c->ctx, hd, kvd, nh / c->n_kv, c->rope_neox, (vkg.bq[l].b ? 1 : 0) | (vkg.bk[l].b ? 2 : 0) | (vkg.bv[l].b ? 4 : 0),
              c->qk_norm, qd, 1.0f / sqrtf((float)hd), c->eps };
        ok &= kvk_dispatch(vkg.att1f, ab, 14, &ap, sizeof ap, (uint32_t)nh, 1, 1);
        ok &= vk_gmv_ex(&L->wo, vkg.o, vkg.x, OR_DUMMY(vkg.bo[l]), d, qd, 1 | (vkg.bo[l].b ? 4 : 0), 0);
        const KvkBuf rb2[3] = { vkg.x, vkg.fn[l], vkg.xn };
        ok &= kvk_dispatch(vkg.rms, rb2, 3, &rp, sizeof rp, 1, 1, 1);
        if (L->w_gate.pair) ok &= vk_gmv_ex(&L->w_gate, vkg.xn, vkg.hb, vkg.dummy, H, d, 0, 1);
        else {
            ok &= vk_gmv_ex(&L->w_gate, vkg.xn, vkg.hb, vkg.dummy, H, d, 0, 0);
            ok &= vk_gmv_ex(&L->w_up, vkg.xn, vkg.hb, vkg.dummy, H, d, 2, 0);
        }
        ok &= vk_gmv_ex(&L->w_down, vkg.hb, vkg.x, vkg.dummy, d, H, 1, 0);
#undef OR_DUMMY
    }
    if (ok && head) ok &= vk_emit_head(m);
    return ok;
}

/* um token pelas camadas todas na GPU; devolve os logits (m->logits) ou NULL se !want_logits */
static float *vk_forward_1(Model *m, int token, int pos, int want_logits) {
    const Config *c = &m->cfg;
    double t0 = now_sec();
    if (pos < 0 || pos >= c->ctx) die("posicao fora do contexto (Vulkan)");
    mat_row(m->x, &m->tok_emb, token, c->dim);
    const int meta[4] = { pos, 0, 0, 0 };
    if (!kvk_buf_write_q(vkg.x, 0, m->x, (size_t)c->dim * sizeof(float)) || !kvk_buf_write_q(vkg.meta, 0, meta, sizeof meta))
        die("falha na execucao em Vulkan (escrita)");
    double t1 = now_sec();
    const int h = want_logits ? 1 : 0;
    if (vkg.rec) {
        if (!vkg.g_ok[h]) {
            if (!kvk_rec_begin()) die("falha ao gravar o grafo Vulkan");
            if (!vk_emit(m, h)) { kvk_rec_cancel(); die("falha ao gravar o grafo Vulkan"); }
            vkg.g[h] = kvk_rec_end();
            if (!vkg.g[h].cb) die("falha ao gravar o grafo Vulkan");
            vkg.g_ok[h] = 1;
            t1 = now_sec();
        }
        if (!kvk_rec_submit(vkg.g[h])) die("falha na execucao em Vulkan (submit)");
    } else {
        if (!vk_emit(m, h) || !kvk_flush()) die("falha na execucao em Vulkan");
    }
    double t2 = now_sec();
    if (!want_logits) { vkg.t_emb += t1 - t0; vkg.t_sub += t2 - t1; return NULL; }
    if (!kvk_buf_read(vkg.lg, 0, m->logits, (size_t)c->vocab * sizeof(float))) die("falha na execucao em Vulkan (leitura)");
    double t3 = now_sec();
    vkg.t_emb += t1 - t0; vkg.t_sub += t2 - t1; vkg.t_wait += t3 - t2;
    return m->logits;
}

/* ---------- prompt em lote em Vulkan (produtos em matrizes cooperativas fp16) ----------
   Camadas na mesma ordem do grafo de um token, mas para B tokens por passada. Layout das ativacoes: TRANSPOSTO,
   X[coluna*B + token] (o mm.comp le x[col*tokens + token] e grava y[linha*tokens + token]; qkv_post, rms e a atencao
   usam o mesmo layout), entao a saida de um produto e a entrada do seguinte sem copiar. Entradas dos produtos em fp16:
   a norma ja grava fp16 (rms_wg16) e a atencao tambem; so a saida do gate/up (fp32) passa por uma conversao.
   Residual em fp32 (bx), produto com modo "soma". So o ULTIMO token vai a norma final e ao vocabulario (gather da
   ultima coluna para o vetor de um token e o mesmo caminho do vk_forward_1). Os produtos arredondam pesos e ativacoes
   a fp16 dentro dos ladrilhos: o resultado difere do caminho float (comparar com `selftest`). */
typedef struct {
    int ok, P, flash;
    KvkBuf x, xh, q, q2, k, v, hb, o3, att3;
    KvkPipe rms16, qkv, flash_p, sc, sm, mix, cvt, gat;
    KvkPipe mm[5][6];               /* [tipo do mm.comp][indice do ladrilho, plano ou glu] */
    float *xT;
    unsigned skip;                  /* KATANA_VK_SKIP: bits de fases a omitir (so para medir o custo de cada uma) */
    double t_emb, t_emit, t_wait; long passes, toks;
} VkB;
static VkB vkb;

/* tipo do mm.comp: 0 Q4_K, 1 Q6_K, 2 Q5_K, 3 Q8_0, 4 F32; vk_kind_idx: 0 F32, 1 Q8_0, 2 Q4_K, 3 Q5_K, 4 Q6_K */
static const int vk_mmt[5] = { 4, 3, 0, 2, 1 };
/* variantes: 0 64x64, 1 64x32w4 (so plano), 2 64x16w4 (so plano), 3 64x32 (glu), 4 64x16 (glu) */
static const int vk_mm_bn[5] = { 64, 32, 16, 32, 16 };
static const uint32_t *vk_mm_spv(int t, int v, int glu, size_t *n) {
#define MMS(T) \
    { { mm_t##T##_64x64_g0_spv, sizeof mm_t##T##_64x64_g0_spv }, { mm_t##T##_64x32w4_g0_spv, sizeof mm_t##T##_64x32w4_g0_spv }, \
      { mm_t##T##_64x16w4_g0_spv, sizeof mm_t##T##_64x16w4_g0_spv }, { mm_t##T##_64x64_g1_spv, sizeof mm_t##T##_64x64_g1_spv }, \
      { mm_t##T##_64x32_g1_spv, sizeof mm_t##T##_64x32_g1_spv }, { mm_t##T##_64x16_g1_spv, sizeof mm_t##T##_64x16_g1_spv } }
    static const struct { const uint32_t *p; size_t n; } tab[5][6] = { MMS(0), MMS(1), MMS(2), MMS(3), MMS(4) };
#undef MMS
    /* indice: plano 0,1,2 = 64x64, 64x32w4, 64x16w4; glu 3,4,5 = 64x64, 64x32, 64x16 */
    const int ix = glu ? (v == 0 ? 3 : v == 3 ? 4 : 5) : v;
    *n = tab[t][ix].n;
    return tab[t][ix].p;
}

/* regra de escolha do ladrilho (tools/vkq4k/NOTAS.md): ate 16 tokens 64x16, ate 32 64x32, acima 64x64
   (Q6_K/Q8_0 com 33..64 tokens: 64x32 de 4 subgrupos) */
static int vk_tile(int ki, int B, int glu) {
    if (B <= 16) return glu ? 4 : 2;
    if (B <= 32) return glu ? 3 : 1;
    if (!glu && B <= 64 && (ki == 1 || ki == 4)) return 1;
    return 0;
}

/* y = W * x (B tokens), x fp16 [cols][B], y fp32 [rows][B]; add: y += W*x; glu: W e o par gate/up intercalado (y = silu(gate)*up) */
static int vk_mm(const Mat *W, KvkBuf xh, KvkBuf y, int rows, int cols, int B, int add, int glu) {
    const int ki = vk_kind_idx(gpu_kind(W)), t = vk_mmt[ki], v = vk_tile(ki, B, glu);
    KvkPipe *pp = &vkb.mm[t][glu ? (v == 0 ? 3 : v == 3 ? 4 : 5) : v];
    if (!pp->p) {
        size_t n; const uint32_t *spv = vk_mm_spv(t, v, glu, &n);
        *pp = kvk_pipe_create(spv, n, 3, 32, NULL, NULL, 0);
        if (!pp->p) { fprintf(stderr, "Vulkan: pipeline do produto em lote falhou: %s\n", kvk_error()); return 0; }
    }
    const uint32_t pc[8] = { (uint32_t)rows, (uint32_t)cols, (uint32_t)B, (uint32_t)gpu_wpr(W, cols), (uint32_t)cols, 0, (uint32_t)add, 0 };
    const KvkBuf bufs[3] = { *(const KvkBuf *)W->gbuf, xh, y };
    const int bm = 64, bn = vk_mm_bn[v], gm = glu ? bm / 2 : bm;
    return kvk_dispatch(*pp, bufs, 3, pc, sizeof pc, (uint32_t)((rows + gm - 1) / gm), (uint32_t)((B + bn - 1) / bn), 1);
}

/* o lote Vulkan so vale para camadas que o caminho acima sabe fazer */
static int vk_batch_supported(const Model *m) {
    const Config *c = &m->cfg;
    const int d = c->dim, qd = c->n_heads * c->hd, H = c->hidden;
    if (d % 64 || qd % 64 || H % 64) return 0;
    for (int l = 0; l < c->n_layers; l++) {
        const Layer *L = &m->layers[l];
        if (L->bo || !L->w_gate.pair) return 0;
        const Mat *ms[5] = { &L->wq, &L->wk, &L->wv, &L->wo, &L->w_down };
        for (int i = 0; i < 5; i++) if (ms[i]->gs != 1 || !ms[i]->gbuf) return 0;
    }
    return 1;
}

static int vk_batch_build(Model *m) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd, qd = (size_t)c->n_heads * c->hd;
    const size_t mx = d > qd ? (d > H ? d : H) : (qd > H ? qd : H);
    int fail = 0;
    memset(&vkb, 0, sizeof vkb);
    if (!vkg.P || !vkg.coop || !vk_batch_supported(m)) { vkg.P = 0; return 0; }
    const char *av = getenv("KATANA_VK_ATT");           /* 1 = um lancamento (padrao), 3 = tres kernels */
    vkb.flash = !(av && atoi(av) == 3) && vkg.sg32 && c->hd % 32 == 0 && c->hd <= 256 && c->n_heads % c->n_kv == 0 && c->n_heads / c->n_kv <= 8;
    int P = vkg.P;
    if (!vkb.flash && P > 64) P = 64;                   /* o buffer de notas cresce com o lote */
    vkb.P = P;
    vkb.x = vk_wk(d * P * 4, &fail); vkb.xh = vk_wk(mx * P * 2, &fail);
    vkb.q = vk_wk(qd * P * 4, &fail); vkb.q2 = vk_wk(qd * P * 4, &fail);
    vkb.k = vk_wk(kvd * P * 4, &fail); vkb.v = vk_wk(kvd * P * 4, &fail); vkb.hb = vk_wk(H * P * 2, &fail);
    if (!vkb.flash) { vkb.o3 = vk_wk(qd * P * 4, &fail); vkb.att3 = vk_wk((size_t)P * c->n_heads * c->ctx * 4, &fail); }
    vkb.xT = xalloc(d * P, sizeof(float));
    if (fail) { fprintf(stderr, "Vulkan: sem memoria para o lote (%s): prompt token a token\n", kvk_error()); vkg.P = 0; return 0; }
    const uint32_t fid[2] = { 0, 1 }, fv[2] = { (uint32_t)c->hd, (uint32_t)(c->n_heads / c->n_kv) };
    vkb.rms16 = kvk_pipe_create(rms_wg16_spv, sizeof rms_wg16_spv, 3, 12, NULL, NULL, 0);
    vkb.qkv = vkg.kv16 ? kvk_pipe_create(qkv_post_kv16_spv, sizeof qkv_post_kv16_spv, 12, 40, NULL, NULL, 0)
                       : kvk_pipe_create(qkv_post_kv32_spv, sizeof qkv_post_kv32_spv, 12, 40, NULL, NULL, 0);
    vkb.cvt = kvk_pipe_create(cvt_spv, sizeof cvt_spv, 3, 4, NULL, NULL, 0);
    vkb.gat = kvk_pipe_create(gather_spv, sizeof gather_spv, 2, 12, NULL, NULL, 0);
    if (vkb.flash)
        vkb.flash_p = vkg.kv16 ? kvk_pipe_create(att_flash_kv16_spv, sizeof att_flash_kv16_spv, 4, 24, fid, fv, 2)
                               : kvk_pipe_create(att_flash_kv32_spv, sizeof att_flash_kv32_spv, 4, 24, fid, fv, 2);
    else {
        vkb.sc = vkg.kv16 ? kvk_pipe_create(att_scores_kv16_spv, sizeof att_scores_kv16_spv, 3, 40, NULL, NULL, 0)
                          : kvk_pipe_create(att_scores_kv32_spv, sizeof att_scores_kv32_spv, 3, 40, NULL, NULL, 0);
        vkb.sm = vkg.kv16 ? kvk_pipe_create(att_softmax_kv16_spv, sizeof att_softmax_kv16_spv, 1, 16, NULL, NULL, 0)
                          : kvk_pipe_create(att_softmax_kv32_spv, sizeof att_softmax_kv32_spv, 1, 16, NULL, NULL, 0);
        vkb.mix = vkg.kv16 ? kvk_pipe_create(att_mix_kv16_spv, sizeof att_mix_kv16_spv, 3, 32, NULL, NULL, 0)
                           : kvk_pipe_create(att_mix_kv32_spv, sizeof att_mix_kv32_spv, 3, 32, NULL, NULL, 0);
    }
    if (!vkb.rms16.p || !vkb.qkv.p || !vkb.cvt.p || !vkb.gat.p || (vkb.flash ? !vkb.flash_p.p : (!vkb.sc.p || !vkb.sm.p || !vkb.mix.p))) {
        fprintf(stderr, "Vulkan: pipeline do lote falhou (%s): prompt token a token\n", kvk_error()); vkg.P = 0; return 0;
    }
    vkb.skip = getenv("KATANA_VK_SKIP") ? (unsigned)strtoul(getenv("KATANA_VK_SKIP"), NULL, 0) : 0;
    vkb.ok = 1;
    return 1;
}

/* fases (bits de KATANA_VK_SKIP, so para medir): 1 norma, 2 q/k/v, 4 qkv_post, 8 atencao, 16 saida, 32 gate/up, 128 down, 256 cabeca (e poda da ultima camada) */
#define VKS(bit) (!(vkb.skip & (bit)))
static int vk_emit_n(const Model *m, int B, int pos0, int head) {
    const Config *c = &m->cfg;
    const int d = c->dim, hd = c->hd, H = c->hidden, kvd = c->n_kv * hd, qd = c->n_heads * hd, nh = c->n_heads;
    const struct { int n; float eps; int B; } rp = { d, c->eps, B };
    int ok = 1, pruned = 0;
    for (int l = 0; l < c->n_layers && ok; l++) {
        const Layer *L = &m->layers[l];
#define OR_DUMMY(v) ((v).b ? (v) : vkg.dummy)
        if (VKS(1)) { const KvkBuf rb[3] = { vkb.x, vkg.an[l], vkb.xh }; ok &= kvk_dispatch(vkb.rms16, rb, 3, &rp, sizeof rp, (uint32_t)((B + 7) / 8), 1, 1); }
        if (VKS(2)) {
            ok &= vk_mm(&L->wq, vkb.xh, vkb.q, qd, d, B, 0, 0);
            ok &= vk_mm(&L->wk, vkb.xh, vkb.k, kvd, d, B, 0, 0);
            ok &= vk_mm(&L->wv, vkb.xh, vkb.v, kvd, d, B, 0, 0);
        }
        if (VKS(4)) {
            const KvkBuf qb[12] = { vkb.q2, vkb.k, vkb.v, OR_DUMMY(vkg.bq[l]), OR_DUMMY(vkg.bk[l]), OR_DUMMY(vkg.bv[l]), vkg.kc[l], vkg.vc[l],
                                    vkg.freq, vkb.q, OR_DUMMY(vkg.qn[l]), OR_DUMMY(vkg.kn[l]) };
            const struct { int base, pos0, hd, neox, has_bias, d, kvd, norm, B; float eps; } qp =
                { 0, pos0, hd, c->rope_neox, (vkg.bq[l].b ? 1 : 0) | (vkg.bk[l].b ? 2 : 0) | (vkg.bv[l].b ? 4 : 0), qd, kvd, c->qk_norm, B, c->eps };
            ok &= kvk_dispatch(vkb.qkv, qb, 12, &qp, sizeof qp, (uint32_t)((B * (qd / 2 + kvd / 2 + kvd) + 127) / 128), 1, 1);
        }
        if (VKS(8)) {
            const float scale = 1.0f / sqrtf((float)hd);
            if (vkb.flash) {
                const KvkBuf ab[4] = { vkb.q2, vkg.kc[l], vkg.vc[l], vkb.xh };
                const struct { int base, pos0, kvd, nkv, B; float scale; } ap = { 0, pos0, kvd, c->n_kv, B, scale };
                ok &= kvk_dispatch(vkb.flash_p, ab, 4, &ap, sizeof ap, (uint32_t)(B * c->n_kv), 1, 1);
            } else {
                const int tmax = pos0 + B;
                const KvkBuf sb[3] = { vkb.q2, vkg.kc[l], vkb.att3 };
                const struct { int base, pos0, tmax, ctx, hd, kvd, group, nh, B; float scale; } sp = { 0, pos0, tmax, c->ctx, hd, kvd, nh / c->n_kv, nh, B, scale };
                ok &= kvk_dispatch(vkb.sc, sb, 3, &sp, sizeof sp, (uint32_t)(((size_t)B * nh * tmax + 127) / 128), 1, 1);
                const struct { int pos0, ctx, nh, B; } mp = { pos0, c->ctx, nh, B };
                ok &= kvk_dispatch(vkb.sm, &vkb.att3, 1, &mp, sizeof mp, (uint32_t)((B * nh + 127) / 128), 1, 1);
                const KvkBuf xb[3] = { vkb.att3, vkg.vc[l], vkb.o3 };
                const struct { int base, pos0, ctx, hd, kvd, group, nh, B; } xp = { 0, pos0, c->ctx, hd, kvd, nh / c->n_kv, nh, B };
                ok &= kvk_dispatch(vkb.mix, xb, 3, &xp, sizeof xp, (uint32_t)(((size_t)B * qd + 127) / 128), 1, 1);
                const uint32_t n4 = (uint32_t)((size_t)B * qd / 4);
                const KvkBuf cb[3] = { vkb.o3, vkb.o3, vkb.xh };
                ok &= kvk_dispatch(vkb.cvt, cb, 3, &n4, 4, (n4 + 255) / 256, 1, 1);
            }
        }
        if (VKS(16)) ok &= vk_mm(&L->wo, vkb.xh, vkb.x, d, qd, B, 1, 0);
        /* ultima camada com logits: so o ultimo token precisa do feed-forward; a coluna B-1 vai para vkg.x (vetor de um token) */
        int Bf = B;
        KvkBuf xf = vkb.x;
        if (head && l == c->n_layers - 1 && VKS(256)) {
            const KvkBuf gb[2] = { vkb.x, vkg.x };
            const int gp[3] = { d, B, B - 1 };
            ok &= kvk_dispatch(vkb.gat, gb, 2, gp, sizeof gp, (uint32_t)((d + 127) / 128), 1, 1);
            Bf = 1; xf = vkg.x; pruned = 1;
        }
        const struct { int n; float eps; int B; } rpf = { d, c->eps, Bf };
        if (VKS(1)) { const KvkBuf rb2[3] = { xf, vkg.fn[l], vkb.xh }; ok &= kvk_dispatch(vkb.rms16, rb2, 3, &rpf, sizeof rpf, (uint32_t)((Bf + 7) / 8), 1, 1); }
        if (VKS(32)) ok &= vk_mm(&L->w_gate, vkb.xh, vkb.hb, H, d, Bf, 0, 1);     /* saida em fp16 (silu(gate)*up) */
        if (VKS(128)) ok &= vk_mm(&L->w_down, vkb.hb, xf, d, H, Bf, 1, 0);
#undef OR_DUMMY
    }
    if (ok && head && VKS(256)) {
        if (!pruned) {      /* so com a poda desligada (KATANA_VK_SKIP): copia a ultima coluna */
            const KvkBuf gb[2] = { vkb.x, vkg.x };
            const int gp[3] = { d, B, B - 1 };
            ok &= kvk_dispatch(vkb.gat, gb, 2, gp, sizeof gp, (uint32_t)((d + 127) / 128), 1, 1);
        }
        ok &= vk_emit_head(m);
    }
    return ok;
}

/* B tokens (B <= vkb.P) a partir de pos0 pelas camadas na GPU; logits do ultimo token se pedido */
static float *vk_forward_n(Model *m, const int *toks, int B, int pos0, int want_logits) {
    const Config *c = &m->cfg;
    const int d = c->dim;
    if (B < 1 || B > vkb.P || pos0 < 0 || pos0 + B > c->ctx) die("lote fora do limite (Vulkan)");
    double t0 = now_sec();
    for (int t = 0; t < B; t++) {
        mat_row(m->x, &m->tok_emb, toks[t], d);
        for (int j = 0; j < d; j++) vkb.xT[(size_t)j * B + t] = m->x[j];
    }
    if (!kvk_buf_write_q(vkb.x, 0, vkb.xT, (size_t)d * B * sizeof(float))) die("falha na execucao em Vulkan (escrita do lote)");
    double t1 = now_sec();
    if (!vk_emit_n(m, B, pos0, want_logits) || !kvk_flush()) die("falha na execucao em Vulkan (lote)");
    double t2 = now_sec();
    vkb.t_emb += t1 - t0; vkb.t_emit += t2 - t1; vkb.passes++; vkb.toks += B;
    if (!want_logits) return NULL;
    if (!kvk_buf_read(vkg.lg, 0, m->logits, (size_t)c->vocab * sizeof(float))) die("falha na execucao em Vulkan (leitura)");
    vkb.t_wait += now_sec() - t2;
    return m->logits;
}

/* inicia o Vulkan e sobe os pesos (mesmo empacotador e mesmo orcamento do OpenCL); 0 = falhou, usar OpenCL */
static int vk_setup(Model *m) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd, V = c->vocab, qd = (size_t)c->n_heads * c->hd;
    KvkInfo I;
    if (c->n_expert && !getenv("KATANA_VK_MOE")) {
        fprintf(stderr, "Vulkan: modelos MoE ainda nao rodam em Vulkan, usando OpenCL/CPU\n");
        return 0;
    }
    if (!kvk_init(&I)) { fprintf(stderr, "Vulkan: indisponivel (%s)\n", kvk_error()); return 0; }
    gpu.vk = 1;
    vkg.sg32 = I.subgroup_size == 32;
    vkg.coop = 0;      /* produto 16x16x16 fp16 x fp16 -> fp32 no escopo de subgrupo (VK_COMPONENT_TYPE_FLOAT16 = 0, FLOAT32 = 1, SCOPE_SUBGROUP = 3) */
    if (I.coopmat && I.fp16 && I.storage16 && I.max_shared >= 32768)
        for (int i = 0; i < I.n_coop; i++)
            if (I.coop[i].m == 16 && I.coop[i].n == 16 && I.coop[i].k == 16 && I.coop[i].a_type == 0 && I.coop[i].b_type == 0
                && I.coop[i].c_type == 1 && I.coop[i].r_type == 1 && I.coop[i].scope == 3) vkg.coop = 1;
    if (!vkg.coop) fprintf(stderr, "Vulkan: sem matriz cooperativa fp16 16x16x16: prompt token a token\n");
    snprintf(gpu.name, sizeof gpu.name, "%s", I.name);
    gpu.vram = I.vram_bytes;
    gpu.seg_target = getenv("KATANA_SEG") ? (size_t)atoll(getenv("KATANA_SEG")) : 16384;
    size_t maxc = d > H ? d : H;
    if (qd > maxc) maxc = qd;
    const size_t maxr = V > maxc ? V : maxc;
    gpu.vxb = kvk_buf_create(maxc * sizeof(float), KVK_USAGE_RW);
    gpu.vob = kvk_buf_create((maxr + 128) * sizeof(float), KVK_USAGE_RW);
    if (!gpu.vxb.b || !gpu.vob.b) { fprintf(stderr, "Vulkan: sem memoria para os buffers (%s)\n", kvk_error()); kvk_shutdown(); gpu.vk = 0; return 0; }
    gpu.used = (maxc + maxr + 128) * sizeof(float);
    const uint64_t budget = gpu_budget();
    const int graph = vk_plan(m, budget);       /* 1 = o grafo completo cabe */
    int nl = 0;
    for (int l = 0; l < c->n_layers; l++) {
        Layer *L = &m->layers[l];
        const size_t ffn = gpu_mat_bytes(&L->w_gate, H, d) + gpu_mat_bytes(&L->w_up, H, d) + gpu_mat_bytes(&L->w_down, d, H);
        const size_t need = gpu_mat_bytes(&L->wq, qd, d) + gpu_mat_bytes(&L->wk, kvd, d) + gpu_mat_bytes(&L->wv, kvd, d)
                          + gpu_mat_bytes(&L->wo, d, qd) + (c->n_expert ? 0 : ffn);
        if (gpu.used + need > budget) break;
        char nm[32];
        snprintf(nm, sizeof nm, "L%03d.wq", l);   gpu_upload(&L->wq, qd, d, nm);
        snprintf(nm, sizeof nm, "L%03d.wk", l);   gpu_upload(&L->wk, kvd, d, nm);
        snprintf(nm, sizeof nm, "L%03d.wv", l);   gpu_upload(&L->wv, kvd, d, nm);
        snprintf(nm, sizeof nm, "L%03d.wo", l);   gpu_upload(&L->wo, d, qd, nm);
        if (!c->n_expert) {
            gpu_upload_glu(&L->w_gate, &L->w_up, H, d, l);
            snprintf(nm, sizeof nm, "L%03d.down", l); gpu_upload(&L->w_down, d, H, nm);
        }
        nl++;
    }
    if (!m->lm_head.gbuf && nl == c->n_layers && gpu.used + gpu_mat_bytes(&m->lm_head, V, d) <= budget) gpu_upload(&m->lm_head, V, d, "head");
    kvk_wait();
    gpu.n_gl = 0; gpu.on = 0; gpu.full = 0;      /* o grafo OpenCL nao e usado; vkg.on liga o grafo Vulkan */
    if (graph) {
        gpu.kv16 = vkg.kv16;
        if (nl == c->n_layers && vk_graph_build(m)) {
            const char *r = getenv("KATANA_VK_REC");
            vkg.rec = r ? atoi(r) != 0 : 1;
        } else fprintf(stderr, "Vulkan: nao cabe (pesos ou buffers do grafo falharam): grafo na CPU\n");
    }
    fprintf(stderr, "GPU: Vulkan %s (%.1f GB) - %d de %d camadas na VRAM, %.0f MB usados em %d buffers%s - %s\n",
            gpu.name, gpu.vram / 1073741824.0, nl, c->n_layers, gpu.used / 1048576.0, gpu.vk_nbuf,
            vkg.on && vkg.kv16 ? ", cache KV em 16 bits" : "",
            vkg.on ? (vkg.rec ? "grafo completo em Vulkan (gravado)" : "grafo completo em Vulkan (ao vivo)") : "Vulkan: grafo na CPU");
    if (gpu.n_shard) fprintf(stderr, "GPU: %d buffers lidos dos shards em %s\n", gpu.n_shard, shards.dir);
    return 1;
}

/* cria os buffers de trabalho do grafo (sem o cache KV); devolve 0 se algo nao couber */
static int gpu_setup_bufs(Model *m) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd, V = c->vocab;
    const size_t ctx = c->ctx, L = c->n_layers, B = GPU_BMAX;
    int fail = 0;
    const size_t qd = (size_t)c->n_heads * c->hd, dq = qd > d ? qd : d;
    gpu.bx = gbuf_rw(d * B * 4);    gpu.bxb = gbuf_rw(dq * B * 4);  gpu.bq = gbuf_rw(dq * B * 4);
    gpu.bk = gbuf_rw(kvd * B * 4);  gpu.bv = gbuf_rw(kvd * B * 4);  gpu.bhb = gbuf_rw(H * B * 4);
    gpu.blog = gbuf_rw(V * 4);      gpu.batt = gbuf_rw(B * c->n_heads * ctx * 4);
    if (!gpu.bx || !gpu.bxb || !gpu.bq || !gpu.bk || !gpu.bv || !gpu.bhb || !gpu.blog || !gpu.batt) return 0;
    gpu.bfreq = gbuf_f(m->rope_freq, c->hd / 2, &fail);
    gpu.bon = gbuf_f(m->out_norm, d, &fail);
    gpu.an = xalloc(L, sizeof(clh));  gpu.fn = xalloc(L, sizeof(clh));
    gpu.lbq = xalloc(L, sizeof(clh)); gpu.lbk = xalloc(L, sizeof(clh));
    gpu.lbv = xalloc(L, sizeof(clh)); gpu.lbo = xalloc(L, sizeof(clh));
    gpu.xT = xalloc(d * B, sizeof(float));
    gpu.blog4 = gbuf_rw(V * 4 * 4);     /* opcional: so a decodificacao especulativa usa */
    gpu.logits_all = xalloc(V * 4, sizeof(float));
    return !fail && gpu.bfreq && gpu.bon;
}

/* bytes que uma matriz ocupa na VRAM no nosso layout */
static size_t gpu_mat_bytes(const Mat *W, size_t rows, size_t cols) {
    return ((rows + 127) & ~(size_t)127) * gpu_wpr(W, cols) * 4;
}

/* katana.cfg fica ao lado do executavel e guarda o ajuste medido por `katana tune` para a
   placa desta maquina; so vale se o nome da GPU for o mesmo. Variaveis de ambiente ganham. */
static void cfg_path(char *out, size_t n) {
#ifdef _WIN32
    char exe[1024];
    DWORD l = GetModuleFileNameA(NULL, exe, sizeof exe);
    while (l > 0 && exe[l - 1] != '\\' && exe[l - 1] != '/') l--;
    exe[l] = 0;
    snprintf(out, n, "%skatana.cfg", exe);
#else
    snprintf(out, n, "katana.cfg");
#endif
}

static void gpu_cfg_load(void) {
    char fn[1100], line[256], name[128] = {0};
    long long seg = -1; int rows = 0, rowsb = 0;
    cfg_path(fn, sizeof fn);
    FILE *f = fopen(fn, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "gpu=", 4) == 0) { strncpy(name, line + 4, sizeof name - 1); name[strcspn(name, "\r\n")] = 0; }
        else if (sscanf(line, "seg=%lld", &seg) == 1) continue;
        else if (sscanf(line, "rows=%d", &rows) == 1) continue;
        else sscanf(line, "rowsb=%d", &rowsb);
    }
    fclose(f);
    if (strcmp(name, gpu.name) != 0) return;
    if (seg >= 0) gpu.cfg_seg = (size_t)seg;
    if (rows == 1 || rows == 2) gpu.cfg_rows = rows;
    if (rowsb == 1 || rowsb == 2) gpu.cfg_rowsb = rowsb;
}

/* Decide o que vai para a VRAM. As camadas sobem em ordem, inteiras, enquanto couberem
   (pesos + cache KV da camada). Se todas couberem, a saida (lm_head) tambem sobe e o token
   inteiro roda na GPU; senao as primeiras n_gl camadas rodam na GPU e o resto na CPU, com
   uma unica transferencia do estado por token. KATANA_GPU_LAYERS limita n_gl. */
static void gpu_setup(Model *m) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd, V = c->vocab;
    const size_t ctx = c->ctx, qd = (size_t)c->n_heads * c->hd;
    size_t kv_layer = 2 * ctx * kvd * sizeof(float);
    gpu.cfg_rows = 1; gpu.cfg_rowsb = 2; gpu.cfg_seg = 16384;       /* padroes medidos numa RTX 3050 */
    {   const char *be = getenv("KATANA_BACKEND");
        if (be && strcmp(be, "vulkan") == 0) {
            if (vk_setup(m)) return;
            fprintf(stderr, "Vulkan: falhou, tentando OpenCL\n");
        } else if (be && strcmp(be, "opencl") != 0) fprintf(stderr, "KATANA_BACKEND=%s desconhecido (use opencl ou vulkan), usando OpenCL\n", be);
    }
    if (!gpu_init()) { fprintf(stderr, "GPU: nenhuma disponivel via OpenCL, usando CPU\n"); return; }
    gpu_cfg_load();     /* ajuste desta placa, se `katana tune` ja rodou */
    const uint64_t vram_budget = gpu_budget();
    const size_t NX = c->n_expert ? (size_t)c->n_expert : 1;
    {   /* se o modelo nao cabe inteiro com o cache KV em float32, o cache vai em float16 */
        size_t total = gpu_mat_bytes(&m->lm_head, V, d) + ((size_t)256 << 20);
        for (int l = 0; l < c->n_layers; l++) {
            const Layer *L = &m->layers[l];
            total += gpu_mat_bytes(&L->wq, qd, d) + gpu_mat_bytes(&L->wk, kvd, d) + gpu_mat_bytes(&L->wv, kvd, d)
                   + gpu_mat_bytes(&L->wo, d, qd) + NX * (gpu_mat_bytes(&L->w_gate, H, d) + gpu_mat_bytes(&L->w_up, H, d)
                   + gpu_mat_bytes(&L->w_down, d, H)) + kv_layer;
        }
        gpu.kv16 = getenv("KATANA_KV16") ? atoi(getenv("KATANA_KV16")) != 0 : total > vram_budget;
        if (gpu.kv16) kv_layer /= 2;
    }
    if (!gpu_build_all()) { fprintf(stderr, "GPU: falha ao preparar os kernels, usando CPU\n"); return; }

    size_t maxc = d > H ? d : H;
    if (qd > maxc) maxc = qd;
    size_t maxr = V > maxc ? V : maxc;
    int32_t e1 = 0, e2 = 0, e4 = 0;
    gpu.xbuf = cl.CreateBuffer(gpu.ctx, 1, maxc * sizeof(float), NULL, &e1);
    gpu.obuf = cl.CreateBuffer(gpu.ctx, 1, (maxr + 128) * sizeof(float), NULL, &e2);
    gpu.seg_target = getenv("KATANA_SEG") ? (size_t)atoll(getenv("KATANA_SEG")) : gpu.cfg_seg;
    gpu.bsx = cl.CreateBuffer(gpu.ctx, 1, (maxc / 32 + 1) * GPU_BMAX * sizeof(float), NULL, &e4);
    if (e1 || e2 || e4 || !gpu_setup_bufs(m)) { fprintf(stderr, "GPU: sem memoria para os buffers, usando CPU\n"); return; }

    if (c->qk_norm) {
        gx.bqn = gbuf_rw(qd * GPU_BMAX * 4); gx.bkn = gbuf_rw(kvd * GPU_BMAX * 4);
        gx.qn = xalloc(c->n_layers, sizeof(clh)); gx.kn = xalloc(c->n_layers, sizeof(clh));
        if (!gx.bqn || !gx.bkn) { fprintf(stderr, "GPU: sem memoria para os buffers, usando CPU\n"); return; }
    }
    int max_layers = getenv("KATANA_GPU_LAYERS") ? atoi(getenv("KATANA_GPU_LAYERS")) : c->n_layers;
    if (max_layers > c->n_layers) max_layers = c->n_layers;
    int n_gl = 0, fail = 0;
    /* KATANA_HEAD_GPU=1: a projecao final sobe antes das camadas. Medido no 7B em 4 GB: ela
       desloca 3 camadas e o conjunto fica mais lento, por isso nao e o padrao. */
    if (gpu.kv16 && getenv("KATANA_HEAD_GPU")) gpu_upload(&m->lm_head, V, d, "head");
    for (int l = 0; l < max_layers; l++) {
        Layer *L = &m->layers[l];
        const size_t ffn = gpu_mat_bytes(&L->w_gate, H, d) + gpu_mat_bytes(&L->w_up, H, d) + gpu_mat_bytes(&L->w_down, d, H);
        /* MoE: a camada so reserva os especialistas de um token; o resto e cache */
        const size_t need = gpu_mat_bytes(&L->wq, qd, d) + gpu_mat_bytes(&L->wk, kvd, d) + gpu_mat_bytes(&L->wv, kvd, d)
                          + gpu_mat_bytes(&L->wo, d, qd) + (c->n_expert ? 0 : ffn) + kv_layer;
        if (gpu.used + need > vram_budget || (size_t)(l + 1) * ctx * kvd > 0x7fffffff) break;
        char nm[32];
        snprintf(nm, sizeof nm, "L%03d.wq", l);   gpu_upload(&L->wq, qd, d, nm);
        snprintf(nm, sizeof nm, "L%03d.wk", l);   gpu_upload(&L->wk, kvd, d, nm);
        snprintf(nm, sizeof nm, "L%03d.wv", l);   gpu_upload(&L->wv, kvd, d, nm);
        snprintf(nm, sizeof nm, "L%03d.wo", l);   gpu_upload(&L->wo, d, qd, nm);
        if (!c->n_expert) {
            gpu_upload_glu(&L->w_gate, &L->w_up, H, d, l);
            snprintf(nm, sizeof nm, "L%03d.down", l); gpu_upload(&L->w_down, d, H, nm);
            if (!L->w_gate.gbuf || !L->w_up.gbuf || !L->w_down.gbuf) break;
        }
        if (!L->wq.gbuf || !L->wk.gbuf || !L->wv.gbuf || !L->wo.gbuf) break;
        if (c->qk_norm) { gx.qn[l] = gbuf_f(L->q_norm, c->qk_norm == 1 ? d : (size_t)c->hd, &fail); gx.kn[l] = gbuf_f(L->k_norm, c->qk_norm == 1 ? kvd : (size_t)c->hd, &fail); }
        gpu.an[l] = gbuf_f(L->attn_norm, d, &fail);  gpu.fn[l] = gbuf_f(L->ffn_norm, d, &fail);
        gpu.lbq[l] = gbuf_f(L->bq, qd, &fail);        gpu.lbk[l] = gbuf_f(L->bk, kvd, &fail);
        gpu.lbv[l] = gbuf_f(L->bv, kvd, &fail);      gpu.lbo[l] = gbuf_f(L->bo, d, &fail);
        if (fail) break;
        gpu.used += kv_layer;       /* reserva o cache KV desta camada */
        n_gl++;
    }
    if (!m->lm_head.gbuf && n_gl == c->n_layers && gpu.used + gpu_mat_bytes(&m->lm_head, V, d) <= vram_budget) gpu_upload(&m->lm_head, V, d, "head");
    if (n_gl > 0) {
        int32_t e5 = 0, e6 = 0;
        gpu.bkc = cl.CreateBuffer(gpu.ctx, 1, (size_t)n_gl * ctx * kvd * (gpu.kv16 ? 2 : 4), NULL, &e5);
        gpu.bvc = cl.CreateBuffer(gpu.ctx, 1, (size_t)n_gl * ctx * kvd * (gpu.kv16 ? 2 : 4), NULL, &e6);
        if (e5 || e6) n_gl = 0;
    }
    if (c->n_expert && n_gl > 0) {
        /* o que sobrou da VRAM vira o cache de especialistas, dividido em vagas iguais por camada
           (KATANA_EXPERT_MB limita o total, para testes) */
        const Layer *L0 = &m->layers[0];
        const int E = c->n_expert, NL = c->n_layers;
        size_t budget = vram_budget > gpu.used ? (size_t)(vram_budget - gpu.used) : 0;
        if (getenv("KATANA_EXPERT_MB")) { const size_t lim = (size_t)atoll(getenv("KATANA_EXPERT_MB")) << 20; if (lim < budget) budget = lim; }
        /* cada camada tem vagas do tamanho dos seus especialistas (a quantizacao varia por camada) */
        gx.bg = xalloc(NL, sizeof(size_t)); gx.bd = xalloc(NL, sizeof(size_t));
        int ok = n_gl == NL && c->n_used <= 16;
        size_t per = 0;
        for (int l = 0; l < NL; l++) {
            const Layer *L = &m->layers[l];
            if (!glu_pair(&L->w_gate, &L->w_up)) ok = 0;
            gx.bg[l] = 2 * gpu_mat_bytes(&L->w_gate, H, d); gx.bd[l] = gpu_mat_bytes(&L->w_down, d, H);
            per += gx.bg[l] + gx.bd[l];
        }
        (void)L0;
        size_t S = budget / per;
        if (S > (size_t)E) S = E;
        if (!ok || S < (size_t)c->n_used) {
            fprintf(stderr, "GPU: MoE nao coube na VRAM (ou gate e up em formatos diferentes), usando CPU\n"); n_gl = 0;
        } else {
            int32_t e1 = 0, e2 = 0, e3 = 0, e4 = 0;
            gx.S = (int)S;
            gx.sg = xalloc(NL, sizeof(clh)); gx.sd = xalloc(NL, sizeof(clh));
            for (int l = 0; l < NL; l++) {
                gx.sg[l] = cl.CreateBuffer(gpu.ctx, 4, S * gx.bg[l], NULL, &e1);
                gx.sd[l] = cl.CreateBuffer(gpu.ctx, 4, S * gx.bd[l], NULL, &e2);
                if (e1 || e2) break;
            }
            if (!e1 && !e2 && !getenv("KATANA_NOTOUCH")) {
                /* a placa so reserva a memoria de um buffer no primeiro uso; preencher agora tira
                   esse custo do caminho do primeiro token */
                const double tt = now_sec();
                const uint32_t zero = 0;
                for (int l = 0; l < NL; l++) {
                    cl.EnqueueFillBuffer(gpu.queue, gx.sg[l], &zero, 4, 0, S * gx.bg[l], 0, NULL, NULL);
                    cl.EnqueueFillBuffer(gpu.queue, gx.sd[l], &zero, 4, 0, S * gx.bd[l], 0, NULL, NULL);
                }
                cl.Finish(gpu.queue);
                if (gpu.debug) fprintf(stderr, "GPU: vagas dos especialistas reservadas em %.2f s\n", now_sec() - tt);
            }
            const size_t RM = (size_t)GPU_BMAX * c->n_used;     /* faixas (token, especialista) por passada */
            gx.bh = cl.CreateBuffer(gpu.ctx, 1, RM * (H + 128) * 4, NULL, &e3);
            gx.bt = cl.CreateBuffer(gpu.ctx, 1, RM * (d + 128) * 4, NULL, &e4);
            if (!e3 && !e4) gx.bmeta = cl.CreateBuffer(gpu.ctx, 1, RM * 8 * 4, NULL, &e3);
            if (!e3 && !e4) gx.bxn = cl.CreateBuffer(gpu.ctx, 1, (size_t)GPU_BMAX * d * 4, NULL, &e4);
            gx.meta = xalloc(RM * 8, sizeof(uint32_t)); gx.w = xalloc(RM, sizeof(float)); gx.rowi = xalloc(RM, sizeof(uint32_t));
            gx.xn = xalloc((size_t)GPU_BMAX * d, sizeof(float));
            if (e1 || e2 || e3 || e4) { fprintf(stderr, "GPU: sem memoria para o cache de especialistas, usando CPU\n"); n_gl = 0; }
            else {
                gx.slot = xalloc((size_t)NL * E, sizeof(ExpSlot));
                for (size_t i = 0; i < (size_t)NL * E; i++) gx.slot[i].slot = -1;
                gx.owner = xalloc((size_t)NL * S, sizeof(int)); gx.stamp = xalloc((size_t)NL * S, sizeof(uint64_t));
                for (size_t i = 0; i < (size_t)NL * S; i++) gx.owner[i] = -1;
                gx.heat = xalloc((size_t)NL * E, sizeof(float));
                gx.cpu_out = xalloc((size_t)GPU_BMAX * c->n_used * d, sizeof(float));
                gx.cpu_cold = !getenv("KATANA_MOE_CPU") || atoi(getenv("KATANA_MOE_CPU")) != 0;
                if (shards.use) {       /* especialistas empacotados por `katana pack`: um arquivo por camada */
                    int n_pk = 0;
                    size_t maxper = 0;
                    gx.pf = xalloc(NL, sizeof(FILE *));
                    for (int l = 0; l < NL; l++) {
                        char nm[32], fn[1100];
                        const size_t want = (size_t)E * (gx.bg[l] + gx.bd[l]);
                        int found = 0;
                        snprintf(nm, sizeof nm, "L%03d.exps", l);
                        for (int i = 0; i < shards.n; i++) if (strcmp(shards.e[i].name, nm) == 0 && shards.e[i].bytes == want) found = 1;
                        if (!found) continue;
                        snprintf(fn, sizeof fn, "%s/%s.bin", shards.dir, nm);
                        gx.pf[l] = fopen(fn, "rb");
                        if (!gx.pf[l]) continue;
                        setvbuf(gx.pf[l], NULL, _IONBF, 0);
                        if (gx.bg[l] + gx.bd[l] > maxper) maxper = gx.bg[l] + gx.bd[l];
                        n_pk++;
                    }
                    if (n_pk) { gx.stage = xalloc(maxper, 1); memset(gx.stage, 1, maxper); }
                    if (n_pk != NL) { for (int l = 0; l < NL; l++) if (gx.pf[l]) { fclose(gx.pf[l]); gx.pf[l] = NULL; } }
                    if (n_pk) fprintf(stderr, "GPU: especialistas de %d camadas direto dos shards em %s\n", n_pk, shards.dir);
                }
                if (gx.cpu_cold && !getenv("KATANA_NOLOADER")) {
                    size_t maxper = 0;
                    for (int l = 0; l < NL; l++) if (gx.bg[l] + gx.bd[l] > maxper) maxper = gx.bg[l] + gx.bd[l];
                    loader_start(m, maxper);
                    gx.swap_margin = getenv("KATANA_SWAP") ? (float)atof(getenv("KATANA_SWAP")) : 4.0f;
                    if (gx.loader) { heat_load(m); preload_feed(m); }
                }
                gx.on = 1;
                fprintf(stderr, "GPU: cache de especialistas: %d de %d por camada na VRAM (%.0f MB)\n", gx.S, E, (double)S * per / 1048576.0);
            }
        }
    }
    gpu.n_gl = n_gl;
    gpu.on = n_gl > 0;
    gpu.full = n_gl == c->n_layers && m->lm_head.gbuf != NULL;
    cl.Finish(gpu.queue);
    fprintf(stderr, "GPU: %s (%.1f GB) - %d de %d camadas na VRAM, %.0f MB usados%s - %s\n",
            gpu.name, gpu.vram / 1073741824.0, n_gl, c->n_layers, gpu.used / 1048576.0, gpu.kv16 ? ", cache KV em 16 bits" : "",
            gpu.full ? "grafo completo na GPU" : n_gl ? "o resto roda na CPU" : "usando CPU");
    if (gpu.n_shard) fprintf(stderr, "GPU: %d buffers lidos dos shards em %s\n", gpu.n_shard, shards.dir);
}

/* Leitura que espera a fila terminar. O padrao e a leitura bloqueante do driver; com
   KATANA_SPIN=1 a leitura entra na fila, a fila e despachada e a thread fica sondando o evento
   (sem dormir), para medir quanto do tempo por token e o despertar da thread. */
static double t_gread;      /* tempo esperando a GPU nas leituras bloqueantes */
static int32_t gread(clh buf, size_t bytes, void *dst) {
    static int spin = -1;
    if (spin < 0) spin = getenv("KATANA_SPIN") ? atoi(getenv("KATANA_SPIN")) : 0;
    if (!spin) {
        const double t0 = now_sec();
        const int32_t e0 = cl.EnqueueReadBuffer(gpu.queue, buf, 1, 0, bytes, dst, 0, NULL, NULL);
        t_gread += now_sec() - t0;
        return e0;
    }
    clh ev = NULL;
    int32_t e = cl.EnqueueReadBuffer(gpu.queue, buf, 0, 0, bytes, dst, 0, NULL, &ev);
    if (e || !ev) return e ? e : -1;
    cl.Flush(gpu.queue);
    int32_t st = 1;
    while (st > 0) {
        if (cl.GetEventInfo(ev, 0x11D3 /* COMMAND_EXECUTION_STATUS */, sizeof st, &st, NULL) != 0) { st = -1; break; }
        if (st > 0) _mm_pause();
    }
    cl.ReleaseEvent(ev);
    return st < 0 ? st : 0;
}

/* ---- pequenos atalhos para montar chamadas de kernel ---- */
static void ka(clh k, uint32_t i, size_t sz, const void *v) { int32_t e = cl.SetKernelArg(k, i, sz, v); if (e && !gerr) fprintf(stderr, "GPU: SetKernelArg(kernel=%p, arg=%u) erro %d\n", k, i, e); gerr |= e; }
static void ka_m(clh k, uint32_t i, clh b) { ka(k, i, sizeof(clh), &b); }
static void ka_i(clh k, uint32_t i, int32_t v) { ka(k, i, sizeof v, &v); }
static void ka_f(clh k, uint32_t i, float v) { ka(k, i, sizeof v, &v); }

static void krun(clh k, size_t n) {
    double t0 = gpu.debug ? now_sec() : 0.0;
    int32_t e = cl.EnqueueNDRangeKernel(gpu.queue, k, 1, NULL, &n, NULL, 0, NULL, NULL);
    if (e && !gerr) fprintf(stderr, "GPU: enqueue(kernel=%p, n=%zu) erro %d\n", k, n, e);
    gerr |= e;
    if (gpu.debug && !gerr) {
        e = cl.Finish(gpu.queue);
        if (e) fprintf(stderr, "GPU: kernel=%p n=%zu falhou na execucao, erro %d\n", k, n, e);
        gerr |= e;
        for (int i = 0; i < n_prof; i++) if (prof[i].k == k) {
            prof[i].t += now_sec() - t0; prof[i].calls++; prof[i].elems += prof_elems;
        }
    }
    prof_elems = 0.0;
}

/* NDRange 2D: n0 linhas (multiplo de 32) x n1 segmentos */
static void krun2(clh k, size_t n0, size_t n1) {
    double t0 = gpu.debug ? now_sec() : 0.0;
    size_t gs[2] = { n0, n1 }, ls[2] = { 32, n1 };     /* com segmentos: grupo = 32 linhas x nseg */
    int32_t e = cl.EnqueueNDRangeKernel(gpu.queue, k, 2, NULL, gs, n1 > 1 ? ls : NULL, 0, NULL, NULL);
    if (e && !gerr) fprintf(stderr, "GPU: enqueue2(kernel=%p, n=%zux%zu) erro %d\n", k, n0, n1, e);
    gerr |= e;
    if (gpu.debug && !gerr) {
        e = cl.Finish(gpu.queue);
        if (e) fprintf(stderr, "GPU: kernel=%p n=%zux%zu falhou na execucao, erro %d\n", k, n0, n1, e);
        gerr |= e;
        for (int i = 0; i < n_prof; i++) if (prof[i].k == k) {
            prof[i].t += now_sec() - t0; prof[i].calls++; prof[i].elems += prof_elems;
        }
    }
    prof_elems = 0.0;
}

/* NDRange 1D com tamanho de grupo explicito */
static void krun_local(clh k, size_t n, size_t local) {
    double t0 = gpu.debug ? now_sec() : 0.0;
    int32_t e = cl.EnqueueNDRangeKernel(gpu.queue, k, 1, NULL, &n, &local, 0, NULL, NULL);
    if (e && !gerr) fprintf(stderr, "GPU: enqueue(kernel=%p, n=%zu, grupo=%zu) erro %d\n", k, n, local, e);
    gerr |= e;
    if (gpu.debug && !gerr) {
        e = cl.Finish(gpu.queue);
        if (e) fprintf(stderr, "GPU: kernel=%p n=%zu falhou na execucao, erro %d\n", k, n, e);
        gerr |= e;
        for (int i = 0; i < n_prof; i++) if (prof[i].k == k) {
            prof[i].t += now_sec() - t0; prof[i].calls++; prof[i].elems += prof_elems;
        }
    }
    prof_elems = 0.0;
}

static void prof_report(void) {
    double tot = 0.0;
    if (!gpu.debug) return;
    for (int i = 0; i < n_prof; i++) tot += prof[i].t;
    fprintf(stderr, "perfil GPU (com sincronizacao por kernel; total %.1f ms):\n", tot * 1e3);
    for (int i = 0; i < n_prof; i++) if (prof[i].calls)
        fprintf(stderr, "  %-12s %7ld chamadas %9.1f ms %5.1f%%  %7.1f us/chamada  %6.2f G pesos/s\n",
                prof[i].name, prof[i].calls, prof[i].t * 1e3, 100.0 * prof[i].t / (tot + 1e-12),
                prof[i].t * 1e6 / prof[i].calls, prof[i].elems / (prof[i].t + 1e-12) / 1e9);
}

/* enfileira out = W * x na GPU (modo: 0 grava, 1 soma em out, 2 SwiGLU com o que ja esta em out).
   Matrizes com poucas linhas e muitas colunas sao divididas em segmentos de colunas para
   ocupar a GPU inteira. Em lote (ks->nb > 1), matrizes Q4_K exigem g_bsum(x) antes. */
static void gmv(const KSet *ks, const Mat *W, clh x, clh out, clh bias, int rows, int cols, int mode) {
    const uint32_t kind = gpu_kind(W);
    clh k = kind == T_F32 ? ks->k_f32 : kind == T_Q4_K ? ks->k_q4k : kind == T_Q5_K ? ks->k_q5k : kind == T_Q6_K ? ks->k_q6k : ks->k_q8;
    const size_t rowsP = ((size_t)rows + 127) & ~(size_t)127, threads = rowsP / ks->nr;
    const int nb = kind == T_F32 ? 1 : kind == T_Q8_0 ? cols / 32 : cols / 256;
    const int nseg = gpu_nseg(threads, nb, ks->nb * ks->nr);
    if (bias) mode |= 4;
    ka_m(k, 0, W->gbuf); ka_m(k, 1, x); ka_m(k, 2, out);
    ka_m(k, 3, bias ? bias : x); ka_m(k, 4, gpu.bsx ? gpu.bsx : x);
    ka_i(k, 5, cols); ka_i(k, 6, mode); ka_i(k, 7, rows); ka_i(k, 8, nseg);
    ka_i(k, 9, W->gs); ka_i(k, 10, W->go);
    prof_elems = (double)rows * cols * ks->nb;
    krun2(k, threads, (size_t)nseg);
}

/* out = silu(gate * x) * (up * x) num kernel so, sobre o buffer com gate e up intercalados */
static void gmv_glu(const KSet *ks, const Mat *gate, clh x, clh out, int rows, int cols) {
    const uint32_t kind = gpu_kind(gate);
    clh k = kind == T_F32 ? ks->kp_f32 : kind == T_Q4_K ? ks->kp_q4k : kind == T_Q5_K ? ks->kp_q5k : kind == T_Q6_K ? ks->kp_q6k : ks->kp_q8;
    const size_t rowsP = ((size_t)rows + 127) & ~(size_t)127, threads = 2 * rowsP / (2 * ks->nr);
    const int nb = kind == T_F32 ? 1 : kind == T_Q8_0 ? cols / 32 : cols / 256;
    const int nseg = gpu_nseg(threads, nb, ks->nb * ks->nr * 2);
    ka_m(k, 0, gate->gbuf); ka_m(k, 1, x); ka_m(k, 2, out);
    ka_m(k, 3, x); ka_m(k, 4, gpu.bsx ? gpu.bsx : x);
    ka_i(k, 5, cols); ka_i(k, 6, 8); ka_i(k, 7, rows); ka_i(k, 8, nseg);
    ka_i(k, 9, 1); ka_i(k, 10, 0);
    prof_elems = 2.0 * rows * cols * ks->nb;
    krun2(k, threads, (size_t)nseg);
}

/* modo hibrido: um produto na GPU, com ida e volta */
static void gpu_matvec(float *out, const Mat *W, const float *x, int rows, int cols) {
    if (gpu.vk) { vk_matvec(out, W, x, rows, cols); return; }
    gerr |= cl.EnqueueWriteBuffer(gpu.queue, gpu.xbuf, 0, 0, cols * sizeof(float), x, 0, NULL, NULL);
    gmv(&gpu.set[gpu.n_sets - 1], W, gpu.xbuf, gpu.obuf, NULL, rows, cols, 0);
    gerr |= cl.EnqueueReadBuffer(gpu.queue, gpu.obuf, 1 /* bloqueante */, 0, (size_t)rows * sizeof(float), out, 0, NULL, NULL);
    if (gerr) die("falha na execucao em GPU");
}

/* perfil da CPU (KATANA_DEBUG=1): tempo e pesos por tipo de produto, e preparo das ativacoes */
static struct { double t, w; long n; } cprof[5];
static const char *cprof_name[5] = { "float32", "outros", "Q4_K", "Q6_K", "quantizar x" };

static void cprof_report(void) {
    double tot = 0.0;
    if (!gpu.debug) return;
    for (int i = 0; i < 5; i++) tot += cprof[i].t;
    if (tot == 0.0) return;
    fprintf(stderr, "perfil CPU (produtos; total %.1f ms):\n", tot * 1e3);
    for (int i = 0; i < 5; i++) if (cprof[i].n)
        fprintf(stderr, "  %-12s %7ld chamadas %9.1f ms %5.1f%%  %6.2f G pesos/s\n", cprof_name[i], cprof[i].n,
                cprof[i].t * 1e3, 100.0 * cprof[i].t / tot, cprof[i].w / (cprof[i].t + 1e-12) / 1e9);
}

typedef struct { float *out; const Mat *W; const float *x, *sx; int cols; } MvJob;
static void mv_job(void *c, int i0, int i1) {
    const MvJob *j = c;
    for (int i = i0; i < i1; i++) j->out[i] = dot_row(j->W, (size_t)i, j->x, j->sx, j->cols);
}

/* out[rows] = W[rows][cols] * x[cols] */
static void matvec(float *out, const Mat *W, const float *x, int rows, int cols) {
    float sx[MAX_SX];
    int i;
    if (W->gbuf && gpu.test_mv) { gpu_matvec(out, W, x, rows, cols); return; }
    const double t0 = gpu.debug ? now_sec() : 0.0;
    if (mat_int(W)) {
        xq_prepare(x, cols);
        if (gpu.debug) { cprof[4].t += now_sec() - t0; cprof[4].n++; }
    }
    const double t1 = gpu.debug ? now_sec() : 0.0;
    if (mat_int(W)) ;
    else if (W->q && W->type == T_Q4_K) {
        if (cols / 32 > MAX_SX) die("matriz larga demais");
        block_sums(sx, x, cols);
    }
    if ((long long)rows * cols >= 200000 && pool.n > 1) {
        /* blocos de ~32 mil pesos, e pelo menos 8 por thread para absorver threads atrasadas */
        MvJob job = { out, W, x, sx, cols };
        int chunk = rows / (pool.n * 8), minc = 32768 / cols + 1;
        if (chunk < minc) chunk = minc;
        pool_for(mv_job, &job, rows, chunk);
    } else for (i = 0; i < rows; i++) out[i] = dot_row(W, (size_t)i, x, sx, cols);
    if (gpu.debug) {
        const int k = !W->q ? 0 : W->type == T_Q4_K ? 2 : W->type == T_Q6_K ? 3 : 1;
        cprof[k].t += now_sec() - t1; cprof[k].w += (double)rows * cols; cprof[k].n++;
    }
}

/* copia a linha `row` de uma matriz para out (usado no embedding) */
static void mat_row(float *out, const Mat *W, int row, int cols) {
    if (W->f) memcpy(out, W->f + (size_t)row * cols, cols * sizeof(float));
    else dequant(W->type, W->q + (size_t)row * W->stride, out, cols);
}

static void rmsnorm(float *out, const float *x, const float *g, int n, float eps) {
    float inv = 1.0f / sqrtf(dot(x, x, n) / n + eps);
    for (int i = 0; i < n; i++) out[i] = g[i] * (x[i] * inv);
}

static void softmax(float *x, int n) {
    float mx = x[0], sum = 0.0f;
    for (int i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    for (int i = 0; i < n; i++) { x[i] = expf(x[i] - mx); sum += x[i]; }
    for (int i = 0; i < n; i++) x[i] /= sum;
}

/* ================= forward com o grafo inteiro na GPU ================= */

static void g_rms(const KSet *ks, clh x, clh g, clh out, int n, float eps) {
    clh k = ks->k_rms;
    ka_m(k, 0, x); ka_m(k, 1, g); ka_m(k, 2, out); ka_i(k, 3, n); ka_f(k, 4, eps);
    krun(k, (size_t)n);
}

/* somas de x por sub-bloco, so quando ha lote e alguma das matrizes consumidoras e Q4_K */
static void g_bsum(const KSet *ks, clh x, int cols, const Mat *a, const Mat *b, const Mat *c3) {
    const Mat *ms[3] = { a, b, c3 };
    int need = 0;
    if (ks->nb == 1) return;
    for (int i = 0; i < 3; i++) if (ms[i] && ms[i]->q && (ms[i]->type == T_Q4_K || ms[i]->type == T_Q5_K)) need = 1;
    if (!need) return;
    ka_m(ks->k_bsum, 0, x); ka_m(ks->k_bsum, 1, gpu.bsx);
    krun(ks->k_bsum, (size_t)cols / 32);
}

static void cpu_layers(Model *m, int l0, int l1, int pos);
static float *cpu_head(Model *m);

/* MoE: contagem de uso de cada especialista [camada][especialista] e os escolhidos do ultimo token */
static struct { uint32_t *cnt; uint64_t tokens; float lg[1024]; int sel[64]; float w[64]; uint16_t *trace; int ti; } moe;

/* MoE: o roteador pontua todos os especialistas sobre xb (entrada normalizada do feed-forward);
   so os n_used melhores rodam, cada um pesado pela sua probabilidade (softmax sobre todos).
   Preenche moe.sel e moe.w. */
static void moe_route(Model *m, const Layer *L, int l) {
    const Config *c = &m->cfg;
    const int E = c->n_expert, K = c->n_used;
    matvec(moe.lg, &L->router, m->xb, E, c->dim);
    softmax(moe.lg, E);
    float wsum = 0.0f;
    for (int k = 0; k < K; k++) {
        int best = 0;
        for (int e = 1; e < E; e++) if (moe.lg[e] > moe.lg[best]) best = e;
        moe.sel[k] = best; moe.w[k] = moe.lg[best]; wsum += moe.lg[best];
        moe.lg[best] = -1.0f;
    }
    if (c->moe_norm) for (int k = 0; k < K; k++) moe.w[k] /= (wsum > 6.103515625e-5f ? wsum : 6.103515625e-5f);
    if (moe.cnt) { for (int k = 0; k < K; k++) moe.cnt[(size_t)l * E + moe.sel[k]]++; if (l == 0) moe.tokens++; }
    if (moe.trace) for (int k = 0; k < K; k++) moe.trace[((size_t)moe.ti * c->n_layers + l) * K + k] = (uint16_t)moe.sel[k];
}

/* n pares (entrada, especialista) da camada na CPU: o j-esimo usa a entrada xs[xi[j]*d ..] e o
   especialista ex[j]; outs[j*d ..] recebe a saida. Cada entrada e quantizada uma vez e os produtos
   de todos os pares saem em dois despachos do pool (gate/up, depois down), cada thread apontando
   para a entrada quantizada do par que esta calculando. */
#define MOE_NMAX (GPU_BMAX * 64)
typedef struct { const Mat *eg, *eu, *ed; const int *ex, *xi; int H, d; float *hg, *hu, *outs; XQ *xin, *xh; } MoeJob;
static void moe_job(void *c, int i0, int i1) {
    const MoeJob *q = c;
    for (int i = i0; i < i1; i++) {
        const int j = i / (2 * q->H), r = i % (2 * q->H);
        xq_tls = &q->xin[q->xi[j]];
        if (r < q->H) q->hg[(size_t)j * q->H + r] = dot_row(&q->eg[q->ex[j]], (size_t)r, NULL, NULL, q->d);
        else q->hu[(size_t)j * q->H + r - q->H] = dot_row(&q->eu[q->ex[j]], (size_t)(r - q->H), NULL, NULL, q->d);
    }
    xq_tls = NULL;
}
static void moe_down_job(void *c, int i0, int i1) {
    const MoeJob *q = c;
    for (int i = i0; i < i1; i++) {
        const int j = i / q->d, r = i % q->d;
        xq_tls = &q->xh[j];
        q->outs[i] = dot_row(&q->ed[q->ex[j]], (size_t)r, NULL, NULL, q->H);
    }
    xq_tls = NULL;
}
static void moe_cpu_multi(Model *m, const Layer *L, const int *ex, const int *xi, int n, int n_x, const float *xs, float *outs) {
    static float *hg, *hu;
    static XQ xin[GPU_BMAX], *xh;
    const Config *c = &m->cfg;
    const int d = c->dim, H = c->hidden;
    if (!hg) {
        hg = xalloc((size_t)MOE_NMAX * H, sizeof(float)); hu = xalloc((size_t)MOE_NMAX * H, sizeof(float));
        xh = xalloc(MOE_NMAX, sizeof(XQ));
    }
    if (n <= 0) return;
    if (n > MOE_NMAX || n_x > GPU_BMAX) die("MoE: pares demais num lote");
    if (!mat_int(&L->eg[ex[0]]) || !mat_int(&L->eu[ex[0]]) || !mat_int(&L->ed[ex[0]]) || pool.n <= 1) {
        for (int j = 0; j < n; j++) {       /* caminho exato/float: um a um */
            const float *x = xs + (size_t)xi[j] * d;
            matvec(hg, &L->eg[ex[j]], x, H, d);
            matvec(hu, &L->eu[ex[j]], x, H, d);
            for (int i = 0; i < H; i++) { const float g = hg[i]; hg[i] = g / (1.0f + expf(-g)) * hu[i]; }
            matvec(outs + (size_t)j * d, &L->ed[ex[j]], hg, d, H);
        }
        return;
    }
    for (int b = 0; b < n_x; b++) { xq_tls = &xin[b]; xq_prepare(xs + (size_t)b * d, d); }
    xq_tls = NULL;
    MoeJob job = { L->eg, L->eu, L->ed, ex, xi, H, d, hg, hu, outs, xin, xh };
    int total = n * 2 * H, chunk = total / (pool.n * 8), minc = 32768 / d + 1;
    if (chunk < minc) chunk = minc;
    pool_for(moe_job, &job, total, chunk);
    for (int i = 0; i < n * H; i++) { const float g = hg[i]; hg[i] = g / (1.0f + expf(-g)) * hu[i]; }
    for (int j = 0; j < n; j++) { xq_tls = &xh[j]; xq_prepare(hg + (size_t)j * H, H); }
    xq_tls = NULL;
    total = n * d; chunk = total / (pool.n * 8); minc = 32768 / H + 1;
    if (chunk < minc) chunk = minc;
    pool_for(moe_down_job, &job, total, chunk);
}
/* n especialistas sobre a mesma entrada m->xb */
static void moe_cpu_experts(Model *m, const Layer *L, const int *ex, int n, float *outs) {
    static const int zero[64];
    moe_cpu_multi(m, L, ex, zero, n, 1, m->xb, outs);
}

/* ---- cache de especialistas na VRAM ----
 * Num modelo MoE a atencao de todas as camadas fica fixa na VRAM e o que sobra do orcamento
 * vira um cache de especialistas: cada camada tem S vagas; um especialista ocupa uma vaga na
 * primeira vez que o roteador o escolhe e, quando a camada enche, sai o usado ha mais tempo.
 *   quente = numa vaga da VRAM;  morno = ja convertido para o layout da GPU, na RAM (subir e
 *   so uma copia);  frio = so no arquivo mapeado (precisa converter antes de subir).
 * Com as vagas num buffer so por camada, um unico lancamento roda os especialistas do token. */
/* poe o especialista e da camada l na vaga v (tirando quem estiver la) */
static void expert_load(Model *m, int l, int e, int v) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden;
    Layer *L = &m->layers[l];
    ExpSlot *sl = &gx.slot[(size_t)l * c->n_expert + e];
    int *own = gx.owner + (size_t)l * gx.S;
    const double t0 = now_sec();
    if (gx.pf && gx.pf[l]) {
        /* empacotado: o arquivo ja esta no layout da GPU. Le o especialista para um buffer fixo
           (o cache de arquivos do sistema faz o papel da RAM morna) e copia para a vaga. A fila
           esta vazia neste ponto (a leitura da entrada acabou de esvaziar), entao a copia
           bloqueante nao espera por nada. */
        const size_t per = gx.bg[l] + gx.bd[l];
        if (FSEEK64(gx.pf[l], (int64_t)((size_t)e * per), SEEK_SET) != 0 || fread(gx.stage, 1, per, gx.pf[l]) != per)
            die("MoE: falha ao ler especialista do shard");
        gx.t_read += now_sec() - t0;
        if (own[v] >= 0) { gx.slot[(size_t)l * c->n_expert + own[v]].slot = -1; gx.evict++; }
        own[v] = e; sl->slot = v;
        gerr |= cl.EnqueueWriteBuffer(gpu.queue, gx.sg[l], 1, (size_t)v * gx.bg[l], gx.bg[l], gx.stage, 0, NULL, NULL);
        gerr |= cl.EnqueueWriteBuffer(gpu.queue, gx.sd[l], 1, (size_t)v * gx.bd[l], gx.bd[l], gx.stage + gx.bg[l], 0, NULL, NULL);
        gx.t_up += now_sec() - t0; gx.loads++;
        return;
    }
    if (!sl->down) {            /* frio -> morno: converte para o layout da GPU, uma vez */
        Mat *two[2] = { &L->eg[e], &L->eu[e] }, *dn = &L->ed[e];
        size_t b0 = 0, b1 = 0;
        sl->glu = gpu_build_buf(two, 2, H, d, &b0);
        sl->down = gpu_build_buf(&dn, 1, d, H, &b1);
        if (!sl->glu || !sl->down || b0 != gx.bg[l] || b1 != gx.bd[l]) die("MoE: falha ao converter especialista");
        gx.conv++;
    }
    gx.loads++;
    if (own[v] >= 0) { gx.slot[(size_t)l * c->n_expert + own[v]].slot = -1; gx.evict++; }
    own[v] = e; sl->slot = v;
    /* os blobs mornos nunca sao liberados, entao a copia pode ficar na fila */
    gerr |= cl.EnqueueWriteBuffer(gpu.queue, gx.sg[l], 0, (size_t)v * gx.bg[l], gx.bg[l], sl->glu, 0, NULL, NULL);
    gerr |= cl.EnqueueWriteBuffer(gpu.queue, gx.sd[l], 0, (size_t)v * gx.bd[l], gx.bd[l], sl->down, 0, NULL, NULL);
    gx.t_up += now_sec() - t0;
}

static void pack_job(void *c, int g0, int g1);

/* thread carregadora: atende os pedidos em ordem; nunca toca na GPU nem no pool de threads */
static void loader_do(LoadReq *r) {
    Model *m = gx.m;
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden;
    const int l = r->l, e = r->e;
    if (gx.pf && gx.pf[l]) {
        const size_t per = gx.bg[l] + gx.bd[l];
        if (FSEEK64(gx.pf[l], (int64_t)((size_t)e * per), SEEK_SET) != 0 || fread(r->buf, 1, per, gx.pf[l]) != per)
            die("MoE: falha ao ler especialista do shard");
    } else {    /* sem shards: converte do GGUF (bem mais lento; `katana pack` evita) */
        Layer *L = &m->layers[l];
        Mat *two[2] = { &L->eg[e], &L->eu[e] }, *dn = &L->ed[e];
        const size_t rpH = (H + 127) & ~(size_t)127, rpd = (d + 127) & ~(size_t)127;
        memset(r->buf, 0, gx.bg[l] + gx.bd[l]);
        PackJob j0 = { two, 2, H, d, gpu_wpr(two[0], d), (uint32_t *)r->buf };
        pack_job(&j0, 0, (int)(rpH / 32) * 2);
        PackJob j1 = { &dn, 1, d, H, gpu_wpr(dn, H), (uint32_t *)(r->buf + gx.bg[l]) };
        pack_job(&j1, 0, (int)(rpd / 32));
    }
}
#ifdef _WIN32
static DWORD WINAPI loader_thread(LPVOID a) {
    long tail = 0;
    (void)a;
    for (;;) {
        long h = gx.head;
        while (tail == h) { WaitOnAddress((volatile VOID *)&gx.head, &h, sizeof h, INFINITE); h = gx.head; }
        LoadReq *r = &gx.rq[tail % LQ];
        loader_do(r);
        r->done = 1;
        tail++;
    }
}
#else
static void *loader_thread(void *a) {
    long tail = 0;
    (void)a;
    for (;;) {
        while (tail == gx.head) usleep(200);
        LoadReq *r = &gx.rq[tail % LQ];
        loader_do(r);
        __atomic_store_n(&r->done, 1, __ATOMIC_RELEASE);
        tail++;
    }
    return NULL;
}
#endif
static void loader_start(Model *m, size_t maxper) {
    gx.m = m;
    for (int i = 0; i < LQ; i++) { gx.rq[i].buf = xalloc(maxper, 1); memset(gx.rq[i].buf, 1, maxper); }
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, loader_thread, NULL, 0, NULL);
    if (h) { CloseHandle(h); gx.loader = 1; }
#else
    pthread_t t;
    if (pthread_create(&t, NULL, loader_thread, NULL) == 0) gx.loader = 1;
#endif
}

/* pede a carga do especialista (se couber na fila) */
static void loader_request(int l, int e, int E) {
    ExpSlot *sl = &gx.slot[(size_t)l * E + e];
    if (sl->pending || gx.head - gx.fin_safe >= LQ) return;
    LoadReq *r = &gx.rq[gx.head % LQ];
    r->l = l; r->e = e; r->done = 0;
    sl->pending = 1;
#ifdef _WIN32
    InterlockedIncrement(&gx.head);
    WakeByAddressSingle((PVOID)&gx.head);
#else
    __atomic_add_fetch(&gx.head, 1, __ATOMIC_RELEASE);
#endif
}

/* Memoria de assunto entre sessoes: o calor de cada especialista e gravado ao sair e, na
   partida seguinte, os mais quentes de cada camada sao pedidos a thread carregadora antes
   mesmo do primeiro token. */
static void heat_path(char *out, size_t n) { snprintf(out, n, "%s.heat", shards.dir); }
static void heat_save(const Model *m) {
    char fn[1100];
    const int32_t hdr[2] = { m->cfg.n_layers, m->cfg.n_expert };
    if (!gx.on || !gx.heat || getenv("KATANA_NOHEAT")) return;
    heat_path(fn, sizeof fn);
    FILE *f = fopen(fn, "wb");
    if (!f) return;
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(gx.heat, sizeof(float), (size_t)hdr[0] * hdr[1], f);
    fclose(f);
}
static int cmp_pre(const void *a, const void *b) {
    const float x = gx.heat[*(const uint32_t *)a], y = gx.heat[*(const uint32_t *)b];
    return x < y ? 1 : x > y ? -1 : 0;
}
static void heat_load(const Model *m) {
    char fn[1100];
    int32_t hdr[2] = { 0, 0 };
    const int NL = m->cfg.n_layers, E = m->cfg.n_expert;
    if (getenv("KATANA_NOHEAT")) return;
    heat_path(fn, sizeof fn);
    FILE *f = fopen(fn, "rb");
    if (!f) return;
    if (fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == NL && hdr[1] == E
        && fread(gx.heat, sizeof(float), (size_t)NL * E, f) == (size_t)NL * E) {
        /* por camada, os S mais quentes; a lista final vai do mais quente para o mais frio */
        gx.pre = xalloc((size_t)NL * gx.S, sizeof(uint32_t));
        uint32_t *idx = xalloc(E, sizeof(uint32_t));
        for (int l = 0; l < NL; l++) {
            for (int e = 0; e < E; e++) idx[e] = (uint32_t)(l * E + e);
            qsort(idx, E, sizeof(uint32_t), cmp_pre);
            for (int i = 0; i < gx.S; i++) if (gx.heat[idx[i]] > 0.05f) gx.pre[gx.n_pre++] = idx[i];
        }
        free(idx);
        qsort(gx.pre, gx.n_pre, sizeof(uint32_t), cmp_pre);
        for (size_t i = 0; i < (size_t)NL * E; i++) gx.heat[i] *= 0.5f;     /* a sessao nova pode discordar */
    } else memset(gx.heat, 0, (size_t)NL * E * sizeof(float));
    fclose(f);
}
static void loader_request(int l, int e, int E);
static void preload_feed(const Model *m) {
    const int E = m->cfg.n_expert;
    while (gx.i_pre < gx.n_pre && gx.head - gx.fin_safe < LQ) {
        const uint32_t id = gx.pre[gx.i_pre++];
        if (gx.slot[id].slot < 0) loader_request((int)(id / E), (int)(id % E), E);
    }
}

/* instala na VRAM as cargas que ficaram prontas. So e chamada logo depois de uma leitura
   bloqueante: a fila da GPU esta vazia, entao nenhuma vaga esta em uso e as copias anteriores
   (dos buffers dos pedidos) ja terminaram. */
static void loader_install(Model *m) {
    const int E = m->cfg.n_expert;
    gx.fin_safe = gx.fin;
    while (gx.fin < gx.head && gx.rq[gx.fin % LQ].done) {
        LoadReq *r = &gx.rq[gx.fin % LQ];
        const int l = r->l, e = r->e;
        ExpSlot *sl = &gx.slot[(size_t)l * E + e];
        int *own = gx.owner + (size_t)l * gx.S;
        const float *heat = gx.heat + (size_t)l * E;
        int v = -1;
        for (int i = 0; i < gx.S; i++) {
            if (own[i] < 0) { v = i; break; }
            if (v < 0 || heat[own[i]] < heat[own[v]]) v = i;
        }
        sl->pending = 0;
        gx.fin++;
        if (own[v] >= 0 && heat[e] <= heat[own[v]]) continue;      /* esfriou enquanto carregava */
        const double t0 = now_sec();
        if (own[v] >= 0) { gx.slot[(size_t)l * E + own[v]].slot = -1; gx.evict++; }
        own[v] = e; sl->slot = v;
        gerr |= cl.EnqueueWriteBuffer(gpu.queue, gx.sg[l], 0, (size_t)v * gx.bg[l], gx.bg[l], r->buf, 0, NULL, NULL);
        gerr |= cl.EnqueueWriteBuffer(gpu.queue, gx.sd[l], 0, (size_t)v * gx.bd[l], gx.bd[l], r->buf + gx.bg[l], 0, NULL, NULL);
        gx.loads++;
        gx.t_up += now_sec() - t0;
    }
    preload_feed(m);
}

/* Feed-forward MoE da camada l com a atencao na GPU, para os B tokens do lote:
     x_b += soma_k w_k * down_k(silu(gate_k xb_b) * up_k xb_b)
   Cada especialista tem um "calor" (usos recentes, com decaimento). Os que estao na VRAM rodam
   na GPU: cada par (token, especialista) e uma faixa, e todas as faixas do lote saem em um
   lancamento. Um que esta fora roda na CPU, direto do arquivo mapeado, enquanto a GPU faz a
   parte dela; se estiver mais quente que o mais frio dos residentes, a thread carregadora o
   traz. Assim a VRAM converge para os especialistas do assunto sem ficar trocando.
   Os vetores do host usados em copias sem bloqueio (tabelas, pesos, entradas, saidas da CPU)
   so sao reescritos depois da proxima leitura bloqueante, quando a fila ja esvaziou. */
static void gpu_moe_ffn(Model *m, const KSet *ks, const Layer *L, int l) {
    const Config *c = &m->cfg;
    const KSet *k1 = &gpu.set[gpu.n_sets - 1];      /* os especialistas usam os kernels de um token */
    const int d = c->dim, H = c->hidden, K = c->n_used, E = c->n_expert, B = ks->nb;
    static int cpu_e[MOE_NMAX], cpu_x[MOE_NMAX], cpu_row[MOE_NMAX];
    float *heat = gx.heat + (size_t)l * E;
    int *own = gx.owner + (size_t)l * gx.S;
    uint64_t *st = gx.stamp + (size_t)l * gx.S;
    int R = 0, n_cpu = 0;
    /* a entrada desce para a CPU escolher os especialistas */
    if (B == 1) gerr |= gread(gpu.bxb, (size_t)d * sizeof(float), gx.xn);
    else {
        gerr |= gread(gpu.bxb, (size_t)d * B * sizeof(float), gpu.xT);
        for (int b = 0; b < B; b++) for (int i = 0; i < d; i++) gx.xn[(size_t)b * d + i] = gpu.xT[(size_t)i * B + b];
    }
    if (gerr) die("falha na execucao em GPU");
    if (gx.loader) loader_install(m);
    const uint64_t tick = ++gx.clock;
    for (int b = 0; b < B; b++) {
        memcpy(m->xb, gx.xn + (size_t)b * d, (size_t)d * sizeof(float));
        moe_route(m, L, l);
        for (int e = 0; e < E; e++) heat[e] *= 0.98f;
        for (int j = 0; j < K; j++) heat[moe.sel[j]] += 1.0f;
        for (int j = 0; j < K; j++) {
            const int e = moe.sel[j], orow = b * K + j;      /* posicao (token, escolha) */
            int v = gx.slot[(size_t)l * E + e].slot;
            gx.w[orow] = moe.w[j];
            if (v >= 0) gx.hits++;
            else {
                gx.miss++;
                int cand = -1;      /* vaga livre, ou o residente mais frio que nao e deste lote */
                for (int i = 0; i < gx.S; i++) {
                    if (own[i] < 0) { cand = i; break; }
                    if (st[i] == tick) continue;
                    if (cand < 0 || heat[own[i]] < heat[own[cand]]) cand = i;
                }
                if (gx.loader) {    /* roda na CPU agora; se merecer, sobe num proximo token */
                    /* em lote (prompt) so ocupa vaga livre: nao expulsa os especialistas da geracao */
                    if (cand >= 0 && (own[cand] < 0 || (B == 1 && heat[e] > heat[own[cand]] + gx.swap_margin))) loader_request(l, e, E);
                } else if (cand >= 0 && (own[cand] < 0 || !gx.cpu_cold || heat[e] > heat[own[cand]] + 0.5f)) {
                    expert_load(m, l, e, cand); v = cand;
                }
            }
            if (v >= 0) {
                st[v] = tick;
                uint32_t *t = gx.meta + 6 * R;      /* gate/up: (vaga, token, faixa); down: (vaga, faixa, faixa) */
                t[0] = (uint32_t)((size_t)v * (gx.bg[l] / 4 / 32)); t[1] = (uint32_t)b; t[2] = (uint32_t)R;
                t[3] = (uint32_t)((size_t)v * (gx.bd[l] / 4 / 32)); t[4] = (uint32_t)R; t[5] = (uint32_t)R;
                gx.rowi[orow] = (uint32_t)R;        /* a saida fica na linha R de bt */
                R++;
            } else { cpu_e[n_cpu] = e; cpu_x[n_cpu] = b; cpu_row[n_cpu++] = orow; }
        }
    }
    /* saidas da CPU ocupam as linhas de bt logo depois das R da GPU; faixas, pesos e linhas
       sobem numa copia so */
    for (int j = 0; j < n_cpu; j++) gx.rowi[cpu_row[j]] = (uint32_t)(R + j);
    memcpy(gx.meta + 6 * R, gx.w, (size_t)B * K * sizeof(float));
    memcpy(gx.meta + 6 * R + B * K, gx.rowi, (size_t)B * K * sizeof(uint32_t));
    gerr |= cl.EnqueueWriteBuffer(gpu.queue, gx.bmeta, 0, 0, ((size_t)6 * R + 2 * (size_t)B * K) * 4, gx.meta, 0, NULL, NULL);
    const size_t rpH = ((size_t)H + 127) & ~(size_t)127, rpd = ((size_t)d + 127) & ~(size_t)127;
    if (R > 0) {
        if (B > 1) gerr |= cl.EnqueueWriteBuffer(gpu.queue, gx.bxn, 0, 0, (size_t)B * d * sizeof(float), gx.xn, 0, NULL, NULL);
        clh xin = B > 1 ? gx.bxn : gpu.bxb;
        {   /* gate e up das R faixas: bh[r*H + i] = silu(gate) * up */
            const uint32_t kind = gpu_kind(&L->eg[0]);
            clh k = kind == T_F32 ? k1->kp_f32 : kind == T_Q4_K ? k1->kp_q4k : kind == T_Q5_K ? k1->kp_q5k : kind == T_Q6_K ? k1->kp_q6k : k1->kp_q8;
            const size_t tp = rpH / k1->nr, threads = tp * R;
            const int nb = kind == T_F32 ? 1 : kind == T_Q8_0 ? d / 32 : d / 256;
            const int nseg = gpu_nseg(threads, nb, k1->nr * 2);
            ka_m(k, 0, gx.sg[l]); ka_m(k, 1, xin); ka_m(k, 2, gx.bh);
            ka_m(k, 3, xin); ka_m(k, 4, gpu.bsx ? gpu.bsx : xin);
            ka_i(k, 5, d); ka_i(k, 6, 8); ka_i(k, 7, H); ka_i(k, 8, nseg); ka_i(k, 9, 1); ka_i(k, 10, 0);
            ka_m(k, 11, gx.bmeta); ka_i(k, 12, (int)tp); ka_i(k, 13, d); ka_i(k, 14, H); ka_i(k, 15, 0);
            prof_elems = 2.0 * H * d * R;
            krun2(k, threads, (size_t)nseg);
            ka_i(k, 12, 0);
        }
        {   /* down das R faixas: bt[(b*K + j)*d + i] */
            const uint32_t kind = gpu_kind(&L->ed[0]);
            clh k = kind == T_F32 ? k1->k_f32 : kind == T_Q4_K ? k1->k_q4k : kind == T_Q5_K ? k1->k_q5k : kind == T_Q6_K ? k1->k_q6k : k1->k_q8;
            const size_t tp = rpd / k1->nr, threads = tp * R;
            const int nb = kind == T_F32 ? 1 : kind == T_Q8_0 ? H / 32 : H / 256;
            const int nseg = gpu_nseg(threads, nb, k1->nr);
            ka_m(k, 0, gx.sd[l]); ka_m(k, 1, gx.bh); ka_m(k, 2, gx.bt);
            ka_m(k, 3, gx.bh); ka_m(k, 4, gpu.bsx ? gpu.bsx : gx.bh);
            ka_i(k, 5, H); ka_i(k, 6, 0); ka_i(k, 7, d); ka_i(k, 8, nseg); ka_i(k, 9, 1); ka_i(k, 10, 0);
            ka_m(k, 11, gx.bmeta); ka_i(k, 12, (int)tp); ka_i(k, 13, H); ka_i(k, 14, d); ka_i(k, 15, 3);
            prof_elems = (double)H * d * R;
            krun2(k, threads, (size_t)nseg);
            ka_i(k, 12, 0);
        }
    }
    if (n_cpu > 0) {    /* os frios, na CPU, enquanto a GPU trabalha */
        const double t0 = now_sec();
        cl.Flush(gpu.queue);
        moe_cpu_multi(m, L, cpu_e, cpu_x, n_cpu, B, gx.xn, gx.cpu_out);
        gerr |= cl.EnqueueWriteBuffer(gpu.queue, gx.bt, 0, (size_t)R * d * sizeof(float), (size_t)n_cpu * d * sizeof(float), gx.cpu_out, 0, NULL, NULL);
        gx.cpu_runs += n_cpu;
        gx.t_cpu += now_sec() - t0;
    }
    clh k = ks->k_moe_sum;
    ka_m(k, 0, gpu.bx); ka_m(k, 1, gx.bt); ka_m(k, 2, gx.bmeta); ka_i(k, 3, 6 * R); ka_i(k, 4, K); ka_i(k, 5, d); ka_i(k, 6, B);
    krun(k, (size_t)d * B);
}

/* processa ks->nb tokens de uma vez (posicoes pos0, pos0+1, ...); devolve os logits do ultimo */
static float *gpu_forward_n(Model *m, const KSet *ks, const int *toks, int pos0, int want_logits) {
    const Config *c = &m->cfg;
    const int B = ks->nb, d = c->dim, hd = c->hd, H = c->hidden, ctx = c->ctx, nh = c->n_heads;
    const int kvd = c->n_kv * hd, group = nh / c->n_kv, tmax = pos0 + B, qd = nh * hd;
    const float scale = 1.0f / sqrtf((float)hd);
    const KSet *k1 = &gpu.set[gpu.n_sets - 1];

    for (int b = 0; b < B; b++) {       /* embeddings transpostos: xT[coluna*B + token] */
        mat_row(m->x, &m->tok_emb, toks[b], d);
        for (int i = 0; i < d; i++) gpu.xT[(size_t)i * B + b] = m->x[i];
    }
    gerr |= cl.EnqueueWriteBuffer(gpu.queue, gpu.bx, 1, 0, (size_t)d * B * sizeof(float), gpu.xT, 0, NULL, NULL);

    for (int l = 0; l < gpu.n_gl; l++) {
        const Layer *L = &m->layers[l];
        const int base = l * ctx * kvd;
        clh k;

        /* --- atencao --- */
        g_rms(ks, gpu.bx, gpu.an[l], gpu.bxb, d, c->eps);
        g_bsum(ks, gpu.bxb, d, &L->wq, &L->wk, &L->wv);
        const uint32_t kq = gpu_kind(&L->wq), kk = gpu_kind(&L->wk), kv = gpu_kind(&L->wv);
        if (ks->k_mv3 && kq >= T_Q4_K && kk >= T_Q4_K && kv >= T_Q4_K && d % 256 == 0) {
            /* q, k e v num lancamento so */
            const size_t rpq = ((size_t)qd + 127) & ~(size_t)127, rpk = ((size_t)kvd + 127) & ~(size_t)127;
            const int nseg = gpu_nseg(rpq + 2 * rpk, d / 256, 1);
            k = ks->k_mv3;
            ka_m(k, 0, L->wq.gbuf); ka_m(k, 1, L->wk.gbuf); ka_m(k, 2, L->wv.gbuf); ka_m(k, 3, gpu.bxb);
            ka_m(k, 4, c->qk_norm ? gx.bqn : gpu.bq); ka_m(k, 5, c->qk_norm ? gx.bkn : gpu.bk); ka_m(k, 6, gpu.bv);
            ka_i(k, 7, (int)kq); ka_i(k, 8, (int)kk); ka_i(k, 9, (int)kv);
            ka_i(k, 10, (int)rpq); ka_i(k, 11, (int)rpk);
            ka_i(k, 12, qd); ka_i(k, 13, kvd); ka_i(k, 14, kvd); ka_i(k, 15, d); ka_i(k, 16, nseg);
            prof_elems = (double)(qd + 2 * kvd) * d;
            krun2(k, rpq + 2 * rpk, (size_t)nseg);
        } else {
            gmv(ks, &L->wq, gpu.bxb, c->qk_norm ? gx.bqn : gpu.bq, NULL, qd, d, 0);
            gmv(ks, &L->wk, gpu.bxb, c->qk_norm ? gx.bkn : gpu.bk, NULL, kvd, d, 0);
            gmv(ks, &L->wv, gpu.bxb, gpu.bv, NULL, kvd, d, 0);
        }
        /* a norma RMS de q e k (vetor inteiro ou por cabeca) e feita dentro de qkv_post, que le
           q e k crus de bqn/bkn; com bias e norma juntos a soma dos quadrados ignoraria o bias,
           mas nenhuma arquitetura suportada tem os dois */
        if (ks->k_att1f && hd <= 256 && hd % 2 == 0 && !getenv("KATANA_NOATTF")) {
            /* um token: bias/norma, RoPE, gravacao no cache KV e a atencao num lancamento so */
            k = ks->k_att1f;
            ka_m(k, 0, c->qk_norm ? gx.bqn : gpu.bq); ka_m(k, 1, c->qk_norm ? gx.bkn : gpu.bk); ka_m(k, 2, gpu.bv);
            ka_m(k, 3, gpu.lbq[l] ? gpu.lbq[l] : gpu.bq); ka_m(k, 4, gpu.lbk[l] ? gpu.lbk[l] : gpu.bk);
            ka_m(k, 5, gpu.lbv[l] ? gpu.lbv[l] : gpu.bv);
            ka_m(k, 6, gpu.bkc); ka_m(k, 7, gpu.bvc); ka_m(k, 8, gpu.batt); ka_m(k, 9, gpu.bxb);
            ka_m(k, 10, gpu.bfreq); ka_m(k, 11, c->qk_norm ? gx.qn[l] : gpu.bq); ka_m(k, 12, c->qk_norm ? gx.kn[l] : gpu.bq);
            ka_i(k, 13, base); ka_i(k, 14, pos0); ka_i(k, 15, ctx); ka_i(k, 16, hd); ka_i(k, 17, kvd); ka_i(k, 18, group);
            ka_i(k, 19, c->rope_neox); ka_i(k, 20, (gpu.lbq[l] ? 1 : 0) | (gpu.lbk[l] ? 2 : 0) | (gpu.lbv[l] ? 4 : 0));
            ka_i(k, 21, c->qk_norm); ka_i(k, 22, qd); ka_f(k, 23, scale); ka_f(k, 24, c->eps);
            krun_local(k, (size_t)nh * hd, (size_t)hd);
        } else {
        k = ks->k_qkv;      /* bias + RoPE em q (no lugar) e k (no cache), v copiado para o cache */
        ka_m(k, 0, gpu.bq); ka_m(k, 1, c->qk_norm ? gx.bkn : gpu.bk); ka_m(k, 2, gpu.bv);
        ka_m(k, 16, c->qk_norm ? gx.bqn : gpu.bq); ka_m(k, 17, c->qk_norm ? gx.qn[l] : gpu.bq);
        ka_m(k, 18, c->qk_norm ? gx.kn[l] : gpu.bq); ka_i(k, 19, c->qk_norm); ka_f(k, 20, c->eps);
        ka_m(k, 3, gpu.lbq[l] ? gpu.lbq[l] : gpu.bq); ka_m(k, 4, gpu.lbk[l] ? gpu.lbk[l] : gpu.bk);
        ka_m(k, 5, gpu.lbv[l] ? gpu.lbv[l] : gpu.bv);
        ka_m(k, 6, gpu.bkc); ka_m(k, 7, gpu.bvc); ka_m(k, 8, gpu.bfreq);
        ka_i(k, 9, base); ka_i(k, 10, pos0); ka_i(k, 11, hd); ka_i(k, 12, c->rope_neox);
        ka_i(k, 13, (gpu.lbq[l] ? 1 : 0) | (gpu.lbk[l] ? 2 : 0) | (gpu.lbv[l] ? 4 : 0));
        ka_i(k, 14, qd); ka_i(k, 15, kvd);
        krun(k, (size_t)B * (qd / 2 + kvd / 2 + kvd));

        if (ks->k_att1 && hd <= 256) {      /* atencao inteira num lancamento */
            k = ks->k_att1;
            ka_m(k, 0, gpu.bq); ka_m(k, 1, gpu.bkc); ka_m(k, 2, gpu.bvc); ka_m(k, 3, gpu.batt); ka_m(k, 4, gpu.bxb);
            ka_i(k, 5, base); ka_i(k, 6, pos0); ka_i(k, 7, ctx); ka_i(k, 8, hd); ka_i(k, 9, kvd); ka_i(k, 10, group);
            ka_f(k, 11, scale);
            krun_local(k, (size_t)nh * hd, (size_t)hd);
        } else {
            k = ks->k_att_scores;
            ka_m(k, 0, gpu.bq); ka_m(k, 1, gpu.bkc); ka_m(k, 2, gpu.batt);
            ka_i(k, 3, base); ka_i(k, 4, pos0); ka_i(k, 5, tmax); ka_i(k, 6, ctx); ka_i(k, 7, hd);
            ka_i(k, 8, kvd); ka_i(k, 9, group); ka_i(k, 10, nh); ka_f(k, 11, scale);
            krun(k, (size_t)B * nh * tmax);
            k = ks->k_att_softmax;
            ka_m(k, 0, gpu.batt); ka_i(k, 1, pos0); ka_i(k, 2, ctx); ka_i(k, 3, nh);
            krun(k, (size_t)B * nh);
            k = ks->k_att_mix;
            ka_m(k, 0, gpu.batt); ka_m(k, 1, gpu.bvc); ka_m(k, 2, gpu.bxb);
            ka_i(k, 3, base); ka_i(k, 4, pos0); ka_i(k, 5, ctx); ka_i(k, 6, hd); ka_i(k, 7, kvd);
            ka_i(k, 8, group); ka_i(k, 9, nh);
            krun(k, (size_t)B * qd);
        }
        }
        g_bsum(ks, gpu.bxb, qd, &L->wo, NULL, NULL);
        gmv(ks, &L->wo, gpu.bxb, gpu.bx, gpu.lbo[l], d, qd, 1);           /* x += Wo*att (+bias) */

        /* --- feed-forward SwiGLU --- */
        g_rms(ks, gpu.bx, gpu.fn[l], gpu.bxb, d, c->eps);
        if (c->n_expert) {
            gpu_moe_ffn(m, ks, L, l);
            continue;
        }
        g_bsum(ks, gpu.bxb, d, &L->w_gate, &L->w_up, NULL);
        if (L->w_gate.pair) gmv_glu(ks, &L->w_gate, gpu.bxb, gpu.bhb, H, d);    /* hb = silu(gate) * up */
        else {
            gmv(ks, &L->w_gate, gpu.bxb, gpu.bhb, NULL, H, d, 0);        /* hb = gate */
            gmv(ks, &L->w_up, gpu.bxb, gpu.bhb, NULL, H, d, 2);          /* hb = silu(hb) * up */
        }
        g_bsum(ks, gpu.bhb, H, &L->w_down, NULL, NULL);
        gmv(ks, &L->w_down, gpu.bhb, gpu.bx, NULL, d, H, 1);             /* x += down */
        { static int fl = -1; if (fl < 0) fl = getenv("KATANA_FLUSH") ? atoi(getenv("KATANA_FLUSH")) : 0; if (fl > 0 && l % fl == fl - 1) cl.Flush(gpu.queue); }
    }

    /* o estado desce uma vez e a CPU faz o que nao esta na VRAM: as camadas restantes e,
       se for o caso, a projecao final */
    if (gpu.n_gl < c->n_layers || (want_logits && !m->lm_head.gbuf)) {
        float *lg = NULL;
        gerr |= gread(gpu.bx, (size_t)d * B * sizeof(float), gpu.xT);
        if (gerr) die("falha na execucao em GPU");
        for (int b = 0; b < B; b++) {
            for (int i = 0; i < d; i++) m->x[i] = gpu.xT[(size_t)i * B + b];
            cpu_layers(m, gpu.n_gl, c->n_layers, pos0 + b);
            if (want_logits && b == B - 1) {
                if (m->lm_head.gbuf) {      /* o estado sobe de novo so para a norma final e a projecao */
                    gerr |= cl.EnqueueWriteBuffer(gpu.queue, gpu.bx, 1, 0, (size_t)d * sizeof(float), m->x, 0, NULL, NULL);
                    g_rms(k1, gpu.bx, gpu.bon, gpu.bxb, d, c->eps);
                    gmv(k1, &m->lm_head, gpu.bxb, gpu.blog, NULL, c->vocab, d, 0);
                    gerr |= gread(gpu.blog, (size_t)c->vocab * sizeof(float), m->logits);
                    if (gerr) die("falha na execucao em GPU");
                    lg = m->logits;
                } else lg = cpu_head(m);
            }
        }
        return lg;
    }
    if (!want_logits) {     /* nada a ler: o trabalho fica na fila e a GPU segue em paralelo */
        if (gerr) die("falha na execucao em GPU");
        return NULL;
    }
    g_rms(ks, gpu.bx, gpu.bon, gpu.bxb, d, c->eps);
    if (gpu.want_all && B == 4 && gpu.blog4) {      /* logits das 4 posicoes, numa passada so */
        g_bsum(ks, gpu.bxb, d, &m->lm_head, NULL, NULL);
        gmv(ks, &m->lm_head, gpu.bxb, gpu.blog4, NULL, c->vocab, d, 0);
        gerr |= gread(gpu.blog4, (size_t)c->vocab * 4 * sizeof(float), gpu.logits_all);
        if (gerr) die("falha na execucao em GPU");
        return gpu.logits_all;
    }
    if (B > 1) {            /* so o ultimo token do lote precisa de logits */
        clh k = ks->k_gather;
        ka_m(k, 0, gpu.bxb); ka_m(k, 1, gpu.bq); ka_i(k, 2, B); ka_i(k, 3, B - 1);
        krun(k, (size_t)d);
    }
    gmv(k1, &m->lm_head, B > 1 ? gpu.bq : gpu.bxb, gpu.blog, NULL, c->vocab, d, 0);
    gerr |= gread(gpu.blog, (size_t)c->vocab * sizeof(float), m->logits);
    if (gerr) die("falha na execucao em GPU");
    return m->logits;
}

/* ================= forward (um token por vez, com KV cache) ================= */

/* Leitura antecipada: enquanto a CPU calcula a camada l, pede ao sistema as paginas da
   camada l+1 do arquivo mapeado (em outra thread, para nao esperar o disco). So liga quando
   o arquivo e grande em relacao a RAM (ou com KATANA_PREFETCH=1): ai as paginas de fato
   saem da memoria entre um token e outro. */
static int prefetch_on;

static void prefetch_setup(int64_t fsz) {
    uint64_t ram = 0;
#ifdef _WIN32
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) ram = ms.ullTotalPhys;
#else
    ram = (uint64_t)sysconf(_SC_PHYS_PAGES) * (uint64_t)sysconf(_SC_PAGE_SIZE);
#endif
    prefetch_on = getenv("KATANA_PREFETCH") ? atoi(getenv("KATANA_PREFETCH")) : (ram && (uint64_t)fsz > ram / 10 * 6);
}

#ifdef _WIN32
typedef struct { WIN32_MEMORY_RANGE_ENTRY r[7]; int n; } Prefetch;
static DWORD WINAPI prefetch_thread(LPVOID a) {
    Prefetch *pf = a;
    PrefetchVirtualMemory(GetCurrentProcess(), (ULONG_PTR)pf->n, pf->r, 0);
    free(pf);
    return 0;
}
#endif

static void layer_prefetch(const Model *m, int l) {
    const Config *c = &m->cfg;
    if (!prefetch_on || l >= c->n_layers) return;
    const Layer *L = &m->layers[l];
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd;
    const Mat *ms[7] = { &L->wq, &L->wk, &L->wv, &L->wo, &L->w_gate, &L->w_up, &L->w_down };
    const size_t rows[7] = { d, kvd, kvd, d, H, H, d };
#ifdef _WIN32
    Prefetch *pf = malloc(sizeof *pf);
    if (!pf) return;
    pf->n = 0;
    for (int i = 0; i < 7; i++) if (ms[i]->q) {
        pf->r[pf->n].VirtualAddress = (PVOID)ms[i]->q;
        pf->r[pf->n].NumberOfBytes = rows[i] * ms[i]->stride;
        pf->n++;
    }
    HANDLE h = pf->n ? CreateThread(NULL, 0, prefetch_thread, pf, 0, NULL) : NULL;
    if (h) CloseHandle(h); else free(pf);
#else
    const size_t pg = (size_t)sysconf(_SC_PAGE_SIZE);
    for (int i = 0; i < 7; i++) if (ms[i]->q) {
        const uintptr_t a = (uintptr_t)ms[i]->q & ~(uintptr_t)(pg - 1);
        madvise((void *)a, (uintptr_t)ms[i]->q + rows[i] * ms[i]->stride - a, MADV_WILLNEED);
    }
#endif
}

/* experimento de esparsidade por assunto (comando `sparsity`): soma |ativacao| de cada
   neuronio do feed-forward e, com mascara, zera os que ficaram fora do conjunto quente */
static struct { int collect; float *sum; uint8_t *mask; double mass_all, mass_kept; int oracle_keep; float *tmp; } sp;
static int cmp_float_desc(const void *a, const void *b);

/* camadas [l0, l1) na CPU sobre o estado m->x, para o token na posicao pos */
static void cpu_layers(Model *m, int l0, int l1, int pos) {
    const Config *c = &m->cfg;
    const int d = c->dim, hd = c->hd, H = c->hidden, ctx = c->ctx;
    const int kvd = c->n_kv * hd, group = c->n_heads / c->n_kv;
    const float scale = 1.0f / sqrtf((float)hd);
    const int step = c->rope_neox ? 1 : 2, gap = c->rope_neox ? hd / 2 : 1;
    const int qd = c->n_heads * hd;

    for (int l = l0; l < l1; l++) {
        const Layer *L = &m->layers[l];
        if (l + 1 < l1) layer_prefetch(m, l + 1);

        /* --- atencao (GQA + RoPE) --- */
        rmsnorm(m->xb, m->x, L->attn_norm, d, c->eps);
        matvec(m->q, &L->wq, m->xb, qd, d);
        matvec(m->k, &L->wk, m->xb, kvd, d);
        matvec(m->v, &L->wv, m->xb, kvd, d);
        if (L->bq) for (int i = 0; i < qd; i++) m->q[i] += L->bq[i];
        if (L->bk) for (int i = 0; i < kvd; i++) m->k[i] += L->bk[i];
        if (L->bv) for (int i = 0; i < kvd; i++) m->v[i] += L->bv[i];

        if (c->qk_norm == 1) {
            rmsnorm(m->q, m->q, L->q_norm, qd, c->eps);
            rmsnorm(m->k, m->k, L->k_norm, kvd, c->eps);
        } else if (c->qk_norm == 2) {
            for (int h = 0; h < c->n_heads; h++) rmsnorm(m->q + h * hd, m->q + h * hd, L->q_norm, hd, c->eps);
            for (int h = 0; h < c->n_kv; h++) rmsnorm(m->k + h * hd, m->k + h * hd, L->k_norm, hd, c->eps);
        }

        /* RoPE: gira pares (2i, 2i+1) no llama, ou (i, i+hd/2) no qwen2 */
        for (int i = 0; i < hd / 2; i++) {
            float ang = pos * m->rope_freq[i], cs = cosf(ang), sn = sinf(ang);
            for (int h = 0; h < c->n_heads; h++) {
                float *p = m->q + h * hd + step * i, a = p[0], b = p[gap];
                p[0] = a * cs - b * sn; p[gap] = a * sn + b * cs;
            }
            for (int h = 0; h < c->n_kv; h++) {
                float *p = m->k + h * hd + step * i, a = p[0], b = p[gap];
                p[0] = a * cs - b * sn; p[gap] = a * sn + b * cs;
            }
        }

        float *kbase = m->kcache + (size_t)l * ctx * kvd;
        float *vbase = m->vcache + (size_t)l * ctx * kvd;
        memcpy(kbase + (size_t)pos * kvd, m->k, kvd * sizeof(float));
        memcpy(vbase + (size_t)pos * kvd, m->v, kvd * sizeof(float));

        for (int h = 0; h < c->n_heads; h++) {
            const float *qh = m->q + h * hd;
            const int kvh = (h / group) * hd;
            for (int t = 0; t <= pos; t++)
                m->att[t] = dot(qh, kbase + (size_t)t * kvd + kvh, hd) * scale;
            softmax(m->att, pos + 1);

            float *out = m->xb + h * hd;
            memset(out, 0, hd * sizeof(float));
            for (int t = 0; t <= pos; t++) {
                const float *vh = vbase + (size_t)t * kvd + kvh;
                const float a = m->att[t];
                for (int i = 0; i < hd; i++) out[i] += a * vh[i];
            }
        }
        matvec(m->xb2, &L->wo, m->xb, d, qd);
        if (L->bo) for (int i = 0; i < d; i++) m->xb2[i] += L->bo[i];
        for (int i = 0; i < d; i++) m->x[i] += m->xb2[i];

        /* --- feed-forward SwiGLU: down(silu(gate(x)) * up(x)) --- */
        rmsnorm(m->xb, m->x, L->ffn_norm, d, c->eps);
        if (c->n_expert) {
            /* MoE: o roteador pontua todos os especialistas; so os n_used melhores rodam,
               cada um pesado pela sua probabilidade (softmax sobre todos) */
            const int K = c->n_used;
            moe_route(m, L, l);
            static float *outs;
            if (!outs) outs = xalloc((size_t)K * d, sizeof(float));
            moe_cpu_experts(m, L, moe.sel, K, outs);
            for (int k = 0; k < K; k++) {
                const float w = moe.w[k];
                const float *o = outs + (size_t)k * d;
                for (int i = 0; i < d; i++) m->x[i] += w * o[i];
            }
            continue;
        }
        matvec(m->hb, &L->w_gate, m->xb, H, d);
        matvec(m->hb2, &L->w_up, m->xb, H, d);
        for (int i = 0; i < H; i++) {
            float g = m->hb[i];
            m->hb[i] = g / (1.0f + expf(-g)) * m->hb2[i];
        }
        if (sp.collect) for (int i = 0; i < H; i++) sp.sum[(size_t)l * H + i] += fabsf(m->hb[i]);
        if (sp.oracle_keep) {   /* experimento: so os N mais ativos DESTE token contribuem */
            for (int i = 0; i < H; i++) sp.tmp[i] = fabsf(m->hb[i]);
            qsort(sp.tmp, H, sizeof(float), cmp_float_desc);
            const float thr = sp.tmp[sp.oracle_keep - 1];
            for (int i = 0; i < H; i++) {
                const double a = fabs((double)m->hb[i]);
                sp.mass_all += a;
                if (a >= thr) sp.mass_kept += a; else m->hb[i] = 0.0f;
            }
        }
        if (sp.mask) {      /* experimento: so os neuronios "quentes" contribuem */
            const uint8_t *mk = sp.mask + (size_t)l * H;
            for (int i = 0; i < H; i++) {
                const double a = fabs((double)m->hb[i]);
                sp.mass_all += a;
                if (mk[i]) sp.mass_kept += a; else m->hb[i] = 0.0f;
            }
        }
        matvec(m->xb2, &L->w_down, m->hb, d, H);
        for (int i = 0; i < d; i++) m->x[i] += m->xb2[i];
    }
}

/* norma final + projecao para o vocabulario, na CPU */
static float *cpu_head(Model *m) {
    const Config *c = &m->cfg;
    rmsnorm(m->x, m->x, m->out_norm, c->dim, c->eps);
    matvec(m->logits, &m->lm_head, m->x, c->vocab, c->dim);
    return m->logits;
}

/* as camadas da GPU so sao usadas pelo grafo da GPU; na CPU os produtos sao sempre locais */
static float *forward(Model *m, int token, int pos, int want_logits) {
    const Config *c = &m->cfg;
    if (vkg.on && !gpu.force_cpu) return vk_forward_1(m, token, pos, want_logits);
    if (gpu.n_gl > 0 && !gpu.force_cpu) return gpu_forward_n(m, &gpu.set[gpu.n_sets - 1], &token, pos, want_logits);
    mat_row(m->x, &m->tok_emb, token, c->dim);
    cpu_layers(m, 0, c->n_layers, pos);
    return want_logits ? cpu_head(m) : NULL;
}

/* Vulkan: menor lote que compensa e maior lote por passada (KATANA_VK_MINB, KATANA_VK_CAP ou KATANA_VK_PASS) */
static int vk_batch_min(void) { return getenv("KATANA_VK_MINB") ? atoi(getenv("KATANA_VK_MINB")) : 2; }
static int vk_pass_cap(void) {
    const int cap = getenv("KATANA_VK_CAP") ? atoi(getenv("KATANA_VK_CAP")) : vkb.P;
    return cap >= 1 && cap < vkb.P ? cap : vkb.P;
}

/* processa n tokens seguidos a partir de pos0 (em lotes, na GPU); logits do ultimo se pedido */
static float *forward_seq(Model *m, const int *toks, int n, int pos0, int want_logits) {
    float *lg = NULL;
    int i = 0;
    if (vkg.on && vkb.ok && !gpu.force_cpu && vk_batch_min() <= n) {
        /* lotes de ate vkb.P tokens; sobra menor que o minimo vai um token por vez */
        while (n - i >= vk_batch_min()) {
            const int nb = n - i < vk_pass_cap() ? n - i : vk_pass_cap();
            lg = vk_forward_n(m, toks + i, nb, pos0 + i, want_logits && i + nb == n);
            i += nb;
        }
        for (; i < n; i++) lg = forward(m, toks[i], pos0 + i, want_logits && i == n - 1);
        return lg;
    }
    if (gpu.n_gl > 0 && !gpu.force_cpu) {
        while (i < n) {
            const KSet *ks = &gpu.set[gpu.n_sets - 1];
            if (!gx.on || !getenv("KATANA_MOE_NOBATCH"))
                for (int s = 0; s < gpu.n_sets; s++) if (gpu.set[s].nb <= n - i) { ks = &gpu.set[s]; break; }
            lg = gpu_forward_n(m, ks, toks + i, pos0 + i, want_logits && i + ks->nb == n);
            i += ks->nb;
        }
        return lg;
    }
    for (; i < n; i++) lg = forward(m, toks[i], pos0 + i, want_logits && i == n - 1);
    return lg;
}

/* ================= amostragem (temperatura + top-k) ================= */

static int sample(const float *logits, int n, float temp) {
    int idx[TOP_K]; float val[TOP_K]; int k = 0;
    if (temp <= 0.0f) {
        int best = 0;
        for (int i = 1; i < n; i++) if (logits[i] > logits[best]) best = i;
        return best;
    }
    for (int i = 0; i < n; i++) {   /* mantem os TOP_K maiores, em ordem decrescente */
        if (k == TOP_K && logits[i] <= val[k - 1]) continue;
        int j = k < TOP_K ? k++ : k - 1;
        while (j > 0 && val[j - 1] < logits[i]) { val[j] = val[j - 1]; idx[j] = idx[j - 1]; j--; }
        val[j] = logits[i]; idx[j] = i;
    }
    for (int i = 0; i < k; i++) val[i] /= temp;
    softmax(val, k);
    float r = (float)rand() / ((float)RAND_MAX + 1.0f), acc = 0.0f;
    for (int i = 0; i < k; i++) { acc += val[i]; if (r < acc) return idx[i]; }
    return idx[k - 1];
}

/* ================= comandos ================= */


static void print_info(const Model *m) {
    const Config *c = &m->cfg;
    fprintf(stderr, "arquitetura=%s  parametros=%.1fM  camadas=%d  dim=%d  cabecas=%d (kv=%d)  ffn=%d  vocab=%d  ctx=%d\n",
            c->arch, m->n_params / 1e6, c->n_layers, c->dim, c->n_heads, c->n_kv, c->hidden, c->vocab, c->ctx);
    if (c->n_expert) fprintf(stderr, "MoE: %d especialistas por camada, %d ativos por token\n", c->n_expert, c->n_used);
}

/* ---------- decodificacao especulativa por repeticao ----------
 * Rascunho gratis: se os ultimos tokens ja apareceram antes no contexto, aposta que a
 * continuacao se repete e propoe os 3 tokens que vieram depois. O modelo confere os 4
 * (o token atual + 3 rascunhos) numa unica passada em lote; cada rascunho so vale se for
 * exatamente o token que o modelo escolheria naquela posicao, entao a saida e a mesma de
 * gerar um por um. Compensa em texto repetitivo (codigo, citacoes, listas); um controle
 * de acerto desliga a aposta quando ela nao esta pagando. KATANA_SPEC=0 desliga. */
static struct { long steps, drafted, accepted; } spec_stat;

/* procura a ocorrencia anterior mais recente dos ultimos K tokens; devolve os 3 seguintes */
static int find_draft(const int *h, int n, int *draft) {
    const int K = 3;
    if (n < K + 4) return 0;
    for (int j = n - K - 1; j >= 0; j--) {
        if (j + K + 3 > n) continue;
        if (h[j] == h[n - K] && h[j + 1] == h[n - K + 1] && h[j + 2] == h[n - K + 2]) {
            draft[0] = h[j + K]; draft[1] = h[j + K + 1]; draft[2] = h[j + K + 2];
            return 1;
        }
    }
    return 0;
}

static const KSet *spec_set(void) {
    if (!gpu.full || gpu.force_cpu || !gpu.blog4 || gx.on) return NULL;
    for (int i = 0; i < gpu.n_sets; i++) if (gpu.set[i].nb == 4) return &gpu.set[i];
    return NULL;
}

static void moe_report(void) {
    if (!gx.on) return;
    const double uses = (double)(gx.hits + gx.miss);
    fprintf(stderr, "[especialistas: %.1f%% dos usos na VRAM, %.1f%% na CPU (%.2f s) | %llu subidas (%llu conversoes, %llu trocas) em %.2f s, leitura do disco %.2f s]\n",
            100.0 * gx.hits / (uses + 1e-9), 100.0 * gx.cpu_runs / (uses + 1e-9), gx.t_cpu,
            (unsigned long long)gx.loads, (unsigned long long)gx.conv, (unsigned long long)gx.evict, gx.t_up, gx.t_read);
}

static int generate(Model *m, const char *text, int max_tokens, float temp, int echo) {
    const Config *c = &m->cfg;
    const Tokenizer *tk = &m->tk;
    const int V = c->vocab;
    Ids ids = {0};
    if (c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
    encode(tk, text, &ids);
    if (ids.n == 0) ids_push(&ids, c->bos >= 0 ? c->bos : 0);
    if (ids.n >= c->ctx) die("prompt maior que o contexto");

    srand((unsigned)time(NULL));
    float *lg = NULL, *col = xalloc(V, sizeof(float));
    int pos = 0, gen = 0, n_prompt = ids.n, stop = 0;
    const KSet *ks4 = getenv("KATANA_SPEC") && atoi(getenv("KATANA_SPEC")) == 0 ? NULL : spec_set();
    double ema = 1.5;       /* media movel de rascunhos aceitos por aposta */
    long probe = 0;
    double t0 = now_sec();
    lg = forward_seq(m, ids.ids, ids.n, 0, 1);
    pos = ids.n;
    double t1 = now_sec();
    if (echo) { fputs(text, stdout); fflush(stdout); }

#define EMIT_TOKEN(t) { if ((t) == c->eos || (tk->toktype && tk->toktype[t] == 3)) { stop = 1; } \
    else { print_token(tk, (t), stdout); fflush(stdout); gen++; ids_push(&ids, (t)); } }

    while (!stop && gen < max_tokens && pos < c->ctx) {
        int next = sample(lg, V, temp), draft[3];
        EMIT_TOKEN(next)
        if (stop) break;
        /* aposta so com rascunho disponivel e taxa de acerto que pague a passada em lote */
        int try_spec = ks4 && pos + 4 < c->ctx && gen + 3 < max_tokens && find_draft(ids.ids, ids.n, draft);
        if (try_spec && ema < 1.0 && (++probe % 8) != 0) try_spec = 0;
        if (!try_spec) { lg = forward(m, next, pos++, 1); continue; }

        int toks[4] = { next, draft[0], draft[1], draft[2] }, ok = 0;
        gpu.want_all = 1;
        const float *all = gpu_forward_n(m, ks4, toks, pos, 1);
        gpu.want_all = 0;
        spec_stat.steps++; spec_stat.drafted += 3;
        int i;
        for (i = 0; i < 3 && !stop && gen < max_tokens; i++) {
            for (int v = 0; v < V; v++) col[v] = all[(size_t)v * 4 + i];
            const int t = sample(col, V, temp);
            EMIT_TOKEN(t)
            if (stop) break;
            if (t != toks[i + 1]) {         /* rascunho errado: o modelo segue a partir do token certo */
                pos += i + 1;
                lg = forward(m, t, pos++, 1);
                break;
            }
            ok++;
        }
        spec_stat.accepted += ok;
        ema = 0.8 * ema + 0.2 * ok;
        if (i == 3 && !stop) {              /* os 3 valeram: os logits da ultima posicao seguem o jogo */
            pos += 4;
            for (int v = 0; v < V; v++) m->logits[v] = all[(size_t)v * 4 + 3];
            lg = m->logits;
        }
    }
#undef EMIT_TOKEN
    double t2 = now_sec();
    printf("\n");
    fflush(stdout);
    prof_report();
    gen = ids.n - n_prompt;
    fprintf(stderr, "[prompt: %d tokens, %.1f tok/s | geracao: %d tokens, %.1f tok/s]\n",
            n_prompt, n_prompt / (t1 - t0 + 1e-9), gen, gen / (t2 - t1 + 1e-9));
    moe_report();
    heat_save(m);
    if (spec_stat.steps)
        fprintf(stderr, "[especulacao: %ld apostas, %ld de %ld rascunhos aceitos]\n",
                spec_stat.steps, spec_stat.accepted, spec_stat.drafted);
    free(ids.ids); free(col);
    return 0;
}

/* confere o produto quantizado nativo contra dequantizar + produto em float */
static double worst_int;    /* pior erro do produto inteiro da CPU (quantizacao de 8 bits das ativacoes) */

static void selftest_mat(const char *name, const Mat *W, int rows, int cols, double *worst) {
    if (!W->q) { printf("  %-10s float32 (sem caminho quantizado)\n", name); return; }
    float *x = xalloc(cols, sizeof(float)), *row = xalloc(cols, sizeof(float)), *sx = xalloc(cols / 32 + 1, sizeof(float));
    double maxerr = 0.0, interr = 0.0;
    for (int i = 0; i < cols; i++) x[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
    block_sums(sx, x, cols);
    int n = rows < 64 ? rows : 64;
    for (int r = 0; r < n; r++) {
        size_t i = (size_t)r * (rows / n);
        mat_row(row, W, (int)i, cols);
        double ref = 0.0, mag = 0.0;
        for (int j = 0; j < cols; j++) { ref += (double)row[j] * x[j]; mag += fabs((double)row[j] * x[j]); }
        cpu_exact = 1;
        double err = fabs(dot_row(W, i, x, sx, cols) - ref) / (mag + 1e-30);
        cpu_exact = 0;
        if (err > maxerr) maxerr = err;
        if (mat_int(W)) {
            xq_prepare(x, cols);
            double ei = fabs(dot_row(W, i, x, sx, cols) - ref) / (mag + 1e-30);
            if (ei > interr) interr = ei;
        }
    }
    printf("  %-10s tipo=%-2u linhas=%-6d colunas=%-5d erro relativo max=%.2e", name, W->type, rows, cols, maxerr);
    if (maxerr > *worst) *worst = maxerr;
    if (mat_int(W)) { printf("  | CPU inteiro=%.2e", interr); if (interr > worst_int) worst_int = interr; }
    if (W->gbuf) {   /* resultado inteiro da GPU contra a CPU, linha a linha */
        float *og = xalloc(rows, sizeof(float));
        double gerr = 0.0;
        gpu.test_mv = 1; matvec(og, W, x, rows, cols); gpu.test_mv = 0;
        for (int r = 0; r < rows; r++) {
            mat_row(row, W, r, cols);
            double ref = 0.0, mag = 0.0;
            for (int j = 0; j < cols; j++) { ref += (double)row[j] * x[j]; mag += fabs((double)row[j] * x[j]); }
            double err = fabs(og[r] - ref) / (mag + 1e-30);
            if (err > gerr) gerr = err;
        }
        printf("  | GPU=%.2e", gerr);
        if (gerr > *worst) *worst = gerr;
        free(og);
    }
    printf("\n");
    free(x); free(row); free(sx);
}

static void setenv_(const char *k, const char *v) {
#ifdef _WIN32
    char b[128]; snprintf(b, sizeof b, "%s=%s", k, v ? v : ""); _putenv(b);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static int selftest(Model *m) {
    const Config *c = &m->cfg;
    const int d = c->dim, kvd = c->n_kv * c->hd, H = c->hidden;
    double worst = 0.0;
    srand(1234);
    selftest_mat("tok_emb", &m->tok_emb, c->vocab, d, &worst);
    for (int l = 0; l < c->n_layers; l += c->n_layers - 1 > 0 ? c->n_layers - 1 : 1) {
        const Layer *L = &m->layers[l];
        printf(" camada %d\n", l);
        selftest_mat("attn_q", &L->wq, m->cfg.n_heads * m->cfg.hd, d, &worst);
        selftest_mat("attn_k", &L->wk, kvd, d, &worst);
        selftest_mat("attn_v", &L->wv, kvd, d, &worst);
        selftest_mat("attn_out", &L->wo, d, m->cfg.n_heads * m->cfg.hd, &worst);
        selftest_mat("ffn_gate", &L->w_gate, H, d, &worst);
        selftest_mat("ffn_up", &L->w_up, H, d, &worst);
        selftest_mat("ffn_down", &L->w_down, d, H, &worst);
    }
    int ok = worst < 1e-4 && worst_int < 1e-4;
    printf("produtos: %s (pior erro relativo: exato %.2e, CPU inteiro %.2e)\n", ok ? "OK" : "FALHOU", worst, worst_int);
    if (gpu.vk && !vkg.on) printf("Vulkan: grafo na CPU (verificacoes de grafo completo puladas)\n");

    if (gpu.n_gl > 0 || vkg.on) {   /* mesma sequencia pela GPU (em lote e token a token) e pela CPU pura */
        const int V = c->vocab, N = 23;
        int toks[23];
        float *ref = xalloc(V, sizeof(float)), *o = NULL;
        for (int i = 0; i < N; i++) toks[i] = (int)((1000003u * (i + 1)) % (unsigned)V);
        gpu.force_cpu = 1; cpu_exact = 1;
        for (int i = 0; i < N; i++) o = forward(m, toks[i], i, i == N - 1);
        gpu.force_cpu = 0; cpu_exact = 0;
        memcpy(ref, o, V * sizeof(float));
        for (int mode = 0; mode < 4; mode++) {
            if (mode == 3 && !(vkg.on && vkb.ok)) continue;
            const long p0 = vkb.passes;
            if (mode == 0) o = forward_seq(m, toks, N, 0, 1);
            else if (mode == 3) { setenv_("KATANA_VK_CAP", "16"); o = forward_seq(m, toks, N, 0, 1); setenv_("KATANA_VK_CAP", NULL); }
            else if (mode == 1) for (int i = 0; i < N; i++) o = forward(m, toks[i], i, i == N - 1);
            else { gpu.force_cpu = 1; for (int i = 0; i < N; i++) o = forward(m, toks[i], i, i == N - 1); gpu.force_cpu = 0; }
            int ag = 0, ac = 0; double md = 0.0;
            for (int i = 0; i < V; i++) {
                if (o[i] > o[ag]) ag = i;
                if (ref[i] > ref[ac]) ac = i;
                if (fabs(o[i] - ref[i]) > md) md = fabs(o[i] - ref[i]);
            }
            int e2e = ag == ac && md < (gpu.kv16 ? 1.0 : 0.05);
            if (vkb.ok && (mode == 0 || mode == 3)) printf("  (%ld passadas em lote)\n", vkb.passes - p0);
            printf("%s x CPU exata (%d tokens): maior diferenca de logit=%.6f, argmax %d x %d: %s\n",
                   mode == 0 ? (vkg.on ? (vkb.ok ? "GPU (Vk, lote)   " : "GPU (Vk, 1 a 1)  ") : "GPU em lote      ") : mode == 1 ? "GPU token a token" : mode == 3 ? "GPU (Vk, lote 16)" : "CPU inteira      ", N, md, ag, ac, e2e ? "OK" : "FALHOU");
            ok = ok && e2e;
        }
        free(ref);
    }
    return ok ? 0 : 1;
}

/* comando oculto `vkcheck modelo texto.txt [ngen]`: processa o texto (ate ~600 tokens) em lote e depois token a token em Vulkan
   (a mesma sequencia, caches KV refeitos) e compara os logits finais e os proximos ngen tokens gulosos */
static char *read_text(const char *path);
static int vkcheck(Model *m, const char *path, int ngen, int ntok) {
    const Config *c = &m->cfg;
    const int V = c->vocab;
    if (!vkg.on || !vkb.ok) { printf("vkcheck: lote Vulkan inativo\n"); return 1; }
    Ids ids = {0};
    if (c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
    encode(&m->tk, read_text(path), &ids);
    int n = ids.n < ntok ? ids.n : ntok;
    if (n + ngen >= c->ctx) n = c->ctx - ngen - 1;
    float *lgs[2] = { xalloc(V, sizeof(float)), xalloc(V, sizeof(float)) };
    int *gen[2] = { xalloc(ngen, sizeof(int)), xalloc(ngen, sizeof(int)) };
    double tp[2];
    for (int run = 0; run < 2; run++) {
        vkb.ok = run == 0;              /* 0: lote; 1: um token por vez */
        double t0 = now_sec();
        float *lg = forward_seq(m, ids.ids, n, 0, 1);
        tp[run] = now_sec() - t0;
        memcpy(lgs[run], lg, V * sizeof(float));
        int pos = n;
        for (int g = 0; g < ngen; g++) {
            int best = 0;
            for (int i = 1; i < V; i++) if (lg[i] > lg[best]) best = i;
            gen[run][g] = best;
            lg = forward(m, best, pos++, 1);
        }
    }
    vkb.ok = 1;
    double md = 0, mx = 0;
    int a0 = 0, a1 = 0;
    for (int i = 0; i < V; i++) {
        if (lgs[0][i] > lgs[0][a0]) a0 = i;
        if (lgs[1][i] > lgs[1][a1]) a1 = i;
        if (fabs(lgs[0][i] - lgs[1][i]) > md) md = fabs(lgs[0][i] - lgs[1][i]);
        if (fabs(lgs[1][i]) > mx) mx = fabs(lgs[1][i]);
    }
    int first = -1;
    for (int g = 0; g < ngen; g++) if (gen[0][g] != gen[1][g]) { first = g; break; }
    printf("vkcheck: prompt de %d tokens: lote %.1f tok/s, token a token %.1f tok/s\n", n, n / tp[0], n / tp[1]);
    printf("  logits finais: maior diferenca %.6f (maior |logit| %.2f), argmax lote %d x 1 a 1 %d\n", md, mx, a0, a1);
    printf("  proximos %d tokens gulosos: %s", ngen, first < 0 ? "todos iguais" : "primeira diferenca no token");
    if (first >= 0) printf(" %d (lote %d x 1 a 1 %d)", first, gen[0][first], gen[1][first]);
    printf("\n");
    return (a0 == a1 && first < 0) ? 0 : 2;
}

/* mede prompt e geracao com tokens sinteticos, depois de aquecer (pesos ja na VRAM) */
static int bench(Model *m, int n_prompt, int n_gen) {
    const Config *c = &m->cfg;
    if (n_prompt < 1) n_prompt = 1;
    if (n_prompt + n_gen >= c->ctx) { n_prompt = c->ctx / 2; n_gen = c->ctx / 4; }
    int *toks = xalloc(n_prompt, sizeof(int));
    for (int i = 0; i < n_prompt; i++) toks[i] = (int)((1000003u * (i + 1)) % (unsigned)c->vocab);

    forward_seq(m, toks, n_prompt < 32 ? n_prompt : 32, 0, 1);      /* aquecimento */
    for (int r = 0; r < 3; r++) {
        double t0 = now_sec();
        float *lg = forward_seq(m, toks, n_prompt, 0, 1);
        double t1 = now_sec();
        int pos = n_prompt;
        for (int g = 0; g < n_gen; g++) lg = forward(m, sample(lg, c->vocab, 0.0f), pos++, 1);
        double t2 = now_sec();
        printf("rodada %d: prompt %d tokens %.1f tok/s | geracao %d tokens %.1f tok/s (%.2f ms/token)\n",
               r + 1, n_prompt, n_prompt / (t1 - t0), n_gen, n_gen / (t2 - t1), (t2 - t1) * 1e3 / n_gen);
    }
    if (getenv("KATANA_TIME")) fprintf(stderr, "[espera pela GPU nas leituras: %.2f s no total]\n", t_gread);
    if (getenv("KATANA_TIME") && vkb.passes)
        fprintf(stderr, "[lote Vulkan: %ld passadas, %ld tokens; CPU embedding+envio %.3f s, enfileirar %.3f s, espera %.3f s]\n",
                vkb.passes, vkb.toks, vkb.t_emb, vkb.t_emit, vkb.t_wait);
    prof_report();
    cprof_report();
    moe_report();
    free(toks);
    return 0;
}

/* microbenchmark do produto em Vulkan: muitos despachos num fluxo so e uma unica espera no fim */
static double vk_time_run(const Mat *const *ws, const int *rr, const int *cc, int nw, int reps, KvkBuf x, KvkBuf out, double *weights) {
    double w = 0.0;
    kvk_wait();
    const double t0 = now_sec();
    for (int i = 0; i < reps; i++) {
        const int k = i % nw;
        if (!vk_gmv(ws[k], x, out, rr[k], cc[k], 0)) die("vkmicro: despacho falhou");
        w += (double)rr[k] * cc[k];
    }
    kvk_wait();
    *weights = w;
    return now_sec() - t0;
}

static int vkmicro(Model *m) {
    const Config *c = &m->cfg;
    const int d = c->dim, kvd = c->n_kv * c->hd, H = c->hidden, qd = c->n_heads * c->hd;
    if (!gpu.vk) { printf("vkmicro: Vulkan indisponivel\n"); return 1; }
    float *xh = xalloc(H > d ? (H > qd ? H : qd) : (d > qd ? d : qd), sizeof(float));
    for (int i = 0; i < d; i++) xh[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
    kvk_buf_write(gpu.vxb, 0, xh, (size_t)d * sizeof(float));
    printf("Vulkan: produto matriz x vetor, 400 despachos por espera, G pesos/s\n");
    const Layer *L = &m->layers[0];
    const struct { const char *n; const Mat *w; int r, c; } t[] = {
        { "attn_q", &L->wq, qd, d }, { "attn_k", &L->wk, kvd, d }, { "attn_v", &L->wv, kvd, d }, { "attn_out", &L->wo, d, qd },
        { "ffn_gate", &L->w_gate, H, d }, { "ffn_up", &L->w_up, H, d }, { "ffn_down", &L->w_down, d, H } };
    for (int i = 0; i < 7; i++) {
        if (!t[i].w->gbuf) continue;
        double w;
        vk_time_run(&t[i].w, &t[i].r, &t[i].c, 1, 20, gpu.vxb, gpu.vob, &w);     /* aquece */
        const double s = vk_time_run(&t[i].w, &t[i].r, &t[i].c, 1, 400, gpu.vxb, gpu.vob, &w);
        printf("  %-9s tipo=%-2u %6dx%-5d (mesma matriz, pode estar na L2): %7.1f G pesos/s  %6.1f us/despacho\n",
               t[i].n, t[i].w->type, t[i].r, t[i].c, w / s / 1e9, s / 400 * 1e6);
    }
    /* todas as matrizes ja na VRAM em sequencia (como uma passada de geracao): fora da L2 */
    int n = 0, cap = c->n_layers * 7 + 1;
    const Mat **ws = xalloc(cap, sizeof(Mat *)); int *rr = xalloc(cap, sizeof(int)), *cc = xalloc(cap, sizeof(int));
    for (int l = 0; l < c->n_layers; l++) {
        const Layer *Ll = &m->layers[l];
        const struct { const Mat *w; int r, c; } u[] = { { &Ll->wq, qd, d }, { &Ll->wk, kvd, d }, { &Ll->wv, kvd, d }, { &Ll->wo, d, qd },
            { &Ll->w_gate, H, d }, { &Ll->w_up, H, d }, { &Ll->w_down, d, H } };
        for (int i = 0; i < 7; i++) if (u[i].w->gbuf) { ws[n] = u[i].w; rr[n] = u[i].r; cc[n] = u[i].c; n++; }
    }
    if (n) {
        const int reps = n * (n > 100 ? 2 : 6);
        double w;
        vk_time_run(ws, rr, cc, n, n, gpu.vxb, gpu.vob, &w);
        const double s = vk_time_run(ws, rr, cc, n, reps, gpu.vxb, gpu.vob, &w);
        printf("  todas as %d matrizes na VRAM em sequencia (%d despachos): %7.1f G pesos/s  %6.1f us/despacho\n", n, reps, w / s / 1e9, s / reps * 1e6);
    }
    KvkStats st; kvk_stats(&st);
    printf("  despachos=%llu submissoes=%llu conjuntos de descritores=%llu\n", (unsigned long long)st.dispatches, (unsigned long long)st.submits, (unsigned long long)st.ds_allocs);
    free(xh); free(ws); free(rr); free(cc);
    return 0;
}

/* microbenchmark da CPU numa matriz: a mesma linha repetida (so calculo, dados no cache L1)
   contra todas as linhas em sequencia (calculo + leitura da memoria), em uma thread */
static void micro_mat(const char *name, const Mat *W, int rows, int cols) {
    if (!W->q) return;
    float *x = xalloc(cols, sizeof(float)), *sx = xalloc(cols / 32 + 1, sizeof(float));
    for (int i = 0; i < cols; i++) x[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
    block_sums(sx, x, cols);
    for (int exact = 1; exact >= 0; exact--) {
        cpu_exact = exact;
        if (!exact && !mat_int(W)) break;
        if (mat_int(W)) xq_prepare(x, cols);
        volatile float sink = 0.0f;
        const int reps = (int)(400000000.0 / cols) + 1;
        double t0 = now_sec();
        for (int r = 0; r < reps; r++) sink += dot_row(W, 0, x, sx, cols);
        double t1 = now_sec();
        int passes = (int)(400000000.0 / ((double)rows * cols)) + 1;
        for (int ps = 0; ps < passes; ps++)
            for (int i = 0; i < rows; i++) sink += dot_row(W, (size_t)i, x, sx, cols);
        double t2 = now_sec();
        printf("  %-9s tipo=%-2u %-8s  mesma linha: %6.2f G pesos/s | matriz inteira: %6.2f G pesos/s (%.2f GB/s lidos)\n",
               name, W->type, exact ? "float" : "inteiro", (double)reps * cols / (t1 - t0) / 1e9,
               (double)passes * rows * cols / (t2 - t1) / 1e9, (double)passes * rows * W->stride / (t2 - t1) / 1e9);
    }
    cpu_exact = 0;
    free(x); free(sx);
}

/* o mesmo teste com todas as threads do pool: se "mesma linha" escala e "matriz inteira"
   nao, o limite e a memoria; se nem "mesma linha" escala, sao os nucleos (ocupados ou
   com a frequencia reduzida) */
typedef struct { const Mat *W; const float *x, *sx; int cols; volatile float sink; } MicroJob;
static void micro_same_row(void *c, int i0, int i1) {
    MicroJob *j = c;
    float a = 0.0f;
    for (int i = i0; i < i1; i++) a += dot_row(j->W, 0, j->x, j->sx, j->cols);
    j->sink += a;
}
static void micro_calib(void *c, int i0, int i1) {
    uint64_t acc = (uint64_t)i0 + 1;
    (void)c;
    for (int r = i0; r < i1; r++)
        for (int i = 0; i < 1000000; i++) acc = acc * 6364136223846793005ull + 1442695040888963407ull;
    if (acc == 42) printf(" ");
}

static void micro_par(const char *name, const Mat *W, int rows, int cols) {
    if (!mat_int(W)) return;
    float *x = xalloc(cols, sizeof(float)), *out = xalloc(rows, sizeof(float));
    for (int i = 0; i < cols; i++) x[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
    xq_prepare(x, cols);
    MicroJob job = { W, x, NULL, cols, 0.0f };
    const int reps = (int)(1600000000.0 / cols) + 1;
    double t0 = now_sec();
    pool_for(micro_same_row, &job, reps, reps / (pool.n * 16) + 1);
    double t1 = now_sec();
    pool.st_jobs = pool.st_main = pool.st_work = pool.st_sleep = 0;
    const int passes = (int)(1600000000.0 / ((double)rows * cols)) + 1;
    for (int ps = 0; ps < passes; ps++) matvec(out, W, x, rows, cols);
    double t2 = now_sec();
    printf("  %-9s tipo=%-2u %d threads  mesma linha: %6.2f G pesos/s | matriz inteira: %6.2f G pesos/s (%.2f GB/s lidos)\n",
           name, W->type, pool.n, (double)reps * cols / (t1 - t0) / 1e9,
           (double)passes * rows * cols / (t2 - t1) / 1e9, (double)passes * rows * W->stride / (t2 - t1) / 1e9);
    printf("            pool: %ld trabalhos, blocos feitos pela principal %ld e pelas outras %ld, sonos %ld, %.0f us por trabalho\n",
           (long)pool.st_jobs, (long)pool.st_main, (long)pool.st_work, (long)pool.st_sleep, (t2 - t1) * 1e6 / passes);
    pool.st_jobs = pool.st_main = pool.st_work = pool.st_sleep = 0;
    free(x); free(out);
}

static int micro(const Model *m) {
    const Config *c = &m->cfg;
    const Layer *L = &m->layers[0];
    const int d = c->dim, H = c->hidden;
    printf("microbenchmark da CPU (1 thread):\n");
    {   /* calibracao: o que esta maquina entrega agora em contas simples, com dados no cache L1 */
        float *v = xalloc(1024, sizeof(float));
        volatile float sink = 0.0f;
        for (int i = 0; i < 1024; i++) v[i] = (float)i * 0.001f;
        double t0 = now_sec();
        for (int r = 0; r < 2000000; r++) sink += dot(v, v, 1024);
        double t1 = now_sec();
        uint64_t acc = 1;
        for (uint64_t i = 0; i < 400000000ull; i++) acc = acc * 6364136223846793005ull + 1442695040888963407ull;
        double t2 = now_sec();
        printf("  calibracao: dot float L1 = %.2f G elementos/s | cadeia de multiplicacoes = %.2f GHz equivalentes (%u)\n",
               2000000.0 * 1024 / (t1 - t0) / 1e9, 400000000.0 * 4 / (t2 - t1) / 1e9, (unsigned)(acc & 7));
        free(v);
    }
    micro_mat("attn_q", &L->wq, m->cfg.n_heads * m->cfg.hd, d);
    micro_mat("ffn_gate", &L->w_gate, H, d);
    micro_mat("ffn_down", &L->w_down, d, H);
    micro_mat("lm_head", &m->lm_head, c->vocab, d);
    {   /* calibracao paralela: contas escalares puras em todas as threads */
        double t0 = now_sec();
        pool_for(micro_calib, NULL, 64 * pool.n, 1);
        double t1 = now_sec();
        printf("em paralelo (%d threads): contas escalares = %.2f GHz equivalentes por thread\n",
               pool.n, 64.0 * 1000000 * 4 / (t1 - t0) / 1e9);
    }
    micro_par("attn_q", &L->wq, m->cfg.n_heads * m->cfg.hd, d);
    micro_par("ffn_gate", &L->w_gate, H, d);
    micro_par("ffn_down", &L->w_down, d, H);
    return 0;
}

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

/* grava um shard e acrescenta a linha dele ao manifesto */
static int pack_one(FILE *mf, Mat **ms, int n, size_t rows, size_t cols, const char *name, double *total) {
    char fn[1100], hex[65];
    size_t bytes = 0;
    uint32_t *buf = gpu_build_buf(ms, n, rows, cols, &bytes);
    if (!buf) return 0;
    sha_hex(buf, bytes, hex);
    snprintf(fn, sizeof fn, "%s/%s.bin", shards.dir, name);
    FILE *f = fopen(fn, "wb");
    int ok = f && fwrite(buf, 1, bytes, f) == bytes;
    if (f) fclose(f);
    free(buf);
    if (ok) { fprintf(mf, "%s %zu %s\n", hex, bytes, name); *total += (double)bytes; }
    return ok;
}

/* converte o modelo inteiro para shards no layout da GPU */
/* MoE: todos os especialistas da camada num arquivo so, cada um como par gate/up + down no layout da GPU */
static int pack_experts(FILE *mf, Model *m, int l, double *total) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden;
    Layer *L = &m->layers[l];
    char fn[1100], hex[65], nm[32];
    uint8_t *all = NULL;
    size_t bytes = 0, per = 0;
    int ok = 1;
    for (int e = 0; e < c->n_expert && ok; e++) {
        Mat *two[2] = { &L->eg[e], &L->eu[e] }, *dn = &L->ed[e];
        size_t b0 = 0, b1 = 0;
        uint32_t *g = gpu_build_buf(two, 2, H, d, &b0), *w = gpu_build_buf(&dn, 1, d, H, &b1);
        if (!all && g && w) { per = b0 + b1; all = malloc(per * c->n_expert); }
        ok = g && w && all && b0 + b1 == per;
        if (ok) { memcpy(all + bytes, g, b0); memcpy(all + bytes + b0, w, b1); bytes += per; }
        free(g); free(w);
    }
    if (ok) {
        sha_hex(all, bytes, hex);
        snprintf(nm, sizeof nm, "L%03d.exps", l);
        snprintf(fn, sizeof fn, "%s/%s.bin", shards.dir, nm);
        FILE *f = fopen(fn, "wb");
        ok = f && fwrite(all, 1, bytes, f) == bytes;
        if (f) fclose(f);
    }
    free(all);
    if (!ok) return 0;
    fprintf(mf, "%s %zu %s\n", hex, bytes, nm);
    *total += (double)bytes;
    return 1;
}

static int pack(Model *m, const char *path, int64_t fsz) {
    const Config *c = &m->cfg;
    const size_t d = c->dim, H = c->hidden, kvd = (size_t)c->n_kv * c->hd;
    char mfn[1100], nm[32];
    double total = 0.0, t0 = now_sec();
    int ok = 1, count = 0;
    snprintf(shards.dir, sizeof shards.dir, "%s.ktn", path);
    MKDIR(shards.dir);
    snprintf(mfn, sizeof mfn, "%s/manifest.txt", shards.dir);
    FILE *mf = fopen(mfn, "w");
    if (!mf) { fprintf(stderr, "Nao consegui criar %s\n", mfn); return 1; }
    fprintf(mf, "katana-shards 1\norigem %lld\n", (long long)fsz);
    for (int l = 0; l < c->n_layers && ok; l++) {
        Layer *L = &m->layers[l];
        Mat *one;
#define P1(mat, rows, cols, suf) one = &L->mat; snprintf(nm, sizeof nm, "L%03d." suf, l); ok = ok && pack_one(mf, &one, 1, rows, cols, nm, &total); count++
        P1(wq, m->cfg.n_heads * m->cfg.hd, d, "wq"); P1(wk, kvd, d, "wk"); P1(wv, kvd, d, "wv"); P1(wo, d, m->cfg.n_heads * m->cfg.hd, "wo");
        if (c->n_expert) {
            if (!glu_pair(&L->w_gate, &L->w_up)) { fprintf(stderr, "MoE com gate e up em formatos diferentes: nao empacotado\n"); ok = 0; }
            ok = ok && pack_experts(mf, m, l, &total); count++;
            printf("\rcamada %d de %d", l + 1, c->n_layers); fflush(stdout);
        } else if (glu_pair(&L->w_gate, &L->w_up)) {
            Mat *two[2] = { &L->w_gate, &L->w_up };
            snprintf(nm, sizeof nm, "L%03d.glu", l);
            ok = ok && pack_one(mf, two, 2, H, d, nm, &total); count++;
        } else { P1(w_gate, H, d, "gate"); P1(w_up, H, d, "up"); }
        if (!c->n_expert) { P1(w_down, d, H, "down"); }
#undef P1
    }
    if (ok) { Mat *one = &m->lm_head; ok = pack_one(mf, &one, 1, c->vocab, d, "head", &total); count++; }
    fclose(mf);
    if (!ok) { fprintf(stderr, "Falha ao gravar os shards (disco cheio?)\n"); return 1; }
    printf("%d shards, %.0f MB em %s (%.1f s)\n", count, total / 1048576.0, shards.dir, now_sec() - t0);
    return 0;
}

/* confere o hash de todos os shards do manifesto */
static int verify(const char *path) {
    char mfn[1100], line[256], fn[1100], hex[65];
    int n = 0, bad = 0;
    snprintf(shards.dir, sizeof shards.dir, "%s.ktn", path);
    snprintf(mfn, sizeof mfn, "%s/manifest.txt", shards.dir);
    FILE *mf = fopen(mfn, "r");
    if (!mf) { fprintf(stderr, "Sem manifesto em %s (rode: katana pack)\n", shards.dir); return 1; }
    while (fgets(line, sizeof line, mf)) {
        char want[65], name[32]; unsigned long long bytes;
        if (sscanf(line, "%64s %llu %31s", want, &bytes, name) != 3 || strlen(want) != 64) continue;
        snprintf(fn, sizeof fn, "%s/%s.bin", shards.dir, name);
        FILE *f = fopen(fn, "rb");
        uint8_t *buf = f ? malloc((size_t)bytes) : NULL;
        int ok = buf && fread(buf, 1, (size_t)bytes, f) == (size_t)bytes && fgetc(f) == EOF;
        if (ok) { sha_hex(buf, (size_t)bytes, hex); ok = strcmp(hex, want) == 0; }
        if (f) fclose(f);
        free(buf);
        n++;
        if (!ok) { bad++; printf("CORROMPIDO ou ausente: %s\n", name); }
    }
    fclose(mf);
    printf("%d shards conferidos, %d com problema: %s\n", n, bad, bad || !n ? "FALHOU" : "OK");
    return bad || !n ? 1 : 0;
}

/* tempo de uma passada: prompt em lote e geracao token a token (melhor de 2) */
static void tune_measure(Model *m, const int *toks, int n_prompt, int n_gen, double *pt, double *gt) {
    *pt = *gt = 1e30;
    forward_seq(m, toks, 16, 0, 1);
    for (int r = 0; r < 2; r++) {
        double t0 = now_sec();
        float *lg = forward_seq(m, toks, n_prompt, 0, 1);
        double t1 = now_sec();
        int pos = n_prompt;
        for (int g = 0; g < n_gen; g++) lg = forward(m, sample(lg, m->cfg.vocab, 0.0f), pos++, 1);
        double t2 = now_sec();
        if (t1 - t0 < *pt) *pt = t1 - t0;
        if (t2 - t1 < *gt) *gt = t2 - t1;
    }
}

/* mede as opcoes de ajuste nesta placa e grava a melhor combinacao em katana.cfg */
static int tune(Model *m) {
    static const size_t segs[] = { 0, 4096, 16384, 65536 };
    const int NP = 128, NG = 32;
    int toks[128];
    double pt, gt, best;
    if (!gpu.full) { fprintf(stderr, "tune: use um modelo que caiba inteiro na VRAM\n"); return 1; }
    for (int i = 0; i < NP; i++) toks[i] = (int)((1000003u * (i + 1)) % (unsigned)m->cfg.vocab);

    size_t best_seg = gpu.seg_target; best = 1e30;
    for (int i = 0; i < 4; i++) {
        gpu.seg_target = segs[i];
        tune_measure(m, toks, NP, NG, &pt, &gt);
        printf("segmentos alvo=%-6zu  geracao %.1f tok/s  prompt %.1f tok/s\n", segs[i], NG / gt, NP / pt);
        if (gt < best) { best = gt; best_seg = segs[i]; }
    }
    gpu.seg_target = best_seg;

    int best_rows = 1, best_rowsb = 1; double bg = 1e30, bp = 1e30;
    for (int nr = 1; nr <= 2; nr++) {       /* linhas por thread: mede geracao (rows) e lote (rowsb) juntos */
        gpu.cfg_rows = nr; gpu.cfg_rowsb = nr;
        if (!gpu_build_all()) { fprintf(stderr, "tune: falha ao recompilar os kernels\n"); return 1; }
        tune_measure(m, toks, NP, NG, &pt, &gt);
        printf("linhas por thread=%d      geracao %.1f tok/s  prompt %.1f tok/s\n", nr, NG / gt, NP / pt);
        if (gt < bg) { bg = gt; best_rows = nr; }
        if (pt < bp) { bp = pt; best_rowsb = nr; }
    }
    gpu.cfg_rows = best_rows; gpu.cfg_rowsb = best_rowsb;
    if (!gpu_build_all()) return 1;

    char fn[1100];
    cfg_path(fn, sizeof fn);
    FILE *f = fopen(fn, "w");
    if (!f) { fprintf(stderr, "tune: nao consegui gravar %s\n", fn); return 1; }
    fprintf(f, "gpu=%s\nseg=%zu\nrows=%d\nrowsb=%d\n", gpu.name, best_seg, best_rows, best_rowsb);
    fclose(f);
    printf("melhor: segmentos alvo=%zu, linhas por thread=%d (geracao) e %d (lote); gravado em %s\n",
           best_seg, best_rows, best_rowsb, fn);
    return 0;
}

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Nao consegui abrir %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *t = xalloc((size_t)n + 1, 1);
    if (fread(t, 1, (size_t)n, f) != (size_t)n) die("falha ao ler o texto");
    fclose(f);
    return t;
}

static int cmp_float_desc(const void *a, const void *b) {
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* Mede se o feed-forward e esparso POR ASSUNTO.
   1) roda o texto de calibracao e soma a ativacao de cada neuronio (por camada);
   2) roda o texto de avaliacao com o modelo inteiro e guarda o token previsto em cada posicao;
   3) repete a avaliacao mantendo so os p% neuronios mais ativos na calibracao (e, para
      comparar, p% escolhidos ao acaso) e conta em quantas posicoes a previsao continua igual. */
static int sparsity(Model *m, const char *calib_path, const char *eval_path) {
    const Config *c = &m->cfg;
    const Tokenizer *tk = &m->tk;
    const int H = c->hidden, L = c->n_layers, V = c->vocab;
    Ids ca = {0}, ev = {0};
    if (c->add_bos && c->bos >= 0) ids_push(&ca, c->bos);
    encode(tk, read_text(calib_path), &ca);
    encode(tk, read_text(eval_path), &ev);
    if (ca.n + ev.n >= c->ctx) die("textos maiores que o contexto");
    printf("calibracao: %d tokens | avaliacao: %d tokens | %d camadas x %d neuronios\n", ca.n, ev.n, L, H);

    sp.sum = xalloc((size_t)L * H, sizeof(float));
    sp.collect = 1;
    for (int i = 0; i < ca.n; i++) forward(m, ca.ids[i], i, 0);
    sp.collect = 0;

    int *full = xalloc(ev.n, sizeof(int));
    float *ref = xalloc((size_t)ev.n, sizeof(float));
    for (int i = 0; i < ev.n; i++) {
        const float *lg = forward(m, ev.ids[i], ca.n + i, 1);
        full[i] = sample(lg, V, 0.0f); ref[i] = lg[full[i]];
    }

    /* quanto da ativacao total da calibracao os p% mais ativos concentram */
    float *tmp = xalloc(H, sizeof(float));
    uint8_t *mask = xalloc((size_t)L * H, 1);
    static const int pcts[] = { 10, 20, 30, 50, 70 };
    printf("%-10s %-22s %-28s %-22s\n", "mantidos", "ativacao coberta", "mesma previsao (quentes)", "mesma previsao (acaso)");
    for (int pi = 0; pi < 5; pi++) {
        const int keep = H * pcts[pi] / 100;
        double agree[2] = { 0, 0 }, cover = 0.0;
        for (int mode = 0; mode < 2; mode++) {      /* 0 = quentes da calibracao, 1 = ao acaso */
            srand(4242);
            for (int l = 0; l < L; l++) {
                uint8_t *mk = mask + (size_t)l * H;
                memset(mk, 0, H);
                if (mode == 0) {
                    memcpy(tmp, sp.sum + (size_t)l * H, H * sizeof(float));
                    qsort(tmp, H, sizeof(float), cmp_float_desc);
                    const float thr = tmp[keep - 1];
                    int n = 0;
                    for (int i = 0; i < H && n < keep; i++) if (sp.sum[(size_t)l * H + i] >= thr) { mk[i] = 1; n++; }
                } else {
                    for (int n = 0; n < keep; ) { const int i = (int)(((unsigned)rand() * 32768u + (unsigned)rand()) % (unsigned)H); if (!mk[i]) { mk[i] = 1; n++; } }
                }
            }
            sp.mask = mask; sp.mass_all = sp.mass_kept = 0.0;
            int same = 0;
            for (int i = 0; i < ev.n; i++) {
                const float *lg = forward(m, ev.ids[i], ca.n + i, 1);
                if (sample(lg, V, 0.0f) == full[i]) same++;
            }
            sp.mask = NULL;
            agree[mode] = 100.0 * same / ev.n;
            if (mode == 0) cover = 100.0 * sp.mass_kept / (sp.mass_all + 1e-30);
        }
        printf("%3d%%       %5.1f%%                 %5.1f%%                       %5.1f%%\n", pcts[pi], cover, agree[0], agree[1]);
    }
    /* teto da escolha por token: mantem os p% mais ativos de cada token (oraculo) */
    printf("escolha por token (oraculo):\n%-10s %-22s %-28s\n", "mantidos", "ativacao coberta", "mesma previsao");
    sp.tmp = xalloc(H, sizeof(float));
    static const int op[] = { 5, 10, 20, 30, 50 };
    for (int pi = 0; pi < 5; pi++) {
        int same = 0;
        sp.oracle_keep = H * op[pi] / 100; sp.mass_all = sp.mass_kept = 0.0;
        for (int i = 0; i < ev.n; i++) {
            const float *lg = forward(m, ev.ids[i], ca.n + i, 1);
            if (sample(lg, V, 0.0f) == full[i]) same++;
        }
        sp.oracle_keep = 0;
        printf("%3d%%       %5.1f%%                 %5.1f%%\n", op[pi], 100.0 * sp.mass_kept / (sp.mass_all + 1e-30), 100.0 * same / ev.n);
    }
    (void)ref;
    return 0;
}

typedef struct { uint32_t cnt; int id; } RtRank;
static int cmp_rtrank(const void *a, const void *b) {
    const RtRank *x = a, *y = b;
    return x->cnt < y->cnt ? 1 : x->cnt > y->cnt ? -1 : x->id - y->id;
}

/* Mede quanto o roteamento de um modelo MoE se concentra POR ASSUNTO, e quanto isso vale
   para manter na VRAM so parte dos especialistas.
   1) roda o texto de calibracao contando o uso de cada especialista (camada, indice);
   2) roda o texto de avaliacao guardando os especialistas escolhidos em cada token;
   3) para uma VRAM que comporta p% dos especialistas, conta quantos usos da avaliacao
      cairiam na VRAM: com o conjunto fixo mais usado na calibracao, com um cache que
      troca o menos recente (LRU, partindo desse conjunto) e com um conjunto ao acaso. */
static int routing(Model *m, const char *calib_path, const char *eval_path) {
    const Config *c = &m->cfg;
    const int E = c->n_expert, K = c->n_used, L = c->n_layers, N = L * E;
    if (!E) die("este modelo nao e MoE");
    Ids ca = {0}, ev = {0};
    if (c->add_bos && c->bos >= 0) ids_push(&ca, c->bos);
    encode(&m->tk, read_text(calib_path), &ca);
    encode(&m->tk, read_text(eval_path), &ev);
    if (ca.n + ev.n >= c->ctx) die("textos maiores que o contexto");
    printf("calibracao: %d tokens | avaliacao: %d tokens | %d camadas x %d especialistas (%d por token)\n", ca.n, ev.n, L, E, K);

    moe.cnt = xalloc(N, sizeof(uint32_t));
    for (int i = 0; i < ca.n; i++) forward(m, ca.ids[i], i, 0);
    uint32_t *ccal = moe.cnt;
    moe.cnt = xalloc(N, sizeof(uint32_t));
    moe.trace = xalloc((size_t)ev.n * L * K, sizeof(uint16_t));
    for (int i = 0; i < ev.n; i++) { moe.ti = i; forward(m, ev.ids[i], ca.n + i, 0); }
    const uint16_t *tr = moe.trace; moe.trace = NULL;
    uint32_t *cev = moe.cnt; moe.cnt = NULL;

    int used_c = 0, used_e = 0, both = 0;
    for (int i = 0; i < N; i++) { used_c += ccal[i] > 0; used_e += cev[i] > 0; both += ccal[i] > 0 && cev[i] > 0; }
    printf("especialistas distintos usados: calibracao %d de %d (%.0f%%), avaliacao %d (%.0f%%), em comum %d\n",
           used_c, N, 100.0 * used_c / N, used_e, 100.0 * used_e / N, both);

    RtRank *rk = xalloc(N, sizeof(RtRank)), *rke = xalloc(N, sizeof(RtRank));
    for (int i = 0; i < N; i++) { rk[i].cnt = ccal[i]; rk[i].id = i; rke[i].cnt = cev[i]; rke[i].id = i; }
    qsort(rk, N, sizeof(RtRank), cmp_rtrank);
    qsort(rke, N, sizeof(RtRank), cmp_rtrank);
    const double total = (double)ev.n * L * K;
    int *stamp = xalloc(N, sizeof(int));        /* 0 = fora da VRAM; senao, ultimo uso */
    uint8_t *in = xalloc(N, 1);
    static const int pcts[] = { 5, 10, 15, 25, 35, 50 };
    printf("%-10s %-12s %-14s %-14s %-12s %-10s\n", "na VRAM", "teto", "conjunto fixo", "cache LRU", "trocas/token", "acaso");
    for (int pi = 0; pi < 6; pi++) {
        const int cap = N * pcts[pi] / 100;
        double hit_fix = 0, hit_lru = 0, hit_best = 0;
        long swaps = 0;
        memset(in, 0, N);
        for (int i = 0; i < cap; i++) in[rk[i].id] = 1;
        for (int i = 0; i < cap; i++) hit_best += rke[i].cnt;       /* teto: os mais usados da propria avaliacao */
        {   /* teto com o mesmo numero de vagas em toda camada */
            const int per = E * pcts[pi] / 100;
            double hu = 0;
            RtRank *tmp = xalloc(E, sizeof(RtRank));
            for (int l = 0; l < L; l++) {
                for (int e = 0; e < E; e++) { tmp[e].cnt = cev[l * E + e]; tmp[e].id = e; }
                qsort(tmp, E, sizeof(RtRank), cmp_rtrank);
                for (int i = 0; i < per; i++) hu += tmp[i].cnt;
            }
            free(tmp);
            printf("%3d%%: teto com vagas iguais por camada %5.1f%%\n", pcts[pi], 100.0 * hu / total);
        }
        memset(stamp, 0, N * sizeof(int));
        for (int i = 0; i < cap; i++) stamp[rk[i].id] = 1;
        int clock = 1;
        for (int t = 0; t < ev.n; t++)
            for (int l = 0; l < L; l++)
                for (int k = 0; k < K; k++) {
                    const int id = l * E + tr[((size_t)t * L + l) * K + k];
                    if (in[id]) hit_fix++;
                    clock++;
                    if (stamp[id]) { hit_lru++; stamp[id] = clock; continue; }
                    int old = -1;                               /* troca o usado ha mais tempo */
                    for (int j = 0; j < N; j++) if (stamp[j] && (old < 0 || stamp[j] < stamp[old])) old = j;
                    if (old >= 0) stamp[old] = 0;
                    stamp[id] = clock; swaps++;
                }
        printf("%3d%%       %5.1f%%       %5.1f%%         %5.1f%%         %6.2f       %3d%%\n", pcts[pi],
               100.0 * hit_best / total, 100.0 * hit_fix / total, 100.0 * hit_lru / total, (double)swaps / ev.n, pcts[pi]);
    }
    return 0;
}

/* ---------- conversa de varios turnos ----------
 * O processo fica vivo e o cache KV guarda a conversa: cada turno so processa o texto novo.
 * Num MoE isso tambem mantem na VRAM os especialistas do assunto. Comandos: /novo recomeca,
 * /sair termina. Quando o contexto enche, a conversa recomeca (mantendo o prompt de sistema). */
static void turn_text(const Config *c, char *out, size_t n, const char *sys, const char *user, int first) {
    char sp[4096] = "";
    if (first && sys && *sys) switch (c->chat_fmt) {
        case 0:  snprintf(sp, sizeof sp, "<|im_start|>system\n%s<|im_end|>\n", sys); break;
        case 1:  snprintf(sp, sizeof sp, "<|start_header_id|>system<|end_header_id|>\n\n%s<|eot_id|>", sys); break;
        case 2:  snprintf(sp, sizeof sp, "<|system|>\n%s</s>\n", sys); break;
        case 3:  snprintf(sp, sizeof sp, "%s\n\n", sys); break;
        default: snprintf(sp, sizeof sp, "%s\n", sys); break;
    }
    switch (c->chat_fmt) {
        case 0:  snprintf(out, n, "%s%s<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n", first ? "" : "<|im_end|>\n", sp, user); break;
        case 1:  snprintf(out, n, "%s%s%s<|start_header_id|>user<|end_header_id|>\n\n%s<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n\n",
                          first ? "" : "<|eot_id|>", first && !c->add_bos ? "<|begin_of_text|>" : "", sp, user); break;
        case 2:  snprintf(out, n, "%s%s<|user|>\n%s</s>\n<|assistant|>\n", first ? "" : "</s>\n", sp, user); break;
        case 3:  snprintf(out, n, "%s[INST] %s%s [/INST]", first ? "" : "</s>", sp, user); break;
        default: snprintf(out, n, "%s%sUser: %s\nAssistant:", first ? "" : "\n", sp, user); break;
    }
}

static int session(Model *m, const char *sys, int max_tokens, float temp) {
    const Config *c = &m->cfg;
    const Tokenizer *tk = &m->tk;
    static char line[16384], text[24576];
    int pos = 0, first = 1;
    srand((unsigned)time(NULL));
    fprintf(stderr, "Conversa iniciada (/novo recomeca, /sair termina).\n");
    for (;;) {
        fprintf(stderr, "\n> "); fflush(stderr);
        if (!fgets(line, sizeof line, stdin)) break;
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln - 1] == '\n' || line[ln - 1] == '\r')) line[--ln] = 0;
        if (ln == 0) continue;
        if (strcmp(line, "/sair") == 0) break;
        if (strcmp(line, "/novo") == 0) { pos = 0; first = 1; fprintf(stderr, "[conversa nova]\n"); continue; }

        Ids ids = {0};
        for (int attempt = 0; attempt < 2; attempt++) {
            ids.n = 0;
            if (first && c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
            turn_text(c, text, sizeof text, sys, line, first);
            encode(tk, text, &ids);
            if (pos + ids.n + 64 < c->ctx) break;
            if (first) { fprintf(stderr, "[mensagem maior que o contexto]\n"); ids.n = 0; break; }
            fprintf(stderr, "[contexto cheio (%d de %d tokens): conversa recomecada]\n", pos, c->ctx);
            pos = 0; first = 1;
        }
        if (ids.n == 0) { free(ids.ids); continue; }

        const double t0 = now_sec();
        float *lg = forward_seq(m, ids.ids, ids.n, pos, 1);
        pos += ids.n;
        first = 0;
        const double t1 = now_sec();
        int gen = 0;
        while (gen < max_tokens && pos < c->ctx - 1) {
            const int t = sample(lg, c->vocab, temp);
            if (t == c->eos || (tk->toktype && tk->toktype[t] == 3)) break;
            print_token(tk, t, stdout); fflush(stdout);
            gen++;
            lg = forward(m, t, pos++, 1);
        }
        const double t2 = now_sec();
        printf("\n"); fflush(stdout);
        fprintf(stderr, "[entrada: %d tokens, %.1f tok/s | resposta: %d tokens, %.1f tok/s | contexto %d de %d]\n",
                ids.n, ids.n / (t1 - t0 + 1e-9), gen, gen / (t2 - t1 + 1e-9), pos, c->ctx);
        free(ids.ids);
    }
    moe_report();
    heat_save(m);
    return 0;
}

/* ---------- agente: conversa com ferramentas ----------
 * O modelo pode pedir ferramentas no formato <tool_call>{"name":..,"arguments":{..}}</tool_call>
 * (o do Qwen); o motor executa e devolve o resultado em <tool_response>. Ler e listar ficam
 * restritos a pasta atual; gravar arquivo e executar comando so acontecem depois de o usuario
 * confirmar no terminal, a cada vez. */
#define TOOL_OUT 3000
static const char *AGENT_SYS =
    "You are katana, a coding assistant running locally on the user's machine. "
    "Use the tools to inspect and change files in the current directory when that helps. Be concise.\n\n"
    "# Tools\n\nYou may call one or more functions to assist with the user query.\n\n"
    "You are provided with function signatures within <tools></tools> XML tags:\n<tools>\n"
    "{\"type\": \"function\", \"function\": {\"name\": \"read_file\", \"description\": \"Read a text file in the current directory\", \"parameters\": {\"type\": \"object\", \"properties\": {\"path\": {\"type\": \"string\"}}, \"required\": [\"path\"]}}}\n"
    "{\"type\": \"function\", \"function\": {\"name\": \"list_dir\", \"description\": \"List the files of a directory (use . for the current one)\", \"parameters\": {\"type\": \"object\", \"properties\": {\"path\": {\"type\": \"string\"}}, \"required\": [\"path\"]}}}\n"
    "{\"type\": \"function\", \"function\": {\"name\": \"write_file\", \"description\": \"Create or overwrite a text file (the user must approve)\", \"parameters\": {\"type\": \"object\", \"properties\": {\"path\": {\"type\": \"string\"}, \"content\": {\"type\": \"string\"}}, \"required\": [\"path\", \"content\"]}}}\n"
    "{\"type\": \"function\", \"function\": {\"name\": \"run_command\", \"description\": \"Run a shell command and return its output (the user must approve)\", \"parameters\": {\"type\": \"object\", \"properties\": {\"command\": {\"type\": \"string\"}}, \"required\": [\"command\"]}}}\n"
    "</tools>\n\nFor each function call, return a json object with function name and arguments within <tool_call></tool_call> XML tags:\n"
    "<tool_call>\n{\"name\": <function-name>, \"arguments\": <args-json-object>}\n</tool_call>";

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

static int agent_confirm(const char *what, const char *detail) {
    char ans[64];
    fprintf(stderr, "\n[o modelo quer %s]\n%s\n[permitir? s/N] ", what, detail);
    fflush(stderr);
    if (!fgets(ans, sizeof ans, stdin)) return 0;
    return ans[0] == 's' || ans[0] == 'S';
}

static void agent_tool(const char *name, const char *js, const char *end, char *out, size_t cap) {
    static char arg[8192], content[16384];
    out[0] = 0;
    if (strcmp(name, "read_file") == 0) {
        if (!json_str(js, end, "path", arg, sizeof arg) || !path_ok(arg)) { snprintf(out, cap, "error: invalid path (only relative paths inside the current directory)"); return; }
        FILE *f = fopen(arg, "rb");
        if (!f) { snprintf(out, cap, "error: cannot open %s", arg); return; }
        size_t n = fread(out, 1, TOOL_OUT, f);
        out[n] = 0;
        if (!feof(f)) snprintf(out + n, cap - n, "\n[truncated at %d bytes]", TOOL_OUT);
        fclose(f);
    } else if (strcmp(name, "list_dir") == 0) {
        if (!json_str(js, end, "path", arg, sizeof arg)) strcpy(arg, ".");
        if (strcmp(arg, ".") != 0 && !path_ok(arg)) { snprintf(out, cap, "error: invalid path"); return; }
        size_t n = 0;
#ifdef _WIN32
        char pat[8300]; WIN32_FIND_DATAA fd;
        snprintf(pat, sizeof pat, "%s\\*", arg);
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) { snprintf(out, cap, "error: cannot list %s", arg); return; }
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            if (n + strlen(fd.cFileName) + 4 >= TOOL_OUT) { n += snprintf(out + n, cap - n, "[more files omitted]\n"); break; }
            n += snprintf(out + n, cap - n, "%s%s\n", fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "/" : "");
        } while (FindNextFileA(h, &fd));
        FindClose(h);
#else
        snprintf(out, cap, "error: list_dir not available on this system");
#endif
        if (n == 0 && !out[0]) snprintf(out, cap, "(empty)");
    } else if (strcmp(name, "write_file") == 0) {
        if (!json_str(js, end, "path", arg, sizeof arg) || !path_ok(arg)) { snprintf(out, cap, "error: invalid path (only relative paths inside the current directory)"); return; }
        if (!json_str(js, end, "content", content, sizeof content)) { snprintf(out, cap, "error: missing content"); return; }
        char what[8300];
        snprintf(what, sizeof what, "gravar o arquivo %s (%zu bytes)", arg, strlen(content));
        if (!agent_confirm(what, content)) { snprintf(out, cap, "error: the user denied this action"); return; }
        FILE *f = fopen(arg, "wb");
        if (!f) { snprintf(out, cap, "error: cannot create %s", arg); return; }
        fwrite(content, 1, strlen(content), f);
        fclose(f);
        snprintf(out, cap, "ok: wrote %zu bytes to %s", strlen(content), arg);
    } else if (strcmp(name, "run_command") == 0) {
        if (!json_str(js, end, "command", arg, sizeof arg)) { snprintf(out, cap, "error: missing command"); return; }
        if (!agent_confirm("executar o comando", arg)) { snprintf(out, cap, "error: the user denied this action"); return; }
#ifdef _WIN32
        FILE *f = _popen(arg, "r");
#else
        FILE *f = popen(arg, "r");
#endif
        if (!f) { snprintf(out, cap, "error: cannot run the command"); return; }
        size_t n = fread(out, 1, TOOL_OUT, f);
        out[n] = 0;
        char sink[512];
        int more = 0;
        while (fread(sink, 1, sizeof sink, f) > 0) more = 1;
#ifdef _WIN32
        const int rc = _pclose(f);
#else
        const int rc = pclose(f);
#endif
        snprintf(out + n, cap - n, "%s\n[exit code %d]", more ? "\n[output truncated]" : "", rc);
    } else snprintf(out, cap, "error: unknown tool %s", name);
}

static int agent(Model *m, int max_tokens, float temp) {
    const Config *c = &m->cfg;
    const Tokenizer *tk = &m->tk;
    static char line[16384], text[40000], reply[65536], result[TOOL_OUT + 512], name[64];
    const int im_end = map_get(&tk->vocab, "<|im_end|>", 10);
    int pos = 0, first = 1;
    if (c->chat_fmt != 0 || im_end < 0) die("o agente precisa de um modelo com formato ChatML (ex.: Qwen)");
    srand((unsigned)time(NULL));
    fprintf(stderr, "Agente iniciado na pasta atual (/novo recomeca, /sair termina).\n"
                    "Gravar arquivo e executar comando pedem a sua confirmacao a cada vez.\n");
    for (;;) {
        fprintf(stderr, "\n> "); fflush(stderr);
        if (!fgets(line, sizeof line, stdin)) break;
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln - 1] == '\n' || line[ln - 1] == '\r')) line[--ln] = 0;
        if (ln == 0) continue;
        if (strcmp(line, "/sair") == 0) break;
        if (strcmp(line, "/novo") == 0) { pos = 0; first = 1; fprintf(stderr, "[conversa nova]\n"); continue; }
        if (first) snprintf(text, sizeof text, "<|im_start|>system\n%s<|im_end|>\n<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n", AGENT_SYS, line);
        else snprintf(text, sizeof text, "<|im_end|>\n<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n", line);

        for (int round = 0; round < 12; round++) {      /* resposta, e de novo a cada resultado de ferramenta */
            Ids ids = {0};
            if (first && c->add_bos && c->bos >= 0) ids_push(&ids, c->bos);
            encode(tk, text, &ids);
            if (pos + ids.n + 64 >= c->ctx) {
                fprintf(stderr, "[contexto cheio (%d de %d tokens): use /novo para recomecar]\n", pos, c->ctx);
                free(ids.ids);
                break;
            }
            float *lg = forward_seq(m, ids.ids, ids.n, pos, 1);
            pos += ids.n; first = 0;
            free(ids.ids);
            size_t rl = 0;
            int gen = 0;
            const double t1 = now_sec();
            reply[0] = 0;
            while (gen < max_tokens && pos < c->ctx - 1) {
                const int t = sample(lg, c->vocab, temp);
                if (t == c->eos || t == im_end) break;
                print_token(tk, t, stdout); fflush(stdout);
                rl = tok_append(tk, t, reply, rl, sizeof reply);
                gen++;
                lg = forward(m, t, pos++, 1);
            }
            printf("\n"); fflush(stdout);
            fprintf(stderr, "[%d tokens, %.1f tok/s | contexto %d de %d]\n", gen, gen / (now_sec() - t1 + 1e-9), pos, c->ctx);

            /* chamadas de ferramenta na resposta (depois do raciocinio, se houver) */
            const char *scan = strstr(reply, "</think>");
            scan = scan ? scan : reply;
            size_t tl = (size_t)snprintf(text, sizeof text, "<|im_end|>\n<|im_start|>user");
            int calls = 0;
            for (const char *tc = strstr(scan, "<tool_call>"); tc; tc = strstr(tc + 1, "<tool_call>")) {
                const char *end = strstr(tc, "</tool_call>");
                if (!end) end = reply + rl;
                if (!json_str(tc, end, "name", name, sizeof name)) continue;
                const char *args = strstr(tc, "\"arguments\"");
                agent_tool(name, args && args < end ? args : tc, end, result, sizeof result);
                fprintf(stderr, "[ferramenta %s: %zu bytes de resultado]\n", name, strlen(result));
                tl += (size_t)snprintf(text + tl, sizeof text - tl, "\n<tool_response>\n%s\n</tool_response>", result);
                calls++;
                if (tl + TOOL_OUT + 1024 >= sizeof text) break;
            }
            if (!calls) break;
            snprintf(text + tl, sizeof text - tl, "<|im_end|>\n<|im_start|>assistant\n");
        }
    }
    moe_report();
    heat_save(m);
    return 0;
}

static int run_main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "agent") == 0) {
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        print_info(&m);
        return agent(&m, getenv("KATANA_MAXTOK") ? atoi(getenv("KATANA_MAXTOK")) : 1024,
                     getenv("KATANA_TEMP") ? (float)atof(getenv("KATANA_TEMP")) : 0.3f);
    }
    if (argc >= 5 && strcmp(argv[1], "routing") == 0) {
        Model m; memset(&m, 0, sizeof m);
        _putenv("KATANA_CPU=1");
        load_model(&m, argv[2]);
        print_info(&m);
        return routing(&m, argv[3], argv[4]);
    }
    if (argc >= 5 && strcmp(argv[1], "sparsity") == 0) {
        Model m; memset(&m, 0, sizeof m);
        _putenv("KATANA_CPU=1");
        load_model(&m, argv[2]);
        return sparsity(&m, argv[3], argv[4]);
    }
    if (argc >= 3 && strcmp(argv[1], "tune") == 0) {
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        return tune(&m);
    }
    if (argc >= 3 && strcmp(argv[1], "verify") == 0) return verify(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "pack") == 0) {
        Model m; memset(&m, 0, sizeof m);
        _putenv("KATANA_CPU=1");
        load_model(&m, argv[2]);
        return pack(&m, argv[2], m.fsize);
    }
    if (argc >= 3 && strcmp(argv[1], "micro") == 0) {
        Model m; memset(&m, 0, sizeof m);
        _putenv("KATANA_CPU=1");
        load_model(&m, argv[2]);
        return micro(&m);
    }
    if (argc >= 3 && strcmp(argv[1], "vkmicro") == 0) {
        Model m; memset(&m, 0, sizeof m);
        _putenv("KATANA_BACKEND=vulkan");
        load_model(&m, argv[2]);
        return vkmicro(&m);
    }
    gpu.debug = getenv("KATANA_DEBUG") != NULL;
    if (argc >= 3 && strcmp(argv[1], "bench") == 0) {
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        print_info(&m);
        return bench(&m, argc > 3 ? atoi(argv[3]) : 256, argc > 4 ? atoi(argv[4]) : 64);
    }
    if (argc >= 4 && strcmp(argv[1], "vkcheck") == 0) {
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        return vkcheck(&m, argv[3], argc > 4 ? atoi(argv[4]) : 40, argc > 5 ? atoi(argv[5]) : 600);
    }
    if (argc >= 3 && strcmp(argv[1], "selftest") == 0) {
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        print_info(&m);
        return selftest(&m);
    }
    if (argc >= 3 && strcmp(argv[1], "info") == 0) {
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        print_info(&m);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "chat") == 0) {       /* sem pergunta: conversa interativa */
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        print_info(&m);
        return session(&m, getenv("KATANA_SYSTEM"), getenv("KATANA_MAXTOK") ? atoi(getenv("KATANA_MAXTOK")) : 1024,
                       getenv("KATANA_TEMP") ? (float)atof(getenv("KATANA_TEMP")) : 0.7f);
    }
    if (argc >= 4 && (strcmp(argv[1], "run") == 0 || strcmp(argv[1], "chat") == 0)) {
        Model m; memset(&m, 0, sizeof m);
        load_model(&m, argv[2]);
        print_info(&m);
        int max_tokens = argc > 4 ? atoi(argv[4]) : 200;
        float temp = argc > 5 ? (float)atof(argv[5]) : 0.7f;
        if (strcmp(argv[1], "run") == 0) return generate(&m, argv[3], max_tokens, temp, 1);

        size_t n = strlen(argv[3]) + 128;
        char *p = xalloc(n, 1);
        switch (m.cfg.chat_fmt) {
            case 1:  /* Llama 3 espera <|begin_of_text|> no inicio; se o GGUF nao manda por BOS, vai no texto */
                     snprintf(p, n, "%s<|start_header_id|>user<|end_header_id|>\n\n%s<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n\n",
                              m.cfg.add_bos ? "" : "<|begin_of_text|>", argv[3]); break;
            case 2:  snprintf(p, n, "<|user|>\n%s</s>\n<|assistant|>\n", argv[3]); break;
            case 3:  snprintf(p, n, "[INST] %s [/INST]", argv[3]); break;
            case 4:  snprintf(p, n, "User: %s\nAssistant:", argv[3]); break;
            default: snprintf(p, n, "<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n", argv[3]); break;
        }
        return generate(&m, p, max_tokens, temp, 0);
    }
    fprintf(stderr,
        "Uso:\n"
        "  katana info <modelo.gguf>\n"
        "  katana selftest <modelo.gguf>\n"
        "  katana bench <modelo.gguf> [tokens_prompt] [tokens_geracao]\n"
        "  katana tune <modelo.gguf>      mede e grava o melhor ajuste para a GPU desta maquina\n"
        "  katana pack <modelo.gguf>      gera os shards no layout da GPU (<modelo>.gguf.ktn)\n"
        "  katana verify <modelo.gguf>    confere os hashes dos shards\n"
        "  katana sparsity <modelo.gguf> <calib.txt> <aval.txt>   esparsidade por assunto (modelo denso)\n"
        "  katana routing  <modelo.gguf> <calib.txt> <aval.txt>   especialistas por assunto (modelo MoE)\n"
        "  katana run  <modelo.gguf> \"texto\" [max_tokens] [temperatura]\n"
        "  katana chat <modelo.gguf> \"pergunta\" [max_tokens] [temperatura]\n"
        "  katana agent <modelo.gguf>     conversa com ferramentas (ler, listar, gravar e executar com confirmacao)\n"
        "  katana chat <modelo.gguf>      conversa interativa de varios turnos (KATANA_SYSTEM = prompt de sistema)\n");
    return 1;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    /* argumentos e saida em UTF-8 no Windows */
    int wargc;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        char **u8 = xalloc(wargc + 1, sizeof(char *));
        for (int i = 0; i < wargc; i++) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
            u8[i] = xalloc(n, 1);
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, u8[i], n, NULL, NULL);
        }
        argc = wargc; argv = u8;
    }
    SetConsoleOutputCP(CP_UTF8);
#endif
    pool_init();
    return run_main(argc, argv);
}
