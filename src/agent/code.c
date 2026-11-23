/* ---------- camada de codigo: verificacao de sintaxe depois de gravar ----------
 * Depois de Write e Edit o motor confere a sintaxe do arquivo (python, shell, json, javascript, php, ruby) e junta o
 * resultado ao retorno da ferramenta: o modelo ve o erro na hora, sem gastar uma rodada para descobrir. */
static int g_in_sub;            /* 1 dentro de um subagente (delegate.c) */

/* confere a sintaxe do arquivo gravado e junta o resultado em out. partial: gravacao com append (podem faltar partes).
   Sem a ferramenta da linguagem instalada, ou para outras extensoes, nao acrescenta nada. */
static void code_check(const char *path, int partial, char *out, size_t cap) {
#ifndef _WIN32
    const char *base = strrchr(path, '/'), *ext = strrchr(path, '.');
    if (!ext || (base && ext < base)) return;
    static char q[4400], cmd[9400];
    if (!shq(path, q, sizeof q)) return;
    if (strcmp(ext, ".py") == 0)
        snprintf(cmd, sizeof cmd, "timeout 10 python3 -c 'import ast,sys\ntry:\n    ast.parse(open(sys.argv[1],\"rb\").read(), sys.argv[1])\n"
                 "except SyntaxError as e:\n    print(\"line %%s: %%s\\n%%s\" %% (e.lineno, e.msg, (e.text or \"\").rstrip())); sys.exit(1)\n"
                 "except Exception:\n    pass' %s 2>&1", q);
    else if (strcmp(ext, ".sh") == 0 || strcmp(ext, ".bash") == 0) snprintf(cmd, sizeof cmd, "timeout 10 bash -n %s 2>&1", q);
    else if (strcmp(ext, ".json") == 0)
        snprintf(cmd, sizeof cmd, "timeout 10 python3 -c 'import json,sys\ntry:\n    json.load(open(sys.argv[1]))\n"
                 "except ValueError as e:\n    print(e); sys.exit(1)\nexcept Exception:\n    pass' %s 2>&1", q);
    else if (strcmp(ext, ".js") == 0 || strcmp(ext, ".mjs") == 0 || strcmp(ext, ".cjs") == 0) snprintf(cmd, sizeof cmd, "timeout 10 node --check %s 2>&1", q);
    else if (strcmp(ext, ".php") == 0) snprintf(cmd, sizeof cmd, "timeout 10 php -l %s 2>&1", q);
    else if (strcmp(ext, ".rb") == 0) snprintf(cmd, sizeof cmd, "timeout 10 ruby -c %s 2>&1", q);
    else return;
    int rc = 0;
    size_t n = 0;
    char *o = run_capture(cmd, 4000, &rc, &n);
    if (!o) return;
    const size_t l = strlen(out);
    if (rc == 0) snprintf(out + l, cap - l, "\n[syntax check: ok]");
    else if (rc < 124) {
        const char *t = tail_lines(o, n, 8, 700);
        snprintf(out + l, cap - l, "\n[syntax check FAILED%s]\n%.*s", partial ? "; if more parts are still to be appended, finish them, otherwise fix this now"
                 : ": fix this now with an edit of the file before doing anything else", (int)(n - (size_t)(t - o) > 700 ? 700 : n - (size_t)(t - o)), t);
    }
    free(o);
#else
    (void)path; (void)partial; (void)out; (void)cap;
#endif
}
