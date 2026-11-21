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
    // altura dos cartoes acima da caixa de mensagem (tarefas do agente, pergunta): a conversa termina antes deles
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
    Connections { target: backend; function onTeacherKeyNeeded() { tchDlg.open(); } }
    Popup {
        id: tchDlg
        objectName: "teacherDialog"
        modal: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(460, win.width - 40)
        padding: 18
        onOpened: { tchModel.text = backend.teacherModel; tchKey.text = ""; tchKey.forceActiveFocus(); }
        function save() { backend.setTeacher(tchKey.text, tchModel.text); tchKey.text = ""; close(); }
        background: Rectangle { radius: 14; color: Theme.card; border.color: Theme.bd }
        contentItem: Column {
            spacing: 10
            Text { font.family: Theme.sans; text: "Professor"; color: Theme.fg; font.pixelSize: 16; font.bold: true }
            Text { font.family: Theme.sans; width: parent.width; wrapMode: Text.Wrap; color: Theme.mut; font.pixelSize: 13
                text: "Um modelo maior responde no lugar do modelo local, usando as mesmas ferramentas nesta máquina. Sem chave, o katana usa o Claude Code em que você já está logado neste computador. Com chave, usa a API da Anthropic (a chave fica em ~/.katana/teacher.key, legível só por você). Nos dois casos a conversa, o prompt do agente e os resultados das ferramentas saem desta máquina." }
            TextField {
                id: tchKey
                objectName: "teacherKey"
                width: parent.width
                echoMode: TextInput.Password
                placeholderText: backend.hasTeacherKey() ? "Chave da API (em branco mantém a atual)" : "Chave da API (opcional: em branco usa o Claude Code)"
                color: Theme.fg
                Accessible.name: "Chave da API do professor"
                onAccepted: tchDlg.save()
            }
            TextField {
                id: tchModel
                objectName: "teacherModel"
                width: parent.width
                placeholderText: "Modelo (claude-opus-5-5)"
                color: Theme.fg
                Accessible.name: "Modelo do professor"
                onAccepted: tchDlg.save()
            }
            Row {
                spacing: 8
                anchors.right: parent.right
                Button { text: "Cancelar"; onClicked: tchDlg.close() }
                Button { objectName: "teacherSave"; text: "Salvar e ligar"; highlighted: true; onClicked: tchDlg.save() }
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
                                Text { font.family: Theme.sans; text: "PROFESSOR"; color: Theme.mut; font.pixelSize: 11; leftPadding: 8; topPadding: 8; bottomPadding: 4; font.letterSpacing: 0.6 }
                                AbstractButton {
                                    id: tchCfg
                                    objectName: "teacherConfig"
                                    width: apMenu.width - 16; height: 32
                                    hoverEnabled: true; focusPolicy: Qt.StrongFocus
                                    Accessible.name: "Chave e modelo do professor"
                                    onClicked: { apMenu.close(); tchDlg.open(); }
                                    background: Rectangle { radius: 8; color: tchCfg.hovered ? Theme.hov : "transparent"
                                        border.width: tchCfg.visualFocus ? 2 : 0; border.color: Theme.foc }
                                    contentItem: Text { font.family: Theme.sans; leftPadding: 8; rightPadding: 8; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                                        text: "Chave e modelo…  (" + backend.teacherModel + ")"; color: Theme.fg; font.pixelSize: 13 }
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
            // lista de tarefas do agente e modo plano: cartao discreto acima da caixa de mensagem
            Rectangle {
                id: workCard
                objectName: "workCard"
                property bool open: false      // recolhido: uma linha com o passo atual; a lista inteira so quando o dono pede
                visible: backend.todoSteps.length > 0 || backend.planMode
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
                    Item {
                        id: workHead
                        objectName: "todoHeader"
                        visible: backend.todoSteps.length > 0
                        width: workCol.width
                        height: visible ? 20 : 0
                        readonly property int total: backend.todoSteps.length
                        readonly property int doneCount: { var n = 0; for (var i = 0; i < backend.todoSteps.length; i++) if (backend.todoSteps[i].indexOf("[x]") === 0) n++; return n; }
                        readonly property string current: { for (var i = 0; i < backend.todoSteps.length; i++) if (backend.todoSteps[i].indexOf("[x]") !== 0) return backend.todoSteps[i].slice(4); return "tudo feito"; }
                        Text { id: workCount; anchors.verticalCenter: parent.verticalCenter; font.family: Theme.sans; color: Theme.mut; font.pixelSize: 12
                            text: "Tarefas do agente  " + workHead.doneCount + " de " + workHead.total }
                        Text { id: workStats; objectName: "workStats"; anchors.right: workToggle.left; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter
                            font.family: Theme.sans; color: Theme.mut; font.pixelSize: 12; text: backend.progressStats }
                        Text { anchors.left: workCount.right; anchors.leftMargin: 10; anchors.right: workStats.left; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter
                            visible: !workCard.open; elide: Text.ElideRight; font.family: Theme.sans; color: Theme.fg; font.pixelSize: 13
                            text: "\u25CB  " + workHead.current }
                        Text { id: workToggle; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; font.family: Theme.sans; color: Theme.mut; font.pixelSize: 12
                            text: workCard.open ? "recolher" : "ver todas" }
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: workCard.open = !workCard.open }
                    }
                    Repeater {
                        model: workCard.open ? backend.todoSteps : []
                        Text {
                            readonly property bool done: modelData.indexOf("[x]") === 0
                            width: workCol.width; wrapMode: Text.Wrap
                            font.family: Theme.sans; font.pixelSize: 13
                            color: done ? Theme.mut : Theme.fg
                            font.strikeout: done
                            text: (done ? "\u2713  " : "\u25CB  ") + modelData.slice(4)
                        }
                    }
                }
            }
            // pergunta do agente (ask_user): fica junto da caixa de mensagem, com as opcoes em botoes; digitar tambem responde
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
