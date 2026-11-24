import QtQuick
import Katana
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ApplicationWindow {
    id: win
    objectName: "mainWindow"
    width: 1180
    height: 820
    minimumWidth: 700
    minimumHeight: 500
    visible: true
    title: "Katana"
    color: Theme.bg
    flags: Qt.Window | Qt.FramelessWindowHint
    font.family: Theme.sans
    readonly property bool maxed: visibility === Window.Maximized || visibility === Window.FullScreen

    readonly property bool narrow: width < 760
    property bool sideOpen: !backend.uiFlag("lateralRecolhida")
    // altura dos cartoes acima da caixa de mensagem (modo plano, pergunta): a conversa termina antes deles
    readonly property real cardsHeight: (workCard.visible ? workCard.height + 6 : 0) + (choiceCard.visible ? choiceCard.height + 6 : 0) + composer.queueHeight
    property bool drawerOpen: false

    Component.onCompleted: { Theme.force = backend.themeOverride; Theme.fonte = backend.fonte(); Theme.tema = backend.tema() }

    function applyTheme(t) { Theme.tema = t; }     // so para o selftest: troca o tema sem abrir o popup

    property bool termOpen: false
    function toggleTerminal() {
        termOpen = !termOpen;
        if (termOpen) { if (!terminal.running) terminal.start(); Qt.callLater(termPanel.focusTerm); }
        else composer.input.forceActiveFocus();
    }
    Shortcut { sequence: "Ctrl+`"; onActivated: win.toggleTerminal() }

    function toggleSide() {
        if (narrow) drawerOpen = !drawerOpen;
        else { sideOpen = !sideOpen; backend.setUiFlag("lateralRecolhida", !sideOpen); }
    }
    onNarrowChanged: drawerOpen = false
    Shortcut { sequence: "Escape"; onActivated: { if (win.drawerOpen) win.drawerOpen = false; else if (backend.answering) backend.stop(); } }

    Connections {
        target: backend
        function onPasswordRequested(retry) { pwDlg.retry = retry; pwField.text = ""; pwDlg.open(); pwField.forceActiveFocus(); }
        function onAskRequested(kind, what, detail) { askDlg.kind = kind; askDlg.what = what; askDlg.detail = detail; askDlg.open(); }
        function onAskClosed() { askDlg.answered = true; askDlg.close(); }
    }
    // pedido de permissao do agente: confiar na pasta de trabalho, gravar fora dela, ou uma acao (quando a pasta nao e confiavel)
    Popup {
        id: askDlg
        objectName: "askDialog"
        property string kind: ""
        property string what: ""
        property string detail: ""
        modal: true
        closePolicy: Popup.NoAutoClose
        anchors.centerIn: Overlay.overlay
        width: Math.min(480, win.width - 40)
        padding: 18
        property bool answered: false
        onOpened: answered = false
        onClosed: { if (!answered) backend.answerAsk(false); }
        function answer(ok) { answered = true; backend.answerAsk(ok); close(); }
        background: Rectangle { radius: 14; color: Theme.card; border.color: Theme.bd }
        contentItem: Column {
            spacing: 10
            Text { font.family: Theme.sans; color: Theme.fg; font.pixelSize: 16; font.bold: true; width: parent.width; wrapMode: Text.Wrap
                text: askDlg.kind === "trust" ? "Confiar nesta pasta?"
                    : askDlg.kind === "write" ? "Gravar fora da pasta de trabalho?" : "Permitir esta ação?" }
            Text { font.family: Theme.sans; width: parent.width; wrapMode: Text.Wrap; color: Theme.mut; font.pixelSize: 13
                text: askDlg.kind === "trust"
                      ? (askDlg.what.indexOf("very broad") >= 0
                         ? "Atenção: esta pasta é muito ampla (a sua home ou a raiz). O Katana poderá ler e alterar tudo dentro dela, e executar comandos."
                         : "O Katana poderá ler e gravar arquivos nesta pasta e executar comandos. Sem confiança, cada ação pergunta antes.")
                      : askDlg.kind === "write" ? "O agente quer gravar numa pasta fora da pasta de trabalho. Permitir vale para esta pasta, nesta sessão."
                      : askDlg.what }
            Rectangle {
                width: parent.width; height: pathTxt.implicitHeight + 16; radius: 8
                color: Theme.dark ? "#0E0B12" : "#F1EEF5"; border.color: Theme.bd
                TextEdit { id: pathTxt; x: 8; y: 8; width: parent.width - 16; text: askDlg.detail; readOnly: true; selectByMouse: true
                    wrapMode: TextEdit.WrapAnywhere; color: Theme.fg; font.family: Theme.mono; font.pixelSize: 12 }
            }
            Row {
                spacing: 8
                anchors.right: parent.right
                Button { text: askDlg.kind === "trust" ? "Não confiar" : "Negar"; onClicked: askDlg.answer(false) }
                Button { text: askDlg.kind === "trust" ? "Confiar na pasta" : "Permitir"; highlighted: true; onClicked: askDlg.answer(true) }
            }
        }
    }
    Popup {
        id: pwDlg
        objectName: "passwordDialog"
        modal: true
        closePolicy: Popup.NoAutoClose
        anchors.centerIn: Overlay.overlay
        width: Math.min(420, win.width - 40)
        padding: 18
        property bool answered: false
        property bool retry: false
        onOpened: answered = false
        onClosed: { if (!answered) backend.sendPassword(""); }
        function answer(ok) { answered = true; backend.sendPassword(ok ? pwField.text : ""); pwField.text = ""; close(); }
        background: Rectangle { radius: 14; color: Theme.card; border.color: Theme.bd }
        contentItem: Column {
            spacing: 10
            Text { font.family: Theme.sans; text: "Senha do sudo"; color: Theme.fg; font.pixelSize: 16; font.bold: true }
            Text { font.family: Theme.sans; visible: pwDlg.retry; width: parent.width; wrapMode: Text.Wrap; color: Theme.err; font.pixelSize: 13; text: "Senha incorreta. Tente de novo." }
            Text { font.family: Theme.sans; width: parent.width; wrapMode: Text.Wrap; color: Theme.mut; font.pixelSize: 13
                text: "O agente quer executar um comando com sudo. A senha fica só na memória desta sessão e não vai para o modelo." }
            TextField {
                id: pwField
                width: parent.width
                echoMode: TextInput.Password
                placeholderText: "Senha"
                color: Theme.fg
                Accessible.name: "Senha do sudo"
                onAccepted: pwDlg.answer(true)
            }
            Row {
                spacing: 8
                anchors.right: parent.right
                Button { text: "Cancelar"; onClicked: pwDlg.answer(false) }
                Button { text: "Autorizar"; highlighted: true; onClicked: pwDlg.answer(true) }
            }
        }
    }

    Item {
        anchors.fill: parent

        Sidebar {
            id: side
            objectName: "sidebar"
            width: Theme.sideWidth
            height: parent.height
            z: 30
            x: (win.narrow ? win.drawerOpen : win.sideOpen) ? 0 : -width
            visible: x > -width
            Behavior on x { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
            onCollapseRequested: { if (win.narrow) win.drawerOpen = false; else win.toggleSide(); }
            onChosen: win.drawerOpen = false
        }
        Rectangle {
            visible: win.narrow && win.drawerOpen
            anchors.fill: parent
            z: 25
            color: "#66000000"
            MouseArea { anchors.fill: parent; onClicked: win.drawerOpen = false }
        }

        Item {
            id: mainArea
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            anchors.left: parent.left
            anchors.leftMargin: win.narrow ? 0 : side.x + side.width

            Item {
                id: top
                height: 48
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                z: 5
                // arrastar a janela / duplo clique maximiza
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    onPressed: win.startSystemMove()
                    onDoubleClicked: win.maxed ? win.showNormal() : win.showMaximized()
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 8
                    spacing: 2
                    IconButton {
                        objectName: "sidebarToggle"
                        glyph: "menu"
                        tip: "Mostrar ou ocultar a lateral"
                        onClicked: win.toggleSide()
                    }
                    Item { Layout.fillWidth: true }
                    IconButton {
                        objectName: "storeButton"
                        glyph: "download"
                        tip: "Modelos: baixar e trocar"
                        onClicked: { backend.refreshCatalog(); store.open = !store.open; }
                    }
                    IconButton {
                        objectName: "terminalButton"
                        glyph: "terminal"
                        tip: "Terminal (Ctrl+`)"
                        onClicked: win.toggleTerminal()
                    }
                    IconButton {
                        id: gearBtn
                        objectName: "appearanceButton"
                        glyph: "gear"
                        tip: "Configurações"
                        onClicked: apMenu.opened ? apMenu.close() : apMenu.open()
                        Popup {
                            id: apMenu
                            x: gearBtn.width - width
                            y: gearBtn.height + 6
                            width: 230
                            padding: 8
                            background: Rectangle { radius: 12; color: Theme.card; border.color: Theme.bd }
                            height: Math.min(apCol.implicitHeight + 16, (Overlay.overlay ? Overlay.overlay.height : 700) - y - 12)
                            contentItem: Flickable {
                                id: apFlick; objectName: "apFlick"
                                clip: true; contentWidth: width; contentHeight: apCol.implicitHeight
                                boundsBehavior: Flickable.StopAtBounds
                                ScrollBar.vertical: ScrollBar { policy: apFlick.contentHeight > apFlick.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }
                                Column {
                                id: apCol
                                property string sec: ""
                                function lbl(m, c) { for (var i = 0; i < m.length; i++) if (m[i][0] === c) return m[i][1]; return "" }
                                    spacing: 2
                                    AbstractButton {
                                        objectName: "sec_tema"; width: apMenu.width - 16; height: 30; hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                        Accessible.name: "Tema: " + apCol.lbl([["escuro", "Escuro (padrão)"], ["claro", "Claro"], ["ide", "IDE escuro (código)"]], Theme.tema); Accessible.role: Accessible.Button
                                        onClicked: apCol.sec = apCol.sec === "tema" ? "" : "tema"
                                        background: Rectangle { radius: 8; color: parent.hovered ? Theme.hov : "transparent"; border.width: parent.visualFocus ? 2 : 0; border.color: Theme.foc }
                                        contentItem: Item {
                                            Text { anchors.left: parent.left; anchors.leftMargin: 8; anchors.verticalCenter: parent.verticalCenter; font.family: Theme.sans; text: "TEMA"; color: Theme.mut; font.pixelSize: 11; font.letterSpacing: 0.6 }
                                            Text { anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter; font.family: Theme.sans; text: apCol.lbl([["escuro", "Escuro (padrão)"], ["claro", "Claro"], ["ide", "IDE escuro (código)"]], Theme.tema) + (apCol.sec === "tema" ? "  ▾" : "  ▸"); color: Theme.fg; font.pixelSize: 12; elide: Text.ElideRight; horizontalAlignment: Text.AlignRight; width: Math.max(40, parent.width - 16 - parent.children[0].implicitWidth - 12) }
                                        }
                                    }
                                    Repeater {
                                        model: [["escuro", "Escuro (padrão)"], ["claro", "Claro"], ["ide", "IDE escuro (código)"]]
                                        AbstractButton {
                                            id: to
                                            required property var modelData
                                            visible: apCol.sec === "tema"
                                            objectName: "tema_" + modelData[0]
                                            width: apMenu.width - 16; height: 32
                                            hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                            Accessible.name: modelData[1]; Accessible.role: Accessible.RadioButton
                                            onClicked: { backend.setTema(modelData[0]); Theme.tema = modelData[0]; }
                                            background: Rectangle { radius: 8; color: to.hovered ? Theme.hov : "transparent"
                                                border.width: to.visualFocus ? 2 : 0; border.color: Theme.foc }
                                            contentItem: Text { font.family: Theme.sans; leftPadding: 8; verticalAlignment: Text.AlignVCenter
                                                text: (Theme.tema === to.modelData[0] ? "●  " : "○  ") + to.modelData[1]; color: Theme.fg; font.pixelSize: 13 }
                                        }
                                    }
                                    Text { font.family: Theme.sans; text: "TELA"; color: Theme.mut; font.pixelSize: 11; leftPadding: 8; topPadding: 8; bottomPadding: 4; font.letterSpacing: 0.6 }
                                    AbstractButton {
                                        id: awk
                                        objectName: "keepAwake"
                                        property bool on: backend.keepAwake()
                                        width: apMenu.width - 16; height: 32
                                        hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                        Accessible.name: "Não bloquear a tela enquanto o agente trabalha"; Accessible.role: Accessible.CheckBox
                                        onClicked: { on = !on; backend.setKeepAwake(on); }
                                        background: Rectangle { radius: 8; color: awk.hovered ? Theme.hov : "transparent"
                                            border.width: awk.visualFocus ? 2 : 0; border.color: Theme.foc }
                                        contentItem: Text { font.family: Theme.sans; leftPadding: 8; verticalAlignment: Text.AlignVCenter
                                            text: (awk.on ? "☑  " : "☐  ") + "Não bloquear enquanto trabalha"; color: Theme.fg; font.pixelSize: 13 }
                                    }
                                    AbstractButton {
                                        objectName: "sec_proc"; width: apMenu.width - 16; height: 30; hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                        Accessible.name: "Processamento da imagem: " + apCol.lbl([["auto", "Auto"], ["cuda", "CUDA"], ["vulkan", "Vulkan"], ["cpu", "CPU"]], backend.imageBackend); Accessible.role: Accessible.Button
                                        onClicked: apCol.sec = apCol.sec === "proc" ? "" : "proc"
                                        background: Rectangle { radius: 8; color: parent.hovered ? Theme.hov : "transparent"; border.width: parent.visualFocus ? 2 : 0; border.color: Theme.foc }
                                        contentItem: Item {
                                            Text { anchors.left: parent.left; anchors.leftMargin: 8; anchors.verticalCenter: parent.verticalCenter; font.family: Theme.sans; text: "PROCESSAMENTO"; color: Theme.mut; font.pixelSize: 11; font.letterSpacing: 0.6 }
                                            Text { anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter; font.family: Theme.sans; text: apCol.lbl([["auto", "Auto"], ["cuda", "CUDA"], ["vulkan", "Vulkan"], ["cpu", "CPU"]], backend.imageBackend) + (apCol.sec === "proc" ? "  ▾" : "  ▸"); color: Theme.fg; font.pixelSize: 12; elide: Text.ElideRight; horizontalAlignment: Text.AlignRight; width: Math.max(40, parent.width - 16 - parent.children[0].implicitWidth - 12) }
                                        }
                                    }
                                    Repeater {
                                        model: [["auto", "Auto"], ["cuda", "CUDA"], ["vulkan", "Vulkan"], ["cpu", "CPU"]]
                                        AbstractButton {
                                            id: ib
                                            required property var modelData
                                            visible: apCol.sec === "proc"
                                            objectName: "backend_" + modelData[0]
                                            width: apMenu.width - 16; height: 32
                                            hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                            Accessible.name: "Processamento da imagem: " + modelData[1]; Accessible.role: Accessible.RadioButton
                                            onClicked: backend.imageBackend = modelData[0]
                                            background: Rectangle { radius: 8; color: ib.hovered ? Theme.hov : "transparent"
                                                border.width: ib.visualFocus ? 2 : 0; border.color: Theme.foc }
                                            contentItem: Text { font.family: Theme.sans; leftPadding: 8; verticalAlignment: Text.AlignVCenter
                                                text: (backend.imageBackend === ib.modelData[0] ? "●  " : "○  ") + ib.modelData[1]; color: Theme.fg; font.pixelSize: 13 }
                                        }
                                    }
                                    AbstractButton {
                                        objectName: "sec_tam"; width: apMenu.width - 16; height: 30; hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                        Accessible.name: "Tamanho da imagem: " + apCol.lbl([[1024, "1024 × 576 (menor)"], [1280, "1280 × 720"], [1600, "1600 × 900"], [1920, "1920 × 1080 (muito lento)"]], backend.imageSize); Accessible.role: Accessible.Button
                                        onClicked: apCol.sec = apCol.sec === "tam" ? "" : "tam"
                                        background: Rectangle { radius: 8; color: parent.hovered ? Theme.hov : "transparent"; border.width: parent.visualFocus ? 2 : 0; border.color: Theme.foc }
                                        contentItem: Item {
                                            Text { anchors.left: parent.left; anchors.leftMargin: 8; anchors.verticalCenter: parent.verticalCenter; font.family: Theme.sans; text: "TAMANHO"; color: Theme.mut; font.pixelSize: 11; font.letterSpacing: 0.6 }
                                            Text { anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter; font.family: Theme.sans; text: apCol.lbl([[1024, "1024 × 576 (menor)"], [1280, "1280 × 720"], [1600, "1600 × 900"], [1920, "1920 × 1080 (muito lento)"]], backend.imageSize) + (apCol.sec === "tam" ? "  ▾" : "  ▸"); color: Theme.fg; font.pixelSize: 12; elide: Text.ElideRight; horizontalAlignment: Text.AlignRight; width: Math.max(40, parent.width - 16 - parent.children[0].implicitWidth - 12) }
                                        }
                                    }
                                    Repeater {
                                        model: [[1024, "1024 × 576 (menor)"], [1280, "1280 × 720"], [1600, "1600 × 900"], [1920, "1920 × 1080 (muito lento)"]]
                                        AbstractButton {
                                            id: zb
                                            required property var modelData
                                            visible: apCol.sec === "tam"
                                            objectName: "size_" + modelData[0]
                                            width: apMenu.width - 16; height: 32
                                            hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                            Accessible.name: "Tamanho da imagem: " + modelData[1]; Accessible.role: Accessible.RadioButton
                                            onClicked: backend.imageSize = modelData[0]
                                            background: Rectangle { radius: 8; color: zb.hovered ? Theme.hov : "transparent"
                                                border.width: zb.visualFocus ? 2 : 0; border.color: Theme.foc }
                                            contentItem: Text { font.family: Theme.sans; leftPadding: 8; verticalAlignment: Text.AlignVCenter
                                                text: (backend.imageSize === zb.modelData[0] ? "●  " : "○  ") + zb.modelData[1]; color: Theme.fg; font.pixelSize: 13 }
                                        }
                                    }
                                    AbstractButton {
                                        objectName: "sec_fluxp"; width: apMenu.width - 16; height: 30; hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                        Accessible.name: "Passos do flux: " + apCol.lbl([[1, "1"], [2, "2"], [3, "3"], [4, "4 (padrão do modelo)"], [5, "5"], [6, "6"], [7, "7"], [8, "8"]], backend.imageSteps); Accessible.role: Accessible.Button
                                        onClicked: apCol.sec = apCol.sec === "fluxp" ? "" : "fluxp"
                                        background: Rectangle { radius: 8; color: parent.hovered ? Theme.hov : "transparent"; border.width: parent.visualFocus ? 2 : 0; border.color: Theme.foc }
                                        contentItem: Item {
                                            Text { anchors.left: parent.left; anchors.leftMargin: 8; anchors.verticalCenter: parent.verticalCenter; font.family: Theme.sans; text: "PASSOS FLUX"; color: Theme.mut; font.pixelSize: 11; font.letterSpacing: 0.6 }
                                            Text { anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter; font.family: Theme.sans; text: apCol.lbl([[1, "1"], [2, "2"], [3, "3"], [4, "4 (padrão do modelo)"], [5, "5"], [6, "6"], [7, "7"], [8, "8"]], backend.imageSteps) + (apCol.sec === "fluxp" ? "  ▾" : "  ▸"); color: Theme.fg; font.pixelSize: 12; elide: Text.ElideRight; horizontalAlignment: Text.AlignRight; width: Math.max(40, parent.width - 16 - parent.children[0].implicitWidth - 12) }
                                        }
                                    }
                                    Repeater {
                                        model: [[1, "1"], [2, "2"], [3, "3"], [4, "4 (padrão do modelo)"], [5, "5"], [6, "6"], [7, "7"], [8, "8"]]
                                        AbstractButton {
                                            id: sb
                                            required property var modelData
                                            visible: apCol.sec === "fluxp"
                                            objectName: "steps_" + modelData[0]
                                            width: apMenu.width - 16; height: 32
                                            hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                            Accessible.name: "Passos do FLUX: " + modelData[1]; Accessible.role: Accessible.RadioButton
                                            onClicked: backend.imageSteps = modelData[0]
                                            background: Rectangle { radius: 8; color: sb.hovered ? Theme.hov : "transparent"
                                                border.width: sb.visualFocus ? 2 : 0; border.color: Theme.foc }
                                            contentItem: Text { font.family: Theme.sans; leftPadding: 8; verticalAlignment: Text.AlignVCenter
                                                text: (backend.imageSteps === sb.modelData[0] ? "●  " : "○  ") + sb.modelData[1]; color: Theme.fg; font.pixelSize: 13 }
                                        }
                                    }
                                    AbstractButton {
                                        objectName: "sec_smodel"; width: apMenu.width - 16; height: 30; hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                        Accessible.name: "Modelo do stable: " + apCol.lbl([["realistic-vision", "Realistic Vision (geral)"], ["interior", "InteriorDesign (interiores)"], ["xsachi", "XSAchi (interiores, uso não comercial)"]], backend.stableModel); Accessible.role: Accessible.Button
                                        onClicked: apCol.sec = apCol.sec === "smodel" ? "" : "smodel"
                                        background: Rectangle { radius: 8; color: parent.hovered ? Theme.hov : "transparent"; border.width: parent.visualFocus ? 2 : 0; border.color: Theme.foc }
                                        contentItem: Item {
                                            Text { anchors.left: parent.left; anchors.leftMargin: 8; anchors.verticalCenter: parent.verticalCenter; font.family: Theme.sans; text: "MODELO STABLE"; color: Theme.mut; font.pixelSize: 11; font.letterSpacing: 0.6 }
                                            Text { anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter; font.family: Theme.sans; text: apCol.lbl([["realistic-vision", "Realistic Vision (geral)"], ["interior", "InteriorDesign (interiores)"], ["xsachi", "XSAchi (interiores, uso não comercial)"]], backend.stableModel) + (apCol.sec === "smodel" ? "  ▾" : "  ▸"); color: Theme.fg; font.pixelSize: 12; elide: Text.ElideRight; horizontalAlignment: Text.AlignRight; width: Math.max(40, parent.width - 16 - parent.children[0].implicitWidth - 12) }
                                        }
                                    }
                                    Repeater {
                                        model: [["realistic-vision", "Realistic Vision (geral)"], ["interior", "InteriorDesign (interiores)"], ["xsachi", "XSAchi (interiores, uso não comercial)"]]
                                        AbstractButton {
                                            id: smd
                                            required property var modelData
                                            visible: apCol.sec === "smodel"
                                            objectName: "smodel_" + modelData[0]
                                            width: apMenu.width - 16; height: 32
                                            hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                            Accessible.name: "Modelo do Stable: " + modelData[1]; Accessible.role: Accessible.RadioButton
                                            onClicked: backend.stableModel = modelData[0]
                                            background: Rectangle { radius: 8; color: smd.hovered ? Theme.hov : "transparent"
                                                border.width: smd.visualFocus ? 2 : 0; border.color: Theme.foc }
                                            contentItem: Text { font.family: Theme.sans; leftPadding: 8; verticalAlignment: Text.AlignVCenter
                                                text: (backend.stableModel === smd.modelData[0] ? "●  " : "○  ") + smd.modelData[1]; color: Theme.fg; font.pixelSize: 13 }
                                        }
                                    }
                                    AbstractButton {
                                        objectName: "sec_sstep"; width: apMenu.width - 16; height: 30; hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                        Accessible.name: "Passos do stable: " + apCol.lbl([[4, "4 (rápido, LCM)"], [8, "8 (LCM)"], [20, "20 (realista)"], [40, "40 (mais realista, lento)"]], backend.stableSteps); Accessible.role: Accessible.Button
                                        onClicked: apCol.sec = apCol.sec === "sstep" ? "" : "sstep"
                                        background: Rectangle { radius: 8; color: parent.hovered ? Theme.hov : "transparent"; border.width: parent.visualFocus ? 2 : 0; border.color: Theme.foc }
                                        contentItem: Item {
                                            Text { anchors.left: parent.left; anchors.leftMargin: 8; anchors.verticalCenter: parent.verticalCenter; font.family: Theme.sans; text: "PASSOS STABLE"; color: Theme.mut; font.pixelSize: 11; font.letterSpacing: 0.6 }
                                            Text { anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter; font.family: Theme.sans; text: apCol.lbl([[4, "4 (rápido, LCM)"], [8, "8 (LCM)"], [20, "20 (realista)"], [40, "40 (mais realista, lento)"]], backend.stableSteps) + (apCol.sec === "sstep" ? "  ▾" : "  ▸"); color: Theme.fg; font.pixelSize: 12; elide: Text.ElideRight; horizontalAlignment: Text.AlignRight; width: Math.max(40, parent.width - 16 - parent.children[0].implicitWidth - 12) }
                                        }
                                    }
                                    Repeater {
                                        model: [[4, "4 (rápido, LCM)"], [8, "8 (LCM)"], [20, "20 (realista)"], [40, "40 (mais realista, lento)"]]
                                        AbstractButton {
                                            id: sst
                                            required property var modelData
                                            visible: apCol.sec === "sstep"
                                            objectName: "ssteps_" + modelData[0]
                                            width: apMenu.width - 16; height: 32
                                            hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                            Accessible.name: "Passos do Stable: " + modelData[1]; Accessible.role: Accessible.RadioButton
                                            onClicked: backend.stableSteps = modelData[0]
                                            background: Rectangle { radius: 8; color: sst.hovered ? Theme.hov : "transparent"
                                                border.width: sst.visualFocus ? 2 : 0; border.color: Theme.foc }
                                            contentItem: Text { font.family: Theme.sans; leftPadding: 8; verticalAlignment: Text.AlignVCenter
                                                text: (backend.stableSteps === sst.modelData[0] ? "●  " : "○  ") + sst.modelData[1]; color: Theme.fg; font.pixelSize: 13 }
                                        }
                                    }
                                AbstractButton {
                                    objectName: "sec_sfid"; width: apMenu.width - 16; height: 30; hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                    Accessible.name: "Fidelidade à vista (stable): " + apCol.lbl([[0, "Automática"], [25, "Quase igual à vista (25%)"], [40, "Alta (40%)"], [55, "Média (55%)"], [70, "Baixa (70%)"], [85, "Mínima, recria (85%)"]], backend.stableStrength); Accessible.role: Accessible.Button
                                    onClicked: apCol.sec = apCol.sec === "sfid" ? "" : "sfid"
                                    background: Rectangle { radius: 8; color: parent.hovered ? Theme.hov : "transparent"; border.width: parent.visualFocus ? 2 : 0; border.color: Theme.foc }
                                    contentItem: Item {
                                        Text { anchors.left: parent.left; anchors.leftMargin: 8; anchors.verticalCenter: parent.verticalCenter; font.family: Theme.sans; text: "FIDELIDADE"; color: Theme.mut; font.pixelSize: 11; font.letterSpacing: 0.6 }
                                        Text { anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter; font.family: Theme.sans; text: apCol.lbl([[0, "Automática"], [25, "Quase igual à vista (25%)"], [40, "Alta (40%)"], [55, "Média (55%)"], [70, "Baixa (70%)"], [85, "Mínima, recria (85%)"]], backend.stableStrength) + (apCol.sec === "sfid" ? "  ▾" : "  ▸"); color: Theme.fg; font.pixelSize: 12; elide: Text.ElideRight; horizontalAlignment: Text.AlignRight; width: Math.max(40, parent.width - 16 - parent.children[0].implicitWidth - 12) }
                                    }
                                }
                                Repeater {
                                        model: [[0, "Automática"], [25, "Quase igual à vista (25%)"], [40, "Alta (40%)"], [55, "Média (55%)"], [70, "Baixa (70%)"], [85, "Mínima, recria (85%)"]]
                                        AbstractButton {
                                            id: sstr
                                            required property var modelData
                                            visible: apCol.sec === "sfid"
                                            objectName: "sstr_" + modelData[0]
                                            width: apMenu.width - 16; height: 32
                                            hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                            Accessible.name: "Fidelidade à vista: " + modelData[1]; Accessible.role: Accessible.RadioButton
                                            onClicked: backend.stableStrength = modelData[0]
                                            background: Rectangle { radius: 8; color: sstr.hovered ? Theme.hov : "transparent"
                                                border.width: sstr.visualFocus ? 2 : 0; border.color: Theme.foc }
                                            contentItem: Text { font.family: Theme.sans; leftPadding: 8; verticalAlignment: Text.AlignVCenter
                                                text: (backend.stableStrength === sstr.modelData[0] ? "●  " : "○  ") + sstr.modelData[1]; color: Theme.fg; font.pixelSize: 13 }
                                        }
                                    }
                                    Text { font.family: Theme.sans; text: "ESPECIALIZAÇÕES"; color: Theme.mut; font.pixelSize: 11; leftPadding: 8; topPadding: 8; bottomPadding: 4; font.letterSpacing: 0.6 }
                                    Text { objectName: "specsActive"; font.family: Theme.sans; leftPadding: 8; rightPadding: 8; width: apMenu.width - 16; wrapMode: Text.Wrap; font.pixelSize: 12
                                        color: backend.specsActive.length ? Theme.fg : Theme.mut
                                        text: backend.specsActive.length ? "Em uso neste modelo:\n" + backend.specsActive.join("\n") : "Nenhuma em uso neste modelo." }
                                    Text { objectName: "specsSkipped"; visible: backend.specsSkipped.length > 0; font.family: Theme.sans; leftPadding: 8; rightPadding: 8; width: apMenu.width - 16; wrapMode: Text.Wrap; font.pixelSize: 12
                                        color: Theme.mut
                                        text: "Puladas:\n" + backend.specsSkipped.join("\n") }
                                    AbstractButton {
                                        id: spBtn
                                        objectName: "specsFolder"
                                        width: apMenu.width - 16; height: 32
                                        hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                        Accessible.name: "Abrir a pasta das especializações"
                                        onClicked: backend.openSpecsFolder()
                                        background: Rectangle { radius: 8; color: spBtn.hovered ? Theme.hov : "transparent"
                                            border.width: spBtn.visualFocus ? 2 : 0; border.color: Theme.foc }
                                        contentItem: Text { font.family: Theme.sans; leftPadding: 8; verticalAlignment: Text.AlignVCenter
                                            text: "Abrir a pasta"; color: Theme.fg; font.pixelSize: 13 }
                                        ToolTip.visible: hovered
                                        ToolTip.delay: 400
                                        ToolTip.text: backend.specsDir
                                    }
                                    Text { font.family: Theme.sans; text: "PASTA DE TRABALHO"; color: Theme.mut; font.pixelSize: 11; leftPadding: 8; topPadding: 8; bottomPadding: 4; font.letterSpacing: 0.6 }
                                    // pasta onde o agente trabalha: clique para escolher outra (o agente pergunta se confia nela)
                                    AbstractButton {
                                        id: wsBtn
                                        objectName: "workspaceButton"
                                        width: apMenu.width - 16; height: 32
                                        enabled: backend.ready || !backend.running
                                        hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                        readonly property string folder: { var p = backend.workspace || ""; var i = p.lastIndexOf("/"); return i >= 0 && i < p.length - 1 ? p.slice(i + 1) : p; }
                                        Accessible.name: "Pasta de trabalho"
                                        onClicked: { apMenu.close(); folderDlg.open(); }
                                        background: Rectangle { radius: 8; color: wsBtn.hovered ? Theme.hov : "transparent"
                                            border.width: wsBtn.visualFocus ? 2 : 0; border.color: Theme.foc }
                                        contentItem: Text { font.family: Theme.sans; leftPadding: 8; rightPadding: 8; verticalAlignment: Text.AlignVCenter; elide: Text.ElideMiddle
                                            text: wsBtn.folder + (backend.workspaceTrusted ? "" : "  (sem confiança)")
                                            color: backend.workspaceTrusted ? Theme.fg : Theme.err; font.pixelSize: 13 }
                                        ToolTip.visible: hovered
                                        ToolTip.delay: 400
                                        ToolTip.text: (backend.workspace || "") + "\nClique para trocar a pasta."
                                    }
                                    AbstractButton {
                                        objectName: "sec_fonte"; width: apMenu.width - 16; height: 30; hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                        Accessible.name: "Fonte: " + apCol.lbl([["jetbrains", "JetBrains Mono"], ["plex", "IBM Plex"]], Theme.fonte); Accessible.role: Accessible.Button
                                        onClicked: apCol.sec = apCol.sec === "fonte" ? "" : "fonte"
                                        background: Rectangle { radius: 8; color: parent.hovered ? Theme.hov : "transparent"; border.width: parent.visualFocus ? 2 : 0; border.color: Theme.foc }
                                        contentItem: Item {
                                            Text { anchors.left: parent.left; anchors.leftMargin: 8; anchors.verticalCenter: parent.verticalCenter; font.family: Theme.sans; text: "FONTE"; color: Theme.mut; font.pixelSize: 11; font.letterSpacing: 0.6 }
                                            Text { anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter; font.family: Theme.sans; text: apCol.lbl([["jetbrains", "JetBrains Mono"], ["plex", "IBM Plex"]], Theme.fonte) + (apCol.sec === "fonte" ? "  ▾" : "  ▸"); color: Theme.fg; font.pixelSize: 12; elide: Text.ElideRight; horizontalAlignment: Text.AlignRight; width: Math.max(40, parent.width - 16 - parent.children[0].implicitWidth - 12) }
                                        }
                                    }
                                    Repeater {
                                        model: [["jetbrains", "JetBrains Mono"], ["plex", "IBM Plex"]]
                                        AbstractButton {
                                            id: fo
                                            required property var modelData
                                            visible: apCol.sec === "fonte"
                                            objectName: "font_" + modelData[0]
                                            width: apMenu.width - 16; height: 32
                                            hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                            Accessible.name: modelData[1]; Accessible.role: Accessible.RadioButton
                                            onClicked: { backend.setFonte(modelData[0]); Theme.fonte = modelData[0]; }
                                            background: Rectangle { radius: 8; color: fo.hovered ? Theme.hov : "transparent"
                                                border.width: fo.visualFocus ? 2 : 0; border.color: Theme.foc }
                                            contentItem: Text { font.family: Theme.sans; leftPadding: 8; verticalAlignment: Text.AlignVCenter
                                                text: (Theme.fonte === fo.modelData[0] ? "●  " : "○  ") + fo.modelData[1]; color: Theme.fg; font.pixelSize: 13 }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    FolderDialog {
                        id: folderDlg
                        title: "Escolha a pasta de trabalho do agente"
                        currentFolder: "file://" + backend.workspace
                        onAccepted: backend.setWorkspace(selectedFolder.toString())
                    }
                    IconButton { objectName: "winMin"; glyph: "minimize"; tip: "Minimizar"; onClicked: win.showMinimized() }
                    IconButton { objectName: "winMax"; glyph: win.maxed ? "restore" : "maximize"; tip: win.maxed ? "Restaurar" : "Maximizar"
                        onClicked: win.maxed ? win.showNormal() : win.showMaximized() }
                    IconButton { objectName: "winClose"; glyph: "close"; tip: "Fechar"; hoverFill: "#c42b1c"; hoverIconColor: "#ffffff"; onClicked: win.close() }
                }
            }

            TerminalPanel {
                id: termPanel
                visible: win.termOpen
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: win.termOpen ? Math.min(340, Math.max(160, mainArea.height * 0.4)) : 0
                z: 6
                onCloseRequested: win.toggleTerminal()
            }

            MessageView {
                id: mv
                anchors.top: top.bottom
                anchors.topMargin: 0
                anchors.bottom: termPanel.visible ? termPanel.top : parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                bottomSpace: composer.height + 24 + win.cardsHeight
                onSuggestionChosen: (t) => {
                    composer.setText(t);
                    if (backend.ready) composer.submit(); else composer.input.forceActiveFocus();
                }
            }

            // modelos: abre sozinho quando a maquina ainda nao tem nenhum modelo utilizavel
            ModelStore {
                id: store
                anchors.top: top.bottom
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                z: 8
            }

            // AppImage, primeira abertura: pergunta uma vez se quer o atalho no menu e na area de trabalho
            Rectangle {
                objectName: "shortcutOffer"
                visible: backend.shortcutOffer
                z: 9
                anchors.top: top.bottom
                anchors.topMargin: 12
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(560, parent.width - 32)
                height: scCol.height + 24
                radius: 14
                color: Theme.card
                border.color: Theme.ac
                Column {
                    id: scCol
                    x: 16; y: 12
                    width: parent.width - 32
                    spacing: 10
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "Adicionar o Katana ao menu de aplicativos e à área de trabalho?"
                        color: Theme.fg
                        font.family: Theme.sans
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }
                    Row {
                        spacing: 8
                        Repeater {
                            model: [{t: "Adicionar", yes: true}, {t: "Agora não", yes: false}]
                            delegate: Rectangle {
                                required property var modelData
                                objectName: modelData.yes ? "shortcutYes" : "shortcutNo"
                                width: scTxt.implicitWidth + 28
                                height: 32
                                radius: 10
                                color: modelData.yes ? Theme.ac : (scMa.containsMouse ? Theme.hov : "transparent")
                                border.color: Theme.bd
                                Text {
                                    id: scTxt
                                    anchors.centerIn: parent
                                    text: modelData.t
                                    color: modelData.yes ? Theme.acfg : Theme.fg
                                    font.family: Theme.sans
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                }
                                MouseArea {
                                    id: scMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: backend.answerShortcut(modelData.yes)
                                }
                            }
                        }
                    }
                }
            }

            // esmaece o texto que passa por tras da caixa
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: composer.bottom
                height: composer.height + 40 + win.cardsHeight
                gradient: Gradient {
                    GradientStop { position: 0.0; color: Qt.rgba(Theme.bg.r, Theme.bg.g, Theme.bg.b, 0) }
                    GradientStop { position: 0.35; color: Theme.bg }
                    GradientStop { position: 1.0; color: Theme.bg }
                }
            }
            // modo plano: cartao discreto acima da caixa de mensagem (a lista de tarefas fica so na conversa, no cartao do plano)
            Rectangle {
                id: workCard
                objectName: "workCard"
                visible: backend.planMode
                anchors.bottom: choiceCard.visible ? choiceCard.top : composer.top
                anchors.bottomMargin: 6 + (choiceCard.visible ? 0 : composer.queueHeight)      // a fila de mensagens fica entre os cartoes e a caixa
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(parent.width - 32, 760)
                height: visible ? workCol.implicitHeight + 20 : 0
                radius: 14
                color: Theme.card
                border.color: Theme.bd
                Column {
                    id: workCol
                    x: 14; y: 10
                    width: parent.width - 28
                    spacing: 5
                    Row {
                        visible: backend.planMode
                        spacing: 10
                        Text { font.family: Theme.sans; color: Theme.fg; font.pixelSize: 13; anchors.verticalCenter: parent.verticalCenter
                            text: "Modo plano: o agente só lê. Nada é alterado até você aprovar." }
                        Rectangle {
                            objectName: "approvePlanButton"
                            height: 28; width: apTxt.implicitWidth + 22; radius: 8
                            opacity: backend.ready ? 1 : 0.45
                            color: apMa.containsMouse ? Theme.ac : Theme.hov
                            border.color: apMa.containsMouse ? Theme.ac : Theme.bd
                            Text { id: apTxt; anchors.centerIn: parent; text: "Aprovar e executar"; font.family: Theme.sans; font.pixelSize: 13
                                color: apMa.containsMouse ? Theme.acfg : Theme.fg }
                            MouseArea { id: apMa; anchors.fill: parent; hoverEnabled: true; enabled: backend.ready; cursorShape: Qt.PointingHandCursor
                                onClicked: backend.approvePlan() }
                        }
                    }
                }
            }
            // pergunta do agente (AskUserQuestion): fica junto da caixa de mensagem, com as opcoes em botoes; digitar tambem responde
            Rectangle {
                id: choiceCard
                objectName: "choiceCard"
                visible: backend.choicePending
                anchors.bottom: composer.top
                anchors.bottomMargin: 6 + composer.queueHeight
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(parent.width - 32, 760)
                height: visible ? choiceCol.implicitHeight + 24 : 0
                radius: 14
                color: Theme.card
                border.color: Theme.bd
                Column {
                    id: choiceCol
                    x: 14; y: 12
                    width: parent.width - 28
                    spacing: 10
                    Text { font.family: Theme.sans; color: Theme.mut; font.pixelSize: 12; text: "O agente pergunta" }
                    Text { font.family: Theme.sans; color: Theme.fg; font.pixelSize: 15; width: parent.width; wrapMode: Text.Wrap; text: backend.choiceQuestion }
                    Flow {
                        width: parent.width
                        spacing: 8
                        Repeater {
                            model: backend.choiceOptions
                            Rectangle {
                                height: 34; width: optTxt.implicitWidth + 28; radius: 9
                                color: optMa.containsMouse ? Theme.ac : Theme.hov
                                border.color: optMa.containsMouse ? Theme.ac : Theme.bd
                                Text { id: optTxt; anchors.centerIn: parent; text: modelData; font.family: Theme.sans; font.pixelSize: 14
                                    color: optMa.containsMouse ? Theme.acfg : Theme.fg }
                                MouseArea { id: optMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                    onClicked: backend.answerChoice(modelData) }
                            }
                        }
                    }
                    Text { font.family: Theme.sans; color: Theme.mut; font.pixelSize: 12; width: parent.width; wrapMode: Text.Wrap
                        text: backend.choiceOptions.length ? "Ou escreva outra resposta na caixa abaixo." : "Escreva a resposta na caixa abaixo." }
                }
            }
            Composer {
                id: composer
                anchors.bottom: termPanel.visible ? termPanel.top : parent.bottom
                anchors.bottomMargin: 6
                anchors.left: parent.left
                anchors.right: parent.right
            }
        }
    }

    // redimensionar sem moldura (bordas e cantos)
    Repeater {
        model: win.maxed ? [] : [
            {e: Qt.LeftEdge, x: 0, y: 6, w: 5, h: -12, c: Qt.SizeHorCursor},
            {e: Qt.RightEdge, x: -5, y: 6, w: 5, h: -12, c: Qt.SizeHorCursor},
            {e: Qt.TopEdge, x: 6, y: 0, w: -12, h: 5, c: Qt.SizeVerCursor},
            {e: Qt.BottomEdge, x: 6, y: -5, w: -12, h: 5, c: Qt.SizeVerCursor},
            {e: Qt.LeftEdge | Qt.TopEdge, x: 0, y: 0, w: 6, h: 6, c: Qt.SizeFDiagCursor},
            {e: Qt.RightEdge | Qt.BottomEdge, x: -6, y: -6, w: 6, h: 6, c: Qt.SizeFDiagCursor},
            {e: Qt.RightEdge | Qt.TopEdge, x: -6, y: 0, w: 6, h: 6, c: Qt.SizeBDiagCursor},
            {e: Qt.LeftEdge | Qt.BottomEdge, x: 0, y: -6, w: 6, h: 6, c: Qt.SizeBDiagCursor}
        ]
        MouseArea {
            required property var modelData
            z: 100
            x: modelData.x < 0 ? win.contentItem.width + modelData.x : modelData.x
            y: modelData.y < 0 ? win.contentItem.height + modelData.y : modelData.y
            width: modelData.w < 0 ? win.contentItem.width + modelData.w : modelData.w
            height: modelData.h < 0 ? win.contentItem.height + modelData.h : modelData.h
            cursorShape: modelData.c
            onPressed: win.startSystemResize(modelData.e)
        }
    }
}
