import QtQuick
import QtQuick.Controls.Basic
import Launcher

Item {
    id: page
    readonly property color accent: Theme.accents.advanced

    ListView {
        id: list
        anchors.fill: parent
        anchors.leftMargin: 36; anchors.rightMargin: 36
        clip: true
        model: Backend.advancedModel
        spacing: 2
        boundsBehavior: Flickable.StopAtBounds
        delegate: FixRow { d: model.d }
        footer: Item { width: 1; height: 24 }
        header: Column {
            width: list.width; spacing: 10
            topPadding: 26; bottomPadding: 8
            Row {
                spacing: 16
                Text { text: Theme.tr("adv_title"); color: "#fff"; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 42
                       font.weight: Font.ExtraBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 2.1
                       anchors.verticalCenter: parent.verticalCenter }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: prof.implicitWidth + 24; height: 30; radius: 15
                    color: Theme.chip; border.width: 1
                    border.color: Backend.advancedIsDefault ? Theme.ok : Theme.warn
                    Text { id: prof; anchors.centerIn: parent; text: Backend.advancedText; color: Theme.textChip
                           font.family: Theme.fontText; font.pixelSize: 13; font.weight: Font.Bold }
                }
            }
            Text { width: parent.width; text: Theme.tr("adv_intro"); wrapMode: Text.WordWrap; color: Theme.textDim
                   font.family: Theme.fontText; font.pixelSize: 13; lineHeight: 1.35; lineHeightMode: Text.ProportionalHeight }
            Item {
                width: parent.width; height: warn.implicitHeight
                Rectangle { width: 3; height: parent.height; color: Theme.warn }
                Text { id: warn; x: 15; width: parent.width - 15; text: Theme.tr("adv_warning"); wrapMode: Text.WordWrap
                       color: Theme.textDim; font.family: Theme.fontText; font.pixelSize: 12; lineHeight: 1.35
                       lineHeightMode: Text.ProportionalHeight }
            }
            Row {
                spacing: 10
                TextField {
                    id: search
                    width: 300; height: 38
                    placeholderText: Theme.tr("adv_search")
                    color: Theme.text; placeholderTextColor: Theme.textMuted
                    font.family: Theme.fontText; font.pixelSize: 14
                    leftPadding: 12
                    background: Rectangle { radius: 10; color: Theme.chip; border.width: 1
                                            border.color: search.activeFocus ? page.accent : Theme.cardLine }
                    onTextChanged: Backend.setAdvancedFilter(text)
                }
                GhostButton { compact: true; text: Theme.tr("adv_reset"); onClicked: Backend.resetAdvanced() }
                GhostButton { compact: true; text: Theme.tr("adv_copy"); onClicked: Backend.copyCombination() }
            }
            Text { visible: list.count === 0; text: Theme.tr("adv_nothing"); color: Theme.textDim
                   font.family: Theme.fontText; font.pixelSize: 14 }
        }
        ScrollBar.vertical: ScrollBar {
            id: bar
            contentItem: Rectangle { implicitWidth: 6; radius: 3; color: bar.pressed ? Theme.ghostLineHover : Theme.ghostLine }
            background: Item { }
        }
    }
}
