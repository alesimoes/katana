.pragma library

// Verbos das ferramentas e formatacao de tempo, comuns ao grupo de acoes e ao plano
var verbos = ({
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
    // ferramentas do indice do codigo e de mudanca em varios lugares
    "Find": "Procurando no projeto", "Symbols": "Consultando os símbolos", "Rename": "Renomeando símbolo",
    "Replace": "Substituindo texto", "Patch": "Aplicando patch",
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
    var t = resumo(act.name, act.args);
    return verbo(act.name) + (t ? ": " + t : "");
}
// argumentos como "chave: valor | chave: valor" (ja limpos pelo backend) em um mapa
function campos(a) {
    var m = ({}), ps = (a || "").split(" | ");
    for (var i = 0; i < ps.length; i++) {
        var k = ps[i].indexOf(": ");
        if (k > 0 && k < 24) m[ps[i].slice(0, k)] = ps[i].slice(k + 2);
    }
    return m;
}
function corta(s, n) { return s.length > n ? s.slice(0, n) + "…" : s; }
// resumo de uma linha dos argumentos: Find = consulta (e modo), Symbols = acao + nome/arquivo, Rename = de -> para
function resumo(name, args) {
    var m = campos(args);
    if (name === "Find") {
        if (!m.query) return alvo(args);
        return corta(m.query, 70) + (m.mode ? " (" + m.mode + ")" : "") + (m.path ? " em " + corta(m.path, 30) : "");
    }
    if (name === "Symbols") {
        if (!m.action) return alvo(args);
        var al = m.name || m.file || m.path || "";
        if (m.name && m.file) al = m.name + " em " + m.file;
        else if (m.file && m.line) al = m.file + ":" + m.line;
        return m.action + (al ? " " + corta(al, 70) : "");
    }
    if (name === "Rename" && m.symbol) return corta(m.symbol, 40) + " -> " + corta(m.to || "?", 40);
    if (name === "Replace" && m.pattern) return corta(m.pattern, 40) + " -> " + corta(m.replacement || "", 40);
    return alvo(args);
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

// resultado de Find e Symbols em HTML simples (Qt rich text) com o que importa em cor: [confidence N nivel], "called by:", "calls:",
// "links:" e NOT FOUND. c = {ok, mid, low, err, lbl} (cores do tema como texto). Outras ferramentas: null (texto puro)
function realcavel(name) { return name === "Find" || name === "Symbols"; }
function esc(t) { return t.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;"); }
function realce(text, c) {
    var lines = (text || "").split("\n"), out = [];
    for (var i = 0; i < lines.length; i++) {
        var l = esc(lines[i]);
        l = l.replace(/\[confidence ([^\]]*)\]/g, function (m, g) {
            var col = /\bhigh\b/.test(g) ? c.ok : /\blow\b/.test(g) ? c.low : c.mid;
            return '<b><font color="' + col + '">' + m + '</font></b>';
        });
        l = l.replace(/^(\s*)(called by:|calls:|links:|also referenced in:)/, function (m, sp, lab) {
            return sp + '<b><font color="' + c.lbl + '">' + lab + '</font></b>';
        });
        l = l.replace(/^(\s*)(NOT FOUND)/, function (m, sp, lab) { return sp + '<b><font color="' + c.err + '">' + lab + '</font></b>'; });
        out.push(l);
    }
    return '<pre style="margin:0; white-space:pre-wrap">' + out.join("\n") + '</pre>';
}
