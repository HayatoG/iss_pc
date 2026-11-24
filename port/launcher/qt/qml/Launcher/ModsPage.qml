import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic
import Launcher

Item {
    id: page
    objectName: "modsLibrary"
    property bool sectionActive: false
    property bool initialized: false
    property int selectedGame: -1
    property string homeGame: ""
    readonly property var games: { var language = Backend.lang; var folder = Backend.gameDir; return Backend.gameOptions }
    function currentGame() { return Backend.gameOptions[Backend.gameIndex].value }
    function selectGame(index) {
        if (!sectionActive || Backend.playState !== "idle" || index < 0 || index >= games.length) return
        selectedGame = index
        if (currentGame() !== games[index].value) Backend.setGame(games[index].value)
        list.positionViewAtBeginning()
    }
    function restoreHomeGame() {
        if (homeGame !== "" && currentGame() !== homeGame && Backend.playState === "idle") Backend.setGame(homeGame)
    }
    function enterSection() {
        homeGame = currentGame()
        selectGame(selectedGame < 0 ? Backend.gameIndex : selectedGame)
    }
    onSectionActiveChanged: {
        if (!initialized) return
        if (sectionActive) enterSection()
        else restoreHomeGame()
    }
    Component.onCompleted: { initialized = true; if (sectionActive) enterSection() }
    Connections {
        target: page.Window.window
        function onClosing(close) { if (page.sectionActive) page.restoreHomeGame() }
    }
    readonly property color accent: Theme.accents.mods
    // Credits under a mod, clickable, in the Graphics page's preset style ("By {name} ↗").
    // key: the mod's selection key (mods.json); text: a strings.json key with {name}.
    readonly property var credits: ({
        "iss_video_dash_endless.xml/Endless video dash":
            { "text": "gfx_preset_by", "name": "sunnyGeezy", "url": "https://github.com/sunnyGeezy" },
        "iss_builtin_intro.xml/Skip intro movie":
            { "text": "mods_fixed_by", "name": "sunnyGeezy", "url": "https://github.com/sunnyGeezy",
              "original": "junminlee2004" },
        "iss_builtin_intro.xml/Disable motion blur":
            { "text": "gfx_preset_by", "name": "sunnyGeezy", "url": "https://github.com/sunnyGeezy" },
        "native/camera-speed-1.5x":
            { "text": "gfx_preset_by", "name": "sunnyGeezy", "url": "https://github.com/sunnyGeezy" },
        "iss_godmode.xml/God mode":
            { "text": "mods_ported_by", "name": "sunnyGeezy", "url": "https://github.com/sunnyGeezy",
              "original": "Weysincha" },
        "iss_infinite_energy.xml/Unlimited energy":
            { "text": "mods_ported_by", "name": "sunnyGeezy", "url": "https://github.com/sunnyGeezy",
              "original": "Weysincha" },
        "iss_infinite_energy.xml/Unlimited missiles":
            { "text": "mods_ported_by", "name": "sunnyGeezy", "url": "https://github.com/sunnyGeezy",
              "original": "Weysincha" }
    })

    ListView {
        id: list
        anchors.fill: parent
        anchors.leftMargin: 36; anchors.rightMargin: 324
        clip: true
        model: Backend.modsModel
        spacing: 2
        boundsBehavior: Flickable.StopAtBounds
        footer: Item { width: 1; height: 24 }
        header: Column {
            width: list.width; spacing: 10
            topPadding: 26; bottomPadding: 8
            Text { text: Theme.tr("mods_title"); color: "#fff"; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 42
                   font.weight: Font.ExtraBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 2.1 }
            Text { width: parent.width; text: Theme.tr("mods_intro"); wrapMode: Text.WordWrap; color: Theme.textDim
                   font.family: Theme.fontText; font.pixelSize: 14; lineHeight: 1.35; lineHeightMode: Text.ProportionalHeight }
            Item {
                width: parent.width; height: warn.implicitHeight
                Rectangle { width: 3; height: parent.height; radius: 1.5; color: Theme.warn }
                Text { id: warn; x: 18; width: parent.width - 18; text: Theme.tr("mods_warning"); wrapMode: Text.WordWrap
                       color: Theme.textDim; font.family: Theme.fontText; font.pixelSize: 13; lineHeight: 1.35
                       lineHeightMode: Text.ProportionalHeight }
            }
            Row {
                spacing: 10
                GhostButton { compact: true; text: Theme.tr("mods_folder"); onClicked: Backend.openModsFolder() }
                GhostButton { compact: true; text: Theme.tr("mods_refresh"); onClicked: Backend.refreshMods() }
            }
            Text { visible: list.count === 0; text: Theme.tr("mods_empty"); color: Theme.textDim
                   font.family: Theme.fontText; font.pixelSize: 15 }
        }
        delegate: Item {
            id: row
            property bool on: modelData.on === true   // flipped here at once; the list is not rebuilt
            width: list.width
            height: card.height + 8
            PolishCard {
                id: card
                y: 4; width: parent.width; radius: 12
                height: Math.max(66, info.implicitHeight + 28)
                color: Theme.card
                border.width: 1; border.color: Theme.cardLine
                Column {
                    id: info
                    x: 18; y: 14; width: parent.width - 36 - 80; spacing: 5
                    Text { text: modelData.name || ""; color: Theme.text; font.family: Theme.fontText; font.pixelSize: 16; font.weight: Font.DemiBold }
                    Text { visible: !!modelData.note; width: parent.width; text: modelData.note || ""; wrapMode: Text.WordWrap
                           color: Theme.textDim; font.family: Theme.fontText; font.pixelSize: 14; lineHeight: 1.3
                           lineHeightMode: Text.ProportionalHeight }
                    Row {
                        id: creditRow
                        readonly property var credit: page.credits[modelData.key] || null
                        visible: credit !== null
                        spacing: 6
                        // The mod's original author (plain text, no link), before the fix credit.
                        Text {
                            visible: !!(creditRow.credit && creditRow.credit.original)
                            text: visible ? Theme.tr("mods_original_by").replace("{name}", creditRow.credit.original) + "  ·" : ""
                            color: Theme.textDim; font.family: Theme.fontText; font.pixelSize: 13; font.weight: Font.DemiBold
                        }
                        Text {
                            text: creditRow.credit ? Theme.tr(creditRow.credit.text).replace("{name}", creditRow.credit.name) : ""
                            color: Theme.ok; font.family: Theme.fontText; font.pixelSize: 13; font.weight: Font.DemiBold
                            Accessible.role: Accessible.Button; Accessible.name: text
                            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                        onClicked: function(mouse) { mouse.accepted = true; Qt.openUrlExternally(creditRow.credit.url) } }
                        }
                    }
                }
                FocusHalo { }
                Toggle {
                    controllerFocusTarget: card
                    objectName: "modToggle_" + modelData.key
                    anchors.right: parent.right; anchors.rightMargin: 20; anchors.verticalCenter: parent.verticalCenter
                    accent: page.accent
                    checked: row.on
                    onToggled: (v) => { row.on = v; Backend.setModEnabled(modelData.key, v) }
                }
            }
        }
        // In the page's right margin, like the other tabs (inside the list it sat on the mods' switches).
        ScrollBar.vertical: ScrollBar {
            id: bar
            parent: page
            x: list.x + list.width + 12; y: list.y; height: list.height
            contentItem: Rectangle { implicitWidth: 6; radius: 3; color: bar.pressed ? Theme.ghostLineHover : Theme.ghostLine }
            background: Item { }
        }
    }

    Column {
        x: page.width - 288; y: 26; width: 252; spacing: 12
        Text {
            text: Backend.lang === "pt_BR" ? "Mods por jogo" : "Mods by game"
            color: "white"; font.family: Theme.fontText; font.pixelSize: 20; font.weight: Font.Medium
        }
        Repeater {
            model: page.games
            FocusScope {
                id: gameButton
                objectName: "modsGameSelector_" + index
                width: 252; height: 94; activeFocusOnTab: true
                enabled: Backend.playState === "idle"
                FocusHalo { active: gameButton.activeFocus }
                Rectangle {
                    anchors.fill: parent; radius: 2
                    color: index === page.selectedGame ? "#65417eb7" : hover.containsMouse ? "#353b83c6" : "#780a294d"
                    border.color: index === page.selectedGame || gameButton.activeFocus ? "white" : "#558ebce6"
                    border.width: index === page.selectedGame || gameButton.activeFocus ? 2 : 1
                }
                Image { x: 12; anchors.verticalCenter: parent.verticalCenter; width: 66; height: 66; source: modelData.iconSource || ""; fillMode: Image.PreserveAspectCrop }
                Text {
                    x: 90; width: parent.width - 102; anchors.verticalCenter: parent.verticalCenter
                    text: modelData.label; wrapMode: Text.WordWrap
                    color: "white"; font.family: Theme.fontText; font.pixelSize: 15; font.weight: index === page.selectedGame ? Font.DemiBold : Font.Normal
                }
                MouseArea { id: hover; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { gameButton.forceActiveFocus(); page.selectGame(index) } }
                Keys.onReturnPressed: page.selectGame(index)
                Keys.onSpacePressed: page.selectGame(index)
                Accessible.role: Accessible.Button; Accessible.name: modelData.label
                Accessible.onPressAction: page.selectGame(index)
            }
        }
    }
}
