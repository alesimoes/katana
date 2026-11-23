/* ---------- modo plano ----------
 * /plan liga: o agente so le e investiga (o motor recusa gravar, editar, instalar, baixar, terminal, ferramentas MCP e
 * qualquer comando que nao seja reconhecidamente de leitura) e termina com um plano. /go aprova e manda executar;
 * /plan off sai sem executar. A trava fica no motor, nao depende de o modelo obedecer. */
static int g_plan_mode;
#define PLAN_NOTE "[Plan mode is ON: you may only read and investigate; the engine refuses anything that changes files or the system. " \
                  "Investigate what you need, then answer with a short numbered plan of what you would do, and stop. The user will approve it.]\n"
#define PLAN_DENY "error: plan mode is on (read-only), so this was NOT done. Keep investigating with read-only tools, then present your numbered plan and stop; the user approves it."

static void plan_ui(void) { if (getenv("KATANA_UI")) { printf("\x01" "MODE %s\n", g_plan_mode ? "plan" : "normal"); fflush(stdout); } }

/* 1 se a ferramenta pode rodar no modo plano (as da tabela decidem por ct_readonly; aqui so as MCP somente leitura) */
static int plan_allows(const char *name, const char *js, const char *end) {
    (void)js; (void)end;
    return mcpx_plan_ok(name);
}
