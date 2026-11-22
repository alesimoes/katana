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
 * Uma noite so: `katana specialize <modelo>` sem mais nada faz a rotina inteira com os padroes (todos trocaveis):
 * transcricoes de transcripts=... no katana.cfg (ou --from), sessoes do proprio agente (~/.katana/desktop/sessoes),
 * base de conhecimento atualizada de forma incremental (~/.katana/knowledge) e corpus + holdout regerados dela, dados,
 * plano de memoria, treino por --hours (padrao 5) continuando da noite anterior, historico e relatorio. As trocas
 * (replace=REGEX=TEXTO, cut-matching=REGEX) tambem vem do katana.cfg. Tudo o que e grande fica ao lado da saida
 * (training-dir=..., padrao ~/.katana/training), nunca em /tmp.
 * Varias noites se somam: o treino continua da especializacao mais nova do mesmo tipo na pasta de cima (--fresh
 * comeca do base) e cada noite acrescenta uma linha em history.jsonl. Quem treina com o prompt curto grava no
 * .kspec a marca de que o agente deve tirar a secao de ferramentas.
 * Passos: prompt de sistema do agente no formato do modelo (aqui mesmo), katana import de cada entrada (chaves com
 * forma 8-4-4-4-12 sempre mascaradas), conferencia do arquivo de dados (cada linha JSON inteira, contagem igual a das
 * fontes), plano de memoria (recusa se nao cabe), tempo de um passo, quantos passos cabem em --hours, katana train com
 * adaptadores na atencao, conferencia de que o modelo responde com o treino, relatorio para uma pessoa.
 * Toda gravacao e conferida (io_write/io_close): disco cheio ou outro erro para tudo com o arquivo e a causa.
 * Os passos pesados rodam o proprio binario como processo filho, como o katana learn. Nada e aplicado sozinho. */

#define SP_KEYRX "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}=00000000-0000-0000-0000-000000000000"

static FILE *g_sp_rep; static char g_sp_reppath[2300];
static void sp_env(const char *k, const char *v) { char b[2400]; snprintf(b, sizeof b, "%s=%s", k, v); putenv_dup(b); }
static void sp_say(const char *fmt, ...) {
    char msg[4096]; va_list ap;
    va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    time_t t = time(NULL); struct tm *tm = localtime(&t);
    char ts[16]; strftime(ts, sizeof ts, "%H:%M:%S", tm);
    printf("%s %s\n", ts, msg); fflush(stdout);
    if (g_sp_rep) { io_printf(g_sp_rep, g_sp_reppath, "%s %s\n", ts, msg); fflush(g_sp_rep); }
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
static int sp_isdir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int sp_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* primeira (ou ultima) linha de path que contem key; copia a linha em out */
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

/* uma linha (sem o \n) de f em *buf; devolve o tamanho, -1 no fim, -2 em erro de leitura; nl diz se a linha tinha \n */
static long sp_getline(FILE *f, char **buf, size_t *cap, int *nl) {
    size_t n = 0; *nl = 0;
    if (!*buf) { *cap = 1 << 16; *buf = xalloc(*cap, 1); }
    for (;;) {
        if (*cap - n < 4096) { *cap *= 2; *buf = realloc(*buf, *cap); }
        if (!fgets(*buf + n, (int)(*cap - n), f)) break;
        n += strlen(*buf + n);
        if (n && (*buf)[n - 1] == '\n') { *nl = 1; (*buf)[--n] = 0; return (long)n; }
    }
    if (ferror(f)) return -2;
    return n ? (long)n : -1;
}

/* acrescenta o conteudo de src em dst (gravacao conferida); head, se dado, vem antes */
static int sp_append_file(const char *src, const char *dst, const char *head) {
    FILE *a = fopen(src, "rb");
    if (!a) return 1;                                    /* nada a acrescentar */
    FILE *b = fopen(dst, "ab");
    if (!b) { fclose(a); return io_fail(dst, "nao consegui abrir", errno); }
    int ok = !head || io_puts(b, head, dst);
    char buf[65536]; size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, a)) > 0) ok = io_write(b, buf, n, dst);
    fclose(a);
    return io_close(b, dst) && ok;
}
static int sp_truncate(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return io_fail(path, "nao consegui criar", errno);
    return io_close(f, path);
}

/* argumentos montados aos poucos */
typedef struct { char *v[128]; int n; } SpArgs;
static void sp_a(SpArgs *a, const char *s) { if (a->n < 126) a->v[a->n++] = (char *)s; a->v[a->n] = NULL; }

/* fontes: tools, code, teacher, knowledge */
enum { SP_TOOLS, SP_CODE, SP_TEACHER, SP_KNOW, SP_CONV, SP_NSRC };
static const char *const sp_srcname[SP_NSRC] = { "tools", "code", "teacher", "knowledge", "conventions" };

#define SP_MAXREP 28
typedef struct {
    const char *self, *model, *kind, *out, *sysf, *cut; const char *rep[SP_MAXREP]; int nrep;
    long kept, dropped, turns, chars; int ninputs;
    long s_kept[SP_NSRC], s_turns[SP_NSRC], s_chars[SP_NSRC];
    const char *extra[16]; int nextra;           /* opcoes de selecao a mais para a fonte code */
} SpCtx;

/* ---------- configuracao: katana.cfg (linhas chave=valor; # comenta) ----------
 * Lido de ~/.katana/katana.cfg e depois de ./katana.cfg (o da pasta atual vale mais). Chaves de uma noite:
 *   transcripts=PASTA   training-dir=PASTA   replace=REGEX=TEXTO (repetivel)   cut-matching=REGEX
 * Os mesmos arquivos guardam o ajuste da placa (gpu=, seg=, rows=, rowsb=); as outras linhas sao ignoradas la. */
typedef struct { char *k[256], *v[256]; int n; } SpCfg;
static void sp_cfg_file(SpCfg *c, const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return;
    char *line = NULL; size_t cap = 0; int nl;
    while (sp_getline(f, &line, &cap, &nl) >= 0) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        size_t l = strlen(p); while (l && (p[l - 1] == '\r' || p[l - 1] == '\n')) p[--l] = 0;
        if (!*p || *p == '#') continue;
        char *eq = strchr(p, '='); if (!eq || c->n >= 256) continue;
        *eq = 0; c->k[c->n] = strdup(p); c->v[c->n] = strdup(eq + 1); c->n++;
    }
    free(line); fclose(f);
}
static void sp_cfg_load(SpCfg *c, const char *home) {
    char p[2400];
    memset(c, 0, sizeof *c);
    snprintf(p, sizeof p, "%s/.katana/katana.cfg", home); sp_cfg_file(c, p);
    sp_cfg_file(c, "katana.cfg");
}
static const char *sp_cfg_get(const SpCfg *c, const char *key) {         /* o ultimo valor da chave */
    const char *r = NULL;
    for (int i = 0; i < c->n; i++) if (!strcmp(c->k[i], key)) r = c->v[i];
    return r;
}
/* caminho de um valor: ~/ vira a pasta do usuario */
static char *sp_cfg_path(const char *v, const char *home) {
    char b[2400];
    if (v[0] == '~' && (v[1] == '/' || !v[1])) snprintf(b, sizeof b, "%s%s", home, v + 1); else snprintf(b, sizeof b, "%s", v);
    return strdup(b);
}

