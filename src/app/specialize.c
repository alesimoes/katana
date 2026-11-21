/* ---------- katana specialize: faz uma versao especialista do modelo local, de ponta a ponta ----------
 * O produto e UM modelo especializado: o tipo all (padrao) junta todas as fontes dadas, cada uma com as regras
 * do seu tipo, numa mistura explicita (--mix, em % das janelas de treino) e grava katana.kspec. Os tipos separados
 * ficam para experimentos:
 *   teacher: as sessoes do agente com falas do professor (.ksess com o arquivo .teacher ao lado); so elas contam;
 *   tools:   transcricoes de trabalho (--from) e sessoes; prompt de sistema SEM a secao de ferramentas (o modelo
 *            aprende as ferramentas pelo treino); todas as falas do modelo contam;
 *   code:    trabalho planejado: as mesmas entradas; cada conversa com 4 ou mais chamadas ganha no comeco um plano
 *            (TodoWrite) tirado das chamadas que vieram depois (--plan-first); contam as duas primeiras falas do
 *            modelo (o plano e a primeira acao), as que chamam TodoWrite/Write/Edit e a resposta final;
 *   knowledge: um corpus de perguntas e respostas sobre os projetos (--corpus, formato de mensagens); todas contam.
 * Varias noites se somam: o treino continua da especializacao mais nova do mesmo tipo na pasta de cima (--fresh
 * comeca do base) e cada noite acrescenta uma linha em history.jsonl. Quem treina com o prompt curto grava no
 * .kspec a marca de que o agente deve tirar a secao de ferramentas.
 * Passos: prompt de sistema do agente no formato do modelo (aqui mesmo), katana import de cada entrada (chaves com
 * forma 8-4-4-4-12 sempre mascaradas), plano de memoria (recusa se nao cabe), tempo de um passo, quantos passos
 * cabem em --hours, katana train com adaptadores na atencao, conferencia de que o modelo responde com o treino.
 * Os passos pesados rodam o proprio binario como processo filho, como o katana learn. Nada e aplicado sozinho. */

#define SP_KEYRX "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}=00000000-0000-0000-0000-000000000000"

static FILE *g_sp_rep;
static void sp_env(const char *k, const char *v) { char b[2400]; snprintf(b, sizeof b, "%s=%s", k, v); putenv_dup(b); }
static void sp_say(const char *fmt, ...) {
    char msg[4096]; va_list ap;
    va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    time_t t = time(NULL); struct tm *tm = localtime(&t);
    char ts[16]; strftime(ts, sizeof ts, "%H:%M:%S", tm);
    printf("%s %s\n", ts, msg); fflush(stdout);
    if (g_sp_rep) { fprintf(g_sp_rep, "%s %s\n", ts, msg); fflush(g_sp_rep); }
}

/* roda o binario com argumentos; stdout e stderr vao para outp */
static int sp_run(char *const *argv, const char *outp) {
#ifdef _WIN32
    return lr_run(argv, outp);
#else
    fflush(stdout); fflush(stderr);
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        FILE *f = fopen(outp, "wb"); if (!f) _exit(127);
        dup2(fileno(f), 1); dup2(fileno(f), 2);
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}

/* cria a pasta e as de cima (mkdir -p) */
static int sp_mkdirs(const char *path) {
    char p[2048]; snprintf(p, sizeof p, "%s", path);
    for (char *q = p + 1; *q; q++) if (*q == '/') { *q = 0; lr_mkdir(p); *q = '/'; }
    return lr_mkdir(p);
}

/* primeira linha de path que contem key; copia a linha em out */
static int sp_grep(const char *path, const char *key, char *out, size_t cap, int last) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[8192]; int found = 0;
    while (fgets(line, sizeof line, f)) if (strstr(line, key)) {
        size_t n = strlen(line); while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        snprintf(out, cap, "%s", line); found = 1;
        if (!last) break;
    }
    fclose(f);
    return found;
}

static long sp_mem_avail_mb(void) {
    FILE *f = fopen("/proc/meminfo", "rb");
    if (!f) return -1;
    char line[256]; long kb = -1;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "MemAvailable:", 13)) { kb = atol(line + 13); break; }
    fclose(f);
    return kb < 0 ? -1 : kb / 1024;
}

