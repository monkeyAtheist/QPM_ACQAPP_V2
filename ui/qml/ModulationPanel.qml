import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// Tab "Liaison" of the right panel of the module "Modulations": the link in detail. The transmitter (modulation, on
// a carrier or as I/Q, symbol rate, carrier, amplitude and phase, pulses and roll-off, the low level of an ASK, the
// deviation of an FSK, the rate and the length of the record), what is sent (bits at random, a text, framed or
// not, bits typed), the channel (white noise at Eb/N0), then what came out: the bit error rate measured against
// the theory, the symbols, Eb/N0 measured, EVM, and a text as received. Every field writes into the recipe at once,
// and the link is computed again.
ScrollView {
    id: root
    required property var modulations

    readonly property var recipe: modulations.recipe
    readonly property var result: modulations.result
    readonly property bool linkOk: result.ok === true
    readonly property bool fsk: recipe.modulation === "fsk"
    readonly property bool ask: recipe.modulation === "ask"
    readonly property bool iq: recipe.representation === "iq"
    readonly property bool rrc: !fsk && recipe.pulse === "rrc"
    readonly property real fs: recipe.sampleRate ?? 48000
    readonly property real samplesPerSymbol: fs / Math.max(1e-9, recipe.symbolRate ?? 1000)

    contentWidth: availableWidth
    clip: true

    function set(values) {
        modulations.setRecipe(values)
    }
    // A rate as the core writes it ("1,2×10⁻⁴"), or "—".
    function rateOf(key) {
        const v = root.result[key + "Text"]
        return v === undefined || v.length === 0 ? "—" : v
    }

    component Hint: Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textMuted
        font.pixelSize: Theme.smallText
    }
    // A name and its value on one line, in the results.
    component Line: RowLayout {
        property string name: ""
        property string value: ""
        property color tint: Theme.text
        Layout.fillWidth: true
        spacing: 8
        Label {
            text: parent.name
            color: Theme.textMuted
            font.pixelSize: Theme.smallText
            Layout.preferredWidth: 120
            elide: Text.ElideRight
        }
        Label {
            text: parent.value
            color: parent.tint
            font.pixelSize: Theme.bodyText
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: 10

        // ---- The transmitter ----
        SectionTitle { text: qsTr("Émetteur") }
        ChoiceField {
            label: qsTr("Modulation")
            choices: Choices.names(Choices.linkModulations)
            value: Choices.indexOf(Choices.linkModulations, root.recipe.modulation ?? "qpsk")
            onEdited: (index) => root.set({ modulation: Choices.linkModulations[index].key })
        }
        Hint {
            text: (Choices.linkModulations.find((m) => m.key === root.recipe.modulation) ?? {}).tip ?? ""
        }
        ChoiceField {
            label: qsTr("Signal")
            choices: Choices.names(Choices.linkRepresentations)
            value: Choices.indexOf(Choices.linkRepresentations, root.recipe.representation ?? "passband")
            onEdited: (index) => root.set({ representation: Choices.linkRepresentations[index].key })
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            QuantityField {
                label: qsTr("Débit de symboles")
                value: root.recipe.symbolRate ?? 1000
                unit: "Bd"
                minimum: 1e-3
                maximum: root.fs
                tip: qsTr("Symboles par seconde (bauds) ; le débit binaire en est le produit par les bits de chaque symbole")
                onEdited: (v) => root.set({ symbolRate: v })
            }
            QuantityField {
                label: qsTr("fs")
                value: root.fs
                unit: "Hz"
                minimum: 1
                maximum: 1e9
                tip: qsTr("Fréquence d'échantillonnage des signaux émis et reçus (48 kHz : ce que joue une carte son)")
                onEdited: (v) => root.set({ sampleRate: v })
            }
        }
        Hint {
            text: qsTr("%1 échantillons par symbole%2.").arg(Units.compact(root.samplesPerSymbol, "", 4))
                  .arg(root.rrc && Math.abs(root.samplesPerSymbol - Math.round(root.samplesPerSymbol)) > 1e-9
                       ? qsTr(" : il en faut un nombre entier pour le cosinus surélevé") : "")
        }
        RowLayout {
            visible: !root.iq
            Layout.fillWidth: true
            spacing: 8
            QuantityField {
                label: root.fsk ? qsTr("Fréquence centrale fc") : qsTr("Porteuse fc")
                value: root.recipe.carrier ?? 6000
                unit: "Hz"
                minimum: 1e-3
                maximum: root.fs / 2
                tip: qsTr("Fréquence de la porteuse (au moins la moitié de la bande occupée, et loin de fs/2)")
                onEdited: (v) => root.set({ carrier: v })
            }
            QuantityField {
                label: qsTr("Phase")
                value: root.recipe.phase ?? 0
                unit: "°"
                minimum: -360
                maximum: 360
                tip: qsTr("Phase de la porteuse à l'origine : le récepteur, cohérent, la connaît")
                onEdited: (v) => root.set({ phase: v })
            }
        }
        QuantityField {
            label: qsTr("Amplitude de la porteuse")
            value: root.recipe.amplitude ?? 1
            unit: "V"
            minimum: 1e-9
            maximum: 1e6
            tip: qsTr("Les points de la constellation sont à cette amplitude (en moyenne d'énergie pour la PSK et la QAM)")
            onEdited: (v) => root.set({ amplitude: v })
        }
        ChoiceField {
            visible: !root.fsk
            label: qsTr("Impulsions")
            choices: Choices.names(Choices.linkPulses)
            value: Choices.indexOf(Choices.linkPulses, root.recipe.pulse ?? "rrc")
            onEdited: (index) => root.set({ pulse: Choices.linkPulses[index].key })
        }
        Hint {
            visible: !root.fsk
            text: (Choices.linkPulses.find((p) => p.key === root.recipe.pulse) ?? {}).tip ?? ""
        }
        QuantityField {
            visible: root.rrc
            label: qsTr("Facteur de retombée α")
            value: root.recipe.rolloff ?? 0.35
            minimum: 0
            maximum: 1
            tip: qsTr("De 0 (la bande la plus étroite, Rs, mais une impulsion qui sonne longtemps) à 1 (une bande de 2 Rs, une impulsion brève)")
            onEdited: (v) => root.set({ rolloff: v })
        }
        QuantityField {
            visible: root.ask
            label: qsTr("Niveau d'un 0 (fois celui d'un 1)")
            value: root.recipe.askLow ?? 0
            minimum: 0
            maximum: 0.95
            tip: qsTr("0 : tout ou rien (la porteuse éteinte) ; plus haut, la porteuse jamais éteinte, mais les deux niveaux plus proches")
            onEdited: (v) => root.set({ askLow: v })
        }
        QuantityField {
            visible: root.fsk
            label: qsTr("Excursion Δf (tonalités à fc ± Δf)")
            value: root.recipe.fskDeviation ?? 500
            unit: "Hz"
            minimum: 1e-3
            maximum: root.fs / 2
            tip: qsTr("Les tonalités sont orthogonales quand leur écart 2 Δf est un multiple du débit : la théorie ne vaut qu'alors")
            onEdited: (v) => root.set({ fskDeviation: v })
        }
        QuantityField {
            label: qsTr("Symboles dans l'enregistrement")
            value: root.recipe.symbols ?? 4000
            minimum: 16
            maximum: 1048576
            tip: qsTr("La longueur de l'aperçu (1 048 576 échantillons au plus) ; plus de symboles, un TEB mesuré plus sûr")
            onEdited: (v) => root.set({ symbols: Math.round(v) })
        }

        // ---- What is sent ----
        SectionTitle { text: qsTr("Données") }
        ChoiceField {
            label: qsTr("Envoyer")
            choices: Choices.names(Choices.linkData)
            value: Choices.indexOf(Choices.linkData, root.recipe.data ?? "random")
            onEdited: (index) => root.set({ data: Choices.linkData[index].key })
        }
        FormulaField {
            visible: root.recipe.data === "text"
            label: qsTr("Texte")
            value: root.recipe.text ?? ""
            placeholder: qsTr("Bonjour !")
            tip: qsTr("Ses octets (UTF-8) l'un après l'autre, répétés jusqu'à la fin de l'enregistrement")
            onEdited: (text) => root.set({ text: text })
        }
        CheckBox {
            visible: root.recipe.data === "text"
            text: qsTr("En trame série 8N1 (start, 8 bits, stop)")
            checked: root.recipe.uart === true
            onToggled: root.set({ uart: checked })
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: qsTr("Comme une liaison série : un bit de start à 0, les 8 bits de poids faible en tête, un bit de stop à 1")
        }
        FormulaField {
            visible: root.recipe.data === "bits"
            label: qsTr("Bits")
            value: root.recipe.bits ?? ""
            placeholder: "1011 0010"
            tip: qsTr("Des 0 et des 1 (le reste est ignoré), répétés")
            onEdited: (text) => root.set({ bits: text })
        }
        QuantityField {
            visible: root.recipe.data === "random"
            label: qsTr("Graine des bits")
            value: root.recipe.dataSeed ?? 1
            minimum: 0
            maximum: 4294967295
            tip: qsTr("La même graine, les mêmes bits")
            onEdited: (v) => root.set({ dataSeed: Math.round(v) })
        }

        // ---- The channel ----
        SectionTitle { text: qsTr("Canal") }
        CheckBox {
            text: qsTr("Bruit blanc gaussien")
            checked: root.recipe.noise === true
            onToggled: root.set({ noise: checked })
        }
        RowLayout {
            visible: root.recipe.noise === true
            Layout.fillWidth: true
            spacing: 8
            QuantityField {
                label: qsTr("Eb/N0")
                value: root.recipe.ebN0 ?? 7
                unit: "dB"
                minimum: -20
                maximum: 60
                tip: qsTr("Énergie d'un bit sur la densité du bruit : ce que fixe la théorie du TEB, quelle que soit la modulation")
                onEdited: (v) => root.set({ ebN0: v })
            }
            QuantityField {
                label: qsTr("Graine du bruit")
                value: root.recipe.noiseSeed ?? 2
                minimum: 0
                maximum: 4294967295
                tip: qsTr("La même graine, le même bruit")
                onEdited: (v) => root.set({ noiseSeed: Math.round(v) })
            }
        }
        Hint {
            visible: root.recipe.noise === true
            text: qsTr("Le bruit est réglé sur la puissance du signal mesurée : Eb = P / débit binaire, N0 = Eb / (Eb/N0) ; sa variance par échantillon est N0 fs / 2.")
        }

        // ---- What came out ----
        SectionTitle { text: qsTr("Résultat") }
        Label {
            visible: !root.linkOk && (root.result.error ?? "").length > 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.result.error ?? ""
            color: Theme.error
            font.pixelSize: Theme.bodyText
        }
        Repeater {
            model: root.linkOk ? (root.result.warnings ?? []) : []
            delegate: Label {
                required property string modelData
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: "⚠ " + modelData
                color: Theme.warning
                font.pixelSize: Theme.smallText
            }
        }
        ColumnLayout {
            visible: root.linkOk
            Layout.fillWidth: true
            spacing: 4
            Line {
                name: qsTr("TEB mesuré")
                value: root.linkOk ? qsTr("%1 (%2 sur %3 bits)").arg(root.result.berText ?? "")
                                         .arg(Units.compact(root.result.bitErrors ?? 0, "", 9))
                                         .arg(Units.compact(root.result.comparedBits ?? 0, "", 9)) : ""
                tint: (root.result.bitErrors ?? 0) > 0 ? Theme.warning : Theme.text
            }
            Line {
                visible: root.recipe.noise === true
                name: qsTr("Intervalle à 95 %")
                value: qsTr("%1 à %2").arg(root.rateOf("berLower")).arg(root.rateOf("berUpper"))
            }
            Line {
                visible: root.recipe.noise === true
                name: qsTr("TEB théorique")
                value: root.result.theoryText ?? ""
            }
            Line {
                name: qsTr("TES (symboles)")
                value: qsTr("%1 ; théorie %2").arg(root.rateOf("ser")).arg(root.rateOf("theorySer"))
            }
            Line {
                visible: root.recipe.noise === true
                name: qsTr("Eb/N0 mesuré")
                value: Units.compact(root.result.measuredEbN0 ?? 0, "dB", 4)
            }
            Line {
                visible: !root.fsk
                name: qsTr("EVM, MER")
                value: isNaN(root.result.evm ?? NaN) ? "—"
                       : qsTr("%1 %, %2").arg(Units.compact(root.result.evm, "", 3))
                                        .arg(isFinite(root.result.mer) ? Units.compact(root.result.mer, "dB", 3) : "∞")
            }
            Line {
                name: qsTr("Débit binaire")
                value: Units.compact(root.result.bitRate ?? 0, "bit/s", 4)
            }
            Line {
                name: qsTr("Bande occupée")
                value: Units.compact(root.result.bandwidth ?? 0, "Hz", 4)
            }
        }
        Repeater {
            model: root.linkOk ? (root.result.summary ?? []) : []
            delegate: Hint {
                required property string modelData
                text: modelData
            }
        }
        // A text: as sent, as received.
        ColumnLayout {
            visible: root.linkOk && root.recipe.data === "text" && (root.result.textSent ?? "").length > 0
            Layout.fillWidth: true
            spacing: 4
            SectionTitle { text: qsTr("Texte") }
            Label {
                text: qsTr("Envoyé")
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                maximumLineCount: 6
                elide: Text.ElideRight
                text: root.result.textSent ?? ""
                font.family: Theme.monospace
                font.pixelSize: Theme.smallText + 1
            }
            Label {
                text: (root.result.textErrors ?? 0) > 0
                      ? qsTr("Reçu : %1 caractères faux").arg(root.result.textErrors)
                      : qsTr("Reçu : sans faute")
                color: (root.result.textErrors ?? 0) > 0 ? Theme.warning : Theme.success
                font.pixelSize: Theme.smallText
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                maximumLineCount: 6
                elide: Text.ElideRight
                text: root.result.textReceived ?? ""
                font.family: Theme.monospace
                font.pixelSize: Theme.smallText + 1
            }
        }
        Item {
            Layout.preferredHeight: 8
        }
    }
}
