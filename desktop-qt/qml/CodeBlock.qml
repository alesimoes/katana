import QtQuick
import Katana
import QtQuick.Controls
import "Highlight.js" as Hl

// Bloco de codigo com cabecalho e botao Copiar
Rectangle {
    id: cb
    property string lang: "codigo"
    property string code: ""
    height: head.height + body.height + 1
    radius: 12
    color: Theme.code
    border.color: Theme.bd
    clip: true

    Rectangle {
        id: head
        width: parent.width
        height: 32
        color: Theme.codeh
        Text { font.family: Theme.sans;
            anchors.verticalCenter: parent.verticalCenter
            x: 14
            text: cb.lang
            color: Theme.mut
            font.pixelSize: 12
        }
        AbstractButton {
            id: copy
            property bool done: false
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right
            anchors.rightMargin: 6
            width: lbl.implicitWidth + 20
            height: 24
            hoverEnabled: true
            focusPolicy: Qt.StrongFocus
            Accessible.name: "Copiar o codigo"
            Accessible.role: Accessible.Button
            onClicked: {
                backend.copyText(cb.code);
                done = true;
                reset.restart();
            }
            Timer { id: reset; interval: 1500; onTriggered: copy.done = false }
            background: Rectangle {
                radius: 7
                color: copy.hovered ? Theme.hov : "transparent"
                border.width: copy.visualFocus ? 2 : 0
                border.color: Theme.foc
            }
            contentItem: Text { font.family: Theme.sans;
                id: lbl
                text: copy.done ? "Copiado" : "Copiar"
                color: copy.hovered ? Theme.fg : Theme.mut
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
    }
    Flickable {
        id: body
        y: head.height + 1
        width: parent.width
        height: ed.implicitHeight + 24
        contentWidth: Math.max(width, ed.implicitWidth + 28)
        contentHeight: height
        clip: true
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.horizontal: ScrollBar { policy: body.contentWidth > body.width ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }
        TextEdit {
            id: ed
            x: 14
            y: 12
            textFormat: TextEdit.RichText
            text: Hl.html(cb.code, cb.lang, Theme.pal)
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.NoWrap
            color: Theme.fg
            selectionColor: Theme.ac
            selectedTextColor: Theme.acfg
            font.family: Theme.mono
            font.pixelSize: 13
            Accessible.role: Accessible.StaticText
            Accessible.name: "Codigo"
            TextMenu { id: cmenu; target: ed }
            TapHandler { acceptedButtons: Qt.RightButton; onTapped: cmenu.popup() }
        }
    }
}
