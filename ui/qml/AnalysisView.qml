import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Module "Analyse": the signal chosen above; under it, only the curves added (group "Courbes"): a graph of
// time (the channels themselves, integral, derivative, values per period) that follows the time axis of
// the signal, and a graph of the frequencies (spectra). With no curve, the signal takes the whole height.
// The settings of the computations and the measurements of each channel are in the right panel.
Item {
    id: root
    required property var session
    required property var analysis
    required property var curves
    property var signalId: 0
    property Item ribbonHost: null
    property bool leftFolded: false
    property bool rightFolded: false
    signal unfoldLeft()
    signal unfoldRight()
    signal exportRequested()
    signal settingsRequested()
    signal curveRequested(var view)
    signal infoMessage(string text)
    signal errorMessage(string text)
    // The plot pointed at, zoomed or slid last: "Précédente" and the zoom buttons act on it.
    property Item lastZoomed: null
    readonly property Item activePlot: lastZoomed && lastZoomed.visible ? lastZoomed : timePlot

    // The count is read so that this follows the list (a signal deleted, an undo).
    readonly property bool hasSignal: (session.signalList.count, signalId > 0 && session.signalList.rowOf(signalId) >= 0)
    readonly property var settings: analysis.settings
    readonly property bool density: settings.kind === "density"
    readonly property bool decibel: settings.scale !== "linear"
    readonly property bool hasSpectra: curves.hasSpectra
    readonly property bool hasTimeCurves: curves.hasTimeCurves

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
        spectrum.cursorsVisible = on
        if (on && curvePlot.hasData) {
            curvePlot.cursorA = timePlot.cursorA
            curvePlot.cursorB = timePlot.cursorB
        }
    }
    function zoom(factor) {
        if (activePlot === spectrum)
            spectrum.zoomFrequency(factor)
        else
            timePlot.zoomTime(factor)
    }

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
    // The plot of the signal leads; the graph of time follows its view and its cursors, and the other way.
    Connections {
        target: timePlot
        function onViewChanged() {
            if (root.zoneMode === "view")
                Qt.callLater(root.pushZone)
            if (timePlot.canGoBack)
                root.lastZoomed = timePlot
            if (curvePlot.hasData)
                curvePlot.setTimeView(timePlot.viewLow, timePlot.viewHigh)
        }
        function onCursorsChanged() {
            if (root.zoneMode === "cursors")
                Qt.callLater(root.pushZone)
            if (curvePlot.cursorsVisible) {
                curvePlot.cursorA = timePlot.cursorA
                curvePlot.cursorB = timePlot.cursorB
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
        target: spectrum
        function onViewChanged() { if (spectrum.canGoBack) root.lastZoomed = spectrum }
    }
    onVisibleChanged: Qt.callLater(root.pushZone)
    onSignalIdChanged: Qt.callLater(root.pushZone)

    Ribbon {
        parent: root.ribbonHost ? root.ribbonHost : root
        visible: root.visible
        tabs: [qsTr("Analyser")]
        RibbonGroup {
            title: qsTr("Courbes")
            RibbonButton {
                glyph: "addcurve"
                text: qsTr("Ajouter…")
                primary: !root.hasSpectra && !root.hasTimeCurves
                enabled: root.hasSignal
                tip: qsTr("Un spectre, ou une courbe du temps (voie d'origine, intégrale, dérivée, valeurs par période) sous le signal")
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
                enabled: root.hasSpectra || root.hasTimeCurves
                tip: qsTr("Retire toutes les courbes ajoutées (la croix d'une étiquette n'en retire qu'une)")
                onClicked: root.curves.clear()
            }
        }
        RibbonGroup {
            title: qsTr("Voies du signal")
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
            title: qsTr("Zone analysée")
            RibbonButton {
                glyph: "zoneall"
                text: qsTr("Tout")
                checked: root.zoneMode === "all"
                tip: qsTr("Spectres et mesures sur tout le signal, quel que soit le zoom")
                onClicked: root.setZoneMode("all")
            }
            RibbonButton {
                glyph: "zoneview"
                text: qsTr("Visible")
                checked: root.zoneMode === "view"
                tip: qsTr("La partie du signal visible au-dessus : zoomer ou se déplacer change le spectre")
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
            title: qsTr("Spectre")
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
        RibbonGroup {
            title: qsTr("Vue")
            RibbonButton {
                glyph: "fit"
                text: qsTr("Tout voir")
                enabled: root.hasSignal
                tip: qsTr("Tout le signal, toutes les fréquences (double clic sur un tracé : ce tracé seulement)")
                onClicked: {
                    timePlot.showAll()
                    spectrum.showAll()
                }
            }
            RibbonButton {
                glyph: "zoomin"
                text: qsTr("Zoom +")
                enabled: root.hasSignal
                tip: qsTr("Le dernier tracé cliqué, deux fois plus resserré (molette sur un tracé : autour du pointeur)")
                onClicked: root.zoom(0.5)
            }
            RibbonButton {
                glyph: "zoomout"
                text: qsTr("Zoom −")
                enabled: root.activePlot.zoomed
                onClicked: root.zoom(2)
            }
            RibbonButton {
                readonly property Item backTarget: root.lastZoomed && root.lastZoomed.canGoBack ? root.lastZoomed
                                                   : spectrum.canGoBack ? spectrum : timePlot
                glyph: "back"
                text: qsTr("Précédente")
                enabled: spectrum.canGoBack || timePlot.canGoBack || curvePlot.canGoBack
                tip: qsTr("Revenir à la vue d'avant du dernier tracé zoomé")
                onClicked: backTarget.goBack()
            }
        }
        RibbonGroup {
            title: qsTr("Mesurer")
            RibbonButton {
                glyph: "cursors"
                text: qsTr("Curseurs")
                checked: timePlot.cursorsVisible
                enabled: root.hasSignal
                tip: qsTr("Curseurs A et B sur le signal et les courbes du temps (Δt, valeurs, intégrale entre A et B) et sur le spectre (Δf, écart de niveau, valeur efficace de la bande)")
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
        ImageActions {
            target: split
            baseName: "analyse"
            onDone: (m) => root.infoMessage(m)
            onFailed: (m) => root.errorMessage(m)
        }
        RibbonGroup {
            title: qsTr("Fichier")
            RibbonButton {
                glyph: "export"
                text: qsTr("Exporter")
                enabled: root.analysis.hasResult
                tip: qsTr("Le spectre en CSV (fréquence, niveau en dB et en valeur), pour un tableur")
                onClicked: root.exportRequested()
            }
            RibbonButton {
                glyph: "preset"
                text: qsTr("Réglages")
                tip: qsTr("Fenêtre, longueur des segments, harmoniques… et les mesures, dans le panneau de droite")
                onClicked: root.settingsRequested()
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
                // The same time axis as the graph of the curves under it.
                rightAxisRoom: root.hasTimeCurves
                backgroundColor: Theme.canvas
                gridColor: Theme.grid
                textColor: Theme.text
                mutedColor: Theme.textMuted
                accentColor: Theme.accent
                panelColor: Theme.panel
                channelColors: Theme.channelColors
                emptyText: root.session.signalList.count === 0
                           ? qsTr("La session est vide : importez un fichier (Ctrl+I) ou passez au module Acquisition.")
                           : qsTr("Choisissez un signal dans la liste de gauche.")
                onContextMenuRequested: (x, y) => signalMenu.openAt(timePlot, x, y)
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
                anchors.rightMargin: root.hasTimeCurves ? 62 : 14
                anchors.bottomMargin: 2
                height: implicitHeight
            }
            CursorReadout {
                view: timePlot
            }
            Rectangle {
                visible: root.hasSignal && root.analysis.zoneText.length > 0
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: 8
                anchors.rightMargin: root.hasTimeCurves ? 66 : 18
                width: zoneLabel.implicitWidth + 16
                height: zoneLabel.implicitHeight + 8
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
                              ? qsTr("Les spectres et les mesures portent sur tout le signal, quel que soit le zoom (groupe « Zone analysée »).")
                              : root.zoneMode === "cursors"
                                ? qsTr("Les spectres et les mesures portent sur ce qui est entre les curseurs A et B : déplacez-les pour changer de zone.")
                                : qsTr("Les spectres et les mesures portent sur la partie du signal visible ici : zoomez pour en analyser un morceau.")
                HoverHandler { id: zoneHover }
            }
            // With no curve: how to add one.
            Rectangle {
                visible: root.hasSignal && !root.hasSpectra && !root.hasTimeCurves
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
                    text: qsTr("Pour un spectre, une intégrale ou d'autres courbes sous le signal : Courbes ▸ Ajouter…")
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
                // The plot under the pointer last is the one the buttons of the view act on.
                HoverHandler {
                    onHoveredChanged: if (hovered) root.lastZoomed = curvePlot
                }
            }
            CursorReadout {
                view: curvePlot
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
                }
                Rectangle {
                    Layout.alignment: Qt.AlignRight
                    visible: root.analysis.summary.length > 0 || root.analysis.busy
                    Layout.preferredWidth: Math.min(summaryColumn.implicitWidth + 16, spectrum.width * 0.6)
                    Layout.preferredHeight: summaryColumn.implicitHeight + 8
                    radius: 6
                    color: Qt.alpha(Theme.panel, 0.92)
                    border.color: Theme.border
                    ColumnLayout {
                        id: summaryColumn
                        anchors.fill: parent
                        anchors.margins: 4
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 1
                        RowLayout {
                            spacing: 6
                            BusyIndicator {
                                visible: root.analysis.busy
                                running: visible
                                Layout.preferredWidth: 16
                                Layout.preferredHeight: 16
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.analysis.busy && root.analysis.summary.length === 0 ? qsTr("Calcul…") : root.analysis.summary
                                color: Theme.textMuted
                                font.pixelSize: Theme.smallText
                                elide: Text.ElideRight
                            }
                        }
                        Repeater {
                            model: root.analysis.notes
                            delegate: Label {
                                required property string modelData
                                Layout.fillWidth: true
                                text: modelData
                                color: Theme.textMuted
                                font.pixelSize: Theme.smallText - 1
                                font.italic: true
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }
        }
    }

    // Right click on a plot: what it shows, superposed or not, a few actions.
    PlotMenu {
        id: signalMenu
        view: timePlot
        mode: "channels"
    }
    PlotMenu {
        id: curveMenu
        view: curvePlot
        curves: root.curves
        mode: "time"
        actions: [{ key: "add", text: qsTr("Ajouter une courbe…") },
                  { key: "keep", text: qsTr("Garder comme signal") },
                  { key: "removeHidden", text: qsTr("Retirer les courbes masquées") }]
        onActionTriggered: (key) => {
            if (key === "add")
                root.curveRequested(timePlot)
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
        id: spectrumMenu
        view: spectrum
        curves: root.curves
        mode: "frequency"
        actions: [{ key: "add", text: qsTr("Ajouter une courbe…") }]
        onActionTriggered: (key) => { if (key === "add") root.curveRequested(timePlot) }
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
        toolTip: qsTr("Afficher les réglages et les mesures (F10)")
        onClicked: root.unfoldRight()
    }
}
