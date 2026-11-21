import QtQuick
import Katana
import "Verbs.js" as V

// O plano do pedido na conversa: cada passo da lista de tarefas do agente com as acoes que rodaram sob ele.
// Passo feito = uma linha (acoes e tempo; abre ao clicar); em andamento = aberto; pendente = apagado; descartado = riscado.
Rectangle {
    id: pv
    objectName: "planView"
    property int planId: 0
    property real maxWidth: 600
    readonly property var pdata: {
        var rv = backend.messages.revision;
        return backend.messages.planData(planId);
    }
    readonly property var steps: pdata.steps || []
    readonly property bool running: backend.thinking && pdata.t1 === 0 && pdata.t0 > 0
    property real now: Date.now()
    property var open: ({})          // passo (id) -> true/false: o que o dono abriu ou fechou
    readonly property int total: pdata.total || 0
    readonly property int done: pdata.done || 0
    readonly property string headText: {
        var t = "Plano · " + done + " de " + total + (total === 1 ? " passo" : " passos");
        if (pdata.acts > 0) t += " · " + pdata.acts + (pdata.acts === 1 ? " ação" : " ações");
        if (pdata.t1 > pdata.t0) t += " · " + V.dur(pdata.t1 - pdata.t0);
        else if (running) t += " · " + V.dur(now - pdata.t0);
        return t;
    }

    width: maxWidth
    height: col.height + 20
    radius: 12
    color: Theme.card
    border.color: Theme.bd
    Accessible.role: Accessible.Grouping
    Accessible.name: headText

    Timer { interval: 1000; repeat: true; running: pv.running; onTriggered: pv.now = Date.now() }

    Column {
        id: col
        x: 12; y: 10
        width: pv.width - 24
        spacing: 4
        Text {
            objectName: "planHeader"
            width: parent.width
            font.family: Theme.sans; font.pixelSize: 12; font.weight: Font.DemiBold
            color: Theme.mut
            elide: Text.ElideRight
            text: pv.headText
        }
        Repeater {
            model: pv.steps.length
            Column {
                id: st
                objectName: "planStep"
                required property int index
                readonly property var sd: pv.steps[index] || ({})
                readonly property int status: sd.status === undefined ? 0 : sd.status   // 0 pendente, 1 andando, 2 feito, 3 descartado
                readonly property int count: sd.count || 0
                readonly property bool canOpen: count > 0
                readonly property bool expanded: canOpen && (pv.open[sd.id] !== undefined ? pv.open[sd.id] : status === 1)
                readonly property real liveMs: {
                    var m = sd.ms || 0;
                    var acts = sd.actions || [];
                    for (var i = 0; i < acts.length; i++) if (acts[i].state === 1) m += Math.max(0, pv.now - acts[i].t0);
                    return m;
                }
                width: col.width
                spacing: 3
                Item {
                    width: st.width
                    height: 26
                    Item {
                        id: smk
                        width: 16; height: 26
                        Icon {
                            visible: st.status === 1 && pv.running
                            anchors.centerIn: parent
                            name: "gear"; size: 14; color: Theme.ac
                            RotationAnimator on rotation { running: st.status === 1 && pv.running; from: 0; to: 360; duration: 2400; loops: Animation.Infinite }
                        }
                        Text {
                            visible: !(st.status === 1 && pv.running)
                            anchors.centerIn: parent
                            font.family: Theme.sans; font.pixelSize: 14
                            text: st.status === 2 ? "✓" : st.status === 1 ? "▸" : st.status === 3 ? "–" : "○"
                            color: st.status === 2 || st.status === 1 ? Theme.ac : Theme.mut
                        }
                    }
                    Text {
                        id: stt
                        objectName: "stepTitle"
                        anchors.left: smk.right; anchors.leftMargin: 8
                        anchors.right: smeta.left; anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        font.family: Theme.sans; font.pixelSize: 13
                        font.weight: st.status === 1 ? Font.DemiBold : Font.Normal
                        font.strikeout: st.status === 3
                        elide: Text.ElideRight
                        text: st.sd.title || ""
                        color: st.status === 0 || st.status === 3 ? Theme.mut : Theme.fg
                    }
                    Text {
                        id: smeta
                        objectName: "stepMeta"
                        anchors.right: schev.left; anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        font.family: Theme.sans; font.pixelSize: 11
                        color: (st.sd.failed || 0) > 0 ? Theme.err : Theme.mut
                        text: {
                            var p = [];
                            if (st.status === 3) p.push("descartado");
                            if (st.count > 0) p.push(st.count + (st.count === 1 ? " ação" : " ações"));
                            if ((st.sd.failed || 0) > 0) p.push(st.sd.failed + (st.sd.failed === 1 ? " falhou" : " falharam"));
                            if (st.count > 0 && st.liveMs > 0) p.push(V.dur(st.liveMs));
                            return p.join(" · ");
                        }
                    }
                    Icon {
                        id: schev
                        visible: st.canOpen
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        name: "down"; size: 12; color: Theme.mut
                        rotation: st.expanded ? 180 : 0
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: st.canOpen
                        cursorShape: st.canOpen ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: { var o = Object.assign({}, pv.open); o[st.sd.id] = !st.expanded; pv.open = o; }
                    }
                }
                Column {
                    visible: st.expanded
                    x: 24
                    width: st.width - 24
                    spacing: 4
                    Repeater {
                        model: st.expanded ? (st.sd.actions || []).length : 0
                        ActionRow {
                            required property int index
                            width: st.width - 24
                            action: (st.sd.actions || [])[index] || ({})
                            now: pv.now
                        }
                    }
                }
            }
        }
    }
}
