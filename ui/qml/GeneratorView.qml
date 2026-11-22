import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// Module "Générateur": a signal composed channel by channel, each channel the sum of waveforms (sine,
// square, triangle, sawtooth, pulses, constant, step, Dirac, noise, sweep) and of an offset. The recipe
// is edited in the right panel (Composition) and from the tool header; the signal it gives is computed a
// moment after each change and shown here with its spectrum (its measurements: panel Mesures), then added
// to the session in one click (undoable). The recipe is remembered from one run to the next.
Item {
    id: root
    required property var generator
    required property var player
    required property var session
    required property var appSettings
    property int channel: 0 // the channel edited in the panel Composition
    property Item ribbonHost: null
    property bool leftFolded: false
    property bool rightFolded: false
    signal unfoldLeft()
    signal unfoldRight()
    signal channelSelected(int index)
    signal compositionRequested()
    signal signalAdded(var id)
    signal infoMessage(string text)
    signal errorMessage(string text)

    property int ribbonTab: 0
    readonly property var recipe: generator.recipe
    readonly property var channels: recipe.channels ?? []
    readonly property string channelName: (channels[Math.min(channel, channels.length - 1)] ?? {}).name ?? ""
    readonly property var analysis: generator.previewAnalysis
    readonly property var settings: analysis.settings
    readonly property bool decibel: settings.scale !== "linear"
    // The plot pointed at last: the buttons of the view act on it.
    property Item lastZoomed: null
    readonly property Item activePlot: lastZoomed && lastZoomed.visible ? lastZoomed : timePlot

    function set(values) {
        analysis.setSettings(values)
    }
    function setCursors(on) {
        timePlot.cursorsVisible = on
        spectrum.cursorsVisible = on
    }
    function addToSession() {
        const id = generator.addToSession()
        if (id > 0) {
            root.signalAdded(id)
            root.infoMessage(qsTr("« %1 » est dans la session (Ctrl+Z l'enlève) ; sa recette le suit (panneau Signal).")
                             .arg(session.signalList.info(id).name ?? ""))
        }
        // A failure: the window shows generator.lastError.
    }
    function addComponent(key) {
        if (generator.addComponent(root.channel, key) >= 0)
            root.compositionRequested()
    }

    // While the preview plays, each new preview plays in its place: what is changed in the recipe is heard.
    readonly property bool previewReady: root.generator.ready
    onPreviewReadyChanged: if (previewReady) listenGroup.relisten()

    // Generated only while the module is on screen; so is the analysis of the preview.
    Binding {
        target: root.generator
        property: "active"
        value: root.visible
    }
    Binding {
        target: root.analysis
        property: "signalId"
        value: root.visible ? root.generator.previewId : 0
    }

    // The recipe of the last run comes back; each change is remembered a moment after.
    Component.onCompleted: {
        const saved = appSettings.value("generator/recipe", "")
        if (saved && saved.length > 0)
            generator.restoreRecipe(saved)
    }
    Timer {
        id: rememberTimer
        interval: 800
        onTriggered: root.appSettings.setValue("generator/recipe", root.generator.recipeText())
    }

    // Another rate or duration: the plots show all of the new preview (its spectrum once computed).
    property string shownSpan: ""
    property bool fitSpectrum: false
    Connections {
        target: root.generator
        function onRecipeChanged() { rememberTimer.restart() }
        function onStateChanged() {
            if (!root.generator.ready)
                return
            const span = root.recipe.sampleRate + "|" + root.recipe.duration
            if (span === root.shownSpan)
                return
            root.shownSpan = span
            timePlot.showAll()
            root.fitSpectrum = true
        }
    }
    Connections {
        target: root.analysis
        function onResultChanged() {
            if (root.fitSpectrum && root.analysis.hasResult) {
                root.fitSpectrum = false
                spectrum.showAll()
            }
        }
    }
    Connections {
        target: timePlot
        function onViewChanged() { if (timePlot.canGoBack) root.lastZoomed = timePlot }
    }
    Connections {
        target: spectrum
        function onViewChanged() { if (spectrum.canGoBack) root.lastZoomed = spectrum }
    }
    // The wheel while generating: shown only if it lasts.
    property bool showBusy: false
    Timer {
        id: busyDelay
        interval: 300
        onTriggered: root.showBusy = root.generator.busy
    }
    Connections {
        target: root.generator
        function onBusyChanged() {
            if (root.generator.busy)
                busyDelay.restart()
            else
                root.showBusy = false
        }
    }

    Ribbon {
        parent: root.ribbonHost ? root.ribbonHost : root
        visible: root.visible
        tabs: [qsTr("Composer"), qsTr("Visualiser")]
        tabIcons: ["generator", "signals"]
        tabTips: [qsTr("La recette du signal : exemples, échantillonnage, formes d'onde et voies (le détail : panneau Composition)"),
                  qsTr("Regarder et mesurer l'aperçu : vue, voies, curseurs, spectre, image")]
        currentTab: root.ribbonTab
        onTabClicked: (index) => root.ribbonTab = index

        // ---- In both tabs: into the session, listen to the preview ----
        RibbonGroup {
            title: qsTr("Session")
            RibbonButton {
                glyph: "keep"
                text: qsTr("Ajouter")
                primary: root.generator.ready
                enabled: root.generator.ready
                tip: root.generator.ready
                     ? qsTr("Le signal de l'aperçu devient un signal de la session, dans la liste de gauche (Ctrl+Z l'enlève) ; sa recette le suit")
                     : root.generator.busy ? qsTr("Génération en cours…")
                     : root.generator.error.length > 0 ? root.generator.error : qsTr("L'aperçu se met à jour…")
                onClicked: root.addToSession()
            }
        }
        ListenGroup {
            id: listenGroup
            player: root.player
            session: root.generator.preview
            signalId: root.generator.hasPreview ? root.generator.previewId : 0
            channels: root.channels
            onInfoMessage: (text) => root.infoMessage(text)
            onErrorMessage: (text) => root.errorMessage(text)
        }

        // ---- Composer: the recipe ----
        RibbonGroup {
            visible: root.ribbonTab === 0
            title: qsTr("Recette")
            RibbonButton {
                id: examplesButton
                glyph: "preset"
                text: qsTr("Exemples")
                dropDown: true
                tip: qsTr("Des recettes toutes prêtes : sinus et bruit, multiton, carré avec ou sans repliement, balayage, échelon, Dirac, réseau 50 Hz…")
                onClicked: examplesMenu.popup(examplesButton, 0, examplesButton.height)
                Menu {
                    id: examplesMenu
                    Instantiator {
                        model: root.generator.examples
                        delegate: MenuItem {
                            required property var modelData
                            required property int index
                            text: modelData.name
                            onTriggered: root.generator.useExample(index)
                            ToolTip.visible: hovered
                            ToolTip.delay: 400
                            ToolTip.text: modelData.tip
                        }
                        onObjectAdded: (index, object) => examplesMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) => examplesMenu.removeItem(object)
                    }
                }
            }
            RibbonButton {
                glyph: "rotate"
                text: qsTr("Par défaut")
                tip: qsTr("Repartir d'une seule voie : un sinus de 1 kHz, 1 V crête, échantillonné à 48 kHz pendant 1 s")
                onClicked: {
                    root.generator.reset()
                    root.channelSelected(0)
                }
            }
        }
        RibbonGroup {
            visible: root.ribbonTab === 0
            title: qsTr("Échantillonnage")
            RibbonField {
                anchors.verticalCenter: parent.verticalCenter
                label: qsTr("Fréquence")
                fieldWidth: 84
                unit: "Hz"
                minimum: 0.001
                maximum: 1e11
                value: root.recipe.sampleRate ?? 48000
                tip: qsTr("Fréquence d'échantillonnage fs (48 kHz, 1 MHz, 100 Hz…) : les fréquences au-delà de fs/2 se replient")
                onEdited: (v) => root.generator.setSignalSettings({ sampleRate: v })
            }
            RibbonField {
                anchors.verticalCenter: parent.verticalCenter
                label: qsTr("Durée")
                fieldWidth: 74
                unit: "s"
                minimum: 1e-9
                maximum: 1e9
                value: root.recipe.duration ?? 1
                tip: root.generator.sizeText.length > 0 ? root.generator.sizeText : root.generator.problem
                onEdited: (v) => root.generator.setSignalSettings({ duration: v })
            }
        }
        RibbonGroup {
            visible: root.ribbonTab === 0
            title: qsTr("Composer")
            RibbonButton {
                id: componentButton
                glyph: "addcurve"
                text: qsTr("Forme d'onde")
                dropDown: true
                tip: qsTr("Ajouter une forme d'onde à « %1 » : les formes d'onde d'une voie s'additionnent (sinus + bruit, multiton…)").arg(root.channelName)
                onClicked: componentMenu.popup(componentButton, 0, componentButton.height)
                Menu {
                    id: componentMenu
                    Instantiator {
                        model: Choices.generatorWaveforms
                        delegate: MenuItem {
                            required property var modelData
                            text: modelData.name
                            onTriggered: root.addComponent(modelData.key)
                            ToolTip.visible: hovered
                            ToolTip.delay: 400
                            ToolTip.text: modelData.tip
                        }
                        onObjectAdded: (index, object) => componentMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) => componentMenu.removeItem(object)
                    }
                }
            }
            RibbonButton {
                glyph: "addchannel"
                text: qsTr("Voie")
                enabled: root.channels.length < 16
                tip: qsTr("Ajouter une voie (16 au plus), un sinus de 1 kHz pour commencer : chaque voie a ses formes d'onde, son nom et son unité")
                onClicked: {
                    const index = root.generator.addChannel()
                    if (index >= 0) {
                        root.channelSelected(index)
                        root.compositionRequested()
                    }
                }
            }
        }

        // ---- Visualiser: look at the preview and measure it ----
        ViewGroup {
            visible: root.ribbonTab === 1
            target: root.activePlot
            plots: [timePlot, spectrum]
            backTarget: root.lastZoomed && root.lastZoomed.canGoBack ? root.lastZoomed
                        : spectrum.canGoBack ? spectrum : timePlot
            fitTip: qsTr("Tout le signal et toutes les fréquences, échelles automatiques (double clic sur un tracé : ce tracé seulement)")
        }
        RibbonGroup {
            visible: root.ribbonTab === 1
            title: qsTr("Voies")
            RibbonButton {
                glyph: "overlay"
                text: qsTr("Superposées")
                checked: !timePlot.stacked
                enabled: timePlot.hasData
                tip: qsTr("Les voies sur un même tracé (une échelle par unité, à gauche et à droite), ou une bande chacune ; clic droit sur le tracé : choisir les voies")
                onClicked: timePlot.stacked = !timePlot.stacked
            }
        }
        RibbonGroup {
            visible: root.ribbonTab === 1
            title: qsTr("Mesurer")
            RibbonButton {
                glyph: "cursors"
                text: qsTr("Curseurs")
                checked: timePlot.cursorsVisible
                enabled: timePlot.hasData
                tip: qsTr("Curseurs A et B sur le signal (Δt, valeurs, statistiques entre A et B) et sur le spectre (Δf, écart de niveau)")
                onClicked: root.setCursors(!timePlot.cursorsVisible)
            }
            RibbonButton {
                glyph: "peak"
                text: qsTr("Pic")
                checked: spectrum.markerText.length > 0
                enabled: spectrum.hasData
                tip: spectrum.markerText.length > 0 ? qsTr("Retirer le marqueur") : qsTr("Marqueur sur le plus haut pic du spectre")
                onClicked: spectrum.markerText.length > 0 ? spectrum.clearMarker() : spectrum.markPeak(0)
            }
            RibbonButton {
                glyph: "nextpeak"
                text: qsTr("Suivant")
                enabled: spectrum.markerText.length > 0
                tip: qsTr("Le marqueur sur le pic suivant, moins haut (clic droit : le précédent)")
                onClicked: spectrum.markPeak(1)
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: spectrum.markPeak(-1)
                }
            }
        }
        RibbonGroup {
            visible: root.ribbonTab === 1
            title: qsTr("Spectre")
            RibbonButton {
                glyph: "decibel"
                text: qsTr("Décibels")
                checked: root.decibel
                tip: qsTr("20 log₁₀ de l'amplitude : 0 dBV pour 1 V crête ; linéaire : dans l'unité de la voie (réglages : clic droit sur le spectre)")
                onClicked: root.set({ scale: root.decibel ? "linear" : "db" })
            }
            RibbonButton {
                glyph: "logscale"
                text: qsTr("Log")
                checked: root.settings.logFrequency === true
                tip: qsTr("Échelle logarithmique des fréquences : chaque décade a la même largeur")
                onClicked: root.set({ logFrequency: !(root.settings.logFrequency === true) })
            }
            RibbonButton {
                glyph: "harmonics"
                text: qsTr("Harmoniques")
                checked: spectrum.showHarmonics
                tip: qsTr("Repère H2, H3… les harmoniques trouvées par la mesure de tonalité")
                onClicked: spectrum.showHarmonics = !spectrum.showHarmonics
            }
        }
        ImageActions {
            visible: root.ribbonTab === 1
            target: split
            baseName: "signal-genere"
            onDone: (m) => root.infoMessage(m)
            onFailed: (m) => root.errorMessage(m)
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.canvas
    }

    SplitView {
        id: split
        anchors.fill: parent
        anchors.leftMargin: root.leftFolded ? 22 : 0
        anchors.rightMargin: root.rightFolded ? 22 : 0
        orientation: Qt.Vertical
        handle: Rectangle {
            implicitHeight: 5
            color: SplitHandle.hovered || SplitHandle.pressed ? Qt.alpha(Theme.accent, 0.5) : Theme.border
        }

        // The signal of the recipe.
        Item {
            SplitView.fillHeight: true
            SplitView.minimumHeight: 120
            SignalPlot {
                id: timePlot
                anchors.fill: parent
                anchors.bottomMargin: timeBar.height
                session: root.generator.preview
                signalId: root.generator.previewId
                stacked: true
                backgroundColor: Theme.canvas
                gridColor: Theme.grid
                textColor: Theme.text
                mutedColor: Theme.textMuted
                accentColor: Theme.accent
                panelColor: Theme.panel
                channelColors: Theme.channelColors
                emptyText: root.generator.error.length > 0 ? root.generator.error : qsTr("Génération de l'aperçu…")
                onContextMenuRequested: (x, y) => signalMenu.openAt(timePlot, x, y)
                onScaleMenuRequested: (x, y, scale) => scaleMenu.openFor(timePlot, x, y, scale)
                HoverHandler {
                    onHoveredChanged: if (hovered) root.lastZoomed = timePlot
                }
            }
            ViewScrollBar {
                id: timeBar
                view: timePlot
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.leftMargin: 62
                anchors.rightMargin: 14
                anchors.bottomMargin: 2
                height: implicitHeight
            }
            CursorReadout {
                view: timePlot
            }
            // Top left: what it is, and whether it is up to date.
            Rectangle {
                id: previewChip
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.topMargin: 8
                anchors.leftMargin: 70
                implicitWidth: stateRow.implicitWidth + 16
                implicitHeight: 28
                radius: 6
                color: Qt.alpha(Theme.panel, 0.92)
                border.color: Theme.border
                RowLayout {
                    id: stateRow
                    anchors.centerIn: parent
                    spacing: 6
                    Label {
                        text: qsTr("Aperçu")
                        color: Theme.textMuted
                        font.pixelSize: Theme.smallText
                        font.bold: true
                    }
                    Label {
                        text: root.recipe.name ?? ""
                        color: Theme.text
                        font.pixelSize: Theme.smallText
                        elide: Text.ElideRight
                        Layout.maximumWidth: 260
                    }
                    BusyIndicator {
                        visible: root.showBusy
                        running: visible
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                    }
                    Label {
                        visible: root.showBusy && root.generator.progress > 0
                        text: Math.round(100 * root.generator.progress) + " %"
                        color: Theme.textMuted
                        font.pixelSize: Theme.smallText
                    }
                    Label {
                        text: "?"
                        font.pixelSize: Theme.bodyText
                        font.bold: true
                        color: infoArea.containsMouse ? Theme.text : Theme.textMuted
                        Layout.leftMargin: 2
                        Layout.rightMargin: 2
                        MouseArea {
                            id: infoArea
                            anchors.fill: parent
                            anchors.margins: -4
                            hoverEnabled: true
                        }
                        ToolTip.visible: infoArea.containsMouse
                        ToolTip.text: {
                            const lines = []
                            if (root.generator.sizeText.length > 0)
                                lines.push(root.generator.sizeText)
                            for (let c = 0; c < root.channels.length; ++c) {
                                const ch = root.channels[c]
                                const parts = []
                                for (let k = 0; k < (ch.components ?? []).length; ++k) {
                                    if (ch.components[k].enabled !== false)
                                        parts.push(root.generator.componentText(c, k))
                                }
                                if ((ch.offset ?? 0) !== 0)
                                    parts.push(qsTr("décalage %1").arg(Units.compact(ch.offset, ch.unit ?? "", 4)))
                                lines.push(ch.name + " : " + (parts.length > 0 ? parts.join(" + ") : qsTr("nulle")))
                            }
                            lines.push(qsTr("Le détail de la recette : panneau Composition ; ses mesures : panneau Mesures."))
                            return lines.join("\n")
                        }
                    }
                }
            }
            ColumnLayout {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: 8
                anchors.rightMargin: 18
                spacing: 6
                PlotToolbar {
                    Layout.alignment: Qt.AlignRight
                    view: timePlot
                }
            }
            // What the generation warns about (aliasing, a Dirac out of the record...).
            Rectangle {
                visible: root.generator.warnings.length > 0
                anchors.left: previewChip.left
                anchors.top: previewChip.bottom
                anchors.topMargin: 6
                width: Math.min(warningLabel.implicitWidth + 20, parent.width - 100)
                height: warningLabel.implicitHeight + 10
                radius: 6
                color: Qt.alpha(Theme.panel, 0.94)
                border.color: Theme.warning
                Label {
                    id: warningLabel
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    text: "⚠ " + (root.generator.warnings[0] ?? "")
                          + (root.generator.warnings.length > 1 ? qsTr(" (et %1 autres)").arg(root.generator.warnings.length - 1) : "")
                    color: Theme.warning
                    font.pixelSize: Theme.smallText
                }
                HoverHandler { id: warningHover }
                ToolTip.visible: warningHover.hovered
                ToolTip.delay: 300
                ToolTip.text: root.generator.warnings.join("\n")
            }
        }

        // Its spectrum: every channel.
        Item {
            SplitView.preferredHeight: Math.round(root.height * 0.42)
            SplitView.minimumHeight: 140
            SpectrumPlot {
                id: spectrum
                anchors.fill: parent
                anchors.bottomMargin: spectrumBar.height
                analysis: root.analysis
                logFrequency: root.settings.logFrequency === true
                backgroundColor: Theme.canvas
                gridColor: Theme.grid
                textColor: Theme.text
                mutedColor: Theme.textMuted
                accentColor: Theme.accent
                panelColor: Theme.panel
                channelColors: Theme.channelColors
                emptyText: root.analysis.error.length > 0 ? root.analysis.error
                           : root.generator.hasPreview ? qsTr("Calcul du spectre…") : ""
                onContextMenuRequested: (x, y) => spectrumMenu.openAt(spectrum, x, y)
                onScaleMenuRequested: (x, y, scale) => scaleMenu.openFor(spectrum, x, y, scale)
                HoverHandler {
                    onHoveredChanged: if (hovered) root.lastZoomed = spectrum
                }
            }
            ViewScrollBar {
                id: spectrumBar
                view: spectrum
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.leftMargin: 62
                anchors.rightMargin: 14
                anchors.bottomMargin: 2
                height: implicitHeight
            }
            CursorReadout {
                view: spectrum
                deltaName: "Δf"
            }
            // The peak marked (Pic, Suivant).
            Rectangle {
                visible: spectrum.markerText.length > 0
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                anchors.bottomMargin: 48
                anchors.rightMargin: 22
                width: markerLabel.implicitWidth + 16
                height: markerLabel.implicitHeight + 8
                radius: 6
                color: Qt.alpha(Theme.panel, 0.94)
                border.color: Theme.border
                Label {
                    id: markerLabel
                    anchors.centerIn: parent
                    text: "◆ " + spectrum.markerText
                    color: Theme.text
                    font.pixelSize: Theme.smallText
                }
            }
            // Top right: how it is computed ("?"), and a wheel while it is.
            Rectangle {
                visible: spectrum.hasData || root.analysis.busy
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: 8
                anchors.rightMargin: 18
                implicitWidth: spectrumRow.implicitWidth + 12
                implicitHeight: 28
                radius: 6
                color: Qt.alpha(Theme.panel, 0.92)
                border.color: Theme.border
                RowLayout {
                    id: spectrumRow
                    anchors.centerIn: parent
                    spacing: 6
                    Label {
                        text: qsTr("Spectre de chaque voie")
                        color: Theme.textMuted
                        font.pixelSize: Theme.smallText
                    }
                    BusyIndicator {
                        visible: root.analysis.busy
                        running: visible
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                    }
                    Label {
                        id: spectrumInfoLabel
                        readonly property string spectrumInfo: {
                            const lines = []
                            if (root.analysis.summary.length > 0)
                                lines.push(root.analysis.summary)
                            const notes = root.analysis.notes
                            for (let i = 0; i < notes.length; ++i)
                                lines.push(notes[i])
                            lines.push(qsTr("Réglages du calcul : clic droit sur le spectre, ou le panneau Mesures."))
                            return lines.join("\n")
                        }
                        text: "?"
                        font.pixelSize: Theme.bodyText
                        font.bold: true
                        color: spectrumInfoArea.containsMouse ? Theme.text : Theme.textMuted
                        Layout.leftMargin: 2
                        Layout.rightMargin: 2
                        MouseArea {
                            id: spectrumInfoArea
                            anchors.fill: parent
                            anchors.margins: -4
                            hoverEnabled: true
                        }
                        ToolTip.visible: spectrumInfoArea.containsMouse
                        ToolTip.text: spectrumInfoLabel.spectrumInfo
                    }
                }
            }
        }
    }

    // Right click on a scale of a plot: its ends, automatic; for the spectrum, log frequencies and dB.
    ScaleMenu {
        id: scaleMenu
        optionsProvider: (scale) => {
            if (scaleMenu.view !== spectrum)
                return []
            if (scale.kind === "frequency")
                return [{ key: "log", text: qsTr("Fréquences logarithmiques"), checked: root.settings.logFrequency === true,
                          enabled: !root.analysis.twoSided }]
            if (scale.kind === "levels")
                return [{ key: "db", text: qsTr("Niveaux en décibels"), checked: root.decibel, enabled: true }]
            return []
        }
        onOptionToggled: (key, on) => {
            if (key === "log")
                root.set({ logFrequency: on })
            else if (key === "db")
                root.set({ scale: on ? "db" : "linear" })
        }
    }

    // Right click on a plot: its channels, a few actions; for the spectrum, how it is computed.
    PlotMenu {
        id: signalMenu
        view: timePlot
        mode: "channels"
        actions: [{ key: "add", text: qsTr("Ajouter à la session") }]
        onActionTriggered: (key) => { if (key === "add") root.addToSession() }
    }
    PlotMenu {
        id: spectrumMenu
        view: spectrum
        mode: "plain"
        actions: [{ key: "add", text: qsTr("Ajouter à la session") }]
        onActionTriggered: (key) => { if (key === "add") root.addToSession() }
        settingsTitle: qsTr("Calcul du spectre")
        settings: Component {
            SpectrumSettings {
                analysis: root.analysis
                compact: true
            }
        }
    }

    EdgeTab {
        visible: root.leftFolded
        atLeft: true
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        toolTip: qsTr("Afficher la liste des signaux (F9)")
        onClicked: root.unfoldLeft()
    }
    EdgeTab {
        visible: root.rightFolded
        atLeft: false
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        toolTip: qsTr("Afficher la composition et les mesures (F10)")
        onClicked: root.unfoldRight()
    }
}
