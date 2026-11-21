pragma Singleton
import QtQuick

// The lists of choices of the acquisition settings: key as the settings store it, name as shown.
QtObject {
    readonly property var sources: [
        { key: "simulated", name: qsTr("Simulateur"), icon: "sim" },
        { key: "serial", name: qsTr("Port série"), icon: "port" },
        { key: "tcp", name: qsTr("TCP (client)"), icon: "network" },
        { key: "udp", name: qsTr("UDP"), icon: "network" },
        { key: "scpi", name: qsTr("Instrument SCPI"), icon: "instrument" },
        { key: "audio", name: qsTr("Carte son"), icon: "audio" }
    ]
    readonly property var decoders: [
        { key: "text", name: qsTr("Texte (valeurs séparées par ligne)") },
        { key: "raw", name: qsTr("Binaire brut (échantillons à la suite)") },
        { key: "frames", name: qsTr("Trames binaires (synchro, compteur, CRC)") }
    ]
    readonly property var sampleTypes: [
        { key: "int8", name: qsTr("Entier 8 bits signé"), bytes: 1 },
        { key: "uint8", name: qsTr("Entier 8 bits non signé"), bytes: 1 },
        { key: "int16", name: qsTr("Entier 16 bits signé"), bytes: 2 },
        { key: "uint16", name: qsTr("Entier 16 bits non signé"), bytes: 2 },
        { key: "int32", name: qsTr("Entier 32 bits signé"), bytes: 4 },
        { key: "uint32", name: qsTr("Entier 32 bits non signé"), bytes: 4 },
        { key: "float32", name: qsTr("Flottant 32 bits"), bytes: 4 },
        { key: "float64", name: qsTr("Flottant 64 bits"), bytes: 8 }
    ]
    readonly property var byteOrders: [
        { key: "le", name: qsTr("Petit-boutiste (Arduino, STM32, ESP32, PC)") },
        { key: "be", name: qsTr("Gros-boutiste (ordre réseau)") }
    ]
    readonly property var counters: [
        { key: "none", name: qsTr("Aucun") },
        { key: "u8", name: qsTr("8 bits") },
        { key: "u16", name: qsTr("16 bits") }
    ]
    readonly property var checksums: [
        { key: "none", name: qsTr("Aucune") },
        { key: "sum8", name: qsTr("Somme 8 bits") },
        { key: "xor8", name: qsTr("XOR 8 bits") },
        { key: "crc8", name: qsTr("CRC-8") },
        { key: "crc8-maxim", name: qsTr("CRC-8 Maxim (1-Wire)") },
        { key: "crc16-ccitt", name: qsTr("CRC-16 CCITT") },
        { key: "crc16-xmodem", name: qsTr("CRC-16 XMODEM") },
        { key: "crc16-modbus", name: qsTr("CRC-16 Modbus") },
        { key: "crc32", name: qsTr("CRC-32") }
    ]
    readonly property var parities: [
        { key: "none", name: qsTr("Aucune") },
        { key: "even", name: qsTr("Paire") },
        { key: "odd", name: qsTr("Impaire") },
        { key: "mark", name: qsTr("Marque") },
        { key: "space", name: qsTr("Espace") }
    ]
    readonly property var flows: [
        { key: "none", name: qsTr("Aucun") },
        { key: "hardware", name: qsTr("Matériel (RTS/CTS)") },
        { key: "software", name: qsTr("Logiciel (XON/XOFF)") }
    ]
    readonly property var waveforms: [
        { key: "sine", name: qsTr("Sinus") },
        { key: "square", name: qsTr("Carré") },
        { key: "triangle", name: qsTr("Triangle") },
        { key: "sawtooth", name: qsTr("Dent de scie") },
        { key: "pulse", name: qsTr("Impulsion") },
        { key: "dc", name: qsTr("Continu") },
        { key: "noise", name: qsTr("Bruit blanc") }
    ]
    readonly property var audioTypes: [
        { key: "int16", name: qsTr("16 bits") },
        { key: "int32", name: qsTr("32 bits") },
        { key: "float32", name: qsTr("Flottant 32 bits") }
    ]
    readonly property var scpiTransports: [
        { key: "tcp", name: qsTr("Réseau (TCP, port 5025)") },
        { key: "visa", name: qsTr("USB (VISA) : bientôt disponible") }
    ]
    readonly property var edges: [
        { key: "rising", name: qsTr("Front montant"), icon: "rising" },
        { key: "falling", name: qsTr("Front descendant"), icon: "falling" },
        { key: "either", name: qsTr("Les deux fronts"), icon: "either" }
    ]
    readonly property var triggerModes: [
        { key: "auto", name: qsTr("Auto"), tip: qsTr("Une capture libre si rien ne déclenche") },
        { key: "normal", name: qsTr("Normal"), tip: qsTr("Seulement sur déclenchement") },
        { key: "single", name: qsTr("Unique"), tip: qsTr("Une capture, puis on réarme à la main") }
    ]
    readonly property var baudRates: [300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 250000,
                                      460800, 500000, 921600, 1000000, 2000000]
    readonly property var audioRates: [8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000]

    function names(list) {
        return list.map((entry) => entry.name)
    }
    function indexOf(list, key) {
        for (let i = 0; i < list.length; ++i) {
            if (list[i].key === key)
                return i
        }
        return 0
    }
    function nameOf(list, key) {
        return list[indexOf(list, key)].name
    }
}