/* copia part para data trocando a fonte de cada linha ("source": "import" -> o nome da fonte); devolve as linhas copiadas, -1 se falhou */
static long sp_tag_copy(const char *src, const char *dst, const char *tag) {
    FILE *a = fopen(src, "rb");
    if (!a) { io_fail(src, "nao consegui abrir", errno); return -1; }
    FILE *b = fopen(dst, "ab");
    if (!b) { io_fail(dst, "nao consegui abrir", errno); fclose(a); return -1; }
    static const char from[] = "{\"source\": \"import\"";
    char *line = NULL; size_t lc = 0; int nl; long n, lines = 0, ok = 1;
    while (ok && (n = sp_getline(a, &line, &lc, &nl)) != -1) {
        if (n == -2) { io_fail(src, "erro de leitura em", errno); ok = 0; break; }
        if (!n) continue;
        if (!strncmp(line, from, sizeof from - 1)) ok = io_printf(b, dst, "{\"source\": \"%s\"", tag) && io_write(b, line + sizeof from - 1, (size_t)n - (sizeof from - 1), dst);
        else ok = io_write(b, line, (size_t)n, dst);
        ok = ok && io_write(b, "\n", 1, dst);
        lines++;
    }
    free(line); fclose(a);
    ok = io_close(b, dst) && ok;
    return ok ? lines : -1;
}

/* a primeira linha de erro de um log de processo filho (para a mensagem) */
static void sp_why(const char *log, char *out, size_t cap) {
    out[0] = 0;
    if (!sp_grep(log, "ERRO", out, cap, 0) && !sp_grep(log, "nao consegui", out, cap, 0) && !sp_grep(log, "falhou", out, cap, 0)) sp_grep(log, ":", out, cap, 1);
}

/* katana import de uma entrada com as regras da fonte; acrescenta em data.jsonl e soma a tabela. 0 = parou com erro */
static int sp_import(SpCtx *x, const char *in, int src) {
    char part[2300], log[2300], all[2300], sysarg[2300], line[512], why[1024];
    snprintf(part, sizeof part, "%s/part.jsonl", x->out);
    snprintf(log, sizeof log, "%s/part.log", x->out);
    snprintf(all, sizeof all, "%s/data.jsonl", x->out);
    /* o prompt de sistema por fonte: as conversas de conventions trazem o seu (arquivo de instrucoes do projeto, skills, cartoes MCP
       do cenario) e ele fica (--system keep); nas outras o do agente de hoje (--system file:), que o import poe no lugar do da conversa */
    if (src == SP_CONV) snprintf(sysarg, sizeof sysarg, "keep"); else snprintf(sysarg, sizeof sysarg, "file:%s", x->sysf);
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
    char ilog[2300], head[2400]; snprintf(ilog, sizeof ilog, "%s/import.log", x->out);
    snprintf(head, sizeof head, "== %s (fonte %s)\n", in, sp_srcname[src]);
    if (!sp_append_file(log, ilog, head)) { sp_say("PAROU: nao consegui gravar %s", ilog); return 0; }
    long k = 0, d = 0, t = 0, c = 0;
    const int has_stats = sp_grep(log, "import_stats:", line, sizeof line, 1);
    if (has_stats) sscanf(strstr(line, "import_stats:"), "import_stats: kept=%ld dropped=%ld turns=%ld chars=%ld", &k, &d, &t, &c);
    /* codigo 1 com a tabela: nenhuma conversa serviu (normal para uma sessao); qualquer outro caso e falha de verdade */
    if (rc != 0 && !(rc == 1 && has_stats)) {
        sp_why(log, why, sizeof why);
        sp_say("PAROU: katana import de %s (fonte %s) falhou com codigo %d%s%s; veja %s", in, sp_srcname[src], rc, why[0] ? ": " : "", why, log);
        return 0;
    }
    if (rc == 0) {
        const long got = sp_tag_copy(part, all, sp_srcname[src]);
        if (got < 0) { sp_say("PAROU: nao consegui acrescentar os dados de %s em %s (veja a mensagem acima)", in, all); return 0; }
        if (got != k) { sp_say("PAROU: o import de %s disse %ld conversas gravadas e o arquivo tem %ld linhas", in, k, got); return 0; }
    }
    x->kept += k; x->dropped += d; x->turns += t; x->chars += c; x->ninputs++;
    x->s_kept[src] += k; x->s_turns[src] += t; x->s_chars[src] += c;
    remove(part); remove(log);
    return 1;
}

/* confere data.jsonl antes de treinar: toda linha e um objeto JSON inteiro com text e train_spans, a fonte e conhecida,
   o numero de linhas (total e por fonte) e igual ao que as fontes disseram (x NULL: so a forma das linhas). 1 = ok */
