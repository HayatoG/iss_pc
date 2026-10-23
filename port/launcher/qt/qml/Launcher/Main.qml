import QtQuick
import QtQuick.Controls.Basic
import Launcher

Rectangle {
    id: root
    color: Theme.bg
    implicitWidth: 1120
    implicitHeight: 700
    property int current: 0
    // Graphics, Fixes and Advanced only in the hidden advanced mode (Ctrl+Shift+A or ISS_ADVANCED=1):
    // otherwise the launcher sets the graphics up for the hardware by itself (hardware.py).
    readonly property var pages: Backend.advancedMode
        ? ["play", "graphics", "controls", "fixes", "saves", "guides", "support", "updates", "advanced", "mods"]
        : ["play", "controls", "saves", "guides", "support", "updates", "mods"]
    onPagesChanged: current = 0
    Shortcut { sequence: "Ctrl+Shift+A"; onActivated: Backend.toggleAdvanced() }
    property bool animate: true

    Rail {
        id: rail
        anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
        current: root.current
        keys: root.pages
        onPick: (i) => root.current = i
    }
    Item {
        id: stage
        anchors.left: rail.right; anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
        clip: true
        Repeater {
            model: root.pages.length
            Item {
                id: slot
                readonly property bool on: root.current === index
                width: stage.width; height: stage.height
                opacity: on ? 1 : 0
                visible: opacity > 0.01
                y: on ? 0 : 14
                Behavior on opacity { enabled: root.animate; NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                Behavior on y { enabled: root.animate; NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
                Loader {
                    anchors.fill: parent
                    sourceComponent: { var k = root.pages[index]
                        return k === "play" ? home : k === "graphics" ? graphics : k === "controls" ? controls
                             : k === "fixes" ? fixes : k === "saves" ? saves : k === "guides" ? guides
                             : k === "support" ? support : k === "updates" ? updates : k === "advanced" ? advanced
                             : k === "mods" ? mods : soon }
                    onLoaded: if (item.key !== undefined) item.key = root.pages[index]
                }
            }
        }
    }
    Component { id: home; HomePage { animate: root.animate; onOpenFixes: { var i = root.pages.indexOf("fixes"); if (i >= 0) root.current = i }
                                    onOpenSaves: root.current = root.pages.indexOf("saves") } }
    Component { id: graphics; GraphicsPage { } }
    Component { id: controls
        ControlsPage { guidesShown: root.pages.indexOf("guides") >= 0
                       onOpenGuides: root.current = root.pages.indexOf("guides") } }
    Component { id: fixes; FixesPage { } }
    Component { id: advanced; AdvancedPage { } }
    Component { id: mods; ModsPage { } }
    Component { id: saves; SavesPage { } }
    Component { id: support; SupportPage { } }
    Component { id: updates; UpdatesPage { } }
    Component { id: guides; GuidesPage { } }
    Component { id: soon; SoonPage { } }
}
