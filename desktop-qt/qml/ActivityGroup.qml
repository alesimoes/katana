import QtQuick
import Katana
import QtQuick.Controls
import "Verbs.js" as V

// Grupo de acoes do agente: uma linha de resumo (com o que esta fazendo agora) e, ao abrir, a lista de passos
Rectangle {
    id: g
    property var steps: []          // [{idx, name, args, desc, state, t0, dur, hasOut, note}]
    property real now: Date.now()
    property bool active: false     // o agente ainda esta trabalhando nesta sequencia
    property real maxWidth: 600
    property bool userSet: false
    property bool userVal: false
    readonly property bool expanded: userSet ? userVal : active

    width: maxWidth
    height: head.height + (expanded ? list.height + 10 : 0)
    radius: 12
    color: Theme.card
    border.color: Theme.bd
    clip: true
    Behavior on height { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
    Accessible.role: Accessible.Grouping
    Accessible.name: "Acoes do agente: " + summary

    readonly property var last: steps.length ? steps[steps.length - 1] : null
    readonly property int failedCount: { var n = 0; for (var i = 0; i < steps.length; i++) if (steps[i].state === 3) n++; return n; }
    readonly property string summary: active && last ? V.titulo(last) + "…"
                                    : steps.length + (steps.length === 1 ? " ação executada" : " ações executadas")
                                      + (failedCount > 0 ? " (" + failedCount + (failedCount === 1 ? " falhou)" : " falharam)") : "")
    Timer { interval: 1000; repeat: true; running: g.active; onTriggered: g.now = Date.now() }

    AbstractButton {
        id: head
        width: parent.width
        height: 42
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.name: g.summary
        onClicked: { g.userSet = true; g.userVal = !g.expanded; }
        background: Rectangle { color: head.hovered ? Theme.hov : "transparent"; radius: 12
            border.width: head.visualFocus ? 2 : 0; border.color: Theme.foc }
        Item {
            id: ico
            x: 12; anchors.verticalCenter: parent.verticalCenter
            width: 18; height: 18
            Icon {
                anchors.centerIn: parent
                name: g.active ? "gear" : "check"
                size: 16
                color: g.active ? Theme.ac : Theme.mut
                RotationAnimator on rotation { running: g.active; from: 0; to: 360; duration: 2400; loops: Animation.Infinite }
            }
        }
        Text {
            anchors.left: ico.right; anchors.leftMargin: 10
            anchors.right: chev.left; anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.sans
            text: g.summary
            elide: Text.ElideRight
            font.pixelSize: 13
            font.weight: g.active ? Font.DemiBold : Font.Normal
            color: g.active ? Theme.fg : Theme.mut
        }
        Icon {
            id: chev
            anchors.right: parent.right; anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            name: "down"; size: 14; color: Theme.mut
            rotation: g.expanded ? 180 : 0
            Behavior on rotation { NumberAnimation { duration: 140 } }
        }
    }

    Column {
        id: list
        y: head.height
        width: parent.width
        leftPadding: 40
        rightPadding: 12
        spacing: 4
        Repeater {
            model: g.steps.length
            ActionRow {
                required property int index
                width: g.width - 52
                action: g.steps[index] || ({})
                now: g.now
            }
        }
    }
}
