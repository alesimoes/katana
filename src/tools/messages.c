/* ---------- ferramentas SendMessage, ListAgents, SubagentHandback: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_SENDMESSAGE_DESC[] = "SendMessage (not implemented yet)";
static const char CT_SENDMESSAGE_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_SENDMESSAGE_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_sendmessage(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: SendMessage is not implemented yet");
}
static const char CT_LISTAGENTS_DESC[] = "ListAgents (not implemented yet)";
static const char CT_LISTAGENTS_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_LISTAGENTS_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_listagents(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: ListAgents is not implemented yet");
}
static const char CT_SUBAGENTHANDBACK_DESC[] = "SubagentHandback (not implemented yet)";
static const char CT_SUBAGENTHANDBACK_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_SUBAGENTHANDBACK_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_subagenthandback(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: SubagentHandback is not implemented yet");
}
