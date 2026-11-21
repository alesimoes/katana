/* ---------- ferramenta Edit: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_EDIT_DESC[] = "Edit (not implemented yet)";
static const char CT_EDIT_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_EDIT_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_edit(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: Edit is not implemented yet");
}
