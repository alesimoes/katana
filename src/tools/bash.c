/* ---------- ferramentas Bash, TaskStop, Monitor: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_BASH_DESC[] = "Bash (not implemented yet)";
static const char CT_BASH_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_BASH_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_bash(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: Bash is not implemented yet");
}
static const char CT_TASKSTOP_DESC[] = "TaskStop (not implemented yet)";
static const char CT_TASKSTOP_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_TASKSTOP_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_taskstop(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: TaskStop is not implemented yet");
}
static const char CT_MONITOR_DESC[] = "Monitor (not implemented yet)";
static const char CT_MONITOR_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_MONITOR_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_monitor(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: Monitor is not implemented yet");
}
