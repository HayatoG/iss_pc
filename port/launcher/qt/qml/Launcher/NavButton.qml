import QtQuick
import Launcher
FocusScope {
 id: root
 property string label: ""
 property string iconPath: ""
 property color accent: Theme.smoke
 property bool current: false
 property bool badge: false
 signal clicked()
 height: 44
 activeFocusOnTab: true
 scale: mouse.pressed ? 0.98 : 1
 Behavior on scale { NumberAnimation { duration: Theme.motionDuration; easing.type: Easing.OutCubic } }
 GlassSurface {
  anchors.fill: parent; radius: height / 2; blurEnabled: false
  tint: root.current ? Qt.darker(root.accent, 1.45) : Qt.darker(root.accent, 4.0)
  accent: root.accent
  density: root.current ? 0.44 : 0.22
  opacity: root.current || mouse.containsMouse || root.activeFocus ? 1 : 0
  highlighted: root.current || mouse.containsMouse; pressed: mouse.pressed; focused: root.activeFocus
  light: Qt.point(mouse.width > 0 ? mouse.mouseX / mouse.width : 0.3, 0)
  Behavior on opacity { NumberAnimation { duration: Theme.motionDuration } }
 }
 Row {
  anchors.verticalCenter: parent.verticalCenter; x: 16; spacing: 12
  Icon { path: root.iconPath; size: 20; anchors.verticalCenter: parent.verticalCenter; color: root.current ? Qt.lighter(root.accent, 1.3) : Theme.textMuted }
  Text { text: root.label; color: root.current || mouse.containsMouse ? "white" : Theme.textMuted
   font.family: Theme.fontText; font.pixelSize: 15; font.weight: root.current ? Font.DemiBold : Font.Medium; anchors.verticalCenter: parent.verticalCenter }
 }
 Rectangle { visible: root.badge; width: 7; height: 7; radius: 4; color: Theme.warn
  anchors.right: parent.right; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter }
 MouseArea { id: mouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { root.forceActiveFocus(Qt.MouseFocusReason); root.clicked() } }
 Keys.onSpacePressed: root.clicked()
 Keys.onReturnPressed: root.clicked()
 Accessible.role: Accessible.Button
 Accessible.name: label
 Accessible.onPressAction: root.clicked()
}
