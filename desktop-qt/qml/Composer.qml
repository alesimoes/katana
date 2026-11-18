import QtQuick
import Katana
import QtQuick.Controls
import QtQuick.Layouts

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
        if (backend.answering) { backend.stop(); return; }
        var t = ta.text.trim();
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
            TextArea.flickable: TextArea {
                id: ta
                objectName: "composerInput"
                enabled: backend.ready
                wrapMode: TextArea.Wrap
                padding: 0
                topPadding: 6
                bottomPadding: 6
                color: Theme.fg
                font.pixelSize: 15
                placeholderText: backend.ready ? "Escreva uma mensagem..."
                               : backend.loading ? "Carregando o modelo..."
                               : backend.answering ? "Respondendo..."
                               : backend.running ? "Aguarde..." : "Escolha um modelo para começar"
                placeholderTextColor: Theme.mut
                selectionColor: Theme.ac
                selectedTextColor: Theme.acfg
                background: null
                Accessible.name: "Mensagem"
                Accessible.role: Accessible.EditableText
                Keys.onPressed: (e) => {
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
            glyph: backend.answering ? "stop" : "up"
            iconSize: backend.answering ? 18 : 18
            iconColor: Theme.acfg
            hoverIconColor: Theme.acfg
            fillColor: Theme.ac
            hoverFill: Qt.lighter(Theme.ac, 1.1)
            enabled: backend.answering || (backend.ready && ta.text.trim().length > 0)
            opacity: enabled ? 1 : 0.35
            tip: backend.answering ? "Parar" : "Enviar"
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
        Text {
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
        Text {
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
