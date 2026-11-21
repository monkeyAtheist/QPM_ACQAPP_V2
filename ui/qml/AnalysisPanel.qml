import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// Right panel of the "Analyse" module: the settings of the computation, then the measurements of every
// channel of the zone (statistics, tone: frequency, THD, SINAD, SNR, SFDR, ENOB, harmonics). The
// settings are remembered from one run to the next.
ScrollView {
    id: root
    required property var analysis
    required property var appSettings

    readonly property var s: analysis.settings
    readonly property bool density: s.kind === "density"

    contentWidth: availableWidth
    clip: true

    readonly property var windows: [
        { key: "hann", name: qsTr("Hann (usage courant)") },
        { key: "flatTop", name: qsTr("À sommet plat (amplitudes exactes)") },
        { key: "blackmanHarris", name: qsTr("Blackman-Harris (−92 dB : petits signaux)") },
        { key: "blackman", name: qsTr("Blackman") },
        { key: "hamming", name: qsTr("Hamming") },
        { key: "kaiser", name: qsTr("Kaiser (β réglable)") },
        { key: "rectangular", name: qsTr("Rectangulaire (aucune)") }
    ]
    readonly property var paddings: [1, 2, 4, 8]
    readonly property var segments: [256, 512, 1024, 2048, 4096, 8192, 16384, 65536]
    readonly property var overlaps: [0, 0.25, 0.5, 0.75]
    readonly property var harmonicCounts: [5, 10, 20]

    function set(values) {
        analysis.setSettings(values)
    }
    function indexOf(list, value, key) {
        for (let i = 0; i < list.length; ++i) {
            if ((key ? list[i][key] : list[i]) === value)
                return i
        }
        return 0
    }

    Component.onCompleted: {
        const saved = appSettings.value("analysis/settings", "")
        if (saved && saved.length > 0)
            analysis.restoreSettings(saved)
    }
    Connections {
        target: root.analysis
        function onSettingsChanged() { rememberTimer.restart() }
    }
    Timer {
        id: rememberTimer
        interval: 800
        onTriggered: root.appSettings.setValue("analysis/settings", root.analysis.settingsText())
    }

    component Hint: Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textMuted
        font.pixelSize: Theme.smallText
    }
    // A measurement: its name on the left, its value on the right.
    component Measure: RowLayout {
        property string label
        property string value
        Layout.fillWidth: true
        spacing: 8
        Label {
            Layout.fillWidth: true
            text: parent.label
            color: Theme.textMuted
            font.pixelSize: Theme.smallText
            elide: Text.ElideRight
        }
        Label {
            text: parent.value
            color: Theme.text
            font.pixelSize: Theme.smallText
            font.family: Theme.monospace
        }
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: 10

        SectionTitle { text: root.density ? qsTr("Densité spectrale (Welch)") : qsTr("Spectre d'amplitude") }
        Hint {
            text: root.density
                  ? qsTr("La zone est coupée en segments qui se recouvrent ; leurs spectres sont moyennés. Le bruit se lit en unité/√Hz, indépendamment de la longueur des segments.")
                  : qsTr("Une seule FFT de toute la zone : une sinusoïde se lit à son amplitude crête. Plus la zone est longue, plus les fréquences sont fines (Δf = fréquence d'échantillonnage / nombre d'échantillons).")
        }
        ChoiceField {
            label: qsTr("Fenêtre")
            choices: root.windows.map(w => w.name)
            value: root.indexOf(root.windows, root.s.window, "key")
            onEdited: (index) => root.set({ window: root.windows[index].key })
        }
        Hint {
            text: qsTr("Une sinusoïde qui ne tombe pas pile sur une fréquence de la FFT se lit un peu bas avec Hann (jusqu'à −1,4 dB) ; la fenêtre à sommet plat donne l'amplitude exacte, au prix de pics plus larges.")
            visible: !root.density && root.s.window === "hann"
        }
        ColumnLayout {
            visible: root.s.window === "kaiser"
            Layout.fillWidth: true
            spacing: 2
            Label {
                text: qsTr("β de Kaiser : %1").arg(Number(betaSlider.value).toLocaleString(Qt.locale(), "f", 1))
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
            }
            Slider {
                id: betaSlider
                Layout.fillWidth: true
                from: 0
                to: 20
                stepSize: 0.5
                value: root.s.kaiserBeta ?? 8.6
                onMoved: root.set({ kaiserBeta: value })
            }
        }
        ChoiceField {
            visible: !root.density
            label: qsTr("Zéros ajoutés (fréquences plus serrées, pas plus de détails)")
            choices: [qsTr("Aucun"), qsTr("× 2"), qsTr("× 4"), qsTr("× 8")]
            value: root.indexOf(root.paddings, root.s.zeroPadFactor)
            onEdited: (index) => root.set({ zeroPadFactor: root.paddings[index] })
        }
        ChoiceField {
            visible: root.density
            label: qsTr("Longueur des segments")
            choices: root.segments.map(n => qsTr("%1 échantillons").arg(Units.count(n)))
            value: root.indexOf(root.segments, root.s.segmentLength)
            onEdited: (index) => root.set({ segmentLength: root.segments[index] })
        }
        ChoiceField {
            visible: root.density
            label: qsTr("Recouvrement des segments")
            choices: ["0 %", "25 %", "50 %", "75 %"]
            value: root.indexOf(root.overlaps, root.s.overlap)
            onEdited: (index) => root.set({ overlap: root.overlaps[index] })
        }
        CheckBox {
            text: qsTr("Retirer la moyenne (composante continue)")
            checked: root.s.removeMean ?? true
            onToggled: root.set({ removeMean: checked })
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: qsTr("Sans elle, une tension continue fait un grand pic à 0 Hz qui peut cacher les basses fréquences. Elle reste mesurée ci-dessous (Moyenne).")
        }
        ChoiceField {
            visible: !root.density
            label: qsTr("Harmoniques mesurées (fondamentale comprise)")
            choices: root.harmonicCounts.map(n => String(n))
            value: root.indexOf(root.harmonicCounts, root.s.harmonics)
            onEdited: (index) => root.set({ harmonics: root.harmonicCounts[index] })
        }
        ChoiceField {
            label: qsTr("Temps de montée et de descente")
            choices: [qsTr("De 10 % à 90 % de l'amplitude"), qsTr("De 20 % à 80 % (fronts lents ou bruités)")]
            value: (root.s.edgeLow ?? 0.1) === 0.2 ? 1 : 0
            onEdited: (index) => root.set({ edgeLow: index === 1 ? 0.2 : 0.1 })
        }

        SectionTitle { text: qsTr("Mesures") }
        Hint {
            visible: root.analysis.zoneText.length > 0
            text: qsTr("Sur %1").arg(root.analysis.zoneText)
        }
        Hint {
            visible: root.analysis.error.length > 0
            text: root.analysis.error
            color: Theme.warning
        }
        Hint {
            visible: root.analysis.measures.length === 0 && root.analysis.error.length === 0
            text: root.analysis.busy ? qsTr("Calcul…") : qsTr("Choisissez un signal dans la liste de gauche.")
        }

        Repeater {
            model: root.analysis.measures
            delegate: ColumnLayout {
                id: card
                required property var modelData
                Layout.fillWidth: true
                spacing: 4

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    spacing: 6
                    Rectangle {
                        width: 10
                        height: 4
                        radius: 1.5
                        color: Theme.channelColors[card.modelData.channel % Theme.channelColors.length]
                    }
                    Label {
                        Layout.fillWidth: true
                        text: card.modelData.unit.length > 0 ? "%1 (%2)".arg(card.modelData.name).arg(card.modelData.unit) : card.modelData.name
                        color: Theme.text
                        font.pixelSize: Theme.bodyText
                        font.bold: true
                        elide: Text.ElideRight
                    }
                }
                Repeater {
                    model: card.modelData.values
                    delegate: Measure {
                        required property var modelData
                        label: modelData.label
                        value: modelData.value
                    }
                }
                Label {
                    visible: card.modelData.toneText.length > 0
                    Layout.fillWidth: true
                    Layout.topMargin: 2
                    wrapMode: Text.WordWrap
                    text: card.modelData.toneText
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                    font.italic: true
                }
                Label {
                    visible: card.modelData.tone
                    Layout.topMargin: 4
                    text: qsTr("Tonalité")
                    color: Theme.accent
                    font.pixelSize: Theme.smallText
                    font.bold: true
                }
                Repeater {
                    model: card.modelData.toneValues
                    delegate: Measure {
                        required property var modelData
                        label: modelData.label
                        value: modelData.value
                    }
                }
                Label {
                    visible: card.modelData.harmonics.length > 0
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    wrapMode: Text.WordWrap
                    text: qsTr("En clair : les harmoniques qui dépassent d'environ 10 dB le spectre autour d'elles, repérées sur le spectre ; en gris : noyées dans ce qui les entoure (mesure incertaine).")
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText - 1
                }
                // Harmonics: order, frequency, amplitude, level relative to the fundamental.
                GridLayout {
                    visible: card.modelData.harmonics.length > 0
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    columns: 4
                    columnSpacing: 8
                    rowSpacing: 1
                    Repeater {
                        model: [qsTr("Rang"), qsTr("Fréquence"), qsTr("Amplitude"), qsTr("Relatif")]
                        delegate: Label {
                            required property string modelData
                            required property int index
                            Layout.fillWidth: index === 1
                            horizontalAlignment: index === 0 ? Text.AlignLeft : Text.AlignRight
                            text: modelData
                            color: Theme.textMuted
                            font.pixelSize: Theme.smallText - 1
                        }
                    }
                    Repeater {
                        // Flattened: four cells per harmonic. Those at the level of the noise are greyed.
                        model: {
                            const cells = []
                            for (const h of card.modelData.harmonics) {
                                for (const v of [h.order, h.frequency, h.amplitude, h.relative])
                                    cells.push({ text: v, clear: h.clear })
                            }
                            return cells
                        }
                        delegate: Label {
                            required property var modelData
                            required property int index
                            Layout.fillWidth: index % 4 === 1
                            horizontalAlignment: index % 4 === 0 ? Text.AlignLeft : Text.AlignRight
                            text: modelData.text
                            color: index % 4 === 0 || !modelData.clear ? Theme.textMuted : Theme.text
                            opacity: modelData.clear ? 1.0 : 0.7
                            font.pixelSize: Theme.smallText
                            font.family: Theme.monospace
                        }
                    }
                }
                // What an oscilloscope measures on its own.
                Label {
                    visible: card.modelData.wave.length > 0 || card.modelData.waveText.length > 0
                    Layout.topMargin: 4
                    text: qsTr("Mesures temporelles")
                    color: Theme.accent
                    font.pixelSize: Theme.smallText
                    font.bold: true
                }
                Repeater {
                    model: card.modelData.wave
                    delegate: Measure {
                        required property var modelData
                        label: modelData.label
                        value: modelData.value
                    }
                }
                Label {
                    visible: card.modelData.waveText.length > 0
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: card.modelData.waveText
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                    font.italic: true
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    height: 1
                    color: Theme.border
                }
            }
        }

        // The highest peaks of each curve of the spectrum.
        SectionTitle {
            visible: root.analysis.curves.length > 0
            text: qsTr("Pics du spectre")
        }
        Repeater {
            model: root.analysis.curves
            delegate: ColumnLayout {
                id: peakCard
                required property var modelData
                Layout.fillWidth: true
                spacing: 1
                RowLayout {
                    spacing: 6
                    Rectangle {
                        width: 10
                        height: 4
                        radius: 1.5
                        color: Theme.channelColors[peakCard.modelData.channel % Theme.channelColors.length]
                    }
                    Label {
                        text: peakCard.modelData.name
                        color: Theme.text
                        font.pixelSize: Theme.bodyText
                        font.bold: true
                    }
                }
                Label {
                    visible: (peakCard.modelData.band ?? "").length > 0
                    Layout.fillWidth: true
                    Layout.bottomMargin: 2
                    text: peakCard.modelData.band ?? ""
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                    ToolTip.visible: bandHover.hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Où la courbe reste à moins de 3 dB de son maximum (moitié de la puissance). Sur un filtre ou un bruit coloré : sa bande passante ; sur une raie pure, c'est seulement la largeur du lobe de la fenêtre.")
                    HoverHandler { id: bandHover }
                }
                Repeater {
                    model: peakCard.modelData.peaks
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            text: "%1.".arg(modelData.rank)
                            color: Theme.textMuted
                            font.pixelSize: Theme.smallText
                            font.family: Theme.monospace
                        }
                        Label {
                            Layout.fillWidth: true
                            text: modelData.frequency
                            color: Theme.text
                            font.pixelSize: Theme.smallText
                            font.family: Theme.monospace
                        }
                        Label {
                            text: modelData.level
                            color: Theme.text
                            font.pixelSize: Theme.smallText
                            font.family: Theme.monospace
                        }
                    }
                }
            }
        }
        Item { Layout.preferredHeight: 8 }
    }
}
