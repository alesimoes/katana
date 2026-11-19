/* ---------- maquina, catalogo de modelos e download ----------
 * katana hw [pasta]            mostra a maquina (RAM, GPU, VRAM, nucleos, disco livre na pasta de modelos)
 * katana catalog [pasta]       lista os modelos do catalogo: o que ja esta na pasta, o que cabe e o recomendado
 * katana pull <id> [pasta]     baixa o modelo (curl, retoma download interrompido), confere o sha256 e renomeia
 * A saida e feita para pessoa e para a interface: uma linha por item, campos separados por TAB.
 * O catalogo e fixo no motor: cada item aponta para uma revisao exata do repositorio e tem o sha256 do arquivo.
 * So entram modelos com especialistas, medidos com as ferramentas do agente; requisito minimo: 24 GB de memoria
 * (decisao do dono em 2026-11-19: modelos densos pequenos ficaram de fora). */

#ifndef _WIN32
#include <sys/statvfs.h>
#endif

typedef struct {
    const char *id, *name, *file, *repo, *rev, *sha;
    long long bytes;
    int tier;               /* 1 leve, 2 medio, 3 grande: quanto maior, melhor a resposta */
    int moe;                /* especialistas: so uma parte dos pesos trabalha por token (roda bem com pouca VRAM) */
    const char *note;       /* para quem serve */
} CatItem;

static const CatItem g_cat[] = {
    { "gemma-4-26b", "Gemma 4 26B A4B", "gemma-4-26B-A4B-it-UD-Q4_K_M.gguf", "unsloth/gemma-4-26B-A4B-it-GGUF", "c099eb48e663fd284577b04978a94ffccb261841",
      "f2c28b3dc4776931ac6f879e11f203dec637ea0f14267a86ec8f6165f63f293f", 16947541728LL, 3, 1,
      "24 GB de memoria ou mais; usa a placa de video que houver" },
    { "qwen3.6-35b", "Qwen3.6 35B A3B", "Qwen3.6-35B-A3B-UD-Q4_K_M.gguf", "unsloth/Qwen3.6-35B-A3B-GGUF", "a483e9e6cbd595906af30beda3187c2663a1118c",
      "ac0e2c1189e055faa36eff361580e79c5bd6f8e76bffb4ce547f167d53e31a61", 22134528992LL, 3, 1,
      "32 GB de memoria ou mais; o melhor com ferramentas e programacao" },
};
#define N_CAT ((int)(sizeof g_cat / sizeof g_cat[0]))

/* sha256 de um arquivo (funcoes de devtools.c), com linhas de progresso (VERIFY <feito>) a cada 512 MB; 0 = nao abriu */
static int sha_file(const char *path, char out[65], int progress) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    Sha2 s; sha2_init(&s);
    static unsigned char b[1 << 20];
    size_t n; long long done = 0, mark = 0;
    while ((n = fread(b, 1, sizeof b, f)) > 0) {
        sha2_add(&s, b, n); done += (long long)n;
        if (progress && done - mark >= (1LL << 29)) { mark = done; printf("VERIFY\t%lld\n", done); fflush(stdout); }
    }
    fclose(f);
    sha2_hex(&s, out);
    return 1;
}

/* ---- maquina ---- */
typedef struct { double ram_gb, avail_gb, vram_gb, disk_gb; int cores; char gpu[256]; } HwInfo;

static void hw_probe(HwInfo *h, const char *dir) {
    memset(h, 0, sizeof *h);
#ifdef _WIN32
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) { h->ram_gb = ms.ullTotalPhys / 1073741824.0; h->avail_gb = ms.ullAvailPhys / 1073741824.0; }
    SYSTEM_INFO si; GetSystemInfo(&si); h->cores = (int)si.dwNumberOfProcessors;
    ULARGE_INTEGER fr;
    if (GetDiskFreeSpaceExA(dir, &fr, NULL, NULL)) h->disk_gb = fr.QuadPart / 1073741824.0;
#else
    h->ram_gb = (double)sysconf(_SC_PHYS_PAGES) * (double)sysconf(_SC_PAGE_SIZE) / 1073741824.0;
    h->avail_gb = h->ram_gb;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char l[256]; long long kb;
        while (fgets(l, sizeof l, f)) if (sscanf(l, "MemAvailable: %lld", &kb) == 1) h->avail_gb = kb / 1048576.0;
        fclose(f);
    }
    h->cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    struct statvfs sv;
    if (statvfs(dir, &sv) == 0) h->disk_gb = (double)sv.f_bavail * (double)sv.f_frsize / 1073741824.0;
