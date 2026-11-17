import QtQuick
import Katana
import QtQuick.Controls

// Indicador discreto de uso do contexto: anel de progresso + texto, com dica ao passar o mouse.
// Cores: normal (<70%), aviso (>=70%), critico (>=85%).
Item {
    id: m
    property int used: 0
    property int total: 0
    readonly property bool known: total > 0
    readonly property real frac: known ? Math.min(1, used / total) : 0
    readonly property int pct: Math.round(frac * 100)
    readonly property string level: !known ? "none" : frac >= 0.85 ? "crit" : frac >= 0.70 ? "warn" : "ok"
    readonly property color tone: level === "crit" ? Theme.err
                                 : level === "warn" ? (Theme.dark ? "#e0b050" : "#b7791f") : Theme.mut
    readonly property string label: known ? fmt(used) + " / " + fmt(total) + " tokens · " + pct + "%" : ""
    readonly property string tipText: known
        ? "Tokens usados: " + used + "\nContexto total: " + total + "\nLivre: " + Math.max(0, total - used)
          + " (" + (100 - pct) + "%)"
        : ""

    function fmt(n) {
        if (n >= 10000) return Math.round(n / 1000) + " mil";
        if (n >= 1000) return (n / 1000).toFixed(1).replace(".", ",") + " mil";
        return String(n);
    }

    visible: known
    implicitWidth: row.implicitWidth
    implicitHeight: 20
    Accessible.role: Accessible.StaticText
    Accessible.name: known ? "Contexto usado: " + label : ""

    Row {
        id: row
        spacing: 6
        anchors.verticalCenter: parent.verticalCenter
        Canvas {
            id: ring
            width: 14; height: 14
            anchors.verticalCenter: parent.verticalCenter
            property real f: m.frac
            property color c: m.level === "ok" ? Theme.ac : m.tone
            property color track: Theme.bd
            onFChanged: requestPaint()
            onCChanged: requestPaint()
            onTrackChanged: requestPaint()
            onPaint: {
                var ctx = getContext("2d");
                ctx.reset();
                ctx.lineWidth = 2;
                ctx.strokeStyle = track;
                ctx.beginPath(); ctx.arc(7, 7, 5.5, 0, 2 * Math.PI); ctx.stroke();
                if (f > 0) {
                    ctx.strokeStyle = c;
                    ctx.beginPath();
                    ctx.arc(7, 7, 5.5, -Math.PI / 2, -Math.PI / 2 + 2 * Math.PI * Math.max(0.02, f));
                    ctx.stroke();
                }
            }
        }
        Text {
            objectName: "ctxLabel"
            anchors.verticalCenter: parent.verticalCenter
            font.pixelSize: 12
            color: m.tone
            text: m.label
        }
    }
    HoverHandler { id: hh }
    ToolTip.visible: hh.hovered && known
    ToolTip.delay: 300
    ToolTip.text: tipText
}
