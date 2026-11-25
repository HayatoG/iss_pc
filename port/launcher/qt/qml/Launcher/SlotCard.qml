import QtQuick
import Launcher

// One save slot: thumbnail (icon0.png of the player's own save, read at run time), title, date, size, actions.
PolishCard {
    id: card
    property var d: ({})
    signal exportRequested(string slotId)
    readonly property color accent: Theme.ok
    height: 140; radius: 12
    color: Theme.card; border.width: 1; border.color: Theme.cardLine

    Rectangle {
        id: thumb
        x: 14; y: 14; width: 190; height: 107; radius: 8
        color: "#0e1014"; border.width: 1; border.color: Theme.chipLine
        clip: true
        Icon { anchors.centerIn: parent; size: 30; color: Theme.off; path: Theme.icons.saves; visible: img.status !== Image.Ready }
        Image { id: img; anchors.fill: parent; anchors.margins: 1; source: d.icon || ""; asynchronous: true
                fillMode: Image.PreserveAspectCrop; smooth: true }
    }
    Column {
        x: 220; y: 14; width: parent.width - 220 - 16; spacing: 6
        Row {
            spacing: 10
            Tag { text: d.kindText || ""; tone: d.kind === "auto" ? "info" : "good" }
            Text { text: d.slot || ""; color: Theme.textFaint; font.family: Theme.fontText; font.pixelSize: 13
                   anchors.verticalCenter: parent.verticalCenter }
        }
        Text { width: parent.width; text: d.title || ""; color: "#fff"; elide: Text.ElideRight
               font.family: Theme.fontText; font.pixelSize: 17; font.weight: Font.DemiBold }
        Text { width: parent.width; text: (d.when || "") + "  ·  " + (d.size || ""); color: Theme.textDim
               font.family: Theme.fontText; font.pixelSize: 14; elide: Text.ElideRight }
        Row {
            spacing: 8; topPadding: 4
            GhostButton { compact: true; text: Theme.tr("saves_export"); onClicked: card.exportRequested(d.id) }
            GhostButton { compact: true; text: Theme.tr("saves_duplicate"); enabled: !Saves.running; onClicked: Saves.duplicate(d.id) }
            GhostButton { compact: true; text: Theme.tr("saves_delete"); tone: "danger"; enabled: !Saves.running; onClicked: Saves.askDelete(d.id) }
        }
    }
}
