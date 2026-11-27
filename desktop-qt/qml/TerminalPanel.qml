import QtQuick
import Katana
import QtQuick.Controls

// Terminal embutido: shell real (pty) com cores e edicao de linha; as teclas vao direto ao shell
Rectangle {
    id: tp
    objectName: "terminalPanel"
    color: "#222733"      // azul-ardosia escuro, estilo editor de codigo: borda e texto aparecem
    property bool stick: true
    signal closeRequested()

    function focusTerm() { ed.forceActiveFocus(); }

    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: "#4a5266" }

    Item {
        id: head
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 32
        Rectangle { anchors.fill: parent; color: "#2a3040" }      // faixa do titulo um pouco mais clara
        Text { font.family: Theme.sans; x: 14; anchors.verticalCenter: parent.verticalCenter; text: "Terminal"; color: "#aab2c5"; font.pixelSize: 12; font.bold: true }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            AbstractButton {
                width: 64; height: 24; hoverEnabled: true
                Accessible.name: "Limpar o terminal"
                onClicked: { terminal.clear(); tp.focusTerm(); }
                background: Rectangle { radius: 6; color: parent.hovered ? "#343c4f" : "transparent" }
                contentItem: Text { font.family: Theme.sans; text: "Limpar"; color: "#aab2c5"; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            }
            AbstractButton {
                width: 64; height: 24; hoverEnabled: true
                Accessible.name: "Reiniciar o terminal"
                onClicked: { terminal.stop(); terminal.clear(); terminal.start(); tp.focusTerm(); }
                background: Rectangle { radius: 6; color: parent.hovered ? "#343c4f" : "transparent" }
                contentItem: Text { font.family: Theme.sans; text: "Reiniciar"; color: "#aab2c5"; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            }
            AbstractButton {
                width: 28; height: 24; hoverEnabled: true
                Accessible.name: "Fechar o terminal"
                onClicked: tp.closeRequested()
                background: Rectangle { radius: 6; color: parent.hovered ? "#343c4f" : "transparent" }
                contentItem: Text { font.family: Theme.sans; text: "✕"; color: "#aab2c5"; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            }
        }
    }

    Flickable {
        id: fl
        anchors.top: head.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 12
        anchors.bottomMargin: 6
        clip: true
        contentWidth: Math.max(width, ed.implicitWidth)
        contentHeight: ed.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
        onContentYChanged: tp.stick = contentHeight - contentY - height < 30
        onContentHeightChanged: if (tp.stick) Qt.callLater(() => fl.contentY = Math.max(0, fl.contentHeight - fl.height))

        TextEdit {
            id: ed
            objectName: "terminalView"
            width: Math.max(fl.width, implicitWidth)
            readOnly: true
            selectByMouse: true
            persistentSelection: true
            textFormat: TextEdit.RichText
            wrapMode: TextEdit.NoWrap
            text: terminal.html
            color: "#d7dce5"
            selectionColor: Theme.ac
            selectedTextColor: Theme.acfg
            font.family: Theme.mono
            font.pixelSize: 13
            cursorVisible: false
            Accessible.role: Accessible.EditableText
            Accessible.name: "Terminal"
            focus: true

            Keys.onPressed: (e) => {
                const ctrl = e.modifiers & Qt.ControlModifier, shift = e.modifiers & Qt.ShiftModifier, alt = e.modifiers & Qt.AltModifier;
                e.accepted = true;
                if (ctrl && shift && e.key === Qt.Key_C) { ed.copy(); return; }
                if ((ctrl && shift && e.key === Qt.Key_V) || (shift && e.key === Qt.Key_Insert)) { terminal.paste(); return; }
                switch (e.key) {
                case Qt.Key_Return: case Qt.Key_Enter: terminal.sendKey("\r"); return;
                case Qt.Key_Backspace: terminal.sendKey("\x7f"); return;
                case Qt.Key_Tab: terminal.sendKey("\t"); return;
                case Qt.Key_Escape: terminal.sendKey("\x1b"); return;
                case Qt.Key_Up: terminal.sendKey("\x1b[A"); return;
                case Qt.Key_Down: terminal.sendKey("\x1b[B"); return;
                case Qt.Key_Right: terminal.sendKey(ctrl ? "\x1b[1;5C" : "\x1b[C"); return;
                case Qt.Key_Left: terminal.sendKey(ctrl ? "\x1b[1;5D" : "\x1b[D"); return;
                case Qt.Key_Home: terminal.sendKey("\x1b[H"); return;
                case Qt.Key_End: terminal.sendKey("\x1b[F"); return;
                case Qt.Key_Delete: terminal.sendKey("\x1b[3~"); return;
                case Qt.Key_PageUp: fl.contentY = Math.max(0, fl.contentY - fl.height * 0.9); return;
                case Qt.Key_PageDown: fl.contentY = Math.min(Math.max(0, fl.contentHeight - fl.height), fl.contentY + fl.height * 0.9); return;
                case Qt.Key_Shift: case Qt.Key_Control: case Qt.Key_Alt: case Qt.Key_Meta: case Qt.Key_AltGr: case Qt.Key_CapsLock: return;
                }
                if (e.text.length > 0) { terminal.sendKey(alt ? "\x1b" + e.text : e.text); tp.stick = true; return; }
                e.accepted = false;
            }
            MouseArea {  // clique foca o terminal sem perder a selecao com o mouse
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                onWheel: (w) => w.accepted = false
            }
        }
    }
}
