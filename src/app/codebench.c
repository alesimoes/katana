/* ---------- katana codebench: o juiz do katana (pequenos projetos com testes escondidos) ----------
 * Tarefas em tools/codebench/tasks/<nome>/: instruction.md (o pedido), repo/ (o projeto inicial), hidden/ (testes que
 * o agente nunca ve), solution/ (a correcao de referencia, usada so no verify), setup.py opcional (prepara o
 * ambiente da tarefa) e solution/solve.sh opcional. As tarefas sao material de teste na linguagem que quiserem; o
 * juiz e este arquivo, em C: copia repo/ para uma pasta temporaria nova, roda o preparo, inicia o agente (este mesmo
 * binario) como processo filho com o pedido, mata o grupo de processos no fim do prazo, roda os testes escondidos
 * e grava uma linha JSON por execucao (os campos do run.py de comparacao, mais as medidas lidas do registro do
 * proprio agente: chamadas, falhas, recusadas, repetidas, rodadas, ultimo turno em texto, tokens lidos e escritos).
 *
 *   katana codebench run --model M.gguf --out r.jsonl [--tasks 'a,b-*'] [--repeat N] [--timeout S] [--label L]
 *                        [--katana BIN] [--env K=V]... [--sessions DIR] [--keep DIR] [--mode agent|code]
 *                        [--teacher URL --teacher-model M --distill-out f.jsonl ...] [--dir PASTA] [--python python3]
 *   katana codebench verify [--tasks ...]        sem modelo: cada tarefa falha intacta e passa com a solucao
 *   katana codebench compare A.jsonl B.jsonl     pareado: ganhas, perdidas, teste do sinal, custo medio
 *   katana codebench list
 *
 * O preparo (setup.py) e codigo da tarefa, em Python: um pequeno hospedeiro (cb_shim) carrega o arquivo, chama
 * setup(pasta), devolve uma linha JSON (vars, agent_env) e fica vivo ate o juiz fechar o cano; ai chama stop() (que
 * encerra servidores e apaga temporarios). Se o juiz morrer, o fim do cano faz o mesmo. Processos e grupos: so POSIX. */

#ifndef _WIN32
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
extern char **environ;

#define CB_MAXKV 64
typedef struct { char *k, *v; } CbKV;
typedef struct { CbKV kv[CB_MAXKV]; int n; } CbKVs;

typedef struct {
    const char *dir, *tasks, *out, *label, *katana, *mode, *keep, *sessions, *model, *python;
    const char *teacher, *tmodel, *tkeyenv, *tformat, *distill_out;
    int timeout, repeat;
    char *envs[64]; int nenv;
} CbOpt;

typedef struct { Buf out, err; int rc, timed_out; } CbRes;

typedef struct {                                  /* contexto do preparo de uma tarefa */
    CbKVs vars, agent_env;
    char home[1100];                              /* HOME so desta tarefa: nada da casa do dono e lido nem mudado */
    pid_t shim; int wfd;                          /* hospedeiro do setup.py (0: tarefa sem preparo) */
} CbCtx;

static double cb_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }

static void cb_kv_set(CbKVs *m, const char *k, const char *v) {
    for (int i = 0; i < m->n; i++) if (!strcmp(m->kv[i].k, k)) { free(m->kv[i].v); m->kv[i].v = strdup(v); return; }
    if (m->n >= CB_MAXKV) return;
    m->kv[m->n].k = strdup(k); m->kv[m->n].v = strdup(v); m->n++;
}
static void cb_kv_free(CbKVs *m) { for (int i = 0; i < m->n; i++) { free(m->kv[i].k); free(m->kv[i].v); } m->n = 0; }

/* ---- ambiente: vetor envp que se clona e se altera ---- */
static char **cb_env_new(void) {
    int n = 0; while (environ[n]) n++;
    char **e = xalloc((size_t)n + 1, sizeof(char *));
    for (int i = 0; i < n; i++) e[i] = strdup(environ[i]);
    e[n] = NULL;
    return e;
}
static void cb_env_set(char ***pe, const char *k, const char *v) {
    char **e = *pe; int n = 0; const size_t kl = strlen(k);
    for (; e[n]; n++) if (!strncmp(e[n], k, kl) && e[n][kl] == '=') {
        free(e[n]); e[n] = xalloc(kl + strlen(v) + 2, 1); sprintf(e[n], "%s=%s", k, v); return;
    }
    e = realloc(e, sizeof(char *) * (size_t)(n + 2));
    e[n] = xalloc(kl + strlen(v) + 2, 1); sprintf(e[n], "%s=%s", k, v); e[n + 1] = NULL;
    *pe = e;
}
static void cb_env_set_kv(char ***pe, const char *kv) {          /* "K=V" */
    const char *eq = strchr(kv, '=');
    if (!eq) return;
    char k[256]; snprintf(k, sizeof k, "%.*s", (int)(eq - kv < 255 ? eq - kv : 255), kv);
    cb_env_set(pe, k, eq + 1);
}
static void cb_env_free(char **e) { for (int i = 0; e[i]; i++) free(e[i]); free(e); }

/* ambiente de agente (agent=1) ou de testes e solucao (agent=0: com CB_<VAR> do preparo e as variaveis do --env) */
static char **cb_ctx_env(const CbOpt *o, const CbCtx *c, int agent) {
    char **e = cb_env_new();
    if (c->home[0]) {
        const char *rh = getenv("HOME"), *cd = getenv("KATANA_CACHE_DIR");
        char cache[1200];
        if (!cd && rh && *rh) { snprintf(cache, sizeof cache, "%s/.katana/cache", rh); cd = cache; }      /* o prefixo do prompt de sistema e o mesmo: aproveita */
        cb_env_set(&e, "HOME", c->home);
        if (cd) cb_env_set(&e, "KATANA_CACHE_DIR", cd);
        cb_env_set(&e, "XDG_CONFIG_HOME", ""); cb_env_set(&e, "XDG_DATA_HOME", "");
    }
    if (agent) cb_env_set(&e, "KATANA_MCP_TRUST", "1");      /* os servidores MCP da tarefa sao da propria tarefa; sem terminal ninguem responde a pergunta */
    for (int i = 0; i < c->agent_env.n; i++) cb_env_set(&e, c->agent_env.kv[i].k, c->agent_env.kv[i].v);
    if (!agent) {
        for (int i = 0; i < c->vars.n; i++) {
            char k[300]; snprintf(k, sizeof k, "CB_%s", c->vars.kv[i].k);
            for (char *p = k; *p; p++) *p = (char)toupper((unsigned char)*p);
            cb_env_set(&e, k, c->vars.kv[i].v);
        }
        for (int i = 0; i < o->nenv; i++) cb_env_set_kv(&e, o->envs[i]);
    }
    return e;
}

/* ---- arquivos ---- */
static int cb_isdir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int cb_exists(const char *p) { struct stat st; return !stat(p, &st); }

static int cb_copy_file(const char *src, const char *dst, const struct stat *st) {
    FILE *a = fopen(src, "rb"); if (!a) return -1;
    FILE *b = fopen(dst, "wb"); if (!b) { fclose(a); return -1; }
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    fclose(a); fclose(b);
    chmod(dst, st->st_mode & 07777);
    struct timespec ts[2] = { st->st_atim, st->st_mtim };
    utimensat(AT_FDCWD, dst, ts, 0);
    return 0;
}

/* copia a arvore src para dst (cria dst); links para arquivo viram copia do conteudo; links para pasta ficam de fora */
static int cb_copy_tree(const char *src, const char *dst) {
    mkdir(dst, 0777);
    DIR *d = opendir(src);
    if (!d) return -1;
    struct dirent *de; int rc = 0;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char s[4400], t[4400]; struct stat st;
        snprintf(s, sizeof s, "%s/%s", src, de->d_name); snprintf(t, sizeof t, "%s/%s", dst, de->d_name);
        if (lstat(s, &st)) continue;
        if (S_ISLNK(st.st_mode)) { if (stat(s, &st) || S_ISDIR(st.st_mode)) continue; }
        if (S_ISDIR(st.st_mode)) { if (cb_copy_tree(s, t)) rc = -1; }
        else if (S_ISREG(st.st_mode)) { if (cb_copy_file(s, t, &st)) rc = -1; }
    }
    closedir(d);
    return rc;
}

