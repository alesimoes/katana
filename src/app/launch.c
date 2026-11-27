/* ---------- katana sem argumentos: console do agente na pasta atual, modo headless, install, adopt ----------
 * Tudo o que e novo para quem usa o katana como comando do dia a dia fica aqui (docs/console.md):
 *   katana [opcoes] ["primeira mensagem"]   console do agente na pasta atual (modelo escolhido sozinho)
 *   katana -p "pedido"                      headless: um pedido, uma resposta (text, json ou stream-json)
 *   katana install | uninstall              launcher no PATH (link simbolico para este binario)
 *   katana adopt                            traz para o katana o que um projeto ja tem de outro agente
 * Os ganchos chamados pelo laco do agente (loop.c), pelo despacho (dispatch.c) e pelo console (console.c) estao
 * declarados no inicio de console.c; aqui ficam as definicoes. Este arquivo e incluido antes de main.c. */
#ifndef _WIN32
#include <termios.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <poll.h>
#include <limits.h>
#endif

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#endif

static int run_main(int argc, char **argv);
static char g_hist_path[4400];          /* historico do editor de linha, por pasta */

/* ============================ utilitarios ============================ */
static void lc_home(char *out, size_t cap) { agent_home(out, cap); }
static void lc_join(char *out, size_t cap, const char *a, const char *b) { snprintf(out, cap, "%.2000s/%.2000s", a, b); }
static int lc_isdir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int lc_isfile(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }
static int lc_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static char *lc_slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *b = xalloc((size_t)n + 1, 1);
    const size_t r = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[r] = 0;
    if (len) *len = r;
    return b;
}
static int lc_write(const char *path, const char *data, size_t n) {
    mkdir_parents(path);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const int ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
static uint64_t lc_hash(const char *s) { uint64_t h = 1469598103934665603ULL; for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; } return h; }
/* caminho com o inicio trocado por ~ (pasta pessoal) ou ./ (pasta atual), para mostrar */
static const char *lc_short(const char *p, char *out, size_t cap) {
    char home[1024], cwd[4200];
    lc_home(home, sizeof home);
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    const size_t cl = strlen(cwd), hl = strlen(home);
    if (cl > 1 && !strncmp(p, cwd, cl) && p[cl] == '/') snprintf(out, cap, ".%s", p + cl);
    else if (hl > 1 && !strncmp(p, home, hl) && (p[hl] == '/' || !p[hl])) snprintf(out, cap, "~%s", p + hl);
    else snprintf(out, cap, "%s", p);
    return out;
}
static void lc_realpath(const char *p, char *out, size_t cap) {
#ifndef _WIN32
    char r[PATH_MAX];
    if (realpath(p, r)) { snprintf(out, cap, "%s", r); return; }
#endif
    snprintf(out, cap, "%s", p);
}
static void lc_json_esc(Buf *b, const char *s, size_t n) {
    char t[8];
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '"') buf_str(b, "\\\"");
        else if (c == '\\') buf_str(b, "\\\\");
        else if (c == '\n') buf_str(b, "\\n");
        else if (c == '\r') buf_str(b, "\\r");
        else if (c == '\t') buf_str(b, "\\t");
        else if (c < 0x20) { snprintf(t, sizeof t, "\\u%04x", c); buf_str(b, t); }
        else buf_add(b, (const char *)&c, 1);
    }
}
static void lc_json_str(Buf *b, const char *s) { buf_str(b, "\""); lc_json_esc(b, s, strlen(s)); buf_str(b, "\""); }
static double lc_now(void) { return now_sec(); }

/* ============================ configuracao (katana.cfg) ============================ */
/* valor da chave em ./.katana/katana.cfg e depois ~/.katana/katana.cfg (linhas chave=valor; o resto do arquivo e do ajuste da GPU) */
static int lc_cfg_get(const char *key, char *out, size_t cap) {
    char home[1024], p[4400], cwd[4200];
    const size_t kl = strlen(key);
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    lc_home(home, sizeof home);
    for (int w = 0; w < 2; w++) {
        if (!w) { if (!cwd[0]) continue; snprintf(p, sizeof p, "%s/.katana/katana.cfg", cwd); }
        else snprintf(p, sizeof p, "%s/.katana/katana.cfg", home);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        char l[4400];
        while (fgets(l, sizeof l, f)) {
            if (strncmp(l, key, kl) != 0 || l[kl] != '=') continue;
            l[strcspn(l, "\r\n")] = 0;
            snprintf(out, cap, "%s", l + kl + 1);
            fclose(f);
            return out[0] != 0;
        }
        fclose(f);
    }
    return 0;
}
/* grava chave=valor no ~/.katana/katana.cfg (troca a linha da chave, mantem as outras) */
static int lc_cfg_set(const char *key, const char *val) {
    char home[1024], p[4400];
    lc_home(home, sizeof home);
    snprintf(p, sizeof p, "%s/.katana/katana.cfg", home);
    char *old = lc_slurp(p, NULL);
    Buf o = {0};
    const size_t kl = strlen(key);
    for (const char *l = old ? old : ""; *l; ) {
        const char *e = strchr(l, '\n');
        const size_t n = e ? (size_t)(e - l) + 1 : strlen(l);
        if (!(strncmp(l, key, kl) == 0 && l[kl] == '=')) buf_add(&o, l, n);
        l += n;
    }
    if (o.n && o.p[o.n - 1] != '\n') buf_str(&o, "\n");
    buf_str(&o, key); buf_str(&o, "="); buf_str(&o, val); buf_str(&o, "\n");
    const int ok = lc_write(p, o.p, o.n);
    free(old); free(o.p);
    return ok;
}

/* ============================ modelos ============================ */
typedef struct { char name[300]; long long bytes; } LcModelFile;
static int lc_models_scan(const char *dir, LcModelFile *v, int max) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        const size_t l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, ".gguf") != 0) continue;
        char p[4600]; lc_join(p, sizeof p, dir, e->d_name);
        if (!lc_isfile(p)) continue;
        snprintf(v[n].name, sizeof v[n].name, "%s", e->d_name);
        v[n].bytes = file_bytes(p);
        n++;
    }
    closedir(d);
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && strcmp(v[j - 1].name, v[j].name) > 0; j--) { LcModelFile t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    return n;
}
static void lc_lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }
/* NOME ou CAMINHO -> caminho do arquivo. 0 = ok; 1 = nao achou; 2 = ambiguo (err lista os candidatos) */
static int lc_model_pick(const char *spec, char *out, size_t cap, char *err, size_t ecap) {
    static LcModelFile v[256];
    const char *dir = models_dir();
    if ((strchr(spec, '/') || strchr(spec, '\\') || (strlen(spec) > 5 && !strcmp(spec + strlen(spec) - 5, ".gguf"))) && lc_isfile(spec)) {
        lc_realpath(spec, out, cap); return 0;
    }
    const int n = lc_models_scan(dir, v, 256);
    char want[300], nm[300];
    snprintf(want, sizeof want, "%s", spec);
    for (int i = 0; i < n; i++) {     /* nome exato (com ou sem .gguf) */
        snprintf(nm, sizeof nm, "%s.gguf", spec);
        if (!strcmp(v[i].name, spec) || !strcmp(v[i].name, nm)) { char p[4600]; lc_join(p, sizeof p, dir, v[i].name); lc_realpath(p, out, cap); return 0; }
    }
    lc_lower(want);
    int hit[256], nh = 0;
    for (int i = 0; i < n; i++) { snprintf(nm, sizeof nm, "%s", v[i].name); lc_lower(nm); if (strstr(nm, want)) hit[nh++] = i; }
    if (nh == 1) { char p[4600]; lc_join(p, sizeof p, dir, v[hit[0]].name); lc_realpath(p, out, cap); return 0; }
    if (nh > 1) {
        size_t o = (size_t)snprintf(err, ecap, "\"%s\" combina com mais de um modelo em %s:", spec, dir);
        for (int i = 0; i < nh && o + 340 < ecap; i++) o += (size_t)snprintf(err + o, ecap - o, "\n  %s", v[hit[i]].name);
        return 2;
    }
    snprintf(err, ecap, "nenhum modelo chamado \"%s\" em %s (katana models lista; katana catalog mostra o que da para baixar)", spec, dir);
    return 1;
}
/* o modelo que o aplicativo de mesa usou por ultimo (ultimoModelo= em ~/.katana/desktop/ui.ini) */
static int lc_desktop_last(char *out, size_t cap) {
    char home[1024], p[4400], l[1200];
    lc_home(home, sizeof home);
    snprintf(p, sizeof p, "%s/.katana/desktop/ui.ini", home);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int ok = 0;
    while (fgets(l, sizeof l, f)) if (!strncmp(l, "ultimoModelo=", 13)) { l[strcspn(l, "\r\n")] = 0; snprintf(out, cap, "%s", l + 13); ok = out[0] != 0; break; }
    fclose(f);
    return ok;
}
/* escolhe o modelo; devolve 0 (out e why preenchidos) ou o codigo de saida (2) com a mensagem em err */
static int lc_model_resolve(const char *opt, char *out, size_t cap, char *why, size_t wcap, char *err, size_t ecap) {
    char spec[4400], p[4600];
    int r;
    const char *dir = models_dir();
    if (opt && *opt) {
        if ((r = lc_model_pick(opt, out, cap, err, ecap)) != 0) return 2;
        snprintf(why, wcap, "escolhido por --model"); return 0;
    }
    const char *e = getenv("KATANA_MODEL");
    if (e && *e) {
        if ((r = lc_model_pick(e, out, cap, err, ecap)) != 0) return 2;
        snprintf(why, wcap, "escolhido por KATANA_MODEL"); return 0;
    }
    if (lc_cfg_get("model", spec, sizeof spec)) {
        if (lc_model_pick(spec, out, cap, err, ecap) == 0) { snprintf(why, wcap, "escolhido por model= em katana.cfg"); return 0; }
        fprintf(stderr, "katana: aviso: model=%s de katana.cfg nao existe; seguindo para o proximo criterio\n", spec);
    }
    if (lc_desktop_last(spec, sizeof spec)) {
        if (lc_model_pick(spec, out, cap, err, ecap) == 0) { snprintf(why, wcap, "o ultimo usado no aplicativo de mesa"); return 0; }
    }
    static LcModelFile v[256];
    const int n = lc_models_scan(dir, v, 256);
    if (n == 0) {
        snprintf(err, ecap, "nenhum modelo instalado em %s\nrun: katana setup", dir);
        return 2;
    }
    HwInfo h; memset(&h, 0, sizeof h);      /* so a memoria: nao acorda a placa de video so para escolher */
#ifndef _WIN32
    h.ram_gb = (double)sysconf(_SC_PHYS_PAGES) * (double)sysconf(_SC_PAGE_SIZE) / 1073741824.0;
#else
    { MEMORYSTATUSEX ms; ms.dwLength = sizeof ms; if (GlobalMemoryStatusEx(&ms)) h.ram_gb = ms.ullTotalPhys / 1073741824.0; }
#endif
    if (getenv("KATANA_LAUNCH_RAM_GB")) h.ram_gb = atof(getenv("KATANA_LAUNCH_RAM_GB"));      /* testes */
    const CatItem *best = cat_best(&h);
    if (best) for (int i = 0; i < n; i++) if (!strcmp(v[i].name, best->files[0].dest)) {
        lc_join(p, sizeof p, dir, v[i].name); lc_realpath(p, out, cap);
        snprintf(why, wcap, "o recomendado do catalogo (%s)", best->name); return 0;
    }
    int bi = 0;
    for (int i = 1; i < n; i++) if (v[i].bytes > v[bi].bytes) bi = i;
    lc_join(p, sizeof p, dir, v[bi].name); lc_realpath(p, out, cap);
    snprintf(why, wcap, "o maior modelo instalado (%.1f GB)", v[bi].bytes / 1073741824.0);
    return 0;
}

