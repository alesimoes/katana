/* ---------- ferramenta Skill: ainda por fazer (ver docs/claude-tools.md) ---------- */
static const char CT_SKILL_DESC[] = "Skill (not implemented yet)";
static const char CT_SKILL_PROPS[] = "";      /* propriedades do esquema, em JSON, sem as chaves de fora */
static const char CT_SKILL_REQ[] = "";        /* nomes obrigatorios: "\"a\", \"b\"" */
static void ct_skill(const char *js, const char *end, char *out, size_t cap) {
    (void)js; (void)end;
    snprintf(out, cap, "error: Skill is not implemented yet");
}