static void cb_rmtree(const char *p) {
    struct stat st;
    if (lstat(p, &st)) return;
    if (S_ISDIR(st.st_mode)) {
        chmod(p, 0700);
        DIR *d = opendir(p);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                char c[4400]; snprintf(c, sizeof c, "%s/%s", p, de->d_name);
                cb_rmtree(c);
            }
            closedir(d);
        }
        rmdir(p);
    } else unlink(p);
}

static void cb_tmpbase(char *out, size_t cap) {
    const char *t = getenv("TMPDIR");
    snprintf(out, cap, "%s", t && *t ? t : "/tmp");
    size_t n = strlen(out); while (n > 1 && out[n - 1] == '/') out[--n] = 0;
}
/* pasta temporaria nova: <TMPDIR>/<prefixo>XXXXXX */
static int cb_mkdtemp(const char *prefix, char *out, size_t cap) {
    char base[1024]; cb_tmpbase(base, sizeof base);
    snprintf(out, cap, "%s/%sXXXXXX", base, prefix);
    return mkdtemp(out) ? 0 : -1;
}

static char *cb_slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    const size_t got = s ? fread(s, 1, (size_t)n, f) : 0;
    fclose(f);
    if (!s) return NULL;
    s[got] = 0; if (len) *len = got;
    return s;
}

/* glob simples: * ? [abc] [a-z] */
static int cb_glob(const char *p, const char *s) {
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (p[1] == '*') p++;
            if (!p[1]) return 1;
            for (; *s; s++) if (cb_glob(p + 1, s)) return 1;
            return cb_glob(p + 1, s);
        }
        if (!*s) return 0;
        if (*p == '?') continue;
        if (*p == '[') {
            const char *q = p + 1; int neg = 0, ok = 0;
            if (*q == '!' || *q == '^') { neg = 1; q++; }
            for (int first = 1; *q && (*q != ']' || first); q++, first = 0) {
                if (q[1] == '-' && q[2] && q[2] != ']') { if (*s >= q[0] && *s <= q[2]) ok = 1; q += 2; }
                else if (*q == *s) ok = 1;
            }
            if (ok == neg) return 0;
            p = *q ? q : q - 1;
            continue;
        }
        if (*p != *s) return 0;
    }
    return !*s;
}
static int cb_match_any(const char *list, const char *name) {      /* a, b-*, ... */
    char tmp[2048]; snprintf(tmp, sizeof tmp, "%s", list);
    for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ",")) if (cb_glob(t, name)) return 1;
    return 0;
}

static int cb_cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int cb_select(const CbOpt *o, char ***out) {
    char tdir[2100]; snprintf(tdir, sizeof tdir, "%s/tasks", o->dir);
    DIR *d = opendir(tdir); int n = 0; *out = NULL;
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[2400]; snprintf(p, sizeof p, "%s/%s", tdir, de->d_name);
        if (!cb_isdir(p)) continue;
        if (o->tasks && !cb_match_any(o->tasks, de->d_name)) continue;
        *out = realloc(*out, sizeof(char *) * (size_t)(n + 1)); (*out)[n++] = strdup(de->d_name);
    }
    closedir(d);
    if (n) qsort(*out, (size_t)n, sizeof(char *), cb_cmp_str);
    return n;
}

/* ---- JSON plano (so o necessario para ler as linhas de resultados e a resposta do hospedeiro do preparo) ---- */
typedef struct { char *k; int t; char *s; double d; } CbVal;       /* t: 0 texto, 1 numero, 2 booleano, 3 nulo/aninhado */
typedef struct { CbVal v[48]; int n; } CbObj;

static void cb_obj_free(CbObj *o) { for (int i = 0; i < o->n; i++) { free(o->v[i].k); free(o->v[i].s); } o->n = 0; }

static const char *cb_skip_ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }

static const char *cb_parse_str(const char *p, char **out) {        /* p em cima da aspa de abertura */
    Buf b = {0}; p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') buf_str(&b, "\n");
            else if (*p == 't') buf_str(&b, "\t");
            else if (*p == 'r') buf_str(&b, "\r");
            else if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) {
                unsigned cp = 0; sscanf(p + 1, "%4x", &cp); p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u') { unsigned lo = 0; sscanf(p + 3, "%4x", &lo); cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                char u[4]; int k = 0;
                if (cp < 0x80) u[k++] = (char)cp;
                else if (cp < 0x800) { u[k++] = (char)(0xC0 | (cp >> 6)); u[k++] = (char)(0x80 | (cp & 63)); }
                else if (cp < 0x10000) { u[k++] = (char)(0xE0 | (cp >> 12)); u[k++] = (char)(0x80 | ((cp >> 6) & 63)); u[k++] = (char)(0x80 | (cp & 63)); }
                else { buf_str(&b, "?"); k = 0; }
                buf_add(&b, u, (size_t)k);
            } else buf_add(&b, p, 1);
            p++;
        } else { buf_add(&b, p, 1); p++; }
    }
    if (*p == '"') p++;
    *out = b.p ? b.p : strdup("");
    return p;
}
static const char *cb_skip_nested(const char *p) {                 /* pula [..] ou {..} balanceado */
    int depth = 0;
    for (; *p; p++) {
        if (*p == '"') { char *t; p = cb_parse_str(p, &t); free(t); p--; }
        else if (*p == '[' || *p == '{') depth++;
        else if (*p == ']' || *p == '}') { if (--depth == 0) return p + 1; }
    }
    return p;
}
/* 0 se nao for um objeto */
static int cb_parse_obj(const char *s, CbObj *o) {
    o->n = 0;
    const char *p = cb_skip_ws(s);
    if (*p != '{') return 0;
    p = cb_skip_ws(p + 1);
    while (*p && *p != '}') {
        if (*p != '"') { cb_obj_free(o); return 0; }
        CbVal *v = &o->v[o->n]; memset(v, 0, sizeof *v);
        p = cb_parse_str(p, &v->k);
        p = cb_skip_ws(p); if (*p != ':') { free(v->k); return o->n ? 1 : 0; }
        p = cb_skip_ws(p + 1);
        if (*p == '"') { v->t = 0; p = cb_parse_str(p, &v->s); }
        else if (!strncmp(p, "true", 4)) { v->t = 2; v->d = 1; p += 4; }
        else if (!strncmp(p, "false", 5)) { v->t = 2; v->d = 0; p += 5; }
        else if (!strncmp(p, "null", 4)) { v->t = 3; p += 4; }
        else if (*p == '[' || *p == '{') { v->t = 3; p = cb_skip_nested(p); }
        else { char *e; v->t = 1; v->d = strtod(p, &e); if (e == p) { free(v->k); return o->n ? 1 : 0; } p = e; }
        if (o->n < 47) o->n++; else { free(v->k); free(v->s); }
        p = cb_skip_ws(p); if (*p == ',') p = cb_skip_ws(p + 1);
    }
    return 1;
}
static const CbVal *cb_get(const CbObj *o, const char *k) {
    for (int i = 0; i < o->n; i++) if (!strcmp(o->v[i].k, k)) return &o->v[i];
    return NULL;
}
static double cb_num(const CbObj *o, const char *k, double dflt) { const CbVal *v = cb_get(o, k); return v && (v->t == 1 || v->t == 2) ? v->d : dflt; }
static const char *cb_str(const CbObj *o, const char *k) { const CbVal *v = cb_get(o, k); return v && v->t == 0 ? v->s : NULL; }

