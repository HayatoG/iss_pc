import QtQuick
import Launcher
FocusScope {
 id: root
 property string text: ""
 property bool compact: false
 property string tone: "ghost"
 property color accent: Theme.ok
 property bool glossy: false
 property real cornerRadius: height / 2
 signal clicked()
 readonly property color toneColor: tone === "danger" ? Theme.bad : accent
 implicitHeight: compact ? 36 : 44
 implicitWidth: label.implicitWidth + 36
 opacity: enabled ? 1 : 0.38
 activeFocusOnTab: true
 property var controllerFocusTarget: null
 FocusHalo { active: parent.activeFocus }
 scale: mouse.pressed ? 0.975 : 1
 Behavior on scale { NumberAnimation { duration: Theme.motionDuration; easing.type: Easing.OutCubic } }
 GlassSurface { anchors.fill: parent; radius: root.cornerRadius; blurEnabled: false
  tint: root.tone === "primary" ? root.accent : root.tone === "danger" ? "#840c27" : Qt.darker(Theme.surfaceAccent, 4.5)
  accent: root.tone === "primary" || root.tone === "danger" ? root.toneColor : Theme.surfaceAccent
  density: root.tone === "primary" ? 0.82 : Qt.colorEqual(Theme.surfaceAccent, "#a9afb9") ? 0.80 : 0.44
  highlighted: mouse.containsMouse; pressed: mouse.pressed; focused: root.activeFocus
  light: Qt.point(mouse.width > 0 ? mouse.mouseX / mouse.width : 0.3, 0) }
 Text { id: label; anchors.centerIn: parent; text: root.text
  color: root.tone === "primary" ? "#101820" : root.tone === "danger" ? "#ffb8c0" : "#f6f8fc"
  font.family: Theme.fontText; font.pixelSize: 14; font.weight: Font.DemiBold
  elide: Text.ElideRight; width: Math.min(implicitWidth, parent.width - 16); horizontalAlignment: Text.AlignHCenter }
 MouseArea { id: mouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { root.forceActiveFocus(Qt.MouseFocusReason); root.clicked() } }
 Keys.onSpacePressed: root.clicked()
 Keys.onReturnPressed: root.clicked()
 Accessible.role: Accessible.Button
 Accessible.name: text
 Accessible.onPressAction: root.clicked()
}
