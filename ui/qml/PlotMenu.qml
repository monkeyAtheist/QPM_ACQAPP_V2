import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The menu of a plot, on a right click (a right drag still zooms): what the plot shows, one box to tick
// per channel or curve, whether they are superposed, and a few actions; beside them, for a plot of what
// is computed (spectra, spectrograms), how it is computed (`settings`, given by the page). It stays open
// while boxes are ticked and settings changed; a click outside or Échap closes it.
//   mode "channels": the channels of a SignalPlot (`view`), hidden through its hiddenChannels;
//   mode "time" / "spectrogram" / "frequency": the curves of that graph of the module "Signaux" (`curves`, a
//   CurvesController).
// (No property here is named "on" + a capital letter: QML takes such a name for the handler of a signal
// and never evaluates its binding.)
Popup {
    id: root
    property var view: null // the SignalPlot or SpectrumPlot (never named like the id it is given)
    property var curves: null
    property string mode: "channels"
    // Extra actions: [{ key, text }]; actionTriggered(key) when one is clicked (the menu closes).
    property var actions: []
    signal actionTriggered(string key)
    // How what is drawn is computed, in a column beside: a component of the page, and its title.
    property Component settings: null
    property string settingsTitle: ""

    readonly property bool listsCurves: mode !== "channels"
    readonly property bool stackable: mode !== "frequency" && mode !== "spectrogram" && view !== null && view.stacked !== undefined
    readonly property var items: {
        const colors = Theme.channelColors
        if (!listsCurves) {
            if (!view || !view.channelNames || !view.hiddenChannels)
                return []
            const hidden = view.hiddenChannels
            return view.channelNames.map((name, i) => ({ key: i, name: name, color: colors[i % colors.length],
                                                         shown: hidden.indexOf(i) < 0 }))
        }
        if (!curves)
            return []
        return curves.curves.filter((c) => c.graph === root.mode)
                            .map((c) => ({ key: c.id, name: c.name + (c.part.length > 0 ? " · " + c.part : ""),
                                           color: colors[c.colorIndex % colors.length], shown: c.visible }))
    }

    function openAt(item, x, y) {
        const p = item.mapToItem(root.parent, x, y)
        root.x = p.x
        root.y = p.y
        open()
    }
    function setShown(key, shown) {
        if (listsCurves) {
            curves.setVisible(key, shown)
            return
        }
        const hidden = view.hiddenChannels.filter((c) => c !== key)
        if (!shown)
            hidden.push(key)
        view.hiddenChannels = hidden
    }
    function showEverything() {
        if (listsCurves) {
            for (const item of items)
                if (!item.shown)
                    curves.setVisible(item.key, true)
        } else {
            view.hiddenChannels = []
        }
    }

    parent: Overlay.overlay
    margins: 8
    padding: 10
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle {
        color: Theme.panel
        border.color: Theme.border
        radius: 6
    }

    contentItem: RowLayout {
        spacing: 14
        ColumnLayout {
            Layout.alignment: Qt.AlignTop
            spacing: 2
            Label {
                text: root.mode === "frequency" ? qsTr("Spectres affichés")
                      : root.mode === "spectrogram" ? qsTr("Spectrogrammes affichés")
                      : root.listsCurves ? qsTr("Courbes affichées") : qsTr("Voies affichées")
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
                font.bold: true
            }
            Label {
                visible: root.items.length === 0
                text: qsTr("Aucune")
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
            }
            Repeater {
                model: root.items
                delegate: RowLayout {
                    id: line
                    required property var modelData
                    spacing: 2
                    Rectangle {
                        Layout.leftMargin: 4
                        width: 10
                        height: 4
                        radius: 1.5
                        color: line.modelData.color
                    }
                    CheckBox {
                        text: line.modelData.name
                        checked: line.modelData.shown
                        onToggled: root.setShown(line.modelData.key, checked)
                    }
                }
            }
            MenuSeparator {
                visible: root.stackable
                Layout.fillWidth: true
            }
            CheckBox {
                visible: root.stackable
                text: qsTr("Superposées")
                checked: root.view !== null && !root.view.stacked
                onToggled: root.view.stacked = !checked
            }
            Label {
                visible: root.stackable
                Layout.maximumWidth: 320
                Layout.leftMargin: 8
                wrapMode: Text.WordWrap
                text: qsTr("Deux unités au plus sur un même tracé (échelles à gauche et à droite) ; au-delà, une bande par unité. Décochez des courbes pour en superposer deux.")
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
            }
            MenuSeparator { Layout.fillWidth: true }
            ItemDelegate {
                Layout.fillWidth: true
                enabled: root.items.some((item) => !item.shown)
                text: root.listsCurves ? qsTr("Tout afficher") : qsTr("Afficher toutes les voies")
                font.pixelSize: Theme.bodyText
                onClicked: root.showEverything()
            }
            ItemDelegate {
                Layout.fillWidth: true
                enabled: root.view !== null && root.view.manualScales === true
                text: root.mode === "spectrogram" ? qsTr("Fréquences et couleurs automatiques") : qsTr("Échelle verticale automatique")
                font.pixelSize: Theme.bodyText
                onClicked: {
                    root.close()
                    root.view.autoScale()
                }
            }
            ItemDelegate {
                Layout.fillWidth: true
                enabled: root.view !== null && (root.view.zoomed || root.view.manualScales === true)
                text: qsTr("Tout voir (sans zoom)")
                font.pixelSize: Theme.bodyText
                onClicked: {
                    root.close()
                    root.view.showAll()
                }
            }
            Repeater {
                model: root.actions
                delegate: ItemDelegate {
                    required property var modelData
                    Layout.fillWidth: true
                    text: modelData.text
                    font.pixelSize: Theme.bodyText
                    onClicked: {
                        root.close()
                        root.actionTriggered(modelData.key)
                    }
                }
            }
        }
        Rectangle {
            visible: root.settings !== null
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            color: Theme.border
        }
        ColumnLayout {
            visible: root.settings !== null
            Layout.alignment: Qt.AlignTop
            Layout.preferredWidth: 300
            spacing: 4
            Label {
                text: root.settingsTitle
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
                font.bold: true
            }
            Loader {
                Layout.fillWidth: true
                sourceComponent: root.settings
            }
        }
    }
}
