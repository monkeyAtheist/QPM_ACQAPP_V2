import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// Settings of the acquisition: the source and its link, the format of the data, the rate, the names of
// the channels, presets. Every field writes into the controller at once (acquisition.setSettings with
// just what changed); the settings are remembered from one run to the next. Locked while acquiring.
ScrollView {
    id: root
    required property var acquisition
    required property var serialPorts
    required property var appSettings
    signal firmwareRequested()

    readonly property var s: acquisition.settings
    readonly property string source: s.source ?? "simulated"
    readonly property bool locked: acquisition.running
    readonly property bool hasDecoder: source !== "audio" && source !== "scpi"
    readonly property string decoderKind: s.decoder?.kind ?? "text"
    property string problem: ""
    property var audioInputs: []
    property var presetNames: []

    contentWidth: availableWidth
    clip: true

    function set(values) {
        acquisition.setSettings(values)
    }
    function copy(value) {
        return JSON.parse(JSON.stringify(value))
    }
    function editSimulated(index, key, value) {
        const channels = copy(s.simulated.channels)
        channels[index][key] = value
        set({ simulated: { channels: channels } })
    }
    function editChannelInfo(index, key, value) {
        const count = Math.max(acquisition.channelNames.length, (s.channels ?? []).length, index + 1)
        const list = []
        for (let k = 0; k < count; ++k) {
            const c = (s.channels ?? [])[k] ?? {}
            list.push({ name: c.name ?? "", unit: c.unit ?? "" })
        }
        list[index][key] = value
        // Trailing empty entries are not worth keeping.
        while (list.length > 0 && list[list.length - 1].name === "" && list[list.length - 1].unit === "")
            list.pop()
        set({ channels: list })
    }
    function refreshPresets() {
        presetNames = appSettings.presetNames("acquisition")
    }
    Component.onCompleted: {
        problem = acquisition.validate(acquisition.settings)
        audioInputs = acquisition.audioInputs()
        refreshPresets()
    }
    Connections {
        target: root.acquisition
        function onSettingsChanged() {
            root.problem = root.acquisition.validate(root.acquisition.settings)
            rememberTimer.restart()
            baudBox.editText = String(root.s.serial?.baudRate ?? 115200)
        }
    }
    Connections {
        target: root.appSettings
        function onPresetsChanged(category) { if (category === "acquisition") root.refreshPresets() }
    }
    // The last settings are remembered (in the application's settings) shortly after each change.
    Timer {
        id: rememberTimer
        interval: 800
        onTriggered: root.appSettings.setValue("acquisition/last", JSON.stringify(root.acquisition.settings))
    }

    // An integer with its label.
    component IntField: ColumnLayout {
        id: intField
        property string label
        property int value: 0
        property int from: 0
        property int to: 1000000
        property string tip: ""
        signal edited(int value)
        spacing: 2
        Layout.fillWidth: true
        Label {
            text: intField.label
            color: Theme.textMuted
            font.pixelSize: Theme.smallText
            visible: intField.label.length > 0
        }
        SpinBox {
            Layout.fillWidth: true
            from: intField.from
            to: intField.to
            value: intField.value
            editable: true
            onValueModified: intField.edited(value)
            ToolTip.visible: hovered && intField.tip.length > 0
            ToolTip.delay: 600
            ToolTip.text: intField.tip
        }
    }
    // How a sample is coded: type, byte order, scale and offset (value = raw × scale + offset).
    component SampleFormatFields: ColumnLayout {
        id: formatFields
        property var format: ({})
        signal changed(var format)
        spacing: 8
        Layout.fillWidth: true
        function edit(key, value) {
            const f = JSON.parse(JSON.stringify(formatFields.format))
            f[key] = value
            formatFields.changed(f)
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            ChoiceField {
                label: qsTr("Type des échantillons")
                choices: Choices.names(Choices.sampleTypes)
                value: Choices.indexOf(Choices.sampleTypes, formatFields.format.type)
                onEdited: (i) => formatFields.edit("type", Choices.sampleTypes[i].key)
            }
            ChoiceField {
                visible: Choices.sampleTypes[Choices.indexOf(Choices.sampleTypes, formatFields.format.type)].bytes > 1
                label: qsTr("Ordre des octets")
                choices: Choices.names(Choices.byteOrders)
                value: Choices.indexOf(Choices.byteOrders, formatFields.format.byteOrder)
                onEdited: (i) => formatFields.edit("byteOrder", Choices.byteOrders[i].key)
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            QuantityField {
                label: qsTr("Échelle")
                value: formatFields.format.scale ?? 1
                tip: qsTr("valeur = brut × échelle + décalage ; par exemple 3,3/4096 = 0,000806 pour un CAN 12 bits sous 3,3 V")
                onEdited: (v) => formatFields.edit("scale", v)
            }
            QuantityField {
                label: qsTr("Décalage")
                value: formatFields.format.offset ?? 0
                onEdited: (v) => formatFields.edit("offset", v)
            }
        }
    }
    component Hint: Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textMuted
        font.pixelSize: Theme.smallText
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: 10

        Label {
            visible: root.locked
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Acquisition en cours : arrêtez-la pour changer ses réglages.")
            color: Theme.warning
            font.pixelSize: Theme.smallText
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 10
            enabled: !root.locked

            // ---- Source ----
            SectionTitle { text: qsTr("Source") }
            ChoiceField {
                label: qsTr("Type de source")
                choices: Choices.names(Choices.sources)
                value: Choices.indexOf(Choices.sources, root.source)
                onEdited: (i) => root.set({ source: Choices.sources[i].key })
            }
            TextValueField {
                label: qsTr("Nom du signal acquis")
                value: root.s.name ?? ""
                placeholder: qsTr("Acquisition")
                onEdited: (text) => root.set({ name: text })
            }

            // Serial port.
            ColumnLayout {
                visible: root.source === "serial"
                Layout.fillWidth: true
                spacing: 8
                Label {
                    text: qsTr("Port")
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
                RowLayout {
                    Layout.fillWidth: true
                    ComboBox {
                        id: portBox
                        Layout.fillWidth: true
                        model: root.serialPorts
                        textRole: "label"
                        valueRole: "portName"
                        // The count is read so that the choice follows a board plugged in or out.
                        currentIndex: (root.serialPorts.count, root.serialPorts.rowOf(root.s.serial?.port ?? ""))
                        displayText: currentIndex >= 0 ? currentText
                                     : (root.s.serial?.port ?? "").length > 0 ? qsTr("%1 (absent)").arg(root.s.serial.port)
                                     : qsTr("Choisissez un port")
                        onActivated: (i) => root.set({ serial: { port: root.serialPorts.portAt(i) } })
                    }
                    ToolButton {
                        implicitWidth: 36
                        contentItem: Item {
                            RibbonIcon {
                                anchors.centerIn: parent
                                width: 18
                                height: 18
                                name: "rotate"
                                tint: Theme.textMuted
                            }
                        }
                        onClicked: root.serialPorts.refresh()
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Relire la liste des ports (elle se met aussi à jour seule)")
                    }
                }
                Hint {
                    visible: root.serialPorts.count === 0
                    text: qsTr("Aucun port série : branchez la carte, elle apparaît d'elle-même.")
                }
                Label {
                    text: qsTr("Débit (bauds)")
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
                ComboBox {
                    id: baudBox
                    Layout.fillWidth: true
                    editable: true
                    model: Choices.baudRates
                    Component.onCompleted: editText = String(root.s.serial?.baudRate ?? 115200)
                    onActivated: (i) => root.set({ serial: { baudRate: Choices.baudRates[i] } })
                    onAccepted: {
                        const v = parseInt(editText.replace(/\s/g, ""))
                        if (v > 0)
                            root.set({ serial: { baudRate: v } })
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    ChoiceField {
                        label: qsTr("Bits de données")
                        choices: ["5", "6", "7", "8"]
                        value: Math.max(0, (root.s.serial?.dataBits ?? 8) - 5)
                        onEdited: (i) => root.set({ serial: { dataBits: i + 5 } })
                    }
                    ChoiceField {
                        label: qsTr("Parité")
                        choices: Choices.names(Choices.parities)
                        value: Choices.indexOf(Choices.parities, root.s.serial?.parity)
                        onEdited: (i) => root.set({ serial: { parity: Choices.parities[i].key } })
                    }
                    ChoiceField {
                        label: qsTr("Bits d'arrêt")
                        choices: ["1", "2"]
                        value: (root.s.serial?.stopBits ?? 1) === 2 ? 1 : 0
                        onEdited: (i) => root.set({ serial: { stopBits: i + 1 } })
                    }
                }
                ChoiceField {
                    label: qsTr("Contrôle de flux")
                    choices: Choices.names(Choices.flows)
                    value: Choices.indexOf(Choices.flows, root.s.serial?.flow)
                    onEdited: (i) => root.set({ serial: { flow: Choices.flows[i].key } })
                }
                CheckBox {
                    text: qsTr("Signal DTR")
                    checked: root.s.serial?.dtr ?? true
                    onToggled: root.set({ serial: { dtr: checked } })
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Activé, un Arduino Uno ou Nano redémarre à l'ouverture du port (son programme repart de zéro)")
                }
            }

            // TCP client.
            ColumnLayout {
                visible: root.source === "tcp"
                Layout.fillWidth: true
                spacing: 8
                TextValueField {
                    label: qsTr("Adresse de la carte ou du serveur")
                    value: root.s.tcp?.host ?? ""
                    placeholder: "192.168.1.50"
                    onEdited: (text) => root.set({ tcp: { host: text.trim() } })
                }
                IntField {
                    label: qsTr("Port")
                    from: 1
                    to: 65535
                    value: root.s.tcp?.port ?? 5000
                    onEdited: (v) => root.set({ tcp: { port: v } })
                }
                Hint { text: qsTr("La carte (ESP32, Raspberry Pi…) attend la connexion ; l'application se connecte à elle.") }
            }

            // UDP.
            ColumnLayout {
                visible: root.source === "udp"
                Layout.fillWidth: true
                spacing: 8
                IntField {
                    label: qsTr("Port d'écoute")
                    from: 1
                    to: 65535
                    value: root.s.udp?.port ?? 5005
                    onEdited: (v) => root.set({ udp: { port: v } })
                }
                ChoiceField {
                    label: qsTr("Écouter")
                    choices: [qsTr("Sur toutes les interfaces (réseau local)"), qsTr("Ce PC seulement (127.0.0.1)")]
                    value: (root.s.udp?.bindAddress ?? "") === "127.0.0.1" ? 1 : 0
                    onEdited: (i) => root.set({ udp: { bindAddress: i === 1 ? "127.0.0.1" : "" } })
                }
                TextValueField {
                    label: qsTr("Groupe de multidiffusion (facultatif)")
                    value: root.s.udp?.multicastGroup ?? ""
                    placeholder: "239.1.2.3"
                    onEdited: (text) => root.set({ udp: { multicastGroup: text.trim() } })
                }
                Hint { text: qsTr("La première fois, Windows peut demander d'autoriser l'application dans son pare-feu (pas pour « Ce PC seulement »).") }
            }

            // SCPI instrument.
            ColumnLayout {
                visible: root.source === "scpi"
                Layout.fillWidth: true
                spacing: 8
                readonly property bool visa: (root.s.scpi?.transport ?? "tcp") === "visa"
                ChoiceField {
                    label: qsTr("Liaison")
                    choices: Choices.names(Choices.scpiTransports)
                    value: Choices.indexOf(Choices.scpiTransports, root.s.scpi?.transport)
                    onEdited: (i) => root.set({ scpi: { transport: Choices.scpiTransports[i].key } })
                }
                TextValueField {
                    visible: !parent.visa
                    label: qsTr("Adresse de l'instrument")
                    value: root.s.scpi?.host ?? ""
                    placeholder: "192.168.1.20"
                    onEdited: (text) => root.set({ scpi: { host: text.trim() } })
                }
                IntField {
                    visible: !parent.visa
                    label: qsTr("Port")
                    from: 1
                    to: 65535
                    value: root.s.scpi?.port ?? 5025
                    onEdited: (v) => root.set({ scpi: { port: v } })
                }
                TextValueField {
                    visible: parent.visa
                    label: qsTr("Ressource VISA")
                    value: root.s.scpi?.visaResource ?? ""
                    placeholder: "USB0::0x0957::0x1796::MY123::INSTR"
                    monospace: true
                    onEdited: (text) => root.set({ scpi: { visaResource: text.trim() } })
                }
                Label {
                    text: qsTr("Commandes de préparation (une par ligne)")
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
                TextArea {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(60, implicitHeight)
                    wrapMode: TextEdit.NoWrap
                    selectByMouse: true
                    font.family: Theme.monospace
                    font.pixelSize: Theme.smallText
                    placeholderText: text.length === 0 && !activeFocus ? "*RST\nCONF:VOLT:DC 10" : ""
                    text: (root.s.scpi?.setupCommands ?? []).join("\n")
                    onActiveFocusChanged: {
                        if (activeFocus)
                            return
                        const lines = text.split("\n").map((l) => l.trim()).filter((l) => l.length > 0)
                        if (lines.join("\n") !== (root.s.scpi?.setupCommands ?? []).join("\n"))
                            root.set({ scpi: { setupCommands: lines } })
                    }
                }
                TextValueField {
                    label: qsTr("Requête répétée")
                    value: root.s.scpi?.query ?? ""
                    placeholder: "MEAS:VOLT:DC?"
                    monospace: true
                    tip: qsTr("Une mesure (MEAS:VOLT:DC?, READ?) ou une forme d'onde (:WAV:DATA?, CURV?)")
                    onEdited: (text) => root.set({ scpi: { query: text.trim() } })
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    QuantityField {
                        label: qsTr("Intervalle")
                        unit: "s"
                        minimum: 0
                        value: root.s.scpi?.intervalSeconds ?? 0.1
                        tip: qsTr("Attente entre une réponse et la requête suivante (0 : au plus vite)")
                        onEdited: (v) => root.set({ scpi: { intervalSeconds: v } })
                    }
                    QuantityField {
                        label: qsTr("Délai de réponse")
                        unit: "s"
                        minimum: 0.01
                        value: root.s.scpi?.timeoutSeconds ?? 3
                        onEdited: (v) => root.set({ scpi: { timeoutSeconds: v } })
                    }
                }
                CheckBox {
                    text: qsTr("Lire ses erreurs (SYST:ERR?)")
                    checked: root.s.scpi?.checkErrors ?? true
                    onToggled: root.set({ scpi: { checkErrors: checked } })
                }
                IntField {
                    label: qsTr("Valeurs par réponse")
                    from: 1
                    to: 64
                    value: root.s.decoder?.scpi?.channels ?? 1
                    tip: qsTr("Une réponse « 1,2;3,4 » donne deux voies ; une forme d'onde, une voie")
                    onEdited: (v) => root.set({ decoder: { scpi: { channels: v } } })
                }
                Label {
                    text: qsTr("Formes d'onde en bloc binaire (#…)")
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
                SampleFormatFields {
                    format: root.s.decoder?.scpi?.block ?? ({})
                    onChanged: (f) => root.set({ decoder: { scpi: { block: f } } })
                }
            }

            // Sound card.
            ColumnLayout {
                visible: root.source === "audio"
                Layout.fillWidth: true
                spacing: 8
                Label {
                    text: qsTr("Entrée")
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
                RowLayout {
                    Layout.fillWidth: true
                    ComboBox {
                        Layout.fillWidth: true
                        model: [qsTr("Entrée par défaut")].concat(root.audioInputs)
                        currentIndex: (root.s.audio?.device ?? "") === "" ? 0
                                      : root.audioInputs.indexOf(root.s.audio.device) >= 0 ? root.audioInputs.indexOf(root.s.audio.device) + 1 : -1
                        displayText: currentIndex >= 0 ? currentText : qsTr("%1 (absente)").arg(root.s.audio?.device ?? "")
                        onActivated: (i) => root.set({ audio: { device: i === 0 ? "" : root.audioInputs[i - 1] } })
                    }
                    ToolButton {
                        implicitWidth: 36
                        contentItem: Item {
                            RibbonIcon {
                                anchors.centerIn: parent
                                width: 18
                                height: 18
                                name: "rotate"
                                tint: Theme.textMuted
                            }
                        }
                        onClicked: root.audioInputs = root.acquisition.audioInputs()
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Relire la liste des entrées")
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    ChoiceField {
                        label: qsTr("Fréquence")
                        choices: Choices.audioRates.map((r) => Units.compact(r, "Hz", 6))
                        value: Math.max(0, Choices.audioRates.indexOf(root.s.audio?.sampleRate ?? 48000))
                        onEdited: (i) => root.set({ audio: { sampleRate: Choices.audioRates[i] } })
                    }
                    ChoiceField {
                        label: qsTr("Voies")
                        choices: [qsTr("1 (mono)"), qsTr("2 (stéréo)")]
                        value: (root.s.audio?.channels ?? 1) >= 2 ? 1 : 0
                        onEdited: (i) => root.set({ audio: { channels: i + 1 } })
                    }
                }
                ChoiceField {
                    label: qsTr("Format")
                    choices: Choices.names(Choices.audioTypes)
                    value: Choices.indexOf(Choices.audioTypes, root.s.audio?.sampleType)
                    onEdited: (i) => root.set({ audio: { sampleType: Choices.audioTypes[i].key } })
                }
                Hint { text: qsTr("Les échantillons sont entre −1 et 1 (pleine échelle de la carte).") }
            }

            // Simulator.
            ColumnLayout {
                visible: root.source === "simulated"
                Layout.fillWidth: true
                spacing: 8
                Hint { text: qsTr("Une carte imaginaire, au rythme réel : elle envoie les mêmes octets qu'une vraie, dans le format choisi plus bas. Rien à brancher.") }
                Repeater {
                    model: root.s.simulated?.channels ?? []
                    delegate: ColumnLayout {
                        id: simChannel
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        spacing: 6
                        RowLayout {
                            Layout.fillWidth: true
                            Rectangle {
                                width: 10
                                height: 10
                                radius: 3
                                color: Theme.channelColors[simChannel.index % Theme.channelColors.length]
                            }
                            Label {
                                text: qsTr("Voie %1").arg(simChannel.index + 1)
                                color: Theme.text
                                font.pixelSize: Theme.bodyText
                                font.bold: true
                            }
                            Item { Layout.fillWidth: true }
                            ToolButton {
                                text: "×"
                                enabled: (root.s.simulated?.channels ?? []).length > 1
                                onClicked: {
                                    const channels = root.copy(root.s.simulated.channels)
                                    channels.splice(simChannel.index, 1)
                                    root.set({ simulated: { channels: channels } })
                                }
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Retirer cette voie")
                            }
                        }
                        ChoiceField {
                            label: qsTr("Forme")
                            choices: Choices.names(Choices.waveforms)
                            value: Choices.indexOf(Choices.waveforms, simChannel.modelData.waveform)
                            onEdited: (i) => root.editSimulated(simChannel.index, "waveform", Choices.waveforms[i].key)
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            QuantityField {
                                label: qsTr("Amplitude")
                                value: simChannel.modelData.amplitude
                                onEdited: (v) => root.editSimulated(simChannel.index, "amplitude", v)
                            }
                            QuantityField {
                                label: qsTr("Fréquence")
                                unit: "Hz"
                                minimum: 0
                                value: simChannel.modelData.frequencyHz
                                onEdited: (v) => root.editSimulated(simChannel.index, "frequencyHz", v)
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            QuantityField {
                                label: qsTr("Décalage")
                                value: simChannel.modelData.offset
                                onEdited: (v) => root.editSimulated(simChannel.index, "offset", v)
                            }
                            QuantityField {
                                label: qsTr("Bruit (écart type)")
                                minimum: 0
                                value: simChannel.modelData.noise
                                onEdited: (v) => root.editSimulated(simChannel.index, "noise", v)
                            }
                        }
                    }
                }
                Button {
                    flat: true
                    enabled: (root.s.simulated?.channels ?? []).length < 8
                    text: qsTr("Ajouter une voie")
                    onClicked: {
                        const channels = root.copy(root.s.simulated?.channels ?? [])
                        channels.push({ waveform: "sine", amplitude: 1, frequencyHz: 10 * (channels.length + 1),
                                        phaseDegrees: 0, offset: 0, dutyCycle: 0.5, noise: 0 })
                        root.set({ simulated: { channels: channels } })
                    }
                }
                Label {
                    text: qsTr("Défauts de la liaison simulée")
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    QuantityField {
                        label: qsTr("Octets abîmés (%)")
                        minimum: 0
                        maximum: 100
                        value: 100 * (root.s.simulated?.byteErrorRate ?? 0)
                        onEdited: (v) => root.set({ simulated: { byteErrorRate: v / 100 } })
                    }
                    QuantityField {
                        label: qsTr("Trames perdues (%)")
                        minimum: 0
                        maximum: 100
                        value: 100 * (root.s.simulated?.frameLossRate ?? 0)
                        onEdited: (v) => root.set({ simulated: { frameLossRate: v / 100 } })
                    }
                }
                QuantityField {
                    label: qsTr("Écart de l'horloge de la carte (ppm)")
                    value: root.s.simulated?.rateErrorPpm ?? 0
                    tip: qsTr("Une carte dont le quartz dérive : la fréquence mesurée le montre")
                    onEdited: (v) => root.set({ simulated: { rateErrorPpm: v } })
                }
            }

            // ---- Format of the data ----
            SectionTitle {
                visible: root.hasDecoder
                text: qsTr("Format des données")
            }
            ColumnLayout {
                visible: root.hasDecoder
                Layout.fillWidth: true
                spacing: 8
                ChoiceField {
                    label: qsTr("La carte envoie")
                    choices: Choices.names(Choices.decoders)
                    value: Choices.indexOf(Choices.decoders, root.decoderKind)
                    onEdited: (i) => root.set({ decoder: { kind: Choices.decoders[i].key } })
                }
                // Text lines.
                ColumnLayout {
                    visible: root.decoderKind === "text"
                    Layout.fillWidth: true
                    spacing: 8
                    IntField {
                        label: qsTr("Voies (0 : d'après la première ligne)")
                        from: 0
                        to: 64
                        value: root.s.decoder?.text?.channelCount ?? 0
                        onEdited: (v) => root.set({ decoder: { text: { channelCount: v } } })
                    }
                    CheckBox {
                        text: qsTr("Virgule décimale (3,14)")
                        checked: root.s.decoder?.text?.decimalComma ?? false
                        onToggled: root.set({ decoder: { text: { decimalComma: checked } } })
                    }
                    Hint { text: qsTr("Une ligne par instant : « 1.25, 3.3 », « 1.25 3.3 » ou « U=1.25 I=3.3 » (les étiquettes nomment les voies).") }
                }
                // Raw binary.
                ColumnLayout {
                    visible: root.decoderKind === "raw"
                    Layout.fillWidth: true
                    spacing: 8
                    IntField {
                        label: qsTr("Voies (échantillons entrelacés)")
                        from: 1
                        to: 64
                        value: root.s.decoder?.raw?.channels ?? 1
                        onEdited: (v) => root.set({ decoder: { raw: { channels: v } } })
                    }
                    SampleFormatFields {
                        format: root.s.decoder?.raw ?? ({})
                        onChanged: (f) => root.set({ decoder: { raw: f } })
                    }
                    Hint { text: qsTr("Sans repère de début : si un octet se perd, tout ce qui suit est décalé. Préférez les trames pour une liaison qui peut perdre des octets.") }
                }
                // Frames.
                ColumnLayout {
                    visible: root.decoderKind === "frames"
                    Layout.fillWidth: true
                    spacing: 8
                    TextValueField {
                        label: qsTr("Octets de synchronisation (hexadécimal)")
                        value: root.s.decoder?.frame?.sync ?? "AA 55"
                        placeholder: "AA 55"
                        monospace: true
                        onEdited: (text) => root.set({ decoder: { frame: { sync: text.trim() } } })
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        IntField {
                            label: qsTr("Voies")
                            from: 1
                            to: 64
                            value: root.s.decoder?.frame?.channels ?? 1
                            onEdited: (v) => root.set({ decoder: { frame: { channels: v } } })
                        }
                        IntField {
                            label: qsTr("Échantillons par voie")
                            from: 1
                            to: 4096
                            value: root.s.decoder?.frame?.samplesPerChannel ?? 1
                            onEdited: (v) => root.set({ decoder: { frame: { samplesPerChannel: v } } })
                        }
                    }
                    SampleFormatFields {
                        format: root.s.decoder?.frame?.sample ?? ({})
                        onChanged: (f) => root.set({ decoder: { frame: { sample: f } } })
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        ChoiceField {
                            label: qsTr("Compteur de trames")
                            choices: Choices.names(Choices.counters)
                            value: Choices.indexOf(Choices.counters, root.s.decoder?.frame?.counter)
                            onEdited: (i) => root.set({ decoder: { frame: { counter: Choices.counters[i].key } } })
                        }
                        ChoiceField {
                            label: qsTr("Somme de contrôle")
                            choices: Choices.names(Choices.checksums)
                            value: Choices.indexOf(Choices.checksums, root.s.decoder?.frame?.checksum)
                            onEdited: (i) => root.set({ decoder: { frame: { checksum: Choices.checksums[i].key } } })
                        }
                    }
                    CheckBox {
                        visible: (root.s.decoder?.frame?.checksum ?? "none") !== "none"
                        text: qsTr("Somme sur la synchro aussi")
                        checked: root.s.decoder?.frame?.checksumCoversSync ?? false
                        onToggled: root.set({ decoder: { frame: { checksumCoversSync: checked } } })
                    }
                    Hint { text: qsTr("Trame = synchro, compteur, échantillons voie par voie, somme. Le compteur révèle les trames perdues, la somme écarte les trames abîmées.") }
                }
            }

            // ---- Rate ----
            SectionTitle { text: qsTr("Cadence") }
            QuantityField {
                visible: root.source !== "audio"
                label: qsTr("Fréquence d'échantillonnage annoncée")
                unit: "Hz"
                minimum: 0
                value: root.s.declaredSampleRateHz ?? 0
                tip: qsTr("0 : mesurée à la réception. Si la carte échantillonne à une cadence connue, annoncez-la : la mesure sert alors de contrôle.")
                onEdited: (v) => root.set({ declaredSampleRateHz: v })
            }
            Hint {
                text: root.source === "audio" ? qsTr("La carte son donne sa propre cadence.")
                      : (root.s.declaredSampleRateHz ?? 0) > 0 ? qsTr("Annoncée : la fréquence mesurée s'affiche pour contrôle sous le tracé.")
                      : qsTr("Mesurée : par régression sur les arrivées des 5 dernières secondes.")
            }

            // ---- Channels ----
            SectionTitle { text: qsTr("Voies") }
            Repeater {
                model: root.acquisition.channelNames
                delegate: RowLayout {
                    id: channelRow
                    required property string modelData
                    required property int index
                    Layout.fillWidth: true
                    spacing: 6
                    Rectangle {
                        Layout.alignment: Qt.AlignBottom
                        Layout.bottomMargin: 12
                        width: 10
                        height: 10
                        radius: 3
                        color: Theme.channelColors[channelRow.index % Theme.channelColors.length]
                    }
                    TextValueField {
                        label: channelRow.index === 0 ? qsTr("Nom") : ""
                        value: ((root.s.channels ?? [])[channelRow.index] ?? {}).name ?? ""
                        placeholder: channelRow.modelData
                        onEdited: (text) => root.editChannelInfo(channelRow.index, "name", text.trim())
                    }
                    TextValueField {
                        Layout.fillWidth: false
                        Layout.preferredWidth: 70
                        label: channelRow.index === 0 ? qsTr("Unité") : ""
                        value: ((root.s.channels ?? [])[channelRow.index] ?? {}).unit ?? ""
                        onEdited: (text) => root.editChannelInfo(channelRow.index, "unit", text.trim())
                    }
                }
            }
            Hint { text: qsTr("Vides : les étiquettes envoyées par la carte, sinon « Voie 1 », « Voie 2 »…") }
        }

        // ---- Example program for the board ----
        Button {
            visible: root.source === "serial" || root.source === "tcp" || root.source === "udp" || root.source === "simulated"
            Layout.fillWidth: true
            text: qsTr("Programme d'exemple pour la carte…")
            onClicked: root.firmwareRequested()
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
                enabled: presetBox.count > 0 && !root.locked
                onClicked: root.set(root.appSettings.preset("acquisition", presetBox.currentText))
            }
            ToolButton {
                text: "×"
                enabled: presetBox.count > 0
                onClicked: root.appSettings.removePreset("acquisition", presetBox.currentText)
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
                placeholderText: text.length === 0 && !activeFocus ? qsTr("Nom du préréglage (ex. « Carte STM32 trames »)") : ""
            }
            Button {
                text: qsTr("Enregistrer")
                enabled: presetName.text.trim().length > 0
                onClicked: {
                    root.appSettings.savePreset("acquisition", presetName.text.trim(), root.acquisition.settings)
                    presetName.text = ""
                }
            }
        }

        Label {
            visible: root.problem.length > 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.problem
            color: Theme.error
            font.pixelSize: Theme.smallText
        }
        Item { Layout.preferredHeight: 8 }
    }
}
