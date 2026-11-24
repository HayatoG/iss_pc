import QtQuick
import QtQuick.Window
import Launcher

Window {
    id: win
    property alias current: main.current
    property alias animate: main.animate
    // Opening size (2026-11-22): 1424 x 877, the size the maintainer uses (about 16:10), shrunk to fit
    // smaller screens; the minimum below still applies.
    width: Math.max(minimumWidth, Math.min(1424, Screen.desktopAvailableWidth - 48))
    height: Math.max(minimumHeight, Math.min(877, Screen.desktopAvailableHeight - 64))
    minimumWidth: 1000
    minimumHeight: 640
    visible: true
    color: Theme.bg
    title: "inFAMOUS Second Son — PC"
    Main { id: main; anchors.fill: parent }
}
