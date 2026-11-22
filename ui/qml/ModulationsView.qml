import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// Module "Modulations": a digital link simulated end to end. Bits (at random, a text, bits typed) → a modulator (ASK,
// FSK, BPSK, QPSK, 8-PSK, 16-, 64- or 256-QAM; rectangular or root-raised-cosine pulses; on a carrier or as I/Q) →
// white noise at Eb/N0 → a coherent receiver (matched filter, nearest point) → the bits again. Tab "Liaison": the
// signals sent and received and their spectra, the constellation received (the points decided wrong in red), the
// eye diagram, the bit error rate measured against the exact theory; heard on the sound card; sent or received into
// the session. Tab "Courbe TEB": the bit error rate along Eb/N0, measured point by point on the same chain, several
// curves to compare. The recipe is remembered from one run to the next; its detail is in the right panel (Liaison).
Item {
    id: root
    required property var modulations
    required property var player
    required property var session
    required property var appSettings
    property Item ribbonHost: null
    // The height its header needs (one row of groups, or more in a narrow window): the host takes it.
    readonly property real ribbonHeight: moduleHeader.implicitHeight
    property bool leftFolded: false
    property bool rightFolded: false
    signal unfoldLeft()
    signal unfoldRight()
    signal detailRequested()
    signal signalAdded(var id)
    signal infoMessage(string text)
    signal errorMessage(string text)

    property int ribbonTab: 0
    readonly property var recipe: modulations.recipe
    readonly property var result: modulations.result
    readonly property bool linkOk: result.ok === true
    readonly property bool fsk: recipe.modulation === "fsk"
    readonly property bool iq: recipe.representation === "iq"
    readonly property bool rrc: !fsk && recipe.pulse === "rrc"
    readonly property var sweep: modulations.sweep
    readonly property var analysis: modulations.previewAnalysis
    readonly property var settings: analysis.settings
    readonly property bool decibel: settings.scale !== "linear"
    readonly property real fs: recipe.sampleRate ?? 48000
    // The channels of the preview, for the listening and the menus.
    readonly property var previewChannels: iq ? [{ name: qsTr("I émis") }, { name: qsTr("Q émis") },
                                                 { name: qsTr("I reçu") }, { name: qsTr("Q reçu") }]
                                              : [{ name: qsTr("Émis") }, { name: qsTr("Reçu") }]
    // The plot pointed at last, in the tab "Liaison": the buttons of the view act on it.
    property Item lastZoomed: null
    readonly property Item linkPlot: lastZoomed && lastZoomed.visible ? lastZoomed : timePlot

    function nameOf(list, key) {
        const item = list.find((x) => x.key === key)
        return item ? item.name : ""
    }
    function set(values) {
        modulations.setRecipe(values)
    }

    // Computed only while the module is on screen; so is the analysis of the preview.
    Binding {
        target: root.modulations
        property: "active"
        value: root.visible
    }
    Binding {
        target: root.analysis
        property: "signalId"
        value: root.visible && root.ribbonTab === 0 ? root.modulations.previewId : 0
    }

    // The recipe and the sweep of the last run come back; each change is remembered a moment after.
    Component.onCompleted: {
        const saved = appSettings.value("modulations/recipe", "")
        if (saved && saved.length > 0)
            modulations.restoreRecipe(saved)
        const sweepSaved = appSettings.value("modulations/sweep", "")
        if (sweepSaved && sweepSaved.length > 0)
            modulations.restoreSweep(sweepSaved)
    }
    Timer {
        id: rememberTimer
        interval: 800
        onTriggered: root.appSettings.setValue("modulations/recipe", root.modulations.recipeText())
    }
    Timer {
        id: rememberSweepTimer
        interval: 800
        onTriggered: root.appSettings.setValue("modulations/sweep", root.modulations.sweepText())
    }

    // While the preview plays, each new preview plays in its place: what is changed is heard.
    readonly property bool previewReady: root.modulations.ready
    onPreviewReadyChanged: if (previewReady) listenGroup.relisten()

    // Another kind of link shown (modulation, carrier or I/Q, rate): the time plot shows its first symbols, the
    // spectrum all of it.
    property string shownLink: ""
    property bool fitSpectrum: false
    Connections {
        target: root.modulations
        function onRecipeChanged() { rememberTimer.restart() }
        function onSweepChanged() { rememberSweepTimer.restart() }
        function onResultChanged() {
            if (!root.linkOk)
                return
            const shown = [root.recipe.modulation, root.recipe.representation, root.recipe.symbolRate, root.fs].join("|")
            if (shown === root.shownLink)
                return
            root.shownLink = shown
            timePlot.setTimeView(0, Math.min(24 / Math.max(1e-9, root.recipe.symbolRate ?? 1000),
                                             (root.result.symbols ?? 0) / Math.max(1e-9, root.recipe.symbolRate ?? 1000)))
            root.fitSpectrum = true
        }
        function onAdded(id) {
            root.signalAdded(id)
            root.infoMessage(qsTr("« %1 » est dans la session, dans la liste de gauche (Ctrl+Z l'enlève).")
                             .arg(root.session.signalList.info(id).name ?? ""))
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
    Connections {
        target: constellationPlot
        function onViewChanged() { if (constellationPlot.canGoBack) root.lastZoomed = constellationPlot }
    }
    Connections {
        target: eyePlot
        function onViewChanged() { if (eyePlot.canGoBack) root.lastZoomed = eyePlot }
    }
    // The wheel while computing: shown only if it lasts.
    property bool showBusy: false
    Timer {
        id: busyDelay
        interval: 300
        onTriggered: root.showBusy = root.modulations.busy
    }
    Connections {
        target: root.modulations
        function onBusyChanged() {
            if (root.modulations.busy)
                busyDelay.restart()
            else
                root.showBusy = false
        }
    }

    // A button of the header that opens a list of choices, the current one checked.
    component ChoiceButton: RibbonButton {
        id: choice
        property var options: []
        property string current: ""
        signal chosen(string key)
        dropDown: true
        onClicked: choiceMenu.popup(choice, 0, choice.height)
        FitMenu {
            id: choiceMenu
            Instantiator {
                model: choice.options
                delegate: MenuItem {
                    required property var modelData
                    text: modelData.name
                    checkable: true
                    checked: modelData.key === choice.current
                    onTriggered: {
                        choice.chosen(modelData.key)
                        checked = Qt.binding(() => modelData.key === choice.current)
                    }
                    ToolTip.visible: hovered && (modelData.tip ?? "").length > 0
                    ToolTip.delay: 400
                    ToolTip.text: modelData.tip ?? ""
                }
                onObjectAdded: (index, object) => choiceMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => choiceMenu.removeItem(object)
            }
        }
    }

    Ribbon {
        id: moduleHeader
        parent: root.ribbonHost ? root.ribbonHost : root
        visible: root.visible
        tabs: [qsTr("Liaison"), qsTr("Courbe TEB")]
        tabIcons: ["modulation", "ber"]
        tabTips: [qsTr("La liaison : l'émetteur, le bruit, le récepteur ; les signaux et leurs spectres, la constellation reçue, le diagramme de l'œil, le TEB mesuré"),
                  qsTr("Le taux d'erreur binaire le long d'Eb/N0, mesuré point par point sur la même chaîne, contre la théorie ; plusieurs courbes se comparent")]
        currentTab: root.ribbonTab
        onTabClicked: (index) => root.ribbonTab = index

        // ---- In both tabs: the signals into the session ----
        RibbonGroup {
            title: qsTr("Session")
            RibbonButton {
                id: addButton
                glyph: "keep"
                text: qsTr("Ajouter")
                dropDown: true
                primary: root.linkOk
                enabled: root.linkOk
                tip: root.linkOk ? qsTr("Le signal émis, ou reçu avec son bruit, devient un signal de la session (écoute, filtres, mesures ; Ctrl+Z l'enlève)")
                                 : (root.result.error ?? "")
                onClicked: addMenu.popup(addButton, 0, addButton.height)
                FitMenu {
                    id: addMenu
                    MenuItem {
                        text: qsTr("Le signal émis")
                        onTriggered: root.modulations.addToSession(false)
                    }
                    MenuItem {
                        text: root.recipe.noise === true ? qsTr("Le signal reçu (avec le bruit, Eb/N0 = %1 dB)").arg(Units.compact(root.recipe.ebN0 ?? 0, "", 3))
                                                         : qsTr("Le signal reçu (sans bruit)")
                        onTriggered: root.modulations.addToSession(true)
                    }
                }
            }
        }

        // ---- Liaison ----
        RibbonGroup {
            visible: root.ribbonTab === 0
            title: qsTr("Recette")
            RibbonButton {
                id: examplesButton
                glyph: "preset"
                text: qsTr("Exemples")
                dropDown: true
                tip: qsTr("Des liaisons toutes prêtes : BPSK, QPSK, 8-PSK, QAM, un texte en ASK, un modem FSK…")
                onClicked: examplesMenu.popup(examplesButton, 0, examplesButton.height)
                FitMenu {
                    id: examplesMenu
                    Instantiator {
                        model: root.modulations.examples
                        delegate: MenuItem {
                            required property var modelData
                            required property int index
                            text: modelData.name
                            onTriggered: root.modulations.useExample(index)
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
                tip: qsTr("Repartir d'une QPSK à 1 kBd en racine de cosinus surélevé, porteuse 6 kHz, Eb/N0 = 7 dB")
                onClicked: root.modulations.reset()
            }
            RibbonButton {
                glyph: "rename"
                text: qsTr("Détail")
                tip: qsTr("La liaison en détail, les données envoyées, le résultat : panneau Liaison (F10)")
                onClicked: root.detailRequested()
            }
        }
        RibbonGroup {
            visible: root.ribbonTab === 0
            title: qsTr("Émetteur")
            ChoiceButton {
                glyph: "constellation"
                text: (Choices.linkModulations.find((m) => m.key === root.recipe.modulation) ?? {}).short ?? "QPSK"
                options: Choices.linkModulations
                current: root.recipe.modulation ?? "qpsk"
                tip: (Choices.linkModulations.find((m) => m.key === root.recipe.modulation) ?? {}).tip ?? ""
                onChosen: (key) => root.set({ modulation: key })
            }
            ChoiceButton {
                glyph: "carrier"
                text: root.iq ? qsTr("I/Q") : qsTr("Porteuse")
                options: Choices.linkRepresentations
                current: root.recipe.representation ?? "passband"
                tip: root.nameOf(Choices.linkRepresentations, root.recipe.representation ?? "passband")
                onChosen: (key) => root.set({ representation: key })
            }
            RibbonField {
                anchors.verticalCenter: parent.verticalCenter
                label: qsTr("Débit")
                fieldWidth: 70
                unit: "Bd"
                minimum: 1e-3
                maximum: root.fs
                value: root.recipe.symbolRate ?? 1000
                tip: qsTr("Symboles par seconde ; débit binaire : %1").arg(Units.compact(root.result.bitRate ?? 0, "bit/s", 4))
                onEdited: (v) => root.set({ symbolRate: v })
            }
            RibbonField {
                visible: !root.iq
                anchors.verticalCenter: parent.verticalCenter
                label: root.fsk ? qsTr("fc") : qsTr("Porteuse")
                fieldWidth: 70
                unit: "Hz"
                minimum: 1e-3
                maximum: root.fs / 2
                value: root.recipe.carrier ?? 6000
                tip: root.fsk ? qsTr("Fréquence centrale : les tonalités à fc ± Δf") : qsTr("Fréquence de la porteuse")
                onEdited: (v) => root.set({ carrier: v })
            }
            RibbonField {
                visible: root.fsk
                anchors.verticalCenter: parent.verticalCenter
                label: qsTr("Δf")
                fieldWidth: 64
                unit: "Hz"
                minimum: 1e-3
                maximum: root.fs / 2
                value: root.recipe.fskDeviation ?? 500
                tip: qsTr("Excursion : les tonalités à fc ± Δf, orthogonales quand 2 Δf est un multiple du débit")
                onEdited: (v) => root.set({ fskDeviation: v })
            }
            ChoiceButton {
                visible: !root.fsk
                glyph: "pulse"
                text: root.rrc ? qsTr("Cosinus surélevé") : qsTr("Rectangulaire")
                options: Choices.linkPulses
                current: root.recipe.pulse ?? "rrc"
                tip: (Choices.linkPulses.find((p) => p.key === root.recipe.pulse) ?? {}).tip ?? ""
                onChosen: (key) => root.set({ pulse: key })
            }
            RibbonField {
                visible: root.rrc
                anchors.verticalCenter: parent.verticalCenter
                label: qsTr("α")
                fieldWidth: 50
                minimum: 0
                maximum: 1
                value: root.recipe.rolloff ?? 0.35
                tip: qsTr("Facteur de retombée : la bande occupée vaut (1 + α) Rs")
                onEdited: (v) => root.set({ rolloff: v })
            }
        }
        RibbonGroup {
            visible: root.ribbonTab === 0
            title: qsTr("Données")
            ChoiceButton {
                glyph: "bits"
                text: root.recipe.data === "text" ? qsTr("Texte") : root.recipe.data === "bits" ? qsTr("Bits tapés") : qsTr("Au hasard")
                options: Choices.linkData
                current: root.recipe.data ?? "random"
                tip: qsTr("%1 ; le texte, les bits, la graine : panneau Liaison").arg(root.nameOf(Choices.linkData, root.recipe.data ?? "random"))
                onChosen: (key) => {
                    root.set({ data: key })
                    if (key !== "random")
                        root.detailRequested()
                }
            }
        }
        RibbonGroup {
            visible: root.ribbonTab === 0
            title: qsTr("Canal")
            RibbonButton {
                glyph: "noise"
                text: qsTr("Bruit")
                checked: root.recipe.noise === true
                tip: root.recipe.noise === true ? qsTr("Bruit blanc gaussien à Eb/N0 donné (cliquer : sans bruit)")
                                                : qsTr("Sans bruit (cliquer : ajouter un bruit blanc gaussien)")
                onClicked: root.set({ noise: !(root.recipe.noise === true) })
            }
            RibbonField {
                enabled: root.recipe.noise === true
                anchors.verticalCenter: parent.verticalCenter
                label: qsTr("Eb/N0")
                fieldWidth: 58
                unit: "dB"
                minimum: -20
                maximum: 60
                value: root.recipe.ebN0 ?? 7
                tip: qsTr("Énergie d'un bit sur la densité du bruit ; mesuré : %1").arg(Units.compact(root.result.measuredEbN0 ?? 0, "dB", 4))
                onEdited: (v) => root.set({ ebN0: v })
            }
        }
        ListenGroup {
            id: listenGroup
            visible: root.ribbonTab === 0
            player: root.player
            session: root.modulations.preview
            signalId: root.modulations.hasPreview ? root.modulations.previewId : 0
            channels: root.previewChannels
            onInfoMessage: (text) => root.infoMessage(text)
            onErrorMessage: (text) => root.errorMessage(text)
        }
        ViewGroup {
            visible: root.ribbonTab === 0
            target: root.linkPlot
            plots: [timePlot, spectrum, constellationPlot, eyePlot]
            backTarget: root.lastZoomed && root.lastZoomed.canGoBack ? root.lastZoomed : timePlot
            fitTip: qsTr("Tous les tracés en entier, échelles automatiques (double clic sur un tracé : ce tracé seulement)")
        }
        RibbonGroup {
            visible: root.ribbonTab === 0
            title: qsTr("Spectre")
            RibbonButton {
                glyph: "decibel"
                text: qsTr("Décibels")
                checked: root.decibel
                tip: qsTr("20 log₁₀ de l'amplitude : le plancher du bruit se lit sous le signal")
                onClicked: root.analysis.setSettings({ scale: root.decibel ? "linear" : "db" })
            }
        }
        ImageActions {
            visible: root.ribbonTab === 0
            target: linkArea
            baseName: "liaison"
            onDone: (m) => root.infoMessage(m)
            onFailed: (m) => root.errorMessage(m)
        }

        // ---- Courbe TEB ----
        RibbonGroup {
            visible: root.ribbonTab === 1
            title: qsTr("Courbe")
            RibbonField {
                anchors.verticalCenter: parent.verticalCenter
                label: qsTr("Eb/N0 de")
                fieldWidth: 56
                unit: "dB"
                minimum: -20
                maximum: 60
                value: root.sweep.from ?? 0
                invalid: (root.modulations.sweepError ?? "").length > 0
                onEdited: (v) => root.modulations.setSweep({ from: v })
            }
            RibbonField {
                anchors.verticalCenter: parent.verticalCenter
                label: qsTr("à")
                fieldWidth: 56
                unit: "dB"
                minimum: -20
                maximum: 60
                value: root.sweep.to ?? 12
                invalid: (root.modulations.sweepError ?? "").length > 0
                onEdited: (v) => root.modulations.setSweep({ to: v })
            }
            RibbonField {
                anchors.verticalCenter: parent.verticalCenter
                label: qsTr("Pas")
                fieldWidth: 50
                unit: "dB"
                minimum: 0.01
                maximum: 20
                value: root.sweep.step ?? 1
                invalid: (root.modulations.sweepError ?? "").length > 0
                onEdited: (v) => root.modulations.setSweep({ step: v })
            }
            RibbonField {
                anchors.verticalCenter: parent.verticalCenter
                label: qsTr("Bits par point (au plus)")
                fieldWidth: 90
                minimum: 1000
                maximum: 100000000
                value: root.sweep.maxBits ?? 200000
                tip: qsTr("Chaque point compte de quoi trouver environ %1 erreurs d'après la théorie, sans dépasser ce nombre de bits : plus, des points plus bas et plus sûrs, mais plus longs")
                     .arg(root.sweep.targetErrors ?? 200)
                onEdited: (v) => root.modulations.setSweep({ maxBits: Math.round(v) })
            }
            RibbonButton {
                glyph: root.modulations.curveBusy ? "stop" : "play"
                text: root.modulations.curveBusy ? qsTr("Arrêter") : qsTr("Calculer")
                primary: !root.modulations.curveBusy
                enabled: root.modulations.curveBusy || (root.modulations.sweepError ?? "").length === 0
                tip: root.modulations.curveBusy ? qsTr("Arrêter la courbe en cours (ses points déjà mesurés restent)")
                     : (root.modulations.sweepError ?? "").length > 0 ? root.modulations.sweepError
                     : qsTr("Une courbe de plus pour « %1 » (des bits au hasard), de %2 à %3 dB")
                           .arg(root.result.text ?? "").arg(Units.compact(root.sweep.from ?? 0, "", 4)).arg(Units.compact(root.sweep.to ?? 12, "", 4))
                onClicked: root.modulations.curveBusy ? root.modulations.stopCurve() : root.modulations.startCurve()
            }
            RibbonButton {
                glyph: "clear"
                text: qsTr("Effacer")
                enabled: root.modulations.curves.length > 0
                tip: qsTr("Effacer toutes les courbes")
                onClicked: root.modulations.clearCurves()
            }
        }
        ViewGroup {
            visible: root.ribbonTab === 1
            target: berPlot
            plots: [berPlot]
            fitTip: qsTr("Toutes les courbes, échelles automatiques (double clic sur le tracé)")
        }
        ImageActions {
            visible: root.ribbonTab === 1
            target: berPlot
            baseName: "courbe-teb"
            onDone: (m) => root.infoMessage(m)
            onFailed: (m) => root.errorMessage(m)
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.canvas
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: root.leftFolded ? 22 : 0
        anchors.rightMargin: root.rightFolded ? 22 : 0
        spacing: 0

        // What the link is, and what it measured.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: Theme.panel
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.border
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8
                Label {
                    readonly property real room: Math.max(120, root.width * 0.5)
                    text: root.linkOk || (root.result.error ?? "").length === 0 ? (root.result.text ?? "") : (root.result.error ?? "")
                    color: root.linkOk || (root.result.error ?? "").length === 0 ? Theme.text : Theme.error
                    font.pixelSize: Theme.bodyText
                    font.bold: root.linkOk
                    elide: Text.ElideRight
                    Layout.maximumWidth: room
                    Layout.minimumWidth: Math.min(implicitWidth, room)
                }
                Label {
                    visible: root.linkOk
                    text: root.recipe.noise === true
                          ? qsTr("TEB %1 (%2 erreurs) · théorie %3 · Eb/N0 = %4 dB").arg(root.result.berText ?? "")
                                .arg(root.result.bitErrors ?? 0).arg(root.result.theoryText ?? "")
                                .arg(Units.compact(root.recipe.ebN0 ?? 0, "", 3))
                          : qsTr("Sans bruit · %1 erreurs").arg(root.result.bitErrors ?? 0)
                    color: (root.result.bitErrors ?? 0) > 0 ? Theme.warning : Theme.textMuted
                    font.pixelSize: Theme.smallText
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                }
                Item {
                    visible: !root.linkOk
                    Layout.fillWidth: true
                }
                Label {
                    visible: root.linkOk
                    text: (root.result.warnings ?? []).length > 0 ? "⚠" : "?"
                    font.pixelSize: Theme.bodyText
                    font.bold: true
                    color: (root.result.warnings ?? []).length > 0 ? Theme.warning
                           : linkInfoArea.containsMouse ? Theme.text : Theme.textMuted
                    MouseArea {
                        id: linkInfoArea
                        anchors.fill: parent
                        anchors.margins: -4
                        hoverEnabled: true
                        onClicked: root.detailRequested()
                    }
                    ToolTip.visible: linkInfoArea.containsMouse
                    ToolTip.text: (root.result.warnings ?? []).concat(root.result.summary ?? [])
                                  .concat([qsTr("Le détail : panneau Liaison (clic).")]).join("\n")
                }
                BusyIndicator {
                    visible: root.showBusy || (root.ribbonTab === 1 && root.modulations.curveBusy)
                    running: visible
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 18
                }
                Label {
                    visible: root.ribbonTab === 1 && root.modulations.curveBusy
                    text: Math.round(100 * root.modulations.curveProgress) + " %"
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
            }
        }

        // ---- Liaison: the signals and their spectra above; the constellation and the eye below ----
        SplitView {
            id: linkArea
            visible: root.ribbonTab === 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Vertical
            handle: Rectangle {
                implicitHeight: 5
                color: SplitHandle.hovered || SplitHandle.pressed ? Qt.alpha(Theme.accent, 0.5) : Theme.border
            }

            SplitView {
                SplitView.fillHeight: true
                SplitView.minimumHeight: 140
                orientation: Qt.Horizontal
                handle: Rectangle {
                    implicitWidth: 5
                    color: SplitHandle.hovered || SplitHandle.pressed ? Qt.alpha(Theme.accent, 0.5) : Theme.border
                }
                Item {
                    SplitView.fillWidth: true
                    SplitView.minimumWidth: 200
                    SignalPlot {
                        id: timePlot
                        anchors.fill: parent
                        anchors.bottomMargin: timeBar.height
                        session: root.modulations.preview
                        signalId: root.modulations.previewId
                        stacked: true
                        backgroundColor: Theme.canvas
                        gridColor: Theme.grid
                        textColor: Theme.text
                        mutedColor: Theme.textMuted
                        accentColor: Theme.accent
                        panelColor: Theme.panel
                        channelColors: Theme.channelColors
                        emptyText: (root.result.error ?? "").length > 0 ? root.result.error : qsTr("Calcul de la liaison…")
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
                    PlotToolbar {
                        anchors.top: parent.top
                        anchors.right: parent.right
                        anchors.topMargin: 8
                        anchors.rightMargin: 18
                        view: timePlot
                    }
                }
                Item {
                    SplitView.preferredWidth: Math.round(linkArea.width * 0.4)
                    SplitView.minimumWidth: 180
                    SpectrumPlot {
                        id: spectrum
                        anchors.fill: parent
                        anchors.bottomMargin: spectrumBar.height
                        analysis: root.analysis
                        logFrequency: false
                        backgroundColor: Theme.canvas
                        gridColor: Theme.grid
                        textColor: Theme.text
                        mutedColor: Theme.textMuted
                        accentColor: Theme.accent
                        panelColor: Theme.panel
                        channelColors: Theme.channelColors
                        emptyText: root.analysis.error.length > 0 ? root.analysis.error
                                   : root.modulations.hasPreview ? qsTr("Calcul des spectres…") : ""
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
                                text: qsTr("Spectres émis et reçu")
                                color: Theme.textMuted
                                font.pixelSize: Theme.smallText
                            }
                            BusyIndicator {
                                visible: root.analysis.busy
                                running: visible
                                Layout.preferredWidth: 16
                                Layout.preferredHeight: 16
                            }
                        }
                    }
                }
            }

            SplitView {
                SplitView.preferredHeight: Math.round(linkArea.height * 0.5)
                SplitView.minimumHeight: 160
                orientation: Qt.Horizontal
                handle: Rectangle {
                    implicitWidth: 5
                    color: SplitHandle.hovered || SplitHandle.pressed ? Qt.alpha(Theme.accent, 0.5) : Theme.border
                }
                Item {
                    SplitView.preferredWidth: Math.round(linkArea.width * 0.45)
                    SplitView.minimumWidth: 180
                    ModulationPlot {
                        id: constellationPlot
                        anchors.fill: parent
                        modulation: root.modulations
                        view: "constellation"
                        backgroundColor: Theme.canvas
                        gridColor: Theme.grid
                        textColor: Theme.text
                        mutedColor: Theme.textMuted
                        accentColor: Theme.accent
                        warningColor: Theme.error
                        panelColor: Theme.panel
                        curveColors: Theme.channelColors
                        emptyText: qsTr("Calcul de la liaison…")
                        HoverHandler {
                            onHoveredChanged: if (hovered) root.lastZoomed = constellationPlot
                        }
                    }
                }
                Item {
                    SplitView.fillWidth: true
                    SplitView.minimumWidth: 180
                    ModulationPlot {
                        id: eyePlot
                        anchors.fill: parent
                        modulation: root.modulations
                        view: "eye"
                        backgroundColor: Theme.canvas
                        gridColor: Theme.grid
                        textColor: Theme.text
                        mutedColor: Theme.textMuted
                        accentColor: Theme.accent
                        warningColor: Theme.error
                        panelColor: Theme.panel
                        curveColors: Theme.channelColors
                        emptyText: qsTr("Calcul de la liaison…")
                        HoverHandler {
                            onHoveredChanged: if (hovered) root.lastZoomed = eyePlot
                        }
                    }
                }
            }
        }

        // ---- Courbe TEB ----
        Item {
            visible: root.ribbonTab === 1
            Layout.fillWidth: true
            Layout.fillHeight: true
            ModulationPlot {
                id: berPlot
                anchors.fill: parent
                modulation: root.modulations
                view: "ber"
                backgroundColor: Theme.canvas
                gridColor: Theme.grid
                textColor: Theme.text
                mutedColor: Theme.textMuted
                accentColor: Theme.accent
                warningColor: Theme.error
                panelColor: Theme.panel
                curveColors: Theme.channelColors
            }
            Label {
                visible: (root.modulations.sweepError ?? "").length > 0
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.leftMargin: 70
                anchors.bottomMargin: 46
                text: "⚠ " + (root.modulations.sweepError ?? "")
                color: Theme.warning
                font.pixelSize: Theme.smallText
            }
        }
    }

    // Right click on a scale: its ends, automatic; for the spectrum, dB.
    ScaleMenu {
        id: scaleMenu
        optionsProvider: (scale) => {
            if (scaleMenu.view !== spectrum)
                return []
            if (scale.kind === "levels")
                return [{ key: "db", text: qsTr("Niveaux en décibels"), checked: root.decibel, enabled: true }]
            return []
        }
        onOptionToggled: (key, on) => {
            if (key === "db")
                root.analysis.setSettings({ scale: on ? "db" : "linear" })
        }
    }
    PlotMenu {
        id: signalMenu
        view: timePlot
        mode: "channels"
        actions: [{ key: "sent", text: qsTr("Ajouter le signal émis à la session") },
                  { key: "received", text: qsTr("Ajouter le signal reçu à la session") }]
        onActionTriggered: (key) => root.modulations.addToSession(key === "received")
    }
    PlotMenu {
        id: spectrumMenu
        view: spectrum
        mode: "plain"
        settingsTitle: qsTr("Calcul des spectres")
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
        toolTip: qsTr("Afficher la liaison en détail (F10)")
        onClicked: root.unfoldRight()
    }
}
