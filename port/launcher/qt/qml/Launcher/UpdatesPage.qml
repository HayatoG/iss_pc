import QtQuick
import QtQuick.Controls.Basic
import Launcher

Item {
    id: page
    readonly property color accent: Theme.warn
    readonly property int side: 36

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width; contentHeight: body.implicitHeight + 60
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {
            id: bar
            contentItem: Rectangle { implicitWidth: 6; radius: 3; color: bar.pressed ? Theme.ghostLineHover : Theme.ghostLine }
            background: Item { }
        }

        Column {
            id: body
            x: page.side; y: 26; width: flick.width - 2 * page.side; spacing: 14

            Text { text: Theme.tr("upd_title"); color: "#fff"; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 42
                   font.weight: Font.ExtraBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 2.1 }
            Text { width: parent.width; text: Updates.introText; wrapMode: Text.WordWrap; color: Theme.textDim
                   font.family: Theme.fontText; font.pixelSize: 14; lineHeight: 1.35; lineHeightMode: Text.ProportionalHeight }

            PolishCard {
                width: parent.width; height: versions.implicitHeight + 40
                radius: 14; color: Theme.card; border.width: 1
                border.color: Updates.available ? Qt.rgba(Theme.warn.r, Theme.warn.g, Theme.warn.b, 0.7) : Theme.cardLine
                Rectangle { visible: Updates.available; x: 10; y: 14; width: 3; height: parent.height - 28; radius: 1.5; color: Theme.warn }
                Column {
                    id: versions
                    x: 30; y: 20; width: parent.width - 52; spacing: 14
                    Row {
                        spacing: 48
                        Column {
                            spacing: 4
                            Text { text: Theme.tr("upd_current"); color: Theme.textMuted; font.family: Theme.fontText; font.pixelSize: 13
                                   font.weight: Font.Bold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1 }
                            Text { text: Updates.current; color: "#fff"; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 30
                                   font.weight: Font.ExtraBold; font.letterSpacing: 1 }
                        }
                        Column {
                            spacing: 4
                            Text { text: Theme.tr("upd_latest"); color: Theme.textMuted; font.family: Theme.fontText; font.pixelSize: 13
                                   font.weight: Font.Bold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1 }
                            Text { text: Updates.latest + (Updates.published !== "" ? "  ·  " + Updates.published : ""); color: Updates.available ? Theme.warn : "#fff"
                                   font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 30
                                   font.weight: Font.ExtraBold; font.letterSpacing: 1 }
                        }
                    }
                    Row {
                        spacing: 10
                        Rectangle { width: 4; height: statusText.implicitHeight; color: Updates.tone === "bad" ? Theme.bad : Updates.tone === "warn" ? Theme.warn
                                                                                  : Updates.tone === "good" ? Theme.ok : Theme.textMuted }
                        Text { id: statusText; width: versions.width - 14; text: Updates.statusText; wrapMode: Text.WordWrap; color: Theme.text
                               font.family: Theme.fontText; font.pixelSize: 16; font.weight: Font.DemiBold }
                    }
                    Column {
                        visible: Updates.busy
                        width: versions.width; spacing: 6
                        Rectangle {
                            width: parent.width; height: 6; radius: 3; color: Theme.ghostLine
                            Rectangle { width: parent.width * Math.max(0, Math.min(1, Updates.progress)); height: parent.height; radius: 3; color: Theme.warn }
                        }
                        Text { text: Updates.progressText; color: Theme.textDim; font.family: Theme.fontText; font.pixelSize: 13 }
                    }
                    Flow {
                        width: versions.width; spacing: 10
                        GhostButton { tone: "primary"; accent: Theme.warn; text: Theme.tr("upd_check"); enabled: !Updates.checking && !Updates.busy; onClicked: Updates.check() }
                        GhostButton { visible: Updates.packaged && !Updates.canRestart; accent: Theme.warn; text: Theme.tr("upd_install")
                                      enabled: Updates.canInstall; onClicked: Updates.install() }
                        GhostButton { visible: Updates.canCancel; text: Theme.tr("upd_cancel"); onClicked: Updates.cancelDownload() }
                        GhostButton { visible: Updates.canRollback; text: Theme.tr("upd_rollback"); onClicked: Updates.rollback() }
                        GhostButton { visible: Updates.canRestart; tone: "primary"; accent: Theme.warn; text: Theme.tr("upd_restart"); onClicked: Updates.restart() }
                        GhostButton { text: Theme.tr("upd_download"); enabled: Updates.canDownload; onClicked: Updates.download() }
                    }
                    Text { visible: Updates.installHint !== ""; width: versions.width; text: Updates.installHint; wrapMode: Text.WordWrap
                           color: Theme.textDim; font.family: Theme.fontText; font.pixelSize: 13 }
                }
            }

            // Latest beta: verified in-place update, preserving both games' personal data.
            PolishCard {
                width: parent.width; height: betaCol.implicitHeight + 40
                radius: 14; color: Theme.card; border.width: 1
                border.color: Updates.hasBeta ? Qt.rgba(Theme.cyan.r, Theme.cyan.g, Theme.cyan.b, 0.6) : Theme.cardLine
                Rectangle { visible: Updates.hasBeta; x: 10; y: 14; width: 3; height: parent.height - 28; radius: 1.5; color: Theme.cyan }
                Column {
                    id: betaCol
                    x: 30; y: 20; width: parent.width - 52; spacing: 12
                    Row {
                        spacing: 14
                        Text { text: Theme.tr("upd_beta_title"); color: Theme.textMuted; font.family: Theme.fontText; font.pixelSize: 13
                               font.weight: Font.Bold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1
                               anchors.verticalCenter: parent.verticalCenter }
                        Tag { visible: Updates.hasBeta; text: "BETA"; tone: "warn"; anchors.verticalCenter: parent.verticalCenter }
                    }
                    Text { visible: Updates.hasBeta; width: parent.width; text: Updates.betaTag + (Updates.betaPublished !== "" ? "  ·  " + Updates.betaPublished : "")
                           color: Theme.cyan; wrapMode: Text.WordWrap; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 26
                           font.weight: Font.ExtraBold; font.letterSpacing: 1 }
                    Text { width: parent.width; text: Updates.betaStatus; wrapMode: Text.WordWrap; color: Theme.text
                           font.family: Theme.fontText; font.pixelSize: 15; font.weight: Font.DemiBold }
                    Text { width: parent.width; text: Theme.tr("upd_beta_intro"); wrapMode: Text.WordWrap; color: Theme.textDim
                           font.family: Theme.fontText; font.pixelSize: 13; lineHeight: 1.3; lineHeightMode: Text.ProportionalHeight }
                    Column {
                        visible: Updates.busy
                        width: betaCol.width; spacing: 6
                        Rectangle {
                            width: parent.width; height: 6; radius: 3; color: Theme.ghostLine
                            Rectangle { width: parent.width * Math.max(0, Math.min(1, Updates.progress)); height: parent.height; radius: 3; color: Theme.cyan }
                        }
                        Text { text: Updates.progressText; color: Theme.textDim; font.family: Theme.fontText; font.pixelSize: 13 }
                    }
                    Flow {
                        width: betaCol.width; spacing: 10
                        GhostButton { visible: Updates.packaged && !Updates.canRestart; tone: "primary"; accent: Theme.cyan
                                      text: Theme.tr("upd_beta_install"); enabled: Updates.canInstallBeta; onClicked: Updates.installBeta() }
                        GhostButton { visible: Updates.canCancel; text: Theme.tr("upd_cancel"); onClicked: Updates.cancelDownload() }
                        GhostButton { visible: Updates.canRestart; tone: "primary"; accent: Theme.cyan; text: Theme.tr("upd_restart"); onClicked: Updates.restart() }
                        GhostButton { visible: Updates.hasBeta; text: Theme.tr("upd_beta_page"); onClicked: Updates.openBetaPage() }
                    }
                }
            }
            Row {
                visible: Updates.betaNotes !== ""
                spacing: 10
                Rectangle { width: 4; height: 20; color: Theme.cyan; anchors.verticalCenter: parent.verticalCenter }
                Text { text: Theme.tr("upd_beta_notes"); color: "#fff"; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 22
                       font.weight: Font.ExtraBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1.3
                       anchors.verticalCenter: parent.verticalCenter }
            }
            PolishCard {
                visible: Updates.betaNotes !== ""
                width: parent.width; height: Math.min(betaNotesText.implicitHeight + 36, 420)
                radius: 12; color: Theme.card; border.width: 1; border.color: Theme.cardLine
                Flickable {
                    id: betaNotesFlick
                    x: 20; y: 18; width: parent.width - 40; height: parent.height - 36
                    contentWidth: width; contentHeight: betaNotesText.implicitHeight
                    clip: true; boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { }
                    TextEdit { id: betaNotesText; width: betaNotesFlick.width - 10; text: Updates.betaNotes; readOnly: true; selectByMouse: true
                               wrapMode: TextEdit.Wrap; color: Theme.textSoft; selectionColor: Theme.cyan; selectedTextColor: "#001318"
                               font.family: Theme.fontText; font.pixelSize: 15 }
                }
            }

            PolishCard {
                width: parent.width; height: optionRow.implicitHeight + 36
                radius: 12; color: Theme.card; border.width: 1; border.color: Theme.cardLine
                Row {
                    id: optionRow
                    x: 20; y: 18; width: parent.width - 40; spacing: 16
                    Toggle { id: sw; accent: page.accent; checked: Updates.checkOnOpen; onToggled: (v) => Updates.setCheckOnOpen(v)
                             anchors.verticalCenter: parent.verticalCenter }
                    Column {
                        width: parent.width - sw.width - 16; spacing: 3
                        anchors.verticalCenter: parent.verticalCenter
                        Text { text: Theme.tr("upd_check_on_open"); color: "#fff"; font.family: Theme.fontText; font.pixelSize: 16; font.weight: Font.DemiBold }
                        Text { width: parent.width; text: Theme.tr("upd_check_on_open_note"); wrapMode: Text.WordWrap; color: Theme.textDim
                               font.family: Theme.fontText; font.pixelSize: 13 }
                    }
                }
            }

            Row {
                visible: Updates.notes !== ""
                spacing: 10
                Rectangle { width: 4; height: 20; color: page.accent; anchors.verticalCenter: parent.verticalCenter }
                Text { text: Theme.tr("upd_notes"); color: "#fff"; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 22
                       font.weight: Font.ExtraBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1.3
                       anchors.verticalCenter: parent.verticalCenter }
            }
            PolishCard {
                visible: Updates.notes !== ""
                width: parent.width; height: notesText.implicitHeight + 36
                radius: 12; color: Theme.card; border.width: 1; border.color: Theme.cardLine
                TextEdit { id: notesText; x: 20; y: 18; width: parent.width - 40; text: Updates.notes; readOnly: true; selectByMouse: true
                           wrapMode: TextEdit.Wrap; color: Theme.textSoft; selectionColor: page.accent; selectedTextColor: "#171100"
                           font.family: Theme.fontText; font.pixelSize: 15 }
            }
            Item { width: 1; height: 30 }
        }
    }

    ConfirmCard { anchors.fill: parent; target: Updates }
}
