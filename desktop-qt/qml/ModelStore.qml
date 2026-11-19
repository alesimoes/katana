import QtQuick
import QtQuick.Controls
import Katana

// Modelos: o que esta maquina tem, o que cabe nela e o download (com progresso, pausa e retomada).
// Abre sozinho na primeira vez, quando ainda nao ha nenhum modelo utilizavel na pasta.
Rectangle {
    id: store
    objectName: "modelStore"
    property bool open: false
    readonly property bool first: !backend.hasUsableModel
    visible: open || first
    color: Theme.bg

    MouseArea { anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = false }

    Flickable {
        anchors.fill: parent
        contentHeight: col.height + 48
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { }

        Column {
            id: col
            width: Math.min(680, parent.width - 32)
            x: (parent.width - width) / 2
            y: 20
            spacing: 12

            Item {
                width: parent.width
                height: 34
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: store.first ? "Escolha um modelo para começar" : "Modelos"
                    color: Theme.fg
                    font.family: Theme.sans
                    font.pixelSize: 22
                    font.weight: Font.DemiBold
                }
                IconButton {
                    objectName: "storeClose"
                    visible: !store.first
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    glyph: "close"
                    tip: "Fechar"
                    onClicked: store.open = false
                }
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: store.first
                      ? "O katana roda o modelo no seu computador, sem enviar nada para fora. O download é feito uma vez; depois funciona sem internet."
                      : "Os modelos ficam no seu computador. Baixar outro não apaga os que você já tem."
                color: Theme.mut
                font.family: Theme.sans
                font.pixelSize: 14
            }
            Text {
                objectName: "storeMachine"
                width: parent.width
                wrapMode: Text.WordWrap
                visible: text.length > 0
                text: backend.machine.text ? "Esta máquina: " + backend.machine.text
                      + (backend.machine.ram_gb && parseFloat(backend.machine.ram_gb) < 21.5
                         ? ". O katana pede 24 GB de memória ou mais: nenhum dos modelos cabe aqui." : "") : ""
                color: Theme.fg
                font.family: Theme.sans
                font.pixelSize: 13
            }
            Rectangle {
                width: parent.width
                visible: backend.pullError.length > 0
                height: errTxt.implicitHeight + 20
                radius: 10
                color: "transparent"
                border.color: Theme.err
                Text {
                    id: errTxt
                    x: 12; y: 10
                    width: parent.width - 24
                    wrapMode: Text.WordWrap
                    text: backend.pullError
                    color: Theme.fg
                    font.family: Theme.sans
                    font.pixelSize: 13
                }
            }

            Repeater {
                model: backend.catalog
                delegate: Rectangle {
                    id: card
                    required property var modelData
                    readonly property bool pulling: backend.pullId === modelData.id
                    readonly property bool inUse: modelData.file === backend.currentModel
                    objectName: "storeItem_" + modelData.id
                    width: col.width
                    height: body.height + 26
                    radius: 14
                    color: Theme.card
                    border.color: modelData.recommended ? Theme.ac : Theme.bd
                    border.width: modelData.recommended ? 2 : 1

                    Column {
                        id: body
                        x: 16; y: 13
                        width: parent.width - 32
                        spacing: 6
                        Item {
                            width: parent.width
                            height: Math.max(34, nameCol.height)
                            Column {
                                id: nameCol
                                width: parent.width - act.width - 12
                                spacing: 2
                                Row {
                                    spacing: 8
                                    Text { text: card.modelData.name; color: Theme.fg; font.family: Theme.sans; font.pixelSize: 16; font.weight: Font.DemiBold }
                                    Text { text: card.modelData.sizeText; color: Theme.mut; font.family: Theme.sans; font.pixelSize: 13; anchors.baseline: parent.children[0].baseline }
                                }
                                Text {
                                    visible: text.length > 0
                                    text: card.modelData.recommended ? "Recomendado para esta máquina"
                                          : card.modelData.fit === 0 ? "Não cabe na memória desta máquina"
                                          : card.modelData.fit === 1 ? "Roda nesta máquina, mas devagar" : ""
                                    color: card.modelData.recommended ? Theme.ac : Theme.mut
                                    font.family: Theme.sans
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                }
                            }
                            Rectangle {
                                id: act
                                objectName: "storeAction_" + card.modelData.id
                                readonly property string label: card.pulling ? "Pausar"
                                    : card.inUse ? "Em uso"
                                    : card.modelData.state === "installed" ? "Usar"
                                    : card.modelData.state === "partial" ? "Continuar" : "Baixar"
                                readonly property bool active: card.pulling || (!card.inUse && card.modelData.fit > 0
                                    && (card.modelData.state === "installed" || backend.pullId === ""))
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                width: actTxt.implicitWidth + 28
                                height: 34
                                radius: 10
                                opacity: active ? 1 : 0.45
                                color: card.modelData.recommended && !card.pulling && card.modelData.state !== "installed"
                                       ? Theme.ac : (actMa.containsMouse && active ? Theme.hov : "transparent")
                                border.color: Theme.bd
                                Text {
                                    id: actTxt
                                    anchors.centerIn: parent
                                    text: act.label
                                    color: card.modelData.recommended && !card.pulling && card.modelData.state !== "installed" ? Theme.acfg : Theme.fg
                                    font.family: Theme.sans
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                }
                                MouseArea {
                                    id: actMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    enabled: act.active
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (card.pulling) backend.cancelPull();
                                        else if (card.modelData.state === "installed") { backend.start(card.modelData.file); store.open = false; }
                                        else backend.pull(card.modelData.id);
                                    }
                                }
                            }
                        }
                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: card.modelData.note
                            color: Theme.mut
                            font.family: Theme.sans
                            font.pixelSize: 13
                        }
                        // progresso: do download em curso, ou do que ja esta guardado de um download pausado
                        Rectangle {
                            visible: card.pulling || card.modelData.state === "partial"
                            width: parent.width
                            height: 6
                            radius: 3
                            color: Theme.hov
                            Rectangle {
                                width: parent.width * Math.max(0, Math.min(1, card.pulling ? backend.pullFraction : card.modelData.haveFraction))
                                height: parent.height
                                radius: 3
                                color: Theme.ac
                            }
                        }
                        Text {
                            objectName: "storeProgress_" + card.modelData.id
                            visible: card.pulling || card.modelData.state === "partial"
                            text: card.pulling ? backend.pullText : Math.round(card.modelData.haveFraction * 100) + "% já baixado"
                            color: Theme.mut
                            font.family: Theme.sans
                            font.pixelSize: 12
                        }
                    }
                }
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                visible: backend.catalog.length === 0
                text: "Lendo a lista de modelos..."
                color: Theme.mut
                font.family: Theme.sans
                font.pixelSize: 13
            }
        }
    }
}