#endif
    KvkInfo I;
    if (kvk_init(&I)) { snprintf(h->gpu, sizeof h->gpu, "%s", I.name); h->vram_gb = I.vram_bytes / 1073741824.0; }
}

/* como o item roda nesta maquina: 2 = bem, 1 = roda devagar, 0 = nao cabe. O arquivo e mapeado na memoria: precisa
   caber na RAM com folga para o sistema. Modelo denso fora da placa roda na CPU (devagar acima de ~5 GB); com
   especialistas so uma parte dos pesos trabalha por token, entao a placa pequena ja ajuda. */
static int cat_fit(const CatItem *c, const HwInfo *h) {
    const double gb = c->bytes / 1073741824.0;
    if (gb + 2.0 > h->ram_gb) return 0;
    if (c->moe) return gb * 1.4 <= h->ram_gb ? 2 : 1;
    if (h->vram_gb >= gb + 1.0) return 2;
    return gb <= 3.5 ? 2 : 1;
}

static const CatItem *cat_find(const char *id) {
    for (int i = 0; i < N_CAT; i++)
        if (!strcmp(g_cat[i].id, id) || !strcmp(g_cat[i].file, id)) return &g_cat[i];
    return NULL;
}

/* recomendado: o de maior nivel que roda bem; empate fica com o ultimo da lista (o mais testado com ferramentas) */
static const CatItem *cat_best(const HwInfo *h) {
    const CatItem *b = NULL;
    for (int i = 0; i < N_CAT; i++)
        if (cat_fit(&g_cat[i], h) == 2 && (!b || g_cat[i].tier >= b->tier)) b = &g_cat[i];
    if (!b) for (int i = 0; i < N_CAT; i++) if (cat_fit(&g_cat[i], h) == 1 && !b) b = &g_cat[i];
    return b;
}

static long long file_bytes(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? (long long)st.st_size : -1;
}

static int hw_cmd(const char *dir) {
    HwInfo h; hw_probe(&h, dir);
    printf("ram_gb\t%.1f\navail_gb\t%.1f\ncores\t%d\ngpu\t%s\nvram_gb\t%.1f\ndisk_gb\t%.1f\nmodels\t%s\n", h.ram_gb, h.avail_gb, h.cores,
           h.gpu[0] ? h.gpu : "-", h.vram_gb, h.disk_gb, dir);
    return 0;
}

/* uma linha por modelo: id, nome, arquivo, bytes, estado (installed|partial|absent), bytes ja baixados, cabe (2|1|0),
   recomendado (1|0), nota */
static int catalog_cmd(const char *dir) {
    HwInfo h; hw_probe(&h, dir);
    const CatItem *best = cat_best(&h);
    char p[4200];
    for (int i = 0; i < N_CAT; i++) {
        const CatItem *c = &g_cat[i];
        snprintf(p, sizeof p, "%s/%s", dir, c->file);
        long long have = file_bytes(p);
        const char *st = "absent";
        if (have == c->bytes) st = "installed";
        else {
            snprintf(p, sizeof p, "%s/%s.part", dir, c->file);
            have = file_bytes(p);
            if (have > 0) st = "partial"; else have = 0;
        }
        printf("%s\t%s\t%s\t%lld\t%s\t%lld\t%d\t%d\t%s\n", c->id, c->name, c->file, c->bytes, st, have, cat_fit(c, &h), c == best, c->note);
    }
    return 0;
}

#ifndef _WIN32
static volatile pid_t g_pull_pid;
static void pull_sig(int s) { if (g_pull_pid > 0) kill(g_pull_pid, SIGTERM); signal(s, SIG_DFL); raise(s); }
#endif

/* baixa o item para a pasta. Linhas na saida: PROGRESS <bytes> <total> <bytes por segundo>, VERIFY <bytes>,
   DONE <arquivo> ou ERROR <motivo>. O arquivo parcial (.part) fica na pasta quando o download para: a proxima
   chamada continua de onde parou. KATANA_PULL_URL troca a origem (testes). */
