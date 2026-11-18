import QtQuick
import Katana
import QtQuick.Controls

// Grupo de acoes do agente: uma linha de resumo (com o que esta fazendo agora) e, ao abrir, a lista de passos
Rectangle {
    id: g
    property var steps: []          // [{name, args}]
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

    readonly property var verbos: ({
        "list_dir": "Listando pasta", "read_file": "Lendo arquivo", "write_file": "Gravando arquivo",
        "edit_file": "Editando arquivo", "run_command": "Executando comando", "search_files": "Procurando nos arquivos",
        "glob": "Buscando arquivos", "web_search": "Pesquisando na web", "web_fetch": "Abrindo página",
        "download_file": "Baixando arquivo", "install_package": "Instalando pacote", "git": "Usando git",
        "read_document": "Lendo documento", "terminal": "Usando o terminal", "read_log": "Lendo logs", "terminal_screen": "Lendo a tela do terminal", "remember": "Guardando na memória", "recall": "Consultando a memória"
    })
    function verbo(n) { return verbos[n] || n || "Usando ferramenta"; }
    function alvo(a) {
        var s = (a || "").split(" | ")[0];
        var i = s.indexOf(": ");
        if (i > 0 && i < 20) s = s.slice(i + 2);
        return s.length > 90 ? s.slice(0, 90) + "…" : s;
    }
    readonly property var last: steps.length ? steps[steps.length - 1] : null
    readonly property string summary: active && last ? verbo(last.name) + (alvo(last.args) ? ": " + alvo(last.args) : "") + "…"
                                    : steps.length + (steps.length === 1 ? " ação executada" : " ações executadas")

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
            model: g.steps
            Row {
                required property var modelData
                required property int index
                spacing: 8
                width: g.width - 52
                Text { font.family: Theme.sans; text: index === g.steps.length - 1 && g.active ? "▸" : "✓"
                    color: index === g.steps.length - 1 && g.active ? Theme.ac : Theme.mut; font.pixelSize: 12; width: 12 }
                Text {
                    font.family: Theme.sans
                    width: parent.width - 20
                    text: g.verbo(modelData.name) + (g.alvo(modelData.args) ? ": " + g.alvo(modelData.args) : "")
                    elide: Text.ElideRight
                    color: Theme.mut; font.pixelSize: 12
                    ToolTip.visible: ma.containsMouse && modelData.args.length > 0
                    ToolTip.text: modelData.name + ": " + modelData.args
                    MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true }
                }
            }
        }
    }
}
