import QtQuick
import QtQuick.Controls.Basic
import Launcher

Item {
    id: page
    readonly property color accent: Theme.smoke

    ListView {
        id: list
        anchors.fill: parent
        anchors.leftMargin: 36; anchors.rightMargin: 36
        clip: true
        model: Backend.fixesModel
        spacing: 2
        boundsBehavior: Flickable.StopAtBounds
        Connections { target: Backend; function onScrollRequested(y) { list.contentY = y } }
        delegate: FixRow { d: model.d }
        footer: Item { width: 1; height: 24 }
        header: Column {
            width: list.width; spacing: 10
            topPadding: 26; bottomPadding: 8
            Row {
                spacing: 16
                Text { text: Theme.tr("fixes_title"); color: "#fff"; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 42
                       font.weight: Font.ExtraBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 2.1
                       anchors.verticalCenter: parent.verticalCenter }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: prof.implicitWidth + 24; height: 30; radius: 15
                    color: Theme.chip; border.width: 1
                    border.color: Backend.profileIsRecommended ? Theme.ok : Theme.warn
                    Text { id: prof; anchors.centerIn: parent; text: Backend.profileText; color: Theme.textChip
                           font.family: Theme.fontText; font.pixelSize: 14; font.weight: Font.Bold }
                }
            }
            Text { width: parent.width; text: Theme.tr("fixes_intro"); wrapMode: Text.WordWrap; color: Theme.textDim
                   font.family: Theme.fontText; font.pixelSize: 14; lineHeight: 1.35; lineHeightMode: Text.ProportionalHeight }
            Row {
                spacing: 10
                GhostButton { compact: true; text: Theme.tr("fixes_previous"); onClicked: Backend.resetFixes("previous") }
                GhostButton { compact: true; text: Theme.tr("fixes_reset"); onClicked: Backend.resetFixes("defaults") }
            }
            Item {
                width: parent.width; height: warn.implicitHeight
                Rectangle { width: 3; height: parent.height; radius: 1.5; color: Theme.warn }
                Text { id: warn; x: 18; width: parent.width - 18; text: Theme.tr("fixes_recompile"); wrapMode: Text.WordWrap
                       color: Theme.textDim; font.family: Theme.fontText; font.pixelSize: 13; lineHeight: 1.35
                       lineHeightMode: Text.ProportionalHeight }
            }
        }
        ScrollBar.vertical: ScrollBar {
            id: bar
            contentItem: Rectangle { implicitWidth: 6; radius: 3; color: bar.pressed ? Theme.ghostLineHover : Theme.ghostLine }
            background: Item { }
        }
    }
}
