import QtQuick
import Launcher

// Controls tab (2026-11-22): the controller settings and the Controller guides in one tab, switched by
// the pill at the top right ("Settings" / "Controller guides"). Both screens keep their own layout
// and are built the first time they are shown.
Item {
    id: hub
    property int view: 0          // 0 = settings, 1 = controller guides
    property bool guidesVisited: false
    onViewChanged: if (view === 1) guidesVisited = true

    Loader {
        anchors.fill: parent; anchors.topMargin: 76
        visible: hub.view === 0
        sourceComponent: ControlsPage { guidesShown: true; onOpenGuides: hub.view = 1 }
    }
    Loader {
        anchors.fill: parent; anchors.topMargin: 76
        visible: hub.view === 1
        active: hub.guidesVisited
        sourceComponent: GuidesPage { }
    }

    Rectangle {   // the switch between the two screens
        anchors.right: parent.right; anchors.rightMargin: 36; y: 18
        width: pillRow.width + 6; height: 36; radius: 18
        color: Qt.rgba(0, 0, 0, 0.35); border.width: 1; border.color: Qt.rgba(1, 1, 1, 0.10)
        Row {
            id: pillRow
            x: 3; y: 3; height: parent.height - 6
            Repeater {
                model: [ "ctl_view_settings", "nav_guides" ]
                Rectangle {
                    activeFocusOnTab: true
                    property bool controllerHeader: true
                    objectName: "controlsView_" + index
                    Keys.onReturnPressed: hub.view = index
                    Keys.onSpacePressed: hub.view = index
                    FocusHalo { active: parent.activeFocus }
                    readonly property bool on: hub.view === index
                    height: parent.height; radius: height / 2; width: pillText.implicitWidth + 28
                    color: on ? Theme.neon : (pillMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.07) : "transparent")
                    Rectangle {   // sheen on the chosen side
                        visible: parent.on; anchors.fill: parent; radius: parent.radius
                        gradient: Gradient {
                            GradientStop { position: 0; color: Qt.rgba(1, 1, 1, 0.22) }
                            GradientStop { position: 0.6; color: Qt.rgba(1, 1, 1, 0) }
                        }
                    }
                    Text { id: pillText; anchors.centerIn: parent; text: Theme.tr(modelData)
                           color: parent.on ? "#1a0610" : Theme.textMuted
                           font.family: Theme.fontText; font.pixelSize: 14; font.weight: Font.Bold }
                    MouseArea { id: pillMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: hub.view = index }
                    Accessible.role: Accessible.Button
                    Accessible.name: pillText.text
                }
            }
        }
    }
}
