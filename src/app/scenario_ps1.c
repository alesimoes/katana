/* ---------- katana curriculum, cenarios compostos: scripts PowerShell (.ps1) ----------
 * x-ps1: o usuario pede um script PowerShell ou uma mudanca num que existe. O modelo escreve com Write, muda com Read e
 * Edit, e NAO diz que rodou: nesta maquina o pwsh nao existe (o `command -v pwsh` roda de verdade e nao acha nada), entao a
 * fala final diz que o script foi escrito e nao executado aqui, e como rodar. Os scripts sao textos curtos deste arquivo.
 * Cada .ps1 que fica no disco passa por um conferidor de estrutura (x_ps1_check); como o check refaz a conversa inteira, o
 * conferidor roda de novo no `curriculum check`. O .sh gemeo, quando ha, roda de verdade. */

static const char *const g_x_psverbs[] = {"Get", "Set", "New", "Remove", "Test", "Write", "Read", "Copy", "Move", "Invoke", "Show", "Export", "Import", "Start", "Stop", "Add", "Clear", "Find", "Update", "Measure"};
/* estrutura de um script PowerShell: chaves, parenteses e aspas fechados; param( no topo vem antes de tudo; funcoes com
   nome Verbo-Substantivo de verbo aprovado; nada de sintaxe de shell; os cmdlets esperados (separados por |) presentes */
