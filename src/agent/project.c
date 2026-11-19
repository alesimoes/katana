/* ---------- instrucoes do projeto ----------
 * KATANA.md ou AGENTS.md na pasta de trabalho, ou numa pasta acima ate a raiz do repositorio: como compilar, testar e
 * as regras do projeto, escritas pelo dono. Entram no prompt de sistema depois do bloco de ambiente (fora da parte
 * estavel do cache de prefixo). */
#define PROJ_MAX 5000
static size_t proj_instr(char *out, size_t cap, char *path, size_t pcap) {
    static const char *names[] = {"KATANA.md", "AGENTS.md", NULL};
    char d[4200], home[1024];
    out[0] = 0; if (pcap) path[0] = 0;
    if (!getcwd(d, sizeof d)) return 0;
    agent_home(home, sizeof home);
    for (int up = 0; up < 12; up++) {
        for (int i = 0; names[i]; i++) {
            char f[4400];
            snprintf(f, sizeof f, "%s/%s", d, names[i]);
            FILE *fp = fopen(f, "rb");
            if (!fp) continue;
            const size_t room = cap > PROJ_MAX + 100 ? PROJ_MAX : cap > 200 ? cap - 100 : 0;
            size_t n = fread(out, 1, room, fp);
            const int more = n == room && fgetc(fp) != EOF;
            fclose(fp);
            out[n] = 0;
            if (memchr(out, 0, n)) { out[0] = 0; continue; }
            while (n && (out[n - 1] == '\n' || out[n - 1] == ' ')) out[--n] = 0;
            if (!n) continue;
            if (more) n += (size_t)snprintf(out + n, cap - n, "\n[... the file continues: read %s for the rest]", f);
            snprintf(path, pcap, "%s", f);
            return n;
        }
        char g[4400]; struct stat st;
        snprintf(g, sizeof g, "%s/.git", d);
        if (stat(g, &st) == 0 || strcmp(d, home) == 0) break;     /* raiz do repositorio ou a pasta pessoal: nao sobe mais */
        char *sl = strrchr(d, '/');
        if (!sl || sl == d) break;
        *sl = 0;
    }
    return 0;
}