static int pull_cmd(const char *id, const char *dir) {
    const CatItem *c = cat_find(id);
    if (!c) { printf("ERROR\tmodelo desconhecido: %s (veja katana catalog)\n", id); return 1; }
    char dst[4200], part[4300], url[1024], hex[65];
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    snprintf(dst, sizeof dst, "%s/%s", dir, c->file);
    snprintf(part, sizeof part, "%s.part", dst);
    if (file_bytes(dst) == c->bytes) { printf("DONE\t%s\n", dst); return 0; }
    HwInfo h; hw_probe(&h, dir);
    const long long have0 = file_bytes(part) > 0 ? file_bytes(part) : 0;
    if (h.disk_gb > 0 && (c->bytes - have0) / 1073741824.0 + 0.5 > h.disk_gb) {
        printf("ERROR\tespaco em disco insuficiente: faltam %.1f GB e a pasta tem %.1f GB livres\n", (c->bytes - have0) / 1073741824.0, h.disk_gb);
        return 1;
    }
    const char *alt = getenv("KATANA_PULL_URL");
    if (alt && alt[0]) snprintf(url, sizeof url, "%s", alt);
    else snprintf(url, sizeof url, "https://huggingface.co/%s/resolve/%s/%s", c->repo, c->rev, c->file);
    fflush(stdout);
    int rc = 1;
    if (have0 == c->bytes) rc = 0;          /* parcial ja completo: so falta conferir */
    else {
#ifdef _WIN32
        char cmd[6000];
        snprintf(cmd, sizeof cmd, "curl -sS -L --fail --retry 3 -C - -o \"%s\" \"%s\"", part, url);
        rc = system(cmd);
#else
        signal(SIGTERM, pull_sig); signal(SIGINT, pull_sig);
        const pid_t pid = fork();
        if (pid == 0) {
            execlp("curl", "curl", "-sS", "-L", "--fail", "--retry", "3", "--connect-timeout", "20", "-C", "-", "-o", part, url, (char *)NULL);
            _exit(127);
        }
        if (pid < 0) { printf("ERROR\tnao foi possivel iniciar o curl\n"); return 1; }
        g_pull_pid = pid;
        long long last = have0;
        int st = 0;
        for (;;) {
            const pid_t w = waitpid(pid, &st, WNOHANG);
            const long long now = file_bytes(part) > 0 ? file_bytes(part) : 0;
            printf("PROGRESS\t%lld\t%lld\t%lld\n", now, c->bytes, now - last);
            fflush(stdout);
            last = now;
            if (w == pid) break;
            sleep(1);
        }
        g_pull_pid = 0;
        rc = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
        if (rc == 127) { printf("ERROR\to programa curl nao esta instalado\n"); return 1; }
#endif
    }
    const long long got = file_bytes(part);
    if (rc != 0 && got != c->bytes) {
        printf("ERROR\tdownload interrompido (curl %d) em %.2f de %.2f GB; chame de novo para continuar\n", rc, (got > 0 ? got : 0) / 1073741824.0, c->bytes / 1073741824.0);
        return 1;
    }
    if (alt && alt[0]) { rename(part, dst); printf("DONE\t%s\n", dst); return 0; }      /* origem de teste: sem conferencia */
    if (got != c->bytes || !sha_file(part, hex, 1) || strcmp(hex, c->sha) != 0) {
        remove(part);
        printf("ERROR\to arquivo baixado nao confere com o esperado (tamanho ou sha256); foi apagado, baixe de novo\n");
        return 1;
    }
    if (rename(part, dst) != 0) { printf("ERROR\tnao foi possivel gravar %s\n", dst); return 1; }
    printf("DONE\t%s\n", dst);
    return 0;
}

/* pasta de modelos quando nenhuma e dada: KATANA_MODELS, ./models se existir, senao ~/.katana/models */
static const char *models_dir(void) {
    static char d[4096];
    const char *e = getenv("KATANA_MODELS");
    struct stat st;
    if (e && e[0]) return e;
    if (stat("models", &st) == 0) return "models";
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home) home = getenv("USERPROFILE");
#endif
    snprintf(d, sizeof d, "%s/.katana", home ? home : ".");
#ifdef _WIN32
    _mkdir(d);
#else
    mkdir(d, 0755);
#endif
    snprintf(d, sizeof d, "%s/.katana/models", home ? home : ".");
#ifdef _WIN32
    _mkdir(d);
#else
    mkdir(d, 0755);
#endif
    return d;
}
