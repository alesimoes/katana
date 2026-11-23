/* Lateral: marca, "Nova conversa" e as conversas salvas (Sidebar do desktop) */
(function () {
  var B = K.be, confirmId = "";
  function shortModel(m) { return (m || "").replace(/\.gguf$/i, ""); }
  var lastSig = "";
  function render() {
    var nb = $("newChat"), en = B.ready || !B.running;
    nb.disabled = !en;
    var sig = [confirmId, Math.floor(Date.now() / 60000), B.sessions.map(function (s) { return s.id + "|" + s.titulo + "|" + s.atualizada + "|" + s.atual + "|" + s.modelo; }).join("\n"), B.sess ? B.sess.id : ""].join("#");
    if (sig === lastSig) return;
    lastSig = sig;
    var list = $("sessList");
    list.textContent = "";
    if (!B.sessions.length) list.appendChild(h("div", { class: "sess-empty", text: "Nenhuma conversa salva ainda." }));
    B.sessions.forEach(function (s) {
      s.atual = !!(B.sess && B.sess.id === s.id);
      var confirming = confirmId === s.id;
      var row = h("div", { class: "sess" + (s.atual ? " cur" : "") + (confirming ? " conf" : "") });
      if (!confirming) {
        var open = h("button", { class: "sess-open", "aria-label": s.titulo || "Conversa sem titulo" },
          h("div", { class: "sess-t", text: s.titulo || "(sem título)" }),
          h("div", { class: "sess-m", text: K.relTime(s.atualizadaMs) + (s.modelo ? " · " + shortModel(s.modelo) : "") }));
        open.addEventListener("click", function () { K.closeDrawer(); B.openSession(s.id); });
        var del = h("button", { class: "ib sess-del", "aria-label": "Apagar conversa" }, K.icon("trash", 16));
        del.addEventListener("click", function () { confirmId = s.id; lastSig = ""; render(); });
        K.tip(del, "Apagar conversa", 600);
        row.appendChild(open); row.appendChild(del);
      } else {
        row.appendChild(h("div", { class: "sess-conf" }, h("div", { text: "Apagar esta conversa?" }),
          h("div", { class: "row" },
            h("button", { class: "cbtn danger", text: "Apagar", onclick: function () { confirmId = ""; lastSig = ""; B.deleteSession(s.id); } }),
            h("button", { class: "cbtn", text: "Cancelar", onclick: function () { confirmId = ""; lastSig = ""; render(); } }))));
      }
      list.appendChild(row);
    });
  }
  K.initSidebar = function () {
    $("newChatIcon").appendChild(K.icon("plus", 18, "var(--fg)"));
    $("sideCollapse").appendChild(K.icon("left", 18));
    $("newChat").addEventListener("click", function () { K.closeDrawer(); B.newChat(); });
    K.tip($("sideCollapse"), "Recolher a lateral", 600);
    K.view(render);
    setInterval(function () { lastSig = ""; render(); }, 60000);
  };
})();