/* ============================ sessoes por pasta ============================ */
/* ~/.katana/sessions/<hash da pasta>/: <id>.ksess, index (uma linha por sessao: id TAB epoca TAB turnos TAB primeira mensagem), history */
static void lc_sess_dir(const char *folder, char *out, size_t cap) {
    char home[1024]; lc_home(home, sizeof home);
    snprintf(out, cap, "%s/.katana/sessions/%016llx", home, (unsigned long long)lc_hash(folder));
}
/* raiz do repositorio que contem a pasta (ou "" se nao houver) */
static int lc_repo_root(const char *folder, char *out, size_t cap) {
    char d[4200], g[4400];
    snprintf(d, sizeof d, "%s", folder);
    for (;;) {
        snprintf(g, sizeof g, "%s/.git", d);
        if (lc_exists(g)) { snprintf(out, cap, "%s", d); return 1; }
        char *sl = strrchr(d, '/');
        if (!sl || sl == d) break;
        *sl = 0;
    }
    out[0] = 0;
    return 0;
}
typedef struct { char id[64]; char path[4600]; long long when; int turns; char first[160]; } LcSess;
/* primeira mensagem do usuario e numero de pedidos de um .ksess (le so o que precisa) */
static int lc_sess_peek(const char *path, LcSess *s) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint64_t mg = 0, fp = 0; int64_t ts = 0; int32_t NL = 0, E = 0, nid = 0;
    AHist cp = {0}, fl = {0};
    int ok = fread(&mg, 8, 1, f) == 1 && mg == SESS_MAGIC && fread(&fp, 8, 1, f) == 1 && fread(&ts, 8, 1, f) == 1
          && fread(&NL, 4, 1, f) == 1 && fread(&E, 4, 1, f) == 1 && NL >= 0 && E >= 0 && (size_t)NL * E < (1u << 24);
    if (ok && NL) ok = fseek(f, (long)((size_t)NL * E * sizeof(float)), SEEK_CUR) == 0;
    ok = ok && sess_rmsgs(f, &cp) && fread(&nid, 4, 1, f) == 1 && nid >= 0 && nid < (1 << 24) && fseek(f, (long)nid * (long)sizeof(int), SEEK_CUR) == 0 && sess_rmsgs(f, &fl);
    fclose(f);
    if (!ok) { ah_clear(&cp); ah_clear(&fl); free(cp.v); free(fl.v); return 0; }
    const AHist *use = fl.n ? &fl : &cp;
    s->turns = 0; s->first[0] = 0; s->when = (long long)ts;
    for (int i = 0; i < use->n; i++) if (use->v[i].role == 'u') {
        if (!s->turns) {
            const char *t = use->v[i].s;
            while (*t == '[') { const char *nl = strchr(t, '\n'); if (!nl) break; t = nl + 1; }    /* notas do motor antes do texto */
            snprintf(s->first, sizeof s->first, "%.150s", t);
            for (char *q = s->first; *q; q++) if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
        }
        s->turns++;
    }
    ah_clear(&cp); ah_clear(&fl); free(cp.v); free(fl.v);
    return 1;
}
static int lc_sess_cmp(const void *a, const void *b) { const LcSess *x = a, *y = b; return x->when < y->when ? 1 : x->when > y->when ? -1 : strcmp(y->id, x->id); }
/* sessoes de uma pasta de sessoes: arquivos .ksess (a verdade); o index acelera e guarda o que a leitura nao da */
static int lc_sess_list(const char *dir, LcSess *v, int max) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        const size_t l = strlen(e->d_name);
        if (l < 7 || strcmp(e->d_name + l - 6, ".ksess") != 0 || l - 6 >= sizeof v[n].id) continue;
        LcSess *s = &v[n];
        memset(s, 0, sizeof *s);
        snprintf(s->id, sizeof s->id, "%.*s", (int)(l - 6), e->d_name);
        lc_join(s->path, sizeof s->path, dir, e->d_name);
        struct stat st;
        if (stat(s->path, &st) != 0) continue;
        s->when = (long long)st.st_mtime;
        if (lc_sess_peek(s->path, s)) { if (!s->when) s->when = (long long)st.st_mtime; n++; }
        else { s->turns = 0; s->first[0] = 0; n++; }
    }
    closedir(d);
    qsort(v, (size_t)n, sizeof *v, lc_sess_cmp);
    return n;
}
/* sessoes desta pasta e da raiz do repositorio, mais recentes primeiro */
static int lc_sess_here(LcSess *v, int max) {
    char cwd[4200], root[4200], d[4400];
    if (!getcwd(cwd, sizeof cwd)) return 0;
    lc_sess_dir(cwd, d, sizeof d);
    int n = lc_sess_list(d, v, max);
    if (lc_repo_root(cwd, root, sizeof root) && strcmp(root, cwd) != 0) {
        lc_sess_dir(root, d, sizeof d);
        n += lc_sess_list(d, v + n, max - n);
        qsort(v, (size_t)n, sizeof *v, lc_sess_cmp);
    }
    return n;
}
static void lc_fmt_when(long long t, char *out, size_t cap) { time_t tt = (time_t)t; struct tm *tmv = localtime(&tt); if (tmv) strftime(out, cap, "%Y-%m-%d %H:%M", tmv); else snprintf(out, cap, "?"); }
/* registra a sessao no index da pasta dela (a linha mais nova de um id vale) */
static void lc_sess_index(const char *path) {
    LcSess s; memset(&s, 0, sizeof s);
    if (!path[0] || !lc_isfile(path) || !lc_sess_peek(path, &s)) return;
    char dir[4400], idx[4500];
    snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
    snprintf(idx, sizeof idx, "%s/index", dir);
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    FILE *f = fopen(idx, "a");
    if (!f) return;
    fprintf(f, "%.*s\t%lld\t%d\t%s\n", (int)(strlen(b) > 6 ? strlen(b) - 6 : 0), b, (long long)time(NULL), s.turns, s.first);
    fclose(f);
}

/* ============================ lista de ferramentas permitidas ============================ */
static int lc_glob(const char *p, const char *s) {
    while (*p) {
        if (*p == '*') { while (*p == '*') p++; if (!*p) return 1; for (; *s; s++) if (lc_glob(p, s)) return 1; return lc_glob(p, s); }
        if (!*s) return 0;
        if (*p != '?' && *p != *s) return 0;
        p++; s++;
    }
    return !*s;
}
/* um padrao da lista casa com o nome? padrao "Bash(git:*)" vale como "Bash"; "mcp__servidor" vale para todas as ferramentas do servidor */
static int lc_pat_match(const char *list, const char *name) {
    char alt[400];
    snprintf(alt, sizeof alt, "mcp__%s", name);
    for (const char *p = list; *p; ) {
        while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\t') p++;
        const char *e = p; while (*e && *e != ',' && *e != '\n') e++;
        char pat[200]; size_t l = (size_t)(e - p);
        while (l && (p[l - 1] == ' ' || p[l - 1] == '\t')) l--;
        if (l >= sizeof pat - 4) l = sizeof pat - 5;
        memcpy(pat, p, l); pat[l] = 0;
        char *par = strchr(pat, '('); if (par) *par = 0;       /* argumentos do padrao nao se aplicam aqui */
        if (pat[0]) {
            char more[220]; snprintf(more, sizeof more, "%s__*", pat);
            if (lc_glob(pat, name) || lc_glob(pat, alt) || lc_glob(more, name) || lc_glob(more, alt)) return 1;
        }
        p = *e ? e + 1 : e;
    }
    return 0;
}
static int launch_tool_allowed(const char *name) {
    const char *al = getenv("KATANA_ALLOWED_TOOLS"), *dis = getenv("KATANA_DISALLOWED_TOOLS");
    if (dis && *dis && lc_pat_match(dis, name)) return 0;
    if (al && *al && !lc_pat_match(al, name)) return 0;
    return 1;
}

/* ============================ saida headless e eventos ============================ */
typedef struct { char name[128]; char *args; double t0; } LcCall;
static FILE *g_hl_out, *g_hl_err;      /* a saida e o erro de verdade (os descritores 1 e 2 ficam so para o motor) */
static int g_hl_fmt;                   /* 0 texto, 1 json, 2 stream-json */
static int g_hl_turns, g_hl_ncalls, g_hl_nerr;
static long g_hl_in, g_hl_gen;
static double g_hl_t0;
static char *g_hl_prompt;              /* o pedido, entregue ao laco no lugar de uma linha do teclado */
static char *g_hl_last;                /* texto limpo da ultima resposta do modelo */
static char g_hl_sid[160];             /* id da sessao (vazio: sem sessao gravada) */
static Buf g_hl_calls;                 /* "tool_calls" do resultado final (elementos de um array JSON) */
static LcCall g_cur;                   /* chamada de ferramenta em andamento */
static char g_hl_plan[3000];

static void hl_emit(const char *line) { if (g_hl_fmt == 2 && g_hl_out) { fputs(line, g_hl_out); fputc('\n', g_hl_out); fflush(g_hl_out); } }

/* argumentos de uma chamada como objeto JSON valido e compacto (ap..end: o texto da chamada) */
static char *lc_args_json(const char *ap, const char *end) {
    const char *p = NULL;
    for (const char *q = ap; q + 11 < end; q++) if (!strncmp(q, "\"arguments\"", 11)) { p = q + 11; break; }
    if (p) { while (p < end && (isspace((unsigned char)*p) || *p == ':')) p++; if (p >= end || *p != '{') p = NULL; }
    const char *e = p ? mcp_skip(p) : NULL;
    if (!p || !e || e > end + 1) return strdup("{}");
    Buf b = {0};
    int in = 0;
    for (const char *q = p; q < e; q++) {
        const unsigned char c = (unsigned char)*q;
        if (in) {
            if (c == '\\' && q + 1 < e) { buf_add(&b, q, 2); q++; continue; }
            if (c == '"') in = 0;
            if (c == '\n') { buf_str(&b, "\\n"); continue; }
            if (c == '\r') { buf_str(&b, "\\r"); continue; }
            if (c == '\t') { buf_str(&b, "\\t"); continue; }
            if (c < 0x20) continue;
            buf_add(&b, q, 1);
        } else {
            if (c == '"') in = 1;
            if (isspace(c)) continue;
            buf_add(&b, q, 1);
        }
    }
    return b.p ? b.p : strdup("{}");
}
/* texto de uma resposta sem raciocinio, sem chamadas de ferramenta e sem marcas do formato */
static char *lc_clean_reply(const char *r) {
    Buf b = {0};
    const char *s = r;
    const char *x;
    if ((x = strstr(s, "</think>"))) { while ((x = strstr(s, "</think>"))) s = x + 8; }
    else if (strstr(s, "<think>")) s = s + strlen(s);                          /* raciocinio nao fechado */
    if ((x = strstr(s, "<channel|>"))) { while ((x = strstr(s, "<channel|>"))) s = x + 10; }
    else if ((x = strstr(s, "<|channel>"))) s = s + strlen(s);
    if ((x = strstr(s, "<|channel|>final<|message|>"))) s = x + 27;            /* harmony */
    static const char *const open_[] = {"<tool_call>", "<|tool_call>", "<function=", "<|start|>", "to=functions.", NULL};
    size_t n = strlen(s);
    for (int i = 0; open_[i]; i++) { const char *o = strstr(s, open_[i]); if (o && (size_t)(o - s) < n) n = (size_t)(o - s); }
    const char *ends[] = {"<|end|>", "<|return|>", "<|im_end|>", "<turn|>", NULL};
    for (int i = 0; ends[i]; i++) { const char *o = strstr(s, ends[i]); if (o && (size_t)(o - s) < n) n = (size_t)(o - s); }
    while (n && isspace((unsigned char)*s)) { s++; n--; }
    while (n && isspace((unsigned char)s[n - 1])) n--;
    buf_add(&b, s, n);
    return b.p ? b.p : strdup("");
}
static void lc_plan_steps(Buf *o) {
    buf_str(o, "[");
    int first = 1, cur = 0;
    for (const char *p = g_todo; *p; ) {
        const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
        const int done = !strncmp(p, "[x]", 3);
        const char *t = p + (!strncmp(p, "[x]", 3) || !strncmp(p, "[ ]", 3) ? 4 : 0);
        if (t > e) t = e;
        const char *st = done ? "completed" : !cur ? "in_progress" : "pending";
        if (!done) cur = 1;
        if (!first) buf_str(o, ",");
        first = 0;
        buf_str(o, "{\"content\":"); Buf q = {0}; lc_json_esc(&q, t, (size_t)(e - t));
        buf_str(o, "\""); if (q.p) buf_str(o, q.p); buf_str(o, "\"");
        free(q.p);
        buf_str(o, ",\"status\":\""); buf_str(o, st); buf_str(o, "\"}");
        if (!*e) break;
        p = e + 1;
    }
    buf_str(o, "]");
}

/* ============================ console rico ============================ */
static int g_con_color;                  /* cores e spinner (terminal e sem NO_COLOR) */
static int g_con_cols = 80;
static double g_req_t0;                  /* inicio do pedido atual */
static long g_req_in, g_req_gen; static double g_req_model_s; static int g_req_rounds, g_req_pos, g_req_ctx;
static char g_con_plan[3000];

