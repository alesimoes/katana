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

/* organizacao no Hugging Face que hospeda os arquivos convertidos pelo katana (embedder, imagem). */
#define KATANA_HF_ORG "alesimoes"
/* revisoes exatas (hash do commit) dos repositorios com os arquivos convertidos, enviados em 2026-11-27 (privados por enquanto:
   o download so funciona com token ate o repositorio ficar publico). O sha256 e o tamanho abaixo sao conferidos no download.
   O sd15 ainda nao foi enviado (licenca em revisao, ver tools/release/LICENSES.md): a revisao "main" e provisoria. */
#define KATANA_REV_FLUX "97cc03901b5925c3fe372a63407c9375442fd92c"
#define KATANA_REV_EMBED "e2057c5b795ebbe0890c8bebe609c53c26792a68"
#define KATANA_HF_REV "main"

typedef struct { const char *remote, *dest, *sha; long long bytes; } CatFile;   /* remote: caminho no repositorio; dest: caminho dentro da pasta de modelos */

typedef struct {
    const char *id, *name, *kind;       /* kind: llm | vision | embedder | image */
    const char *repo, *rev;
    const CatFile *files; int nfiles;
    int tier;               /* 1 leve, 2 medio, 3 grande: quanto maior, melhor a resposta */
    int moe;                /* especialistas: so uma parte dos pesos trabalha por token (roda bem com pouca VRAM) */
    const char *note;       /* para quem serve */
} CatItem;

static const CatFile cf_gemma[]  = { { "gemma-4-26B-A4B-it-UD-Q4_K_M.gguf", "gemma-4-26B-A4B-it-UD-Q4_K_M.gguf", "f2c28b3dc4776931ac6f879e11f203dec637ea0f14267a86ec8f6165f63f293f", 16947541728LL } };
static const CatFile cf_qwen[]   = { { "Qwen3.6-35B-A3B-UD-Q4_K_M.gguf", "Qwen3.6-35B-A3B-UD-Q4_K_M.gguf", "ac0e2c1189e055faa36eff361580e79c5bd6f8e76bffb4ce547f167d53e31a61", 22134528992LL } };
/* projetor de visao: arquivo original do repositorio da Unsloth, sem conversao; fica ao lado do modelo (mmproj*.gguf) */
static const CatFile cf_vision[] = { { "mmproj-F16.gguf", "mmproj-Qwen3.6-35B-A3B-F16.gguf", "8971ee4f331ff0a4c609374f32984b3d4e6dc086c0aa35f1d637fad1829e887f", 899283680LL } };
/* o motor procura o embedder em embeddinggemma-2/embeddinggemma-2-q8_0.gguf (src/app/embsvc.c) */
static const CatFile cf_embed[]  = { { "embeddinggemma-2-q8_0.gguf", "embeddinggemma-2/embeddinggemma-2-q8_0.gguf", "999c356d24986c25ead5cfc7b5b5ba5dd90f2633dbf5fc959be97360b82e401c", 302907044LL } };
/* o motor procura o FLUX.2 klein em flux2klein/gguf/ (src/tools/render.c, src/render/fxgen.c) */
static const CatFile cf_flux[]   = {
    { "flux-2-klein-4b-Q4_K_M.gguf", "flux2klein/gguf/flux-2-klein-4b-Q4_K_M.gguf", "0b25d143c8469b342bc5af3bce92b783bf6b0636d285f7b2f75e38af63af9a15", 2604311104LL },
    { "flux2-vae.gguf", "flux2klein/gguf/flux2-vae.gguf", "e43c1074d0a80562e7639d6c82849f3af7693f55785add404f23ab1bda511f3f", 336206784LL },
    { "qwen3-4b-text.gguf", "flux2klein/gguf/qwen3-4b-text.gguf", "3a390a0869d9507a1c80a2cbf1dbe5673b6ff8630030d86fd502cdb605e7b270", 6234412640LL } };
/* Stable Diffusion 1.5 (render/sd15-*.gguf) fica fora do catalogo ate a licenca (OpenRAIL) ser revista: ver tools/release/LICENSES.md */

