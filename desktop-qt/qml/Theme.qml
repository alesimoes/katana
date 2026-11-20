pragma Singleton
import QtQuick

// Cores (neutros levemente lilases, destaque roxo #820AD1) e tema claro/escuro que segue o sistema
QtObject {
    id: t
    property string force: ""  // "light" ou "dark" (so para teste)
    property string tema: "escuro"   // "escuro" (padrao), "claro" ou "ide" (escuro, estilo editor de codigo)
    property string fonte: "jetbrains"  // "jetbrains" ou "plex"
    readonly property string sans: fonte === "plex" ? "IBM Plex Sans" : "JetBrains Mono"
    readonly property string mono: fonte === "plex" ? "IBM Plex Mono" : "JetBrains Mono"
    readonly property bool dark: tema === "ide" ? true
                                : force === "dark" ? true
                                : force === "light" ? false
                                : tema !== "claro"

    readonly property var lt: ({bg: "#faf9fc", side: "#f3f0f7", fg: "#1f1c24", mut: "#6b6773", bd: "#e2dde9", hov: "#ebe6f1", us: "#ece7f3", ac: "#820ad1", acfg: "#ffffff", card: "#ffffff", code: "#f5f2f8", codeh: "#ece7f3", err: "#b3261e", foc: "#820ad1", shadow: "#1e1028", kw: "#8a2bb5", str: "#a1470b", num: "#0b6e99", com: "#7a8a6a", fn: "#7a5a00", ty: "#00796b"})
    readonly property var dk: ({bg: "#221f26", side: "#1b1920", fg: "#ece9f0", mut: "#a39fab", bd: "#38343f", hov: "#2b2831", us: "#332f3a", ac: "#b66cf2", acfg: "#1a1022", card: "#2b2831", code: "#1a181e", codeh: "#26232b", err: "#f2928b", foc: "#cfa3f7", shadow: "#000000", kw: "#d391f2", str: "#e8a56d", num: "#7cc4e8", com: "#7f8a74", fn: "#e6d88a", ty: "#6fd1c2"})
    readonly property var ide: ({bg: "#1e1e1e", side: "#252526", fg: "#d4d4d4", mut: "#8b8b8b", bd: "#3c3c3c", hov: "#2a2d2e", us: "#2d2d30", ac: "#c586c0", acfg: "#1e1e1e", card: "#252526", code: "#161616", codeh: "#252526", err: "#f48771", foc: "#c586c0", shadow: "#000000", kw: "#c586c0", str: "#ce9178", num: "#b5cea8", com: "#6a9955", fn: "#dcdcaa", ty: "#4ec9b0"})
    readonly property var pal: tema === "ide" ? ide : (dark ? dk : lt)
    readonly property color bg: pal.bg
    readonly property color side: pal.side
    readonly property color fg: pal.fg
    readonly property color mut: pal.mut
    readonly property color bd: pal.bd
    readonly property color hov: pal.hov
    readonly property color us: pal.us
    readonly property color ac: pal.ac
    readonly property color acfg: pal.acfg
    readonly property color card: pal.card
    readonly property color code: pal.code
    readonly property color codeh: pal.codeh
    readonly property color err: pal.err
    readonly property color foc: pal.foc
    readonly property color shadow: pal.shadow
    readonly property color kw: pal.kw
    readonly property color str: pal.str
    readonly property color num: pal.num
    readonly property color com: pal.com
    readonly property color fn: pal.fn
    readonly property color ty: pal.ty
    readonly property int sideWidth: 304
    readonly property int colWidth: 1100     // largura maxima da coluna da conversa e da caixa de mensagem
}