static void con_term_cols(void) {
#ifndef _WIN32
    struct winsize ws;
    if (ioctl(2, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 40) g_con_cols = ws.ws_col;
#endif
}
static void lc_fmt_dur(double s, char *o, size_t cap) { if (s < 10) snprintf(o, cap, "%.1fs", s); else if (s < 120) snprintf(o, cap, "%.0fs", s); else snprintf(o, cap, "%dm%02ds", (int)(s / 60), (int)s % 60); }
static void lc_fmt_k(long n, char *o, size_t cap) { if (n < 10000) snprintf(o, cap, "%ld", n); else snprintf(o, cap, "%.1fk", n / 1000.0); }

/* ---- renderizador de Markdown em fluxo: negrito, codigo em linha, blocos de codigo, listas, titulos; esconde as chamadas de ferramenta ---- */
typedef struct {
    int bol, fence, bold, code, head, hid, star, skiplang;
    char lead[12]; int nl;        /* inicio da linha ainda sem decisao */
    char tag[16]; int nt;         /* inicio de uma possivel chamada de ferramenta */
    char tail[16];                /* fim do que foi escondido (procura o fecho) */
} ConMd;
static ConMd g_md = {.bol = 1};
static void md_out(const char *s) { fputs(s, stdout); }
static void md_style(int bold, int dim, int cyan) { if (!g_con_color) return; if (bold) md_out("\033[1m"); if (dim) md_out("\033[2m"); if (cyan) md_out("\033[36m"); }
static void md_reset(void) { if (g_con_color) md_out("\033[0m"); }
static void md_inline(unsigned char c);
static void md_line_start(unsigned char c);
static void md_emit_raw(unsigned char c) { fputc(c, stdout); }
/* processa um caractere de texto (ja fora de chamada de ferramenta) */
static void md_text(unsigned char c) {
    ConMd *m = &g_md;
    if (m->skiplang) { if (c == '\n') { m->skiplang = 0; m->bol = 1; } return; }
    if (m->fence) {       /* dentro de bloco de codigo: so o fecho no inicio da linha importa */
        if (m->bol) {
            if (c == '`' || (c == ' ' && m->nl == 0)) { m->lead[m->nl++] = (char)c; if (c == '`' && m->nl >= 3) { m->fence = 0; m->nl = 0; md_reset(); m->skiplang = 1; } return; }
            if (m->nl) { for (int i = 0; i < m->nl; i++) md_emit_raw((unsigned char)m->lead[i]); m->nl = 0; }
            m->bol = 0;
        }
        md_emit_raw(c);
        if (c == '\n') { m->bol = 1; }
        return;
    }
    if (m->bol) { md_line_start(c); return; }
    md_inline(c);
}
static void md_line_start(unsigned char c) {
    ConMd *m = &g_md;
    /* acumula ate decidir: espacos, #, -, *, ` */
    if (((c == ' ' && (size_t)strspn(m->lead, " ") == (size_t)m->nl) || c == '#' || c == '-' || c == '*' || c == '`') && m->nl < 8) { m->lead[m->nl++] = (char)c; return; }
    m->lead[m->nl] = 0;
    const char *l = m->lead; int sp = 0;
    while (l[sp] == ' ') sp++;
    const char *r = l + sp;
    const size_t rn = strlen(r);
    int handled = 0;
    if (rn >= 1 && rn <= 6 && strspn(r, "#") == rn && c == ' ') {            /* titulo */
        m->bol = 0; m->nl = 0; m->head = 1; md_style(1, 0, 0); handled = 1;
    } else if (rn == 3 && !strcmp(r, "```")) {                                /* abre bloco de codigo (a linguagem e descartada) */
        m->nl = 0; m->fence = 1; m->bol = 1; m->skiplang = c != '\n'; md_style(0, 1, 0);
        return;
    } else if (rn == 1 && (r[0] == '-' || r[0] == '*') && c == ' ') {         /* item de lista */
        for (int i = 0; i < sp; i++) md_emit_raw(' ');
        md_out("\xE2\x80\xA2 ");
        m->bol = 0; m->nl = 0; handled = 1;
    }
    if (handled) return;
    m->bol = 0;
    const int n = m->nl; char tmp[12]; memcpy(tmp, m->lead, (size_t)n); m->nl = 0;
    for (int i = 0; i < n; i++) md_inline((unsigned char)tmp[i]);
    if (c == '\n') { m->bol = 1; md_out("\n"); return; }
    md_inline(c);
}
static void md_inline(unsigned char c) {
    ConMd *m = &g_md;
    if (m->star) {
        m->star = 0;
        if (c == '*') { m->bold = !m->bold; if (m->bold) md_style(1, 0, 0); else if (g_con_color) md_out("\033[22m"); return; }
        md_emit_raw('*');
    }
    if (c == '*') { m->star = 1; return; }
    if (c == '`') { m->code = !m->code; if (m->code) md_style(0, 0, 1); else if (g_con_color) md_out("\033[39m"); return; }
    if (c == '\n') {
        if (m->bold || m->code || m->head) md_reset();
        m->bold = m->code = m->head = 0;
        md_emit_raw('\n'); m->bol = 1; return;
    }
    md_emit_raw(c);
}
static void md_feed(unsigned char c);
static void md_tag_flush(void) { char t[16]; const int n = g_md.nt; memcpy(t, g_md.tag, (size_t)n); g_md.nt = 0; for (int i = 0; i < n; i++) md_text((unsigned char)t[i]); }
static const char *const MD_OPEN[] = {"<tool_call>", "<|tool_call>", "<function=", NULL};
static void md_feed(unsigned char c) {
    ConMd *m = &g_md;
    if (m->hid) {      /* chamada de ferramenta: some ate o fecho */
        const size_t n = strlen(m->tail);
        if (n >= sizeof m->tail - 1) { memmove(m->tail, m->tail + 1, n); m->tail[n - 1] = (char)c; m->tail[n] = 0; }
        else { m->tail[n] = (char)c; m->tail[n + 1] = 0; }
        if (m->hid == 2 ? strstr(m->tail, "</function>") != NULL : (strstr(m->tail, "</tool_call>") || strstr(m->tail, "<tool_call|>"))) { m->hid = 0; m->tail[0] = 0; }
        return;
    }
    if (m->nt || c == '<') {
        m->tag[m->nt++] = (char)c; m->tag[m->nt] = 0;
        int pre = 0, full = 0;
        for (int i = 0; MD_OPEN[i]; i++) { const size_t ol = strlen(MD_OPEN[i]); if (m->nt <= (int)ol && !strncmp(MD_OPEN[i], m->tag, (size_t)m->nt)) { pre = 1; if (m->nt == (int)ol) full = i == 2 ? 2 : 1; } }
        if (full) { m->nt = 0; m->hid = full; m->tail[0] = 0; if (m->bold || m->code) { md_reset(); m->bold = m->code = 0; } return; }
        if (pre && m->nt < 14) return;
        /* nao era chamada: solta o que segurou (o primeiro '<' primeiro, o resto reprocessado) */
        char t[16]; const int n = m->nt; memcpy(t, m->tag, (size_t)n); m->nt = 0;
        md_text((unsigned char)t[0]);
        for (int i = 1; i < n; i++) md_feed((unsigned char)t[i]);
        return;
    }
    md_text(c);
}
static void md_flush_end(void) {       /* fim da resposta: solta o que estava preso e zera o estilo */
    ConMd *m = &g_md;
    if (m->nt && !m->hid) md_tag_flush();
    if (m->star) { m->star = 0; md_emit_raw('*'); }
    if (m->nl) { char t[12]; const int n = m->nl; memcpy(t, m->lead, (size_t)n); m->nl = 0; m->bol = 0; for (int i = 0; i < n; i++) md_emit_raw((unsigned char)t[i]); }
    if (m->bold || m->code || m->head || m->fence) md_reset();
    memset(m, 0, sizeof *m); m->bol = 1;
}
/* um token do modelo para o terminal (console rico): decodifica os bytes e passa pelo renderizador */
static void con_token(const Tokenizer *tk, int id) {
    if (!g_con_rich) { print_token(tk, id, stdout); return; }
    char tmp[512];
    const int tl = (int)tok_append(tk, id, tmp, 0, sizeof tmp - 1);
    for (int i = 0; i < tl; i++) md_feed((unsigned char)tmp[i]);
}
static void con_token_end(void) { if (g_con_rich) md_flush_end(); }
static void con_puts(const char *s) { if (!g_con_rich) { fputs(s, stdout); return; } for (; *s; s++) md_feed((unsigned char)*s); md_flush_end(); }

/* ---- ferramentas: uma linha por chamada, spinner com tempo, marca de fim, saida apagada e dobrada ---- */
static char g_spin_text[400]; static int g_cur_spin;
#ifndef _WIN32
static pthread_t g_spin_th; static volatile int g_spin_on, g_spin_stop;
static void *spin_main(void *arg) {
    static const char *const F[] = {"\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8", "\xE2\xA0\xBC", "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7", "\xE2\xA0\x87", "\xE2\xA0\x8F"};
    (void)arg;
    int i = 0;
    while (!g_spin_stop) {
        char d[24]; lc_fmt_dur(lc_now() - g_cur.t0, d, sizeof d);
        fprintf(stderr, "\r\033[K  %s%s%s %s %s%s%s", CE(C_CYAN), F[i++ % 10], CE(C_RESET), g_spin_text, CE(C_DIM), d, CE(C_RESET));
        fflush(stderr);
        usleep(100000);
    }
    return NULL;
}
#endif
static void launch_spin_pause(void) {           /* uma pergunta vai aparecer: o spinner sai da frente */
#ifndef _WIN32
    if (!g_cur_spin) return;
    g_spin_stop = 1; pthread_join(g_spin_th, NULL); g_spin_on = 0; g_cur_spin = 0;
    fprintf(stderr, "\r\033[K  > %s\n", g_spin_text);
#endif
}
static int lc_quiet_tool(const char *n) {       /* ferramentas que perguntam ou escrevem na tela por conta propria: sem spinner */
    static const char *const Q[] = {"AskUserQuestion", "Agent", "Workflow", "sudo", NULL};
    for (int i = 0; Q[i]; i++) if (!strcmp(n, Q[i])) return 1;
    const char *c = getenv("KATANA_CONFIRM");
    return c && atoi(c);
}
static void lc_args_summary(const char *name, const char *ap, const char *end, char *out, size_t cap, char *desc, size_t dcap, int *is_cmd) {
    static const char *const K[] = {"command", "file_path", "path", "pattern", "url", "query", "skill", "name", "text", "question", NULL};
    char v[400] = "";
    desc[0] = 0; *is_cmd = 0;
    json_str(ap, end, "description", desc, dcap);
    for (int i = 0; K[i]; i++) if (json_str(ap, end, K[i], v, sizeof v) && v[0]) { *is_cmd = i == 0; break; }
    if (!v[0]) {        /* sem chave conhecida: o primeiro valor de texto dos argumentos */
        const char *a = NULL;
        for (const char *q = ap; q + 11 < end; q++) if (!strncmp(q, "\"arguments\"", 11)) { a = q + 11; break; }
        if (a) { const char *q = strchr(a + 1, ':'); if (q) { q = strchr(q, '"'); if (q && q < end) { const char *e = strchr(q + 1, '"'); if (e && e < end) snprintf(v, sizeof v, "%.*s", (int)(e - q - 1 < 300 ? e - q - 1 : 300), q + 1); } } }
    }
    for (char *q = v; *q; q++) if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
    for (char *q = desc; *q; q++) if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
    snprintf(out, cap, "%s", v);
    (void)name;
}
static void lc_trunc(char *s, int max) {       /* corta em max colunas (UTF-8) com reticencias */
    int cols = 0; char *q = s;
    while (*q) { if (((unsigned char)*q & 0xC0) != 0x80) { if (cols == max) { strcpy(q, "..."); return; } cols++; } q++; }
}
static char g_cur_desc[300], g_cur_sum[400], g_cur_cmd[400]; static int g_cur_is_cmd;

static void launch_tool_call(const char *name, const char *ap, const char *end) {
    snprintf(g_cur.name, sizeof g_cur.name, "%s", name);
    free(g_cur.args); g_cur.args = lc_args_json(ap, end);
    g_cur.t0 = lc_now();
    if (g_hl) {
        if (g_hl_fmt == 2) {
            Buf b = {0};
            buf_str(&b, "{\"type\":\"tool_call\",\"name\":"); lc_json_str(&b, name);
            buf_str(&b, ",\"arguments\":"); buf_str(&b, g_cur.args); buf_str(&b, "}");
            hl_emit(b.p); free(b.p);
        }
        return;
    }
    if (!g_con_rich) return;
    lc_args_summary(name, ap, end, g_cur_sum, sizeof g_cur_sum, g_cur_desc, sizeof g_cur_desc, &g_cur_is_cmd);
    snprintf(g_cur_cmd, sizeof g_cur_cmd, "%s", g_cur_sum);
    const char *what;
    lc_trunc(g_cur_desc, g_con_cols - 24 - (int)strlen(name)); lc_trunc(g_cur_sum, g_con_cols - 24 - (int)strlen(name));
    what = g_cur_desc[0] ? g_cur_desc : g_cur_sum;
    snprintf(g_spin_text, sizeof g_spin_text, "%s%s%s  %s", CE("\033[1m"), name, CE(C_RESET), what);
    g_cur_spin = 0;
#ifndef _WIN32
    if (g_con_color && !lc_quiet_tool(name)) {
        g_spin_stop = 0; g_spin_on = 1; g_cur_spin = 1;
        if (pthread_create(&g_spin_th, NULL, spin_main, NULL) != 0) { g_spin_on = 0; g_cur_spin = 0; }
    }
#endif
    if (!g_cur_spin) { fprintf(stderr, "  > %s\n", g_spin_text); fflush(stderr); }
}
static int lc_is_note(const char *l) { return !strncmp(l, "[katana:", 8) || !strncmp(l, "[plan:", 6); }
static void launch_tool_result(const char *name, const char *res) {
    const double dt = lc_now() - g_cur.t0;
    const int failed = rp_failed(res);
    /* eventos e contas do headless */
    if (g_hl) {
        Buf b = {0};
        buf_str(&b, "{\"name\":"); lc_json_str(&b, name);
        buf_str(&b, ",\"arguments\":"); buf_str(&b, g_cur.args ? g_cur.args : "{}");
        buf_str(&b, failed ? ",\"is_error\":true}" : ",\"is_error\":false}");
        if (g_hl_ncalls) buf_str(&g_hl_calls, ",");
        buf_str(&g_hl_calls, b.p);
        g_hl_ncalls++; g_hl_nerr += failed;
        free(b.p);
        if (g_hl_fmt == 2) {
            Buf e = {0}; char sm[260];
            snprintf(sm, sizeof sm, "%.240s", res);
            for (char *q = sm; *q; q++) if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
            buf_str(&e, "{\"type\":\"tool_result\",\"name\":"); lc_json_str(&e, name);
            buf_str(&e, failed ? ",\"is_error\":true,\"summary\":" : ",\"is_error\":false,\"summary\":"); lc_json_str(&e, sm); buf_str(&e, "}");
            hl_emit(e.p); free(e.p);
        }
    }
    /* a lista de tarefas mudou */
    const int plan_changed = (!strcmp(name, "TodoWrite") || !strncmp(name, "Task", 4)) && strcmp(g_todo, g_hl ? g_hl_plan : g_con_plan) != 0;
    if (g_hl) {
        if (plan_changed) {
            snprintf(g_hl_plan, sizeof g_hl_plan, "%s", g_todo);
            if (g_hl_fmt == 2) { Buf e = {0}; buf_str(&e, "{\"type\":\"plan\",\"steps\":"); lc_plan_steps(&e); buf_str(&e, "}"); hl_emit(e.p); free(e.p); }
        }
        return;
    }
    if (!g_con_rich) return;
#ifndef _WIN32
    if (g_cur_spin) { g_spin_stop = 1; pthread_join(g_spin_th, NULL); g_spin_on = 0; g_cur_spin = 0; fprintf(stderr, "\r\033[K"); }
#endif
    char d[24]; lc_fmt_dur(dt, d, sizeof d);
    const char *mark = failed ? (g_con_color ? "\033[31m\xE2\x9C\x97\033[0m" : "failed") : (g_con_color ? "\033[32m\xE2\x9C\x93\033[0m" : "ok");
    const char *what = g_cur_desc[0] ? g_cur_desc : g_cur_sum;
    fprintf(stderr, "  %s %s%s%s  %s  %s%s%s\n", mark, CE("\033[1m"), name, CE(C_RESET), what, CE(C_DIM), d, CE(C_RESET));
    if (g_cur_desc[0] && g_cur_cmd[0]) { char c[420]; snprintf(c, sizeof c, "%s", g_cur_cmd); lc_trunc(c, g_con_cols - 10); fprintf(stderr, "    %s%s%s\n", CE(C_DIM), c, CE(C_RESET)); }
    if (plan_changed) {
        snprintf(g_con_plan, sizeof g_con_plan, "%s", g_todo);
        int n = 0, done = 0;
        for (const char *p = g_todo; *p; ) { n++; if (!strncmp(p, "[x]", 3)) done++; const char *e = strchr(p, '\n'); if (!e) break; p = e + 1; }
        fprintf(stderr, "  %sPlan %d/%d%s\n", CE("\033[1m"), done, n, CE(C_RESET));
        int cur = 0;
        for (const char *p = g_todo; *p; ) {
            const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
            const int dn = !strncmp(p, "[x]", 3);
            const char *t = p + 4; if (t > e) t = e;
            const char *icon = dn ? (g_con_color ? "\xE2\x9C\x93" : "x") : !cur ? (g_con_color ? "\xE2\x96\xB6" : ">") : (g_con_color ? "\xE2\x97\x8B" : "-");
            if (!dn) cur = 1;
            fprintf(stderr, "   %s%s %.*s%s\n", CE(dn ? C_DIM : ""), icon, (int)(e - t < g_con_cols - 8 ? e - t : g_con_cols - 8), t, CE(C_RESET));
            if (!*e) break;
            p = e + 1;
        }
        fflush(stderr);
        return;       /* a resposta do TodoWrite nao precisa aparecer */
    }
    /* as primeiras linhas da saida, apagadas; o resto dobrado com a contagem; notas do motor para o modelo ficam de fora */
    int shown = 0, total = 0;
    for (const char *p = res; *p; ) {
        const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
        if (!lc_is_note(p) && e > p) total++;
        if (!*e) break;
        p = e + 1;
    }
    const int MAXL = failed ? 8 : 5;
    for (const char *p = res; *p && shown < MAXL; ) {
        const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
        if (!lc_is_note(p) && e > p) {
            char ln[420]; snprintf(ln, sizeof ln, "%.*s", (int)(e - p < 400 ? e - p : 400), p);
            for (char *q = ln; *q; q++) if (*q == '\t' || *q == '\r') *q = ' ';
            lc_trunc(ln, g_con_cols - 10);
            fprintf(stderr, "    %s%s%s\n", CE(C_DIM), ln, CE(C_RESET));
            shown++;
        }
        if (!*e) break;
        p = e + 1;
    }
    if (total > shown) fprintf(stderr, "    %s... +%d lines%s\n", CE(C_DIM), total - shown, CE(C_RESET));
    fflush(stderr);
}
static void launch_reply(const char *p, size_t n) {
    (void)n;
    g_req_rounds++;
    if (!g_hl) return;
    g_hl_turns++;
    char *c = lc_clean_reply(p ? p : "");
    free(g_hl_last); g_hl_last = c;
    if (c[0] && g_hl_fmt == 2) { Buf b = {0}; buf_str(&b, "{\"type\":\"text\",\"text\":"); lc_json_str(&b, c); buf_str(&b, "}"); hl_emit(b.p); free(b.p); }
}
static void launch_stats(int in_n, double in_s, int gen, double gen_s, int pos, int ctx) {
    g_hl_in += in_n; g_hl_gen += gen;
    g_req_in += in_n; g_req_gen += gen; g_req_model_s += in_s + gen_s; g_req_pos = pos; g_req_ctx = ctx;
}
/* linha de estado depois de cada pedido (console rico) */
static void launch_status(void) {
    if (!g_con_rich || !g_req_rounds) { g_req_rounds = 0; return; }
    const double wall = lc_now() - g_req_t0;
    char a[24], b[24], c[24], d[24], e[24];
    lc_fmt_dur(wall > 0 && wall < 1e6 ? wall : g_req_model_s, a, sizeof a);
    lc_fmt_k(g_req_in, b, sizeof b); lc_fmt_k(g_req_gen, c, sizeof c); lc_fmt_k(g_req_pos, d, sizeof d); lc_fmt_k(g_req_ctx, e, sizeof e);
    fprintf(stderr, "\n  %s%s \xC2\xB7 %s in \xC2\xB7 %s out \xC2\xB7 %.1f tok/s \xC2\xB7 context %s/%s (%d%%)%s\n", CE(C_DIM), a, b, c,
            g_req_gen / (g_req_model_s + 1e-9), d, e, g_req_ctx ? (int)(100L * g_req_pos / g_req_ctx) : 0, CE(C_RESET));
    g_req_in = g_req_gen = 0; g_req_model_s = 0; g_req_rounds = 0;
}
static void launch_request_start(void) { g_req_t0 = lc_now(); }

/* ---- cabecalho ---- */
static int lc_count_skills(char *desc, size_t cap) {
    char roots[4][1100];
    const int nr = sk_roots(roots);
    int total = 0; size_t o = 0;
    desc[0] = 0;
    for (int i = 0; i < nr; i++) {
        DIR *d = opendir(roots[i]);
        if (!d) continue;
        int k = 0; struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char p[2400]; snprintf(p, sizeof p, "%.1100s/%.200s/SKILL.md", roots[i], e->d_name);
            if (lc_isfile(p)) k++;
        }
        closedir(d);
        if (k) { char s[1200]; o += (size_t)snprintf(desc + o, cap - o, "%s%s: %d", o ? "; " : "", lc_short(roots[i], s, sizeof s), k); total += k; }
    }
    return total;
}
static void launch_header(Model *m) {
    if (!g_con_rich) return;
    char cwd[4200], sh[4400], mp[4400], home[1024], p[4600];
    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "?");
    lc_home(home, sizeof home);
    const char *mpath = g_orig_argv && g_orig_argc >= 3 ? g_orig_argv[2] : "?";
    const char *bn = strrchr(mpath, '/'); bn = bn ? bn + 1 : mpath;
    (void)mp;
    fprintf(stderr, "%skatana%s  %sagent console%s\n", CE("\033[1m"), CE(C_RESET), CE(C_DIM), CE(C_RESET));
    fprintf(stderr, "  folder    %s\n", lc_short(cwd, sh, sizeof sh));
    fprintf(stderr, "  model     %s%s%s\n", bn, g_launch_why[0] ? "  " : "", g_launch_why[0] ? g_launch_why : "");
    fprintf(stderr, "  context   %d tokens\n", m->cfg.ctx);
    {   /* servidores MCP e de que arquivos */
        char files[600] = ""; size_t o = 0;
        const char *e = getenv("KATANA_MCP_CONFIG");
        char u[4400], pj[4400];
        if (e && *e) { if (lc_isfile(e)) o += (size_t)snprintf(files + o, sizeof files - o, "%s", lc_short(e, sh, sizeof sh)); }
        else { snprintf(u, sizeof u, "%s/.katana/mcp.json", home); if (lc_isfile(u)) o += (size_t)snprintf(files + o, sizeof files - o, "~/.katana/mcp.json"); }
        snprintf(pj, sizeof pj, "%s/.katana/mcp.json", cwd);
        if (!getenv("KATANA_MCP_STRICT") && lc_isfile(pj)) o += (size_t)snprintf(files + o, sizeof files - o, "%s./.katana/mcp.json", o ? ", " : "");
        { char rj[4400]; if (mcpx_root_file(rj, sizeof rj)) o += (size_t)snprintf(files + o, sizeof files - o, "%s%s", o ? ", " : "", lc_short(rj, sh, sizeof sh)); }      /* .mcp.json da raiz do projeto */
        int alive = 0; for (int i = 0; i < g_nmcp; i++) alive += g_mcp[i].alive;
        if (g_nmcp) fprintf(stderr, "  mcp       %d server%s (%d up) from %s\n", g_nmcp, g_nmcp == 1 ? "" : "s", alive, files[0] ? files : "configuration");
        else fprintf(stderr, "  mcp       none\n");
        for (int i = 0; i < g_nmcp; i++) if (!g_mcp[i].alive) fprintf(stderr, "            %s%s: unavailable (%s)%s\n", CE(C_DIM), g_mcp[i].name, g_mcp[i].err, CE(C_RESET));
    }
    {   char sd[1500]; const int ns = lc_count_skills(sd, sizeof sd);
        if (ns) fprintf(stderr, "  skills    %d (%s)\n", ns, sd); else fprintf(stderr, "  skills    none\n"); }
    {   static char pi[PROJ_MAX + 200]; char pp[4400]; char list[1200] = ""; size_t o = 0;
        snprintf(p, sizeof p, "%s/.katana/KATANA.md", home);
        if (lc_isfile(p)) o += (size_t)snprintf(list + o, sizeof list - o, "~/.katana/KATANA.md (%.1f KB)", file_bytes(p) / 1024.0);
        const size_t pl = proj_instr(pi, sizeof pi, pp, sizeof pp);
        if (pl) o += (size_t)snprintf(list + o, sizeof list - o, "%s%s (%.1f KB)", o ? ", " : "", lc_short(pp, sh, sizeof sh), file_bytes(pp) / 1024.0);
        fprintf(stderr, "  project   %s\n", list[0] ? list : "no KATANA.md or AGENTS.md");
    }
    if (g_sess_path[0]) fprintf(stderr, "  session   %s\n", lc_short(g_sess_path, sh, sizeof sh));
    fprintf(stderr, "  %s/help lists the commands. Ctrl+C interrupts an answer, Ctrl+D leaves.%s\n", CE(C_DIM), CE(C_RESET));
}

