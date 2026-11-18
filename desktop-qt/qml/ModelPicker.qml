import QtQuick
import Katana
import QtQuick.Controls

// Dropdown de modelos: itens nao utilizaveis aparecem desabilitados com uma nota
ComboBox {
    id: cb
    objectName: "modelCombo"
    model: backend.models
    textRole: "name"
    implicitHeight: 38
    property real maxWidth: 380
    property bool compact: false
    implicitWidth: Math.min(maxWidth, Math.max(compact ? 90 : 200, lbl.implicitWidth + (compact ? 34 : 64)))
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.name: "Escolher modelo"

    function syncIndex() {
        var l = backend.models;
        for (var i = 0; i < l.length; i++)
            if (l[i].file === backend.currentModel) { currentIndex = i; return; }
        currentIndex = -1;
    }
    Component.onCompleted: syncIndex()
    Connections {
        target: backend
        function onModelsChanged() { cb.syncIndex(); }
        function onCurrentModelChanged() { cb.syncIndex(); }
    }
    displayText: currentIndex >= 0 && currentIndex < backend.models.length
                 ? backend.models[currentIndex].name : "Selecione um modelo"
    onActivated: (i) => {
        var f = backend.models[i].file;
        if (f === backend.currentModel && backend.running) return;
        backend.start(f);
    }

    background: Rectangle {
        radius: cb.compact ? 8 : 12
        color: cb.hovered || cb.popup.visible ? Theme.hov : "transparent"
        border.color: cb.visualFocus ? Theme.foc : (cb.hovered || cb.popup.visible ? Theme.bd : "transparent")
        border.width: cb.visualFocus ? 2 : 1
    }
    contentItem: Text { font.family: Theme.sans;
        id: lbl
        leftPadding: cb.compact ? 8 : 12
        rightPadding: cb.compact ? 22 : 34
        text: cb.displayText
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
        color: cb.compact ? Theme.mut : Theme.fg
        font.pixelSize: cb.compact ? 12 : 15
        font.weight: cb.compact ? Font.Normal : Font.DemiBold
    }
    indicator: Icon {
        x: cb.width - width - (cb.compact ? 6 : 12)
        y: (cb.height - height) / 2
        name: "down"
        size: cb.compact ? 11 : 15
        color: Theme.mut
    }

    delegate: ItemDelegate {
        id: del
        required property var modelData
        required property int index
        width: ListView.view ? ListView.view.width : 400
        height: 52
        enabled: modelData.usable
        highlighted: cb.highlightedIndex === index
        hoverEnabled: true
        Accessible.name: modelData.name + (modelData.usable ? "" : ", indisponivel")
        background: Rectangle {
            radius: 9
            color: del.index === cb.currentIndex ? Theme.us : (del.highlighted || (del.hovered && del.enabled) ? Theme.hov : "transparent")
        }
        contentItem: Item {
            Text { font.family: Theme.sans;
                x: 12; y: 8
                width: parent.width - 24 - tag.width
                text: del.modelData.name
                elide: Text.ElideRight
                color: Theme.fg
                opacity: del.enabled ? 1 : 0.6
                font.pixelSize: 14
                font.weight: Font.Medium
            }
            Text { font.family: Theme.sans;
                id: tag
                visible: del.modelData.isDefault && del.modelData.usable
                anchors.right: parent.right
                anchors.rightMargin: 12
                y: 9
                text: "padrão"
                color: Theme.ac
                font.pixelSize: 11
                font.weight: Font.DemiBold
            }
            Text { font.family: Theme.sans;
                x: 12; y: 29
                width: parent.width - 24
                text: del.modelData.usable ? del.modelData.sizeText
                                           : del.modelData.note + " · " + del.modelData.sizeText
                elide: Text.ElideRight
                color: Theme.mut
                opacity: del.enabled ? 1 : 0.8
                font.pixelSize: 12
            }
        }
    }

    popup: Popup {
        y: cb.compact ? -height - 6 : cb.height + 6
        x: 0
        width: Math.min(420, (cb.Window.width || 420) - 28)
        height: Math.min(list.contentHeight + 12, 460)
        padding: 6
        onOpened: list.positionViewAtBeginning()
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        contentItem: ListView {
            id: list
            objectName: "modelList"
            clip: true
            cacheBuffer: 100000
            implicitHeight: contentHeight
            model: cb.popup.visible ? cb.delegateModel : null
            currentIndex: cb.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }
        }
        background: Rectangle {
            radius: 14
            color: Theme.card
            border.color: Theme.bd
        }
    }
}
