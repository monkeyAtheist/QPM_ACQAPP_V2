import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// Tab "Filtre" of the right panel of the module "Filtres": the filter in detail. Its type, response, family
// or window, frequencies, order or number of coefficients; the rate it is designed for (that of the signal
// chosen, or one typed); how it is applied; what it does (sections, stability, delays, what it warns about)
// and its coefficients, to copy. Every field writes into the recipe at once, and the plots follow.
ScrollView {
    id: root
    required property var filters

    readonly property var recipe: filters.recipe
    readonly property var design: filters.design
    readonly property bool designed: design.ok === true
    readonly property bool iir: recipe.kind !== "fir"
    readonly property bool band: recipe.response === "bandpass" || recipe.response === "bandstop"
    readonly property var input: filters.input
    readonly property bool hasInput: (input.name ?? "").length > 0
    // Aligned: a FIR only.
    readonly property var applications: root.iir ? Choices.filterApplications.filter((a) => a.key !== "aligned")
                                                 : Choices.filterApplications

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
        ChoiceField {
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
            visible: !root.iir
            label: qsTr("Fenêtre")
            choices: Choices.names(Choices.firWindows)
            value: Choices.indexOf(Choices.firWindows, root.recipe.window ?? "hamming")
            onEdited: (index) => root.set({ window: Choices.firWindows[index].key })
        }
        Hint {
            visible: !root.iir
            text: (Choices.firWindows.find((w) => w.key === root.recipe.window) ?? {}).tip ?? ""
        }
        RowLayout {
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
                visible: !root.iir
                label: qsTr("Coefficients N")
                minimum: 3
                maximum: 65535
                value: root.recipe.taps ?? 101
                tip: qsTr("De 3 à 65 535 : retard de (N − 1)/2 échantillons ; impair pour un passe-haut ou un coupe-bande")
                onEdited: (v) => root.set({ taps: Math.round(v) })
            }
            QuantityField {
                visible: root.iir && root.recipe.family === "chebyshev1"
                label: qsTr("Ondulation")
                unit: "dB"
                minimum: 0.001
                maximum: 40
                value: root.recipe.ripple ?? 1
                onEdited: (v) => root.set({ ripple: v })
            }
            QuantityField {
                visible: root.iir && root.recipe.family === "chebyshev2"
                label: qsTr("Atténuation")
                unit: "dB"
                minimum: 1
                maximum: 300
                value: root.recipe.attenuation ?? 40
                onEdited: (v) => root.set({ attenuation: v })
            }
            QuantityField {
                visible: !root.iir && root.recipe.window === "kaiser"
                label: "β (Kaiser)"
                minimum: 0
                maximum: 50
                value: root.recipe.kaiserBeta ?? 8.6
                onEdited: (v) => root.set({ kaiserBeta: v })
            }
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
                  + (root.iir ? qsTr(" (« Alignée » : pour un RIF seulement.)") : "")
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
            text: root.iir ? qsTr("Coefficients (sections du second ordre)") : qsTr("Coefficients h[n]")
        }
        Hint {
            visible: root.designed
            text: root.iir ? qsTr("Chaque section, avec a0 = 1 : y[n] = b0·x[n] + b1·x[n−1] + b2·x[n−2] − a1·y[n−1] − a2·y[n−2] ; la sortie d'une section entre dans la suivante.")
                           : qsTr("y[n] = Σ h[k]·x[n − k], de k = 0 à N − 1.")
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
