import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// Tab "Composition" of the right panel of the module "Générateur": the recipe of the signal. Its name,
// rate and duration (the seed of its noises), its channels (name, unit, offset, whether it is among the
// channels of the signal made), and for the channel chosen its waveforms, which add up; what the generation
// warns about; presets. Every field writes into
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

        // Right click on a chip: what can be done with that channel (a popup: not laid out; inside the column, so
        // that the scroll view keeps a single child, whose height it reads).
        FitMenu {
            id: channelMenu
            property int target: 0
            readonly property var info: root.channels[target] ?? ({})
            readonly property bool hasWaves: (info.components ?? []).length > 0
            readonly property bool kept: (info.output ?? true) !== false
            function openFor(index, item) {
                target = index
                popup(item, 0, item.height)
            }
            MenuItem {
                text: qsTr("Vider la voie")
                enabled: channelMenu.hasWaves || (channelMenu.info.offset ?? 0) !== 0
                onTriggered: root.generator.clearChannel(channelMenu.target)
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: qsTr("Sans forme d'onde ni décalage : une voie vierge, qui garde son nom et sa place")
            }
            MenuItem {
                text: qsTr("Supprimer la voie")
                enabled: root.channelCount > 1
                onTriggered: root.generator.removeChannel(channelMenu.target)
            }
            MenuSeparator { }
            MenuItem {
                text: channelMenu.kept ? qsTr("Hors du signal (voie de calcul)") : qsTr("Dans le signal")
                onTriggered: root.generator.setChannel(channelMenu.target, { output: !channelMenu.kept })
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: qsTr("Une voie de calcul est faite et lue par les autres (v%1), mais ni tracée ni ajoutée à la session").arg(channelMenu.target + 1)
            }
            MenuItem {
                text: qsTr("Moduler une porteuse avec cette voie (AM)")
                enabled: channelMenu.hasWaves
                onTriggered: root.generator.modulateChannel(channelMenu.target, "am")
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: qsTr("Une voie de plus : une porteuse modulée en amplitude dont le message est cette voie, lue telle quelle (v%1) ; sa carte règle aussi FM, PM, DBL, BLU").arg(channelMenu.target + 1)
            }
        }

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
                    // Left out of the signal: made, read by the others, not added.
                    readonly property bool kept: ((root.channels[chip.index] ?? {}).output ?? true) !== false
                    // Its cross: there is always a channel left.
                    readonly property bool removable: root.channelCount > 1
                    width: chipRow.implicitWidth + (removable ? 14 : 18)
                    height: 26
                    radius: 13
                    color: chosen ? Qt.alpha(Theme.accent, 0.18) : chipArea.containsMouse ? Qt.alpha(Theme.text, 0.06) : "transparent"
                    border.color: chosen ? Theme.accent : Theme.border
                    ToolTip.visible: chipArea.containsMouse && !crossArea.containsMouse
                    ToolTip.delay: 700
                    ToolTip.text: (chip.kept ? "" : qsTr("Voie de calcul : lue par les autres, hors du signal ajouté.") + "\n")
                                  + qsTr("Clic droit : vider la voie, la supprimer, moduler une porteuse avec elle…")
                    // Under the cross, which takes its own clicks.
                    MouseArea {
                        id: chipArea
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: (mouse) => {
                            root.channelSelected(chip.index)
                            if (mouse.button === Qt.RightButton)
                                channelMenu.openFor(chip.index, chip)
                        }
                    }
                    Row {
                        id: chipRow
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 9
                        spacing: 6
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 8
                            height: 8
                            radius: 4
                            color: chip.kept ? Theme.channelColors[chip.index % Theme.channelColors.length] : "transparent"
                            border.color: chip.kept ? "transparent" : Theme.textMuted
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: (root.channels[chip.index] ?? {}).name ?? ""
                            font.pixelSize: Theme.smallText
                            font.italic: !chip.kept
                            color: chip.chosen ? Theme.text : Theme.textMuted
                        }
                        // The channel taken away, its waveforms with it.
                        Label {
                            visible: chip.removable
                            anchors.verticalCenter: parent.verticalCenter
                            text: "×"
                            font.pixelSize: Theme.smallText + 3
                            color: crossArea.containsMouse ? Theme.error : Theme.textMuted
                            MouseArea {
                                id: crossArea
                                anchors.fill: parent
                                anchors.margins: -4
                                hoverEnabled: true
                                onClicked: root.generator.removeChannel(chip.index)
                                ToolTip.visible: containsMouse
                                ToolTip.delay: 500
                                ToolTip.text: qsTr("Supprimer « %1 » et ses formes d'onde").arg((root.channels[chip.index] ?? {}).name ?? "")
                            }
                        }
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
        QuantityField {
            label: qsTr("Décalage")
            unit: root.currentChannel.unit ?? ""
            value: root.currentChannel.offset ?? 0
            tip: qsTr("Ajouté à la somme des formes d'onde (composante continue)")
            onEdited: (v) => root.generator.setChannel(root.current, { offset: v })
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            Button {
                text: qsTr("Vider la voie")
                flat: true
                enabled: root.waves.length > 0 || (root.currentChannel.offset ?? 0) !== 0
                onClicked: root.generator.clearChannel(root.current)
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("« %1 » sans forme d'onde ni décalage : une voie vierge, qui garde son nom et sa place").arg(root.currentChannel.name ?? "")
            }
            Button {
                text: qsTr("Supprimer la voie")
                flat: true
                enabled: root.channelCount > 1
                onClicked: root.generator.removeChannel(root.current)
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: root.channelCount > 1
                              ? qsTr("Supprimer « %1 » et ses formes d'onde (aussi : la croix de sa pastille, ou clic droit dessus). Une voie lue par l'opération ou le message d'une autre reste : changez d'abord sa formule, ou décochez « Dans le signal » pour qu'elle n'y soit pas").arg(root.currentChannel.name ?? "")
                              : qsTr("Il reste toujours une voie : « Vider la voie » la remet à zéro")
            }
            Item { Layout.fillWidth: true }
        }
        CheckBox {
            text: qsTr("Dans le signal")
            checked: (root.currentChannel.output ?? true) !== false
            onToggled: root.generator.setChannel(root.current, { output: checked })
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: qsTr("Décochée, la voie est une voie de calcul : toujours faite et lue par les opérations des autres (v%1), mais ni tracée ni ajoutée à la session. Une porteuse, un message, une étape…").arg(root.current + 1)
        }
        Hint {
            visible: (root.currentChannel.output ?? true) === false
            text: qsTr("Voie de calcul : les autres la lisent (v%1), le signal ajouté ne la contient pas.").arg(root.current + 1)
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
