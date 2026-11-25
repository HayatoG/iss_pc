import QtQuick
import QtQuick.Controls.Basic
import Launcher

ComboBox {
    id: root
    activeFocusOnTab: true
    property string controllerKind: "choice"
    property bool controllerEditing: false
    property int controllerDraft: 0
    property var controllerFocusTarget: null
    function controllerInput(action) {
        if (!controllerEditing) {
            if (action !== "confirm" || count === 0) return
            controllerDraft = currentIndex; controllerEditing = true; popup.open(); forceActiveFocus(Qt.TabFocusReason); return
        }
        if (action === "back") { controllerEditing = false; popup.close(); return }
        if (action === "confirm") {
            var selected = controllerDraft; controllerEditing = false; currentIndex = selected; popup.close(); activated(selected); return
        }
        if (action === "up" || action === "left") controllerDraft = Math.max(0, controllerDraft - 1)
        if (action === "down" || action === "right") controllerDraft = Math.min(count - 1, controllerDraft + 1)
        popup.contentItem.positionViewAtIndex(controllerDraft, ListView.Contain)
    }
    FocusHalo { active: root.activeFocus || root.controllerEditing }
    property color accent: Theme.smoke
    implicitWidth: 220
    implicitHeight: 36
    // The mouse wheel scrolls the page under it instead of changing the choice (scrolling past the
    // Game list on the Play tab switched the game).
    wheelEnabled: false
    textRole: "label"
    font.family: Theme.fontText
    font.pixelSize: 14
    font.weight: Font.DemiBold
    background: GlassSurface {
        radius: height / 2; blurEnabled: false; tint: Qt.darker(root.accent, 4.5); density: 0.58; accent: root.accent
        highlighted: root.hovered || root.popup.visible
        focused: root.visualFocus
    }
    contentItem: Text {
        leftPadding: 12; rightPadding: 28
        text: root.displayText; color: Theme.text; font: root.font
        verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
    }
    indicator: Icon { path: "M6 9l6 6 6-6"; color: Theme.textMuted; size: 14
                      x: root.width - width - 12; anchors.verticalCenter: parent.verticalCenter }
    delegate: ItemDelegate {
        width: root.width
        highlighted: (root.controllerEditing ? root.controllerDraft : root.highlightedIndex) === index
        contentItem: Text { text: modelData.label !== undefined ? modelData.label : modelData
                            color: Theme.text; font: root.font; elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter }
        background: Rectangle { radius: 6; color: highlighted ? "#754e9ed4" : "transparent"; border.width: highlighted ? 2 : 0; border.color: "#bceaff" }
    }
    popup: Popup {
        focus: !root.controllerEditing
        onClosed: root.controllerEditing = false
        y: root.height + 6; width: root.width; padding: 6
        implicitHeight: Math.min(contentItem.implicitHeight + 12, 260)
        contentItem: ListView { clip: true; implicitHeight: contentHeight; model: root.popup.visible ? root.delegateModel : null
                                currentIndex: root.controllerEditing ? root.controllerDraft : root.highlightedIndex }
        background: GlassSurface { radius: 16; blurEnabled: false; tint: Qt.darker(root.accent, 5.5); density: 0.96; accent: root.accent }
    }
}
