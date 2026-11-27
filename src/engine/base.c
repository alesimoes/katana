/*
 * katana.c - motor de inferencia em C puro para modelos abertos no formato GGUF.
 * Sem dependencias externas (so libc; AVX2 e OpenMP sao opcionais).
 *
 * Suporta: arquiteturas "llama" e "qwen2" (RMSNorm, RoPE, GQA, SwiGLU),
 *          tensores F32/F16/Q8_0/Q4_K/Q6_K (os quantizados sao usados direto do
 *          arquivo mapeado em memoria, sem conversao), tokenizador BPE lido do GGUF.
 *
 * Uso:
 *   katana models [pasta]         lista os GGUF numa tabela (padrao: models/) (data, tamanho, algoritmo, como roda)
  katana info <modelo.gguf>
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
#include <ctype.h>
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
#include <dirent.h>
#include <stdarg.h>
#include <sys/stat.h>
#define _putenv(s) putenv((char *)(s))
#endif
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>                 /* inet_pton, socklen_t (servidor) */
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/time.h>
#endif
/* camada Vulkan (backend opcional, KATANA_BACKEND=vulkan); o kvk.h deixa as macros G e F, removidas aqui */
#define KVK_IMPL
#include "vk/kvk.h"
#include "qtabs.h"
#undef G
#undef F
#include "vk/shaders/mv_f32.h"
#include "vk/shaders/mv_q8.h"
#include "vk/shaders/mv_q51.h"
#include "vk/shaders/mv_q4k.h"
#include "vk/shaders/mv_q5k.h"
#include "vk/shaders/mv_q6k.h"
#include "vk/shaders/mv_q2k.h"
#include "vk/shaders/rms_wg.h"
#include "vk/shaders/moe_sum.h"
#include "vk/shaders/moe_sum_rms.h"
#include "vk/shaders/moe_router.h"
#include "vk/shaders/att1f_kv32.h"
#include "vk/shaders/att1f_kv16.h"
#include "vk/shaders/att1r.h"
#include "vk/shaders/dn_step.h"
#include "vk/shaders/dn_step_sig.h"
#include "vk/shaders/q4mv.h"
#include "vk/shaders/q4hc.h"
#include "vk/shaders/dn_prep.h"
#include "vk/shaders/dn_cstate.h"
#include "vk/shaders/dn_chunk.h"
#include "vk/shaders/dn_state.h"
#include "vk/shaders/dn_norm.h"
#include "vk/shaders/dn_seq.h"
#include "vk/shaders/rms_wg32.h"
#include "vk/shaders/qn_gate.h"
#include "vk/shaders/moe_gath.h"
#include "vk/shaders/moe_scat.h"
#include "vk/shaders/mla_att.h"
#include "vk/shaders/fx_tp16.h"
#include "vk/shaders/fx_tp32.h"
#include "vk/shaders/fx_ln.h"
#include "vk/shaders/fx_prep.h"
#include "vk/shaders/fx_att.h"
#include "vk/shaders/fx_swi.h"
#include "vk/shaders/fx_axg.h"
#include "vk/shaders/fx_tpb.h"
#include "vk/shaders/fx_tpbr.h"
#include "vk/shaders/fx_im2col.h"
#include "vk/shaders/fx_gn0.h"
#include "vk/shaders/fx_gn1.h"
#include "vk/shaders/fx_gn2.h"
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
#include "vk/shaders/mm_t0_64x64_ws.h"
#include "vk/shaders/mm_t0_64x32w4_ws.h"
#include "vk/shaders/mm_t0_64x16w4_ws.h"
#include "vk/shaders/mm_t1_64x64_ws.h"
#include "vk/shaders/mm_t1_64x32w4_ws.h"
#include "vk/shaders/mm_t1_64x16w4_ws.h"
#include "vk/shaders/mm_t2_64x64_ws.h"
#include "vk/shaders/mm_t2_64x32w4_ws.h"
#include "vk/shaders/mm_t2_64x16w4_ws.h"
#include "vk/shaders/mm_t3_64x64_ws.h"
#include "vk/shaders/mm_t3_64x32w4_ws.h"
#include "vk/shaders/mm_t3_64x16w4_ws.h"
#include "vk/shaders/mm_t4_64x64_ws.h"
#include "vk/shaders/mm_t4_64x32w4_ws.h"
#include "vk/shaders/mm_t4_64x16w4_ws.h"
#include "vk/shaders/mm_t0_64x64_gx.h"
#include "vk/shaders/mm_t0_64x32_gx.h"
#include "vk/shaders/mm_t0_64x16_gx.h"
#include "vk/shaders/mm_t1_64x64_gx.h"
#include "vk/shaders/mm_t1_64x32_gx.h"
#include "vk/shaders/mm_t1_64x16_gx.h"
#include "vk/shaders/mm_t2_64x64_gx.h"
#include "vk/shaders/mm_t2_64x32_gx.h"
#include "vk/shaders/mm_t2_64x16_gx.h"
#include "vk/shaders/mm_t3_64x64_gx.h"
#include "vk/shaders/mm_t3_64x32_gx.h"
#include "vk/shaders/mm_t3_64x16_gx.h"
#include "vk/shaders/mm_t4_64x64_gx.h"
#include "vk/shaders/mm_t4_64x32_gx.h"
#include "vk/shaders/mm_t4_64x16_gx.h"
#include "vk/shaders/moe_gath32.h"
#include "vk/shaders/qn_gate32.h"
#include "vk/shaders/att_flash32_kv16.h"
#include "vk/shaders/att_flash32_kv32.h"
#include "vk/shaders/mm_t0_64x64_xs.h"
#include "vk/shaders/mm_t0_64x32w4_xs.h"
#include "vk/shaders/mm_t0_64x16w4_xs.h"
#include "vk/shaders/mm_t1_64x64_xs.h"
#include "vk/shaders/mm_t1_64x32w4_xs.h"
#include "vk/shaders/mm_t1_64x16w4_xs.h"
#include "vk/shaders/mm_t2_64x64_xs.h"
#include "vk/shaders/mm_t2_64x32w4_xs.h"
#include "vk/shaders/mm_t2_64x16w4_xs.h"
#include "vk/shaders/mm_t3_64x64_xs.h"
#include "vk/shaders/mm_t3_64x32w4_xs.h"
#include "vk/shaders/mm_t3_64x16w4_xs.h"
#include "vk/shaders/mm_t4_64x64_xs.h"
#include "vk/shaders/mm_t4_64x32w4_xs.h"
#include "vk/shaders/mm_t4_64x16w4_xs.h"
#include "vk/shaders/mm_t5_64x64_g0.h"
#include "vk/shaders/mm_t5_64x64_g1.h"
#include "vk/shaders/mm_t5_64x32w4_g0.h"
#include "vk/shaders/mm_t5_64x32_g1.h"
#include "vk/shaders/mm_t5_64x16w4_g0.h"
#include "vk/shaders/mm_t5_64x16_g1.h"
#include "vk/shaders/mm_t5_64x64_ws.h"
#include "vk/shaders/mm_t5_64x32w4_ws.h"
#include "vk/shaders/mm_t5_64x16w4_ws.h"
#include "vk/shaders/mm_t5_64x64_gx.h"
#include "vk/shaders/mm_t5_64x32_gx.h"
#include "vk/shaders/mm_t5_64x16_gx.h"
#include "vk/shaders/mm_t5_64x64_xs.h"
#include "vk/shaders/mm_t5_64x32w4_xs.h"
#include "vk/shaders/mm_t5_64x16w4_xs.h"
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
#define TOP_K 64         /* teto de candidatos guardados; g_top_k diz quantos valem */

