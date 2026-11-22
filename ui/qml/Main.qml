import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts
import Qacq.App

// The window: the shell of QTsignalApp V2 (module bar, tool header across the window, side panels that
// fold away) around the modules. One session: its signals are listed on the left in every module; on the
// right, the measurements (Signaux), the settings of the source (Acquisition) or the composition of the
// signal (Générateur), and the signal chosen.
ApplicationWindow {
    id: window
    required property var session
    required property var acquisition
    required property var analysis
    required property var curves
    required property var generator
    required property var player
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

    // 0 Signaux (with the curves and the spectra), 1 Acquisition, 2 Générateur (filters and modulations
    // come next).
    property int module: 1
    property var selectedId: 0
    // 0 measurements or settings of the module, then the selected signal (1; 2 in the generator, whose
    // measurements of the preview are tab 1).
    property int inspectorTab: 0
    property int generatorChannel: 0 // the channel of the recipe edited
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
        // Not "module" any more: that one could be 2, the former module "Analyse" (now in Signaux).
        property alias moduleShown: window.module
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
        // In "Acquisition", a signal chosen shows its properties; in "Signaux", the panel keeps its tab
        // (the measurements follow the signal).
        if (id > 0 && module === 1)
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
        inspectorTab = 0
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
    // Files to import, one after the other: a WAV file is read at once; a text file first shows what was
    // recognised in it (ImportDialog). The reading runs on a thread of its own (importDialogProgress).
    property var importQueue: []
    function importFiles(urls) {
        const queue = importQueue.slice()
        for (let i = 0; i < urls.length; ++i)
            queue.push(urls[i])
        importQueue = queue
        nextImport()
    }
    function importFile(url) {
        importFiles([url])
    }
    function nextImport() {
        if (session.importing || importOptions.asking || importQueue.length === 0)
            return
        const url = importQueue[0]
        importQueue = importQueue.slice(1)
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
        if (module < 0 || module > 2)
            module = 0
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
        function onImportingChanged() {
            // Shown after a moment only: a small file comes in without a window flashing.
            if (window.session.importing) {
                importShowDelay.restart()
            } else {
                importShowDelay.stop()
                importProgressDialog.close()
            }
        }
        function onImportFinished(id, cancelled) {
            if (cancelled) {
                // "Annuler": the files still waiting are dropped too.
                window.importQueue = []
                window.showInfo(qsTr("Import annulé."))
                return
            }
            window.imported(id)
            window.nextImport()
        }
    }
    Timer {
        id: importShowDelay
        interval: 300
        onTriggered: if (window.session.importing) importProgressDialog.open()
    }
    Connections {
        target: window.session.signalList
        function onCountChanged() { window.selectAfterChange() }
        function onErrorOccurred(message) { window.showError(message) }
    }
    Connections {
        target: window.generator
        function onLastErrorChanged() { window.showError(window.generator.lastError) }
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
        onAccepted: window.importFiles(selectedFiles)
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
    FileDialog {
        id: curvesDialog
        title: qsTr("Exporter les courbes du temps")
        fileMode: FileDialog.SaveFile
        currentFolder: window.folderOf(window.appSettings.lastDirectory)
        defaultSuffix: "csv"
        nameFilters: [qsTr("Tableau CSV (*.csv)")]
        onAccepted: {
            // A failure is said by the list of the signals (errorOccurred).
            if (window.curves.exportTimeCurves(selectedFile))
                window.showInfo(qsTr("Courbes exportées : %1").arg(decodeURIComponent(selectedFile.toString().split("/").pop())))
        }
    }
    FileDialog {
        id: spectrumDialog
        title: qsTr("Exporter les spectres")
        fileMode: FileDialog.SaveFile
        currentFolder: window.folderOf(window.appSettings.lastDirectory)
        defaultSuffix: "csv"
        nameFilters: [qsTr("Tableau CSV (*.csv)")]
        onAccepted: {
            if (window.analysis.exportSpectrum(selectedFile))
                window.showInfo(qsTr("Spectres exportés : %1").arg(decodeURIComponent(selectedFile.toString().split("/").pop())))
            else
                window.showError(window.analysis.lastError)
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
                // Where the session is, and whether it is saved.
                HoverHandler { id: sessionHover }
                ToolTip.visible: sessionHover.hovered
                ToolTip.delay: 500
                ToolTip.text: (window.session.fileUrl.toString().length > 0
                               ? decodeURIComponent(window.session.fileUrl.toString().replace(/^file:\/{2,3}/, ""))
                               : qsTr("Session pas encore enregistrée"))
                              + "\n" + (window.session.modified ? qsTr("Modifications non enregistrées (Ctrl+S)") : qsTr("Tout est enregistré"))
            }
            ToolButton { action: newAction }
            ToolButton { action: openAction }
            // The sessions opened lately.
            ToolButton {
                id: recentButton
                visible: window.appSettings.recentFiles.length > 0
                text: "▾"
                leftPadding: 4
                rightPadding: 4
                Layout.leftMargin: -6
                onClicked: recentMenu.popup(recentButton, 0, recentButton.height)
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: qsTr("Sessions récentes")
                Menu {
                    id: recentMenu
                    Instantiator {
                        model: window.appSettings.recentFiles
                        delegate: MenuItem {
                            required property string modelData
                            text: modelData.split("/").pop()
                            onTriggered: window.confirmDiscard(() => window.openSession(window.folderOf(modelData)))
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            ToolTip.text: modelData
                        }
                        onObjectAdded: (index, object) => recentMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) => recentMenu.removeItem(object)
                    }
                    MenuSeparator { }
                    MenuItem {
                        text: qsTr("Vider la liste")
                        onTriggered: window.appSettings.clearRecentFiles()
                    }
                }
            }
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
                ToolTip.text: (window.inspectorOpen ? qsTr("Masquer le panneau de droite") : qsTr("Afficher le panneau de droite")) + " (F10)"
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
                        analysis: window.analysis
                        curves: window.curves
                        player: window.player
                        signalId: window.selectedId
                        leftFolded: !window.signalsPanelOpen
                        rightFolded: !window.inspectorOpen
                        onUnfoldLeft: window.signalsPanelOpen = true
                        onUnfoldRight: window.inspectorOpen = true
                        onImportRequested: importDialog.open()
                        onSpectrumExportRequested: spectrumDialog.open()
                        onTimeCurvesExportRequested: curvesDialog.open()
                        onCurveRequested: (view) => curveDialog.openFor(window.selectedId, view)
                        onInfoMessage: (text) => window.showInfo(text)
                        onErrorMessage: (text) => window.showError(text)
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
                        onInfoMessage: (text) => window.showInfo(text)
                        onErrorMessage: (text) => window.showError(text)
                    }
                    GeneratorView {
                        id: generatorView
                        anchors.fill: parent
                        visible: window.module === 2
                        ribbonHost: moduleRibbon
                        generator: window.generator
                        player: window.player
                        session: window.session
                        appSettings: window.appSettings
                        channel: window.generatorChannel
                        leftFolded: !window.signalsPanelOpen
                        rightFolded: !window.inspectorOpen
                        onUnfoldLeft: window.signalsPanelOpen = true
                        onUnfoldRight: window.inspectorOpen = true
                        onChannelSelected: (index) => window.generatorChannel = index
                        onCompositionRequested: {
                            window.inspectorOpen = true
                            window.inspectorTab = 0
                        }
                        onSignalAdded: (id) => window.selectSignal(id)
                        onInfoMessage: (text) => window.showInfo(text)
                        onErrorMessage: (text) => window.showError(text)
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
                    analysis: window.analysis
                    curves: window.curves
                    generator: window.generator
                    module: window.module
                    signalId: window.selectedId
                    tab: window.inspectorTab
                    generatorChannel: window.generatorChannel
                    onTabSelected: (index) => window.inspectorTab = index
                    onGeneratorChannelSelected: (index) => window.generatorChannel = index
                    onTakeUpRequested: (id) => {
                        if (window.generator.takeUp(id)) {
                            window.generatorChannel = 0
                            window.switchModule(2)
                        }
                    }
                    onCollapseRequested: window.inspectorOpen = false
                    onFirmwareRequested: firmwareDialog.open()
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
            const files = []
            for (let i = 0; i < drop.urls.length; ++i) {
                const url = drop.urls[i]
                if (url.toString().toLowerCase().endsWith(".qacq")) {
                    window.confirmDiscard(() => window.openSession(url))
                    return
                }
                files.push(url)
            }
            window.importFiles(files)
        }
    }

    FirmwareDialog {
        id: firmwareDialog
        acquisition: window.acquisition
    }
    CurveDialog {
        id: curveDialog
        session: window.session
        curves: window.curves
        onAdded: (count) => { if (count === 0) window.showInfo(qsTr("Ces courbes sont déjà affichées.")) }
    }
    ImportDialog {
        id: importOptions
        session: window.session
        // Not started (dialog cancelled, or refused at once): the next file.
        onSkipped: Qt.callLater(window.nextImport)
    }
    // While a file is read (on its own thread): which, how far, about how long still, and Annuler.
    Dialog {
        id: importProgressDialog
        // (No property named like a member of the Qt items: "left", "progress"... — Qt 6.11 refuses to
        // load a file that overrides a FINAL one.)
        property real startedMs: 0
        readonly property real readFraction: window.session.importProgress
        title: qsTr("Import en cours")
        modal: true
        closePolicy: Popup.NoAutoClose
        anchors.centerIn: Overlay.overlay
        width: 440
        onAboutToShow: {
            startedMs = Date.now() - importShowDelay.interval
            importProgressText.clockMs = Date.now()
        }
        footer: DialogButtonBox {
            Button {
                text: qsTr("Annuler")
                enabled: window.session.importing
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }
        // "Annuler": the reading stops at the next block (a fraction of a second); the dialog closes with it.
        onRejected: window.session.cancelImport()

        ColumnLayout {
            anchors.fill: parent
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: qsTr("Lecture de « %1 »").arg(window.session.importName)
                elide: Text.ElideMiddle
                color: Theme.text
                font.pixelSize: Theme.bodyText
            }
            ProgressBar {
                Layout.fillWidth: true
                from: 0
                to: 1
                value: importProgressDialog.readFraction
            }
            Label {
                id: importProgressText
                // The clock, read every half second (the time left moves even when the progress stalls).
                property real clockMs: Date.now()
                readonly property real secondsElapsed: (clockMs - importProgressDialog.startedMs) / 1000
                readonly property real secondsLeft: importProgressDialog.readFraction > 0.03
                                                    ? secondsElapsed * (1 - importProgressDialog.readFraction) / importProgressDialog.readFraction
                                                    : -1
                Layout.fillWidth: true
                text: Math.round(100 * importProgressDialog.readFraction) + " %"
                      + (secondsLeft >= 0 ? qsTr(" · encore environ %1").arg(Units.duration(Math.max(1, Math.round(secondsLeft)))) : "")
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
                Timer {
                    interval: 500
                    repeat: true
                    running: importProgressDialog.visible
                    onTriggered: importProgressText.clockMs = Date.now()
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Un gros fichier texte se lit ligne à ligne : quelques dizaines de mégaoctets par seconde.")
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
            }
        }
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
