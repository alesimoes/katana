/* ---------- ferramentas CronCreate, CronDelete, CronList, ScheduleWakeup: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_CRONCREATE_DESC[] = "CronCreate (not implemented yet)";
static const char CT_CRONCREATE_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_CRONCREATE_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_croncreate(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: CronCreate is not implemented yet");
}
static const char CT_CRONDELETE_DESC[] = "CronDelete (not implemented yet)";
static const char CT_CRONDELETE_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_CRONDELETE_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_crondelete(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: CronDelete is not implemented yet");
}
static const char CT_CRONLIST_DESC[] = "CronList (not implemented yet)";
static const char CT_CRONLIST_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_CRONLIST_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_cronlist(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: CronList is not implemented yet");
}
static const char CT_SCHEDULEWAKEUP_DESC[] = "ScheduleWakeup (not implemented yet)";
static const char CT_SCHEDULEWAKEUP_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_SCHEDULEWAKEUP_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_schedulewakeup(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: ScheduleWakeup is not implemented yet");
}
