/* ---------- fluxos declarados (playbooks) e a linha `[katana: next ...]` (docs/flows.md) ----------
 * O motor, e nao o modelo, guarda em que ponto de um trabalho a conversa esta. Um playbook e uma maquina de estados
 * declarada em tabela: estados (com o texto do proximo passo e as ferramentas esperadas), transicoes por EVENTO e a lista
 * de ferramentas que nao tem lugar no fluxo. O evento vem do que a ferramenta devolveu (fl_event), nunca do que o modelo
 * disse que fez. A cada resultado de ferramenta o laco chama flow_after: o estado anda e, quando ha o que dizer (estado
 * novo, erro, desvio, repeticao entre pedidos, ou algumas voltas sem lembrete), UMA linha curta entra no fim do resultado:
 *     [katana: next <fluxo> <i>/<n> <estado>: <proximo passo>]
 * A linha vai sempre no fim do contexto (fim de um resultado ou do pedido do usuario): nada que ja foi lido e reescrito,
 * entao o cache de prefixo nao e tocado. O estado e da CONVERSA (o controle de repeticao rr_ e rp_ do laco e do pedido).
 * O motor nunca recusa uma chamada fora do fluxo: so anota o desvio na linha. */

enum { FL_NONE, FL_IMAGE, FL_TERMINAL, FL_DEBUG, FL_CODE, FL_WEB, FL_PLAN, FL_DELEGATE, FL_WORKTREE, FL_PLANMODE, FL_SCHEDULE, FL_MCP, FL_REVIEW, FL_DELIVER, FL_COUNT };
typedef enum {
    FE_NONE, FE_OK, FE_FAIL, FE_READ, FE_WRITE, FE_CMD_OK, FE_CMD_FAIL, FE_SEARCH_OK, FE_SEARCH_EMPTY, FE_FETCH_OK, FE_FETCH_BAD,
    FE_RENDER_OK, FE_RENDER_LONG, FE_RENDER_ERR, FE_IMG_CMD, FE_IMG_BG, FE_SENT_FILE, FE_WAIT_INPUT, FE_BG, FE_TYPED, FE_STOPPED,
    FE_PLAN, FE_PLAN_DONE, FE_ASKED, FE_SUB_OK, FE_SUB_FAIL, FE_WT_IN, FE_WT_OUT, FE_PM_IN, FE_PM_OUT, FE_SCHED, FE_SCHED_LIST,
    FE_MCP_LIST, FE_MCP_READ, FE_FINDINGS, FE_NOTIFY, FE_USER_YES, FE_USER_CHANGE, FE_TASK_ENDED, FE_COUNT
} FlowEv;
static const char *const FE_NAME[FE_COUNT] = {
    "none", "ok", "fail", "read", "write", "cmd_ok", "cmd_fail", "search_ok", "search_empty", "fetch_ok", "fetch_bad",
    "render_ok", "render_long", "render_err", "img_cmd", "img_bg", "sent_file", "wait_input", "bg", "typed", "stopped",
    "plan", "plan_done", "asked", "sub_ok", "sub_fail", "wt_in", "wt_out", "pm_in", "pm_out", "sched", "sched_list",
    "mcp_list", "mcp_read", "findings", "notify", "user_yes", "user_change", "task_ended" };

#define FS_ANY 255
#define FS_END 254
/* expect: ferramentas esperadas no estado ("A|B"); "" = responder ao usuario sem ferramenta. No texto: $T = numero da tarefa,
 * $P = caminho guardado, $E = primeira linha do erro, $F = arquivo:linha apontado pelo erro, $S = passo atual do plano. */
typedef struct { const char *name, *next, *expect; } FlowSt;
typedef struct { unsigned char from, ev, to; } FlowTr;
typedef struct { const char *name, *goal; const FlowSt *st; int nst; const FlowTr *tr; int ntr; const char *off; } Playbook;

