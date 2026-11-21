/* ---------- ferramenta Glob: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_GLOB_DESC[] = "Glob (not implemented yet)";
static const char CT_GLOB_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_GLOB_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_glob(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: Glob is not implemented yet");
}
