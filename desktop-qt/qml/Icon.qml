import QtQuick

// Icones simples desenhados com Canvas (caminhos em coordenadas 24x24)
Canvas {
    id: c
    property string name: ""
    property color color: "black"
    property real size: 18
    property real stroke: 2
    width: size
    height: size
    antialiasing: true

    readonly property var paths: ({
        "plus": "M12 5v14M5 12h14",
        "menu": "M4 7h16M4 12h16M4 17h16",
        "left": "M15 6l-6 6 6 6",
        "down": "M6 9l6 6 6-6",
        "up": "M12 19V5M5 12l7-7 7 7",
        "trash": "M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3",
        "check": "M5 12l5 5 9-10",
        "wrench": "M4 20L12 12M15 9m-1 0a4 4 0 1 0 8 0",
        "close": "M6 6l12 12M18 6L6 18",
        "terminal": "M5 8l5 4-5 4M12 17h7",
        "minimize": "M6 12h12",
        "maximize": "M6 6h12v12H6V6",
        "restore": "M8 8h10v10H8V8M8 8V5h10"
    })

    function draw(ctx, d) {
        var t = d.match(/[a-zA-Z]|-?\d*\.?\d+/g) || [];
        var i = 0, x = 0, y = 0, cmd = "", sx = 0, sy = 0;
        function num() { return parseFloat(t[i++]); }
        while (i < t.length) {
            if (/[a-zA-Z]/.test(t[i])) cmd = t[i++];
            if (cmd === "M") { x = num(); y = num(); ctx.moveTo(x, y); sx = x; sy = y; cmd = "L"; }
            else if (cmd === "m") { x += num(); y += num(); ctx.moveTo(x, y); cmd = "l"; }
            else if (cmd === "L") { x = num(); y = num(); ctx.lineTo(x, y); }
            else if (cmd === "l") { x += num(); y += num(); ctx.lineTo(x, y); }
            else if (cmd === "H") { x = num(); ctx.lineTo(x, y); }
            else if (cmd === "h") { x += num(); ctx.lineTo(x, y); }
            else if (cmd === "V") { y = num(); ctx.lineTo(x, y); }
            else if (cmd === "v") { y += num(); ctx.lineTo(x, y); }
            else if (cmd === "a") { i += 7; }  // so usado no circulo da ferramenta (tratado a parte)
            else break;
        }
    }

    onPaint: {
        var ctx = getContext("2d");
        ctx.reset();
        ctx.scale(width / 24, height / 24);
        ctx.strokeStyle = color;
        ctx.fillStyle = color;
        ctx.lineWidth = stroke;
        ctx.lineCap = "round";
        ctx.lineJoin = "round";
        if (name === "stop") {
            ctx.beginPath();
            ctx.moveTo(7, 5); ctx.lineTo(17, 5); ctx.quadraticCurveTo(19, 5, 19, 7);
            ctx.lineTo(19, 17); ctx.quadraticCurveTo(19, 19, 17, 19);
            ctx.lineTo(7, 19); ctx.quadraticCurveTo(5, 19, 5, 17);
            ctx.lineTo(5, 7); ctx.quadraticCurveTo(5, 5, 7, 5);
            ctx.fill();
            return;
        }
        if (name === "gear") {
            ctx.beginPath(); ctx.arc(12, 12, 3.2, 0, Math.PI * 2); ctx.stroke();
            ctx.beginPath(); ctx.arc(12, 12, 6.6, 0, Math.PI * 2); ctx.stroke();
            ctx.beginPath();
            for (var k = 0; k < 8; k++) {
                var a = k * Math.PI / 4;
                ctx.moveTo(12 + Math.cos(a) * 6.6, 12 + Math.sin(a) * 6.6);
                ctx.lineTo(12 + Math.cos(a) * 9.4, 12 + Math.sin(a) * 9.4);
            }
            ctx.stroke();
            return;
        }
        if (name === "wrench") {
            ctx.beginPath();
            ctx.moveTo(4, 20); ctx.lineTo(12, 12);
            ctx.stroke();
            ctx.beginPath();
            ctx.arc(16, 8, 4.2, 0, Math.PI * 2);
            ctx.stroke();
            return;
        }
        ctx.beginPath();
        draw(ctx, paths[name] || "");
        ctx.stroke();
    }
    onColorChanged: requestPaint()
    onNameChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    Component.onCompleted: requestPaint()
}
