/* ---------- katana curriculum, cenarios compostos: as ferramentas raras ----------
 * x-rare: toda ferramenta que o prompt cita e que as outras familias quase nao usam ganha pelo menos cinco situacoes de
 * treino, mais as reservadas para o arquivo de validacao (hold = 1; o rodizio do treino nunca passa por elas). Situacao e
 * uma linha da tabela: um pedido com outra intencao e outra sequencia de chamadas ou outro desfecho, nao o mesmo pedido
 * com outras palavras. Tipos de linha: o uso certo (com os argumentos achados pelo ToolSearch quando a ferramenta so e
 * citada pelo nome), um uso composto com outra ferramenta, o erro do motor e a reacao, o erro seguido de outro caminho, e
 * o contraste entre agendar (ScheduleWakeup, CronCreate), rodar agora em segundo plano (Bash) e monitorar (Monitor).
 * Tudo roda no motor, numa pasta HOME propria; o que existe em volta vem de scenario_rare2.c (servidor MCP de verdade,
 * outra sessao na caixa de correio, subagente pelo gancho de teste, servidor de linguagem minimo, repositorio git, caderno).
 * Cada linha pode exigir uma prova no ultimo resultado (need) e dizer qual linha de resultado a fala final cita (quote).
 * Onde rodar de verdade nao da ou faria mal, roda o caminho de recusa/validacao do motor ou um substituto do programa de
 * fora, e a celula leva a marca "standin":
 *   - PushNotification e ReportFindings: com KATANA_UI (o marcador que a interface leria) e notify-send de mentira;
 *   - SubagentHandback: so a recusa para o agente principal (o caminho de dentro de um subagente pede modelo);
 *   - Agent sem tarefa: so a validacao do prompt. Agent com tarefa, SendMessage para agent-N e Workflow rodam com o gancho
 *     de subagente (KATANA_AGENT_FAKE=1), que devolve o que o Grep/Glob do motor acham no projeto naquela hora.
 * `KATANA_CU_RARE_TABLE=1 katana curriculum x.jsonl --families x-rare=1` imprime as situacoes por ferramenta. */

