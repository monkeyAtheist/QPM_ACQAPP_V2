pragma Singleton
import QtQuick

// The lists of choices of the settings (acquisition, generator): key as the settings store it, name as shown.
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
    // The waveforms of the generator (keys of qacq::analysis::waveformKey), in the order of its menus.
    readonly property var generatorWaveforms: [
        { key: "sine", name: qsTr("Sinus"), tip: qsTr("A·sin(2π f t + φ)") },
        { key: "square", name: qsTr("Carré"), tip: qsTr("+A puis −A sur chaque période, selon le rapport cyclique") },
        { key: "triangle", name: qsTr("Triangle"), tip: qsTr("Montée et descente linéaires entre −A et +A") },
        { key: "sawtooth", name: qsTr("Dent de scie"), tip: qsTr("Montée linéaire de −A à +A, puis retour brusque") },
        { key: "pulse", name: qsTr("Impulsions"), tip: qsTr("A pendant le rapport cyclique de chaque période, 0 ensuite") },
        { key: "dc", name: qsTr("Continu"), tip: qsTr("Une valeur constante") },
        { key: "step", name: qsTr("Échelon"), tip: qsTr("0, puis A à partir de l'instant choisi") },
        { key: "impulse", name: qsTr("Dirac"), tip: qsTr("Une seule valeur non nulle, A, à l'instant choisi : un spectre plat") },
        { key: "noise", name: qsTr("Bruit"), tip: qsTr("Bruit blanc, gaussien ou uniforme") },
        { key: "sweep", name: qsTr("Balayage"), tip: qsTr("Un sinus dont la fréquence va d'une valeur à une autre sur toute la durée (vobulation, chirp)") },
        { key: "fourier", name: qsTr("Série de Fourier"), tip: qsTr("Une somme d'harmoniques de f₀ dont les coefficients aₙ, bₙ (ou Cₙ) sont des formules de n") },
        { key: "drawn", name: qsTr("Motif dessiné"), tip: qsTr("Une forme dessinée à la souris (ou tapée point par point), répétée à sa fréquence ou étalée sur toute la durée") },
        { key: "recurrence", name: qsTr("Récurrence"), tip: qsTr("y[n] calculé à partir des valeurs d'avant et d'une entrée x[n] : oscillateurs, filtres, suites") }
    ]
    // The input x[n] of a recurrence (keys of the recipe).
    readonly property var recurrenceInputs: [
        { key: "none", name: qsTr("Aucune (0)"), tip: qsTr("x[n] = 0 : y[n] ne vit que de ses valeurs initiales.") },
        { key: "impulse", name: qsTr("Dirac"), tip: qsTr("x[0] = 1, puis 0 : y[n] est la réponse impulsionnelle de la récurrence.") },
        { key: "step", name: qsTr("Échelon"), tip: qsTr("x[n] = 1 dès n = 0 : y[n] est sa réponse indicielle.") },
        { key: "noise", name: qsTr("Bruit blanc"), tip: qsTr("x[n] gaussien d'écart type 1, tiré de la graine du signal.") },
        { key: "above", name: qsTr("Les formes d'onde au-dessus"), tip: qsTr("x[n] est la somme des formes d'onde au-dessus de celle-ci, que y[n] remplace : la récurrence les filtre.") }
    ]
    // Ready-made recurrences (GeneratorController.applyRecurrenceModel).
    readonly property var recurrenceModels: [
        { key: "oscillator", name: qsTr("Oscillateur (sinus)") },
        { key: "lowpass", name: qsTr("Passe-bas du premier ordre (RC)") },
        { key: "resonator", name: qsTr("Résonateur (réponse à un Dirac)") },
        { key: "average", name: qsTr("Moyenne glissante sur 4 points") },
        { key: "walk", name: qsTr("Marche aléatoire") },
        { key: "logistic", name: qsTr("Suite logistique (chaos)") }
    ]
    // How the points of a drawn motif are joined (keys of the recipe).
    readonly property var motifInterpolations: [
        { key: "steps", name: qsTr("Paliers") },
        { key: "linear", name: qsTr("Segments") },
        { key: "smooth", name: qsTr("Courbe lisse") }
    ]
    // Ready-made drawn motifs (GeneratorController.applyMotifModel).
    readonly property var motifModels: [
        { key: "ramp", name: qsTr("Rampe de −A à +A") },
        { key: "stairs", name: qsTr("Escalier (4 marches)") },
        { key: "trapezoid", name: qsTr("Trapèze") },
        { key: "rc", name: qsTr("Charge et décharge RC (τ = T/10)") },
        { key: "heartbeat", name: qsTr("Cardiogramme") }
    ]
    // Forms of the coefficients of a Fourier series, and the names of its three formulas in each.
    readonly property var seriesForms: [
        { key: "ab", name: qsTr("aₙ et bₙ (réels)"), terms: ["a₀", "aₙ", "bₙ"],
          tip: qsTr("x(t) = a₀ + Σ aₙ cos(nω₀t) + bₙ sin(nω₀t), n = 1…N ; a₀ est la valeur moyenne") },
        { key: "cartesian", name: qsTr("Cₙ : parties réelle et imaginaire"), terms: ["C₀", "Re Cₙ", "Im Cₙ"],
          tip: qsTr("Coefficients complexes d'un signal réel : C₀ = a₀, Cₙ = (aₙ − j bₙ)/2 et C₋ₙ = Cₙ*") },
        { key: "polar", name: qsTr("Cₙ : module et phase"), terms: ["C₀", "|Cₙ|", "arg Cₙ (°)"],
          tip: qsTr("Coefficients complexes d'un signal réel, en module et argument (degrés) : Cₙ = |Cₙ| e^(j arg Cₙ), C₋ₙ = Cₙ*") }
    ]
    // Models of coefficients (GeneratorController.applySeriesModel).
    readonly property var seriesModels: [
        { key: "square", name: qsTr("Carré ±E") },
        { key: "triangle", name: qsTr("Triangle ±E") },
        { key: "sawtooth", name: qsTr("Dent de scie de −E à +E") },
        { key: "pulse", name: qsTr("Impulsions E, rapport cyclique D") },
        { key: "fullwave", name: qsTr("Sinus redressé double alternance (E |sin|)") },
        { key: "halfwave", name: qsTr("Sinus redressé simple alternance") }
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