/* imagem: o pedido vira um prompt, o Render desenha, a interface mostra, o usuario aprova; flux so depois do sim */
static const FlowSt FL_ST_IMAGE[] = {
    {"brief",   "write the final prompt yourself and call Render once (engine stable, brief = the prompt as text, English tags separated by commas)", "Render|ToolSearch"},
    {"show",    "the image is on screen from the Render result. Answer now: give the path and ask if it is approved. Call no tool", ""},
    {"deliver", "the image file $P was made outside Render, so the user has not seen it: hand it over with SendUserFile(files=[\"$P\"]), then ask if it is approved", "SendUserFile|ToolSearch"},
    {"wait",    "the image command still runs as background task $T: Read its output file until it ends. Start nothing else", "Read|Monitor"},
    {"final",   "approved: call Render with engine flux and the same subject written as English prose", "Render"},
    {"failed",  "Render failed. Tell the user the error in one sentence and stop. Do not try Bash, the katana command line or the web for images", ""},
};
static const FlowTr FL_TR_IMAGE[] = {
    {FS_ANY, FE_RENDER_OK, 1}, {FS_ANY, FE_RENDER_LONG, 0}, {FS_ANY, FE_RENDER_ERR, 5}, {FS_ANY, FE_IMG_CMD, 2}, {FS_ANY, FE_IMG_BG, 3},
    {3, FE_TASK_ENDED, 2}, {2, FE_SENT_FILE, 1}, {1, FE_USER_YES, 4}, {1, FE_USER_CHANGE, 0}, {5, FE_USER_CHANGE, 0},
};
/* terminal interativo: um comando parou numa pergunta (ou foi para segundo plano no prazo); responde-se a tarefa, nao se recomeca */
static const FlowSt FL_ST_TERMINAL[] = {
    {"answer", "task $T waits at a question. Answer with SendInput(task_id=\"$T\", text=...) only if you KNOW the value (the user said it, or it is in [hints]); otherwise SendInput(task_id=\"$T\", ask_user=true, ask=...). Do not start the command again and do not stop the task", "SendInput|Read|ToolSearch"},
    {"watch",  "task $T is still running. Read its output file once: if it shows a question, answer it with SendInput(task_id=\"$T\"); if it shows progress, wait with Monitor. Do not start the same command again", "Read|SendInput|Monitor|ToolSearch"},
    {"ended",  "task $T ended. Check the result with the program's own status command, then go on", "Bash|Read"},
};
static const FlowTr FL_TR_TERMINAL[] = {
    {FS_ANY, FE_WAIT_INPUT, 0}, {FS_ANY, FE_BG, 1}, {FS_ANY, FE_TYPED, 1}, {FS_ANY, FE_TASK_ENDED, 2}, {FS_ANY, FE_STOPPED, 2},
    {2, FE_CMD_OK, FS_END}, {2, FE_CMD_FAIL, FS_END}, {2, FE_READ, FS_END},
};
/* depuracao: reproduzir, achar a origem do PRIMEIRO erro, menor correcao, repetir o mesmo comando */
static const FlowSt FL_ST_DEBUG[] = {
    {"reproduce", "run the failing command once and keep its first error line. Do not change anything yet", "Bash|Read|Grep|Find|Symbols"},
    {"locate",    "first error: $E$F. Find where it comes from (Grep the error text, or Symbols on the name) and read only that part", "Grep|Find|Symbols|Read|LSP"},
    {"fix",       "you have read the code around the error. When you know the cause, make the smallest Edit that removes it; one more read is fine, a rewrite is not", "Edit|Write|Read|Grep|Symbols"},
    {"verify",    "run the SAME command again and compare with the first error ($E)", "Bash"},
    {"report",    "the command passes now. Tell the user the cause and the change in a few sentences. Call no tool", ""},
    {"stuck",     "three fixes did not remove the error ($E). Stop editing: tell the user what you tried, the exact error, and what you need from them", ""},
};
static const FlowTr FL_TR_DEBUG[] = {
    {0, FE_CMD_FAIL, 1}, {1, FE_READ, 2}, {0, FE_WRITE, 3}, {1, FE_WRITE, 3}, {2, FE_WRITE, 3}, {3, FE_CMD_OK, 4}, {3, FE_CMD_FAIL, 1},
    {4, FE_WRITE, 3}, {4, FE_CMD_FAIL, 1},
};
/* mudanca de codigo: ler o que vai mudar, mudar, verificar, relatar */
static const FlowSt FL_ST_CODE[] = {
    {"read",   "before editing, find and read the exact code you will change (Find or Symbols for the place, Read for that part)", "Find|Symbols|Grep|Read|LSP"},
    {"edit",   "make the change with Edit (Write only for a new file); keep it to what the request asks", "Edit|Write|NotebookEdit|Read"},
    {"verify", "files changed: build or run the tests once with Bash and read the exit code$V", "Bash"},
    {"repair", "the check failed: $E$F. Fix that first error with the smallest Edit, then run the same check again", "Edit|Read|Grep"},
    {"report", "the check passed. Tell the user what changed (files and behaviour). Call no tool", ""},
};
static const FlowTr FL_TR_CODE[] = {
    {0, FE_READ, 1}, {FS_ANY, FE_WRITE, 2}, {2, FE_CMD_OK, 4}, {2, FE_CMD_FAIL, 3}, {3, FE_CMD_OK, 4},
};
/* pesquisa na web: uma busca especifica, abrir o melhor resultado, responder com a fonte; teto de chamadas */
static const FlowSt FL_ST_WEB[] = {
    {"search", "one WebSearch with the specific words of the question (names, version, exact error text)", "WebSearch"},
    {"read",   "if the passages above already answer, answer now; otherwise open the best result with WebFetch. Never the same address twice", "WebFetch|WebSearch"},
    {"answer", "answer the user now from what you read, with the source address, and say plainly what you did not find. No more searches", ""},
};
static const FlowTr FL_TR_WEB[] = {
    {FS_ANY, FE_SEARCH_OK, 1}, {FS_ANY, FE_FETCH_OK, 2}, {2, FE_SEARCH_OK, 2}, {2, FE_SEARCH_EMPTY, 2}, {2, FE_FETCH_BAD, 2},
};
/* tarefa longa: o plano e escrito uma vez; o progresso e um numero (done=N) junto da proxima acao */
static const FlowSt FL_ST_PLAN[] = {
    {"plan",   "write the plan once with TodoWrite: 3 to 8 concrete steps, one per line", "TodoWrite"},
    {"step",   "$S. If it is finished, send TodoWrite done=N together with the next action; if it is blocked by something only the user has, say so in words", NULL},
    {"report", "all steps are done. Tell the user what was done and what was not. Call no tool", ""},
};
static const FlowTr FL_TR_PLAN[] = { {FS_ANY, FE_PLAN, 1}, {FS_ANY, FE_PLAN_DONE, 2} };
/* delegacao: tarefa fechada para um subagente, ler a resposta, conferir uma afirmacao antes de usar */
static const FlowSt FL_ST_DELEGATE[] = {
    {"brief",  "give the sub-agent one closed task: what to find, where, and what to report back", "Agent|Workflow"},
    {"use",    "the sub-agent answered. Check ONE claim it made (a Read or Grep of the cited place) before you rely on it; do not delegate the same task again", "Read|Grep|Find|Symbols"},
    {"redo",   "the sub-agent did not reach an answer. Do the task yourself in smaller steps; do not delegate it again", NULL},
};
static const FlowTr FL_TR_DELEGATE[] = { {FS_ANY, FE_SUB_OK, 1}, {FS_ANY, FE_SUB_FAIL, 2}, {1, FE_READ, FS_END}, {2, FE_READ, FS_END}, {2, FE_CMD_OK, FS_END} };
/* arvore de trabalho: entrar, trabalhar la dentro, sair dizendo se fica ou se some */
static const FlowSt FL_ST_WORKTREE[] = {
    {"inside", "you are inside a worktree: do the work here, then call ExitWorktree (keep it if the changes matter, remove it if not)", NULL},
    {"left",   "you left the worktree. Tell the user where the changes are (branch or path)", ""},
};
static const FlowTr FL_TR_WORKTREE[] = { {FS_ANY, FE_WT_IN, 0}, {FS_ANY, FE_WT_OUT, 1} };
/* modo plano: so leitura ate o usuario aprovar */
static const FlowSt FL_ST_PLANMODE[] = {
    {"explore", "plan mode: read only (Read, Grep, Find, Symbols). When you know the steps, call ExitPlanMode with the plan", "Read|Grep|Find|Symbols|LSP|ExitPlanMode"},
    {"decided", "the user answered the plan. If approved, start with step 1; if not, change the plan as they said", NULL},
};
static const FlowTr FL_TR_PLANMODE[] = { {FS_ANY, FE_PM_IN, 0}, {FS_ANY, FE_PM_OUT, 1}, {1, FE_WRITE, FS_END}, {1, FE_CMD_OK, FS_END}, {1, FE_READ, FS_END} };
/* agendar e esperar: criar, conferir na lista, dizer ao usuario o que ficou marcado */
static const FlowSt FL_ST_SCHEDULE[] = {
    {"create",  "create the schedule once (CronCreate for a repeating job, ScheduleWakeup for one time)", "CronCreate|ScheduleWakeup|ToolSearch"},
    {"confirm", "it is scheduled. Tell the user its id, when it runs and how to cancel it (CronDelete). Do not create it again", ""},
};
static const FlowTr FL_TR_SCHEDULE[] = { {FS_ANY, FE_SCHED, 1}, {FS_ANY, FE_SCHED_LIST, 1} };
/* recursos MCP: listar, ler o que serve, usar */
static const FlowSt FL_ST_MCP[] = {
    {"list", "list the resources once (ListMcpResourcesTool); if a server is still starting, WaitForMcpServers first", "ListMcpResourcesTool|WaitForMcpServers"},
    {"read", "read the one resource that matches the request with ReadMcpResourceTool (its exact uri from the list)", "ReadMcpResourceTool"},
    {"use",  "you have the resource text: use it for the request; do not list or read it again", NULL},
};
static const FlowTr FL_TR_MCP[] = { {FS_ANY, FE_MCP_LIST, 1}, {FS_ANY, FE_MCP_READ, 2}, {2, FE_READ, FS_END}, {2, FE_WRITE, FS_END}, {2, FE_CMD_OK, FS_END} };
/* revisao: ler a mudanca, conferir cada achado na linha citada, entregar pelo ReportFindings */
static const FlowSt FL_ST_REVIEW[] = {
    {"scope",  "get the change under review once (git diff or the files named) and read it", "Bash|Read|Grep"},
    {"check",  "for each problem you suspect, read the cited lines and confirm it is real before you list it", "Read|Grep|Symbols|Find|LSP"},
    {"report", "send the confirmed findings with ReportFindings (file, line, what breaks); leave out what you could not confirm", "ReportFindings|ToolSearch"},
    {"done",   "the findings were delivered. Give the user a two-line summary. Call no tool", ""},
};
static const FlowTr FL_TR_REVIEW[] = { {0, FE_CMD_OK, 1}, {0, FE_READ, 1}, {1, FE_READ, 2}, {FS_ANY, FE_FINDINGS, 3} };
/* entregar ao usuario: o arquivo pronto vai pelo SendUserFile (o desktop mostra); aviso so quando o usuario esta longe */
static const FlowSt FL_ST_DELIVER[] = {
    {"hand", "hand the finished file to the user with SendUserFile(files=[path]) so the interface shows it; do not paste its content in the chat", "SendUserFile|ToolSearch|Read|Bash"},
    {"tell", "the file was handed over. Say in one sentence what it is. Call no tool", ""},
};
static const FlowTr FL_TR_DELIVER[] = { {FS_ANY, FE_SENT_FILE, 1} };

#define FL_PB(n, g, S, T, off) {n, g, S, (int)(sizeof S / sizeof S[0]), T, (int)(sizeof T / sizeof T[0]), off}
static const Playbook FL_BOOK[FL_COUNT] = {
    [FL_NONE]     = {"none", "no declared flow", NULL, 0, NULL, 0, NULL},
    [FL_IMAGE]    = FL_PB("image",    "make a picture and get it approved", FL_ST_IMAGE, FL_TR_IMAGE, "WebSearch|WebFetch|Write|Edit"),
    [FL_TERMINAL] = FL_PB("terminal", "answer a command that asks questions", FL_ST_TERMINAL, FL_TR_TERMINAL, "WebSearch|WebFetch"),
    [FL_DEBUG]    = FL_PB("debug",    "remove one failure", FL_ST_DEBUG, FL_TR_DEBUG, NULL),
    [FL_CODE]     = FL_PB("code",     "change code and verify it", FL_ST_CODE, FL_TR_CODE, NULL),
    [FL_WEB]      = FL_PB("web",      "answer from the web with a source", FL_ST_WEB, FL_TR_WEB, NULL),
    [FL_PLAN]     = FL_PB("plan",     "long job: plan once, progress by number", FL_ST_PLAN, FL_TR_PLAN, NULL),
    [FL_DELEGATE] = FL_PB("delegate", "hand a closed task to a sub-agent", FL_ST_DELEGATE, FL_TR_DELEGATE, NULL),
    [FL_WORKTREE] = FL_PB("worktree", "work in an isolated copy of the repository", FL_ST_WORKTREE, FL_TR_WORKTREE, NULL),
    [FL_PLANMODE] = FL_PB("planmode", "read only until the user approves the plan", FL_ST_PLANMODE, FL_TR_PLANMODE, "Write|Edit|NotebookEdit"),
    [FL_SCHEDULE] = FL_PB("schedule", "schedule a job or a wake-up", FL_ST_SCHEDULE, FL_TR_SCHEDULE, NULL),
    [FL_MCP]      = FL_PB("mcp",      "use a resource of an MCP server", FL_ST_MCP, FL_TR_MCP, NULL),
    [FL_REVIEW]   = FL_PB("review",   "review a change and report confirmed findings", FL_ST_REVIEW, FL_TR_REVIEW, "Write|Edit|NotebookEdit"),
    [FL_DELIVER]  = FL_PB("deliver",  "hand a finished file to the user", FL_ST_DELIVER, FL_TR_DELIVER, NULL),
};

