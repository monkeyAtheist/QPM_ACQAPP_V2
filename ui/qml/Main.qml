import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts
import Qacq.App

// The window: the shell of QTsignalApp V2 (module bar, tool header across the window, side panels that
// fold away) around the modules. One session: its signals are listed on the left in every module, the
// properties are on the right.
ApplicationWindow {
    id: window
    required property var session
    required property var acquisition
    required property var serialPorts
    required property var appSettings

    width: 1480
    height: 900
    minimumWidth: 1100
    minimumHeight: 680
    visible: true
    // session.title is already "name • — QTacquisitionApp".
    title: session.title
    readonly property string sessionName: session.fileName.length > 0 ? session.fileName : qsTr("Sans titre")

    Material.theme: Theme.dark ? Material.Dark : Material.Light
    Material.accent: Theme.accent
    Material.primary: Theme.panel
    Material.background: Theme.window
    color: Theme.window

    // 0 Signaux, 1 Acquisition (the other modules come in phase 4).
    property int module: 1
    property var selectedId: 0
    property int inspectorTab: 0 // 0 settings of the module, 1 the selected signal
    property bool signalsPanelOpen: true
    property bool inspectorOpen: true
    property bool darkTheme: true
    // An action waiting for the answer to "save the session first?", and one waiting for a file name.
    property var pendingAction: null
    property var afterSave: null
    property bool closeConfirmed: false

    // Remembered between runs (per user, under "QTacquisitionApp").
    Settings {
        id: windowSettings
        category: "window"
        property alias signalsPanelOpen: window.signalsPanelOpen
        property alias inspectorOpen: window.inspectorOpen
        property alias darkTheme: window.darkTheme
        property alias module: window.module
        property var splitState
    }
    Binding {
        target: Theme
        property: "dark"
        value: window.darkTheme
    }

    function folderOf(path) {
        if (!path || path.length === 0)
            return StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        return path.startsWith("/") ? "file://" + path : "file:///" + path
    }
    function showError(message) {
        if (!message || message.length === 0)
            return
        messageLabel.text = message
        messagePopup.error = true
        messagePopup.open()
    }
    function showInfo(message) {
        if (!message || message.length === 0)
            return
        messageLabel.text = message
        messagePopup.error = false
        messagePopup.open()
    }
    function selectSignal(id) {
        selectedId = id
        if (id > 0)
            inspectorTab = 1
    }
    function selectAfterChange() {
        const list = session.signalList
        if (selectedId > 0 && list.rowOf(selectedId) >= 0)
            return
        selectedId = list.count > 0 ? list.idAt(list.count - 1) : 0
    }
    function switchModule(index) {
        if (index === module)
            return
        module = index
        inspectorTab = index === 1 ? 0 : (selectedId > 0 ? 1 : 0)
    }
    // Runs `action` once the session is saved or its changes given up (asks when there are some).
    function confirmDiscard(action) {
        if (!session.modified) {
            action()
            return
        }
        pendingAction = action
        discardDialog.open()
    }
    function saveThen(action) {
        if (session.fileUrl.toString().length > 0) {
            if (session.save() && action)
                action()
            return
        }
        afterSave = action
        saveDialog.open()
    }
    function newSession() {
        if (acquisition.running)
            acquisition.stop(false)
        session.newSession()
        selectedId = 0
    }
    function openSession(url) {
        if (acquisition.running)
            acquisition.stop(true)
        if (session.open(url)) {
            selectedId = 0
            selectAfterChange()
        }
    }
    // A WAV file is imported at once; a text file first shows what was recognised in it (ImportDialog).
    function importFile(url) {
        importOptions.openFor(url)
    }
    function imported(id) {
        if (id > 0) {
            selectSignal(id)
            if (session.lastWarnings.length > 0)
                showInfo(session.lastWarnings.join("\n"))
        }
    }
    function typing() {
        return window.activeFocusItem instanceof TextInput || window.activeFocusItem instanceof TextEdit
    }

    Component.onCompleted: {
        if (windowSettings.splitState)
            split.restoreState(windowSettings.splitState)
        // The acquisition settings of the last run.
        const saved = appSettings.value("acquisition/last", "")
        if (saved && saved.length > 0) {
            try {
                acquisition.setSettings(JSON.parse(saved))
            } catch (e) {
                console.warn("Réglages d'acquisition illisibles, réglages par défaut :", e)
            }
        } else {
            // First run: the simulator, ready to go without any hardware.
            acquisition.setSettings({
                name: qsTr("Essai du simulateur"),
                source: "simulated",
                declaredSampleRateHz: 1000,
                simulated: { channels: [
                    { waveform: "sine", amplitude: 1, frequencyHz: 5, phaseDegrees: 0, offset: 0, dutyCycle: 0.5, noise: 0.02 },
                    { waveform: "square", amplitude: 0.5, frequencyHz: 2, phaseDegrees: 0, offset: 0.25, dutyCycle: 0.3, noise: 0 }
                ] }
            })
        }
    }

    onClosing: (close) => {
        if (window.closeConfirmed)
            return
        // An acquisition in progress is kept as a signal, then the session question is asked.
        if (acquisition.running)
            acquisition.stop(true)
        if (session.modified) {
            close.accepted = false
            confirmDiscard(() => {
                window.closeConfirmed = true
                window.close()
            })
        }
    }

    Connections {
        target: window.session
        function onLastErrorChanged() { window.showError(window.session.lastError) }
        function onSessionReplaced() { window.selectAfterChange() }
    }
    Connections {
        target: window.session.signalList
        function onCountChanged() { window.selectAfterChange() }
        function onErrorOccurred(message) { window.showError(message) }
    }
    Connections {
        target: window.acquisition
        function onLastErrorChanged() { window.showError(window.acquisition.lastError) }
        function onLastSignalIdChanged() {
            if (window.acquisition.lastSignalId > 0)
                window.selectedId = window.acquisition.lastSignalId
        }
    }

    Shortcut { sequence: "F9"; onActivated: window.signalsPanelOpen = !window.signalsPanelOpen }
    Shortcut { sequence: "F10"; onActivated: window.inspectorOpen = !window.inspectorOpen }
    Shortcut { sequence: "F2"; onActivated: signalsPanel.renameSelected() }
    Shortcut {
        sequence: "Space"
        enabled: window.module === 1 && !window.typing()
        onActivated: acquisitionView.toggle()
    }

    // ---- Commands ----
    Action { id: newAction; text: qsTr("Nouveau"); shortcut: StandardKey.New; onTriggered: window.confirmDiscard(window.newSession) }
    Action { id: openAction; text: qsTr("Ouvrir…"); shortcut: StandardKey.Open; onTriggered: window.confirmDiscard(() => openDialog.open()) }
    Action { id: saveAction; text: qsTr("Enregistrer"); shortcut: StandardKey.Save; onTriggered: window.saveThen(null) }
    Action { id: saveAsAction; text: qsTr("Enregistrer sous…"); shortcut: StandardKey.SaveAs; onTriggered: { window.afterSave = null; saveDialog.open() } }
    Action { id: importAction; text: qsTr("Importer…"); shortcut: "Ctrl+I"; onTriggered: importDialog.open() }
    Action {
        id: exportAction
        text: qsTr("Exporter…")
        shortcut: "Ctrl+E"
        enabled: (window.session.signalList.count, window.selectedId > 0 && window.session.signalList.rowOf(window.selectedId) >= 0)
        onTriggered: { exportDialog.signalId = window.selectedId; exportDialog.open() }
    }
    Action {
        id: undoAction
        text: qsTr("Annuler")
        shortcut: StandardKey.Undo
        enabled: window.session.signalList.canUndo && !window.typing()
        onTriggered: window.session.signalList.undo()
    }
    Action {
        id: redoAction
        text: qsTr("Rétablir")
        shortcut: StandardKey.Redo
        enabled: window.session.signalList.canRedo && !window.typing()
        onTriggered: window.session.signalList.redo()
    }
    Action {
        id: deleteAction
        text: qsTr("Supprimer le signal")
        shortcut: StandardKey.Delete
        enabled: exportAction.enabled && !window.typing()
        onTriggered: window.session.signalList.removeSignal(window.selectedId)
    }

    FileDialog {
        id: openDialog
        title: qsTr("Ouvrir une session")
        currentFolder: window.folderOf(window.appSettings.lastDirectory)
        nameFilters: window.session.sessionFilters()
        onAccepted: window.openSession(selectedFile)
    }
    FileDialog {
        id: saveDialog
        title: qsTr("Enregistrer la session")
        fileMode: FileDialog.SaveFile
        currentFolder: window.folderOf(window.appSettings.lastDirectory)
        defaultSuffix: "qacq"
        nameFilters: window.session.sessionFilters()
        onAccepted: {
            if (window.session.saveAs(selectedFile) && window.afterSave) {
                const next = window.afterSave
                window.afterSave = null
                next()
            }
        }
        onRejected: window.afterSave = null
    }
    FileDialog {
        id: importDialog
        title: qsTr("Importer un signal")
        currentFolder: window.folderOf(window.appSettings.lastDirectory)
        nameFilters: window.session.importFilters()
        fileMode: FileDialog.OpenFiles
        onAccepted: {
            for (let i = 0; i < selectedFiles.length; ++i)
                window.importFile(selectedFiles[i])
        }
    }
    FileDialog {
        id: exportDialog
        property var signalId: 0
        title: qsTr("Exporter le signal")
        fileMode: FileDialog.SaveFile
        currentFolder: window.folderOf(window.appSettings.lastDirectory)
        defaultSuffix: "csv"
        nameFilters: window.session.exportFilters()
        onAccepted: {
            if (window.session.exportSignal(signalId, selectedFile, {}))
                window.showInfo(qsTr("Signal exporté : %1").arg(decodeURIComponent(selectedFile.toString().split("/").pop())))
        }
    }

    header: ToolBar {
        Material.background: Theme.panel
        height: 52
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 12
            spacing: 4

            Label {
                text: "QTacquisitionApp"
                font.pixelSize: 16
                font.bold: true
                color: Theme.text
            }
            Label {
                text: "  /  " + window.sessionName + (window.session.modified ? " •" : "")
                font.pixelSize: 15
                color: Theme.textMuted
                Layout.rightMargin: 18
                Layout.maximumWidth: 320
                elide: Text.ElideRight
            }
            ToolButton { action: newAction }
            ToolButton { action: openAction }
            ToolButton { action: saveAction }
            Rectangle { width: 1; height: 24; color: Theme.border; Layout.leftMargin: 6; Layout.rightMargin: 6 }
            ToolButton { action: importAction }
            ToolButton { action: exportAction }
            Rectangle { width: 1; height: 24; color: Theme.border; Layout.leftMargin: 6; Layout.rightMargin: 6 }
            ToolButton {
                action: undoAction
                ToolTip.visible: hovered && window.session.signalList.canUndo
                ToolTip.text: qsTr("Annuler : %1").arg(window.session.signalList.undoText)
            }
            ToolButton {
                action: redoAction
                ToolTip.visible: hovered && window.session.signalList.canRedo
                ToolTip.text: qsTr("Rétablir : %1").arg(window.session.signalList.redoText)
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                checkable: true
                checked: window.signalsPanelOpen
                onToggled: window.signalsPanelOpen = checked
                contentItem: Item {
                    implicitWidth: 24
                    implicitHeight: 24
                    PanelIcon { anchors.centerIn: parent; active: window.signalsPanelOpen }
                }
                ToolTip.visible: hovered
                ToolTip.text: (window.signalsPanelOpen ? qsTr("Masquer la liste des signaux") : qsTr("Afficher la liste des signaux")) + " (F9)"
            }
            ToolButton {
                checkable: true
                checked: window.inspectorOpen
                onToggled: window.inspectorOpen = checked
                contentItem: Item {
                    implicitWidth: 24
                    implicitHeight: 24
                    PanelIcon { anchors.centerIn: parent; rightSide: true; active: window.inspectorOpen }
                }
                ToolTip.visible: hovered
                ToolTip.text: (window.inspectorOpen ? qsTr("Masquer les propriétés") : qsTr("Afficher les propriétés")) + " (F10)"
            }
            Rectangle { width: 1; height: 24; color: Theme.border; Layout.leftMargin: 6; Layout.rightMargin: 6 }
            Switch {
                text: window.darkTheme ? qsTr("Sombre") : qsTr("Clair")
                checked: window.darkTheme
                onToggled: window.darkTheme = checked
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.border
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Sidebar {
            Layout.fillHeight: true
            current: window.module
            onModuleSelected: (index) => window.switchModule(index)
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // Tool header of the module, across the list, the view and the properties: the view of the
            // module puts its header there, and shows it with itself.
            Item {
                id: moduleRibbon
                Layout.fillWidth: true
                Layout.preferredHeight: 110
            }

            SplitView {
                id: split
                Layout.fillWidth: true
                Layout.fillHeight: true
                orientation: Qt.Horizontal
                onResizingChanged: if (!resizing) windowSettings.splitState = saveState()
                handle: Rectangle {
                    implicitWidth: 4
                    color: SplitHandle.hovered || SplitHandle.pressed ? Qt.alpha(Theme.accent, 0.5) : "transparent"
                }

                SignalsPanel {
                    id: signalsPanel
                    visible: window.signalsPanelOpen
                    SplitView.preferredWidth: 280
                    SplitView.minimumWidth: 220
                    session: window.session
                    selectedId: window.selectedId
                    onSignalSelected: (id) => window.selectSignal(id)
                    onCollapseRequested: window.signalsPanelOpen = false
                    onImportRequested: importDialog.open()
                    onExportRequested: (id) => { exportDialog.signalId = id; exportDialog.open() }
                }

                // Centre: the view of the module.
                Item {
                    SplitView.fillWidth: true
                    SplitView.minimumWidth: 420

                    SignalsView {
                        id: signalsView
                        anchors.fill: parent
                        visible: window.module === 0
                        ribbonHost: moduleRibbon
                        session: window.session
                        signalId: window.selectedId
                        leftFolded: !window.signalsPanelOpen
                        rightFolded: !window.inspectorOpen
                        onUnfoldLeft: window.signalsPanelOpen = true
                        onUnfoldRight: window.inspectorOpen = true
                        onImportRequested: importDialog.open()
                        onExportRequested: exportAction.trigger()
                        onRenameRequested: signalsPanel.renameSelected()
                    }
                    AcquisitionView {
                        id: acquisitionView
                        anchors.fill: parent
                        visible: window.module === 1
                        ribbonHost: moduleRibbon
                        session: window.session
                        acquisition: window.acquisition
                        serialPorts: window.serialPorts
                        signalId: window.selectedId
                        leftFolded: !window.signalsPanelOpen
                        rightFolded: !window.inspectorOpen
                        onUnfoldLeft: window.signalsPanelOpen = true
                        onUnfoldRight: window.inspectorOpen = true
                        onFirmwareRequested: firmwareDialog.open()
                        onSettingsRequested: {
                            window.inspectorOpen = true
                            window.inspectorTab = 0
                        }
                    }
                }

                Inspector {
                    visible: window.inspectorOpen
                    SplitView.preferredWidth: 360
                    SplitView.minimumWidth: 300
                    session: window.session
                    acquisition: window.acquisition
                    serialPorts: window.serialPorts
                    appSettings: window.appSettings
                    module: window.module
                    signalId: window.selectedId
                    tab: window.inspectorTab
                    onTabSelected: (index) => window.inspectorTab = index
                    onCollapseRequested: window.inspectorOpen = false
                    onFirmwareRequested: firmwareDialog.open()
                    onRecentFileChosen: (file) => window.confirmDiscard(() => window.openSession(window.folderOf(file)))
                }
            }
        }
    }

    // Files dropped on the window: a session opens, anything else is imported.
    DropArea {
        anchors.fill: parent
        onDropped: (drop) => {
            if (!drop.hasUrls)
                return
            drop.accept()
            for (let i = 0; i < drop.urls.length; ++i) {
                const url = drop.urls[i]
                if (url.toString().toLowerCase().endsWith(".qacq")) {
                    window.confirmDiscard(() => window.openSession(url))
                    return
                }
                window.importFile(url)
            }
        }
    }

    FirmwareDialog {
        id: firmwareDialog
        acquisition: window.acquisition
    }
    ImportDialog {
        id: importOptions
        session: window.session
        onImported: (id) => window.imported(id)
    }

    Dialog {
        id: discardDialog
        title: qsTr("Session modifiée")
        modal: true
        // A width given: with its row of buttons, the dialog would otherwise loop on its implicit width.
        width: 460
        anchors.centerIn: Overlay.overlay
        onRejected: window.pendingAction = null
        Label {
            width: parent ? parent.width : implicitWidth
            wrapMode: Text.WordWrap
            text: qsTr("Enregistrer les modifications de « %1 » ?").arg(window.sessionName)
            color: Theme.text
        }
        footer: DialogButtonBox {
            Button {
                text: qsTr("Enregistrer")
                highlighted: true
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                onClicked: {
                    const next = window.pendingAction
                    window.pendingAction = null
                    discardDialog.close()
                    window.saveThen(next)
                }
            }
            Button {
                text: qsTr("Ne pas enregistrer")
                flat: true
                DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
                onClicked: {
                    const next = window.pendingAction
                    window.pendingAction = null
                    discardDialog.close()
                    if (next)
                        next()
                }
            }
            Button {
                text: qsTr("Annuler")
                flat: true
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }
    }

    Popup {
        id: messagePopup
        property bool error: true
        x: (window.width - width) / 2
        y: 70
        padding: 14
        background: Rectangle {
            radius: Theme.radius
            color: Theme.panel
            border.color: messagePopup.error ? Theme.error : Theme.accent
        }
        onOpened: messageTimer.restart()
        Label {
            id: messageLabel
            color: Theme.text
            wrapMode: Text.WordWrap
            width: Math.min(implicitWidth, window.width * 0.6)
        }
        Timer {
            id: messageTimer
            interval: messagePopup.error ? 8000 : 4000
            onTriggered: messagePopup.close()
        }
    }
}
