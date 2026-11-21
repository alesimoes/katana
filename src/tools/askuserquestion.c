/* ---------- ferramenta AskUserQuestion: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_ASKUSERQUESTION_DESC[] = "AskUserQuestion (not implemented yet)";
static const char CT_ASKUSERQUESTION_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_ASKUSERQUESTION_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_askuserquestion(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: AskUserQuestion is not implemented yet");
}
