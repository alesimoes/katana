/* ---------- ferramentas EnterPlanMode, ExitPlanMode: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_ENTERPLANMODE_DESC[] = "EnterPlanMode (not implemented yet)";
static const char CT_ENTERPLANMODE_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_ENTERPLANMODE_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_enterplanmode(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: EnterPlanMode is not implemented yet");
}
static const char CT_EXITPLANMODE_DESC[] = "ExitPlanMode (not implemented yet)";
static const char CT_EXITPLANMODE_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_EXITPLANMODE_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_exitplanmode(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: ExitPlanMode is not implemented yet");
}
