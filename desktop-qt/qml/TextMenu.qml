import QtQuick
import Katana
import QtQuick.Controls

// Menu de botao direito para TextEdit/TextArea: copiar, colar, recortar, selecionar tudo
Menu {
    id: m
    property var target: null
    readonly property bool editable: !!target && !target.readOnly
    readonly property bool hasSel: !!target && target.selectedText.length > 0
    // texto inteiro (sem formatacao) para "Copiar tudo"
    function allText() { return target ? target.getText(0, target.length) : ""; }

    MenuItem { text: "Recortar"; visible: m.editable; height: visible ? implicitHeight : 0; enabled: m.hasSel; onTriggered: m.target.cut() }
    MenuItem { text: "Copiar"; enabled: m.hasSel; onTriggered: m.target.copy() }
    MenuItem { text: "Colar"; visible: m.editable; height: visible ? implicitHeight : 0; enabled: !!m.target && m.target.canPaste; onTriggered: m.target.paste() }
    MenuSeparator {}
    MenuItem { text: "Selecionar tudo"; onTriggered: { m.target.forceActiveFocus(); m.target.selectAll(); } }
    MenuItem { text: "Copiar tudo"; visible: !m.editable; height: visible ? implicitHeight : 0; onTriggered: backend.copyText(m.allText()) }
}
