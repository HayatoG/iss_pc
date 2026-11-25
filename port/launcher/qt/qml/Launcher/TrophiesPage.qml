import QtQuick
import QtQuick.Controls.Basic
import Launcher

// Trophies tab (2026-11-22): every trophy of the selected game as a card with its icon, name, how to
// earn it and when it was earned. Earned cards are in colour with a gold frame and glow; the others
// are dimmed. Filters: all, earned, not earned. Data: Backend.trophies (the Play tab's showcase).
Item {
    id: page
    readonly property color accent: Theme.accents.trophies
    readonly property color gold: "#eebf4c"
    readonly property int side: 36
    property string filter: "all"   // all | earned | locked
    readonly property int total: Backend.trophies.length
    readonly property int earnedCount: Backend.trophiesEarned

    function shown(t) {
        return page.filter === "all" || (page.filter === "earned") === !!t.earned
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width; contentHeight: col.implicitHeight + 52
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { }

        Column {
            id: col
            x: page.side; y: 26; width: flick.width - 2 * page.side; spacing: 16

            Text { text: Theme.tr("trophies_title"); color: "#fff"; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 42
                   font.weight: Font.ExtraBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 2.1 }
            Text { width: parent.width; text: Theme.tr("trophies_page_intro"); wrapMode: Text.WordWrap; color: Theme.textSoft
                   font.family: Theme.fontText; font.pixelSize: 15; lineHeight: 1.35; lineHeightMode: Text.ProportionalHeight }

            Text {   // no game folder, or the game's trophy icons could not be read
                visible: page.total === 0
                width: parent.width; wrapMode: Text.WordWrap; text: Theme.tr("trophies_none"); color: Theme.textDim
                font.family: Theme.fontText; font.pixelSize: 15
            }

            Column {   // count, progress and filters
                visible: page.total > 0
                width: parent.width; spacing: 12
                Row {
                    spacing: 14
                    Text { text: page.earnedCount + " / " + page.total; color: page.gold; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 })
                           font.pixelSize: 30; font.weight: Font.ExtraBold; anchors.verticalCenter: parent.verticalCenter }
                    Text { text: Theme.tr(Backend.gameIndex === 1 ? "trophies_count_fl" : "trophies_count").replace("{earned}", page.earnedCount).replace("{total}", page.total)
                           color: Theme.textMuted; font.family: Theme.fontText; font.pixelSize: 15; font.weight: Font.DemiBold
                           anchors.verticalCenter: parent.verticalCenter }
                }
                Rectangle {
                    width: parent.width; height: 8; radius: 4; color: Theme.cardLine
                    Rectangle { width: parent.width * (page.total ? page.earnedCount / page.total : 0); height: parent.height; radius: 4
                                gradient: Gradient { orientation: Gradient.Horizontal
                                    GradientStop { position: 0; color: "#c9952a" }
                                    GradientStop { position: 1; color: page.gold } } }
                }
                Row {
                    spacing: 8
                    Repeater {
                        model: [ { key: "all", label: "trophies_filter_all" }, { key: "earned", label: "trophies_filter_earned" },
                                 { key: "locked", label: "trophies_filter_locked" } ]
                        Rectangle {
                            activeFocusOnTab: true
                            objectName: "trophyFilter_" + modelData.key
                            Keys.onReturnPressed: page.filter = modelData.key
                            Keys.onSpacePressed: page.filter = modelData.key
                            FocusHalo { active: parent.activeFocus }
                            readonly property bool on: page.filter === modelData.key
                            height: 32; radius: 16; width: filterText.implicitWidth + 28
                            color: on ? Qt.rgba(0.93, 0.75, 0.30, 0.18) : (filterMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(1, 1, 1, 0.04))
                            border.width: 1; border.color: on ? page.gold : Qt.rgba(1, 1, 1, 0.12)
                            Text { id: filterText; anchors.centerIn: parent; text: Theme.tr(modelData.label); color: parent.on ? page.gold : Theme.textSoft
                                   font.family: Theme.fontText; font.pixelSize: 14; font.weight: Font.DemiBold }
                            MouseArea { id: filterMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                        onClicked: page.filter = modelData.key }
                        }
                    }
                }
            }

            Flow {
                id: grid
                visible: page.total > 0
                width: parent.width; spacing: 14
                readonly property int cols: Math.max(1, Math.floor((width + spacing) / 360))
                readonly property real cardWidth: Math.floor((width - (cols - 1) * spacing) / cols)
                Repeater {
                    model: Backend.trophies
                    Item {
                        id: card
                        activeFocusOnTab: true
                        objectName: "trophyCard_" + modelData.id
                        Keys.onReturnPressed: (event) => event.accepted = true
                        FocusHalo { active: card.activeFocus }
                        readonly property bool earned: !!modelData.earned
                        visible: page.shown(modelData)
                        width: grid.cardWidth; height: Math.max(108, info.implicitHeight + 28)
                        Rectangle {   // shadow
                            x: 0; y: 4; width: parent.width; height: parent.height; radius: 16; color: Qt.rgba(0, 0, 0, 0.28)
                        }
                        Rectangle {   // gold glow behind an earned card
                            visible: card.earned
                            anchors.fill: parent; anchors.margins: -3; radius: 19; color: "transparent"
                            border.width: 3; border.color: Qt.rgba(0.93, 0.75, 0.30, 0.18)
                        }
                        Rectangle {
                            anchors.fill: parent; radius: 16
                            gradient: Gradient {
                                GradientStop { position: 0; color: card.earned ? "#33280f" : "#1d2128" }
                                GradientStop { position: 1; color: card.earned ? "#1a140a" : "#121419" }
                            }
                            border.width: 1
                            border.color: card.earned ? Qt.rgba(0.93, 0.75, 0.30, cardMouse.containsMouse ? 0.95 : 0.70)
                                                      : Qt.rgba(1, 1, 1, cardMouse.containsMouse ? 0.18 : 0.08)
                        }
                        Rectangle {   // highlight on the top edge
                            x: 18; y: 1; width: parent.width - 36; height: 1
                            gradient: Gradient { orientation: Gradient.Horizontal
                                GradientStop { position: 0; color: Qt.rgba(1, 1, 1, 0) }
                                GradientStop { position: 0.5; color: card.earned ? Qt.rgba(1, 0.88, 0.55, 0.45) : Qt.rgba(1, 1, 1, 0.18) }
                                GradientStop { position: 1; color: Qt.rgba(1, 1, 1, 0) } }
                        }
                        MouseArea { id: cardMouse; anchors.fill: parent; hoverEnabled: true }
                        Item {   // earned Platinum: glowing spotlight beams turning behind its icon (kept inside the card)
                            visible: modelData.id === 0 && card.earned
                            anchors.fill: parent; anchors.margins: 1
                            clip: true
                            PlatinumRays {
                                radius: 100
                                x: tile.x - 1 + tile.width / 2 - radius; y: tile.y - 1 + tile.height / 2 - radius
                            }
                        }
                        TrophyTile {
                            id: tile
                            x: 14; anchors.verticalCenter: parent.verticalCenter
                            side: 80; trophy: modelData; tooltip: false
                        }
                        Column {
                            id: info
                            x: tile.x + tile.width + 16; anchors.verticalCenter: parent.verticalCenter
                            width: card.width - x - 16; spacing: 5
                            Row {
                                width: parent.width; spacing: 8
                                Text { id: nameText; text: modelData.name; width: Math.min(implicitWidth, parent.width - (platTag.visible ? platTag.width + 8 : 0))
                                       elide: Text.ElideRight; color: card.activeFocus || card.earned ? "#ffffff" : Theme.textMuted
                                       font.family: Theme.fontText; font.pixelSize: 17; font.weight: Font.Bold }
                                Rectangle {   // the platinum (always the first trophy of the list)
                                    id: platTag
                                    visible: modelData.id === 0
                                    anchors.verticalCenter: nameText.verticalCenter
                                    height: 20; radius: 10; width: platText.implicitWidth + 16
                                    color: Qt.rgba(0.80, 0.85, 0.95, 0.16); border.width: 1; border.color: Qt.rgba(0.80, 0.85, 0.95, 0.55)
                                    Text { id: platText; anchors.centerIn: parent; text: Theme.tr("trophies_platinum"); color: "#dfe6f5"
                                           font.family: Theme.fontText; font.pixelSize: 11; font.weight: Font.ExtraBold
                                           font.capitalization: Font.AllUppercase; font.letterSpacing: 0.8 }
                                }
                            }
                            Text { width: parent.width; text: modelData.requirement; wrapMode: Text.WordWrap; maximumLineCount: 3; elide: Text.ElideRight
                                   color: card.earned ? Theme.textSoft : Theme.textDim
                                   font.family: Theme.fontText; font.pixelSize: 14; lineHeight: 1.2; lineHeightMode: Text.ProportionalHeight }
                            Text { text: (card.earned ? "★  " : "") + modelData.status; color: card.earned ? page.gold : Theme.textFaint
                                   font.family: Theme.fontText; font.pixelSize: 13; font.weight: Font.Bold }
                        }
                    }
                }
            }

            Text { visible: page.total > 0; width: parent.width; wrapMode: Text.WordWrap; color: Theme.textDim
                   text: Theme.tr(Backend.gameIndex === 1 ? "trophies_hint_fl" : "trophies_hint")
                   font.family: Theme.fontText; font.pixelSize: 13; lineHeight: 1.3; lineHeightMode: Text.ProportionalHeight }
        }
    }
}
