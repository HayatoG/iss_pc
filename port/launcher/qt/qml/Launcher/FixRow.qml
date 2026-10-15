import QtQuick
import QtQuick.Controls.Basic
import Launcher

Item {
    id: row
    property var d: ({})
    readonly property color accent: Theme.smoke
    width: ListView.view ? ListView.view.width : 600
    height: d.kind === "header" ? (d.note ? 64 : 52) : card.height + 8

    // group header
    Column {
        visible: d.kind === "header"
        anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.bottomMargin: 6; width: parent.width; spacing: 3
        Row {
            spacing: 10
            Rectangle { width: 4; height: 20; color: row.accent; anchors.verticalCenter: parent.verticalCenter }
            Text { text: d.title || ""; color: "#fff"; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 22; font.weight: Font.ExtraBold
                   font.capitalization: Font.AllUppercase; font.letterSpacing: 1.3; anchors.verticalCenter: parent.verticalCenter }
        }
        Text { visible: !!d.note; x: 14; text: d.note || ""; color: Theme.textDim; font.family: Theme.fontText; font.pixelSize: 12 }
    }

    // fix card
    Rectangle {
        id: card
        visible: d.kind === "item"
        y: 4; width: parent.width; radius: 12
        height: Math.max(78, info.implicitHeight + 28)
        color: Theme.card
        border.width: 1; border.color: d.changed ? Qt.rgba(row.accent.r, row.accent.g, row.accent.b, 0.7) : Theme.cardLine
        Column {
            id: info
            x: 18; y: 14; width: parent.width - 36 - 240; spacing: 5
            Row {
                spacing: 10
                Text { visible: !!d.code; text: d.code || ""; color: row.accent; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 700 }); font.pixelSize: 16
                       font.weight: Font.Bold; font.letterSpacing: 1; anchors.verticalCenter: parent.verticalCenter }
                Text { text: d.label || ""; color: Theme.text; font.family: Theme.fontText; font.pixelSize: 15; font.weight: Font.DemiBold
                       anchors.verticalCenter: parent.verticalCenter }
            }
            Flow {
                width: parent.width; spacing: 6
                Repeater { model: d.tags || []; Tag { text: modelData.text; tone: modelData.tone } }
                Tag { visible: !!d.shaderWarn; text: "⚠ " + (d.shaderWarn || ""); tone: "warn" }
            }
            Text { width: parent.width; text: d.what || ""; wrapMode: Text.WordWrap; color: Theme.textDim
                   font.family: Theme.fontText; font.pixelSize: 13; lineHeight: 1.3; lineHeightMode: Text.ProportionalHeight }
        }
        Toggle {
            visible: d.control === "switch"
            anchors.right: parent.right; anchors.rightMargin: 20; anchors.verticalCenter: parent.verticalCenter
            accent: row.accent
            checked: d.on === true
            onToggled: (v) => Backend.setFix(d.id, v ? "1" : "0")
        }
        Row {
            visible: d.control === "number"
            anchors.right: parent.right; anchors.rightMargin: 20; anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            TextField {
                id: num
                width: 96; height: 34
                text: d.value === undefined ? "" : d.value
                placeholderText: d.placeholder === undefined ? "" : d.placeholder
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: d.lo || 0; top: d.hi || 0 }
                color: Theme.text; placeholderTextColor: Theme.textMuted
                font.family: Theme.fontText; font.pixelSize: 14
                horizontalAlignment: Text.AlignRight
                background: Rectangle { radius: 8; color: Theme.chip; border.width: 1
                                        border.color: num.activeFocus ? row.accent : Theme.cardLine }
                onEditingFinished: Backend.setFix(d.id, text === "" ? "auto" : text)
            }
            Text { visible: !!d.unit; text: d.unit || ""; color: Theme.textDim; font.family: Theme.fontText
                   font.pixelSize: 13; anchors.verticalCenter: parent.verticalCenter }
        }
        Select {
            visible: d.control === "combo"
            anchors.right: parent.right; anchors.rightMargin: 20; anchors.verticalCenter: parent.verticalCenter
            width: 210
            model: d.options || []
            currentIndex: d.index || 0
            onActivated: (i) => Backend.setFix(d.id, d.options[i].value)
        }
    }
}
