import QtQuick
import Katana
import QtQuick.Controls
import "Verbs.js" as V

// Uma acao do agente: marca de estado, linha principal (descricao do modelo, ou verbo + alvo), tempo; ao abrir mostra o
// comando, a saida e as notas do motor (que sao para o modelo, por isso discretas)
Column {
    id: r
    objectName: "actionRow"
    property var action: ({})         // {idx, name, args, desc, state (0 ?, 1 rodando, 2 ok, 3 falhou), t0, dur, hasOut, note}
    property real now: Date.now()
    property bool userSet: false
    property bool userVal: false
    readonly property bool running: action.state === 1
    readonly property bool failed: action.state === 3
    readonly property bool hasDetail: !!action.hasOut || !!action.args || !!action.note
    readonly property bool expanded: hasDetail && (userSet ? userVal : (running && !!action.hasOut))
    readonly property string title: V.titulo(action)
    readonly property string timeText: running ? V.dur(Math.max(0, r.now - action.t0)) : (action.dur >= 0 ? V.dur(action.dur) : "")
    readonly property string outText: {
        var rv = backend.messages.revision;
        return expanded ? backend.messages.get(action.idx).out : "";
    }
    spacing: 3

    Item {
        width: r.width
        height: 22
        Item {
            id: mk
            width: 14; height: 22
            Icon {
                visible: r.running
                anchors.centerIn: parent
                name: "gear"; size: 13; color: Theme.ac
                RotationAnimator on rotation { running: r.running; from: 0; to: 360; duration: 2400; loops: Animation.Infinite }
            }
            Text {
                visible: !r.running
                anchors.centerIn: parent
                font.family: Theme.sans; font.pixelSize: 13; font.bold: r.failed
                text: r.failed ? "×" : "✓"
                color: r.failed ? Theme.err : Theme.mut
            }
        }
        Text {
            id: tt
            objectName: "actionTitle"
            anchors.left: mk.right; anchors.leftMargin: 8
            anchors.right: tm.left; anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.sans; font.pixelSize: 12
            font.weight: r.running ? Font.DemiBold : Font.Normal
            elide: Text.ElideRight
            text: r.title
            color: r.failed ? Theme.err : (r.running ? Theme.fg : Theme.mut)
        }
        Text {
            id: tm
            objectName: "actionTime"
            anchors.right: chev.left; anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.sans; font.pixelSize: 11
            text: r.timeText
            color: r.running ? Theme.ac : Theme.mut
        }
        Icon {
            id: chev
            visible: r.hasDetail
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            name: "down"; size: 12; color: Theme.mut
            rotation: r.expanded ? 180 : 0
        }
        MouseArea {
            anchors.fill: parent
            enabled: r.hasDetail
            cursorShape: r.hasDetail ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: { r.userSet = true; r.userVal = !r.expanded; }
        }
    }
    // comando ou argumentos (so quando a linha principal e a descricao do modelo)
    Text {
        visible: r.expanded && !!r.action.args
        x: 22; width: r.width - 22
        objectName: "actionCommand"
        font.family: Theme.mono; font.pixelSize: 11
        wrapMode: Text.WrapAnywhere
        color: Theme.mut
        text: r.action.name === "Bash" && r.action.args.indexOf("command: ") === 0 ? "$ " + r.action.args.slice(9) : r.action.name + ": " + r.action.args
    }
    Rectangle {
        visible: r.expanded && r.outText.length > 0
        x: 22; width: r.width - 22
        height: visible ? Math.min(outFl.contentHeight + 16, 220) : 0
        radius: 8
        color: Theme.dark ? "#0E0B12" : "#F1EEF5"
        border.color: Theme.bd
        clip: true
        Flickable {
            id: outFl
            anchors.fill: parent; anchors.margins: 8
            contentWidth: outTe.width; contentHeight: outTe.height
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick
            onContentHeightChanged: if (r.running) contentY = Math.max(0, contentHeight - height)
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            TextEdit {
                id: outTe
                objectName: "actionOutput"
                width: outFl.width - 10
                readonly property bool rich: V.realcavel(r.action.name)
                textFormat: rich ? TextEdit.RichText : TextEdit.PlainText
                text: rich ? V.realce(r.outText, ({ok: String(Theme.ty), mid: String(Theme.fn), low: String(Theme.str), err: String(Theme.err), lbl: String(Theme.kw)})) : r.outText
                readOnly: true; selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                color: Theme.fg; font.family: Theme.mono; font.pixelSize: 12
                selectionColor: Theme.ac; selectedTextColor: Theme.acfg
                TextMenu { id: omenu; target: outTe }
                TapHandler { acceptedButtons: Qt.RightButton; onTapped: omenu.popup() }
            }
        }
    }
    // notas do motor para o modelo (lembretes do plano, "pare e relate"): nao fazem parte do que a ferramenta imprimiu; legenda
    // discreta, em uma linha quando longa (clique para ver tudo)
    Text {
        id: noteTx
        property bool open: false
        readonly property string clean: (r.action.note || "").replace(/^\[(katana|plan): ?/mg, "").replace(/\]\s*$/mg, "")
        readonly property bool isLong: clean.length > 130 || clean.indexOf("\n") >= 0
        visible: r.expanded && !!r.action.note
        x: 22; width: r.width - 22
        objectName: "actionNote"
        font.family: Theme.sans; font.pixelSize: 11; font.italic: true
        wrapMode: Text.Wrap
        maximumLineCount: isLong && !open ? 1 : 1000
        elide: Text.ElideRight
        color: Theme.mut; opacity: 0.8
        text: "nota do motor (para o modelo): " + clean
        MouseArea {
            anchors.fill: parent
            enabled: noteTx.isLong
            cursorShape: Qt.PointingHandCursor
            onClicked: noteTx.open = !noteTx.open
        }
    }
}
