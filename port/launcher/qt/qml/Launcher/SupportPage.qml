import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import Launcher

Item {
    id: page
    readonly property color accent: Theme.cyan
    readonly property int side: 36
    property bool showOthers: false

    component SectionTitle: Row {
        property string text: ""
        spacing: 10
        Rectangle { width: 4; height: 20; color: page.accent; anchors.verticalCenter: parent.verticalCenter }
        Text { text: parent.text; color: "#fff"; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 22
               font.weight: Font.ExtraBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1.3
               anchors.verticalCenter: parent.verticalCenter }
    }
    component Body: Text {
        width: parent ? parent.width : 400
        wrapMode: Text.WordWrap; color: Theme.textDim
        font.family: Theme.fontText; font.pixelSize: 13; lineHeight: 1.35; lineHeightMode: Text.ProportionalHeight
    }
    component Mono: Rectangle {      // dark selectable text box
        property alias text: edit.text
        property int boxHeight: 150
        width: parent ? parent.width : 400; height: boxHeight
        radius: 10; color: "#0e1014"; border.width: 1; border.color: Theme.cardLine
        Flickable {
            id: fl
            anchors.fill: parent; anchors.margins: 10
            clip: true; contentWidth: width; contentHeight: edit.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {
                id: sb
                contentItem: Rectangle { implicitWidth: 5; radius: 2; color: sb.pressed ? Theme.ghostLineHover : Theme.ghostLine }
                background: Item { }
            }
            TextEdit { id: edit; width: fl.width - 8; readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap
                       color: Theme.textSoft; selectionColor: page.accent; selectedTextColor: "#00131a"
                       font.family: "monospace"; font.pixelSize: 12 }
        }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width; contentHeight: body.implicitHeight + 90
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        Connections { target: Support; function onScrollRequested(y) { flick.contentY = y } }
        ScrollBar.vertical: ScrollBar {
            id: bar
            contentItem: Rectangle { implicitWidth: 6; radius: 3; color: bar.pressed ? Theme.ghostLineHover : Theme.ghostLine }
            background: Item { }
        }

        Column {
            id: body
            x: page.side; y: 26; width: flick.width - 2 * page.side; spacing: 14

            Text { text: Theme.tr("sup_title"); color: "#fff"; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 42
                   font.weight: Font.ExtraBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 2.1 }
            Body { text: Theme.tr("sup_intro") }

            // ---- known problems for the GPU -----------------------------------------------------------
            SectionTitle { text: Theme.tr("sup_known_title") }
            Text { visible: Support.gpuName !== ""; text: Theme.tr("sup_known_gpu").replace("{gpu}", Support.gpuName); color: Theme.textMuted
                   font.family: Theme.fontText; font.pixelSize: 14; font.weight: Font.DemiBold }
            Body { visible: Support.hwReady && Support.matchedCount === 0; text: Theme.tr("sup_known_none") }
            GhostButton {
                visible: Support.known.length > Support.matchedCount
                compact: true
                text: (page.showOthers ? Theme.tr("sup_known_hide") : Theme.tr("sup_known_show")).replace("{n}", Support.known.length - Support.matchedCount)
                onClicked: page.showOthers = !page.showOthers
            }
            Repeater {
                model: Support.known
                Column {
                    visible: modelData.matched || page.showOthers
                    width: body.width; spacing: 8
                    Rectangle {
                        width: parent.width; height: card.implicitHeight + 30; radius: 12
                        color: Theme.card; border.width: 1
                        border.color: modelData.matched ? Qt.rgba(Theme.cyan.r, Theme.cyan.g, Theme.cyan.b, 0.7) : Theme.cardLine
                        opacity: modelData.matched ? 1 : 0.72
                        Rectangle { visible: modelData.matched; width: 3; height: parent.height - 2; x: 1; y: 1; color: Theme.cyan }
                        Column {
                            id: card
                            x: 20; y: 15; width: parent.width - 40; spacing: 6
                            Row {
                                spacing: 10
                                Text { text: modelData.title; color: "#fff"; font.family: Theme.fontText; font.pixelSize: 16; font.weight: Font.DemiBold
                                       anchors.verticalCenter: parent.verticalCenter }
                                Tag { visible: modelData.matched; text: Theme.tr("sup_known_match"); tone: "warn"; anchors.verticalCenter: parent.verticalCenter }
                            }
                            Body { text: modelData.what; color: Theme.textSoft }
                            Text { visible: modelData.issues !== ""; text: Theme.tr("sup_issues_label") + ": " + modelData.issues; color: Theme.textFaint
                                   font.family: Theme.fontText; font.pixelSize: 12 }
                        }
                    }
                }
            }

            // ---- the wizard -----------------------------------------------------------------------------
            Item { width: 1; height: 6 }
            SectionTitle { text: Theme.tr("sup_wizard_title") }
            Row {
                spacing: 8
                Repeater {
                    model: ["sup_step1", "sup_step2", "sup_step3"]
                    Rectangle {
                        readonly property bool on: Support.step === index
                        width: stepText.implicitWidth + 32; height: 34; radius: 17
                        color: on ? Qt.rgba(Theme.cyan.r, Theme.cyan.g, Theme.cyan.b, 0.16) : Theme.chip
                        border.width: 1; border.color: on ? Theme.cyan : Theme.chipLine
                        Text { id: stepText; anchors.centerIn: parent; text: Theme.tr(modelData); color: parent.on ? "#fff" : Theme.textMuted
                               font.family: Theme.fontText; font.pixelSize: 13; font.weight: Font.Bold }
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: Support.setStep(index) }
                    }
                }
            }

            Rectangle {
                width: parent.width; height: stepCol.implicitHeight + 40
                radius: 14; color: Theme.card; border.width: 1; border.color: Theme.cardLine
                Column {
                    id: stepCol
                    x: 20; y: 20; width: parent.width - 40; spacing: 12

                    // step 1: your PC
                    Column {
                        visible: Support.step === 0
                        width: parent.width; spacing: 10
                        Body { text: Theme.tr("sup_step1_text") }
                        Text { visible: Support.collecting; text: Theme.tr("sup_collecting"); color: page.accent
                               font.family: Theme.fontText; font.pixelSize: 14; font.weight: Font.DemiBold }
                        Repeater {
                            model: Support.hwRows
                            Row {
                                spacing: 14; width: stepCol.width
                                Text { width: 130; text: modelData.label; color: Theme.textMuted; font.family: Theme.fontText; font.pixelSize: 14
                                       font.weight: Font.Bold }
                                Text { width: parent.width - 144; text: modelData.value; color: "#fff"; wrapMode: Text.WordWrap
                                       font.family: Theme.fontText; font.pixelSize: 14 }
                            }
                        }
                    }

                    // step 2: the log
                    Column {
                        visible: Support.step === 1
                        width: parent.width; spacing: 10
                        Body { text: Theme.tr("sup_step2_text") }
                        Body { visible: !Support.hasLogs; text: Theme.tr("sup_no_logs"); color: Theme.textSoft }
                        Rectangle {
                            visible: Support.hasLogs
                            width: parent.width; height: Math.min(Support.logs.length, 4) * 42 + 2
                            radius: 10; color: "#0e1014"; border.width: 1; border.color: Theme.cardLine
                            clip: true
                            ListView {
                                id: logList
                                anchors.fill: parent; anchors.margins: 1
                                model: Support.logs; clip: true; boundsBehavior: Flickable.StopAtBounds
                                ScrollBar.vertical: ScrollBar { }
                                delegate: Rectangle {
                                    width: logList.width; height: 42
                                    color: modelData.selected ? Qt.rgba(Theme.cyan.r, Theme.cyan.g, Theme.cyan.b, 0.12) : (rowMouse.containsMouse ? Theme.navHover : "transparent")
                                    Rectangle { x: 14; anchors.verticalCenter: parent.verticalCenter; width: 14; height: 14; radius: 7
                                                color: "transparent"; border.width: 2; border.color: modelData.selected ? Theme.cyan : Theme.off
                                                Rectangle { anchors.centerIn: parent; width: 6; height: 6; radius: 3; color: Theme.cyan; visible: modelData.selected } }
                                    Text { x: 42; anchors.verticalCenter: parent.verticalCenter; text: modelData.name; color: "#fff"
                                           font.family: Theme.fontText; font.pixelSize: 14; font.weight: Font.DemiBold }
                                    Tag { visible: modelData.latest; x: 250; anchors.verticalCenter: parent.verticalCenter; text: Theme.tr("sup_latest"); tone: "good" }
                                    Text { anchors.right: parent.right; anchors.rightMargin: 16; anchors.verticalCenter: parent.verticalCenter
                                           text: modelData.when + " · " + modelData.size; color: Theme.textDim; font.family: Theme.fontText; font.pixelSize: 12 }
                                    MouseArea { id: rowMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: Support.selectLog(index) }
                                }
                            }
                        }
                        Text { visible: Support.hasLogs; text: Theme.tr("sup_log_summary"); color: Theme.textMuted; font.family: Theme.fontText
                               font.pixelSize: 13; font.weight: Font.Bold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1 }
                        Text { visible: Support.busyLog; text: Theme.tr("sup_reading"); color: page.accent; font.family: Theme.fontText; font.pixelSize: 14 }
                        Mono { visible: Support.hasLogs && !Support.busyLog; text: Support.summaryText; boxHeight: 170 }
                        Body { visible: Support.redactionNote !== ""; text: Support.redactionNote; color: Theme.ok }
                        Row {
                            spacing: 10
                            GhostButton { compact: true; text: Theme.tr("sup_open_logs"); onClicked: Support.openLogsFolder() }
                        }
                    }

                    // step 3: send
                    Column {
                        visible: Support.step === 2
                        width: parent.width; spacing: 12
                        Body { text: Theme.tr("sup_step3_text") }
                        Text { text: Theme.tr("sup_describe"); color: Theme.textMuted; font.family: Theme.fontText; font.pixelSize: 13
                               font.weight: Font.Bold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1 }
                        Rectangle {
                            width: parent.width; height: 92; radius: 10; color: "#0e1014"; border.width: 1
                            border.color: desc.activeFocus ? page.accent : Theme.cardLine
                            Flickable {
                                id: descFlick
                                anchors.fill: parent; anchors.margins: 10; clip: true
                                contentWidth: width; contentHeight: desc.implicitHeight
                                boundsBehavior: Flickable.StopAtBounds
                                TextArea.flickable: TextArea {
                                    id: desc
                                    wrapMode: TextEdit.Wrap; color: "#fff"; selectByMouse: true
                                    selectionColor: page.accent; selectedTextColor: "#00131a"
                                    placeholderText: Theme.tr("sup_describe_hint"); placeholderTextColor: Theme.textFaint
                                    font.family: Theme.fontText; font.pixelSize: 14
                                    padding: 0; background: null
                                    text: Support.description
                                    onTextChanged: if (text !== Support.description) Support.setDescription(text)
                                }
                            }
                        }
                        Flow {
                            width: parent.width; spacing: 10
                            GhostButton { compact: true; text: Theme.tr("sup_copy"); onClicked: Support.copyReport() }
                            GhostButton { compact: true; text: Theme.tr("sup_save_zip"); onClicked: saveDialog.open() }
                            GhostButton { compact: true; tone: "primary"; accent: Theme.cyan; text: Theme.tr("sup_open_issue"); onClicked: Support.openIssue() }
                        }
                        Body { visible: Support.reportIsLong; text: Theme.tr("sup_long_hint"); color: Theme.warn }
                        Text { text: Theme.tr("sup_preview"); color: Theme.textMuted; font.family: Theme.fontText; font.pixelSize: 13
                               font.weight: Font.Bold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1 }
                        Mono { text: Support.reportText; boxHeight: 210 }
                    }

                    Row {
                        spacing: 10
                        GhostButton { compact: true; visible: Support.step === 0; text: Theme.tr("sup_recollect"); enabled: !Support.collecting; onClicked: Support.collect() }
                        GhostButton { compact: true; visible: Support.step > 0; text: Theme.tr("sup_back"); onClicked: Support.setStep(Support.step - 1) }
                        GhostButton { compact: true; visible: Support.step < 2; tone: "primary"; accent: Theme.cyan; text: Theme.tr("sup_next")
                                      onClicked: Support.setStep(Support.step + 1) }
                    }
                }
            }

            // ---- links ------------------------------------------------------------------------------------
            Item { width: 1; height: 6 }
            SectionTitle { text: Theme.tr("sup_links_title") }
            Flow {
                width: parent.width; spacing: 10
                GhostButton { text: "☕  " + Theme.tr("coffee_label"); tone: "primary"; accent: "#FFDD00"; onClicked: Backend.openUrl("coffee") }
                GhostButton { text: Theme.tr("sponsor_label"); onClicked: Backend.openUrl("sponsor") }
                GhostButton { text: Theme.tr("sup_discord"); onClicked: Support.openLink("discord") }
                GhostButton { text: Theme.tr("sup_youtube"); onClicked: Support.openLink("youtube") }
                GhostButton { text: Theme.tr("sup_github"); onClicked: Support.openLink("github") }
                GhostButton { text: Theme.tr("sup_open_logs"); onClicked: Support.openLogsFolder() }
            }
        }
    }

    Rectangle {
        visible: Support.message !== ""
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        anchors.leftMargin: page.side; anchors.rightMargin: page.side; anchors.bottomMargin: 16
        height: msg.implicitHeight + 24; radius: 10
        readonly property color tone: Support.tone === "bad" ? Theme.bad : Support.tone === "good" ? Theme.ok
                                      : Support.tone === "warn" ? Theme.warn : Theme.textMuted
        color: "#0e1014"; border.width: 1; border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.7)
        Text { id: msg; x: 16; y: 12; width: parent.width - 60; text: Support.message; wrapMode: Text.WordWrap; color: Theme.text
               font.family: Theme.fontText; font.pixelSize: 13; lineHeight: 1.3; lineHeightMode: Text.ProportionalHeight }
        Text { text: "✕"; color: Theme.textMuted; anchors.right: parent.right; anchors.rightMargin: 16; y: 12; font.pixelSize: 14
               MouseArea { anchors.fill: parent; anchors.margins: -8; cursorShape: Qt.PointingHandCursor; onClicked: Support.dismissMessage() } }
    }

    FileDialog {
        id: saveDialog
        title: Theme.tr("sup_save_zip")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "zip"
        nameFilters: ["ZIP (*.zip)"]
        currentFile: Support.defaultZipUrl
        onAccepted: Support.saveZip(selectedFile.toString())
    }
}
