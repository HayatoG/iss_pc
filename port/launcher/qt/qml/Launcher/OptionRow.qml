import QtQuick
import Launcher

// One option of the Graphics and Controls screens: a card with the name, tags, an explanation and
// a switch, a choice or a slider on the right. `d` comes from a RowsModel (qt/rowsmodel.py).
Item {
    id: row
    property var d: ({})
    property color accent: Theme.video
    signal valueSet(string rowId, string value)
    signal actionTriggered(string rowId, string actionId)
    readonly property bool isHeader: d.kind === "header"
    width: parent ? parent.width : 600
    height: isHeader ? 52 : card.height + 8

    SectionTitle {
        visible: row.isHeader
        anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.bottomMargin: 6
        text: d.title || ""; accent: row.accent
    }

    PolishCard {
        id: card
        visible: !row.isHeader
        y: 4; width: parent.width; radius: 12
        height: Math.max(78, info.implicitHeight + 28)
        color: Theme.card
        border.width: 1; border.color: d.changed ? Qt.rgba(row.accent.r, row.accent.g, row.accent.b, 0.7) : Theme.cardLine
        opacity: d.disabled ? 0.45 : 1
        FocusHalo { }
        Column {
            id: info
            x: 18; y: 14; width: parent.width - 36 - 264; spacing: 5
            Text { text: d.label || ""; color: Theme.text; font.family: Theme.fontText; font.pixelSize: 16; font.weight: Font.DemiBold }
            Flow {
                width: parent.width; spacing: 6
                Repeater { model: d.tags || []; Tag { text: modelData.text; tone: modelData.tone } }
            }
            Text { width: parent.width; text: d.what || ""; wrapMode: Text.WordWrap; color: Theme.textDim
                   font.family: Theme.fontText; font.pixelSize: 14; lineHeight: 1.3; lineHeightMode: Text.ProportionalHeight }
            Text { visible: !!d.info; width: parent.width; text: d.info || ""; wrapMode: Text.WordWrap; color: Theme.textSoft
                   font.family: Theme.fontText; font.pixelSize: 14; font.weight: Font.DemiBold }
            Text { visible: !!d.note; width: parent.width; text: d.note || ""; wrapMode: Text.WordWrap; color: Theme.warn
                   font.family: Theme.fontText; font.pixelSize: 14; lineHeight: 1.3; lineHeightMode: Text.ProportionalHeight }
            GhostButton { visible: !!d.action; compact: true; text: d.action ? d.action.label : ""
                          onClicked: row.actionTriggered(d.id, d.action.id) }
        }
        Item {
            id: ctl
            enabled: !d.disabled
            anchors.right: parent.right; anchors.rightMargin: 20; anchors.verticalCenter: parent.verticalCenter
            width: 232; height: 36
            Toggle {
                controllerFocusTarget: card
                visible: d.control === "switch"
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                accent: row.accent
                checked: d.on === true
                onToggled: (v) => row.valueSet(d.id, v ? "1" : "0")
            }
            Select {
                controllerFocusTarget: card
                visible: d.control === "combo"
                accent: row.accent
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                width: 232
                model: d.options || []
                currentIndex: d.index || 0
                onActivated: (i) => row.valueSet(d.id, d.options[i].value)
            }
            Row {
                visible: d.control === "slider"
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                spacing: 10
                LevelSlider {
                    controllerFocusTarget: card
                    id: slide
                    anchors.verticalCenter: parent.verticalCenter
                    accent: row.accent
                    width: 150
                    from: d.lo || 0; to: d.hi || 1; stepSize: d.step || 1
                    value: d.value || 0
                    onCommitted: (v) => row.valueSet(d.id, String(v))
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 72; horizontalAlignment: Text.AlignRight
                    text: slide.controllerEditing ? slide.controllerDraft.toFixed(d.decimals || 0) + (d.unit ? " " + d.unit : "") : slide.pressed ? slide.value.toFixed(d.decimals || 0) + (d.unit ? (d.unit === "MB" ? " " : "") + d.unit : "") : (d.valueText || "")
                    color: Theme.text; font.family: Theme.fontText; font.pixelSize: 14; font.weight: Font.DemiBold
                    elide: Text.ElideLeft
                }
            }
        }
    }
}
