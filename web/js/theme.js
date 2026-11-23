/* Cores e fontes: os mesmos valores de desktop-qt/qml/Theme.qml (tema escuro padrao, claro, ide; JetBrains Mono ou IBM Plex) */
var K = { rev: 0 };
K.pal = {
  claro:  { bg: "#faf9fc", side: "#f3f0f7", fg: "#1f1c24", mut: "#6b6773", bd: "#e2dde9", hov: "#ebe6f1", us: "#ece7f3", ac: "#820ad1", acfg: "#ffffff", card: "#ffffff", code: "#f5f2f8", codeh: "#ece7f3", err: "#b3261e", foc: "#820ad1", shadow: "#1e1028", kw: "#8a2bb5", str: "#a1470b", num: "#0b6e99", com: "#7a8a6a", fn: "#7a5a00", ty: "#00796b" },
  escuro: { bg: "#221f26", side: "#1b1920", fg: "#ece9f0", mut: "#a39fab", bd: "#38343f", hov: "#2b2831", us: "#332f3a", ac: "#b66cf2", acfg: "#1a1022", card: "#2b2831", code: "#1a181e", codeh: "#26232b", err: "#f2928b", foc: "#cfa3f7", shadow: "#000000", kw: "#d391f2", str: "#e8a56d", num: "#7cc4e8", com: "#7f8a74", fn: "#e6d88a", ty: "#6fd1c2" },
  ide:    { bg: "#1e1e1e", side: "#252526", fg: "#d4d4d4", mut: "#8b8b8b", bd: "#3c3c3c", hov: "#2a2d2e", us: "#2d2d30", ac: "#c586c0", acfg: "#1e1e1e", card: "#252526", code: "#161616", codeh: "#252526", err: "#f48771", foc: "#c586c0", shadow: "#000000", kw: "#c586c0", str: "#ce9178", num: "#b5cea8", com: "#6a9955", fn: "#dcdcaa", ty: "#4ec9b0" }
};
K.tema = "escuro";
K.fonte = "jetbrains";
K.dark = function () { return K.tema !== "claro"; };
K.cur = function () { return K.pal[K.tema] || K.pal.escuro; };
K.applyTheme = function () {
  var r = document.documentElement, p = K.cur();
  for (var k in p) r.style.setProperty("--" + k, p[k]);
  var sh = p.shadow, rr = parseInt(sh.slice(1, 3), 16) + "," + parseInt(sh.slice(3, 5), 16) + "," + parseInt(sh.slice(5, 7), 16);
  var al = K.dark() ? 0.07 : 0.028, layers = [];
  for (var i = 1; i <= 5; i++) layers.push("0 5px 0 " + (i * 3) + "px rgba(" + rr + "," + al + ")");
  r.style.setProperty("--shd", layers.join(","));
  var bg = p.bg; r.style.setProperty("--bg0", "rgba(" + parseInt(bg.slice(1, 3), 16) + "," + parseInt(bg.slice(3, 5), 16) + "," + parseInt(bg.slice(5, 7), 16) + ",0)");
  r.style.setProperty("--outbg", K.dark() ? "#0E0B12" : "#F1EEF5");
  r.style.setProperty("--warn", K.dark() ? "#e0b050" : "#b7791f");
  r.style.setProperty("--sans", K.fonte === "plex" ? "'IBM Plex Sans', sans-serif" : "'JetBrains Mono', monospace");
  r.style.setProperty("--mono", K.fonte === "plex" ? "'IBM Plex Mono', monospace" : "'JetBrains Mono', monospace");
  r.style.colorScheme = K.dark() ? "dark" : "light";
  r.dataset.theme = K.tema;
  var t = document.getElementById("sideTitle");
  if (t) t.src = K.dark() ? "img/titulo-escuro.png" : "img/titulo-claro.png";
};
try {
  var _t = localStorage.getItem("katana.tema"), _f = localStorage.getItem("katana.fonte");
  if (_t === "claro" || _t === "ide" || _t === "escuro") K.tema = _t;
  if (_f === "plex" || _f === "jetbrains") K.fonte = _f;
} catch (e) {}
K.applyTheme();
