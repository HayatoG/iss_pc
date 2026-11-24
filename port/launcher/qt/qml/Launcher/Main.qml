import QtQuick
import QtQuick.Controls.Basic
import Launcher

Rectangle {
    id: root
    objectName: "consoleMain"
    property var homeItem: null
    color: Theme.bg
    implicitWidth: 1120
    implicitHeight: 700
    property int current: 0
    // The rail's order (Rail.qml entries): Play, Trophies, Mods, Graphics, Controls (with the Controller
    // guides inside since 2026-11-22), Saves & data, Updates, Support, Advanced, Fixes. Advanced and
    // Fixes show only with "Advanced settings" on (Backend.showAdvanced).
    readonly property var pages: ["play", "trophies", "mods", "graphics", "controls", "saves", "updates", "support", "advanced", "fixes"]
    readonly property var advancedPages: ["advanced", "fixes"]
    property bool animate: true
    Binding { target: Theme; property: "surfaceAccent"; value: "#82baff" }

    function controllerPopupOpen() {
        var layer = Overlay.overlay
        if (!layer) return false
        for (var i = 0; i < layer.children.length; ++i) {
            var item = layer.children[i]
            if (item.visible && item.width > 0 && item.height > 0) return true
        }
        return false
    }
    function controllerAction(action) {
        if (Backend.playState !== "idle") return
        if (experimental.open) { ControllerNav.navigate(experimental, action); return }
        if (homeItem && homeItem.optionsOpen) {
            if (action !== "next_tab" && action !== "previous_tab") ControllerNav.navigate(homeItem.controllerOptionsRoot, action)
            return
        }
        var popup = controllerPopupOpen()
        if (action === "next_tab" || action === "previous_tab") {
            if (popup || ControllerNav.editing) return
            var count = Backend.showAdvanced ? pages.length : pages.length - advancedPages.length
            current = (current + (action === "next_tab" ? 1 : -1) + count) % count
            if (current === 0 && homeItem) homeItem.focusControllerItem(Backend.gameIndex)
            else Qt.callLater(function() { ControllerNav.focusContent(stage) })
            return
        }
        if (!popup && action === "down" && rail.controllerOnTab()) {
            if (current === 0 && homeItem) homeItem.focusControllerItem(Backend.gameIndex)
            else ControllerNav.focusContent(stage)
            return
        }
        if (!popup && rail.controllerHasFocus()) { ControllerNav.navigate(rail, action); return }
        if (!popup && current === 0 && homeItem) homeItem.controllerAction(action)
        else ControllerNav.navigate(popup ? Overlay.overlay : stage, action)
    }
    Connections {
        target: typeof ControllerNav === "undefined" ? null : ControllerNav
        function onScroll(delta) { if (homeItem && homeItem.optionsOpen) ControllerNav.scrollPage(homeItem.controllerOptionsRoot, delta); else if (!root.controllerPopupOpen() && !experimental.open) ControllerNav.scrollPage(stage, delta) }
        function onAction(action) { root.controllerAction(action) }
        function onBoundary(action) { if (action === "up" && !root.controllerPopupOpen() && !(homeItem && homeItem.optionsOpen) && !rail.controllerHasFocus()) rail.focusCurrent() }
    }

    Rectangle {
        z: 500
        readonly property rect focusBox: typeof ControllerNav === "undefined" ? Qt.rect(0,0,0,0) : ControllerNav.focusRect
        x: focusBox.x - 4; y: focusBox.y - 4; width: focusBox.width + 8; height: focusBox.height + 8
        visible: focusBox.width > 0 && focusBox.height > 0
        color: "#124faaf0"; radius: 8; border.width: 3; border.color: "#d6f1ff"
        Rectangle { anchors.fill: parent; anchors.margins: -4; radius: 12; color: "transparent"; border.width: 4; border.color: "#3552bdff" }
    }

    ConsoleBackdrop {
        id: atmosphere
        anchors.fill: parent
        artwork: root.current === 0
    }
    Component.onCompleted: { Theme.glassBackdrop = atmosphere; Theme.glassRevision = width + height }
    onWidthChanged: Theme.glassRevision += 1
    onHeightChanged: Theme.glassRevision += 1
    onCurrentChanged: { Theme.glassRevision += 1; if (typeof ControllerNav !== "undefined") ControllerNav.resetFocus() }

    Rail {
        id: rail
        modalBlocked: !!(homeItem && homeItem.optionsOpen)
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        height: 112; z: 20
        current: root.current
        hiddenKeys: Backend.showAdvanced ? [] : root.advancedPages
        onPick: (i) => root.current = i
        onHomeRequested: { root.current = 0; root.forceActiveFocus() }
        onAdvancedRequested: (on) => { if (on) experimental.open = true; else Backend.setShowAdvanced(false) }
    }
    // leaving Advanced settings while on one of its tabs goes back to Play
    Connections {
        target: Backend
        function onShowAdvancedChanged() {
            if (!Backend.showAdvanced && root.advancedPages.indexOf(root.pages[root.current]) >= 0) root.current = 0
        }
    }
    Item {
        id: stage; objectName: "consoleStage"
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: rail.bottom; anchors.bottom: footer.top
        clip: true
        Repeater {
            model: root.pages.length
            Item {
                id: slot
                readonly property bool on: root.current === index
                // A page is built the first time it is shown and then kept (its scroll and state stay);
                // building all ten at start made every state change reach ten pages' bindings.
                property bool visited: false
                onOnChanged: if (on) visited = true
                Component.onCompleted: if (on) visited = true
                width: stage.width; height: stage.height
                property bool controllerActive: on
                opacity: 1
                visible: on
                enabled: on
                y: 0
                // Backdrop of every tab but Play (2026-11-23, the Play tab's depth everywhere): a deep
                // top-to-bottom shade and a soft glow of the tab's accent in the top right corner.
                Rectangle {
                    anchors.fill: parent
                    visible: root.pages[index] !== "play" && slot.visited
                    gradient: Gradient {
                        GradientStop { position: 0; color: "#30151820" }
                        GradientStop { position: 1; color: "#a00c0e12" }
                    }
                    Canvas {
                        anchors.fill: parent
                        readonly property color glow: Theme.accents[root.pages[index]] || Theme.smoke
                        onPaint: {
                            var ctx = getContext("2d"); ctx.reset()
                            var r = Math.max(width, height) * 0.75
                            var g = ctx.createRadialGradient(width, 0, 0, width, 0, r)
                            g.addColorStop(0, Qt.rgba(glow.r, glow.g, glow.b, 0.16))
                            g.addColorStop(0.45, Qt.rgba(glow.r, glow.g, glow.b, 0.05))
                            g.addColorStop(1, Qt.rgba(glow.r, glow.g, glow.b, 0))
                            ctx.fillStyle = g; ctx.fillRect(0, 0, width, height)
                        }
                        onWidthChanged: requestPaint(); onHeightChanged: requestPaint()
                    }
                }
                Loader {
                    anchors.fill: parent
                    active: slot.visited
                    sourceComponent: { var k = root.pages[index]
                        return k === "play" ? home : k === "graphics" ? graphics : k === "controls" ? controls
                             : k === "fixes" ? fixes : k === "saves" ? saves
                             : k === "support" ? support : k === "updates" ? updates : k === "advanced" ? advanced
                             : k === "mods" ? mods : k === "trophies" ? trophiesPage : soon }
                    onLoaded: if (item.key !== undefined) item.key = root.pages[index]
                }
            }
        }
    }
    Component { id: home; ConsoleHome { Component.onCompleted: root.homeItem = this; animate: root.animate; onOpenFixes: root.current = root.pages.indexOf("fixes"); onOpenSaves: root.current = root.pages.indexOf("saves")
                                         onOpenTrophies: root.current = root.pages.indexOf("trophies"); onOpenControls: root.current = root.pages.indexOf("controls")
                                         onNavigationRequested: rail.focusCurrent() } }
    Item {
        id: footer; anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 34
        Text { x: 40; anchors.verticalCenter: parent.verticalCenter; text: (typeof ControllerNav !== "undefined" && ControllerNav.editing) ? (Backend.lang === "pt_BR" ? "D-pad / Analógico  Ajustar    A / ×  Aplicar    B / ○  Cancelar" : "D-pad / Stick  Browse or adjust    A / ×  Apply    B / ○  Cancel") : Backend.lang === "pt_BR" ? "D-pad / Analógico esquerdo  Navegar    Analógico direito  Rolar    A / ×  Confirmar    B / ○  Voltar    LB / L1 · RB / R1  Abas" : "D-pad / Left stick  Navigate    Right stick  Scroll    A / ×  Confirm    B / ○  Back    LB / L1 · RB / R1  Tabs"
               color: "#bbd8f5"; font.family: Theme.fontText; font.pixelSize: 12 }
        Text { anchors.right: parent.right; anchors.rightMargin: 40; anchors.verticalCenter: parent.verticalCenter
               text: Theme.tr("footer_credit") + " · " + Theme.tr("footer_codev"); color: "#d0e0f2"; font.family: Theme.fontText; font.pixelSize: 14; font.weight: Font.Medium }
    }
    Keys.onEscapePressed: root.current = 0
    Keys.onLeftPressed: (event) => { if (root.current === 0 && Backend.playState === "idle" && !root.controllerPopupOpen()) Backend.setGame(Backend.gameOptions[0].value); else event.accepted = false }
    Keys.onRightPressed: (event) => { if (root.current === 0 && Backend.playState === "idle" && !root.controllerPopupOpen()) Backend.setGame(Backend.gameOptions[1].value); else event.accepted = false }
    Keys.onReturnPressed: (event) => { if (root.current === 0 && Backend.canPlay && !root.controllerPopupOpen()) Backend.play(); else event.accepted = false }
    Component { id: trophiesPage; ConsoleTrophies { sectionActive: root.current === root.pages.indexOf("trophies"); onLeaveRequested: root.current = 0 } }
    Component { id: graphics; GraphicsPage { } }
    Component { id: controls; ControlsHub { } }   // controller settings + Controller guides
    Component { id: fixes; FixesPage { } }
    Component { id: advanced; AdvancedPage { } }
    Component { id: mods; ModsPage { sectionActive: root.current === root.pages.indexOf("mods") } }
    Component { id: saves; SavesPage { } }
    Component { id: support; SupportPage { } }
    Component { id: updates; UpdatesPage { } }
    Component { id: soon; SoonPage { } }

    // "Experimental options" notice, shown when Advanced settings is switched on; OK shows the tabs.
    Rectangle {
        id: experimental
        property bool open: false
        onOpenChanged: if (open && typeof ControllerNav !== "undefined") Qt.callLater(function() { ControllerNav.focusContent(experimental) })
        anchors.fill: parent
        visible: open
        z: 100
        color: Qt.rgba(0, 0, 0, 0.62)
        MouseArea { anchors.fill: parent; hoverEnabled: true }   // the page behind stays still
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(480, parent.width - 80); height: noteCol.implicitHeight + 48
            radius: 18
            gradient: Gradient {
                GradientStop { position: 0; color: "#232730" }
                GradientStop { position: 1; color: "#14161b" }
            }
            border.width: 1; border.color: Qt.rgba(Theme.warn.r, Theme.warn.g, Theme.warn.b, 0.55)
            Rectangle {   // highlight on the top edge
                x: 18; y: 1; width: parent.width - 36; height: 1
                gradient: Gradient { orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: Qt.rgba(1, 1, 1, 0) }
                    GradientStop { position: 0.5; color: Qt.rgba(1, 1, 1, 0.25) }
                    GradientStop { position: 1; color: Qt.rgba(1, 1, 1, 0) } }
            }
            Column {
                id: noteCol
                x: 24; y: 24; width: parent.width - 48; spacing: 14
                Row {
                    spacing: 10
                    Rectangle { width: 10; height: 10; radius: 5; color: Theme.warn; anchors.verticalCenter: parent.verticalCenter }
                    Text { text: Theme.tr("adv_tabs_title"); color: Theme.warn; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 })
                           font.pixelSize: 26; font.weight: Font.ExtraBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1.2 }
                }
                Text { width: parent.width; text: Theme.tr("adv_tabs_body"); wrapMode: Text.WordWrap; color: Theme.textSoft
                       font.family: Theme.fontText; font.pixelSize: 15; lineHeight: 1.35; lineHeightMode: Text.ProportionalHeight }
                GhostButton {
                    anchors.right: parent.right
                    width: 120; glossy: true; tone: "primary"; accent: Theme.warn
                    text: Theme.tr("adv_tabs_ok")
                    onClicked: { experimental.open = false; Backend.setShowAdvanced(true) }
                }
            }
        }
    }
}
