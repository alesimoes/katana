/* ---------- modo plano ----------
 * /plan liga: o agente so le e investiga (o motor recusa gravar, editar, instalar, baixar, terminal, ferramentas MCP e
 * qualquer comando que nao seja reconhecidamente de leitura) e termina com um plano. /go aprova e manda executar;
 * /plan off sai sem executar. A trava fica no motor, nao depende de o modelo obedecer. */
static int g_plan_mode;
#define PLAN_NOTE "[Plan mode is ON: you may only read and investigate; the engine refuses anything that changes files or the system. " \
                  "Investigate what you need, then answer with a short numbered plan of what you would do, and stop. The user will approve it.]\n"
#define PLAN_DENY "error: plan mode is on (read-only), so this was NOT done. Keep investigating with read-only tools, then present your numbered plan and stop; the user approves it."

static void plan_ui(void) { if (getenv("KATANA_UI")) { printf("\x01" "MODE %s\n", g_plan_mode ? "plan" : "normal"); fflush(stdout); } }

static int plan_word_in(const char *w, size_t l, const char *const *list) {
    for (int i = 0; list[i]; i++) if (strlen(list[i]) == l && memcmp(list[i], w, l) == 0) return 1;
    return 0;
}
/* 1 se o comando do shell e so leitura: cada parte (separada por | && || ;) comeca por um programa da lista, sem
   redirecionar para arquivo nem substituicao de comando */
static int plan_cmd_ok(const char *cmd) {
    static const char *const ro[] = {"ls", "cat", "head", "tail", "wc", "grep", "egrep", "fgrep", "rg", "find", "file", "stat", "tree", "pwd", "which", "whereis",
        "echo", "printf", "date", "du", "df", "sort", "uniq", "cut", "tr", "diff", "cmp", "nl", "tac", "basename", "dirname", "realpath", "readlink", "env",
        "printenv", "uname", "whoami", "id", "ps", "free", "lscpu", "nproc", "awk", "jq", "xxd", "hexdump", "od", "strings", "md5sum", "sha256sum", "column",
        "test", "[", "true", "cd", "type", "hostname", "uptime", "lsblk", "ip", "ss", "git", "sed", NULL};
    static const char *const gitro[] = {"status", "log", "diff", "show", "branch", "blame", "ls-files", "grep", "remote", "describe", "rev-parse", "shortlog", "tag", NULL};
    if (strchr(cmd, '`') || strstr(cmd, "$(") || strstr(cmd, "-delete") || strstr(cmd, "-exec") || strstr(cmd, "sudo")) return 0;
    for (const char *q = cmd; *q; q++) if (*q == '>' && !(q[1] == '&' && (q[2] == '1' || q[2] == '2')) && strncmp(q, ">/dev/null", 10) != 0 && strncmp(q, "> /dev/null", 11) != 0) return 0;
    const char *p = cmd;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '(') p++;
        if (!*p) break;
        const size_t l = strcspn(p, " \t\n|&;)");
        if (!plan_word_in(p, l, ro)) return 0;
        const int isgit = l == 3 && memcmp(p, "git", 3) == 0, issed = l == 3 && memcmp(p, "sed", 3) == 0;
        const char *a = p + l;
        while (*a == ' ') a++;
        const size_t al = strcspn(a, " \t\n|&;)");
        if (isgit && !plan_word_in(a, al, gitro)) return 0;
        if (issed && !(al == 2 && memcmp(a, "-n", 2) == 0)) return 0;
        int quote = 0;                                           /* ate o proximo separador fora de aspas */
        for (p = a; *p; p++) {
            if (quote) { if (*p == quote) quote = 0; continue; }
            if (*p == '\'' || *p == '"') { quote = *p; continue; }
            if (*p == '|' || *p == ';' || *p == '&' || *p == '\n') break;
        }
        while (*p == '|' || *p == ';' || *p == '&') p++;
    }
    return 1;
}
/* 1 se a ferramenta pode rodar no modo plano */
static int plan_allows(const char *name, const char *js, const char *end) {
    static const char *const ok[] = {"read_file", "list_dir", "find_file", "search_files", "glob", "web_search", "web_fetch", "recall", "howto", "read_log",
        "read_document", "ask_user", "todo", "job", "terminal_screen", NULL};
    static char arg[8192];
    if (plan_word_in(name, strlen(name), ok)) return 1;
    if (strcmp(name, "run_command") == 0) return json_str(js, end, "command", arg, sizeof arg) && !(json_has(js, end, "background") && json_bool(js, end, "background")) && plan_cmd_ok(arg);
    if (strcmp(name, "git") == 0) { char g[8200]; if (!json_str(js, end, "args", arg, sizeof arg)) return 0; snprintf(g, sizeof g, "git %s", arg); return plan_cmd_ok(g); }
    return 0;
}
