.pragma library

// Verbos das ferramentas e formatacao de tempo, comuns ao grupo de acoes e ao plano
var verbos = ({
    "list_dir": "Listando pasta", "read_file": "Lendo arquivo", "write_file": "Gravando arquivo",
    "edit_file": "Editando arquivo", "run_command": "Executando comando", "search_files": "Procurando nos arquivos",
    "glob": "Buscando arquivos", "web_search": "Pesquisando na web", "web_fetch": "Abrindo página",
    "download_file": "Baixando arquivo", "install_package": "Instalando pacote", "git": "Usando git",
    "read_document": "Lendo documento", "terminal": "Usando o terminal", "read_log": "Lendo logs", "terminal_screen": "Lendo a tela do terminal", "remember": "Guardando na memória", "recall": "Consultando a memória",
    // segundo conjunto de ferramentas (src/tools/)
    "Bash": "Executando comando", "Read": "Lendo arquivo", "Write": "Gravando arquivo", "Edit": "Editando arquivo",
    "NotebookEdit": "Editando notebook", "Glob": "Buscando arquivos", "Grep": "Procurando nos arquivos",
    "WebSearch": "Pesquisando na web", "WebFetch": "Abrindo página", "Agent": "Subagente trabalhando",
    "TodoWrite": "Atualizando as tarefas", "TaskCreate": "Criando tarefa", "TaskUpdate": "Atualizando tarefa", "TaskList": "Listando tarefas", "TaskGet": "Lendo tarefa",
    "TaskStop": "Parando tarefa", "Monitor": "Acompanhando comando", "AskUserQuestion": "Perguntando", "EnterPlanMode": "Entrando no modo plano", "ExitPlanMode": "Apresentando o plano",
    "LSP": "Consultando o código", "EnterWorktree": "Abrindo cópia de trabalho", "ExitWorktree": "Saindo da cópia de trabalho", "PowerShell": "Executando PowerShell",
    "CronCreate": "Agendando tarefa", "CronDelete": "Cancelando agendamento", "CronList": "Listando agendamentos", "ScheduleWakeup": "Marcando retomada",
    "PushNotification": "Enviando notificação", "SendUserFile": "Entregando arquivo", "EndConversation": "Encerrando a conversa",
    "ListMcpResourcesTool": "Listando recursos MCP", "ReadMcpResourceTool": "Lendo recurso MCP", "WaitForMcpServers": "Aguardando servidores MCP",
    "ToolSearch": "Carregando ferramenta", "Skill": "Carregando instruções", "ReportFindings": "Relatando achados",
    "SendMessage": "Falando com outro agente", "ListAgents": "Listando agentes", "SubagentHandback": "Entregando relatório", "Workflow": "Rodando fluxo de agentes"
});

function verbo(n) { return verbos[n] || n || "Usando ferramenta"; }
function alvo(a) {
    var s = (a || "").split(" | ")[0];
    var i = s.indexOf(": ");
    if (i > 0 && i < 20) s = s.slice(i + 2);
    return s.length > 90 ? s.slice(0, 90) + "…" : s;
}
// linha principal de uma acao: a descricao dada pelo modelo, ou verbo + alvo
function titulo(act) {
    if (act.desc) return act.desc;
    var t = alvo(act.args);
    return verbo(act.name) + (t ? ": " + t : "");
}
// duracao curta: "<1 s", "35 s", "1 min 20 s", "4 min"
function dur(ms) {
    if (ms < 0 || isNaN(ms)) return "";
    if (ms < 1000) return "<1 s";
    var s = Math.round(ms / 1000);
    if (s < 60) return s + " s";
    if (s >= 120) return Math.round(s / 60) + " min";
    return s % 60 ? "1 min " + (s - 60) + " s" : "1 min";
}
