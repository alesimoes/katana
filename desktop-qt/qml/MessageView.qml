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
    readonly property bool prep: /^(Retomando|Preparando|Carregando)/.test(backend.statusText)

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
            required property string out
            required property string time
            required property int index
            required property int plan
            required property string vkind
            // sequencia de ferramentas seguidas vira um unico grupo (so o primeiro item desenha)
            // (acoes que rodaram sob um passo do plano aparecem no item "plan", nao aqui: vkind "hidden")
            readonly property bool grouped: vkind === "tool" || vkind === "finding" || vkind === "hidden"
            readonly property bool groupHead: (vkind === "tool" || vkind === "finding") && (index === 0 || backend.messages.get(index - 1).vkind !== vkind)
            readonly property var run: {
                lv.count;
                var rv = backend.messages.revision;
                var r = [];
                if (!groupHead) return r;
                for (var i = index; i < lv.count; i++) {
                    var m = backend.messages.get(i);
                    if (m.vkind !== d.vkind) break;
                    r.push({idx: i, name: m.name, args: m.text, desc: m.desc, state: m.state, t0: m.t0, dur: m.dur, note: m.note, hasOut: m.out.length > 0});
                }
                return r;
            }
            width: lv.width
            height: grouped && !groupHead ? 0 : (ld.item ? ld.item.height : 0)
            Loader {
                id: ld
                width: Math.min(Theme.colWidth, d.width - 40)
                x: (d.width - width) / 2
                sourceComponent: d.grouped && !d.groupHead ? emptyC
                               : d.kind === "user" ? userC
                               : d.kind === "assistant" ? asstC
                               : d.kind === "plan" ? planC
                               : d.kind === "tool" ? toolC
                               : d.kind === "notify" ? notifyC
                               : d.kind === "file" ? fileC
                               : d.kind === "finding" ? findingC
                               : d.kind === "stopped" ? stoppedC : noticeC
            }
            Component {
                id: userC
                Item {
                    height: bubble.height + 22
                    TextMetrics { font.family: Theme.sans; id: tm; text: d.text; font.pixelSize: 15 }
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
                        TextEdit { font.family: Theme.sans;
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
                            TextMenu { id: umenu; target: ut }
                            TapHandler { acceptedButtons: Qt.RightButton; onTapped: umenu.popup() }
                        }
                    }
                    Text {      // hora em que a mensagem foi enviada
                        objectName: "msgTime"
                        visible: d.time.length > 0
                        anchors.right: parent.right
                        anchors.rightMargin: 6
                        y: bubble.height + 3
                        text: d.time
                        color: Theme.mut
                        font.family: Theme.sans
                        font.pixelSize: 10
                    }
                }
            }
            Component {
                id: asstC
                Item {
                    height: Math.max(body.height, 26) + 22
                    Image { width: 26; height: 26; source: "qrc:/icons/katana-64.png"; sourceSize: Qt.size(26*2, 26*2); smooth: true; mipmap: true }
                    Text {      // hora em que a resposta comecou
                        visible: d.time.length > 0 && d.time.length <= 5
                        x: 0; y: 29; width: 26
                        horizontalAlignment: Text.AlignHCenter
                        text: d.time
                        color: Theme.mut
                        font.family: Theme.sans
                        font.pixelSize: 9
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
            Component { id: emptyC; Item { height: 0 } }
            Component {
                id: planC
                Item {
                    height: pvw.height + 12
                    PlanView {
                        id: pvw
                        x: 38
                        planId: d.plan
                        maxWidth: parent.width - 38
                    }
                }
            }
            Component {
                id: toolC
                Item {
                    height: grp.height + 12
                    ActivityGroup {
                        id: grp
                        x: 38
                        maxWidth: parent.width - 38
                        steps: d.run
                        active: backend.thinking && (d.index + d.run.length === lv.count || (d.run.length > 0 && d.run[d.run.length - 1].state === 1))
                    }
                }
            }
            // PushNotification do agente: linha destacada
            Component {
                id: notifyC
                Item {
                    height: nt.height + 14
                    Rectangle {
                        id: nt
                        objectName: "notifyLine"
                        x: 38; width: parent.width - 38
                        height: ntCol.implicitHeight + 16
                        radius: 10
                        color: Theme.hov
                        border.color: Theme.ac
                        Column {
                            id: ntCol
                            x: 12; y: 8
                            width: parent.width - 24
                            spacing: 2
                            Text { font.family: Theme.sans; color: Theme.ac; font.pixelSize: 12; font.bold: true; width: parent.width; elide: Text.ElideRight; text: "Aviso do agente · " + d.name }
                            Text { font.family: Theme.sans; color: Theme.fg; font.pixelSize: 14; width: parent.width; wrapMode: Text.Wrap; text: d.text }
                        }
                    }
                }
            }
            // SendUserFile: cartao com o arquivo e botoes para abrir o arquivo ou a pasta
            Component {
                id: fileC
                Item {
                    height: fc.height + 14
                    Rectangle {
                        id: fc
                        objectName: "fileCard"
                        x: 38; width: Math.min(parent.width - 38, 620)
                        height: fcCol.implicitHeight + 24
                        radius: 14
                        color: Theme.card
                        border.color: Theme.bd
                        Column {
                            id: fcCol
                            x: 14; y: 12
                            width: parent.width - 28
                            spacing: 8
                            Text { font.family: Theme.sans; color: Theme.mut; font.pixelSize: 12; text: "Arquivo do agente" }
                            Text { objectName: "fileName"; font.family: Theme.sans; color: Theme.fg; font.pixelSize: 15; font.bold: true; width: parent.width; elide: Text.ElideMiddle
                                text: { var i = d.text.lastIndexOf("/"); return i >= 0 ? d.text.slice(i + 1) : d.text; } }
                            Text { visible: d.out.length > 0; font.family: Theme.sans; color: Theme.fg; font.pixelSize: 13; width: parent.width; wrapMode: Text.Wrap; text: d.out }
                            Text { font.family: Theme.sans; color: Theme.mut; font.pixelSize: 11; width: parent.width; elide: Text.ElideMiddle; text: d.text }
                            Row {
                                spacing: 8
                                Repeater {
                                    model: [["fileOpen", "Abrir"], ["fileFolder", "Abrir pasta"]]
                                    Rectangle {
                                        required property var modelData
                                        objectName: modelData[0]
                                        height: 34; width: bt.implicitWidth + 28; radius: 9
                                        color: bma.containsMouse ? Theme.ac : Theme.hov
                                        border.color: bma.containsMouse ? Theme.ac : Theme.bd
                                        Text { id: bt; anchors.centerIn: parent; text: modelData[1]; font.family: Theme.sans; font.pixelSize: 14
                                            color: bma.containsMouse ? Theme.acfg : Theme.fg }
                                        MouseArea { id: bma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                            onClicked: modelData[0] === "fileOpen" ? backend.openPath(d.text) : backend.openFolderOf(d.text) }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            // ReportFindings: lista compacta (arquivo:linha, resumo); cada item abre o arquivo
            Component {
                id: findingC
                Item {
                    height: fg.height + 14
                    Rectangle {
                        id: fg
                        objectName: "findingsCard"
                        x: 38; width: Math.min(parent.width - 38, 760)
                        height: fgCol.implicitHeight + 24
                        radius: 14
                        color: Theme.card
                        border.color: Theme.bd
                        Column {
                            id: fgCol
                            x: 14; y: 12
                            width: parent.width - 28
                            spacing: 4
                            Text { font.family: Theme.sans; color: Theme.mut; font.pixelSize: 12; text: "Achados (" + d.run.length + ")" }
                            Repeater {
                                model: d.run
                                Rectangle {
                                    required property var modelData
                                    objectName: "findingItem"
                                    width: fgCol.width
                                    height: frow.implicitHeight + 10
                                    radius: 8
                                    color: fma.containsMouse ? Theme.hov : "transparent"
                                    Column {
                                        id: frow
                                        x: 8; y: 5
                                        width: parent.width - 16
                                        Text { font.family: Theme.mono; color: Theme.ac; font.pixelSize: 12; width: parent.width; elide: Text.ElideMiddle
                                            text: modelData.name + (modelData.out ? "  [" + modelData.out + "]" : "") }
                                        Text { font.family: Theme.sans; color: Theme.fg; font.pixelSize: 13; width: parent.width; wrapMode: Text.Wrap; text: modelData.args }
                                    }
                                    MouseArea { id: fma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                        onClicked: { var l = modelData.name; var m = l.match(/^(.*):(\d+)$/); backend.openPath(m ? m[1] : l); } }
                                }
                            }
                        }
                    }
                }
            }
            // o pedido terminou e o ultimo turno do modelo nao teve texto
            Component {
                id: stoppedC
                Item {
                    height: 40
                    Row {
                        x: 38; anchors.verticalCenter: parent.verticalCenter
                        spacing: 12
                        Text { objectName: "stoppedLine"; anchors.verticalCenter: parent.verticalCenter; font.family: Theme.sans; color: Theme.mut; font.pixelSize: 12; font.italic: true
                            text: "o agente parou sem responder" }
                        Rectangle {
                            objectName: "askSummary"
                            visible: d.index === lv.count - 1 && backend.ready
                            anchors.verticalCenter: parent.verticalCenter
                            height: 28; width: sbt.implicitWidth + 22; radius: 8
                            color: sma.containsMouse ? Theme.ac : Theme.hov
                            border.color: sma.containsMouse ? Theme.ac : Theme.bd
                            Text { id: sbt; anchors.centerIn: parent; text: "pedir resumo"; font.family: Theme.sans; font.pixelSize: 12
                                color: sma.containsMouse ? Theme.acfg : Theme.fg }
                            MouseArea { id: sma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: backend.askSummary() }
                        }
                    }
                }
            }
            Component {
                id: noticeC
                Item {
                    height: 34
                    Text { font.family: Theme.sans;
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
                visible: backend.thinking && !mv.prep && !(lv.count > 0 && backend.messages.get(lv.count - 1).kind === "tool")
                x: (parent.width - Math.min(Theme.colWidth, parent.width - 40)) / 2
                width: 200
                height: 44
                Accessible.role: Accessible.StaticText
                Accessible.name: "Pensando"
                Image { width: 26; height: 26; source: "qrc:/icons/katana-64.png"; sourceSize: Qt.size(26*2, 26*2); smooth: true; mipmap: true }
                Text { font.family: Theme.sans; x: 80; y: 5; text: "Pensando…"; color: Theme.mut; font.pixelSize: 13; font.italic: true }
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
            // emblema: so o K do icone do app, pequeno
            Image {
                id: samurai
                objectName: "samurai"
                anchors.horizontalCenter: parent.horizontalCenter
                source: "qrc:/icons/katana-k.png"
                readonly property int escala: mv.height < 560 ? 4 : 5
                width: sourceSize.width * escala / 8
                height: sourceSize.height * escala / 8
                smooth: false
                mipmap: false
                Accessible.role: Accessible.Graphic
                Accessible.name: "Katana"
            }
            Item { width: 1; height: 22 }
            // titulo retro (anos 80): letras blocadas em pixel com degrade roxo e sombra, gerado por tools/gerar-samurai.py
            Image {
                objectName: "welcomeTitle"
                anchors.horizontalCenter: parent.horizontalCenter
                readonly property string text: "Katana"
                readonly property int escala: mv.height < 560 ? 4 : 5
                source: Theme.dark ? "qrc:/icons/titulo-escuro.png" : "qrc:/icons/titulo-claro.png"
                width: sourceSize.width * escala
                height: sourceSize.height * escala
                smooth: false
                mipmap: false
                Accessible.role: Accessible.StaticText
                Accessible.name: "Katana"
            }
            Item { width: 1; height: 14 }
            Text { font.family: Theme.sans;
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
            Text { font.family: Theme.sans;
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                color: Theme.mut
                font.pixelSize: 15
                text: mv.prep && backend.running ? backend.statusText
                      : !backend.running && !backend.loading ? "Escolha um modelo embaixo, ao lado de enviar, para iniciar o agente."
                      : backend.loading ? "Carregando o modelo, pode levar alguns minutos..."
                      : "Escreva abaixo ou comece por uma sugestão."
            }
            Item { width: 1; height: 8 }
            Text { font.family: Theme.sans;
                objectName: "engineVersion"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WrapAnywhere
                color: Theme.mut
                opacity: 0.7
                font.pixelSize: 11
                text: backend.engineVersion + (backend.specsActive.length ? " · " + backend.specsActive.join(", ") : "")
            }
            Text { font.family: Theme.sans;
                objectName: "engineChanges"
                visible: text.length > 0
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                color: Theme.mut
                opacity: 0.6
                font.pixelSize: 11
                topPadding: 4
                text: backend.engineChanges
            }
            Item { width: 1; height: 18 }
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
                            Text { font.family: Theme.sans; text: sg.modelData[0]; color: Theme.mut; font.pixelSize: 12; width: sg.width - 28 }
                            Text { font.family: Theme.sans; text: sg.modelData[1]; color: Theme.fg; font.pixelSize: 14; wrapMode: Text.Wrap; width: sg.width - 28 }
                        }
                    }
                }
            }
        }
    }
}
