/* ---------- ferramentas PushNotification, SendUserFile, EndConversation: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_PUSHNOTIFICATION_DESC[] = "PushNotification (not implemented yet)";
static const char CT_PUSHNOTIFICATION_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_PUSHNOTIFICATION_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_pushnotification(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: PushNotification is not implemented yet");
}
static const char CT_SENDUSERFILE_DESC[] = "SendUserFile (not implemented yet)";
static const char CT_SENDUSERFILE_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_SENDUSERFILE_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_senduserfile(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: SendUserFile is not implemented yet");
}
static const char CT_ENDCONVERSATION_DESC[] = "EndConversation (not implemented yet)";
static const char CT_ENDCONVERSATION_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_ENDCONVERSATION_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_endconversation(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: EndConversation is not implemented yet");
}