/* argumentos montados aos poucos */
typedef struct { char *v[96]; int n; } SpArgs;
static void sp_a(SpArgs *a, const char *s) { if (a->n < 94) a->v[a->n++] = (char *)s; a->v[a->n] = NULL; }

/* fontes: tools, code, teacher, knowledge */
enum { SP_TOOLS, SP_CODE, SP_TEACHER, SP_KNOW, SP_NSRC };
static const char *const sp_srcname[SP_NSRC] = { "tools", "code", "teacher", "knowledge" };

typedef struct {
    const char *self, *model, *kind, *out, *sysf, *cut; const char *rep[16]; int nrep;
    long kept, dropped, turns, chars; int ninputs;
    long s_kept[SP_NSRC], s_turns[SP_NSRC], s_chars[SP_NSRC];
    const char *extra[16]; int nextra;           /* opcoes de selecao a mais para a fonte code */
} SpCtx;

/* copia part para data trocando a fonte de cada linha ("source": "import" -> o nome da fonte) */
static void sp_tag_copy(const char *src, const char *dst, const char *tag) {
    FILE *a = fopen(src, "rb"); if (!a) return;
    FILE *b = fopen(dst, "ab"); if (!b) { fclose(a); return; }
    static const char from[] = "{\"source\": \"import\"";
    size_t lc = 1 << 20; char *line = xalloc(lc, 1);
    for (;;) {
        size_t n = 0; int ch;
        while ((ch = fgetc(a)) != EOF && ch != '\n') { if (n + 2 >= lc) { lc *= 2; line = realloc(line, lc); } line[n++] = (char)ch; }
        line[n] = 0;
        if (n) {
            if (!strncmp(line, from, sizeof from - 1)) fprintf(b, "{\"source\": \"%s\"%s\n", tag, line + sizeof from - 1);
            else fprintf(b, "%s\n", line);
        }
        if (ch == EOF) break;
    }
    free(line); fclose(a); fclose(b);
}

/* katana import de uma entrada com as regras da fonte; acrescenta em data.jsonl e soma a tabela */
static void sp_import(SpCtx *x, const char *in, int src) {
    char part[2300], log[2300], all[2300], sysarg[2300], line[512];
    snprintf(part, sizeof part, "%s/part.jsonl", x->out);
    snprintf(log, sizeof log, "%s/part.log", x->out);
    snprintf(all, sizeof all, "%s/data.jsonl", x->out);
    snprintf(sysarg, sizeof sysarg, "file:%s", x->sysf);
    SpArgs a = {0};
    sp_a(&a, x->self); sp_a(&a, "import"); sp_a(&a, x->model); sp_a(&a, in); sp_a(&a, part);
    sp_a(&a, "--system"); sp_a(&a, sysarg); sp_a(&a, "--replace"); sp_a(&a, SP_KEYRX);
    for (int i = 0; i < x->nrep; i++) { sp_a(&a, "--replace"); sp_a(&a, x->rep[i]); }
    if (x->cut) { sp_a(&a, "--cut-matching"); sp_a(&a, x->cut); }
    if (src == SP_TEACHER) sp_a(&a, "--teacher");
    if (src == SP_CODE) {                         /* trabalho planejado */
        sp_a(&a, "--plan-first"); sp_a(&a, "--train-first"); sp_a(&a, "2");
        sp_a(&a, "--train-tools"); sp_a(&a, "TodoWrite,Write,Edit"); sp_a(&a, "--train-final");
        sp_a(&a, "--drop-failed-streak"); sp_a(&a, "4"); sp_a(&a, "--min-calls"); sp_a(&a, "4");
        for (int i = 0; i < x->nextra; i++) sp_a(&a, x->extra[i]);
    }
    const int rc = sp_run(a.v, log);
    char ilog[2300]; snprintf(ilog, sizeof ilog, "%s/import.log", x->out);
    FILE *f = fopen(ilog, "ab"); if (f) { fprintf(f, "== %s (fonte %s)\n", in, sp_srcname[src]); fclose(f); }
    lr_copy(log, ilog, 1);
    long k = 0, d = 0, t = 0, c = 0;
    if (sp_grep(log, "import_stats:", line, sizeof line, 1)) sscanf(strstr(line, "import_stats:"), "import_stats: kept=%ld dropped=%ld turns=%ld chars=%ld", &k, &d, &t, &c);
    x->kept += k; x->dropped += d; x->turns += t; x->chars += c; x->ninputs++;
    x->s_kept[src] += k; x->s_turns[src] += t; x->s_chars[src] += c;
    if (rc == 0) sp_tag_copy(part, all, sp_srcname[src]);
    remove(part); remove(log);
}

