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
static const char *x_ps1_largest(int top) {
    return cu_fmt("# lists the largest files of a folder, size first\nparam(\n    [string]$Path = \".\",\n    [int]$Top = %d\n)\n\nfunction Get-LargestFile {\n    param([string]$Folder, [int]$Count)\n"
                  "    Get-ChildItem -Path $Folder -File | Sort-Object -Property Length -Descending | Select-Object -First $Count\n}\n\n"
                  "Get-LargestFile -Folder $Path -Count $Top | ForEach-Object { Write-Output \"$($_.Length) $($_.Name)\" }\n", top);
}
static const char *x_ps1_stage(const char *stage) {
    return cu_fmt("# sets the stage for this session and shows it\nparam(\n    [string]$Stage = \"%s\"\n)\n\nfunction Set-Stage {\n    param([string]$Name)\n    $env:APP_STAGE = $Name\n}\n\n"
                  "Set-Stage -Name $Stage\nWrite-Output \"APP_STAGE is $env:APP_STAGE\"\n", stage);
}
static const char *x_ps1_settings(void) {
    return "# checks that the settings file exists and has a port line\nparam(\n    [string]$File = \"settings.conf\"\n)\n\nfunction Test-Setting {\n    param([string]$Path, [string]$Key)\n"
           "    if (-not (Test-Path -Path $Path)) { return $false }\n    [bool](Select-String -Path $Path -Pattern \"^$Key = \")\n}\n\n"
           "if (Test-Setting -Path $File -Key \"port\") {\n    Write-Output \"settings ok\"\n} else {\n    Write-Output \"settings invalid\"\n}\n";
}
/* situacoes: 0 a 11, 13 e 14 saem no treino; 12, 15 e 16 so no arquivo de validacao */
static const int g_x_ps1_train[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14}, g_x_ps1_hold[] = {12, 15, 16};
static const char *const g_x_ps1_cell[] = {"new", "change", "pasted-error", "sh-and-ps1", "project-rule", "sh-is-right", "new-largest", "add-switch", "explain", "port-from-sh", "pasted-error-in-the-call", "shell-syntax",
                                           "run-with-twin", "run-without-twin", "new-env", "new-settings-check", "help-comments"};
