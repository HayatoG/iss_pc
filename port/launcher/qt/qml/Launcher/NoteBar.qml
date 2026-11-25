import QtQuick
import Launcher

// A line of explanation with a colored bar on its left.
Item {
    id: root
    property string text: ""
    property color bar: Theme.warn
    property color textColor: Theme.textDim
    width: parent ? parent.width : 400
    height: t.implicitHeight
    Rectangle { width: 3; height: parent.height; radius: 1.5; color: root.bar }
    Text { id: t; x: 18; width: parent.width - 18; text: root.text; wrapMode: Text.WordWrap; color: root.textColor
           font.family: Theme.fontText; font.pixelSize: 14; lineHeight: 1.35; lineHeightMode: Text.ProportionalHeight }
}
