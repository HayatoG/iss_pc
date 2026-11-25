import QtQuick
import Launcher

FocusScope {
    id: button
    property string text: ""
    property string iconPath: ""
    property bool primary: false
    signal clicked()
    implicitWidth: caption.implicitWidth + (iconPath !== "" ? 70 : 42)
    implicitHeight: 44
    activeFocusOnTab: true
 property var controllerFocusTarget: null
 FocusHalo { active: parent.activeFocus }
    opacity: enabled ? 1 : 0.38
    Rectangle {
        anchors.fill: parent; radius: 2
        color: button.primary ? (mouse.containsMouse ? "#55a7f2" : "#2579cf") : mouse.containsMouse ? "#35ffffff" : "#18ffffff"
        border.width: button.activeFocus ? 2 : 1
        border.color: button.activeFocus || button.primary ? "#ffffff" : "#6bffffff"
        Behavior on color { ColorAnimation { duration: Theme.motionDuration } }
    }
    Row {
        anchors.centerIn: parent; spacing: 12
        Icon { visible: button.iconPath !== ""; path: button.iconPath; size: 19; color: "white"; anchors.verticalCenter: parent.verticalCenter }
        Text { id: caption; text: button.text; color: "white"; font.family: Theme.fontText; font.pixelSize: button.primary ? 18 : 14; font.weight: Font.Medium; anchors.verticalCenter: parent.verticalCenter
               width: Math.min(implicitWidth, Math.max(0, button.width - (button.iconPath !== "" ? 62 : 24))); elide: Text.ElideRight }
    }
    MouseArea { id: mouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { button.forceActiveFocus(); button.clicked() } }
    Keys.onReturnPressed: button.clicked()
    Keys.onSpacePressed: button.clicked()
    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.onPressAction: button.clicked()
}
