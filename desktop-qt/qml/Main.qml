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
    title: "katana"
    color: Theme.bg

    readonly property bool narrow: width < 760
    property bool sideOpen: !backend.uiFlag("lateralRecolhida")
    property bool drawerOpen: false

    Component.onCompleted: { Theme.force = backend.themeOverride }

    function toggleSide() {
        if (narrow) drawerOpen = !drawerOpen;
        else { sideOpen = !sideOpen; backend.setUiFlag("lateralRecolhida", !sideOpen); }
    }
    onNarrowChanged: drawerOpen = false
    Shortcut { sequence: "Escape"; onActivated: win.drawerOpen = false }

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

            RowLayout {
                id: top
                x: 14
                y: 9
                width: parent.width - 28
                height: 40
                spacing: 6
                IconButton {
                    objectName: "sidebarToggle"
                    glyph: "menu"
                    tip: "Mostrar ou ocultar a lateral"
                    onClicked: win.toggleSide()
                }
                ModelPicker { Layout.maximumWidth: parent.width - 120 }
                Item { Layout.fillWidth: true }
            }

            MessageView {
                id: mv
                anchors.top: top.bottom
                anchors.topMargin: 6
                anchors.bottom: parent.bottom
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
                anchors.bottom: parent.bottom
                height: composer.height + 40
                gradient: Gradient {
                    GradientStop { position: 0.0; color: Qt.rgba(Theme.bg.r, Theme.bg.g, Theme.bg.b, 0) }
                    GradientStop { position: 0.35; color: Theme.bg }
                    GradientStop { position: 1.0; color: Theme.bg }
                }
            }
            Composer {
                id: composer
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 6
                anchors.left: parent.left
                anchors.right: parent.right
            }
        }
    }
}
