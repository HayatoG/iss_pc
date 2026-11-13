import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import Launcher

Item {
    id: page
    signal openHome()
    readonly property color accent: Theme.ok
    property string exportTarget: ""      // "" = all saves; else a slot id
    readonly property int side: 36

    component SectionTitle: Row {
        property string text: ""
        spacing: 10
        Rectangle { width: 4; height: 20; color: page.accent; anchors.verticalCenter: parent.verticalCenter }
        Text { text: parent.text; color: "#fff"; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 22
               font.weight: Font.ExtraBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1.3
               anchors.verticalCenter: parent.verticalCenter }
    }
    component Note: Item {
        property string text: ""
        property color bar: Theme.warn
        width: parent ? parent.width : 400; height: t.implicitHeight
        Rectangle { width: 3; height: parent.height; color: parent.bar }
        Text { id: t; x: 15; width: parent.width - 15; text: parent.text; wrapMode: Text.WordWrap; color: Theme.textDim
               font.family: Theme.fontText; font.pixelSize: 13; lineHeight: 1.35; lineHeightMode: Text.ProportionalHeight }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width; contentHeight: body.implicitHeight + 60
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        Connections { target: Saves; function onScrollRequested(y) { flick.contentY = y } }
        ScrollBar.vertical: ScrollBar {
            id: bar
            contentItem: Rectangle { implicitWidth: 6; radius: 3; color: bar.pressed ? Theme.ghostLineHover : Theme.ghostLine }
            background: Item { }
        }

        Column {
            id: body
            x: page.side; y: 26; width: flick.width - 2 * page.side; spacing: 14

            Text { text: Theme.tr("saves_title"); color: "#fff"; font.family: Theme.fontDisplay; font.variableAxes: ({ "wght": 800 }); font.pixelSize: 42
                   font.weight: Font.ExtraBold; font.capitalization: Font.AllUppercase; font.letterSpacing: 2.1 }
            Text { width: parent.width; text: Theme.tr("saves_intro"); wrapMode: Text.WordWrap; color: Theme.textDim
                   font.family: Theme.fontText; font.pixelSize: 13; lineHeight: 1.35; lineHeightMode: Text.ProportionalHeight }
            Note { visible: Saves.running; text: Theme.tr("saves_running"); bar: Theme.bad }

            // ---- data folder ------------------------------------------------------------------
            Rectangle {
                width: parent.width; height: folderCol.implicitHeight + 36
                radius: 14; color: Theme.card; border.width: 1; border.color: Theme.cardLine
                Column {
                    id: folderCol
                    x: 18; y: 18; width: parent.width - 36; spacing: 10
                    Row {
                        spacing: 12
                        Text { text: Theme.tr("saves_folder_title"); color: Theme.textMuted; font.family: Theme.fontDisplay
                               font.variableAxes: ({ "wght": 700 }); font.pixelSize: 18; font.weight: Font.Bold
                               font.capitalization: Font.AllUppercase; font.letterSpacing: 0.9; anchors.verticalCenter: parent.verticalCenter }
                        Tag { text: Saves.sourceText; tone: Saves.source === "default" ? "warn" : "good"; anchors.verticalCenter: parent.verticalCenter }
                    }
                    Text { width: parent.width; text: Saves.userDir; color: "#fff"; wrapMode: Text.WrapAnywhere
                           font.family: Theme.fontText; font.pixelSize: 17; font.weight: Font.DemiBold }
                    Note { visible: Saves.source === "default"; text: Theme.tr("saves_portable_warn") }
                    Note { visible: !Saves.canChange; text: Theme.tr("saves_env_locked"); bar: Theme.bad }
                    Flow {
                        width: parent.width; spacing: 10
                        GhostButton { compact: true; text: Theme.tr("saves_open_folder"); onClicked: Saves.openFolder() }
                        GhostButton { compact: true; text: Theme.tr("saves_choose_folder"); enabled: Saves.canChange; onClicked: { folderPurpose = "data"; folderDialog.open() } }
                        GhostButton { compact: true; text: Theme.tr("saves_reset_folder"); enabled: Saves.canChange && Saves.source === "pointer"; onClicked: Saves.resetDir() }
                        GhostButton { compact: true; text: Theme.tr("saves_use_suggested"); tone: "primary"; enabled: Saves.canChange && Saves.source === "default"
                                      onClicked: Saves.useSuggested() }
                    }
                    Text { width: parent.width; text: Theme.tr("saves_suggest").replace("{path}", Saves.suggestion); color: Theme.textFaint
                           font.family: Theme.fontText; font.pixelSize: 12; wrapMode: Text.WrapAnywhere }
                }
            }

            // ---- saves found elsewhere -----------------------------------------------------------
            Repeater {
                model: Saves.found
                Rectangle {
                    width: body.width; height: foundRow.implicitHeight + 28
                    radius: 12; color: Theme.card; border.width: 1; border.color: Qt.rgba(Theme.warn.r, Theme.warn.g, Theme.warn.b, 0.55)
                    Rectangle { width: 3; height: parent.height - 2; y: 1; x: 1; color: Theme.warn }
                    Row {
                        id: foundRow
                        x: 20; y: 14; width: parent.width - 40; spacing: 16
                        Text { width: parent.width - importBtn.width - 16; text: modelData.text; color: Theme.text; wrapMode: Text.WrapAnywhere
                               font.family: Theme.fontText; font.pixelSize: 14; lineHeight: 1.3; lineHeightMode: Text.ProportionalHeight
                               anchors.verticalCenter: parent.verticalCenter }
                        GhostButton { id: importBtn; compact: true; tone: "primary"; text: Theme.tr("saves_import"); enabled: !Saves.running
                                      anchors.verticalCenter: parent.verticalCenter; onClicked: Saves.importFound(index) }
                    }
                }
            }
            Row {
                spacing: 12
                GhostButton { compact: true; text: Saves.scanning ? Theme.tr("saves_scanning") : Theme.tr("saves_scan"); enabled: !Saves.scanning; onClicked: Saves.scan() }
                Text { visible: Saves.scanned && Saves.found.length === 0 && !Saves.scanning; text: Theme.tr("saves_scan_none"); color: Theme.textFaint
                       font.family: Theme.fontText; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
            }

            // ---- saves -------------------------------------------------------------------------------
            Item { width: 1; height: 6 }
            SectionTitle { text: Theme.tr("saves_list_title") }
            Flow {
                width: parent.width; spacing: 10
                GhostButton { compact: true; text: Theme.tr("saves_backup_now"); tone: "primary"; onClicked: Saves.backupNow() }
                GhostButton { compact: true; text: Theme.tr("saves_import_zip"); enabled: !Saves.running; onClicked: openZip.open() }
                GhostButton { compact: true; text: Theme.tr("saves_export_all"); enabled: Saves.hasSaves; onClicked: { page.exportTarget = ""; saveZip.open() } }
            }
            Text { visible: !Saves.hasSaves; width: parent.width; text: Theme.tr("saves_none"); color: Theme.textDim; wrapMode: Text.WordWrap
                   font.family: Theme.fontText; font.pixelSize: 14 }
            Repeater {
                model: Saves.slots
                SlotCard { width: body.width; d: modelData; onExportRequested: (id) => { page.exportTarget = id; saveZip.open() } }
            }

            // ---- backups -----------------------------------------------------------------------------------
            Item { width: 1; height: 6 }
            SectionTitle { text: Theme.tr("backups_title") }
            Text { width: parent.width; text: Theme.tr("backups_intro"); wrapMode: Text.WordWrap; color: Theme.textDim
                   font.family: Theme.fontText; font.pixelSize: 13; lineHeight: 1.35; lineHeightMode: Text.ProportionalHeight }
            Rectangle {
                width: parent.width; height: settingsCol.implicitHeight + 32
                radius: 12; color: Theme.card; border.width: 1; border.color: Theme.cardLine
                Column {
                    id: settingsCol
                    x: 18; y: 16; width: parent.width - 36; spacing: 12
                    Row {
                        spacing: 12
                        Text { text: Backend.countText("backups_keep", Saves.keep); color: Theme.text; font.family: Theme.fontText
                               font.pixelSize: 14; anchors.verticalCenter: parent.verticalCenter }
                        GhostButton { compact: true; width: 40; implicitWidth: 40; text: "−"; onClicked: Saves.setKeep(Saves.keep - 1) }
                        GhostButton { compact: true; width: 40; implicitWidth: 40; text: "+"; onClicked: Saves.setKeep(Saves.keep + 1) }
                    }
                    Text { text: Theme.tr("backups_extra"); color: Theme.text; font.family: Theme.fontText; font.pixelSize: 14 }
                    Row {
                        spacing: 10
                        Text { width: Math.min(implicitWidth, body.width - 330); text: Saves.extraDir || Theme.tr("backups_extra_none"); elide: Text.ElideMiddle
                               color: Saves.extraDir ? "#fff" : Theme.textFaint; font.family: Theme.fontText; font.pixelSize: 14
                               anchors.verticalCenter: parent.verticalCenter }
                        GhostButton { compact: true; text: Theme.tr("backups_extra_choose"); onClicked: { folderPurpose = "extra"; folderDialog.open() } }
                        GhostButton { compact: true; visible: Saves.extraDir !== ""; text: Theme.tr("backups_extra_clear"); onClicked: Saves.clearExtra() }
                    }
                }
            }
            Text { visible: Saves.backups.length === 0; text: Theme.tr("backups_none"); color: Theme.textDim; font.family: Theme.fontText; font.pixelSize: 14 }
            Repeater {
                model: Saves.backups
                Rectangle {
                    width: body.width; height: 56; radius: 10; color: Theme.card; border.width: 1; border.color: Theme.cardLine
                    Column {
                        x: 18; anchors.verticalCenter: parent.verticalCenter; spacing: 2
                        Text { text: modelData.when; color: "#fff"; font.family: Theme.fontText; font.pixelSize: 15; font.weight: Font.DemiBold }
                        Text { text: modelData.info; color: Theme.textDim; font.family: Theme.fontText; font.pixelSize: 12 }
                    }
                    GhostButton { compact: true; text: Theme.tr("saves_restore"); enabled: !Saves.running
                                  anchors.right: parent.right; anchors.rightMargin: 14; anchors.verticalCenter: parent.verticalCenter
                                  onClicked: Saves.askRestore(modelData.name) }
                }
            }
            Item { width: 1; height: 60 }
        }
    }

    // ---- result / error line ----------------------------------------------------------------------------
    Rectangle {
        visible: Saves.message !== ""
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        anchors.leftMargin: page.side; anchors.rightMargin: page.side; anchors.bottomMargin: 16
        height: msg.implicitHeight + 24; radius: 10
        readonly property color tone: Saves.messageTone === "bad" ? Theme.bad : Saves.messageTone === "good" ? Theme.ok
                                      : Saves.messageTone === "warn" ? Theme.warn : Theme.textMuted
        color: "#0e1014"; border.width: 1; border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.7)
        Text { id: msg; x: 16; y: 12; width: parent.width - 60; text: Saves.message; wrapMode: Text.WordWrap; color: Theme.text
               font.family: Theme.fontText; font.pixelSize: 13; lineHeight: 1.3; lineHeightMode: Text.ProportionalHeight }
        Text { text: "✕"; color: Theme.textMuted; anchors.right: parent.right; anchors.rightMargin: 16; y: 12
               font.pixelSize: 14
               MouseArea { anchors.fill: parent; anchors.margins: -8; cursorShape: Qt.PointingHandCursor; onClicked: Saves.dismissMessage() } }
    }

    ConfirmCard { anchors.fill: parent }

    property string folderPurpose: "data"
    FolderDialog {
        id: folderDialog
        title: Theme.tr(page.folderPurpose === "data" ? "saves_choose_title" : "backups_extra_title")
        currentFolder: "file://" + (page.folderPurpose === "data" ? Saves.userDir : Saves.extraDir)
        onAccepted: page.folderPurpose === "data" ? Saves.chooseDir(selectedFolder.toString()) : Saves.chooseExtra(selectedFolder.toString())
    }
    FileDialog {
        id: openZip
        title: Theme.tr("saves_zip_open_title")
        nameFilters: ["ZIP (*.zip)"]
        onAccepted: Saves.importZip(selectedFile.toString())
    }
    FileDialog {
        id: saveZip
        title: Theme.tr("saves_zip_save_title")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "zip"
        nameFilters: ["ZIP (*.zip)"]
        onAccepted: page.exportTarget === "" ? Saves.exportAll(selectedFile.toString()) : Saves.exportSlot(page.exportTarget, selectedFile.toString())
    }
}