/* contrato de cada ferramenta: o evento que ela gera quando da certo e quando falha, o fluxo em que ela ENTRA sozinha (a
 * chamada abre o fluxo mesmo sem o pedido ter sido classificado) e o papel dela. Toda ferramenta de tools/table.c esta aqui;
 * `katana flow tools` imprime a tabela e acusa a que faltar. Ferramenta MCP (nome com "__") e generica: FE_OK / FE_FAIL. */
typedef struct { const char *tool; unsigned char ok, fail, enter; const char *role; } FlowTool;
static const FlowTool FL_TOOL[] = {
    {"Bash",                 FE_CMD_OK,     FE_CMD_FAIL,     0,           "run and verify; a question becomes the terminal flow"},
    {"PowerShell",           FE_CMD_OK,     FE_CMD_FAIL,     0,           "the same contract as Bash on Windows"},
    {"TaskStop",             FE_STOPPED,    FE_FAIL,         0,           "terminal flow: ends a task; off the flow while that task waits for an answer"},
    {"Monitor",              FE_BG,         FE_FAIL,         FL_TERMINAL, "terminal flow: wait for a running task"},
    {"SendInput",            FE_TYPED,      FE_FAIL,         FL_TERMINAL, "terminal flow: the answer to the question"},
    {"Read",                 FE_READ,       FE_FAIL,         0,           "evidence: read"},
    {"Grep",                 FE_READ,       FE_FAIL,         0,           "evidence: locate"},
    {"Find",                 FE_READ,       FE_FAIL,         0,           "evidence: locate"},
    {"Symbols",              FE_READ,       FE_FAIL,         0,           "evidence: exact facts about a name"},
    {"LSP",                  FE_READ,       FE_FAIL,         0,           "evidence: exact facts from the language server"},
    {"Write",                FE_WRITE,      FE_FAIL,         0,           "change: opens the verify step"},
    {"Edit",                 FE_WRITE,      FE_FAIL,         0,           "change: opens the verify step"},
    {"NotebookEdit",         FE_WRITE,      FE_FAIL,         0,           "change (code flow): a notebook cell"},
    {"WebSearch",            FE_SEARCH_OK,  FE_SEARCH_EMPTY, 0,           "web flow: search"},
    {"WebFetch",             FE_FETCH_OK,   FE_FETCH_BAD,    0,           "web flow: read one page"},
    {"TodoWrite",            FE_PLAN,       FE_FAIL,         FL_PLAN,     "plan flow: the plan and its progress"},
    {"AskUserQuestion",      FE_ASKED,      FE_FAIL,         0,           "any flow: a decision only the user has; never for a password"},
    {"EnterPlanMode",        FE_PM_IN,      FE_FAIL,         FL_PLANMODE, "planmode flow: start"},
    {"ExitPlanMode",         FE_PM_OUT,     FE_FAIL,         FL_PLANMODE, "planmode flow: the plan goes to the user"},
    {"Agent",                FE_SUB_OK,     FE_SUB_FAIL,     FL_DELEGATE, "delegate flow: one closed task"},
    {"Workflow",             FE_SUB_OK,     FE_SUB_FAIL,     FL_DELEGATE, "delegate flow: several sub-agents by script"},
    {"SendMessage",          FE_OK,         FE_FAIL,         0,           "delegate flow: continue a sub-agent that already has context"},
    {"ListAgents",           FE_OK,         FE_FAIL,         0,           "delegate flow: which sub-agents exist"},
    {"SubagentHandback",     FE_OK,         FE_FAIL,         0,           "delegate flow, inside the sub-agent: the final report"},
    {"EnterWorktree",        FE_WT_IN,      FE_FAIL,         FL_WORKTREE, "worktree flow: start"},
    {"ExitWorktree",         FE_WT_OUT,     FE_FAIL,         FL_WORKTREE, "worktree flow: end"},
    {"CronCreate",           FE_SCHED,      FE_FAIL,         FL_SCHEDULE, "schedule flow: a repeating job"},
    {"CronDelete",           FE_OK,         FE_FAIL,         0,           "schedule flow: cancel"},
    {"CronList",             FE_SCHED_LIST, FE_FAIL,         0,           "schedule flow: what is scheduled"},
    {"ScheduleWakeup",       FE_SCHED,      FE_FAIL,         FL_SCHEDULE, "schedule flow: one wake-up"},
    {"PushNotification",     FE_NOTIFY,     FE_FAIL,         0,           "deliver: tell a user who is away; once per finished job"},
    {"SendUserFile",         FE_SENT_FILE,  FE_FAIL,         0,           "deliver: the file the user must see (image flow, deliver flow)"},
    {"EndConversation",      FE_OK,         FE_FAIL,         0,           "deliver: last call of a conversation, only when the user asked to end"},
    {"ListMcpResourcesTool", FE_MCP_LIST,   FE_FAIL,         FL_MCP,      "mcp flow: list"},
    {"ReadMcpResourceTool",  FE_MCP_READ,   FE_FAIL,         FL_MCP,      "mcp flow: read"},
    {"WaitForMcpServers",    FE_OK,         FE_FAIL,         0,           "mcp flow: wait for a server that is starting"},
    {"ToolSearch",           FE_NONE,       FE_NONE,         0,           "neutral: loads a declaration; never a step of a flow"},
    {"Skill",                FE_PLAN,       FE_FAIL,         FL_PLAN,     "plan flow: the skill's steps become the plan"},
    {"ReportFindings",       FE_FINDINGS,   FE_FAIL,         0,           "review flow: the confirmed findings"},
    {"Render",               FE_RENDER_OK,  FE_RENDER_ERR,   FL_IMAGE,    "image flow: the picture"},
};
#define FL_NTOOL ((int)(sizeof FL_TOOL / sizeof FL_TOOL[0]))

typedef struct { unsigned char id, st; } FlowPos;
#define FL_DEPTH 4
#define FL_RING 64
static struct {
    FlowPos stk[FL_DEPTH]; int depth;                 /* pilha: o fluxo ativo e stk[depth-1]; terminal, delegate etc. entram por cima */
    int req;                                          /* numero do pedido nesta conversa */
    int task;                                         /* tarefa em segundo plano que o fluxo acompanha */
    char path[400], err[170], errfile[330], verify[200];
    int since_emit, emitted_st, emitted_id;           /* voltas desde a ultima linha; estado da ultima linha */
    int cycles, webcalls, badfetch, plan_calls, plan_done;
    char dev[260]; int dev_soft;                      /* desvio visto por flow_before, para a linha desta chamada; soft: so conselho, nunca recusa */
    struct { uint64_t sig, res; int n, req; } ring[FL_RING]; int ring_n, ring_w;   /* chamadas da conversa: repeticao entre pedidos */
    uint64_t cur_sig;
    uint64_t seen[256]; int seen_n, seen_w;           /* o que a conversa ja viu: resultados e assinaturas de erro (evidencia ganha por passo) */
    char jit[240];                                    /* declaracoes ja entregues nesta conversa (KATANA_FLOW_JIT) */
    int stall, plan_was;                              /* passos seguidos sem ganho; passos do plano fechados na ultima conta */
} g_fl;
static int g_fl_last_ev, g_fl_last_gain;           /* o evento e o ganho da ultima chamada (flow_after): o estado duravel os le (resume.c) */

static uint64_t fl_hash(const char *s, size_t n, uint64_t h) { for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; } return h; }
static FlowPos *fl_top(void) { return g_fl.depth ? &g_fl.stk[g_fl.depth - 1] : NULL; }
static int fl_id(void) { return g_fl.depth ? g_fl.stk[g_fl.depth - 1].id : FL_NONE; }
static int fl_in(int id) { for (int i = 0; i < g_fl.depth; i++) if (g_fl.stk[i].id == id) return i + 1; return 0; }
static int fl_list_has(const char *list, const char *name) {
    if (!list) return 0;
    const size_t nl = strlen(name);
    for (const char *p = list; *p; ) { const char *e = strchr(p, '|'); const size_t l = e ? (size_t)(e - p) : strlen(p); if (l == nl && !strncmp(p, name, nl)) return 1; if (!e) break; p = e + 1; }
    return 0;
}
static const FlowTool *fl_tool(const char *name) { for (int i = 0; i < FL_NTOOL; i++) if (!strcmp(FL_TOOL[i].tool, name)) return &FL_TOOL[i]; return NULL; }

/* registro das transicoes (~/.katana/flow.log): o material do aprendizado de fluxos e da conferencia de uma sessao */
static void fl_log(const char *fmt, ...) {
    char h[1024], p[1200]; agent_home(h, sizeof h); snprintf(p, sizeof p, "%s/.katana/flow.log", h);
    FILE *f = fopen(p, "ab"); if (!f) return;
    char ts[32]; const time_t t = time(NULL); strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(f, "%s\t%d\t", ts, g_fl.req);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}