/* ---- /status e /model ---- */
static int launch_cmd(const char *line, Model *m, int pos, const char *sys, const AHist *hist, const AHist *slog) {
    if (strcmp(line, "/status") == 0) {
        char cwd[4200], sh[4400], sd[1500];
        if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
        const char *mpath = g_orig_argc >= 3 ? g_orig_argv[2] : "?";
        fprintf(stderr, "  folder    %s\n  model     %s\n  context   %d of %d tokens (%d%%)\n", cwd, mpath, pos, m->cfg.ctx, m->cfg.ctx ? 100 * pos / m->cfg.ctx : 0);
        fprintf(stderr, "  session   %s\n", g_sess_path[0] ? lc_short(g_sess_path, sh, sizeof sh) : "not saved yet");
        int alive = 0; for (int i = 0; i < g_nmcp; i++) alive += g_mcp[i].alive;
        fprintf(stderr, "  mcp       %d server%s (%d up)\n", g_nmcp, g_nmcp == 1 ? "" : "s", alive);
        for (int i = 0; i < g_nmcp; i++) fprintf(stderr, "            %s [%s] %s%s%s\n", g_mcp[i].name, mcpx_kind(&g_mcp[i]), g_mcp[i].alive ? "up" : "unavailable: ", g_mcp[i].alive ? "" : g_mcp[i].err, "");
        const int ns = lc_count_skills(sd, sizeof sd);
        fprintf(stderr, "  skills    %d%s%s\n", ns, ns ? " in " : "", sd);
        static char pi[PROJ_MAX + 200]; char pp[4400];
        if (proj_instr(pi, sizeof pi, pp, sizeof pp)) fprintf(stderr, "  project   %s (%.1f KB)\n", lc_short(pp, sh, sizeof sh), file_bytes(pp) / 1024.0);
        else fprintf(stderr, "  project   no KATANA.md or AGENTS.md\n");
        return 1;
    }
    if (strncmp(line, "/model", 6) == 0 && (line[6] == 0 || line[6] == ' ')) {
        const char *a = line + 6; while (*a == ' ') a++;
        static LcModelFile v[256];
        const char *mpath = g_orig_argc >= 3 ? g_orig_argv[2] : "?";
        if (!*a) {
            const char *dir = models_dir();
            const int n = lc_models_scan(dir, v, 256);
            fprintf(stderr, "  current   %s\n  folder    %s\n", mpath, dir);
            for (int i = 0; i < n; i++) fprintf(stderr, "  %s %s  (%.1f GB)\n", strstr(mpath, v[i].name) ? "*" : " ", v[i].name, v[i].bytes / 1073741824.0);
            fprintf(stderr, "  /model <name> switches (the conversation is saved and the model reloads)\n");
            return 1;
        }
#ifdef _WIN32
        fprintf(stderr, "[/model: switching needs a restart on Windows: start katana --model %s]\n", a);
        (void)sys; (void)hist; (void)slog;
        return 1;
#else
        char path[4600], err[1500];
        if (lc_model_pick(a, path, sizeof path, err, sizeof err) != 0) { fprintf(stderr, "[/model: %s]\n", err); return 1; }
        char self[4096];
        if (!exe_path(self, sizeof self)) { fprintf(stderr, "[/model: cannot find the katana executable; restart with --model %s]\n", a); return 1; }
        if (mcpch_pending()) { fprintf(stderr, "[/model: channel messages are waiting; try again after they run]\n"); return 1; }
        if (hist->n && g_sess_path[0]) sess_save(m, sys, hist, slog, 0);
        fprintf(stderr, "%s[switching to %s; reloading]%s\n", CE(C_DIM), path, CE(C_RESET));
        char *av[8]; int k = 0;
        av[k++] = self; av[k++] = "agent"; av[k++] = path;
        if (hist->n && g_sess_path[0]) { av[k++] = "--retomar"; av[k++] = g_sess_path; setenv("KATANA_RESUME_OTHER", "1", 1); }
        av[k] = NULL;
        fflush(stdout); fflush(stderr);
        execv(self, av);
        fprintf(stderr, "[/model: reload failed: %s]\n", strerror(errno));
        return 1;
#endif
    }
    return 0;
}

/* ---- ajuste do prompt de sistema (--system-prompt-file, --append-system-prompt) ---- */
static void launch_sys_adjust(char *out, size_t cap) {
    const char *rep = getenv("KATANA_SYSTEM_FILE"), *app = getenv("KATANA_SYSTEM_APPEND");
    if (rep && *rep) {
        char *t = lc_slurp(rep, NULL);
        if (t) {
            char *tail = strstr(out, "\n# Tools\n");
            if (!tail) tail = strstr(out, "\n# Environment");
            char *keep = tail ? strdup(tail) : strdup("");
            snprintf(out, cap, "%s%s", t, keep);
            free(keep); free(t);
        }
    }
    if (app && *app) { const size_t l = strlen(out); snprintf(out + l, cap - l, "\n\n%s", app); }
}

