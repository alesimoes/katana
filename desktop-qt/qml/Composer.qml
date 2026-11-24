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
    // altura da fila de mensagens, que fica acima da caixa (fora da altura deste item): quem empilha cartoes acima soma isto
    readonly property real queueHeight: qcol.visible ? qcol.height + 6 : 0
    implicitHeight: box.height + meta.height + 12 + (sys.visible ? sys.height + 2 : 0)
    width: parent ? parent.width : 400
    height: implicitHeight

    function setText(t) { ta.text = t; ta.cursorPosition = t.length; }
    function submit() {
        var t = ta.text.trim();
        if (backend.attachedImage) {                  // imagem anexada: vai para o gerador, nao para o agente
            if (!backend.imageBusy && backend.sendImage(t)) ta.clear();
            return;
        }
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

    // arrastar uma imagem para a caixa a anexa
    DropArea {
        anchors.fill: parent
        onDropped: (d) => { if (d.hasUrls && d.urls.length > 0) backend.attachImage(d.urls[0].toString()); }
    }
    FileDialog {
        id: imgDialog
        title: "Anexar imagem"
        nameFilters: ["Imagens (*.png *.jpg *.jpeg *.webp *.bmp)"]
        onAccepted: backend.attachImage(selectedFile.toString())
    }


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
        // cresce com o conteudo: minimo ~4 linhas, maximo ~14 linhas (depois rola)
        readonly property real chipH: backend.attachedImage.length > 0 ? 62 : 0
        height: Math.max(116, chipH + fl.height + 24 + 38)
        radius: 22
        color: Theme.card
        border.color: ta.activeFocus ? Theme.mut : Theme.bd

        Flickable {
            id: fl
            objectName: "composerFlick"
            x: 20
            y: 12 + box.chipH
            width: parent.width - 20 - 20
            height: Math.min(ta.implicitHeight, 330)
            contentWidth: width
            contentHeight: ta.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            TextArea.flickable: TextArea { font.family: Theme.sans;
                id: ta
                objectName: "composerInput"
                enabled: backend.ready || (backend.answering && !backend.loading) || backend.attachedImage.length > 0
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
                    if ((e.matches(StandardKey.Paste) || (e.key === Qt.Key_V && (e.modifiers & Qt.ControlModifier))) && backend.pasteClipboardImage()) { e.accepted = true; return; }
                    if (e.key === Qt.Key_Escape && backend.answering) { e.accepted = true; backend.stop(); return; }
                    if ((e.key === Qt.Key_Return || e.key === Qt.Key_Enter) && !(e.modifiers & Qt.ShiftModifier)) {
                        e.accepted = true;
                        comp.submit();
                    }
                }
            }
        }

        // miniatura da imagem anexada (chip removivel) em cima do texto
        Rectangle {
            id: thumb
            objectName: "attachThumb"
            visible: backend.attachedImage.length > 0
            x: 20; y: 12
            width: 72; height: 48; radius: 8
            color: Theme.hov
            border.color: Theme.bd
            clip: true
            Image {
                anchors.fill: parent
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                source: backend.attachedImage ? "file://" + backend.attachedImage : ""
                sourceSize.width: 144
            }
            Rectangle {
                objectName: "attachRemove"
                anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 3
                width: 18; height: 18; radius: 9
                color: Theme.card; opacity: 0.92
                Text { anchors.centerIn: parent; text: "✕"; font.pixelSize: 10; color: Theme.fg }
                MouseArea { anchors.fill: parent; anchors.margins: -3; cursorShape: Qt.PointingHandCursor; onClicked: backend.clearImage() }
            }
        }
        // anexar imagem (+), dentro da caixa, a esquerda
        IconButton {
            id: attachBtn
            objectName: "attachButton"
            anchors.left: parent.left
            anchors.leftMargin: 8
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 8
            implicitWidth: 34
            implicitHeight: 34
            glyph: "plus"
            iconSize: 20
            tip: "Anexar uma imagem"
            onClicked: imgDialog.open()
        }
        // motor da imagem: so aparece com uma imagem anexada
        ComboBox {
            id: engineBox
            objectName: "imageEngineBox"
            visible: backend.attachedImage.length > 0
            anchors.right: sendBtn.left
            anchors.rightMargin: 8
            anchors.verticalCenter: sendBtn.verticalCenter
            width: 168; height: 30
            model: [{ id: "stable", label: "Stable (rápido)" }, { id: "flux", label: "FLUX (qualidade)" }]
            textRole: "label"
            currentIndex: backend.imageEngine === "flux" ? 1 : 0
            onActivated: (i) => backend.imageEngine = model[i].id
            Accessible.name: "Motor da imagem"
            font.family: Theme.sans; font.pixelSize: 12
            background: Rectangle { radius: 15; color: engineBox.hovered ? Theme.hov : "transparent"; border.color: Theme.bd }
            contentItem: Text { leftPadding: 12; rightPadding: 24; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                font: engineBox.font; color: Theme.fg; text: engineBox.displayText }
            indicator: Icon { x: engineBox.width - width - 10; y: (engineBox.height - height) / 2; name: "down"; size: 12; color: Theme.mut }
            popup: Popup {
                y: -implicitHeight - 4
                width: engineBox.width
                padding: 4
                implicitHeight: contentItem.implicitHeight + 8
                background: Rectangle { radius: 10; color: Theme.card; border.color: Theme.bd }
                contentItem: ListView {
                    implicitHeight: contentHeight
                    model: engineBox.popup.visible ? engineBox.delegateModel : null
                    interactive: false
                }
            }
            delegate: ItemDelegate {
                required property var modelData
                required property int index
                width: engineBox.width - 8; height: 30
                contentItem: Text { text: modelData.label; font: engineBox.font; color: Theme.fg; verticalAlignment: Text.AlignVCenter; leftPadding: 4 }
                background: Rectangle { radius: 8; color: hovered || engineBox.currentIndex === index ? Theme.hov : "transparent" }
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
            glyph: backend.answering && ta.text.trim().length === 0 && !backend.attachedImage ? "stop" : "up"
            iconSize: 18
            iconColor: Theme.acfg
            hoverIconColor: Theme.acfg
            fillColor: Theme.ac
            hoverFill: Qt.lighter(Theme.ac, 1.1)
            enabled: backend.attachedImage.length > 0 ? !backend.imageBusy : (backend.answering || (backend.ready && ta.text.trim().length > 0))
            opacity: enabled ? 1 : 0.35
            tip: backend.attachedImage.length > 0 ? "Gerar a imagem" : backend.answering ? (ta.text.trim().length ? "Enfileirar (envia quando o agente terminar)" : "Parar (Esc)") : "Enviar"
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
        // pasta sem confianca: aviso aqui (a escolha da pasta fica nas configuracoes)
        Text {
            objectName: "workspaceWarn"
            visible: !backend.workspaceTrusted
            Layout.leftMargin: 6
            font.family: Theme.sans; font.pixelSize: 12; color: Theme.err
            text: "pasta sem confiança"
        }
        // modo plano
        AbstractButton {
            id: planBtn
            objectName: "planButton"
            implicitHeight: 22
            implicitWidth: planTxt.implicitWidth + 14
            enabled: backend.ready
            hoverEnabled: true
            background: Rectangle { radius: 6; color: backend.planMode ? Theme.hov : planBtn.hovered ? Theme.hov : "transparent" }
            contentItem: Text { id: planTxt; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignHCenter; font.family: Theme.sans; font.pixelSize: 12; color: backend.planMode ? Theme.fg : Theme.mut
                text: backend.planMode ? "plano: ligado" : "plano" }
            ToolTip.visible: hovered
            ToolTip.delay: 400
            ToolTip.text: "No modo plano o agente só lê e propõe um plano; nada é alterado até você aprovar."
            Accessible.name: "Modo plano"
            onClicked: backend.togglePlan()
        }
        Text { font.family: Theme.sans;
            Layout.fillWidth: true
            Layout.leftMargin: 8
            elide: Text.ElideRight
            font.pixelSize: 12
            objectName: "statusLine"
            color: backend.errorText || backend.stalled ? Theme.err : Theme.mut
            text: backend.imageBusy ? "Gerando a imagem (" + (backend.imageEngine === "flux" ? "FLUX" : "Stable") + ")..."
                  : backend.errorText ? backend.errorText
                  : backend.stalled ? backend.activityText
                  : backend.statusText && !/^(Preparando|Retomando|Carregando)/.test(backend.statusText) ? backend.statusText
                  : backend.progressText ? backend.progressText
                  : backend.activityText ? backend.activityText
                  : ta.text.indexOf("\n") >= 0 ? "As quebras de linha serão enviadas como espaço."
                  : ""
            Accessible.role: Accessible.StaticText
        }
        // estado do indice de codigo da pasta (so aparece enquanto o agente trabalha e o motor sabe dizer)
        Text { font.family: Theme.sans;
            objectName: "indexStatus"
            Layout.rightMargin: 8
            font.pixelSize: 12
            color: Theme.mut
            opacity: 0.85
            visible: text.length > 0
            text: backend.indexText
            HoverHandler { id: idxHover }
            ToolTip.visible: idxHover.hovered && backend.indexTip.length > 0
            ToolTip.delay: 400
            ToolTip.text: backend.indexTip
            Accessible.role: Accessible.StaticText
            Accessible.name: backend.indexText
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
    // uso da maquina: processador, memoria e placa de video
    Text {
        id: sys
        objectName: "sysStats"
        visible: text.length > 0
        x: box.x
        y: meta.y + meta.height + 2
        width: box.width
        height: 16
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        font.family: Theme.sans
        font.pixelSize: 11
        color: Theme.mut
        text: backend.sysText
    }
}