static void flow_reset(void) { memset(&g_fl, 0, sizeof g_fl); g_fl.emitted_st = -1; g_cg_skip = 0; }      /* conversa nova */
static void fl_set(int st) { FlowPos *t = fl_top(); if (t) t->st = (unsigned char)st; }
static void fl_pop(const char *why) {
    if (!g_fl.depth) return;
    fl_log("end\t%s\t%s\t%s", FL_BOOK[fl_id()].name, FL_BOOK[fl_id()].st[fl_top()->st].name, why);
    g_fl.depth--; g_fl.emitted_st = -1;
}
/* entra num fluxo: por cima do atual (push = 1: terminal, delegate, plano...) ou no lugar dele */
static void fl_enter(int id, int st, int push, const char *why) {
    const int at = fl_in(id);
    if (at) { g_fl.depth = at; fl_set(st); return; }                      /* ja estava na pilha: volta a ele */
    if (!push) g_fl.depth = 0;
    if (g_fl.depth >= FL_DEPTH) { memmove(g_fl.stk, g_fl.stk + 1, sizeof g_fl.stk[0] * (FL_DEPTH - 1)); g_fl.depth--; }
    g_fl.stk[g_fl.depth].id = (unsigned char)id; g_fl.stk[g_fl.depth].st = (unsigned char)st; g_fl.depth++;
    g_fl.emitted_st = -1;
    if (id == FL_DEBUG || id == FL_CODE) g_fl.cycles = 0;
    if (id == FL_WEB) { g_fl.webcalls = 0; g_fl.badfetch = 0; }
    fl_log("enter\t%s\t%s\t%s", FL_BOOK[id].name, FL_BOOK[id].st[st].name, why);
}

/* ---- texto do pedido: minusculas e sem acento (so o necessario do portugues), para as regras de entrada ---- */
static void fl_fold(const char *s, char *o, size_t cap) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n + 2 < cap; p++) {
        if (*p == 0xC3 && p[1]) {
            const unsigned char c = p[1] | 0x20; p++;
            o[n++] = (c >= 0xA0 && c <= 0xA5) ? 'a' : c == 0xA7 ? 'c' : (c >= 0xA8 && c <= 0xAB) ? 'e' : (c >= 0xAC && c <= 0xAF) ? 'i' :
                     (c >= 0xB2 && c <= 0xB6) ? 'o' : (c >= 0xB9 && c <= 0xBC) ? 'u' : '?';
        } else o[n++] = (char)tolower(*p);
    }
    o[n] = 0;
}
static int fl_any(const char *t, const char *const *v) { for (int i = 0; v[i]; i++) if (strstr(t, v[i])) return 1; return 0; }
/* fluxo que o pedido pede, pelas palavras dele (FL_NONE: nenhum claro). Regras estreitas de proposito: sem fluxo o laco faz o de antes */
static int fl_classify(const char *t) {
    static const char *const img_n[] = {"imagem", "image", "ilustracao", "illustration", "desenho", "desenhe", "picture", "wallpaper", "uma foto", "a photo", NULL};
    static const char *const img_v[] = {"cri", "ger", "faz", "faca", "desenh", "draw", "generat", "creat", "make", "render", "quero", "produz", NULL};
    static const char *const codeish[] = {"funcao", "function", "codigo", " code", "script", "classe", "class ", "html", "css", "endpoint", "arquivo", " file", "modulo", "module", "teste", "test", "metodo", "method", ".c ", ".py", ".js", ".ts", "api", NULL};
    static const char *const web[] = {"pesquis", "busque", "procure na", "na web", "na internet", "search the web", "search for", "look up", "noticia", "news", "google", NULL};
    static const char *const dbg[] = {" erro", "error", " bug", "falha", "nao funciona", "not work", "crash", "quebr", "traceback", "segfault", "exception", "stack trace", "nao compila", "does not compile", NULL};
    static const char *const chg[] = {"implement", "adicion", "add ", "refator", "refactor", "alter", "modifi", "mude ", "muda ", "troque", "renome", "rename", "escreva um", "write a ", "crie um", "create a ", "corrij", "conserte", "fix ", NULL};
    static const char *const rev[] = {"revise o codigo", "revisao de codigo", "code review", "review the", "review this", "revise a mudanca", "revise o diff", NULL};
    static const char *const dlv[] = {"me envi", "me mand", "send me", "me entregue", NULL};
    static const char *const dlv_n[] = {"arquivo", "file", "pdf", "planilha", "relatorio", "report", NULL};
    static const char *const sch[] = {"agende", "agendar", "schedule", "todo dia", "every day", "a cada ", "me lembre", "remind me", "cron", NULL};
    if (fl_any(t, img_n) && fl_any(t, img_v) && !fl_any(t, codeish) && (render_have(0) || render_have(1))) return FL_IMAGE;
    if (fl_any(t, rev)) return FL_REVIEW;
    if (fl_any(t, sch)) return FL_SCHEDULE;
    if (fl_any(t, web)) return FL_WEB;
    if (fl_any(t, dlv) && fl_any(t, dlv_n)) return FL_DELIVER;
    if (fl_any(t, dbg)) return FL_DEBUG;
    if (fl_any(t, chg) && fl_any(t, codeish)) return FL_CODE;
    return FL_NONE;
}

/* ---- o que o resultado diz ---- */
static int fl_num_after(const char *r, const char *mark) { const char *p = strstr(r, mark); if (!p) return 0; p += strlen(mark); while (*p == ' ' || *p == '"') p++; return atoi(p); }
#ifndef _WIN32
static Job *fl_job(int id) { for (int i = 0; i < JOB_N; i++) if (id && g_jobs[i].id == id) { job_poll(&g_jobs[i]); return &g_jobs[i]; } return NULL; }
static int fl_job_alive(int id) { Job *j = fl_job(id); return j && !j->done; }
/* tarefa em andamento que roda este mesmo comando (0: nenhuma) */
static int fl_job_same_cmd(const char *cmd) {
    for (int i = 0; i < JOB_N; i++) { Job *j = &g_jobs[i]; if (!j->id || j->done) continue; job_poll(j); if (!j->done && j->cmd[0] && !strncmp(j->cmd, cmd, strlen(j->cmd))) return j->id; }
    return 0;
}
#else
static int fl_job_alive(int id) { (void)id; return 0; }
static int fl_job_same_cmd(const char *cmd) { (void)cmd; return 0; }
#endif
/* primeira linha de erro de uma saida e o arquivo:linha que ela (ou outra linha) aponta e que existe no disco */
static void fl_first_error(const char *r) {
    static const char *const kw[] = {"error", "Error", "ERROR", "FAILED", "failed", "undefined", "No such", "not found", "Traceback", "panic", "Exception", NULL};
    const char *best = NULL;
    for (const char *p = r; *p && !best; ) { const char *e = strchr(p, '\n'); const size_t l = e ? (size_t)(e - p) : strlen(p);
        for (int i = 0; kw[i] && !best; i++) { const char *h = strstr(p, kw[i]); if (h && h < p + l) best = p; }
        if (!e) break; p = e + 1; }
    if (!best) { best = r; while (*best == '\n' || *best == ' ') best++; }
    size_t n = 0; for (const char *q = best; *q && *q != '\n' && n < sizeof g_fl.err - 1; q++) g_fl.err[n++] = (*q == '\t' || *q == '"' || *q == ']') ? ' ' : *q;
    while (n && g_fl.err[n - 1] == ' ') n--;
    g_fl.err[n] = 0;
    g_fl.errfile[0] = 0;
    for (const char *p = r; *p; p++) {      /* caminho.ext:numero */
        if (*p != ':' || !isdigit((unsigned char)p[1]) || p == r) continue;
        const char *a = p; while (a > r && (isalnum((unsigned char)a[-1]) || strchr("._/-+", a[-1]))) a--;
        const size_t l = (size_t)(p - a); if (l < 3 || l > 280 || !memchr(a, '.', l)) continue;
        char path[300]; memcpy(path, a, l); path[l] = 0;
        if (access(path, R_OK) != 0) continue;
        snprintf(g_fl.errfile, sizeof g_fl.errfile, " (it points at %s:%d, which exists)", path, atoi(p + 1));
        break;
    }
}
/* como este projeto se verifica, pela pasta de trabalho: so o que existe de fato */
static void fl_find_verify(void) {
    static const struct { const char *file, *cmd; } v[] = { {"build.sh", "sh build.sh"}, {"Makefile", "make"}, {"package.json", "npm test"}, {"Cargo.toml", "cargo test"},
        {"go.mod", "go test ./..."}, {"pyproject.toml", "pytest"}, {"CMakeLists.txt", "cmake --build build"}, {NULL, NULL} };
    g_fl.verify[0] = 0;
    for (int i = 0; v[i].file; i++) if (access(v[i].file, R_OK) == 0) { snprintf(g_fl.verify, sizeof g_fl.verify, " (this folder has %s: `%s`)", v[i].file, v[i].cmd); return; }
}
static FlowEv fl_event(const char *name, const char *js, const char *end, const char *r, int failed) {
    const FlowTool *t = fl_tool(name);
    FlowEv ev = t ? (FlowEv)(failed ? t->fail : t->ok) : (failed ? FE_FAIL : FE_OK);
    if (!strcmp(name, "Render")) {
        if (!strncmp(r, "rendered with", 13)) ev = FE_RENDER_OK;
        else ev = strstr(r, "error: the brief has ") ? FE_RENDER_LONG : FE_RENDER_ERR;
    } else if (!strcmp(name, "Bash") || !strcmp(name, "PowerShell") || !strcmp(name, "SendInput") || !strcmp(name, "Monitor")) {
        char *cmd = ct_str(js, end, "command");
        const int img = cmd && strstr(cmd, "render generate") != NULL;
        if (strstr(r, "[waiting for input")) { ev = FE_WAIT_INPUT; const int id = fl_num_after(r, ", task "); if (id) g_fl.task = id; }
        else if (strstr(r, "moved to the background with ID:")) { ev = img ? FE_IMG_BG : FE_BG; g_fl.task = fl_num_after(r, "with ID:"); }
        else if (img && !failed) ev = FE_IMG_CMD;
        if (img && cmd) {      /* o arquivo que o comando grava: --out <caminho> */
            const char *o = strstr(cmd, "--out "); if (o) { o += 6; while (*o == ' ' || *o == '"' || *o == '\'') o++; size_t n = 0; while (o[n] && o[n] != ' ' && o[n] != '"' && o[n] != '\'' && n < sizeof g_fl.path - 1) n++; snprintf(g_fl.path, sizeof g_fl.path, "%.*s", (int)n, o); }
        }
        if (ev == FE_CMD_FAIL) fl_first_error(r);
        free(cmd);
    } else if (!strcmp(name, "TodoWrite") || !strcmp(name, "Skill")) {
        if (!failed && (strstr(r, "All steps are completed") || !strncmp(r, "All ", 4))) ev = FE_PLAN_DONE;
    } else if (!strcmp(name, "WebFetch")) {
        if (!failed && (strstr(r, "returned all 13 bytes") || strstr(r, " 0 of 0 bytes"))) ev = FE_FETCH_BAD;
    } else if (!strcmp(name, "WebSearch")) {
        if (!failed && (strstr(r, "0 results") || strstr(r, "sem resultados") || strlen(r) < 40)) ev = FE_SEARCH_EMPTY;
    }
    return ev;
}

