import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// One waveform of a channel of the generator, in the panel Composition: whether it counts, its kind, and
// the values that kind takes (amplitude, frequency, phase, duty cycle, instant, distribution of a noise,
// ends and law of a sweep, Fourier series). Each field writes into the recipe at once. A Fourier series
// has its fundamental, its number of harmonics, the form of its coefficients, its variables and its three
// formulas, models of coefficients, and the table of its first coefficients. A drawn motif has its editor
// (MotifEditor: points placed, moved, removed, strokes drawn), how its points are joined, whether it repeats,
// ready-made motifs, and its points as text. A recurrence has its input, its variables, the formula of y[n],
// its initial values, ready-made recurrences and its first values. An operation between channels has its
// variables, its formula y of v1, v2…, t and x, ready-made operations, and the channels it reads by name. A
// modulated carrier has its amplitude, frequency and phase, how it is modulated (AM, FM, PM, double or single
// sideband) and its index or excursion, its variables, its message, ready-made messages. A recorded signal has
// its signal and channel among those of the session, its level (a gain, or a peak), where it is read from, its
// delay, whether it repeats, and a button that gives the recipe its rate and duration. A line of a digital frame
// has its own fields (FrameFields).
Rectangle {
    id: root
    required property var generator
    property int channel: 0
    required property int index // in the channel (the Repeater gives it)
    property int count: 1
    property var wave: ({}) // the component, as GeneratorController.recipe gives it
    property string unit: ""
    readonly property string waveform: wave.waveform ?? "sine"
    // (Comparisons rather than [..].indexOf: the QML compiler turns indexOf into C++ that converts
    // qsizetype to int, a -Wconversion warning of the build.)
    readonly property bool series: waveform === "square" || waveform === "triangle" || waveform === "sawtooth"
                                   || waveform === "pulse"
    readonly property bool periodic: series || waveform === "sine"
    readonly property bool takesDuty: waveform === "square" || waveform === "pulse"
    readonly property bool timed: waveform === "step" || waveform === "impulse"
    readonly property bool noise: waveform === "noise"
    readonly property bool sweep: waveform === "sweep"
    readonly property bool counted: wave.enabled !== false
    readonly property bool fourier: wave.bandLimit === "fourier"
    readonly property bool fourierSeries: waveform === "fourier"
    readonly property bool drawn: waveform === "drawn"
    readonly property bool drawnRepeated: drawn && wave.once !== true
    // Its points as text (again at each change of the recipe), and what was wrong in the last ones typed.
    readonly property string motifText: drawn ? (generator.recipe, generator.motifText(channel, index)) : ""
    property string motifError: ""
    onMotifTextChanged: motifError = ""
    readonly property bool operation: waveform === "formula"
    readonly property bool modulated: waveform === "modulation"
    readonly property string modulationKind: wave.modulation ?? "am"
    // What is wrong in an operation or a message (at once), what it reads (again at each change of the recipe).
    readonly property var operationState: operation || modulated ? (generator.recipe, generator.operationInfo(channel, index)) : ({})
    readonly property string operationError: operationState.error ?? ""
    readonly property string operationCaution: operationState.caution ?? ""
    readonly property string operationNote: operationState.note ?? ""
    readonly property var operationChoices: operation || modulated
                                            ? (generator.recipe, operation ? generator.operationModels(channel) : generator.messageModels(channel))
                                            : []
    readonly property bool recorded: waveform === "recorded"
    // The record it reads among the signals of the session (again when the recipe or the session change).
    readonly property var sourceState: recorded ? (generator.recipe, generator.sources, generator.sourceInfo(channel, index)) : ({})
    readonly property string sourceError: sourceState.error ?? ""
    readonly property bool levelled: wave.normalize === true
    readonly property bool framed: waveform === "frame"
    readonly property bool recurrent: waveform === "recurrence"
    // What is wrong in it (at once), and its first values as the preview made them.
    readonly property var trace: recurrent ? (generator.recipe, generator.ready, generator.recurrenceInfo(channel, index)) : ({})
    readonly property string traceError: trace.error ?? ""
    readonly property string traceNote: trace.note ?? ""
    readonly property var traceRows: trace.rows ?? []
    readonly property int formIndex: wave.form === "cartesian" ? 1 : wave.form === "polar" ? 2 : 0
    readonly property var terms: Choices.seriesForms[formIndex].terms
    // Its first coefficients, or what is wrong in its formulas (again at each change of the recipe).
    readonly property var info: fourierSeries ? (generator.recipe, generator.seriesInfo(channel, index)) : ({})
    readonly property string infoError: info.error ?? ""
    readonly property var rows: info.rows ?? []

    function set(values) {
        generator.setComponent(channel, index, values)
    }
    // Where the record read is in the list of the session's signals (-1: not there).
    function sourceIndex() {
        const list = generator.sources
        const id = sourceState.id ?? 0
        for (let i = 0; i < list.length; ++i) {
            if (list[i].id === id)
                return i
        }
        return -1
    }
    // The key of formula i of a series in the recipe (a₀, aₙ, bₙ or the parts of Cₙ).
    function formulaKey(i) {
        return i === 0 ? "a0" : i === 1 ? "an" : "bn"
    }

    Layout.fillWidth: true
    implicitHeight: column.implicitHeight + 16
    radius: Theme.radius
    color: Theme.panelAlt
    border.color: Theme.border

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 2
            CheckBox {
                checked: root.counted
                onToggled: root.set({ enabled: checked })
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: root.counted ? qsTr("Comptée dans la somme : décochez pour l'écarter sans la perdre")
                                           : qsTr("Écartée de la somme : cochez pour la compter")
            }
            ComboBox {
                Layout.fillWidth: true
                model: Choices.names(Choices.generatorWaveforms)
                currentIndex: Choices.indexOf(Choices.generatorWaveforms, root.waveform)
                font.pixelSize: Theme.bodyText
                onActivated: (i) => {
                    if (Choices.generatorWaveforms[i].key !== root.waveform)
                        root.set({ waveform: Choices.generatorWaveforms[i].key })
                }
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: Choices.generatorWaveforms[Choices.indexOf(Choices.generatorWaveforms, root.waveform)].tip
            }
            ToolButton {
                text: "↑"
                enabled: root.index > 0
                implicitWidth: 28
                onClicked: root.generator.moveComponent(root.channel, root.index, root.index - 1)
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Monter")
            }
            ToolButton {
                text: "↓"
                enabled: root.index < root.count - 1
                implicitWidth: 28
                onClicked: root.generator.moveComponent(root.channel, root.index, root.index + 1)
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Descendre")
            }
            ToolButton {
                text: "×"
                implicitWidth: 28
                onClicked: root.generator.removeComponent(root.channel, root.index)
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Retirer cette forme d'onde")
            }
        }

        // ---- A recorded signal: which one ----
        ColumnLayout {
            visible: root.recorded
            Layout.fillWidth: true
            opacity: root.counted ? 1 : 0.55
            spacing: 6
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                ChoiceField {
                    label: qsTr("Signal de la session")
                    choices: root.recorded ? root.generator.sources.map((s) => s.name) : []
                    value: root.recorded ? root.sourceIndex() : -1
                    onEdited: (i) => root.set({ source: root.generator.sources[i].id, sourceChannel: 0 })
                }
                ChoiceField {
                    Layout.fillWidth: false
                    Layout.preferredWidth: 130
                    visible: (root.sourceState.channels ?? []).length > 1
                    label: qsTr("Voie")
                    choices: root.sourceState.channels ?? []
                    value: root.sourceState.channel ?? 0
                    onEdited: (i) => root.set({ sourceChannel: i })
                }
            }
            Label {
                visible: root.sourceError.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.sourceError
                color: Theme.error
                font.pixelSize: Theme.smallText
            }
            RowLayout {
                visible: root.sourceError.length === 0
                Layout.fillWidth: true
                spacing: 6
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: (root.sourceState.summary ?? "")
                          + (root.sourceState.resampled === true ? qsTr(" — rééchantillonné à %1").arg(Units.compact(root.generator.recipe.sampleRate ?? 48000, "Hz", 6)) : "")
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
                Button {
                    visible: root.sourceState.differs === true
                    text: qsTr("Sa fréquence et sa durée")
                    flat: true
                    onClicked: root.generator.adoptSourceTiming(root.channel, root.index)
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Le signal généré prend la fréquence d'échantillonnage de cet enregistrement (plus de rééchantillonnage) et sa durée")
                }
            }
        }

        // ---- A line of a digital frame ----
        Loader {
            Layout.fillWidth: true
            visible: root.framed
            active: root.framed
            opacity: root.counted ? 1 : 0.55
            sourceComponent: FrameFields {
                generator: root.generator
                channel: root.channel
                index: root.index
                wave: root.wave
                unit: root.unit
            }
        }

        GridLayout {
            // (A recurrence, an operation or a frame has none of these fields: an empty layout would keep the room
            // of the ones before.)
            visible: !root.recurrent && !root.operation && !root.framed
            Layout.fillWidth: true
            opacity: root.counted ? 1 : 0.55
            columns: 2
            columnSpacing: 8
            rowSpacing: 6
            QuantityField {
                visible: !root.fourierSeries && !root.recurrent
                label: root.waveform === "dc" || root.waveform === "impulse" ? qsTr("Valeur")
                       : root.waveform === "step" ? qsTr("Hauteur")
                       : root.modulated ? qsTr("Amplitude de la porteuse")
                       : root.recorded ? (root.levelled ? qsTr("Crête ramenée à") : qsTr("Gain (×)"))
                       : root.noise ? (root.wave.noise === "uniform" ? qsTr("Crête (±)") : qsTr("Écart type"))
                       : qsTr("Amplitude crête")
                unit: root.recorded && !root.levelled ? "" : root.unit
                value: root.wave.amplitude ?? 1
                tip: root.noise ? qsTr("Gaussien : l'écart type, la valeur efficace du bruit ; uniforme : entre −A et +A")
                     : root.recorded ? (root.levelled ? qsTr("La plus grande valeur absolue de la partie lue devient cette valeur (1 : un message prêt pour une porteuse, dont l'indice m est alors le taux de modulation)")
                                                      : qsTr("L'enregistrement est multiplié par ce nombre : 1 le laisse tel quel, −1 l'inverse, 0,5 le divise par deux"))
                     : root.modulated ? qsTr("La porteuse A·cos(2π fc t + φ), avant sa modulation")
                     : root.drawn ? qsTr("Le motif va de −A (en bas du dessin) à +A (en haut)")
                     : root.periodic || root.sweep ? qsTr("Valeur crête : 1 V crête, c'est 0,707 V efficaces pour un sinus") : ""
                onEdited: (v) => root.set({ amplitude: v })
            }
            QuantityField {
                visible: root.periodic || root.sweep || root.fourierSeries || root.drawnRepeated || root.modulated
                label: root.sweep ? qsTr("De") : root.fourierSeries ? qsTr("Fondamentale f₀")
                       : root.modulated ? qsTr("Porteuse fc") : qsTr("Fréquence")
                unit: "Hz"
                minimum: 0
                value: root.wave.frequency ?? 1000
                tip: root.sweep ? qsTr("Fréquence au début du balayage")
                     : root.fourierSeries ? qsTr("Fréquence de l'harmonique 1 : l'harmonique n est à n f₀")
                     : root.drawn ? qsTr("Le motif se répète à cette fréquence : la largeur du dessin est une période")
                     : root.modulated ? qsTr("Fréquence de la porteuse : au-delà de fs/2, elle se replie")
                     : qsTr("Au-delà de fs/2, la fréquence se replie")
                onEdited: (v) => root.set({ frequency: v })
            }
            ColumnLayout {
                visible: root.fourierSeries
                Layout.fillWidth: true
                spacing: 2
                Label {
                    text: qsTr("Harmoniques (N)")
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
                SpinBox {
                    Layout.fillWidth: true
                    from: 0
                    to: 10000
                    editable: true
                    value: root.wave.harmonics ?? 15
                    textFromValue: (v) => v === 0 ? qsTr("jusqu'à fs/2") : String(v)
                    valueFromText: (text) => {
                        const v = parseInt(text.replace(/\s/g, ""))
                        return isNaN(v) ? 0 : v
                    }
                    onValueModified: root.set({ harmonics: value })
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Nombre d'harmoniques n = 1…N de la somme (0 : tous ceux sous fs/2)")
                }
            }
            QuantityField {
                visible: root.sweep
                label: qsTr("À")
                unit: "Hz"
                minimum: 0
                value: root.wave.endFrequency ?? 10000
                tip: qsTr("Fréquence à la fin du balayage, atteinte à la fin du signal")
                onEdited: (v) => root.set({ endFrequency: v })
            }
            ChoiceField {
                visible: root.sweep
                label: qsTr("Loi")
                choices: [qsTr("Linéaire"), qsTr("Logarithmique")]
                value: root.wave.law === "log" ? 1 : 0
                onEdited: (i) => root.set({ law: i === 1 ? "log" : "linear" })
            }
            QuantityField {
                visible: root.periodic || root.sweep || root.drawnRepeated || root.modulated
                label: qsTr("Phase")
                unit: "°"
                value: root.wave.phase ?? 0
                tip: qsTr("Phase à l'origine des temps, en degrés : −30° retarde d'un douzième de période")
                onEdited: (v) => root.set({ phase: v })
            }
            QuantityField {
                visible: root.takesDuty
                label: qsTr("Rapport cyclique")
                unit: "%"
                minimum: 0.1
                maximum: 99.9
                value: 100 * (root.wave.duty ?? 0.5)
                tip: qsTr("Part de chaque période à l'état haut")
                onEdited: (v) => root.set({ duty: v / 100 })
            }
            QuantityField {
                visible: root.timed
                label: qsTr("Instant")
                unit: "s"
                value: root.wave.time ?? 0
                tip: root.waveform === "step" ? qsTr("Instant du saut, depuis le début du signal")
                                              : qsTr("Instant de l'impulsion (arrondi à l'échantillon le plus proche)")
                onEdited: (v) => root.set({ time: v })
            }
            ChoiceField {
                visible: root.noise
                label: qsTr("Distribution")
                choices: [qsTr("Gaussienne"), qsTr("Uniforme")]
                value: root.wave.noise === "uniform" ? 1 : 0
                onEdited: (i) => root.set({ noise: i === 1 ? "uniform" : "gaussian" })
            }
            ChoiceField {
                visible: root.drawn
                label: qsTr("Entre les points")
                choices: Choices.names(Choices.motifInterpolations)
                value: root.wave.interpolation === "steps" ? 0 : root.wave.interpolation === "smooth" ? 2 : 1
                onEdited: (i) => root.set({ interpolation: Choices.motifInterpolations[i].key })
            }
            ChoiceField {
                visible: root.drawn
                label: qsTr("Le motif")
                choices: [qsTr("À chaque période"), qsTr("Sur toute la durée")]
                value: root.wave.once === true ? 1 : 0
                onEdited: (i) => root.set({ once: i === 1 })
            }
            ChoiceField {
                visible: root.recorded
                label: qsTr("Niveau")
                choices: [qsTr("Tel quel, fois un gain"), qsTr("Crête ramenée à une valeur")]
                value: root.levelled ? 1 : 0
                onEdited: (i) => root.set({ normalize: i === 1 })
            }
            QuantityField {
                visible: root.recorded
                label: qsTr("Lu à partir de")
                unit: "s"
                minimum: 0
                value: root.wave.sourceStart ?? 0
                tip: qsTr("L'instant de l'enregistrement où la lecture commence (0 : son début)")
                onEdited: (v) => root.set({ sourceStart: v })
            }
            QuantityField {
                visible: root.recorded
                label: qsTr("Retard")
                unit: "s"
                value: root.wave.time ?? 0
                tip: qsTr("L'instant du signal généré où l'enregistrement commence (avant : 0), arrondi à l'échantillon")
                onEdited: (v) => root.set({ time: v })
            }
            ChoiceField {
                visible: root.recorded
                label: qsTr("Lecture")
                choices: [qsTr("Une fois (0 ensuite)"), qsTr("En boucle")]
                value: root.wave.loop === true ? 1 : 0
                onEdited: (i) => root.set({ loop: i === 1 })
            }
            ChoiceField {
                visible: root.modulated
                label: qsTr("Modulation")
                choices: Choices.names(Choices.carrierModulations)
                value: Choices.indexOf(Choices.carrierModulations, root.modulationKind)
                onEdited: (i) => root.set({ modulation: Choices.carrierModulations[i].key })
            }
            QuantityField {
                visible: root.modulated && root.modulationKind === "am"
                label: qsTr("Indice m")
                unit: ""
                minimum: 0
                value: root.wave.index ?? 0.5
                tip: qsTr("A·(1 + m·message) : avec un message de crête 1 et m = 0,5, l'enveloppe va de A/2 à 1,5 A ; au-delà de 1, surmodulation")
                onEdited: (v) => root.set({ index: v })
            }
            QuantityField {
                visible: root.modulated && root.modulationKind === "fm"
                label: qsTr("Excursion Δf")
                unit: "Hz"
                value: root.wave.deviation ?? 1000
                tip: qsTr("Fréquence instantanée fc + Δf·message : avec un message de crête 1, de fc − Δf à fc + Δf (indice β = Δf/fm)")
                onEdited: (v) => root.set({ deviation: v })
            }
            QuantityField {
                visible: root.modulated && root.modulationKind === "pm"
                label: qsTr("Excursion Δφ")
                unit: "°"
                value: root.wave.phaseDeviation ?? 90
                tip: qsTr("Phase φ + Δφ·message : avec un message de crête 1, de φ − Δφ à φ + Δφ")
                onEdited: (v) => root.set({ phaseDeviation: v })
            }
        }
        // ---- An operation between channels, or the message of a modulated carrier ----
        ColumnLayout {
            visible: root.operation || root.modulated
            Layout.fillWidth: true
            opacity: root.counted ? 1 : 0.55
            spacing: 6
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: root.operation ? qsTr("y, une formule des voies v1, v2…, du temps t et des formes d'onde au-dessus (x).")
                                         : Choices.carrierModulations[Choices.indexOf(Choices.carrierModulations, root.modulationKind)].tip
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
                Button {
                    id: operationModelButton
                    visible: root.operation
                    Layout.alignment: Qt.AlignBottom
                    text: qsTr("Modèles ▾")
                    flat: true
                    onClicked: operationModelMenu.popup(operationModelButton, 0, operationModelButton.height)
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Une opération toute prête des deux premières autres voies : somme, différence, produit, quotient, moyenne, module…")
                }
            }
            // The ready-made operations, or messages (beside the field of the message).
            FitMenu {
                id: operationModelMenu
                Instantiator {
                    model: root.operationChoices
                    delegate: MenuItem {
                        required property var modelData
                        text: modelData.name + " : " + modelData.formula
                        onTriggered: root.operation ? root.generator.applyOperationModel(root.channel, root.index, modelData.key)
                                                    : root.generator.applyMessageModel(root.channel, root.index, modelData.key)
                    }
                    onObjectAdded: (index, object) => operationModelMenu.insertItem(index, object)
                    onObjectRemoved: (index, object) => operationModelMenu.removeItem(object)
                }
            }
            FormulaField {
                label: qsTr("Variables")
                value: root.wave.variables ?? ""
                placeholder: root.operation ? "k = 2" : "fm = 1000"
                lineSeparator: " ; "
                tip: qsTr("Des constantes pour la formule, séparées par « ; » ; elles peuvent utiliser fs, Te et N : fm = fs/48")
                onEdited: (text) => root.set({ variables: text })
            }
            FormulaField {
                visible: root.operation
                label: "y ="
                value: root.wave.expression ?? "x"
                tip: qsTr("Une formule des voies : v1 + v2, v1*v2, (1 + 0,5*v2)*v1, abs(v1), sqrt(v1^2 + v2^2), sin(2*pi*50*t)^3…")
                onEdited: (text) => root.set({ expression: text })
            }
            RowLayout {
                visible: root.modulated
                Layout.fillWidth: true
                spacing: 6
                FormulaField {
                    Layout.fillWidth: true
                    label: qsTr("Message m(t) =")
                    value: root.wave.message ?? ""
                    tip: qsTr("Ce qui module la porteuse : sin(2*pi*fm*t), v1 (une autre voie), x (les formes d'onde au-dessus de celle-ci)… ou, avec « Message ▾ », un signal de la session")
                    onEdited: (text) => root.set({ message: text })
                }
                Button {
                    id: messageModelButton
                    Layout.alignment: Qt.AlignBottom
                    text: qsTr("Message ▾")
                    flat: true
                    onClicked: operationModelMenu.popup(messageModelButton, 0, messageModelButton.height)
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Le message de la porteuse : un sinus ou un carré de fréquence fm, les formes d'onde au-dessus, une autre voie, ou un signal de la session (ramené à 1 V crête, placé au-dessus de la porteuse)")
                }
            }
            Label {
                visible: root.operationError.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.operationError
                color: Theme.error
                font.pixelSize: Theme.smallText
            }
            Label {
                visible: root.operationCaution.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.operationCaution
                color: Theme.warning
                font.pixelSize: Theme.smallText
            }
            Label {
                visible: root.operationNote.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.operationNote
                color: Theme.text
                font.pixelSize: Theme.smallText
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Dans la formule : v1… v16 (les voies, décalage compris), x (les formes d'onde au-dessus de celle-ci, qu'elle remplace alors), t, n, fs, Te (1/fs), N (nombre d'échantillons) et les variables ; les fonctions des séries de Fourier (sin, abs, sqrt, if…). Une voie ne peut pas se lire elle-même ; une valeur qui n'est pas un nombre (division par zéro) vaut 0.")
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
            }
        }
        // ---- A recurrence: y[n] from the values before it ----
        ColumnLayout {
            visible: root.recurrent
            Layout.fillWidth: true
            opacity: root.counted ? 1 : 0.55
            spacing: 6
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                ChoiceField {
                    label: qsTr("Entrée x[n]")
                    choices: Choices.names(Choices.recurrenceInputs)
                    value: Choices.indexOf(Choices.recurrenceInputs, root.wave.input ?? "none")
                    onEdited: (i) => root.set({ input: Choices.recurrenceInputs[i].key })
                }
                Button {
                    id: recurrenceModelButton
                    Layout.alignment: Qt.AlignBottom
                    text: qsTr("Modèles ▾")
                    flat: true
                    onClicked: recurrenceModelMenu.popup(recurrenceModelButton, 0, recurrenceModelButton.height)
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Une récurrence toute prête : oscillateur, passe-bas, résonateur, moyenne glissante, marche aléatoire, suite logistique")
                    FitMenu {
                        id: recurrenceModelMenu
                        Instantiator {
                            model: Choices.recurrenceModels
                            delegate: MenuItem {
                                required property var modelData
                                text: modelData.name
                                onTriggered: root.generator.applyRecurrenceModel(root.channel, root.index, modelData.key)
                            }
                            onObjectAdded: (index, object) => recurrenceModelMenu.insertItem(index, object)
                            onObjectRemoved: (index, object) => recurrenceModelMenu.removeItem(object)
                        }
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: Choices.recurrenceInputs[Choices.indexOf(Choices.recurrenceInputs, root.wave.input ?? "none")].tip
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
            }
            FormulaField {
                label: qsTr("Variables")
                value: root.wave.variables ?? ""
                placeholder: "a = 0,9 ; f = 1000"
                lineSeparator: " ; "
                tip: qsTr("Des constantes pour la formule, séparées par « ; » ; elles peuvent utiliser fs, Te et N : w = 2*pi*f/fs")
                onEdited: (text) => root.set({ variables: text })
            }
            FormulaField {
                label: "y[n] ="
                value: root.wave.recurrence ?? "0"
                tip: qsTr("Une formule des valeurs d'avant : a*y[n-1] + (1 - a)*x[n], 2*cos(w)*y[n-1] - y[n-2]…")
                onEdited: (text) => root.set({ recurrence: text })
            }
            FormulaField {
                label: qsTr("Valeurs initiales")
                value: root.wave.initial ?? ""
                placeholder: "y[-1] = 0 ; y[-2] = 0"
                lineSeparator: " ; "
                tip: qsTr("y[-1] à y[-16] avant le début ; celles qui ne sont pas données valent 0")
                onEdited: (text) => root.set({ initial: text })
            }
            Label {
                visible: root.traceError.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.traceError
                color: Theme.error
                font.pixelSize: Theme.smallText
            }
            Label {
                visible: root.traceNote.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.traceNote
                color: Theme.warning
                font.pixelSize: Theme.smallText
            }
            // Its first values, as generated.
            ColumnLayout {
                visible: root.traceError.length === 0 && root.traceRows.length > 0
                Layout.fillWidth: true
                spacing: 1
                Label {
                    Layout.fillWidth: true
                    text: (root.trace.current ?? true) ? qsTr("Premières valeurs") : qsTr("Premières valeurs (calcul en cours…)")
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
                GridLayout {
                    Layout.fillWidth: true
                    columns: 3
                    columnSpacing: 14
                    rowSpacing: 0
                    Repeater {
                        model: ["n", "x[n]", "y[n]"]
                        delegate: Label {
                            required property string modelData
                            Layout.alignment: Qt.AlignRight
                            text: modelData
                            color: Theme.textMuted
                            font.pixelSize: Theme.smallText
                            font.bold: true
                        }
                    }
                    Repeater {
                        model: root.traceRows.length * 3
                        delegate: Label {
                            id: stepCell
                            required property int index
                            readonly property var entry: root.traceRows[Math.floor(index / 3)] ?? ({})
                            readonly property int place: index % 3
                            Layout.alignment: Qt.AlignRight
                            text: stepCell.place === 0 ? String(stepCell.entry.n ?? "")
                                  : stepCell.place === 1 ? Units.compact(stepCell.entry.x ?? 0, "", 5)
                                  : Units.compact(stepCell.entry.y ?? 0, "", 5)
                            color: Theme.text
                            font.pixelSize: Theme.smallText
                            font.family: Theme.monospace
                        }
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Dans la formule : y[n-1]… y[n-16], x[n], x[n-1]… x[n-16], n, t (n/fs), fs, Te (1/fs), N (nombre d'échantillons) et les variables ; les fonctions des séries de Fourier (sin, exp, sqrt, if…). Une valeur qui n'est plus un nombre fini arrête le calcul (la récurrence diverge).")
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
            }
        }
        // ---- A drawn motif: its editor, ready-made motifs, its points as text ----
        ColumnLayout {
            visible: root.drawn
            Layout.fillWidth: true
            opacity: root.counted ? 1 : 0.55
            spacing: 4
            MotifEditor {
                id: editor
                Layout.fillWidth: true
                Layout.preferredHeight: 170
                points: root.wave.motif ?? []
                interpolation: root.wave.interpolation ?? "linear"
                periodic: root.wave.once !== true
                snap: snapBox.checked
                endLabel: periodic ? "T" : qsTr("fin")
                emptyText: qsTr("Cliquez pour poser des points, ou dessinez à main levée")
                backgroundColor: Theme.canvas
                gridColor: Theme.grid
                textColor: Theme.text
                mutedColor: Theme.textMuted
                accentColor: Theme.accent
                fontSize: Theme.smallText
                onEdited: (list) => root.set({ motif: list })
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: editor.readout.length > 0 ? editor.readout
                          : editor.pointCount === 1 ? qsTr("1 point") : qsTr("%1 points").arg(editor.pointCount)
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
                CheckBox {
                    id: snapBox
                    text: qsTr("Grille")
                    font.pixelSize: Theme.smallText
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Les points vont sur la grille : 1/32 de la période, 1/8 de la hauteur (Maj : l'inverse, le temps d'un geste)")
                }
                Button {
                    id: motifModelButton
                    text: qsTr("Modèles ▾")
                    flat: true
                    onClicked: motifModelMenu.popup(motifModelButton, 0, motifModelButton.height)
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Un motif tout prêt, à retoucher : rampe, escalier, trapèze, charge RC, cardiogramme")
                    FitMenu {
                        id: motifModelMenu
                        Instantiator {
                            model: Choices.motifModels
                            delegate: MenuItem {
                                required property var modelData
                                text: modelData.name
                                onTriggered: root.generator.applyMotifModel(root.channel, root.index, modelData.key)
                            }
                            onObjectAdded: (index, object) => motifModelMenu.insertItem(index, object)
                            onObjectRemoved: (index, object) => motifModelMenu.removeItem(object)
                        }
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Clic : un point · glisser un point : le déplacer · double-clic ou clic droit : le retirer · à main levée : redessiner là où l'on passe · Suppr et flèches : le point choisi.")
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
            }
            FormulaField {
                label: qsTr("Points (x y ; …)")
                value: root.motifText
                placeholder: "0 0 ; 0,25 1 ; 0,75 -1"
                lineSeparator: " ; "
                tip: qsTr("x de 0 à 1 (la période, ou la durée), y de −1 à 1 (fois l'amplitude), séparés par une espace ; « ; » entre les points ; deux points au même x : un saut")
                onEdited: (text) => root.motifError = root.generator.setMotifText(root.channel, root.index, text)
            }
            Label {
                visible: root.motifError.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.motifError
                color: Theme.error
                font.pixelSize: Theme.smallText
            }
        }
        // ---- A Fourier series: its coefficients as formulas of n ----
        ColumnLayout {
            visible: root.fourierSeries
            Layout.fillWidth: true
            opacity: root.counted ? 1 : 0.55
            spacing: 6
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                ChoiceField {
                    label: qsTr("Coefficients")
                    choices: Choices.names(Choices.seriesForms)
                    value: root.formIndex
                    onEdited: (i) => root.set({ form: Choices.seriesForms[i].key })
                }
                Button {
                    id: modelButton
                    Layout.alignment: Qt.AlignBottom
                    text: qsTr("Modèles ▾")
                    flat: true
                    onClicked: modelMenu.popup(modelButton, 0, modelButton.height)
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Les coefficients d'une forme connue (carré, triangle, redressé…), avec le E et le D des variables")
                    FitMenu {
                        id: modelMenu
                        Instantiator {
                            model: Choices.seriesModels
                            delegate: MenuItem {
                                required property var modelData
                                text: modelData.name
                                onTriggered: root.generator.applySeriesModel(root.channel, root.index, modelData.key)
                            }
                            onObjectAdded: (index, object) => modelMenu.insertItem(index, object)
                            onObjectRemoved: (index, object) => modelMenu.removeItem(object)
                        }
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: Choices.seriesForms[root.formIndex].tip
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
            }
            FormulaField {
                label: qsTr("Variables")
                value: root.wave.variables ?? ""
                placeholder: "E = 1 ; D = 0,25"
                lineSeparator: " ; "
                tip: qsTr("Des constantes pour les formules, séparées par « ; » : E = 1 ; D = 0,25 ; théta = pi/4")
                onEdited: (text) => root.set({ variables: text })
            }
            Repeater {
                model: 3
                delegate: FormulaField {
                    required property int index
                    label: root.terms[index] + " ="
                    value: root.wave[root.formulaKey(index)] ?? "0"
                    tip: index === 0 ? qsTr("Une constante (sans n) : la valeur moyenne du signal")
                                     : qsTr("Une formule de n (rang de l'harmonique) : 4*E/(pi*n)*odd(n), if(n == 1; E/2; 0)…")
                    onEdited: (text) => {
                        const values = {}
                        values[root.formulaKey(index)] = text
                        root.set(values)
                    }
                }
            }
            Label {
                visible: root.infoError.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.infoError
                color: Theme.error
                font.pixelSize: Theme.smallText
            }
            // The first coefficients, and how many harmonics the sum has.
            ColumnLayout {
                visible: root.infoError.length === 0 && root.rows.length > 0
                Layout.fillWidth: true
                spacing: 1
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("a₀ = %1 ; %2 harmoniques dans la somme%3")
                          .arg(Units.compact(root.info.dc ?? 0, root.unit, 4))
                          .arg(root.info.count ?? 0)
                          .arg((root.info.requested ?? 0) > (root.info.count ?? 0)
                               ? qsTr(" (sur %1 : les autres dépassent fs/2)").arg(root.info.requested) : "")
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
                GridLayout {
                    Layout.fillWidth: true
                    columns: 5
                    columnSpacing: 10
                    rowSpacing: 0
                    Repeater {
                        model: ["n", "aₙ", "bₙ", "Aₙ", "φₙ"]
                        delegate: Label {
                            required property string modelData
                            Layout.alignment: Qt.AlignRight
                            text: modelData
                            color: Theme.textMuted
                            font.pixelSize: Theme.smallText
                            font.bold: true
                        }
                    }
                    Repeater {
                        model: root.rows.length * 5
                        delegate: Label {
                            id: cell
                            required property int index
                            // (Not "row" nor "column": the card's layout has the id "column".)
                            readonly property var coefficient: root.rows[Math.floor(index / 5)] ?? ({})
                            readonly property int place: index % 5
                            Layout.alignment: Qt.AlignRight
                            text: cell.place === 0 ? String(cell.coefficient.n ?? "")
                                  : cell.place === 1 ? Units.compact(cell.coefficient.a ?? 0, "", 4)
                                  : cell.place === 2 ? Units.compact(cell.coefficient.b ?? 0, "", 4)
                                  : cell.place === 3 ? Units.compact(cell.coefficient.amplitude ?? 0, "", 4)
                                  : ((cell.coefficient.amplitude ?? 0) > 0 ? Units.compact(cell.coefficient.phase ?? 0, "", 4) + "°" : "—")
                            color: Theme.text
                            font.pixelSize: Theme.smallText
                            font.family: Theme.monospace
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("aₙ cos(nω₀t) + bₙ sin(nω₀t) = Aₙ sin(nω₀t + φₙ)")
                          + ((root.info.count ?? 0) > root.rows.length ? qsTr(" ; suite non montrée") : "")
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Dans les formules : n (rang), Nmax, f0, w0, T0, fs, pi, e et les variables ; + − * / ^, comparaisons (1 ou 0) ; sin, cos, sqrt, exp, ln, log, abs, sinc, odd, even, mod, min, max, if(condition; alors; sinon)… ; « ; » entre les arguments ; 0,5 ou 0.5.")
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
            }
        }
        RowLayout {
            visible: root.series || root.fourierSeries || root.drawnRepeated
            Layout.fillWidth: true
            opacity: root.counted ? 1 : 0.55
            spacing: 8
            CheckBox {
                Layout.fillWidth: true
                text: root.fourierSeries ? qsTr("Sans repliement (rien à fs/2 ni au-delà)") : qsTr("Sans repliement (série de Fourier)")
                checked: root.fourier
                onToggled: root.set({ bandLimit: checked ? "fourier" : "sampled" })
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: root.fourierSeries
                              ? qsTr("Coché : les harmoniques à fs/2 ou au-delà sont laissés de côté. Décoché : ils sont échantillonnés tels quels et se replient entre les raies.")
                              : qsTr("Échantillonnée telle quelle, la forme d'onde replie ses harmoniques au-delà de fs/2 entre les raies (comme un CAN sans filtre). Par sa série de Fourier jusqu'à fs/2 : pas de repliement, mais des ondulations de Gibbs sur les fronts.")
            }
            SpinBox {
                visible: (root.series || root.drawnRepeated) && root.fourier
                Layout.preferredWidth: 110
                from: 0
                to: 100000
                editable: true
                value: root.wave.harmonics ?? 0
                textFromValue: (v) => v === 0 ? qsTr("toutes") : String(v)
                valueFromText: (text) => {
                    const v = parseInt(text.replace(/\s/g, ""))
                    return isNaN(v) ? 0 : v
                }
                onValueModified: root.set({ harmonics: value })
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Nombre d'harmoniques de la série (0 : toutes celles sous fs/2) ; peu d'harmoniques montrent bien les ondulations de Gibbs")
            }
        }
    }
}