/* ============================ editor de linha (termios) ============================ */
#ifndef _WIN32
static struct termios g_ed_old; static int g_ed_raw;
static char **g_ed_hist; static int g_ed_nh;
static void ed_restore(void) {
    if (!g_ed_raw) return;
    (void)!write(2, "\033[?2004l", 8);
    tcsetattr(0, TCSANOW, &g_ed_old);
    g_ed_raw = 0;
}
static int ed_enter(void) {
    struct termios t;
    if (tcgetattr(0, &g_ed_old) != 0) return 0;
    t = g_ed_old;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &t) != 0) return 0;
    g_ed_raw = 1;
    (void)!write(2, "\033[?2004h", 8);
    return 1;
}
static void ed_hist_add(const char *s) {
    if (!s[0]) return;
    if (g_ed_nh && !strcmp(g_ed_hist[g_ed_nh - 1], s)) return;
    g_ed_hist = realloc(g_ed_hist, sizeof(char *) * (size_t)(g_ed_nh + 1));
    g_ed_hist[g_ed_nh++] = strdup(s);
    if (!g_hist_path[0]) return;
    FILE *f = fopen(g_hist_path, "a");
    if (!f) return;
    for (const char *q = s; *q; q++) { if (*q == '\n') fputs("\\n", f); else if (*q == '\\') fputs("\\\\", f); else fputc(*q, f); }
    fputc('\n', f); fclose(f);
}
static void ed_hist_load(void) {
    if (!g_hist_path[0]) return;
    FILE *f = fopen(g_hist_path, "r");
    if (!f) return;
    static char l[20000];
    while (fgets(l, sizeof l, f)) {
        l[strcspn(l, "\n")] = 0;
        char *o = l;
        for (char *q = l; *q; q++) { if (*q == '\\' && q[1] == 'n') { *o++ = '\n'; q++; } else if (*q == '\\' && q[1] == '\\') { *o++ = '\\'; q++; } else *o++ = *q; }
        *o = 0;
        if (!l[0]) continue;
        g_ed_hist = realloc(g_ed_hist, sizeof(char *) * (size_t)(g_ed_nh + 1));
        g_ed_hist[g_ed_nh++] = strdup(l);
    }
    fclose(f);
    if (g_ed_nh > 600) {      /* poda o arquivo: so os 400 mais novos */
        FILE *w = fopen(g_hist_path, "w");
        if (w) { for (int i = g_ed_nh - 400; i < g_ed_nh; i++) { for (const char *q = g_ed_hist[i]; *q; q++) { if (*q == '\n') fputs("\\n", w); else if (*q == '\\') fputs("\\\\", w); else fputc(*q, w); } fputc('\n', w); } fclose(w); }
        memmove(g_ed_hist, g_ed_hist + (g_ed_nh - 400), 400 * sizeof(char *)); g_ed_nh = 400;
    }
}
static int ed_width(const char *s, size_t n) { int w = 0; for (size_t i = 0; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) w++; return w; }
/* o texto como aparece na tela: quebra de linha dentro do texto colado vira o simbolo de retorno */
static void ed_show(Buf *o, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) { if (s[i] == '\n') buf_str(o, "\xE2\x86\xB5"); else if ((unsigned char)s[i] < 0x20 && s[i] != '\t') buf_str(o, " "); else buf_add(o, s + i, 1); }
}
static int g_ed_row;      /* linha (relativa ao inicio do texto) em que o cursor ficou depois do ultimo desenho */
static void ed_draw(const char *buf, size_t len, size_t cur) {
    con_term_cols();
    Buf o = {0};
    if (g_ed_row > 0) { char t[24]; snprintf(t, sizeof t, "\033[%dA", g_ed_row); buf_str(&o, t); }
    buf_str(&o, "\r\033[J");
    buf_str(&o, g_con_color ? "\033[36myou>\033[0m " : "you> ");
    ed_show(&o, buf, len);
    const int total = 5 + ed_width(buf, len), cc = 5 + ed_width(buf, cur);
    if (total > 0 && total % g_con_cols == 0) buf_str(&o, " \r");      /* cursor no limite da linha: forca a quebra */
    const int endrow = total / g_con_cols, currow = cc / g_con_cols, curcol = cc % g_con_cols;
    if (endrow > currow) { char t[24]; snprintf(t, sizeof t, "\033[%dA", endrow - currow); buf_str(&o, t); }
    { char t[24]; snprintf(t, sizeof t, "\r\033[%dC", curcol); buf_str(&o, curcol ? t : "\r"); }
    g_ed_row = currow;
    (void)!write(2, o.p, o.n);
    free(o.p);
}
static int ed_ready(void) {
    static int v = -1;
    if (v < 0) {
        const char *t = getenv("TERM");
        v = K_ISATTY(0) && K_ISATTY(2) && !getenv("KATANA_UI") && !(t && !strcmp(t, "dumb")) && !getenv("KATANA_PLAIN_INPUT");
    }
    return v;
}
static int ed_poll(int ms) { struct pollfd pf = {0, POLLIN, 0}; return poll(&pf, 1, ms) > 0; }
static int g_ed_idle;            /* o laco do agente esta esperando: o editor acorda para entregar o que chegou */
static char *g_ed_draft;         /* linha pela metade quando algo foi entregue; volta no proximo prompt */
/* espera uma tecla; 2 = algo foi entregue em line. Com uma linha pela metade, so entrega depois de um tempo sem teclar */
static int ed_wait(size_t len, double lastkey, char *line, size_t cap) {
    if (!g_ed_idle) return 1;
    const double quiet = chn_env("KATANA_CHANNEL_QUIET", 5, 0, 3600);
    for (;;) {
        if (ed_poll(0)) return 1;
        if ((!len || now_sec() - lastkey >= quiet) && ct_idle_line(line, cap)) return 2;
        int ms = ct_idle_ms();
        if (len && ms > 500) ms = 500;
        if (mcpch_wait(ms, 1)) return 1;
        if (len) mcpch_pump(0);          /* com a linha pela metade a mensagem entra na fila e espera */
    }
}
/* le uma linha com edicao; 0 = fim da entrada (Ctrl+D numa linha vazia); 2 = entrega (ver ed_wait) */
static int ed_read_line(char *line, size_t cap) {
    if (!ed_enter()) return 0;
    char *buf = xalloc(cap + 8, 1);
    size_t len = 0, cur = 0;
    int hi = g_ed_nh, paste = 0, ret = 1, lastcr = 0;
    char *draft = NULL;
    double lastkey = now_sec();
    if (g_ed_draft) { len = strlen(g_ed_draft) < cap ? strlen(g_ed_draft) : cap - 1; memcpy(buf, g_ed_draft, len); cur = len; free(g_ed_draft); g_ed_draft = NULL; }
    g_ed_row = 0;
    ed_draw(buf, len, cur);
    for (;;) {
        unsigned char c;
        if (!paste && ed_wait(len, lastkey, line, cap) == 2) { ret = 2; break; }
        lastkey = now_sec();
        const ssize_t r = read(0, &c, 1);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; ret = 0; break; }
        if (paste) {       /* texto colado (marcas ESC[200~ ... ESC[201~): as quebras de linha fazem parte do texto */
            if (c == 0x1b) {
                unsigned char s[5]; int k = 0;
                while (k < 5 && ed_poll(50) && read(0, &s[k], 1) == 1) k++;
                if (k == 5 && !memcmp(s, "[201~", 5)) { paste = 0; ed_draw(buf, len, cur); }
                continue;
            }
            if (c == '\n' && lastcr) { lastcr = 0; continue; }
            lastcr = c == '\r';
            if (c == '\r') c = '\n';
            if (len + 1 < cap) { memmove(buf + cur + 1, buf + cur, len - cur); buf[cur++] = (char)c; len++; }
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (len && buf[len - 1] == '\\') { buf[len - 1] = '\n'; cur = len; ed_draw(buf, len, cur); continue; }     /* barra no fim: continua na linha seguinte */
            break;
        }
        if (c == 0x1b) {
            unsigned char b1, f = 0, x; char pr[12]; int k = 0;
            if (!ed_poll(30) || read(0, &b1, 1) != 1) continue;
            if (b1 == '[') { while (k < 10 && ed_poll(30) && read(0, &x, 1) == 1) { if (x >= 0x40 && x <= 0x7e) { f = x; break; } pr[k++] = (char)x; } }
            else if (b1 == 'O') { if (ed_poll(30) && read(0, &x, 1) == 1) f = x; }
            else if (b1 == '\r' || b1 == '\n') { if (len + 1 < cap) { memmove(buf + cur + 1, buf + cur, len - cur); buf[cur++] = '\n'; len++; } ed_draw(buf, len, cur); continue; }     /* Alt+Enter */
            else if (b1 == 'b') { while (cur > 0 && buf[cur - 1] == ' ') cur--; while (cur > 0 && buf[cur - 1] != ' ') cur--; ed_draw(buf, len, cur); continue; }
            else if (b1 == 'f') { while (cur < len && buf[cur] == ' ') cur++; while (cur < len && buf[cur] != ' ') cur++; ed_draw(buf, len, cur); continue; }
            else continue;
            pr[k] = 0;
            if (f == '~' && !strcmp(pr, "200")) { paste = 1; continue; }
            if (f == 'D') { while (cur > 0 && ((unsigned char)buf[--cur] & 0xC0) == 0x80) ; }
            else if (f == 'C') { if (cur < len) { cur++; while (cur < len && ((unsigned char)buf[cur] & 0xC0) == 0x80) cur++; } }
            else if (f == 'H' || (f == '~' && (pr[0] == '1' || pr[0] == '7'))) cur = 0;
            else if (f == 'F' || (f == '~' && (pr[0] == '4' || pr[0] == '8'))) cur = len;
            else if (f == '~' && pr[0] == '3') { if (cur < len) { size_t e = cur + 1; while (e < len && ((unsigned char)buf[e] & 0xC0) == 0x80) e++; memmove(buf + cur, buf + e, len - e); len -= e - cur; } }
            else if (f == 'A' || f == 'B') {
                if (hi == g_ed_nh && f == 'A') { free(draft); draft = strndup(buf, len); }
                if (f == 'A' && hi > 0) hi--; else if (f == 'B' && hi < g_ed_nh) hi++; else continue;
                const char *src = hi == g_ed_nh ? (draft ? draft : "") : g_ed_hist[hi];
                len = strlen(src) < cap ? strlen(src) : cap - 1; memcpy(buf, src, len); cur = len;
            }
            ed_draw(buf, len, cur);
            continue;
        }
        if (c == 4) { if (len == 0) { ret = 0; break; } if (cur < len) { size_t e = cur + 1; while (e < len && ((unsigned char)buf[e] & 0xC0) == 0x80) e++; memmove(buf + cur, buf + e, len - e); len -= e - cur; } }
        else if (c == 1) cur = 0;
        else if (c == 5) cur = len;
        else if (c == 2) { while (cur > 0 && ((unsigned char)buf[--cur] & 0xC0) == 0x80) ; }
        else if (c == 6) { if (cur < len) { cur++; while (cur < len && ((unsigned char)buf[cur] & 0xC0) == 0x80) cur++; } }
        else if (c == 11) len = cur;
        else if (c == 21) { memmove(buf, buf + cur, len - cur); len -= cur; cur = 0; }
        else if (c == 23) { size_t s0 = cur; while (s0 > 0 && buf[s0 - 1] == ' ') s0--; while (s0 > 0 && buf[s0 - 1] != ' ') s0--; memmove(buf + s0, buf + cur, len - cur); len -= cur - s0; cur = s0; }
        else if (c == 12) { (void)!write(2, "\033[2J\033[H", 7); g_ed_row = 0; }
        else if (c == 16 || c == 14) { /* Ctrl+P / Ctrl+N: historico, como as setas */
            if (c == 16 && hi == g_ed_nh) { free(draft); draft = strndup(buf, len); }
            if (c == 16 && hi > 0) hi--; else if (c == 14 && hi < g_ed_nh) hi++; else continue;
            const char *src = hi == g_ed_nh ? (draft ? draft : "") : g_ed_hist[hi];
            len = strlen(src) < cap ? strlen(src) : cap - 1; memcpy(buf, src, len); cur = len;
        }
        else if (c == 127 || c == 8) { if (cur > 0) { size_t s0 = cur - 1; while (s0 > 0 && ((unsigned char)buf[s0] & 0xC0) == 0x80) s0--; memmove(buf + s0, buf + cur, len - cur); len -= cur - s0; cur = s0; } }
        else if (c == '\t') { if (len + 4 < cap) { memmove(buf + cur + 4, buf + cur, len - cur); memset(buf + cur, ' ', 4); cur += 4; len += 4; } }
        else if (c >= 0x20 || c >= 0x80) { if (len + 1 < cap) { memmove(buf + cur + 1, buf + cur, len - cur); buf[cur++] = (char)c; len++; } }
        ed_draw(buf, len, cur);
    }
    buf[len] = 0;
    if (ret == 2 && len) g_ed_draft = strdup(buf);
    if (ret == 1) { cur = len; ed_draw(buf, len, cur); (void)!write(2, "\r\n", 2); }
    ed_restore();
    if (ret == 1) { snprintf(line, cap, "%s", buf); ed_hist_add(buf); }
    free(buf); free(draft);
    return ret;
}
static int launch_read_line(char *line, size_t cap, int *res) {
    if (g_hl) {        /* headless: o pedido e a unica linha */
        if (g_hl_prompt) { snprintf(line, cap, "%s", g_hl_prompt); free(g_hl_prompt); g_hl_prompt = NULL; *res = 1; }
        else *res = 0;
        return 1;
    }
    if (!ed_ready()) return 0;
    *res = ed_read_line(line, cap);
    return 1;
}
static int launch_editing(void) { return !g_hl && ed_ready(); }
static int launch_idle_read(char *line, size_t cap) {
    g_ed_idle = 1;
    const int r = read_line(line, cap);
    g_ed_idle = 0;
    return r;
}
#else
static void ed_restore(void) {}
static int launch_read_line(char *line, size_t cap, int *res) {
    if (g_hl) {
        if (g_hl_prompt) { snprintf(line, cap, "%s", g_hl_prompt); free(g_hl_prompt); g_hl_prompt = NULL; *res = 1; } else *res = 0;
        return 1;
    }
    (void)line; (void)cap; (void)res;
    return 0;
}
#endif