/* texto do proximo passo do estado atual, com as variaveis trocadas */
static void fl_next_text(char *o, size_t cap) {
    const FlowPos *t = fl_top(); o[0] = 0; if (!t) return;
    const char *s = FL_BOOK[t->id].st[t->st].next; size_t n = 0;
    char step[260] = "";
    if (t->id == FL_PLAN) {
        ct_task_sync(); int cur = -1; for (int i = 0; i < g_ct_ntasks && cur < 0; i++) if (g_ct_tasks[i].st != 2) cur = i;
        if (cur >= 0) snprintf(step, sizeof step, "step %d of %d: %.180s", cur + 1, g_ct_ntasks, g_ct_tasks[cur].subject);
    }
    for (; *s && n + 1 < cap; s++) {
        if (*s != '$' || !s[1]) { o[n++] = *s; continue; }
        const char c = *++s;
        if (c == 'T') n += (size_t)snprintf(o + n, cap - n, "%d", g_fl.task);
        else if (c == 'P') n += (size_t)snprintf(o + n, cap - n, "%s", g_fl.path[0] ? g_fl.path : "<path>");
        else if (c == 'E') n += (size_t)snprintf(o + n, cap - n, "%.120s", g_fl.err[0] ? g_fl.err : "the error above");
        else if (c == 'F') n += (size_t)snprintf(o + n, cap - n, "%s", g_fl.errfile);
        else if (c == 'V') n += (size_t)snprintf(o + n, cap - n, "%s", g_fl.verify);
        else if (c == 'S') n += (size_t)snprintf(o + n, cap - n, "%s", step[0] ? step : "the next open step");
        else { o[n++] = '$'; if (n + 1 < cap) o[n++] = c; }
        if (n >= cap) n = cap - 1;
    }
    o[n] = 0;
}
/* a linha: "\n[katana: next <fluxo> <i>/<n> <estado>: <pre><texto>]"; sem fluxo, "\n[katana: next: <pre>]". Devolve o tamanho */
static size_t fl_line(char *out, size_t cap, const char *pre) {
    const FlowPos *t = fl_top();
    char nx[900], head[80];
    if (!t) {      /* sem fluxo: so o aviso (desvio, repeticao, falta de progresso, licao) */
        if (!pre || !pre[0]) return 0;
        char p2[700]; snprintf(p2, sizeof p2, "%s", pre); size_t l = strlen(p2); while (l && p2[l - 1] == ' ') p2[--l] = 0;
        return KNOTE(FLOW_NEXT, out, cap, "", p2, "");
    }
    fl_next_text(nx, sizeof nx);
    snprintf(head, sizeof head, " %s %d/%d %s", FL_BOOK[t->id].name, t->st + 1, FL_BOOK[t->id].nst, FL_BOOK[t->id].st[t->st].name);
    g_fl.since_emit = 0; g_fl.emitted_st = t->st; g_fl.emitted_id = t->id;
    return KNOTE(FLOW_NEXT, out, cap, head, pre ? pre : "", nx);
}
/* aplica o evento ao fluxo mais alto da pilha que o conhece: 1 = o estado mudou */
static int fl_step(FlowEv ev) {
    for (int d = g_fl.depth - 1; d >= 0; d--) {
        const Playbook *b = &FL_BOOK[g_fl.stk[d].id];
        for (int i = 0; i < b->ntr; i++) {
            if (b->tr[i].ev != ev || (b->tr[i].from != FS_ANY && b->tr[i].from != g_fl.stk[d].st)) continue;
            const int from = g_fl.stk[d].st;
            if (b->tr[i].to == FS_END) {                          /* este fluxo acabou: sai da pilha, os outros ficam */
                fl_log("end\t%s\t%s\t%s", b->name, b->st[from].name, FE_NAME[ev]);
                memmove(g_fl.stk + d, g_fl.stk + d + 1, sizeof g_fl.stk[0] * (size_t)(g_fl.depth - d - 1));
                g_fl.depth--; g_fl.emitted_st = -1;
                return 1;
            }
            g_fl.stk[d].st = b->tr[i].to;
            if (from != b->tr[i].to) fl_log("step\t%s\t%s>%s\t%s", b->name, b->st[from].name, b->st[b->tr[i].to].name, FE_NAME[ev]);
            return from != b->tr[i].to;
        }
        if (g_fl.stk[d].id == FL_TERMINAL) break;                /* com uma tarefa viva no topo, ler a saida dela nao e passo do fluxo de baixo */
    }
    return 0;
}

/* evidencia nova? 1 na primeira vez que a conversa ve este valor */
static int fl_new(uint64_t h) {
    for (int i = 0; i < g_fl.seen_n; i++) if (g_fl.seen[i] == h) return 0;
    const int w = g_fl.seen_n < 256 ? g_fl.seen_n++ : (g_fl.seen_w++ & 255);
    g_fl.seen[w] = h;
    return 1;
}
/* o passo trouxe algo? resultado nunca visto de uma chamada que deu certo, erro de assinatura nova, arquivo mudado, passo do plano fechado, estado do fluxo que andou */
static int fl_gain(FlowEv ev, const char *result, size_t rl, int failed, int changed) {
    int g = changed;
    if (ev == FE_WRITE || ev == FE_SENT_FILE || ev == FE_TYPED || ev == FE_ASKED || ev == FE_PLAN_DONE) g = 1;
    if (!failed) g |= fl_new(fl_hash(result, rl, 1469598103934665603ULL));
    else {      /* um erro so conta como informacao nova na primeira vez: os numeros saem da assinatura */
        uint64_t h = 88172645463325252ULL; int ld = 0, n = 0;
        for (const char *q = result; *q && *q != '\n' && n < 120; q++, n++) { if (isdigit((unsigned char)*q)) { if (ld) continue; ld = 1; h = fl_hash("#", 1, h); } else { ld = 0; h = fl_hash(q, 1, h); } }
        g |= fl_new(h);
    }
    { int td = 0; todo_count(&td); if (td > g_fl.plan_was) g = 1; g_fl.plan_was = td; }
    return g;
}
static int flow_retrieve(const char *name, const char *result, char *out, size_t cap);      /* flowmem.c */
static void flow_grammar_mask(void);
static void flow_jit(char *out, size_t cap);
static size_t flow_memory_note(const char *name, const char *js, const char *end, const char *result, int failed, char *out, size_t cap);      /* flowmem.c */
static int rz_grace(void);      /* resume_note.c: o primeiro pedido depois de reabrir a sessao nao encerra o trabalho aberto */
static size_t rz_block(char *out, size_t cap);
static void rz_request_begin(const char *ureq);
static void rz_before(const char *name, const char *js, const char *end);
static void rz_after(const char *name, const char *js, const char *end, const char *result, int failed, int ev, int gain);
static int rz_cmd_state(const char *file);
static void rz_suspend(void);      /* resume_note.c (KATANA_RESUME_RECALL) */
static void rz_try_recall(const char *t, int is_go);
static size_t rz_probe_clause(char *out, size_t cap);      /* resume_note.c (KATANA_RESUME_PROBE) */
static void rz_dead_check(const char *name, const char *js, const char *end);
static int rz_cmd_report(int argc, char **argv);
static size_t rz_job_clause(char *out, size_t cap);      /* resume.c: tarefas terminadas que o modelo nao soube */
static void rz_sim_begin(void), rz_sim_restart(void), rz_sim_end(void);

