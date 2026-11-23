/* icones: os mesmos tracos de desktop-qt/qml/Icon.qml (grade 24x24, traco 2, pontas redondas) */
K.iconPaths = {
  plus: "M12 5v14M5 12h14", menu: "M4 7h16M4 12h16M4 17h16", left: "M15 6l-6 6 6 6", down: "M6 9l6 6 6-6", up: "M12 19V5M5 12l7-7 7 7",
  trash: "M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3", check: "M5 12l5 5 9-10", close: "M6 6l12 12M18 6L6 18",
  terminal: "M5 8l5 4-5 4M12 17h7", download: "M12 4v11M7 11l5 5 5-5M5 20h14", minimize: "M6 12h12", maximize: "M6 6h12v12H6V6",
  restore: "M8 8h10v10H8V8M8 8V5h10"
};
K.icon = function (name, size, color) {
  size = size || 18;
  var ns = "http://www.w3.org/2000/svg";
  var s = document.createElementNS(ns, "svg");
  s.setAttribute("viewBox", "0 0 24 24"); s.setAttribute("width", size); s.setAttribute("height", size);
  s.setAttribute("class", "ico ico-" + name);
  s.style.color = color || "";
  var inner = "";
  var st = 'fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"';
  if (name === "stop") inner = '<path d="M7 5H17Q19 5 19 7V17Q19 19 17 19H7Q5 19 5 17V7Q5 5 7 5Z" fill="currentColor"/>';
  else if (name === "gear") {
    var sp = "";
    for (var k = 0; k < 8; k++) { var a = k * Math.PI / 4; sp += "M" + (12 + Math.cos(a) * 6.6).toFixed(3) + " " + (12 + Math.sin(a) * 6.6).toFixed(3) + "L" + (12 + Math.cos(a) * 9.4).toFixed(3) + " " + (12 + Math.sin(a) * 9.4).toFixed(3); }
    inner = '<g ' + st + '><circle cx="12" cy="12" r="3.2"/><circle cx="12" cy="12" r="6.6"/><path d="' + sp + '"/></g>';
  } else if (name === "wrench") inner = '<g ' + st + '><path d="M4 20L12 12"/><circle cx="16" cy="8" r="4.2"/></g>';
  else inner = '<path ' + st + ' d="' + (K.iconPaths[name] || "") + '"/>';
  s.innerHTML = inner;
  return s;
};
