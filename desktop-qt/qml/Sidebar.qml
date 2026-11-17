import QtQuick
import Katana
import QtQuick.Controls
import QtQuick.Layouts

// Lateral: marca, "Nova conversa" e a lista de conversas salvas
Rectangle {
    id: side
    color: Theme.side
    signal collapseRequested()
    signal chosen()
    property string confirmId: ""
    property int tick: 0
    Timer { interval: 60000; running: true; repeat: true; onTriggered: side.tick++ }

    Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.bd }

    function relTime(ms) {
        tick;
        var s = (Date.now() - ms) / 1000;
        if (isNaN(s)) return "";
        if (s < 60) return "agora";
        if (s < 3600) return "há " + Math.floor(s / 60) + " min";
        if (s < 86400) return "há " + Math.floor(s / 3600) + " h";
        if (s < 172800) return "ontem";
        if (s < 604800) return "há " + Math.floor(s / 86400) + " dias";
        return Qt.formatDate(new Date(ms), "dd/MM");
    }
    function shortModel(m) { return (m || "").replace(/\.gguf$/i, ""); }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        anchors.rightMargin: 11
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            spacing: 8
            Rectangle {
                width: 22; height: 22; radius: 7; color: Theme.ac
                Text { anchors.centerIn: parent; text: "K"; color: Theme.acfg; font.bold: true; font.pixelSize: 13 }
            }
            Text { text: "katana"; color: Theme.fg; font.bold: true; font.pixelSize: 17; Layout.fillWidth: true }
            IconButton {
                objectName: "sidebarCollapse"
                glyph: "left"
                tip: "Recolher a lateral"
                onClicked: side.collapseRequested()
            }
        }

        AbstractButton {
            id: newBtn
            objectName: "newChatButton"
            Layout.fillWidth: true
            implicitHeight: 40
            enabled: backend.ready || !backend.running
            hoverEnabled: true
            focusPolicy: Qt.StrongFocus
            Accessible.name: "Nova conversa"
            Accessible.role: Accessible.Button
            opacity: enabled ? 1 : 0.55
            onClicked: { side.chosen(); backend.newChat(); }
            background: Rectangle {
                radius: 12
                color: newBtn.hovered && newBtn.enabled ? Theme.hov : Theme.card
                border.color: newBtn.visualFocus ? Theme.foc : Theme.bd
                border.width: newBtn.visualFocus ? 2 : 1
            }
            contentItem: Row {
                spacing: 8
                leftPadding: 12
                Item {
                    width: 18; height: 40
                    Icon { anchors.centerIn: parent; name: "plus"; color: Theme.fg }
                }
                Text {
                    height: 40
                    verticalAlignment: Text.AlignVCenter
                    text: "Nova conversa"
                    color: Theme.fg
                    font.pixelSize: 14
                    font.weight: Font.Medium
                }
            }
        }

        Text {
            text: "CONVERSAS"
            color: Theme.mut
            font.pixelSize: 11
            font.weight: Font.DemiBold
            font.letterSpacing: 0.6
            Layout.leftMargin: 10
            Layout.topMargin: 4
        }

        ListView {
            id: list
            objectName: "sessionList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: backend.sessions
            boundsBehavior: Flickable.StopAtBounds
            Accessible.role: Accessible.List
            Accessible.name: "Conversas salvas"
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            Text {
                visible: list.count === 0
                text: "Nenhuma conversa salva ainda."
                color: Theme.mut
                font.pixelSize: 13
                x: 10; y: 6
            }

            delegate: Rectangle {
                id: row
                required property var modelData
                objectName: "sessionItem"
                readonly property bool confirming: side.confirmId === modelData.id
                width: list.width
                height: confirming ? 78 : 52
                radius: 10
                color: modelData.atual ? Theme.us : (hov.hovered ? Theme.hov : "transparent")
                border.color: modelData.atual ? Theme.bd : "transparent"
                HoverHandler { id: hov }

                // linha normal
                AbstractButton {
                    id: openBtn
                    objectName: "sessionOpen"
                    visible: !row.confirming
                    anchors.fill: parent
                    anchors.rightMargin: 34
                    focusPolicy: Qt.StrongFocus
                    Accessible.name: row.modelData.titulo || "Conversa sem titulo"
                    Accessible.role: Accessible.Button
                    onClicked: { var id = row.modelData.id; side.chosen(); backend.openSession(id); }
                    background: Rectangle {
                        radius: 10
                        color: "transparent"
                        border.width: openBtn.visualFocus ? 2 : 0
                        border.color: Theme.foc
                    }
                    contentItem: Column {
                        leftPadding: 10
                        topPadding: 8
                        spacing: 0
                        Text {
                            width: openBtn.width - 14
                            text: row.modelData.titulo || "(sem título)"
                            elide: Text.ElideRight
                            color: Theme.fg
                            font.pixelSize: 14
                        }
                        Text {
                            width: openBtn.width - 14
                            text: side.relTime(row.modelData.atualizadaMs)
                                  + (row.modelData.modelo ? " · " + side.shortModel(row.modelData.modelo) : "")
                            elide: Text.ElideRight
                            color: Theme.mut
                            font.pixelSize: 12
                        }
                    }
                }
                IconButton {
                    objectName: "sessionDelete"
                    visible: !row.confirming
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 28
                    implicitHeight: 28
                    glyph: "trash"
                    iconSize: 16
                    hoverIconColor: Theme.err
                    tip: "Apagar conversa"
                    opacity: hov.hovered || visualFocus || activeFocus ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 120 } }
                    onClicked: side.confirmId = row.modelData.id
                }

                // confirmacao inline (sem dialogo modal)
                Column {
                    visible: row.confirming
                    x: 10
                    y: 8
                    spacing: 6
                    Text { text: "Apagar esta conversa?"; color: Theme.fg; font.pixelSize: 13 }
                    Row {
                        spacing: 6
                        AbstractButton {
                            id: yes
                            objectName: "confirmDelete"
                            width: 64; height: 26
                            hoverEnabled: true
                            focusPolicy: Qt.StrongFocus
                            Accessible.name: "Confirmar: apagar a conversa"
                            Accessible.role: Accessible.Button
                            onClicked: { side.confirmId = ""; backend.deleteSession(row.modelData.id); }
                            background: Rectangle {
                                radius: 8; color: Theme.err
                                opacity: yes.hovered ? 0.85 : 1
                                border.width: yes.visualFocus ? 2 : 0
                                border.color: Theme.foc
                            }
                            contentItem: Text {
                                text: "Apagar"; color: Theme.acfg; font.pixelSize: 13
                                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                            }
                        }
                        AbstractButton {
                            id: no
                            objectName: "cancelDelete"
                            width: 72; height: 26
                            hoverEnabled: true
                            focusPolicy: Qt.StrongFocus
                            Accessible.name: "Cancelar"
                            Accessible.role: Accessible.Button
                            onClicked: side.confirmId = ""
                            background: Rectangle {
                                radius: 8; color: no.hovered ? Theme.hov : Theme.card
                                border.color: no.visualFocus ? Theme.foc : Theme.bd
                                border.width: no.visualFocus ? 2 : 1
                            }
                            contentItem: Text {
                                text: "Cancelar"; color: Theme.fg; font.pixelSize: 13
                                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }
            }
        }
    }
}
