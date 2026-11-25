import QtQuick
import Launcher

// Confirmation overlay driven by Saves.dialog ({open, title, body, lines, buttons:[{id,label,tone}]}).
Rectangle {
    id: root
    property var target: Saves
    property var d: target.dialog
    visible: !!d.open
    color: Qt.rgba(0, 0, 0, 0.62)
    MouseArea { anchors.fill: parent }   // blocks clicks behind the card
    PolishCard {
        id: box
        anchors.centerIn: parent
        width: Math.min(600, parent.width - 72)
        height: col.implicitHeight + 48
        radius: 14; color: Theme.card; border.width: 1; border.color: Theme.ghostLineHover
        Column {
            id: col
            x: 24; y: 24; width: parent.width - 48; spacing: 12
            Text { width: parent.width; text: d.title || ""; color: "#fff"; wrapMode: Text.WordWrap
                   font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 28; font.weight: Font.ExtraBold
                   font.capitalization: Font.AllUppercase; font.letterSpacing: 1.4 }
            Text { width: parent.width; text: d.body || ""; color: Theme.textSoft; wrapMode: Text.WordWrap
                   font.family: Theme.fontText; font.pixelSize: 15; lineHeight: 1.35; lineHeightMode: Text.ProportionalHeight }
            Column {
                visible: !!d.lines && d.lines.length > 0
                width: parent.width; spacing: 4
                Repeater {
                    model: (d.lines || []).slice(0, 8)
                    Row {
                        width: parent.width; spacing: 8
                        Text { text: "•"; color: Theme.ok; font.family: Theme.fontText; font.pixelSize: 14 }
                        Text { width: parent.width - 18; text: modelData; color: Theme.textMuted; wrapMode: Text.WordWrap
                               font.family: Theme.fontText; font.pixelSize: 14 }
                    }
                }
            }
            Flow {
                width: parent.width; spacing: 10; topPadding: 6
                Repeater {
                    model: d.buttons || []
                    GhostButton { text: modelData.label; tone: modelData.tone; onClicked: root.target.answer(modelData.id) }
                }
            }
        }
    }
}
