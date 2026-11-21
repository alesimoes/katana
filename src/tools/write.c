/* ---------- ferramenta Write: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_WRITE_DESC[] = "Write (not implemented yet)";
static const char CT_WRITE_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_WRITE_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_write(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: Write is not implemented yet");
}