/* ============================ install / uninstall ============================ */
static int lc_on_path(const char *dir) {
    const char *p = getenv("PATH");
    if (!p) return 0;
    char rd[4400]; lc_realpath(dir, rd, sizeof rd);
    for (const char *s = p; *s; ) {
        const char *e = strchr(s, ':'); if (!e) e = s + strlen(s);
        char one[4400], ro[4400];
        snprintf(one, sizeof one, "%.*s", (int)(e - s < 4300 ? e - s : 4300), s);
        if (one[0]) { lc_realpath(one, ro, sizeof ro); if (!strcmp(ro, rd) || !strcmp(one, dir)) return 1; }
        s = *e ? e + 1 : e;
    }
    return 0;
}
static int install_cmd(int argc, char **argv, int un) {
#ifdef _WIN32
    (void)argc; (void)argv; (void)un;
    fprintf(stderr, "katana %s: not available on Windows; add the folder of katana.exe to PATH.\n", un ? "uninstall" : "install");
    return 1;
#else
    char dir[4300] = "", home[1024]; int force = 0;
    lc_home(home, sizeof home);
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) snprintf(dir, sizeof dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--force")) force = 1;
        else { fprintf(stderr, "uso: katana %s [--dir DIR] [--force]\n", un ? "uninstall" : "install"); return 1; }
    }
    if (!dir[0]) snprintf(dir, sizeof dir, "%s/.local/bin", home);
    char self[4096], link[4500];
    if (!exe_path(self, sizeof self)) { fprintf(stderr, "katana: nao consegui descobrir o caminho deste executavel\n"); return 1; }
    snprintf(link, sizeof link, "%s/katana", dir);
    struct stat ls;
    const int have = lstat(link, &ls) == 0;
    char target[4400] = ""; int same = 0;
    if (have && S_ISLNK(ls.st_mode)) { char rt[4400]; lc_realpath(link, rt, sizeof rt); snprintf(target, sizeof target, "%s", rt); same = !strcmp(rt, self); }
    if (un) {
        if (!have) { printf("nada a remover: %s nao existe\n", link); return 0; }
        if (!S_ISLNK(ls.st_mode)) { fprintf(stderr, "katana: %s nao e um link simbolico; nao removi\n", link); return 1; }
        if (!same && !force) { fprintf(stderr, "katana: %s aponta para %s, nao para este executavel; use --force para remover\n", link, target); return 1; }
        if (unlink(link) != 0) { fprintf(stderr, "katana: nao removi %s: %s\n", link, strerror(errno)); return 1; }
        printf("removido: %s\n(o models= de ~/.katana/katana.cfg, se existir, ficou; apague a linha se quiser)\n", link);
        return 0;
    }
    mkdir_parents(link);
    mkdir(dir, 0755);
    if (!lc_isdir(dir)) { fprintf(stderr, "katana: nao consegui criar %s\n", dir); return 1; }
    if (have) {
        if (same) printf("ja instalado: %s -> %s\n", link, self);
        else if (!force) { fprintf(stderr, "katana: %s ja existe%s%s; use --force para trocar\n", link, target[0] ? " e aponta para " : "", target); return 1; }
        else if (unlink(link) != 0) { fprintf(stderr, "katana: nao removi %s: %s\n", link, strerror(errno)); return 1; }
    }
    if (!have || !same) {
        if (symlink(self, link) != 0) { fprintf(stderr, "katana: nao criei o link %s: %s\n", link, strerror(errno)); return 1; }
        printf("instalado: %s -> %s\n", link, self);
    }
    if (!lc_on_path(dir)) {
        printf("\nATENCAO: %s nao esta no PATH. Acrescente ao ~/.bashrc (ou ~/.zshrc) e abra um terminal novo:\n  export PATH=\"%s:$PATH\"\n", dir, dir);
    }
    /* a pasta de modelos tem de ser achada de qualquer pasta: ao lado do executavel, grava models= no katana.cfg */
    char ed[4300] = "", mdir[4400];
    if (exe_dir(ed, sizeof ed)) {
        snprintf(mdir, sizeof mdir, "%s/models", ed);
        LcModelFile v[4];
        if (lc_isdir(mdir) && lc_models_scan(mdir, v, 4) > 0 && !(getenv("KATANA_MODELS") && *getenv("KATANA_MODELS"))) {
            if (lc_cfg_set("models", mdir)) printf("modelos: %s (gravado como models= em ~/.katana/katana.cfg)\n", mdir);
            else printf("aviso: nao consegui gravar ~/.katana/katana.cfg; defina KATANA_MODELS=%s\n", mdir);
        }
    }
    char cwd0[4200] = "";
    if (getcwd(cwd0, sizeof cwd0) && chdir("/") == 0) {       /* confere de uma pasta qualquer, como o launcher vai ver */
        const char *md = models_dir();
        LcModelFile v[4];
        const int n = lc_models_scan(md, v, 4);
        printf("pasta de modelos vista de qualquer pasta: %s (%s)\n", md, n ? "tem modelos" : "vazia: katana catalog / katana pull <id>");
        if (chdir(cwd0) != 0) { /* sem efeito */ }
    }
    printf("\nPronto. Numa pasta de projeto, digite: katana\n");
    return 0;
#endif
}

/* ============================ adopt ============================ */
static int adopt_copy_file(const char *src, const char *dst, int force, int dry, int *nw) {
    if (lc_exists(dst) && !force) { printf("  pulou (ja existe, use --force): %s\n", dst); return 0; }
    size_t n; char *t = lc_slurp(src, &n);
    if (!t) { printf("  nao li: %s\n", src); return 0; }
    if (dry) printf("  copiaria: %s -> %s\n", src, dst);
    else if (!lc_write(dst, t, n)) { printf("  nao gravei: %s\n", dst); free(t); return 0; }
    else printf("  gravou: %s\n", dst);
    free(t); (*nw)++;
    return 1;
}
static void adopt_copy_tree(const char *src, const char *dst, int force, int dry, int *nw, int depth) {
    DIR *d = opendir(src);
    if (!d || depth > 5) { if (d) closedir(d); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, ".git")) continue;
        char s[4600], t[4600]; lc_join(s, sizeof s, src, e->d_name); lc_join(t, sizeof t, dst, e->d_name);
        if (lc_isdir(s)) adopt_copy_tree(s, t, force, dry, nw, depth + 1);
        else if (lc_isfile(s)) adopt_copy_file(s, t, force, dry, nw);
    }
    closedir(d);
}
static int adopt_cmd(int argc, char **argv) {
    const char *dir = ".", *instr = NULL, *mcp = NULL, *skills = NULL;
    int dry = 0, force = 0, dset = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--instructions") && i + 1 < argc) instr = argv[++i];
        else if (!strcmp(argv[i], "--mcp") && i + 1 < argc) mcp = argv[++i];
        else if (!strcmp(argv[i], "--skills") && i + 1 < argc) skills = argv[++i];
        else if (!strcmp(argv[i], "--dry-run")) dry = 1;
        else if (!strcmp(argv[i], "--force")) force = 1;
        else if (argv[i][0] != '-' && !dset) { dir = argv[i]; dset = 1; }
        else { fprintf(stderr, "uso: katana adopt [dir] [--instructions ARQ] [--mcp ARQ] [--skills PASTA] [--dry-run] [--force]\n"); return 1; }
    }
    if (!instr && !mcp && !skills) { fprintf(stderr, "katana adopt: informe pelo menos um de --instructions, --mcp, --skills (os caminhos de um projeto que ja tem agente configurado)\n"); return 1; }
    if (!lc_isdir(dir)) { fprintf(stderr, "katana adopt: %s nao e uma pasta\n", dir); return 1; }
    char root[4300]; lc_realpath(dir, root, sizeof root);
    int nw = 0;
    printf("%sProjeto: %s\n", dry ? "[dry-run] " : "", root);
    if (instr) {
        char dst[4500]; snprintf(dst, sizeof dst, "%s/KATANA.md", root);
        printf("Instrucoes:\n");
        if (!lc_isfile(instr)) printf("  nao achei %s\n", instr);
        else if (adopt_copy_file(instr, dst, force, dry, &nw)) printf("  (copiado como esta; o katana le texto simples. /init dentro do agente pode reescrever no formato compacto do katana)\n");
    }
    if (mcp) {
        char dst[4500]; snprintf(dst, sizeof dst, "%s/.katana/mcp.json", root);
        printf("Servidores MCP:\n");
        char *src = lc_slurp(mcp, NULL);
        const char *sr = src ? strchr(src, '{') : NULL, *ss = sr ? mcp_get(sr, "mcpServers") : NULL;
        if (!src) printf("  nao li %s\n", mcp);
        else if (!ss || *ss != '{') printf("  %s nao tem \"mcpServers\"\n", mcp);
        else {
            char *old = lc_slurp(dst, NULL);
            const char *or_ = old ? strchr(old, '{') : NULL, *os = or_ ? mcp_get(or_, "mcpServers") : NULL;
            if (old && (!os || *os != '{') && !force) printf("  %s existe mas nao tem \"mcpServers\"; nao mexi (use --force para recriar)\n", dst);
            else {
                Buf o = {0}; int n = 0, added = 0, kept = 0;
                char key[128], k2[128]; const char *v, *np, *v2, *np2;
                buf_str(&o, "{\n  \"mcpServers\": {\n");
                if (os && *os == '{') for (const char *p = os + 1; (np = mcp_member(p, key, sizeof key, &v)) != NULL; p = np) {     /* os que ja estavam */
                    int replaced = 0;
                    for (const char *q = ss + 1; (np2 = mcp_member(q, k2, sizeof k2, &v2)) != NULL; q = np2) if (!strcmp(key, k2)) replaced = 1;
                    if (replaced && force) continue;
                    const char *ve = mcp_skip(v); if (!ve) continue;
                    if (n++) buf_str(&o, ",\n");
                    buf_str(&o, "    "); lc_json_str(&o, key); buf_str(&o, ": "); buf_add(&o, v, (size_t)(ve - v));
                    if (replaced) { printf("  mantido (ja existia, use --force para trocar): %s\n", key); kept++; }
                }
                for (const char *q = ss + 1; (np2 = mcp_member(q, k2, sizeof k2, &v2)) != NULL; q = np2) {
                    int exists = 0;
                    if (os && *os == '{') for (const char *p = os + 1; (np = mcp_member(p, key, sizeof key, &v)) != NULL; p = np) if (!strcmp(key, k2)) exists = 1;
                    if (exists && !force) continue;
                    const char *ve = mcp_skip(v2); if (!ve) continue;
                    if (n++) buf_str(&o, ",\n");
                    buf_str(&o, "    "); lc_json_str(&o, k2); buf_str(&o, ": "); buf_add(&o, v2, (size_t)(ve - v2));
                    printf("  servidor %s: %s\n", k2, dry ? "seria acrescentado" : "acrescentado");
                    added++;
                }
                buf_str(&o, "\n  }\n}\n");
                if (!added) printf("  nada novo para acrescentar%s\n", kept ? "" : "");
                else if (dry) printf("  gravaria: %s\n", dst);
                else if (lc_write(dst, o.p, o.n)) { printf("  gravou: %s\n", dst); nw++; }
                else printf("  nao gravei: %s\n", dst);
                free(o.p);
            }
            free(old);
        }
        free(src);
    }
    if (skills) {
        char dst[4500]; snprintf(dst, sizeof dst, "%s/.katana/skills", root);
        printf("Skills:\n");
        int ns = 0;
        char sp[4600]; snprintf(sp, sizeof sp, "%s/SKILL.md", skills);
        if (lc_isfile(sp)) {      /* a propria pasta e uma skill */
            char rp[4400]; lc_realpath(skills, rp, sizeof rp);
            const char *b = strrchr(rp, '/'); b = b ? b + 1 : rp;
            char t[4700]; snprintf(t, sizeof t, "%s/%s", dst, b);
            adopt_copy_tree(skills, t, force, dry, &nw, 0); ns++;
        } else {
            DIR *d = opendir(skills);
            struct dirent *e;
            while (d && (e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                char s[4600], t[4700]; lc_join(s, sizeof s, skills, e->d_name);
                snprintf(sp, sizeof sp, "%s/SKILL.md", s);
                if (!lc_isdir(s) || !lc_isfile(sp)) continue;
                snprintf(t, sizeof t, "%s/%s", dst, e->d_name);
                adopt_copy_tree(s, t, force, dry, &nw, 0); ns++;
            }
            if (d) closedir(d);
        }
        if (!ns) printf("  nenhuma pasta com SKILL.md em %s\n", skills);
    }
    printf("\n%s%d arquivo(s)%s.\nConfira:\n", dry ? "Nada foi gravado (--dry-run); seriam " : "Gravado: ", nw, dry ? "" : " escrito(s)");
    printf("  - KATANA.md: leia; o que for especifico de outro agente (nomes de ferramentas, comandos de barra) nao vale aqui.\n"
           "  - .katana/mcp.json: os comandos dos servidores existem nesta maquina e as variaveis de ambiente/chaves que eles pedem estao definidas.\n"
           "  - .katana/skills: skills sao instrucoes em texto; revise as que citam ferramentas que o katana nao tem (katana tool lista as dele).\n"
           "  - katana mcp lista os servidores e as ferramentas que subiram.\n");
    return 0;
}

/* variavel de ambiente que o resto do motor ja le */
static void lc_setenv(const char *k, const char *v) {
#ifdef _WIN32
    char b[8200]; snprintf(b, sizeof b, "%s=%s", k, v); _putenv(strdup(b));
#else
    setenv(k, v, 1);
#endif
}
static int launch_first_line(char *line, size_t cap) {
    const char *f = getenv("KATANA_FIRST_MESSAGE");
    if (!f || !*f || g_hl) return 0;
    snprintf(line, cap, "%s", f);
    fprintf(stderr, "%s\n", line);
    lc_setenv("KATANA_FIRST_MESSAGE", "");
    return 1;
}

