/* utilitarios: criar elementos, chamadas ao servidor, tempo */
function $(id) { return document.getElementById(id); }

/* h("div", {class:"x", style:"...", onclick:fn, text:"..."}, filhos...) */
function h(tag, props) {
  var e = document.createElement(tag);
  if (props) for (var k in props) {
    var v = props[k];
    if (v === undefined || v === null || v === false) continue;
    if (k === "class") e.className = v;
    else if (k === "text") e.textContent = v;
    else if (k === "html") e.innerHTML = v;
    else if (k.slice(0, 2) === "on") e.addEventListener(k.slice(2), v);
    else if (k === "style" && typeof v === "object") { for (var s in v) e.style[s] = v[s]; }
    else e.setAttribute(k, v === true ? "" : v);
  }
  for (var i = 2; i < arguments.length; i++) {
    var c = arguments[i];
    if (c === undefined || c === null || c === false) continue;
    e.appendChild(typeof c === "string" ? document.createTextNode(c) : c);
  }
  return e;
}

K.api = function (method, path, body) {
  var o = { method: method, headers: { "X-Katana": "web" }, credentials: "same-origin" };
  if (body !== undefined) { o.headers["Content-Type"] = "application/json"; o.body = JSON.stringify(body); }
  else if (method !== "GET") { o.headers["Content-Type"] = "application/json"; o.body = "{}"; }
  return fetch(path, o).then(function (r) {
    if (r.status === 401 || r.status === 403) { K.authLost && K.authLost(); }
    return r.text().then(function (t) {
      var j = null;
      try { j = t ? JSON.parse(t) : null; } catch (e) { j = null; }
      if (!r.ok) { var er = new Error((j && j.error) || ("HTTP " + r.status)); er.status = r.status; throw er; }
      return j;
    });
  });
};
K.get = function (p) { return K.api("GET", p); };
K.post = function (p, b) { return K.api("POST", p, b === undefined ? {} : b); };

K.pad = function (n, w) { n = String(n); while (n.length < (w || 2)) n = "0" + n; return n; };
/* duracao como o motor mostra na barra: "35 s", "1 min 20 s" (Backend::fmtDuration) */
K.fmtDuration = function (ms) {
  var s = Math.floor((ms + 500) / 1000);
  if (s < 60) return s + " s";
  if (s >= 120) return Math.floor((s + 30) / 60) + " min";
  return s % 60 ? "1 min " + (s - 60) + " s" : "1 min";
};
K.secsText = function (s) { return s < 60 ? s + " s" : Math.floor(s / 60) + " min " + K.pad(s % 60) + " s"; };
K.simplified = function (s) { return String(s).replace(/\s+/g, " ").trim(); };
/* "yyyy-MM-ddTHH:mm:ss" na hora local, e com o deslocamento */
K.isoLocal = function (d) {
  d = d || new Date();
  return d.getFullYear() + "-" + K.pad(d.getMonth() + 1) + "-" + K.pad(d.getDate()) + "T" + K.pad(d.getHours()) + ":" + K.pad(d.getMinutes()) + ":" + K.pad(d.getSeconds());
};
K.isoOffset = function (d) {
  d = d || new Date();
  var o = -d.getTimezoneOffset(), a = Math.abs(o);
  return K.isoLocal(d) + (o >= 0 ? "+" : "-") + K.pad(Math.floor(a / 60)) + ":" + K.pad(a % 60);
};
K.showTime = function (iso) {
  if (!iso) return "";
  var t = new Date(iso);
  if (isNaN(t.getTime())) return "";
  var n = new Date();
  var hm = K.pad(t.getHours()) + ":" + K.pad(t.getMinutes());
  if (t.toDateString() === n.toDateString()) return hm;
  return K.pad(t.getDate()) + "/" + K.pad(t.getMonth() + 1) + " " + hm;
};
K.relTime = function (ms) {
  var s = (Date.now() - ms) / 1000;
  if (isNaN(s)) return "";
  if (s < 60) return "agora";
  if (s < 3600) return "há " + Math.floor(s / 60) + " min";
  if (s < 86400) return "há " + Math.floor(s / 3600) + " h";
  if (s < 172800) return "ontem";
  if (s < 604800) return "há " + Math.floor(s / 86400) + " dias";
  var d = new Date(ms);
  return K.pad(d.getDate()) + "/" + K.pad(d.getMonth() + 1);
};
K.ptNum = function (n) { return String(n).replace(/\B(?=(\d{3})+(?!\d))/g, "."); };
K.ptDec = function (x, d) { return x.toFixed(d).replace(".", ","); };

/* dica ao passar o mouse (como o ToolTip do desktop) */
K.tip = function (el, textFn, delay) {
  var timer = null;
  el.addEventListener("mouseenter", function () {
    timer = setTimeout(function () {
      var t = typeof textFn === "function" ? textFn() : textFn;
      if (!t) return;
      var tp = $("tip"); tp.textContent = t; tp.hidden = false;
      var r = el.getBoundingClientRect();
      var w = tp.offsetWidth, hh = tp.offsetHeight;
      var x = Math.max(6, Math.min(r.left, innerWidth - w - 6));
      var y = r.bottom + 6; if (y + hh > innerHeight - 4) y = r.top - hh - 6;
      tp.style.left = x + "px"; tp.style.top = y + "px";
    }, delay || 500);
  });
  function off() { clearTimeout(timer); $("tip").hidden = true; }
  el.addEventListener("mouseleave", off);
  el.addEventListener("mousedown", off);
};

/* redesenho agrupado: quem muda o estado chama K.redraw() */
K._views = [];
K._pending = false;
K.view = function (fn) { K._views.push(fn); };
K.redraw = function () {
  if (K._pending) return;
  K._pending = true;
  requestAnimationFrame(function () { K._pending = false; K.redrawNow(); });
};
K.redrawNow = function () { for (var i = 0; i < K._views.length; i++) K._views[i](); };
