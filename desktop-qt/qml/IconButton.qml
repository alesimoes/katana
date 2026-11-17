import QtQuick
import Katana
import QtQuick.Controls

// Botao de icone com hover, anel de foco por teclado e nome acessivel
AbstractButton {
    id: b
    property string glyph: ""
    property string tip: ""
    property real iconSize: 18
    property color iconColor: Theme.mut
    property color hoverIconColor: Theme.fg
    property color fillColor: "transparent"
    property color hoverFill: Theme.hov
    property real corner: 9
    property bool circle: false

    implicitWidth: 34
    implicitHeight: 34
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.name: tip
    Accessible.role: Accessible.Button
    ToolTip.visible: hovered && tip.length > 0 && enabled
    ToolTip.delay: 600
    ToolTip.text: tip

    background: Rectangle {
        radius: b.circle ? width / 2 : b.corner
        color: b.hovered && b.enabled ? b.hoverFill : b.fillColor
        border.width: b.visualFocus ? 2 : 0
        border.color: Theme.foc
    }
    contentItem: Item {
        Icon {
            anchors.centerIn: parent
            name: b.glyph
            size: b.iconSize
            color: b.hovered && b.enabled ? b.hoverIconColor : b.iconColor
        }
    }
}