/* ============================ opcoes e partida ============================ */
typedef struct {
    char channels[600];
    const char *model, *prompt, *mcp_config, *allowed, *disallowed, *sysfile, *append, *cwd, *session_id, *outfmt, *resume_id;
    int headless, cont, resume, strict, max_turns, no_session, quiet, stdin_prompt, plain;
} LcOpts;
static char g_mcp_tmp[4400];
static void lc_cleanup_tmp(void) { if (g_mcp_tmp[0]) remove(g_mcp_tmp); }
static int lc_known_cmd(const char *a) {
    static const char *const K[] = {"serve", "mcp", "jsontest", "toolrun", "tool", "agentsys", "toolparse", "calibrar", "agent", "routing", "sparsity", "tune",
        "verify", "pack", "prepare", "micro", "vkmicro", "bench", "vkcheck", "qtest", "quantize", "convert", "knowledge", "import", "merge", "distill", "agentlog", "export",
        "learn", "specialize", "gradcheck", "train", "selftest", "snaptest", "snapreplay", "attbench", "q4check", "q4bench", "q4gpu", "toktest", "tokenize", "hw", "catalog", "setup", "pull", "sha256", "models", "info", "chat", "run",
        "install", "uninstall", "adopt", "init", "skill", "brief", "memory", "calltest", "samplertest", "curriculum", "lbtest", "compare", "compacttest", "codebench", "coder", "docsel", "uioe",
        "kerneltest", "lookuptest", "spectime", "placenote", "version", "update", "vision", NULL};      /* a mesma lista do main.c: acrescente aqui os subcomandos novos */
    for (int i = 0; K[i]; i++) if (!strcmp(a, K[i])) return 1;
    return 0;
}
static void lc_usage(FILE *f) {
    fprintf(f,
        "katana [opcoes] [\"primeira mensagem\"]      console do agente na pasta atual\n"
        "  --model NOME|CAMINHO        modelo (padrao: KATANA_MODEL, model= no katana.cfg, o ultimo do aplicativo de mesa, o recomendado, o maior)\n"
        "  -c, --continue              retoma a sessao mais recente desta pasta (ou da raiz do repositorio)\n"
        "  -r, --resume [ID]           lista as sessoes desta pasta e retoma a escolhida\n"
        "  --cwd DIR                   trabalha em DIR\n"
        "  -p, --prompt [TEXTO]        headless: um pedido, uma resposta (sem TEXTO, o pedido vem da entrada padrao)\n"
        "      --output-format text|json|stream-json\n"
        "      --mcp-config ARQ|JSON   servidores MCP deste pedido; --strict-mcp-config: so estes\n"
        "      --channel NOME          o servidor MCP NOME pode mandar mensagens e responder confirmacoes (docs/channels.md)\n"
        "      --allowed-tools 'a,b'   so estas ferramentas (globs; MCP como servidor__ferramenta ou mcp__servidor__ferramenta)\n"
        "      --disallowed-tools 'a'  nao estas\n"
        "      --max-turns N           no maximo N rodadas do modelo\n"
        "      --system-prompt-file F  troca as regras do prompt de sistema; --append-system-prompt TEXTO acrescenta\n"
        "      --session-id ID         grava (ou retoma) a sessao com este id; --no-session nao grava\n"
        "      --quiet                 sem o progresso no erro padrao\n"
        "katana install [--dir DIR] [--force]   coloca o comando katana no PATH (link simbolico)   katana uninstall\n"
        "katana adopt [dir] --instructions ARQ --mcp ARQ --skills PASTA [--dry-run] [--force]   traz a configuracao de um projeto\n"
        "katana help PALAVRA           so as linhas das ajudas (opcoes, subcomandos, comandos do console) que tem a palavra\n"
        "docs/console.md tem tudo.\n");
}
/* a lista dos subcomandos (o uso geral e o `katana help <palavra>` usam a mesma) */
static void main_usage(FILE *f) {
    fprintf(f,
        "Uso:\n"
        "  katana [opcoes] [\"mensagem\"]   console do agente na pasta atual (katana --help; docs/console.md); katana -p \"pedido\" e o modo headless\n"
        "  katana install | uninstall     coloca o comando katana no PATH; katana adopt traz a configuracao de outro projeto\n"
        "  katana info <modelo.gguf>\n"
        "  katana selftest <modelo.gguf>\n"
        "  katana toktest <modelo.gguf> <casos.hex> [--chat]   ids do tokenizador (ou do formato de conversa) de cada caso\n"
        "  katana qtest                   confere a decodificacao e os produtos de cada tipo de quantizacao\n"
        "  katana convert <pasta-hf> <saida.gguf> [tipo]   checkpoint do Hugging Face (safetensors) -> GGUF (tipo: q4_k_m padrao, q4_k, q5_k_m, q6_k, q8_0, f16, bf16, f32)\n"
        "  katana quantize <entrada.gguf> [saida.gguf] [tipo]   grava um GGUF requantizado (tipo: Q8_0, Q6_K, Q5_K_M, Q4_K_M, Q2_K)\n"
        "                                 sem tipo, escolhe o melhor para esta maquina; sem saida, gera o nome a partir da entrada\n"
        "  katana bench <modelo.gguf> [tokens_prompt] [tokens_geracao]\n"
        "  katana tune <modelo.gguf>      mede e grava o melhor ajuste para a GPU desta maquina\n"
        "  katana pack <modelo.gguf>      gera os shards no layout da GPU (<modelo>.gguf.ktn)\n"
        "  katana prepare <modelo.gguf>   inspeciona, decide o plano, prepara (pack/tune/calor) e grava o perfil do modelo\n"
        "  katana verify <modelo.gguf>    confere os hashes dos shards\n"
        "  katana sparsity <modelo.gguf> <calib.txt> <aval.txt>   esparsidade por assunto (modelo denso)\n"
        "  katana routing  <modelo.gguf> <calib.txt> <aval.txt>   especialistas por assunto (modelo MoE)\n"
        "  katana tiersim run <modelo.gguf> <texto> <rastro.bin> | sim <rastro.bin>   rastro de roteamento e simulacao de caches/previsores\n"
        "  katana snaptest <modelo> [--engine]   copias do estado recorrente contra o calculo do zero\n"
        "  katana snapreplay <modelo> <.ksess...>  tokens relidos por compactacao, com e sem copias (sem rodar o modelo)\n"
        "  katana attbench [posicoes...]         custo da atencao (qwen35moe) por posicao, sem modelo\n"
        "  katana iobench <arquivo> [bloco_KB] [leituras] [bufferizado]   leitura aleatoria do disco (threads e io_uring)\n"
        "  katana run  <modelo.gguf> \"texto\" [max_tokens] [temperatura]\n"
        "  katana chat <modelo.gguf> \"pergunta\" [max_tokens] [temperatura]\n"
        "                                 --image ARQ (run, chat, agent): imagem na mensagem, com o projetor mmproj*.gguf ao lado do modelo\n"
        "  katana vision info|stages|bench|fuzz ...   codificador de imagem: cabecalho, etapas, custo, teste dos decodificadores\n"
        "  katana train <modelo.gguf> <dados.jsonl> [--train padroes] [--rank R] [--experts N] [--steps N] [--lr X] [--seq T] [--out f.kspec]\n"
        "                                       especializa o modelo (grava so os tensores treinados; KATANA_SPEC=f.kspec aplica)\n"
        "  katana import <modelo.gguf> <entrada.jsonl> <saida.jsonl> [--system keep|ksess:ARQ|file:ARQ|none] [--only-known-tools]\n"
        "                                       conversas (mensagens ou ShareGPT) -> dados de treino no formato exato do modelo\n"
        "  katana distill --url URL --model M [--format qwen|json|gemma4|glm] [--out f.jsonl] \"tarefa\"\n"
        "  katana export <saida.jsonl> <sessoes.ksess ou pastas> [--bench resultados.jsonl]   sessoes do agente -> dados de treino\n"
        "  katana learn <modelo.gguf> [--rounds N] [--repeat K] [--teacher URL --teacher-model M] [-- opcoes do train]\n"
        "                                 aprende com o proprio uso: coleta no codebench, treina no que passou, prova e guarda o melhor\n"
        "  katana codebench run --model M.gguf --out r.jsonl [--tasks 'a,b-*'] [--repeat N] [--timeout S] [--label L] [--env K=V]...\n"
        "  katana codebench verify|list [--tasks ...]   /   katana codebench compare A.jsonl B.jsonl\n"
        "                                 o juiz: tarefas com testes escondidos (tools/codebench); o agente roda como processo filho\n"
        "  katana specialize <modelo.gguf> [all|teacher|tools|code|knowledge] [--from PASTA] [--sessions PASTA] [--hours H] [--wait-idle [H]] [--dry-run]\n"
        "                                 a rotina da noite: conhecimento, dados, plano de memoria, treino e relatorio (padroes no katana.cfg)\n"
        "  katana specialize --status           relatorio das noites (janelas vistas, perda, comandos para testar e ativar)\n"
        "  katana knowledge quiz <modelo.gguf> <holdout.jsonl> [--limit N] [--spec ARQ.kspec]   o conhecimento entrou? (nota por sobreposicao)\n"
        "                                       um modelo professor (API no formato da OpenAI) resolve a tarefa com as ferramentas do katana\n"
        "  katana merge <modelo.gguf> <espec.kspec> <saida.gguf>   GGUF novo com a especializacao dentro\n"
        "  katana compare <modelo.gguf> --val-data F [--drift-data G] a.kspec [b.kspec ...]   perda por fonte e deriva (KL) de especializacoes contra a base\n"
        "  katana gradcheck <modelo.gguf> [T]   confere o passo para tras do treino (forward x motor, gradientes x diferencas finitas)\n"
        "  katana serve <modelo.gguf> [porta]   conversa no navegador e API da OpenAI em http://127.0.0.1:porta (padrao 8080;\n"
        "                                       so conversa). Rede: KATANA_HOST=0.0.0.0 KATANA_API_KEY=<chave de 16+ caracteres>\n"
        "  katana serve <modelo.gguf> [porta] --agent   o agente pela rede (/v1/agent/sessions, docs/api.md): quem tem a chave\n"
        "                                       executa comandos nesta maquina\n"
        "  katana agent <modelo.gguf>     conversa com ferramentas (ler, listar, gravar e executar; KATANA_CONFIRM=1 pede confirmacao)\n"
        "  katana chat <modelo.gguf>      conversa interativa de varios turnos (KATANA_SYSTEM = prompt de sistema)\n");
}
/* katana help <palavra...>: so as linhas das ajudas que tem alguma das palavras (sem diferenciar maiusculas), por secao.
   Serve para perguntar por um assunto sem despejar a ajuda inteira; uma linha de continuacao acompanha a linha de cima */
static int lc_help_has(const char *line, const char *w) {
    const size_t n = strlen(w);
    for (const char *p = line; *p; p++) { size_t i = 0; while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)w[i])) i++; if (i == n) return 1; }
    return 0;
}
static int lc_help_topic(int argc, char **argv) {
    static const char *const title[3] = {"katana --help", "katana help", "console /help"};
    int total = 0;
    for (int s = 0; s < 3; s++) {
        FILE *tf = tmpfile(); if (!tf) continue;
        if (s == 0) lc_usage(tf); else if (s == 1) main_usage(tf); else con_help_text(tf);
        rewind(tf);
        char line[1200]; int shown = 0, prev = 0;
        while (fgets(line, sizeof line, tf)) {
            int hit = 0, lead = 0;
            while (line[lead] == ' ') lead++;
            for (int i = 2; i < argc && !hit; i++) if (argv[i][0]) hit = lc_help_has(line, argv[i]);
            if (!hit && prev && lead >= 20) hit = 1;      /* continuacao da linha de cima */
            prev = hit;
            if (!hit) continue;
            if (!shown++) printf("%s:\n", title[s]);
            printf("  %s", line + (lead > 2 ? 2 : lead)); total++;
        }
        fclose(tf);
    }
    if (!total) { printf("no help line has \"%s\". `katana --help` lists the options, `katana help` the subcommands, /help the console commands.\n", argv[2]); return 1; }
    return 0;
}
static int lc_looks_like_id(const char *a) { return isdigit((unsigned char)a[0]) && strlen(a) >= 6 && strspn(a, "0123456789-") == strlen(a); }
static int lc_stdin_all(Buf *b) {
    char t[4096]; size_t n;
    while ((n = fread(t, 1, sizeof t, stdin)) > 0) buf_add(b, t, n);
    return b->n > 0;
}
/* saida do modo headless */
static int launch_finish(int rc) {
    const double dur = (lc_now() - g_hl_t0) * 1000.0;
    const char *txt = g_hl_last ? g_hl_last : "";
    int err = rc != 0 || !txt[0];
    const char *sub = "success", *reason = NULL;
    if (rc != 0) { sub = "error"; reason = "the agent stopped with an error"; }
    else if (!g_hl_turns) { sub = "error"; reason = "the model produced no answer"; }
    else if (!txt[0]) { sub = g_max_turns && g_hl_ncalls >= g_max_turns ? "error_max_turns" : "error"; reason = !strcmp(sub, "error_max_turns") ? "max turns reached before a final answer" : "the model ended without a final answer (context full or the answer was cut)"; }
    if (g_hl_fmt != 0) {
        Buf b = {0}; char t[96];
        buf_str(&b, "{\"type\":\"result\",\"subtype\":\""); buf_str(&b, sub); buf_str(&b, "\"");
        buf_str(&b, ",\"result\":"); lc_json_str(&b, txt);
        buf_str(&b, err ? ",\"is_error\":true" : ",\"is_error\":false");
        snprintf(t, sizeof t, ",\"num_turns\":%d,\"duration_ms\":%.0f", g_hl_turns, dur); buf_str(&b, t);
        buf_str(&b, ",\"session_id\":"); if (g_hl_sid[0]) lc_json_str(&b, g_hl_sid); else buf_str(&b, "null");
        snprintf(t, sizeof t, ",\"usage\":{\"prompt_tokens\":%ld,\"completion_tokens\":%ld}", g_hl_in, g_hl_gen); buf_str(&b, t);
        buf_str(&b, ",\"tool_calls\":["); if (g_hl_calls.p) buf_str(&b, g_hl_calls.p); buf_str(&b, "]}");
        fputs(b.p, g_hl_out); fputc('\n', g_hl_out); fflush(g_hl_out);
        free(b.p);
    } else if (!err) { fputs(txt, g_hl_out); fputc('\n', g_hl_out); fflush(g_hl_out); }
    if (err) fprintf(g_hl_err, "katana: erro: %s\n", reason ? reason : "unknown");
    if (g_hl_sid[0] && g_sess_path[0]) lc_sess_index(g_sess_path);
    return err ? 1 : 0;
}

/* katana mdtest [plain]: passa a entrada padrao pelo renderizador de Markdown do console (teste, sem modelo) */
static int mdtest_cmd(int argc, char **argv) {
    g_con_rich = 1; g_con_color = !(argc > 2 && !strcmp(argv[2], "plain"));
    int c;
    while ((c = getchar()) != EOF) md_feed((unsigned char)c);
    md_flush_end();
    return 0;
}

