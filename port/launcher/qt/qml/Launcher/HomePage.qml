import QtQuick
import QtQuick.Dialogs
import Launcher

Item {
    id: page
    signal openFixes()
    signal openSaves()
    property real pulse: 0
    property bool animate: true
    readonly property color accent: Theme.smoke

    SequentialAnimation on pulse {
        running: page.animate && page.visible && !Backend.reducedMotion
        loops: Animation.Infinite
        NumberAnimation { from: 0; to: 1; duration: 1300; easing.type: Easing.InOutSine }
        NumberAnimation { from: 1; to: 0; duration: 1300; easing.type: Easing.InOutSine }
    }

    // ---- hero ------------------------------------------------------------------------------
    Item {
        id: hero
        width: parent.width; height: Theme.heroHeight
        clip: true
        Image { anchors.fill: parent; source: "image://hero/bg"; cache: false }   // neon + rain (drawn in Python)
        Image {   // the player's own pic1.png (read at run time; never in the repo)
            anchors.fill: parent
            source: Backend.artSource
            visible: Backend.artSource !== ""
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            opacity: status === Image.Ready ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 300 } }
        }
        Rectangle {
            anchors.bottom: parent.bottom; width: parent.width; height: 150
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.rgba(16/255, 18/255, 22/255, 0) }
                GradientStop { position: 1; color: Theme.bg }
            }
        }
        Rectangle {   // the data folder has no saves but another place does (typically after an update)
            visible: Saves.homeHint !== ""
            x: 36; y: 18; width: Math.min(parent.width - 72, 640); height: hintCol.implicitHeight + 24
            radius: 10; color: Qt.rgba(0.05, 0.06, 0.07, 0.86); border.width: 1; border.color: Qt.rgba(Theme.ok.r, Theme.ok.g, Theme.ok.b, 0.6)
            Column {
                id: hintCol
                x: 16; y: 12; width: parent.width - 32; spacing: 6
                Text { width: parent.width; wrapMode: Text.WrapAnywhere; text: Saves.homeHint; color: Theme.text
                       font.family: Theme.fontText; font.pixelSize: 13; lineHeight: 1.3; lineHeightMode: Text.ProportionalHeight }
                Text { text: Theme.tr("saves_home_open") + " →"; color: Theme.ok; font.family: Theme.fontText; font.pixelSize: 13; font.weight: Font.Bold
                       MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: page.openSaves() } }
            }
        }
        Column {
            anchors.left: parent.left; anchors.leftMargin: 36
            anchors.bottom: parent.bottom; anchors.bottomMargin: 34
            spacing: 6
            Text { text: Backend.heroKicker; color: page.accent; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 700 }); font.pixelSize: 15
                   font.weight: Font.Bold; font.capitalization: Font.AllUppercase; font.letterSpacing: 4.5 }
            Text { text: Theme.tr("hero_tagline"); color: "#fff"; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 64
                   font.weight: Font.ExtraBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 3.2
                   lineHeight: 0.95; lineHeightMode: Text.ProportionalHeight }
        }
        Column {
            id: actions
            anchors.right: parent.right; anchors.rightMargin: 36
            anchors.bottom: parent.bottom; anchors.bottomMargin: 34
            width: 300; spacing: 10
            Item {
                id: playBox
                width: parent.width; height: 80
                // pulsing glow (soft rings; the mock animates a box-shadow)
                Repeater {
                    model: 4
                    Rectangle {
                        anchors.centerIn: parent
                        width: playBtn.width + 2 * (4 + index * 7) * (0.5 + page.pulse)
                        height: playBtn.height + 2 * (4 + index * 7) * (0.5 + page.pulse)
                        radius: 14 + (4 + index * 7) * (0.5 + page.pulse)
                        color: Qt.rgba(1, 106/255, 43/255, (Backend.canPlay ? 0.075 : 0.0) * (1 - index * 0.2) * (0.6 + page.pulse * 0.8))
                    }
                }
                Rectangle {
                    id: playBtn
                    anchors.fill: parent
                    radius: 14
                    color: !Backend.canPlay ? "#3a2a22" : (playMouse.containsMouse ? Qt.lighter(page.accent, 1.1) : page.accent)
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        x: 24; spacing: 2
                        Text { text: Theme.tr("play_label"); font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 34
                               font.weight: Font.ExtraBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1.7
                               color: Backend.canPlay ? "#160a04" : "#8a6a5a"; lineHeight: 0.95; lineHeightMode: Text.ProportionalHeight }
                        Text { text: Backend.playSubtitle; width: playBtn.width - 48; elide: Text.ElideRight
                               font.family: Theme.fontText; font.pixelSize: 13; font.weight: Font.DemiBold
                               color: Backend.canPlay ? "#160a04" : "#8a6a5a" }
                    }
                    Rectangle {   // shader list progress (first start): thin bar along the button's bottom edge
                        visible: Backend.jobsProgressValue >= 0
                        x: 24; width: playBtn.width - 48; height: 4; radius: 2
                        anchors.bottom: parent.bottom; anchors.bottomMargin: 8
                        color: "#5a3a2a"
                        Rectangle { width: parent.width * Math.max(0, Backend.jobsProgressValue); height: parent.height
                                    radius: 2; color: "#160a04" }
                    }
                    MouseArea { id: playMouse; anchors.fill: parent; hoverEnabled: true
                                enabled: Backend.canPlay; cursorShape: Qt.PointingHandCursor; onClicked: Backend.play() }
                    Accessible.role: Accessible.Button
                    Accessible.name: Theme.tr("play_label")
                }
            }
            GhostButton {
                width: parent.width
                text: Theme.tr("choose_folder")
                onClicked: folderDialog.open()
            }
            GhostButton {   // Optional support: opens the creator page in the system browser.
                width: parent.width
                tone: "primary"; accent: "#FFDD00"
                text: "☕  " + Theme.tr("coffee_label")
                onClicked: Backend.openUrl("coffee")
            }
            Column {   // game language (voice and text): next start; the interface language is separate
                width: parent.width; spacing: 4
                Row {
                    spacing: 10
                    Text { text: Theme.tr("game_language_label"); color: Theme.textMuted; font.family: Theme.fontText; font.pixelSize: 12; font.weight: Font.Bold
                           width: 60; wrapMode: Text.WordWrap; anchors.verticalCenter: parent.verticalCenter }
                    Select {
                        accent: page.accent; width: actions.width - 70
                        model: Backend.gameLanguageOptions
                        currentIndex: Backend.gameLanguageIndex
                        onActivated: (i) => Backend.setGameLanguage(Backend.gameLanguageOptions[i].value)
                    }
                }
                Text { visible: Backend.gameLanguageNow !== ""; width: parent.width; text: Backend.gameLanguageNow; color: page.accent
                       font.family: Theme.fontText; font.pixelSize: 12; font.weight: Font.Bold }
                Text { width: parent.width; text: Theme.tr("game_language_hint"); wrapMode: Text.WordWrap; color: Theme.textSoft
                       font.family: Theme.fontText; font.pixelSize: 12; lineHeight: 1.2; lineHeightMode: Text.ProportionalHeight }
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: Theme.tr("choose_folder_title")
        currentFolder: Backend.gameDir ? "file://" + Backend.gameDir : ""
        onAccepted: Backend.chooseFolder(selectedFolder.toString())
    }

    // ---- state + news ------------------------------------------------------------------------
    Row {
        id: lower
        anchors.top: hero.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left; anchors.right: parent.right
        anchors.leftMargin: 36; anchors.rightMargin: 36
        anchors.topMargin: 22; anchors.bottomMargin: 22
        spacing: 22
        readonly property real avail: width - spacing
        Column {
            id: stateCol
            width: lower.avail * 1.25 / 2.25; spacing: 14
            Text { text: Backend.statusTitle; color: Theme.textMuted; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 700 }); font.pixelSize: 18
                   font.weight: Font.Bold; font.capitalization: Font.AllUppercase; font.letterSpacing: 0.9 }
            Flow {
                width: parent.width; spacing: 10
                Repeater { model: Backend.chips; Chip { text: modelData.text; level: modelData.level } }
            }
            Column {   // light glare (occlusion): same file/env path as the other switches
                visible: Backend.advancedMode   // automatic setup: hidden (hardware.py)
                width: parent.width; spacing: 6
                Row {
                    spacing: 12
                    Text { text: Theme.tr("occlusion"); color: Theme.textMuted; font.family: Theme.fontText; font.pixelSize: 13; font.weight: Font.Bold
                           anchors.verticalCenter: parent.verticalCenter }
                    Select {
                        accent: page.accent; width: 262
                        model: Backend.occlusionOptions
                        currentIndex: Backend.occlusionIndex
                        onActivated: (i) => Backend.setOcclusion(Backend.occlusionOptions[i].value)
                    }
                }
                Text { width: parent.width; text: Theme.tr("occlusion_hint"); wrapMode: Text.WordWrap; color: Theme.textDim
                       font.family: Theme.fontText; font.pixelSize: 12; lineHeight: 1.25; lineHeightMode: Text.ProportionalHeight }
            }
            Column {   // detailed log (ISS_VERBOSE_LOG), right under the light glare selector
                visible: Backend.advancedMode
                width: parent.width; spacing: 6
                Row {
                    spacing: 12
                    Text { text: Theme.tr("verbose_log"); color: Theme.textMuted; font.family: Theme.fontText; font.pixelSize: 13; font.weight: Font.Bold
                           anchors.verticalCenter: parent.verticalCenter }
                    Toggle {
                        accent: page.accent
                        checked: Backend.verboseLog
                        onToggled: (v) => Backend.setVerboseLog(v)
                    }
                }
                Text { width: parent.width; text: Theme.tr("verbose_log_hint"); wrapMode: Text.WordWrap; color: Theme.textDim
                       font.family: Theme.fontText; font.pixelSize: 12; lineHeight: 1.25; lineHeightMode: Text.ProportionalHeight }
            }
            Item {
                visible: Backend.showFirstRun || Backend.notice !== ""
                width: parent.width; height: warnText.implicitHeight
                Rectangle { width: 3; height: parent.height; color: Theme.warn }
                Text { id: warnText; x: 15; width: parent.width - 15; wrapMode: Text.WordWrap
                       text: Backend.notice !== "" ? Backend.notice : Theme.tr("first_run_warning")
                       color: Theme.textDim; font.family: Theme.fontText; font.pixelSize: 13; lineHeight: 1.4
                       lineHeightMode: Text.ProportionalHeight }
            }
        }
        Rectangle {
            width: lower.avail * 1.0 / 2.25; height: parent.height
            radius: 14; color: Theme.card; border.width: 1; border.color: Theme.cardLine
            Column {
                x: 18; y: 16; width: parent.width - 36; spacing: 10
                Text { text: Theme.tr("news_title") + (Backend.version ? " · " + Backend.version : "")
                       color: "#fff"; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 700 }); font.pixelSize: 18; font.weight: Font.Bold
                       font.capitalization: Font.AllUppercase; font.letterSpacing: 0.9 }
                Column {
                    width: parent.width; spacing: 3
                    Repeater {
                        model: Backend.newsLines
                        Row {
                            width: parent.width; spacing: 8
                            Text { text: "•"; color: Theme.textSoft; font.pixelSize: 14; font.family: Theme.fontText }
                            Text { width: parent.width - 20; text: modelData; color: Theme.textSoft; wrapMode: Text.WordWrap
                                   font.family: Theme.fontText; font.pixelSize: 14; lineHeight: 1.3; lineHeightMode: Text.ProportionalHeight }
                        }
                    }
                }
            }
            Text {
                visible: Backend.advancedMode   // opens the Fixes screen, hidden in the automatic setup
                anchors.left: parent.left; anchors.leftMargin: 18; anchors.bottom: parent.bottom; anchors.bottomMargin: 16
                text: Theme.tr("news_link"); color: page.accent; font.family: Theme.fontText
                font.pixelSize: 14; font.weight: Font.Bold
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: page.openFixes() }
            }
        }
    }

    // ---- game log, only while the game runs or after it failed --------------------------------
    Rectangle {
        visible: Backend.playState !== "idle" || (Backend.logText !== "" && Backend.logShown)
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        height: 140; color: "#0a0b0e"; border.width: 1; border.color: Theme.cardLine
        Flickable {
            id: flick
            anchors.fill: parent; anchors.margins: 10; clip: true
            contentHeight: logText.implicitHeight
            onContentHeightChanged: contentY = Math.max(0, contentHeight - height)
            Text { id: logText; width: flick.width; text: Backend.logText; color: Theme.textDim
                   font.family: "monospace"; font.pixelSize: 11; wrapMode: Text.WrapAnywhere }
        }
    }
}
