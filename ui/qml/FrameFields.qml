import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// A line of a digital frame, in a card of the panel Composition (WaveCard): its protocol and which of its
// lines; the data (typed as bits, hexadecimal, decimal or text), their format and the order of their bits; what
// the protocol asks (the line code of bits sent as they are; data bits, parity and stop bits of a UART; address
// and exchange of an I2C bus; mode and reply of an SPI bus; speed, packet, address and endpoint of USB); its
// bit rate, its start, its two levels, whether it repeats. Then the frame as built, field by field, with what
// its marks mean, its length in bits and in time, and buttons: the signal framed on it, the bit rate of its
// protocol, its other lines as new channels. What is set here goes to the other lines of the same frame.
ColumnLayout {
    id: root
    required property var generator
    property int channel: 0
    property int index: 0
    property var wave: ({})
    property string unit: ""
    // The frame as built (again at each change of the recipe).
    readonly property var info: (generator.recipe, generator.frameInfo(channel, index))
    readonly property string problem: info.error ?? ""
    readonly property string protocol: wave.protocol ?? "bits"
    readonly property string packet: wave.usbPacket ?? "setup"
    readonly property string transfer: wave.i2cTransfer ?? "write"
    readonly property bool usbAddressed: packet === "setup" || packet === "out" || packet === "in"
                                         || packet === "setup-transaction"
    readonly property bool usbCarries: packet === "data0" || packet === "data1" || packet === "setup-transaction"
    readonly property bool takesData: protocol !== "usb" || usbCarries
    readonly property bool takesReply: protocol === "spi" || (protocol === "i2c" && transfer === "write-read")
    readonly property bool ordered: protocol === "bits" || protocol === "uart" || protocol === "spi"
    readonly property bool repeated: wave.repeat === true
    readonly property var missing: info.missing ?? []
    readonly property var cautions: info.warnings ?? []

    function set(values) {
        generator.setComponent(channel, index, values)
    }
    // "0x48" for the address typed in an I2C field; what is typed back: "0x48", "72", "48h".
    function hex(value) {
        const digits = Math.round(value).toString(16).toUpperCase()
        return "0x" + (digits.length < 2 ? "0" + digits : digits)
    }
    function typedAddress(text) {
        const t = text.trim()
        if (t.length === 0)
            return -1
        const v = /^[0-9a-fA-F]+[hH]$/.test(t) ? parseInt(t.slice(0, -1), 16) : Number(t)
        return isNaN(v) || v < 0 || v > 127 || Math.round(v) !== v ? -1 : v
    }

    spacing: 6
    Layout.fillWidth: true

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        ChoiceField {
            label: qsTr("Protocole")
            choices: Choices.names(Choices.frameProtocols)
            value: Choices.indexOf(Choices.frameProtocols, root.protocol)
            onEdited: (i) => root.set({ protocol: Choices.frameProtocols[i].key })
        }
        ChoiceField {
            Layout.fillWidth: false
            Layout.preferredWidth: 110
            visible: (root.info.lines ?? []).length > 1
            label: qsTr("Ligne")
            choices: root.info.lines ?? []
            value: root.info.line ?? 0
            onEdited: (i) => root.set({ line: root.info.lineKeys[i] })
        }
    }
    FormulaField {
        visible: root.takesData
        label: root.protocol === "i2c" && root.transfer === "read" ? qsTr("Octets lus (envoyés par l'esclave)")
               : root.protocol === "i2c" ? qsTr("Octets écrits")
               : root.protocol === "spi" ? qsTr("Données (MOSI)")
               : root.packet === "setup-transaction" && root.protocol === "usb" ? qsTr("Requête (8 octets)")
               : qsTr("Données")
        value: root.wave.data ?? ""
        placeholder: "0011 0010 100   ·   0x2831   ·   Bonjour"
        tip: qsTr("Des bits (0011 0010 100), de l'hexadécimal (0x2831 ou 28 31), du texte (Bonjour, ou \"bonjour\" entre guillemets ; \\n, \\r, \\t, \\0, \\x41) ; « hex: », « bin: », « dec: » ou « ascii: » devant les données en dit le format. Sans préfixe, des chiffres autres que 0 et 1 sont du texte : choisissez alors le format.")
        onEdited: (text) => root.set({ data: text })
    }
    FormulaField {
        visible: root.takesReply
        label: root.protocol === "spi" ? qsTr("Réponse (MISO)") : qsTr("Octets lus après le redémarrage")
        value: root.wave.reply ?? ""
        placeholder: root.protocol === "spi" ? qsTr("vide : MISO reste à 0") : "0x7F 0x80"
        tip: root.protocol === "spi" ? qsTr("Ce que l'esclave renvoie sur MISO pendant que le maître envoie, dans le même format que les données ; le plus court des deux est complété par des 0")
                                     : qsTr("Ce que l'esclave renvoie après le redémarrage et son adresse en lecture, dans le même format que les données")
        onEdited: (text) => root.set({ reply: text })
    }
    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: 8
        rowSpacing: 6
        ChoiceField {
            visible: root.takesData
            label: qsTr("Format des données")
            choices: Choices.names(Choices.frameFormats)
            value: Choices.indexOf(Choices.frameFormats, root.wave.format ?? "auto")
            onEdited: (i) => root.set({ format: Choices.frameFormats[i].key })
        }
        ChoiceField {
            visible: root.ordered
            label: qsTr("Ordre des bits")
            choices: [qsTr("Poids fort d'abord (MSB)"), qsTr("Poids faible d'abord (LSB)")]
            value: root.wave.lsbFirst === true ? 1 : 0
            onEdited: (i) => root.set({ lsbFirst: i === 1 })
        }
        // ---- Bits as they are ----
        ChoiceField {
            visible: root.protocol === "bits"
            label: qsTr("Code de ligne")
            choices: Choices.names(Choices.lineCodes)
            value: Choices.indexOf(Choices.lineCodes, root.wave.code ?? "nrz")
            onEdited: (i) => root.set({ code: Choices.lineCodes[i].key })
        }
        // ---- UART ----
        ChoiceField {
            visible: root.protocol === "uart"
            label: qsTr("Bits de données")
            choices: ["5", "6", "7", "8"]
            value: Math.max(0, Math.min(3, (root.wave.dataBits ?? 8) - 5))
            onEdited: (i) => root.set({ dataBits: i + 5 })
        }
        ChoiceField {
            visible: root.protocol === "uart"
            label: qsTr("Parité")
            choices: [qsTr("Aucune"), qsTr("Paire"), qsTr("Impaire")]
            value: root.wave.parity === "even" ? 1 : root.wave.parity === "odd" ? 2 : 0
            onEdited: (i) => root.set({ parity: i === 1 ? "even" : i === 2 ? "odd" : "none" })
        }
        ChoiceField {
            visible: root.protocol === "uart"
            label: qsTr("Bits d'arrêt")
            choices: ["1", "1,5", "2"]
            value: (root.wave.stopBits ?? 1) >= 1.75 ? 2 : (root.wave.stopBits ?? 1) >= 1.25 ? 1 : 0
            onEdited: (i) => root.set({ stopBits: i === 2 ? 2 : i === 1 ? 1.5 : 1 })
        }
        // ---- I2C ----
        TextValueField {
            visible: root.protocol === "i2c"
            label: qsTr("Adresse (7 bits)")
            value: root.hex(root.wave.i2cAddress ?? 0x48)
            monospace: true
            tip: qsTr("L'adresse de l'esclave, de 0x00 à 0x7F, sans le bit de lecture ou d'écriture : 0x48, 72 ou 48h")
            onEdited: (text) => {
                const v = root.typedAddress(text)
                if (v >= 0)
                    root.set({ i2cAddress: v })
            }
        }
        ChoiceField {
            visible: root.protocol === "i2c"
            label: qsTr("Échange")
            choices: [qsTr("Écriture"), qsTr("Lecture"), qsTr("Écriture puis lecture")]
            value: root.transfer === "read" ? 1 : root.transfer === "write-read" ? 2 : 0
            onEdited: (i) => root.set({ i2cTransfer: i === 1 ? "read" : i === 2 ? "write-read" : "write" })
        }
        CheckBox {
            visible: root.protocol === "i2c"
            Layout.columnSpan: 2
            text: qsTr("Personne ne répond à l'adresse (NACK)")
            checked: root.wave.i2cNoAnswer === true
            onToggled: root.set({ i2cNoAnswer: checked })
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: qsTr("Aucun esclave n'acquitte l'adresse : SDA reste à 1 pendant le neuvième bit, et le maître arrête la trame")
        }
        // ---- SPI ----
        ChoiceField {
            visible: root.protocol === "spi"
            label: qsTr("Mode (horloge)")
            choices: [qsTr("0 : repos à 0, front montant"), qsTr("1 : repos à 0, front descendant"),
                      qsTr("2 : repos à 1, front descendant"), qsTr("3 : repos à 1, front montant")]
            value: Math.max(0, Math.min(3, root.wave.spiMode ?? 0))
            onEdited: (i) => root.set({ spiMode: i })
        }
        // ---- USB ----
        ChoiceField {
            visible: root.protocol === "usb"
            label: qsTr("Vitesse")
            choices: [qsTr("Bas débit (1,5 Mbit/s)"), qsTr("Plein débit (12 Mbit/s)")]
            value: root.wave.usbFullSpeed === true ? 1 : 0
            onEdited: (i) => root.set({ usbFullSpeed: i === 1 })
        }
        ChoiceField {
            visible: root.protocol === "usb"
            label: qsTr("Paquet")
            choices: Choices.names(Choices.usbPackets)
            value: Choices.indexOf(Choices.usbPackets, root.packet)
            onEdited: (i) => root.set({ usbPacket: Choices.usbPackets[i].key })
        }
        QuantityField {
            visible: root.protocol === "usb" && root.usbAddressed
            label: qsTr("Adresse (0 à 127)")
            unit: ""
            minimum: 0
            maximum: 127
            value: root.wave.usbAddress ?? 0
            tip: qsTr("L'adresse de l'appareil sur le bus : 0 tant qu'il n'en a pas reçu")
            onEdited: (v) => root.set({ usbAddress: Math.round(v) })
        }
        QuantityField {
            visible: root.protocol === "usb" && root.usbAddressed
            label: qsTr("Point de terminaison (0 à 15)")
            unit: ""
            minimum: 0
            maximum: 15
            value: root.wave.usbEndpoint ?? 0
            tip: qsTr("Le point de terminaison (endpoint) visé : 0 pour les requêtes de contrôle")
            onEdited: (v) => root.set({ usbEndpoint: Math.round(v) })
        }
        QuantityField {
            visible: root.protocol === "usb" && root.packet === "sof"
            label: qsTr("Numéro de trame (0 à 2047)")
            unit: ""
            minimum: 0
            maximum: 2047
            value: root.wave.usbFrameNumber ?? 0
            tip: qsTr("Le compteur de trames de l'hôte, une par milliseconde, sur 11 bits")
            onEdited: (v) => root.set({ usbFrameNumber: Math.round(v) })
        }
        // ---- Rate, start, levels ----
        RowLayout {
            Layout.fillWidth: true
            spacing: 2
            QuantityField {
                label: qsTr("Débit")
                unit: "bit/s"
                plainNumbers: true
                minimum: 1e-6
                value: root.wave.frequency ?? 1000
                tip: qsTr("Le nombre de bits par seconde : 9600, 115200 (un nombre seul est en bit/s), 100 k, 1,5 M… Il faut au moins 2 échantillons par bit (4 en I2C) pour que tous les niveaux de la trame existent")
                onEdited: (v) => root.set({ frequency: v })
            }
            ToolButton {
                id: rateButton
                Layout.alignment: Qt.AlignBottom
                text: "▾"
                implicitWidth: 28
                onClicked: rateMenu.popup(rateButton, 0, rateButton.height)
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Les débits usuels de ce protocole")
                FitMenu {
                    id: rateMenu
                    Instantiator {
                        model: Choices.frameRates[root.protocol] ?? []
                        delegate: MenuItem {
                            required property var modelData
                            text: Units.compact(modelData, "bit/s", 6)
                            onTriggered: root.set({ frequency: modelData })
                        }
                        onObjectAdded: (index, object) => rateMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) => rateMenu.removeItem(object)
                    }
                }
            }
        }
        QuantityField {
            label: qsTr("Début")
            unit: "s"
            value: root.wave.time ?? 0
            tip: qsTr("L'instant du signal où la trame commence ; avant, la ligne est au repos")
            onEdited: (v) => root.set({ time: v })
        }
        QuantityField {
            label: qsTr("Niveau haut (1)")
            unit: root.unit
            value: root.wave.amplitude ?? 1
            tip: qsTr("La valeur d'un niveau haut : 3,3 V, 5 V… ou 1 pour lire les bits sur le graphique")
            onEdited: (v) => root.set({ amplitude: v })
        }
        QuantityField {
            label: qsTr("Niveau bas (0)")
            unit: root.unit
            value: root.wave.low ?? 0
            tip: qsTr("La valeur d'un niveau bas : 0 V, ou l'opposé du niveau haut pour un signal symétrique (−1 et +1)")
            onEdited: (v) => root.set({ low: v })
        }
        ChoiceField {
            label: qsTr("Envoi")
            choices: [qsTr("Une fois"), qsTr("En boucle")]
            value: root.repeated ? 1 : 0
            onEdited: (i) => root.set({ repeat: i === 1 })
        }
        QuantityField {
            visible: root.repeated
            label: qsTr("Repos entre deux trames (bits)")
            unit: ""
            minimum: 0
            value: root.wave.gap ?? 10
            tip: qsTr("La durée du repos entre la fin d'une trame et le début de la suivante, en durées de bit ; 0 : les trames se suivent (« 10 » en boucle fait un signal carré)")
            onEdited: (v) => root.set({ gap: v })
        }
    }

    // ---- The frame as built ----
    Label {
        visible: root.problem.length > 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: root.problem
        color: Theme.error
        font.pixelSize: Theme.smallText
    }
    Label {
        visible: root.problem.length === 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: qsTr("Trame construite — %1 : %2").arg(root.info.title ?? "").arg(root.info.summary ?? "")
        color: Theme.textMuted
        font.pixelSize: Theme.smallText
    }
    Rectangle {
        visible: root.problem.length === 0
        Layout.fillWidth: true
        implicitHeight: built.implicitHeight + 12
        radius: 4
        color: Theme.canvas
        border.color: Theme.border
        TextEdit {
            id: built
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 6
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.Wrap
            text: root.info.construction ?? ""
            color: Theme.text
            selectionColor: Theme.accent
            font.family: Theme.monospace
            font.pixelSize: Theme.smallText + 1
        }
    }
    Label {
        visible: root.problem.length === 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: root.info.legend ?? ""
        color: Theme.textMuted
        font.pixelSize: Theme.smallText
    }
    Repeater {
        model: root.problem.length === 0 ? root.cautions : []
        delegate: Label {
            required property string modelData
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: modelData
            color: Theme.warning
            font.pixelSize: Theme.smallText
        }
    }
    Label {
        visible: root.problem.length === 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: (root.info.timing ?? "")
              + (root.info.sampled === false ? qsTr(" — trop peu d'échantillons : des niveaux manquent")
                 : root.info.fits === false ? qsTr(" — elle dépasse la fin du signal") : "")
        color: root.info.sampled === false || root.info.fits === false ? Theme.warning : Theme.text
        font.pixelSize: Theme.smallText
    }
    Flow {
        // (Hidden with nothing to offer: empty, it would still take the spacing of the column.)
        visible: root.problem.length === 0
                 && (root.info.differs === true || (root.info.realRate ?? 0) > 0 || root.missing.length > 0)
        Layout.fillWidth: true
        spacing: 4
        Button {
            visible: root.info.differs === true
            text: qsTr("Cadrer la trame")
            flat: true
            onClicked: root.generator.adoptFrameTiming(root.channel, root.index, false)
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: qsTr("Le signal prend la durée de la trame, avec deux bits de repos avant et après, et une fréquence d'échantillonnage d'au moins dix échantillons par bit (un nombre entier)")
        }
        Button {
            visible: (root.info.realRate ?? 0) > 0
            text: qsTr("Débit réel : %1").arg(Units.compact(root.info.realRate ?? 0, "bit/s", 4))
            flat: true
            onClicked: root.generator.adoptFrameTiming(root.channel, root.index, true)
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: qsTr("La trame prend le débit de son protocole, et le signal se cadre sur elle (fréquence d'échantillonnage et durée)")
        }
        Button {
            visible: root.missing.length > 0
            text: root.missing.length === 1 ? qsTr("Ajouter la ligne %1").arg(root.missing[0])
                                            : qsTr("Ajouter les lignes %1").arg(root.missing.join(", "))
            flat: true
            onClicked: root.generator.addFrameLines(root.channel, root.index)
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: qsTr("Chaque autre ligne de la trame devient une voie de plus, à son nom ; un réglage fait sur l'une vaut ensuite pour toutes")
        }
    }
    Label {
        visible: (root.info.members ?? "").length > 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: qsTr("Lignes de cette trame : %1. Un réglage fait ici vaut pour toutes.").arg(root.info.members ?? "")
        color: Theme.textMuted
        font.pixelSize: Theme.smallText
    }
}
