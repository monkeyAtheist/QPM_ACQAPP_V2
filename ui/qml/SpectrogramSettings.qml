import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// How the spectrograms are computed: their segments and overlap (with Δf and the duration of a segment at
// the rate of the signal); the window and the mean removed are those of the spectra. In the panel Mesures
// (with what they do) and in the menu of the spectrograms, a right click on them (compact: the window and
// the mean there too).
ColumnLayout {
    id: root
    required property var analysis
    property real sampleRate: 0 // of the signal chosen
    property bool compact: false
    readonly property var s: analysis.settings
    readonly property var segments: [256, 512, 1024, 2048, 4096, 8192, 16384]
    readonly property var overlaps: [0, 0.5, 0.75, 0.875]

    function set(values) {
        analysis.setSettings(values)
    }
    function indexOf(list, value) {
        return Math.max(0, list.indexOf(value))
    }
    // "1 024 échantillons · Δf = 46,9 Hz · 21,3 ms" (the resolution at the rate of the signal chosen).
    function segmentText(n) {
        const count = qsTr("%1 échantillons").arg(Units.count(n))
        if (!(sampleRate > 0))
            return count
        return qsTr("%1 · Δf = %2 · %3").arg(count).arg(Units.format(sampleRate / n, "Hz", 3)).arg(Units.format(n / sampleRate, "s", 3))
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
        text: qsTr("Spectrogrammes")
        color: Theme.accent
        font.pixelSize: Theme.smallText
        font.bold: true
    }
    Hint {
        visible: !root.compact
        text: qsTr("Le signal est coupé en segments qui se recouvrent (même fenêtre que les spectres) : chaque colonne est le spectre d'un instant. Des segments longs séparent mieux les fréquences, des segments courts suivent mieux les changements rapides. Sur un long signal, une colonne moyenne plusieurs segments voisins : aucun échantillon n'est laissé de côté.")
    }
    ChoiceField {
        label: qsTr("Longueur des segments")
        choices: root.segments.map(n => root.segmentText(n))
        value: root.indexOf(root.segments, root.s.spectrogramSegment ?? 1024)
        onEdited: (index) => root.set({ spectrogramSegment: root.segments[index] })
    }
    ChoiceField {
        label: qsTr("Recouvrement des segments")
        choices: ["0 %", "50 %", "75 %", "87,5 %"]
        value: root.indexOf(root.overlaps, root.s.spectrogramOverlap ?? 0.75)
        onEdited: (index) => root.set({ spectrogramOverlap: root.overlaps[index] })
    }
    WindowField {
        visible: root.compact
        analysis: root.analysis
    }
    CheckBox {
        visible: root.compact
        text: qsTr("Retirer la moyenne (composante continue)")
        checked: root.s.removeMean ?? true
        onToggled: root.set({ removeMean: checked })
        ToolTip.visible: hovered
        ToolTip.delay: 500
        ToolTip.text: qsTr("Sans elle, une tension continue fait une bande vive à 0 Hz. Le même réglage que pour les spectres.")
    }
    Hint {
        visible: root.compact
        text: qsTr("La fenêtre et la moyenne retirée sont aussi celles des spectres.")
    }
}