static int sp_verify(const char *path, const SpCtx *x) {
    FILE *f = fopen(path, "rb");
    if (!f) { sp_say("PAROU: nao abri %s para conferir: %s", path, strerror(errno)); return 0; }
    char *line = NULL; size_t cap = 0; int nl = 1; long n, lines = 0, bysrc[SP_NSRC] = {0}; int ok = 1;
    while (ok && (n = sp_getline(f, &line, &cap, &nl)) != -1) {
        lines++;
        if (n == -2) { sp_say("PAROU: erro de leitura em %s na linha %ld: %s", path, lines, strerror(errno)); ok = 0; break; }
        const char *why = NULL;
        if (!nl) why = "sem quebra de linha no fim (arquivo cortado)";
        else if (n == 0) why = "linha vazia";
        else if (line[0] != '{' || !io_json_ok(line, (size_t)n)) why = "nao e um objeto JSON inteiro (cortada?)";
        else {
            int has_text = 0, has_spans = 0, src = -1; char k[32]; const char *v, *e;
            for (const char *p = line + 1; (e = gs_member(p, k, sizeof k, &v)); p = e) {
                if (!strcmp(k, "text") && *v == '"') has_text = 1;
                else if (!strcmp(k, "train_spans") && *v == '[') has_spans = 1;
                else if (!strcmp(k, "source") && *v == '"') for (int s = 0; s < SP_NSRC; s++) { const size_t l = strlen(sp_srcname[s]); if (!strncmp(v + 1, sp_srcname[s], l) && v[l + 1] == '"') src = s; }
            }
            if (!has_text) why = "falta \"text\""; else if (!has_spans) why = "falta \"train_spans\""; else if (src < 0) why = "fonte desconhecida";
            else bysrc[src]++;
        }
        if (why) { sp_say("PAROU: %s linha %ld: %s; o arquivo de dados esta corrompido e NAO sera usado (disco cheio?)", path, lines, why); ok = 0; }
    }
    free(line); fclose(f);
    if (!ok) return 0;
    if (!x) {
        sp_say("%s: %ld linhas, todas objetos JSON inteiros com text e train_spans (tools %ld, code %ld, teacher %ld, knowledge %ld)", path, lines, bysrc[SP_TOOLS], bysrc[SP_CODE], bysrc[SP_TEACHER], bysrc[SP_KNOW]);
        return 1;
    }
    if (lines != x->kept) { sp_say("PAROU: %s tem %ld linhas e as fontes disseram %ld conversas gravadas: o arquivo esta incompleto", path, lines, x->kept); return 0; }
    for (int s = 0; s < SP_NSRC; s++) if (bysrc[s] != x->s_kept[s]) {
        sp_say("PAROU: fonte %s: %ld linhas no arquivo e %ld conversas gravadas pelas fontes", sp_srcname[s], bysrc[s], x->s_kept[s]); return 0;
    }
    sp_say("dados conferidos: %ld linhas, todas objetos JSON inteiros com text e train_spans; contagem por fonte igual a das fontes", lines);
    return 1;
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

/* ---------- esperar a maquina ficar livre (--wait-idle) ----------
 * Como o script antigo decidia: com um `katana agent` rodando, a maquina esta ocupada se uma chamada ao professor esta em
 * andamento ou se a sessao mais nova do agente foi gravada ha menos de 10 minutos; precisa estar livre em 3 consultas
 * seguidas (de 30 s). Sem nenhum agente rodando, segue na hora. Passou do limite: desiste sem treinar (codigo 2).
 * KATANA_IDLE_SECS e KATANA_IDLE_POLL (segundos) mudam os 600 s e os 30 s, para teste. */
#ifndef _WIN32
static int sp_proc_has(const char *needle) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *de; int found = 0; const pid_t me = getpid();
    while (!found && (de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0]) || atoi(de->d_name) == (int)me) continue;
        char p[300], buf[8192]; snprintf(p, sizeof p, "/proc/%s/cmdline", de->d_name);
        FILE *f = fopen(p, "rb"); if (!f) continue;
        const size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f);
        for (size_t i = 0; i < n; i++) if (!buf[i]) buf[i] = ' ';
        buf[n] = 0;
        if (strstr(buf, needle)) found = 1;
    }
    closedir(d);
    return found;
}
static time_t sp_newest_session(const char *dir) {
    DIR *d = opendir(dir); time_t best = 0;
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        const size_t l = strlen(de->d_name); char p[2400]; struct stat st;
        if (l < 7 || strcmp(de->d_name + l - 6, ".ksess")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (!stat(p, &st) && st.st_mtime > best) best = st.st_mtime;
    }
    closedir(d);
    return best;
}
static int sp_wait_idle(double hours, const char *sessions) {
    const long idle = getenv("KATANA_IDLE_SECS") ? atol(getenv("KATANA_IDLE_SECS")) : 600;
    const long poll = getenv("KATANA_IDLE_POLL") ? atol(getenv("KATANA_IDLE_POLL")) : 30;
    const time_t t0 = time(NULL); int quiet = 0; long polls = 0;
    sp_say("esperando o agente ficar parado (sem gravar sessao ha %ld s; limite de espera %.2f h)", idle, hours);
    for (;;) {
        const time_t now = time(NULL), last = sp_newest_session(sessions);
        const long age = last ? (long)(now - last) : 999999;
        if (!sp_proc_has("katana agent ")) { sp_say("nenhum agente rodando: sigo"); return 0; }
        if (sp_proc_has("katana_teacher_") || age < idle) quiet = 0; else quiet++;
        if (quiet >= 3) { sp_say("agente parado ha mais de %ld s (ultima gravacao de sessao ha %ld s): sigo", idle, age); return 0; }
        if ((double)(now - t0) >= hours * 3600.0) { sp_say("o agente ainda trabalhava depois de %.2f h de espera: DESISTI, nada foi treinado", hours); return 2; }
        if (polls++ % 20 == 19) sp_say("ainda esperando (ultima gravacao de sessao ha %ld s)", age);
        sleep((unsigned)poll);
    }
}
#else
static int sp_wait_idle(double hours, const char *sessions) { (void)hours; (void)sessions; sp_say("--wait-idle so no Linux: sigo"); return 0; }
#endif

/* ---------- historico e relatorio ---------- */
static int sp_jnum(const char *line, const char *key, double *v) {
    char k[64]; snprintf(k, sizeof k, "\"%s\": ", key);
    const char *p = strstr(line, k);
    if (!p) return 0;
    *v = atof(p + strlen(k));
    return 1;
}
static long sp_jmap(const char *line, const char *map, const char *name) {          /* "map": {"name": N, ...} (mapa plano) */
    char k[64]; snprintf(k, sizeof k, "\"%s\": {", map);
    const char *p = strstr(line, k);
    if (!p) return 0;
    p += strlen(k);
    const char *end = strchr(p, '}'); if (!end) return 0;
    snprintf(k, sizeof k, "\"%s\": ", name);
    const char *q = strstr(p, k);
    return q && q < end ? atol(q + strlen(k)) : 0;
}
static int sp_jstr(const char *line, const char *key, char *out, size_t cap) {
    char k[64]; snprintf(k, sizeof k, "\"%s\": \"", key);
    const char *p = strstr(line, k);
    if (!p) { out[0] = 0; return 0; }
    p += strlen(k); size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) { if (*p == '\\' && p[1]) p++; out[n++] = *p++; }
    out[n] = 0;
    return 1;
}
/* janelas por fonte de uma linha de log: realised = "tools 10 janelas (50%)"; senao "tools 30% (123 janelas)" */
static int sp_win_parse(const char *log, const char *tag, int realised, long out[SP_NSRC]) {
    char line[8192];
    if (!sp_grep(log, tag, line, sizeof line, 1)) return 0;
    for (int s = 0; s < SP_NSRC; s++) {
        char key[40]; snprintf(key, sizeof key, " %s ", sp_srcname[s]);
        const char *p = strstr(line, key);
        if (!p) continue;
        p += strlen(key);
        if (realised) out[s] = atol(p); else { const char *q = strchr(p, '('); out[s] = q ? atol(q + 1) : 0; }
    }
    return 1;
}

