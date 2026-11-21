import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The window of the spectra and the spectrograms (one setting of the analysis for both), and its β when
// it is Kaiser's. In the panel Mesures and in the menus of the spectra and spectrograms.
ColumnLayout {
    id: root
    required property var analysis
    readonly property var s: analysis.settings
    readonly property var windows: [
        { key: "hann", name: qsTr("Hann (usage courant)") },
        { key: "flatTop", name: qsTr("À sommet plat (amplitudes exactes)") },
        { key: "blackmanHarris", name: qsTr("Blackman-Harris (−92 dB : petits signaux)") },
        { key: "blackman", name: qsTr("Blackman") },
        { key: "hamming", name: qsTr("Hamming") },
        { key: "kaiser", name: qsTr("Kaiser (β réglable)") },
        { key: "rectangular", name: qsTr("Rectangulaire (aucune)") }
    ]

    spacing: 2
    Layout.fillWidth: true

    ChoiceField {
        label: qsTr("Fenêtre")
        choices: root.windows.map(w => w.name)
        value: Math.max(0, root.windows.findIndex(w => w.key === root.s.window))
        onEdited: (index) => root.analysis.setSettings({ window: root.windows[index].key })
    }
    ColumnLayout {
        visible: root.s.window === "kaiser"
        Layout.fillWidth: true
        Layout.topMargin: 4
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
            onMoved: root.analysis.setSettings({ kaiserBeta: value })
        }
    }
}
