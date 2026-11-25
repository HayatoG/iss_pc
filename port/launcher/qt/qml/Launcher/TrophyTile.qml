import QtQuick
import QtQuick.Controls.Basic
import Launcher

// One trophy icon (2026-11-22), used on the Play tab, the Platinum square and the Trophies tab.
// Earned: full colour on a warm tile, a gold frame with a soft gold glow and a light sheen.
// Not earned: dimmed on a dark tile. Hover shows its name, how to earn it and when it was earned.
Item {
    id: root
    property var trophy: ({})
    property int side: 44
    property int corner: Math.max(6, Math.round(side * 0.14))
    property bool tooltip: true
    property bool clickable: false
    signal clicked()
    readonly property bool earned: trophy && trophy.earned ? true : false
    readonly property color gold: "#eebf4c"
    width: side; height: side

    Repeater {   // soft gold glow around an earned trophy
        model: root.earned ? 3 : 0
        Rectangle {
            anchors.centerIn: parent
            width: root.side + 4 + index * 5; height: width; radius: root.corner + 2 + index * 2.5
            color: "transparent"; border.width: 2
            border.color: Qt.rgba(0.93, 0.75, 0.30, 0.34 - index * 0.10)
        }
    }
    Rectangle { anchors.fill: parent; radius: root.corner; color: root.earned ? "#2b2210" : "#0e1014" }
    Image {
        anchors.fill: parent; anchors.margins: Math.max(2, Math.round(root.side * 0.045))
        source: root.trophy && root.trophy.icon ? root.trophy.icon : ""
        asynchronous: true; smooth: true; mipmap: true
        sourceSize.width: 256; sourceSize.height: 256
        opacity: root.earned ? 1.0 : 0.28
    }
    Rectangle {   // sheen on an earned trophy
        visible: root.earned
        anchors.fill: parent; anchors.margins: 2; radius: root.corner - 1
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.rgba(1, 1, 1, 0.16) }
            GradientStop { position: 0.45; color: Qt.rgba(1, 1, 1, 0) }
        }
    }
    Rectangle {   // frame: gold when earned, a faint line on hover otherwise
        anchors.fill: parent; radius: root.corner; color: "transparent"
        border.width: root.earned ? 2 : (hover.containsMouse ? 1 : 0)
        border.color: root.earned ? (hover.containsMouse ? Qt.lighter(root.gold, 1.2) : root.gold) : Theme.ghostLineHover
    }
    MouseArea {
        id: hover; anchors.fill: parent; hoverEnabled: true
        cursorShape: root.clickable ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: if (root.clickable) root.clicked()
    }
    ToolTip {
        visible: root.tooltip && hover.containsMouse
        delay: 150
        contentItem: Column {
            spacing: 4
            Text { text: root.trophy.name || ""; color: "#fff"; font.family: Theme.fontText; font.pixelSize: 15; font.weight: Font.Bold }
            Text { width: 260; text: root.trophy.requirement || ""; wrapMode: Text.WordWrap; color: Theme.textSoft
                   font.family: Theme.fontText; font.pixelSize: 13 }
            Text { text: root.trophy.status || ""; color: root.earned ? root.gold : Theme.textDim
                   font.family: Theme.fontText; font.pixelSize: 13; font.weight: Font.DemiBold }
        }
        background: PolishCard { color: Theme.card; radius: 8; border.width: 1; border.color: Theme.ghostLine }
    }
}
