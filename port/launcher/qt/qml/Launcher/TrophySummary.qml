import QtQuick
import Launcher

FocusScope {
    id: summary
    objectName: "combinedTrophySummary"
    property var games: []
    property bool interactive: false
    readonly property var totals: {
        var result = {platinum: 0, gold: 0, silver: 0, bronze: 0}
        for (var i = 0; i < games.length; ++i) {
            var medals = games[i].trophyMedals || {}
            for (var grade in result) result[grade] += medals[grade] || 0
        }
        return result
    }
    readonly property int earned: totals.platinum + totals.gold + totals.silver + totals.bronze
    signal activated()
    implicitHeight: 154
    activeFocusOnTab: interactive
    Rectangle {
        anchors.fill: parent; radius: 2
        color: hover.containsMouse && summary.interactive ? "#654274a8" : "#890a294d"
        border.color: summary.activeFocus ? "white" : "#558ebce6"; border.width: summary.activeFocus ? 2 : 1
    }
    Text {
        x: 18; y: 12; width: parent.width - 36
        text: (Backend.lang === "pt_BR" ? "Troféus · ambos os jogos" : "Trophies · both games") + "  ·  " + summary.earned
        color: "#e5f1ff"; font.family: Theme.fontText; font.pixelSize: 14; elide: Text.ElideRight
    }
    Row {
        x: 12; y: 43; width: parent.width - 24
        Repeater {
            model: [
                {key: "platinum", label: Backend.lang === "pt_BR" ? "Platina" : "Platinum", color: "#aeefff"},
                {key: "gold", label: Backend.lang === "pt_BR" ? "Ouro" : "Gold", color: "#ffce65"},
                {key: "silver", label: Backend.lang === "pt_BR" ? "Prata" : "Silver", color: "#e0e7f3"},
                {key: "bronze", label: "Bronze", color: "#e5a17b"}
            ]
            Column {
                width: (summary.width - 24) / 4; spacing: 4
                // Original angular award cup: drawn locally, no Sony icon assets.
                Icon { anchors.horizontalCenter: parent.horizontalCenter; size: summary.height < 130 ? 23 : 30; color: modelData.color
                       path: "M6 3h12l-2 9-4 4-4-4z M6.5 5H3l1 5 4 2 M17.5 5H21l-1 5-4 2 M12 16v4 M8 22h8 M9 20h6" }
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: summary.totals[modelData.key]; color: "white"; font.family: Theme.fontText; font.pixelSize: summary.height < 130 ? 20 : 25; font.weight: Font.Medium }
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: modelData.label; color: modelData.color; font.family: Theme.fontText; font.pixelSize: 12 }
            }
        }
    }
    MouseArea { id: hover; anchors.fill: parent; enabled: summary.interactive; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { summary.forceActiveFocus(); summary.activated() } }
    Keys.onReturnPressed: if (interactive) activated()
    Keys.onSpacePressed: if (interactive) activated()
    Accessible.role: interactive ? Accessible.Button : Accessible.StaticText
    Accessible.name: "Trophies: " + earned + ", Platinum " + totals.platinum + ", Gold " + totals.gold + ", Silver " + totals.silver + ", Bronze " + totals.bronze
    Accessible.onPressAction: if (interactive) activated()
}
