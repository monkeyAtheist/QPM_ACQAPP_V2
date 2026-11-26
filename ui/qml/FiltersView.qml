import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// Module "Filtres": a filter designed (IIR Butterworth, Chebyshev I or II, Bessel, elliptic; FIR by the
// window method or equiripple, Parks-McClellan; its order chosen, or the least that holds a template: Gabarit,
// the template then drawn on the gain), what it does seen along the frequencies (gain, phase, group
// delay), along the samples (impulse and step responses) and in the z-plane (poles and zeros), with a gain at
// its output; a band low in the spectrum shown on logarithmic frequencies; then tried on the signal chosen:
// one channel before and after, with their spectra, applied causal, at zero phase or aligned; and the whole
// signal filtered into the session in one click (undoable); or written in C for a microcontroller (Code C).
// The page opens blank, without filter (H = 1), and so does « Par défaut »; the recipe of the last run can be taken
// up again (Exemples ▸ Reprendre…). Its detail and its coefficients are in the right panel (Filtre).
Item {
    id: root
    required property var filters
    required property var player
    required property var session
    required property var appSettings
    property var signalId: 0 // the signal chosen in the list: the one the filter is tried on
    property Item ribbonHost: null
    // The height its header needs (one row of groups, or more in a narrow window): the host takes it.
    readonly property real ribbonHeight: moduleHeader.implicitHeight
    property bool leftFolded: false
    property bool rightFolded: false
    signal unfoldLeft()
    signal unfoldRight()
    signal designRequested()
    signal infoMessage(string text)
    signal errorMessage(string text)

    property int ribbonTab: 0
    readonly property var recipe: filters.recipe
    readonly property var design: filters.design
    readonly property bool designed: design.ok === true
    // Designed (IIR of a family, FIR by a window or equiripple), or coefficients typed (b and a).
    readonly property bool typed: recipe.kind === "typed"
    readonly property bool iir: recipe.kind === "iir"
    readonly property bool fir: recipe.kind === "fir"
    // No filter yet (the blank page): H = 1, the output is the input.
    readonly property bool none: recipe.kind === "none"
    readonly property bool band: !typed && !none && (recipe.response === "bandpass" || recipe.response === "bandstop")
    // A FIR: by a window (its key), or equiripple (Parks-McClellan).
    readonly property bool equiripple: fir && recipe.firDesign === "equiripple"
    readonly property string firDesign: equiripple ? "equiripple" : (recipe.window ?? "hamming")
    // What the family asks for: a ripple (Chebyshev I, elliptic, equiripple), an attenuation (Chebyshev II,
    // elliptic, equiripple), the width of the transitions (equiripple).
    readonly property bool hasRipple: (iir && (recipe.family === "chebyshev1" || recipe.family === "elliptic")) || equiripple
    readonly property bool hasAttenuation: (iir && (recipe.family === "chebyshev2" || recipe.family === "elliptic")) || equiripple
    // The order (the taps) computed from a template (gabarit): the edges of its bands, Amax over the pass band,
    // Amin over the stop band; the recipe as designed has what was computed.
    readonly property bool fromTemplate: !typed && !none && recipe.fromTemplate === true
    readonly property var designedRecipe: design.recipe ?? recipe
    // The edges of the template in their order along the frequencies, as its response has them.
    readonly property var templateEdges: {
        const edge = (key, label, tip) => ({ key: key, label: label, tip: tip })
        switch (recipe.response) {
        case "highpass":
            return [edge("stop1", "fa", qsTr("La bande coupée va jusqu'à fa : au moins Amin d'atténuation en deçà")),
                    edge("pass1", "fp", qsTr("La bande passante commence à fp : au plus Amax d'atténuation au-delà"))]
        case "bandpass":
            return [edge("stop1", "fa₁", qsTr("La bande coupée basse va jusqu'à fa₁ (au moins Amin d'atténuation)")),
                    edge("pass1", "fp₁", qsTr("La bande passante commence à fp₁ (au plus Amax d'atténuation)")),
                    edge("pass2", "fp₂", qsTr("La bande passante va jusqu'à fp₂")),
                    edge("stop2", "fa₂", qsTr("La bande coupée haute commence à fa₂"))]
        case "bandstop":
            return [edge("pass1", "fp₁", qsTr("La bande passante basse va jusqu'à fp₁ (au plus Amax d'atténuation)")),
                    edge("stop1", "fa₁", qsTr("La bande coupée commence à fa₁ (au moins Amin d'atténuation)")),
                    edge("stop2", "fa₂", qsTr("La bande coupée va jusqu'à fa₂")),
                    edge("pass2", "fp₂", qsTr("La bande passante haute commence à fp₂"))]
        default:
            return [edge("pass1", "fp", qsTr("La bande passante va jusqu'à fp : au plus Amax d'atténuation en deçà")),
                    edge("stop1", "fa", qsTr("La bande coupée commence à fa : au moins Amin d'atténuation au-delà"))]
        }
    }
    // Its highest edge under 3 % of fs/2 (20 Hz at 10 kHz, a band around 50 Hz): on linear frequencies all of
    // it would be a sliver at the left. The frequencies follow the filter when it crosses that line, either way
    // (logarithmic below, linear above); in between, the button « Log » decides. (A template: its highest edge.)
    readonly property real topEdge: fromTemplate ? (band ? Math.max(recipe.pass2 ?? 0, recipe.stop2 ?? 0)
                                                         : Math.max(recipe.pass1 ?? 0, recipe.stop1 ?? 0))
                                    : band ? (recipe.frequency2 ?? 0) : (recipe.frequency1 ?? 0)
    readonly property bool lowBand: !typed && !none && topEdge > 0 && topEdge < 0.03 * (recipe.sampleRate ?? 0) / 2
    onLowBandChanged: followScale()
    function followScale() {
        responsePlot.logFrequency = root.lowBand
        if ((root.settings.logFrequency === true) !== root.lowBand)
            root.analysis.setSettings({ logFrequency: root.lowBand })
    }
    // The gain at the output, as a factor ("×2").
    readonly property real gainFactor: Math.pow(10, (recipe.gain ?? 0) / 20)
    readonly property var input: filters.input
    readonly property bool hasInput: (input.name ?? "").length > 0
    readonly property var inputChannels: input.channels ?? []
    readonly property string channelName: inputChannels[Math.min(filters.channel, inputChannels.length - 1)] ?? ""
    readonly property var analysis: filters.previewAnalysis
    readonly property var settings: analysis.settings
    readonly property bool decibel: settings.scale !== "linear"
    readonly property string rateText: Units.compact(recipe.sampleRate ?? 0, "Hz", 6)
    // The plot pointed at last, in the tab "Appliquer": the buttons of the view act on it.
    property Item lastZoomed: null
    readonly property Item appliedPlot: lastZoomed && lastZoomed.visible ? lastZoomed : timePlot

    function nameOf(list, key) {
        const item = list.find((x) => x.key === key)
        return item ? item.name : ""
    }
    function set(values) {
        filters.setRecipe(values)
    }
    function addToSession() {
        if (!root.hasInput) {
            root.errorMessage(qsTr("Choisissez d'abord un signal dans la liste de gauche : c'est lui qui sera filtré."))
            return
        }
        root.filters.addToSession()
    }

    // Tried only while the tab "Appliquer" is on screen; so is the analysis of the preview.
    Binding {
        target: root.filters
        property: "active"
        value: root.visible && root.ribbonTab === 1
    }
    Binding {
        target: root.filters
        property: "inputId"
        value: root.signalId
    }
    Binding {
        target: root.analysis
        property: "signalId"
        value: root.visible && root.ribbonTab === 1 ? root.filters.previewId : 0
    }

    // The page opens blank (no filter); the recipe of the last run is kept, to be taken up again (Exemples ▸
    // Reprendre…), and each change is remembered a moment after.
    property string previousRecipe: ""
    Component.onCompleted: {
        const saved = appSettings.value("filters/recipe", "")
        root.previousRecipe = saved && saved.length > 0 ? saved : ""
    }
    Timer {
        id: rememberTimer
        interval: 800
        onTriggered: if (!root.none) root.appSettings.setValue("filters/recipe", root.filters.recipeText()) // not "no filter"
    }

    // While the preview plays, each new preview plays in its place: what is changed in the filter is heard.
    readonly property bool previewReady: root.filters.ready
    onPreviewReadyChanged: if (previewReady) listenGroup.relisten()

    // Another signal or channel tried: the plots show all of it (its spectrum once computed).
    property string shownInput: ""
    property bool fitSpectrum: false
    Connections {
        target: root.filters
        function onRecipeChanged() { rememberTimer.restart() }
        function onStateChanged() {
            if (!root.filters.ready || !root.filters.hasPreview)
                return
            const shown = root.signalId + "|" + root.filters.channel + "|" + (root.input.frames ?? 0)
            if (shown === root.shownInput)
                return
            root.shownInput = shown
            timePlot.showAll()
            root.fitSpectrum = true
        }
        function onAdded(id) {
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
    // The wheel while filtering: shown only if it lasts.
    property bool showBusy: false
    Timer {
        id: busyDelay
        interval: 300
        onTriggered: root.showBusy = root.filters.busy || root.filters.adding
    }
    Connections {
        target: root.filters
        function onBusyChanged() {
            if (root.filters.busy)
                busyDelay.restart()
            else
                root.showBusy = root.filters.adding
        }
        function onAddingChanged() {
            if (root.filters.adding)
                busyDelay.restart()
            else
                root.showBusy = root.filters.busy
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
                    enabled: modelData.enabled !== false
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
    // The group "Sortie": the gain at the output (both tabs: what it does is seen on the response and on the
    // signal filtered).
    component OutputGroup: RibbonGroup {
        id: output
        property var view: null
        title: qsTr("Sortie")
        RibbonField {
            anchors.verticalCenter: parent.verticalCenter
            label: qsTr("Gain")
            fieldWidth: 64
            unit: "dB"
            minimum: -120
            maximum: 120
            value: output.view ? (output.view.recipe.gain ?? 0) : 0
            tip: qsTr("L'amplification du signal filtré : +6 dB le double, −6 dB la moitié (ici ×%1). Une fois, quelle que soit l'application : à phase nulle aussi, où le reste du filtre compte deux fois.")
                 .arg(output.view ? Units.compact(output.view.gainFactor, "", 4) : "1")
            onEdited: (v) => { if (output.view) output.view.set({ gain: v }) }
        }
    }
    // A button of the group "Afficher": one of the responses of `plot`.
    component ResponseButton: RibbonButton {
        property string key: ""
        property var plot: null
        checked: plot !== null && plot.response === key
        onClicked: if (plot !== null) plot.response = key
    }

    Ribbon {
        id: moduleHeader
        parent: root.ribbonHost ? root.ribbonHost : root
        visible: root.visible
        tabs: [qsTr("Concevoir"), qsTr("Appliquer")]
        tabIcons: ["filter", "signals"]
        tabTips: [qsTr("Le filtre : son type, sa réponse, ses fréquences, son ordre ; ce qu'il fait (gain, phase, retard, réponses, pôles et zéros)"),
                  qsTr("Le filtre essayé sur le signal choisi dans la liste : une voie avant et après, leurs spectres, l'écoute")]
        currentTab: root.ribbonTab
        onTabClicked: (index) => root.ribbonTab = index

        // ---- In both tabs: the signal chosen, filtered into the session ----
        RibbonGroup {
            title: qsTr("Session")
            RibbonButton {
                glyph: "keep"
                text: qsTr("Ajouter")
                primary: root.hasInput && root.designed && !root.filters.adding
                enabled: root.designed && !root.filters.adding
                tip: !root.designed ? (root.design.error ?? "")
                     : root.filters.adding ? qsTr("Filtrage en cours…")
                     : root.hasInput ? qsTr("« %1 » filtré (toutes ses voies) devient un signal de la session, dans la liste de gauche (Ctrl+Z l'enlève)").arg(root.input.name)
                     : qsTr("Choisissez d'abord un signal dans la liste de gauche : c'est lui qui sera filtré")
                onClicked: root.addToSession()
            }
        }

        // ---- Concevoir: the filter ----
        RibbonGroup {
            visible: root.ribbonTab === 0
            title: qsTr("Recette")
            RibbonButton {
                id: examplesButton
                glyph: "preset"
                text: qsTr("Exemples")
                dropDown: true
                tip: qsTr("Des filtres tout prêts : passe-bas, coupe-bande 50 Hz, passe-haut contre la dérive, bande téléphone, RIF…")
                onClicked: examplesMenu.popup(examplesButton, 0, examplesButton.height)
                FitMenu {
                    id: examplesMenu
                    Instantiator {
                        model: root.filters.examples
                        delegate: MenuItem {
                            required property var modelData
                            required property int index
                            text: modelData.name
                            onTriggered: root.filters.useExample(index)
                            ToolTip.visible: hovered
                            ToolTip.delay: 400
                            ToolTip.text: modelData.tip
                        }
                        onObjectAdded: (index, object) => examplesMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) => examplesMenu.removeItem(object)
                    }
                    MenuSeparator { }
                    MenuItem {
                        text: qsTr("Reprendre le filtre de la dernière fois")
                        enabled: root.previousRecipe.length > 0
                        onTriggered: {
                            root.filters.restoreRecipe(root.previousRecipe)
                            Qt.callLater(() => { if (root.lowBand) root.followScale() })
                        }
                        ToolTip.visible: hovered
                        ToolTip.delay: 400
                        ToolTip.text: qsTr("Le filtre où le module en était quand l'application a été fermée (il s'ouvre sur une page vierge, sans filtre)")
                    }
                }
            }
            RibbonButton {
                glyph: "rotate"
                text: qsTr("Par défaut")
                tip: qsTr("Repartir d'une page vierge : aucun filtre (la sortie est l'entrée) ; le premier type choisi part d'un passe-bas à 1 kHz (ou fs/10)")
                onClicked: root.filters.reset()
            }
            RibbonButton {
                glyph: "rename"
                text: qsTr("Détail")
                tip: qsTr("Le filtre en détail, ce qu'il fait et ses coefficients : panneau Filtre (F10)")
                onClicked: root.designRequested()
            }
        }
        RibbonGroup {
            visible: root.ribbonTab === 0
            title: qsTr("Microcontrôleur")
            RibbonButton {
                glyph: "chip"
                text: qsTr("Code C")
                enabled: root.designed && !root.none
                tip: root.none ? qsTr("Aucun filtre : rien à écrire en C ; choisissez d'abord un type de filtre")
                     : root.designed ? qsTr("Le filtre en C pour microcontrôleur : flottant (float, double) ou virgule fixe (Q15, Q31), ce que l'arrondi lui fait, un autotest ; à copier ou enregistrer (.h et .c)")
                                     : (root.design.error ?? "")
                onClicked: codeDialog.open()
            }
        }
        RibbonGroup {
            visible: root.ribbonTab === 0
            title: qsTr("Filtre")
            ChoiceButton {
                glyph: root.none ? "nofilter" : root.typed ? "typed" : root.iir ? "iir" : "fir"
                text: root.none ? qsTr("Aucun") : root.typed ? qsTr("b, a") : root.iir ? qsTr("RII") : qsTr("RIF")
                primary: root.none
                options: Choices.filterKinds
                current: root.recipe.kind ?? "none"
                tip: root.none ? qsTr("Aucun filtre pour l'instant (la sortie est l'entrée) : choisissez un RII, un RIF ou des coefficients tapés ; le premier part d'un passe-bas à 1 kHz")
                     : root.typed ? qsTr("Coefficients tapés : b[k] et a[k] de y[n] = Σ b[k] x[n − k] − Σ a[k] y[n − k], dans le panneau Filtre (F10)")
                     : root.iir ? qsTr("Récursif (réponse impulsionnelle infinie) : peu de calcul pour des flancs raides ; sa phase n'est pas linéaire")
                                : qsTr("Non récursif (réponse impulsionnelle finie) : phase linéaire, toujours stable ; il lui faut plus de coefficients")
                onChosen: (key) => root.set({ kind: key })
            }
            ChoiceButton {
                visible: !root.typed && !root.none
                glyph: "response"
                text: root.nameOf(Choices.filterResponses, root.recipe.response ?? "")
                options: Choices.filterResponses
                current: root.recipe.response ?? "lowpass"
                tip: qsTr("Ce qu'il laisse passer : en dessous de la coupure, au-dessus, dans une bande, ou tout sauf une bande")
                onChosen: (key) => root.set({ response: key })
            }
            ChoiceButton {
                visible: root.iir
                glyph: "family"
                text: root.nameOf(Choices.filterFamilies, root.recipe.family ?? "")
                options: Choices.filterFamilies
                current: root.recipe.family ?? "butterworth"
                tip: qsTr("La forme de sa réponse : plate (Butterworth), plus raide avec une ondulation (Chebyshev), sans dépassement (Bessel), la plus raide avec une ondulation dans les deux bandes (elliptique)")
                onChosen: (key) => root.set({ family: key })
            }
            ChoiceButton {
                visible: root.fir
                glyph: "window"
                text: root.equiripple ? qsTr("Équiondulation") : root.nameOf(Choices.firWindows, root.recipe.window ?? "")
                options: Choices.firDesigns
                current: root.firDesign
                tip: root.equiripple ? qsTr("Parks-McClellan : les transitions données, l'erreur répartie au mieux entre les bandes ; le RIF optimal à nombre de coefficients donné")
                                     : qsTr("La fenêtre qui tronque la réponse idéale : elle fixe le niveau des lobes hors de la bande et la largeur de la transition ; ou l'équiondulation (Parks-McClellan)")
                onChosen: (key) => key === "equiripple" ? root.set({ firDesign: "equiripple" })
                                                        : root.set({ firDesign: "window", window: key })
            }
        }
        RibbonGroup {
            visible: root.ribbonTab === 0
            title: qsTr("Réglage")
            // The order chosen, or computed from a template.
            RibbonButton {
                visible: !root.typed && !root.none
                glyph: "template"
                text: qsTr("Gabarit")
                checked: root.fromTemplate
                tip: root.fromTemplate ? qsTr("Ordre calculé : le plus petit qui tient le gabarit (jusqu'où il passe avec au plus Amax d'atténuation, d'où il coupe avec au moins Amin). Cliquer revient à l'ordre et aux fréquences choisis, en partant du filtre calculé")
                                       : qsTr("Calculer l'ordre (un RIF : le nombre de coefficients) le plus petit qui tient un gabarit : jusqu'où il passe avec au plus Amax d'atténuation, d'où il coupe avec au moins Amin")
                onClicked: root.set({ fromTemplate: !root.fromTemplate })
            }
            // Coefficients typed: in the panel, where there is room for them.
            RibbonButton {
                visible: root.typed
                glyph: "typed"
                text: qsTr("Coefficients")
                primary: !root.designed
                tip: root.designed ? qsTr("b[k] et a[k], des nombres ou des formules de k, et les variables : panneau Filtre (F10)")
                                   : (root.design.error ?? "")
                onClicked: root.designRequested()
            }
            RibbonField {
                visible: !root.typed && !root.none && !root.fromTemplate
                anchors.verticalCenter: parent.verticalCenter
                label: root.band ? qsTr("f basse") : qsTr("Coupure")
                fieldWidth: 80
                unit: "Hz"
                minimum: 0
                maximum: 1e12
                value: root.recipe.frequency1 ?? 1000
                invalid: !root.designed
                tip: root.band ? qsTr("Le bas de la bande, au-dessous de fs/2 = %1").arg(Units.compact((root.recipe.sampleRate ?? 0) / 2, "Hz", 4))
                               : qsTr("La fréquence de coupure, au-dessous de fs/2 = %1").arg(Units.compact((root.recipe.sampleRate ?? 0) / 2, "Hz", 4))
                onEdited: (v) => root.set({ frequency1: v })
            }
            RibbonField {
                visible: root.band && !root.fromTemplate
                anchors.verticalCenter: parent.verticalCenter
                label: qsTr("f haute")
                fieldWidth: 80
                unit: "Hz"
                minimum: 0
                maximum: 1e12
                value: root.recipe.frequency2 ?? 2000
                invalid: !root.designed
                tip: qsTr("Le haut de la bande, au-dessus de la fréquence basse")
                onEdited: (v) => root.set({ frequency2: v })
            }
            // The template: its edges in their order along the frequencies.
            Repeater {
                model: root.fromTemplate ? root.templateEdges.length : 0
                delegate: RibbonField {
                    required property int index
                    readonly property var edge: root.templateEdges[index] ?? ({ key: "pass1", label: "", tip: "" })
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    label: edge.label
                    fieldWidth: 74
                    unit: "Hz"
                    minimum: 0
                    maximum: 1e12
                    value: root.recipe[edge.key] ?? 0
                    invalid: !root.designed
                    tip: edge.tip + qsTr(" (fs/2 = %1)").arg(Units.compact((root.recipe.sampleRate ?? 0) / 2, "Hz", 4))
                    onEdited: (v) => {
                        const change = {}
                        change[edge.key] = v
                        root.set(change)
                    }
                }
            }
            RibbonField {
                visible: root.iir && (!root.fromTemplate || root.designed)
                anchors.verticalCenter: parent.verticalCenter
                label: qsTr("Ordre")
                fieldWidth: 46
                minimum: 1
                maximum: 32
                readOnly: root.fromTemplate
                value: root.fromTemplate ? (root.designedRecipe.order ?? 0) : (root.recipe.order ?? 4)
                tip: root.fromTemplate ? qsTr("Calculé : le plus petit ordre qui tient le gabarit (ce qu'il faudrait avec les autres familles : panneau Filtre)")
                     : root.band ? qsTr("L'ordre du prototype passe-bas (1 à 32) : la bande en a deux fois plus, chaque section du second ordre en porte deux")
                                 : qsTr("De 1 à 32 : chaque ordre de plus raidit le flanc de 6 dB par octave (Butterworth)")
                onEdited: (v) => root.set({ order: Math.round(v) })
            }
            RibbonField {
                visible: root.fir && (!root.fromTemplate || root.designed)
                anchors.verticalCenter: parent.verticalCenter
                label: qsTr("Coefficients")
                fieldWidth: 66
                minimum: 3
                maximum: root.equiripple ? 4095 : 65535
                readOnly: root.fromTemplate
                value: root.fromTemplate ? (root.designedRecipe.taps ?? 0) : (root.recipe.taps ?? 101)
                tip: root.fromTemplate ? qsTr("Calculé : le plus petit nombre de coefficients qui tient le gabarit ; le retard est de (N − 1)/2 échantillons")
                     : root.equiripple ? qsTr("Le nombre N de coefficients (3 à 4 095) : plus il y en a, mieux le gabarit est tenu ; le retard est de (N − 1)/2 échantillons. Un passe-haut ou un coupe-bande en veut un nombre impair.")
                                       : qsTr("Le nombre N de coefficients (3 à 65 535) : plus il y en a, plus la transition est étroite ; le retard est de (N − 1)/2 échantillons. Un passe-haut ou un coupe-bande en veut un nombre impair.")
                onEdited: (v) => root.set({ taps: Math.round(v) })
            }
            RibbonField {
                visible: root.hasRipple || root.fromTemplate
                anchors.verticalCenter: parent.verticalCenter
                label: root.fromTemplate ? qsTr("Amax") : qsTr("Ondulation")
                fieldWidth: 58
                unit: "dB"
                minimum: 0.001
                maximum: 40
                value: root.recipe.ripple ?? 1
                tip: root.fromTemplate ? (root.fir ? qsTr("L'ondulation permise au plus dans la bande passante, d'un extrême à l'autre")
                                                   : qsTr("L'atténuation permise au plus dans la bande passante : le gain y reste entre 0 et −Amax dB"))
                     : root.equiripple ? qsTr("L'écart permis dans la bande passante, d'un extrême à l'autre : avec l'atténuation, il fixe le poids de chaque bande")
                                       : qsTr("L'ondulation permise dans la bande passante (le gain y reste entre 0 et −R dB) : plus elle est grande, plus le flanc est raide")
                onEdited: (v) => root.set({ ripple: v })
            }
            RibbonField {
                visible: root.hasAttenuation || root.fromTemplate
                anchors.verticalCenter: parent.verticalCenter
                label: root.fromTemplate ? qsTr("Amin") : qsTr("Atténuation")
                fieldWidth: 58
                unit: "dB"
                minimum: 1
                maximum: 300
                value: root.recipe.attenuation ?? 40
                tip: root.fromTemplate ? qsTr("L'atténuation exigée au moins dans la bande coupée")
                     : root.equiripple ? qsTr("Ce que la bande coupée doit atténuer au moins : ce qui est atteint est dit dans le panneau Filtre")
                     : root.recipe.family === "elliptic" ? qsTr("L'atténuation garantie dans la bande coupée, à partir d'une fréquence d'arrêt calculée (dite dans le panneau Filtre)")
                                                         : qsTr("L'atténuation garantie dans la bande coupée, à partir de la fréquence donnée")
                onEdited: (v) => root.set({ attenuation: v })
            }
            RibbonField {
                visible: root.equiripple && !root.fromTemplate
                anchors.verticalCenter: parent.verticalCenter
                label: qsTr("Transition")
                fieldWidth: 70
                unit: "Hz"
                minimum: 1e-6
                maximum: 1e12
                value: root.recipe.transition ?? 200
                tip: qsTr("La largeur de chaque transition, centrée sur sa fréquence : bande passante et bande coupée s'arrêtent à une demi-transition de part et d'autre")
                onEdited: (v) => root.set({ transition: v })
            }
            RibbonField {
                visible: root.fir && !root.equiripple && root.recipe.window === "kaiser" && (!root.fromTemplate || root.designed)
                anchors.verticalCenter: parent.verticalCenter
                label: "β"
                fieldWidth: 46
                minimum: 0
                maximum: 50
                readOnly: root.fromTemplate
                value: root.fromTemplate ? (root.designedRecipe.kaiserBeta ?? 0) : (root.recipe.kaiserBeta ?? 8.6)
                tip: root.fromTemplate ? qsTr("Calculé d'après l'atténuation et l'ondulation demandées (formule de Kaiser)")
                                       : qsTr("La forme de la fenêtre de Kaiser : 5 donne des lobes vers −54 dB, 8,6 vers −87 dB (une transition plus large)")
                onEdited: (v) => root.set({ kaiserBeta: v })
            }
        }
        OutputGroup {
            visible: root.ribbonTab === 0
            view: root
        }
        RibbonGroup {
            visible: root.ribbonTab === 0
            title: qsTr("Échantillonnage")
            RibbonButton {
                glyph: "rate"
                text: qsTr("fs du signal")
                checked: root.recipe.rateOfSignal === true
                tip: root.hasInput ? qsTr("Le filtre est conçu pour la fréquence d'échantillonnage du signal choisi (%1)").arg(Units.compact(root.input.sampleRate ?? 0, "Hz", 6))
                                   : qsTr("Le filtre prend la fréquence d'échantillonnage du signal choisi dans la liste")
                onClicked: root.set({ rateOfSignal: !(root.recipe.rateOfSignal === true) })
            }
            RibbonField {
                anchors.verticalCenter: parent.verticalCenter
                label: qsTr("fs")
                fieldWidth: 80
                unit: "Hz"
                minimum: 1e-6
                maximum: 1e12
                value: root.recipe.sampleRate ?? 48000
                tip: qsTr("La fréquence d'échantillonnage pour laquelle il est conçu : la taper la fixe (« fs du signal » la suit à nouveau)")
                onEdited: (v) => root.set({ sampleRate: v, rateOfSignal: false })
            }
        }
        RibbonGroup {
            visible: root.ribbonTab === 0
            title: qsTr("Afficher")
            ResponseButton {
                plot: responsePlot
                key: "gain"
                glyph: "gain"
                text: qsTr("Gain")
                tip: qsTr("|H(f)| le long des fréquences, de 0 à fs/2 (en dB ou linéaire)")
            }
            ResponseButton {
                plot: responsePlot
                key: "phase"
                glyph: "phase"
                text: qsTr("Phase")
                tip: qsTr("La phase de H(f) en degrés, dépliée (un saut de 180° là où un zéro est sur le cercle)")
            }
            ResponseButton {
                plot: responsePlot
                key: "delay"
                glyph: "delay"
                text: qsTr("Retard")
                tip: qsTr("Le temps de propagation de groupe −dφ/dω, en échantillons : constant, les formes ne sont pas déformées")
            }
            ResponseButton {
                plot: responsePlot
                key: "impulse"
                glyph: "impulse"
                text: qsTr("Impulsion")
                tip: qsTr("La réponse impulsionnelle h[n] : ce que donne un Dirac")
            }
            ResponseButton {
                plot: responsePlot
                key: "step"
                glyph: "stepresp"
                text: qsTr("Indice")
                tip: qsTr("La réponse indicielle : ce que donne un échelon (dépassement, temps de montée)")
            }
            ResponseButton {
                plot: responsePlot
                key: "polezero"
                glyph: "polezero"
                text: qsTr("Pôles, zéros")
                tip: qsTr("Les pôles (×) et les zéros (○) dans le plan z : stable si tous les pôles sont dans le cercle unité")
            }
        }
        RibbonGroup {
            visible: root.ribbonTab === 0
            title: qsTr("Échelles")
            RibbonButton {
                glyph: "logscale"
                text: qsTr("Log")
                checked: responsePlot.logFrequency
                enabled: responsePlot.response === "gain" || responsePlot.response === "phase" || responsePlot.response === "delay"
                tip: qsTr("Fréquences logarithmiques : chaque décade a la même largeur")
                onClicked: responsePlot.logFrequency = !responsePlot.logFrequency
            }
            RibbonButton {
                glyph: "decibel"
                text: qsTr("Décibels")
                checked: responsePlot.decibels
                enabled: responsePlot.response === "gain"
                tip: qsTr("Le gain en dB (20 log₁₀ |H|) ou linéaire")
                onClicked: responsePlot.decibels = !responsePlot.decibels
            }
            RibbonButton {
                glyph: "wrap"
                text: qsTr("Repliée")
                checked: responsePlot.wrapPhase
                enabled: responsePlot.response === "phase"
                tip: qsTr("La phase entre −180° et 180°, plutôt que dépliée")
                onClicked: responsePlot.wrapPhase = !responsePlot.wrapPhase
            }
            RibbonButton {
                glyph: "applied"
                text: qsTr("Appliqué")
                checked: responsePlot.showApplied
                enabled: responsePlot.appliedDiffers
                tip: root.recipe.application === "causal"
                     ? qsTr("Appliqué en causal, il fait ce qui est tracé (l'application : onglet Appliquer)")
                     : !responsePlot.appliedDiffers ? qsTr("Ici, l'application ne change rien à ce qui est tracé (un RIF aligné garde son gain)")
                     : root.recipe.application === "zerophase"
                     ? qsTr("En pointillé, le filtre tel qu'il est appliqué à phase nulle : gain au carré, ni phase ni retard")
                     : qsTr("En pointillé, le RIF tel qu'il est appliqué : sans son retard de (N − 1)/2 échantillons")
                onClicked: responsePlot.showApplied = !responsePlot.showApplied
            }
        }
        ViewGroup {
            visible: root.ribbonTab === 0
            target: responsePlot
            fitTip: qsTr("Toute la réponse, échelle automatique (double clic sur le tracé)")
        }
        ImageActions {
            visible: root.ribbonTab === 0
            target: responseArea
            baseName: "reponse-filtre"
            onDone: (m) => root.infoMessage(m)
            onFailed: (m) => root.errorMessage(m)
        }

        // ---- Appliquer: the filter on the signal chosen ----
        ListenGroup {
            id: listenGroup
            visible: root.ribbonTab === 1
            player: root.player
            session: root.filters.preview
            signalId: root.filters.hasPreview ? root.filters.previewId : 0
            channels: [{ name: qsTr("Avant") }, { name: qsTr("Après") }]
            onInfoMessage: (text) => root.infoMessage(text)
            onErrorMessage: (text) => root.errorMessage(text)
        }
        RibbonGroup {
            visible: root.ribbonTab === 1
            title: qsTr("Signal")
            RibbonButton {
                id: channelButton
                glyph: "signals"
                text: root.channelName.length > 0 ? root.channelName : qsTr("Voie")
                dropDown: true
                enabled: root.inputChannels.length > 1
                tip: root.hasInput ? qsTr("La voie de « %1 » montrée avant et après (« Ajouter » les filtre toutes)").arg(root.input.name)
                                   : qsTr("Choisissez un signal dans la liste de gauche")
                onClicked: channelMenu.popup(channelButton, 0, channelButton.height)
                FitMenu {
                    id: channelMenu
                    Instantiator {
                        model: root.inputChannels
                        delegate: MenuItem {
                            required property var modelData
                            required property int index
                            text: modelData
                            checkable: true
                            checked: index === root.filters.channel
                            onTriggered: {
                                root.filters.channel = index
                                checked = Qt.binding(() => index === root.filters.channel)
                            }
                        }
                        onObjectAdded: (index, object) => channelMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) => channelMenu.removeItem(object)
                    }
                }
            }
        }
        RibbonGroup {
            visible: root.ribbonTab === 1 && !root.none
            title: qsTr("Application")
            RibbonButton {
                glyph: "causal"
                text: qsTr("Causale")
                checked: root.recipe.application === "causal"
                tip: qsTr("Comme en temps réel, échantillon après échantillon : avec son retard et sa phase")
                onClicked: root.set({ application: "causal" })
            }
            RibbonButton {
                glyph: "zerophase"
                text: qsTr("Phase nulle")
                checked: root.recipe.application === "zerophase"
                tip: qsTr("Aller puis retour (filtfilt) : ni retard ni déphasage, le gain au carré (l'atténuation double) ; hors temps réel")
                onClicked: root.set({ application: "zerophase" })
            }
            RibbonButton {
                readonly property bool allowed: root.fir || (root.typed && root.design.fir === true)
                glyph: "aligned"
                text: qsTr("Alignée")
                checked: root.recipe.application === "aligned"
                enabled: allowed
                tip: allowed ? qsTr("Le RIF une fois, son retard de (N − 1)/2 échantillons retiré : le signal filtré reste en face de l'original")
                             : qsTr("Pour un RIF seulement (sa phase linéaire est un simple retard)")
                onClicked: root.set({ application: "aligned" })
            }
            ChoiceButton {
                glyph: "edges"
                text: qsTr("Bords")
                options: Choices.edgeExtensions
                current: root.recipe.edges ?? "predicted"
                enabled: root.recipe.application === "zerophase"
                tip: root.recipe.application === "zerophase"
                     ? qsTr("Comment l'enregistrement est prolongé au-delà de ses bords avant l'aller-retour, pour que le filtre y démarre comme si le signal continuait : %1")
                       .arg(root.nameOf(Choices.edgeExtensions, root.recipe.edges ?? "predicted"))
                     : qsTr("À phase nulle seulement : comment l'enregistrement est prolongé au-delà de ses bords avant l'aller-retour")
                onChosen: (key) => root.set({ edges: key })
            }
        }
        OutputGroup {
            visible: root.ribbonTab === 1
            view: root
        }
        ViewGroup {
            visible: root.ribbonTab === 1
            target: root.appliedPlot
            plots: [timePlot, spectrum]
            backTarget: root.lastZoomed && root.lastZoomed.canGoBack ? root.lastZoomed
                        : spectrum.canGoBack ? spectrum : timePlot
            fitTip: qsTr("Tout l'aperçu et toutes les fréquences, échelles automatiques (double clic sur un tracé : ce tracé seulement)")
        }
        RibbonGroup {
            visible: root.ribbonTab === 1
            title: qsTr("Voies")
            RibbonButton {
                glyph: "overlay"
                text: qsTr("Superposées")
                checked: !timePlot.stacked
                enabled: timePlot.hasData
                tip: qsTr("Avant et après sur un même tracé, ou une bande chacun")
                onClicked: timePlot.stacked = !timePlot.stacked
            }
            RibbonButton {
                glyph: "cursors"
                text: qsTr("Curseurs")
                checked: timePlot.cursorsVisible
                enabled: timePlot.hasData
                tip: qsTr("Curseurs A et B sur l'aperçu (Δt, valeurs avant et après) et sur les spectres (Δf, écart de niveau)")
                onClicked: {
                    const on = !timePlot.cursorsVisible
                    timePlot.cursorsVisible = on
                    spectrum.cursorsVisible = on
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
                tip: qsTr("20 log₁₀ de l'amplitude : l'atténuation se lit en dB entre « Avant » et « Après »")
                onClicked: root.analysis.setSettings({ scale: root.decibel ? "linear" : "db" })
            }
            RibbonButton {
                glyph: "logscale"
                text: qsTr("Log")
                checked: root.settings.logFrequency === true
                tip: qsTr("Échelle logarithmique des fréquences : chaque décade a la même largeur")
                onClicked: root.analysis.setSettings({ logFrequency: !(root.settings.logFrequency === true) })
            }
        }
        ImageActions {
            visible: root.ribbonTab === 1
            target: split
            baseName: "signal-filtre"
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

        // What the filter is, and what it is tried on.
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
                // What the filter is: it keeps its room (most of the row); the rate, then the signal tried,
                // give way first.
                Label {
                    readonly property real room: Math.max(120, root.width * (root.ribbonTab === 1 ? 0.45 : 0.7))
                    text: root.designed ? (root.design.text ?? "") : (root.design.error ?? "")
                    color: root.designed ? Theme.text : Theme.error
                    font.pixelSize: Theme.bodyText
                    font.bold: root.designed
                    elide: Text.ElideRight
                    Layout.maximumWidth: room
                    Layout.minimumWidth: Math.min(implicitWidth, room)
                }
                Label {
                    visible: root.designed
                    text: qsTr("fs = %1 · appliqué %2").arg(root.rateText)
                          .arg((Choices.filterApplications.find((x) => x.key === root.recipe.application) ?? {}).short ?? "")
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                }
                Item {
                    visible: !root.designed
                    Layout.fillWidth: true
                }
                // What the design says: sections, stability, delays; and what it warns about.
                Label {
                    visible: root.designed
                    text: (root.design.warnings ?? []).length > 0 ? "⚠" : "?"
                    font.pixelSize: Theme.bodyText
                    font.bold: true
                    color: (root.design.warnings ?? []).length > 0 ? Theme.warning
                           : designInfoArea.containsMouse ? Theme.text : Theme.textMuted
                    MouseArea {
                        id: designInfoArea
                        anchors.fill: parent
                        anchors.margins: -4
                        hoverEnabled: true
                        onClicked: root.designRequested()
                    }
                    ToolTip.visible: designInfoArea.containsMouse
                    ToolTip.text: (root.design.warnings ?? []).concat(root.design.summary ?? [])
                                  .concat([qsTr("Le détail et les coefficients : panneau Filtre (clic).")]).join("\n")
                }
                Rectangle {
                    visible: root.ribbonTab === 1
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 18
                    color: Theme.border
                }
                Label {
                    visible: root.ribbonTab === 1
                    text: root.hasInput ? qsTr("Essayé sur « %1 », %2").arg(root.input.name).arg(root.channelName)
                                        : qsTr("Choisissez un signal dans la liste")
                    color: root.hasInput ? Theme.text : Theme.textMuted
                    font.pixelSize: Theme.smallText
                    elide: Text.ElideMiddle
                    Layout.maximumWidth: Math.max(120, root.width * 0.35)
                    Layout.minimumWidth: Math.min(implicitWidth, 100)
                }
                BusyIndicator {
                    visible: root.showBusy
                    running: visible
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 18
                }
                Label {
                    visible: root.showBusy && root.filters.busy && root.filters.progress > 0
                    text: Math.round(100 * root.filters.progress) + " %"
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
            }
        }

        // ---- Concevoir: the responses ----
        Item {
            id: responseArea
            visible: root.ribbonTab === 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            ResponsePlot {
                id: responsePlot
                anchors.fill: parent
                filter: root.filters
                backgroundColor: Theme.canvas
                gridColor: Theme.grid
                textColor: Theme.text
                mutedColor: Theme.textMuted
                accentColor: Theme.accent
                appliedColor: Theme.warning
                warningColor: Theme.error
                panelColor: Theme.panel
                emptyText: qsTr("Aucun filtre")
            }
            // The blank page: what to do with it.
            Rectangle {
                visible: root.none
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 44
                width: Math.min(parent.width - 140, 560)
                height: noneHint.implicitHeight + 24
                radius: 8
                color: Qt.alpha(Theme.panel, 0.94)
                border.color: Theme.border
                Label {
                    id: noneHint
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 12
                    wrapMode: Text.WordWrap
                    color: Theme.text
                    font.pixelSize: Theme.smallText
                    lineHeight: 1.15
                    text: qsTr("Page vierge : aucun filtre, la sortie est l'entrée (gain plat à 0 dB).")
                          + "\n• " + qsTr("Aucun ▾ (ruban ▸ Filtre) : un RII (Butterworth, Chebyshev, Bessel, elliptique), un RIF ou des coefficients tapés ; le premier part d'un passe-bas à %1 ;").arg(Units.compact(root.recipe.frequency1 ?? 1000, "Hz", 4))
                          + "\n• " + qsTr("Exemples ▾ : un filtre tout prêt, ou celui de la dernière fois.")
                }
            }
        }

        // ---- Appliquer: before and after ----
        SplitView {
            id: split
            visible: root.ribbonTab === 1
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Vertical
            handle: Rectangle {
                implicitHeight: 5
                color: SplitHandle.hovered || SplitHandle.pressed ? Qt.alpha(Theme.accent, 0.5) : Theme.border
            }

            Item {
                SplitView.fillHeight: true
                SplitView.minimumHeight: 120
                SignalPlot {
                    id: timePlot
                    anchors.fill: parent
                    // Under the note when there is one, so that it hides neither the names of the curves nor the top
                    // of the curves.
                    anchors.topMargin: noteBox.visible ? noteBox.height + 12 : 0
                    anchors.bottomMargin: timeBar.height
                    session: root.filters.preview
                    signalId: root.filters.previewId
                    stacked: false
                    backgroundColor: Theme.canvas
                    gridColor: Theme.grid
                    textColor: Theme.text
                    mutedColor: Theme.textMuted
                    accentColor: Theme.accent
                    panelColor: Theme.panel
                    channelColors: Theme.channelColors
                    emptyText: !root.hasInput ? qsTr("Choisissez un signal dans la liste de gauche : le filtre s'y applique ici, avant et après.")
                               : root.filters.previewError.length > 0 ? root.filters.previewError
                               : qsTr("Filtrage de l'aperçu…")
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
                ColumnLayout {
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.topMargin: 8
                    anchors.rightMargin: 18
                    spacing: 6
                    PlotToolbar {
                        id: timeToolbar
                        Layout.alignment: Qt.AlignRight
                        view: timePlot
                    }
                }
                // What the preview says: only the start of a long signal, holes, a rate that is not the signal's. Above
                // the plot, beside its strip (the chips "Avant" and "Après"), on two lines at most (all of it under
                // the pointer).
                Rectangle {
                    id: noteBox
                    readonly property var lines: root.filters.previewWarnings.concat(root.filters.previewNote.length > 0 ? [root.filters.previewNote] : [])
                    visible: lines.length > 0 && timePlot.hasData
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.leftMargin: 70
                    anchors.topMargin: 8
                    width: Math.max(120, Math.min(noteLabel.implicitWidth + 20,
                                                  parent.width - anchors.leftMargin - (timeToolbar.visible ? timeToolbar.width + 12 : 0) - 18))
                    height: noteLabel.implicitHeight + 10
                    radius: 6
                    color: Qt.alpha(Theme.panel, 0.94)
                    border.color: root.filters.previewWarnings.length > 0 ? Theme.warning : Theme.border
                    Label {
                        id: noteLabel
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        verticalAlignment: Text.AlignVCenter
                        wrapMode: Text.WordWrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        text: (root.filters.previewWarnings.length > 0 ? "⚠ " : "") + (parent.lines[0] ?? "")
                              + (parent.lines.length > 1 ? qsTr(" (et %1 autres)").arg(parent.lines.length - 1) : "")
                        color: root.filters.previewWarnings.length > 0 ? Theme.warning : Theme.textMuted
                        font.pixelSize: Theme.smallText
                    }
                    HoverHandler { id: noteHover }
                    ToolTip.visible: noteHover.hovered
                    ToolTip.delay: 300
                    ToolTip.text: lines.join("\n")
                }
            }

            // The spectra before and after.
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
                               : root.filters.hasPreview ? qsTr("Calcul des spectres…") : ""
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
                            text: qsTr("Spectres avant et après")
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
                root.analysis.setSettings({ logFrequency: on })
            else if (key === "db")
                root.analysis.setSettings({ scale: on ? "db" : "linear" })
        }
    }
    PlotMenu {
        id: signalMenu
        view: timePlot
        mode: "channels"
        actions: [{ key: "add", text: qsTr("Ajouter le signal filtré à la session") }]
        onActionTriggered: (key) => { if (key === "add") root.addToSession() }
    }
    PlotMenu {
        id: spectrumMenu
        view: spectrum
        mode: "plain"
        actions: [{ key: "add", text: qsTr("Ajouter le signal filtré à la session") }]
        onActionTriggered: (key) => { if (key === "add") root.addToSession() }
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
        toolTip: qsTr("Afficher le filtre et les mesures (F10)")
        onClicked: root.unfoldRight()
    }

    // The filter in C for a microcontroller.
    FilterCodeDialog {
        id: codeDialog
        filters: root.filters
        appSettings: root.appSettings
        onDone: (message) => root.infoMessage(message)
        onFailed: (message) => root.errorMessage(message)
    }
}
