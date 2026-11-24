import QtQuick
import Launcher

Item {
    id: page
    property string key: "support"
    readonly property string title: Theme.tr("nav_" + key)
    readonly property color accent: Theme.accents[key]
    readonly property string iconPath: Theme.icons[key]
    Column {
        anchors.centerIn: parent
        spacing: 14; width: Math.min(460, parent.width - 72)
        Icon { path: page.iconPath; color: page.accent; size: 44; anchors.horizontalCenter: parent.horizontalCenter }
        Text { anchors.horizontalCenter: parent.horizontalCenter; text: page.title; color: "#fff"
               font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 44; font.weight: Font.ExtraBold
               font.capitalization: Font.AllUppercase; font.letterSpacing: 2.2 }
        Rectangle { anchors.horizontalCenter: parent.horizontalCenter; width: 120; height: 5; color: Theme.warn
                    // checkpoint-tape stripe
                    Repeater { model: 8
                        Rectangle { x: index * 16; width: 8; height: 5; color: "#101216"; rotation: 0 } } }
        Text { anchors.horizontalCenter: parent.horizontalCenter; text: Theme.tr("soon_title"); color: page.accent
               font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 700 }); font.pixelSize: 20; font.weight: Font.Bold
               font.capitalization: Font.AllUppercase; font.letterSpacing: 4 }
        Text { width: parent.width; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; text: Theme.tr("soon_body")
               color: Theme.textDim; font.family: Theme.fontText; font.pixelSize: 15; lineHeight: 1.4
               lineHeightMode: Text.ProportionalHeight }
    }
}
