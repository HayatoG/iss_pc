import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic
import Launcher

FocusScope {
    id: library
    objectName: "trophiesLibrary"
    property bool sectionActive: false
    property bool detailsOpen: false
    property int chosenGame: -1
    property string homeGame: ""
    property bool initialized: false
    property int progressRevision: 0
    signal leaveRequested()
    readonly property var games: { var folder = Backend.gameDir; var language = Backend.lang; var earned = Backend.trophiesEarned; var revision = progressRevision; return Backend.gameOptions }
    function currentGame() { return Backend.gameOptions[Backend.gameIndex].value }
    function openGame(index) {
        if (Backend.playState !== "idle") return
        if (!detailsOpen) homeGame = currentGame()
        chosenGame = index
        Backend.setGame(games[index].value)
        detailsOpen = true
        Qt.callLater(function() { if (typeof ControllerNav !== "undefined") ControllerNav.focusContent(trophyDetails) })
    }
    function backToGames() {
        detailsOpen = false
        progressRevision += 1
        if (homeGame !== "" && currentGame() !== homeGame && Backend.playState === "idle") Backend.setGame(homeGame)
        chosenGame = -1
        var row = gameRows.itemAt(Backend.gameIndex)
        if (sectionActive && row) row.forceActiveFocus()
    }
    onSectionActiveChanged: {
        if (!initialized) return
        if (sectionActive) { homeGame = currentGame(); detailsOpen = false; chosenGame = -1; progressRevision += 1; forceActiveFocus() }
        else backToGames()
    }
    Component.onCompleted: { initialized = true; homeGame = currentGame() }
    Connections {
        target: library.Window.window
        function onClosing(close) {
            if (library.detailsOpen && library.homeGame !== "" && library.currentGame() !== library.homeGame && Backend.playState === "idle") Backend.setGame(library.homeGame)
        }
    }
    Keys.onEscapePressed: { if (detailsOpen) backToGames(); else leaveRequested() }

    Flickable {
        anchors.fill: parent; visible: !library.detailsOpen
        contentWidth: width; contentHeight: gameColumn.implicitHeight + 80
        clip: true; boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { }
        Column {
            id: gameColumn
            x: 48; y: 32; width: parent.width - 96; spacing: 16
            Text { text: Theme.tr("nav_trophies"); color: "white"; font.family: Theme.fontText; font.pixelSize: 36; font.weight: Font.Light }
            Text { text: Backend.lang === "pt_BR" ? "Escolha um jogo para ver os troféus." : "Choose a game to view its trophies."
                   color: "#bad7f4"; font.family: Theme.fontText; font.pixelSize: 15 }
            TrophySummary { width: parent.width; height: 120; games: library.games }
            Item { width: 1; height: 8 }
            Repeater {
                id: gameRows; model: library.games
                FocusScope {
                    id: row
                    width: gameColumn.width; height: 164; activeFocusOnTab: true
                    enabled: Backend.playState === "idle"
                    FocusHalo { active: row.activeFocus }
                    Rectangle {
                        anchors.fill: parent
                        color: row.activeFocus || hover.containsMouse ? "#3c6fa8e0" : "#2a0751a0"
                        border.width: row.activeFocus ? 2 : 1
                        border.color: row.activeFocus ? "white" : "#42b7ddff"
                    }
                    Rectangle {
                        x: 20; anchors.verticalCenter: parent.verticalCenter; width: 104; height: 104; color: "#164a7f"
                        Image { id: icon; anchors.fill: parent; source: modelData.iconSource || ""; fillMode: Image.PreserveAspectCrop; smooth: true; mipmap: true; visible: status === Image.Ready }
                        Icon { anchors.centerIn: parent; visible: icon.status !== Image.Ready; path: Theme.icons.play; size: 40; color: "white" }
                    }
                    Column {
                        x: 152; anchors.verticalCenter: parent.verticalCenter; width: parent.width - 214; spacing: 12
                        Text { width: parent.width; text: modelData.label; color: "white"; font.family: Theme.fontText; font.pixelSize: 26; font.weight: Font.Light; elide: Text.ElideRight }
                        Item {
                            width: parent.width; height: 22
                            Text {
                                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                                text: (modelData.trophyAvailable ? modelData.trophyEarned : "—") + " / " + modelData.trophyTotal + "  ·  " + Theme.tr("nav_trophies")
                                color: "#d5eaff"; font.family: Theme.fontText; font.pixelSize: 15
                            }
                            Text {
                                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                text: modelData.trophyAvailable ? modelData.trophyPercent + "%" + (modelData.trophyPlatinum ? "  ·  Platinum" : "") : "—"
                                color: modelData.trophyPlatinum ? "#ffd16b" : "white"; font.family: Theme.fontText; font.pixelSize: 17; font.weight: Font.Bold
                            }
                        }
                        Rectangle {
                            objectName: "trophyCompletionBar"
                            property real percent: modelData.trophyPercent || 0
                            width: parent.width; height: 9; radius: 4.5; color: "#30ffffff"
                            Rectangle {
                                width: parent.width * Math.max(0, Math.min(100, parent.percent)) / 100
                                height: parent.height; radius: parent.radius
                                color: modelData.trophyPlatinum ? "#ffd16b" : "#79c7ff"
                                Behavior on width { NumberAnimation { duration: Theme.motionDuration } }
                            }
                            Accessible.role: Accessible.ProgressBar
                            Accessible.name: modelData.label + " " + modelData.trophyPercent + "%"
                        }
                    }
                    Icon { anchors.right: parent.right; anchors.rightMargin: 24; anchors.verticalCenter: parent.verticalCenter; size: 22; path: "M9 5l7 7-7 7"; color: "white" }
                    MouseArea { id: hover; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { row.forceActiveFocus(); library.openGame(index) } }
                    Keys.onReturnPressed: library.openGame(index)
                    Keys.onSpacePressed: library.openGame(index)
                    Keys.onDownPressed: if (index + 1 < gameRows.count) gameRows.itemAt(index + 1).forceActiveFocus()
                    Keys.onUpPressed: if (index > 0) gameRows.itemAt(index - 1).forceActiveFocus()
                    Accessible.role: Accessible.Button; Accessible.name: modelData.label
                    Accessible.onPressAction: library.openGame(index)
                }
            }
        }
    }

    Item {
        id: trophyDetails
        anchors.fill: parent; visible: library.detailsOpen
        ConsoleButton {
            x: 36; y: 14; height: 36
            text: Backend.lang === "pt_BR" ? "Todos os jogos" : "All games"
            iconPath: "M15 5l-7 7 7 7"
            onClicked: library.backToGames()
        }
        Text { x: 240; y: 17; width: parent.width - 276; text: library.chosenGame >= 0 ? library.games[library.chosenGame].label : ""
               color: "white"; font.family: Theme.fontText; font.pixelSize: 23; font.weight: Font.Light; elide: Text.ElideRight }
        Loader {
            x: 0; y: 62; width: parent.width; height: parent.height - 62
            active: library.detailsOpen
            sourceComponent: TrophiesPage { }
        }
    }
}
