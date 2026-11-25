import QtQuick
import Launcher

Rectangle {
    id: root
    property string text: ""
    property string tone: "info"   // info | good | warn | bad
    readonly property color toneColor: tone === "good" ? Theme.ok : tone === "warn" ? Theme.warn
                                     : tone === "bad" ? Theme.bad : Theme.textMuted
    implicitHeight: 20
    implicitWidth: t.implicitWidth + 16
    radius: 4
    color: Qt.rgba(toneColor.r, toneColor.g, toneColor.b, 0.12)
    border.width: 1
    border.color: Qt.rgba(toneColor.r, toneColor.g, toneColor.b, 0.45)
    Text { id: t; anchors.centerIn: parent; text: root.text; color: root.toneColor
           font.family: Theme.fontText; font.pixelSize: 12; font.weight: Font.Bold
           font.capitalization: Font.AllUppercase; font.letterSpacing: 0.6 }
}
