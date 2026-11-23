/* Terminal embutido: um shell real (pty no servidor) com cores e edicao de linha; as teclas vao direto ao shell.
   O emulador e o mesmo de desktop-qt/src/terminal.cpp (celulas, SGR, cursor, apagar linha/tela). */
(function () {
  var ANSI = [0x1e1e24, 0xe06c75, 0x98c379, 0xe5c07b, 0x61afef, 0xc678dd, 0x56b6c2, 0xd4d4d8, 0x6b7280, 0xff7b86, 0xb5e890, 0xf0d58c, 0x7dc0ff, 0xd899f0, 0x7fd4de, 0xffffff];
  var MAXLINES = 5000;
  function xterm256(n) {
    if (n < 16) return ANSI[n];
    if (n >= 232) { var v = 8 + (n - 232) * 10; return (v << 16) | (v << 8) | v; }
    n -= 16;
    function lv(x) { return x === 0 ? 0 : 55 + x * 40; }
    return (lv(Math.floor(n / 36)) << 16) | (lv(Math.floor(n / 6) % 6) << 8) | lv(n % 6);
  }
  function hex(c) { return "#" + ("000000" + (c & 0xffffff).toString(16)).slice(-6); }
  function cell() { return { ch: " ", fg: -1, bg: -1, bold: false, inv: false }; }

  var T = K.term = { lines: [[]], r: 0, c: 0, cols: 100, rows: 24, pen: cell(), st: 0, par: "", es: null, open: false, running: false, timer: 0, stick: true };
  function cur() { while (T.r >= T.lines.length) T.lines.push([]); return T.lines[T.r]; }
  function newline() {
    T.r++;
    while (T.r >= T.lines.length) T.lines.push([]);
    if (T.lines.length > MAXLINES) { var d = T.lines.length - MAXLINES; T.lines.splice(0, d); T.r -= d; if (T.r < 0) T.r = 0; }
  }
  function putChar(ch) {
    if (T.c >= T.cols) { T.c = 0; newline(); }
    var l = cur();
    while (l.length <= T.c) l.push(cell());
    var c = { ch: ch, fg: T.pen.fg, bg: T.pen.bg, bold: T.pen.bold, inv: T.pen.inv };
    l[T.c++] = c;
  }
  function sgr(p) {
    if (!p.length) { T.pen = cell(); return; }
    for (var i = 0; i < p.length; i++) {
      var v = p[i];
      if (v === 0) T.pen = cell();
      else if (v === 1) T.pen.bold = true;
      else if (v === 22) T.pen.bold = false;
      else if (v === 7) T.pen.inv = true;
      else if (v === 27) T.pen.inv = false;
      else if (v >= 30 && v <= 37) T.pen.fg = ANSI[v - 30];
      else if (v >= 90 && v <= 97) T.pen.fg = ANSI[v - 90 + 8];
      else if (v >= 40 && v <= 47) T.pen.bg = ANSI[v - 40];
      else if (v >= 100 && v <= 107) T.pen.bg = ANSI[v - 100 + 8];
      else if (v === 39) T.pen.fg = -1;
      else if (v === 49) T.pen.bg = -1;
      else if ((v === 38 || v === 48) && i + 1 < p.length) {
        var col = 0;
        if (p[i + 1] === 5 && i + 2 < p.length) { col = xterm256(p[i + 2] & 255); i += 2; }
        else if (p[i + 1] === 2 && i + 4 < p.length) { col = ((p[i + 2] & 255) << 16) | ((p[i + 3] & 255) << 8) | (p[i + 4] & 255); i += 4; }
        else break;
        if (v === 38) T.pen.fg = col; else T.pen.bg = col;
      }
    }
  }
  function csi(ps, fin) {
    if (ps && (ps[0] === "?" || ps[0] === ">" || ps[0] === "=")) return;
    var p = ps.split(";").map(function (s) { return s === "" ? -1 : parseInt(s, 10); });
    function arg(i, def) { return i < p.length && p[i] > 0 ? p[i] : def; }
    var l, n, i, mode;
    switch (fin) {
      case "m": sgr(p.map(function (v) { return v < 0 ? 0 : v; })); break;
      case "A": T.r = Math.max(0, T.r - arg(0, 1)); break;
      case "B": T.r += arg(0, 1); cur(); break;
      case "C": T.c = Math.min(T.cols, T.c + arg(0, 1)); break;
      case "D": T.c = Math.max(0, T.c - arg(0, 1)); break;
      case "G": T.c = Math.min(T.cols - 1, arg(0, 1) - 1); break;
      case "H": case "f": T.r = Math.max(0, T.lines.length - T.rows) + arg(0, 1) - 1; T.c = Math.min(T.cols - 1, arg(1, 1) - 1); cur(); break;
      case "J":
        mode = !p.length || p[0] < 0 ? 0 : p[0];
        if (mode >= 2) { T.lines = [[]]; T.r = T.c = 0; }
        else if (mode === 0) { l = cur(); if (l.length > T.c) l.length = T.c; T.lines.length = T.r + 1; }
        break;
      case "K":
        mode = !p.length || p[0] < 0 ? 0 : p[0]; l = cur();
        if (mode === 0) { if (l.length > T.c) l.length = T.c; }
        else if (mode === 1) { for (i = 0; i < Math.min(T.c + 1, l.length); i++) l[i] = cell(); }
        else l.length = 0;
        break;
      case "P": l = cur(); n = arg(0, 1); if (T.c < l.length) l.splice(T.c, Math.min(n, l.length - T.c)); break;
      case "@": l = cur(); while (l.length < T.c) l.push(cell()); for (i = 0; i < arg(0, 1); i++) l.splice(T.c, 0, cell()); break;
      case "X": l = cur(); for (i = T.c; i < Math.min(l.length, T.c + arg(0, 1)); i++) l[i] = cell(); break;
    }
  }
  function feed(s) {
    for (var k = 0; k < s.length; k++) {
      var ch = s[k], u = ch.charCodeAt(0);
      switch (T.st) {
        case 0:
          if (u === 0x1b) T.st = 1;
          else if (ch === "\r") T.c = 0;
          else if (ch === "\n") { newline(); T.c = 0; }
          else if (ch === "\b") { if (T.c > 0) T.c--; }
          else if (ch === "\t") { var n = 8 - T.c % 8; for (var i = 0; i < n; i++) putChar(" "); }
          else if (u === 7 || u < 32) { }
          else putChar(ch);
          break;
        case 1:
          if (ch === "[") { T.st = 2; T.par = ""; } else if (ch === "]") T.st = 3; else T.st = 0;
          break;
        case 2:
          if ((ch >= "0" && ch <= "9") || ch === ";" || ch === "?" || ch === ">" || ch === "=" || ch === " " || ch === "!") T.par += ch;
          else { csi(T.par, ch); T.st = 0; }
          break;
        case 3: if (u === 7) T.st = 0; else if (u === 0x1b) T.st = 4; break;
        case 4: T.st = 0; break;
      }
    }
    schedule();
  }
  function schedule() { if (!T.timer) T.timer = setTimeout(render, 30); }
  function esc(ch) { return ch === "<" ? "&lt;" : ch === ">" ? "&gt;" : ch === "&" ? "&amp;" : ch; }
  function render() {
    T.timer = 0;
    var view = $("termView");
    if (!view) return;
    var out = '<div style="white-space:pre">';
    for (var r = 0; r < T.lines.length; r++) {
      var l = T.lines[r], n = l.length, hasCur = r === T.r;
      if (!hasCur) while (n > 0 && l[n - 1].ch === " " && l[n - 1].bg < 0 && !l[n - 1].inv) n--;
      var lim = hasCur ? Math.max(n, T.c + 1) : n, pf = -2, pb = -2, pbold = false, pinv = false, open = false;
      for (var i = 0; i < lim; i++) {
        var c = i < l.length ? l[i] : cell(), isCur = hasCur && i === T.c, inv = c.inv !== isCur;
        if (!open || c.fg !== pf || c.bg !== pb || c.bold !== pbold || inv !== pinv) {
          if (open) out += "</span>";
          var fg = c.fg, bg = c.bg, st = "";
          if (inv) { var f = fg >= 0 ? fg : 0xd4d4d8, b = bg >= 0 ? bg : 0x1b1b21; fg = b; bg = f; }
          if (fg >= 0) st += "color:" + hex(fg) + ";";
          if (bg >= 0) st += "background-color:" + hex(bg) + ";";
          if (c.bold) st += "font-weight:bold;";
          out += '<span style="' + st + '">';
          open = true; pf = c.fg; pb = c.bg; pbold = c.bold; pinv = inv;
        }
        out += esc(c.ch);
      }
      if (open) out += "</span>";
      if (r + 1 < T.lines.length) out += "<br>";
    }
    out += "</div>";
    view.innerHTML = out;
    var body = $("termBody");
    if (T.stick) body.scrollTop = body.scrollHeight;
  }

  function send(data) { K.post("web/term/key", { data: data }).catch(function () {}); }
  function sizeNow() {
    var body = $("termBody");
    T.cols = Math.max(20, Math.floor(((body ? body.clientWidth : 900) - 12) / 7.8));
    T.rows = Math.max(5, Math.floor(((body ? body.clientHeight : 300)) / 17));
  }
  K.termStart = function () {
    if (T.running) return;
    sizeNow();
    T.lines = [[]]; T.r = T.c = 0; T.pen = cell(); T.st = 0;
    K.post("web/term/start", { cols: T.cols, rows: T.rows }).then(function () {
      T.running = true;
      if (T.es) T.es.close();
      T.es = new EventSource("web/term/events?from=0");
      var dec = function (e) { feed(JSON.parse(e.data).d); };
      T.es.addEventListener("d", dec);
      T.es.addEventListener("x", function () { T.running = false; for (var i = 0; i < "\r\n[processo encerrado]".length; i++) { } feed("\r\n[processo encerrado]"); });
      T.es.addEventListener("reset", function () { });
    }).catch(function (e) { feed("Nao foi possivel abrir o terminal: " + e.message); });
  };
  K.termStop = function () { if (T.es) { T.es.close(); T.es = null; } T.running = false; K.post("web/term/stop").catch(function () {}); };
  K.termFocus = function () { var v = $("termView"); if (v) v.focus(); };

  var LABELS = [["Limpar", "Limpar o terminal"], ["Reiniciar", "Reiniciar o terminal"]];
  K.buildTerm = function () {
    var panel = $("term");
    panel.textContent = "";
    var head = h("div", { class: "term-head" }, h("span", { class: "term-title", text: "Terminal" }), h("span", { class: "grow" }),
      h("button", { class: "tbtn", text: "Limpar", onclick: function () { T.lines = [[]]; T.r = T.c = 0; render(); K.termFocus(); } }),
      h("button", { class: "tbtn", text: "Reiniciar", onclick: function () { K.termStop(); T.lines = [[]]; T.r = T.c = 0; render(); K.termStart(); K.termFocus(); } }),
      h("button", { class: "tbtn sq", text: "✕", onclick: function () { K.toggleTerminal(); } }));
    var view = h("div", { id: "termView", class: "term-view", tabindex: "0", "aria-label": "Terminal" });
    var body = h("div", { id: "termBody", class: "term-body" }, view);
    panel.appendChild(h("div", { class: "term-line" }));
    panel.appendChild(head); panel.appendChild(body);
    body.addEventListener("scroll", function () { T.stick = body.scrollHeight - body.scrollTop - body.clientHeight < 30; });
    view.addEventListener("paste", function (e) {
      var t = (e.clipboardData || window.clipboardData).getData("text");
      if (t) send(t.replace(/\r\n/g, "\n").replace(/\n/g, "\r"));
      e.preventDefault();
    });
    view.addEventListener("keydown", function (e) {
      var ctrl = e.ctrlKey, shift = e.shiftKey, alt = e.altKey;
      if (ctrl && shift && (e.key === "C" || e.key === "c")) { var s = getSelection().toString(); if (s) B_copy(s); e.preventDefault(); return; }
      if ((ctrl && shift && (e.key === "V" || e.key === "v")) || (shift && e.key === "Insert")) { return; }   /* o evento paste cuida */
      if (e.metaKey) return;
      var map = { Enter: "\r", Backspace: "\x7f", Tab: "\t", Escape: "\x1b", ArrowUp: "\x1b[A", ArrowDown: "\x1b[B", ArrowRight: ctrl ? "\x1b[1;5C" : "\x1b[C",
        ArrowLeft: ctrl ? "\x1b[1;5D" : "\x1b[D", Home: "\x1b[H", End: "\x1b[F", Delete: "\x1b[3~" };
      if (e.key === "PageUp") { body.scrollTop = Math.max(0, body.scrollTop - body.clientHeight * 0.9); e.preventDefault(); return; }
      if (e.key === "PageDown") { body.scrollTop += body.clientHeight * 0.9; e.preventDefault(); return; }
      if (["Shift", "Control", "Alt", "Meta", "AltGraph", "CapsLock"].indexOf(e.key) >= 0) return;
      if (map[e.key] !== undefined) { send(map[e.key]); T.stick = true; e.preventDefault(); return; }
      if (ctrl && !alt && e.key.length === 1) {
        var code = e.key.toUpperCase().charCodeAt(0);
        if (code >= 64 && code <= 95) { send(String.fromCharCode(code - 64)); T.stick = true; e.preventDefault(); return; }
      }
      if (e.key.length === 1) { send(alt ? "\x1b" + e.key : e.key); T.stick = true; e.preventDefault(); }
    });
  };
  function B_copy(t) { K.be.copyText(t); }
})();