static const char *x_tsel(const char *names);
static int x_web_up(void);
typedef struct { const char *name, *args; int pre; } XRc;      /* pre: 1 espera a saida do que roda em segundo plano; 2 o usuario aprova o plano; 4 o usuario nao aprova */
typedef struct {
    const char *tool, *sel, *en, *pt;
    XRc c[6]; int nc;
    int bad;                     /* 1: o ultimo resultado e um erro do motor e a fala final reage a ele */
    const char *env;             /* variavel ligada so nesta conversa (valor 1, ou o relogio fixo do agendador) */
    const char *nen, *npt;       /* o que o modelo acrescenta depois de citar o resultado */
    int standin;
    int prep;                    /* P_*: o que existe em volta (servidor MCP de verdade, outra sessao, subagente...) */
    int hold;                    /* 1: situacao reservada para o arquivo de validacao; nunca sai no treino */
    const char *need;            /* trecho que o ultimo resultado tem de trazer (a prova); sem ele a conversa e descartada */
    const char *quote;           /* a linha citada na fala final e a que tem este trecho (padrao: a primeira do ultimo resultado) */
    const char *fen, *fpt;       /* fala final propria; {q} e a linha citada */
    int sub1, sub2;              /* P_AGENT: o que o subagente procura na primeira e na segunda rodada (x_r2_subans) */
} XRare;
static const XRare g_x_rare[] = {
    {"ScheduleWakeup", "ScheduleWakeup", "wake yourself up in five minutes to run the tests again", "se acorda daqui a cinco minutos pra rodar os testes de novo",
        {{"ScheduleWakeup", "{\"delaySeconds\": 300, \"prompt\": \"Run the tests of {name} again\", \"reason\": \"re-check the tests\"}"}}, 1, 0, "KATANA_SCHED_NOW", "", ""},
    {"ScheduleWakeup", "ScheduleWakeup", "check on the build again in ten seconds", "olha o build de novo daqui a dez segundos",
        {{"ScheduleWakeup", "{\"delaySeconds\": 10, \"prompt\": \"Check the build of {name}\", \"reason\": \"wait for the build\"}"}}, 1, 0, "KATANA_SCHED_NOW", " The engine does not wake up sooner than one minute.", " O motor nao acorda antes de um minuto."},
    {"ScheduleWakeup", "ScheduleWakeup", "cancel the wakeup you scheduled", "cancela o despertar que voce agendou",
        {{"ScheduleWakeup", "{\"stop\": true}"}}, 1, 0, "KATANA_SCHED_NOW", "", ""},
    {"CronDelete", "CronCreate,CronDelete", "schedule a reminder to run the tests every hour, then remove it again", "agenda um lembrete de rodar os testes a cada hora e depois remove ele",
        {{"CronCreate", "{\"cron\": \"0 * * * *\", \"prompt\": \"Run the tests of {name}\", \"recurring\": true}"}, {"CronDelete", "{\"id\": \"{id}\"}"}}, 2, 0, "KATANA_SCHED_NOW", "", ""},
    {"CronDelete", "CronDelete", "remove the scheduled task 0badc0de", "remove a tarefa agendada 0badc0de",
        {{"CronDelete", "{\"id\": \"0badc0de\"}"}}, 1, 1, "KATANA_SCHED_NOW", " Nothing was removed; CronList shows the ids that exist.", " Nada foi removido; o CronList mostra os ids que existem."},
    {"CronDelete", "CronList,CronDelete", "delete every scheduled task of this session", "apaga todas as tarefas agendadas desta sessao",
        {{"CronList", "{}"}}, 1, 0, "KATANA_SCHED_NOW", " So there is nothing to delete, and I did not call CronDelete.", " Entao nao ha o que apagar, e nao chamei o CronDelete."},
    {"PushNotification", "PushNotification", "notify me on the desktop that the tests of this project passed", "me avisa na area de trabalho que os testes deste projeto passaram",
        {{"PushNotification", "{\"message\": \"The tests of {name} passed\", \"title\": \"{name}\"}"}}, 1, 0, "KATANA_UI", "", "", 1},
    {"PushNotification", "PushNotification", "send me a notification saying the export is ready", "me manda uma notificacao dizendo que o export esta pronto",
        {{"PushNotification", "{\"message\": \"The export of {name} is ready\"}"}}, 1, 0, "KATANA_UI", "", "", 1},
    {"PushNotification", "PushNotification", "send an empty notification just to test", "manda uma notificacao vazia so pra testar",
        {{"PushNotification", "{\"message\": \"\"}"}}, 1, 1, "KATANA_UI", " A notification needs a text; tell me what it should say.", " Uma notificacao precisa de um texto; me diz o que ela deve dizer.", 1},
    {"EndConversation", "EndConversation", "that is all for today, you can end the conversation", "por hoje e so, pode encerrar a conversa",
        {{"EndConversation", "{\"reason\": \"the user said the work is finished for today\"}"}}, 1, 0, NULL, "", ""},
    {"EndConversation", "EndConversation", "we are done here, close this session", "terminamos aqui, fecha esta sessao",
        {{"EndConversation", "{\"reason\": \"the user asked to close the session\"}"}}, 1, 0, NULL, "", ""},
    {"EndConversation", "EndConversation", "end the conversation without giving any reason", "encerra a conversa sem dar motivo nenhum",
        {{"EndConversation", "{\"reason\": \"\"}"}}, 1, 1, NULL, " The tool wants a reason, so the conversation is still open.", " A ferramenta quer um motivo, entao a conversa continua aberta."},
    {"ReadMcpResourceTool", "ReadMcpResourceTool", "read the MCP resource notes://today from the server daybook", "le o recurso MCP notes://today do servidor daybook",
        {{"ReadMcpResourceTool", "{\"server\": \"daybook\", \"uri\": \"notes://today\"}"}}, 1, 1, NULL, " No MCP server is connected here, so there is no resource to read.", " Nao ha servidor MCP ligado aqui, entao nao ha recurso para ler."},
    {"ReadMcpResourceTool", "ListMcpResourcesTool,ReadMcpResourceTool", "show me the first resource the MCP servers offer", "me mostra o primeiro recurso que os servidores MCP oferecem",
        {{"ListMcpResourcesTool", "{}"}}, 1, 0, NULL, " With nothing listed, there is nothing for ReadMcpResourceTool to read.", " Sem nada listado, nao ha o que o ReadMcpResourceTool ler."},
    {"ReadMcpResourceTool", "ReadMcpResourceTool", "read a resource from the wiki server, any one", "le um recurso do servidor wiki, qualquer um",
        {{"ReadMcpResourceTool", "{\"server\": \"wiki\", \"uri\": \"\"}"}}, 1, 1, NULL, " I need the uri of a resource, and ListMcpResourcesTool is what lists them.", " Preciso do uri de um recurso, e quem lista e o ListMcpResourcesTool."},
    {"WaitForMcpServers", "WaitForMcpServers", "wait until the MCP servers are ready", "espera os servidores MCP ficarem prontos",
        {{"WaitForMcpServers", "{}"}}, 1, 0, NULL, "", ""},
    {"WaitForMcpServers", "WaitForMcpServers", "wait one second for the MCP server tickets", "espera um segundo pelo servidor MCP tickets",
        {{"WaitForMcpServers", "{\"servers\": [\"tickets\"], \"timeout_ms\": 1000}"}}, 1, 0, NULL, "", ""},
    {"WaitForMcpServers", "WaitForMcpServers", "wait for the servers wiki and mail, at most two seconds", "espera os servidores wiki e mail, no maximo dois segundos",
        {{"WaitForMcpServers", "{\"servers\": [\"wiki\", \"mail\"], \"timeout_ms\": 2000}"}}, 1, 0, NULL, "", ""},
    {"ReportFindings", "ReportFindings", "report as a finding that the test runner stops at the first failure", "registra como achado que os testes param na primeira falha",
        {{"ReportFindings", "{\"findings\": [{\"file\": \"{cwd}/{testrel}\", \"summary\": \"The test runner stops at the first failure\", \"failure_scenario\": \"Two functions are broken; only the first failure is printed until it is fixed.\"}]}"}}, 1, 0, "KATANA_UI", "", "", 1},
    {"ReportFindings", "ReportFindings", "report two findings: no README, and the settings are not validated", "registra dois achados: nao ha README, e as configuracoes nao sao validadas",
        {{"ReportFindings", "{\"findings\": [{\"file\": \"{cwd}/README.md\", \"summary\": \"No README\", \"failure_scenario\": \"A new contributor does not find the test command.\"}, {\"file\": \"{cwd}/settings.conf\", \"summary\": \"Settings are not validated\", \"failure_scenario\": \"A port outside the valid range is only noticed when the service starts.\"}], \"level\": \"warning\"}"}}, 1, 0, "KATANA_UI", "", "", 1},
    {"ReportFindings", "ReportFindings", "report your findings, even if you have none", "registra os seus achados, mesmo que nao tenha nenhum",
        {{"ReportFindings", "{\"findings\": []}"}}, 1, 0, "KATANA_UI", " I have no finding to report yet.", " Ainda nao tenho achado para registrar.", 1},
    {"ListAgents", "ListAgents", "which other katana sessions are running on this machine?", "que outras sessoes do katana estao rodando nesta maquina?",
        {{"ListAgents", "{}"}}, 1, 0, NULL, "", ""},
    {"ListAgents", "ListAgents,SendMessage", "tell every other katana session that the tests are green", "avisa todas as outras sessoes do katana que os testes estao verdes",
        {{"ListAgents", "{}"}}, 1, 0, NULL, " There is nobody to send the message to, so I did not call SendMessage.", " Nao ha ninguem para receber a mensagem, entao nao chamei o SendMessage."},
    {"ListAgents", "ListAgents", "is any sub-agent of yours still open?", "tem algum subagente seu ainda aberto?",
        {{"ListAgents", "{}"}}, 1, 0, NULL, "", ""},
    {"SendMessage", "SendMessage", "send the katana session called builder the message: tests are green", "manda para a sessao do katana chamada builder a mensagem: testes verdes",
        {{"SendMessage", "{\"to\": \"builder\", \"message\": \"The tests of {name} are green.\"}"}}, 1, 1, NULL, " The message was not delivered.", " A mensagem nao foi entregue."},
    {"SendMessage", "SendMessage", "ask the sub-agent scout to also check the tools folder", "pede ao subagente scout para olhar tambem a pasta tools",
        {{"SendMessage", "{\"to\": \"scout\", \"message\": \"Also check the tools folder of {name}.\", \"summary\": \"check tools/\"}"}}, 1, 1, NULL, " No sub-agent or session has that name; ListAgents shows who exists.", " Nenhum subagente ou sessao tem esse nome; o ListAgents mostra quem existe."},
    {"SendMessage", "SendMessage", "send an empty message to the session builder", "manda uma mensagem vazia para a sessao builder",
        {{"SendMessage", "{\"to\": \"builder\", \"message\": \"\"}"}}, 1, 1, NULL, " Tell me what the message should say.", " Me diz o que a mensagem deve dizer."},
    {"SubagentHandback", "SubagentHandback", "hand your report back to whoever started you", "entrega o seu relatorio para quem te iniciou",
        {{"SubagentHandback", "{\"message\": \"Report: the project {name} has its tests in {testrel}.\"}"}}, 1, 1, NULL, " I am not a sub-agent in this conversation: you started me, so my report goes to you directly.", " Nao sou um subagente nesta conversa: quem me iniciou foi voce, entao o relatorio vai direto para voce.", 1},
    {"SubagentHandback", "SubagentHandback", "use the handback tool to finish", "usa a ferramenta de devolucao para terminar",
        {{"SubagentHandback", "{\"message\": \"Finished: nothing was changed in {name}.\"}"}}, 1, 1, NULL, " That tool only ends the run of a sub-agent; here I just answer you.", " Essa ferramenta so encerra a rodada de um subagente; aqui eu so respondo a voce.", 1},
    {"SubagentHandback", "SubagentHandback", "hand back an empty report", "devolve um relatorio vazio",
        {{"SubagentHandback", "{\"message\": \"\"}"}}, 1, 1, NULL, " And I am the main agent here, so there is nobody to hand back to.", " E aqui eu sou o agente principal, entao nao ha a quem devolver.", 1},
    {"Agent", "Agent", "start a sub-agent, I will tell you the task later", "inicia um subagente, depois eu te digo a tarefa",
        {{"Agent", "{\"description\": \"Task to be given later\", \"prompt\": \"\"}"}}, 1, 1, NULL, " A sub-agent starts with its whole task, so tell me the task first.", " Um subagente comeca com a tarefa inteira, entao me diz a tarefa antes.", 1},
    {"Agent", "Agent", "have a sub-agent look", "manda um subagente olhar",
        {{"Agent", "{\"description\": \"Look\", \"prompt\": \"look\"}"}}, 1, 1, NULL, " Tell me what it should look for and what it should report.", " Me diz o que ele deve procurar e o que deve relatar.", 1},
    {"Agent", "Agent", "delegate it", "delega isso",
        {{"Agent", "{\"description\": \"Delegated task\", \"prompt\": \"do it\"}"}}, 1, 1, NULL, " I do not know what \"it\" is yet: say what the sub-agent should do.", " Ainda nao sei o que e \"isso\": diz o que o subagente deve fazer.", 1},
    {"Workflow", "Workflow", "run a workflow with one phase and one agent that lists the test files", "roda um workflow com uma fase e um agente que lista os arquivos de teste",
        {{"Workflow", "{\"script\": \"{\\\"phases\\\": [{\\\"name\\\": \\\"look\\\", \\\"agents\\\": [{\\\"name\\\": \\\"lister\\\", \\\"prompt\\\": \\\"List the test files of the project.\\\"}]}]}\"}"}}, 1, 0, "KATANA_AGENT_FAKE", "", "", 1},
    {"Workflow", "Workflow", "run the saved workflow called nightly", "roda o workflow salvo chamado nightly",
        {{"Workflow", "{\"name\": \"nightly\"}"}}, 1, 1, "KATANA_AGENT_FAKE", " No workflow with that name is saved here.", " Nao ha workflow salvo com esse nome aqui.", 1},
    {"Workflow", "Workflow", "run a workflow from the script file tools/flow.json", "roda um workflow a partir do arquivo tools/flow.json",
        {{"Workflow", "{\"scriptPath\": \"{cwd}/tools/flow.json\"}"}}, 1, 1, "KATANA_AGENT_FAKE", " That file does not exist in this project.", " Esse arquivo nao existe neste projeto.", 1},
                                                    /* agendamento: uma vez e recorrente, dias uteis, listar, cancelar, expressao invalida (a hora e a local da maquina: a ferramenta nao tem argumento de fuso) */
    {"CronCreate", "CronCreate", "tomorrow at 9:30, once, remind yourself to run the tests", "amanha as 9:30, uma vez so, se lembra de rodar os testes",
        {{"CronCreate", "{\"cron\": \"30 9 * * *\", \"prompt\": \"Run the tests of {name}\", \"recurring\": false}"}}, 1, 0, "KATANA_SCHED_NOW", " The time is this machine's local time.", " A hora e a hora local desta maquina."},
    {"CronCreate", "CronCreate", "every weekday at 9 in the morning, check the settings of this project", "todo dia util as 9 da manha, confere as configuracoes deste projeto",
        {{"CronCreate", "{\"cron\": \"0 9 * * 1-5\", \"prompt\": \"Check the settings of {name}\", \"recurring\": true}"}}, 1, 0, "KATANA_SCHED_NOW", " It only lives while this session is open.", " So vale enquanto esta sessao estiver aberta."},
    {"CronCreate", "CronCreate", "schedule the tests for every monday", "agenda os testes para toda segunda",
        {{"CronCreate", "{\"cron\": \"every monday\", \"prompt\": \"Run the tests of {name}\", \"recurring\": true}"}}, 1, 1, "KATANA_SCHED_NOW", " Nothing was scheduled. Tell me the hour and I will write it as five cron fields.", " Nada foi agendado. Me diz a hora que eu escrevo nos cinco campos do cron."},
    {"CronList", "CronCreate,CronList,CronDelete", "schedule a test run every 15 minutes, show me the list, then cancel it", "agenda uma rodada de testes a cada 15 minutos, me mostra a lista e depois cancela",
        {{"CronCreate", "{\"cron\": \"*/15 * * * *\", \"prompt\": \"Run the tests of {name}\", \"recurring\": true}"}, {"CronList", "{}"}, {"CronDelete", "{\"id\": \"{id}\"}"}}, 3, 0, "KATANA_SCHED_NOW", "", ""},
    /* PowerShell: nesta maquina nao esta instalado; o modelo diz e faz com o Bash, ou oferece instalar; um script .ps1 se escreve com Write */
    {"PowerShell", "PowerShell", "use PowerShell to list the files of this folder", "usa o PowerShell pra listar os arquivos desta pasta",
        {{"PowerShell", "{\"command\": \"Get-ChildItem\", \"description\": \"List the files\"}"}, {"Bash", "{\"command\": \"ls\", \"description\": \"List the files of {name} with the shell\"}"}}, 2, 0, NULL, " That is the first entry of the listing above: PowerShell is not installed on this machine, so I listed the files with Bash.", " Essa e a primeira entrada da listagem acima: o PowerShell nao esta instalado nesta maquina, entao listei os arquivos com o Bash."},
    {"PowerShell", "PowerShell", "run Get-Date in PowerShell", "roda Get-Date no PowerShell",
        {{"PowerShell", "{\"command\": \"Get-Date\"}"}}, 1, 1, NULL, " I can install PowerShell (the pwsh package) if you want, or give you the date with Bash.", " Posso instalar o PowerShell (o pacote pwsh) se voce quiser, ou te dar a data com o Bash."},
    {"PowerShell", "PowerShell", "count the lines of the test file with PowerShell", "conta as linhas do arquivo de testes com o PowerShell",
        {{"PowerShell", "{\"command\": \"(Get-Content {testrel}).Count\"}"}, {"Bash", "{\"command\": \"wc -l {testrel}\", \"description\": \"Count the lines of the test file\"}"}}, 2, 0, NULL, " PowerShell is not installed here, so the count comes from Bash.", " O PowerShell nao esta instalado aqui, entao a contagem vem do Bash."},
    {"Write", NULL, "write me a PowerShell script tools/list.ps1 that lists the files here; do not run it", "escreve pra mim um script PowerShell tools/list.ps1 que lista os arquivos daqui; nao roda",
        {{"Write", "{\"file_path\": \"{cwd}/tools/list.ps1\", \"content\": \"# lists the files of the project folder\\nGet-ChildItem -Path (Split-Path -Parent $PSScriptRoot) | Select-Object Name, Length\\n\"}"}}, 1, 0, NULL, " I wrote the script and did not run it, as you asked.", " Escrevi o script e nao rodei, como voce pediu."},
    /* Task*: so quando o usuario pede tarefas numeradas com detalhes ou cita a ferramenta; o plano do proprio trabalho continua sendo TodoWrite */
    /* ---- agendar x rodar agora em segundo plano x monitorar: o detalhe do pedido decide ---- */
    {"ScheduleWakeup", "ScheduleWakeup", "start tools/watch.sh in the background now, and wake yourself up in five minutes to look at its output", "sobe o tools/watch.sh em segundo plano agora, e se acorda daqui a cinco minutos pra olhar a saida dele",
        {{"Bash", "{\"command\": \"sh tools/watch.sh\", \"run_in_background\": true, \"description\": \"Start the watcher in the background\"}"}, {"ScheduleWakeup", "{\"delaySeconds\": 300, \"prompt\": \"Read the output of tools/watch.sh in {name}\", \"reason\": \"look at the watcher output\"}"}}, 2, 0, "KATANA_SCHED_NOW", "", "", .prep = P_BG,
        .fen = "Two separate things were done. The script is running now in the background (task {bg}). And the wakeup is set: \"{q}\".", .fpt = "Foram duas coisas separadas. O script esta rodando agora em segundo plano (tarefa {bg}). E o despertar esta marcado: \"{q}\"."},
    {"ScheduleWakeup", "ScheduleWakeup", "run `{testcmd}` now, and again in half an hour", "roda `{testcmd}` agora, e de novo daqui a meia hora",
        {{"Bash", "{\"command\": \"{testcmd}\", \"description\": \"Run the tests now\"}"}, {"ScheduleWakeup", "{\"delaySeconds\": 1800, \"prompt\": \"Run {testcmd} in {name} again\", \"reason\": \"second test run\"}"}}, 2, 0, "KATANA_SCHED_NOW", "", "", .quote = " passed, ",
        .fen = "The run of now ended with \"{q}\". The second run is not a background task: it is a wakeup set for 30 minutes from now, in this session.", .fpt = "A rodada de agora terminou com \"{q}\". A segunda rodada nao e uma tarefa em segundo plano: e um despertar marcado para daqui a 30 minutos, nesta sessao."},
    {"ScheduleWakeup", "ScheduleWakeup", "in two hours, remind yourself to read the release notes", "daqui a duas horas, se lembra de ler as notas da versao",
        {{"ScheduleWakeup", "{\"delaySeconds\": 7200, \"prompt\": \"Read the release notes of {name}\", \"reason\": \"release notes\"}"}}, 1, 0, "KATANA_SCHED_NOW", " A wakeup waits at most one hour, so check the time it gives; for a fixed hour of the day CronCreate is the tool.", " Um despertar espera no maximo uma hora, entao confere o horario que ele da; para uma hora fixa do dia a ferramenta e o CronCreate."},
    {"CronCreate", "CronCreate", "run tools/watch.sh every 30 minutes while this session is open", "roda o tools/watch.sh a cada 30 minutos enquanto esta sessao estiver aberta",
        {{"CronCreate", "{\"cron\": \"*/30 * * * *\", \"prompt\": \"Run sh tools/watch.sh in {name}\", \"recurring\": true}"}}, 1, 0, "KATANA_SCHED_NOW", "", "", .prep = P_BG,
        .fen = "Scheduled, not started: \"{q}\". Nothing is running now; each run begins at the next half hour and the schedule ends with this session.", .fpt = "Agendado, nao iniciado: \"{q}\". Nada esta rodando agora; cada rodada comeca na proxima meia hora e o agendamento acaba com esta sessao."},
    {"TaskStop", "TaskStop", "start tools/watch.sh in the background and stop it right away, I only want to know that it starts", "sobe o tools/watch.sh em segundo plano e para logo em seguida, so quero saber se ele sobe",
        {{"Bash", "{\"command\": \"sh tools/watch.sh\", \"run_in_background\": true, \"description\": \"Start the watcher in the background\"}"}, {"TaskStop", "{\"task_id\": \"{bg}\"}"}}, 2, 0, NULL, "", "", .prep = P_BG, .need = "Successfully stopped",
        .fen = "It starts: the engine gave it the background task {bg}, and then \"{q}\". Nothing is left running.", .fpt = "Ele sobe: o motor deu a ele a tarefa em segundo plano {bg}, e depois \"{q}\". Nada ficou rodando."},
    {"TaskStop", "TaskStop", "stop the background task 7", "para a tarefa em segundo plano 7",
        {{"TaskStop", "{\"task_id\": \"7\"}"}}, 1, 1, NULL, " I started no background task in this conversation, so there is nothing to stop.", " Nao subi nenhuma tarefa em segundo plano nesta conversa, entao nao ha o que parar."},
    {"Monitor", "Monitor", "monitor `{testcmd}` and give me the totals line when it ends", "monitora `{testcmd}` e me da a linha dos totais quando terminar",
        {{"Monitor", "{\"command\": \"{testcmd}\", \"description\": \"Watch the test run\"}"}, {"Read", "{\"file_path\": \"{log}\"}", 1}}, 2, 0, NULL, "", "", .quote = " passed, ",
        .fen = "The watched run ended with \"{q}\". It finished on its own, so there is nothing to stop.", .fpt = "A rodada monitorada terminou com \"{q}\". Ela acabou sozinha, entao nao ha o que parar."},
    {"Monitor", "Monitor", "monitor a command for me, I will tell you which one in a moment", "monitora um comando pra mim, ja te digo qual",
        {{"Monitor", "{\"command\": \"\", \"description\": \"Watch a command\"}"}}, 1, 1, NULL, " Tell me the command and I will watch it.", " Me diz o comando que eu monitoro."},
    {"Monitor", "Monitor", "watch the output of tools/deploy.sh and tell me what it prints first", "monitora a saida do tools/deploy.sh e me diz o que ele imprime primeiro",
        {{"Monitor", "{\"command\": \"sh tools/deploy.sh\", \"description\": \"Watch the deploy script\"}"}, {"Read", "{\"file_path\": \"{log}\"}", 1}}, 2, 0, NULL, "", "", .quote = "deploy.sh",
        .fen = "There is no tools/deploy.sh in this project: the watched command only printed \"{q}\" and ended. Nothing is being watched now.", .fpt = "Nao existe tools/deploy.sh neste projeto: o comando monitorado so imprimiu \"{q}\" e terminou. Nada esta sendo monitorado agora."},
    {"Monitor", "Monitor,TaskStop", "do not schedule anything: watch tools/watch.sh right now, show me its first line and stop watching", "nao agenda nada: monitora o tools/watch.sh agora mesmo, me mostra a primeira linha e para de monitorar",
        {{"Monitor", "{\"command\": \"sh tools/watch.sh\", \"description\": \"Watch the script\"}"}, {"Read", "{\"file_path\": \"{log}\"}", 1}, {"TaskStop", "{\"task_id\": \"{bg}\"}"}}, 3, 0, NULL, "", "", .prep = P_BG, .need = "Successfully stopped", .quote = "watching ",
        .fen = "Its first line was \"{q}\". I watched it live (task {bg}) and stopped it; nothing was scheduled.", .fpt = "A primeira linha foi \"{q}\". Monitorei ao vivo (tarefa {bg}) e parei; nada foi agendado."},
    {"CronList", "CronList", "is anything scheduled in this session right now?", "tem alguma coisa agendada nesta sessao agora?",
        {{"CronList", "{}"}}, 1, 0, "KATANA_SCHED_NOW", "", "", .fen = "No. CronList answers \"{q}\", and I set no wakeup in this conversation either.", .fpt = "Nao. O CronList responde \"{q}\", e tambem nao marquei nenhum despertar nesta conversa."},
    {"CronList", "CronCreate,CronList", "schedule two things for this session: `{testcmd}` every hour, and a look at settings every day at 8; then show me the list", "agenda duas coisas nesta sessao: `{testcmd}` a cada hora, e uma olhada nas configuracoes todo dia as 8; depois me mostra a lista",
        {{"CronCreate", "{\"cron\": \"0 * * * *\", \"prompt\": \"Run {testcmd} in {name}\", \"recurring\": true}"}, {"CronCreate", "{\"cron\": \"0 8 * * *\", \"prompt\": \"Look at the settings of {name}\", \"recurring\": true}"}, {"CronList", "{}"}}, 3, 0, "KATANA_SCHED_NOW", "", "", .quote = "0 8 * * *",
        .fen = "Both are scheduled; the list shows two tasks, for example \"{q}\". They only run while this session is open.", .fpt = "As duas estao agendadas; a lista mostra duas tarefas, por exemplo \"{q}\". Elas so rodam enquanto esta sessao estiver aberta."},
    {"CronDelete", "CronCreate,CronDelete,CronList", "schedule `{testcmd}` for 18:00 today, once; then cancel it and prove that the list is empty", "agenda `{testcmd}` para as 18:00 de hoje, uma vez; depois cancela e prova que a lista ficou vazia",
        {{"CronCreate", "{\"cron\": \"0 18 * * *\", \"prompt\": \"Run {testcmd} in {name}\", \"recurring\": false}"}, {"CronDelete", "{\"id\": \"{id}\"}"}, {"CronList", "{}"}}, 3, 0, "KATANA_SCHED_NOW", "", "",
        .fen = "Created and cancelled (task {id}). The proof is the list afterwards: \"{q}\".", .fpt = "Criada e cancelada (tarefa {id}). A prova e a lista depois: \"{q}\"."},
    {"CronDelete", "CronDelete", "cancel the scheduled task, you know which one", "cancela a tarefa agendada, voce sabe qual",
        {{"CronDelete", "{\"id\": \"\"}"}}, 1, 1, "KATANA_SCHED_NOW", " I scheduled nothing in this conversation, so I do not know which one; give me its id or ask me to list them.", " Nao agendei nada nesta conversa, entao nao sei qual; me passa o id ou pede pra eu listar."},
    {"CronList", "ScheduleWakeup,CronList", "set a wakeup for ten minutes from now and tell me whether it shows up in the list of scheduled tasks", "marca um despertar para daqui a dez minutos e me diz se ele aparece na lista de tarefas agendadas",
        {{"ScheduleWakeup", "{\"delaySeconds\": 600, \"prompt\": \"Check on {name}\", \"reason\": \"ten minute check\"}"}, {"CronList", "{}"}}, 2, 0, "KATANA_SCHED_NOW", "", "",
        .fen = "The wakeup is set. What CronList shows after it: \"{q}\".", .fpt = "O despertar esta marcado. O que o CronList mostra depois dele: \"{q}\"."},
    /* ---- avisos e achados ---- */
    {"PushNotification", "PushNotification", "run `{testcmd}` and notify me on the desktop with the totals", "roda `{testcmd}` e me avisa na area de trabalho com os totais",
        {{"Bash", "{\"command\": \"{testcmd}\", \"description\": \"Run the tests\"}"}, {"PushNotification", "{\"message\": \"{name}: {totals}\", \"title\": \"Tests\"}"}}, 2, 0, "KATANA_UI", "", "", 1, .quote = " passed, ",
        .fen = "The tests ended with \"{q}\", and that line is what the notification says.", .fpt = "Os testes terminaram com \"{q}\", e e essa linha que a notificacao diz."},
    {"PushNotification", "PushNotification", "notify me with the title Deploy and the text: staging is updated", "me notifica com o titulo Deploy e o texto: staging is updated",
        {{"PushNotification", "{\"message\": \"staging is updated\", \"title\": \"Deploy\"}"}}, 1, 0, "KATANA_UI", " I only sent the text you gave; I did not check staging.", " So mandei o texto que voce deu; nao conferi o staging.", 1},
    {"ReportFindings", "ReportFindings", "read {src} and report as an error-level finding that it has no usage example", "le o {src} e registra como achado de nivel erro que ele nao tem exemplo de uso",
        {{"Read", "{\"file_path\": \"{cwd}/{src}\"}"}, {"ReportFindings", "{\"findings\": [{\"file\": \"{cwd}/{src}\", \"line\": 1, \"summary\": \"No usage example in the file\", \"failure_scenario\": \"A reader has to open the tests to learn how {fn} is called.\"}], \"level\": \"error\"}"}}, 2, 0, "KATANA_UI", " I read the file first: it has the functions and no example.", " Li o arquivo antes: ele tem as funcoes e nenhum exemplo.", 1},
    {"ReportFindings", "ReportFindings", "report a finding for {testrel}, I will give you the details later", "registra um achado para o {testrel}, os detalhes eu te passo depois",
        {{"ReportFindings", "{\"findings\": [{\"file\": \"{cwd}/{testrel}\", \"summary\": \"\", \"failure_scenario\": \"\"}]}"}}, 1, 1, "KATANA_UI", " A finding needs to say what is wrong; give me that and I will report it.", " Um achado precisa dizer o que esta errado; me passa isso que eu registro.", 1},
    /* ---- encerrar ---- */
    {"EndConversation", "EndConversation", "run `{testcmd}` one last time and then end the conversation", "roda `{testcmd}` uma ultima vez e depois encerra a conversa",
        {{"Bash", "{\"command\": \"{testcmd}\", \"description\": \"Last test run\"}"}, {"EndConversation", "{\"reason\": \"the user asked to end after the last test run\"}"}}, 2, 0, NULL, "", "", .quote = " passed, ",
        .fen = "Last run: \"{q}\". The conversation is ended, as you asked.", .fpt = "Ultima rodada: \"{q}\". A conversa esta encerrada, como voce pediu."},
    {"EndConversation", "EndConversation", "tell me how many test files this project has and then close the session", "me diz quantos arquivos de teste este projeto tem e depois fecha a sessao",
        {{"Bash", "{\"command\": \"find . -name 'test_*' -type f | sort\", \"description\": \"List the test files\"}"}, {"EndConversation", "{\"reason\": \"the user asked to close the session after the answer\"}"}}, 2, 0, NULL, "", "", .quote = "test_",
        .fen = "One test file: {q}. The session is closed now.", .fpt = "Um arquivo de teste: {q}. A sessao esta fechada agora."},
    /* ---- recursos MCP de um servidor de verdade ---- */
    {"ListMcpResourcesTool", "ListMcpResourcesTool", "which resources do the MCP servers of this project offer?", "que recursos os servidores MCP deste projeto oferecem?",
        {{"ListMcpResourcesTool", "{}"}}, 1, 0, NULL, "", "", .prep = P_MCP, .need = "notes://logo", .quote = "notes://release-checklist",
        .fen = "The server {srv} offers four: notes://today, notes://release-checklist, notes://decisions and notes://logo. One line of the list: \"{q}\".", .fpt = "O servidor {srv} oferece quatro: notes://today, notes://release-checklist, notes://decisions e notes://logo. Uma linha da lista: \"{q}\"."},
    {"ReadMcpResourceTool", "ListMcpResourcesTool,ReadMcpResourceTool", "read today's notes from the {srv} server", "le as notas de hoje do servidor {srv}",
        {{"ListMcpResourcesTool", "{\"server\": \"{srv}\"}"}, {"ReadMcpResourceTool", "{\"server\": \"{srv}\", \"uri\": \"notes://today\"}"}}, 2, 0, NULL, "", "", .prep = P_MCP, .quote = "Today:",
        .fen = "The resource is notes://today (I took the uri from the server's list). It says: \"{q}\". It also says nothing is blocked.", .fpt = "O recurso e o notes://today (peguei o uri na lista do servidor). Ele diz: \"{q}\". Diz tambem que nada esta bloqueado."},
    {"ReadMcpResourceTool", "ReadMcpResourceTool", "read the MCP resource notes://decisions from {srv}", "le o recurso MCP notes://decisions do {srv}",
        {{"ReadMcpResourceTool", "{\"server\": \"{srv}\", \"uri\": \"notes://decisions\"}"}}, 1, 0, NULL, "", "", .prep = P_MCP, .quote = "Whole numbers",
        .fen = "notes://decisions says: \"{q}\".", .fpt = "O notes://decisions diz: \"{q}\"."},
    {"ReadMcpResourceTool", "ReadMcpResourceTool", "what is inside notes://logo on the {srv} server?", "o que tem dentro do notes://logo no servidor {srv}?",
        {{"ReadMcpResourceTool", "{\"server\": \"{srv}\", \"uri\": \"notes://logo\"}"}}, 1, 0, NULL, "", "", .prep = P_MCP, .quote = "binary content",
        .fen = "It is an image, not text: the tool reports \"{q}\" and does not show the bytes.", .fpt = "E uma imagem, nao texto: a ferramenta informa \"{q}\" e nao mostra os bytes."},
    {"ReadMcpResourceTool", "ReadMcpResourceTool,ListMcpResourcesTool", "read notes://release from {srv}", "le o notes://release do {srv}",
        {{"ReadMcpResourceTool", "{\"server\": \"{srv}\", \"uri\": \"notes://release\"}"}, {"ListMcpResourcesTool", "{\"server\": \"{srv}\"}"}, {"ReadMcpResourceTool", "{\"server\": \"{srv}\", \"uri\": \"notes://release-checklist\"}"}}, 3, 0, NULL, "", "", .prep = P_MCP, .quote = "1. run the tests",
        .fen = "There is no notes://release on {srv}; its list has notes://release-checklist, so I read that one. It starts with \"{q}\" and has four steps.", .fpt = "Nao existe notes://release no {srv}; a lista dele tem notes://release-checklist, entao li esse. Ele comeca com \"{q}\" e tem quatro passos."},
    {"WaitForMcpServers", "WaitForMcpServers", "wait for the MCP servers and tell me the state of each", "espera os servidores MCP e me diz o estado de cada um",
        {{"WaitForMcpServers", "{}"}}, 1, 0, NULL, "", "", .prep = P_MCP, .need = "1 connected, 0 failed", .quote = ": connected",
        .fen = "One server, and it is up: \"{q}\".", .fpt = "Um servidor, e ele esta no ar: \"{q}\"."},
    {"WaitForMcpServers", "WaitForMcpServers", "are all the MCP servers of this project up?", "todos os servidores MCP deste projeto estao no ar?",
        {{"WaitForMcpServers", "{}"}}, 1, 0, NULL, "", "", .prep = P_MCPDOWN, .need = "1 connected, 1 failed", .quote = "archive: failed",
        .fen = "No. {srv} is connected, but \"{q}\". That command is not installed on this machine, so the tools and resources of archive are not available.", .fpt = "Nao. O {srv} esta ligado, mas \"{q}\". Esse comando nao esta instalado nesta maquina, entao as ferramentas e os recursos do archive nao estao disponiveis."},
    {"ReadMcpResourceTool", "ReadMcpResourceTool", "read notes://old from the archive server", "le o notes://old do servidor archive",
        {{"ReadMcpResourceTool", "{\"server\": \"archive\", \"uri\": \"notes://old\"}"}}, 1, 1, NULL, " The other server, {srv}, is connected, if the note is there.", " O outro servidor, o {srv}, esta ligado, se a nota estiver la.", .prep = P_MCPDOWN},
    {"ListMcpResourcesTool", "ListMcpResourcesTool", "list the resources of the MCP server wiki", "lista os recursos do servidor MCP wiki",
        {{"ListMcpResourcesTool", "{\"server\": \"wiki\"}"}}, 1, 1, NULL, " I can list the resources of {srv} instead.", " Posso listar os recursos do {srv} no lugar.", .prep = P_MCP},
    {"ListMcpResourcesTool", "ListMcpResourcesTool,ReadMcpResourceTool", "does the {srv} server have a release checklist? if it does, how many steps?", "o servidor {srv} tem um checklist de release? se tiver, quantos passos?",
        {{"ListMcpResourcesTool", "{\"server\": \"{srv}\"}"}, {"ReadMcpResourceTool", "{\"server\": \"{srv}\", \"uri\": \"notes://release-checklist\"}"}}, 2, 0, NULL, "", "", .prep = P_MCP, .quote = "4. ",
        .fen = "Yes: notes://release-checklist, with four steps. The last one is \"{q}\".", .fpt = "Tem: notes://release-checklist, com quatro passos. O ultimo e \"{q}\"."},
    /* ---- outra sessao do katana de verdade na caixa de correio; subagente que devolve o que a busca acha ---- */
    {"ListAgents", "ListAgents", "is there another katana session on this machine right now?", "tem outra sessao do katana nesta maquina agora?",
        {{"ListAgents", "{}"}}, 1, 0, NULL, "", "", .prep = P_MAIL, .quote = "name: builder",
        .fen = "Yes, one: \"{q}\". It can receive messages by that name or by its pid.", .fpt = "Tem, uma: \"{q}\". Ela recebe mensagens por esse nome ou pelo pid."},
    {"SendMessage", "SendMessage", "tell the katana session builder that the tests of {name} are green", "avisa a sessao do katana builder que os testes de {name} estao verdes",
        {{"SendMessage", "{\"to\": \"builder\", \"message\": \"The tests of {name} are green.\", \"summary\": \"tests are green\"}"}}, 1, 0, NULL, "", "", .prep = P_MAIL, .need = "Message sent to session",
        .fen = "Sent. The engine says: \"{q}\". So builder sees it when it is idle, and I get no reply here.", .fpt = "Enviado. O motor diz: \"{q}\". Entao o builder ve quando estiver ocioso, e eu nao recebo resposta aqui."},
    {"SendMessage", "ListAgents,SendMessage", "send every other katana session on this machine the message: freeze at 17:00", "manda para todas as outras sessoes do katana nesta maquina a mensagem: freeze at 17:00",
        {{"ListAgents", "{}"}, {"SendMessage", "{\"to\": \"builder\", \"message\": \"freeze at 17:00\"}"}}, 2, 0, NULL, "", "", .prep = P_MAIL, .need = "Message sent to session",
        .fen = "There is one other session, builder, and it has the message now: \"{q}\".", .fpt = "Ha uma outra sessao, a builder, e ela ja tem a mensagem: \"{q}\"."},
    {"SendMessage", "SendMessage,ListAgents", "message the katana session deployer: start the rollout", "manda para a sessao do katana deployer: start the rollout",
        {{"SendMessage", "{\"to\": \"deployer\", \"message\": \"start the rollout\"}"}, {"ListAgents", "{}"}}, 2, 0, NULL, "", "", .prep = P_MAIL, .quote = "name: builder",
        .fen = "Not sent: no session is called deployer. The only other session is \"{q}\". Tell me if the message should go to builder.", .fpt = "Nao foi enviada: nenhuma sessao se chama deployer. A unica outra sessao e \"{q}\". Me diz se a mensagem deve ir para a builder."},
    {"Agent", "Agent", "have a sub-agent find every line that mentions {fn} and tell me how many there are", "manda um subagente achar toda linha que cita {fn} e me diz quantas sao",
        {{"Agent", "{\"description\": \"Find mentions of {fn}\", \"prompt\": \"Search every file of the project for the text {fn}. Report how many lines mention it and list them as path:line:text. The project is at {cwd}.\"}"}}, 1, 0, NULL, "", "", .prep = P_AGENT, .sub1 = 1, .need = "{fn} is mentioned on {n1} lines",
        .fen = "The sub-agent found {n1} lines that mention `{fn}`; its list is in the result above, and it is agent-1 if you want me to ask it more.", .fpt = "O subagente achou {n1} linhas que citam `{fn}`; a lista dele esta no resultado acima, e ele e o agent-1 se voce quiser que eu pergunte mais."},
    {"Agent", "Agent,SendMessage", "delegate to a sub-agent: list the test files. Then ask the same sub-agent whether any line of the project has a TODO mark", "delega a um subagente: listar os arquivos de teste. Depois pergunta ao mesmo subagente se alguma linha do projeto tem marca TODO",
        {{"Agent", "{\"description\": \"List the test files\", \"prompt\": \"List the test files of the project (names starting with test_). Report how many there are and their paths. The project is at {cwd}.\"}"}, {"SendMessage", "{\"to\": \"agent-1\", \"message\": \"Also search every file of the project for the text TODO and report the lines that have it.\", \"summary\": \"search for TODO marks\"}"}}, 2, 0, NULL, "", "", .prep = P_AGENT, .sub1 = 2, .sub2 = 3, .need = "TODO",
        .fen = "First answer of the sub-agent: {n1} test file. Second answer, from the same agent-1 continued with my message: \"{q}\".", .fpt = "Primeira resposta do subagente: {n1} arquivo de teste. Segunda resposta, do mesmo agent-1 continuado com a minha mensagem: \"{q}\".", .quote = "TODO"},
    {"ListAgents", "Agent,ListAgents", "run a sub-agent that counts the source files, then show me the agents of this session", "roda um subagente que conta os arquivos de codigo, depois me mostra os agentes desta sessao",
        {{"Agent", "{\"description\": \"Count the source files\", \"prompt\": \"List the source files of the project (not the tests). Report how many there are and their paths. The project is at {cwd}.\"}"}, {"ListAgents", "{}"}}, 2, 0, NULL, "", "", .prep = P_AGENT, .sub1 = 4, .quote = "agent-1",
        .fen = "The sub-agent counted {n1} source files. The session now lists it as finished: \"{q}\".", .fpt = "O subagente contou {n1} arquivos de codigo. A sessao agora mostra ele como terminado: \"{q}\"."},
    {"SubagentHandback", "SubagentHandback", "when you finish counting the test files, hand the count back with the handback tool", "quando terminar de contar os arquivos de teste, devolve a contagem com a ferramenta de devolucao",
        {{"Bash", "{\"command\": \"find . -name 'test_*' -type f | sort\", \"description\": \"List the test files\"}"}, {"SubagentHandback", "{\"message\": \"The project {name} has one test file: {testrel}.\"}"}}, 2, 1, NULL, " So here is the count directly: one test file, {testrel}.", " Entao a contagem vai direto: um arquivo de teste, {testrel}.", 1},
    {"SubagentHandback", "SubagentHandback", "you are a sub-agent, right? hand back what you know about this project", "voce e um subagente, certo? devolve o que voce sabe deste projeto",
        {{"SubagentHandback", "{\"message\": \"Project {name}: tests in {testrel}.\"}"}}, 1, 1, NULL, " So no: in this conversation I am the main agent and you are the one I answer to.", " Entao nao: nesta conversa eu sou o agente principal e e a voce que eu respondo.", 1},
    /* ---- workflow ---- */
    {"Workflow", "Workflow", "run the workflow of tools/flow.json", "roda o workflow do tools/flow.json",
        {{"Workflow", "{\"scriptPath\": \"{cwd}/tools/flow.json\"}"}}, 1, 0, NULL, " The file has two phases, look and count, and each ran one agent: {n1} test file, {n2} source files.", " O arquivo tem duas fases, look e count, e cada uma rodou um agente: {n1} arquivo de teste, {n2} arquivos de codigo.", .prep = P_FLOW, .sub1 = 2, .sub2 = 4},
    {"Workflow", "Workflow", "run a workflow with this script: {\"phases\": [}", "roda um workflow com este roteiro: {\"phases\": [}",
        {{"Workflow", "{\"script\": \"{\\\"phases\\\": [}\"}"}}, 1, 1, "KATANA_AGENT_FAKE", " The script you gave is not complete JSON: the list of phases is never closed.", " O roteiro que voce deu nao e um JSON completo: a lista de fases nao fecha.", 1},
    {"Workflow", "Workflow", "run a workflow with no script at all", "roda um workflow sem roteiro nenhum",
        {{"Workflow", "{}"}}, 1, 1, "KATANA_AGENT_FAKE", " Give me the script, a file with it, or the name of a saved one.", " Me passa o roteiro, um arquivo com ele ou o nome de um salvo.", 1},
    /* ---- sessoes de shell de verdade ---- */
                    /* ---- pacotes, logs, receitas ---- */
                            /* ---- tarefas numeradas ---- */
    /* ---- git de verdade (repositorio com um commit) ---- */
                    /* ---- memoria ---- */
                        /* ---- baixar arquivos (servidor local de paginas) ---- */
                    /* ---- servidor de linguagem minimo, de verdade ---- */
    {"LSP", "LSP", "ask the language server where {fn2} is defined", "pergunta ao servidor de linguagem onde o {fn2} e definido",
        {{"LSP", "{\"operation\": \"workspaceSymbol\", \"filePath\": \"{cwd}/{src}\", \"query\": \"{fn2}\"}"}}, 1, 0, NULL, "", "", .prep = P_LSP, .need = "{fn2}", .quote = "{fn2}",
        .fen = "The language server answers: \"{q}\". So `{fn2}` is defined in {src2}.", .fpt = "O servidor de linguagem responde: \"{q}\". Entao `{fn2}` e definida em {src2}."},
    {"LSP", "LSP", "use the language server to list the symbols of {src}", "usa o servidor de linguagem pra listar os simbolos de {src}",
        {{"LSP", "{\"operation\": \"documentSymbol\", \"filePath\": \"{cwd}/{src}\"}"}}, 1, 0, NULL, "", "", .prep = P_LSP, .need = "{fn}", .quote = "{fn}",
        .fen = "{src} has two functions according to the language server; the first is \"{q}\".", .fpt = "O {src} tem duas funcoes segundo o servidor de linguagem; a primeira e \"{q}\"."},
    {"LSP", "LSP", "the tests call {fn}: use the language server to jump from that call to the definition", "os testes chamam {fn}: usa o servidor de linguagem pra pular dessa chamada ate a definicao",
        {{"Grep", "{\"pattern\": \"{fn}\", \"path\": \"{cwd}/{testrel}\", \"output_mode\": \"content\"}"}, {"LSP", "{\"operation\": \"goToDefinition\", \"filePath\": \"{cwd}/{testrel}\", \"line\": {gl}, \"character\": {gc}}"}}, 2, 0, NULL, "", "", .prep = P_LSP, .need = "{src}", .quote = "{src}",
        .fen = "From the call on line {gl} of {testrel}, the language server goes to \"{q}\".", .fpt = "Da chamada na linha {gl} de {testrel}, o servidor de linguagem vai para \"{q}\"."},
    {"LSP", "LSP", "ask the language server who references {fn}", "pergunta ao servidor de linguagem quem referencia {fn}",
        {{"Grep", "{\"pattern\": \"def {fn}\", \"path\": \"{cwd}/{src}\", \"output_mode\": \"content\"}"}, {"LSP", "{\"operation\": \"findReferences\", \"filePath\": \"{cwd}/{src}\", \"line\": {gl}, \"character\": {gc}}"}}, 2, 0, NULL, "", "", .prep = P_LSP, .need = "{testrel}", .quote = "reference",
        .fen = "The language server reports \"{q}\": the definition in {src} and the two test cases in {testrel}.", .fpt = "O servidor de linguagem informa \"{q}\": a definicao em {src} e os dois casos de teste em {testrel}."},
    /* ---- cadernos ---- */
    {"NotebookEdit", "NotebookEdit", "in {nb}, add a markdown cell with the title \"## Totals\" right after the setup cell", "em {nb}, acrescenta uma celula markdown com o titulo \"## Totals\" logo depois da celula setup",
        {{"Read", "{\"file_path\": \"{cwd}/{nb}\"}"}, {"NotebookEdit", "{\"notebook_path\": \"{cwd}/{nb}\", \"cell_id\": \"setup\", \"new_source\": \"## Totals\", \"cell_type\": \"markdown\", \"edit_mode\": \"insert\"}"}}, 2, 0, NULL, " The notebook now has three cells: setup, the new title, calc.", " O caderno agora tem tres celulas: setup, o titulo novo, calc.", .prep = P_NB},
    {"NotebookEdit", "NotebookEdit", "delete the cell calc from {nb}", "apaga a celula calc de {nb}",
        {{"Read", "{\"file_path\": \"{cwd}/{nb}\"}"}, {"NotebookEdit", "{\"notebook_path\": \"{cwd}/{nb}\", \"cell_id\": \"calc\", \"new_source\": \"\", \"edit_mode\": \"delete\"}"}}, 2, 0, NULL, " Only the setup cell is left.", " So sobrou a celula setup.", .prep = P_NB},
    {"NotebookEdit", "NotebookEdit", "in {nb}, the calc cell should multiply by 52 instead of 12", "em {nb}, a celula calc tem que multiplicar por 52 em vez de 12",
        {{"Read", "{\"file_path\": \"{cwd}/{nb}\"}"}, {"NotebookEdit", "{\"notebook_path\": \"{cwd}/{nb}\", \"cell_id\": \"calc\", \"new_source\": \"total = rate * 52\\ntotal\"}"}}, 2, 0, NULL, " The second line of the cell, `total`, is kept; the cell was not run.", " A segunda linha da celula, `total`, foi mantida; a celula nao foi rodada.", .prep = P_NB},
    {"NotebookEdit", "NotebookEdit", "change the cell summary of {nb} to print the total", "muda a celula summary de {nb} para imprimir o total",
        {{"Read", "{\"file_path\": \"{cwd}/{nb}\"}"}, {"NotebookEdit", "{\"notebook_path\": \"{cwd}/{nb}\", \"cell_id\": \"summary\", \"new_source\": \"print(total)\"}"}}, 2, 1, NULL, " The notebook has the cells setup and calc; tell me which one you meant, or if summary should be a new cell.", " O caderno tem as celulas setup e calc; me diz qual voce quis dizer, ou se summary deve ser uma celula nova.", .prep = P_NB},
    /* ---- entregar arquivos ---- */
    {"SendUserFile", "SendUserFile", "send me {src} and {testrel}, both", "me manda o {src} e o {testrel}, os dois",
        {{"SendUserFile", "{\"files\": [\"{cwd}/{src}\", \"{cwd}/{testrel}\"]}"}}, 1, 0, NULL, " Both files went in the same call.", " Os dois arquivos foram na mesma chamada."},
    {"SendUserFile", "SendUserFile", "hand me {src} with the caption: for review", "me entrega o {src} com a legenda: for review",
        {{"SendUserFile", "{\"files\": [\"{cwd}/{src}\"], \"caption\": \"for review\"}"}}, 1, 0, NULL, "", ""},
    {"SendUserFile", "SendUserFile", "send me the file docs/handbook.pdf", "me manda o arquivo docs/handbook.pdf",
        {{"SendUserFile", "{\"files\": [\"{cwd}/docs/handbook.pdf\"]}"}}, 1, 1, NULL, " Nothing was sent; this project has no such file.", " Nada foi enviado; este projeto nao tem esse arquivo."},
    {"SendUserFile", "SendUserFile", "write notes/summary.txt saying that {name} has one test file, and send it to me", "escreve notes/summary.txt dizendo que {name} tem um arquivo de teste, e me manda",
        {{"Write", "{\"file_path\": \"{cwd}/notes/summary.txt\", \"content\": \"{name} has one test file: {testrel}\\n\"}"}, {"SendUserFile", "{\"files\": [\"{cwd}/notes/summary.txt\"]}"}}, 2, 0, NULL, " The file has one line, naming {testrel}.", " O arquivo tem uma linha, com o nome de {testrel}."},
    /* ---- modo plano ---- */
    {"ExitPlanMode", "EnterPlanMode,ExitPlanMode", "plan first and wait for my ok: workers should become 8 in settings.conf", "planeja primeiro e espera o meu ok: workers tem que virar 8 no settings.conf",
        {{"EnterPlanMode", "{}"}, {"Read", "{\"file_path\": \"{cwd}/settings.conf\"}"}, {"ExitPlanMode", "{\"plan\": \"1. In settings.conf change `workers = 4` to `workers = 8`\\n2. Run `sh tools/check_settings.sh`\"}", 4}}, 3, 0, NULL, "", "", .prep = P_CONF, .need = "did not approve",
        .fen = "You did not approve the plan, so nothing was changed: settings.conf still has `workers = 4` and plan mode is still on. Tell me what to change in the plan.", .fpt = "Voce nao aprovou o plano, entao nada foi alterado: o settings.conf continua com `workers = 4` e o modo plano continua ligado. Me diz o que mudar no plano."},
    {"ExitPlanMode", "ExitPlanMode", "leave plan mode", "sai do modo plano",
        {{"ExitPlanMode", "{\"plan\": \"1. Nothing to do\"}"}}, 1, 1, NULL, " I never entered plan mode in this conversation, so I can work normally.", " Nao entrei no modo plano nesta conversa, entao posso trabalhar normalmente."},
    {"EnterPlanMode", "EnterPlanMode", "enter plan mode and stay there; I will give you the task next", "entra no modo plano e fica nele; a tarefa eu te passo em seguida",
        {{"EnterPlanMode", "{}"}}, 1, 0, NULL, " From now on I only read until you approve a plan.", " A partir de agora eu so leio, ate voce aprovar um plano."},
    {"EnterPlanMode", "EnterPlanMode,ExitPlanMode", "show me a plan for renaming {fn} and wait; I will approve it but do not apply it yet", "me mostra um plano para renomear {fn} e espera; vou aprovar mas nao aplica ainda",
        {{"EnterPlanMode", "{}"}, {"Grep", "{\"pattern\": \"{fn}\", \"output_mode\": \"files_with_matches\"}"}, {"ExitPlanMode", "{\"plan\": \"1. Rename {fn} in {src}\\n2. Update the two cases in {testrel}\\n3. Run {testcmd}\"}", 2}}, 3, 0, NULL, "", "", .need = "approved the plan",
        .fen = "The plan is approved and plan mode is off. As you asked, I applied nothing yet: `{fn}` is still in {src} and in {testrel}. Say the new name and I start.", .fpt = "O plano esta aprovado e o modo plano esta desligado. Como voce pediu, ainda nao apliquei nada: `{fn}` continua em {src} e em {testrel}. Diz o nome novo que eu comeco."},
    {"ExitPlanMode", "EnterPlanMode,ExitPlanMode", "use plan mode: propose raising retries to 5; if I do not approve at first, show the same plan once more", "usa o modo plano: propoe subir retries para 5; se eu nao aprovar de primeira, mostra o mesmo plano mais uma vez",
        {{"EnterPlanMode", "{}"}, {"Read", "{\"file_path\": \"{cwd}/settings.conf\"}"}, {"ExitPlanMode", "{\"plan\": \"1. In settings.conf change `retries = 3` to `retries = 5`\\n2. Run `sh tools/check_settings.sh`\"}", 4}, {"ExitPlanMode", "{\"plan\": \"1. In settings.conf change `retries = 3` to `retries = 5`\\n2. Run `sh tools/check_settings.sh`\"}", 2}}, 4, 0, NULL, "", "", .prep = P_CONF, .need = "approved the plan",
        .fen = "The first time you did not approve; the second time you did, so plan mode is off now. I have not changed settings.conf yet: say go and I apply the two steps.", .fpt = "Na primeira vez voce nao aprovou; na segunda aprovou, entao o modo plano esta desligado. Ainda nao mudei o settings.conf: diz que pode e eu aplico os dois passos."},
    /* ---- worktrees (repositorio de verdade) ---- */
    {"EnterWorktree", "EnterWorktree,ExitWorktree", "create a worktree called draft and leave it on disk for later", "cria um worktree chamado draft e deixa no disco para depois",
        {{"EnterWorktree", "{\"name\": \"draft\"}"}, {"ExitWorktree", "{\"action\": \"keep\"}"}}, 2, 0, NULL, " I am back in the original folder; the worktree and its branch are kept.", " Voltei para a pasta original; o worktree e a branch dele ficaram.", .prep = P_GIT},
    {"EnterWorktree", "EnterWorktree", "make a worktree named my draft", "cria um worktree chamado my draft",
        {{"EnterWorktree", "{\"name\": \"my draft\"}"}}, 1, 1, NULL, " A name like my-draft works; tell me if I should use it.", " Um nome como my-draft funciona; me diz se devo usar.", .prep = P_GIT},
    {"ExitWorktree", "ExitWorktree", "remove the worktree you are in", "remove o worktree em que voce esta",
        {{"ExitWorktree", "{\"action\": \"remove\"}"}}, 1, 1, NULL, " I am in the project folder itself, so there is nothing to remove.", " Estou na propria pasta do projeto, entao nao ha o que remover.", .prep = P_GIT},
        {"ExitWorktree", "EnterWorktree,ExitWorktree", "in a worktree called listing, show me the files at the top of the project, then remove the worktree", "num worktree chamado listing, me mostra os arquivos do topo do projeto e depois remove o worktree",
        {{"EnterWorktree", "{\"name\": \"listing\"}"}, {"Bash", "{\"command\": \"ls\", \"description\": \"List the top of the worktree\"}"}, {"ExitWorktree", "{\"action\": \"remove\"}"}}, 3, 0, NULL, " The listing above is the worktree's copy of the project, and it is gone again.", " A listagem acima e a copia do projeto no worktree, e ela ja foi removida.", .prep = P_GIT, .need = "removed the worktree"},
    /* ---- PowerShell ---- */
    {"PowerShell", "PowerShell", "is PowerShell installed on this machine? check before answering", "o PowerShell esta instalado nesta maquina? confere antes de responder",
        {{"Bash", "{\"command\": \"command -v pwsh powershell || echo none\", \"description\": \"Look for PowerShell\"}"}}, 1, 0, NULL, "", "", .need = "none",
        .fen = "No: neither `pwsh` nor `powershell` is on the PATH (the check printed \"{q}\"). I can still write .ps1 scripts; they just cannot be run here.", .fpt = "Nao: nem `pwsh` nem `powershell` estao no PATH (a conferencia imprimiu \"{q}\"). Ainda posso escrever scripts .ps1; so nao da para rodar aqui."},
    {"PowerShell", "PowerShell", "use PowerShell to show me the value of HOME", "usa o PowerShell pra me mostrar o valor de HOME",
        {{"PowerShell", "{\"command\": \"$env:HOME\"}"}, {"Bash", "{\"command\": \"echo $HOME\", \"description\": \"Show HOME with the shell\"}"}}, 2, 0, NULL, " PowerShell is not installed here, so that value comes from Bash.", " O PowerShell nao esta instalado aqui, entao esse valor vem do Bash."},
            {"LSP", "LSP", "ask the language server for the symbols of settings.conf", "pede ao servidor de linguagem os simbolos do settings.conf",
        {{"LSP", "{\"operation\": \"documentSymbol\", \"filePath\": \"{cwd}/settings.conf\"}"}}, 1, 1, NULL, " A language server reads code, not a settings file; I can read it with Read if you want the keys.", " Um servidor de linguagem le codigo, nao um arquivo de configuracoes; posso ler com o Read se voce quiser as chaves.", .prep = P_CONF},
    {"PowerShell", "PowerShell", "with PowerShell, count the entries at the top of this project", "com o PowerShell, conta as entradas do topo deste projeto",
        {{"PowerShell", "{\"command\": \"(Get-ChildItem).Count\"}"}, {"Bash", "{\"command\": \"ls | wc -l\", \"description\": \"Count the entries at the top of the project\"}"}}, 2, 0, NULL, " PowerShell is not installed on this machine, so I counted with Bash.", " O PowerShell nao esta instalado nesta maquina, entao contei com o Bash."},
    /* ======== reservadas para o arquivo de validacao (hold): nunca saem no treino ======== */
    {"CronList", "CronCreate,ScheduleWakeup,CronList", "schedule a look at the settings every day at 7:15, and separately wake yourself up in twenty minutes; then show me what the schedule list has", "agenda uma olhada nas configuracoes todo dia as 7:15, e separadamente se acorda daqui a vinte minutos; depois me mostra o que a lista de agendamentos tem",
        {{"CronCreate", "{\"cron\": \"15 7 * * *\", \"prompt\": \"Look at the settings of {name}\", \"recurring\": true}"}, {"ScheduleWakeup", "{\"delaySeconds\": 1200, \"prompt\": \"Check on {name}\", \"reason\": \"twenty minute check\"}"}, {"CronList", "{}"}}, 3, 0, "KATANA_SCHED_NOW", "", "", .hold = 1, .quote = "15 7 * * *",
        .fen = "Both are set. The list has the daily task, \"{q}\"; the wakeup is a separate one-time alarm and is not a line of that list.", .fpt = "As duas coisas estao marcadas. A lista tem a tarefa diaria, \"{q}\"; o despertar e um alarme unico, separado, e nao e uma linha dessa lista."},
    {"CronDelete", "CronCreate,CronDelete", "schedule a one-shot at 12:00 to tag the release; no, wait: remove it and make it 13:00", "agenda uma vez as 12:00 para marcar a versao; nao, espera: remove e coloca as 13:00",
        {{"CronCreate", "{\"cron\": \"0 12 * * *\", \"prompt\": \"Tag the release of {name}\", \"recurring\": false}"}, {"CronDelete", "{\"id\": \"{id}\"}"}, {"CronCreate", "{\"cron\": \"0 13 * * *\", \"prompt\": \"Tag the release of {name}\", \"recurring\": false}"}}, 3, 0, "KATANA_SCHED_NOW", "", "", .hold = 1, .need = "0 13 * * *",
        .fen = "The 12:00 task was removed and the 13:00 one replaces it: \"{q}\".", .fpt = "A tarefa das 12:00 foi removida e a das 13:00 ficou no lugar: \"{q}\"."},
            {"WaitForMcpServers", "WaitForMcpServers,ListMcpResourcesTool,ReadMcpResourceTool", "wait only for the {srv} server, see what it offers and read me the decisions note", "espera so o servidor {srv}, ve o que ele oferece e le pra mim a nota de decisoes",
        {{"WaitForMcpServers", "{\"servers\": [\"{srv}\"]}"}, {"ListMcpResourcesTool", "{\"server\": \"{srv}\"}"}, {"ReadMcpResourceTool", "{\"server\": \"{srv}\", \"uri\": \"notes://decisions\"}"}}, 3, 0, NULL, "", "", .prep = P_MCP, .hold = 1, .quote = "Whole numbers",
        .fen = "{srv} is connected and offers four resources. The decisions note (notes://decisions) says: \"{q}\".", .fpt = "O {srv} esta ligado e oferece quatro recursos. A nota de decisoes (notes://decisions) diz: \"{q}\"."},
    {"SendMessage", "ListAgents,SendMessage", "check who else is running katana here; if there is someone, send them by pid: rebase before pushing", "confere quem mais esta rodando o katana aqui; se tiver alguem, manda pelo pid: rebase before pushing",
        {{"ListAgents", "{}"}, {"SendMessage", "{\"to\": \"{pid}\", \"message\": \"rebase before pushing\"}"}}, 2, 0, NULL, "", "", .prep = P_MAIL, .hold = 1, .need = "Message sent to session",
        .fen = "One other session is running, builder, and I sent the message to its pid: \"{q}\".", .fpt = "Ha uma outra sessao rodando, a builder, e mandei a mensagem para o pid dela: \"{q}\"."},
        {"Agent", "Agent,ListAgents,SendMessage", "have a sub-agent look for TODO marks; then show me the agents of this session and ask that same sub-agent to list the test files too", "manda um subagente procurar marcas TODO; depois me mostra os agentes desta sessao e pede ao mesmo subagente para listar tambem os arquivos de teste",
        {{"Agent", "{\"description\": \"Look for TODO marks\", \"prompt\": \"Search every file of the project for the text TODO and report the lines that have it. The project is at {cwd}.\"}"}, {"ListAgents", "{}"}, {"SendMessage", "{\"to\": \"agent-1\", \"message\": \"Also list the test files of the project (names starting with test_) and say how many there are.\"}"}}, 3, 0, NULL, "", "", .prep = P_AGENT, .sub1 = 3, .sub2 = 2, .hold = 1, .quote = "test file",
        .fen = "The sub-agent (agent-1) found no TODO mark. Continued with my second message, it answered: \"{q}\" and gave the path.", .fpt = "O subagente (agent-1) nao achou marca TODO. Continuado com a minha segunda mensagem, ele respondeu: \"{q}\" e deu o caminho."},
    {"Workflow", "Workflow", "run a workflow with one phase called survey and two agents: one lists the test files, the other counts the source files", "roda um workflow com uma fase chamada survey e dois agentes: um lista os arquivos de teste, o outro conta os arquivos de codigo",
        {{"Workflow", "{\"script\": \"{\\\"phases\\\": [{\\\"title\\\": \\\"survey\\\", \\\"agents\\\": [{\\\"label\\\": \\\"tests\\\", \\\"prompt\\\": \\\"List the test files of the project.\\\"}, {\\\"label\\\": \\\"sources\\\", \\\"prompt\\\": \\\"Count the source files of the project.\\\"}]}]}\"}"}}, 1, 0, NULL, "", "", .prep = P_FLOW, .sub1 = 2, .sub2 = 4, .hold = 1, .need = "source file",
        .fen = "The workflow ran its two agents in sequence: \"{q}\". They found {n1} test file and {n2} source files.", .fpt = "O workflow rodou os dois agentes em sequencia: \"{q}\". Eles acharam {n1} arquivo de teste e {n2} arquivos de codigo."},
    {"PowerShell", "PowerShell", "print the PowerShell version of this machine", "imprime a versao do PowerShell desta maquina",
        {{"PowerShell", "{\"command\": \"$PSVersionTable.PSVersion\"}"}, {"Bash", "{\"command\": \"command -v pwsh powershell || echo none\", \"description\": \"Look for PowerShell\"}"}}, 2, 0, NULL, "", "", .hold = 1, .need = "none",
        .fen = "There is no version to print: PowerShell is not installed here. The tool said so, and the PATH check confirms it (\"{q}\").", .fpt = "Nao ha versao para imprimir: o PowerShell nao esta instalado aqui. A ferramenta disse isso, e a conferencia do PATH confirma (\"{q}\")."},
        {"NotebookEdit", "NotebookEdit", "add a code cell at the end of {nb}, after calc, that prints the total", "acrescenta uma celula de codigo no fim de {nb}, depois da calc, que imprime o total",
        {{"Read", "{\"file_path\": \"{cwd}/{nb}\"}"}, {"NotebookEdit", "{\"notebook_path\": \"{cwd}/{nb}\", \"cell_id\": \"calc\", \"new_source\": \"print(total)\", \"cell_type\": \"code\", \"edit_mode\": \"insert\"}"}}, 2, 0, NULL, " It is the third cell now, after setup and calc; nothing was run.", " Ela e a terceira celula agora, depois de setup e calc; nada foi rodado.", .prep = P_NB, .hold = 1},
    {"SendUserFile", "SendUserFile", "send me the settings file as an attachment", "me manda o arquivo de configuracoes como anexo",
        {{"Bash", "{\"command\": \"ls settings*\", \"description\": \"Find the settings file\"}"}, {"SendUserFile", "{\"files\": [\"{cwd}/settings.conf\"], \"display\": \"attachment\"}"}}, 2, 0, NULL, " The settings file of this project is settings.conf.", " O arquivo de configuracoes deste projeto e o settings.conf.", .prep = P_CONF, .hold = 1},
        {"LSP", "LSP", "list the functions of {src2} through the language server", "lista as funcoes de {src2} pelo servidor de linguagem",
        {{"LSP", "{\"operation\": \"documentSymbol\", \"filePath\": \"{cwd}/{src2}\"}"}}, 1, 0, NULL, "", "", .prep = P_LSP, .hold = 1, .need = "{fn2}", .quote = "{fn2}",
        .fen = "The language server lists two functions in {src2}; the first one is \"{q}\".", .fpt = "O servidor de linguagem lista duas funcoes em {src2}; a primeira e \"{q}\"."},
        {"Monitor", "Monitor", "monitor `sh tools/check_settings.sh` and tell me its last line", "monitora `sh tools/check_settings.sh` e me diz a ultima linha",
        {{"Monitor", "{\"command\": \"sh tools/check_settings.sh\", \"description\": \"Watch the settings check\"}"}, {"Read", "{\"file_path\": \"{log}\"}", 1}}, 2, 0, NULL, "", "", .prep = P_CONF, .hold = 1, .quote = "settings ",
        .fen = "The check ended on its own and its last line is \"{q}\".", .fpt = "A verificacao terminou sozinha e a ultima linha dela e \"{q}\"."},
    /* ---- mais uma situacao para as ferramentas que estavam com quatro ---- */
    {"TaskStop", "TaskStop", "start tools/watch.sh twice in the background and then stop only the second one", "sobe o tools/watch.sh duas vezes em segundo plano e depois para so o segundo",
        {{"Bash", "{\"command\": \"sh tools/watch.sh\", \"run_in_background\": true, \"description\": \"Start the first watcher\"}"}, {"Bash", "{\"command\": \"sh tools/watch.sh\", \"run_in_background\": true, \"description\": \"Start the second watcher\"}"}, {"TaskStop", "{\"task_id\": \"2\"}"}}, 3, 0, NULL, "", "", .prep = P_BG, .need = "Successfully stopped task: 2",
        .fen = "Two watchers were started, tasks 1 and 2. I stopped only the second: \"{q}\". Task 1 is still running.", .fpt = "Dois monitores foram iniciados, as tarefas 1 e 2. Parei so o segundo: \"{q}\". A tarefa 1 continua rodando."},
    {"TaskStop", "Monitor,TaskStop", "start monitoring tools/watch.sh... actually never mind, stop it again", "comeca a monitorar o tools/watch.sh... na verdade deixa, para de novo",
        {{"Monitor", "{\"command\": \"sh tools/watch.sh\", \"description\": \"Watch the script\"}"}, {"TaskStop", "{\"task_id\": \"{bg}\"}"}}, 2, 0, NULL, " I did not read its output, since you changed your mind.", " Nao li a saida dele, ja que voce mudou de ideia.", .prep = P_BG, .need = "Successfully stopped"},
    {"EnterPlanMode", "EnterPlanMode,ExitPlanMode", "plan first: you will create notes/TODO.txt with the line \"tag the release\"; wait for my ok and then do it", "planeja primeiro: voce vai criar notes/TODO.txt com a linha \"tag the release\"; espera o meu ok e depois faz",
        {{"EnterPlanMode", "{}"}, {"ExitPlanMode", "{\"plan\": \"1. Create notes/TODO.txt with the line: tag the release\"}", 2}, {"Write", "{\"file_path\": \"{cwd}/notes/TODO.txt\", \"content\": \"tag the release\\n\"}"}}, 3, 0, NULL, " I wrote it only after you approved the one-step plan.", " So gravei depois que voce aprovou o plano de um passo.", .need = "File created"},
        {"NotebookEdit", "NotebookEdit", "turn the setup cell of {nb} into a markdown cell that says \"Rate table\"", "transforma a celula setup de {nb} numa celula markdown que diz \"Rate table\"",
        {{"Read", "{\"file_path\": \"{cwd}/{nb}\"}"}, {"NotebookEdit", "{\"notebook_path\": \"{cwd}/{nb}\", \"cell_id\": \"setup\", \"new_source\": \"Rate table\", \"cell_type\": \"markdown\"}"}}, 2, 0, NULL, " Note that calc still uses `rate`, which that cell no longer defines.", " Repare que a calc ainda usa `rate`, que essa celula nao define mais.", .prep = P_NB},
    {"SendUserFile", "SendUserFile", "run `{testcmd}`, keep the output in notes/test-run.log and send me that file", "roda `{testcmd}`, guarda a saida em notes/test-run.log e me manda esse arquivo",
        {{"Bash", "{\"command\": \"mkdir -p notes && {testcmd} > notes/test-run.log 2>&1; tail -1 notes/test-run.log\", \"description\": \"Run the tests and keep the output\"}"}, {"SendUserFile", "{\"files\": [\"{cwd}/notes/test-run.log\"]}"}}, 2, 0, NULL, "", "", .quote = " passed, ",
        .fen = "The run ended with \"{q}\"; the whole output is in notes/test-run.log, and that file was handed to you.", .fpt = "A rodada terminou com \"{q}\"; a saida inteira esta em notes/test-run.log, e esse arquivo foi entregue a voce."},
                {"ExitPlanMode", "EnterPlanMode,ExitPlanMode", "plan first and wait for my ok; after it, set timeout to 45 and retries to 5 in settings.conf with a single edit and run the settings check", "planeja primeiro e espera o meu ok; depois dele, coloca timeout em 45 e retries em 5 no settings.conf com uma edicao so e roda a verificacao das configuracoes",
        {{"EnterPlanMode", "{}"}, {"Read", "{\"file_path\": \"{cwd}/settings.conf\"}"}, {"ExitPlanMode", "{\"plan\": \"1. In settings.conf change `timeout = 30` to `timeout = 45` and `retries = 3` to `retries = 5`, in one edit\\n2. Run `sh tools/check_settings.sh`\"}", 2},
         {"Edit", "{\"file_path\": \"{cwd}/settings.conf\", \"edits\": \"<<<<<<<\\ntimeout = 30\\n=======\\ntimeout = 45\\n>>>>>>>\\n<<<<<<<\\nretries = 3\\n=======\\nretries = 5\\n>>>>>>>\\n\"}"}, {"Bash", "{\"command\": \"sh tools/check_settings.sh\", \"description\": \"Check the settings\"}"}}, 5, 0, NULL, "", "", .prep = P_CONF, .hold = 1, .need = "settings ok", .quote = "^settings ",
        .fen = "You approved the plan, so I applied it in one edit: settings.conf now has `timeout = 45` and `retries = 5`. The check ends with \"{q}\".", .fpt = "Voce aprovou o plano, entao apliquei numa edicao so: o settings.conf agora tem `timeout = 45` e `retries = 5`. A verificacao termina com \"{q}\"."},
    /* ---- mais situacoes: worktrees (treino) e uma situacao reservada para a validacao de cada ferramenta que nao tinha ---- */
    {"EnterWorktree", "EnterWorktree,ExitWorktree", "open an isolated copy called fix-rounding and tell me which branch it is on", "abre uma copia isolada chamada fix-rounding e me diz em que branch ela esta",
        {{"EnterWorktree", "{\"name\": \"fix-rounding\"}"}, {"Bash", "{\"command\": \"git branch --show-current\", \"description\": \"Show the branch of the worktree\"}"}}, 2, 0, NULL, "", "", .prep = P_GIT, .need = "worktree-fix-rounding"},
    {"EnterWorktree", "EnterWorktree", "switch into the old worktree that is in .katana/worktrees/old", "volta para o worktree antigo que esta em .katana/worktrees/old",
        {{"EnterWorktree", "{\"path\": \"{cwd}/.katana/worktrees/old\"}"}}, 1, 1, NULL, " There is no worktree there; I can create one with that name if you want.", " Nao ha worktree ali; posso criar um com esse nome se voce quiser.", .prep = P_GIT},
    {"ExitWorktree", "EnterWorktree,ExitWorktree", "make a worktree called scratch, create the file scratch.txt in it, then leave and delete the worktree", "cria um worktree chamado scratch, cria o arquivo scratch.txt nele, depois sai e apaga o worktree",
        {{"EnterWorktree", "{\"name\": \"scratch\"}"}, {"Bash", "{\"command\": \"echo scratch > scratch.txt\", \"description\": \"Create the file in the worktree\"}"}, {"ExitWorktree", "{\"action\": \"remove\"}"}}, 3, 1, NULL, " scratch.txt is not committed, so nothing was removed and I am still inside the worktree; say if I should discard it or keep it.", " O scratch.txt nao esta commitado, entao nada foi removido e continuo dentro do worktree; diz se devo descartar ou manter.", .prep = P_GIT, .need = "scratch.txt"},
    {"ExitWorktree", "EnterWorktree,ExitWorktree", "worktree called notes-copy with a file named todo.txt in it; leave it on disk when you get out, do not delete anything", "worktree chamado notes-copy com um arquivo todo.txt dentro; deixa no disco quando sair, nao apaga nada",
        {{"EnterWorktree", "{\"name\": \"notes-copy\"}"}, {"Bash", "{\"command\": \"echo todo > todo.txt\", \"description\": \"Create the file in the worktree\"}"}, {"ExitWorktree", "{\"action\": \"keep\"}"}}, 3, 0, NULL, " The file todo.txt is still in the worktree.", " O todo.txt continua no worktree.", .prep = P_GIT, .need = "were kept"},
    {"EnterWorktree", "EnterWorktree,ExitWorktree", "go into a worktree named review, show me the last commit message there and come back keeping the worktree", "entra num worktree chamado review, me mostra a mensagem do ultimo commit de la e volta mantendo o worktree",
        {{"EnterWorktree", "{\"name\": \"review\"}"}, {"Bash", "{\"command\": \"git log -1 --format=%s\", \"description\": \"Show the last commit message\"}"}, {"ExitWorktree", "{\"action\": \"keep\"}"}}, 3, 0, NULL, " The commit message above is the worktree's own, copied from the original branch.", " A mensagem de commit acima e a do proprio worktree, copiada da branch original.", .prep = P_GIT, .hold = 1, .need = "were kept"},
    {"ExitWorktree", "ExitWorktree", "get out of the worktree and keep it", "sai do worktree e mantem ele",
        {{"ExitWorktree", "{\"action\": \"keep\"}"}}, 1, 1, NULL, " I never entered a worktree in this conversation, so I am still in the project folder.", " Nao entrei em nenhum worktree nesta conversa, entao continuo na pasta do projeto.", .prep = P_GIT, .hold = 1},
    {"PushNotification", "PushNotification", "pop up a notice titled Build with the text: all green", "mostra um aviso com o titulo Build e o texto: all green",
        {{"PushNotification", "{\"message\": \"all green\", \"title\": \"Build\"}"}}, 1, 0, "KATANA_UI", " I did not run the build; I only sent the text.", " Nao rodei o build; so mandei o texto.", 1, .hold = 1},
    {"ReportFindings", "ReportFindings", "report a warning on {src}: it has no header comment", "registra um aviso sobre o {src}: ele nao tem comentario de cabecalho",
        {{"ReportFindings", "{\"findings\": [{\"file\": \"{cwd}/{src}\", \"summary\": \"No header comment\", \"failure_scenario\": \"A reader does not know what the module is for.\"}], \"level\": \"warning\"}"}}, 1, 0, "KATANA_UI", "", "", 1, .hold = 1},
    {"EndConversation", "EndConversation", "ok thanks, that is enough, close the chat", "ok valeu, ja deu, fecha o chat",
        {{"EndConversation", "{\"reason\": \"the user thanked and asked to close the chat\"}"}}, 1, 0, NULL, "", "", .hold = 1},
    {"SubagentHandback", "SubagentHandback", "finish by handing back the word done", "termina devolvendo a palavra done",
        {{"SubagentHandback", "{\"message\": \"done\"}"}}, 1, 1, NULL, " Nobody started me as a sub-agent here, so the word goes to you: done.", " Ninguem me iniciou como subagente aqui, entao a palavra vai para voce: done.", 1, .hold = 1},
    {"TaskStop", "TaskStop", "run tools/watch.sh twice in the background and stop the first one only", "roda o tools/watch.sh duas vezes em segundo plano e para so a primeira",
        {{"Bash", "{\"command\": \"sh tools/watch.sh\", \"run_in_background\": true, \"description\": \"Start the first watcher\"}"}, {"Bash", "{\"command\": \"sh tools/watch.sh\", \"run_in_background\": true, \"description\": \"Start the second watcher\"}"}, {"TaskStop", "{\"task_id\": \"1\"}"}}, 3, 0, NULL, "", "", .prep = P_BG, .hold = 1, .need = "Successfully stopped task: 1",
        .fen = "Two watchers are running, tasks 1 and 2, and I stopped the first: \"{q}\". Task 2 is still running.", .fpt = "Dois monitores estao rodando, as tarefas 1 e 2, e parei o primeiro: \"{q}\". A tarefa 2 continua rodando."},
};
#define X_NRARE CU_N(g_x_rare)
static const char *x_rsub(const char *t, const char *key, const char *val) {
    const char *q = strstr(t, key); if (!q) return t;
    return x_rsub(cu_fmt("%.*s%s%s", (int)(q - t), t, val, q + strlen(key)), key, val);
}
/* as ferramentas que o prompt declara inteiras (as outras do motor pedem ToolSearch select:Nome antes) */
static int x_rare_declared(const char *n) {
    static const char *const d[] = {"Bash", "Read", "Edit", "Write", "Grep", "TodoWrite", "AskUserQuestion", "ToolSearch", "WebSearch", "WebFetch", "Skill"};
    for (size_t i = 0; i < CU_N(d); i++) if (!strcmp(d[i], n)) return 1;
    return 0;
}
static int x_rare_insel(const char *sel, const char *n) {
    const size_t l = strlen(n);
    for (const char *q = sel; q && (q = strstr(q, n)) != NULL; q += l) if ((q == sel || q[-1] == ',') && (q[l] == 0 || q[l] == ',')) return 1;
    return 0;
}
typedef struct { const char *k, *v; } XRkv;
static const char *x_rare_fill(const char *t, const XRkv *kv, int n) { for (int i = 0; i < n; i++) if (kv[i].v) t = x_rsub(t, kv[i].k, kv[i].v); return t; }
/* quantas situacoes de treino (e reservadas) trazem uma chamada treinada de cada ferramenta: `katana curriculum rare` imprime a tabela */
static int cu_rare_main(void) {
    const char *seen[200]; int ns = 0;
    for (int i = 0; i < (int)X_NRARE; i++) for (int k = 0; k < g_x_rare[i].nc; k++) {
        int has = 0; for (int j = 0; j < ns; j++) has |= !strcmp(seen[j], g_x_rare[i].c[k].name);
        if (!has && ns < 200) seen[ns++] = g_x_rare[i].c[k].name;
    }
    int tr = 0, ho = 0; for (int i = 0; i < (int)X_NRARE; i++) { if (g_x_rare[i].hold) ho++; else tr++; }
    printf("x-rare: %d situacoes de treino, %d reservadas para a validacao\n%-24s %6s %6s %6s %6s\n", tr, ho, "ferramenta", "treino", "reais", "valid.", "erro");
    for (int j = 0; j < ns; j++) {
        int a = 0, b = 0, real = 0, bad = 0;
        for (int i = 0; i < (int)X_NRARE; i++) {
            int has = 0; for (int k = 0; k < g_x_rare[i].nc; k++) has |= !strcmp(seen[j], g_x_rare[i].c[k].name);
            if (!has) continue;
            if (g_x_rare[i].hold) b++; else { a++; real += !g_x_rare[i].standin; bad += g_x_rare[i].bad; }
        }
        printf("%-24s %6d %6d %6d %6d\n", seen[j], a, real, b, bad);
    }
    return 0;
}
static void fam_x_rare(FILE *f, Scn *s) {
    if (getenv("KATANA_CU_RARE_TABLE")) { cu_rare_main(); exit(0); }      /* so imprime a tabela de situacoes por ferramenta */
    x_r2_cleanup();
    int ei = g_x_kforce;
    if (ei < 0) {      /* rodizio so entre as situacoes do arquivo que esta sendo escrito: treino ou validacao */
        static int tr[X_NRARE], ho[X_NRARE]; static int ntr, nho;
        if (!ntr) for (int i = 0; i < (int)X_NRARE; i++) { if (g_x_rare[i].hold) ho[nho++] = i; else tr[ntr++] = i; }
        ei = g_cu_hold && nho ? ho[(g_x_cycle2 + g_cu_att) % nho] : tr[(g_x_cycle2 + g_cu_att) % ntr];
    }
    const XRare *e = &g_x_rare[ei];
    XC c; memset(&c, 0, sizeof c); c.s = s;
    x_proj(&c.p, s, e->prep == P_LSP ? X_PY : CU_PA(((const int[]){X_PY, X_JS, X_SH, X_PL})), 2, 2); XProj *p = &c.p;
    p->readme = cu_pct(50); p->knows = 0;
    const char *nb = "notebooks/analysis.ipynb", *dlname = "release-notes.txt";
    char site[80]; snprintf(site, sizeof site, "files-%04x", (unsigned)(g_x_rng0 & 0xffff));
    if (e->prep == P_BG) x_extra(p, "tools/watch.sh", cu_fmt("#!/bin/sh\n# prints a line when it starts watching, then waits\necho \"watching %s\"\nexec sleep 300\n", p->name), 0755);
    if (e->prep == P_NB) x_extra(p, nb, "{\"cells\": [{\"cell_type\": \"code\", \"id\": \"setup\", \"metadata\": {}, \"outputs\": [], \"execution_count\": null, \"source\": [\"rate = 4\\n\"]}, "
                                        "{\"cell_type\": \"code\", \"id\": \"calc\", \"metadata\": {}, \"outputs\": [], \"execution_count\": null, \"source\": [\"total = rate * 12\\n\", \"total\\n\"]}], \"metadata\": {}, \"nbformat\": 4, \"nbformat_minor\": 5}\n", 0);
    if (e->prep == P_CONF) { p->conf = 1; for (int i = 0; i < CU_N(x_conf_keys); i++) x_conf_add(p, x_conf_keys[i][0], x_conf_keys[i][1], x_conf_keys[i][2]); }
    if (e->prep == P_FLOW) x_extra(p, "tools/flow.json", "{\"phases\": [{\"title\": \"look\", \"agents\": [{\"label\": \"lister\", \"prompt\": \"List the test files of the project.\"}]}, {\"title\": \"count\", \"agents\": [{\"label\": \"counter\", \"prompt\": \"Count the source files of the project.\"}]}]}\n", 0);
    if (e->prep == P_WWW) { if (!x_web_up()) cu_die("o servidor local de paginas nao subiu"); cu_put(cu_fmt("%s/www/%s/%s", g_cu_sbroot, site, dlname), cu_fmt("release notes of %s\n1.4.0 adds the settings check\n1.3.2 fixes rounding\n", p->name), 0); }
    g_x_realgit = e->prep == P_GIT || e->prep == P_GITMOD;
    if (e->prep == P_MCP || e->prep == P_MCPDOWN) { g_r2_srvname = CU_PA(((const char *const[]){"daybook", "logbook", "teamnotes"})); g_r2_down = e->prep == P_MCPDOWN; g_x_prehook = x_r2_mcp_hook; }
    if (e->env) setenv(e->env, !strcmp(e->env, "KATANA_SCHED_NOW") ? "1770000000" : "1", 1);
    g_ct_sub_run = NULL;
    x_begin("x-rare", s, p, 0, 0);
    g_x_prehook = NULL; g_x_realgit = 0;
    g_x.k = ei; g_x.k2 = 1; const int tasks = !strncmp(e->tool, "Task", 4) && strcmp(e->tool, "TaskStop");      /* TaskCreate, TaskUpdate, TaskGet, TaskList */
    g_x.nudge_ok = tasks;
    x_cell("rare/%s/%d%s", e->tool, ei, e->standin ? "/standin" : "");
    /* nenhum aviso de verdade na area de trabalho: um notify-send de mentira fica na frente no PATH */
    cu_put(cu_fmt("%s/bin/notify-send", g_cu_sb), "#!/bin/sh\nexit 0\n", 0755);
    XFi *fi = &p->mod[0].fn[0], *fi2 = &p->mod[1].fn[0];
    const char *pid = NULL;
    if (e->prep == P_MAIL) {
        pid = cu_fmt("%ld", x_r2_peer_start(s, "builder"));
        if (g_x.nsub + 4 <= 4) {      /* o pid muda a cada execucao: sai do texto so nos lugares em que o motor o escreve */
            g_x.sfrom[g_x.nsub] = cu_fmt("%s  name: builder", pid); g_x.sto[g_x.nsub++] = "40021  name: builder";
            g_x.sfrom[g_x.nsub] = cu_fmt("session %s (", pid); g_x.sto[g_x.nsub++] = "session 40021 (";
            g_x.sfrom[g_x.nsub] = cu_fmt("\"to\": \"%s\"", pid); g_x.sto[g_x.nsub++] = "\"to\": \"40021\"";
            g_x.sfrom[g_x.nsub] = cu_fmt("%s)", pid); g_x.sto[g_x.nsub++] = "40021)";
        }
    }
    if (e->prep == P_LSP) cu_put(cu_fmt("%s/bin/pylsp", g_cu_sb), g_r2_pylsp, 0755);
    if (e->prep == P_GITMOD) {      /* o usuario mexeu num arquivo e criou outro depois do ultimo commit */
        const char *rel = x_src(p, 0), *cur = x_slurp(s, rel), *oldl = x_snip(p->st, fi->name, fi->f, fi->num), *newl = x_snip(p->st, fi->name, fi->f, fi->f->bad);
        const char *q = cur ? strstr(cur, oldl) : NULL; if (!q) cu_die("trecho nao achado");
        cu_put(x_abs(s, rel), cu_fmt("%.*s%s%s", (int)(q - cur), cur, newl, q + strlen(oldl)), 0);
        cu_put(x_abs(s, "notes.txt"), "remember to tag the release\n", 0);
    }
    char n1[16] = "", n2[16] = "";
    if (e->prep == P_AGENT || e->sub1) {
        const char *gl = cu_fmt("%s*.%s", x_srcdir(p), g_x_ext[p->st]);
        snprintf(n1, sizeof n1, "%d", x_r2_subans(e->sub1, fi->name, gl, g_r2_sub[0], sizeof g_r2_sub[0]));
        if (e->sub2) snprintf(n2, sizeof n2, "%d", x_r2_subans(e->sub2, fi->name, gl, g_r2_sub[1], sizeof g_r2_sub[1]));
        g_r2_subn = 0; g_ct_sub_run = x_r2_oracle; setenv("KATANA_AGENT_FAKE", "1", 1);
    }
    XRkv kv[32]; int nkv = 0;
#define KV(key, val) do { kv[nkv].k = key; kv[nkv++].v = val; } while (0)
    KV("{cwd}", s->cwd); KV("{name}", p->name); KV("{testrel}", p->testrel); KV("{testcmd}", p->testcmd); KV("{fn}", fi->name); KV("{fn2}", fi2->name);
    KV("{src}", x_src(p, 0)); KV("{src2}", x_src(p, 1)); KV("{srv}", g_r2_srvname); KV("{pid}", pid); KV("{nb}", nb); KV("{home}", s->home);
    KV("{url}", e->prep == P_WWW ? cu_fmt("http://127.0.0.1:%d/%s/%s", g_x_port, site, dlname) : NULL); KV("{n1}", n1); KV("{n2}", n2);
    const int kid = nkv; KV("{id}", ""); const int kbg = nkv; KV("{bg}", ""); const int klog = nkv; KV("{log}", ""); const int kq = nkv; KV("{q}", "");
    const int ktot = nkv; KV("{totals}", ""); const int kgl = nkv; KV("{gl}", ""); const int kgc = nkv; KV("{gc}", "");
#undef KV
    x_user(x_rare_fill(L(e->en, e->pt), kv, nkv));
    for (int i = 0; i < e->nc; i++) if (!x_rare_declared(e->c[i].name) && !x_rare_insel(e->sel, e->c[i].name)) cu_die("x-rare %d: %s nao esta no ToolSearch da situacao", ei, e->c[i].name);
    const int ncalls = e->nc + (e->sel != NULL);
    const char *plan[8]; int np = 0;
    if (ncalls >= 3 && !tasks) {      /* Task* e TodoWrite sao a mesma lista por baixo: sem plano proprio nessas conversas */
        if (e->sel) plan[np++] = L("Get the tools", "Pegar as ferramentas");
        for (int i = 0; i < e->nc; i++) { const char *n = e->c[i].name;
            plan[np++] = !strcmp(n, "Bash") ? L("Run the command", "Rodar o comando") : !strcmp(n, "Read") ? L("Read the result", "Ler o resultado") : !strcmp(n, "Grep") || !strcmp(n, "Glob") ? L("Search the project", "Procurar no projeto")
                       : !strcmp(n, "Write") ? L("Write the file", "Gravar o arquivo") : !strcmp(n, "Edit") ? L("Change the file", "Mudar o arquivo") : cu_fmt(L("Call %s", "Chamar %s"), n); }
        x_plan(plan, np);
    }
    if (e->sel) { x_tsel(e->sel); if (np) x_done(1); }
    const char *r = "", *res[6] = {0};
    for (int i = 0; i < e->nc; i++) {
        const char *a = x_rare_fill(e->c[i].args, kv, nkv);
        if (e->c[i].pre & 1) {      /* ate 8 s pela primeira saida, e mais um pouco para a linha chegar inteira */
            struct stat st; const char *lg = cu_fmt("%s%s", g_cu_sb, kv[klog].v);
            for (int w = 0; w < 80 && *kv[klog].v && !(stat(kv[klog].v, &st) == 0 && st.st_size > 0) && !(stat(lg, &st) == 0 && st.st_size > 0); w++) usleep(100000);
            usleep(300000);
        }
        if (e->c[i].pre & 6) x_feed(e->c[i].pre & 2 ? "approve" : "keep planning");
        r = cu_fmt("%s", x_call(NULL, e->c[i].name, a, XF_ERR));
        if (e->c[i].pre & 6) x_unfeed();
        res[i] = r;
        if (getenv("KATANA_CU_SHOW")) fprintf(stderr, "RARE %d %s %s => %.400s\n", ei, e->c[i].name, a, r);
        const char *tk = strstr(r, "task ");
        if (tk && (!strcmp(e->c[i].name, "CronCreate") || !strcmp(e->c[i].name, "ScheduleWakeup"))) {      /* o id e sorteado pelo motor */
            const char *id = cu_fmt("%.*s", (int)strcspn(tk + 5, " "), tk + 5);
            if (!*kv[kid].v) kv[kid].v = id;
            int known = 0; for (int z = 0; z < g_x.nsub; z++) known |= !strcmp(g_x.sfrom[z], id);
            if (!known && g_x.nsub < 4 && strlen(id) == 8) { g_x.sfrom[g_x.nsub] = id; g_x.sto[g_x.nsub] = cu_fmt("5c0de00%d", g_x.nsub + 1); g_x.nsub++; }
        }
        if (!strcmp(e->c[i].name, "NotebookEdit")) { const char *pp = strstr(r, "(id "); if (pp && strlen(pp) > 12 && pp[12] == ')' && g_x.nsub < 4) { g_x.sfrom[g_x.nsub] = cu_fmt("(id %.8s)", pp + 4); g_x.sto[g_x.nsub++] = "(id c0ffee01)"; } }
        const char *idp = strstr(r, "ID: "), *fp = strstr(r, "written to: ");
        if (idp && fp && !*kv[kbg].v) { kv[kbg].v = cu_fmt("%d", atoi(idp + 4)); kv[klog].v = cu_fmt("%.*s", (int)strcspn(fp + 12, "\n"), fp + 12); }
        { const char *tl = x_line_with(r, " passed, "); if (tl && !*kv[ktot].v) kv[ktot].v = tl; }
        if (!strcmp(e->c[i].name, "Grep") && !*kv[kgl].v) {      /* primeira ocorrencia do Grep: arquivo:linha:texto -> linha e coluna da funcao */
            const char *c1 = strchr(r, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : NULL, *w = c2 ? strstr(c2 + 1, fi->name) : NULL;
            if (c1 && c2 && w && w < c2 + strcspn(c2, "\n") && atoi(c1 + 1) > 0) { kv[kgl].v = cu_fmt("%d", atoi(c1 + 1)); kv[kgc].v = cu_fmt("%d", (int)(w - c2)); }
        }
        if (np && i + 1 < e->nc) x_done(i + 1 + (e->sel != NULL));
    }
    if (e->env) unsetenv(e->env);
    unsetenv("KATANA_AGENT_FAKE");
    ct_sched_reset(); g_ct_end_conversation = 0; g_ct_sub_run = NULL;
    job_all_kill();
    if (g_plan_mode) { g_plan_mode = 0; }
    { static char wt[3000]; if (g_wt_active) ct_dispatch("ExitWorktree", "{\"action\": \"remove\", \"discard_changes\": true}", "", wt, sizeof wt); }
    x_r2_cleanup();
    if (x_err(r) != (e->bad != 0)) cu_die("x-rare %d (%s): esperava %s e veio: %.200s", ei, e->tool, e->bad ? "erro" : "resultado", r);
    if (e->need && !strstr(r, x_rare_fill(e->need, kv, nkv))) cu_die("x-rare %d (%s): o resultado nao traz a prova \"%s\": %.200s", ei, e->tool, e->need, r);
    const char *one = x_err(r) ? r + 7 : r; while (*one == '\n' || *one == ' ') one++;
    if (e->quote) {
        const char *qs = x_rare_fill(e->quote, kv, nkv), *hit = NULL;
        if (*qs == '^') {      /* a linha tem de comecar pelo trecho */
            const size_t ql = strlen(qs + 1);
            for (int i = e->nc - 1; i >= 0 && !hit; i--) for (const char *l = res[i]; l && *l && !hit; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) if (!strncmp(l, qs + 1, ql)) hit = l;
        } else for (int i = e->nc - 1; i >= 0 && !hit; i--) hit = x_line_with(res[i], qs);
        if (!hit) cu_die("x-rare %d (%s): nenhuma linha com \"%s\"", ei, e->tool, qs);
        one = hit; while (*one == ' ' || (*one == '-' && one[1] == ' ')) one++;
        if (!strncmp(one, "error: ", 7)) one += 7;
    }
    { const char *d = one; while (*d == ' ') d++; const char *d0 = d; while (isdigit((unsigned char)*d)) d++; if (d > d0 && *d == '\t') one = d + 1; }
    char *ln = cu_fmt("%.*s", (int)strcspn(one, "\n"), one);
    { size_t l = strlen(ln); while (l && (ln[l - 1] == '.' || ln[l - 1] == ' ' || ln[l - 1] == ':')) ln[--l] = 0; }
    if (strlen(ln) > 240) { ln[240] = 0; char *sp = strrchr(ln, ' '); if (sp) *sp = 0; ln = cu_fmt("%s...", ln); }
    if (!*ln) cu_die("x-rare %d: resultado vazio", ei);
    kv[kq].v = ln;
    if (e->fen) x_final(x_rare_fill(L(e->fen, e->fpt), kv, nkv));
    else {      /* tres formas de abrir a fala, fixas por situacao */
        static const char *const good[3][2] = {{"Done. The engine says: \"%s\".%s", "Feito. O motor diz: \"%s\".%s"}, {"It worked; the answer was \"%s\".%s", "Deu certo; a resposta foi \"%s\".%s"}, {"Result: \"%s\".%s", "Resultado: \"%s\".%s"}};
        static const char *const bad[3][2] = {{"That did not work. The engine says: \"%s\".%s", "Nao deu certo. O motor diz: \"%s\".%s"}, {"It failed with \"%s\".%s", "Falhou com \"%s\".%s"}, {"No: the answer was \"%s\".%s", "Nao: a resposta foi \"%s\".%s"}};
        x_final(cu_fmt(e->bad ? bad[ei % 3][g_cu_lang] : good[ei % 3][g_cu_lang], ln, x_rare_fill(L(e->nen, e->npt), kv, nkv)));
    }
    if (g_cu_hold && !tasks && strcmp(e->tool, "EndConversation") && e->prep != P_GIT && e->prep != P_GITMOD) { c.fu = cu_pct(50) ? F_FIND : F_EXPLAIN; x_followup(&c); }      /* holdout: um pedido de outro mecanismo na mesma conversa */
    x_end(f);
}
