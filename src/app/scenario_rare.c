/* ---------- katana curriculum, cenarios compostos: as ferramentas raras ----------
 * x-rare: toda ferramenta que o prompt cita e que as outras familias nao usam ganha pelo menos tres situacoes: o uso certo
 * (com os argumentos achados pelo ToolSearch quando ela so e citada pelo nome), um segundo uso diferente, e um caso em que
 * ela falha ou nao serve e o modelo reage. Tudo roda no motor, numa pasta HOME propria. Onde rodar de verdade nao da ou
 * faria mal, o que roda e o caminho de recusa/validacao do proprio motor ou um substituto do programa de fora, e a celula
 * leva a marca "standin":
 *   - PushNotification e ReportFindings: com KATANA_UI (o marcador que a interface leria), sem aviso na area de trabalho;
 *   - install_package: pip e npm de mentira na pasta bin do cenario (nada e instalado);
 *   - Agent, Workflow, SubagentHandback: o subagente de teste do motor (KATANA_AGENT_FAKE=1), porque o de verdade pede modelo;
 *   - SendMessage, ListAgents, ReadMcpResourceTool, WaitForMcpServers: so os caminhos sem destinatario / sem servidor. */

static const char *x_tsel(const char *names);
typedef struct { const char *name, *args; int old; } XRc;
typedef struct {
    const char *tool, *sel, *en, *pt;
    XRc c[3]; int nc;
    int bad;                     /* 1: o ultimo resultado e um erro do motor e a fala final reage a ele */
    const char *env;             /* variavel ligada so nesta conversa (valor 1, ou o relogio fixo do agendador) */
    const char *nen, *npt;       /* o que o modelo acrescenta depois de citar o resultado */
    int standin;
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
    {"terminal", NULL, "which shell sessions do you have open?", "que sessoes de shell voce tem abertas?",
        {{"terminal", "{\"action\": \"list\"}", 1}}, 1, 0, NULL, "", ""},
    {"terminal", NULL, "show me the output of the shell session called build", "me mostra a saida da sessao de shell chamada build",
        {{"terminal", "{\"action\": \"read\", \"name\": \"build\"}", 1}}, 1, 1, NULL, " Tell me if I should open that session first.", " Me diz se devo abrir essa sessao antes."},
    {"terminal", NULL, "close the shell session named deploy", "fecha a sessao de shell chamada deploy",
        {{"terminal", "{\"action\": \"close\", \"name\": \"deploy\"}", 1}}, 1, 1, NULL, " There was nothing to close.", " Nao havia o que fechar."},
    {"install_package", NULL, "install the Python package tabulate", "instala o pacote Python tabulate",
        {{"install_package", "{\"manager\": \"pip\", \"name\": \"tabulate\"}", 1}}, 1, 0, NULL, "", "", 1},
    {"install_package", NULL, "install left-pad with npm", "instala o left-pad com npm",
        {{"install_package", "{\"manager\": \"npm\", \"name\": \"left-pad\"}", 1}}, 1, 0, NULL, "", "", 1},
    {"install_package", NULL, "install the package tabulate with the manager conda", "instala o pacote tabulate com o gerenciador conda",
        {{"install_package", "{\"manager\": \"conda\", \"name\": \"tabulate\"}", 1}}, 1, 1, NULL, " Tell me which of the supported managers to use.", " Me diz qual dos gerenciadores aceitos devo usar.", 1},
    {"read_log", NULL, "show me the last lines of logs/app.log", "me mostra as ultimas linhas de logs/app.log",
        {{"read_log", "{\"source\": \"{cwd}/logs/app.log\", \"lines\": 5}", 1}}, 1, 0, NULL, "", ""},
    {"read_log", NULL, "are there errors in logs/app.log?", "tem erro em logs/app.log?",
        {{"read_log", "{\"source\": \"{cwd}/logs/app.log\", \"lines\": 20, \"filter\": \"ERROR\"}", 1}}, 1, 0, NULL, "", ""},
    {"read_log", NULL, "show me the log logs/worker.log", "me mostra o log logs/worker.log",
        {{"read_log", "{\"source\": \"{cwd}/logs/worker.log\", \"lines\": 10}", 1}}, 1, 1, NULL, " The only log in logs/ is app.log.", " O unico log em logs/ e o app.log."},
    {"howto", NULL, "what is your recipe for installing packages?", "qual e a sua receita para instalar pacotes?",
        {{"howto", "{\"topic\": \"packages\"}", 1}}, 1, 0, NULL, "", ""},
    {"howto", NULL, "show me your recipe for reading logs", "me mostra a sua receita de ler logs",
        {{"howto", "{\"topic\": \"logs\"}", 1}}, 1, 0, NULL, "", ""},
    {"howto", NULL, "look up your recipe for flying a kite", "procura a sua receita de empinar pipa",
        {{"howto", "{\"topic\": \"flying a kite\"}", 1}}, 1, 0, NULL, " If that is not what you wanted, there is no recipe for it.", " Se nao era isso que voce queria, nao ha receita para isso."},
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
    {"TaskCreate", "TaskCreate", "create two numbered tasks with details for me: 1) Write the parser (tokens first) 2) Add the tests (one per rule)", "cria duas tarefas numeradas com detalhes pra mim: 1) Escrever o parser (tokens primeiro) 2) Acrescentar os testes (um por regra)",
        {{"TaskCreate", "{\"subject\": \"Write the parser\", \"description\": \"tokens first\"}"}, {"TaskCreate", "{\"subject\": \"Add the tests\", \"description\": \"one per rule\"}"}}, 2, 0, NULL, " Both tasks are on the list, with their details.", " As duas tarefas estao na lista, com os detalhes."},
    {"TaskCreate", "TaskCreate,TaskList", "use TaskCreate to add the task \"Update the docs\" with the detail \"list every function\", then show me the task list", "usa o TaskCreate pra criar a tarefa \"Update the docs\" com o detalhe \"list every function\", depois me mostra a lista de tarefas",
        {{"TaskCreate", "{\"subject\": \"Update the docs\", \"description\": \"list every function\"}"}, {"TaskList", "{}"}}, 2, 0, NULL, "", ""},
    {"TaskCreate", "TaskCreate", "add a task with TaskCreate, I will give you the title later", "cria uma tarefa com o TaskCreate, o titulo eu te passo depois",
        {{"TaskCreate", "{\"subject\": \"\", \"description\": \"\"}"}}, 1, 1, NULL, " Give me the title and I will create it.", " Me passa o titulo que eu crio."},
    {"TaskUpdate", "TaskCreate,TaskUpdate", "create the task \"Review the settings\" (detail: ports and timeouts) and mark it as in progress", "cria a tarefa \"Review the settings\" (detalhe: ports and timeouts) e marca como em andamento",
        {{"TaskCreate", "{\"subject\": \"Review the settings\", \"description\": \"ports and timeouts\"}"}, {"TaskUpdate", "{\"taskId\": \"1\", \"status\": \"in_progress\"}"}}, 2, 0, NULL, "", ""},
    {"TaskUpdate", "TaskCreate,TaskUpdate", "create the task \"Tag the release\" (detail: after the tests pass) and then mark it completed, it is already done", "cria a tarefa \"Tag the release\" (detalhe: after the tests pass) e depois marca como concluida, ja esta feita",
        {{"TaskCreate", "{\"subject\": \"Tag the release\", \"description\": \"after the tests pass\"}"}, {"TaskUpdate", "{\"taskId\": \"1\", \"status\": \"completed\"}"}}, 2, 0, NULL, "", ""},
    {"TaskUpdate", "TaskUpdate", "mark task 9 as completed", "marca a tarefa 9 como concluida",
        {{"TaskUpdate", "{\"taskId\": \"9\", \"status\": \"completed\"}"}}, 1, 1, NULL, " Nothing was changed; TaskList shows the numbers that exist.", " Nada foi alterado; o TaskList mostra os numeros que existem."},
    {"TaskGet", "TaskCreate,TaskGet", "create the task \"Clean tools/\" with the detail \"remove the old scripts\" and show me everything stored about it", "cria a tarefa \"Clean tools/\" com o detalhe \"remove the old scripts\" e me mostra tudo que ficou guardado dela",
        {{"TaskCreate", "{\"subject\": \"Clean tools/\", \"description\": \"remove the old scripts\"}"}, {"TaskGet", "{\"taskId\": \"1\"}"}}, 2, 0, NULL, " The rest of its fields are in the result above.", " Os outros campos dela estao no resultado acima."},
    {"TaskGet", "TaskGet", "show me the details of task 7", "me mostra os detalhes da tarefa 7",
        {{"TaskGet", "{\"taskId\": \"7\"}"}}, 1, 1, NULL, " TaskList shows the tasks that exist.", " O TaskList mostra as tarefas que existem."},
    {"TaskGet", "TaskCreate,TaskGet", "add the task \"Write the changelog\" (detail: one line per version) and read it back to me", "cria a tarefa \"Write the changelog\" (detalhe: one line per version) e le ela de volta pra mim",
        {{"TaskCreate", "{\"subject\": \"Write the changelog\", \"description\": \"one line per version\"}"}, {"TaskGet", "{\"taskId\": \"1\"}"}}, 2, 0, NULL, " The rest of its fields are in the result above.", " Os outros campos dela estao no resultado acima."},
    {"TaskList", "TaskList", "show me the task list", "me mostra a lista de tarefas",
        {{"TaskList", "{}"}}, 1, 0, NULL, "", ""},
    {"TaskList", "TaskCreate,TaskList", "add the task \"Check the ports\" (detail: compare with docs) and list the tasks", "cria a tarefa \"Check the ports\" (detalhe: compare with docs) e lista as tarefas",
        {{"TaskCreate", "{\"subject\": \"Check the ports\", \"description\": \"compare with docs\"}"}, {"TaskList", "{}"}}, 2, 0, NULL, "", ""},
    {"TaskList", "TaskList", "which numbered tasks are still open?", "que tarefas numeradas ainda estao abertas?",
        {{"TaskList", "{}"}}, 1, 0, NULL, "", ""},
};
#define X_NRARE CU_N(g_x_rare)
static const char *x_rsub(const char *t, const char *key, const char *val) {
    const char *q = strstr(t, key); if (!q) return t;
    return x_rsub(cu_fmt("%.*s%s%s", (int)(q - t), t, val, q + strlen(key)), key, val);
}
static void fam_x_rare(FILE *f, Scn *s) {
    const int ei = g_x_kforce >= 0 ? g_x_kforce : (int)((g_x_cycle2 + g_cu_att) % X_NRARE);
    const XRare *e = &g_x_rare[ei];
    XC c; memset(&c, 0, sizeof c); c.s = s;
    x_proj(&c.p, s, CU_PA(((const int[]){X_PY, X_JS, X_SH, X_PL})), 2, 2); XProj *p = &c.p;
    p->readme = cu_pct(50); p->knows = 0;
    if (!strcmp(e->tool, "read_log")) x_extra(p, "logs/app.log", cu_fmt("2026-03-02 10:00:01 INFO %s started\n2026-03-02 10:00:02 INFO loaded 3 modules\n2026-03-02 10:04:10 ERROR could not open cache file\n2026-03-02 10:04:11 INFO retrying without cache\n2026-03-02 10:04:12 INFO done\n", p->name), 0);
    if (e->env) setenv(e->env, !strcmp(e->env, "KATANA_SCHED_NOW") ? "1770000000" : "1", 1);
    g_ct_sub_run = NULL;
    x_begin("x-rare", s, p, 0, 0);
    g_x.k = ei; g_x.k2 = 1; g_x.nudge_ok = !strncmp(e->tool, "Task", 4);
    x_cell("rare/%s/%d%s", e->tool, ei, e->standin ? "/standin" : "");
    if (!strcmp(e->tool, "install_package")) {      /* gerenciadores de mentira: dizem o que fariam e nao instalam nada */
        cu_put(cu_fmt("%s/bin/python3", g_cu_sb), "#!/bin/sh\n# stand-in: only answers python3 -m pip, and installs nothing\nif [ \"$1\" = \"-m\" ] && [ \"$2\" = \"pip\" ]; then echo \"Successfully installed (stand-in, nothing was installed)\"; exit 0; fi\nexit 1\n", 0755);
        cu_put(cu_fmt("%s/bin/npm", g_cu_sb), "#!/bin/sh\necho \"added 1 package in 1s\"\n", 0755);
    }
    /* nenhum aviso de verdade na area de trabalho: um notify-send de mentira fica na frente no PATH */
    cu_put(cu_fmt("%s/bin/notify-send", g_cu_sb), "#!/bin/sh\nexit 0\n", 0755);
    x_user(L(e->en, e->pt));
    const int ncalls = e->nc + (e->sel != NULL);
    const char *plan[4]; int np = 0;
    if (ncalls >= 3 && strncmp(e->tool, "Task", 4)) {      /* Task* e TodoWrite sao a mesma lista por baixo: sem plano proprio nessas conversas */
        plan[np++] = L("Get the tools", "Pegar as ferramentas");
        for (int i = 0; i < e->nc; i++) plan[np++] = cu_fmt(L("Call %s", "Chamar %s"), e->c[i].name);
        x_plan(plan, np);
    }
    if (e->sel) { x_tsel(e->sel); if (np) x_done(1); }
    const char *r = "", *id = "";
    for (int i = 0; i < e->nc; i++) {
        const char *a = x_rsub(x_rsub(x_rsub(x_rsub(e->c[i].args, "{cwd}", s->cwd), "{name}", p->name), "{testrel}", p->testrel), "{id}", id);
        r = cu_fmt("%s", x_call(NULL, e->c[i].name, a, XF_ERR | (e->c[i].old ? XF_OLD : 0)));
        if (getenv("KATANA_CU_SHOW")) fprintf(stderr, "RARE %d %s %s => %.260s\n", ei, e->c[i].name, a, r);
        const char *tk = strstr(r, "task ");
        if (tk && !*id) { id = cu_fmt("%.*s", (int)strcspn(tk + 5, " "), tk + 5); if (g_x.nsub < 4 && strlen(id) == 8) { g_x.sfrom[g_x.nsub] = id; g_x.sto[g_x.nsub++] = "5c0de001"; } }
        if (np && i + 1 < e->nc) x_done(i + 2);
    }
    if (e->env) unsetenv(e->env);
    ct_sched_reset(); g_ct_end_conversation = 0; g_ct_sub_run = NULL;
    if (x_err(r) != (e->bad != 0)) cu_die("x-rare %d (%s): esperava %s e veio: %.200s", ei, e->tool, e->bad ? "erro" : "resultado", r);
    const char *one = x_err(r) ? r + 7 : r; while (*one == '\n' || *one == ' ') one++;
    char *ln = cu_fmt("%.*s", (int)strcspn(one, "\n"), one);
    if (strlen(ln) > 240) { ln[240] = 0; char *sp = strrchr(ln, ' '); if (sp) *sp = 0; ln = cu_fmt("%s...", ln); }
    if (!*ln) cu_die("x-rare %d: resultado vazio", ei);
    x_final(cu_fmt(e->bad ? L("That did not work. The engine says: \"%s\".%s", "Nao deu certo. O motor diz: \"%s\".%s") : L("Done. The engine says: \"%s\".%s", "Feito. O motor diz: \"%s\".%s"), ln, L(e->nen, e->npt)));
    if (g_cu_hold && strncmp(e->tool, "Task", 4) && strcmp(e->tool, "EndConversation")) { c.fu = cu_pct(50) ? F_FIND : F_EXPLAIN; x_followup(&c); }      /* holdout: um pedido de outro mecanismo na mesma conversa */
    x_end(f);
}
