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

    FileDialog {
        id: briefDialog
        title: "Anexar briefing de renderizacao"
        nameFilters: ["Briefing (*.md *.txt)"]
        onAccepted: backend.attachBrief(selectedFile.toString())
    }
    FileDialog {
        id: logoDialog
        title: "Anexar logo (PNG com transparencia)"
        nameFilters: ["Imagens (*.png *.webp *.jpg *.jpeg *.bmp)"]
        onAccepted: backend.attachLogo(selectedFile.toString())
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
        height: Math.max(116, fl.height + 24 + 38)
        radius: 22
        color: Theme.card
        border.color: ta.activeFocus ? Theme.mut : Theme.bd

        Flickable {
            id: fl
            objectName: "composerFlick"
            x: 20
            y: 12
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
                    if (e.key === Qt.Key_Escape && backend.answering) { e.accepted = true; backend.stop(); return; }
                    if ((e.key === Qt.Key_Return || e.key === Qt.Key_Enter) && !(e.modifiers & Qt.ShiftModifier)) {
                        e.accepted = true;
                        comp.submit();
                    }
                }
            }
        }

        // anexar imagem (clipe) e miniatura da imagem anexada, com botao de remover
        IconButton {
            id: attachBtn
            objectName: "attachButton"
            anchors.left: parent.left
            anchors.leftMargin: 8
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 8
            implicitWidth: 34
            implicitHeight: 34
            glyph: "image"
            iconSize: 18
            tip: "Anexar uma imagem (gera uma imagem a partir dela)"
            onClicked: imgDialog.open()
        }
        Rectangle {
            id: thumb
            objectName: "attachThumb"
            visible: backend.attachedImage.length > 0
            anchors.left: attachBtn.right
            anchors.leftMargin: 6
            anchors.verticalCenter: attachBtn.verticalCenter
            width: 56; height: 32; radius: 6
            color: Theme.hov
            border.color: Theme.bd
            clip: true
            Image {
                anchors.fill: parent
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                source: backend.attachedImage ? "file://" + backend.attachedImage : ""
                sourceSize.width: 112
            }
            Rectangle {
                objectName: "attachRemove"
                anchors.right: parent.right; anchors.top: parent.top
                width: 16; height: 16; radius: 8
                color: Theme.card; opacity: 0.9
                Text { anchors.centerIn: parent; text: "✕"; font.pixelSize: 9; color: Theme.fg }
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: backend.clearImage() }
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
            iconSize: backend.answering ? 18 : 18
            iconColor: Theme.acfg
            hoverIconColor: Theme.acfg
            fillColor: Theme.ac
            hoverFill: Qt.lighter(Theme.ac, 1.1)
            enabled: backend.attachedImage.length > 0 ? !backend.imageBusy : (backend.answering || (backend.ready && ta.text.trim().length > 0))
            opacity: enabled ? 1 : 0.35
            tip: backend.answering ? (ta.text.trim().length ? "Enfileirar (envia quando o agente terminar)" : "Parar (Esc)") : "Enviar"
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
        // modo imagem e motor de geracao: Stable (padrao), FLUX e Krea; os dois ultimos e o modo ainda sao WIP
        AbstractButton {
            id: imgModeBtn
            objectName: "imageModeButton"
            implicitHeight: 22
            implicitWidth: imgModeTxt.implicitWidth + 14
            hoverEnabled: true
            background: Rectangle { radius: 6; color: backend.imageMode ? Theme.hov : imgModeBtn.hovered ? Theme.hov : "transparent" }
            contentItem: Text { id: imgModeTxt; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignHCenter; font.family: Theme.sans; font.pixelSize: 12
                color: backend.imageMode ? Theme.fg : Theme.mut; text: "imagem · WIP" }
            ToolTip.visible: hovered
            ToolTip.delay: 400
            ToolTip.text: "Modo imagem (em desenvolvimento): anexe uma imagem (modelo 3D) e escolha o motor que gera a versão realista."
            Accessible.name: "Modo imagem"
            onClicked: backend.imageMode = !backend.imageMode
        }
        Repeater {
            model: [["stable", "Stable", ""], ["flux", "FLUX", ""], ["krea", "Krea", " WIP"]]
            AbstractButton {
                required property var modelData
                objectName: "engine_" + modelData[0]
                visible: backend.imageMode || backend.attachedImage.length > 0
                implicitHeight: 22
                implicitWidth: engTxt.implicitWidth + 14
                hoverEnabled: true
                readonly property bool sel: backend.imageEngine === modelData[0]
                background: Rectangle { radius: 6; color: sel ? Theme.hov : parent.hovered ? Theme.hov : "transparent"; border.width: sel ? 1 : 0; border.color: Theme.bd }
                contentItem: Text { id: engTxt; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignHCenter; font.family: Theme.sans; font.pixelSize: 12
                    color: sel ? Theme.fg : Theme.mut; text: modelData[1] + modelData[2] }
                Accessible.name: "Motor de imagem " + modelData[1]
                onClicked: backend.imageEngine = modelData[0]
            }
        }
        // processamento do FLUX: Auto (o katana escolhe o mais rapido verificado: CUDA, Vulkan, CPU), CUDA, Vulkan ou CPU
        Repeater {
            model: [["auto", "Auto", "O katana escolhe: CUDA se houver placa NVIDIA, senão Vulkan, senão CPU."], ["cuda", "CUDA", "Força o CUDA (só placa NVIDIA)."],
                    ["vulkan", "Vulkan", "Força o Vulkan (NVIDIA, AMD ou Intel)."], ["cpu", "CPU", "Força a CPU: mais lento, não usa a placa."]]
            AbstractButton {
                required property var modelData
                objectName: "backend_" + modelData[0]
                visible: (backend.imageMode || backend.attachedImage.length > 0) && backend.imageEngine === "flux"
                implicitHeight: 22
                implicitWidth: beTxt.implicitWidth + 14
                hoverEnabled: true
                readonly property bool sel: backend.imageBackend === modelData[0]
                background: Rectangle { radius: 6; color: sel ? Theme.hov : parent.hovered ? Theme.hov : "transparent"; border.width: sel ? 1 : 0; border.color: Theme.bd }
                contentItem: Text { id: beTxt; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignHCenter; font.family: Theme.sans; font.pixelSize: 12
                    color: sel ? Theme.fg : Theme.mut; text: modelData[1] }
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: modelData[2]
                Accessible.name: "Processamento " + modelData[1]
                onClicked: backend.imageBackend = modelData[0]
            }
        }
        // briefing (.md/.txt) e logo opcionais do modo imagem: o briefing vira o prompt (materiais, cores, luz, regras de fidelidade);
        // o logo (PNG com alfa) e composto no render depois de gerado, dentro da caixa x,y,w,h (fracao da imagem ou pixels)
        AbstractButton {
            id: briefBtn
            objectName: "briefButton"
            visible: backend.imageMode || backend.attachedImage.length > 0
            implicitHeight: 22
            implicitWidth: briefTxt.implicitWidth + 14
            hoverEnabled: true
            background: Rectangle { radius: 6; color: backend.attachedBrief.length > 0 || briefBtn.hovered ? Theme.hov : "transparent" }
            contentItem: Text { id: briefTxt; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignHCenter; font.family: Theme.sans; font.pixelSize: 12
                color: backend.attachedBrief.length > 0 ? Theme.fg : Theme.mut
                text: backend.attachedBrief.length > 0 ? "briefing ✓" : "briefing" }
            ToolTip.visible: hovered
            ToolTip.delay: 400
            ToolTip.text: backend.attachedBrief.length > 0 ? "Briefing: " + backend.attachedBrief : "Anexar o briefing de renderizacao (.md ou .txt): materiais, cores, luz e regras viram o prompt, sem resumo a mao."
            Accessible.name: "Anexar briefing"
            onClicked: briefDialog.open()
        }
        Text {
            objectName: "briefRemove"
            visible: backend.attachedBrief.length > 0
            font.family: Theme.sans; font.pixelSize: 11; color: Theme.mut; text: "✕"
            Accessible.name: "Remover briefing"
            MouseArea { anchors.fill: parent; anchors.margins: -5; cursorShape: Qt.PointingHandCursor; onClicked: backend.clearBrief() }
        }
        AbstractButton {
            id: logoBtn
            objectName: "logoButton"
            visible: backend.imageMode || backend.attachedImage.length > 0
            implicitHeight: 22
            implicitWidth: logoTxt.implicitWidth + 14
            hoverEnabled: true
            background: Rectangle { radius: 6; color: backend.attachedLogo.length > 0 || logoBtn.hovered ? Theme.hov : "transparent" }
            contentItem: Text { id: logoTxt; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignHCenter; font.family: Theme.sans; font.pixelSize: 12
                color: backend.attachedLogo.length > 0 ? Theme.fg : Theme.mut
                text: backend.attachedLogo.length > 0 ? "logo ✓" : "logo" }
            ToolTip.visible: hovered
            ToolTip.delay: 400
            ToolTip.text: backend.attachedLogo.length > 0 ? "Logo: " + backend.attachedLogo : "Anexar o logo real (PNG com transparencia): o modelo de imagem nao escreve marcas, entao o logo e composto na imagem pronta."
            Accessible.name: "Anexar logo"
            onClicked: logoDialog.open()
        }
        TextField {
            objectName: "logoBoxField"
            visible: backend.attachedLogo.length > 0
            implicitWidth: 118; implicitHeight: 22; leftPadding: 6; rightPadding: 6; topPadding: 0; bottomPadding: 0
            font.family: Theme.sans; font.pixelSize: 11
            color: Theme.fg
            text: backend.logoBox
            placeholderText: "x,y,w,h"
            background: Rectangle { radius: 6; color: Theme.hov; border.color: Theme.bd }
            ToolTip.visible: hovered
            ToolTip.delay: 400
            ToolTip.text: "Caixa do logo na imagem: x,y,w,h em fracao da imagem (0.04,0.04,0.22,0.12) ou em pixels."
            Accessible.name: "Caixa do logo"
            onEditingFinished: backend.logoBox = text
        }
        Text {
            objectName: "logoRemove"
            visible: backend.attachedLogo.length > 0
            font.family: Theme.sans; font.pixelSize: 11; color: Theme.mut; text: "✕"
            Accessible.name: "Remover logo"
            MouseArea { anchors.fill: parent; anchors.margins: -5; cursorShape: Qt.PointingHandCursor; onClicked: backend.clearLogo() }
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
        AbstractButton {
            id: tchBtn
            objectName: "teacherButton"
            implicitHeight: 22
            implicitWidth: tchTxt.implicitWidth + 14
            enabled: backend.ready
            hoverEnabled: true
            background: Rectangle { radius: 6; color: backend.teacher ? Theme.hov : tchBtn.hovered ? Theme.hov : "transparent" }
            contentItem: Text { id: tchTxt; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignHCenter; font.family: Theme.sans; font.pixelSize: 12; color: backend.teacher ? Theme.fg : Theme.mut
                text: backend.teacher ? "professor: " + backend.teacherModel : "professor" }
            ToolTip.visible: hovered
            ToolTip.delay: 400
            ToolTip.text: "Com o professor ligado, um modelo maior (pela API) responde no lugar do modelo local, com as mesmas ferramentas.\nA conversa sai desta máquina; as respostas ficam guardadas para treinar o modelo local."
            Accessible.name: "Modo professor"
            onClicked: backend.toggleTeacher()
        }
        Text { font.family: Theme.sans;
            Layout.fillWidth: true
            Layout.leftMargin: 8
            elide: Text.ElideRight
            font.pixelSize: 12
            objectName: "statusLine"
            color: backend.errorText || backend.stalled ? Theme.err : Theme.mut
            text: backend.imageBusy ? "Gerando a imagem (" + backend.imageEngine + ")..."
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
