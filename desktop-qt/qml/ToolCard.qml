import QtQuick
import Katana
import QtQuick.Layouts
import QtQuick.Controls

// Cartao de ferramenta: icone, nome e argumentos truncados
Rectangle {
    id: card
    property string name: ""
    property string args: ""
    property real maxWidth: 600
    width: Math.min(maxWidth, lay.implicitWidth + 23)
    height: 40
    radius: 12
    color: Theme.card
    border.color: Theme.bd
    Accessible.role: Accessible.StaticText
    Accessible.name: "Ferramenta " + name + " " + args

    RowLayout {
        id: lay
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 13
        spacing: 10
        Rectangle {
            Layout.preferredWidth: 24
            Layout.preferredHeight: 24
            radius: 7
            color: Theme.us
            Icon {
                anchors.centerIn: parent
                name: "wrench"
                size: 14
                color: Theme.ac
            }
        }
        Text { font.family: Theme.sans;
            text: card.name || "ferramenta"
            font.pixelSize: 13
            font.weight: Font.DemiBold
            color: Theme.fg
        }
        Text { font.family: Theme.sans;
            Layout.fillWidth: true
            text: card.args.length > 140 ? card.args.slice(0, 140) + "..." : card.args
            elide: Text.ElideRight
            font.pixelSize: 13
            color: Theme.mut
        }
    }
    HoverHandler { id: hh }
    ToolTip.visible: hh.hovered && card.args.length > 0
    ToolTip.delay: 500
    ToolTip.text: card.name + ": " + card.args
}