/* o modelo confere de verdade se ha PowerShell nesta maquina antes de dizer que nao rodou */
static void x_ps1_nopwsh(void) {
    const char *r = x_bash(NULL, "command -v pwsh powershell", L("Check whether PowerShell is installed here", "Ve se o PowerShell esta instalado aqui"), 0);
    if (strchr(r, '/')) cu_die("ha PowerShell nesta maquina: a conversa teria de rodar o script");
}
#define X_PSRUN L(" It was not executed: PowerShell is not installed on this machine. Run it with `powershell -ExecutionPolicy Bypass -File %s`.", " Ele nao foi executado: o PowerShell nao esta instalado nesta maquina. Rode com `powershell -ExecutionPolicy Bypass -File %s`.")
static void fam_x_ps1(FILE *f, Scn *s) {
    static const char *const names[] = {"count", "tally", "census", "inventory", "sizecheck", "filecount", "quota", "audit"};
    const int kind = g_x_kforce >= 0 ? g_x_kforce : g_cu_hold ? g_x_ps1_hold[(g_x_cycle3 + g_cu_att) % CU_N(g_x_ps1_hold)] : g_x_ps1_train[(g_x_cycle3 + g_cu_att) % CU_N(g_x_ps1_train)];
    XC c; memset(&c, 0, sizeof c); c.s = s;
    x_proj(&c.p, s, CU_PA(((const int[]){X_PY, X_JS, X_SH, X_PL})), 2, 2); XProj *p = &c.p;
    const char *nm = CU_PK(names), *ps = cu_fmt("tools/%s.ps1", nm), *sh = cu_fmt("tools/%s.sh", nm);
    const int lim = 10 * (1 + cu_n(9)), nl = lim + 5 * (1 + cu_n(6));
    p->readme = kind != 4 && cu_pct(50); p->brief = kind == 4; p->psrule = kind == 4; p->knows = p->brief;
    if (kind == 1 || kind == 7 || kind == 8 || kind == 10 || kind == 13 || kind == 16) x_extra(p, ps, x_ps1_count(lim, "Get"), 0);
    if (kind == 9) x_extra(p, sh, x_sh_count(lim), 0755);
    if (kind == 11) { const char *good = x_ps1_count(lim, "Get"), *q = strstr(good, "if ($count -gt $Limit) {"); x_extra(p, ps, cu_fmt("%.*sif [ $count -gt $Limit ]; then%s", (int)(q - good), good, q + 24), 0); }
    if (kind == 15) { p->conf = 1; for (int i = 0; i < CU_N(x_conf_keys); i++) x_conf_add(p, x_conf_keys[i][0], x_conf_keys[i][1], x_conf_keys[i][2]); }
    if (kind == 2) { char *bad = cu_fmt("%s", x_ps1_count(lim, "Get")); char *q = strstr(bad, "Get-ChildItem"); memcpy(q, "Get-ChildItms", 13); x_extra(p, ps, bad, 0); }
    if (kind == 3 || kind == 12) { x_extra(p, ps, x_ps1_count(lim, "Get"), 0); x_extra(p, sh, x_sh_count(lim), 0755); }
    x_begin("x-ps1", s, p, 0, 0);
    g_x.k = kind; g_x.k2 = 2;
    x_cell("ps1/%s/%s", g_x_ps1_cell[kind], g_x_stid[p->st]);
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
    } else if (kind == 6) {      /* outro script: os maiores arquivos de uma pasta */
        const int top = 3 + cu_n(8);
        x_user(cu_fmt(XV(L("write a PowerShell script %s that lists the %d largest files of a folder, size first", "escreve um script PowerShell %s que lista os %d maiores arquivos de uma pasta, o tamanho primeiro"),
                         L("for the Windows machines I need %s: the %d biggest files in a folder, one per line with the size", "para as maquinas Windows preciso de %s: os %d maiores arquivos de uma pasta, um por linha com o tamanho")), ps, top));
        x_write(s, NULL, ps, x_ps1_largest(top));
        x_ps1_nopwsh();
        x_ps1_verify(s, ps, "param(|function Get-LargestFile|Sort-Object|Select-Object -First|Write-Output");
        x_final(cu_fmt(cu_fmt("%s%s", L("Wrote %s: -Path picks the folder and -Top how many (default %d); it sorts the files by Length, largest first, and prints size and name.", "Escrevi %s: -Path escolhe a pasta e -Top quantos (padrao %d); ordena os arquivos por Length, do maior para o menor, e imprime tamanho e nome."), X_PSRUN), ps, top, ps));
    } else if (kind == 7) {      /* parametro novo num script que existe: quatro trocas numa edicao */
        x_user(cu_fmt(XV(L("add a -Recurse switch to %s so it can count the files of the subfolders too", "acrescenta um switch -Recurse em %s para ele contar tambem os arquivos das subpastas"),
                         L("%s only counts the top folder; give it an optional -Recurse", "o %s so conta a pasta de cima; coloca um -Recurse opcional nele")), ps));
        const char *plan[3] = {cu_fmt(L("Read %s", "Ler %s"), ps), L("Add the switch and pass it down to Get-ChildItem", "Acrescentar o switch e passar ate o Get-ChildItem"), L("See if PowerShell is here to run it", "Ver se ha PowerShell aqui para rodar")};
        x_plan(plan, 3);
        x_read(s, ps, 0); x_done(1);
        const char *old[4] = {cu_fmt("    [int]$Limit = %d", lim), "    param([string]$Folder)", "    (Get-ChildItem -Path $Folder -File).Count", "$count = Get-FileCount -Folder $Path"};
        const char *nw[4] = {cu_fmt("    [int]$Limit = %d,\n    [switch]$Recurse", lim), "    param([string]$Folder, [switch]$Deep)", "    (Get-ChildItem -Path $Folder -File -Recurse:$Deep).Count", "$count = Get-FileCount -Folder $Path -Deep:$Recurse"};
        x_edits(s, ps, old, nw, 4); x_done(2);
        x_ps1_nopwsh();
        x_ps1_verify(s, ps, "param(|[switch]$Recurse|-Recurse:$Deep|-Deep:$Recurse|function Get-FileCount");
        x_final(cu_fmt(cu_fmt("%s%s", L("%s now has a -Recurse switch: it is declared in param(, handed to Get-FileCount as -Deep and from there to Get-ChildItem. Without the switch it counts as before.", "O %s agora tem o switch -Recurse: ele esta declarado no param(, vai para Get-FileCount como -Deep e dali para o Get-ChildItem. Sem o switch ele conta como antes."), X_PSRUN), ps, ps));
    } else if (kind == 8) {      /* so explicar: le e nao muda nada */
        x_user(cu_fmt(XV(L("what does %s do? do not change it", "o que o %s faz? nao muda nada nele"), L("explain %s to me, I did not write it", "me explica o %s, nao fui eu que escrevi")), ps));
        const char *r = x_read(s, ps, 0); if (!strstr(r, "Get-FileCount")) cu_die("o script nao e o esperado");
        x_final(cu_fmt(L("%s takes -Path (default the current folder) and -Limit (default %d). Its function Get-FileCount counts the files of the folder with Get-ChildItem; above the limit it prints `too many files`, otherwise `files:` and the count. I only read it: nothing was changed or run.",
                         "O %s recebe -Path (padrao: a pasta atual) e -Limit (padrao %d). A funcao Get-FileCount conta os arquivos da pasta com Get-ChildItem; acima do limite ele imprime `too many files`, senao `files:` e a contagem. So li: nada foi alterado nem rodado."), ps, lim));
    } else if (kind == 9) {      /* portar o .sh que existe para PowerShell */
        x_user(cu_fmt(XV(L("port %s to PowerShell for the colleagues on Windows; keep the shell one", "porta o %s para PowerShell para os colegas no Windows; mantem o de shell"), L("we have %s; I need the same thing as a .ps1 next to it", "temos o %s; preciso da mesma coisa em .ps1 ao lado dele")), sh));
        const char *plan[3] = {cu_fmt(L("Read %s", "Ler %s"), sh), cu_fmt(L("Write %s with the same behaviour", "Escrever %s com o mesmo comportamento"), ps), L("Run the shell one for reference and see if PowerShell is here", "Rodar o de shell como referencia e ver se ha PowerShell aqui")};
        x_plan(plan, 3);
        x_read(s, sh, 0); x_done(1);
        x_write(s, NULL, ps, x_ps1_count(lim, "Get")); x_done(2);
        const char *o = cu_fmt("%s", x_bash(NULL, cu_fmt("sh %s .", sh), cu_fmt(L("Run %s on the project folder", "Roda %s na pasta do projeto"), sh), 0));
        if (!strstr(o, "files: ")) cu_die("o script de shell nao rodou: %.200s", o);
        x_ps1_nopwsh();
        x_ps1_verify(s, ps, cu_fmt("param(|$Limit = %d|Get-ChildItem|Write-Output", lim));
        x_final(cu_fmt(L("Wrote %s next to %s with the same limit (%d) and the same two messages. For reference, `sh %s .` prints \"%.*s\" here. The .ps1 was not executed: PowerShell is not installed on this machine; run it with `powershell -ExecutionPolicy Bypass -File %s`.",
                         "Escrevi %s ao lado de %s com o mesmo limite (%d) e as mesmas duas mensagens. Como referencia, o `sh %s .` imprime \"%.*s\" aqui. O .ps1 nao foi executado: o PowerShell nao esta instalado nesta maquina; rode com `powershell -ExecutionPolicy Bypass -File %s`."),
                       ps, sh, lim, sh, (int)strcspn(o, "\n"), o, ps));
    } else if (kind == 10) {      /* erro colado que vem da chamada, nao do script: nada a corrigir */
        x_user(cu_fmt(L("%s fails on my Windows machine with:\nCannot process argument transformation on parameter 'Limit'. Cannot convert value \"ten\" to type \"System.Int32\".\nfix the script", "o %s falha na minha maquina Windows com:\nCannot process argument transformation on parameter 'Limit'. Cannot convert value \"ten\" to type \"System.Int32\".\ncorrige o script"), ps));
        const char *r = x_read(s, ps, 0); if (!strstr(r, "[int]$Limit")) cu_die("o script nao e o esperado");
        x_final(cu_fmt(L("The script is not the problem, so I changed nothing in %s. It declares `[int]$Limit`, and the error says the call passed the word \"ten\". Call it with a number: `powershell -ExecutionPolicy Bypass -File %s -Limit 10`.",
                         "O problema nao e o script, entao nao mudei nada em %s. Ele declara `[int]$Limit`, e o erro diz que a chamada passou a palavra \"ten\". Chame com um numero: `powershell -ExecutionPolicy Bypass -File %s -Limit 10`."), ps, ps));
    } else if (kind == 11) {      /* sintaxe de shell dentro do .ps1 */
        x_user(cu_fmt(L("%s does not even start on Windows:\nMissing '(' after 'if' in if statement.\ncan you fix it?", "o %s nem comeca no Windows:\nMissing '(' after 'if' in if statement.\nda pra corrigir?"), ps));
        const char *plan[3] = {cu_fmt(L("Read %s", "Ler %s"), ps), L("Rewrite the shell-style test as a PowerShell one", "Reescrever o teste em estilo shell como um de PowerShell"), L("See if PowerShell is here to run it", "Ver se ha PowerShell aqui para rodar")};
        x_plan(plan, 3);
        const char *r = x_read(s, ps, 0); if (!strstr(r, "if [ $count -gt $Limit ]; then")) cu_die("o defeito nao esta no arquivo");
        x_done(1);
        x_edit(s, ps, "if [ $count -gt $Limit ]; then", "if ($count -gt $Limit) {", 0, 0); x_done(2);
        x_ps1_nopwsh();
        x_ps1_verify(s, ps, "param(|if ($count -gt $Limit) {|Get-ChildItem");
        x_final(cu_fmt(cu_fmt("%s%s", L("Line `if [ $count -gt $Limit ]; then` in %s was shell syntax; it is now `if ($count -gt $Limit) {`, which matches the `} else {` below it.", "A linha `if [ $count -gt $Limit ]; then` em %s era sintaxe de shell; agora e `if ($count -gt $Limit) {`, que casa com o `} else {` logo abaixo."), X_PSRUN), ps, ps));
    } else if (kind == 12 || kind == 13) {      /* pedem para RODAR o .ps1: sem PowerShell aqui, o modelo tenta outro caminho antes de dizer que nao da */
        x_user(cu_fmt(kind == 12 ? XV(L("execute %s against the project folder and give me its output", "executa o %s na pasta do projeto e me passa a saida"))
                                 : XV(L("run %s on this folder and tell me what it prints", "roda o %s nesta pasta e me diz o que ele imprime"), L("what does %s print for this project folder? run it", "o que o %s imprime para a pasta deste projeto? roda ele")), ps));
        const char *plan[3] = {L("See if PowerShell is here", "Ver se ha PowerShell aqui"), kind == 12 ? L("Look for a shell twin of the script", "Procurar um gemeo do script em shell") : L("Read the script", "Ler o script"), L("Get the same answer with the shell", "Conseguir a mesma resposta com o shell")};
        x_plan(plan, 3);
        x_ps1_nopwsh(); x_done(1);
        if (kind == 12) {
            const char *g = x_glob(cu_fmt("tools/%s.*", nm)); if (!strstr(g, sh)) cu_die("Glob nao achou o gemeo");
            x_done(2);
            const char *o = cu_fmt("%s", x_bash(NULL, cu_fmt("sh %s .", sh), cu_fmt(L("Run the shell twin on the project folder", "Roda o gemeo em shell na pasta do projeto")), 0));
            if (!strstr(o, "files: ")) cu_die("o script de shell nao rodou: %.200s", o);
            x_final(cu_fmt(L("I could not run %s itself: PowerShell is not installed on this machine. Its shell twin %s does the same count, and `sh %s .` prints \"%.*s\". On Windows, run the .ps1 with `powershell -ExecutionPolicy Bypass -File %s`.",
                             "Nao consegui rodar o %s em si: o PowerShell nao esta instalado nesta maquina. O gemeo dele em shell, %s, faz a mesma contagem, e o `sh %s .` imprime \"%.*s\". No Windows, rode o .ps1 com `powershell -ExecutionPolicy Bypass -File %s`."),
                           ps, sh, sh, (int)strcspn(o, "\n"), o, ps));
        } else {
            const char *r = x_read(s, ps, 0); if (!strstr(r, "-File).Count")) cu_die("o script nao e o esperado");
            x_done(2);
            const char *o = cu_fmt("%s", x_bash(NULL, "find . -maxdepth 1 -type f | wc -l", L("Count the files at the top of the project folder", "Conta os arquivos do topo da pasta do projeto"), 0));
            const int n = atoi(o);
            if (n > lim) cu_die("contagem acima do limite");
            x_final(cu_fmt(L("I could not run %s: PowerShell is not installed on this machine, and the script has no shell twin. What it does is count the files at the top of the folder, so I counted them with the shell: %d. That is under its limit of %d, so on Windows it would print `files: %d`.",
                             "Nao consegui rodar o %s: o PowerShell nao esta instalado nesta maquina, e o script nao tem gemeo em shell. O que ele faz e contar os arquivos do topo da pasta, entao contei com o shell: %d. Isso fica abaixo do limite dele, %d, entao no Windows ele imprimiria `files: %d`."), ps, n, lim, n));
        }
    } else if (kind == 14) {      /* outro script: variavel de ambiente da sessao */
        const char *stage = XV("staging", "canary", "nightly");
        x_user(cu_fmt(XV(L("write %s: it sets APP_STAGE for the PowerShell session (default %s) and prints the value", "escreve %s: ele define APP_STAGE para a sessao do PowerShell (padrao %s) e imprime o valor"),
                         L("I want a PowerShell script %s that sets the environment variable APP_STAGE, %s when nothing is given, and shows it", "quero um script PowerShell %s que define a variavel de ambiente APP_STAGE, %s quando nada e passado, e mostra ela")), ps, stage));
        x_write(s, NULL, ps, x_ps1_stage(stage));
        x_ps1_nopwsh();
        x_ps1_verify(s, ps, "param(|function Set-Stage|$env:APP_STAGE = $Name|Write-Output");
        x_final(cu_fmt(cu_fmt("%s%s", L("Wrote %s: -Stage defaults to \"%s\", Set-Stage puts it in $env:APP_STAGE and the last line prints it. The variable lives only in the session that runs the script.", "Escrevi %s: -Stage tem o padrao \"%s\", o Set-Stage coloca em $env:APP_STAGE e a ultima linha imprime. A variavel so vale na sessao que roda o script."), X_PSRUN), ps, stage, ps));
    } else if (kind == 15) {      /* outro script: confere o arquivo de configuracoes */
        x_user(cu_fmt(XV(L("write a PowerShell script %s that checks that settings.conf exists and has a port line; it prints settings ok or settings invalid", "escreve um script PowerShell %s que confere se o settings.conf existe e tem uma linha port; ele imprime settings ok ou settings invalid")), ps));
        x_write(s, NULL, ps, x_ps1_settings());
        x_ps1_nopwsh();
        x_ps1_verify(s, ps, "param(|function Test-Setting|Test-Path|Select-String|Write-Output \"settings ok\"");
        x_final(cu_fmt(cu_fmt("%s%s", L("Wrote %s: Test-Setting returns false when the file is missing and otherwise looks for a line starting with `port = `; the script prints `settings ok` or `settings invalid`.", "Escrevi %s: o Test-Setting devolve falso quando o arquivo nao existe e, se existe, procura uma linha que comeca com `port = `; o script imprime `settings ok` ou `settings invalid`."), X_PSRUN), ps, ps));
    } else if (kind == 16) {      /* ajuda em comentarios no topo: o param( continua sendo a primeira instrucao */
        x_user(cu_fmt(XV(L("add a help comment at the top of %s: a synopsis and what -Limit means", "acrescenta um comentario de ajuda no topo de %s: uma sinopse e o que o -Limit significa")), ps));
        const char *plan[3] = {cu_fmt(L("Read %s", "Ler %s"), ps), L("Add the help lines above param(", "Acrescentar as linhas de ajuda acima do param("), L("See if PowerShell is here to run it", "Ver se ha PowerShell aqui para rodar")};
        x_plan(plan, 3);
        x_read(s, ps, 0); x_done(1);
        x_edit(s, ps, "# counts the files of a folder and compares the count with a limit\nparam(", "# .SYNOPSIS\n#   Counts the files of a folder and compares the count with a limit.\n# .PARAMETER Limit\n#   The script warns when the folder has more files than this.\nparam(", 0, 0); x_done(2);
        x_ps1_nopwsh();
        x_ps1_verify(s, ps, "# .SYNOPSIS|# .PARAMETER Limit|param(|Get-ChildItem");
        x_final(cu_fmt(cu_fmt("%s%s", L("%s now starts with help comments (.SYNOPSIS and .PARAMETER Limit). They are comment lines, so `param(` is still the first statement of the script.", "O %s agora comeca com comentarios de ajuda (.SYNOPSIS e .PARAMETER Limit). Sao linhas de comentario, entao o `param(` continua sendo a primeira instrucao do script."), X_PSRUN), ps, ps));
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
