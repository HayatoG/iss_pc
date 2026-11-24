import QtQuick
import QtQuick.Controls.Basic
import Launcher

Rectangle {
    id: root
    property int current: 0
    property bool modalBlocked: false
    property var hiddenKeys: []
    signal pick(int index)
    signal advancedRequested(bool on)
    signal homeRequested()
    height: 112; color: "#40102652"
    readonly property var entries: [
        {key:"play",label:"nav_play"},{key:"trophies",label:"nav_trophies"},{key:"mods",label:"nav_mods"},
        {key:"graphics",label:"nav_graphics"},{key:"controls",label:"nav_controls"},{key:"saves",label:"nav_saves"},
        {key:"updates",label:"nav_updates"},{key:"support",label:"nav_support"},{key:"advanced",label:"nav_advanced"},{key:"fixes",label:"nav_fixes"}]
    function controllerOnTab() { for (var i = 0; i < entries.length; ++i) { var nav = navRepeater.itemAt(i); if (nav && nav.activeFocus) return true } return false }
    function controllerHasFocus() { return languageButton.activeFocus || advancedButton.activeFocus || controllerOnTab() }
    function focusCurrent() { var item = navRepeater.itemAt(current); if (item) item.forceActiveFocus() }
    function moveFocus(index, delta) {
        for (var next = index + delta; next >= 0 && next < entries.length; next += delta) {
            var item = navRepeater.itemAt(next)
            if (item && item.visible) { item.forceActiveFocus(); return }
        }
    }
    Row {
        x: 40; y: 13; spacing: 14
        Text { text: "inFAMOUS"; color: "white"; font.family: Backend.fontDisplay; font.pixelSize: 25; font.weight: Font.Bold; anchors.verticalCenter: parent.verticalCenter }
        Text { text: "PC  /  Second Son + First Light"; color: "#b9d8f6"; font.family: Theme.fontText; font.pixelSize: 13; anchors.verticalCenter: parent.verticalCenter }
    }
    Row {
        anchors.right: parent.right; anchors.rightMargin: 40; y: 13; spacing: 22
        Text { text: Backend.footerVersion; color: "#b9d8f6"; font.family: Theme.fontText; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
        ConsoleButton { id: languageButton; objectName: "launcherLanguage"; height: 30; width: 66; text: Backend.lang === "pt_BR" ? "PT-BR" : "EN-US"; onClicked: Backend.setLang(Backend.lang === "pt_BR" ? "en_US" : "pt_BR") }
        ConsoleButton { id: advancedButton; objectName: "launcherAdvanced"; height: 30; width: Backend.lang === "pt_BR" ? 184 : 146; text: Theme.tr("adv_tabs_label"); onClicked: root.advancedRequested(!Backend.showAdvanced) }
        Text {
            id: clock; color: "white"; font.family: Theme.fontText; font.pixelSize: 16
            anchors.verticalCenter: parent.verticalCenter; text: Qt.formatTime(new Date(), "hh:mm")
            Timer { interval: 10000; repeat: true; running: root.visible; onTriggered: clock.text = Qt.formatTime(new Date(), "hh:mm") }
        }
    }
    Flickable {
        x: 40; y: 56; width: parent.width - 80; height: 50; clip: true
        contentWidth: navRow.width; contentHeight: height; boundsBehavior: Flickable.StopAtBounds
        Row {
            id: navRow; spacing: 8
            Repeater {
                id: navRepeater; model: root.entries
                FocusScope {
                    id: nav
                    visible: root.hiddenKeys.indexOf(modelData.key) < 0
                    readonly property bool selected: root.current === index
                    width: selected ? 148 : 90; height: 46; activeFocusOnTab: true
                    Rectangle { anchors.fill: parent; color: root.modalBlocked ? "transparent" : nav.selected ? "#26ffffff" : navMouse.containsMouse || nav.activeFocus ? "#15ffffff" : "transparent" }
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 2; color: "white"; visible: !root.modalBlocked && (nav.selected || nav.activeFocus) }
                    Icon { path: Theme.icons[modelData.key]; size: 22; color: "white"; anchors.verticalCenter: parent.verticalCenter; x: nav.selected ? 14 : (nav.width - 22) / 2 }
                    Text { visible: nav.selected; text: Theme.tr(modelData.label); color: "white"; font.family: Theme.fontText; font.pixelSize: 14; x: 47; width: parent.width - 54; anchors.verticalCenter: parent.verticalCenter; elide: Text.ElideRight }
                    Rectangle { visible: modelData.key === "updates" && Updates.available; x: parent.width - 14; y: 7; width: 6; height: 6; radius: 3; color: Theme.warn }
                    MouseArea { id: navMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { nav.forceActiveFocus(); root.pick(index) } }
                    ToolTip.visible: navMouse.containsMouse; ToolTip.delay: 500; ToolTip.text: Theme.tr(modelData.label)
                    Keys.onReturnPressed: root.pick(index)
                    Keys.onSpacePressed: root.pick(index)
                    Keys.onLeftPressed: root.moveFocus(index, -1)
                    Keys.onRightPressed: root.moveFocus(index, 1)
                    Keys.onDownPressed: root.homeRequested()
                    Accessible.role: Accessible.Button; Accessible.name: Theme.tr(modelData.label)
                    Accessible.onPressAction: root.pick(index)
                }
            }
        }
    }
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#25ffffff" }
}