/* texto JSON com escape de ASCII, como o json.dumps do Python (nao ASCII vira \uXXXX) */
static void cb_json_str(Buf *b, const char *s) {
    buf_str(b, "\"");
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned cp = *p; int len = 1;
        if (cp >= 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) { cp = ((cp & 7) << 18) | ((p[1] & 63) << 12) | ((p[2] & 63) << 6) | (p[3] & 63); len = 4; }
        else if (cp >= 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) { cp = ((cp & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); len = 3; }
        else if (cp >= 0xC0 && (p[1] & 0xC0) == 0x80) { cp = ((cp & 31) << 6) | (p[1] & 63); len = 2; }
        else if (cp >= 0x80) cp = 0xFFFD;
        char t[16];
        if (cp == '"') buf_str(b, "\\\"");
        else if (cp == '\\') buf_str(b, "\\\\");
        else if (cp == '\n') buf_str(b, "\\n");
        else if (cp == '\r') buf_str(b, "\\r");
        else if (cp == '\t') buf_str(b, "\\t");
        else if (cp == '\b') buf_str(b, "\\b");
        else if (cp == '\f') buf_str(b, "\\f");
        else if (cp < 0x20 || cp > 0x7E) {
            if (cp >= 0x10000) { const unsigned v = cp - 0x10000; snprintf(t, sizeof t, "\\u%04x\\u%04x", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF)); }
            else snprintf(t, sizeof t, "\\u%04x", cp);
            buf_str(b, t);
        } else { t[0] = (char)cp; buf_add(b, t, 1); }
        p += len;
    }
    buf_str(b, "\"");
}

/* ---- processos ---- */
static void cb_drain(int fd, Buf *b, int *open_) {
    char tmp[16384];
    for (;;) {
        const ssize_t n = read(fd, tmp, sizeof tmp);
        if (n > 0) { buf_add(b, tmp, (size_t)n); continue; }
        if (n == 0) { close(fd); *open_ = 0; }
        break;                                   /* EAGAIN ou fim */
    }
}

/* roda argv em cwd com o ambiente envp e a entrada; stdout e stderr separados; no fim do prazo mata o grupo inteiro.
 * Quando o filho sai, o que sobrou no grupo (servidores em segundo plano) tambem e morto. */
static int cb_exec(char *const *argv, const char *cwd, char **envp, const char *input, double tmo, CbRes *r) {
    memset(r, 0, sizeof *r);
    int pin[2], pout[2], perr[2];
    if (pipe(pin)) return -1;
    if (pipe(pout)) { close(pin[0]); close(pin[1]); return -1; }
    if (pipe(perr)) { close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]); return -1; }
    fflush(stdout); fflush(stderr);
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setpgid(0, 0);
        dup2(pin[0], 0); dup2(pout[1], 1); dup2(perr[1], 2);
        close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]); close(perr[0]); close(perr[1]);
        if (cwd && chdir(cwd)) _exit(126);
        environ = envp;
        execvp(argv[0], argv);
        _exit(127);
    }
    setpgid(pid, pid);
    close(pin[0]); close(pout[1]); close(perr[1]);
    if (input && *input) { const ssize_t w = write(pin[1], input, strlen(input)); (void)w; }
    close(pin[1]);
    fcntl(pout[0], F_SETFL, O_NONBLOCK); fcntl(perr[0], F_SETFL, O_NONBLOCK);
    int o_open = 1, e_open = 1, exited = 0, st = 0;
    const double t0 = cb_now();
    for (;;) {
        struct pollfd p[2]; int np = 0;
        if (o_open) { p[np].fd = pout[0]; p[np].events = POLLIN; np++; }
        if (e_open) { p[np].fd = perr[0]; p[np].events = POLLIN; np++; }
        if (!r->timed_out && tmo > 0 && cb_now() - t0 >= tmo) { kill(-pid, SIGKILL); r->timed_out = 1; }
        poll(p, (nfds_t)np, 100);
        if (o_open) cb_drain(pout[0], &r->out, &o_open);
        if (e_open) cb_drain(perr[0], &r->err, &e_open);
        if (waitpid(pid, &st, WNOHANG) == pid) { exited = 1; break; }
        if (r->timed_out && cb_now() - t0 >= tmo + 5) break;        /* nao deveria acontecer: o grupo foi morto */
    }
    kill(-pid, SIGKILL);
    if (o_open) cb_drain(pout[0], &r->out, &o_open);
    if (e_open) cb_drain(perr[0], &r->err, &e_open);
    if (o_open) close(pout[0]);
    if (e_open) close(perr[0]);
    if (!exited) waitpid(pid, &st, 0);
    r->rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return 0;
}
static void cb_res_free(CbRes *r) { free(r->out.p); free(r->err.p); memset(r, 0, sizeof *r); }

/* ---- preparo da tarefa (setup.py): hospedeiro Python que fica vivo ate o fim ---- */
static const char cb_shim[] =
    "import sys, os, json, importlib.util\n"
    "here, path, name, work = sys.argv[1:5]\n"
    "real = os.fdopen(os.dup(1), 'w')\n"
    "os.dup2(2, 1)\n"
    "sys.path.insert(0, here)\n"
    "ctx = {}\n"
    "try:\n"
    "    spec = importlib.util.spec_from_file_location('cb_setup_' + name.replace('-', '_'), path)\n"
    "    mod = importlib.util.module_from_spec(spec)\n"
    "    spec.loader.exec_module(mod)\n"
    "    ctx = mod.setup(work) or {}\n"
    "except BaseException as e:\n"
    "    real.write(json.dumps({'error': str(e)[:300]}) + '\\n'); real.flush(); sys.exit(1)\n"
    "out = {'vars': {k: str(v) for k, v in (ctx.get('vars') or {}).items()},\n"
    "       'agent_env': {k: str(v) for k, v in (ctx.get('agent_env') or {}).items()}}\n"
    "real.write(json.dumps(out) + '\\n'); real.flush()\n"
    "try:\n"
    "    sys.stdin.read()\n"
    "finally:\n"
    "    if ctx.get('stop'):\n"
    "        ctx['stop']()\n";

static void cb_ctx_free(CbCtx *c) { cb_kv_free(&c->vars); cb_kv_free(&c->agent_env); }

