/* Janela principal: barra de cima, configuracoes, dialogos, modelos, terminal, layout e a partida (Main.qml do desktop) */
(function () {
  var B = K.be, drawerOpen = false, termOpen = false, sideOpen = true, storeOpen = false, menuOpen = false, wake = null;

  function narrow() { return innerWidth < 760; }
  K.closeDrawer = function () { if (drawerOpen) { drawerOpen = false; applySide(); } };
  function applySide() {
    var side = $("side"), n = narrow(), open = n ? drawerOpen : sideOpen;
    side.classList.toggle("open", open);
    $("scrim").classList.toggle("show", n && drawerOpen);
    $("main").style.marginLeft = n ? "0px" : (open ? "304px" : "0px");
    $("main").classList.toggle("narrow", n);
    K.layout();
  }
  function toggleSide() {
    if (narrow()) drawerOpen = !drawerOpen;
    else { sideOpen = !sideOpen; setUi("lateralRecolhida", sideOpen ? "false" : "true"); }
    applySide();
  }
  function setUi(key, val) { B.ui[key] = val; K.post("web/ui", { key: key, value: val }).catch(function () {}); }

  /* ---- layout: altura da caixa, dos cartoes e do terminal ---- */
  K.layout = function () {
    var main = $("main"), comp = $("composer"), mv = $("mv");
    var termH = termOpen ? Math.min(340, Math.max(160, main.clientHeight * 0.4)) : 0;
    main.style.setProperty("--termh", termH + "px");
    $("term").style.height = termH + "px";
    var cw = Math.min(1100, comp.clientWidth - 32);
    main.style.setProperty("--cw", cw + "px");
    var qh = $("queue").children.length ? $("queue").offsetHeight + 6 : 0;
    var wc = $("workCard"), cc = $("choiceCard");
    var cardsH = (wc.hidden ? 0 : wc.offsetHeight + 6) + (cc.hidden ? 0 : cc.offsetHeight + 6) + qh;
    var compTop = termH + 6 + comp.offsetHeight;
    cc.style.bottom = (compTop + 6 + qh) + "px";
    wc.style.bottom = (cc.hidden ? compTop + 6 + qh : compTop + 6 + qh + cc.offsetHeight + 6) + "px";
    var bs = comp.offsetHeight + 24 + cardsH;
    document.documentElement.style.setProperty("--bottomspace", bs + "px");
    $("foot").style.height = bs + "px";
    var fade = $("fade");
    fade.style.height = (comp.offsetHeight + 40 + cardsH) + "px";
    fade.style.bottom = (termH + 6) + "px";
    K.placeEmpty && K.placeEmpty();
    if (mv && K._stickAfter) mv.scrollTop = mv.scrollHeight;
  };

  /* ---- dialogos ---- */
  function dialog(width, build, opts) {
    opts = opts || {};
    var back = h("div", { class: "dlg-back" }), card = h("div", { class: "dlg", style: { width: Math.min(width, innerWidth - 40) + "px" } });
    back.appendChild(card);
    var close = function () { back.remove(); if (opts.onclose) opts.onclose(); };
    build(card, close);
    if (!opts.sticky) back.addEventListener("mousedown", function (e) { if (e.target === back) close(); });
    $("dlgs").appendChild(back);
    return close;
  }
  function btnRow(buttons) { var r = h("div", { class: "dlg-btns" }); buttons.forEach(function (b) { r.appendChild(b); }); return r; }
  function dbtn(text, hl, fn) { return h("button", { class: "dbtn" + (hl ? " hl" : ""), text: text, onclick: fn }); }

  var askClose = null, pwClose = null;
  function renderDialogs() {
    if (B.ask && !askClose) {
      var a = B.ask, answered = false;
      askClose = dialog(480, function (card, close) {
        function answer(ok) { answered = true; B.answerAsk(ok); close(); }
        card.appendChild(h("div", { class: "dlg-t", text: a.kind === "trust" ? "Confiar nesta pasta?" : a.kind === "write" ? "Gravar fora da pasta de trabalho?" : "Permitir esta ação?" }));
        card.appendChild(h("div", { class: "dlg-p", text: a.kind === "trust"
          ? (a.what.indexOf("very broad") >= 0 ? "Atenção: esta pasta é muito ampla (a sua home ou a raiz). O Katana poderá ler e alterar tudo dentro dela, e executar comandos."
            : "O Katana poderá ler e gravar arquivos nesta pasta e executar comandos. Sem confiança, cada ação pergunta antes.")
          : a.kind === "write" ? "O agente quer gravar numa pasta fora da pasta de trabalho. Permitir vale para esta pasta, nesta sessão." : a.what }));
        card.appendChild(h("div", { class: "dlg-path", text: a.detail }));
        card.appendChild(btnRow([dbtn(a.kind === "trust" ? "Não confiar" : "Negar", false, function () { answer(false); }), dbtn(a.kind === "trust" ? "Confiar na pasta" : "Permitir", true, function () { answer(true); })]));
      }, { sticky: true, onclose: function () { askClose = null; if (B.ask) { B.answerAsk(false); } } });
    } else if (!B.ask && askClose) { var c = askClose; askClose = null; c(); }
    if (B.pw && !pwClose) {
      var retry = B.pw.retry;
      pwClose = dialog(420, function (card, close) {
        var f = h("input", { type: "password", class: "dinput", placeholder: "Senha", "aria-label": "Senha do sudo" });
        function answer(ok) { var v = ok ? f.value : ""; f.value = ""; B.sendPassword(v); close(); }
        card.appendChild(h("div", { class: "dlg-t", text: "Senha do sudo" }));
        if (retry) card.appendChild(h("div", { class: "dlg-p err", text: "Senha incorreta. Tente de novo." }));
        card.appendChild(h("div", { class: "dlg-p", text: "O agente quer executar um comando com sudo. A senha fica só na memória desta sessão e não vai para o modelo." }));
        card.appendChild(f);
        f.addEventListener("keydown", function (e) { if (e.key === "Enter") answer(true); });
        card.appendChild(btnRow([dbtn("Cancelar", false, function () { answer(false); }), dbtn("Autorizar", true, function () { answer(true); })]));
        setTimeout(function () { f.focus(); }, 0);
      }, { sticky: true, onclose: function () { pwClose = null; if (B.pw) B.sendPassword(""); } });
    } else if (!B.pw && pwClose) { var c2 = pwClose; pwClose = null; c2(); }
  }

  K.openTeacherDialog = function () {
    dialog(460, function (card, close) {
      var key = h("input", { type: "password", class: "dinput", placeholder: B.hasTeacherKey ? "Chave da API (em branco mantém a atual)" : "Chave da API (opcional; depende do plugin)", "aria-label": "Chave da API do professor" });
      var model = h("input", { type: "text", class: "dinput", placeholder: "Modelo (em branco: o padrão do plugin)", "aria-label": "Modelo do professor", value: B.teacherModel });
      function save() {
        var k = key.value.trim(), m = model.value.trim();
        key.value = "";
        K.post("web/teacher", { key: k, model: m }).then(function () {
          if (k) B.hasTeacherKey = true;
          if (m) B.teacherModel = m;
          B.ui.professorModelo = m;
          if (B.running && B.ready) B.writeLine("/teacher " + (B.teacherModel === "" ? "on" : B.teacherModel));
          K.redraw();
        }).catch(function () {});
        close();
      }
      card.appendChild(h("div", { class: "dlg-t", text: "Professor" }));
      card.appendChild(h("div", { class: "dlg-p", text: "Um modelo maior responde no lugar do modelo local, usando as mesmas ferramentas nesta máquina. Quem fala com ele é um programa externo (plugin do professor, configurado em ~/.katana/teacher.cmd ou KATANA_TEACHER_CMD). A chave, se o plugin pedir uma, fica em ~/.katana/teacher.key (legível só por você). A conversa, o prompt do agente e os resultados das ferramentas são entregues ao plugin e podem sair desta máquina." }));
      card.appendChild(key); card.appendChild(model);
      [key, model].forEach(function (f) { f.addEventListener("keydown", function (e) { if (e.key === "Enter") save(); }); });
      card.appendChild(btnRow([dbtn("Cancelar", false, close), dbtn("Salvar e ligar", true, save)]));
      setTimeout(function () { key.focus(); }, 0);
    });
  };

  /* escolha da pasta de trabalho: o navegador nao tem seletor de pasta da maquina, entao a lista vem do servidor */
  K.openFolderDialog = function () {
    dialog(520, function (card, close) {
      var path = h("input", { type: "text", class: "dinput mono", "aria-label": "Pasta" }), list = h("div", { class: "fd-list" }), err = h("div", { class: "dlg-p err", hidden: true });
      var cur = "";
      function go(p) {
        K.get("web/dirs?path=" + encodeURIComponent(p || "")).then(function (j) {
          cur = j.path; path.value = j.path; err.hidden = true; list.textContent = "";
          if (j.path !== j.parent) list.appendChild(h("button", { class: "fd-it", text: "..", onclick: function () { go(j.parent); } }));
          j.dirs.forEach(function (d) { list.appendChild(h("button", { class: "fd-it", text: d, onclick: function () { go((j.path === "/" ? "" : j.path) + "/" + d); } })); });
          if (!j.dirs.length) list.appendChild(h("div", { class: "fd-none", text: "(sem subpastas)" }));
        }).catch(function (e) { err.textContent = e.message; err.hidden = false; if (p) go(""); });
      }
      path.addEventListener("keydown", function (e) { if (e.key === "Enter") go(path.value); });
      card.appendChild(h("div", { class: "dlg-t", text: "Escolha a pasta de trabalho do agente" }));
      card.appendChild(path); card.appendChild(err); card.appendChild(list);
      card.appendChild(btnRow([dbtn("Cancelar", false, close), dbtn("Escolher esta pasta", true, function () { if (cur) B.setWorkspace(cur); close(); })]));
      go(B.workspace());
    });
  };

  /* ---- configuracoes ---- */
  function menuLabel(t, top) { return h("div", { class: "ap-l", style: { paddingTop: (top || 8) + "px" }, text: t }); }
  function menuItem(text, fn, opts) {
    opts = opts || {};
    return h("button", { class: "ap-it" + (opts.err ? " err" : ""), text: text, onclick: fn, disabled: opts.disabled, title: opts.title });
  }
  function renderMenu() {
    var m = $("apMenu"); m.hidden = !menuOpen;
    if (!menuOpen) return;
    m.textContent = "";
    m.appendChild(menuLabel("TEMA", 2));
    [["escuro", "Escuro (padrão)"], ["claro", "Claro"], ["ide", "IDE escuro (código)"]].forEach(function (t) {
      m.appendChild(menuItem((K.tema === t[0] ? "●  " : "○  ") + t[1], function () { setTheme(t[0]); }));
    });
    m.appendChild(menuLabel("TELA"));
    var keep = B.ui.manterAcordado !== "false";
    m.appendChild(menuItem((keep ? "☑  " : "☐  ") + "Não bloquear enquanto trabalha", function () { setUi("manterAcordado", keep ? "false" : "true"); updateWake(); renderMenu(); }));
    m.appendChild(menuLabel("PROFESSOR"));
    m.appendChild(menuItem("Chave e modelo…  (" + B.teacherModel + ")", function () { menuOpen = false; renderMenu(); K.openTeacherDialog(); }));
    m.appendChild(menuLabel("ESPECIALIZAÇÕES"));
    m.appendChild(h("div", { class: "ap-t" + (B.specsActive.length ? "" : " mut"), text: B.specsActive.length ? "Em uso neste modelo:\n" + B.specsActive.join("\n") : "Nenhuma em uso neste modelo." }));
    if (B.specsSkipped.length) m.appendChild(h("div", { class: "ap-t mut", text: "Puladas:\n" + B.specsSkipped.join("\n") }));
    m.appendChild(menuItem("Abrir a pasta", function () { K.post("web/open", { path: B.specsDir, mkdir: true }).catch(function () {}); }, { title: B.specsDir }));
    m.appendChild(menuLabel("PASTA DE TRABALHO"));
    var p = B.workspace() || "", i = p.lastIndexOf("/"), folder = i >= 0 && i < p.length - 1 ? p.slice(i + 1) : p;
    m.appendChild(menuItem(folder + (B.wsTrusted ? "" : "  (sem confiança)"), function () { menuOpen = false; renderMenu(); K.openFolderDialog(); },
      { err: !B.wsTrusted, disabled: !(B.ready || !B.running), title: p + "\nClique para trocar a pasta." }));
    m.appendChild(menuLabel("FONTE"));
    [["jetbrains", "JetBrains Mono"], ["plex", "IBM Plex"]].forEach(function (f) {
      m.appendChild(menuItem((K.fonte === f[0] ? "●  " : "○  ") + f[1], function () { setFont(f[0]); }));
    });
  }
  K.setTheme = function (t) { setTheme(t); };
  function setTheme(t) { K.tema = t; try { localStorage.setItem("katana.tema", t); } catch (e) {} setUi("tema", t); K.applyTheme(); K.ui.rev++; K.redrawNow(); renderMenu(); }
  function setFont(f) { K.fonte = f; try { localStorage.setItem("katana.fonte", f); } catch (e) {} setUi("fonte", f); K.applyTheme(); K.redrawNow(); renderMenu(); K.layout(); }

  /* ---- modelos ---- */
  function renderStore() {
    var st = $("store"), first = !B.hasUsableModel();
    var show = storeOpen || (first && B.models !== undefined && B.loaded);
    st.hidden = !show;
    if (!show) return;
    var sig = [first, B.model, B.models.map(function (m) { return m.file; }).join(",")].join("|");
    if (st.dataset.sig === sig) return;
    st.dataset.sig = sig;
    st.textContent = "";
    var col = h("div", { class: "st-col" });
    var head = h("div", { class: "st-head" }, h("span", { class: "st-t", text: first ? "Escolha um modelo para começar" : "Modelos" }));
    if (!first) { var x = h("button", { class: "ib", "aria-label": "Fechar" }, K.icon("close", 18)); x.addEventListener("click", function () { storeOpen = false; render(); }); head.appendChild(x); }
    col.appendChild(head);
    col.appendChild(h("div", { class: "st-p", text: first ? "O katana roda o modelo no seu computador, sem enviar nada para fora. O download é feito uma vez; depois funciona sem internet."
      : "Os modelos ficam no seu computador. Baixar outro não apaga os que você já tem." }));
    B.models.forEach(function (m) {
      var cur = m.file === B.model && B.running;
      col.appendChild(h("div", { class: "st-card" }, h("div", { class: "st-n" }, h("span", { text: m.name }), m.isDefault && m.usable ? h("span", { class: "pk-def", text: "padrão" }) : null),
        h("div", { class: "st-s", text: (m.usable ? "" : m.note + " · ") + m.sizeText }),
        m.usable ? h("button", { class: "pbtn", text: cur ? "Em uso" : "Usar", disabled: cur, onclick: function () { storeOpen = false; B.start(m.file); render(); } }) : null));
    });
    col.appendChild(h("div", { class: "st-p", text: "Pasta dos modelos: " + (B.modelsDir || "") + ". Para baixar um modelo novo, use no terminal: katana catalog e katana pull <id> (o download pela página não existe ainda)." }));
    st.appendChild(h("div", { class: "st-scroll" }, col));
  }

  /* ---- terminal ---- */
  K.toggleTerminal = function () {
    termOpen = !termOpen;
    $("term").hidden = !termOpen;
    if (termOpen) { if (!K.term.running) K.termStart(); setTimeout(K.termFocus, 0); } else $("ta").focus();
    K.layout();
  };

  /* ---- tela acordada enquanto o agente trabalha (Wake Lock do navegador, no lugar do inibidor do desktop) ---- */
  function updateWake() {
    var want = (B.ui.manterAcordado !== "false") && (B.answering || B.loading);
    if (want && !wake && navigator.wakeLock) navigator.wakeLock.request("screen").then(function (l) { wake = l; l.addEventListener("release", function () { wake = null; }); }).catch(function () {});
    else if (!want && wake) { wake.release().catch(function () {}); wake = null; }
  }

  function render() {
    renderDialogs(); renderMenu(); renderStore(); updateWake();
    document.title = "Katana";
  }
  K.authLost = function () { B.error = "A chave desta página não vale mais (o servidor foi reiniciado?). Abra de novo o endereço que o katana serve imprimiu no terminal."; K.redraw(); };

  /* dicas dos elementos que mudam (data-tip) */

  function boot() {
    document.documentElement.lang = "pt-BR";
    $("menuBtn").appendChild(K.icon("menu", 18)); $("storeBtn").appendChild(K.icon("download", 18));
    $("termBtn").appendChild(K.icon("terminal", 18)); $("gearBtn").appendChild(K.icon("gear", 18));
    $("menuBtn").addEventListener("click", toggleSide);
    $("scrim").addEventListener("click", K.closeDrawer);
    $("storeBtn").addEventListener("click", function () { storeOpen = !storeOpen; $("store").dataset.sig = ""; render(); });
    $("termBtn").addEventListener("click", K.toggleTerminal);
    $("gearBtn").addEventListener("click", function (e) { e.stopPropagation(); menuOpen = !menuOpen; renderMenu(); });
    document.addEventListener("click", function (e) { if (menuOpen && !e.target.closest("#apMenu")) { menuOpen = false; renderMenu(); } });
    K.tip($("menuBtn"), "Mostrar ou ocultar a lateral", 600); K.tip($("storeBtn"), "Modelos: baixar e trocar", 600);
    K.tip($("termBtn"), "Terminal (Ctrl+`)", 600); K.tip($("gearBtn"), "Configurações", 600);
    document.addEventListener("keydown", function (e) {
      if (e.ctrlKey && e.key === "`") { e.preventDefault(); K.toggleTerminal(); return; }
      if (e.key === "Escape" && !e.defaultPrevented) {
        if (menuOpen) { menuOpen = false; renderMenu(); return; }
        if (drawerOpen) { K.closeDrawer(); return; }
        if (B.answering) B.stop();
      }
    });
    addEventListener("resize", function () { applySide(); K.redraw(); });
    var stl = $("sideTitle");
    function szTitle() { if (stl.naturalWidth) { stl.width = stl.naturalWidth * 2; stl.height = stl.naturalHeight * 2; } }
    stl.addEventListener("load", szTitle); szTitle();
    var dots = $("dots");
    dots.appendChild(h("img", { src: "img/katana-64.png", width: 26, height: 26, alt: "" }));
    dots.appendChild(h("div", { class: "dt", text: "Pensando…" }));
    dots.appendChild(h("div", { class: "dd" }, h("i"), h("i"), h("i")));
    K.buildTerm();
    K.initMessages(); K.initComposer(); K.initSidebar();
    K.view(render);
    new ResizeObserver(function () { K.layout(); }).observe($("composer"));
    new ResizeObserver(function () { K.layout(); }).observe($("workCard"));
    new ResizeObserver(function () { K.layout(); }).observe($("choiceCard"));
    new ResizeObserver(function () { K.layout(); }).observe($("queue"));
    K.get("web/state").then(function (st) {
      B.loaded = true;
      var ui = st.ui || {};
      if (ui.tema) { K.tema = ui.tema === "claro" || ui.tema === "ide" ? ui.tema : "escuro"; }
      if (ui.fonte) K.fonte = ui.fonte === "plex" ? "plex" : "jetbrains";
      try { localStorage.setItem("katana.tema", K.tema); localStorage.setItem("katana.fonte", K.fonte); } catch (e) {}
      K.applyTheme();
      sideOpen = ui.lateralRecolhida !== "true";
      B.init(st);
            B.connect(st.next);
      applySide();
      B.refreshSessions().then(function () {
        if (B.running) {                             /* a pagina foi recarregada: volta a conversa que estava aberta */
          var id = ""; try { id = localStorage.getItem("katana.sessao") || ""; } catch (e) {}
          if (id && B.sessions.some(function (s) { return s.id === id; })) B.reopenDisplay(id);
        } else B.autoStartDefault();
      });
      setInterval(B.sysTick, 2000); B.sysTick();
      setInterval(B.indexPoll, 3000);
      K.redrawNow();
    }).catch(function () { K.authLost(); });
    applySide();
    K.redrawNow();
  }
  /* reabre a conversa so para mostrar (o agente ja esta rodando com ela) */
  B.reopenDisplay = function (id) {
    return B.openSession(id, true);
  };
  document.addEventListener("DOMContentLoaded", boot);
})();
