/* ---------- ferramenta PowerShell (docs/tools.md) ----------
 * Roda o comando com pwsh (ou powershell) se estiver no PATH, pelo mesmo ajudante do Bash (tool_run_command). */
#ifndef _WIN32
static const char CT_POWERSHELL_DESC[] =
    "Runs a PowerShell command with pwsh (or powershell) and returns its output and exit code. 'timeout' is in "
    "milliseconds (default 120000, maximum 600000). Each call starts a fresh process. Background mode is not "
    "supported here. Fails with an error when PowerShell is not installed; use Bash then.";
static const char CT_POWERSHELL_PROPS[] =
    "\"command\": {\"type\": \"string\", \"description\": \"The PowerShell command to run.\"}, "
    "\"timeout\": {\"type\": \"integer\", \"description\": \"Timeout in milliseconds (default 120000, maximum 600000).\"}, "
    "\"run_in_background\": {\"type\": \"boolean\", \"description\": \"Not supported here; use Bash.\"}, "
    "\"description\": {\"type\": \"string\", \"description\": \"Short description of what the command does.\"}";
static const char CT_POWERSHELL_REQ[] = "\"command\"";

/* procura um programa executavel no PATH */
static int ct_ps_which(const char *prog) {
    const char *path = getenv("PATH");
    if (!path) return 0;
    char buf[4400];
    for (const char *p = path; *p; ) {
        size_t l = strcspn(p, ":");
        if (l && l + strlen(prog) + 2 < sizeof buf) {
            struct stat st;
            snprintf(buf, sizeof buf, "%.*s/%s", (int)l, p, prog);
            if (access(buf, X_OK) == 0 && stat(buf, &st) == 0 && S_ISREG(st.st_mode)) return 1;
        }
        p += l; if (*p == ':') p++;
    }
    return 0;
}
static void ct_powershell(const char *js, const char *end, char *out, size_t cap) {
    char *cmd = ct_str(js, end, "command");
    out[0] = 0;
    if (!cmd || !cmd[0]) { snprintf(out, cap, "error: missing command"); free(cmd); return; }
    if (ct_bool(js, end, "run_in_background")) { snprintf(out, cap, "error: run_in_background is not supported for PowerShell here; use Bash with run_in_background instead"); free(cmd); return; }
    const char *exe = ct_ps_which("pwsh") ? "pwsh" : ct_ps_which("powershell") ? "powershell" : NULL;
    if (!exe) { snprintf(out, cap, "error: PowerShell is not installed on this machine (neither pwsh nor powershell is on PATH); use the Bash tool instead"); free(cmd); return; }
    long long ms = ct_int(js, end, "timeout", 120000);
    if (ms <= 0) ms = 120000;
    if (ms > 600000) ms = 600000;
    const int secs = (int)((ms + 999) / 1000);
    if (!agent_act("executar o comando PowerShell", cmd)) { snprintf(out, cap, "error: the user denied this action"); free(cmd); return; }
    const size_t cl = strlen(cmd);
    char *full = (char *)malloc(cl * 4 + 128);
    if (!full) { snprintf(out, cap, "error: out of memory"); free(cmd); return; }
    char *w = full + sprintf(full, "%s -NoProfile -NonInteractive -Command '", exe);
    for (const char *q = cmd; *q; q++) { if (*q == '\'') { memcpy(w, "'\\''", 4); w += 4; } else *w++ = *q; }
    *w++ = '\''; *w = 0;
    char args[64];
    snprintf(args, sizeof args, "{\"timeout_s\": %d}", secs);
    tool_run_command(full, args, args + strlen(args), out, cap);
    free(full); free(cmd);
}
#else
static const char CT_POWERSHELL_DESC[] = "PowerShell (not available on Windows yet)";
static const char CT_POWERSHELL_PROPS[] = "";
static const char CT_POWERSHELL_REQ[] = "";
static void ct_powershell(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: not available on Windows yet"); }
#endif
