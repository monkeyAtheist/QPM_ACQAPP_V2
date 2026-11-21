import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// How the spectra are computed (settings of the analysis, remembered from one run to the next): amplitude
// or density, window, zeros added or segments and their overlap, mean removed, harmonics measured. In the
// panel Mesures (with what each one does) and in the menu of the spectra, a right click on them (compact).
ColumnLayout {
    id: root
    required property var analysis
    property bool compact: false
    readonly property var s: analysis.settings
    readonly property bool density: s.kind === "density"
    readonly property var paddings: [1, 2, 4, 8]
    readonly property var segments: [256, 512, 1024, 2048, 4096, 8192, 16384, 65536]
    readonly property var overlaps: [0, 0.25, 0.5, 0.75]
    readonly property var harmonicCounts: [5, 10, 20]

    function set(values) {
        analysis.setSettings(values)
    }
    function indexOf(list, value) {
        return Math.max(0, list.indexOf(value))
    }

    component Hint: Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textMuted
        font.pixelSize: Theme.smallText
    }

    spacing: 6
    Layout.fillWidth: true

    Label {
        visible: !root.compact
        Layout.topMargin: 4
        text: root.density ? qsTr("Spectres : densité spectrale (Welch)") : qsTr("Spectres : amplitude")
        color: Theme.accent
        font.pixelSize: Theme.smallText
        font.bold: true
    }
    Hint {
        visible: !root.compact
        text: root.density
              ? qsTr("La zone est coupée en segments qui se recouvrent ; leurs spectres sont moyennés. Le bruit se lit en unité/√Hz, indépendamment de la longueur des segments.")
              : qsTr("Une seule FFT de toute la zone : une sinusoïde se lit à son amplitude crête. Plus la zone est longue, plus les fréquences sont fines (Δf = fréquence d'échantillonnage / nombre d'échantillons).")
    }
    ChoiceField {
        label: qsTr("Spectre")
        choices: [qsTr("Amplitude (une FFT de toute la zone)"), qsTr("Densité (Welch : segments moyennés)")]
        value: root.density ? 1 : 0
        onEdited: (index) => root.set({ kind: index === 1 ? "density" : "amplitude" })
    }
    WindowField {
        analysis: root.analysis
    }
    Hint {
        text: qsTr("Une sinusoïde qui ne tombe pas pile sur une fréquence de la FFT se lit un peu bas avec Hann (jusqu'à −1,4 dB) ; la fenêtre à sommet plat donne l'amplitude exacte, au prix de pics plus larges.")
        visible: !root.compact && !root.density && root.s.window === "hann"
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
        ToolTip.text: qsTr("Sans elle, une tension continue fait un grand pic à 0 Hz qui peut cacher les basses fréquences (spectres et spectrogrammes). Elle reste mesurée (Moyenne).")
    }
    ChoiceField {
        visible: !root.density
        label: qsTr("Harmoniques mesurées (fondamentale comprise)")
        choices: root.harmonicCounts.map(n => String(n))
        value: root.indexOf(root.harmonicCounts, root.s.harmonics)
        onEdited: (index) => root.set({ harmonics: root.harmonicCounts[index] })
    }
}
