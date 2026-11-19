import QtQuick
import Katana
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// Caixa de mensagem flutuante embaixo, centralizada, com estatisticas discretas
Item {
    id: comp
    property alias input: ta
    readonly property real boxHeight: box.height
    implicitHeight: box.height + meta.height + 12
    width: parent ? parent.width : 400
    height: implicitHeight

    function setText(t) { ta.text = t; ta.cursorPosition = t.length; }
    function submit() {
        var t = ta.text.trim();
        if (backend.choicePending) {                  // o agente fez uma pergunta: o texto digitado e a resposta
            if (t) { backend.answerChoice(t); ta.clear(); }
            return;
        }
        if (backend.answering) {
            if (!t) { backend.stop(); return; }      // vazio: Parar
            if (backend.enqueue(t)) ta.clear();      // com texto: entra na fila
            return;
        }
        if (!t || !backend.ready) return;
        if (backend.send(t)) ta.clear();
    }
    Connections {
        target: backend
        function onStateChanged() { if (backend.ready) ta.forceActiveFocus(); }
    }

    readonly property real cw: Math.min(Theme.colWidth, width - 32)

    // sombra suave: camadas translucidas
    Repeater {
        model: 5
        Rectangle {
            required property int index
            x: box.x - (index + 1) * 3
            y: box.y - (index + 1) * 3 + 5
            width: box.width + (index + 1) * 6
            height: box.height + (index + 1) * 6
            radius: box.radius + (index + 1) * 3
            color: Theme.shadow
            opacity: Theme.dark ? 0.07 : 0.028
        }
    }

    // fila de mensagens: aparece acima da caixa; cada item tem X para remover
    Column {
        id: qcol
        visible: backend.queue.length > 0
        x: box.x
        y: -height - 6
        width: box.width
        spacing: 4
        Repeater {
            model: backend.queue
            Rectangle {
                required property string modelData
                required property int index
                width: qcol.width
                height: 30
                radius: 15
                color: Theme.card
                border.color: Theme.bd
                Text { font.family: Theme.sans; x: 14; anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 100; elide: Text.ElideRight; font.pixelSize: 12; color: Theme.fg
                    text: (backend.queuePaused ? "⏸ " : "⏳ ") + (index + 1) + ". " + modelData.replace(/\s+/g, " ") }
                Text { font.family: Theme.sans; anchors.right: xbtn.left; anchors.rightMargin: 10; anchors.verticalCenter: parent.verticalCenter
                    visible: index === 0 && backend.queuePaused; text: "retomar"; font.pixelSize: 12; color: Theme.ac
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: backend.resumeQueue() } }
                Text { id: xbtn; font.family: Theme.sans; anchors.right: parent.right; anchors.rightMargin: 14; anchors.verticalCenter: parent.verticalCenter
                    text: "✕"; font.pixelSize: 12; color: Theme.mut
                    MouseArea { anchors.fill: parent; anchors.margins: -6; cursorShape: Qt.PointingHandCursor; onClicked: backend.removeQueued(index) } }
            }
        }
    }

    Rectangle {
        id: box
        x: (comp.width - width) / 2
        y: 0
        width: comp.cw
        // cresce com o conteudo: minimo ~2 linhas, maximo ~10 linhas (depois rola)
        height: Math.max(76, fl.height + 24 + 38)
        radius: 22
        color: Theme.card
        border.color: ta.activeFocus ? Theme.mut : Theme.bd

        Flickable {
            id: fl
            objectName: "composerFlick"
            x: 20
            y: 12
            width: parent.width - 20 - 20
            height: Math.min(ta.implicitHeight, 240)
            contentWidth: width
            contentHeight: ta.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            TextArea.flickable: TextArea { font.family: Theme.sans;
                id: ta
                objectName: "composerInput"
                enabled: backend.ready || (backend.answering && !backend.loading)
                wrapMode: TextArea.Wrap
                padding: 0
                topPadding: 6
                bottomPadding: 6
                color: Theme.fg
                font.pixelSize: 15
                placeholderText: backend.ready ? "Escreva uma mensagem..."
                               : backend.loading ? (/^Preparando/.test(backend.statusText) ? backend.statusText : "Carregando o modelo...")
                               : backend.answering && /^(Retomando|Preparando)/.test(backend.statusText) ? backend.statusText
                               : backend.answering ? "Respondendo..."
                               : backend.running ? "Aguarde..." : "Escolha um modelo para começar"
                placeholderTextColor: Theme.mut
                selectionColor: Theme.ac
                selectedTextColor: Theme.acfg
                background: null
                Accessible.name: "Mensagem"
                TextMenu { id: tmenu; target: ta }
                TapHandler { acceptedButtons: Qt.RightButton; onTapped: { ta.forceActiveFocus(); tmenu.popup(); } }
                Accessible.role: Accessible.EditableText
                Keys.onPressed: (e) => {
                    if (e.key === Qt.Key_Escape && backend.answering) { e.accepted = true; backend.stop(); return; }
                    if ((e.key === Qt.Key_Return || e.key === Qt.Key_Enter) && !(e.modifiers & Qt.ShiftModifier)) {
                        e.accepted = true;
                        comp.submit();
                    }
                }
            }
        }

        IconButton {
            id: sendBtn
            objectName: "sendButton"
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 8
            implicitWidth: 38
            implicitHeight: 38
            circle: true
            glyph: backend.answering && ta.text.trim().length === 0 ? "stop" : "up"
            iconSize: backend.answering ? 18 : 18
            iconColor: Theme.acfg
            hoverIconColor: Theme.acfg
            fillColor: Theme.ac
            hoverFill: Qt.lighter(Theme.ac, 1.1)
            enabled: backend.answering || (backend.ready && ta.text.trim().length > 0)
            opacity: enabled ? 1 : 0.35
            tip: backend.answering ? (ta.text.trim().length ? "Enfileirar (envia quando o agente terminar)" : "Parar (Esc)") : "Enviar"
            onClicked: comp.submit()
        }
    }

    RowLayout {
        id: meta
        x: box.x
        y: box.height + 4
        width: box.width
        height: 22
        // seletor de modelo discreto (a lista abre para cima)
        ModelPicker {
            id: picker
            Layout.leftMargin: 6
            implicitHeight: 22
            maxWidth: Math.max(120, box.width * 0.45)
            compact: true
        }
        // pasta de trabalho do agente: mostra o nome da pasta; clique para escolher outra (o agente pergunta se confia nela)
        AbstractButton {
            id: wsBtn
            objectName: "workspaceButton"
            Layout.leftMargin: 2
            implicitHeight: 22
            implicitWidth: Math.min(wsTxt.implicitWidth + 14, 190)
            enabled: backend.ready || !backend.running
            hoverEnabled: true
            readonly property string folder: { var p = backend.workspace || ""; var i = p.lastIndexOf("/"); return i >= 0 && i < p.length - 1 ? p.slice(i + 1) : p; }
            background: Rectangle { radius: 6; color: wsBtn.hovered ? Theme.hov : "transparent" }
            contentItem: Text { id: wsTxt; font.family: Theme.sans; font.pixelSize: 12; elide: Text.ElideRight
                color: backend.workspaceTrusted ? Theme.mut : Theme.err
                text: (backend.workspaceTrusted ? "pasta: " : "sem confiança: ") + wsBtn.folder }
            ToolTip.visible: hovered
            ToolTip.delay: 400
            ToolTip.text: (backend.workspace || "") + (backend.workspaceTrusted ? "\n(pasta confiável: clique para trocar)" : "\n(sem confiança: toda ação pergunta antes; clique para trocar)")
            Accessible.name: "Pasta de trabalho"
            onClicked: folderDlg.open()
        }
        FolderDialog {
            id: folderDlg
            title: "Escolha a pasta de trabalho do agente"
            currentFolder: "file://" + backend.workspace
            onAccepted: backend.setWorkspace(selectedFolder.toString())
        }
        Text { font.family: Theme.sans;
            Layout.fillWidth: true
            Layout.leftMargin: 8
            elide: Text.ElideRight
            font.pixelSize: 12
            color: backend.errorText ? Theme.err : Theme.mut
            text: backend.errorText ? backend.errorText
                  : backend.statusText ? backend.statusText
                  : ta.text.indexOf("\n") >= 0 ? "As quebras de linha serão enviadas como espaço."
                  : ""
            Accessible.role: Accessible.StaticText
        }
        Text { font.family: Theme.sans;
            Layout.rightMargin: 4
            font.pixelSize: 12
            color: Theme.mut
            visible: text.length > 0
            text: backend.speedText
        }
        ContextMeter {
            id: meter
            objectName: "contextMeter"
            Layout.rightMargin: 14
            used: backend.ctxUsed
            total: backend.ctxTotal
        }
    }
}
