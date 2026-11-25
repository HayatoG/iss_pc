import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import Launcher

FocusScope {
    id: page
    objectName: "consoleHome"
    property bool animate: true
    property bool optionsOpen: false
    readonly property var controllerOptionsRoot: options.contentItem
    signal openFixes()
    signal openSaves()
    signal openTrophies()
    signal openControls()
    signal navigationRequested()
    readonly property real margin: width < 1150 ? 40 : 64
    readonly property var gameOptions: { var folder = Backend.gameDir; var language = Backend.lang; var earned = Backend.trophiesEarned; return Backend.gameOptions }
    readonly property string gameTitle: gameOptions[Backend.gameIndex].label
    readonly property string startLabel: Backend.lang === "pt_BR" ? "Iniciar" : "Start"
    readonly property string optionsLabel: Backend.lang === "pt_BR" ? "Opções" : "Options"
    readonly property real tileSize: height < 550 ? 120 : height < 650 ? 168 : 242
    readonly property color labelColor: "#ffffff"
    readonly property color controlAccent: "#a9afb9"
    function selectGame(index) {
        if (Backend.playState !== "idle") return
        if (Backend.gameIndex !== index) Backend.setGame(page.gameOptions[index].value)
        gameRepeater.itemAt(index).forceActiveFocus()
    }
    function controllerItems() { return [gameRepeater.itemAt(0), gameRepeater.itemAt(1), savesTile, controlsTile, optionsButton, folderButton, coffeeButton, trophySummary] }
    function focusControllerItem(index) {
        var items = controllerItems()
        if (index < 2) selectGame(index)
        else if (items[index]) items[index].forceActiveFocus()
    }
    function controllerAction(action) {
        var items = controllerItems(), index = Backend.gameIndex
        for (var i = 0; i < items.length; ++i) if (items[i] && items[i].activeFocus) { index = i; break }
        if (action === "options") { optionsOpen = true; return }
        if (action === "back") { navigationRequested(); return }
        if (action === "up") { if (index < 4) navigationRequested(); else focusControllerItem(index - 4); return }
        if (action === "down") { focusControllerItem(index < 4 ? index + 4 : index); return }
        if (action === "left" || action === "right") {
            var lo = index < 4 ? 0 : 4, hi = index < 4 ? 3 : 7
            focusControllerItem(Math.max(lo, Math.min(hi, index + (action === "left" ? -1 : 1)))); return
        }
        if (action === "confirm") {
            if (index < 2) activateGame()
            else if (index === 2) openSaves()
            else if (index === 3) openControls()
            else if (index === 4) optionsOpen = true
            else if (index === 5) folderDialog.open()
            else if (index === 6) Backend.openUrl("coffee")
            else openTrophies()
        }
    }
    function activateGame() { if (Backend.canPlay) Backend.play(); else optionsOpen = true }
    onVisibleChanged: if (visible) forceActiveFocus()
    Component.onCompleted: forceActiveFocus()
    Connections {
        target: Backend
        function onGameDirChanged() {
            var tile = gameRepeater.itemAt(Backend.gameIndex)
            if (tile && page.visible && !page.optionsOpen) tile.forceActiveFocus()
        }
    }
    Keys.onLeftPressed: selectGame(Math.max(0, Backend.gameIndex - 1))
    Keys.onRightPressed: selectGame(Math.min(1, Backend.gameIndex + 1))
    Keys.onUpPressed: navigationRequested()
    Keys.onReturnPressed: activateGame()
    Keys.onMenuPressed: optionsOpen = true
    Keys.onEscapePressed: { if (optionsOpen) optionsOpen = false; else navigationRequested() }

    Flickable {
        id: gameStrip
        x: page.margin; y: 44; width: page.width - 2 * page.margin
        height: page.tileSize + 64
        contentWidth: tiles.width + 12; contentHeight: height
        clip: true; boundsBehavior: Flickable.StopAtBounds
        Row {
            id: tiles; x: 6; y: 6; spacing: 20
            Repeater {
                id: gameRepeater; model: 2
                ConsoleTile {
                    objectName: "homeGameTile_" + index
                    readonly property var optionData: page.gameOptions[index] || ({})
                    game: true; selected: Backend.gameIndex === index
                    width: selected ? page.tileSize : page.tileSize * 0.72
                    title: optionData.label || ""
                    artwork: optionData.iconSource || ""
                    actionLabel: Backend.canPlay ? page.startLabel : page.optionsLabel
                    onSelectedByUser: page.selectGame(index)
                    onActivated: page.activateGame()
                }
            }
            ConsoleTile {
                width: page.tileSize * 0.58
                id: savesTile; title: Theme.tr("nav_saves"); iconPath: Theme.icons.saves
                onSelectedByUser: page.openSaves(); onActivated: page.openSaves()
            }
            ConsoleTile {
                width: page.tileSize * 0.58
                id: controlsTile; title: Theme.tr("nav_controls"); iconPath: Theme.icons.controls
                onSelectedByUser: page.openControls(); onActivated: page.openControls()
            }
        }
    }

    Column {
        id: details
        x: page.margin + 6; y: gameStrip.y + gameStrip.height + 22
        width: Math.min(860, page.width - 2 * page.margin); spacing: 12
        Text {
            width: parent.width; text: page.gameTitle; color: "white"
            font.family: Theme.fontText; font.pixelSize: page.height < 650 ? 34 : 43; font.weight: Font.Light
            fontSizeMode: Text.HorizontalFit; minimumPixelSize: 28; elide: Text.ElideRight
        }
        Text { text: Backend.heroKicker; color: "#c6e0fc"; font.family: Theme.fontText; font.pixelSize: 16; font.letterSpacing: 2 }
        Row {
            spacing: 10
            Rectangle { width: 7; height: 7; radius: 4; color: Backend.ready ? Theme.ok : Theme.warn; anchors.verticalCenter: parent.verticalCenter }
            Text { text: Backend.statusTitle; color: "white"; font.family: Theme.fontText; font.pixelSize: 14; anchors.verticalCenter: parent.verticalCenter }
            Text { text: "·  " + Backend.playSubtitle; color: "#c6e0fc"; font.family: Theme.fontText; font.pixelSize: 14; anchors.verticalCenter: parent.verticalCenter
                   width: Math.min(600, details.width - 160); elide: Text.ElideRight }
        }
        Row {
            spacing: 12
            ConsoleButton { id: optionsButton; height: 48; text: page.optionsLabel; iconPath: Theme.icons.advanced; onClicked: page.optionsOpen = true }
            ConsoleButton { id: folderButton; height: 48; text: Theme.tr("choose_folder"); iconPath: Theme.icons.saves; onClicked: folderDialog.open() }
            GhostButton {
                id: coffeeButton
                width: 210; height: 48; glossy: true; cornerRadius: 2
                tone: "primary"; accent: "#FFDD00"
                text: "☕  " + Theme.tr("coffee_label")
                onClicked: Backend.openUrl("coffee")
            }
        }
        Text { visible: Backend.notice !== ""; text: Backend.notice; width: parent.width; wrapMode: Text.WordWrap; color: Theme.warn; font.family: Theme.fontText; font.pixelSize: 13 }
        Rectangle {
            visible: Backend.jobsProgressValue >= 0
            width: parent.width; height: 4; color: "#25ffffff"
            Rectangle { height: parent.height; width: parent.width * Math.max(0, Backend.jobsProgressValue); color: "white" }
        }
    }

    TrophySummary {
        id: trophySummary
        readonly property bool beside: page.width >= 1250
        x: page.width - page.margin - width
        y: beside ? details.y + 12 : 44
        width: 330
        height: 154
        games: page.gameOptions; interactive: true
        onActivated: page.openTrophies()
    }

    Rectangle {
        id: lower
        x: page.margin; width: page.width - 2 * page.margin; height: 96
        anchors.bottom: parent.bottom; anchors.bottomMargin: 24
        visible: Math.max(details.y + details.height, trophySummary.y + trophySummary.height) + 22 < y
        color: "#3005224e"; border.width: 1; border.color: "#25ffffff"
        Row {
            x: 22; anchors.verticalCenter: parent.verticalCenter; spacing: 38
            Column {
                spacing: 5; width: Math.max(160, lower.width * 0.24)
                Text { text: Theme.tr("nav_trophies"); color: "#a9ccee"; font.family: Theme.fontText; font.pixelSize: 12; font.letterSpacing: 1.2 }
                Text { text: Backend.trophiesEarned + " / " + Backend.trophies.length; color: "white"; font.family: Theme.fontText; font.pixelSize: 22; font.weight: Font.Light }
            }
            Column {
                spacing: 5; width: Math.max(160, lower.width * 0.24)
                Text { text: Theme.tr("home_fps_limit"); color: "#a9ccee"; font.family: Theme.fontText; font.pixelSize: 12; font.letterSpacing: 1.2 }
                Text { text: Backend.fpsLimitOptions[Backend.fpsLimitIndex].label; color: "white"; font.family: Theme.fontText; font.pixelSize: 22; font.weight: Font.Light }
            }
            Column {
                spacing: 5; width: Math.max(160, lower.width * 0.24)
                Text { text: Theme.tr("upscaler_label"); color: "#a9ccee"; font.family: Theme.fontText; font.pixelSize: 12; font.letterSpacing: 1.2 }
                Text { text: Backend.upscalerOptions[Backend.upscalerIndex].label; color: "white"; font.family: Theme.fontText; font.pixelSize: 22; font.weight: Font.Light }
            }
        }
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: page.optionsOpen = true }
    }

    FolderDialog {
        id: folderDialog; title: Theme.tr("choose_folder_title")
        currentFolder: Backend.gameDir ? "file://" + Backend.gameDir : ""
        onAccepted: Backend.chooseFolder(selectedFolder.toString())
    }
    Drawer {
        id: options
        objectName: "gameOptionsDrawer"
        focus: true
        onOpened: if (typeof ControllerNav !== "undefined") ControllerNav.focusContent(options.contentItem)
        parent: Overlay.overlay
        edge: Qt.RightEdge; width: Math.min(1200, page.width * 0.96); height: parent ? parent.height : page.height
        visible: page.optionsOpen; modal: true; dim: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onClosed: { page.optionsOpen = false; Qt.callLater(function() { page.focusControllerItem(Backend.gameIndex) }) }
        background: Rectangle { color: "#071d3a"; border.width: 1; border.color: "#507aab" }
        contentItem: Item {
            Text { x: 28; y: 16; width: parent.width - 176; text: page.optionsLabel + " · " + page.gameTitle; color: "white"; font.family: Theme.fontText; font.pixelSize: 24; elide: Text.ElideRight }
            ConsoleButton { objectName: "gameOptionsClose"; property bool controllerHeader: true; anchors.right: parent.right; anchors.rightMargin: 24; y: 12; height: 36; text: Backend.lang === "pt_BR" ? "Fechar" : "Close"; onClicked: page.optionsOpen = false }
            Loader {
                x: 0; y: 64; width: parent.width; height: parent.height - 64
                active: options.visible
                sourceComponent: HomePage {
                    animate: page.animate
                    onOpenFixes: { page.optionsOpen = false; page.openFixes() }
                    onOpenSaves: { page.optionsOpen = false; page.openSaves() }
                    onOpenTrophies: { page.optionsOpen = false; page.openTrophies() }
                }
            }
        }
    }
}