/* ---- pedido novo do usuario: escolhe, continua ou encerra o fluxo; out recebe a linha para o fim do pedido (0: nada) ---- */
static void rz_sync_flow(void);      /* resume.c: o espelho durável do estado do fluxo */
static size_t flow_request_body(const char *ureq, char *out, size_t cap) {
    static const char *const yes[] = {"aprov", "sim", "pode", "gostei", "perfeito", "otim", "approved", "yes", "looks good", "alta defini", "flux", "final", NULL};
    static const char *const chg[] = {"mais ", "menos", "cor", "mude", "muda", "troque", "troca", "change", "more ", "less ", "add", "adicion", "remov", "tir", "coloc", "fundo", "estilo", "style", "maior", "menor", "outra", "de novo", "again", "nao gostei", "refaz", NULL};
    static const char *const go[] = {"continu", "segue", "siga", "go on", "prossiga", "vai", "pode", "sim", "ok", "yes", NULL};
    /* o usuario cancela ou troca o objetivo no meio da conversa (nao so uma mudanca de detalhe, como "chg" acima): o plano
       antigo (TodoWrite) e descartado e o proximo pedido do motor avisa para nao reciclar a moldura, o plano nem as
       conclusoes de antes. Sem isto o modelo seguia preso ao pedido original (visto na pasta rea, 2026-11-27: o usuario
       disse para esquecer o flash.locked e buscar root, e o modelo repetiu o plano e a resposta de antes por duas vezes). */
    static const char *const cancel[] = {"esquec", "mudei de ideia", "muda de ideia", "nao e isso", "nao quero mais isso", "nao quero isso",
        "deixa isso de lado", "ignora isso", "ignore isso", "scratch that", "never mind", "forget it", "forget about", "nevermind", NULL};
    char t[2048]; fl_fold(ureq, t, sizeof t);
    const int canceled = fl_any(t, cancel);
    g_fl.req++; g_fl.dev[0] = 0; g_fl.plan_calls = 0;
    out[0] = 0;
    if (canceled) { todo_clear(); g_fl.stall = 0; g_fl.plan_was = 0; }      /* plano velho fora: o proximo TodoWrite comeca do zero */
    if (fl_in(FL_TERMINAL) && !fl_job_alive(g_fl.task)) { g_fl.depth = fl_in(FL_TERMINAL); fl_pop("task gone"); }      /* a tarefa acabou entre os pedidos */
    const int want = fl_classify(t);
    const FlowPos *top = fl_top();
    if (top && top->id == FL_IMAGE && (top->st == 1 || top->st == 5) && want != FL_IMAGE) {      /* imagem mostrada: sim, mudanca, ou outro assunto */
        if (top->st == 1 && fl_any(t, yes) && strlen(t) < 160) fl_step(FE_USER_YES);
        else if (fl_any(t, chg)) fl_step(FE_USER_CHANGE);
        else { rz_suspend(); g_fl.depth = 0; fl_log("end\timage\t-\tnew subject"); }
    } else if (want != FL_NONE) {
        const int ti = fl_in(FL_TERMINAL);                       /* a tarefa viva continua por cima do fluxo novo */
        FlowPos tp = {0, 0}; if (ti) tp = g_fl.stk[ti - 1];
        rz_suspend(); g_fl.depth = 0; fl_enter(want, 0, 0, "request");
        if (ti && want != FL_IMAGE) g_fl.stk[g_fl.depth++] = tp;
    } else if (top && top->id != FL_TERMINAL && top->id != FL_PLAN && top->id != FL_WORKTREE && top->id != FL_PLANMODE) {
        const int closed = FL_BOOK[top->id].st[top->st].expect && !FL_BOOK[top->id].st[top->st].expect[0];      /* estado de responder: o fluxo ja acabou */
        if (!rz_grace() && (closed || !(fl_any(t, go) && strlen(t) < 120))) { rz_suspend(); fl_log("end\t%s\t-\tnew request", FL_BOOK[top->id].name); g_fl.depth = 0; }
    }
    if (want == FL_NONE && !g_fl.depth) rz_try_recall(t, fl_any(t, go) && strlen(t) < 120);      /* o usuario volta a um trabalho suspenso (desligado) */
    if (fl_id() == FL_CODE) fl_find_verify();
    flow_grammar_mask();
    static const char CANCEL_NOTE[] =
        "[katana: the user just canceled or redirected the previous goal in this message. Do not reuse the earlier framing, "
        "the earlier plan or the earlier conclusions, even if the new goal looks related; if you wrote a TodoWrite plan or an "
        "answer about the old goal, that plan and that answer no longer apply. Treat only this message as what the user wants now.]\n";
    if (!fl_top()) { if (canceled) snprintf(out, cap, "%s", CANCEL_NOTE); return strlen(out); }
    g_fl.emitted_st = -1;
    size_t n = canceled ? (size_t)snprintf(out, cap, "%s", CANCEL_NOTE) : 0;
    n += fl_line(out + n, cap - n, "");
    if (n + 400 < cap) flow_jit(out + n, cap - n);
    return strlen(out);
}

static size_t flow_request(const char *ureq, char *out, size_t cap) { const size_t r = flow_request_body(ureq, out, cap); rz_sync_flow(); return r; }

/* ---- antes de a ferramenta rodar: desvio do fluxo. Nunca recusa (devolve 0): guarda o desvio para a linha ---- */
static int flow_before(const char *name, const char *js, const char *end, char *out, size_t cap) {
    g_fl.dev[0] = 0; g_fl.dev_soft = 0;
    const FlowPos *t = fl_top();
    char *cmd = (!strcmp(name, "Bash") || !strcmp(name, "PowerShell")) ? ct_str(js, end, "command") : NULL;
    {   /* assinatura da chamada para a repeticao entre pedidos: o comando (Bash), senao os argumentos */
        const uint64_t h0 = fl_hash(name, strlen(name), 1469598103934665603ULL);
        g_fl.cur_sig = cmd ? fl_hash(cmd, strlen(cmd), h0) : fl_hash(js, (size_t)(end - js), h0);
    }
    if (cmd) {
        const int dup = fl_job_same_cmd(cmd);
        if (dup) { snprintf(g_fl.dev, sizeof g_fl.dev, "this same command is ALREADY running as task %d; a second copy will stop at the same place. ", dup); g_fl.task = dup; if (!fl_in(FL_TERMINAL)) fl_enter(FL_TERMINAL, 1, 1, "duplicate command"); }
        else if (t && t->id == FL_IMAGE && !strstr(cmd, "render generate") && t->st != 3) snprintf(g_fl.dev, sizeof g_fl.dev, "off the flow: a picture is made by the Render tool, not by a shell command. ");
        else if (strstr(cmd, "katana render") && !fl_in(FL_IMAGE) && (render_have(0) || render_have(1))) snprintf(g_fl.dev, sizeof g_fl.dev, "pictures are made with the Render tool (ToolSearch select:Render), not with the katana command line. ");
    }
    free(cmd);
#ifndef _WIN32
    if (!g_fl.dev[0] && !strcmp(name, "TaskStop") && fl_in(FL_TERMINAL)) {
        char *id = ct_str(js, end, "task_id"); Job *j = id ? fl_job(atoi(id)) : NULL; free(id);
        if (j && !j->done && j->asked && !j->typed) snprintf(g_fl.dev, sizeof g_fl.dev, "off the flow: task %d is waiting for an ANSWER; stopping it throws the question away and the next run stops at the same place. ", j->id);
    }
#endif
    if (!g_fl.dev[0] && t) {
        for (int d = g_fl.depth - 1; d >= 0 && !g_fl.dev[0]; d--)
            if (fl_list_has(FL_BOOK[g_fl.stk[d].id].off, name)) snprintf(g_fl.dev, sizeof g_fl.dev, "off the flow: %s has no part in the %s flow. ", name, FL_BOOK[g_fl.stk[d].id].name);
    }
    if (!g_fl.dev[0] && !strcmp(name, "WebSearch")) {      /* a web nao documenta o programa em que o modelo roda */
        char *q = ct_str(js, end, "query"); char f[300] = ""; if (q) fl_fold(q, f, sizeof f); free(q);
        if (strstr(f, "katana")) { g_fl.dev_soft = 1; snprintf(g_fl.dev, sizeof g_fl.dev, "if you mean the katana program you run in: the web does not document it. Its help is local (`katana <command>` with no arguments prints the usage; ToolSearch gives each tool's declaration). "); }
    }
    rz_dead_check(name, js, end);      /* tentativa morta de antes da reentrada (resume.c): so anota o desvio */
    return 0;
}

