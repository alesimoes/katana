.pragma library

// Renderizacao simples: `codigo`, **negrito** e blocos ``` (o resto e texto puro).
function esc(s) {
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

function inl(s, codeBg) {
    s = s.replace(/^\n+|\n+$/g, "");
    if (!s.trim()) return "";
    var h = esc(s)
        .replace(/`([^`\n]+)`/g, '<span style="background-color:' + codeBg + '; font-family:monospace">&nbsp;$1&nbsp;</span>')
        .replace(/\*\*([^*\n]+)\*\*/g, "<b>$1</b>");
    return '<div style="white-space:pre-wrap; line-height:150%">' + h + "</div>";
}

// devolve [{type:"text", html}, {type:"code", lang, code}]
function parse(s, codeBg) {
    var out = [], re = /```([^\n`]*)\n?([\s\S]*?)(```|$)/g, last = 0, m, h;
    while ((m = re.exec(s))) {
        h = inl(s.slice(last, m.index), codeBg);
        if (h) out.push({ type: "text", html: h });
        out.push({ type: "code", lang: m[1].trim() || "codigo", code: m[2].replace(/\n$/, "") });
        last = re.lastIndex;
        if (m[0] === "") break;
    }
    h = inl(s.slice(last), codeBg);
    if (h) out.push({ type: "text", html: h });
    return out;
}
