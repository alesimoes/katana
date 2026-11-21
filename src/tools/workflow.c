/* ---------- ferramenta Workflow: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_WORKFLOW_DESC[] = "Workflow (not implemented yet)";
static const char CT_WORKFLOW_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_WORKFLOW_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_workflow(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: Workflow is not implemented yet");
}
