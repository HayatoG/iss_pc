import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import Launcher

// Controller guides (2026-11-23, new UI): the player's photo of the controller, clean, and the list of
// what each button does beside it. Pointing at a line (or clicking it, to keep it) draws rings around
// only that line's buttons, with its number. Data: Backend.guideUi (qt/data/controller_ui.json).
Item {
    id: page
    objectName: "controllerGuides"
    readonly property color accent: Theme.neon
    readonly property int side: 36
    readonly property var ui: Backend.guideUi
    readonly property bool ready: !!(ui && ui.photoUrl)
    property int hovered: 0
    property int pinned: 0
    readonly property int hot: hovered > 0 ? hovered : pinned
    readonly property var hotRow: ready && hot > 0 ? ui.rows[hot - 1] : null
    readonly property color hotColor: ready && hot > 0 ? ui.colors[hot - 1] : "transparent"
    onUiChanged: { hovered = 0; pinned = 0 }

    Column {
        id: head
        x: page.side; y: 26; width: parent.width - 2 * page.side; spacing: 12
        Text { text: Theme.tr("guides_title"); color: "#fff"; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 42
               font.weight: Font.ExtraBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 2.1 }
        Text { width: parent.width; text: Theme.tr("guides_intro") + " " + Theme.tr("guides_hover")
               wrapMode: Text.WordWrap; color: Theme.textSoft
               font.family: Theme.fontText; font.pixelSize: 15; lineHeight: 1.35; lineHeightMode: Text.ProportionalHeight }
        Row {
            spacing: 8
            Text { text: Theme.tr("guides_kb"); color: Theme.textDim; font.family: Theme.fontText; font.pixelSize: 14
                   anchors.verticalCenter: parent.verticalCenter }
            Text { text: Theme.tr("guides_kb_open") + " →"; color: page.accent; font.family: Theme.fontText; font.pixelSize: 14; font.weight: Font.Bold
                   anchors.verticalCenter: parent.verticalCenter
                   MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: Backend.openReadme() } }
        }
        Flow {
            width: parent.width; spacing: 10
            Repeater {
                model: Backend.guideKinds
                Rectangle {
                    activeFocusOnTab: true
                    objectName: "controllerGuideKind_" + modelData.key
                    Keys.onReturnPressed: Backend.setGuideKind(modelData.key)
                    Keys.onSpacePressed: Backend.setGuideKind(modelData.key)
                    FocusHalo { active: parent.activeFocus }
                    readonly property bool on: Backend.guideKind === modelData.key
                    height: 34; radius: 17; width: kindText.implicitWidth + 28
                    color: on ? Theme.navOn : Theme.chip; border.width: 1; border.color: on ? page.accent : Theme.chipLine
                    Text { id: kindText; anchors.centerIn: parent; text: modelData.label; color: parent.on ? "#fff" : Theme.textChip
                           font.family: Theme.fontText; font.pixelSize: 14; font.weight: Font.DemiBold }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: Backend.setGuideKind(modelData.key) }
                }
            }
            Chip {   // the controller the launcher sees
                text: Backend.guideDetected !== "" ? Backend.guideDetected : Theme.tr("chip_pad_none")
                level: Backend.guideDetected !== "" ? "ok" : "off"
            }
            Item { width: 10; height: 1 }
            GhostButton { compact: true; text: Theme.tr("guides_save"); accent: page.accent; enabled: page.ready; onClicked: saveDialog.open() }
            GhostButton { compact: true; text: Theme.tr("guides_copy"); enabled: page.ready
                          onClicked: card.grabToImage(function (r) { r.saveToFile(Backend.guideGrabPath); Backend.copyGuideImage() }) }
        }
        Text { visible: Backend.guideMessage !== ""; width: parent.width; text: Backend.guideMessage; color: Theme.textSoft; wrapMode: Text.WordWrap
               font.family: Theme.fontText; font.pixelSize: 14 }
    }

    // ---- the guide card: photo on the left, the list on the right --------------------------------------
    PolishCard {
        id: card
        x: page.side; y: head.y + head.height + 16
        width: parent.width - 2 * page.side; height: Math.max(80, parent.height - y - 24)
        radius: 16
        readonly property int listWidth: Math.min(470, Math.max(340, width * 0.38))

        Text { visible: !page.ready; anchors.centerIn: parent; text: Theme.tr("guides_missing"); color: Theme.textDim
               font.family: Theme.fontText; font.pixelSize: 16 }

        // photo side: a soft light stage so the black controllers stand out
        Rectangle {
            id: stageBg
            visible: page.ready
            x: 1; y: 1; width: parent.width - card.listWidth - 1; height: parent.height - 2
            radius: card.radius - 1
            gradient: Gradient {
                GradientStop { position: 0; color: "#eef0f3" }
                GradientStop { position: 1; color: "#cfd4dc" }
            }
            Rectangle { anchors.right: parent.right; width: parent.radius; height: parent.height
                        gradient: Gradient { GradientStop { position: 0; color: "#eef0f3" } GradientStop { position: 1; color: "#cfd4dc" } } }

            Item {
                id: stage
                anchors.fill: parent; anchors.margins: 18
                // room around the photo for the trigger labels drawn above it (photo pixels)
                readonly property real padL: 20; readonly property real padT: 100
                readonly property real padR: 130; readonly property real padB: 20
                readonly property real pw: page.ready ? page.ui.size[0] : 1
                readonly property real ph: page.ready ? page.ui.size[1] : 1
                readonly property real k: Math.min(width / (pw + padL + padR), height / (ph + padT + padB))

                Item {
                    id: photoBox
                    width: stage.pw * stage.k; height: stage.ph * stage.k
                    x: (stage.width - (stage.pw + stage.padL + stage.padR) * stage.k) / 2 + stage.padL * stage.k
                    y: (stage.height - (stage.ph + stage.padT + stage.padB) * stage.k) / 2 + stage.padT * stage.k
                    Image {
                        anchors.fill: parent
                        source: page.ready ? page.ui.photoUrl : ""
                        asynchronous: true; smooth: true; mipmap: true
                        fillMode: Image.Stretch
                    }

                    // rings around the hot line's buttons, each with its number
                    Repeater {
                        model: page.hotRow ? page.hotRow.controls : []
                        Item {
                            id: mark
                            readonly property var ctl: page.ui.controls[modelData] || null
                            readonly property var call: page.ui.callouts[modelData] || null
                            readonly property real k: stage.k
                            anchors.fill: parent
                            opacity: 0
                            scale: 0.92
                            Component.onCompleted: { opacity = 1; scale = 1 }
                            Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
                            Behavior on scale { NumberAnimation { duration: 200; easing.type: Easing.OutBack } }

                            // a button: glow + ring + number badge at the ring's top right
                            Item {
                                visible: mark.ctl !== null
                                readonly property real rr: mark.ctl ? mark.ctl.r * mark.k + 7 : 0
                                x: mark.ctl ? mark.ctl.x * mark.k : 0; y: mark.ctl ? mark.ctl.y * mark.k : 0
                                Rectangle {
                                    x: -parent.rr - 5; y: -parent.rr - 5; width: 2 * (parent.rr + 5); height: width; radius: width / 2
                                    color: "transparent"; border.width: 10
                                    border.color: Qt.rgba(page.hotColor.r, page.hotColor.g, page.hotColor.b, 0.28)
                                }
                                Rectangle {
                                    x: -parent.rr; y: -parent.rr; width: 2 * parent.rr; height: width; radius: width / 2
                                    color: Qt.rgba(page.hotColor.r, page.hotColor.g, page.hotColor.b, 0.10)
                                    border.width: 4; border.color: page.hotColor
                                }
                                Badge { n: page.hot; x: parent.rr * 0.7071 - width / 2; y: -parent.rr * 0.7071 - height / 2 }
                            }

                            // a trigger the front view hides: a label above with a line to where it is
                            Item {
                                visible: mark.call !== null
                                readonly property real lx: mark.call ? mark.call.x * mark.k : 0
                                readonly property real ly: mark.call ? mark.call.y * mark.k : 0
                                readonly property real tx: mark.call ? mark.call.to[0] * mark.k : 0
                                readonly property real ty: mark.call ? mark.call.to[1] * mark.k : 0
                                Rectangle {   // the line
                                    readonly property real dx: parent.tx - parent.lx
                                    readonly property real dy: parent.ty - parent.ly
                                    x: parent.lx; y: parent.ly - 2
                                    width: Math.sqrt(dx * dx + dy * dy); height: 4; radius: 2
                                    color: page.hotColor
                                    transformOrigin: Item.Left
                                    rotation: Math.atan2(dy, dx) * 180 / Math.PI
                                }
                                Rectangle {   // the dot on the trigger
                                    x: parent.tx - 7; y: parent.ty - 7; width: 14; height: 14; radius: 7
                                    color: page.hotColor; border.width: 2; border.color: "#fff"
                                }
                                Rectangle {   // the label
                                    id: pill
                                    width: callText.implicitWidth + 30; height: 40; radius: 20
                                    x: parent.lx - width / 2; y: parent.ly - height / 2
                                    color: "#fff"; border.width: 4; border.color: page.hotColor
                                    Text { id: callText; anchors.centerIn: parent; text: mark.call ? mark.call.text : ""
                                           color: "#111"; font.family: Theme.fontText; font.pixelSize: 19; font.weight: Font.ExtraBold }
                                }
                                Badge { n: page.hot; x: pill.x + pill.width - width * 0.6; y: pill.y - height * 0.45 }
                            }
                        }
                    }
                }
            }
        }

        // list side
        Item {
            id: listSide
            visible: page.ready
            x: card.width - card.listWidth; width: card.listWidth; height: card.height
            Flickable {
                id: listFlick
                anchors.fill: parent
                clip: true
                contentHeight: legend.height + 36
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: listFlick.contentHeight > listFlick.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff }
            Column {
                id: legend
                x: 22; y: 22; width: listSide.width - 40; spacing: 4
                Text { text: page.ready ? page.ui.kicker : ""; color: Theme.smoke; font.family: Theme.fontText; font.pixelSize: 13
                       font.weight: Font.ExtraBold; font.letterSpacing: 0.6 }
                Text { width: parent.width; text: page.ready ? page.ui.title : ""; color: "#fff"; wrapMode: Text.WordWrap
                       font.family: Theme.fontText; font.pixelSize: 24; font.weight: Font.Bold }
                Text { width: parent.width; text: page.ready ? page.ui.subtitle : ""; color: Theme.textDim; wrapMode: Text.WordWrap
                       font.family: Theme.fontText; font.pixelSize: 14 }
                Text { text: page.ready ? page.ui.name : ""; color: Theme.warn; font.family: Theme.fontText; font.pixelSize: 15
                       font.weight: Font.Bold; bottomPadding: 8 }
                Repeater {
                    model: page.ready ? page.ui.rows : []
                    Rectangle {
                        id: row
                        activeFocusOnTab: true
                        objectName: "controllerGuideCallout_" + modelData.n
                        onActiveFocusChanged: { if (activeFocus) page.hovered = modelData.n; else if (page.hovered === modelData.n) page.hovered = 0 }
                        Keys.onReturnPressed: page.pinned = page.pinned === modelData.n ? 0 : modelData.n
                        Keys.onSpacePressed: page.pinned = page.pinned === modelData.n ? 0 : modelData.n
                        FocusHalo { active: row.activeFocus }
                        readonly property bool lit: page.hot === modelData.n
                        readonly property color c: page.ui.colors[modelData.n - 1]
                        width: legend.width; height: rowCol.implicitHeight + 16; radius: 10
                        color: lit ? Qt.rgba(c.r, c.g, c.b, 0.14) : rowMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.04) : "transparent"
                        border.width: lit ? 1 : 0; border.color: Qt.rgba(c.r, c.g, c.b, 0.75)
                        opacity: page.hot > 0 && !lit ? 0.55 : 1
                        Behavior on opacity { NumberAnimation { duration: 140 } }
                        Behavior on color { ColorAnimation { duration: 140 } }
                        Badge { id: rowBadge; n: modelData.n; x: 10; anchors.verticalCenter: parent.verticalCenter }
                        Column {
                            id: rowCol
                            x: rowBadge.x + rowBadge.width + 12; y: 8; width: row.width - x - 10; spacing: 2
                            Text { width: parent.width; text: modelData.title; color: "#fff"; wrapMode: Text.WordWrap
                                   font.family: Theme.fontText; font.pixelSize: 15; font.weight: Font.Bold }
                            Text { width: parent.width; text: modelData.detail; color: Theme.textDim; wrapMode: Text.WordWrap
                                   font.family: Theme.fontText; font.pixelSize: 13; lineHeight: 1.2; lineHeightMode: Text.ProportionalHeight }
                        }
                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onEntered: page.hovered = modelData.n
                            onExited: if (page.hovered === modelData.n) page.hovered = 0
                            onClicked: page.pinned = page.pinned === modelData.n ? 0 : modelData.n   // keep it lit
                        }
                    }
                }
                Text {
                    width: parent.width; topPadding: 10
                    text: page.ready ? page.ui.footer : ""; color: Theme.textFaint; wrapMode: Text.WordWrap
                    font.family: Theme.fontText; font.pixelSize: 12
                }
            }
            }
        }
    }

    // a number badge in its line's colour
    component Badge: Rectangle {
        property int n: 1
        width: 30; height: 30; radius: 15
        color: page.ready && n > 0 ? page.ui.colors[n - 1] : "transparent"
        border.width: 2; border.color: "#fff"
        Text { anchors.centerIn: parent; text: parent.n; color: "#fff"; font.family: Theme.fontText; font.pixelSize: 15; font.weight: Font.ExtraBold
               style: Text.Raised; styleColor: Qt.rgba(0, 0, 0, 0.35) }
    }

    FileDialog {
        id: saveDialog
        title: Theme.tr("guides_save_title")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "png"
        nameFilters: ["PNG (*.png)"]
        onAccepted: { var target = selectedFile.toString()
                      card.grabToImage(function (r) { r.saveToFile(Backend.guideGrabPath); Backend.saveGuideImage(target) }) }
    }
}
