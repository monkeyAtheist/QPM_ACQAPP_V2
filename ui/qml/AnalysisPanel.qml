import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// Tab "Mesures" of the right panel of the module "Signaux": the measurements of every channel of the zone
// analysed (statistics, tone: frequency, THD, SINAD, SNR, SFDR, ENOB, harmonics; what an oscilloscope
// measures), the peaks of the spectra added, then the settings of the computations (remembered from one
// run to the next), the spectrograms' among them when there are some.
ScrollView {
    id: root
    required property var analysis
    required property var appSettings
    property var curves: null // CurvesController: are there spectrograms
    property real sampleRate: 0 // of the signal chosen: Δf and duration of a segment
    readonly property bool hasSpectrograms: curves !== null && curves.hasSpectrograms === true

    readonly property var s: analysis.settings

    contentWidth: availableWidth
    clip: true

    function set(values) {
        analysis.setSettings(values)
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

        SectionTitle { text: qsTr("Mesures") }
        Hint {
            visible: root.analysis.zoneText.length > 0
            text: qsTr("Sur %1 (zone : Courbes ▸ Zone analysée)").arg(root.analysis.zoneText)
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
        SectionTitle { text: qsTr("Réglages des calculs") }
        ChoiceField {
            label: qsTr("Temps de montée et de descente")
            choices: [qsTr("De 10 % à 90 % de l'amplitude"), qsTr("De 20 % à 80 % (fronts lents ou bruités)")]
            value: (root.s.edgeLow ?? 0.1) === 0.2 ? 1 : 0
            onEdited: (index) => root.set({ edgeLow: index === 1 ? 0.2 : 0.1 })
        }
        // The same controls as the menus of the spectra and spectrograms (right click on them).
        SpectrumSettings {
            analysis: root.analysis
        }
        SpectrogramSettings {
            visible: root.hasSpectrograms
            analysis: root.analysis
            sampleRate: root.sampleRate
        }

        Item { Layout.preferredHeight: 8 }
    }
}
