/* O estado do agente na pagina: a mesma traducao do backend do desktop (desktop-qt/src/backend.cpp). O servidor entrega o que o
   agente escreve como eventos (t = texto, m = linha de marcador \x01..., e = stderr, s = iniciou, x = saiu); aqui eles viram
   mensagens, cartoes, plano, fila e a barra de baixo. Escreve no agente por POST /web/line. */
(function () {
  var B = K.be = {
    ready: false, loading: false, answering: false, running: false, model: "", models: [], status: "", error: "",
    ctxUsed: 0, ctxTotal: 0, speed: "", queue: [], paused: false, planMode: false, teacher: false, teacherModel: "",
    todo: [], choice: null, ws: "", wsTrusted: false, sessions: [], sess: null, dirty: false, bound: "", pendingCtx: "",
    resumeCtx: "", resumeWant: "", resuming: false, resumed: false, msgs: new K.Messages(), curPlan: 0, reqStart: 0, reqActions: 0,
    progress: "", stats: "", errTail: [], specsActive: [], specsSkipped: [], stopRequested: false, engineVersion: "", engineChanges: "",
    specsDir: "", sessDir: "", ui: {}, activityText: "", stalled: false, sysText: "", indexText: "", indexTip: "", ask: null, pw: null,
    hasTeacherKey: false, cwd: "", turnStart: 0, lastOut: 0, lastSecs: 0
  };

  /* ---- filtro das etiquetas de chamada de ferramenta que o modelo escreve no texto ---- */
  var TagFilter = function () { this.buf = ""; this.closer = ""; };
  var OPENS = ["<tool_call>", "<|tool_call>", "<tool_response>", "<|tool_response>"];
  function closerFor(o) { return o === "<tool_call>" ? "</tool_call>" : o === "<|tool_call>" ? "<tool_call|>" : o === "<tool_response>" ? "</tool_response>" : "<tool_response|>"; }
  TagFilter.prototype.feed = function (inp) {
    this.buf += inp;
    var out = "";
    for (;;) {
      if (this.closer) {
        var i = this.buf.indexOf(this.closer);
        if (i < 0) { var keep = this.closer.length - 1; if (this.buf.length > keep) this.buf = this.buf.slice(this.buf.length - keep); return out; }
        this.buf = this.buf.slice(i + this.closer.length); this.closer = "";
        continue;
      }
      var best = "", bi = -1;
      for (var k = 0; k < OPENS.length; k++) { var j = this.buf.indexOf(OPENS[k]); if (j >= 0 && (bi < 0 || j < bi)) { best = OPENS[k]; bi = j; } }
      if (bi >= 0) { out += this.buf.slice(0, bi); this.buf = this.buf.slice(bi + best.length); this.closer = closerFor(best); continue; }
      var cut = this.buf.length;
      for (var n = 1; n < 20 && n <= this.buf.length; n++) {
        var suf = this.buf.slice(this.buf.length - n);
        for (k = 0; k < OPENS.length; k++) if (OPENS[k].indexOf(suf) === 0) cut = this.buf.length - n;
      }
      out += this.buf.slice(0, cut); this.buf = this.buf.slice(cut);
      return out;
    }
  };
  TagFilter.prototype.flush = function () { var s = this.buf; this.buf = ""; return this.closer ? "" : s; };
  TagFilter.prototype.reset = function () { this.buf = ""; this.closer = ""; };
  var filter = new TagFilter();

  B.thinking = function () { return B.answering && B.msgs.lastKind() !== "assistant"; };
  B.workspace = function () { return B.ws || B.ui.workspace || B.cwd; };

  /* ---- estado ---- */
  B.setTexts = function (status, error) { B.status = status; B.error = error; K.redraw(); };
  B.setState = function (ready, loading, answering) {
    if ((loading || answering) && !(B.loading || B.answering)) { B.turnStart = Date.now(); B.lastOut = Date.now(); }
    var was = B.answering;
    if (answering && !was) beginRequest();
    B.ready = ready; B.loading = loading; B.answering = answering;
    if (was && !answering) endRequest();
    if (!loading && !answering && (B.activityText || B.stalled)) { B.activityText = ""; B.stalled = false; }
    K.redraw();
  };
  B.setCtx = function (used, total) { B.ctxUsed = used; B.ctxTotal = total; K.redraw(); };

  B.init = function (st) {
    B.models = st.models || []; B.ui = st.ui || {}; B.engineVersion = st.engine || ""; B.engineChanges = (st.changes || "").trim();
    B.specsDir = st.specsDir; B.sessDir = st.sessDir; B.cwd = st.cwd; B.modelsDir = st.modelsDir; B.preselect = st.preselect;
    B.teacherModel = B.ui.professorModelo || ""; B.hasTeacherKey = !!st.hasTeacherKey;
    if (B.ui.workspace) B.ws = B.ui.workspace;
    if (st.agent && st.agent.running) {          /* a pagina foi recarregada com o agente de pe */
      B.running = true; B.model = st.agent.model; B.ws = st.agent.cwd || B.ws;
      B.setState(!st.agent.busy, false, !!st.agent.busy);
    }
  };
  B.hasUsableModel = function () { return B.models.some(function (m) { return m.usable; }); };
  B.autoStartDefault = function () {
    if (B.model || B.running) return;
    var pre = B.preselect, i, m;
    if (pre) for (i = 0; i < B.models.length; i++) { m = B.models[i]; if (m.file === pre && m.usable) { B.start(pre); return; } }
    var last = B.ui.ultimoModelo;
    if (last) for (i = 0; i < B.models.length; i++) { m = B.models[i]; if (m.file === last && m.usable) { B.start(last); return; } }
    for (i = 0; i < B.models.length; i++) { m = B.models[i]; if (m.isDefault && m.usable) { B.start(m.file); return; } }
  };

  /* ---- eventos do servidor ---- */
  var es = null;
  B.connect = function (from) {
    es = new EventSource("web/events?from=" + (from || 0));
    es.addEventListener("s", function (e) { var j = JSON.parse(e.data); B.running = true; if (j.cwd) { B.ws = j.cwd; } K.redraw(); });
    es.addEventListener("t", function (e) { B.lastOut = Date.now(); emitText(filter.feed(JSON.parse(e.data).t)); });
    es.addEventListener("m", function (e) { B.lastOut = Date.now(); emitText(filter.flush()); filter.reset(); handleMarker(JSON.parse(e.data).l); });
    es.addEventListener("e", function (e) { B.lastOut = Date.now(); onStderrLine(JSON.parse(e.data).l); });
    es.addEventListener("x", function (e) { onFinished(JSON.parse(e.data)); });
    es.addEventListener("reset", function () { location.reload(); });
    es.addEventListener("gap", function () { location.reload(); });
    es.onerror = function () { if (!B.disconnected) { B.disconnected = true; K.redraw(); } };
    es.onopen = function () { if (B.disconnected) { B.disconnected = false; K.redraw(); } };
  };

  function writeLine(line) { return K.post("web/line", { line: line }).catch(function (e) { B.setTexts(B.status, "Não foi possível falar com o agente: " + e.message); }); }
  B.writeLine = writeLine;

  /* ---- saida do agente ---- */
  function emitText(t) {
    if (!t) return;
    if (B.status.indexOf("Lendo ") === 0) B.setTexts("", B.error);
    t = t.replace(/\x1b\[[0-9;?]*[A-Za-z]/g, "").replace("[raciocinio]\n", "");
    if (t) assistantText(t);
  }
  function msgObj(papel, texto, nome, hora) { return { papel: papel, texto: texto, nome: nome || "", hora: hora || "", desc: "", estado: 0, dur: -1, plano: 0, passo: 0 }; }
  function assistantText(t) {
    var lastIsAsst = B.msgs.lastKind() === "assistant", piece = t;
    if (!lastIsAsst) {
      piece = piece.replace(/^\s+/, "");
      if (!piece) return;
      B.msgs.append("assistant", piece);
    } else B.msgs.appendToLast(piece);
    if (B.sess) {
      var ms = B.sess.mensagens, last = ms[ms.length - 1];
      if (last && last.papel === "assistant") last.texto += piece;
      else ms.push(msgObj("assistant", piece, "", K.isoLocal()));
      B.dirty = true;
    }
  }
  function cleanArgs(inp) {
    var s = inp.trim();
    var kv = /"(\w+)"\s*:\s*(?:"((?:[^"\\]|\\.)*)"|(true|false|-?\d+)(?=\s*[,}]|$))/g;
    if (s.indexOf("{") >= 0) {
      var parts = [], m;
      while ((m = kv.exec(s)) !== null) {
        var raw = m[2] === undefined ? m[3] : m[2], v = "";
        for (var i = 0; i < raw.length;) {
          if (raw.substr(i, 2) === '\\"') { v += '"'; i += 2; }
          else if (raw.substr(i, 3) === "\\\\n") { v += " "; i += 3; }
          else if (raw.substr(i, 2) === "\\n") { v += " "; i += 2; }
          else if (raw.substr(i, 2) === "\\\\") { v += "\\"; i += 2; }
          else { v += raw[i]; i++; }
        }
        parts.push(m[1] + ": " + v);
      }
      if (parts.length) return parts.join(" | ");
    }
    return s.replace(/<\/?[a-z_=|]+[^>]*>/g, " ").trim();
  }
  function unesc(o) {
    var r = "";
    for (var i = 0; i < o.length; i++) {
      if (o[i] === "\\" && i + 1 < o.length) { i++; r += o[i] === "n" ? "\n" : o[i]; } else r += o[i];
    }
    return r;
  }
  function handleMarker(line) {
    var sp = line.indexOf(" "), name = sp < 0 ? line : line.slice(0, sp), args = sp < 0 ? "" : line.slice(sp + 1), a, tab;
    if (name === "READY") markReady();
    else if (name === "TOOL") {
      var f = args.split("\t");
      var desc = f.length >= 3 ? f.slice(2).join(" ").trim() : "";
      var ar = f.length >= 2 ? cleanArgs(f[1]) : "";
      if (desc) ar = ar.split(" | ").filter(function (p) { return p.indexOf("description: ") !== 0; }).join(" | ");
      toolCard(f[0] || "", ar, desc);
    } else if (name === "TOOLLIVE") B.msgs.setLastToolOut(unesc(args), true);
    else if (name === "TOOLOUT") { tab = args.indexOf("\t"); if (tab >= 0) toolResult(unesc(args.slice(tab + 1))); }
    else if (name === "TOKENS") { a = args.split(" ").filter(Boolean); if (a.length >= 2) B.setCtx(parseInt(a[0], 10) || 0, parseInt(a[1], 10) || 0); }
    else if (name === "COMPACT") { a = args.split(" ").filter(Boolean); if (a.length >= 2) B.msgs.append("notice", "Contexto compactado: " + a[0] + " -> " + a[1] + " tokens"); }
    else if (name === "REGROW") B.msgs.append("notice", "A placa de vídeo liberou memória: recarregando o modelo para ficar mais rápido. A conversa continua.");
    else if (name === "NEEDPASS") { B.pw = { retry: args.trim() === "retry" }; K.redraw(); }
    else if (name === "ASK") { a = args.split("\t"); B.ask = { kind: a[0] || "", what: a[1] || "", detail: a[2] || "" }; K.redraw(); }
    else if (name === "ASKDONE") { B.ask = null; K.redraw(); }
    else if (name === "CHANNEL") { a = args.split("\t"); B.msgs.append("notice", (a[0] || "") + ": " + a.slice(1).join(" ")); }
    else if (name === "PREP") {
      a = args.trim().split(" ");
      var total = parseInt(a[0], 10) || 0, done = parseInt(a[1], 10) || 0;
      var pct = total > 0 && a.length > 1 ? Math.floor(done * 100 / total) + "% · " : "";
      B.setTexts("Preparando o agente: " + pct + "lendo as instruções (" + a[0] + " tokens, só na primeira vez nesta pasta)...", "");
    } else if (name === "FEED") {
      a = args.trim().split(" ");
      var t2 = parseInt(a[0], 10) || 0, d2 = parseInt(a[1], 10) || 0;
      if (t2 > 0) B.setTexts("Lendo " + K.ptNum(t2) + " tokens: " + Math.floor(d2 * 100 / t2) + "%", B.error);
    } else if (name === "TEACHW") B.setTexts("Professor escrevendo as ações: " + K.ptDec(parseFloat(args.trim()) / 1024, 1) + " KB recebidos", B.error);
    else if (name === "CHOICE") { a = args.split("\t"); B.choice = { q: a[0] || "", opts: a.slice(1) }; K.redraw(); }
    else if (name === "TODO") { B.todo = unesc(args).split("\n").filter(Boolean); todoMarker(B.todo); K.redraw(); }
    else if (name === "MODE") {
      B.planMode = args.trim() === "plan";
      B.msgs.append("notice", B.planMode ? "Modo plano ligado: o agente só lê e propõe um plano; nada é alterado até você aprovar." : "Modo plano desligado.");
      K.redraw();
    } else if (name === "TEACHER") {
      var on = args.trim().indexOf("1") === 0, model = args.trim().slice(2).trim();
      if (on !== B.teacher) B.msgs.append("notice", on ? "Professor ligado: " + model + " responde no lugar do modelo local. A conversa e os resultados das ferramentas saem desta máquina; as respostas dele ficam guardadas para treinar o modelo local." : "Professor desligado: o modelo local volta a responder.");
      B.teacher = on; K.post("web/ui", { key: "professor", value: on ? "true" : "false" }).catch(function () {}); K.redraw();
    } else if (name === "TEACHERR") B.msgs.append("notice", "O professor não respondeu (" + args.trim() + "). Nada foi feito neste passo; mande de novo ou desligue o professor.");
    else if (name === "UNDO") {
      tab = args.indexOf("\t");
      var files = tab < 0 ? "" : args.slice(tab + 1).trim();
      B.msgs.append("notice", files === "" ? "Nada para desfazer: o agente não alterou arquivos nesta conversa." : "Alterações desfeitas: " + files);
    } else if (name === "CWD") {
      a = args.split("\t"); B.ws = a[0] || ""; B.wsTrusted = a[1] === "1";
      if (B.ws) { B.ui.workspace = B.ws; K.post("web/ui", { key: "workspace", value: B.ws }).catch(function () {}); }
      B.indexText = ""; B.indexTip = ""; K.redraw();
    } else if (name === "NOTIFY") { a = args.split("\t"); B.msgs.append("notify", a[1] || "", a[0] || ""); }
    else if (name === "FILE") { a = args.split("\t"); B.msgs.append("file", a[1] || "", a[0] || "", "", a[2] || ""); }
    else if (name === "FINDING") {
      a = args.split("\t");
      var loc = !a[1] ? a[0] : a[0] + ":" + a[1];
      B.msgs.append("finding", a.slice(3).join(" "), loc, "", a[2] || "");
    } else if (name === "RESUMING") B.setTexts("Retomando a sessao: recalculando " + args.trim() + " tokens (so na primeira vez, pode levar alguns minutos)...", "");
    else if (name === "RESUME") {
      a = args.split(" ").filter(Boolean);
      if (a.length >= 6) {
        B.resumed = true; B.resumeCtx = "";
        var info = "Sessao retomada: " + a[2] + " tokens de contexto compacto (" + a[0] + " de " + a[1] + " mensagens) em " + a[4] + " s, "
          + (a.length >= 7 && a[6] === "kv" ? "cache KV lido do disco" : "contexto recalculado") + "; " + a[3] + " especialistas aquecidos";
        B.msgs.append("notice", info);
      }
    }
  }

  /* ---- stderr do agente ---- */
  var RE_STATS = /\[entrada: (\d+) tokens, ([\d.]+) tok\/s \| resposta: (\d+) tokens, ([\d.]+) tok\/s \| contexto (\d+) de (\d+)/;
  function onStderrLine(l) {
    l = l.trim();
    if (l) { B.errTail.push(l); while (B.errTail.length > 8) B.errTail.shift(); }
    var cm = /^\[context: about (\d+) tokens from the project index, (\d+) verified step/.exec(l);
    if (cm && B.answering) B.msgs.append("notice", "Contexto do katana: ~" + cm[1] + " tokens do índice do projeto, " + cm[2] + " passo(s) verificado(s)");
    var bm = /arquitetura=\S+.*\bctx=(\d+)/.exec(l);
    if (bm && B.ctxUsed === 0) B.setCtx(0, parseInt(bm[1], 10));
    var so = /^especializacao: (.+) \(\d+ tensores\)$/.exec(l), ss = /^especializacao pulada \((.+?)\): (.+)$/.exec(l);
    if (so) { var n = so[1].split("/").pop(); if (B.specsActive.indexOf(n) < 0) B.specsActive.push(n); K.redraw(); }
    else if (ss) { B.specsSkipped.push(ss[2].split("/").pop() + " (" + ss[1] + ")"); K.redraw(); }
    var m = RE_STATS.exec(l);
    if (m) { B.speed = m[4] + " tok/s"; B.setCtx(parseInt(m[5], 10), parseInt(m[6], 10)); }
    else if (l.indexOf("contexto cheio") >= 0) B.setTexts(B.status, "Contexto cheio. Inicie uma conversa nova.");
    else if (l.indexOf("interrompido") >= 0) B.setTexts("Interrompido", B.error);
  }

  /* ---- fim do processo ---- */
  function onFinished(j) {
    B.running = false;
    B.queue = []; B.paused = false;
    B.msgs.finishRunning(Date.now(), true);
    var wasLoading = B.loading, wasAnswering = B.answering;
    B.setState(false, false, false);
    var msg = j.signal ? "O agente travou e foi encerrado pelo sistema" : "O agente terminou (codigo " + j.code + ")";
    if (wasLoading) msg = "O agente nao conseguiu carregar o modelo. " + msg;
    else if (wasAnswering) msg += " no meio do pedido";
    msg += ".";
    if (B.errTail.length) msg += "\nUltimas mensagens do motor:\n" + B.errTail.join("\n");
    B.msgs.append("error", msg);
    if (B.dirty) saveSession();
    B.setTexts("", "O agente terminou. Escolha um modelo para iniciar de novo.");
  }

  /* ---- prontidao ---- */
  function markReady() {
    if (B.status.indexOf("Lendo ") === 0) B.setTexts("", B.error);
    if (B.resuming) {
      B.resuming = false;
      if (!B.resumed) { B.pendingCtx = B.resumeCtx; B.bound = ""; B.msgs.append("notice", "Nao foi possivel retomar pelo motor; a conversa segue com o resumo do historico"); }
      B.resumeCtx = "";
    } else if (B.resumeWant && B.running) {
      var kp = B.resumeWant; B.resumeWant = ""; B.resumeCtx = B.pendingCtx; B.pendingCtx = "";
      requestResume(kp);
      return;
    }
    if (B.dirty) saveSession();
    if (B.answering && !B.stopRequested) {
      var last = "", items = B.msgs.items;
      for (var i = items.length - 1; i >= 0; i--) {
        var k = items[i].kind;
        if (k === "notice" || k === "notify" || k === "file" || k === "finding" || k === "plan") continue;
        last = k; break;
      }
      if (last === "tool" || last === "user") B.msgs.append("stopped", "O agente parou sem responder.");
    }
    B.setTexts("", B.error);
    B.setState(true, false, false);
    B.refreshSessions();
    drainQueue();
  }

  /* ---- ferramentas e plano ---- */
  function isPlanTool(n) { return n === "TodoWrite"; }
  function toolCard(name, args, desc) {
    B.msgs.finishRunning(Date.now(), false);
    var meta = isPlanTool(name), plan = 0, step = 0;
    if (B.curPlan && B.msgs.plan(B.curPlan)) { plan = B.curPlan; step = meta ? -1 : B.msgs.currentStep(B.curPlan); }
    var idx = B.msgs.appendTool(name, args, desc, plan, step);
    if (!meta && B.answering) { B.reqActions++; var p = B.msgs.plan(B.curPlan); if (p) p.acts = B.reqActions; }
    if (B.sess) {
      var m = msgObj("tool", args, name); m.desc = desc; m.plano = plan; m.passo = step;
      B.sess.mensagens.push(m); B.msgs.items[idx].sm = m; B.dirty = true;
    }
    updateProgress();
  }
  function toolResult(result) {
    var idx = B.msgs.lastRunningTool();
    if (idx < 0) { B.msgs.setLastToolOut(result === "" ? "(sem saida)" : result, false); return; }
    B.msgs.finishTool(idx, result, Date.now());
    var it = B.msgs.items[idx];
    if (it.sm) { it.sm.estado = it.state; it.sm.dur = it.dur; B.dirty = true; }
    updateProgress();
  }
  function todoMarker(lines) {
    if (!B.answering) return;
    if (!lines.length) { B.curPlan = 0; updateProgress(); return; }
    if (B.curPlan && B.msgs.plan(B.curPlan)) { B.msgs.updatePlan(B.curPlan, lines); syncPlanSess(); }
    else {
      B.curPlan = B.msgs.addPlan(lines, B.reqStart);
      var p = B.msgs.plan(B.curPlan); if (p) p.acts = B.reqActions;
      var run = B.msgs.lastRunningTool();
      if (run >= 0 && isPlanTool(B.msgs.items[run].name)) { B.msgs.setToolStep(run, -1); if (B.msgs.items[run].sm) B.msgs.items[run].sm.passo = -1; }
      B.msgs.insertPlanItem(B.curPlan);
      if (B.sess) {
        var m = msgObj("plan", B.msgs.planJson(B.curPlan), "", K.isoLocal()); m.plano = B.curPlan;
        B.sess.mensagens.push(m);
        if (p) p.sessIdx = B.sess.mensagens.length - 1;
        B.dirty = true;
      }
    }
    updateProgress();
  }
  function syncPlanSess() {
    var p = B.msgs.plan(B.curPlan);
    if (!p || p.sessIdx < 0 || !B.sess || p.sessIdx >= B.sess.mensagens.length) return;
    B.sess.mensagens[p.sessIdx].texto = B.msgs.planJson(p.id); B.dirty = true;
  }
  function beginRequest() { B.curPlan = 0; B.reqStart = Date.now(); B.reqActions = 0; B.progress = ""; B.stats = ""; }
  function endRequest() {
    var now = Date.now();
    B.msgs.finishRunning(now, false);
    var endedPlan = false, done = "", p = B.msgs.plan(B.curPlan);
    if (p) {
      B.msgs.endPlan(p.id, now); endedPlan = true;
      var d = 0, n = 0;
      p.steps.forEach(function (s) { if (s.status !== 3) { n++; if (s.status === 2) d++; } });
      done = d + " de " + n + " passos, ";
      syncPlanSess();
    }
    if (B.reqActions > 0 || done) B.progress = done + B.reqActions + " " + (B.reqActions === 1 ? "ação" : "ações") + ", " + K.fmtDuration(now - B.reqStart);
    else B.progress = "";
    B.stats = B.progress; B.curPlan = 0;
    if (endedPlan && B.dirty) saveSession();
  }
  function updateProgress() {
    var t = "", stats = "";
    if (B.answering && B.reqStart) {
      var parts = [], p = B.msgs.plan(B.curPlan);
      if (p) {
        var n = 0, cur = 0, txt = "";
        p.steps.forEach(function (s) { if (s.status === 3) return; n++; if (s.status === 1 && !cur) { cur = n; txt = s.title; } });
        parts.push(cur ? "passo " + cur + " de " + n + ": " + txt : "todos os " + n + " passos feitos");
      }
      if (B.reqActions > 0 || parts.length) {
        var tail = K.fmtDuration(Date.now() - B.reqStart) + " · " + B.reqActions + " " + (B.reqActions === 1 ? "ação" : "ações");
        parts.push(tail); t = parts.join(" · "); stats = tail;
      }
    } else { t = B.progress; stats = B.progress; }
    if (t !== B.progress || stats !== B.stats) { B.progress = t; B.stats = stats; K.redraw(); }
  }
  B.updateProgress = updateProgress;

  /* ---- fila ---- */
  B.enqueue = function (text) {
    var t = text.trim();
    if (!B.running || !B.answering || !t) return false;
    B.queue.push(t); K.redraw(); return true;
  };
  B.removeQueued = function (i) {
    if (i < 0 || i >= B.queue.length) return;
    B.queue.splice(i, 1); if (!B.queue.length) B.paused = false; K.redraw();
  };
  B.resumeQueue = function () { B.paused = false; K.redraw(); if (B.ready) drainQueue(); };
  function drainQueue() {
    if (!B.queue.length || B.paused || !B.ready || !B.running) return;
    var n = B.queue.shift(); K.redraw();
    setTimeout(function () { if (!B.send(n)) { B.queue.unshift(n); K.redraw(); } }, 0);
  }
  B.stop = function () {
    if (!B.running) return;
    B.stopRequested = true;
    if (B.choice) B.answerChoice("");
    if (B.queue.length) { B.paused = true; K.redraw(); }
    K.post("web/stop").catch(function () {});
  };

  /* ---- comandos do agente ---- */
  B.togglePlan = function () { if (B.running && B.ready) writeLine(B.planMode ? "/plan off" : "/plan"); };
  B.approvePlan = function () {
    if (!B.running || !B.ready || !B.planMode) return;
    var text = "Plano aprovado: pode executar.";
    B.setTexts("", "");
    B.msgs.append("user", text);
    if (B.sess) B.sess.mensagens.push(msgObj("user", text, "", K.isoLocal()));
    saveSession();
    B.setState(false, false, true);
    writeLine("/go");
  };
  B.undo = function () { if (B.running && B.ready) writeLine("/undo"); };
  B.toggleTeacher = function () {
    if (!B.running || !B.ready) return;
    if (B.teacher) { writeLine("/teacher off"); return; }
    writeLine("/teacher " + (B.teacherModel === "" ? "on" : B.teacherModel));
  };
  B.answerChoice = function (textIn) {
    if (!B.choice) return;
    var t = String(textIn).replace(/[\n\r\x01]/g, " ").trim();
    K.post("web/ans", { text: t }).catch(function () {});
    B.choice = null; K.redraw();
  };
  B.answerAsk = function (yes) { K.post("web/ans", { yes: !!yes }).catch(function () {}); B.ask = null; K.redraw(); };
  B.sendPassword = function (pw) { K.post("web/pass", { password: pw }).catch(function () {}); B.pw = null; K.redraw(); };
  B.setWorkspace = function (path) {
    path = path.replace(/\/+$/, "") || "/";
    if (B.running && !B.ready) { B.setTexts("", "Aguarde o agente terminar para trocar de pasta."); return false; }
    B.ui.workspace = path; K.post("web/ui", { key: "workspace", value: path }).catch(function () {});
    if (B.running) { B.setState(false, false, true); writeLine("/cd " + path); }
    else { B.ws = path; K.redraw(); }
    return true;
  };

  /* ---- processo ---- */
  B.start = function (modelFile) {
    modelFile = modelFile.split("/").pop();
    B.model = modelFile; B.ui.ultimoModelo = modelFile;
    B.bound = ""; B.resuming = false; B.resumeWant = "";
    if (!B.pendingCtx && B.sess && B.ksessExists(B.sess.id)) { B.resumeWant = B.ksessPath(B.sess.id); B.resumeCtx = montarContexto(B.sess.mensagens); }
    else if (!B.pendingCtx) { B.sess = null; B.dirty = false; B.msgs.clear(); }
    B.speed = ""; B.ctxUsed = 0; B.ctxTotal = 0; B.specsActive = []; B.specsSkipped = []; B.errTail = [];
    filter.reset();
    B.queue = []; B.paused = false;
    B.setTexts("Carregando o modelo (pode levar alguns minutos)...", "");
    B.setState(false, true, true);
    K.post("web/start", { model: modelFile, cwd: B.workspace() }).then(function () { B.running = true; K.redraw(); })
      .catch(function (e) { B.running = false; B.setState(false, false, false); B.setTexts("", e.message); B.msgs.append("error", "Nao foi possivel iniciar o katana: " + e.message); });
  };

  /* ---- conversas ---- */
  B.ksessPath = function (id) { return B.sessDir ? B.sessDir + "/" + id + ".ksess" : ""; };
  B.ksessExists = function (id) { var s = B.sessions.filter(function (x) { return x.id === id; })[0]; return !!(s && s.ksess); };
  function montarContexto(ms) {
    var b = "";
    ms.forEach(function (m) {
      if (m.papel === "user") b += "Voce: " + K.simplified(m.texto) + "\n";
      else if (m.papel === "assistant") b += "Assistente: " + K.simplified(m.texto) + "\n";
    });
    if (!b) return "";
    var u = Array.from(b);
    if (u.length > 4000) b = "..." + u.slice(u.length - 4000).join("");
    return "Contexto da conversa anterior:\n" + b + "\n";
  }
  function newSession(modelo) {
    var d = new Date(), id = d.getFullYear() + K.pad(d.getMonth() + 1) + K.pad(d.getDate()) + "-" + K.pad(d.getHours()) + K.pad(d.getMinutes()) + K.pad(d.getSeconds()) + "-" + K.pad(d.getMilliseconds(), 3);
    return { id: id, titulo: "", modelo: modelo, criada: K.isoOffset(d), atualizada: K.isoOffset(d), mensagens: [] };
  }
  function saveSession() {
    var s = B.sess;
    if (!s) return;
    s.atualizada = K.isoOffset();
    if (B.model) s.modelo = B.model;
    var arr = s.mensagens.map(function (m) {
      var o = { papel: m.papel, texto: m.texto };
      if (m.nome) o.nome = m.nome;
      if (m.hora) o.hora = m.hora;
      if (m.desc) o.desc = m.desc;
      if (m.estado) o.estado = m.estado;
      if (m.dur >= 0) o.dur = m.dur;
      if (m.plano) o.plano = m.plano;
      if (m.passo) o.passo = m.passo;
      return o;
    });
    var root = { id: s.id, titulo: s.titulo, modelo: s.modelo, criada: s.criada, atualizada: s.atualizada, mensagens: arr };
    B.dirty = false;
    K.api("PUT", "web/sessions/" + s.id, root).catch(function () { B.dirty = true; });
    try { localStorage.setItem("katana.sessao", s.id); } catch (e) {}
  }
  B.saveSession = saveSession;
  B.refreshSessions = function () {
    return K.get("web/sessions").then(function (l) {
      B.sessions = (l || []).map(function (s) { s.atualizadaMs = new Date(s.atualizada).getTime(); s.atual = !!(B.sess && B.sess.id === s.id); return s; });
      K.redraw();
    }).catch(function () {});
  };
  B.send = function (text) {
    if (!B.running || !B.ready || !text.trim()) return false;
    if (B.paused) { B.paused = false; K.redraw(); }
    var msg = text;
    if (B.pendingCtx) { msg = B.pendingCtx + "Nova mensagem: " + text; B.pendingCtx = ""; }
    var line = K.simplified(msg);
    if (!B.sess) B.sess = newSession(B.model);
    var kp = B.ksessPath(B.sess.id);
    if (kp && kp !== B.bound) { writeLine("/session " + kp); B.bound = kp; }
    B.setTexts("", "");
    B.speed = ""; B.stopRequested = false;
    B.msgs.append("user", text);
    B.sess.mensagens.push(msgObj("user", text, "", K.isoLocal()));
    if (!B.sess.titulo) { var u = Array.from(K.simplified(text)); B.sess.titulo = u.length <= 80 ? u.join("") : u.slice(0, 80).join("") + "..."; }
    saveSession();
    B.setState(false, false, true);
    writeLine(line);
    setTimeout(B.refreshSessions, 300);
    return true;
  };
  B.newChat = function () {
    if (!B.running) {
      B.msgs.clear(); B.msgs.clearPlans(); B.sess = null; B.pendingCtx = ""; B.dirty = false; B.refreshSessions(); return true;
    }
    if (!B.ready) return false;
    B.msgs.clear(); B.msgs.clearPlans(); B.queue = []; B.paused = false; B.sess = null; B.pendingCtx = ""; B.bound = ""; B.resumeWant = ""; B.dirty = false;
    B.speed = ""; B.ctxUsed = 0; B.ctxTotal = 0;
    B.setTexts("Preparando a conversa...", "");
    B.setState(false, false, true);
    writeLine("/new");
    B.refreshSessions();
    return true;
  };
  B.openSession = function (id, displayOnly) {
    return K.get("web/sessions/" + id).then(function (s) {
      var agentReady = B.running && B.ready;
      if (B.running && !agentReady && !B.loading) { B.setTexts("", "Aguarde o agente terminar."); return false; }
      s.id = id; s.mensagens = (s.mensagens || []).map(function (m) {
        return { papel: m.papel || "", texto: m.texto || "", nome: m.nome || "", hora: m.hora || "", desc: m.desc || "", estado: m.estado || 0, dur: m.dur === undefined ? -1 : m.dur, plano: m.plano || 0, passo: m.passo || 0 };
      });
      B.sess = s; B.dirty = false;
      B.pendingCtx = montarContexto(s.mensagens);
      var canResume = B.ksessExists(id);
      if (canResume) { B.resumeCtx = B.pendingCtx; B.pendingCtx = ""; }
      var items = []; B.msgs.clearPlans(); B.curPlan = 0;
      s.mensagens.forEach(function (m) {
        if (m.papel === "plan") {
          B.msgs.restorePlan(m.texto);
          var pi = B.msgs.mk("plan", "", "", m.hora, ""); pi.plan = m.plano; items.push(pi);
          var pl = B.msgs.plan(m.plano); if (pl) pl.sessIdx = s.mensagens.indexOf(m);
          return;
        }
        var it = B.msgs.mk(m.papel, m.texto, m.nome, m.hora, "");
        if (m.papel === "tool") { it.desc = m.desc; it.state = m.estado; it.dur = m.dur; it.plan = m.plano; it.step = m.passo; it.sm = m; }
        items.push(it);
      });
      B.msgs.setAll(items);
      B.speed = ""; B.ctxUsed = 0; B.ctxTotal = 0;
      if (displayOnly) { B.pendingCtx = ""; B.resumeCtx = ""; B.bound = B.ksessPath(id); B.refreshSessions(); return true; }   /* a pagina recarregou: o agente ja tem esta conversa */
      var trocar = false;
      if (s.modelo && s.modelo !== B.model) trocar = B.models.some(function (m) { return m.file === s.modelo && m.usable; });
      if (trocar) B.start(s.modelo);
      else if (agentReady && canResume) requestResume(B.ksessPath(id));
      else if (agentReady) { B.setTexts("Preparando a conversa...", ""); B.setState(false, false, true); B.bound = ""; writeLine("/new"); }
      else { if (canResume) B.resumeWant = B.ksessPath(id); }
      try { localStorage.setItem("katana.sessao", id); } catch (e) {}
      B.refreshSessions();
      return true;
    }).catch(function () { B.setTexts("", "Nao foi possivel abrir a conversa."); return false; });
  };
  function requestResume(path) {
    B.resuming = true; B.resumed = false; B.bound = path;
    B.setTexts("Retomando a sessao (especialistas e contexto compacto)...", "");
    B.setState(false, false, true);
    writeLine("/resume " + path);
  }
  B.deleteSession = function (id) {
    var wasCurrent = B.sess && B.sess.id === id;
    return K.api("DELETE", "web/sessions/" + id).then(function () {
      if (wasCurrent) { B.sess = null; B.pendingCtx = ""; B.dirty = false; if (!B.newChat()) B.msgs.clear(); }
      return B.refreshSessions();
    }).catch(function () { B.setTexts("", "Nao foi possivel apagar a conversa."); });
  };
  B.askSummary = function () { B.send("Você parou sem me responder. Resuma o que foi feito neste pedido e o que ainda falta fazer."); };
  B.copyText = function (t) {
    if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(t).catch(function () { fallback(); });
    else fallback();
    function fallback() { var a = h("textarea", { style: { position: "fixed", opacity: 0 } }); a.value = t; document.body.appendChild(a); a.select(); try { document.execCommand("copy"); } catch (e) {} a.remove(); }
  };
  B.resolveFile = function (f) { return f.charAt(0) === "/" ? f : (B.workspace().replace(/\/$/, "") + "/" + f); };
  B.openPath = function (p) { K.post("web/open", { path: B.resolveFile(p) }).catch(function () {}); };
  B.openFolderOf = function (p) { K.post("web/open", { path: B.resolveFile(p), folder: true }).catch(function () {}); };

  /* ---- atividade do agente enquanto trabalha (mesma regra do desktop) e uso da maquina ---- */
  B.activityFor = function (turnSecs, quietSecs, cores, children, idleSecs) {
    var stalled = false;
    if (turnSecs < 5) return { t: "", stalled: false };
    var base = "Trabalhando há " + K.secsText(turnSecs);
    if (quietSecs < 15) return { t: base, stalled: false };
    if (cores >= 0.5) return { t: base + " · processando (sem saída nova há " + K.secsText(quietSecs) + ", processador em uso)", stalled: false };
    if (children) return { t: base + " · aguardando um comando terminar (" + K.secsText(quietSecs) + ")", stalled: false };
    if (idleSecs >= 30) return { t: "Sem atividade há " + K.secsText(quietSecs) + ": o agente pode ter travado. Esc interrompe a resposta.", stalled: true };
    return { t: base + " · sem saída nova há " + K.secsText(quietSecs), stalled: stalled };
  };
  var procTicks = -1, idleSecs = 0, cpuTot = 0, cpuIdle = 0, gpuText = "";
  B.sysTick = function () {
    K.get("web/sys").then(function (j) {
      var cpu = "", ram = "";
      if (cpuTot > 0 && j.cpuTot > cpuTot) cpu = "CPU " + Math.round(100 * ((j.cpuTot - cpuTot) - (j.cpuIdle - cpuIdle)) / (j.cpuTot - cpuTot)) + "%";
      cpuTot = j.cpuTot; cpuIdle = j.cpuIdle;
      if (j.memTotal > 0) ram = "RAM " + Math.round((j.memTotal - j.memAvail) / 1048576) + " de " + Math.round(j.memTotal / 1048576) + " GB";
      if (j.gpu) gpuText = "GPU " + parseInt(j.gpu[0], 10) + "% · " + K.ptDec(parseFloat(j.gpu[1]) / 1024, 1) + " de " + K.ptDec(parseFloat(j.gpu[2]) / 1024, 1) + " GB"; else gpuText = "";
      var parts = []; if (cpu) parts.push(cpu); if (ram) parts.push(ram); if (gpuText) parts.push(gpuText);
      var t = parts.join("   ·   ");
      if (t !== B.sysText) { B.sysText = t; K.redraw(); }
      if (B.running && (B.answering || B.loading) && B.turnStart) {
        var tot = j.agentTicks, cores = procTicks >= 0 && tot >= procTicks ? (tot - procTicks) / 100.0 / 2 : 1.0;   /* a pagina consulta a cada 2 s */
        procTicks = tot;
        idleSecs = (cores < 0.2 && !j.children) ? idleSecs + 2 : 0;
        var a = B.activityFor(Math.floor((Date.now() - B.turnStart) / 1000), Math.floor((Date.now() - B.lastOut) / 1000), cores, j.children, idleSecs);
        if (a.t !== B.activityText || a.stalled !== B.stalled) { B.activityText = a.t; B.stalled = a.stalled; K.redraw(); }
      } else { procTicks = -1; idleSecs = 0; if (B.activityText || B.stalled) { B.activityText = ""; B.stalled = false; K.redraw(); } }
    }).catch(function () {});
  };
  /* indice de codigo da pasta (so enquanto o agente trabalha e uma vez ao terminar) */
  var idxWorking = false, idxBusy = false;
  B.indexPoll = function () {
    var working = B.answering && B.running;
    if (!working && !idxWorking) return;
    idxWorking = working;
    if (idxBusy || !B.workspace()) return;
    idxBusy = true;
    var ws = B.workspace();
    K.get("web/index?path=" + encodeURIComponent(ws)).then(function (j) {
      idxBusy = false;
      var r = ws === B.workspace() ? parseIndexStatus(j.out || "") : null;
      var text = r ? r.text : "", tip = r ? r.tip : "";
      if (text !== B.indexText || tip !== B.indexTip) { B.indexText = text; B.indexTip = tip; K.redraw(); }
    }).catch(function () { idxBusy = false; if (B.indexText) { B.indexText = ""; B.indexTip = ""; K.redraw(); } });
  };
  function parseIndexStatus(out) {
    var mf = /arquivos vigentes: (\d+)/.exec(out);
    if (!mf) return null;
    var m0 = /N0 arvore\s+(\d+) arquivos catalogados \((\d+) sem leitura/.exec(out), m1 = /N1 texto\s+(\d+) de (\d+) elegiveis \((\d+)%\)/.exec(out),
      m2 = /N2 simbolos\s+(\d+) de (\d+) elegiveis \((\d+)%\)/.exec(out), m3 = /N3 ligacoes\s+(\d+) de (\d+) elegiveis \((\d+)%\)/.exec(out),
      mi = /ignorados por regra[^:\n]*: (\d+) entradas/.exec(out), md = /(\d+) discovered/.exec(out);
    var running = out.indexOf("rastreador: em execucao") >= 0, pendingDirs = md && parseInt(md[1], 10) > 0, pct = 100;
    if (m1) pct = Math.min(pct, parseInt(m1[3], 10));
    if (m2) pct = Math.min(pct, parseInt(m2[3], 10));
    var busy = running || pendingDirs || pct < 100;
    var text = busy ? "índice: indexando " + Math.min(pct, 99) + "%" : "índice: pronto";
    var t = [busy ? "Índice do código: indexando" : "Índice do código: pronto"];
    var files = "Arquivos: " + mf[1];
    if (m0 && parseInt(m0[2], 10) > 0) files += " (" + m0[2] + " sem leitura: binários, grandes, ilegíveis)";
    t.push(files);
    var lv = [];
    if (m1) lv.push("texto " + m1[3] + "%");
    if (m2) lv.push("símbolos " + m2[3] + "%");
    if (m3) lv.push("ligações " + m3[3] + "%");
    if (lv.length) t.push("Níveis: " + lv.join(", "));
    if (mi) t.push("Ignorados por regra: " + mi[1]);
    t.push(running ? "Rastreador em execução" : "Rastreador parado");
    return { text: text, tip: t.join("\n") };
  }
})();
