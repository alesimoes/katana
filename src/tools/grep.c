/* ---------- ferramenta Grep: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_GREP_DESC[] = "Grep (not implemented yet)";
static const char CT_GREP_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_GREP_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_grep(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: Grep is not implemented yet");
}
