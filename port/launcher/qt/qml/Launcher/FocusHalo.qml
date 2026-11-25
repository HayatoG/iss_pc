import QtQuick
Item {
    id: halo
    property bool active: false
    anchors.fill: parent
    z: 40
    HoverHandler { id: hover }
    Rectangle {
        anchors.fill: parent; anchors.margins: -3
        visible: halo.active || hover.hovered
        radius: 7; color: halo.active ? "#1648a8ed" : "#2248a8ed"
        border.width: halo.active ? 3 : 2; border.color: halo.active ? "#d6f1ff" : "#83d6ff"
        Rectangle { anchors.fill: parent; anchors.margins: -3; radius: 10; color: "transparent"; border.width: 3; border.color: "#3552bdff" }
    }
}