/* linhas de history.jsonl da cadeia atual do tipo (desde a ultima noite que comecou do base); devolve quantas */
static int sp_chain(const char *root, const char *kind, char ***lines) {
    char hp[2400]; snprintf(hp, sizeof hp, "%s/history.jsonl", root);
    FILE *f = fopen(hp, "rb"); int n = 0;
    *lines = NULL;
    if (!f) return 0;
    char *line = NULL; size_t cap = 0; int nl; long len; char kd[32];
    while ((len = sp_getline(f, &line, &cap, &nl)) != -1) {
        if (len < 2 || !sp_jstr(line, "kind", kd, sizeof kd)) continue;
        if (kind && strcmp(kd, kind)) continue;
        if (strstr(line, "\"resumed_from\": null")) { for (int i = 0; i < n; i++) free((*lines)[i]); n = 0; }
        *lines = realloc(*lines, sizeof(char *) * (size_t)(n + 1)); (*lines)[n++] = strdup(line);
    }
    free(line); fclose(f);
    return n;
}

/* relatorio para uma pessoa: que noite e, janelas vistas contra as que existem, perda por noite e os comandos */
static void sp_report(const char *root, const char *kind) {
    char kd0[32] = "";
    if (!kind) {                                        /* o tipo da ultima noite registrada */
        char **l0; const int n0 = sp_chain(root, NULL, &l0);
        if (n0) { sp_jstr(l0[n0 - 1], "kind", kd0, sizeof kd0); kind = kd0; }
        for (int i = 0; i < n0; i++) free(l0[i]);
        free(l0);
    }
    char **ln; int n = sp_chain(root, kind, &ln);
    if (!n && !kind) { printf("nenhuma noite registrada em %s/history.jsonl\n", root); return; }
    if (!n) { printf("nenhuma noite do tipo %s em %s/history.jsonl\n", kind, root); return; }
    const char *last = ln[n - 1]; char model[2300], spec[2300], date[40], kd[32];
    sp_jstr(last, "model", model, sizeof model); sp_jstr(last, "spec", spec, sizeof spec); sp_jstr(last, "date", date, sizeof date); sp_jstr(last, "kind", kd, sizeof kd);
    char home_h[2300]; const char *home = getenv("HOME"); snprintf(home_h, sizeof home_h, "%s/.katana/specs", home ? home : ".");
    sp_say("--- relatorio: noite %d desta especializacao (%s, tipo %s) ---", n, date, kd);
    long tw = 0, ta = 0; double cover = 0; Buf b = {0};
    for (int s = 0; s < SP_NSRC; s++) {
        long w = 0; for (int i = 0; i < n; i++) w += sp_jmap(ln[i], "windows", sp_srcname[s]);
        const long a = sp_jmap(last, "available", sp_srcname[s]);
        if (!w && !a) continue;
        char t[160]; snprintf(t, sizeof t, "%s%s %ld de %ld (%.1f%%)", b.n ? "; " : "", sp_srcname[s], w, a, a ? 100.0 * w / a : 0.0);
        buf_str(&b, t); tw += w; ta += a; if (a) cover += a * (1.0 - exp(-(double)w / a));
    }
    sp_say("janelas treinadas ate agora de cada fonte, contra as que existem: %s; total %ld de %ld (%.1f%%)", b.p ? b.p : "-", tw, ta, ta ? 100.0 * tw / ta : 0.0);
    sp_say("  as noites sorteiam as janelas de novo (uma semente por noite), entao parte se repete: cobertura distinta esperada cerca de %.1f%% (estimativa, nao medida)", ta ? 100.0 * cover / ta : 0.0);
    free(b.p); b = (Buf){0};
    for (int i = 0; i < n; i++) {
        double l0 = -1, l1 = -1; long st = 0; double stp = 0;
        sp_jnum(ln[i], "loss_first10", &l0); sp_jnum(ln[i], "loss_last10", &l1); if (sp_jnum(ln[i], "steps", &stp)) st = (long)stp;
        char t[160]; snprintf(t, sizeof t, "%snoite %d: %.3f -> %.3f (%ld passos)", i ? "; " : "", i + 1, l0, l1, st);
        buf_str(&b, t);
    }
    sp_say("perda por noite (media dos 10 primeiros passos -> dos 10 ultimos): %s", b.p ? b.p : "-");
    free(b.p);
    sp_say("testar:  KATANA_SPEC=%s ./katana agent %s", spec, model);
    sp_say("ativar no desktop:  mkdir -p %s && cp %s %s/", home_h, spec, home_h);
    char hold[2400]; snprintf(hold, sizeof hold, "%s/holdout.jsonl", root);
    if (sp_exists(hold)) sp_say("medir o conhecimento (antes e depois do treino):  ./katana knowledge quiz %s %s --spec %s", model, hold, spec);
    for (int i = 0; i < n; i++) free(ln[i]);
    free(ln);
}

/* atualiza a base de conhecimento (incremental) e regera corpus e holdout. 0 = parou com erro */
static int sp_knowledge(SpCtx *x, const char *from, const char *kdir, const char *corpus, const char *hold) {
    char log[2300], line[1024], why[1024];
    SpArgs a = {0};
    if (from) {
        snprintf(log, sizeof log, "%s/knowledge-build.log", x->out);
        sp_a(&a, x->self); sp_a(&a, "knowledge"); sp_a(&a, "build"); sp_a(&a, from); sp_a(&a, "--out"); sp_a(&a, kdir);
        for (int i = 0; i < x->nrep; i++) { sp_a(&a, "--replace"); sp_a(&a, x->rep[i]); }
        if (x->cut) { sp_a(&a, "--cut-matching"); sp_a(&a, x->cut); }
        const time_t t0 = time(NULL);
        const int rc = sp_run(a.v, log);
        if (rc != 0) { sp_why(log, why, sizeof why); sp_say("PAROU: katana knowledge build falhou com codigo %d%s%s; veja %s", rc, why[0] ? ": " : "", why, log); return 0; }
        if (sp_grep(log, "knowledge: ", line, sizeof line, 1)) sp_say("base de conhecimento (%lds): %s", (long)(time(NULL) - t0), line);
    } else sp_say("base de conhecimento: sem transcricoes para atualizar; uso as notas que ja existem em %s", kdir);
    char idx[2400]; snprintf(idx, sizeof idx, "%s/INDEX.md", kdir);
    if (!sp_exists(idx)) { sp_say("base de conhecimento: %s nao tem notas; a fonte knowledge fica de fora", kdir); return 1; }
    snprintf(log, sizeof log, "%s/knowledge-corpus.log", x->out);
    SpArgs b = {0};
    sp_a(&b, x->self); sp_a(&b, "knowledge"); sp_a(&b, "corpus"); sp_a(&b, kdir); sp_a(&b, corpus); sp_a(&b, "--holdout"); sp_a(&b, hold);
    const int rc = sp_run(b.v, log);
    if (rc != 0) { sp_why(log, why, sizeof why); sp_say("PAROU: katana knowledge corpus falhou com codigo %d%s%s; veja %s", rc, why[0] ? ": " : "", why, log); return 0; }
    if (sp_grep(log, "corpus: ", line, sizeof line, 1)) sp_say("%s", line);
    if (sp_grep(log, "holdout: ", line, sizeof line, 1)) sp_say("%s", line);
    return 1;
}

