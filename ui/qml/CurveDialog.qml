import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// Adds curves under the signal of the module "Signaux": a spectrum (graph of the frequencies), a
// spectrogram (its own graph, on the time axis of the signal), or a curve of time (graph of time, on the
// time axis of the signal): the channel itself, its integral, derivative, or one value per period. The curves follow the signal chosen and are computed again when it changes;
// "Garder comme signal" makes a signal of the session of them.
Dialog {
    id: root
    required property var session
    required property var curves // the CurvesController
    property var signalId: 0
    property var plot: null // the SignalPlot whose cursors A and B can bound the part
    property var info: ({})
    property string kind: "spectrum"
    property bool betweenCursors: false
    property bool withSources: true
    property bool removeMean: false
    property var chosen: []
    signal added(int count)

    // Computed on a part of the signal (all of it, or between A and B): all but the spectra (on the zone
    // analysed) and the channels themselves.
    readonly property bool hasPart: kind !== "spectrum" && kind !== "source"
    readonly property bool computedTime: hasPart && kind !== "spectrogram"
    readonly property bool cursorsReady: plot !== null && plot.cursorsVisible && plot.cursorFrameCount > 1
    readonly property var kinds: [
        { key: "spectrum", name: qsTr("Spectre"),
          text: qsTr("Dans le graphe des fréquences : amplitude ou densité (groupe Spectre), sur la zone analysée.") },
        { key: "spectrogram", name: qsTr("Spectrogramme"),
          text: qsTr("Le spectre au fil du temps, dans son graphe sous le signal : les fréquences en hauteur, le niveau en couleurs. Long sur un grand signal : la progression est affichée.") },
        { key: "source", name: qsTr("Voie d'origine"),
          text: qsTr("La voie telle quelle, dans le graphe du temps, pour y superposer d'autres courbes.") },
        { key: "integral", name: qsTr("Intégrale  ∫ x dt"),
          text: qsTr("L'aire accumulée depuis le début de la partie, partant de 0 (V devient V·s).") },
        { key: "derivative", name: qsTr("Dérivée  dx/dt"),
          text: qsTr("La pente en chaque point (V devient V/s) ; le bruit y est amplifié.") },
        { key: "cycleMean", name: qsTr("Moyenne par période"),
          text: qsTr("Une valeur par période, d'un front montant au suivant : suit une dérive lente.") },
        { key: "cycleRms", name: qsTr("Efficace par période"),
          text: qsTr("La valeur efficace de chaque période : suit une variation d'amplitude.") },
        { key: "cycleFrequency", name: qsTr("Fréquence par période"),
          text: qsTr("1 / durée de chaque période : suit un balayage ou une dérive de fréquence.") }
    ]

    function openFor(id, view) {
        signalId = id
        plot = view
        info = session.signalList.info(id)
        const n = (info.channels ?? []).length
        const all = []
        for (let i = 0; i < n; ++i)
            all.push(i)
        chosen = all
        betweenCursors = false
        open()
    }
    function toggle(channel, on) {
        const list = chosen.filter((c) => c !== channel)
        if (on)
            list.push(channel)
        list.sort((a, b) => a - b)
        chosen = list
    }

    title: qsTr("Ajouter des courbes sous « %1 »").arg(info.name ?? "")
    modal: true
    anchors.centerIn: Overlay.overlay
    // A width given: with its row of buttons, the dialog would otherwise loop on its implicit width.
    width: 560
    onAccepted: {
        const options = { withSources: root.computedTime && root.withSources, removeMean: root.removeMean }
        if (root.hasPart && root.betweenCursors && root.cursorsReady) {
            options.firstFrame = root.plot.cursorFirstFrame
            options.frameCount = root.plot.cursorFrameCount
        }
        root.added(root.curves.add(root.kind, root.chosen, options))
    }

    footer: DialogButtonBox {
        Button {
            text: qsTr("Ajouter")
            highlighted: true
            enabled: root.chosen.length > 0
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
        Button {
            text: qsTr("Annuler")
            flat: true
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        SectionTitle { text: qsTr("Courbe") }
        ButtonGroup { id: kindGroup }
        Repeater {
            model: root.kinds
            delegate: ColumnLayout {
                id: choice
                required property var modelData
                Layout.fillWidth: true
                spacing: 0
                RadioButton {
                    text: choice.modelData.name
                    checked: root.kind === choice.modelData.key
                    ButtonGroup.group: kindGroup
                    onToggled: if (checked) root.kind = choice.modelData.key
                }
                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 36
                    text: choice.modelData.text
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
            }
        }
        CheckBox {
            visible: root.kind === "integral"
            Layout.leftMargin: 28
            text: qsTr("Retirer la moyenne d'abord (l'intégrale d'un signal décalé ne dérive pas)")
            checked: root.removeMean
            onToggled: root.removeMean = checked
        }

        SectionTitle { text: qsTr("Voies") }
        Flow {
            Layout.fillWidth: true
            spacing: 4
            Repeater {
                model: root.info.channels ?? []
                delegate: CheckBox {
                    required property var modelData
                    required property int index
                    text: modelData.unit.length > 0 ? "%1 (%2)".arg(modelData.name).arg(modelData.unit) : modelData.name
                    checked: root.chosen.indexOf(index) >= 0
                    onToggled: root.toggle(index, checked)
                }
            }
        }
        CheckBox {
            visible: root.computedTime
            text: qsTr("Ajouter aussi les voies d'origine, pour superposer les courbes")
            checked: root.withSources
            onToggled: root.withSources = checked
        }

        SectionTitle {
            visible: root.hasPart
            text: qsTr("Partie du signal")
        }
        ButtonGroup { id: partGroup }
        RadioButton {
            visible: root.hasPart
            text: qsTr("Tout le signal")
            checked: !root.betweenCursors
            ButtonGroup.group: partGroup
            onToggled: if (checked) root.betweenCursors = false
        }
        RadioButton {
            visible: root.hasPart
            enabled: root.cursorsReady
            text: root.cursorsReady
                  ? qsTr("Entre les curseurs A et B (%1 échantillons)").arg(Units.count(root.plot.cursorFrameCount))
                  : qsTr("Entre les curseurs A et B (affichez-les d'abord : Curseurs)")
            checked: root.betweenCursors
            ButtonGroup.group: partGroup
            onToggled: if (checked) root.betweenCursors = true
        }
        Label {
            Layout.fillWidth: true
            Layout.topMargin: 4
            wrapMode: Text.WordWrap
            text: qsTr("Les courbes suivent le signal choisi et se recalculent quand il change. « Garder », dans le groupe Courbes, en fait un signal de la session.")
            color: Theme.textMuted
            font.pixelSize: Theme.smallText
        }
    }
}