/* ---- depois de a ferramenta rodar: o estado anda e a linha entra no fim do resultado (closing: o pedido esta encerrando, so o estado anda) ---- */
static void flow_after_body(const char *name, const char *js, const char *end, char *result, size_t cap, int failed, int closing) {
    const size_t rl = strlen(result);
    char pre[700]; size_t pn = (size_t)snprintf(pre, sizeof pre, "%s", g_fl.dev);
    const int had_dev = g_fl.dev[0] != 0;
    g_fl.dev[0] = 0;
    const FlowEv ev = fl_event(name, js, end, result, failed);
    const FlowTool *ft = fl_tool(name);
    int changed = 0;
    g_fl_last_ev = ev; g_fl_last_gain = 0;
    if (ev == FE_NONE) return;                                           /* ToolSearch: neutro */
    /* entrada por evento: a chamada abre o fluxo dela */
    if (ev == FE_WAIT_INPUT || ev == FE_BG) { if (fl_id() != FL_TERMINAL) { fl_enter(FL_TERMINAL, ev == FE_WAIT_INPUT ? 0 : 1, 1, FE_NAME[ev]); changed = 1; } }
    else if (ev == FE_RENDER_OK || ev == FE_RENDER_LONG || ev == FE_RENDER_ERR || ev == FE_IMG_CMD || ev == FE_IMG_BG) { if (!fl_in(FL_IMAGE)) { fl_enter(FL_IMAGE, 0, 0, FE_NAME[ev]); changed = 1; } }
    else if (ft && ft->enter && ft->enter != FL_TERMINAL && ft->enter != FL_IMAGE && !failed && !fl_in(ft->enter)) {
        if (ft->enter != FL_PLAN || fl_id() == FL_NONE) { fl_enter(ft->enter, 0, fl_id() != FL_NONE, name); changed = 1; }
    }
    if (ev == FE_WRITE && fl_id() == FL_NONE) { fl_find_verify(); if (g_fl.verify[0]) fl_enter(FL_CODE, 1, 0, "write"); }      /* arquivo mudou sem fluxo, numa pasta que tem como verificar: o passo de verificar passa a existir */
    /* tarefa acompanhada que acabou */
    int ended = 0;
    if (fl_in(FL_TERMINAL) && g_fl.task && !fl_job_alive(g_fl.task) && ev != FE_WAIT_INPUT && ev != FE_BG && g_fl.stk[fl_in(FL_TERMINAL) - 1].st != 2) { changed |= fl_step(FE_TASK_ENDED); ended = 1; }
    if (fl_id() == FL_IMAGE && fl_top()->st == 3 && g_fl.task && !fl_job_alive(g_fl.task)) changed |= fl_step(FE_TASK_ENDED);
    /* a imagem feita fora do Render so e entregue se o arquivo existe de fato */
    if (fl_id() == FL_IMAGE && fl_top()->st == 2 && g_fl.path[0] && access(g_fl.path, R_OK) != 0 && ev != FE_IMG_CMD) { fl_set(5); changed = 1; pn += (size_t)snprintf(pre + pn, sizeof pre - pn, "the image command ended but %.200s was not written. ", g_fl.path); }
    const int st0 = fl_top() ? fl_top()->st : -1, id0 = fl_id();
    if (!(ended && (ev == FE_TYPED || ev == FE_STOPPED || ev == FE_FAIL))) changed |= fl_step(ev);
    /* contadores que dobram o fluxo */
    if (id0 == FL_DEBUG && st0 == 3 && ev == FE_CMD_FAIL && ++g_fl.cycles >= 3 && fl_id() == FL_DEBUG) { fl_set(5); changed = 1; }
    if (fl_id() == FL_WEB && (ev == FE_SEARCH_OK || ev == FE_SEARCH_EMPTY || ev == FE_FETCH_OK || ev == FE_FETCH_BAD)) {
        g_fl.webcalls++; if (ev == FE_FETCH_BAD || ev == FE_SEARCH_EMPTY) g_fl.badfetch++;
        if ((g_fl.webcalls >= 6 || g_fl.badfetch >= 3) && fl_top()->st != 2) { fl_set(2); changed = 1; pn += (size_t)snprintf(pre + pn, sizeof pre - pn, "%d web calls in this flow (%d gave nothing): the web has no more to give here. ", g_fl.webcalls, g_fl.badfetch); }
        else if (ev == FE_FETCH_BAD && pn < sizeof pre) pn += (size_t)snprintf(pre + pn, sizeof pre - pn, "that page gave no text; do not open it again, take the next result. ");
    }
    /* plano parado: muitas chamadas de trabalho sem done=N (o lembrete periodico antigo ficava velho; este so sai quando ha indicio de esquecimento) */
    int stall = 0;
    if (!strcmp(name, "TodoWrite")) g_fl.plan_calls = 0;
    else {
        int td = 0; const int tn = todo_count(&td);
        if (tn > td && ++g_fl.plan_calls == 8 && fl_id() != FL_TERMINAL) {
            stall = 1;
            fl_enter(FL_PLAN, 1, fl_id() != FL_NONE, "stall");
            pn += (size_t)snprintf(pre + pn, sizeof pre - pn, "8 calls since the plan last moved. ");
        }
    }
    /* repeticao entre pedidos: a mesma chamada com o mesmo resultado ja aconteceu num pedido anterior desta conversa */
    int repeat = 0;
    {   const uint64_t rs = fl_hash(result, rl, 1469598103934665603ULL); int hit = -1;
        for (int i = 0; i < g_fl.ring_n; i++) if (g_fl.ring[i].sig == g_fl.cur_sig && g_fl.ring[i].res == rs) hit = i;
        if (hit >= 0) { g_fl.ring[hit].n++; if (g_fl.ring[hit].req != g_fl.req && !strstr(result, "[katana: this exact call")) { repeat = g_fl.ring[hit].n; g_fl.ring[hit].req = g_fl.req; } }
        else { const int w = g_fl.ring_n < FL_RING ? g_fl.ring_n++ : (g_fl.ring_w++ % FL_RING); g_fl.ring[w].sig = g_fl.cur_sig; g_fl.ring[w].res = rs; g_fl.ring[w].n = 1; g_fl.ring[w].req = g_fl.req; }
    }
    if (repeat && pn < sizeof pre) pn += (size_t)snprintf(pre + pn, sizeof pre - pn, "you made this exact call in an earlier request of this conversation and got this exact result (%d times now); it will not change by itself. Use that result, or tell the user what blocks you. ", repeat);
    /* o que a memoria de fluxos sabe sobre esta falha (flowmem.c; desligado por padrao) */
    if (failed && pn + 300 < sizeof pre) pn += flow_memory_note(name, js, end, result, failed, pre + pn, sizeof pre - pn);
    if (pn + 500 < sizeof pre) pn += rz_job_clause(pre + pn, sizeof pre - pn);
    if (pn + 500 < sizeof pre) pn += rz_probe_clause(pre + pn, sizeof pre - pn);      /* sonda de orientacao depois da reentrada (desligada) */      /* todas as tarefas, nao so a acompanhada (resume.c) */
    /* falta de progresso: o que conta e a evidencia ganha por passo, nao a repeticao exata (pega o laco que varia os argumentos) */
    if (fl_gain(ev, result, rl, failed, changed)) { g_fl.stall = 0; g_fl_last_gain = 1; }
    else if (++g_fl.stall >= 5) { stall = 1; g_fl.stall = 0; if (pn < sizeof pre) pn += (size_t)snprintf(pre + pn, sizeof pre - pn, "the last 5 calls brought nothing new (no new result, no file changed, no step closed). Do not try another variation: use what you already have, or tell the user in two sentences what blocks you. "); fl_log("stall\t%s\t5 steps without gain", name); }
    g_fl.since_emit++;
    flow_grammar_mask();
    const FlowPos *t = fl_top();
    if (closing) return;
    const int fresh = t && (g_fl.emitted_st != t->st || g_fl.emitted_id != t->id);
    int say = changed || fresh || had_dev || repeat || stall || pn > 0 || (t && failed) || (t && g_fl.since_emit >= 4);
    if (t && t->id == FL_PLAN && !pn) say = 0;      /* o resultado do TodoWrite ja diz o passo; a linha do plano so sai com motivo (parado, desvio, repeticao) */
    if (!say || rl + 1000 >= cap) return;
    if (!t && !pn) return;
    size_t el = 0;
    if (changed && flow_retrieve(name, result, result + rl, cap - rl - 900)) el = strlen(result + rl);      /* o estado novo diz o que falta: a evidencia entra antes da linha */
    el += fl_line(result + rl + el, cap - rl - el, pre);
    if (changed && rl + el + 400 < cap) flow_jit(result + rl + el, cap - rl - el);
}