/* katana specialize <modelo.gguf> [all|tools|code|teacher|knowledge] [opcoes]   |   katana specialize --status */
static int specialize_main(const char *argv0, int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "uso: katana specialize <modelo.gguf> [all|tools|code|teacher|knowledge|conventions] [--from PASTA] [--sessions PASTA] [--corpus ARQ]\n"
                        "         [--curriculum ARQ] [--knowledge PASTA] [--no-refresh] [--training-dir PASTA] [--wait-idle [HORAS]]\n"
                        "         [--mix tools=30,code=30,conventions=40,teacher=15,knowledge=15] [--out PASTA] [--fresh] [--hours H] [--steps N] [--rank R]\n"
                        "         [--seq T] [--lr X] [--margin-mb MB] [--replace REGEX=TEXTO]... [--cut-matching REGEX] [--verified]\n"
                        "         [--plan-updates] [--dry-run] [--no-check]\n"
                        "         [--val F] [--val-max N] [--allow-worse]     validacao separada por conversa (padrao 10%, no maximo ~10% do custo do treino); sem ela nada vale\n"
                        "     katana specialize --status [--training-dir PASTA]\n"
                        "     katana specialize --verify-data data.jsonl      confere um arquivo de dados (cada linha JSON inteira)\n"
                        "     katana.cfg: transcripts=PASTA, training-dir=PASTA, replace=REGEX=TEXTO (repetivel), cut-matching=REGEX\n");
        return 1;
    }
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home) home = getenv("USERPROFILE");
#endif
    if (!home) home = ".";
    SpCfg cfg; sp_cfg_load(&cfg, home);
    if (argc >= 2 && !strcmp(argv[0], "--verify-data")) return sp_verify(argv[1], NULL) ? 0 : 7;       /* conferir um arquivo de dados que ja existe */
    SpCtx x; memset(&x, 0, sizeof x);
    int status = !strcmp(argv[0], "--status");
    x.model = status ? "" : argv[0]; x.kind = "all";
    int i0 = status ? 0 : 1;
    if (!status && argc > 1 && argv[1][0] != '-') { x.kind = argv[1]; i0 = 2; }
    int kind = -1;
    for (int s = 0; s < SP_NSRC; s++) if (!strcmp(x.kind, sp_srcname[s])) kind = s;
    const int all = !strcmp(x.kind, "all");
    if (kind < 0 && !all) { fprintf(stderr, "specialize: tipo %s desconhecido (all, tools, code, teacher, knowledge ou conventions)\n", x.kind); return 1; }
    const char *from = NULL, *sessions = NULL, *corpus = NULL, *outdir = NULL, *rank = "8", *seq = "512", *lr = "2e-4", *kdir = NULL, *trdir = NULL;
    const char *mix = "tools=30,code=30,conventions=40,teacher=15,knowledge=15";
    const char *curriculum = NULL;
    double hours = 5, wait_h = 0, valf = 0.1; int steps = 0, dry = 0, check = 1, fresh = 0, refresh = 1, valmax = 0, allow_worse = 0; long margin = 6000;
    /* trocas e cortes persistentes do katana.cfg; as da linha de comando vem depois (o corte da linha vence) */
    for (int i = 0; i < cfg.n; i++) {
        if (!strcmp(cfg.k[i], "replace") && x.nrep < SP_MAXREP) x.rep[x.nrep++] = cfg.v[i];
        else if (!strcmp(cfg.k[i], "cut-matching")) x.cut = cfg.v[i];
    }
    for (int i = i0; i < argc; i++) {
        const char *o = argv[i]; const int more = i + 1 < argc;
        if (!strcmp(o, "--from") && more) from = argv[++i];
        else if (!strcmp(o, "--sessions") && more) sessions = argv[++i];
        else if (!strcmp(o, "--corpus") && more) corpus = argv[++i];
        else if (!strcmp(o, "--knowledge") && more) kdir = argv[++i];
        else if (!strcmp(o, "--curriculum") && more) curriculum = argv[++i];
        else if (!strcmp(o, "--training-dir") && more) trdir = argv[++i];
        else if (!strcmp(o, "--no-refresh")) refresh = 0;
        else if (!strcmp(o, "--status")) status = 1;
        else if (!strcmp(o, "--wait-idle")) { wait_h = 6; if (more && (isdigit((unsigned char)argv[i + 1][0]) || argv[i + 1][0] == '.')) wait_h = atof(argv[++i]); }
        else if (!strcmp(o, "--mix") && more) mix = argv[++i];
        else if (!strcmp(o, "--out") && more) outdir = argv[++i];
        else if (!strcmp(o, "--hours") && more) hours = atof(argv[++i]);
        else if (!strcmp(o, "--steps") && more) steps = atoi(argv[++i]);
        else if (!strcmp(o, "--rank") && more) rank = argv[++i];
        else if (!strcmp(o, "--seq") && more) seq = argv[++i];
        else if (!strcmp(o, "--lr") && more) lr = argv[++i];
        else if (!strcmp(o, "--margin-mb") && more) margin = atol(argv[++i]);
        else if (!strcmp(o, "--replace") && more) { if (x.nrep < SP_MAXREP) x.rep[x.nrep++] = argv[++i]; else { fprintf(stderr, "specialize: replace demais (no maximo %d, contando o katana.cfg)\n", SP_MAXREP); return 1; } }
        else if (!strcmp(o, "--cut-matching") && more) x.cut = argv[++i];
        else if (!strcmp(o, "--val") && more) valf = atof(argv[++i]);
        else if (!strcmp(o, "--val-max") && more) valmax = atoi(argv[++i]);
        else if (!strcmp(o, "--allow-worse")) allow_worse = 1;
        else if (!strcmp(o, "--dry-run")) dry = 1;
        else if (!strcmp(o, "--no-check")) check = 0;
        else if (!strcmp(o, "--fresh")) fresh = 1;
        else if ((!strcmp(o, "--verified") || !strcmp(o, "--plan-updates")) && x.nextra < 15) x.extra[x.nextra++] = o;
        else { fprintf(stderr, "specialize: opcao desconhecida %s\n", o); return 1; }
    }
    char trbuf[2400], kbuf[2400], sessdef[2400], outbuf[2400], root[2400], self[2400];
    if (!trdir && sp_cfg_get(&cfg, "training-dir")) { char *t = sp_cfg_path(sp_cfg_get(&cfg, "training-dir"), home); snprintf(trbuf, sizeof trbuf, "%s", t); free(t); trdir = trbuf; }
    if (!trdir) { snprintf(trbuf, sizeof trbuf, "%s/.katana/training", home); trdir = trbuf; }
    if (status) {
        if (outdir) { snprintf(root, sizeof root, "%s", outdir); sp_report(root, NULL); }
        else sp_report(trdir, NULL);
        return 0;
    }
    snprintf(sessdef, sizeof sessdef, "%s/.katana/desktop/sessoes", home);
    char *from_cfg = NULL;
    if (!from && (all || kind == SP_TOOLS || kind == SP_CODE) && sp_cfg_get(&cfg, "transcripts")) { from_cfg = sp_cfg_path(sp_cfg_get(&cfg, "transcripts"), home); from = from_cfg; }
    /* decisao do dono (2026-11-21): no treino unico (all) entram so tools, code e conventions. As falas do professor e o
       conhecimento dos projetos ficam de fora, a nao ser que sejam pedidos pelo nome (--sessions, --corpus) ou pelo tipo */
    if (!sessions && !all && kind != SP_KNOW && kind != SP_CONV && sp_isdir(sessdef)) sessions = sessdef;
    if (!outdir) {
        time_t t = time(NULL); char ts[32]; strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", localtime(&t));
        snprintf(outbuf, sizeof outbuf, "%s/%s-%s", trdir, x.kind, ts);
        outdir = outbuf;
    }
    snprintf(root, sizeof root, "%s", outdir);
    { char *sl = strrchr(root, '/'); if (sl && sl != root) *sl = 0; else snprintf(root, sizeof root, "."); }
    if (!sp_mkdirs(outdir)) { fprintf(stderr, "specialize: nao criei %s: %s\n", outdir, strerror(errno)); return 1; }
    x.out = outdir;
    lr_self(self, sizeof self, argv0); x.self = self;
    char path[2400];
    snprintf(g_sp_reppath, sizeof g_sp_reppath, "%s/report.txt", outdir);
    g_sp_rep = fopen(g_sp_reppath, "ab");
    /* nada grande em /tmp (aqui ele fica na RAM): o que um filho criar sem pedir pasta cai ao lado da saida */
    snprintf(path, sizeof path, "%s/tmp", outdir); sp_mkdirs(path); sp_env("TMPDIR", path);