/* as sessoes .ksess da pasta (so as com .teacher ao lado, se only_teacher), em ordem de nome */
static int sp_sessions(const char *dir, int only_teacher, char ***out) {
    int n = 0;
#ifndef _WIN32
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        const size_t l = strlen(de->d_name);
        if (l < 7 || strcmp(de->d_name + l - 6, ".ksess")) continue;
        char p[2400]; struct stat st;
        snprintf(p, sizeof p, "%s/%s.teacher", dir, de->d_name);
        if (only_teacher && stat(p, &st)) continue;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        *out = realloc(*out, sizeof(char *) * (size_t)(n + 1)); (*out)[n++] = strdup(p);
    }
    closedir(d);
#else
    char pat[2100]; WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*.ksess", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char p[2400]; snprintf(p, sizeof p, "%s\\%s.teacher", dir, fd.cFileName);
        if (only_teacher && GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) continue;
        snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
        *out = realloc(*out, sizeof(char *) * (size_t)(n + 1)); (*out)[n++] = strdup(p);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#endif
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && strcmp((*out)[j - 1], (*out)[j]) > 0; j--) { char *t = (*out)[j]; (*out)[j] = (*out)[j - 1]; (*out)[j - 1] = t; }
    return n;
}

/* perda media no comeco e no fim do registro dos passos (10 primeiros e 10 ultimos) */
static void sp_losses(const char *path, double *first, double *last) {
    *first = *last = -1;
    FILE *f = fopen(path, "rb");
    if (!f) { sp_say("registro dos passos: nao li %s", path); return; }
    float *v = NULL; int n = 0, cap = 0; char line[1024];
    while (fgets(line, sizeof line, f)) {
        const char *p = strstr(line, "\"loss\": ");
        if (!p) continue;
        if (n == cap) { cap = cap ? 2 * cap : 256; v = realloc(v, sizeof *v * (size_t)cap); }
        v[n++] = (float)atof(p + 8);
    }
    fclose(f);
    if (n) {
        const int k = n < 10 ? n : 10; double a = 0, b = 0;
        for (int i = 0; i < k; i++) { a += v[i]; b += v[n - 1 - i]; }
        *first = a / k; *last = b / k;
        sp_say("perda nos passos: comeco %.3f (media dos %d primeiros), fim %.3f (media dos %d ultimos), %d passos registrados", *first, k, *last, k, n);
    }
    free(v);
}

/* a especializacao mais nova do mesmo tipo debaixo de root (pastas tipo-data com o arquivo dentro), fora a pasta atual */
static int sp_newest(const char *root, const char *kind, const char *file, const char *skip, char *out, size_t cap) {
    int found = 0;
#ifndef _WIN32
    DIR *d = opendir(root);
    if (!d) return 0;
    struct dirent *de; time_t best = 0;
    const size_t kl = strlen(kind);
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, kind, kl) || de->d_name[kl] != '-') continue;
        char p[2400], dir[2400]; struct stat st;
        snprintf(dir, sizeof dir, "%s/%s", root, de->d_name);
        if (!strcmp(dir, skip)) continue;
        snprintf(p, sizeof p, "%s/%s", dir, file);
        if (stat(p, &st) || st.st_size == 0) continue;
        if (!found || st.st_mtime > best) { best = st.st_mtime; snprintf(out, cap, "%s", p); found = 1; }
    }
    closedir(d);
#endif
    return found;
}