#define CF(a) a, (int)(sizeof a / sizeof a[0])
static const CatItem g_cat[] = {
    { "gemma-4-26b", "Gemma 4 26B A4B", "llm", "unsloth/gemma-4-26B-A4B-it-GGUF", "c099eb48e663fd284577b04978a94ffccb261841", CF(cf_gemma), 3, 1,
      "24 GB de memoria ou mais; usa a placa de video que houver" },
    { "qwen3.6-35b", "Qwen3.6 35B A3B", "llm", "unsloth/Qwen3.6-35B-A3B-GGUF", "a483e9e6cbd595906af30beda3187c2663a1118c", CF(cf_qwen), 3, 1,
      "32 GB de memoria ou mais; o melhor com ferramentas e programacao" },
    { "qwen3.6-vision", "Qwen3.6 vision projector", "vision", "unsloth/Qwen3.6-35B-A3B-GGUF", "a483e9e6cbd595906af30beda3187c2663a1118c", CF(cf_vision), 0, 0,
      "deixa o Qwen3.6 ler imagens e capturas de tela" },
    { "embeddinggemma-2", "EmbeddingGemma 2 (q8_0)", "embedder", KATANA_HF_ORG "/katana-embeddinggemma", KATANA_REV_EMBED, CF(cf_embed), 0, 0,
      "busca por significado nos arquivos, na memoria e nas sessoes" },
    { "flux2-klein", "FLUX.2 klein 4B (Q4_K_M)", "image", KATANA_HF_ORG "/katana-flux2-klein", KATANA_REV_FLUX, CF(cf_flux), 0, 0,
      "gera imagens de alta qualidade; precisa de uma placa de video" },
};
#define N_CAT ((int)(sizeof g_cat / sizeof g_cat[0]))

/* sha256 de um arquivo (funcoes de devtools.c), com linhas de progresso (VERIFY <feito>) a cada 512 MB; 0 = nao abriu */
static int g_pull_human;     /* 1: o setup mostra progresso para pessoa em vez das linhas PROGRESS/VERIFY */
static int sha_file(const char *path, char out[65], int progress) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    Sha2 s; sha2_init(&s);
    static unsigned char b[1 << 20];
    size_t n; long long done = 0, mark = 0;
    while ((n = fread(b, 1, sizeof b, f)) > 0) {
        sha2_add(&s, b, n); done += (long long)n;
        if (progress && done - mark >= (1LL << 29)) { mark = done; if (g_pull_human) printf("\r  verificando... %.1f GB   ", done / 1073741824.0); else printf("VERIFY\t%lld\n", done); fflush(stdout); }
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
    if (strcmp(c->kind, "llm") != 0) return 2;      /* extras: o que limita e o disco (o setup confere) */
    const double gb = c->files[0].bytes / 1073741824.0;
    if (gb + 2.0 > h->ram_gb) return 0;
    if (c->moe) return gb * 1.4 <= h->ram_gb ? 2 : 1;
    if (h->vram_gb >= gb + 1.0) return 2;
    return gb <= 3.5 ? 2 : 1;
}

static const CatItem *cat_find(const char *id) {
    for (int i = 0; i < N_CAT; i++)
        if (!strcmp(g_cat[i].id, id) || !strcmp(g_cat[i].files[0].dest, id)) return &g_cat[i];
    return NULL;
}