static int x_ps1_check(const char *t, const char *need, char *why, size_t cap) {
    int br = 0, pa = 0, first = 1, has_param = 0; char q = 0;
    for (const char *l = t; *l; ) {
        const size_t n = strcspn(l, "\n"); const char *e = l + n;
        const char *a = l; while (a < e && (*a == ' ' || *a == '\t')) a++;
        if (a < e && *a != '#') {
            if (!strncmp(l, "param(", 6)) { has_param = 1; if (!first) { snprintf(why, cap, "param( nao e a primeira instrucao"); return 0; } }
            first = 0;
            if (!strncmp(a, "function ", 9)) {
                const char *nm = a + 9; const size_t nl = strcspn(nm, " {(\n"); const char *dash = memchr(nm, '-', nl);
                int ok = 0;
                for (int i = 0; dash && i < CU_N(g_x_psverbs); i++) if (strlen(g_x_psverbs[i]) == (size_t)(dash - nm) && !strncmp(nm, g_x_psverbs[i], (size_t)(dash - nm))) ok = 1;
                if (!ok || !dash || !isupper((unsigned char)dash[1])) { snprintf(why, cap, "funcao sem nome Verbo-Substantivo aprovado: %.*s", (int)nl, nm); return 0; }
            }
            static const char *const sh[] = {"#!/", "; then", "$((", "; fi", "esac", "[ -", "echo \"$", "$(dirname", "2>/dev/null", NULL};
            for (int i = 0; sh[i]; i++) { const char *h = strstr(a, sh[i]); if (h && h < e) { snprintf(why, cap, "sintaxe de shell num .ps1: %s", sh[i]); return 0; } }
            for (const char *c = a; c < e; c++) {
                if (q) { if (*c == q) q = 0; continue; }
                if (*c == '"' || *c == '\'') q = *c;
                else if (*c == '#') break;
                else if (*c == '{') br++; else if (*c == '}') br--; else if (*c == '(') pa++; else if (*c == ')') pa--;
                if (br < 0 || pa < 0) { snprintf(why, cap, "fecha antes de abrir"); return 0; }
            }
            if (q) { snprintf(why, cap, "aspas abertas no fim da linha"); return 0; }
        }
        l = *e ? e + 1 : e;
    }
    (void)has_param;
    if (br || pa) { snprintf(why, cap, "chaves ou parenteses sem fechar"); return 0; }
    for (int i = 0, k = need && *need ? cu_count(need, '|') : 0; i < k; i++) if (!strstr(t, cu_part(need, '|', i))) { snprintf(why, cap, "falta %s", cu_part(need, '|', i)); return 0; }
    return 1;
}
static void x_ps1_verify(const Scn *s, const char *rel, const char *need) {
    const char *t = x_slurp(s, rel); char why[200];
    if (!t) cu_die("o script %s nao existe", rel);
    if (!x_ps1_check(t, need, why, sizeof why)) cu_die("o conferidor de .ps1 recusa %s: %s", rel, why);
}
static const char *x_ps1_count(int limit, const char *verb) {
    return cu_fmt("# counts the files of a folder and compares the count with a limit\nparam(\n    [string]$Path = \".\",\n    [int]$Limit = %d\n)\n\nfunction %s-FileCount {\n    param([string]$Folder)\n"
                  "    (Get-ChildItem -Path $Folder -File).Count\n}\n\n$count = %s-FileCount -Folder $Path\nif ($count -gt $Limit) {\n    Write-Output \"too many files: $count (limit $Limit)\"\n} else {\n    Write-Output \"files: $count\"\n}\n", limit, verb, verb);
}
static const char *x_sh_count(int limit) {
    return cu_fmt("#!/bin/sh\n# counts the files of a folder and compares the count with a limit\npath=${1:-.}\nlimit=%d\ncount=$(find \"$path\" -maxdepth 1 -type f | wc -l)\n"
                  "if [ \"$count\" -gt \"$limit\" ]; then echo \"too many files: $count (limit $limit)\"; else echo \"files: $count\"; fi\n", limit);
}
/* o modelo confere de verdade se ha PowerShell nesta maquina antes de dizer que nao rodou */
static void x_ps1_nopwsh(void) {
    const char *r = x_bash(NULL, "command -v pwsh powershell", L("Check whether PowerShell is installed here", "Ve se o PowerShell esta instalado aqui"), 0);
    if (strchr(r, '/')) cu_die("ha PowerShell nesta maquina: a conversa teria de rodar o script");
}
#define X_PSRUN L(" It was not executed: PowerShell is not installed on this machine. Run it with `powershell -ExecutionPolicy Bypass -File %s`.", " Ele nao foi executado: o PowerShell nao esta instalado nesta maquina. Rode com `powershell -ExecutionPolicy Bypass -File %s`.")
static void fam_x_ps1(FILE *f, Scn *s) {
    static const char *const names[] = {"count", "tally", "census", "inventory", "sizecheck", "filecount", "quota", "audit"};
    const int kind = g_x_kforce >= 0 ? g_x_kforce : (int)((g_x_cycle3 + g_cu_att) % 6);
    XC c; memset(&c, 0, sizeof c); c.s = s;
    x_proj(&c.p, s, CU_PA(((const int[]){X_PY, X_JS, X_SH, X_PL})), 2, 2); XProj *p = &c.p;
    const char *nm = CU_PK(names), *ps = cu_fmt("tools/%s.ps1", nm), *sh = cu_fmt("tools/%s.sh", nm);
    const int lim = 10 * (1 + cu_n(9)), nl = lim + 5 * (1 + cu_n(6));
    p->readme = kind != 4 && cu_pct(50); p->brief = kind == 4; p->psrule = kind == 4; p->knows = p->brief;
    if (kind == 1) x_extra(p, ps, x_ps1_count(lim, "Get"), 0);
    if (kind == 2) { char *bad = cu_fmt("%s", x_ps1_count(lim, "Get")); char *q = strstr(bad, "Get-ChildItem"); memcpy(q, "Get-ChildItms", 13); x_extra(p, ps, bad, 0); }
    if (kind == 3) { x_extra(p, ps, x_ps1_count(lim, "Get"), 0); x_extra(p, sh, x_sh_count(lim), 0755); }
    x_begin("x-ps1", s, p, 0, 0);
    g_x.k = kind; g_x.k2 = 2;
    x_cell("ps1/%s/%s", kind == 0 ? "new" : kind == 1 ? "change" : kind == 2 ? "pasted-error" : kind == 3 ? "sh-and-ps1" : kind == 4 ? "project-rule" : "sh-is-right", g_x_stid[p->st]);
    if (kind == 0) {
        x_user(cu_fmt(XV(L("write a PowerShell script %s that counts the files of a folder and warns when there are more than %d", "escreve um script PowerShell %s que conta os arquivos de uma pasta e avisa quando ha mais de %d"),
                         L("I need %s for the Windows machines: count the files in a folder, warn above %d", "preciso de %s para as maquinas Windows: contar os arquivos de uma pasta, avisar acima de %d")), ps, lim));
        x_write(s, NULL, ps, x_ps1_count(lim, "Get"));
        x_ps1_nopwsh();
        x_ps1_verify(s, ps, "param(|Get-ChildItem|Write-Output|function Get-FileCount");
        x_final(cu_fmt(cu_fmt("%s%s", L("Wrote %s: it takes -Path and -Limit (default %d), counts the files with Get-ChildItem and prints a warning above the limit.", "Escrevi %s: recebe -Path e -Limit (padrao %d), conta os arquivos com Get-ChildItem e avisa acima do limite."), X_PSRUN), ps, lim, ps));
    } else if (kind == 1) {
        x_user(cu_fmt(XV(L("in %s change the default limit to %d and rename the function to Measure-FileCount", "em %s muda o limite padrao para %d e renomeia a funcao para Measure-FileCount"),
                         L("%s: the default -Limit should be %d and the function should be called Measure-FileCount", "%s: o -Limit padrao tem que ser %d e a funcao tem que se chamar Measure-FileCount")), ps, nl));
        const char *plan[3] = {cu_fmt(L("Read %s", "Ler %s"), ps), L("Change the limit and the function name", "Mudar o limite e o nome da funcao"), L("See if PowerShell is here to run it", "Ver se ha PowerShell aqui para rodar")};
        x_plan(plan, 3);
        x_read(s, ps, 0); x_done(1);
        const char *old[3] = {cu_fmt("    [int]$Limit = %d", lim), "function Get-FileCount {", "$count = Get-FileCount -Folder $Path"};
        const char *nw[3] = {cu_fmt("    [int]$Limit = %d", nl), "function Measure-FileCount {", "$count = Measure-FileCount -Folder $Path"};
        x_edits(s, ps, old, nw, 3); x_done(2);
        x_ps1_nopwsh();
        x_ps1_verify(s, ps, cu_fmt("param(|function Measure-FileCount|Measure-FileCount -Folder|$Limit = %d", nl));
        x_final(cu_fmt(cu_fmt("%s%s", L("Changed %s in one edit: the default -Limit is now %d and the function is Measure-FileCount, in its definition and in the call.", "Mudei %s numa edicao so: o -Limit padrao agora e %d e a funcao e Measure-FileCount, na definicao e na chamada."), X_PSRUN), ps, nl, ps));
    } else if (kind == 2) {
        x_user(cu_fmt(XV(L("on my Windows machine %s stops with this:\nGet-ChildItms : The term 'Get-ChildItms' is not recognized as the name of a cmdlet\nfix the script", "na minha maquina Windows o %s para com isto:\nGet-ChildItms : The term 'Get-ChildItms' is not recognized as the name of a cmdlet\ncorrige o script")), ps));
        const char *plan[3] = {cu_fmt(L("Read %s", "Ler %s"), ps), L("Fix the cmdlet name", "Corrigir o nome do cmdlet"), L("See if PowerShell is here to run it", "Ver se ha PowerShell aqui para rodar")};
        x_plan(plan, 3);
        const char *r = x_read(s, ps, 0); if (!strstr(r, "Get-ChildItms")) cu_die("o defeito nao esta no arquivo");
        x_done(1);
        x_edit(s, ps, "(Get-ChildItms -Path $Folder -File).Count", "(Get-ChildItem -Path $Folder -File).Count", 0, 0); x_done(2);
        x_ps1_nopwsh();
        x_ps1_verify(s, ps, "param(|Get-ChildItem -Path|function Get-FileCount");
        x_final(cu_fmt(cu_fmt("%s%s", L("The error came from a misspelled cmdlet in %s: `Get-ChildItms` is now `Get-ChildItem`.", "O erro vinha de um cmdlet escrito errado em %s: `Get-ChildItms` agora e `Get-ChildItem`."), X_PSRUN), ps, ps));
    } else if (kind == 3) {
        x_user(cu_fmt(XV(L("raise the file limit to %d in the %s scripts", "sobe o limite de arquivos para %d nos scripts %s"), L("the limit should be %d in both %s scripts, shell and PowerShell", "o limite tem que ser %d nos dois scripts %s, shell e PowerShell")), nl, nm));
        const char *plan[3] = {cu_fmt(L("Find the %s scripts", "Achar os scripts %s"), nm), L("Change the limit in each one", "Mudar o limite em cada um"), L("Run the shell one", "Rodar o de shell")};
        x_plan(plan, 3);
        const char *g = x_glob(cu_fmt("tools/%s.*", nm)); if (!strstr(g, ps) || !strstr(g, sh)) cu_die("Glob nao achou os dois scripts");
        x_done(1);
        x_read(s, sh, 0); x_edit(s, sh, cu_fmt("limit=%d", lim), cu_fmt("limit=%d", nl), 0, 0);
        x_read(s, ps, 0); x_edit(s, ps, cu_fmt("    [int]$Limit = %d", lim), cu_fmt("    [int]$Limit = %d", nl), 0, 0);
        x_done(2);
        const char *o = cu_fmt("%s", x_bash(NULL, cu_fmt("sh %s .", sh), cu_fmt(L("Run %s on the project folder", "Roda %s na pasta do projeto"), sh), 0));
        if (!strstr(o, "files: ")) cu_die("o script de shell nao rodou: %.200s", o);
        x_ps1_verify(s, ps, cu_fmt("param(|$Limit = %d|Get-ChildItem", nl));
        x_final(cu_fmt(L("The limit is %d in both: %s (`limit=%d`) and %s (`$Limit = %d`). `sh %s .` prints \"%.*s\". The PowerShell one was changed but not executed, since PowerShell is not installed on this machine; run it with `powershell -ExecutionPolicy Bypass -File %s`.",
                         "O limite e %d nos dois: %s (`limit=%d`) e %s (`$Limit = %d`). O `sh %s .` imprime \"%.*s\". O de PowerShell foi alterado mas nao executado, porque o PowerShell nao esta instalado nesta maquina; rode com `powershell -ExecutionPolicy Bypass -File %s`."),
                       nl, sh, nl, ps, nl, sh, (int)strcspn(o, "\n"), o, ps));
    } else if (kind == 4) {
        x_user(cu_fmt(XV(L("add a script tools/%s that counts the files of a folder and warns above %d", "acrescenta um script tools/%s que conta os arquivos de uma pasta e avisa acima de %d"),
                         L("we need a tools/%s script: count the files in a folder, warn when there are more than %d", "precisamos de um script tools/%s: contar os arquivos de uma pasta, avisar quando houver mais de %d")), nm, lim));
        const char *plan[3] = {cu_fmt(L("Write %s", "Escrever %s"), sh), cu_fmt(L("Write %s with the same behaviour (project rule)", "Escrever %s com o mesmo comportamento (regra do projeto)"), ps), L("Run the shell one and the project tests", "Rodar o de shell e os testes do projeto")};
        x_plan(plan, 3);
        x_write(s, NULL, sh, x_sh_count(lim)); x_done(1);
        x_write(s, NULL, ps, x_ps1_count(lim, "Get")); x_done(2);
        const char *o = cu_fmt("%s", x_bash(NULL, cu_fmt("sh %s .", sh), cu_fmt(L("Run %s on the project folder", "Roda %s na pasta do projeto"), sh), 0));
        if (!strstr(o, "files: ")) cu_die("o script de shell nao rodou: %.200s", o);
        const char *t = x_totals(x_test(p, NULL)); if (!t) cu_die("sem totais");
        x_ps1_verify(s, ps, "param(|Get-ChildItem|Write-Output|function Get-FileCount");
        x_final(cu_fmt(L("Added %s and %s, as KATANA.md asks for every script in both forms. `sh %s .` prints \"%.*s\" and `%s` passes (%s). The .ps1 was written but not executed: PowerShell is not installed on this machine; run it with `powershell -ExecutionPolicy Bypass -File %s`.",
                         "Acrescentei %s e %s, como o KATANA.md pede para todo script nas duas formas. O `sh %s .` imprime \"%.*s\" e o `%s` passa (%s). O .ps1 foi escrito mas nao executado: o PowerShell nao esta instalado nesta maquina; rode com `powershell -ExecutionPolicy Bypass -File %s`."),
                       sh, ps, sh, (int)strcspn(o, "\n"), o, p->testcmd, t, ps));
    } else {      /* aqui o certo e um .sh: o pedido e para esta maquina e para o CI em Linux */
        x_user(cu_fmt(XV(L("write a script tools/%s that counts the files of a folder and warns above %d; it runs here and on the Linux CI", "escreve um script tools/%s que conta os arquivos de uma pasta e avisa acima de %d; roda aqui e no CI em Linux"),
                         L("I want a tools/%s script for this machine and the Linux build server: count files, warn above %d", "quero um script tools/%s para esta maquina e para o servidor de build Linux: contar arquivos, avisar acima de %d")), nm, lim));
        x_write(s, NULL, sh, x_sh_count(lim));
        const char *o = cu_fmt("%s", x_bash(NULL, cu_fmt("sh %s .", sh), cu_fmt(L("Run %s on the project folder", "Roda %s na pasta do projeto"), sh), 0));
        if (!strstr(o, "files: ")) cu_die("o script de shell nao rodou: %.200s", o);
        x_final(cu_fmt(L("Wrote %s as a shell script, since it runs here and on Linux. `sh %s .` prints \"%.*s\". If the Windows machines need it too, I can write the .ps1 version.", "Escrevi %s como script de shell, ja que roda aqui e em Linux. O `sh %s .` imprime \"%.*s\". Se as maquinas Windows tambem precisarem, escrevo a versao .ps1."),
                       sh, sh, (int)strcspn(o, "\n"), o));
    }
    if (g_cu_hold) { c.fu = cu_pct(50) ? F_FIND : F_EXPLAIN; x_followup(&c); }      /* holdout: um pedido de outro mecanismo na mesma conversa */
    x_end(f);
}
