import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// Tab "Composition" of the right panel of the module "Générateur": the recipe of the signal. Its name,
// rate and duration (the seed of its noises), its channels (name, unit, offset), and for the channel
// chosen its waveforms, which add up; what the generation warns about; presets. Every field writes into
// the recipe at once, and the preview follows.
ScrollView {
    id: root
    required property var generator
    required property var appSettings
    property int channel: 0
    signal channelSelected(int index)

    readonly property var recipe: generator.recipe
    readonly property var channels: recipe.channels ?? []
    readonly property int channelCount: channels.length
    readonly property int current: Math.max(0, Math.min(channel, channelCount - 1))
    readonly property var currentChannel: channels[current] ?? ({})
    readonly property var waves: currentChannel.components ?? []
    property var presetNames: []

    contentWidth: availableWidth
    clip: true

    function refreshPresets() {
        presetNames = appSettings.presetNames("generator")
    }
    Component.onCompleted: refreshPresets()
    Connections {
        target: root.appSettings
        function onPresetsChanged(category) { if (category === "generator") root.refreshPresets() }
    }
    // A channel taken away: the one chosen stays in the list.
    onChannelCountChanged: if (channel >= channelCount) root.channelSelected(Math.max(0, channelCount - 1))

    component Hint: Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textMuted
        font.pixelSize: Theme.smallText
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: 10

        // ---- The signal ----
        SectionTitle { text: qsTr("Signal") }
        TextValueField {
            label: qsTr("Nom du signal")
            value: root.recipe.name ?? ""
            placeholder: qsTr("Signal généré")
            tip: qsTr("Le nom qu'il aura dans la session (un numéro s'y ajoute s'il est déjà pris)")
            onEdited: (text) => root.generator.setSignalSettings({ name: text.trim() })
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            QuantityField {
                label: qsTr("Échantillonnage")
                unit: "Hz"
                minimum: 0.001
                maximum: 1e11
                value: root.recipe.sampleRate ?? 48000
                tip: qsTr("Fréquence d'échantillonnage fs : les fréquences au-delà de fs/2 se replient")
                onEdited: (v) => root.generator.setSignalSettings({ sampleRate: v })
            }
            QuantityField {
                label: qsTr("Durée")
                unit: "s"
                minimum: 1e-9
                maximum: 1e9
                value: root.recipe.duration ?? 1
                onEdited: (v) => root.generator.setSignalSettings({ duration: v })
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.generator.problem.length > 0 ? root.generator.problem : root.generator.sizeText
            color: root.generator.problem.length > 0 ? Theme.error : Theme.textMuted
            font.pixelSize: Theme.smallText
        }

        // ---- The channels ----
        SectionTitle { text: qsTr("Voies") }
        Flow {
            Layout.fillWidth: true
            spacing: 6
            Repeater {
                model: root.channelCount
                delegate: Rectangle {
                    id: chip
                    required property int index
                    readonly property bool chosen: index === root.current
                    width: chipRow.implicitWidth + 18
                    height: 26
                    radius: 13
                    color: chosen ? Qt.alpha(Theme.accent, 0.18) : chipArea.containsMouse ? Qt.alpha(Theme.text, 0.06) : "transparent"
                    border.color: chosen ? Theme.accent : Theme.border
                    Row {
                        id: chipRow
                        anchors.centerIn: parent
                        spacing: 6
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 8
                            height: 8
                            radius: 4
                            color: Theme.channelColors[chip.index % Theme.channelColors.length]
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: (root.channels[chip.index] ?? {}).name ?? ""
                            font.pixelSize: Theme.smallText
                            color: chip.chosen ? Theme.text : Theme.textMuted
                        }
                    }
                    MouseArea {
                        id: chipArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: root.channelSelected(chip.index)
                    }
                }
            }
            ToolButton {
                text: "+"
                height: 26
                enabled: root.channelCount < 16
                onClicked: {
                    const index = root.generator.addChannel()
                    if (index >= 0)
                        root.channelSelected(index)
                }
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Ajouter une voie (16 au plus)")
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            TextValueField {
                label: qsTr("Nom de la voie")
                value: root.currentChannel.name ?? ""
                onEdited: (text) => root.generator.setChannel(root.current, { name: text.trim() })
            }
            TextValueField {
                Layout.fillWidth: false
                Layout.preferredWidth: 80
                label: qsTr("Unité")
                value: root.currentChannel.unit ?? ""
                tip: qsTr("V, A, Pa, °C… ou rien pour un nombre sans unité")
                onEdited: (text) => root.generator.setChannel(root.current, { unit: text.trim() })
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            QuantityField {
                label: qsTr("Décalage")
                unit: root.currentChannel.unit ?? ""
                value: root.currentChannel.offset ?? 0
                tip: qsTr("Ajouté à la somme des formes d'onde (composante continue)")
                onEdited: (v) => root.generator.setChannel(root.current, { offset: v })
            }
            Button {
                Layout.alignment: Qt.AlignBottom
                text: qsTr("Retirer la voie")
                flat: true
                enabled: root.channelCount > 1
                onClicked: root.generator.removeChannel(root.current)
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Retirer « %1 » et ses formes d'onde (il reste toujours une voie ; une voie lue par l'opération d'une autre reste)").arg(root.currentChannel.name ?? "")
            }
        }

        // ---- The waveforms of the channel chosen ----
        SectionTitle { text: qsTr("Formes d'onde de « %1 »").arg(root.currentChannel.name ?? "") }
        Hint {
            text: root.waves.length === 0
                  ? qsTr("Aucune forme d'onde : la voie vaut son décalage. Ajoutez un sinus, un bruit, un échelon…")
                  : qsTr("Elles s'additionnent, puis le décalage : un sinus et un bruit donnent un sinus bruité.")
        }
        Repeater {
            model: root.waves.length
            delegate: WaveCard {
                generator: root.generator
                channel: root.current
                count: root.waves.length
                wave: root.waves[index] ?? ({})
                unit: root.currentChannel.unit ?? ""
            }
        }
        Button {
            id: addButton
            text: qsTr("Ajouter une forme d'onde ▾")
            enabled: root.waves.length < 32
            onClicked: addMenu.popup(addButton, 0, addButton.height)
            FitMenu {
                id: addMenu
                Instantiator {
                    model: Choices.generatorWaveforms
                    delegate: MenuItem {
                        required property var modelData
                        text: modelData.name
                        onTriggered: root.generator.addComponent(root.current, modelData.key)
                        ToolTip.visible: hovered
                        ToolTip.delay: 400
                        ToolTip.text: modelData.tip
                    }
                    onObjectAdded: (index, object) => addMenu.insertItem(index, object)
                    onObjectRemoved: (index, object) => addMenu.removeItem(object)
                }
            }
        }

        // ---- What the generation says ----
        SectionTitle {
            visible: root.generator.warnings.length > 0 || (root.generator.error.length > 0 && root.generator.problem.length === 0)
            text: qsTr("Avertissements")
        }
        Label {
            visible: root.generator.error.length > 0 && root.generator.problem.length === 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.generator.error
            color: Theme.error
            font.pixelSize: Theme.smallText
        }
        Repeater {
            model: root.generator.warnings
            delegate: Label {
                required property string modelData
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: "⚠ " + modelData
                color: Theme.warning
                font.pixelSize: Theme.smallText
            }
        }

        // ---- Noise ----
        SectionTitle { text: qsTr("Bruit") }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Graine : la même graine redonne le même bruit ; une autre, un autre tirage.")
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
            }
            SpinBox {
                Layout.preferredWidth: 120
                from: 0
                to: 2147483647
                editable: true
                value: Math.min(2147483647, root.recipe.seed ?? 1)
                onValueModified: root.generator.setSignalSettings({ seed: value })
            }
        }

        // ---- Presets ----
        SectionTitle { text: qsTr("Préréglages") }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            ComboBox {
                id: presetBox
                Layout.fillWidth: true
                model: root.presetNames
                displayText: count > 0 ? currentText : qsTr("Aucun préréglage")
                enabled: count > 0
            }
            Button {
                text: qsTr("Charger")
                enabled: presetBox.count > 0
                onClicked: {
                    root.generator.setRecipe(root.appSettings.preset("generator", presetBox.currentText))
                    root.channelSelected(0)
                }
            }
            ToolButton {
                text: "×"
                enabled: presetBox.count > 0
                onClicked: root.appSettings.removePreset("generator", presetBox.currentText)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Supprimer ce préréglage")
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            TextField {
                id: presetName
                Layout.fillWidth: true
                selectByMouse: true
                placeholderText: text.length === 0 && !activeFocus ? qsTr("Nom du préréglage (ex. « Sinus bruité 1 kHz »)") : ""
            }
            Button {
                text: qsTr("Enregistrer")
                enabled: presetName.text.trim().length > 0
                onClicked: {
                    root.appSettings.savePreset("generator", presetName.text.trim(), root.generator.recipe)
                    presetName.text = ""
                }
            }
        }
        Item { Layout.preferredHeight: 8 }
    }
}
