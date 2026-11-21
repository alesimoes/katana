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
    {"EnterWorktree", ct_enterworktree, 0, CT_ENTERWORKTREE_DESC, CT_ENTERWORKTREE_PROPS, CT_ENTERWORKTREE_REQ},
    {"ExitWorktree", ct_exitworktree, 0, CT_EXITWORKTREE_DESC, CT_EXITWORKTREE_PROPS, CT_EXITWORKTREE_REQ},
    {"PowerShell", ct_powershell, 0, CT_POWERSHELL_DESC, CT_POWERSHELL_PROPS, CT_POWERSHELL_REQ},
    {"LSP", ct_lsp, 1, CT_LSP_DESC, CT_LSP_PROPS, CT_LSP_REQ},
    {"CronCreate", ct_croncreate, 1, CT_CRONCREATE_DESC, CT_CRONCREATE_PROPS, CT_CRONCREATE_REQ},
    {"CronDelete", ct_crondelete, 1, CT_CRONDELETE_DESC, CT_CRONDELETE_PROPS, CT_CRONDELETE_REQ},
    {"CronList", ct_cronlist, 1, CT_CRONLIST_DESC, CT_CRONLIST_PROPS, CT_CRONLIST_REQ},
    {"ScheduleWakeup", ct_schedulewakeup, 1, CT_SCHEDULEWAKEUP_DESC, CT_SCHEDULEWAKEUP_PROPS, CT_SCHEDULEWAKEUP_REQ},
    {"PushNotification", ct_pushnotification, 1, CT_PUSHNOTIFICATION_DESC, CT_PUSHNOTIFICATION_PROPS, CT_PUSHNOTIFICATION_REQ},
    {"SendUserFile", ct_senduserfile, 1, CT_SENDUSERFILE_DESC, CT_SENDUSERFILE_PROPS, CT_SENDUSERFILE_REQ},
    {"EndConversation", ct_endconversation, 1, CT_ENDCONVERSATION_DESC, CT_ENDCONVERSATION_PROPS, CT_ENDCONVERSATION_REQ},
    {"ListMcpResourcesTool", ct_listmcpresourcestool, 1, CT_LISTMCPRESOURCESTOOL_DESC, CT_LISTMCPRESOURCESTOOL_PROPS, CT_LISTMCPRESOURCESTOOL_REQ},
    {"ReadMcpResourceTool", ct_readmcpresourcetool, 1, CT_READMCPRESOURCETOOL_DESC, CT_READMCPRESOURCETOOL_PROPS, CT_READMCPRESOURCETOOL_REQ},
    {"WaitForMcpServers", ct_waitformcpservers, 1, CT_WAITFORMCPSERVERS_DESC, CT_WAITFORMCPSERVERS_PROPS, CT_WAITFORMCPSERVERS_REQ},
    {"ToolSearch", ct_toolsearch, 1, CT_TOOLSEARCH_DESC, CT_TOOLSEARCH_PROPS, CT_TOOLSEARCH_REQ},
    {"Skill", ct_skill, 1, CT_SKILL_DESC, CT_SKILL_PROPS, CT_SKILL_REQ},
    {"ReportFindings", ct_reportfindings, 1, CT_REPORTFINDINGS_DESC, CT_REPORTFINDINGS_PROPS, CT_REPORTFINDINGS_REQ},
    {"SendMessage", ct_sendmessage, 1, CT_SENDMESSAGE_DESC, CT_SENDMESSAGE_PROPS, CT_SENDMESSAGE_REQ},
    {"ListAgents", ct_listagents, 1, CT_LISTAGENTS_DESC, CT_LISTAGENTS_PROPS, CT_LISTAGENTS_REQ},
    {"SubagentHandback", ct_subagenthandback, 1, CT_SUBAGENTHANDBACK_DESC, CT_SUBAGENTHANDBACK_PROPS, CT_SUBAGENTHANDBACK_REQ},
    {"Workflow", ct_workflow, 0, CT_WORKFLOW_DESC, CT_WORKFLOW_PROPS, CT_WORKFLOW_REQ},
};
#define CT_N ((int)(sizeof g_ct / sizeof g_ct[0]))

static const CtTool *ct_lookup(const char *name) {
    for (int i = 0; i < CT_N; i++) if (strcmp(g_ct[i].name, name) == 0) return &g_ct[i];
    return NULL;
}
static void js_escape(Buf *b, const char *s, size_t n);      /* src/app/server.c */
static int ct_ready(const CtTool *t) { return t && !strstr(t->desc, "(not implemented yet)"); }
/* declaracao de uma ferramenta, numa linha, no formato do bloco <tools> do prompt; 0 se nao existe ou nao esta pronta */
static size_t ct_decl(char *out, size_t cap, const char *name) {
    const CtTool *t = ct_lookup(name);
    if (!ct_ready(t) || cap < 64) return 0;
    Buf d = {0};
    js_escape(&d, t->desc, strlen(t->desc));
    const int n = snprintf(out, cap, "{\"type\": \"function\", \"function\": {\"name\": \"%s\", \"description\": \"%s\", \"parameters\": {\"type\": \"object\", \"properties\": {%s}, \"required\": [%s]}}}\n",
                           t->name, d.p ? d.p : "", t->props, t->req);
    free(d.p);
    if (n < 0 || (size_t)n >= cap) { out[0] = 0; return 0; }
    return (size_t)n;
}
/* nomes das ferramentas prontas, separados por virgula, menos as de skip (lista terminada em NULL) */
static size_t ct_names(char *out, size_t cap, const char *const *skip) {
    size_t o = 0; out[0] = 0;
    for (int i = 0; i < CT_N; i++) {
        if (!ct_ready(&g_ct[i])) continue;
        int s = 0;
        for (int k = 0; skip && skip[k]; k++) if (strcmp(skip[k], g_ct[i].name) == 0) s = 1;
        if (s || o + strlen(g_ct[i].name) + 3 >= cap) continue;
        o += (size_t)snprintf(out + o, cap - o, "%s%s", o ? ", " : "", g_ct[i].name);
    }
    return o;
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
