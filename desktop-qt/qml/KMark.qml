import QtQuick
import Katana

// "K" centralizado pelo contorno real do glifo (nao pela altura da linha da fonte)
Item {
    id: km
    property int px: 12
    TextMetrics {
        id: tm
        font.family: Theme.sans
        font.bold: true
        font.pixelSize: km.px
        text: "K"
    }
    Text {
        text: "K"
        color: Theme.acfg
        font: tm.font
        x: (km.width - tm.tightBoundingRect.width) / 2 - tm.tightBoundingRect.x
        y: (km.height - tm.tightBoundingRect.height) / 2 - tm.tightBoundingRect.y
    }
}
