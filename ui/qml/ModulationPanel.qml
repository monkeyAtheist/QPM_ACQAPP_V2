import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// Tab "Liaison" of the right panel of the module "Modulations": the link in detail. The transmitter (modulation, on
// a carrier or as I/Q, symbol rate, carrier, amplitude and phase, pulses and roll-off, the low level of an ASK, the
// deviation of an FSK, the rate and the length of the record), what is sent (bits at random, a text, framed or
// not, bits typed), the channel (white noise at Eb/N0; the carrier offset in frequency and turned, the frame
// delayed, the receiver's clock off; several paths, as typed), the receiver (ideal: it knows the channel;
// synchronized: a preamble and two loops find it; an equalizer after either), then what came out: the bit error
// rate measured against the theory, the symbols, Eb/N0 measured, EVM, what the synchronization found against the
// truth, what the paths do to the decisions and what the equalizer makes of it, and a text as received. Every
// field writes into the recipe at once, and the link is computed again.
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
    readonly property bool synchronizes: recipe.receiver === "sync"
    readonly property bool equalizes: !fsk && (recipe.equalizer === "linear" || recipe.equalizer === "dfe")
    readonly property bool fades: (recipe.fading ?? "none") !== "none"
    // What the synchronized receiver found ({used: false} otherwise).
    readonly property var found: result.sync ?? ({})
    // Several paths and the equalizer, as the decisions see them.
    readonly property var equalized: result.equalization ?? ({})
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
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            QuantityField {
                label: qsTr("Décalage de fréquence")
                value: root.recipe.frequencyOffset ?? 0
                unit: "Hz"
                minimum: -(root.recipe.symbolRate ?? 1000)
                maximum: root.recipe.symbolRate ?? 1000
                tip: qsTr("La porteuse reçue n'est pas à la fréquence que le récepteur attend : ses oscillateurs ne sont pas ceux de l'émetteur (au plus le débit de symboles)")
                onEdited: (v) => root.set({ frequencyOffset: v })
            }
            QuantityField {
                label: qsTr("Phase du canal")
                value: root.recipe.phaseOffset ?? 0
                unit: "°"
                minimum: -360
                maximum: 360
                tip: qsTr("La porteuse reçue tournée d'un angle que le récepteur ne connaît pas : la constellation arrive tournée d'autant")
                onEdited: (v) => root.set({ phaseOffset: v })
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            QuantityField {
                label: qsTr("Retard")
                value: root.recipe.delay ?? 0
                unit: "symb."
                minimum: 0
                maximum: 1000
                tip: qsTr("La trame arrive plus tard, d'un nombre de symboles qui n'a pas à être entier (de 0 à 1 000)")
                onEdited: (v) => root.set({ delay: v })
            }
            QuantityField {
                label: qsTr("Écart d'horloge")
                value: root.recipe.clockPpm ?? 0
                unit: "ppm"
                minimum: -10000
                maximum: 10000
                tip: qsTr("L'horloge d'échantillonnage du récepteur, en millionièmes de celle de l'émetteur : l'instant de décision glisse peu à peu")
                onEdited: (v) => root.set({ clockPpm: v })
            }
        }
        FormulaField {
            label: qsTr("Trajets")
            value: root.recipe.paths ?? ""
            placeholder: qsTr("un seul (plusieurs : 0 : 0 ; 1 : -6 ; 2,5 : -12 @ 90)")
            lineSeparator: " ; "
            tip: qsTr("Un trajet : son retard en symboles, « : », son gain en dB et, s'il est tourné, « @ » puis sa phase en degrés ; « ; » entre deux trajets (8 au plus, jusqu'à 32 symboles de retard)")
            onEdited: (text) => root.set({ paths: text })
        }
        Hint {
            text: root.result.hasPaths === true
                  ? qsTr("Le récepteur reçoit la somme de ces copies du signal, retardées, atténuées, tournées ; leurs puissances s'ajoutent pour donner celle du signal reçu (Eb/N0 reste celui de ce qu'il reçoit). Chaque symbole reçoit alors une part de ses voisins : graphe « Canal », à côté des signaux.")
                  : qsTr("Chaque trajet s'écrit « retard en symboles : gain en dB », suivi de « @ phase en degrés » s'il est tourné ; « ; » entre deux. « 0 : 0 ; 1 : -6 » ajoute un écho à −6 dB, un symbole plus tard (d'autres canaux : ruban, Canal ▸ Trajets).")
        }
        ChoiceField {
            label: qsTr("Évanouissements")
            choices: Choices.names(Choices.linkFadings)
            value: Choices.indexOf(Choices.linkFadings, root.recipe.fading ?? "none")
            onEdited: (index) => root.set({ fading: Choices.linkFadings[index].key })
        }
        RowLayout {
            visible: root.fades
            Layout.fillWidth: true
            spacing: 8
            QuantityField {
                label: qsTr("Doppler fd")
                value: root.recipe.doppler ?? 10
                unit: "Hz"
                minimum: 0
                maximum: 0.25 * (root.recipe.symbolRate ?? 1000)
                tip: qsTr("Décalage Doppler maximal, vitesse sur longueur d'onde (au plus le quart du débit de symboles) : le gain change en 1 / fd environ. 0 : un gain tiré une fois, puis fixe")
                onEdited: (v) => root.set({ doppler: v })
            }
            QuantityField {
                visible: (root.recipe.fading ?? "none") === "rice"
                label: qsTr("K (Rice)")
                value: root.recipe.riceK ?? 6
                unit: "dB"
                minimum: -20
                maximum: 40
                tip: qsTr("Puissance de l'onde directe sur celle des ondes dispersées, sur le premier trajet (les autres : Rayleigh)")
                onEdited: (v) => root.set({ riceK: v })
            }
            QuantityField {
                label: qsTr("Graine")
                value: root.recipe.fadingSeed ?? 3
                minimum: 0
                maximum: 4294967295
                tip: qsTr("La même graine, les mêmes évanouissements ; une autre, d'autres creux (aussi fréquents en moyenne)")
                onEdited: (v) => root.set({ fadingSeed: Math.round(v) })
            }
        }
        Hint {
            visible: root.fades
            text: ((Choices.linkFadings.find((x) => x.key === (root.recipe.fading ?? "none")) ?? {}).tip ?? "")
                  + qsTr(". Chaque trajet s'évanouit à sa façon ; la puissance de chacun reste en moyenne celle donnée. Le récepteur idéal connaît le gain du trajet le plus fort à chaque instant ; le récepteur synchronisé le suit avec ses boucles (et son égaliseur, s'il apprend).")
        }
        Hint {
            text: root.result.channel !== true
                  ? qsTr("Sans décalage, ni rotation, ni retard, ni écart d'horloge, ni échos : le récepteur reçoit ce que l'émetteur envoie, au bruit près.")
                  : root.synchronizes ? qsTr("Le signal reçu est décalé, tourné, retardé, rééchantillonné, mêlé à ses échos : au récepteur de le retrouver.")
                  : root.result.hasPaths === true
                    ? qsTr("Le récepteur idéal connaît ce canal : il défait exactement le retard, l'horloge et la porteuse du trajet le plus fort. Les autres trajets restent mêlés aux symboles : c'est le travail de l'égaliseur.")
                    : qsTr("Le récepteur idéal connaît ce canal et le défait exactement : seul le bruit fait des erreurs. Le récepteur synchronisé, lui, doit le trouver.")
        }

        // ---- The receiver ----
        SectionTitle { text: qsTr("Récepteur") }
        ChoiceField {
            label: qsTr("Récepteur")
            choices: Choices.names(Choices.linkReceivers)
            value: Choices.indexOf(Choices.linkReceivers, root.recipe.receiver ?? "ideal")
            onEdited: (index) => root.set({ receiver: Choices.linkReceivers[index].key })
        }
        Hint {
            text: (Choices.linkReceivers.find((x) => x.key === (root.recipe.receiver ?? "ideal")) ?? {}).tip ?? ""
        }
        QuantityField {
            visible: root.synchronizes
            label: qsTr("Préambule")
            value: root.recipe.preamble ?? 64
            unit: "symb."
            minimum: 8
            maximum: 1024
            tip: qsTr("Symboles connus du récepteur, en tête de trame (de 8 à 1 024) : plus il est long, mieux la trame, la fréquence et la phase sont trouvées dans le bruit ; il ne compte pas dans le TEB")
            onEdited: (v) => root.set({ preamble: Math.round(v) })
        }
        RowLayout {
            visible: root.synchronizes
            Layout.fillWidth: true
            spacing: 8
            QuantityField {
                label: qsTr("Bande, rythme")
                value: root.recipe.timingBandwidth ?? 0.001
                minimum: 0
                maximum: 0.1
                tip: qsTr("Bande de bruit de la boucle de rythme, en fraction du débit de symboles (de 0 à 0,1) : elle suit l'écart d'horloge")
                onEdited: (v) => root.set({ timingBandwidth: v })
            }
            QuantityField {
                visible: !root.fsk
                label: qsTr("Bande, porteuse")
                value: root.recipe.phaseBandwidth ?? 0.003
                minimum: 0
                maximum: 0.1
                tip: qsTr("Bande de bruit de la boucle de porteuse, en fraction du débit de symboles (de 0 à 0,1) : elle suit la phase et le reste de l'écart de fréquence")
                onEdited: (v) => root.set({ phaseBandwidth: v })
            }
        }
        Hint {
            visible: root.synchronizes
            text: root.fsk ? qsTr("La FSK est décidée sans phase : seule la boucle de rythme travaille. Une bande étroite filtre mieux le bruit, mais suit moins vite.")
                           : qsTr("Une boucle étroite filtre mieux le bruit, mais suit moins vite. Trop large pour le bruit reçu, celle de porteuse glisse d'une symétrie de la constellation (un quart de tour en QPSK ou en QAM) : tous les symboles qui suivent sont faux.")
        }
        ChoiceField {
            visible: !root.fsk
            label: qsTr("Égaliseur")
            choices: Choices.names(Choices.linkEqualizers)
            value: Choices.indexOf(Choices.linkEqualizers, root.recipe.equalizer ?? "none")
            onEdited: (index) => root.set({ equalizer: Choices.linkEqualizers[index].key })
        }
        RowLayout {
            visible: root.equalizes
            Layout.fillWidth: true
            spacing: 8
            QuantityField {
                label: qsTr("Prises")
                value: root.recipe.equalizerTaps ?? 11
                minimum: 3
                maximum: 129
                tip: qsTr("Nombre de prises de l'égaliseur, une par symbole (impair, de 3 à 129) : plus il y en a, mieux un creux profond de la bande est comblé. Les derniers symboles de la trame, dont il n'a pas les voisins suivants, ne comptent pas")
                onEdited: (v) => root.set({ equalizerTaps: Math.round(v) })
            }
            QuantityField {
                visible: root.recipe.equalizer === "dfe"
                label: qsTr("Prises de retour")
                value: root.recipe.feedbackTaps ?? 4
                minimum: 1
                maximum: 32
                tip: qsTr("Sur combien de symboles déjà décidés l'égaliseur retire leur écho (de 1 à 32 : au moins le retard du dernier trajet, en symboles)")
                onEdited: (v) => root.set({ feedbackTaps: Math.round(v) })
            }
            QuantityField {
                visible: root.synchronizes
                label: qsTr("Pas d'apprentissage")
                value: root.recipe.equalizerStep ?? 0.05
                minimum: 0.0001
                maximum: 1
                tip: qsTr("Pas du LMS normalisé (jusqu'à 1) : après le préambule, les prises suivent les décisions du récepteur ; un grand pas suit vite, mais ajoute son propre bruit")
                onEdited: (v) => root.set({ equalizerStep: v })
            }
        }
        Hint {
            visible: !root.fsk
            text: (Choices.linkEqualizers.find((x) => x.key === (root.recipe.equalizer ?? "none")) ?? {}).tip ?? ""
        }

        // ---- What came out ----
        SectionTitle { text: qsTr("Résultat") }
        Hint {
            visible: root.modulations.blank
            text: qsTr("Page vierge : rien n'est calculé. Le premier réglage (ici, dans le ruban ou dans Exemples ▾) lance le calcul de la liaison.")
        }
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
                name: (root.result.berBatches ?? 0) > 0 ? qsTr("Intervalle à 95 % (par lots)") : qsTr("Intervalle à 95 %")
                value: qsTr("%1 à %2").arg(root.rateOf("berLower")).arg(root.rateOf("berUpper"))
            }
            Line {
                visible: root.recipe.noise === true
                name: root.fades ? qsTr("TEB théorique, évanouissements") : qsTr("TEB théorique")
                value: root.result.theoryText ?? ""
            }
            Line {
                visible: root.recipe.noise === true && root.fades
                name: qsTr("Sans évanouissements")
                value: root.result.awgnTheoryText ?? ""
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
        // What the synchronized receiver found, against the truth.
        ColumnLayout {
            visible: root.linkOk && root.found.used === true
            Layout.fillWidth: true
            spacing: 4
            SectionTitle { text: qsTr("Synchronisation") }
            Line {
                name: qsTr("Trame")
                value: root.found.frameText ?? ""
                tint: root.found.frameFound === false ? Theme.warning : Theme.text
            }
            Line {
                // (The FSK, decided by the energy of its tones, looks neither for the frequency nor for the phase.)
                visible: root.found.carrier === true
                name: qsTr("Décalage de fréquence")
                value: root.found.frequencyText ?? ""
            }
            Line {
                visible: root.found.carrier === true
                name: qsTr("Phase")
                value: root.found.phaseText ?? ""
            }
            Line {
                name: qsTr("Horloge")
                value: root.found.clockText ?? ""
            }
            Line {
                name: qsTr("Erreur de rythme")
                value: root.found.timingText ?? ""
                tint: root.found.timingLost === true ? Theme.warning : Theme.text
            }
            Line {
                visible: root.found.carrier === true
                name: qsTr("Erreur de phase")
                value: root.found.phaseErrorText ?? ""
            }
            Line {
                visible: root.found.slipsCounted === true
                name: qsTr("Glissements")
                value: (root.found.slips ?? 0) === 0 ? qsTr("aucun") : Units.compact(root.found.slips ?? 0, "", 9)
                tint: (root.found.slips ?? 0) > 0 ? Theme.warning : Theme.text
            }
        }
        // Several paths as the decisions see them, and what the equalizer makes of them.
        ColumnLayout {
            visible: root.linkOk && (root.equalized.paths === true || root.equalized.used === true)
            Layout.fillWidth: true
            spacing: 4
            SectionTitle { text: root.equalized.used === true ? qsTr("Canal et égaliseur") : qsTr("Canal") }
            Line {
                visible: root.equalized.paths === true
                name: qsTr("Trajets reçus")
                value: root.equalized.pathsText ?? ""
            }
            Line {
                visible: (root.equalized.instantText ?? "").length > 0
                name: qsTr("Décisions prises")
                value: root.equalized.instantText ?? ""
            }
            Line {
                visible: (root.equalized.beforeText ?? "").length > 0
                name: qsTr("SINR sans égaliseur")
                value: root.equalized.beforeText ?? ""
            }
            Line {
                visible: root.equalized.used === true
                name: qsTr("Égaliseur")
                value: ((root.equalized.adaptive === true ? qsTr("%1 prises apprises") : qsTr("%1 prises calculées")).arg(root.equalized.taps ?? 0))
                       + ((root.equalized.feedback ?? 0) > 0 ? qsTr(", %1 de retour").arg(root.equalized.feedback) : "")
            }
            Line {
                visible: (root.equalized.afterText ?? "").length > 0
                name: qsTr("SINR égalisé")
                value: root.equalized.afterText ?? ""
            }
            Line {
                visible: (root.equalized.equivalentText ?? "").length > 0
                name: qsTr("Eb/N0 équivalent")
                value: root.equalized.equivalentText ?? ""
            }
            Line {
                visible: (root.equalized.predictedText ?? "").length > 0
                name: qsTr("TEB prévu")
                value: root.equalized.predictedText ?? ""
            }
            Line {
                visible: (root.equalized.errorText ?? "").length > 0
                name: qsTr("Erreur finale")
                value: qsTr("%1 de l'énergie d'un symbole").arg(root.equalized.errorText ?? "")
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
