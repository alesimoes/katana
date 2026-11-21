/* ---------- diagnostico de comandos: uma dica curta junto do erro ----------
 * Quando um comando falha, o motor reconhece as causas comuns na saida (programa que nao existe, modulo que falta,
 * caminho errado, permissao, porta ocupada, pip bloqueado pelo sistema, processo morto) e acrescenta UMA dica com o
 * proximo passo. O modelo pequeno deixa de repetir o mesmo comando e passa a corrigir a causa. */
#ifndef _WIN32
/* copia para w a palavra que termina em `e` (letras, digitos, . _ + -); "" se nao houver */
static void diag_word_before(const char *s, const char *e, char *w, size_t cap) {
    const char *b = e;
    while (b > s && (isalnum((unsigned char)b[-1]) || strchr("._+-", b[-1]))) b--;
    snprintf(w, cap, "%.*s", (int)((size_t)(e - b) < cap - 1 ? (size_t)(e - b) : cap - 1), b);
}
/* copia o texto entre aspas simples ou duplas depois de p */
static void diag_quoted(const char *p, char *w, size_t cap) {
    w[0] = 0;
    while (*p && *p != '\'' && *p != '"' && *p != '\n') p++;
    if (*p != '\'' && *p != '"') return;
    const char q = *p++;
    size_t n = 0;
    while (*p && *p != q && *p != '\n' && n + 1 < cap) w[n++] = *p++;
    w[n] = 0;
}
static void cmd_hint(int rc, int timedout, const char *body, const char *cmd, char *out, size_t cap) {
    if (rc == 0 && !timedout) return;
    char w[128] = "", cwd[1024] = "";
    const char *p;
    const int has_sudo = sudo_word(cmd);
    const int pw = pw_detect(body);
    if (pw == 1 && has_sudo) { snprintf(out, cap, "\n[hint: sudo did not get a password (the dialog was cancelled or is not available). Do not retry; tell the user what needs root and why]"); return; }
    if (pw == 1) { snprintf(out, cap, "\n[hint: this program asks for a password or passphrase on a terminal and the command tool has no keyboard. Open a terminal session (terminal action=open), send the command there and read the prompt; if only the user knows the secret, ask the user]"); return; }
    if ((p = strstr(body, "ModuleNotFoundError: No module named"))) {
        diag_quoted(p, w, sizeof w);
        char *dot = strchr(w, '.'); if (dot) *dot = 0;
        snprintf(out, cap, "\n[hint: the Python module %s is missing. If it is a file of this project, check the folder you ran from and the import path; if it is a library, install it with install_package (manager=pip, name=%s)]", w, w);
        return;
    }
    if ((p = strstr(body, "Cannot find module"))) {
        diag_quoted(p, w, sizeof w);
        snprintf(out, cap, "\n[hint: Node cannot find the module %s. For a library run npm install in the project folder (or install_package manager=npm); for a local file check the relative path]", w);
        return;
    }
    if (strstr(body, "externally-managed-environment")) {
        snprintf(out, cap, "\n[hint: the system Python refuses pip install. Create a virtual environment and use it: python3 -m venv .venv && .venv/bin/pip install <package>, then run with .venv/bin/python]");
        return;
    }
    if ((p = strstr(body, ": command not found")) || (rc == 127 && (p = strstr(body, ": not found")))) {
        diag_word_before(body, p, w, sizeof w);
        snprintf(out, cap, "\n[hint: the program %s is not installed or not in PATH. Check with: command -v %s. Install it (install_package, manager apt/pip/npm/cargo) or use another tool that exists; do not repeat the same command]", w[0] ? w : "you called", w[0] ? w : "<name>");
        return;
    }
    if (strstr(body, "Address already in use") || strstr(body, "EADDRINUSE")) {
        snprintf(out, cap, "\n[hint: the port is taken by another process. Find it with: ss -ltnp | grep :<port> ; stop that process or choose another port]");
        return;
    }
    if (strstr(body, "Could not get lock") || strstr(body, "dpkg frontend lock")) {
        snprintf(out, cap, "\n[hint: another package manager is running. Wait for it to finish (check with: pgrep -a apt) and try once more; do not delete the lock files]");
        return;
    }
    if (strstr(body, "No space left on device")) { snprintf(out, cap, "\n[hint: the disk is full. Check with: df -h . ; tell the user before deleting anything]"); return; }
    if (rc == 126 || (pw == 2 && strstr(cmd, "./"))) {
        snprintf(out, cap, "\n[hint: the file is not executable. Run it through its interpreter (bash file.sh, python3 file.py) or make it executable with chmod +x]");
        return;
    }
    if (pw == 2) {
        if (has_sudo) snprintf(out, cap, "\n[hint: permission denied even with sudo: check the path and the owner (ls -ld) and tell the user what blocks it]");
        else snprintf(out, cap, "\n[hint: permission denied. If this really needs root, run the same command with sudo in front: a password dialog opens for the user. If it is your own file, check the owner and mode with ls -l]");
        return;
    }
    if (strstr(body, "No such file or directory")) {
        if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
        snprintf(out, cap, "\n[hint: a path in this command does not exist. The command ran in %s; check the names with list_dir or glob before trying again]", cwd[0] ? cwd : "the working folder");
        return;
    }
    if (rc == 137) { snprintf(out, cap, "\n[hint: the process was killed (signal 9), usually out of memory. Use smaller input or less parallelism]"); return; }
    if (rc == 139) { snprintf(out, cap, "\n[hint: the program crashed with a segmentation fault (signal 11): this is a bug in the program, not in the command line]"); return; }
}
#endif
