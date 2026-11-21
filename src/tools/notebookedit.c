/* ---------- ferramenta NotebookEdit: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_NOTEBOOKEDIT_DESC[] = "NotebookEdit (not implemented yet)";
static const char CT_NOTEBOOKEDIT_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_NOTEBOOKEDIT_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_notebookedit(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: NotebookEdit is not implemented yet");
}