/* recomendado: o de maior nivel que roda bem; empate fica com o ultimo da lista (o mais testado com ferramentas) */
static const CatItem *cat_best(const HwInfo *h) {
    const CatItem *b = NULL;
    for (int i = 0; i < N_CAT; i++)
        if (!strcmp(g_cat[i].kind, "llm") && cat_fit(&g_cat[i], h) == 2 && (!b || g_cat[i].tier >= b->tier)) b = &g_cat[i];
    if (!b) for (int i = 0; i < N_CAT; i++) if (!strcmp(g_cat[i].kind, "llm") && cat_fit(&g_cat[i], h) == 1 && !b) b = &g_cat[i];
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

static long long cat_total(const CatItem *c) { long long t = 0; for (int k = 0; k < c->nfiles; k++) t += c->files[k].bytes; return t; }

/* estado do item: bytes ja no disco (arquivos completos e parciais .part) e se esta tudo instalado */
static long long cat_have(const CatItem *c, const char *dir, int *installed) {
    long long have = 0; int all = 1;
    char p[4400];
    for (int k = 0; k < c->nfiles; k++) {
        snprintf(p, sizeof p, "%s/%s", dir, c->files[k].dest);
        long long b = file_bytes(p);
        if (b == c->files[k].bytes) { have += b; continue; }
        all = 0;
        snprintf(p, sizeof p, "%s/%s.part", dir, c->files[k].dest);
        b = file_bytes(p);
        if (b > 0 && b <= c->files[k].bytes) have += b;
    }
    if (installed) *installed = all;
    return have;
}

/* uma linha por item: id, nome, arquivo (o primeiro), bytes (soma dos arquivos), estado (installed|partial|absent), bytes ja
   baixados, cabe (2|1|0), recomendado (1|0), nota, tipo (llm|vision|embedder|image: coluna nova, sempre a ultima) */
static int catalog_cmd(const char *dir) {
    HwInfo h; hw_probe(&h, dir);
    const CatItem *best = cat_best(&h);
    for (int i = 0; i < N_CAT; i++) {
        const CatItem *c = &g_cat[i];
        int inst; const long long have = cat_have(c, dir, &inst);
        const char *st = inst ? "installed" : have > 0 ? "partial" : "absent";
        printf("%s\t%s\t%s\t%lld\t%s\t%lld\t%d\t%d\t%s\t%s\n", c->id, c->name, c->files[0].dest, cat_total(c), st, have, cat_fit(c, &h), c == best, c->note, c->kind);
    }
    return 0;
}

#ifndef _WIN32
static volatile pid_t g_pull_pid;
static void pull_sig(int s) { if (g_pull_pid > 0) kill(g_pull_pid, SIGTERM); signal(s, SIG_DFL); raise(s); }
#endif

static void mkdirs(const char *path) {
    char b[4300]; snprintf(b, sizeof b, "%s", path);
    for (char *q = b + 1; *q; q++) if (*q == '/') { *q = 0;
#ifdef _WIN32
        _mkdir(b);
#else
        mkdir(b, 0755);
#endif
        *q = '/'; }
#ifdef _WIN32
    _mkdir(b);
#else
    mkdir(b, 0755);
#endif
}

/* baixa o item para a pasta. Linhas na saida: PROGRESS <bytes> <total> <bytes por segundo>, VERIFY <bytes>,
   DONE <arquivo> ou ERROR <motivo>. O arquivo parcial (.part) fica na pasta quando o download para: a proxima
   chamada continua de onde parou. KATANA_PULL_URL troca a origem (testes); o sha256 e sempre conferido. */
static int pull_file(const CatItem *c, const CatFile *f, const char *dir) {
    char dst[4200], part[4300], url[1024], hex[65];
    mkdirs(dir);
    snprintf(dst, sizeof dst, "%s/%s", dir, f->dest);
    { char d2[4200]; snprintf(d2, sizeof d2, "%s", dst); char *sl = strrchr(d2, '/'); if (sl) { *sl = 0; mkdirs(d2); } }
    snprintf(part, sizeof part, "%s.part", dst);
    if (file_bytes(dst) == f->bytes) { { if (g_pull_human) printf("\r  ok: %s%30s\n", f->dest, ""); else printf("DONE\t%s\n", dst); } return 0; }
    HwInfo h; hw_probe(&h, dir);
    const long long have0 = file_bytes(part) > 0 ? file_bytes(part) : 0;
    if (h.disk_gb > 0 && (f->bytes - have0) / 1073741824.0 + 0.5 > h.disk_gb) {
        printf("ERROR\tespaco em disco insuficiente: faltam %.1f GB e a pasta tem %.1f GB livres\n", (f->bytes - have0) / 1073741824.0, h.disk_gb);
        return 1;
    }
    const char *alt = getenv("KATANA_PULL_URL");
    if (alt && alt[0]) snprintf(url, sizeof url, "%s", alt);
    else snprintf(url, sizeof url, "https://huggingface.co/%s/resolve/%s/%s", c->repo, c->rev, f->remote);
    fflush(stdout);
    int rc = 1;
    if (have0 == f->bytes) rc = 0;          /* parcial ja completo: so falta conferir */
    else {
#ifdef _WIN32
        char cmd[6000];
        snprintf(cmd, sizeof cmd, "curl -sS -L --fail --retry 3 -C - -o \"%s\" \"%s\"", part, url);
        rc = system(cmd);
#else
        signal(SIGTERM, pull_sig); signal(SIGINT, pull_sig);
        const pid_t pid = fork();
        if (pid == 0) {
            /* repositorio privado: HF_TOKEN vira o cabecalho de autorizacao (so para o Hugging Face) */
            const char *tok = getenv("HF_TOKEN");
            char auth[600];
            if (tok && *tok && strstr(url, "://huggingface.co/")) {
                snprintf(auth, sizeof auth, "Authorization: Bearer %s", tok);
                execlp("curl", "curl", "-sS", "-L", "--fail", "--retry", "3", "--connect-timeout", "20", "-C", "-", "-H", auth, "-o", part, url, (char *)NULL);
            }
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
            if (g_pull_human) printf("\r  %s: %.2f de %.2f GB (%.0f MB/s)   ", f->dest, now / 1073741824.0, f->bytes / 1073741824.0, (now - last) / 1048576.0);
        else printf("PROGRESS\t%lld\t%lld\t%lld\n", now, f->bytes, now - last);
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
    if (rc != 0 && got != f->bytes) {
        printf("ERROR\tdownload interrompido (curl %d) em %.2f de %.2f GB; chame de novo para continuar\n", rc, (got > 0 ? got : 0) / 1073741824.0, f->bytes / 1073741824.0);
        return 1;
    }
    if (!f->sha || !f->sha[0]) { remove(part); printf("ERROR\tcatalogo sem sha256 para %s: o download nao e aceito sem conferencia\n", f->dest); return 1; }
    if (got != f->bytes || !sha_file(part, hex, 1) || strcmp(hex, f->sha) != 0) {
        remove(part);
        printf("ERROR\to arquivo baixado nao confere com o esperado (tamanho ou sha256); foi apagado, baixe de novo\n");
        return 1;
    }
    if (rename(part, dst) != 0) { printf("ERROR\tnao foi possivel gravar %s\n", dst); return 1; }
    { if (g_pull_human) printf("\r  ok: %s%30s\n", f->dest, ""); else printf("DONE\t%s\n", dst); }
    return 0;
}

/* baixa todos os arquivos do item; cada um e conferido pelo sha256 do catalogo */
static int pull_cmd(const char *id, const char *dir) {
    const CatItem *c = cat_find(id);
    if (!c) { printf("ERROR\tmodelo desconhecido: %s (veja katana catalog)\n", id); return 1; }
    for (int k = 0; k < c->nfiles; k++) if (pull_file(c, &c->files[k], dir) != 0) return 1;
    return 0;
}

/* pasta de modelos quando nenhuma e dada: KATANA_MODELS, models= do katana.cfg (katana install grava), ./models se existir,
   a pasta models ao lado do executavel (pelo caminho real: vale quando o katana roda por um link, de outra pasta), senao ~/.katana/models */
static int lc_cfg_get(const char *key, char *out, size_t cap);
static const char *models_dir(void) {
    static char d[4096];
    const char *e = getenv("KATANA_MODELS");
    struct stat st;
    if (e && e[0]) return e;
    if (lc_cfg_get("models", d, sizeof d) && stat(d, &st) == 0 && S_ISDIR(st.st_mode)) return d;
    if (stat("models", &st) == 0) return "models";
    { char ed[3900]; if (exe_dir(ed, sizeof ed)) { snprintf(d, sizeof d, "%s/models", ed); if (stat(d, &st) == 0 && S_ISDIR(st.st_mode)) return d; } }
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

/* ---------- katana setup ----------
 * katana setup [pasta] [--yes] [--all] [--with vision,embedder,image,<id>...]
 * Mostra a maquina, escolhe o LLM recomendado que cabe e oferece os extras (visao, embedder, imagem). Padrao: LLM + visao +
 * embedder; imagem e opcional por causa do tamanho (--all marca tudo). Sem terminal e sem --yes: so imprime o plano e sai com 0. */
static int setup_ask(const char *q, int def) {
    char l[64];
    printf("%s [%s] ", q, def ? "Y/n" : "y/N"); fflush(stdout);
    if (!fgets(l, sizeof l, stdin)) return def;
    if (l[0] == 'y' || l[0] == 'Y' || l[0] == 's' || l[0] == 'S') return 1;
    if (l[0] == 'n' || l[0] == 'N') return 0;
    return def;
}

static double setup_left_gb(const CatItem *c, const char *dir) {
    int in; const long long have = cat_have(c, dir, &in);
    return in ? 0.0 : (cat_total(c) - have) / 1073741824.0;
}

static int setup_cmd(int argc, char **argv) {
    const char *dir = NULL, *with = NULL;
    int yes = 0, all = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--yes") || !strcmp(argv[i], "-y")) yes = 1;
        else if (!strcmp(argv[i], "--all")) all = 1;
        else if (!strcmp(argv[i], "--with") && i + 1 < argc) with = argv[++i];
        else if (argv[i][0] != '-') dir = argv[i];
        else { fprintf(stderr, "katana setup: opcao desconhecida: %s\nuso: katana setup [pasta] [--yes] [--all] [--with vision,embedder,image]\n", argv[i]); return 2; }
    }
    if (!dir) dir = models_dir();
    mkdirs(dir);
    const int tty = K_ISATTY(0);
    HwInfo h; hw_probe(&h, dir);
    printf("Machine: %.0f GB RAM, %d cores, GPU: %s%s, %.0f GB free in %s\n\n", h.ram_gb, h.cores, h.gpu[0] ? h.gpu : "none",
           h.gpu[0] ? "" : " (CPU only)", h.disk_gb, dir);
    const CatItem *best = cat_best(&h);
    int have_llm = 0;
    for (int i = 0; i < N_CAT; i++) { int in; cat_have(&g_cat[i], dir, &in); if (in && !strcmp(g_cat[i].kind, "llm")) have_llm = 1; }
    if (!best && !have_llm) { printf("No language model in the catalog fits this machine (minimum 24 GB of RAM).\n"); return 1; }
    int sel[64] = {0};
    for (int i = 0; i < N_CAT && i < 64; i++) {
        const CatItem *c = &g_cat[i];
        const int llm = !strcmp(c->kind, "llm");
        if (llm) sel[i] = (c == best && !have_llm);
        else if (!strcmp(c->kind, "image")) sel[i] = all;
        else sel[i] = 1;
        if (with) {                         /* --with: lista separada por virgula de tipos ou ids; acrescenta ao padrao */
            char w[512]; snprintf(w, sizeof w, "%s", with);
            for (char *t = strtok(w, ","); t; t = strtok(NULL, ","))
                if (!strcmp(t, c->kind) ? (!llm || c == best) : !strcmp(t, c->id)) sel[i] = 1;
        }
        if (all && !llm) sel[i] = 1;
        int in; cat_have(c, dir, &in);
        if (in || (llm && cat_fit(c, &h) == 0)) sel[i] = 0;
    }
    printf("    %-18s %-9s %9s %9s  %s\n", "item", "kind", "size", "to fetch", "status");
    for (int i = 0; i < N_CAT; i++) {
        const CatItem *c = &g_cat[i];
        int in; const long long have = cat_have(c, dir, &in);
        const char *st = in ? "installed" : (!strcmp(c->kind, "llm") && cat_fit(c, &h) == 0) ? "does not fit in RAM" : have > 0 ? "partial (resumes)" : "not installed";
        printf("[%c] %-18s %-9s %6.1f GB %6.1f GB  %s%s\n", sel[i] ? 'x' : ' ', c->id, c->kind, cat_total(c) / 1073741824.0, setup_left_gb(c, dir), st, c == best ? " (recommended)" : "");
        printf("      %s\n", c->note);
    }
    if (!tty && !yes) {
        double s = 0;
        for (int i = 0; i < N_CAT; i++) if (sel[i]) s += setup_left_gb(&g_cat[i], dir);
        printf("\nPlan: download %.1f GB (marked [x]) with %.1f GB free. Run `katana setup --yes` to do it (add --all or --with image for the image models).\n", s, h.disk_gb);
        return 0;
    }
    if (!yes) {
        printf("\n");
        for (int i = 0; i < N_CAT; i++) {
            const CatItem *c = &g_cat[i];
            int in; cat_have(c, dir, &in);
            if (in || (!strcmp(c->kind, "llm") && c != best)) continue;
            char q[200]; snprintf(q, sizeof q, "Download %s (%.1f GB)?", c->name, setup_left_gb(c, dir));
            sel[i] = setup_ask(q, sel[i]);
        }
    }
    double need_gb = 0;
    for (int i = 0; i < N_CAT; i++) if (sel[i]) need_gb += setup_left_gb(&g_cat[i], dir);
    if (need_gb == 0) printf("\nNothing to download.\n");
    else if (h.disk_gb > 0 && need_gb + 0.5 > h.disk_gb) {
        printf("\nNot enough disk: %.1f GB needed, %.1f GB free in %s. Free some space or pick fewer items.\n", need_gb, h.disk_gb, dir);
        return 1;
    }
    g_pull_human = 1;
    for (int i = 0; i < N_CAT; i++) {
        if (!sel[i]) continue;
        printf("\n== %s ==\n", g_cat[i].name); fflush(stdout);
        if (pull_cmd(g_cat[i].id, dir) != 0) { printf("\nsetup stopped; run `katana setup` again to resume.\n"); return 1; }
    }
    printf("\nReady. To start:\n  katana            terminal agent in the current folder\n  katana web        the agent in your browser\n  katana-desktop    native desktop app (if installed)\n");
    return 0;
}