/* katana specialize <modelo.gguf> [all|tools|code|teacher|knowledge] [opcoes] */
static int specialize_main(const char *argv0, int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "uso: katana specialize <modelo.gguf> [all|tools|code|teacher|knowledge] [--from PASTA] [--sessions PASTA] [--corpus ARQ]\n"
                        "         [--mix tools=30,code=30,teacher=20,knowledge=20] [--out PASTA] [--fresh] [--hours H] [--steps N] [--rank R]\n"
                        "         [--seq T] [--lr X] [--margin-mb MB] [--replace REGEX=TEXTO]... [--cut-matching REGEX] [--verified]\n"
                        "         [--plan-updates] [--dry-run] [--no-check]\n");
        return 1;
    }
    SpCtx x; memset(&x, 0, sizeof x);
    x.model = argv[0]; x.kind = "all";
    int i0 = 1;
    if (argc > 1 && argv[1][0] != '-') { x.kind = argv[1]; i0 = 2; }
    int kind = -1;
    for (int s = 0; s < SP_NSRC; s++) if (!strcmp(x.kind, sp_srcname[s])) kind = s;
    const int all = !strcmp(x.kind, "all");
    if (kind < 0 && !all) { fprintf(stderr, "specialize: tipo %s desconhecido (all, tools, code, teacher ou knowledge)\n", x.kind); return 1; }
    const char *from = NULL, *sessions = NULL, *corpus = NULL, *outdir = NULL, *rank = "8", *seq = "512", *lr = "2e-4";
    const char *mix = "tools=30,code=30,teacher=20,knowledge=20";
    double hours = 5; int steps = 0, dry = 0, check = 1, fresh = 0; long margin = 6000;
    for (int i = i0; i < argc; i++) {
        const char *o = argv[i]; const int more = i + 1 < argc;
        if (!strcmp(o, "--from") && more) from = argv[++i];
        else if (!strcmp(o, "--sessions") && more) sessions = argv[++i];
        else if (!strcmp(o, "--corpus") && more) corpus = argv[++i];
        else if (!strcmp(o, "--mix") && more) mix = argv[++i];
        else if (!strcmp(o, "--out") && more) outdir = argv[++i];
        else if (!strcmp(o, "--hours") && more) hours = atof(argv[++i]);
        else if (!strcmp(o, "--steps") && more) steps = atoi(argv[++i]);
        else if (!strcmp(o, "--rank") && more) rank = argv[++i];
        else if (!strcmp(o, "--seq") && more) seq = argv[++i];
        else if (!strcmp(o, "--lr") && more) lr = argv[++i];
        else if (!strcmp(o, "--margin-mb") && more) margin = atol(argv[++i]);
        else if (!strcmp(o, "--replace") && more) { if (x.nrep < 16) x.rep[x.nrep++] = argv[++i]; else ++i; }
        else if (!strcmp(o, "--cut-matching") && more) x.cut = argv[++i];
        else if (!strcmp(o, "--dry-run")) dry = 1;
        else if (!strcmp(o, "--no-check")) check = 0;
        else if (!strcmp(o, "--fresh")) fresh = 1;
        else if ((!strcmp(o, "--verified") || !strcmp(o, "--plan-updates")) && x.nextra < 15) x.extra[x.nextra++] = o;
        else { fprintf(stderr, "specialize: opcao desconhecida %s\n", o); return 1; }
    }
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home) home = getenv("USERPROFILE");
#endif
    if (!home) home = ".";
    char sessdef[2048], outbuf[2048], root[2048], self[2048];
    snprintf(sessdef, sizeof sessdef, "%s/.katana/desktop/sessoes", home);
    if (!sessions && kind == SP_TEACHER) sessions = sessdef;
    if (!outdir) {
        time_t t = time(NULL); char ts[32]; strftime(ts, sizeof ts, "%Y%m%d-%H%M", localtime(&t));
        snprintf(outbuf, sizeof outbuf, "%s/.katana/training/%s-%s", home, x.kind, ts);
        outdir = outbuf;
    }
    snprintf(root, sizeof root, "%s", outdir);
    { char *sl = strrchr(root, '/'); if (sl && sl != root) *sl = 0; else snprintf(root, sizeof root, "."); }
    if (!sp_mkdirs(outdir)) { fprintf(stderr, "specialize: nao criei %s\n", outdir); return 1; }
    x.out = outdir;
    lr_self(self, sizeof self, argv0); x.self = self;
    char path[2300];
    snprintf(path, sizeof path, "%s/report.txt", outdir);
    g_sp_rep = fopen(path, "ab");
