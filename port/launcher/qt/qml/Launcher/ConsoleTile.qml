import QtQuick
import QtQuick.Window
import Launcher

FocusScope {
    id: tile
    property bool controllerOwnFocus: true
    property string title: ""
    property url artwork: ""
    property string iconPath: ""
    property bool selected: false
    property bool game: false
    property string actionLabel: ""
    signal selectedByUser()
    signal activated()
    width: selected ? 242 : game ? 174 : 142
    height: width + 42
    activeFocusOnTab: true
    Behavior on width { enabled: !Backend.reducedMotion; NumberAnimation { duration: 190; easing.type: Easing.OutCubic } }
    Rectangle {
        x: -5; y: -5; width: parent.width + 10; height: parent.width + 52
        color: "transparent"; border.width: tile.selected || tile.activeFocus ? 2 : 0; border.color: "white"
        opacity: tile.selected || tile.activeFocus ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.motionDuration } }
    }
    Rectangle {
        width: parent.width; height: width; color: tile.game ? "#073b74" : "#9c0751a0"
        border.width: 1; border.color: "#50b7ddff"
        Image { id: cover; asynchronous: true; cache: true; sourceSize: Qt.size(Math.ceil(320 * Screen.devicePixelRatio), Math.ceil(320 * Screen.devicePixelRatio)); anchors.fill: parent; source: tile.artwork; visible: status === Image.Ready; fillMode: Image.PreserveAspectCrop; smooth: true; mipmap: true }
        Icon { anchors.centerIn: parent; visible: tile.iconPath !== "" || (tile.game && cover.status !== Image.Ready); path: tile.iconPath !== "" ? tile.iconPath : Theme.icons.play; size: 52; color: "white" }
        Rectangle { anchors.fill: parent; color: "white"; opacity: mouse.containsMouse ? 0.09 : 0; Behavior on opacity { NumberAnimation { duration: Theme.motionDuration } } }
    }
    Rectangle {
        y: tile.width; width: parent.width; height: 42
        color: tile.selected ? "#90245e9f" : "transparent"
        Text {
            anchors.centerIn: parent; width: parent.width - 12; horizontalAlignment: Text.AlignHCenter
            text: tile.selected && tile.actionLabel !== "" ? tile.actionLabel : tile.title
            color: "white"; font.family: Theme.fontText; font.pixelSize: tile.selected ? 18 : 13
            elide: Text.ElideRight; font.weight: Font.Medium
        }
    }
    MouseArea {
        id: mouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
        onClicked: { tile.forceActiveFocus(); if (tile.selected) tile.activated(); else tile.selectedByUser() }
    }
    Keys.onReturnPressed: { if (!tile.selected) tile.selectedByUser(); else tile.activated() }
    Keys.onSpacePressed: tile.selectedByUser()
    Accessible.role: Accessible.Button
    Accessible.name: title
    Accessible.onPressAction: tile.selectedByUser()
}
