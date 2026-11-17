import QtQuick
import Katana
import QtQuick.Controls
import QtQuick.Layouts

// Lista de mensagens + tela inicial vazia; rolagem automatica que nao puxa de volta
Item {
    id: mv
    property real bottomSpace: 100
    signal suggestionChosen(string text)
    property bool stick: true
    property bool prog: false

    readonly property var suggestions: [
        ["Explicar", "Explique como funciona um motor de inferência de modelos de linguagem."],
        ["Programar", "Escreva uma função em C que inverte uma string, com um exemplo de uso."],
        ["Arquivos", "Liste os arquivos da pasta atual e me diga o que cada um parece fazer."]
    ]

    function toEnd() {
        prog = true;
        lv.positionViewAtEnd();
        prog = false;
    }

    ListView {
        id: lv
        objectName: "messageList"
        anchors.fill: parent
        model: backend.messages
        clip: true
        cacheBuffer: 1000000
        topMargin: 8
        boundsBehavior: Flickable.StopAtBounds
        Accessible.role: Accessible.List
        Accessible.name: "Mensagens"
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        onContentYChanged: {
            if (!mv.prog)
                mv.stick = contentHeight - contentY - height < 90;
        }
        onContentHeightChanged: { if (mv.stick) Qt.callLater(mv.toEnd); }
        onCountChanged: {
            if (count > 0 && backend.messages.get(count - 1).kind === "user")
                mv.stick = true;
            if (mv.stick) Qt.callLater(mv.toEnd);
        }
        Connections {
            target: backend.messages
            function onModelReset() { mv.stick = true; Qt.callLater(mv.toEnd); }
        }

        delegate: Item {
            id: d
            required property string kind
            required property string text
            required property string name
            width: lv.width
            height: ld.item ? ld.item.height : 0
            Loader {
                id: ld
                width: Math.min(Theme.colWidth, d.width - 40)
                x: (d.width - width) / 2
                sourceComponent: d.kind === "user" ? userC
                               : d.kind === "assistant" ? asstC
                               : d.kind === "tool" ? toolC : noticeC
            }
            Component {
                id: userC
                Item {
                    height: bubble.height + 22
                    TextMetrics { id: tm; text: d.text; font.pixelSize: 15 }
                    Rectangle {
                        id: bubble
                        anchors.right: parent.right
                        width: Math.min(parent.width * 0.82, tm.advanceWidth + 34)
                        height: ut.implicitHeight + 20
                        radius: 20
                        color: Theme.us
                        Rectangle {  // canto inferior direito menos arredondado
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            width: 20; height: 20; radius: 6
                            color: parent.color
                        }
                        TextEdit {
                            id: ut
                            x: 16; y: 10
                            width: parent.width - 32
                            text: d.text
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.Wrap
                            color: Theme.fg
                            selectionColor: Theme.ac
                            selectedTextColor: Theme.acfg
                            font.pixelSize: 15
                            Accessible.role: Accessible.StaticText
                            Accessible.name: "Voce: " + d.text
                        }
                    }
                }
            }
            Component {
                id: asstC
                Item {
                    height: Math.max(body.height, 26) + 22
                    Rectangle {
                        width: 26; height: 26; radius: 13; color: Theme.ac
                        Text { anchors.centerIn: parent; text: "K"; color: Theme.acfg; font.bold: true; font.pixelSize: 12 }
                    }
                    AssistantBody {
                        id: body
                        x: 38
                        y: 1
                        width: parent.width - 38
                        text: d.text
                    }
                }
            }
            Component {
                id: toolC
                Item {
                    height: 54
                    ToolCard {
                        x: 38
                        maxWidth: parent.width - 38
                        name: d.name
                        args: d.text
                    }
                }
            }
            Component {
                id: noticeC
                Item {
                    height: 34
                    Text {
                        anchors.centerIn: parent
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: d.text
                        color: Theme.mut
                        font.pixelSize: 12
                        font.italic: true
                        elide: Text.ElideRight
                    }
                }
            }
        }

        footer: Item {
            width: lv.width
            height: mv.bottomSpace + (dots.visible ? 44 : 0)
            Item {
                id: dots
                visible: backend.thinking
                x: (parent.width - Math.min(Theme.colWidth, parent.width - 40)) / 2
                width: 200
                height: 44
                Accessible.role: Accessible.StaticText
                Accessible.name: "Pensando"
                Rectangle {
                    width: 26; height: 26; radius: 13; color: Theme.ac
                    Text { anchors.centerIn: parent; text: "K"; color: Theme.acfg; font.bold: true; font.pixelSize: 12 }
                }
                Row {
                    x: 38
                    y: 9
                    spacing: 5
                    Repeater {
                        model: 3
                        Rectangle {
                            id: dot
                            required property int index
                            width: 7; height: 7; radius: 3.5
                            color: Theme.mut
                            opacity: 0.4
                            SequentialAnimation {
                                running: dots.visible
                                loops: Animation.Infinite
                                PauseAnimation { duration: dot.index * 180 }
                                ParallelAnimation {
                                    NumberAnimation { target: dot; property: "opacity"; to: 1; duration: 350 }
                                    NumberAnimation { target: dot; property: "y"; to: -3; duration: 350 }
                                }
                                ParallelAnimation {
                                    NumberAnimation { target: dot; property: "opacity"; to: 0.25; duration: 450 }
                                    NumberAnimation { target: dot; property: "y"; to: 0; duration: 450 }
                                }
                                PauseAnimation { duration: (2 - dot.index) * 180 }
                            }
                        }
                    }
                }
            }
        }
    }

    // tela inicial
    Item {
        id: empty
        objectName: "emptyState"
        visible: lv.count === 0
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.max(16, (parent.height - mv.bottomSpace - height) / 2 - 20)
        width: Math.min(Theme.colWidth, parent.width - 40)
        height: col.implicitHeight

        Column {
            id: col
            width: parent.width
            spacing: 0
            // emblema: katana fina em diagonal, tracos vetoriais na cor de destaque
            Canvas {
                id: samurai
                objectName: "samurai"
                anchors.horizontalCenter: parent.horizontalCenter
                width: 72
                height: 72
                renderTarget: Canvas.Image
                property color cor: Theme.ac
                onCorChanged: requestPaint()
                Accessible.role: Accessible.Graphic
                Accessible.name: "Katana"
                onPaint: {
                    var c = getContext("2d")
                    c.reset()
                    c.translate(width / 2, height / 2)
                    c.rotate(-Math.PI / 4)
                    c.fillStyle = cor
                    c.strokeStyle = cor
                    // lamina: ponta fina, lombo reto, leve curva no fio
                    c.beginPath()
                    c.moveTo(-34, 1.6)
                    c.lineTo(-34, -1.4)
                    c.lineTo(22, -1.4)
                    c.quadraticCurveTo(31, -1.4, 35, -3.4)
                    c.quadraticCurveTo(33, 0.4, 28, 1.6)
                    c.closePath()
                    c.fill()
                    // guarda (tsuba)
                    c.beginPath()
                    c.ellipse(-37.5, -5, 3, 10)
                    c.fill()
                    // empunhadura com trancado discreto
                    c.lineWidth = 3.4
                    c.lineCap = "round"
                    c.globalAlpha = 0.55
                    c.beginPath()
                    c.moveTo(-60, 0.1)
                    c.lineTo(-38, 0.1)
                    c.stroke()
                }
            }
            Item { width: 1; height: 14 }
            Text {
                objectName: "welcomeTitle"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "Katana"
                color: Theme.ac
                font.pixelSize: 30
                font.weight: Font.Bold
                font.letterSpacing: 3
            }
            Item { width: 1; height: 4 }
            Text {
                objectName: "welcomeGreeting"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "Em que posso ajudar hoje?"
                color: Theme.fg
                font.pixelSize: 20
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
            }
            Item { width: 1; height: 6 }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                color: Theme.mut
                font.pixelSize: 15
                text: !backend.running && !backend.loading ? "Escolha um modelo no topo para iniciar o agente."
                      : backend.loading ? "Carregando o modelo, pode levar alguns minutos..."
                      : "Escreva abaixo ou comece por uma sugestão."
            }
            Item { width: 1; height: 26 }
            GridLayout {
                width: parent.width
                columns: mv.width < 640 ? 1 : 3
                columnSpacing: 10
                rowSpacing: 10
                Repeater {
                    model: mv.suggestions
                    AbstractButton {
                        id: sg
                        required property var modelData
                        objectName: "suggestion"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.alignment: Qt.AlignTop
                        implicitHeight: txt.implicitHeight + 26
                        hoverEnabled: true
                        focusPolicy: Qt.StrongFocus
                        Accessible.name: modelData[0] + ": " + modelData[1]
                        Accessible.role: Accessible.Button
                        onClicked: mv.suggestionChosen(modelData[1])
                        background: Rectangle {
                            radius: 14
                            color: sg.hovered ? Theme.hov : Theme.card
                            border.color: sg.visualFocus ? Theme.foc : Theme.bd
                            border.width: sg.visualFocus ? 2 : 1
                        }
                        contentItem: Column {
                            id: txt
                            leftPadding: 14
                            rightPadding: 14
                            topPadding: 12
                            spacing: 2
                            Text { text: sg.modelData[0]; color: Theme.mut; font.pixelSize: 12; width: sg.width - 28 }
                            Text { text: sg.modelData[1]; color: Theme.fg; font.pixelSize: 14; wrapMode: Text.Wrap; width: sg.width - 28 }
                        }
                    }
                }
            }
        }
    }
}