#ifndef _WIN32
    if (nice(10) == -1) { }                     /* os filhos herdam: o treino nao disputa a maquina com quem a usa */
#endif
    sp_say("especializacao %s do modelo %s", x.kind, x.model);
    sp_say("saida: %s", outdir);

    /* 1. prompt de sistema do agente, no formato do modelo. Sem a secao de ferramentas (o modelo as aprende pelo
       treino) em tools, knowledge e all; teacher e code ficam com o prompt completo */
    const int short_sys = all || kind == SP_TOOLS || kind == SP_KNOW;
    Model m; memset(&m, 0, sizeof m);
    g_vocab_only = 1;
    load_model(&m, x.model);
    g_vocab_only = 0;
    const Config *c = &m.cfg;
    const char *fmt = c->chat_fmt == 8 ? "gemma4" : c->chat_fmt == 10 && c->glm_args ? "glm" : c->tool_xml ? "qwen" : "json";
    sp_env("KATANA_TOOL_DECL", short_sys ? "0" : "1");
    g_web = 1;
    static char sys[32000];
    ds_agent_sys(fmt, sys, sizeof sys);
    char sysf[2300]; snprintf(sysf, sizeof sysf, "%s/sys.txt", outdir); x.sysf = sysf;
    {   FILE *f = fopen(sysf, "wb"); if (!f) { sp_say("nao gravei %s", sysf); return 1; } fputs(sys, f); fclose(f); }
    sp_say("prompt de sistema: formato %s, %zu caracteres%s", fmt, strlen(sys), short_sys ? " (sem a secao de ferramentas)" : "");

    /* 2. dados: cada fonte com as regras do seu tipo */
    snprintf(path, sizeof path, "%s/data.jsonl", outdir);
    { FILE *f = fopen(path, "wb"); if (f) fclose(f); }
    snprintf(path, sizeof path, "%s/import.log", outdir);
    { FILE *f = fopen(path, "wb"); if (f) fclose(f); }
    if (from) {
        if (all || kind == SP_TOOLS) sp_import(&x, from, SP_TOOLS);
        if (all || kind == SP_CODE) sp_import(&x, from, SP_CODE);
    }
    if (sessions) {
        const int st = all || kind == SP_TEACHER ? SP_TEACHER : kind == SP_CODE ? SP_CODE : SP_TOOLS;
        char **ls = NULL; const int n = kind == SP_KNOW ? 0 : sp_sessions(sessions, st == SP_TEACHER, &ls);
        for (int i = 0; i < n; i++) { sp_import(&x, ls[i], st); free(ls[i]); }
        free(ls);
        sp_say("sessoes em %s: %d%s", sessions, n, st == SP_TEACHER ? " com falas do professor" : "");
    }
    if (corpus && (all || kind == SP_KNOW)) sp_import(&x, corpus, SP_KNOW);
    if (!x.ninputs) { sp_say("nenhuma entrada para o tipo %s: use --from PASTA, --sessions PASTA e/ou --corpus ARQ", x.kind); return 3; }
    for (int s = 0; s < SP_NSRC; s++) if (x.s_kept[s])
        sp_say("fonte %-9s: %ld conversas, %ld falas que contam, %ld caracteres a aprender", sp_srcname[s], x.s_kept[s], x.s_turns[s], x.s_chars[s]);
    sp_say("dados: %ld conversas gravadas, %ld descartadas, %ld falas do modelo que contam, %ld caracteres a aprender "
           "(estimativa: cerca de %.0f janelas de 512 tokens); detalhes por motivo e por ferramenta em import.log",
           x.kept, x.dropped, x.turns, x.chars, x.chars / 3.3 / 512.0);
    if (!x.turns) { sp_say("nenhuma fala a aprender: nada a treinar"); return 3; }

    /* 3. plano de memoria */
    sp_env("KATANA_TRAIN_GPU", "0");            /* produtos na CPU: a placa pode estar com o desktop */
    char data[2300], plan[2300], line[8192], resume[2300] = "", specname[64];
    snprintf(specname, sizeof specname, "%s.kspec", all ? "katana" : x.kind);
    if (!fresh && sp_newest(root, x.kind, specname, outdir, resume, sizeof resume)) sp_say("continua o treino de %s (--fresh comeca do modelo base)", resume);
    snprintf(data, sizeof data, "%s/data.jsonl", outdir);
    snprintf(plan, sizeof plan, "%s/plan.log", outdir);
    SpArgs base = {0};
    sp_a(&base, self); sp_a(&base, "train"); sp_a(&base, x.model); sp_a(&base, data);
    sp_a(&base, "--train"); sp_a(&base, "blk.*.attn_*"); sp_a(&base, "--rank"); sp_a(&base, rank); sp_a(&base, "--seq"); sp_a(&base, seq);
    sp_a(&base, "--lr"); sp_a(&base, lr); sp_a(&base, "--final"); sp_a(&base, "0"); sp_a(&base, "--val"); sp_a(&base, "0");
    if (short_sys) sp_a(&base, "--no-tool-decl");
    if (all) { sp_a(&base, "--mix"); sp_a(&base, mix); }
    if (resume[0]) { sp_a(&base, "--resume"); sp_a(&base, resume); }
    SpArgs a = base; sp_a(&a, "--plan"); sp_a(&a, "1");
    time_t t0 = time(NULL);
    int rc = sp_run(a.v, plan);
    const long load_s = (long)(time(NULL) - t0);
    double act = -1, st = -1;
    if (sp_grep(plan, "memoria:", line, sizeof line, 1)) {
        const char *p = strstr(line, " tokens "); if (p) act = atof(p + 8);
        p = strstr(line, "estado do treino "); if (p) st = atof(p + 17);
    }
    const long avail = sp_mem_avail_mb();
    if (rc != 0 || act < 0) { sp_say("o plano de treino falhou (codigo %d); veja %s", rc, plan); return 4; }
    if (sp_grep(plan, "mistura pedida:", line, sizeof line, 1)) sp_say("%s", line);
    sp_say("memoria: ativacoes %.0f MB + estado do treino %.0f MB + margem %ld MB; disponivel agora %ld MB", act, st < 0 ? 0 : st, margin, avail);
    if (avail >= 0 && act + (st < 0 ? 0 : st) + margin > avail) { sp_say("memoria insuficiente: NAO treinei"); return 4; }
    if (dry) { sp_say("--dry-run: parei depois dos dados e do plano"); return 0; }

    /* 4. tempo de um passo e quantos cabem */
    if (steps <= 0) {
        char ml[2300], mk[2300];
        snprintf(ml, sizeof ml, "%s/measure.log", outdir); snprintf(mk, sizeof mk, "%s/measure.kspec", outdir);
        a = base; sp_a(&a, "--steps"); sp_a(&a, "2"); sp_a(&a, "--out"); sp_a(&a, mk);
        rc = sp_run(a.v, ml);
        if (rc != 0) { sp_say("o treino de 2 passos falhou (codigo %d); veja %s", rc, ml); return 5; }
        double step_s = 0;
        if (sp_grep(ml, "s por passo", line, sizeof line, 1)) { const char *p = strstr(line, "("); if (p) step_s = atof(p + 1); }
        if (step_s <= 0) step_s = 60;
        step_s += 1;
        remove(mk);
        long n = (long)(hours * 3600 / step_s); if (n > 400) n = 400; if (n < 4) n = 4;
        steps = (int)n;
        sp_say("medido: carga %ld s, cerca de %.0f s por passo (janela de %s tokens, produtos na CPU); %d passos cabem em %.1f h", load_s, step_s, seq, steps, hours);
    } else sp_say("passos: %d (dados por --steps, sem medir)", steps);

    /* 5. treino */
    char nsteps[32], logf[2300], spec[2300], trl[2300];
    snprintf(nsteps, sizeof nsteps, "%d", steps);
    snprintf(logf, sizeof logf, "%s/steps.jsonl", outdir);
    snprintf(spec, sizeof spec, "%s/%s", outdir, specname);
    snprintf(trl, sizeof trl, "%s/train.log", outdir);
    remove(logf);
    a = base; sp_a(&a, "--steps"); sp_a(&a, nsteps); sp_a(&a, "--log"); sp_a(&a, logf); sp_a(&a, "--out"); sp_a(&a, spec);
    t0 = time(NULL);
    rc = sp_run(a.v, trl);
    const double took_h = (double)(time(NULL) - t0) / 3600.0;
    sp_say("treino terminou com codigo %d em %.0f min", rc, took_h * 60);
    if (sp_grep(trl, "mistura realizada:", line, sizeof line, 1)) sp_say("%s", line);
    if (sp_grep(trl, "retomado de", line, sizeof line, 1)) sp_say("%s", line);
    if (sp_grep(trl, "resultado:", line, sizeof line, 1)) sp_say("%s", line);
    double lf0, lf1;
    sp_losses(logf, &lf0, &lf1);
    struct stat sst;
    if (rc != 0 || stat(spec, &sst) || sst.st_size == 0) { sp_say("nenhuma especializacao gravada"); return 6; }
    sp_say("especializacao: %s (%ld KB)", spec, (long)sst.st_size / 1024);
    {   /* historico das noites, na pasta de cima */
        char hp[2300]; snprintf(hp, sizeof hp, "%s/history.jsonl", root);
        FILE *h = fopen(hp, "ab");
        if (h) {
            char ts[32]; time_t t = time(NULL); strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", localtime(&t));
            Buf b = {0};
            buf_str(&b, "{\"date\": \""); buf_str(&b, ts); buf_str(&b, "\", \"kind\": \""); buf_str(&b, x.kind); buf_str(&b, "\", \"spec\": \"");
            js_escape(&b, spec, strlen(spec)); buf_str(&b, "\", \"resumed_from\": ");
            if (resume[0]) { buf_str(&b, "\""); js_escape(&b, resume, strlen(resume)); buf_str(&b, "\""); } else buf_str(&b, "null");
            char tmp[400];
            snprintf(tmp, sizeof tmp, ", \"steps\": %d, \"hours\": %.3f, \"loss_first10\": %.4f, \"loss_last10\": %.4f, \"sources\": {", steps, took_h, lf0, lf1);
            buf_str(&b, tmp);
            int first = 1;
            for (int s = 0; s < SP_NSRC; s++) if (x.s_kept[s]) {
                snprintf(tmp, sizeof tmp, "%s\"%s\": {\"conversations\": %ld, \"turns\": %ld, \"chars\": %ld}", first ? "" : ", ", sp_srcname[s], x.s_kept[s], x.s_turns[s], x.s_chars[s]);
                buf_str(&b, tmp); first = 0;
            }
            buf_str(&b, "}}\n");
            fputs(b.p, h); fclose(h); free(b.p);
            sp_say("historico: %s", hp);
        }
    }

    /* 6. o modelo com a especializacao carrega e responde? (CPU, sem sorteio, base e especializado) */
    if (check) {
        static const char *q = "Em uma frase: o que voce faz primeiro quando um script de instalacao falha?";
        char ab[2300], at[2300];
        snprintf(ab, sizeof ab, "%s/answer_base.txt", outdir); snprintf(at, sizeof at, "%s/answer_trained.txt", outdir);
        sp_env("KATANA_CPU", "1");
        SpArgs ch = {0}; sp_a(&ch, self); sp_a(&ch, "chat"); sp_a(&ch, x.model); sp_a(&ch, q); sp_a(&ch, "60"); sp_a(&ch, "0");
        sp_env("KATANA_SPEC", "");
        sp_run(ch.v, ab);
        sp_env("KATANA_SPEC", spec);
        const int rc2 = sp_run(ch.v, at);
        sp_env("KATANA_SPEC", "");
        sp_say("conferencia: o modelo com o treino %s (respostas em answer_base.txt e answer_trained.txt)", rc2 == 0 ? "carregou e respondeu" : "FALHOU ao responder");
    }
    sp_say("fim. Nada foi aplicado sozinho. Para testar: KATANA_SPEC=%s katana agent %s", spec, x.model);
    sp_say("Para o desktop usar sempre: mkdir -p ~/.katana/specs && cp %s ~/.katana/specs/  (ele carrega todos os .kspec dessa pasta treinados sobre o modelo escolhido)", spec);
    sp_say("Ou um GGUF com o treino dentro: katana merge %s %s <saida.gguf>", x.model, spec);
    if (g_sp_rep) fclose(g_sp_rep);
    return 0;
}
