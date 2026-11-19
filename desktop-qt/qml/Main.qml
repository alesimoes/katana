import QtQuick
import Katana
import QtQuick.Controls
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
    property bool drawerOpen: false

    Component.onCompleted: { Theme.force = backend.themeOverride; Theme.fonte = backend.fonte(); Theme.tema = backend.tema() }

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
                        objectName: "terminalButton"
                        glyph: "terminal"
                        tip: "Terminal (Ctrl+`)"
                        onClicked: win.toggleTerminal()
                    }
                    IconButton {
                        id: gearBtn
                        objectName: "appearanceButton"
                        glyph: "gear"
                        tip: "Aparência"
                        onClicked: apMenu.opened ? apMenu.close() : apMenu.open()
                        Popup {
                            id: apMenu
                            x: gearBtn.width - width
                            y: gearBtn.height + 6
                            width: 230
                            padding: 8
                            background: Rectangle { radius: 12; color: Theme.card; border.color: Theme.bd }
                            contentItem: Column {
                                spacing: 2
                                Text { font.family: Theme.sans; text: "TEMA"; color: Theme.mut; font.pixelSize: 11; leftPadding: 8; topPadding: 2; bottomPadding: 4; font.letterSpacing: 0.6 }
                                Repeater {
                                    model: [["escuro", "Escuro (padrão)"], ["claro", "Claro"], ["ide", "IDE escuro (código)"]]
                                    AbstractButton {
                                        id: to
                                        required property var modelData
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
                                Text { font.family: Theme.sans; text: "FONTE"; color: Theme.mut; font.pixelSize: 11; leftPadding: 8; topPadding: 8; bottomPadding: 4; font.letterSpacing: 0.6 }
                                Repeater {
                                    model: [["jetbrains", "JetBrains Mono"], ["plex", "IBM Plex"]]
                                    AbstractButton {
                                        id: fo
                                        required property var modelData
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
                bottomSpace: composer.height + 24
                onSuggestionChosen: (t) => {
                    composer.setText(t);
                    if (backend.ready) composer.submit(); else composer.input.forceActiveFocus();
                }
            }

            // esmaece o texto que passa por tras da caixa
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: composer.bottom
                height: composer.height + 40
                gradient: Gradient {
                    GradientStop { position: 0.0; color: Qt.rgba(Theme.bg.r, Theme.bg.g, Theme.bg.b, 0) }
                    GradientStop { position: 0.35; color: Theme.bg }
                    GradientStop { position: 1.0; color: Theme.bg }
                }
            }
            // pergunta do agente (ask_user): fica junto da caixa de mensagem, com as opcoes em botoes; digitar tambem responde
            Rectangle {
                id: choiceCard
                objectName: "choiceCard"
                visible: backend.choicePending
                anchors.bottom: composer.top
                anchors.bottomMargin: 6
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
