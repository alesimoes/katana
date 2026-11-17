pragma Singleton
import QtQuick

// Cores (neutros levemente lilases, destaque roxo #820AD1) e tema claro/escuro que segue o sistema
QtObject {
    id: t
    property string force: ""  // "light" ou "dark" (so para teste)
    readonly property bool dark: force === "dark" ? true
                                : force === "light" ? false
                                : Qt.styleHints.colorScheme === Qt.ColorScheme.Dark

    readonly property var lt: ({bg: "#faf9fc", side: "#f3f0f7", fg: "#1f1c24", mut: "#6b6773", bd: "#e2dde9", hov: "#ebe6f1", us: "#ece7f3", ac: "#820ad1", acfg: "#ffffff", card: "#ffffff", code: "#f5f2f8", codeh: "#ece7f3", err: "#b3261e", foc: "#820ad1", shadow: "#1e1028"})
    readonly property var dk: ({bg: "#221f26", side: "#1b1920", fg: "#ece9f0", mut: "#a39fab", bd: "#38343f", hov: "#2b2831", us: "#332f3a", ac: "#b66cf2", acfg: "#1a1022", card: "#2b2831", code: "#1a181e", codeh: "#26232b", err: "#f2928b", foc: "#cfa3f7", shadow: "#000000"})
    readonly property color bg: dark ? dk.bg : lt.bg
    readonly property color side: dark ? dk.side : lt.side
    readonly property color fg: dark ? dk.fg : lt.fg
    readonly property color mut: dark ? dk.mut : lt.mut
    readonly property color bd: dark ? dk.bd : lt.bd
    readonly property color hov: dark ? dk.hov : lt.hov
    readonly property color us: dark ? dk.us : lt.us
    readonly property color ac: dark ? dk.ac : lt.ac
    readonly property color acfg: dark ? dk.acfg : lt.acfg
    readonly property color card: dark ? dk.card : lt.card
    readonly property color code: dark ? dk.code : lt.code
    readonly property color codeh: dark ? dk.codeh : lt.codeh
    readonly property color err: dark ? dk.err : lt.err
    readonly property color foc: dark ? dk.foc : lt.foc
    readonly property color shadow: dark ? dk.shadow : lt.shadow
    readonly property int sideWidth: 276
    readonly property int colWidth: 760
}
