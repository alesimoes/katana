import QtQuick
import Katana
import "Markdown.js" as Md

// Resposta do assistente: sem balao, texto selecionavel, `codigo`, **negrito** e blocos ```
Column {
    id: root
    property string text: ""
    spacing: 8
    readonly property var blocks: Md.parse(text.replace(/^\s+/, ""), Theme.dark ? "#1a181e" : "#ece7f3")

    Repeater {
        model: root.blocks
        delegate: Item {
            id: blk
            required property var modelData
            width: root.width
            height: modelData.type === "code" ? cbl.height : txl.height
            Loader {
                id: txl
                active: blk.modelData.type === "text"
                width: parent.width
                sourceComponent: TextEdit {
                    text: blk.modelData.html
                    textFormat: TextEdit.RichText
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.Wrap
                    color: Theme.fg
                    selectionColor: Theme.ac
                    selectedTextColor: Theme.acfg
                    font.pixelSize: 15
                    Accessible.role: Accessible.StaticText
                    Accessible.name: "Resposta"
                }
            }
            Loader {
                id: cbl
                active: blk.modelData.type === "code"
                width: parent.width
                sourceComponent: CodeBlock {
                    lang: blk.modelData.lang
                    code: blk.modelData.code
                }
            }
        }
    }
}