static void die(const char *msg) { fprintf(stderr, "Erro: %s\n", msg); exit(1); }

/* caminho real deste executavel (links simbolicos resolvidos) e a pasta dele; 0 se nao der. Serve para achar o que fica ao
   lado do programa (models, katana.cfg) de qualquer pasta de trabalho */
static int exe_path(char *out, size_t cap) {
#ifdef _WIN32
    const DWORD l = GetModuleFileNameA(NULL, out, (DWORD)cap);
    return l > 0 && l < cap;
#else
    char t[4096], r[4096];
    const ssize_t n = readlink("/proc/self/exe", t, sizeof t - 1);
    if (n <= 0) return 0;
    t[n] = 0;
    char *tail = strstr(t, " (deleted)");
    if (tail) *tail = 0;
    if (realpath(t, r)) snprintf(out, cap, "%s", r); else snprintf(out, cap, "%s", t);
    return 1;
#endif
}
static int exe_dir(char *out, size_t cap) {
    if (!exe_path(out, cap)) return 0;
    char *s = strrchr(out, '/'), *b = strrchr(out, '\\');
    if (b && (!s || b > s)) s = b;
    if (!s) return 0;
    *s = 0;
    return 1;
}

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
#if defined(__x86_64__) || defined(__i386__)
#define CPU_RELAX() __builtin_ia32_pause()
#else
#define CPU_RELAX() ((void)0)      /* porte ARM, 2026-11-27: so um espera ocupada, sem instrucao equivalente obrigatoria */
#endif
typedef long atom_t;
#endif

static struct {
    int n;                              /* total de threads, contando a principal */
    volatile atom_t gen, open, active, next;
    PoolFn fn; void *ctx; int total, chunk;
    volatile atom_t st_jobs, st_main, st_work, st_sleep;   /* estatistica: trabalhos, blocos da principal e das outras, sonos */
#ifndef _WIN32
    pthread_mutex_t mu; pthread_cond_t cv;
    pthread_mutex_t use;                /* um dono por vez: o carregamento do embedder em outra thread tambem chama pool_for */
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
#ifndef _WIN32
    pthread_mutex_lock(&pool.use);
#endif
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
#ifndef _WIN32
    pthread_mutex_unlock(&pool.use);
#endif
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
    pthread_mutex_init(&pool.mu, NULL); pthread_mutex_init(&pool.use, NULL); pthread_cond_init(&pool.cv, NULL);
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

