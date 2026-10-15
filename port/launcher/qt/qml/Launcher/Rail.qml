import QtQuick
import Launcher

Rectangle {
    id: root
    property int current: 0
    signal pick(int index)
    color: Theme.rail
    width: Theme.railWidth
    Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.railLine }

    readonly property var entries: [
        { key: "play", label: "nav_play" }, { key: "graphics", label: "nav_graphics" },
        { key: "controls", label: "nav_controls" }, { key: "fixes", label: "nav_fixes" },
        { key: "saves", label: "nav_saves" }, { key: "guides", label: "nav_guides" },
        { key: "support", label: "nav_support" }, { key: "updates", label: "nav_updates" },
        { key: "advanced", label: "nav_advanced" } ]

    Column {
        x: 16; y: 22; width: parent.width - 33; spacing: 6
        Item {
            width: parent.width; height: 76
            Column {
                x: 6; y: 2; spacing: 2
                Text { text: "inFAMOUS"; color: "#ffffff"; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 34
                       font.weight: Font.ExtraBold; font.capitalization: Font.AllUppercase
                       font.letterSpacing: 1.7; lineHeight: 0.95 }
                Text { text: "Second Son · PC"; color: Theme.smoke; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 700 }); font.pixelSize: 15
                       font.weight: Font.Bold; font.capitalization: Font.AllUppercase; font.letterSpacing: 4.8 }
            }
        }
        Repeater {
            model: root.entries
            NavButton {
                width: parent.width
                label: Theme.tr(modelData.label)
                iconPath: Theme.icons[modelData.key]
                accent: Theme.accents[modelData.key]
                current: root.current === index
                badge: modelData.key === "updates" && Updates.available
                onClicked: root.pick(index)
            }
        }
    }

    Column {
        anchors.bottom: parent.bottom; anchors.bottomMargin: 22
        x: 22; width: parent.width - 44; spacing: 12
        Row {
            spacing: 6; width: parent.width
            Repeater {
                model: [ { code: "pt_BR", label: "PT-BR" }, { code: "en_US", label: "EN-US" } ]
                Rectangle {
                    readonly property bool on: Backend.lang === modelData.code
                    width: (parent.width - 6) / 2; height: 34; radius: 8
                    color: on ? Theme.navOn : "transparent"
                    border.width: 1; border.color: on ? Theme.smoke : Theme.chipLine
                    Text { anchors.centerIn: parent; text: modelData.label; color: parent.on ? "#fff" : Theme.textMuted
                           font.family: Theme.fontText; font.pixelSize: 13; font.weight: Font.Bold }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: Backend.setLang(modelData.code) }
                }
            }
        }
        Text {
            text: (Backend.version ? Backend.version + " · " : "") + Theme.tr("footer_credit") + "\n" + Theme.tr("footer_license")
            color: Theme.textFaint; font.family: Theme.fontText; font.pixelSize: 12; lineHeight: 1.25
        }
    }
}
