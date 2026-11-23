/* A lista de mensagens da conversa: o mesmo modelo de desktop-qt/src/backend.cpp (MessageModel): itens, ferramentas com estado,
   notas do motor separadas do resultado e planos (a lista de tarefas do agente com as acoes agrupadas por passo). */
K.Messages = function () { this.items = []; this.plans = []; this.nextId = 1; };
(function () {
  var P = K.Messages.prototype;
  P.touch = function (it) { if (it) it.rev = ++K.rev; K.redraw(); };
  P.count = function () { return this.items.length; };
  P.vkind = function (it) { return it.kind === "tool" && it.step !== 0 ? "hidden" : it.kind; };
  P.mk = function (kind, text, name, time, out) {
    return { id: this.nextId++, kind: kind, text: text || "", name: name || "", out: out || "", time: time || "", desc: "", note: "", state: 0, t0: 0, dur: -1, plan: 0, step: 0, rev: ++K.rev };
  };
  P.append = function (kind, text, name, time, out) {
    var t = time || ((kind === "user" || kind === "assistant") ? K.isoLocal() : "");
    var it = this.mk(kind, text, name, t, out);
    this.items.push(it); K.redraw();
    return it;
  };
  P.lastKind = function () { return this.items.length ? this.items[this.items.length - 1].kind : ""; };
  P.appendToLast = function (text) {
    if (!this.items.length) return;
    var it = this.items[this.items.length - 1];
    it.text += text; this.touch(it);
  };
  P.lastRunningTool = function () {
    for (var i = this.items.length - 1; i >= 0; i--) if (this.items[i].kind === "tool" && this.items[i].state === 1) return i;
    return -1;
  };
  P.setLastToolOut = function (out, append) {
    var k = this.lastRunningTool();
    if (k < 0 && this.items.length && this.items[this.items.length - 1].kind === "tool") k = this.items.length - 1;
    if (k < 0) return;
    var it = this.items[k];
    if (append) { if (it.out.length < (1 << 20)) it.out += out; } else it.out = out;
    this.touch(it);
  };
  P.appendTool = function (name, args, desc, plan, step) {
    var it = this.mk("tool", args, name, "", "");
    it.desc = desc; it.state = 1; it.t0 = Date.now(); it.plan = plan; it.step = step;
    this.items.push(it); K.redraw();
    return this.items.length - 1;
  };
  P.setToolStep = function (idx, step) {
    if (idx < 0 || idx >= this.items.length) return;
    this.items[idx].step = step; this.touch(this.items[idx]);
  };
  /* notas do motor dentro do resultado ("[katana: ...]", "[plan: ...]"): sao para o modelo, nao para o usuario */
  P.splitNotes = function (raw) {
    if (raw.indexOf("[katana:") < 0 && raw.indexOf("[plan:") < 0) return { clean: raw, note: "" };
    var ln = raw.split("\n"), keep = [], notes = [];
    for (var i = 0; i < ln.length; i++) {
      var t = ln[i].trim();
      if (t.indexOf("[katana:") !== 0 && t.indexOf("[plan:") !== 0) { keep.push(ln[i]); continue; }
      var end = -1;
      for (var j = i; j < ln.length && j < i + 12; j++) if (/\]$/.test(ln[j].trim())) { end = j; break; }
      if (end < 0) end = i;
      var part = [];
      for (j = i; j <= end; j++) part.push(ln[j].trim());
      notes.push(part.join("\n"));
      i = end;
    }
    if (!notes.length) return { clean: raw, note: "" };
    return { clean: keep.join("\n").trim(), note: notes.join("\n") };
  };
  P.resultFailed = function (clean) {
    if (clean.indexOf("error:") === 0) return true;
    return /(?:Exit code |\[exit code )([1-9])/.test(clean);
  };
  P.finishTool = function (idx, result, nowMs) {
    if (idx < 0 || idx >= this.items.length) return;
    var it = this.items[idx], r = this.splitNotes(result);
    it.out = r.clean === "" && r.note === "" ? "(sem saida)" : r.clean;
    it.note = r.note;
    it.state = this.resultFailed(r.clean) ? 3 : 2;
    it.dur = Math.max(0, nowMs - it.t0);
    this.touch(it);
  };
  P.finishRunning = function (nowMs, failed) {
    for (var k = 0; k < this.items.length; k++) {
      var it = this.items[k];
      if (it.kind === "tool" && it.state === 1) { it.state = failed ? 3 : 2; it.dur = Math.max(0, nowMs - it.t0); it.rev = ++K.rev; }
    }
    K.redraw();
  };
  P.clear = function () { this.items = []; K.redraw(); };
  P.clearPlans = function () { this.plans = []; };
  P.setAll = function (items) { this.items = items; K.redraw(); };

  /* ---- planos ---- */
  P.plan = function (id) { for (var i = 0; i < this.plans.length; i++) if (this.plans[i].id === id) return this.plans[i]; return null; };
  P.maxPlanId = function () { var m = 0; for (var i = 0; i < this.plans.length; i++) m = Math.max(m, this.plans[i].id); return m; };
  function parseTodo(todo) {
    var out = [];
    todo.forEach(function (l) {
      var done = l.indexOf("[x]") === 0 || l.indexOf("[X]") === 0, t = l;
      if (/^\[[xX ]\]/.test(t)) t = t.slice(3);
      t = K.simplified(t);
      if (t) out.push([done, t]);
    });
    return out;
  }
  P.addPlan = function (todo, t0) {
    var p = { id: this.maxPlanId() + 1, t0: t0, t1: 0, acts: 0, steps: [], nextStep: 1, sessIdx: -1 };
    this.plans.push(p);
    this.updatePlan(p.id, todo);
    return p.id;
  };
  /* reconcilia a lista nova com os passos lembrados: mesmo texto = mesmo passo; mesma posicao com outro texto = passo reescrito;
     o que sumiu da lista fica como descartado; o que e novo ganha id novo */
  P.updatePlan = function (id, todo) {
    var p = this.plan(id);
    if (!p) return;
    var n = parseTodo(todo), old = p.steps, map = [], used = [], ni, oi, pass, k;
    for (ni = 0; ni < n.length; ni++) map.push(-1);
    for (oi = 0; oi < old.length; oi++) used.push(false);
    function same(a, b) { return a.toLowerCase() === b.toLowerCase(); }
    for (ni = 0; ni < n.length; ni++)
      for (pass = 0; pass < 2 && map[ni] < 0; pass++)
        for (oi = 0; oi < old.length; oi++)
          if (!used[oi] && (pass === 1 || old[oi].status !== 3) && same(old[oi].title, n[ni][1])) { map[ni] = oi; used[oi] = true; break; }
    for (ni = 0; ni < n.length; ni++)
      if (map[ni] < 0 && ni < old.length && !used[ni] && old[ni].status !== 3) { map[ni] = ni; used[ni] = true; }
    var res = [], emitted = [], resOf = [];
    for (ni = 0; ni < n.length; ni++) { emitted.push(false); resOf.push(-1); }
    function emitNew(ni) {
      if (emitted[ni]) return;
      emitted[ni] = true;
      var s;
      if (map[ni] >= 0) s = { id: old[map[ni]].id, title: old[map[ni]].title, status: old[map[ni]].status };
      else s = { id: p.nextStep++, title: "", status: 0 };
      s.title = n[ni][1];
      resOf[ni] = res.length;
      res.push(s);
    }
    var nextEmit = 0;
    for (oi = 0; oi < old.length; oi++) {
      ni = map.indexOf(oi);
      if (ni >= 0) {
        for (k = nextEmit; k <= ni; k++) emitNew(k);
        nextEmit = Math.max(nextEmit, ni + 1);
      } else {
        var s2 = { id: old[oi].id, title: old[oi].title, status: 3 };
        res.push(s2);
      }
    }
    for (k = 0; k < n.length; k++) emitNew(k);
    var doing = false;
    for (k = 0; k < n.length; k++) {
      var s = res[resOf[k]];
      if (n[k][0]) s.status = 2;
      else if (!doing) { s.status = 1; doing = true; }
      else s.status = 0;
    }
    p.steps = res;
    this.bumpPlan(p);
  };
  P.bumpPlan = function (p) { p.rev = ++K.rev; K.redraw(); };
  P.endPlan = function (id, t1) { var p = this.plan(id); if (p) { p.t1 = t1; this.bumpPlan(p); } };
  P.insertPlanItem = function (id) {
    var it = this.mk("plan", "", "", "", ""); it.plan = id;
    this.items.push(it); K.redraw();
    return it;
  };
  P.currentStep = function (id) {
    var p = this.plan(id);
    if (!p) return 0;
    var last = 0;
    for (var i = 0; i < p.steps.length; i++) {
      if (p.steps[i].status === 1) return p.steps[i].id;
      if (p.steps[i].status === 2) last = p.steps[i].id;
    }
    return last;
  };
  P.planData = function (id) {
    var p = this.plan(id);
    if (!p) return { steps: [], done: 0, total: 0, t0: 0, t1: 0, acts: 0 };
    var pos = {}, steps = [], done = 0, total = 0, i;
    p.steps.forEach(function (s) {
      pos[s.id] = steps.length;
      steps.push({ id: s.id, title: s.title, status: s.status, actions: [], count: 0, ms: 0, failed: 0, t0: 0 });
      if (s.status !== 3) { total++; if (s.status === 2) done++; }
    });
    for (i = 0; i < this.items.length; i++) {
      var it = this.items[i];
      if (it.kind !== "tool" || it.plan !== id || it.step <= 0 || pos[it.step] === undefined) continue;
      var st = steps[pos[it.step]];
      st.actions.push({ idx: i, id: it.id, name: it.name, args: it.text, desc: it.desc, note: it.note, state: it.state, t0: it.t0, dur: it.dur, hasOut: it.out.length > 0, rev: it.rev });
      st.count++;
      if (it.dur > 0) st.ms += it.dur;
      if (it.state === 3) st.failed++;
      if (it.t0 && st.t0 === 0) st.t0 = it.t0;
    }
    return { id: id, steps: steps, done: done, total: total, t0: p.t0, t1: p.t1, acts: p.acts };
  };
  P.planJson = function (id) {
    var p = this.plan(id);
    if (!p) return "";
    return JSON.stringify({ id: p.id, next: p.nextStep, t0: p.t0, t1: p.t1, acts: p.acts, steps: p.steps.map(function (s) { return { id: s.id, t: s.title, s: s.status }; }) });
  };
  P.restorePlan = function (json) {
    var o;
    try { o = JSON.parse(json); } catch (e) { return; }
    if (!o || !o.id) return;
    var p = { id: o.id, nextStep: o.next || 1, t0: o.t0 || 0, t1: o.t1 || 0, acts: o.acts || 0, steps: [], sessIdx: -1, rev: ++K.rev };
    if (p.t1 === 0) p.t1 = -1;
    (o.steps || []).forEach(function (s) { p.steps.push({ id: s.id, title: s.t, status: s.s }); });
    this.plans.push(p);
  };
})();
