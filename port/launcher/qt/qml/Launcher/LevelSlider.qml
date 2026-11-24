import QtQuick
import QtQuick.Controls.Basic
import Launcher

// Slider in the launcher's style. `committed(value)` fires on release and, for the keyboard, a
// moment after the last move, so the game's files are not rewritten at every pixel of a drag.
Slider {
    id: root
    wheelEnabled: false
    activeFocusOnTab: true
    property string controllerKind: "slider"
    property bool controllerEditing: false
    property real controllerDraft: 0
    property var controllerFocusTarget: null
    readonly property real controllerPosition: controllerEditing ? (controllerDraft - from) / Math.max(0.00001, to - from) : visualPosition
    function controllerInput(action) {
        if (!controllerEditing) {
            if (action === "confirm") { settle.stop(); controllerDraft = value; controllerEditing = true }
            return
        }
        if (action === "back") { controllerEditing = false; return }
        if (action === "confirm") { value = controllerDraft; controllerEditing = false; committed(value); return }
        var step = stepSize > 0 ? stepSize : (to - from) / 100
        if (action === "left" || action === "down") controllerDraft = Math.max(from, controllerDraft - step)
        if (action === "right" || action === "up") controllerDraft = Math.min(to, controllerDraft + step)
    }
    FocusHalo { active: root.activeFocus || root.controllerEditing }
    property color accent: Theme.smoke
    signal committed(real v)
    implicitWidth: 170
    implicitHeight: 26
    onMoved: settle.restart()
    onPressedChanged: if (!pressed) { settle.stop(); root.committed(value) }
    Timer { id: settle; interval: 380; onTriggered: root.committed(root.value) }
    background: Rectangle {
        x: root.leftPadding; y: root.topPadding + root.availableHeight / 2 - height / 2
        width: root.availableWidth; height: 6; radius: 3; color: "#262b33"
        Rectangle { width: root.controllerPosition * parent.width; height: parent.height; radius: 3; color: root.accent }
    }
    handle: Rectangle {
        x: root.leftPadding + root.controllerPosition * (root.availableWidth - width)
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width: 18; height: 18; radius: 9
        color: root.pressed ? Qt.lighter(root.accent, 1.25) : "#f1efe9"
        border.width: 2; border.color: root.accent
    }
}
