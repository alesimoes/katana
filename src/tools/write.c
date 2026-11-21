/* ---------- ferramenta Write (nomes e argumentos do Claude Code; ver docs/claude-tools.md) ----------
 * Grava o arquivo inteiro (nunca acrescenta). Sobrescrever exige ter lido o arquivo (ct_seen). Conteudo de qualquer tamanho. */
static const char CT_WRITE_DESC[] =
    "Writes a file to the local filesystem, creating it or replacing it whole (it never appends). file_path should be an absolute path. "
    "If the file already exists you must Read it first in this conversation, otherwise the write is refused. "
    "Prefer Edit to change part of an existing file. The content may be of any size: write a long file in one call. "
    "Missing parent folders are created. If the system denies the write, use Bash with sudo.";
static const char CT_WRITE_PROPS[] =
    "\"file_path\": {\"type\": \"string\", \"description\": \"The absolute path of the file to write\"}, "
    "\"content\": {\"type\": \"string\", \"description\": \"The whole content to write to the file\"}";
static const char CT_WRITE_REQ[] = "\"file_path\", \"content\"";

static void ct_write(const char *js, const char *end, char *out, size_t cap) {
    static char abs[4300], err[1400];
    char *fp = ct_str(js, end, "file_path"), *content = ct_str(js, end, "content"), *old = NULL;
    do {
        if (!fp) { snprintf(out, cap, "error: missing file_path"); break; }
        if (!content) { snprintf(out, cap, "error: missing content (pass content as a string, empty if the file should be empty)"); break; }
        if (!ct_f_abs(fp, abs, sizeof abs, err, sizeof err)) { snprintf(out, cap, "%s", err); break; }
        struct stat st;
        const int exists = stat(abs, &st) == 0;
        if (exists && S_ISDIR(st.st_mode)) { snprintf(out, cap, "error: %s is a directory; give the path of a file", abs); break; }
        const size_t len = strlen(content);
        if (exists) {
            const int sn = ct_seen(abs);
            if (sn == 0 || sn == 1) {
                snprintf(out, cap, "error: %s already exists and you have %s it. Read the whole file with Read first, then write; or use Edit for a small change.",
                         abs, sn == 0 ? "not read" : "only read part of");
                break;
            }
            size_t on = 0;
            old = ct_f_slurp(abs, &on, err, sizeof err);
            if (old && !ct_utf8_ok((const unsigned char *)old, on) && strstr(content, "\xEF\xBF\xBD")) {
                snprintf(out, cap, "error: %s is not valid UTF-8 and the new content contains replacement characters (U+FFFD); the write would destroy the original bytes. Use Bash to handle this file.", abs);
                break;
            }
        }
        char what[4400];
        snprintf(what, sizeof what, "%s o arquivo %s (%zu bytes)", exists ? "sobrescrever" : "criar", abs, len);
        if (!agent_act(what, content)) { snprintf(out, cap, "error: the user denied this action"); break; }
        mkdir_parents(abs);
        undo_note(abs);
        FILE *f = fopen(abs, "wb");
        if (!f) {
            const int perm = errno == EACCES || errno == EPERM || errno == EROFS;
            snprintf(out, cap, "error: cannot write %s (%s).%s", abs, strerror(errno), perm ? " It needs more permission: use Bash with sudo (for example: sudo tee <path>) if the user wants this." : "");
            break;
        }
        const size_t w = fwrite(content, 1, len, f);
        if (fclose(f) != 0 || w != len) { snprintf(out, cap, "error: write to %s failed (%s)", abs, strerror(errno)); break; }
        ct_seen_mark(abs, 0);
        snprintf(out, cap, "%s %s (%zu bytes, %lld lines)", exists ? "The file has been overwritten:" : "File created successfully at:", abs, len, ct_count_lines(content, len));
        code_check(abs, 0, out, cap);
    } while (0);
    free(fp); free(content); free(old);
}
