/* Caixa de mensagem, seletor de modelo, contador de contexto e a fila (Composer, ModelPicker, ContextMeter do desktop) */
(function () {
  var B = K.be, ta, popOpen = false;

  function fmtTok(n) {
    if (n >= 10000) return Math.round(n / 1000) + " mil";
    if (n >= 1000) return (n / 1000).toFixed(1).replace(".", ",") + " mil";
    return String(n);
  }
  function ctxMeter() {
    var el = $("ctxMeter"), used = B.ctxUsed, total = B.ctxTotal, known = total > 0;
    el.hidden = !known;
    if (!known) { el.textContent = ""; return; }
    var frac = Math.min(1, used / total), pct = Math.round(frac * 100);
    var level = frac >= 0.85 ? "crit" : frac >= 0.70 ? "warn" : "ok";
    var tone = level === "crit" ? "var(--err)" : level === "warn" ? "var(--warn)" : "var(--mut)";
    var c = level === "ok" ? "var(--ac)" : tone, R = 5.5, L = 2 * Math.PI * R, arc = frac > 0 ? L * Math.max(0.02, frac) : 0;
    var ns = "http://www.w3.org/2000/svg", s = document.createElementNS(ns, "svg");
    s.setAttribute("width", 14); s.setAttribute("height", 14); s.setAttribute("viewBox", "0 0 14 14");
    s.innerHTML = '<circle cx="7" cy="7" r="5.5" fill="none" stroke="var(--bd)" stroke-width="2"/>' +
      (arc > 0 ? '<circle cx="7" cy="7" r="5.5" fill="none" stroke="' + c + '" stroke-width="2" stroke-dasharray="' + arc.toFixed(3) + ' ' + L.toFixed(3) + '" transform="rotate(-90 7 7)"/>' : "");
    var label = fmtTok(used) + " / " + fmtTok(total) + " tokens · " + pct + "%";
    el.textContent = "";
    el.appendChild(s);
    el.appendChild(h("span", { text: label, style: { color: tone } }));
    el.dataset.tip = "Tokens usados: " + used + "\nContexto total: " + total + "\nLivre: " + Math.max(0, total - used) + " (" + (100 - pct) + "%)";
  }

  /* ---- seletor de modelo (abre para cima) ---- */
  function renderPicker() {
    var box = $("picker"), cur = B.models.filter(function (m) { return m.file === B.model; })[0];
    var label = cur ? cur.name : "Selecione um modelo";
    var sig = label + "|" + popOpen + "|" + B.models.length + "|" + B.model;
    if (box.dataset.sig === sig) return;
    box.dataset.sig = sig;
    box.textContent = "";
    var max = Math.max(120, $("box").offsetWidth * 0.45);
    var btn = h("button", { class: "picker" + (popOpen ? " open" : ""), "aria-label": "Escolher modelo", style: { maxWidth: max + "px" } },
      h("span", { class: "pk-l", text: label }), K.icon("down", 11, "var(--mut)"));
    btn.addEventListener("click", function (e) { e.stopPropagation(); popOpen = !popOpen; box.dataset.sig = ""; renderPicker(); });
    box.appendChild(btn);
    if (popOpen) {
      var list = h("div", { class: "pk-pop" });
      B.models.forEach(function (m) {
        var it = h("button", { class: "pk-it" + (m.file === B.model ? " cur" : "") + (m.usable ? "" : " dis"), disabled: !m.usable },
          h("div", { class: "pk-n" }, h("span", { text: m.name }), m.isDefault && m.usable ? h("span", { class: "pk-def", text: "padrão" }) : null),
          h("div", { class: "pk-s", text: m.usable ? m.sizeText : m.note + " · " + m.sizeText }));
        it.addEventListener("click", function () {
          popOpen = false; box.dataset.sig = ""; renderPicker();
          if (m.file === B.model && B.running) return;
          B.start(m.file);
        });
        list.appendChild(it);
      });
      if (!B.models.length) list.appendChild(h("div", { class: "pk-s", style: { padding: "12px" }, text: "Nenhum modelo em " + (B.modelsDir || "models") }));
      box.appendChild(list);
    }
  }
  document.addEventListener("click", function (e) { if (popOpen && !e.target.closest("#picker")) { popOpen = false; $("picker").dataset.sig = ""; renderPicker(); } });
  document.addEventListener("keydown", function (e) { if (e.key === "Escape" && popOpen) { popOpen = false; $("picker").dataset.sig = ""; renderPicker(); e.stopPropagation(); } }, true);

  /* ---- enviar / parar / enfileirar ---- */
  function submit() {
    var t = ta.value.trim();
    if (B.choice) { if (t) { B.answerChoice(t); ta.value = ""; autosize(); } return; }
    if (B.answering) {
      if (!t) { B.stop(); return; }
      if (B.enqueue(t)) { ta.value = ""; autosize(); }
      return;
    }
    if (!t || !B.ready) return;
    if (B.send(t)) { ta.value = ""; autosize(); K.resetStick(); }
  }
  K.submit = submit;
  K.setComposerText = function (t) { ta.value = t; ta.setSelectionRange(t.length, t.length); autosize(); };
  K.suggestionChosen = function (t) { K.setComposerText(t); if (B.ready) submit(); else ta.focus(); };

  function autosize() {
    ta.style.height = "auto";
    var hgt = Math.min(ta.scrollHeight, 330);
    ta.style.height = hgt + "px";
    $("box").style.height = Math.max(116, hgt + 24 + 38) + "px";
    K.layout();
  }

  function render() {
    var enabled = B.ready || (B.answering && !B.loading);
    ta.disabled = !enabled;
    var ph = B.ready ? "Escreva uma mensagem..."
      : B.loading ? (/^Preparando/.test(B.status) ? B.status : "Carregando o modelo...")
      : B.answering && /^(Retomando|Preparando)/.test(B.status) ? B.status
      : B.answering ? "Respondendo..." : B.running ? "Aguarde..." : "Escolha um modelo para começar";
    if (ta.placeholder !== ph) ta.placeholder = ph;
    var has = ta.value.trim().length > 0, stopMode = B.answering && !has;
    var sb = $("send");
    var sigS = stopMode ? "stop" : "up";
    if (sb.dataset.g !== sigS) { sb.dataset.g = sigS; sb.textContent = ""; sb.appendChild(K.icon(sigS, 18, "var(--acfg)")); }
    sb.disabled = !(B.answering || (B.ready && has));
    sb.dataset.tip = B.answering ? (has ? "Enfileirar (envia quando o agente terminar)" : "Parar (Esc)") : "Enviar";
    if (enabled && !ta.disabled && B.ready && !B._focused) { B._focused = true; ta.focus(); }
    if (!B.ready) B._focused = false;

    var wsWarn = $("wsWarn"); wsWarn.hidden = B.wsTrusted;
    var pb = $("planBtn"); pb.disabled = !B.ready; pb.textContent = B.planMode ? "plano: ligado" : "plano"; pb.classList.toggle("on", B.planMode);
    var tb = $("tchBtn"); tb.disabled = !B.ready; tb.textContent = B.teacher ? "professor: " + B.teacherModel : "professor"; tb.classList.toggle("on", B.teacher);
    var sl = $("statusLine");
    var txt = B.error ? B.error : B.stalled ? B.activityText
      : B.status && !/^(Preparando|Retomando|Carregando)/.test(B.status) ? B.status
      : B.progress ? B.progress : B.activityText ? B.activityText
      : ta.value.indexOf("\n") >= 0 ? "As quebras de linha serão enviadas como espaço." : "";
    if (B.disconnected) txt = "Sem conexão com o katana serve.";
    sl.textContent = txt; sl.classList.toggle("err", !!(B.error || B.stalled || B.disconnected));
    var ix = $("indexStatus"); ix.textContent = B.indexText; ix.hidden = !B.indexText; ix.dataset.tip = B.indexTip;
    $("speed").textContent = B.speed;
    ctxMeter();
    var sys = $("sys"); sys.textContent = B.sysText; sys.hidden = !B.sysText;

    /* fila */
    var q = $("queue"), qsig = B.queue.join("\u0001") + "|" + B.paused;
    if (q.dataset.sig !== qsig) {
      q.dataset.sig = qsig; q.textContent = "";
      B.queue.forEach(function (m, i) {
        var row = h("div", { class: "qit" }, h("span", { class: "qt", text: (B.paused ? "⏸ " : "⏳ ") + (i + 1) + ". " + m.replace(/\s+/g, " ") }),
          i === 0 && B.paused ? h("span", { class: "qres", text: "retomar", onclick: function () { B.resumeQueue(); } }) : null,
          h("span", { class: "qx", text: "✕", onclick: function () { B.removeQueued(i); } }));
        q.appendChild(row);
      });
    }
    renderPicker();
  }

  K.initComposer = function () {
    ta = $("ta");
    ta.addEventListener("input", function () { autosize(); K.redraw(); });
    ta.addEventListener("keydown", function (e) {
      if (e.key === "Escape" && B.answering) { e.preventDefault(); B.stop(); return; }
      if (e.key === "Enter" && !e.shiftKey && !e.isComposing) { e.preventDefault(); submit(); }
    });
    $("send").addEventListener("click", submit);
    $("planBtn").addEventListener("click", B.togglePlan);
    $("tchBtn").addEventListener("click", function () {
      if (!B.hasTeacherKey && !B.teacher && !B.teacherModel) K.openTeacherDialog(); else B.toggleTeacher();
    });
    K.tip($("planBtn"), "No modo plano o agente só lê e propõe um plano; nada é alterado até você aprovar.", 400);
    K.tip($("tchBtn"), "Com o professor ligado, um modelo maior (pela API) responde no lugar do modelo local, com as mesmas ferramentas.\nA conversa sai desta máquina; as respostas ficam guardadas para treinar o modelo local.", 400);
    K.tip($("send"), function () { return $("send").dataset.tip; }, 600);
    K.tip($("ctxMeter"), function () { return $("ctxMeter").dataset.tip; }, 300);
    K.tip($("indexStatus"), function () { return $("indexStatus").dataset.tip; }, 400);
    K.view(render);
    autosize();
  };
})();
