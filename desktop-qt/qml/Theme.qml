pragma Singleton
import QtQuick

// Cores (bege/terracota) e tema claro/escuro que segue o sistema
QtObject {
    id: t
    property string force: ""  // "light" ou "dark" (so para teste)
    readonly property bool dark: force === "dark" ? true
                                : force === "light" ? false
                                : Qt.styleHints.colorScheme === Qt.ColorScheme.Dark

    readonly property var lt: ({bg: "#faf9f5", side: "#f2f0e8", fg: "#1f1e1d", mut: "#6a6963", bd: "#e0ddd2", hov: "#e9e6dc", us: "#ebe8de", ac: "#c4552f", acfg: "#ffffff", card: "#ffffff", code: "#f4f2ea", codeh: "#ebe8de", err: "#b3261e", foc: "#2a6fdb", shadow: "#28190f"})
    readonly property var dk: ({bg: "#262624", side: "#1e1d1c", fg: "#ece9e0", mut: "#a09e95", bd: "#3b3a36", hov: "#2e2d2a", us: "#34332f", ac: "#d97757", acfg: "#1b1a19", card: "#2f2e2b", code: "#1d1c1b", codeh: "#2a2927", err: "#f2928b", foc: "#7ab0ff", shadow: "#000000"})
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