static int launch_try(int argc, char **argv) {
    LcOpts o; memset(&o, 0, sizeof o);
    if (argc >= 2) {
        const char *a1 = argv[1];
        if (!strcmp(a1, "mdtest")) return mdtest_cmd(argc, argv);
        if (!strcmp(a1, "help") && argc >= 3) return lc_help_topic(argc, argv);
        if (lc_known_cmd(a1)) {
            if (!strcmp(a1, "install") || !strcmp(a1, "uninstall")) return install_cmd(argc, argv, a1[0] == 'u');
            if (!strcmp(a1, "adopt")) return adopt_cmd(argc, argv);
            return -1;
        }
        if (strlen(a1) > 5 && !strcmp(a1 + strlen(a1) - 5, ".gguf") && argc >= 2 && a1[0] != '-' && lc_isfile(a1)) return -1;
        /* uma palavra solta que nao e subcomando conhecido NAO vira mensagem para o agente: um subcomando novo esquecido na
           lista acima (ou um erro de digitacao) abriria o agente com o modelo de verdade e ferramentas ligadas. Mensagem de uma
           palavra: use -p, ou escreva mais de uma palavra */
        if (a1[0] != '-' && !strchr(a1, ' ') && !strchr(a1, '\n')) {
            int word = a1[0] != 0;
            for (const char *w = a1; *w; w++) if (!((*w >= 'a' && *w <= 'z') || (*w >= '0' && *w <= '9') || *w == '-' || *w == '_')) word = 0;
            if (word) return -1;        /* segue para o despacho normal, que conhece o subcomando ou mostra o uso */
        }
    }
    const char *first = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT(var) do { if (i + 1 >= argc) { fprintf(stderr, "katana: %s precisa de um valor\n", a); return 2; } var = argv[++i]; } while (0)
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { lc_usage(stdout); return 0; }
        else if (!strcmp(a, "--model") || !strcmp(a, "-m")) NEXT(o.model);
        else if (!strcmp(a, "-c") || !strcmp(a, "--continue")) o.cont = 1;
        else if (!strcmp(a, "-r") || !strcmp(a, "--resume")) {
            o.resume = 1;
            if (i + 1 < argc && lc_looks_like_id(argv[i + 1])) o.resume_id = argv[++i];
        }
        else if (!strcmp(a, "-p") || !strcmp(a, "--print") || !strcmp(a, "--prompt")) {
            o.headless = 1;
            if (i + 1 < argc && (argv[i + 1][0] != '-' || !argv[i + 1][1])) { o.prompt = argv[++i]; if (!strcmp(o.prompt, "-")) o.prompt = NULL; }
        }
        else if (!strcmp(a, "--output-format")) NEXT(o.outfmt);
        else if (!strcmp(a, "--mcp-config")) NEXT(o.mcp_config);
        else if (!strcmp(a, "--channel")) { const char *v; NEXT(v); const size_t l = strlen(o.channels); snprintf(o.channels + l, sizeof o.channels - l, "%s%s", l ? "," : "", v); }
        else if (!strcmp(a, "--strict-mcp-config")) o.strict = 1;
        else if (!strcmp(a, "--allowed-tools") || !strcmp(a, "--allowedTools")) NEXT(o.allowed);
        else if (!strcmp(a, "--disallowed-tools") || !strcmp(a, "--disallowedTools")) NEXT(o.disallowed);
        else if (!strcmp(a, "--max-turns")) { const char *v; NEXT(v); o.max_turns = atoi(v); if (o.max_turns < 1) { fprintf(stderr, "katana: --max-turns precisa de um numero >= 1\n"); return 2; } }
        else if (!strcmp(a, "--system-prompt-file")) NEXT(o.sysfile);
        else if (!strcmp(a, "--append-system-prompt")) NEXT(o.append);
        else if (!strcmp(a, "--cwd")) NEXT(o.cwd);
        else if (!strcmp(a, "--session-id")) NEXT(o.session_id);
        else if (!strcmp(a, "--no-session")) o.no_session = 1;
        else if (!strcmp(a, "--quiet")) o.quiet = 1;
        else if (a[0] == '-' && a[1]) { fprintf(stderr, "katana: opcao desconhecida: %s\n\n", a); lc_usage(stderr); return 2; }
        else if (!first) first = a;
        else { fprintf(stderr, "katana: argumento a mais: %s (a primeira mensagem vai entre aspas)\n", a); return 2; }
#undef NEXT
    }
    if (o.headless && !o.prompt && first) o.prompt = first, first = NULL;
    if (o.outfmt && strcmp(o.outfmt, "text") && strcmp(o.outfmt, "json") && strcmp(o.outfmt, "stream-json")) { fprintf(stderr, "katana: --output-format: text, json ou stream-json\n"); return 2; }
    if (o.cwd && chdir(o.cwd) != 0) { fprintf(stderr, "katana: nao consegui entrar em %s\n", o.cwd); return 2; }
    char cwd[4200], home[1024];
    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "/");
    lc_home(home, sizeof home);
    const int dry = getenv("KATANA_LAUNCH_DRYRUN") && atoi(getenv("KATANA_LAUNCH_DRYRUN"));

    /* modelo */
    char mpath[4600], why[400], err[2000];
    if (lc_model_resolve(o.model, mpath, sizeof mpath, why, sizeof why, err, sizeof err) != 0) { fprintf(stderr, "katana: %s\n", err); return 2; }
    snprintf(g_launch_why, sizeof g_launch_why, "(%s)", why);

    /* prompt do modo headless */
    Buf pb = {0};
    if (o.headless) {
        if (o.prompt) buf_str(&pb, o.prompt);
        else if (K_ISATTY(0)) { fprintf(stderr, "katana: -p precisa do pedido (katana -p \"pedido\") ou da entrada padrao\n"); return 2; }
        else if (!dry) lc_stdin_all(&pb);
        while (pb.n && (pb.p[pb.n - 1] == '\n' || pb.p[pb.n - 1] == '\r')) pb.p[--pb.n] = 0;
        if (!pb.n && !dry) { fprintf(stderr, "katana: o pedido esta vazio\n"); return 2; }
        if (pb.n >= 16000) { fprintf(stderr, "katana: pedido grande demais (%zu bytes; o limite e 16000)\n", pb.n); return 2; }
    }

    /* sessao */
    char sdir[4400], sess[4700] = "", resume[4700] = "", sid[160] = "";
    lc_sess_dir(cwd, sdir, sizeof sdir);
    char note[400] = "";
    if (!o.no_session) {
        if (o.session_id) {
            for (const char *q = o.session_id; *q; q++) if (!(isalnum((unsigned char)*q) || *q == '-' || *q == '_')) { fprintf(stderr, "katana: --session-id: so letras, digitos, - e _\n"); return 2; }
            snprintf(sid, sizeof sid, "%s", o.session_id);
            snprintf(sess, sizeof sess, "%s/%s.ksess", sdir, sid);
            if (lc_isfile(sess)) snprintf(resume, sizeof resume, "%s", sess);
        } else if (o.cont || o.resume) {
            static LcSess ls[300];
            const int n = lc_sess_here(ls, 300);
            int pick = -1;
            if (o.cont) { if (n) pick = 0; else snprintf(note, sizeof note, "nenhuma sessao anterior nesta pasta; comecando uma nova"); }
            else if (o.resume_id) { for (int i = 0; i < n && pick < 0; i++) if (!strncmp(ls[i].id, o.resume_id, strlen(o.resume_id))) pick = i; if (pick < 0) { fprintf(stderr, "katana: nenhuma sessao desta pasta comeca com %s\n", o.resume_id); return 2; } }
            else if (!n) snprintf(note, sizeof note, "nenhuma sessao anterior nesta pasta; comecando uma nova");
            else {
                for (int i = 0; i < n && i < 30; i++) { char w[40]; lc_fmt_when(ls[i].when, w, sizeof w); printf("%2d  %s  %-17s %3d turn%s  %s\n", i + 1, ls[i].id, w, ls[i].turns, ls[i].turns == 1 ? " " : "s", ls[i].first); }
                if (!K_ISATTY(0) || dry) return 0;
                fprintf(stderr, "numero da sessao (Enter para comecar uma nova): "); fflush(stderr);
                char ln[64]; if (fgets(ln, sizeof ln, stdin) && atoi(ln) >= 1 && atoi(ln) <= n && atoi(ln) <= 30) pick = atoi(ln) - 1;
            }
            if (pick >= 0) { snprintf(resume, sizeof resume, "%s", ls[pick].path); snprintf(sess, sizeof sess, "%s", ls[pick].path); snprintf(sid, sizeof sid, "%s", ls[pick].id); }
        }
        if (!sess[0]) {
            const time_t t = time(NULL); char ts[40]; strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", localtime(&t));
            snprintf(sid, sizeof sid, "%s-%03d", ts, (int)(getpid() % 1000));
            snprintf(sess, sizeof sess, "%s/%s.ksess", sdir, sid);
        }
    }

    if (sess[0] && !dry) mkdir_parents(sess);
    if (dry) {      /* decisao e fim: os testes olham estas linhas */
        printf("mode=%s\nmodel=%s\nwhy=%s\nmodels_dir=%s\ncwd=%s\nsession=%s\nresume=%s\nsession_dir=%s\n", o.headless ? "headless" : "console", mpath, why, models_dir(), cwd, sess, resume, sdir);
        printf("format=%s\nprompt=%s\nfirst=%s\nallowed=%s\ndisallowed=%s\nmax_turns=%d\nstrict=%d\nmcp_config=%s\nnote=%s\n", o.outfmt ? o.outfmt : "text", pb.p ? pb.p : "", first ? first : "",
               o.allowed ? o.allowed : "", o.disallowed ? o.disallowed : "", o.max_turns, o.strict, o.mcp_config ? o.mcp_config : "", note);
        return 0;
    }

    /* ambiente que o resto do motor ja le */
    if (o.allowed) lc_setenv("KATANA_ALLOWED_TOOLS", o.allowed);
    if (o.disallowed) lc_setenv("KATANA_DISALLOWED_TOOLS", o.disallowed);
    if (o.sysfile) { if (!lc_isfile(o.sysfile)) { fprintf(stderr, "katana: --system-prompt-file: nao achei %s\n", o.sysfile); return 2; } char rp[4400]; lc_realpath(o.sysfile, rp, sizeof rp); lc_setenv("KATANA_SYSTEM_FILE", rp); }
    if (o.append) lc_setenv("KATANA_SYSTEM_APPEND", o.append);
    if (o.channels[0]) lc_setenv("KATANA_MCP_CHANNEL", o.channels);
    if (o.max_turns) g_max_turns = o.max_turns;
    if (o.mcp_config || o.strict) {
        char path[4400] = "";
        if (o.mcp_config && o.mcp_config[0] != '{') { if (!lc_isfile(o.mcp_config)) { fprintf(stderr, "katana: --mcp-config: nao achei %s\n", o.mcp_config); return 2; } lc_realpath(o.mcp_config, path, sizeof path); }
        else {
            const char *js = o.mcp_config ? o.mcp_config : "{\"mcpServers\":{}}";
            const char *tp = getenv("TMPDIR"); if (!tp || !*tp) tp = "/tmp";
            snprintf(g_mcp_tmp, sizeof g_mcp_tmp, "%s/katana-mcp-%d.json", tp, (int)getpid());
            if (!lc_write(g_mcp_tmp, js, strlen(js))) { fprintf(stderr, "katana: nao consegui gravar %s\n", g_mcp_tmp); return 1; }
            atexit(lc_cleanup_tmp);
            snprintf(path, sizeof path, "%s", g_mcp_tmp);
        }
        if (o.mcp_config) { char *t = lc_slurp(path, NULL); const char *r = t ? strchr(t, '{') : NULL, *ms = r ? mcp_get(r, "mcpServers") : NULL; if (!ms) { fprintf(stderr, "katana: --mcp-config: %s nao tem \"mcpServers\"\n", o.mcp_config); free(t); return 2; } free(t); }
        lc_setenv("KATANA_MCP_CONFIG", path);
        if (o.strict) lc_setenv("KATANA_MCP_STRICT", "1");
    }
    snprintf(g_hist_path, sizeof g_hist_path, "%.4300s/history", sdir);
    g_hl = o.headless;
    g_hl_fmt = !o.outfmt ? 0 : !strcmp(o.outfmt, "json") ? 1 : !strcmp(o.outfmt, "stream-json") ? 2 : 0;
    if (o.headless) {
        lc_setenv("KATANA_NO_REGROW", "1");
        g_hl_t0 = lc_now();
        snprintf(g_hl_sid, sizeof g_hl_sid, "%s", sess[0] ? sid : "");
        g_hl_prompt = strdup(pb.p ? pb.p : "");
        free(pb.p);
        fflush(stdout); fflush(stderr);
        const int fo = dup(1), fe = dup(2);
        g_hl_out = fdopen(fo, "w"); g_hl_err = fdopen(fe, "w");
        const int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, 1); if (o.quiet) dup2(dn, 2); close(dn); }
        { const int di = open("/dev/null", O_RDONLY); if (di >= 0) { dup2(di, 0); close(di); } }
        if (!o.quiet) fprintf(stderr, "katana: modelo %s %s\n", mpath, g_launch_why);
    } else {
        if (note[0]) fprintf(stderr, "katana: %s\n", note);
        fprintf(stderr, "katana: modelo %s %s\n", mpath, g_launch_why);
    }
    if (sess[0]) snprintf(g_sess_path, sizeof g_sess_path, "%s", sess);
    if (resume[0]) snprintf(g_resume_path, sizeof g_resume_path, "%s", resume);
    if (first && !o.headless) lc_setenv("KATANA_FIRST_MESSAGE", first);
    char *av[4] = { argv[0], "agent", mpath, NULL };
    const int rc = run_main(3, av);
    if (o.headless) return launch_finish(rc);
    if (g_sess_path[0]) lc_sess_index(g_sess_path);
    return rc;
}

/* chamado no inicio de agent(): a primeira mensagem de "katana \"mensagem\"" entra como se tivesse sido digitada */
/* inicializacao do console rico depois de console_init() (cores) */
static void launch_console_init(void) {
    g_con_rich = !g_hl && K_ISATTY(1) && K_ISATTY(2) && !getenv("KATANA_UI") && !getenv("KATANA_PLAIN");
    if (!g_hist_path[0]) { char cw[4200], sd[4400]; if (getcwd(cw, sizeof cw)) { lc_sess_dir(cw, sd, sizeof sd); snprintf(g_hist_path, sizeof g_hist_path, "%.4300s/history", sd); } }
    g_con_color = g_con_rich && col_err && col_out;
    if (g_con_rich) con_term_cols();
#ifndef _WIN32
    if (ed_ready() && !g_hl) ed_hist_load();
#endif
}
