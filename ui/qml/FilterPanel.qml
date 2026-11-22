import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// Tab "Filtre" of the right panel of the module "Filtres": the filter in detail. Its type, response, family
// or design (a window, or equiripple), frequencies, order or number of coefficients, ripple, attenuation and
// transition where they count, or a template (gabarit: the edges of its bands, Amax and Amin) whose least order
// is computed, or its coefficients b and a typed (numbers, formulas of k, variables, and what
// they evaluate to); the gain at its output; the rate it is designed for (that of the signal
// chosen, or one typed); how it is applied; what it does (sections, stability, delays, what it reached, what
// it warns about) and its coefficients, to copy. Every field writes into the recipe at once, and the plots
// follow.
ScrollView {
    id: root
    required property var filters

    readonly property var recipe: filters.recipe
    readonly property var design: filters.design
    readonly property bool designed: design.ok === true
    // Designed (IIR of a family, FIR by a window or equiripple), or coefficients typed (b and a).
    readonly property bool typed: recipe.kind === "typed"
    readonly property bool iir: recipe.kind === "iir"
    readonly property bool fir: recipe.kind === "fir"
    readonly property bool band: !typed && (recipe.response === "bandpass" || recipe.response === "bandstop")
    // A FIR: by a window (its key), or equiripple (Parks-McClellan).
    readonly property bool equiripple: fir && recipe.firDesign === "equiripple"
    readonly property string firDesign: equiripple ? "equiripple" : (recipe.window ?? "hamming")
    // What the template asks for: a ripple (Chebyshev I, elliptic, equiripple), an attenuation (Chebyshev II,
    // elliptic, equiripple), the width of the transitions (equiripple).
    readonly property bool hasRipple: (iir && (recipe.family === "chebyshev1" || recipe.family === "elliptic")) || equiripple
    readonly property bool hasAttenuation: (iir && (recipe.family === "chebyshev2" || recipe.family === "elliptic")) || equiripple
    // The least order (taps) that holds a template, computed; the recipe as designed has it.
    readonly property bool fromTemplate: !typed && recipe.fromTemplate === true
    readonly property var designedRecipe: design.recipe ?? recipe
    // The edges of the template in their order along the frequencies, as its response has them.
    readonly property var templateEdges: {
        const edge = (key, label) => ({ key: key, label: label })
        switch (recipe.response) {
        case "highpass": return [edge("stop1", qsTr("Coupe jusqu'à fa")), edge("pass1", qsTr("Passe dès fp"))]
        case "bandpass": return [edge("stop1", qsTr("Coupe jusqu'à fa₁")), edge("pass1", qsTr("Passe dès fp₁")),
                                 edge("pass2", qsTr("Passe jusqu'à fp₂")), edge("stop2", qsTr("Coupe dès fa₂"))]
        case "bandstop": return [edge("pass1", qsTr("Passe jusqu'à fp₁")), edge("stop1", qsTr("Coupe dès fa₁")),
                                 edge("stop2", qsTr("Coupe jusqu'à fa₂")), edge("pass2", qsTr("Passe dès fp₂"))]
        default: return [edge("pass1", qsTr("Passe jusqu'à fp")), edge("stop1", qsTr("Coupe dès fa"))]
        }
    }
    // What was computed from it: "ordre 6", "238 coefficients", "112 coefficients, β = 3,395".
    readonly property string computedText: !designed ? ""
        : iir ? qsTr("ordre %1").arg(designedRecipe.order ?? 0)
        : qsTr("%1 coefficients").arg(Units.compact(designedRecipe.taps ?? 0, "", 6))
          + (!equiripple && recipe.window === "kaiser" ? qsTr(", β = %1").arg(Units.compact(designedRecipe.kaiserBeta ?? 0, "", 4)) : "")
    readonly property var input: filters.input
    readonly property bool hasInput: (input.name ?? "").length > 0
    // Aligned: a FIR only (coefficients typed: when a = 1).
    readonly property bool canAlign: fir || (typed && design.fir === true)
    readonly property var applications: canAlign ? Choices.filterApplications
                                                 : Choices.filterApplications.filter((a) => a.key !== "aligned")
    // Its coefficients as sections (an IIR; coefficients typed with a ≠ 1: b and a first), or as taps.
    readonly property bool sectionsShown: design.fir !== true
    // The first values of a list typed, as evaluated: "= 0,125 ; 0,125 ; … (8)".
    function evaluated(values, count) {
        if (!values || values.length === 0)
            return ""
        const shown = values.slice(0, 12).map((v) => Units.compact(v, "", 6))
        return "= " + shown.join(" ; ") + (count > shown.length ? qsTr(" ; … (%1 en tout)").arg(count) : "")
    }

    contentWidth: availableWidth
    clip: true

    function set(values) {
        filters.setRecipe(values)
    }
    function copied() {
        copiedLabel.visible = true
        copiedTimer.restart()
    }

    component Hint: Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textMuted
        font.pixelSize: Theme.smallText
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: 10

        // ---- The filter ----
        SectionTitle { text: qsTr("Filtre") }
        ChoiceField {
            label: qsTr("Type")
            choices: Choices.names(Choices.filterKinds)
            value: Choices.indexOf(Choices.filterKinds, root.recipe.kind ?? "iir")
            onEdited: (index) => root.set({ kind: Choices.filterKinds[index].key })
        }
        // ---- Coefficients typed ----
        ColumnLayout {
            visible: root.typed
            Layout.fillWidth: true
            spacing: 6
            Hint {
                text: qsTr("y[n] = b[0] x[n] + b[1] x[n − 1] + … − a[1] y[n − 1] − …, a[0] divisant tout : H(z) = Σ b[k] z⁻ᵏ / Σ a[k] z⁻ᵏ, comme lfilter(b, a) de SciPy. Un RIF si a = 1.")
            }
            FormulaField {
                label: qsTr("b : numérateur (les x[n − k])")
                value: root.recipe.b ?? "1"
                placeholder: qsTr("1 ; -2 ; 1   ou une formule de k")
                lineSeparator: " ; "
                tip: qsTr("Des nombres ou des formules séparés par « ; » (ou « , » suivie d'une espace, ou un par ligne) ; une liste de SciPy ou de MATLAB se colle telle quelle")
                onEdited: (text) => root.set({ b: text })
            }
            FormulaField {
                visible: root.design.numeratorOfK === true || (root.recipe.bCount ?? "").length > 0
                label: qsTr("Nombre de b (k de 0 à ce nombre − 1)")
                value: root.recipe.bCount ?? ""
                placeholder: qsTr("8, ou N")
                tip: qsTr("Une seule formule de k donne autant de coefficients : un nombre, ou une formule des variables")
                onEdited: (text) => root.set({ bCount: text })
            }
            Label {
                visible: text.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                text: root.evaluated(root.design.typedB, root.design.typedBCount ?? 0)
                color: Theme.textMuted
                font.family: Theme.monospace
                font.pixelSize: Theme.smallText
            }
            FormulaField {
                label: qsTr("a : dénominateur (les y[n − k])")
                value: root.recipe.a ?? "1"
                placeholder: qsTr("1 ; -0,9   (1 pour un RIF)")
                lineSeparator: " ; "
                tip: qsTr("a[0] multiplie y[n] : tous les coefficients en sont divisés. « 1 » seul : un RIF")
                onEdited: (text) => root.set({ a: text })
            }
            FormulaField {
                visible: root.design.denominatorOfK === true || (root.recipe.aCount ?? "").length > 0
                label: qsTr("Nombre de a (k de 0 à ce nombre − 1)")
                value: root.recipe.aCount ?? ""
                placeholder: qsTr("3, ou N")
                tip: qsTr("Une seule formule de k donne autant de coefficients : un nombre, ou une formule des variables")
                onEdited: (text) => root.set({ aCount: text })
            }
            Label {
                visible: text.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                text: root.evaluated(root.design.typedA, root.design.typedACount ?? 0)
                color: Theme.textMuted
                font.family: Theme.monospace
                font.pixelSize: Theme.smallText
            }
            FormulaField {
                label: qsTr("Variables")
                value: root.recipe.variables ?? ""
                placeholder: qsTr("N = 8 ; w0 = 2 pi 50/fs")
                lineSeparator: " ; "
                tip: qsTr("« nom = valeur », séparées par « ; » ou une par ligne ; chacune peut utiliser fs, Te et celles d'avant")
                onEdited: (text) => root.set({ variables: text })
            }
            Hint {
                text: qsTr("Les formules connaissent k (le rang, de 0), fs, Te = 1/fs, pi, e, les variables, et sin, cos, exp, sqrt, abs, if(…; …; …)… Une seule formule de k donne « Nombre de » coefficients : b = 1/N avec N coefficients, une moyenne glissante.")
            }
        }
        ChoiceField {
            visible: !root.typed
            label: qsTr("Réponse")
            choices: Choices.names(Choices.filterResponses)
            value: Choices.indexOf(Choices.filterResponses, root.recipe.response ?? "lowpass")
            onEdited: (index) => root.set({ response: Choices.filterResponses[index].key })
        }
        ChoiceField {
            visible: root.iir
            label: qsTr("Famille")
            choices: Choices.names(Choices.filterFamilies)
            value: Choices.indexOf(Choices.filterFamilies, root.recipe.family ?? "butterworth")
            onEdited: (index) => root.set({ family: Choices.filterFamilies[index].key })
        }
        Hint {
            visible: root.iir
            text: (Choices.filterFamilies.find((f) => f.key === root.recipe.family) ?? {}).tip ?? ""
        }
        ChoiceField {
            visible: root.fir
            label: qsTr("Conception")
            choices: Choices.names(Choices.firDesigns)
            value: Choices.indexOf(Choices.firDesigns, root.firDesign)
            onEdited: (index) => {
                const key = Choices.firDesigns[index].key
                if (key === "equiripple")
                    root.set({ firDesign: "equiripple" })
                else
                    root.set({ firDesign: "window", window: key })
            }
        }
        Hint {
            visible: root.fir
            text: (root.equiripple ? "" : qsTr("Par une fenêtre : "))
                  + ((Choices.firDesigns.find((w) => w.key === root.firDesign) ?? {}).tip ?? "")
        }
        // ---- The order chosen, or the least that holds a template ----
        CheckBox {
            visible: !root.typed
            text: qsTr("Ordre calculé d'après un gabarit")
            checked: root.fromTemplate
            onToggled: root.set({ fromTemplate: checked })
        }
        ColumnLayout {
            visible: root.fromTemplate
            Layout.fillWidth: true
            spacing: 6
            Hint {
                text: root.fir ? qsTr("Jusqu'où il passe (au plus Amax d'ondulation, d'un extrême à l'autre), d'où il coupe (au moins Amin d'atténuation) : le nombre de coefficients le plus petit qui le tient est cherché pour la conception choisie (fenêtre : les coupures au milieu des transitions).")
                               : qsTr("Jusqu'où il passe (au plus Amax d'atténuation), d'où il coupe (au moins Amin) : l'ordre le plus petit qui le tient est calculé pour la famille choisie, comme buttord, cheb1ord, cheb2ord et ellipord de SciPy ; −Amax tombe juste sur les bords de la bande passante.")
            }
            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 8
                rowSpacing: 6
                Repeater {
                    model: root.fromTemplate ? root.templateEdges.length : 0
                    delegate: QuantityField {
                        required property int index
                        readonly property var edge: root.templateEdges[index] ?? ({ key: "pass1", label: "" })
                        Layout.fillWidth: true
                        label: edge.label
                        unit: "Hz"
                        minimum: 0
                        maximum: 1e12
                        value: root.recipe[edge.key] ?? 0
                        onEdited: (v) => {
                            const change = {}
                            change[edge.key] = v
                            root.set(change)
                        }
                    }
                }
                QuantityField {
                    Layout.fillWidth: true
                    label: qsTr("Amax, bande passante")
                    unit: "dB"
                    minimum: 0.001
                    maximum: 40
                    value: root.recipe.ripple ?? 1
                    tip: root.fir ? qsTr("L'ondulation permise au plus, d'un extrême à l'autre") : qsTr("Le gain y reste entre 0 et −Amax dB")
                    onEdited: (v) => root.set({ ripple: v })
                }
                QuantityField {
                    Layout.fillWidth: true
                    label: qsTr("Amin, bande coupée")
                    unit: "dB"
                    minimum: 1
                    maximum: 300
                    value: root.recipe.attenuation ?? 40
                    tip: qsTr("L'atténuation exigée au moins")
                    onEdited: (v) => root.set({ attenuation: v })
                }
            }
            Label {
                visible: root.computedText.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Calculé : %1%2").arg(root.computedText)
                      .arg(root.design.templateHeld === true ? qsTr(", le gabarit tenu") : qsTr(", le gabarit pas tout à fait tenu"))
                color: root.design.templateHeld === true ? Theme.text : Theme.warning
                font.pixelSize: Theme.bodyText
            }
        }
        RowLayout {
            visible: !root.typed && !root.fromTemplate
            Layout.fillWidth: true
            spacing: 8
            QuantityField {
                label: root.band ? qsTr("Fréquence basse") : qsTr("Fréquence de coupure")
                unit: "Hz"
                minimum: 0
                maximum: 1e12
                value: root.recipe.frequency1 ?? 1000
                onEdited: (v) => root.set({ frequency1: v })
            }
            QuantityField {
                visible: root.band
                label: qsTr("Fréquence haute")
                unit: "Hz"
                minimum: 0
                maximum: 1e12
                value: root.recipe.frequency2 ?? 2000
                onEdited: (v) => root.set({ frequency2: v })
            }
        }
        RowLayout {
            visible: !root.typed && !root.fromTemplate
            Layout.fillWidth: true
            spacing: 8
            QuantityField {
                visible: root.iir
                label: root.band ? qsTr("Ordre (du prototype)") : qsTr("Ordre")
                minimum: 1
                maximum: 32
                value: root.recipe.order ?? 4
                tip: qsTr("De 1 à 32 ; une bande a deux fois plus de pôles que l'ordre")
                onEdited: (v) => root.set({ order: Math.round(v) })
            }
            QuantityField {
                visible: root.fir
                label: qsTr("Coefficients N")
                minimum: 3
                maximum: root.equiripple ? 4095 : 65535
                value: root.recipe.taps ?? 101
                tip: root.equiripple ? qsTr("De 3 à 4 095 : retard de (N − 1)/2 échantillons ; impair pour un passe-haut ou un coupe-bande")
                                     : qsTr("De 3 à 65 535 : retard de (N − 1)/2 échantillons ; impair pour un passe-haut ou un coupe-bande")
                onEdited: (v) => root.set({ taps: Math.round(v) })
            }
            QuantityField {
                visible: root.fir && !root.equiripple && root.recipe.window === "kaiser"
                label: "β (Kaiser)"
                minimum: 0
                maximum: 50
                value: root.recipe.kaiserBeta ?? 8.6
                onEdited: (v) => root.set({ kaiserBeta: v })
            }
        }
        // The template: what it may ripple, what it must attenuate, how wide its transitions are.
        RowLayout {
            visible: (root.hasRipple || root.hasAttenuation) && !root.fromTemplate
            Layout.fillWidth: true
            spacing: 8
            QuantityField {
                visible: root.hasRipple
                label: root.equiripple ? qsTr("Ondulation (crête à crête)") : qsTr("Ondulation")
                unit: "dB"
                minimum: 0.001
                maximum: 40
                value: root.recipe.ripple ?? 1
                tip: root.equiripple ? qsTr("L'écart permis dans la bande passante, d'un extrême à l'autre")
                                     : qsTr("Le gain dans la bande passante reste entre 0 et −R dB")
                onEdited: (v) => root.set({ ripple: v })
            }
            QuantityField {
                visible: root.hasAttenuation
                label: qsTr("Atténuation")
                unit: "dB"
                minimum: 1
                maximum: 300
                value: root.recipe.attenuation ?? 40
                tip: root.equiripple ? qsTr("Ce que la bande coupée doit atténuer au moins")
                                     : qsTr("L'atténuation garantie dans la bande coupée")
                onEdited: (v) => root.set({ attenuation: v })
            }
            QuantityField {
                visible: root.equiripple
                label: qsTr("Transition")
                unit: "Hz"
                minimum: 1e-6
                maximum: 1e12
                value: root.recipe.transition ?? 200
                tip: qsTr("La largeur de chaque transition, centrée sur sa fréquence : la bande passante et la bande coupée s'arrêtent à une demi-transition de part et d'autre")
                onEdited: (v) => root.set({ transition: v })
            }
        }
        Hint {
            visible: root.equiripple && !root.fromTemplate
            text: qsTr("Les poids des bandes sont tirés de l'ondulation et de l'atténuation ; ce qui est atteint est dit plus bas (« Ce qu'il fait »). Pour tenir le gabarit : plus de coefficients ou des transitions plus larges.")
        }
        Hint {
            visible: root.iir && root.recipe.family === "elliptic" && !root.fromTemplate
            text: qsTr("L'atténuation est tenue à partir d'une fréquence d'arrêt calculée (dite plus bas) : plus l'ordre est grand, plus elle est proche de la coupure.")
        }
        QuantityField {
            label: qsTr("Gain en sortie")
            unit: "dB"
            minimum: -120
            maximum: 120
            value: root.recipe.gain ?? 0
            tip: qsTr("De −120 à +120 dB")
            onEdited: (v) => root.set({ gain: v })
        }
        Hint {
            text: (root.recipe.gain ?? 0) === 0
                  ? qsTr("0 dB : le filtre seul. +6 dB double le signal filtré, −6 dB le divise par deux : une fois, quelle que soit l'application (à phase nulle aussi, où le reste du filtre compte deux fois).")
                  : qsTr("×%1 : le signal filtré est multiplié par ce facteur, une fois, quelle que soit l'application. Les réponses le montrent ; les coefficients restent ceux du filtre à 0 dB, g en tête.")
                    .arg(Units.compact(Math.pow(10, (root.recipe.gain ?? 0) / 20), "", 4))
        }

        // ---- The rate ----
        SectionTitle { text: qsTr("Échantillonnage") }
        CheckBox {
            text: root.hasInput ? qsTr("Celle du signal choisi (%1)").arg(Units.compact(root.input.sampleRate ?? 0, "Hz", 6))
                                : qsTr("Celle du signal choisi dans la liste")
            checked: root.recipe.rateOfSignal === true
            onToggled: root.set({ rateOfSignal: checked })
        }
        QuantityField {
            label: qsTr("Fréquence d'échantillonnage fs")
            unit: "Hz"
            minimum: 1e-6
            maximum: 1e12
            value: root.recipe.sampleRate ?? 48000
            readOnly: root.recipe.rateOfSignal === true && root.hasInput
            onEdited: (v) => root.set({ sampleRate: v, rateOfSignal: false })
        }
        Hint {
            text: qsTr("Un filtre numérique est conçu pour une fréquence d'échantillonnage : appliqué à un signal échantillonné autrement, il ne fait pas ce que montrent ses réponses (il est refusé).")
        }

        // ---- How it is applied ----
        SectionTitle { text: qsTr("Application") }
        ChoiceField {
            choices: Choices.names(root.applications)
            value: Choices.indexOf(root.applications, root.recipe.application ?? "causal")
            onEdited: (index) => root.set({ application: root.applications[index].key })
        }
        Hint {
            text: ((root.applications.find((a) => a.key === root.recipe.application) ?? {}).tip ?? "")
                  + (!root.canAlign ? qsTr(" (« Alignée » : pour un RIF seulement.)") : "")
        }
        // At zero phase: how the record is prolonged beyond its ends, for the filter to start there as if it went on.
        ChoiceField {
            visible: root.recipe.application === "zerophase"
            label: qsTr("Bords prolongés")
            choices: Choices.names(Choices.edgeExtensions)
            value: Choices.indexOf(Choices.edgeExtensions, root.recipe.edges ?? "predicted")
            onEdited: (index) => root.set({ edges: Choices.edgeExtensions[index].key })
        }
        Hint {
            visible: root.recipe.application === "zerophase"
            text: ((Choices.edgeExtensions.find((e) => e.key === (root.recipe.edges ?? "predicted")) ?? {}).tip ?? "")
                  + qsTr(". Prolongés le temps que le filtre sonne : au-delà de ses bords, l'enregistrement ne dit pas ce que faisait le signal, le prolongement le suppose.")
        }

        // ---- What it does ----
        SectionTitle { text: qsTr("Ce qu'il fait") }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.designed ? (root.design.text ?? "") : (root.design.error ?? "")
            color: root.designed ? Theme.text : Theme.error
            font.pixelSize: Theme.bodyText
            font.bold: root.designed
        }
        Repeater {
            model: root.designed ? (root.design.warnings ?? []) : []
            delegate: Label {
                required property string modelData
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: "⚠ " + modelData
                color: Theme.warning
                font.pixelSize: Theme.smallText
            }
        }
        Repeater {
            model: root.designed ? (root.design.summary ?? []) : []
            delegate: Label {
                required property string modelData
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: modelData
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
            }
        }

        // ---- Its coefficients ----
        SectionTitle {
            visible: root.designed
            text: !root.sectionsShown ? qsTr("Coefficients h[n]")
                  : root.typed ? qsTr("Coefficients (b et a, puis en sections)") : qsTr("Coefficients (sections du second ordre)")
        }
        Hint {
            visible: root.designed
            text: (!root.sectionsShown ? qsTr("y[n] = Σ h[k]·x[n − k], de k = 0 à N − 1.")
                   : (root.typed ? qsTr("b et a tels qu'appliqués (divisés par a[0]), puis les sections du second ordre qu'ils font, plus justes en calcul. ") : "")
                     + qsTr("Chaque section, avec a0 = 1 : y[n] = b0·x[n] + b1·x[n−1] + b2·x[n−2] − a1·y[n−1] − a2·y[n−2] ; la sortie d'une section entre dans la suivante."))
                  + ((root.recipe.gain ?? 0) !== 0 ? qsTr(" Puis × g, le gain en sortie.") : "")
        }
        Rectangle {
            visible: root.designed
            Layout.fillWidth: true
            Layout.preferredHeight: 190
            radius: 4
            color: Theme.panelAlt
            border.color: Theme.border
            ScrollView {
                anchors.fill: parent
                anchors.margins: 8
                clip: true
                TextArea {
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.WrapAnywhere
                    padding: 0
                    background: null
                    font.family: Theme.monospace
                    font.pixelSize: Theme.smallText
                    color: Theme.text
                    text: root.design.coefficients ?? ""
                }
            }
        }
        RowLayout {
            visible: root.designed
            Layout.fillWidth: true
            spacing: 8
            Button {
                text: qsTr("Copier les coefficients")
                flat: true
                onClicked: {
                    if (ImageTools.copyText(root.filters.coefficientsText()))
                        root.copied()
                }
            }
            Label {
                id: copiedLabel
                visible: false
                text: qsTr("Copiés")
                color: Theme.success
                font.pixelSize: Theme.smallText
            }
            Item { Layout.fillWidth: true }
        }
        Timer {
            id: copiedTimer
            interval: 1500
            onTriggered: copiedLabel.visible = false
        }
        Item { Layout.preferredHeight: 8 }
    }
}
