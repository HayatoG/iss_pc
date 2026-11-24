import QtQuick
import Launcher
FocusScope {
 id: root
 property bool checked: false
 property color accent: Theme.smoke
 signal toggled(bool value)
 width: 46; height: 26
 activeFocusOnTab: true
 property var controllerFocusTarget: null
 FocusHalo { active: parent.activeFocus }
 Rectangle {
  anchors.fill: parent; radius: height / 2
  color: root.checked ? root.accent : "#4b5665"
  border.width: 1; border.color: root.activeFocus ? "white" : "#60ffffff"
  Behavior on color { ColorAnimation { duration: Theme.motionDuration } }
  Rectangle { anchors.fill: parent; anchors.margins: 1; radius: height / 2
   gradient: Gradient { GradientStop { position: 0; color: "#40ffffff" } GradientStop { position: 0.55; color: "#00ffffff" } GradientStop { position: 1; color: "#18000000" } } }
 }
 Rectangle { y: 3; width: 20; height: 20; radius: 10
  x: root.checked ? parent.width - width - 3 : 3
  color: "#f8fbff"; border.color: "#d0e0ef"; border.width: 1
  Behavior on x { NumberAnimation { duration: Theme.motionDuration; easing.type: Easing.OutCubic } } }
 MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { root.forceActiveFocus(Qt.MouseFocusReason); root.toggled(!root.checked) } }
 Keys.onSpacePressed: root.toggled(!root.checked)
 Keys.onReturnPressed: root.toggled(!root.checked)
 Accessible.role: Accessible.CheckBox
 Accessible.checked: checked
 Accessible.onToggleAction: root.toggled(!root.checked)
}
