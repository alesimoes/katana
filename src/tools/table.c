/* ---------- tabela das ferramentas com os nomes e argumentos do Claude Code (src/tools/) ----------
 * Uma linha por ferramenta: nome, funcao, se roda no modo plano, e o esquema (descricao, propriedades, obrigatorios)
 * que vai para o prompt do modelo local e para o professor. Gerada uma vez; edite a mao daqui em diante. */
typedef struct { const char *name; void (*fn)(const char *, const char *, char *, size_t); int readonly; const char *desc, *props, *req; } CtTool;
static const CtTool g_ct[] = {
    {"Bash", ct_bash, 0, CT_BASH_DESC, CT_BASH_PROPS, CT_BASH_REQ},
    {"TaskStop", ct_taskstop, 1, CT_TASKSTOP_DESC, CT_TASKSTOP_PROPS, CT_TASKSTOP_REQ},
    {"Monitor", ct_monitor, 0, CT_MONITOR_DESC, CT_MONITOR_PROPS, CT_MONITOR_REQ},
    {"Read", ct_read, 1, CT_READ_DESC, CT_READ_PROPS, CT_READ_REQ},
    {"Write", ct_write, 0, CT_WRITE_DESC, CT_WRITE_PROPS, CT_WRITE_REQ},
    {"Edit", ct_edit, 0, CT_EDIT_DESC, CT_EDIT_PROPS, CT_EDIT_REQ},
    {"NotebookEdit", ct_notebookedit, 0, CT_NOTEBOOKEDIT_DESC, CT_NOTEBOOKEDIT_PROPS, CT_NOTEBOOKEDIT_REQ},
    {"Glob", ct_glob, 1, CT_GLOB_DESC, CT_GLOB_PROPS, CT_GLOB_REQ},
    {"Grep", ct_grep, 1, CT_GREP_DESC, CT_GREP_PROPS, CT_GREP_REQ},
    {"WebFetch", ct_webfetch, 1, CT_WEBFETCH_DESC, CT_WEBFETCH_PROPS, CT_WEBFETCH_REQ},
    {"WebSearch", ct_websearch, 1, CT_WEBSEARCH_DESC, CT_WEBSEARCH_PROPS, CT_WEBSEARCH_REQ},
    {"TodoWrite", ct_todowrite, 1, CT_TODOWRITE_DESC, CT_TODOWRITE_PROPS, CT_TODOWRITE_REQ},
    {"TaskCreate", ct_taskcreate, 1, CT_TASKCREATE_DESC, CT_TASKCREATE_PROPS, CT_TASKCREATE_REQ},
    {"TaskUpdate", ct_taskupdate, 1, CT_TASKUPDATE_DESC, CT_TASKUPDATE_PROPS, CT_TASKUPDATE_REQ},
    {"TaskList", ct_tasklist, 1, CT_TASKLIST_DESC, CT_TASKLIST_PROPS, CT_TASKLIST_REQ},
    {"TaskGet", ct_taskget, 1, CT_TASKGET_DESC, CT_TASKGET_PROPS, CT_TASKGET_REQ},
    {"AskUserQuestion", ct_askuserquestion, 1, CT_ASKUSERQUESTION_DESC, CT_ASKUSERQUESTION_PROPS, CT_ASKUSERQUESTION_REQ},
    {"EnterPlanMode", ct_enterplanmode, 1, CT_ENTERPLANMODE_DESC, CT_ENTERPLANMODE_PROPS, CT_ENTERPLANMODE_REQ},
    {"ExitPlanMode", ct_exitplanmode, 1, CT_EXITPLANMODE_DESC, CT_EXITPLANMODE_PROPS, CT_EXITPLANMODE_REQ},
    {"Agent", ct_agent, 1, CT_AGENT_DESC, CT_AGENT_PROPS, CT_AGENT_REQ},
};
#define CT_N ((int)(sizeof g_ct / sizeof g_ct[0]))

static const CtTool *ct_lookup(const char *name) {
    for (int i = 0; i < CT_N; i++) if (strcmp(g_ct[i].name, name) == 0) return &g_ct[i];
    return NULL;
}
/* executa a ferramenta se o nome for desta tabela; 0 se nao for */
static int ct_dispatch(const char *name, const char *js, const char *end, char *out, size_t cap) {
    const CtTool *t = ct_lookup(name);
    if (!t) return 0;
    out[0] = 0;
    t->fn(js, end, out, cap);
    return 1;
}
/* -1 nao e desta tabela; 0 altera coisas; 1 so le (pode rodar no modo plano) */
static int ct_readonly(const char *name) { const CtTool *t = ct_lookup(name); return t ? t->readonly : -1; }

/* katana tool <Nome> '<argumentos em JSON>' [<Nome> '<json>' ...]: roda ferramentas em sequencia no mesmo processo,
   sem modelo (para testar: a segunda ve o que a primeira deixou, por exemplo Read antes de Edit) */
static int ct_main(int argc, char **argv) {
    static char out[262144];
    trust_init();
    int bad = 0;
    for (int i = 0; i + 1 < argc; i += 2) {
        const char *a = argv[i + 1];
        if (!ct_dispatch(argv[i], a, a + strlen(a), out, sizeof out)) { fprintf(stderr, "ferramenta desconhecida: %s\n", argv[i]); return 2; }
        if (argc > 2) printf("=== %s\n", argv[i]);
        printf("%s\n", out);
        if (strncmp(out, "error:", 6) == 0) bad = 1;
    }
    return bad;
}
