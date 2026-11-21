/* ---------- ferramenta ToolSearch: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_TOOLSEARCH_DESC[] = "ToolSearch (not implemented yet)";
static const char CT_TOOLSEARCH_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_TOOLSEARCH_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_toolsearch(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: ToolSearch is not implemented yet");
}
