import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Module "Signaux": the signal chosen in the list, and under it only the curves added (tab "Courbes" of
// the tool header): a graph of time (the channels themselves, integral, derivative, values per period)
// and a graph of spectrograms, both on the time axis of the signal, and a graph of the frequencies
// (spectra). With no curve, the signal takes the whole height. The right panel holds the measurements of the zone and the settings of
// the computations (tab "Mesures"), and the properties of the signal (tab "Signal"). Import is in the tool
// header (both tabs) and the top bar; export, rename and delete in the top bar and the list (right click,
// F2).
Item {
    id: root
    required property var session
    required property var analysis
    required property var curves
    required property var player
    property var signalId: 0
    property Item ribbonHost: null
    // The height its header needs (one row of groups, or more in a narrow window): the host takes it.
    readonly property real ribbonHeight: moduleHeader.implicitHeight
    property bool leftFolded: false
    property bool rightFolded: false
    signal unfoldLeft()
    signal unfoldRight()
    signal importRequested()
    signal spectrumExportRequested()
    signal timeCurvesExportRequested()
    signal curveRequested(var view)
    signal infoMessage(string text)
    signal errorMessage(string text)
    // Tab of the tool header: 0 "Visualiser", 1 "Courbes".
    property int ribbonTab: 0
    // The plot pointed at, zoomed or slid last: the group "Vue" acts on it.
    property Item lastZoomed: null
    readonly property Item activePlot: lastZoomed && lastZoomed.visible ? lastZoomed : timePlot

    // The count is read so that this follows the list (a signal deleted, an undo).
    readonly property bool hasSignal: (session.signalList.count, signalId > 0 && session.signalList.rowOf(signalId) >= 0)
    readonly property var settings: analysis.settings
    readonly property bool density: settings.kind === "density"
    readonly property bool decibel: settings.scale !== "linear"
    readonly property bool hasSpectra: curves.hasSpectra
    readonly property bool hasTimeCurves: curves.hasTimeCurves
    readonly property bool hasSpectrograms: curves.hasSpectrograms
    // A graph on the time axis under the signal: the signal keeps room on its right as they do (the time
    // axes line up).
    readonly property bool timeGraphsUnder: hasTimeCurves || hasSpectrograms

    function set(values) {
        analysis.setSettings(values)
    }
    // What the spectra and the measurements cover: the whole signal ("all", the zoom changes nothing),
    // the samples on screen in the plot of the signal ("view"), or those between its cursors A and B.
    property string zoneMode: "all"
    readonly property bool zoneOnCursors: zoneMode === "cursors" && timePlot.cursorsVisible && timePlot.cursorFrameCount > 1
    function pushZone() {
        if (!visible)
            return
        if (zoneMode === "all")
            analysis.setZone(0, -1)
        else if (zoneOnCursors)
            analysis.setZone(timePlot.cursorFirstFrame, timePlot.cursorFrameCount)
        else
            analysis.setZone(timePlot.viewFirstFrame, timePlot.viewFrameCount)
    }
    function setZoneMode(mode) {
        if (mode === "cursors" && !timePlot.cursorsVisible)
            setCursors(true)
        zoneMode = mode
        Qt.callLater(root.pushZone)
    }
    function setCursors(on) {
        if (!on && zoneMode === "cursors") {
            // No cursors, no zone between them: back to the whole signal.
            zoneMode = "all"
            Qt.callLater(root.pushZone)
        }
        timePlot.cursorsVisible = on
        curvePlot.cursorsVisible = on
        spectrogramPlot.cursorsVisible = on
        spectrum.cursorsVisible = on
        if (on && curvePlot.hasData) {
            curvePlot.cursorA = timePlot.cursorA
            curvePlot.cursorB = timePlot.cursorB
        }
        if (on) {
            spectrogramPlot.cursorA = timePlot.cursorA
            spectrogramPlot.cursorB = timePlot.cursorB
        }
    }
    onVisibleChanged: Qt.callLater(root.pushZone)
    onSignalIdChanged: Qt.callLater(root.pushZone)

    // The controllers work only for the module on screen (ten million samples are not computed for
    // nothing while another module is used).
    Binding {
        target: root.analysis
        property: "signalId"
        value: root.visible ? root.signalId : 0
    }
    Binding {
        target: root.curves
        property: "signalId"
        value: root.visible ? root.signalId : 0
    }
    // The plot of the signal leads; the graphs of time and of spectrograms follow its view and its cursors,
    // and the other way.
    Connections {
        target: timePlot
        function onViewChanged() {
            if (root.zoneMode === "view")
                Qt.callLater(root.pushZone)
            if (timePlot.canGoBack)
                root.lastZoomed = timePlot
            if (curvePlot.hasData)
                curvePlot.setTimeView(timePlot.viewLow, timePlot.viewHigh)
            spectrogramPlot.setTimeView(timePlot.viewLow, timePlot.viewHigh)
        }
        function onCursorsChanged() {
            // Hidden (button "Curseurs", or the cross of a readout): on every graph of the time axis, and
            // the zone "Entre A et B" goes back to "Tout" (it fell back silently to the view before).
            if (!timePlot.cursorsVisible) {
                if (curvePlot.cursorsVisible)
                    curvePlot.cursorsVisible = false
                if (spectrogramPlot.cursorsVisible)
                    spectrogramPlot.cursorsVisible = false
                if (root.zoneMode === "cursors") {
                    root.zoneMode = "all"
                    Qt.callLater(root.pushZone)
                }
                return
            }
            if (root.zoneMode === "cursors")
                Qt.callLater(root.pushZone)
            if (curvePlot.cursorsVisible) {
                curvePlot.cursorA = timePlot.cursorA
                curvePlot.cursorB = timePlot.cursorB
            }
            if (spectrogramPlot.cursorsVisible) {
                spectrogramPlot.cursorA = timePlot.cursorA
                spectrogramPlot.cursorB = timePlot.cursorB
            }
        }
    }
    Connections {
        target: curvePlot
        function onViewChanged() {
            if (curvePlot.canGoBack)
                root.lastZoomed = curvePlot
            timePlot.setTimeView(curvePlot.viewLow, curvePlot.viewHigh)
        }
        function onCursorsChanged() {
            if (timePlot.cursorsVisible) {
                timePlot.cursorA = curvePlot.cursorA
                timePlot.cursorB = curvePlot.cursorB
            }
        }
        function onContentChanged() {
            // New curves: on the time span and the cursors of the signal.
            if (curvePlot.hasData)
                curvePlot.setTimeView(timePlot.viewLow, timePlot.viewHigh)
            curvePlot.cursorsVisible = timePlot.cursorsVisible
            if (timePlot.cursorsVisible) {
                curvePlot.cursorA = timePlot.cursorA
                curvePlot.cursorB = timePlot.cursorB
            }
        }
    }
    Connections {
        target: spectrogramPlot
        function onViewChanged() {
            // Before its first result it only waits for the span of the signal (onContentChanged).
            if (!spectrogramPlot.hasData)
                return
            if (spectrogramPlot.canGoBack)
                root.lastZoomed = spectrogramPlot
            timePlot.setTimeView(spectrogramPlot.viewLow, spectrogramPlot.viewHigh)
        }
        function onCursorsChanged() {
            if (timePlot.cursorsVisible) {
                timePlot.cursorA = spectrogramPlot.cursorA
                timePlot.cursorB = spectrogramPlot.cursorB
            }
        }
        function onContentChanged() {
            // New spectrograms: on the time span and the cursors of the signal.
            spectrogramPlot.setTimeView(timePlot.viewLow, timePlot.viewHigh)
            spectrogramPlot.cursorsVisible = timePlot.cursorsVisible
            if (timePlot.cursorsVisible) {
                spectrogramPlot.cursorA = timePlot.cursorA
                spectrogramPlot.cursorB = timePlot.cursorB
            }
        }
    }
    Connections {
        target: spectrum
        function onViewChanged() { if (spectrum.canGoBack) root.lastZoomed = spectrum }
    }

    Ribbon {
        id: moduleHeader
        parent: root.ribbonHost ? root.ribbonHost : root
        visible: root.visible
        tabs: [qsTr("Visualiser"), qsTr("Courbes")]
        tabIcons: ["signals", "analysis"]
        tabTips: [qsTr("Regarder et mesurer le signal : vue, voies, curseurs, image"),
                  qsTr("Ce qu'on calcule à partir du signal : spectres, intégrale, dérivée, valeurs par période, et la zone analysée")]
        currentTab: root.ribbonTab
        onTabClicked: (index) => root.ribbonTab = index

        // ---- In both tabs: bring a file in, listen to the signal ----
        RibbonGroup {
            title: qsTr("Fichier")
            RibbonButton {
                glyph: "import"
                text: qsTr("Importer")
                primary: root.session.signalList.count === 0
                tip: qsTr("Un fichier CSV, texte ou WAV devient un signal de la session (Ctrl+I ; aussi en le déposant sur la fenêtre)")
                onClicked: root.importRequested()
            }
        }
        ListenGroup {
            player: root.player
            session: root.session
            signalId: root.hasSignal ? root.signalId : 0
            // (The count is read so that this follows the list.)
            channels: root.hasSignal ? (root.session.signalList.count, root.session.signalList.info(root.signalId).channels ?? []) : []
            onInfoMessage: (text) => root.infoMessage(text)
            onErrorMessage: (text) => root.errorMessage(text)
        }

        // ---- Visualiser: look and measure ----
        ViewGroup {
            visible: root.ribbonTab === 0
            target: root.activePlot
            plots: [timePlot, curvePlot, spectrogramPlot, spectrum]
            backTarget: root.lastZoomed && root.lastZoomed.canGoBack ? root.lastZoomed
                        : spectrum.canGoBack ? spectrum : spectrogramPlot.canGoBack ? spectrogramPlot
                        : curvePlot.canGoBack ? curvePlot : timePlot
            fitTip: qsTr("Tout le signal et toutes les fréquences, échelles automatiques (double clic sur un tracé : ce tracé seulement)")
        }
        RibbonGroup {
            visible: root.ribbonTab === 0
            title: qsTr("Voies")
            RibbonButton {
                glyph: "overlay"
                text: qsTr("Superposées")
                checked: !timePlot.stacked
                enabled: root.hasSignal
                tip: qsTr("Les voies du signal sur un même tracé (une échelle par unité, à gauche et à droite), ou une bande chacune ; clic droit sur le tracé : choisir les voies")
                onClicked: timePlot.stacked = !timePlot.stacked
            }
        }
        RibbonGroup {
            visible: root.ribbonTab === 0
            title: qsTr("Mesurer")
            RibbonButton {
                glyph: "cursors"
                text: qsTr("Curseurs")
                checked: timePlot.cursorsVisible
                enabled: root.hasSignal
                tip: qsTr("Curseurs A et B sur le signal, les courbes du temps et les spectrogrammes (Δt, valeurs, statistiques et intégrale entre A et B) et sur le spectre (Δf, écart de niveau, valeur efficace de la bande). Les mesures du panneau entre A et B : Courbes ▸ Zone analysée.")
                onClicked: root.setCursors(!timePlot.cursorsVisible)
            }
            RibbonButton {
                visible: root.hasSpectra
                glyph: "peak"
                text: qsTr("Pic")
                checked: spectrum.markerText.length > 0
                enabled: spectrum.hasData
                tip: spectrum.markerText.length > 0 ? qsTr("Retirer le marqueur") : qsTr("Marqueur sur le plus haut pic du spectre")
                onClicked: spectrum.markerText.length > 0 ? spectrum.clearMarker() : spectrum.markPeak(0)
            }
            RibbonButton {
                visible: root.hasSpectra
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
        ImageActions {
            visible: root.ribbonTab === 0
            target: split
            baseName: "signal"
            onDone: (m) => root.infoMessage(m)
            onFailed: (m) => root.errorMessage(m)
        }

        // ---- Courbes: what is computed from the signal ----
        RibbonGroup {
            visible: root.ribbonTab === 1
            title: qsTr("Courbes")
            RibbonButton {
                glyph: "addcurve"
                text: qsTr("Ajouter…")
                primary: root.hasSignal && !root.hasSpectra && !root.hasTimeCurves && !root.hasSpectrograms
                enabled: root.hasSignal
                tip: qsTr("Un spectre, un spectrogramme, ou une courbe du temps (voie d'origine, intégrale, dérivée, valeurs par période) sous le signal")
                onClicked: root.curveRequested(timePlot)
            }
            RibbonButton {
                glyph: "overlay"
                text: qsTr("Superposées")
                checked: !curvePlot.stacked
                enabled: root.hasTimeCurves
                tip: qsTr("Les courbes du temps sur un même tracé (une échelle par unité, à gauche et à droite), ou une bande chacune")
                onClicked: curvePlot.stacked = !curvePlot.stacked
            }
            RibbonButton {
                glyph: "keep"
                text: qsTr("Garder")
                enabled: root.hasTimeCurves && curvePlot.hasData
                tip: qsTr("Les courbes du temps affichées deviennent un signal de la session (pour l'exporter ou le réutiliser ; Ctrl+Z l'enlève)")
                onClicked: {
                    const id = root.curves.keepAsSignal()
                    if (id > 0)
                        root.infoMessage(qsTr("Les courbes sont dans la liste des signaux."))
                }
            }
            RibbonButton {
                glyph: "clear"
                text: qsTr("Tout retirer")
                enabled: root.hasSpectra || root.hasTimeCurves || root.hasSpectrograms
                tip: qsTr("Retire toutes les courbes ajoutées (la croix d'une étiquette n'en retire qu'une)")
                onClicked: root.curves.clear()
            }
            RibbonButton {
                id: exportButton
                glyph: "export"
                text: qsTr("Exporter")
                dropDown: true
                enabled: root.hasTimeCurves || (root.hasSpectra && root.analysis.hasResult)
                tip: qsTr("Les courbes du temps ou les spectres en CSV, pour un tableur (virgule décimale) ; le signal lui-même : Exporter… en haut")
                onClicked: exportMenu.popup(exportButton, 0, exportButton.height)
                FitMenu {
                    id: exportMenu
                    MenuItem {
                        text: qsTr("Courbes du temps (CSV)…")
                        enabled: root.hasTimeCurves && curvePlot.hasData
                        onTriggered: root.timeCurvesExportRequested()
                    }
                    MenuItem {
                        text: qsTr("Spectres (CSV)…")
                        enabled: root.hasSpectra && root.analysis.hasResult
                        onTriggered: root.spectrumExportRequested()
                    }
                }
            }
        }
        RibbonGroup {
            visible: root.ribbonTab === 1
            title: qsTr("Zone analysée")
            RibbonButton {
                glyph: "zoneall"
                text: qsTr("Tout")
                checked: root.zoneMode === "all"
                tip: qsTr("Spectres et mesures du panneau sur tout le signal, quel que soit le zoom")
                onClicked: root.setZoneMode("all")
            }
            RibbonButton {
                glyph: "zoneview"
                text: qsTr("Visible")
                checked: root.zoneMode === "view"
                tip: qsTr("La partie du signal visible : zoomer ou se déplacer change les spectres et les mesures")
                onClicked: root.setZoneMode("view")
            }
            RibbonButton {
                glyph: "zonecursors"
                text: qsTr("Entre A et B")
                checked: root.zoneMode === "cursors"
                enabled: root.hasSignal
                tip: qsTr("Entre les curseurs A et B du signal (affichés au besoin) : la vue reste libre")
                onClicked: root.setZoneMode("cursors")
            }
        }
        RibbonGroup {
            visible: root.ribbonTab === 1
            title: qsTr("Spectres")
            RibbonButton {
                glyph: "spectrum"
                text: qsTr("Amplitude")
                checked: !root.density
                enabled: root.hasSpectra
                tip: qsTr("Une FFT de toute la zone : l'amplitude de chaque sinusoïde, et les mesures de tonalité (THD, SINAD…)")
                onClicked: root.set({ kind: "amplitude" })
            }
            RibbonButton {
                glyph: "density"
                text: qsTr("Densité")
                checked: root.density
                enabled: root.hasSpectra
                tip: qsTr("Méthode de Welch : moyenne de segments qui se recouvrent, le niveau du bruit par √Hz (courbe plus lisse)")
                onClicked: root.set({ kind: "density" })
            }
            RibbonButton {
                glyph: "decibel"
                text: qsTr("Décibels")
                checked: root.decibel
                enabled: root.hasSpectra
                tip: qsTr("20 log₁₀ de l'amplitude : 0 dBV pour 1 V crête ; un rapport de 10 fait 20 dB")
                onClicked: root.set({ scale: "db" })
            }
            RibbonButton {
                glyph: "linear"
                text: qsTr("Linéaire")
                checked: !root.decibel
                enabled: root.hasSpectra
                tip: qsTr("L'amplitude dans l'unité de la voie (densité : unité/√Hz)")
                onClicked: root.set({ scale: "linear" })
            }
            RibbonButton {
                glyph: "logscale"
                text: qsTr("Log")
                checked: root.settings.logFrequency === true
                enabled: root.hasSpectra && !root.analysis.twoSided
                tip: root.analysis.twoSided ? qsTr("Pas d'échelle logarithmique pour une paire I/Q (fréquences négatives)")
                                            : qsTr("Échelle logarithmique des fréquences : chaque décade a la même largeur")
                onClicked: root.set({ logFrequency: !(root.settings.logFrequency === true) })
            }
            RibbonButton {
                glyph: "harmonics"
                text: qsTr("Harmoniques")
                checked: spectrum.showHarmonics
                enabled: root.hasSpectra
                tip: qsTr("Repère H2, H3… les harmoniques trouvées par la mesure de tonalité")
                onClicked: spectrum.showHarmonics = !spectrum.showHarmonics
            }
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

        // The signal: alone when no curve is added.
        Item {
            SplitView.fillHeight: true
            SplitView.minimumHeight: 110
            SignalPlot {
                id: timePlot
                anchors.fill: parent
                anchors.bottomMargin: timeBar.height
                session: root.session
                signalId: root.signalId
                // One band per channel, each at its scale: °C, hPa and % side by side stay readable.
                stacked: true
                // The same time axis as the graphs under it.
                rightAxisRoom: root.timeGraphsUnder
                backgroundColor: Theme.canvas
                gridColor: Theme.grid
                textColor: Theme.text
                mutedColor: Theme.textMuted
                accentColor: Theme.accent
                panelColor: Theme.panel
                channelColors: Theme.channelColors
                emptyText: root.session.signalList.count === 0
                           ? qsTr("La session est vide : importez un fichier CSV ou WAV (Ctrl+I), ou passez au module Acquisition.")
                           : qsTr("Choisissez un signal dans la liste de gauche.")
                onContextMenuRequested: (x, y) => signalMenu.openAt(timePlot, x, y)
                onScaleMenuRequested: (x, y, scale) => scaleMenu.openFor(timePlot, x, y, scale)
                // The plot under the pointer last is the one the buttons of the view act on.
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
                anchors.rightMargin: root.timeGraphsUnder ? 62 : 14
                anchors.bottomMargin: 2
                height: implicitHeight
            }
            CursorReadout {
                view: timePlot
            }
            // Top right: what is shown (and the chips of the channels, the help), then the zone analysed.
            ColumnLayout {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: 8
                anchors.rightMargin: root.timeGraphsUnder ? 66 : 18
                spacing: 6
                PlotToolbar {
                    Layout.alignment: Qt.AlignRight
                    view: timePlot
                }
                Rectangle {
                    Layout.alignment: Qt.AlignRight
                    // Said when it matters: a zone that is not the whole signal (otherwise in the "?" of the
                    // spectra and at the top of the panel Mesures).
                    visible: root.hasSignal && root.analysis.zoneText.length > 0 && root.zoneMode !== "all"
                    implicitWidth: zoneLabel.implicitWidth + 16
                    implicitHeight: zoneLabel.implicitHeight + 8
                    radius: 6
                    color: Qt.alpha(Theme.panel, 0.92)
                    border.color: Theme.border
                    Label {
                        id: zoneLabel
                        anchors.centerIn: parent
                        text: (root.zoneMode === "all" ? qsTr("Zone analysée : tout le signal, %1")
                               : root.zoneOnCursors ? qsTr("Zone analysée : entre A et B, %1")
                               : qsTr("Zone analysée : %1")).arg(root.analysis.zoneText)
                        color: Theme.textMuted
                        font.pixelSize: Theme.smallText
                    }
                    ToolTip.visible: zoneHover.hovered
                    ToolTip.delay: 400
                    ToolTip.text: root.zoneMode === "all"
                                  ? qsTr("Les spectres et les mesures portent sur tout le signal, quel que soit le zoom (Courbes ▸ Zone analysée).")
                                  : root.zoneMode === "cursors"
                                    ? qsTr("Les spectres et les mesures portent sur ce qui est entre les curseurs A et B : déplacez-les pour changer de zone.")
                                    : qsTr("Les spectres et les mesures portent sur la partie du signal visible ici : zoomez pour en analyser un morceau.")
                    HoverHandler { id: zoneHover }
                }
            }
            // In the tab "Courbes", with no curve yet: how to add one.
            Rectangle {
                visible: root.ribbonTab === 1 && root.hasSignal && !root.hasSpectra && !root.hasTimeCurves && !root.hasSpectrograms
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 56
                width: hint.implicitWidth + 24
                height: hint.implicitHeight + 12
                radius: 6
                color: Qt.alpha(Theme.panel, 0.94)
                border.color: Theme.border
                Label {
                    id: hint
                    anchors.centerIn: parent
                    text: qsTr("Pour un spectre, un spectrogramme, une intégrale ou d'autres courbes sous le signal : Ajouter… (ou clic droit sur le tracé)")
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
            }
        }

        // The curves of time, on the time axis of the signal.
        Item {
            visible: root.hasTimeCurves
            SplitView.preferredHeight: Math.round(root.height * 0.36)
            SplitView.minimumHeight: 110
            SignalPlot {
                id: curvePlot
                anchors.fill: parent
                curves: root.curves
                stacked: false
                rightAxisRoom: true
                backgroundColor: Theme.canvas
                gridColor: Theme.grid
                textColor: Theme.text
                mutedColor: Theme.textMuted
                accentColor: Theme.accent
                panelColor: Theme.panel
                channelColors: Theme.channelColors
                emptyText: root.curves.busy ? qsTr("Calcul des courbes…") : qsTr("Pas de courbe à afficher pour ce signal.")
                onContextMenuRequested: (x, y) => curveMenu.openAt(curvePlot, x, y)
                onScaleMenuRequested: (x, y, scale) => scaleMenu.openFor(curvePlot, x, y, scale)
                // The plot under the pointer last is the one the buttons of the view act on.
                HoverHandler {
                    onHoveredChanged: if (hovered) root.lastZoomed = curvePlot
                }
            }
            CursorReadout {
                view: curvePlot
                // Its cross takes away the cursors of the signal, and so of every graph of the time axis.
                closes: false
                onCloseRequested: timePlot.cursorsVisible = false
            }
            CurveChips {
                curves: root.curves
                graph: "time"
                title: qsTr("Courbes du temps")
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: 8
                anchors.rightMargin: 66
            }
        }

        // The spectrograms, on the time axis of the signal: one band per channel, the level in colours.
        Item {
            visible: root.hasSpectrograms
            SplitView.preferredHeight: Math.round(root.height * 0.36)
            SplitView.minimumHeight: 130
            SpectrogramPlot {
                id: spectrogramPlot
                anchors.fill: parent
                anchors.bottomMargin: spectrogramBar.height
                curves: root.curves
                backgroundColor: Theme.canvas
                gridColor: Theme.grid
                textColor: Theme.text
                mutedColor: Theme.textMuted
                accentColor: Theme.accent
                panelColor: Theme.panel
                channelColors: Theme.channelColors
                busyText: root.curves.spectrogramBusy
                          ? qsTr("Calcul du spectrogramme : %1 %").arg(Math.round(100 * root.curves.spectrogramProgress)) : ""
                emptyText: root.curves.spectrogramBusy ? qsTr("Calcul du spectrogramme…")
                                                       : qsTr("Pas de spectrogramme à afficher pour ce signal.")
                onContextMenuRequested: (x, y) => spectrogramMenu.openAt(spectrogramPlot, x, y)
                onScaleMenuRequested: (x, y, scale) => scaleMenu.openFor(spectrogramPlot, x, y, scale)
                // The plot under the pointer last is the one the buttons of the view act on.
                HoverHandler {
                    onHoveredChanged: if (hovered) root.lastZoomed = spectrogramPlot
                }
            }
            ViewScrollBar {
                id: spectrogramBar
                view: spectrogramPlot
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.leftMargin: 62
                anchors.rightMargin: 62
                anchors.bottomMargin: 2
                height: implicitHeight
            }
            // Top right: the spectrograms (a click hides one, its cross takes it away), "?" for how they
            // are computed, and while they are: how far it is.
            ColumnLayout {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: 22
                anchors.rightMargin: 66
                spacing: 6
                CurveChips {
                    Layout.alignment: Qt.AlignRight
                    curves: root.curves
                    graph: "spectrogram"
                    title: qsTr("Spectrogrammes")
                    info: (spectrogramPlot.summary.length > 0 ? spectrogramPlot.summary + "\n" : "")
                          + qsTr("Longueur des segments et recouvrement : panneau de droite, Mesures ▸ Réglages des calculs.\n"
                                 + "Des segments longs séparent mieux les fréquences, des segments courts suivent mieux\n"
                                 + "les changements rapides. Molette sur l'échelle des couleurs : l'étendue des niveaux ;\n"
                                 + "clic droit sur une échelle : ses bornes (minimum, maximum).")
                }
                Rectangle {
                    id: spectrogramBox
                    readonly property bool busy: root.curves.spectrogramBusy
                    Layout.alignment: Qt.AlignRight
                    // Only while they are computed: the progress, well in sight.
                    visible: busy
                    Layout.preferredWidth: Math.min(spectrogramInfo.implicitWidth + 16, spectrogramPlot.width * 0.6)
                    Layout.preferredHeight: spectrogramInfo.implicitHeight + 8
                    radius: 6
                    color: Qt.alpha(Theme.panel, 0.92)
                    border.color: Theme.accent
                    ColumnLayout {
                        id: spectrogramInfo
                        anchors.fill: parent
                        anchors.margins: 4
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 3
                        RowLayout {
                            spacing: 6
                            BusyIndicator {
                                running: spectrogramBox.busy
                                Layout.preferredWidth: 16
                                Layout.preferredHeight: 16
                            }
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Calcul du spectrogramme : %1 %").arg(Math.round(100 * root.curves.spectrogramProgress))
                                color: Theme.text
                                font.pixelSize: Theme.smallText
                                elide: Text.ElideRight
                            }
                        }
                        ProgressBar {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 6
                            from: 0
                            to: 1
                            value: root.curves.spectrogramProgress
                        }
                    }
                }
            }
        }

        // The spectra.
        Item {
            visible: root.hasSpectra
            SplitView.preferredHeight: Math.round(root.height * 0.4)
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
                           : root.hasSignal ? qsTr("Calcul du spectre…") : qsTr("Pas de signal choisi.")
                onContextMenuRequested: (x, y) => spectrumMenu.openAt(spectrum, x, y)
                onScaleMenuRequested: (x, y, scale) => scaleMenu.openFor(spectrum, x, y, scale)
                // The plot under the pointer last is the one the buttons of the view act on.
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
            ColumnLayout {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: 8
                anchors.rightMargin: 18
                spacing: 6
                CurveChips {
                    Layout.alignment: Qt.AlignRight
                    curves: root.curves
                    graph: "frequency"
                    title: qsTr("Spectres")
                    busy: root.analysis.busy
                    // How they are computed, what they cover (also at the top of the panel Mesures).
                    info: {
                        const lines = []
                        if (root.analysis.summary.length > 0)
                            lines.push(root.analysis.summary)
                        const notes = root.analysis.notes
                        for (let i = 0; i < notes.length; ++i)
                            lines.push(notes[i])
                        if (root.analysis.zoneText.length > 0)
                            lines.push((root.zoneMode === "all" ? qsTr("Zone analysée : tout le signal, %1")
                                        : root.zoneOnCursors ? qsTr("Zone analysée : entre A et B, %1")
                                        : qsTr("Zone analysée : la partie visible, %1")).arg(root.analysis.zoneText))
                        return lines.join("\n")
                    }
                }
            }
        }
    }

    // Right click on a scale (or the time axis) of any plot: its ends, automatic, and for the spectra
    // the logarithmic frequencies and the decibels.
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

    // Right click on a plot: what it shows, superposed or not, a few actions.
    PlotMenu {
        id: signalMenu
        view: timePlot
        mode: "channels"
        actions: [{ key: "add", text: qsTr("Ajouter une courbe…") }]
        onActionTriggered: (key) => { if (key === "add") root.curveRequested(timePlot) }
    }
    PlotMenu {
        id: curveMenu
        view: curvePlot
        curves: root.curves
        mode: "time"
        actions: [{ key: "add", text: qsTr("Ajouter une courbe…") },
                  { key: "keep", text: qsTr("Garder comme signal") },
                  { key: "export", text: qsTr("Exporter en CSV…") },
                  { key: "removeHidden", text: qsTr("Retirer les courbes masquées") }]
        onActionTriggered: (key) => {
            if (key === "add")
                root.curveRequested(timePlot)
            else if (key === "export")
                root.timeCurvesExportRequested()
            else if (key === "keep") {
                if (root.curves.keepAsSignal() > 0)
                    root.infoMessage(qsTr("Les courbes sont dans la liste des signaux."))
            } else if (key === "removeHidden") {
                for (const c of root.curves.curves)
                    if (c.graph === "time" && !c.visible)
                        root.curves.remove(c.id)
            }
        }
    }
    PlotMenu {
        id: spectrogramMenu
        view: spectrogramPlot
        curves: root.curves
        mode: "spectrogram"
        actions: [{ key: "add", text: qsTr("Ajouter une courbe…") }]
        onActionTriggered: (key) => { if (key === "add") root.curveRequested(timePlot) }
        // How they are computed, beside the list (the same controls as in the panel Mesures).
        settingsTitle: qsTr("Calcul des spectrogrammes")
        settings: Component {
            SpectrogramSettings {
                analysis: root.analysis
                sampleRate: spectrogramPlot.sampleRate
                compact: true
            }
        }
    }
    PlotMenu {
        id: spectrumMenu
        view: spectrum
        curves: root.curves
        mode: "frequency"
        actions: [{ key: "add", text: qsTr("Ajouter une courbe…") }, { key: "export", text: qsTr("Exporter en CSV…") }]
        // How they are computed and on what, beside the list (the same controls as the panel Mesures and
        // the group Zone analysée).
        settingsTitle: qsTr("Calcul des spectres")
        settings: Component {
            ColumnLayout {
                spacing: 6
                ChoiceField {
                    label: qsTr("Zone analysée")
                    choices: [qsTr("Tout le signal"), qsTr("La partie visible"), qsTr("Entre les curseurs A et B")]
                    value: root.zoneMode === "view" ? 1 : root.zoneMode === "cursors" ? 2 : 0
                    onEdited: (index) => root.setZoneMode(["all", "view", "cursors"][index])
                }
                SpectrumSettings {
                    analysis: root.analysis
                    compact: true
                }
            }
        }
        onActionTriggered: (key) => {
            if (key === "add")
                root.curveRequested(timePlot)
            else if (key === "export")
                root.spectrumExportRequested()
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
        toolTip: qsTr("Afficher les mesures et le signal (F10)")
        onClicked: root.unfoldRight()
    }
}
