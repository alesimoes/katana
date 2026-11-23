/* A conversa: mensagens, grupos de acoes, plano, cartoes e tela inicial (ActivityGroup, ActionRow, PlanView, AssistantBody,
   CodeBlock, MessageView do desktop). Cada entrada tem uma assinatura; so as que mudaram sao refeitas. */
(function () {
  var B = K.be;
  K.ui = { act: {}, grp: {}, step: {}, note: {}, rev: 0 };
  K.toggleUi = function (map, key, val) { map[key] = { v: val }; K.ui.rev++; K.redraw(); };

  function sec() { return Math.floor(Date.now() / 1000); }
  function msgs() { return B.msgs; }

  /* ---- bloco de codigo ---- */
  function codeBlock(lang, code) {
    var done = false, btn = h("button", { class: "cb-copy", text: "Copiar", "aria-label": "Copiar o codigo" });
    btn.addEventListener("click", function () {
      B.copyText(code); btn.textContent = "Copiado";
      setTimeout(function () { btn.textContent = "Copiar"; }, 1500);
    });
    var ed = h("div", { class: "cb-ed", html: Hl.html(code, lang, K.cur()) });
    return h("div", { class: "cb" }, h("div", { class: "cb-head" }, h("span", { text: lang }), btn), h("div", { class: "cb-body" }, ed));
  }
  K.codeBlock = codeBlock;

  /* ---- resposta do assistente ---- */
  function assistantBody(text) {
    var root = h("div", { class: "abody" });
    var blocks = Md.parse(text.replace(/^\s+/, ""), K.dark() ? "#1a181e" : "#ece7f3");
    blocks.forEach(function (b) {
      if (b.type === "code") root.appendChild(codeBlock(b.lang, b.code));
      else root.appendChild(h("div", { class: "atext", html: b.html }));
    });
    return root;
  }

  /* ---- uma acao do agente ---- */
  function actionRow(a, now) {
    var running = a.state === 1, failed = a.state === 3;
    var hasDetail = !!a.hasOut || !!a.args || !!a.note;
    var us = K.ui.act[a.id];
    var expanded = hasDetail && (us ? us.v : (running && !!a.hasOut));
    var title = V.titulo(a);
    var timeText = running ? V.dur(Math.max(0, now - a.t0)) : (a.dur >= 0 ? V.dur(a.dur) : "");
    var mk = running ? h("span", { class: "spin" }, K.icon("gear", 13, "var(--ac)")) : h("span", { class: "mark" + (failed ? " fail" : ""), text: failed ? "×" : "✓" });
    var head = h("div", { class: "ar-head" + (hasDetail ? " clickable" : "") },
      h("span", { class: "ar-mk" }, mk),
      h("span", { class: "ar-title" + (running ? " run" : "") + (failed ? " fail" : ""), text: title }),
      h("span", { class: "ar-time" + (running ? " run" : ""), text: timeText }),
      hasDetail ? h("span", { class: "chev" + (expanded ? " up" : "") }, K.icon("down", 12, "var(--mut)")) : null);
    if (hasDetail) head.addEventListener("click", function () { K.toggleUi(K.ui.act, a.id, !expanded); });
    var el = h("div", { class: "ar" }, head);
    if (expanded) {
      var it = msgs().items[a.idx] || { out: "" };
      if (a.args) el.appendChild(h("div", { class: "ar-cmd", text: a.name === "Bash" && a.args.indexOf("command: ") === 0 ? "$ " + a.args.slice(9) : a.name + ": " + a.args }));
      if (it.out.length > 0) {
        var out = h("div", { class: "ar-out" });
        if (V.realcavel(a.name)) out.innerHTML = V.realce(it.out, { ok: K.cur().ty, mid: K.cur().fn, low: K.cur().str, err: K.cur().err, lbl: K.cur().kw });
        else out.textContent = it.out;
        el.appendChild(out);
        if (running) setTimeout(function () { out.scrollTop = out.scrollHeight; }, 0);
      }
      if (a.note) {
        var clean = a.note.replace(/^\[(katana|plan): ?/mg, "").replace(/\]\s*$/mg, "");
        var isLong = clean.length > 130 || clean.indexOf("\n") >= 0, open = K.ui.note[a.id] && K.ui.note[a.id].v;
        var nt = h("div", { class: "ar-note" + (isLong && !open ? " one" : "") + (isLong ? " clickable" : ""), text: "nota do motor (para o modelo): " + clean });
        if (isLong) nt.addEventListener("click", function () { K.toggleUi(K.ui.note, a.id, !open); });
        el.appendChild(nt);
      }
    }
    return el;
  }

  /* ---- grupo de acoes ---- */
  function groupEl(run, active, key) {
    var now = Date.now(), last = run.length ? run[run.length - 1] : null, failedCount = run.filter(function (s) { return s.state === 3; }).length;
    var summary = active && last ? V.titulo(last) + "…" : run.length + (run.length === 1 ? " ação executada" : " ações executadas") + (failedCount > 0 ? " (" + failedCount + (failedCount === 1 ? " falhou)" : " falharam)") : "");
    var us = K.ui.grp[key], expanded = us ? us.v : active;
    var head = h("button", { class: "ag-head", "aria-label": summary },
      h("span", { class: "ag-ico" + (active ? " spin" : "") }, K.icon(active ? "gear" : "check", 16, active ? "var(--ac)" : "var(--mut)")),
      h("span", { class: "ag-sum" + (active ? " run" : ""), text: summary }),
      h("span", { class: "chev" + (expanded ? " up" : "") }, K.icon("down", 14, "var(--mut)")));
    head.addEventListener("click", function () { K.toggleUi(K.ui.grp, key, !expanded); });
    var g = h("div", { class: "ag" }, head);
    if (expanded) {
      var list = h("div", { class: "ag-list" });
      run.forEach(function (a) { list.appendChild(actionRow(a, now)); });
      g.appendChild(list);
    }
    return g;
  }

  /* ---- o plano do pedido ---- */
  function planView(planId) {
    var pd = msgs().planData(planId), now = Date.now();
    var running = B.thinking() && pd.t1 === 0 && pd.t0 > 0;
    var t = "Plano · " + pd.done + " de " + pd.total + (pd.total === 1 ? " passo" : " passos");
    if (pd.acts > 0) t += " · " + pd.acts + (pd.acts === 1 ? " ação" : " ações");
    if (pd.t1 > pd.t0) t += " · " + V.dur(pd.t1 - pd.t0);
    else if (running) t += " · " + V.dur(now - pd.t0);
    var root = h("div", { class: "pv" }), col = h("div", { class: "pv-col" });
    col.appendChild(h("div", { class: "pv-head", text: t }));
    pd.steps.forEach(function (sd) {
      var status = sd.status, count = sd.count, canOpen = count > 0;
      var key = planId + "." + sd.id, us = K.ui.step[key];
      var expanded = canOpen && (us ? us.v : status === 1);
      var liveMs = sd.ms;
      sd.actions.forEach(function (a) { if (a.state === 1) liveMs += Math.max(0, now - a.t0); });
      var mk;
      if (status === 1 && running) mk = h("span", { class: "spin" }, K.icon("gear", 14, "var(--ac)"));
      else mk = h("span", { class: "pv-mk" + (status === 2 || status === 1 ? " ac" : ""), text: status === 2 ? "✓" : status === 1 ? "▸" : status === 3 ? "–" : "○" });
      var meta = [];
      if (status === 3) meta.push("descartado");
      if (count > 0) meta.push(count + (count === 1 ? " ação" : " ações"));
      if (sd.failed > 0) meta.push(sd.failed + (sd.failed === 1 ? " falhou" : " falharam"));
      if (count > 0 && liveMs > 0) meta.push(V.dur(liveMs));
      var row = h("div", { class: "pv-step" + (canOpen ? " clickable" : "") },
        h("span", { class: "pv-mkw" }, mk),
        h("span", { class: "pv-title s" + status, text: sd.title || "" }),
        h("span", { class: "pv-meta" + (sd.failed > 0 ? " fail" : ""), text: meta.join(" · ") }),
        canOpen ? h("span", { class: "chev" + (expanded ? " up" : "") }, K.icon("down", 12, "var(--mut)")) : null);
      if (canOpen) row.addEventListener("click", function () { K.toggleUi(K.ui.step, key, !expanded); });
      var st = h("div", { class: "pv-stepbox" }, row);
      if (expanded) {
        var acts = h("div", { class: "pv-acts" });
        sd.actions.forEach(function (a) { acts.appendChild(actionRow(a, now)); });
        st.appendChild(acts);
      }
      col.appendChild(st);
    });
    root.appendChild(col);
    return root;
  }

  /* ---- uma entrada da lista ---- */
  function iconAsst() { return h("img", { class: "asst-ico", src: "img/katana-64.png", width: 26, height: 26, alt: "" }); }

  function buildEntry(e) {
    var it = e.item, row;
    if (e.type === "user") {
      var tm = K.showTime(it.time);
      row = h("div", { class: "r r-user" }, h("div", { class: "bubble", text: it.text }), tm ? h("div", { class: "utime", text: tm }) : null);
    } else if (e.type === "assistant") {
      var t2 = K.showTime(it.time);
      row = h("div", { class: "r r-asst" }, iconAsst(), t2 && t2.length <= 5 ? h("div", { class: "atime", text: t2 }) : null, assistantBody(it.text));
    } else if (e.type === "plan") row = h("div", { class: "r r-ind" }, planView(it.plan));
    else if (e.type === "tools") row = h("div", { class: "r r-ind" }, groupEl(e.run, e.active, "g" + it.id));
    else if (e.type === "notify") row = h("div", { class: "r r-ind" }, h("div", { class: "notify" },
      h("div", { class: "nt-h", text: "Aviso do agente · " + it.name }), h("div", { class: "nt-b", text: it.text })));
    else if (e.type === "file") {
      var nm = it.text.lastIndexOf("/") >= 0 ? it.text.slice(it.text.lastIndexOf("/") + 1) : it.text;
      row = h("div", { class: "r r-ind" }, h("div", { class: "fcard" },
        h("div", { class: "fc-l", text: "Arquivo do agente" }), h("div", { class: "fc-n", text: nm }),
        it.out ? h("div", { class: "fc-c", text: it.out }) : null, h("div", { class: "fc-p", text: it.text }),
        h("div", { class: "fc-btns" },
          h("button", { class: "pbtn", text: "Abrir", onclick: function () { B.openPath(it.text); } }),
          h("button", { class: "pbtn", text: "Abrir pasta", onclick: function () { B.openFolderOf(it.text); } }))));
    } else if (e.type === "findings") {
      var box = h("div", { class: "fcard find" }, h("div", { class: "fc-l", text: "Achados (" + e.run.length + ")" }));
      e.run.forEach(function (f) {
        box.appendChild(h("div", { class: "fi", onclick: function () { var m = f.name.match(/^(.*):(\d+)$/); B.openPath(m ? m[1] : f.name); } },
          h("div", { class: "fi-n", text: f.name + (f.out ? "  [" + f.out + "]" : "") }), h("div", { class: "fi-t", text: f.text })));
      });
      row = h("div", { class: "r r-ind" }, box);
    } else if (e.type === "stopped") {
      var last = e.last && B.ready;
      row = h("div", { class: "r r-stop" }, h("span", { class: "stopped", text: "o agente parou sem responder" }),
        last ? h("button", { class: "sbtn", text: "pedir resumo", onclick: function () { B.askSummary(); } }) : null);
    } else if (e.type === "error") row = h("div", { class: "r r-err" }, h("div", { class: "errbox", text: it.text }));
    else row = h("div", { class: "r r-notice" }, h("div", { text: it.text }));
    return row;
  }

  var cache = {};   /* chave -> {sig, el} */
  var stick = true, prog = false;

  function entries() {
    var items = msgs().items, out = [], n = items.length, i = 0, tick = sec();
    while (i < n) {
      var it = items[i], vk = msgs().vkind(it);
      if (vk === "hidden") { i++; continue; }
      if (vk === "tool" || vk === "finding") {
        var j = i, run = [];
        while (j < n && msgs().vkind(items[j]) === vk) {
          var m = items[j];
          run.push({ idx: j, id: m.id, name: m.name, args: m.text, desc: m.desc, state: m.state, t0: m.t0, dur: m.dur, note: m.note, hasOut: m.out.length > 0, text: m.text, out: m.out, rev: m.rev });
          j++;
        }
        if (vk === "finding") out.push({ type: "findings", key: "f" + it.id, item: it, run: run, sig: run.map(function (r) { return r.rev; }).join(",") });
        else {
          var active = B.thinking() && (i + run.length === n || (run.length > 0 && run[run.length - 1].state === 1));
          var running = run.some(function (r) { return r.state === 1; });
          out.push({ type: "tools", key: "g" + it.id, item: it, run: run, active: active,
            sig: run.map(function (r) { return r.rev; }).join(",") + "|" + active + "|" + K.ui.rev + (running || active ? "|" + tick : "") });
        }
        i = j; continue;
      }
      var e = { type: it.kind, key: "i" + it.id, item: it, sig: String(it.rev) };
      if (it.kind === "user" || it.kind === "assistant") e.sig += "|" + K.showTime(it.time);
      if (it.kind === "plan") {
        var p = msgs().plan(it.plan), pr = B.thinking() && p && p.t1 === 0 && p.t0 > 0;
        e.sig = (p ? p.rev : 0) + "|" + K.ui.rev + "|" + items.filter(function (x) { return x.kind === "tool" && x.plan === it.plan; }).map(function (x) { return x.rev; }).join(",") + "|" + !!pr + (pr ? "|" + tick : "") + "|" + B.thinking();
      }
      if (it.kind === "stopped") { e.last = i === n - 1; e.sig += "|" + e.last + "|" + B.ready; }
      if (["user", "assistant", "plan", "notify", "file", "stopped", "error", "notice"].indexOf(it.kind) < 0) e.type = "notice";
      out.push(e);
      i++;
    }
    return out;
  }

  function renderList() {
    var col = $("col"), mv = $("mv"), es = entries(), seen = {}, prev = null;
    es.forEach(function (e) {
      seen[e.key] = true;
      var c = cache[e.key];
      if (!c || c.sig !== e.sig) {
        var el = buildEntry(e);
        if (c && c.el.parentNode === col) col.replaceChild(el, c.el);
        c = cache[e.key] = { sig: e.sig, el: el };
      }
      /* ordem: cada entrada logo depois da anterior */
      var want = prev ? prev.nextSibling : col.firstChild;
      if (c.el !== want) col.insertBefore(c.el, want);
      prev = c.el;
    });
    for (var k in cache) if (!seen[k]) { if (cache[k].el.parentNode === col) col.removeChild(cache[k].el); delete cache[k]; }
    var dots = $("dots");
    var prep = /^(Retomando|Preparando|Carregando)/.test(B.status);
    var items = msgs().items;
    var showDots = B.thinking() && !prep && !(items.length > 0 && items[items.length - 1].kind === "tool");
    dots.hidden = !showDots;
    if (items.length === 0) $("empty").hidden = false; else $("empty").hidden = true;
    col.hidden = items.length === 0;
    if (stick) { prog = true; mv.scrollTop = mv.scrollHeight; prog = false; }
  }

  /* ---- tela inicial ---- */
  var SUG = [["Explicar", "Explique como funciona um motor de inferência de modelos de linguagem."],
    ["Programar", "Escreva uma função em C que inverte uma string, com um exemplo de uso."],
    ["Arquivos", "Liste os arquivos da pasta atual e me diga o que cada um parece fazer."]];
  var emptySig = "";
  function renderEmpty() {
    var el = $("empty");
    if (el.hidden) return;
    var prep = /^(Retomando|Preparando|Carregando)/.test(B.status);
    var sub = prep && B.running ? B.status : !B.running && !B.loading ? "Escolha um modelo embaixo, ao lado de enviar, para iniciar o agente."
      : B.loading ? "Carregando o modelo, pode levar alguns minutos..." : "Escreva abaixo ou comece por uma sugestão.";
    var ver = B.engineVersion + (B.specsActive.length ? " · " + B.specsActive.join(", ") : "");
    var narrow = $("mv").clientWidth < 640, short = $("mv").clientHeight < 560, dark = K.dark();
    var sig = [sub, ver, B.engineChanges, narrow, short, dark].join("|");
    if (sig === emptySig) return;
    emptySig = sig;
    var esc = short ? 4 : 5;
    function px(img, f) { img.addEventListener("load", function () { img.width = img.naturalWidth * f; img.height = img.naturalHeight * f; }); }
    var k = h("img", { class: "pix", src: "img/katana-k.png", alt: "Katana" });
    var title = h("img", { class: "pix", src: dark ? "img/titulo-escuro.png" : "img/titulo-claro.png", alt: "Katana" });
    px(k, esc / 8); px(title, esc);
    var grid = h("div", { class: "sug-grid" + (narrow ? " one" : "") });
    SUG.forEach(function (s) {
      grid.appendChild(h("button", { class: "sug", onclick: function () { K.suggestionChosen(s[1]); } }, h("div", { class: "sug-h", text: s[0] }), h("div", { class: "sug-b", text: s[1] })));
    });
    el.textContent = "";
    el.appendChild(h("div", { class: "emp-col" }, h("div", { class: "emp-img" }, k), h("div", { style: { height: "22px" } }), h("div", { class: "emp-img" }, title),
      h("div", { style: { height: "14px" } }), h("div", { class: "emp-greet", text: "Em que posso ajudar hoje?" }), h("div", { style: { height: "6px" } }),
      h("div", { class: "emp-sub", text: sub }), h("div", { style: { height: "8px" } }), h("div", { class: "emp-ver", text: ver }),
      B.engineChanges ? h("div", { class: "emp-chg", text: B.engineChanges }) : null, h("div", { style: { height: "18px" } }), grid));
    placeEmpty();
  }
  function placeEmpty() {
    var el = $("empty"), mv = $("mv");
    var bs = parseFloat(getComputedStyle(document.documentElement).getPropertyValue("--bottomspace")) || 100;
    el.style.top = Math.max(16, (mv.clientHeight - bs - el.offsetHeight) / 2 - 20 + mv.offsetTop) + "px";
  }
  K.placeEmpty = placeEmpty;

  /* ---- cartao de tarefas e pergunta, acima da caixa ---- */
  var workOpen = false, workSig = "";
  function renderCards() {
    var wc = $("workCard"), steps = B.todo, show = steps.length > 0 || B.planMode;
    var done = steps.filter(function (s) { return s.indexOf("[x]") === 0; }).length;
    var cur = "tudo feito";
    for (var i = 0; i < steps.length; i++) if (steps[i].indexOf("[x]") !== 0) { cur = steps[i].slice(4); break; }
    var sig = [show, workOpen, steps.join("\n"), B.planMode, B.ready, B.stats].join("|");
    if (sig !== workSig) {
      workSig = sig;
      wc.hidden = !show;
      wc.textContent = "";
      if (show) {
        var col = h("div", { class: "wc-col" });
        if (B.planMode) col.appendChild(h("div", { class: "wc-plan" }, h("span", { text: "Modo plano: o agente só lê. Nada é alterado até você aprovar." }),
          h("button", { class: "pbtn" + (B.ready ? "" : " dis"), id: "approvePlan", text: "Aprovar e executar", onclick: function () { if (B.ready) B.approvePlan(); } })));
        if (steps.length) {
          var head = h("div", { class: "wc-head" },
            h("span", { class: "wc-count", text: "Tarefas do agente  " + done + " de " + steps.length }),
            workOpen ? null : h("span", { class: "wc-cur", text: "○  " + cur }),
            h("span", { class: "wc-stats", text: B.stats }),
            h("span", { class: "wc-tog", text: workOpen ? "recolher" : "ver todas" }));
          head.addEventListener("click", function () { workOpen = !workOpen; workSig = ""; K.redraw(); });
          col.appendChild(head);
          if (workOpen) steps.forEach(function (s) {
            var d = s.indexOf("[x]") === 0;
            col.appendChild(h("div", { class: "wc-step" + (d ? " done" : ""), text: (d ? "✓  " : "○  ") + s.slice(4) }));
          });
        }
        wc.appendChild(col);
      }
    }
    var cc = $("choiceCard"), ch = B.choice, csig = ch ? JSON.stringify(ch) : "";
    if (cc.dataset.sig !== csig) {
      cc.dataset.sig = csig; cc.hidden = !ch; cc.textContent = "";
      if (ch) {
        var flow = h("div", { class: "cc-flow" });
        ch.opts.forEach(function (o) { flow.appendChild(h("button", { class: "opt", text: o, onclick: function () { B.answerChoice(o); } })); });
        cc.appendChild(h("div", { class: "cc-col" }, h("div", { class: "cc-l", text: "O agente pergunta" }), h("div", { class: "cc-q", text: ch.q }), flow,
          h("div", { class: "cc-hint", text: ch.opts.length ? "Ou escreva outra resposta na caixa abaixo." : "Escreva a resposta na caixa abaixo." })));
      }
    }
    K.layout();
  }

  /* ---- rolagem que nao puxa de volta ---- */
  K.initMessages = function () {
    var mv = $("mv");
    mv.addEventListener("scroll", function () { if (!prog) stick = mv.scrollHeight - mv.scrollTop - mv.clientHeight < 90; });
    K.stickEnd = function () { stick = true; };
    K.view(function () {
      var items = msgs().items;
      if (items.length && items[items.length - 1].kind === "user" && cache["i" + items[items.length - 1].id] === undefined) stick = true;
      renderList(); renderEmpty(); renderCards();
    });
    setInterval(function () {                      /* relogios das acoes em andamento */
      var running = msgs().items.some(function (x) { return x.kind === "tool" && x.state === 1; }) || B.thinking();
      if (running) { B.updateProgress(); K.redraw(); }
    }, 1000);
  };
  K.resetStick = function () { stick = true; };
})();