static void flow_after(const char *name, const char *js, const char *end, char *result, size_t cap, int failed, int closing) {
    flow_after_body(name, js, end, result, cap, failed, closing); rz_sync_flow();
}

/* ---- declaracao na hora (KATANA_FLOW_JIT=1, desligada por padrao): o estado espera uma ferramenta que nao esta na lista curta do prompt
 * (Render, SendInput, SendUserFile...). Em vez de o modelo gastar uma volta com ToolSearch, o motor entrega a declaracao (o mesmo texto do
 * ToolSearch) junto da linha, uma vez por conversa e por ferramenta. Custa os tokens da declaracao; economiza uma volta. ---- */
static void flow_jit(char *out, size_t cap) {
    static const char lean[] = "Bash|Read|Edit|Write|Find|Symbols|Grep|TodoWrite|AskUserQuestion|WebSearch|WebFetch|ToolSearch";
    const char *e = getenv("KATANA_FLOW_JIT"); if (e && *e && atoi(e) <= 0) return;      /* ligado por padrao; =0 desliga */
    const FlowPos *t = fl_top(); if (!t) return;
    const char *ex = FL_BOOK[t->id].st[t->st].expect; if (!ex || !ex[0]) return;
    char nm[40]; snprintf(nm, sizeof nm, "%.*s", (int)strcspn(ex, "|"), ex);
    if (fl_list_has(lean, nm) || fl_list_has(g_fl.jit, nm) || strlen(g_fl.jit) + strlen(nm) + 2 >= sizeof g_fl.jit) return;
    static char js[96], d[9000];
    snprintf(js, sizeof js, "{\"query\": \"select:%s\"}", nm);
    ct_toolsearch(js, js + strlen(js), d, sizeof d);
    if (!strncmp(d, "error", 5) || !strncmp(d, "No matching", 11) || strlen(d) + 300 >= cap) return;
    const size_t o = KNOTE(FLOW_DECL, out, cap, nm);
    snprintf(out + o, cap - o, "%s", d);
    snprintf(g_fl.jit + strlen(g_fl.jit), sizeof g_fl.jit - strlen(g_fl.jit), "%s%s", g_fl.jit[0] ? "|" : "", nm);
    fl_log("jit\t%s", nm);
}

/* ---- vies suave condicionado ao estado (ligado por padrao; KATANA_FLOW_GRAMMAR=0 desliga): as ferramentas que nao tem lugar no fluxo ativo (listas `off`)
 * perdem KATANA_FLOW_BIAS (3.0) de logit nos tokens do nome da chamada (callgrammar.c, cg_check). Nada e proibido: se o modelo tem muita confianca
 * numa ferramenta fora do fluxo, ele a escolhe; na duvida e empurrado para o fluxo. ---- */
static void flow_grammar_mask(void) {
    uint64_t mk = 0;
    const char *e = getenv("KATANA_FLOW_GRAMMAR");
    { const char *b = getenv("KATANA_FLOW_BIAS"); g_cg_bias = b && *b ? (float)atof(b) : 3.0f; }
    if (!(e && *e && atoi(e) <= 0) && g_cg.nt <= 64)      /* ligado por padrao; =0 desliga */
        for (int d = 0; d < g_fl.depth; d++) { const char *off = FL_BOOK[g_fl.stk[d].id].off; if (!off) continue; for (int i = 0; i < g_cg.nt; i++) if (fl_list_has(off, cg_tool_names[i])) mk |= 1ULL << i; }
    g_cg_skip = mk;
}

/* ---- katana flow list | tools | state: o registro, impresso (sem modelo) ---- */
static int flowmem_main(int argc, char **argv);      /* flowmem.c: learn, lessons */
static int flow_main(int argc, char **argv) {
    const char *cmd = argc ? argv[0] : "list";
    if (!strcmp(cmd, "list")) {
        for (int id = 1; id < FL_COUNT; id++) {
            const Playbook *b = &FL_BOOK[id];
            printf("%s: %s%s%s%s\n", b->name, b->goal, b->off ? "  (off the flow: " : "", b->off ? b->off : "", b->off ? ")" : "");
            for (int i = 0; i < b->nst; i++) printf("  %d %-9s expects %s\n      next: %s\n", i + 1, b->st[i].name, !b->st[i].expect ? "any tool" : b->st[i].expect[0] ? b->st[i].expect : "the answer to the user", b->st[i].next);
            for (int i = 0; i < b->ntr; i++) printf("  %s + %s -> %s\n", b->tr[i].from == FS_ANY ? "*" : b->st[b->tr[i].from].name, FE_NAME[b->tr[i].ev], b->tr[i].to == FS_END ? "(end)" : b->st[b->tr[i].to].name);
        }
        return 0;
    }
    if (!strcmp(cmd, "tools")) {
        int miss = 0;
        for (int i = 0; i < FL_NTOOL; i++) printf("%-22s ok=%-12s fail=%-12s enters=%-9s %s\n", FL_TOOL[i].tool, FE_NAME[FL_TOOL[i].ok], FE_NAME[FL_TOOL[i].fail], FL_TOOL[i].enter ? FL_BOOK[FL_TOOL[i].enter].name : "-", FL_TOOL[i].role);
        for (int i = 0; i < CT_N; i++) if (!fl_tool(g_ct[i].name)) { printf("MISSING contract: %s\n", g_ct[i].name); miss++; }
        printf("%d tools with a contract, %d in the tool table without one\n", FL_NTOOL, miss);
        return miss ? 1 : 0;
    }
    if (!strcmp(cmd, "classify") && argc > 1) { char t[2048]; fl_fold(argv[1], t, sizeof t); printf("%s\n", FL_BOOK[fl_classify(t)].name); return 0; }
    if (!strcmp(cmd, "sim") && argc > 1) {      /* roteiro sem modelo: "U<TAB>pedido" e "C<TAB>ferramenta<TAB>argumentos json<TAB>resultado" (\\n no resultado vira quebra de linha); imprime o que o modelo leria */
        FILE *f = fopen(argv[1], "rb"); if (!f) { fprintf(stderr, "flow sim: cannot open %s\n", argv[1]); return 1; }
        static char ln[20000], res[24000], note[9000];
        flow_reset(); rz_sim_begin();
        while (fgets(ln, sizeof ln, f)) {
            ln[strcspn(ln, "\r\n")] = 0;
            if (ln[0] == 'R' && !ln[1]) { printf("RESTART\n"); rz_sim_restart(); continue; }      /* reinicio: guarda o estado, limpa a memoria, le de volta e reconcilia */
            if (ln[0] == 'U' && ln[1] == '\t') {
                static char rb[4000];
                printf("USER %s\n", ln + 2); rz_request_begin(ln + 2);
                const size_t fnl = flow_request(ln + 2, note, sizeof note);
                if (rz_block(rb, sizeof rb)) printf("    %s\n", rb + 1);
                if (fnl) printf("    %s\n", note + 1);
                continue;
            }
            if (ln[0] != 'C' || ln[1] != '\t') continue;
            char *tool = ln + 2, *js = strchr(tool, '\t'); if (!js) continue; *js++ = 0;
            char *r = strchr(js, '\t'); if (!r) continue; *r++ = 0;
            size_t n = 0; for (const char *q = r; *q && n + 1 < 20000; q++) { if (q[0] == '\\' && q[1] == 'n') { res[n++] = '\n'; q++; } else res[n++] = *q; }
            res[n] = 0;
            printf("CALL %s %.70s\n", tool, js);
            if (flow_before(tool, js, js + strlen(js), res, sizeof res)) { printf("    %s\n", res); continue; }
            const int failed = !strncmp(res, "error:", 6) || strstr(res, "Exit code 1") != NULL;
            rz_before(tool, js, js + strlen(js));
            flow_after(tool, js, js + strlen(js), res, sizeof res, failed, 0);
            rz_after(tool, js, js + strlen(js), res, failed, g_fl_last_ev, g_fl_last_gain);
            const char *k = strstr(res, "\n[katana: "); printf("    %s\n", k ? k + 1 : "(no line)");
        }
        fclose(f);
        rz_sim_end();
        return 0;
    }
    if (!strcmp(cmd, "resume-report")) return rz_cmd_report(argc, argv);
    if (!strcmp(cmd, "state")) { const char *sf = argc > 1 ? argv[1] : getenv("KATANA_RESUME_FILE"); if (!sf) { fprintf(stderr, "usage: katana flow state <file.kstate>\n"); return 1; } return rz_cmd_state(sf); }
    return flowmem_main(argc, argv);
}