/* 0 ok (inclusive sem setup.py); -1 falhou (msg preenchida) */
static int cb_setup(const CbOpt *o, const char *name, const char *work, CbCtx *c, char *msg, size_t mcap) {
    memset(c, 0, sizeof *c); msg[0] = 0;
    char sp[2400]; snprintf(sp, sizeof sp, "%s/tasks/%s/setup.py", o->dir, name);
    if (!cb_exists(sp)) return 0;
    int pin[2], pout[2];
    if (pipe(pin) || pipe(pout)) { snprintf(msg, mcap, "pipe falhou"); return -1; }
    fflush(stdout); fflush(stderr);
    const pid_t pid = fork();
    if (pid < 0) { snprintf(msg, mcap, "fork falhou"); return -1; }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(pin[0], 0); dup2(pout[1], 1);
        const int dn = open("/dev/null", O_WRONLY); if (dn >= 0) dup2(dn, 2);
        close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]);
        execlp(o->python, o->python, "-c", cb_shim, o->dir, sp, name, work, (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    close(pin[0]); close(pout[1]);
    fcntl(pin[1], F_SETFD, FD_CLOEXEC);
    c->shim = pid; c->wfd = pin[1];
    /* espera a linha de resposta (ate 3 minutos: as tarefas de noticias vao a internet) */
    Buf line = {0}; const double t0 = cb_now(); int got = 0;
    while (!got && cb_now() - t0 < 180) {
        struct pollfd p = { pout[0], POLLIN, 0 };
        if (poll(&p, 1, 200) > 0) {
            char ch; const ssize_t n = read(pout[0], &ch, 1);
            if (n <= 0) break;
            if (ch == '\n') got = 1; else buf_add(&line, &ch, 1);
        }
    }
    close(pout[0]);
    CbObj ob; int ok = 0;
    if (got && line.p && cb_parse_obj(line.p, &ob)) {
        const char *er = cb_str(&ob, "error");
        if (er) snprintf(msg, mcap, "%s", er);
        else ok = 1;
        if (ok) {
            /* vars e agent_env: dois objetos aninhados; leitura simples por chaves conhecidas */
            for (int pass = 0; pass < 2; pass++) {
                const char *key = pass ? "\"agent_env\": {" : "\"vars\": {";
                const char *q = strstr(line.p, key);
                if (!q) continue;
                q += strlen(key) - 1;
                const char *end = cb_skip_nested(q);
                char *sub = strndup(q, (size_t)(end - q));
                CbObj so;
                if (sub && cb_parse_obj(sub, &so)) {
                    for (int i = 0; i < so.n; i++) if (so.v[i].t == 0) cb_kv_set(pass ? &c->agent_env : &c->vars, so.v[i].k, so.v[i].s);
                    cb_obj_free(&so);
                }
                free(sub);
            }
        }
        cb_obj_free(&ob);
    } else snprintf(msg, mcap, "o preparo nao respondeu");
    free(line.p);
    if (!ok) {                                         /* falhou: encerra o hospedeiro (que ja chamou o que pode) */
        close(c->wfd); c->wfd = -1;
        for (int i = 0; i < 100; i++) { if (waitpid(pid, NULL, WNOHANG) == pid) break; usleep(100000); if (i == 90) kill(-pid, SIGKILL); }
        c->shim = 0;
        cb_ctx_free(c);
        return -1;
    }
    return 0;
}

/* fecha o cano: o hospedeiro chama stop() (servidores, temporarios) e sai; espera ate 30 s, depois mata o grupo */
static void cb_finish(CbCtx *c) {
    if (c->home[0]) { cb_rmtree(c->home); c->home[0] = 0; }
    if (c->shim) {
        close(c->wfd);
        int done = 0;
        for (int i = 0; i < 300 && !done; i++) {
            if (waitpid(c->shim, NULL, WNOHANG) == c->shim) done = 1; else usleep(100000);
        }
        if (!done) { kill(-c->shim, SIGKILL); waitpid(c->shim, NULL, 0); }
        else kill(-c->shim, SIGKILL);
        c->shim = 0;
    }
    cb_ctx_free(c);
}

/* ---- solucao e testes escondidos ---- */
static int cb_apply_solution(const CbOpt *o, const char *work, const char *name, CbCtx *c, char *msg, size_t mcap) {
    char sol[2400], scr[2500]; msg[0] = 0;
    snprintf(sol, sizeof sol, "%s/tasks/%s/solution", o->dir, name); snprintf(scr, sizeof scr, "%s/solve.sh", sol);
    if (cb_exists(scr)) {
        char **e = cb_ctx_env(o, c, 0); CbRes r;
        char *av[] = { "bash", scr, NULL };
        cb_exec(av, work, e, NULL, 120, &r);
        cb_env_free(e);
        if (r.rc != 0 || r.timed_out) {
            Buf all = {0}; if (r.out.p) buf_str(&all, r.out.p); if (r.err.p) buf_str(&all, r.err.p);
            const size_t n = all.n, from = n > 400 ? n - 400 : 0;
            snprintf(msg, mcap, "solve.sh falhou: %s", all.p ? all.p + from : "");
            free(all.p); cb_res_free(&r); return -1;
        }
        cb_res_free(&r);
        return 0;
    }
    cb_copy_tree(sol, work);
    return 0;
}

/* copia os testes escondidos e roda; devolve 1 se passou; out recebe o fim da saida */
static int cb_hidden(const CbOpt *o, const char *work, const char *name, CbCtx *c, char *out, size_t cap) {
    char hd[2400]; snprintf(hd, sizeof hd, "%s/tasks/%s/hidden", o->dir, name);
    cb_copy_tree(hd, work);
    char **e = cb_ctx_env(o, c, 0); CbRes r;
    char *av[] = { (char *)o->python, "-m", "unittest", "discover", "-p", "test_hidden*.py", NULL };
    cb_exec(av, work, e, NULL, 120, &r);
    cb_env_free(e);
    int pass = 0;
    if (r.timed_out) snprintf(out, cap, "hidden tests timed out");
    else {
        pass = r.rc == 0;
        Buf all = {0}; if (r.out.p) buf_str(&all, r.out.p); if (r.err.p) buf_str(&all, r.err.p);
        const size_t from = all.n > 600 ? all.n - 600 : 0;
        snprintf(out, cap, "%s", all.p ? all.p + from : "");
        free(all.p);
    }
    cb_res_free(&r);
    return pass;
}

/* ---- verify ---- */
static int cb_verify(const CbOpt *o) {
    char **names; const int n = cb_select(o, &names);
    int bad = 0, skipped = 0;
    for (int i = 0; i < n; i++) {
        char w[1100], w2[1100], pre[300], msg[600], out[1000] = "";
        snprintf(pre, sizeof pre, "cb_%s_", names[i]);
        char src[2400]; snprintf(src, sizeof src, "%s/tasks/%s/repo", o->dir, names[i]);
        if (cb_mkdtemp(pre, w, sizeof w)) { fprintf(stderr, "codebench: sem pasta temporaria\n"); return 1; }
        cb_copy_tree(src, w);
        CbCtx c;
        if (cb_setup(o, names[i], w, &c, msg, sizeof msg)) {
            printf("%-14s skipped (setup failed: %.120s)\n", names[i], msg);
            skipped++; cb_rmtree(w); continue;
        }
        int leaked = 0; char lk[300] = "";
        { static const char *bad_[] = { "solution", "hidden", "solve.sh", "setup.py", "site", NULL };
          for (int k = 0; bad_[k]; k++) { char p[1300]; snprintf(p, sizeof p, "%s/%s", w, bad_[k]); if (cb_exists(p)) { leaked++; if (strlen(lk) + 20 < sizeof lk) { strcat(lk, lk[0] ? ", " : ""); strcat(lk, bad_[k]); } } } }
        const int before = cb_hidden(o, w, names[i], &c, out, sizeof out);
        cb_finish(&c);
        if (cb_mkdtemp(pre, w2, sizeof w2)) { fprintf(stderr, "codebench: sem pasta temporaria\n"); return 1; }
        cb_copy_tree(src, w2);
        int after = 0; out[0] = 0;
        if (cb_setup(o, names[i], w2, &c, msg, sizeof msg)) { snprintf(out, sizeof out, "setup falhou na segunda copia: %s", msg); }
        else {
            if (cb_apply_solution(o, w2, names[i], &c, msg, sizeof msg)) snprintf(out, sizeof out, "%s", msg);
            else after = cb_hidden(o, w2, names[i], &c, out, sizeof out);
            cb_finish(&c);
        }
        char extra[1400] = "";
        if (leaked) snprintf(extra, sizeof extra, "solution/hidden material visible to the agent: [%s]\n%s", lk, out);
        else snprintf(extra, sizeof extra, "%s", out);
        const int ok = !before && after && !leaked;
        printf("%-14s before=%s after=%s %s\n", names[i], before ? "pass" : "fail", after ? "pass" : "fail", ok ? "ok" : "BROKEN TASK");
        if (!ok) printf("%s\n", extra);
        bad += !ok;
        cb_rmtree(w); cb_rmtree(w2);
    }
    printf("%d tasks, %d broken", n, bad);
    if (skipped) printf(", %d skipped (setup needs something this machine lacks, e.g. the internet)", skipped);
    printf("\n");
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
    return bad ? 1 : 0;
}

/* ---- medidas do registro do proprio agente (KATANA_LOG) e da saida de erro ---- */
typedef struct { int calls, failed, refused, repeated, rounds, last_text; long tok_in, tok_out; } CbMetrics;

static int cb_log_is_rec(const char *p) {
    for (int i = 0; i < 19; i++) {
        const char c = p[i];
        if (i == 4 || i == 7) { if (c != '-') return 0; }
        else if (i == 10) { if (c != ' ') return 0; }
        else if (i == 13 || i == 16) { if (c != ':') return 0; }
        else if (c < '0' || c > '9') return 0;
    }
    return p[19] == '\t';
}
static int cb_log_failed(const char *r) {
    while (*r == ' ') r++;
    if (!strncasecmp(r, "error", 5) || !strncmp(r, "Traceback", 9)) return 1;
    for (const char *e = strstr(r, "xit code "); e; e = strstr(e + 1, "xit code ")) if (e[9] >= '1' && e[9] <= '9') return 1;
    return 0;
}

/* registro: uma linha por chamada executada: data, ferramenta, argumentos, resultado (tabulacao). Repetida: mesma
 * ferramenta e mesmos argumentos entre as 20 chamadas anteriores (a mesma regra do katana agentlog). */
static void cb_read_log(const char *path, CbMetrics *m) {
    char *buf = cb_slurp(path, NULL);
    if (!buf) return;
    uint64_t last[20] = {0}; int nl20 = 0;
    for (char *p = buf; *p; ) {
        char *nl = strchr(p, '\n'); if (nl) *nl = 0;
        if (cb_log_is_rec(p)) {
            char *t1 = p + 19, *t2 = strchr(t1 + 1, '\t');
            if (t2) {
                char *t3 = strchr(t2 + 1, '\t');
                const char *res = t3 ? t3 + 1 : "";
                uint64_t h = 1469598103934665603ULL;
                for (const char *q = t1 + 1; q < t2; q++) { h ^= (unsigned char)*q; h *= 1099511628211ULL; }
                h ^= 9; h *= 1099511628211ULL;
                for (const char *q = t2 + 1; *q && q != t3; q++) { h ^= (unsigned char)*q; h *= 1099511628211ULL; }
                int rep = 0;
                for (int k = 0; k < nl20 && k < 20; k++) if (last[k] == h) { rep = 1; break; }
                last[nl20 % 20] = h; nl20++;
                m->calls++; m->failed += cb_log_failed(res); m->repeated += rep;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    free(buf);
}

/* a saida de erro do agente: uma linha de estatisticas por rodada ("[entrada: N tokens ... | resposta: M tokens ...]")
 * e uma linha por chamada ("[ferramenta nome args]", seguida de "[ferramenta nome: N bytes de resultado]"). Chamadas
 * recusadas pelo motor (a mesma chamada que ja falhou duas vezes, chamada cortada) nao chegam ao registro: recusadas =
 * chamadas iniciadas menos chamadas registradas. Ultimo turno em texto: nenhuma chamada depois da ultima rodada. */
static void cb_read_stderr(const char *err, int teacher, CbMetrics *m) {
    int started = 0, after_last = 0;
    for (const char *p = err; p && *p; ) {
        const char *nl = strchr(p, '\n'); const size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[2048]; snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line - 1 ? len : sizeof line - 1), p);
        const char *e = strstr(line, "[entrada: ");
        if (e && !teacher) {
            const char *r = strstr(e, "resposta: ");
            m->rounds++; m->tok_in += atol(e + 10); if (r) m->tok_out += atol(r + 10);
            after_last = 0;
        } else {
            const char *f = strstr(line, teacher ? "[professor: " : "[ferramenta ");
            if (f) {
                const size_t l2 = strlen(line);
                const int is_result = !teacher && l2 > 22 && strstr(line, " bytes de resultado]") != NULL;
                if (!is_result) { started++; after_last = 1; }
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    m->refused = started > m->calls ? started - m->calls : 0;
    m->last_text = m->rounds > 0 && !after_last;
}

static int cb_count(const char *s, const char *pat) { int n = 0; for (const char *p = strstr(s, pat); p; p = strstr(p + strlen(pat), pat)) n++; return n; }

/* ---- uma tarefa ---- */
static void cb_collapse_ws(const char *in, Buf *out) {          /* " ".join(texto.split()) */
    int pend = 0, any = 0;
    for (const char *p = in; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') { pend = 1; continue; }
        if (pend && any) buf_str(out, " ");
        pend = 0; any = 1; buf_add(out, p, 1);
    }
}

static void cb_skip_record(Buf *rec, const char *label, const char *name) {
    buf_str(rec, "{\"label\": "); cb_json_str(rec, label);
    buf_str(rec, ", \"task\": "); cb_json_str(rec, name);
    buf_str(rec, ", \"passed\": false, \"seconds\": 0.0, \"tool_calls\": 0, \"gen_tokens\": 0, \"timed_out\": false, \"skipped\": true}");
}

/* roda a tarefa; rec recebe a linha JSON; devolve 1 se passou, e preenche secs/tools/gen/to para a linha de progresso */
static int cb_run_task(const CbOpt *o, const char *name, int attempt, Buf *rec, double *psecs, int *ptools, int *pgen, int *pto) {
    char work[1100], msg[600];
    char src[2400]; snprintf(src, sizeof src, "%s/tasks/%s/repo", o->dir, name);
    char pre[300]; snprintf(pre, sizeof pre, "cb_%s_", name);
    if (cb_mkdtemp(pre, work, sizeof work)) { fprintf(stderr, "codebench: sem pasta temporaria\n"); return 0; }
    cb_copy_tree(src, work);
    CbCtx c;
    if (cb_setup(o, name, work, &c, msg, sizeof msg)) {            /* ex.: tarefa de noticias sem internet: conta como nao rodada */
        cb_rmtree(work);
        fprintf(stderr, "  %s: preparo falhou (%.120s)\n", name, msg);
        cb_skip_record(rec, o->label, name);
        *psecs = 0; *ptools = 0; *pgen = 0; *pto = 0;
        return 0;
    }
    if (!o->teacher && cb_mkdtemp("cb_home_", c.home, sizeof c.home) == 0) {      /* casa propria, so com o ajuste de desempenho da maquina */
        const char *rh = getenv("HOME");
        char a[1300], b[1300]; struct stat st;
        snprintf(b, sizeof b, "%s/.katana", c.home); mkdir(b, 0700);
        snprintf(a, sizeof a, "%s/.katana/katana.cfg", rh ? rh : ""); snprintf(b, sizeof b, "%s/.katana/katana.cfg", c.home);
        if (stat(a, &st) == 0) cb_copy_file(a, b, &st);
    } else c.home[0] = 0;
    char ip[2400]; snprintf(ip, sizeof ip, "%s/tasks/%s/instruction.md", o->dir, name);
    char *raw = cb_slurp(ip, NULL);
    Buf instr = {0}; cb_collapse_ws(raw ? raw : "", &instr); free(raw);
    for (int i = 0; i < c.vars.n; i++) {                           /* {chave} no enunciado */
        char key[300]; snprintf(key, sizeof key, "{%s}", c.vars.kv[i].k);
        Buf nb = {0}; const char *p = instr.p ? instr.p : "";
        for (const char *q; (q = strstr(p, key)); p = q + strlen(key)) { buf_add(&nb, p, (size_t)(q - p)); buf_str(&nb, c.vars.kv[i].v); }
        buf_str(&nb, p); free(instr.p); instr = nb;
    }
    Buf mb = {0};
    buf_str(&mb, "You are working in the current folder, a small project (python3 with the standard library and unittest, "
                 "and the usual shell tools; there is no pytest). Task: ");
    buf_str(&mb, instr.p ? instr.p : "");
    buf_str(&mb, " Check your work by running it. When it is finished, answer with one short sentence.");
    free(instr.p);

    char **env = cb_ctx_env(o, &c, 1);
    for (int i = 0; i < o->nenv; i++) cb_env_set_kv(&env, o->envs[i]);
    char logdir[1100], logp[1200] = "";
    if (cb_mkdtemp("cb_log_", logdir, sizeof logdir) == 0) { snprintf(logp, sizeof logp, "%s/agent.log", logdir); cb_env_set(&env, "KATANA_LOG", logp); }

    char *av[40]; int na = 0; char dtmp[1200] = "", ddir[1100] = "", sess[2400] = "";
    if (o->sessions) {
        mkdir(o->sessions, 0777);
        char abs[4200]; if (!realpath(o->sessions, abs)) snprintf(abs, sizeof abs, "%s", o->sessions);
        snprintf(sess, sizeof sess, "%s/%s.%d.ksess", abs, name, attempt);
        unlink(sess);
    }
    if (o->teacher) {                                              /* o professor resolve com as ferramentas do katana (katana distill) */
        cb_mkdtemp("cbd_", ddir, sizeof ddir); snprintf(dtmp, sizeof dtmp, "%s/s.jsonl", ddir);
        av[na++] = (char *)o->katana; av[na++] = "distill"; av[na++] = "--url"; av[na++] = (char *)o->teacher;
        av[na++] = "--model"; av[na++] = (char *)o->tmodel; av[na++] = "--key-env"; av[na++] = (char *)o->tkeyenv;
        av[na++] = "--format"; av[na++] = (char *)o->tformat; av[na++] = "--out"; av[na++] = dtmp;
        av[na++] = "--task"; av[na++] = (char *)name; av[na++] = mb.p;
    } else { av[na++] = (char *)o->katana; av[na++] = (char *)o->mode; av[na++] = (char *)o->model; }
    if (o->sessions) { av[na++] = "--sessao"; av[na++] = sess; }
    av[na] = NULL;
    CbRes r;
    const double t0 = cb_now();
    Buf stdin_b = {0};
    if (!o->teacher) { buf_str(&stdin_b, mb.p); buf_str(&stdin_b, "\n/exit\n"); }
    if (cb_exec(av, work, env, stdin_b.p, o->timeout, &r)) memset(&r, 0, sizeof r);
    free(stdin_b.p);
    const double secs = cb_now() - t0;
    cb_env_free(env);

    char hout[1000];
    const int passed = cb_hidden(o, work, name, &c, hout, sizeof hout);
    cb_finish(&c);

    Buf log = {0};
    if (r.out.p) buf_str(&log, r.out.p);
    buf_str(&log, "\n");
    if (r.err.p) buf_str(&log, r.err.p);
    long gen = 0;
    for (const char *p = strstr(log.p, "resposta: "); p; p = strstr(p + 10, "resposta: ")) {
        char *e; const long v = strtol(p + 10, &e, 10);
        if (e != p + 10 && !strncmp(e, " tokens", 7)) gen += v;
    }
    CbMetrics met; memset(&met, 0, sizeof met);
    if (logp[0]) cb_read_log(logp, &met);
    cb_read_stderr(r.err.p ? r.err.p : "", o->teacher != NULL, &met);
    /* o registro do agente tem uma linha por chamada; o texto da tela repetia cada uma (chamada e resultado) */
    const int tool_calls = met.calls + met.refused ? met.calls + met.refused : cb_count(log.p, "[ferramenta ") + cb_count(log.p, "[professor: ");

    if (o->teacher && dtmp[0]) {                                   /* sessao do professor: marca passed e guarda */
        size_t dl = 0; char *d = cb_slurp(dtmp, &dl);
        if (d && dl) {
            while (dl && (d[dl - 1] == '\n' || d[dl - 1] == '\r' || d[dl - 1] == ' ')) d[--dl] = 0;
            char *ls = strrchr(d, '\n'); ls = ls ? ls + 1 : d;
            const size_t ll = strlen(ls);
            if (ll > 1 && ls[ll - 1] == '}') {
                FILE *f = fopen(o->distill_out, "ab");
                if (f) {
                    fprintf(f, "%.*s, \"passed\": %s, \"attempt\": %d, \"timed_out\": %s}\n", (int)(ll - 1), ls, passed ? "true" : "false", attempt, r.timed_out ? "true" : "false");
                    fclose(f);
                }
            }
        }
        free(d);
    }
    if (ddir[0]) cb_rmtree(ddir);

    char num[64];
    buf_str(rec, "{\"label\": "); cb_json_str(rec, o->label);
    buf_str(rec, ", \"task\": "); cb_json_str(rec, name);
    snprintf(num, sizeof num, ", \"passed\": %s", passed ? "true" : "false"); buf_str(rec, num);
    snprintf(num, sizeof num, ", \"seconds\": %.1f", secs); buf_str(rec, num);
    snprintf(num, sizeof num, ", \"tool_calls\": %d", tool_calls); buf_str(rec, num);
    snprintf(num, sizeof num, ", \"gen_tokens\": %ld", gen); buf_str(rec, num);
    snprintf(num, sizeof num, ", \"timed_out\": %s", r.timed_out ? "true" : "false"); buf_str(rec, num);
    if (o->repeat > 1) { snprintf(num, sizeof num, ", \"attempt\": %d", attempt); buf_str(rec, num); }
    if (o->sessions) {
        buf_str(rec, ", \"session\": ");
        if (cb_exists(sess)) cb_json_str(rec, sess); else buf_str(rec, "null");
        buf_str(rec, ", \"instruction\": "); cb_json_str(rec, mb.p);
    }
    {   char m2[400];
        snprintf(m2, sizeof m2, ", \"calls\": %d, \"failed_calls\": %d, \"refused_calls\": %d, \"repeated_calls\": %d, \"rounds\": %d, \"last_text\": %s, "
                                "\"tokens_read\": %ld, \"tokens_written\": %ld}", met.calls, met.failed, met.refused, met.repeated, met.rounds,
                 met.last_text ? "true" : "false", met.tok_in, met.tok_out);
        buf_str(rec, m2); }

    if (o->keep) {
        mkdir(o->keep, 0777);
        char kp[2400]; if (o->repeat > 1) snprintf(kp, sizeof kp, "%s/%s.%d.log", o->keep, name, attempt); else snprintf(kp, sizeof kp, "%s/%s.log", o->keep, name);
        FILE *f = fopen(kp, "wb"); if (f) { fwrite(log.p, 1, log.n, f); fclose(f); }
        if (o->repeat > 1) snprintf(kp, sizeof kp, "%s/%s.%d.agent.log", o->keep, name, attempt); else snprintf(kp, sizeof kp, "%s/%s.agent.log", o->keep, name);
        if (logp[0] && cb_exists(logp)) { struct stat st; if (!stat(logp, &st)) cb_copy_file(logp, kp, &st); }
        snprintf(kp, sizeof kp, "%s/%s", o->keep, name);
        cb_copy_tree(work, kp);
    }
    *psecs = secs; *ptools = tool_calls; *pgen = (int)gen; *pto = r.timed_out;
    cb_res_free(&r); free(log.p); free(mb.p);
    if (logdir[0] && cb_isdir(logdir)) cb_rmtree(logdir);
    cb_rmtree(work);
    return passed;
}

/* ---- resultados: leitura das linhas ---- */
typedef struct { CbObj o; char *line; } CbRec;
typedef struct { CbRec *r; int n; } CbRecs;

static void cb_load_all(const char *path, CbRecs *rs) {
    rs->r = NULL; rs->n = 0;
    FILE *f = fopen(path, "rb"); if (!f) return;
    char *line = NULL; size_t cap = 0; ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        const char *p = cb_skip_ws(line);
        if (!*p) continue;
        CbRec rc; memset(&rc, 0, sizeof rc);
        if (!cb_parse_obj(line, &rc.o)) continue;
        rs->r = realloc(rs->r, sizeof(CbRec) * (size_t)(rs->n + 1)); rs->r[rs->n++] = rc;
    }
    free(line); fclose(f);
}
static void cb_free_recs(CbRecs *rs) { for (int i = 0; i < rs->n; i++) cb_obj_free(&rs->r[i].o); free(rs->r); rs->r = NULL; rs->n = 0; }

/* ---- compare ---- */
static double cb_sign_test(int wins, int losses) {
    const int n = wins + losses;
    if (n == 0) return 1.0;
    const int k = wins < losses ? wins : losses;
    double term = 1.0, sum = 0.0;
    for (int i = 0; i <= k; i++) { sum += term; term = term * (n - i) / (i + 1); }
    const double p = 2 * sum / ldexp(1.0, n);
    return p > 1.0 ? 1.0 : p;
}

static const CbObj *cb_last_for(const CbRecs *rs, const char *task) {       /* a ultima linha da tarefa (como o dict do script) */
    const CbObj *res = NULL;
    for (int i = 0; i < rs->n; i++) { const char *t = cb_str(&rs->r[i].o, "task"); if (t && !strcmp(t, task)) res = &rs->r[i].o; }
    return res;
}

static void cb_print_list(const char **names, int n) {
    printf("[");
    for (int i = 0; i < n; i++) printf("%s'%s'", i ? ", " : "", names[i]);
    printf("]");
}

static int cb_compare(const char *pa, const char *pb) {
    CbRecs A, B; cb_load_all(pa, &A); cb_load_all(pb, &B);
    if (!A.n || !B.n) { fprintf(stderr, "compare: arquivo vazio ou ilegivel (%s: %d linhas, %s: %d linhas)\n", pa, A.n, pb, B.n); cb_free_recs(&A); cb_free_recs(&B); return 1; }
    const char **common = xalloc((size_t)A.n + 1, sizeof(char *)); int nc = 0;
    for (int i = 0; i < A.n; i++) {                                 /* tarefas das duas, em ordem alfabetica, sem repetir */
        const char *t = cb_str(&A.r[i].o, "task");
        if (!t || !cb_last_for(&B, t)) continue;
        int dup = 0; for (int k = 0; k < nc; k++) if (!strcmp(common[k], t)) dup = 1;
        if (!dup) common[nc++] = t;
    }
    qsort(common, (size_t)nc, sizeof(char *), cb_cmp_str);
    const char **wins = xalloc((size_t)nc + 1, sizeof(char *)), **loss = xalloc((size_t)nc + 1, sizeof(char *)); int nw = 0, nl = 0, sa = 0, sb = 0;
    for (int i = 0; i < nc; i++) {
        const int pa_ = cb_num(cb_last_for(&A, common[i]), "passed", 0) != 0, pb_ = cb_num(cb_last_for(&B, common[i]), "passed", 0) != 0;
        sa += pa_; sb += pb_;
        if (pb_ && !pa_) wins[nw++] = common[i];
        if (pa_ && !pb_) loss[nl++] = common[i];
    }
    printf("tasks in common: %d\n", nc);
    printf("A (%s): %d/%d solved   B (%s): %d/%d solved\n", pa, sa, nc, pb, sb, nc);
    printf("newly solved by B: %d ", nw); cb_print_list(wins, nw); printf("\n");
    printf("lost by B:         %d ", nl); cb_print_list(loss, nl); printf("\n");
    printf("exact sign test p = %.4f (few tasks: treat small differences as noise)\n", cb_sign_test(nw, nl));
    static const char *basic[] = { "seconds", "tool_calls", "gen_tokens", NULL };
    for (int k = 0; basic[k]; k++) {
        double ma = 0, mb = 0;
        for (int i = 0; i < nc; i++) { ma += cb_num(cb_last_for(&A, common[i]), basic[k], 0); mb += cb_num(cb_last_for(&B, common[i]), basic[k], 0); }
        const int d = nc > 0 ? nc : 1;
        printf("mean %-11s A %.1f   B %.1f\n", basic[k], ma / d, mb / d);
    }
    /* medidas lidas do registro do agente (so nas linhas que as trazem, as de runs antigos ficam de fora) */
    static const char *extra[] = { "calls", "failed_calls", "refused_calls", "repeated_calls", "rounds", "tokens_read", "tokens_written", NULL };
    printf("from the agent log (runs that recorded it):\n");
    double tot_calls[2] = {0, 0}, tot_failed[2] = {0, 0};
    for (int k = 0; extra[k]; k++) {
        double sum[2] = {0, 0}; int cnt[2] = {0, 0};
        for (int s = 0; s < 2; s++) for (int i = 0; i < nc; i++) {
            const CbObj *o = cb_last_for(s ? &B : &A, common[i]);
            if (cb_get(o, extra[k])) { sum[s] += cb_num(o, extra[k], 0); cnt[s]++; }
        }
        if (!k) { tot_calls[0] = sum[0]; tot_calls[1] = sum[1]; }
        if (k == 1) { tot_failed[0] = sum[0]; tot_failed[1] = sum[1]; }
        char a[32], b[32];
        if (cnt[0]) snprintf(a, sizeof a, "%.1f", sum[0] / cnt[0]); else snprintf(a, sizeof a, "-");
        if (cnt[1]) snprintf(b, sizeof b, "%.1f", sum[1] / cnt[1]); else snprintf(b, sizeof b, "-");
        printf("mean %-15s A %s   B %s\n", extra[k], a, b);
    }
    {   char a[32], b[32];
        if (tot_calls[0] > 0) snprintf(a, sizeof a, "%.1f%%", 100 * tot_failed[0] / tot_calls[0]); else snprintf(a, sizeof a, "-");
        if (tot_calls[1] > 0) snprintf(b, sizeof b, "%.1f%%", 100 * tot_failed[1] / tot_calls[1]); else snprintf(b, sizeof b, "-");
        printf("failed calls / calls   A %s   B %s\n", a, b); }
    {   int lt[2] = {0, 0}, cnt[2] = {0, 0};
        for (int s = 0; s < 2; s++) for (int i = 0; i < nc; i++) { const CbObj *o = cb_last_for(s ? &B : &A, common[i]); if (cb_get(o, "last_text")) { cnt[s]++; lt[s] += cb_num(o, "last_text", 0) != 0; } }
        printf("last turn was text     A %d/%d   B %d/%d\n", lt[0], cnt[0], lt[1], cnt[1]); }
    free(common); free(wins); free(loss); cb_free_recs(&A); cb_free_recs(&B);
    return 0;
}

/* ---- list ---- */
static int cb_list(const CbOpt *o) {
    char **names; const int n = cb_select(o, &names);
    for (int i = 0; i < n; i++) {
        char p[2400], first[80] = "";
        snprintf(p, sizeof p, "%s/tasks/%s/setup.py", o->dir, names[i]); const int su = cb_exists(p);
        snprintf(p, sizeof p, "%s/tasks/%s/solution/solve.sh", o->dir, names[i]); const int sv = cb_exists(p);
        snprintf(p, sizeof p, "%s/tasks/%s/instruction.md", o->dir, names[i]);
        char *t = cb_slurp(p, NULL);
        if (t) { Buf b = {0}; cb_collapse_ws(t, &b); snprintf(first, sizeof first, "%.70s", b.p ? b.p : ""); free(b.p); free(t); }
        printf("%-20s %s%s  %s\n", names[i], su ? "setup " : "      ", sv ? "solve.sh" : "        ", first);
        free(names[i]);
    }
    printf("%d tasks\n", n);
    free(names);
    return 0;
}

/* ---- run ---- */
static int cb_run(CbOpt *o) {
    char **names; const int n = cb_select(o, &names);
    if (!n) { fprintf(stderr, "codebench: nenhuma tarefa (pasta %s/tasks, filtro %s)\n", o->dir, o->tasks ? o->tasks : "-"); return 1; }
    /* execucao interrompida continua de onde parou */
    CbRecs done; cb_load_all(o->out, &done);
    for (int k = 0; k < o->repeat; k++) for (int i = 0; i < n; i++) {
        int skip = 0;
        for (int j = 0; j < done.n; j++) {
            const char *t = cb_str(&done.r[j].o, "task");
            if (t && !strcmp(t, names[i]) && (int)cb_num(&done.r[j].o, "attempt", 0) == k) { skip = 1; break; }
        }
        if (skip) continue;
        Buf rec = {0}; double secs = 0; int tools = 0, gen = 0, to = 0;
        const int passed = cb_run_task(o, names[i], k, &rec, &secs, &tools, &gen, &to);
        FILE *f = fopen(o->out, "ab");
        if (f) { fwrite(rec.p, 1, rec.n, f); fputc('\n', f); fclose(f); }
        else fprintf(stderr, "codebench: nao gravou %s\n", o->out);
        free(rec.p);
        char att[40] = ""; if (o->repeat > 1) snprintf(att, sizeof att, "  attempt %d", k);
        printf("%-14s %s  %5.0fs  tools=%d  gen=%d%s%s\n", names[i], passed ? "PASS" : "fail", secs, tools, gen, to ? "  TIMEOUT" : "", att);
        fflush(stdout);
    }
    cb_free_recs(&done);
    CbRecs all; cb_load_all(o->out, &all);
    if (o->repeat > 1) {
        int ps = 0; for (int i = 0; i < all.n; i++) ps += cb_num(&all.r[i].o, "passed", 0) != 0;
        printf("solved %d of %d runs (%d tasks x %d attempts)\n", ps, all.n, n, o->repeat);
    } else {                                                        /* uma linha por tarefa (a ultima) */
        int nt = 0, ps = 0;
        for (int i = 0; i < all.n; i++) {
            const char *t = cb_str(&all.r[i].o, "task"); int later = 0;
            for (int j = i + 1; j < all.n && !later; j++) { const char *u = cb_str(&all.r[j].o, "task"); if (t && u && !strcmp(t, u)) later = 1; }
            if (t && !later) { nt++; ps += cb_num(&all.r[i].o, "passed", 0) != 0; }
        }
        printf("solved %d/%d\n", ps, nt);
    }
    cb_free_recs(&all);
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
    return 0;
}

static void cb_self(char *out, size_t cap) {
    const ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n > 0) out[n] = 0; else out[0] = 0;
}

static void cb_abspath(const char *p, char *out, size_t cap) {
    char rp[4200];                                                 /* realpath exige PATH_MAX */
    if (realpath(p, rp)) { snprintf(out, cap, "%s", rp); return; }
    if (p[0] == '/') { snprintf(out, cap, "%s", p); return; }
    char cwd[1024]; if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    snprintf(out, cap, "%s/%s", cwd, p);
}

static void cb_usage(void) {
    fprintf(stderr,
        "uso: katana codebench run --model M.gguf --out r.jsonl [--tasks 'a,b-*'] [--repeat N] [--timeout S] [--label L] [--katana BIN]\n"
        "                         [--env K=V]... [--sessions DIR] [--keep DIR] [--mode agent|code] [--dir PASTA] [--python python3]\n"
        "                         [--teacher URL --teacher-model M --distill-out f.jsonl [--teacher-key-env VAR] [--teacher-format qwen|json|gemma4|glm]]\n"
        "     katana codebench verify [--tasks ...]      cada tarefa falha intacta e passa com a solucao (sem modelo)\n"
        "     katana codebench compare A.jsonl B.jsonl   ganhas, perdidas, teste do sinal, custo medio e medidas do registro do agente\n"
        "     katana codebench list [--tasks ...]\n");
}

static int codebench_main(int argc, char **argv) {
    if (argc < 1) { cb_usage(); return 1; }
    const char *sub = argv[0];
    if (!strcmp(sub, "compare")) {
        if (argc != 3) { cb_usage(); return 1; }
        return cb_compare(argv[1], argv[2]);
    }
    if (strcmp(sub, "run") && strcmp(sub, "verify") && strcmp(sub, "list")) { cb_usage(); return 1; }
    signal(SIGPIPE, SIG_IGN);
    CbOpt o; memset(&o, 0, sizeof o);
    o.timeout = 900; o.repeat = 1; o.label = "run"; o.mode = "agent"; o.python = "python3"; o.tkeyenv = "TEACHER_API_KEY"; o.tformat = "qwen";
    char self[2048]; cb_self(self, sizeof self);
    static char katana_abs[2500], model_abs[2500], dir_buf[2500];
    for (int i = 1; i < argc; i++) {
        const char *k = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v) { fprintf(stderr, "codebench: falta o valor de %s\n", k); return 1; }
        i++;
        if (!strcmp(k, "--model")) o.model = v;
        else if (!strcmp(k, "--out")) o.out = v;
        else if (!strcmp(k, "--tasks")) o.tasks = v;
        else if (!strcmp(k, "--timeout")) o.timeout = atoi(v);
        else if (!strcmp(k, "--label")) o.label = v;
        else if (!strcmp(k, "--katana")) o.katana = v;
        else if (!strcmp(k, "--mode")) { if (strcmp(v, "agent")) { fprintf(stderr, "codebench: --mode e agent\n"); return 1; } o.mode = v; }
        else if (!strcmp(k, "--env")) { if (o.nenv < 64) o.envs[o.nenv++] = (char *)v; }
        else if (!strcmp(k, "--keep")) o.keep = v;
        else if (!strcmp(k, "--sessions")) o.sessions = v;
        else if (!strcmp(k, "--repeat")) o.repeat = atoi(v);
        else if (!strcmp(k, "--teacher")) o.teacher = v;
        else if (!strcmp(k, "--teacher-model")) o.tmodel = v;
        else if (!strcmp(k, "--teacher-key-env")) o.tkeyenv = v;
        else if (!strcmp(k, "--teacher-format")) o.tformat = v;
        else if (!strcmp(k, "--distill-out")) o.distill_out = v;
        else if (!strcmp(k, "--dir")) o.dir = v;
        else if (!strcmp(k, "--python")) o.python = v;
        else { fprintf(stderr, "codebench: opcao desconhecida %s\n", k); return 1; }
    }
    if (o.repeat < 1) { fprintf(stderr, "codebench: --repeat precisa ser >= 1\n"); return 1; }
    if (!o.dir) {                                                  /* tools/codebench na pasta atual, ou ao lado do binario */
        char tdir[2400];
        snprintf(tdir, sizeof tdir, "tools/codebench/tasks");
        if (cb_isdir(tdir)) o.dir = "tools/codebench";
        else {
            char *sl = strrchr(self, '/');
            if (sl) { *sl = 0; snprintf(dir_buf, sizeof dir_buf, "%s/tools/codebench", self); *sl = '/'; if (cb_isdir(dir_buf)) o.dir = dir_buf; }
        }
        if (!o.dir) { fprintf(stderr, "codebench: nao achei tools/codebench (use --dir)\n"); return 1; }
    }
    { char t[2400]; cb_abspath(o.dir, t, sizeof t); snprintf(dir_buf, sizeof dir_buf, "%s", t); o.dir = dir_buf; }
    if (!strcmp(sub, "list")) return cb_list(&o);
    if (!strcmp(sub, "verify")) return cb_verify(&o);
    /* run */
    if (o.teacher) {
        if (!o.tmodel || !o.distill_out || !o.out) { fprintf(stderr, "codebench: --teacher precisa de --teacher-model, --distill-out e --out\n"); return 1; }
        if (!o.model) o.model = "teacher";
    }
    if (!o.model || !o.out) { fprintf(stderr, "codebench: --model e --out sao obrigatorios\n"); return 1; }
    cb_abspath(o.katana ? o.katana : self, katana_abs, sizeof katana_abs); o.katana = katana_abs;
    if (strcmp(o.model, "teacher") || !o.teacher) { cb_abspath(o.model, model_abs, sizeof model_abs); o.model = model_abs; }
    return cb_run(&o);
}
#else
static int codebench_main(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stderr, "codebench: ainda nao disponivel no Windows (grupos de processos e copia de pastas so em POSIX)\n");
    return 1;
}
#endif
