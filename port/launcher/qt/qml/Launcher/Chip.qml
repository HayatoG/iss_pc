import QtQuick
import Launcher

Rectangle {
    id: root
    property string text: ""
    property string level: "ok"   // ok | warn | bad | off
    // compact (2026-11-22): a slim see-through pill for the glass cards on the Play tab
    property bool compact: false
    implicitHeight: compact ? 26 : 34
    implicitWidth: row.implicitWidth + (compact ? 18 : 24)
    radius: height / 2
    color: compact ? Qt.rgba(1, 1, 1, 0.06) : Theme.chip
    border.width: 1
    border.color: compact ? Qt.rgba(1, 1, 1, 0.11) : Theme.chipLine
    Row {
        id: row
        anchors.centerIn: parent
        spacing: root.compact ? 6 : 8
        Rectangle {
            readonly property int size: root.compact ? 6 : 8
            width: size; height: size; radius: size / 2
            anchors.verticalCenter: parent.verticalCenter
            color: root.level === "ok" ? Theme.ok : root.level === "warn" ? Theme.warn
                 : root.level === "bad" ? Theme.bad : Theme.off
        }
        Text { text: root.text; color: Theme.textChip; font.family: Theme.fontText
               font.pixelSize: root.compact ? 12 : 13; font.weight: Font.DemiBold; anchors.verticalCenter: parent.verticalCenter }
    }
}
