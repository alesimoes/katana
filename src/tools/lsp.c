/* ---------- ferramenta LSP: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_LSP_DESC[] = "LSP (not implemented yet)";
static const char CT_LSP_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_LSP_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_lsp(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: LSP is not implemented yet");
}