#ifndef _WIN32
    if (nice(10) == -1) { }                     /* os filhos herdam: o treino nao disputa a maquina com quem a usa */
#endif
    sp_say("especializacao %s do modelo %s", x.kind, x.model);
    sp_say("saida: %s", outdir);
    if (wait_h > 0) { const int rw = sp_wait_idle(wait_h, sessdef); if (rw) return rw; }

    /* 1. prompt de sistema do agente, no formato do modelo. Sem a secao de ferramentas (o modelo as aprende pelo
       treino) em tools, knowledge e all; teacher e code ficam com o prompt completo */
    const int short_sys = all || kind == SP_TOOLS || kind == SP_KNOW || kind == SP_CONV;
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
    char sysf[2400]; snprintf(sysf, sizeof sysf, "%s/sys.txt", outdir); x.sysf = sysf;
    {   FILE *f = fopen(sysf, "wb");
        if (!f) { sp_say("PAROU: nao gravei %s: %s", sysf, strerror(errno)); return 1; }
        if (!io_puts(f, sys, sysf) | !io_close(f, sysf)) { sp_say("PAROU: nao gravei %s", sysf); return 1; } }
    sp_say("prompt de sistema: formato %s, %zu caracteres%s", fmt, strlen(sys), short_sys ? " (sem a secao de ferramentas)" : "");

    /* 2. conhecimento (base incremental, corpus e holdout) e dados: cada fonte com as regras do seu tipo */
    char corpus_auto[2400], hold_auto[2400], kdir_buf[2400];
    if (!corpus && refresh && kind == SP_KNOW) {
        if (!kdir) {
            if (dry) { snprintf(kdir_buf, sizeof kdir_buf, "%s/knowledge", outdir); kdir = kdir_buf; }     /* ensaio: nada fora de --out */
            else if (getenv("KATANA_KNOWLEDGE") && *getenv("KATANA_KNOWLEDGE")) kdir = getenv("KATANA_KNOWLEDGE");
            else { snprintf(kdir_buf, sizeof kdir_buf, "%s/.katana/knowledge", home); kdir = kdir_buf; }
        }
        snprintf(corpus_auto, sizeof corpus_auto, "%s/corpus.jsonl", dry ? outdir : root);
        snprintf(hold_auto, sizeof hold_auto, "%s/holdout.jsonl", dry ? outdir : root);
        if (!sp_knowledge(&x, from, kdir, corpus_auto, hold_auto)) return 3;
        if (sp_exists(corpus_auto)) corpus = corpus_auto;
    } else if (!corpus && !refresh && kind == SP_KNOW) {
        snprintf(corpus_auto, sizeof corpus_auto, "%s/corpus.jsonl", root);
        if (sp_exists(corpus_auto)) { corpus = corpus_auto; sp_say("--no-refresh: uso o corpus que ja existe, %s", corpus); }
    }
    snprintf(path, sizeof path, "%s/data.jsonl", outdir);
    char ilog0[2400]; snprintf(ilog0, sizeof ilog0, "%s/import.log", outdir);
    if (!sp_truncate(path) || !sp_truncate(ilog0)) { sp_say("PAROU: nao consegui criar os arquivos de dados em %s", outdir); return 1; }
    if (!from && (all || kind == SP_TOOLS || kind == SP_CODE))
        sp_say("transcricoes: nenhuma pasta (use --from PASTA ou a linha transcripts=PASTA no katana.cfg); sigo com as outras fontes");
    if (from) {
        if (all || kind == SP_TOOLS) if (!sp_import(&x, from, SP_TOOLS)) return 3;
        if (all || kind == SP_CODE) if (!sp_import(&x, from, SP_CODE)) return 3;
    }
    if (sessions) {
        const int st = all || kind == SP_TEACHER ? SP_TEACHER : kind == SP_CODE ? SP_CODE : SP_TOOLS;
        char **ls = NULL; const int n = kind == SP_KNOW ? 0 : sp_sessions(sessions, st == SP_TEACHER, &ls);
        for (int i = 0; i < n; i++) { const int ok = sp_import(&x, ls[i], st); free(ls[i]); if (!ok) return 3; }
        free(ls);
        sp_say("sessoes em %s: %d%s", sessions, n, st == SP_TEACHER ? " com falas do professor" : "");
    }
    if (corpus && (all || kind == SP_KNOW)) if (!sp_import(&x, corpus, SP_KNOW)) return 3;
    if (all || kind == SP_CONV) {
        char cur_auto[2400];
        if (!curriculum) {
            /* o subcomando pode nao existir neste binario: pergunta a ele (codigo de saida de --help), nao em tempo de compilacao */
            char hl[2400]; snprintf(hl, sizeof hl, "%s/curriculum-help.log", outdir);
            SpArgs ha = {0}; sp_a(&ha, self); sp_a(&ha, "curriculum"); sp_a(&ha, "--help");
            if (sp_run(ha.v, hl) != 0) sp_say("conventions: este binario ainda nao tem o subcomando katana curriculum; a fonte fica de fora (use --curriculum ARQ para dar um arquivo)");
            else {
                /* tamanho: as janelas que a mistura da a esta fonte numa noite (passos dados, ou o que cabe em --hours a cerca de 245 s por
                   janela, estimativa), com folga de 2x conversas; semente derivada da data (muda a cada rodada) */
                double wsum = 0, wconv = 0;
                for (int s = 0; s < SP_NSRC; s++) { const char *p = mix; const size_t l = strlen(sp_srcname[s]);
                    while (p && *p) { if (!strncmp(p, sp_srcname[s], l) && p[l] == '=') { const double w = atof(p + l + 1); wsum += w; if (s == SP_CONV) wconv = w; break; } p = strchr(p, ','); if (p) p++; } }
                const double share = kind == SP_CONV ? 1.0 : (wsum > 0 ? wconv / wsum : 0.25);
                long est = steps > 0 ? steps : (long)(hours * 3600 / 245.0); if (est > 400) est = 400; if (est < 4) est = 4;
                long nconv = (long)(2.0 * share * est + 0.5); if (nconv < 20) nconv = 20;
                char nn[32], sd[32];
                snprintf(nn, sizeof nn, "%ld", nconv); snprintf(sd, sizeof sd, "%ld", (long)(time(NULL) / 60) & 0x7fffffff);
                snprintf(cur_auto, sizeof cur_auto, "%s/curriculum.jsonl", outdir);
                SpArgs ca = {0}; sp_a(&ca, self); sp_a(&ca, "curriculum"); sp_a(&ca, cur_auto); sp_a(&ca, "--n"); sp_a(&ca, nn); sp_a(&ca, "--seed"); sp_a(&ca, sd);
                char cl[2400], why[1024]; snprintf(cl, sizeof cl, "%s/curriculum.log", outdir);
                const int rc = sp_run(ca.v, cl);
                if (rc != 0) { sp_why(cl, why, sizeof why); sp_say("PAROU: katana curriculum falhou com codigo %d%s%s; veja %s", rc, why[0] ? ": " : "", why, cl); return 3; }
                sp_say("conventions: %ld conversas sinteticas novas (semente %s) em %s", nconv, sd, cur_auto);
                curriculum = cur_auto;
            }
        }
        if (curriculum) if (!sp_import(&x, curriculum, SP_CONV)) return 3;
    }
    if (!x.ninputs) { sp_say("nenhuma entrada para o tipo %s: use --from PASTA (ou transcripts=PASTA no katana.cfg), --sessions PASTA e/ou --corpus ARQ", x.kind); return 3; }
    for (int s = 0; s < SP_NSRC; s++) if (x.s_kept[s])
        sp_say("fonte %-9s: %ld conversas, %ld falas que contam, %ld caracteres a aprender", sp_srcname[s], x.s_kept[s], x.s_turns[s], x.s_chars[s]);
    sp_say("dados: %ld conversas gravadas, %ld descartadas, %ld falas do modelo que contam, %ld caracteres a aprender "
           "(estimativa: cerca de %.0f janelas de 512 tokens); detalhes por motivo e por ferramenta em import.log",
           x.kept, x.dropped, x.turns, x.chars, x.chars / 3.3 / 512.0);
    if (!sp_verify(path, &x)) return 7;
    if (!x.turns) { sp_say("nenhuma fala a aprender: nada a treinar"); return 3; }

    /* 3. plano de memoria */
    /* a placa e usada se houver (o treino mede a VRAM livre e fica na CPU se nao couber); KATANA_TRAIN_GPU=0 desliga */
    { const char *g = getenv("KATANA_TRAIN_GPU"); sp_say("produtos do treino: %s", g && !strcmp(g, "0") ? "so na CPU (KATANA_TRAIN_GPU=0)" : "placa e CPU"); }
    char data[2400], plan[2400], line[8192], resume[2400] = "", specname[64];
    snprintf(specname, sizeof specname, "%s.kspec", all ? "katana" : x.kind);
    if (!fresh && sp_newest(root, x.kind, specname, outdir, resume, sizeof resume)) sp_say("continua o treino de %s (--fresh comeca do modelo base)", resume);
    char **chain = NULL; const int nprev = resume[0] ? sp_chain(root, x.kind, &chain) : 0;
    for (int i = 0; i < nprev; i++) free(chain[i]);
    free(chain);
    const int night = nprev + 1;
    char seedarg[32]; snprintf(seedarg, sizeof seedarg, "%d", night);       /* a semente muda a cada noite: sem isso toda noite treinava as mesmas janelas */
    snprintf(data, sizeof data, "%s/data.jsonl", outdir);
    snprintf(plan, sizeof plan, "%s/plan.log", outdir);
    SpArgs base = {0};
    sp_a(&base, self); sp_a(&base, "train"); sp_a(&base, x.model); sp_a(&base, data);
    sp_a(&base, "--train"); sp_a(&base, "blk.*.attn_*"); sp_a(&base, "--rank"); sp_a(&base, rank); sp_a(&base, "--seq"); sp_a(&base, seq);
    sp_a(&base, "--lr"); sp_a(&base, lr); sp_a(&base, "--final"); sp_a(&base, "0"); sp_a(&base, "--val"); sp_a(&base, "0");
    sp_a(&base, "--seed"); sp_a(&base, seedarg);
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
    long wavail[SP_NSRC] = {0};
    if (!sp_win_parse(plan, "mistura pedida:", 0, wavail) && sp_grep(plan, "treino: ", line, sizeof line, 0) && kind >= 0) wavail[kind] = atol(strstr(line, "treino: ") + 8);
    { long tot = 0; for (int s = 0; s < SP_NSRC; s++) tot += wavail[s];
      Buf wb = {0}; for (int s = 0; s < SP_NSRC; s++) { char t[80]; snprintf(t, sizeof t, "%s%s %ld", s ? ", " : "", sp_srcname[s], wavail[s]); buf_str(&wb, t); }
      sp_say("janelas de treino disponiveis (de %s tokens): %s; total %ld", seq, wb.p, tot); free(wb.p); }
    if (dry) { sp_say("--dry-run: parei depois dos dados e do plano (noite %d se treinasse)", night); if (g_sp_rep) io_close(g_sp_rep, g_sp_reppath); return 0; }

    /* 4. tempo de um passo e quantos cabem */
    if (steps <= 0) {
        char ml[2400], mk[2400];
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
    char nsteps[32], logf[2400], spec[2400], trl[2400];
    snprintf(nsteps, sizeof nsteps, "%d", steps);
    snprintf(logf, sizeof logf, "%s/steps.jsonl", outdir);
    snprintf(spec, sizeof spec, "%s/%s", outdir, specname);
    snprintf(trl, sizeof trl, "%s/train.log", outdir);
    remove(logf);
    a = base; sp_a(&a, "--steps"); sp_a(&a, nsteps); sp_a(&a, "--log"); sp_a(&a, logf); sp_a(&a, "--out"); sp_a(&a, spec);
    /* validacao separada por conversa (a semente do treino fixa quais). Mede a base antes do passo 1 e o estado do treino na
       metade e no fim (3 medidas); cada medida custa cerca de um terco de uma janela de treino por janela de validacao, entao
       com no maximo steps/10 janelas o conjunto todo custa cerca de 10% do treino. O treino recusa gravar o que nao vence a base. */
    char nval[32], nvmax[32], neval[32];
    if (valf > 0) {
        long vm = valmax > 0 ? valmax : steps / 10; if (vm < 6) vm = 6;
        snprintf(nval, sizeof nval, "%g", valf); snprintf(nvmax, sizeof nvmax, "%ld", vm); snprintf(neval, sizeof neval, "%d", steps > 1 ? steps / 2 : 1);
        sp_a(&a, "--val"); sp_a(&a, nval); sp_a(&a, "--val-max"); sp_a(&a, nvmax); sp_a(&a, "--eval"); sp_a(&a, neval);
        sp_say("validacao: %s das conversas separadas, no maximo %ld janelas, medida na base, na metade e no fim", nval, vm);
    } else sp_say("validacao desligada (--val 0): nao ha como saber se o treino melhorou o modelo");
    if (allow_worse) sp_a(&a, "--allow-worse");
    t0 = time(NULL);
    rc = sp_run(a.v, trl);
    const double took_h = (double)(time(NULL) - t0) / 3600.0;
    sp_say("treino terminou com codigo %d em %.0f min", rc, took_h * 60);
    if (sp_grep(trl, "velocidade:", line, sizeof line, 1)) sp_say("%s", line);
    if (rc != 0 && sp_grep(trl, "a GPU falhou", line, sizeof line, 1)) sp_say("a placa falhou no meio do treino: rode de novo com KATANA_TRAIN_GPU=0");
    if (sp_grep(trl, "mistura realizada:", line, sizeof line, 1)) sp_say("%s", line);
    if (sp_grep(trl, "retomado de", line, sizeof line, 1)) sp_say("%s", line);
    if (sp_grep(trl, "validacao BASE", line, sizeof line, 1)) sp_say("%s", line);
    if (sp_grep(trl, "VALIDACAO:", line, sizeof line, 1)) sp_say("%s", line);
    if (sp_grep(trl, "resultado:", line, sizeof line, 1)) sp_say("%s", line);
    if (rc == 2) {            /* o treino recusou gravar: a validacao nunca venceu a base (ou o treino divergiu) */
        sp_say("RECUSADA: a especializacao nao venceu o modelo base na validacao e NAO foi gravada (--allow-worse grava mesmo assim); veja %s", trl);
        return 9;
    }
    double lf0, lf1;
    sp_losses(logf, &lf0, &lf1);
    struct stat sst;
    if (rc != 0 || stat(spec, &sst) || sst.st_size == 0) { sp_say("nenhuma especializacao gravada"); return 6; }
    sp_say("especializacao: %s (%ld KB)", spec, (long)sst.st_size / 1024);
    long wtr[SP_NSRC] = {0};
    if (!sp_win_parse(trl, "mistura realizada:", 1, wtr) && kind >= 0) wtr[kind] = steps;      /* sem mistura: uma janela por passo */
    {   /* historico das noites, na pasta de cima */
        char hp[2400]; snprintf(hp, sizeof hp, "%s/history.jsonl", root);
        FILE *h = fopen(hp, "ab");
        if (!h) { sp_say("PAROU: nao abri %s: %s (a especializacao ficou em %s)", hp, strerror(errno), spec); return 8; }
        char ts[32]; time_t t = time(NULL); strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", localtime(&t));
        Buf b = {0};
        buf_str(&b, "{\"date\": \""); buf_str(&b, ts); buf_str(&b, "\", \"kind\": \""); buf_str(&b, x.kind); buf_str(&b, "\", \"model\": \"");
        js_escape(&b, x.model, strlen(x.model)); buf_str(&b, "\", \"spec\": \"");
        js_escape(&b, spec, strlen(spec)); buf_str(&b, "\", \"resumed_from\": ");
        if (resume[0]) { buf_str(&b, "\""); js_escape(&b, resume, strlen(resume)); buf_str(&b, "\""); } else buf_str(&b, "null");
        char tmp[400];
        snprintf(tmp, sizeof tmp, ", \"night\": %d, \"steps\": %d, \"hours\": %.3f, \"loss_first10\": %.4f, \"loss_last10\": %.4f, \"sources\": {", night, steps, took_h, lf0, lf1);
        buf_str(&b, tmp);
        int first = 1;
        for (int s = 0; s < SP_NSRC; s++) if (x.s_kept[s]) {
            snprintf(tmp, sizeof tmp, "%s\"%s\": {\"conversations\": %ld, \"turns\": %ld, \"chars\": %ld}", first ? "" : ", ", sp_srcname[s], x.s_kept[s], x.s_turns[s], x.s_chars[s]);
            buf_str(&b, tmp); first = 0;
        }
        buf_str(&b, "}, \"windows\": {");
        for (int s = 0; s < SP_NSRC; s++) { snprintf(tmp, sizeof tmp, "%s\"%s\": %ld", s ? ", " : "", sp_srcname[s], wtr[s]); buf_str(&b, tmp); }
        buf_str(&b, "}, \"available\": {");
        for (int s = 0; s < SP_NSRC; s++) { snprintf(tmp, sizeof tmp, "%s\"%s\": %ld", s ? ", " : "", sp_srcname[s], wavail[s]); buf_str(&b, tmp); }
        buf_str(&b, "}}\n");
        const int okh = io_write(h, b.p, b.n, hp);
        free(b.p);
        if (!(io_close(h, hp) && okh)) { sp_say("PAROU: o historico %s nao foi gravado (a especializacao ficou em %s)", hp, spec); return 8; }
        sp_say("historico: %s", hp);
    }

    /* 6. o modelo com a especializacao carrega e responde? (CPU, sem sorteio, base e especializado) */
    if (check) {
        static const char *q = "Em uma frase: o que voce faz primeiro quando um script de instalacao falha?";
        char ab[2400], at[2400];
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
    sp_say("fim. Nada foi aplicado sozinho.");
    sp_report(root, x.kind);
    sp_say("ou um GGUF com o treino dentro: katana merge %s %s <saida.gguf>", x.model, spec);
    if (g_sp_rep) io_close(g_sp_rep, g_sp_reppath);
    free(from_cfg);
    return 0;
}
