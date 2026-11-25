import QtQuick
import QtQuick.Controls.Basic
import Launcher

Item {
    id: page
    readonly property color accent: Theme.neon
    readonly property int side: 36
    property bool guidesShown: false     // set by Main when the Controller guides screen exists
    signal openGuides()

    component Card: PolishCard {
        default property alias content: inner.data
        width: parent.width; height: inner.implicitHeight + 36
        radius: 14; color: Theme.card; border.width: 1; border.color: Theme.cardLine
        Column { id: inner; x: 18; y: 18; width: parent.width - 36; spacing: 10 }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width; contentHeight: body.implicitHeight + 60
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        Connections { target: PadControls; function onScrollRequested(y) { flick.contentY = Math.max(0, Math.min(y, flick.contentHeight - flick.height)) } }
        ScrollBar.vertical: ScrollBar {
            id: bar
            contentItem: Rectangle { implicitWidth: 6; radius: 3; color: bar.pressed ? Theme.ghostLineHover : Theme.ghostLine }
            background: Item { }
        }

        Column {
            id: body
            x: page.side; y: 26; width: flick.width - 2 * page.side; spacing: 14

            Text { text: Theme.tr("ctl_title"); color: "#fff"; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 42
                   font.weight: Font.ExtraBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 2.1 }
            Text { width: parent.width; text: Theme.tr("ctl_intro"); wrapMode: Text.WordWrap; color: Theme.textDim
                   font.family: Theme.fontText; font.pixelSize: 14; lineHeight: 1.35; lineHeightMode: Text.ProportionalHeight }

            // ---- controller --------------------------------------------------------------------
            SectionTitle { text: Theme.tr("ctl_sec_pad"); accent: page.accent }
            Card {
                Row {
                    spacing: 12
                    Text { text: PadControls.padName; color: "#fff"; font.family: Theme.fontText; font.pixelSize: 20; font.weight: Font.Bold
                           anchors.verticalCenter: parent.verticalCenter }
                    Tag { text: PadControls.padKindText; tone: PadControls.padFound ? "good" : "info"; visible: PadControls.padFound; anchors.verticalCenter: parent.verticalCenter }
                }
                Text { visible: !PadControls.padFound; width: parent.width; text: PadControls.padKindText; wrapMode: Text.WordWrap; color: Theme.textDim
                       font.family: Theme.fontText; font.pixelSize: 14 }
                Text { visible: text !== ""; width: parent.width; text: PadControls.padHint; wrapMode: Text.WordWrap; color: Theme.textDim
                       font.family: Theme.fontText; font.pixelSize: 14; lineHeight: 1.3; lineHeightMode: Text.ProportionalHeight }
            }
            Column {
                width: parent.width; spacing: 2
                Repeater {
                    model: PadControls.padRows
                    OptionRow { d: model.d; accent: page.accent; onValueSet: (id, value) => PadControls.setValue(id, value) }
                }
            }

            // ---- sensor / touchpad ---------------------------------------------------------------
            SectionTitle { text: Theme.tr("ctl_sec_sensor"); accent: page.accent }
            Card {
                Text { width: parent.width; text: Theme.tr("ctl_sensor_body"); wrapMode: Text.WordWrap; color: Theme.textSoft
                       font.family: Theme.fontText; font.pixelSize: 15; lineHeight: 1.4; lineHeightMode: Text.ProportionalHeight }
                GhostButton { visible: page.guidesShown; compact: true; text: Theme.tr("ctl_open_guides"); onClicked: page.openGuides() }
            }

            // ---- mouse -----------------------------------------------------------------------------
            SectionTitle { text: Theme.tr("ctl_sec_mouse"); accent: page.accent }
            Card {
                Text { width: parent.width; text: Theme.tr("ctl_mouse_intro"); wrapMode: Text.WordWrap; color: Theme.textSoft
                       font.family: Theme.fontText; font.pixelSize: 15; lineHeight: 1.4; lineHeightMode: Text.ProportionalHeight }
                Row {
                    spacing: 12
                    Tag { text: Theme.tr(PadControls.mouseEnabled ? "ctl_mouse_state_on" : "ctl_mouse_state_off")
                          tone: PadControls.mouseEnabled ? "good" : "bad"; anchors.verticalCenter: parent.verticalCenter }
                    GhostButton { compact: true; text: PadControls.mouseButtonText; tone: PadControls.mouseEnabled ? "danger" : "primary"
                                  accent: page.accent; onClicked: PadControls.toggleMouse() }
                }
                NoteBar { text: Theme.tr("ctl_mouse_build"); bar: Theme.warn }
            }
            Column {
                width: parent.width; spacing: 2
                Repeater {
                    model: PadControls.mouseRows
                    OptionRow { d: model.d; accent: page.accent; onValueSet: (id, value) => PadControls.setValue(id, value) }
                }
            }

            // ---- mouse as touchpad ---------------------------------------------------------------------
            SectionTitle { text: Theme.tr("ctl_sec_touch"); accent: page.accent }
            Text { width: parent.width; text: Theme.tr("ctl_touch_intro"); wrapMode: Text.WordWrap; color: Theme.textDim
                   font.family: Theme.fontText; font.pixelSize: 14; lineHeight: 1.35; lineHeightMode: Text.ProportionalHeight }
            Column {
                width: parent.width; spacing: 2
                Repeater {
                    model: PadControls.touchRows
                    OptionRow { d: model.d; accent: page.accent; onValueSet: (id, value) => PadControls.setValue(id, value) }
                }
            }

            // ---- keyboard ----------------------------------------------------------------------------------
            SectionTitle { text: Theme.tr("ctl_sec_keys"); accent: page.accent }
            Text { width: parent.width; text: Theme.tr("ctl_keys_intro"); wrapMode: Text.WordWrap; color: Theme.textDim
                   font.family: Theme.fontText; font.pixelSize: 14; lineHeight: 1.35; lineHeightMode: Text.ProportionalHeight }
            Flow {
                id: keys
                width: parent.width; spacing: 8
                Repeater {
                    model: PadControls.keyRows
                    PolishCard {
                        width: (keys.width - keys.spacing) / 2; height: 44; radius: 10
                        color: Theme.card; border.width: 1
                        border.color: modelData.changed ? Qt.rgba(page.accent.r, page.accent.g, page.accent.b, 0.7) : Theme.cardLine
                        PadGlyph { id: glyphIcon; x: 10; anchors.verticalCenter: parent.verticalCenter; glyph: modelData.glyph || "" }
                        Text { x: glyphIcon.x + glyphIcon.width + 10; width: parent.width - keyChip.width - x - 30; anchors.verticalCenter: parent.verticalCenter
                               text: modelData.name; color: Theme.text; elide: Text.ElideRight
                               font.family: Theme.fontText; font.pixelSize: 15; font.weight: Font.DemiBold }
                        Row {
                            anchors.right: parent.right; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter; spacing: 8
                            Tag { visible: modelData.changed; text: Theme.tr("ctl_remapped"); tone: "warn"; anchors.verticalCenter: parent.verticalCenter }
                            Rectangle {
                                id: keyChip
                                width: keyText.implicitWidth + 20; height: 28; radius: 6
                                color: "#1d2027"; border.width: 1; border.color: "#3a404b"
                                Text { id: keyText; anchors.centerIn: parent; text: modelData.keys; color: "#fff"
                                       font.family: Theme.fontText; font.pixelSize: 14; font.weight: Font.Bold }
                            }
                        }
                    }
                }
            }

            // ---- advanced: TLG_KEY_MAP ------------------------------------------------------------------------
            Card {
                Text { text: Theme.tr("keymap_title"); color: Theme.textMuted; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 700 })
                       font.pixelSize: 18; font.weight: Font.Bold; font.capitalization: Font.AllUppercase; font.letterSpacing: 0.9 }
                Text { width: parent.width; text: Theme.tr("keymap_intro"); wrapMode: Text.WordWrap; color: Theme.textDim
                       font.family: Theme.fontText; font.pixelSize: 13; lineHeight: 1.35; lineHeightMode: Text.ProportionalHeight }
                TextField {
                    id: field
                    width: parent.width; height: 40
                    placeholderText: Theme.tr("keymap_placeholder")
                    placeholderTextColor: Theme.textFaint
                    color: "#fff"; selectByMouse: true
                    font.family: Theme.fontText; font.pixelSize: 15
                    leftPadding: 12; rightPadding: 12
                    background: Rectangle { radius: 8; color: Theme.ghost; border.width: 1
                                            border.color: !PadControls.draftValid ? Theme.bad : field.activeFocus ? page.accent : Theme.ghostLine }
                    Component.onCompleted: text = PadControls.draftText
                    onTextChanged: PadControls.setDraft(text)
                    Connections { target: PadControls
                                  function onDraftChanged() { if (field.text !== PadControls.draftText) field.text = PadControls.draftText } }
                }
                Repeater {
                    model: PadControls.draftMessages
                    NoteBar { width: parent.width; text: modelData.text
                              bar: modelData.tone === "good" ? Theme.ok : modelData.tone === "bad" ? Theme.bad : Theme.warn }
                }
                Row {
                    spacing: 10
                    GhostButton { compact: true; tone: "primary"; accent: page.accent; text: Theme.tr("keymap_apply")
                                  enabled: PadControls.draftValid && PadControls.draftDirty; onClicked: PadControls.applyKeyMap() }
                    GhostButton { compact: true; text: Theme.tr("keymap_clear"); enabled: PadControls.savedKeyMap !== "" || field.text !== ""
                                  onClicked: PadControls.clearKeyMap() }
                }
            }
        }
    }
}
