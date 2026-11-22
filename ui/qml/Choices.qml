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
        { key: "recurrence", name: qsTr("Récurrence"), tip: qsTr("y[n] calculé à partir des valeurs d'avant et d'une entrée x[n] : oscillateurs, filtres, suites") },
        { key: "formula", name: qsTr("Opération entre voies"), tip: qsTr("y, une formule des autres voies (v1 + v2, v1*v2, v1/v2…), du temps t ou des formes d'onde au-dessus (x)") },
        { key: "modulation", name: qsTr("Porteuse modulée"), tip: qsTr("Une porteuse modulée en amplitude (AM), en fréquence (FM) ou en phase (PM) par un message : un sinus, une autre voie, les formes d'onde au-dessus") }
    ]
    // How a carrier is modulated (keys of the recipe).
    readonly property var carrierModulations: [
        { key: "am", name: qsTr("AM (amplitude)"), tip: qsTr("A·(1 + m·message)·cos(2π fc t + φ) : l'enveloppe suit le message ; avec un sinus de fréquence fm, deux raies latérales à fc ± fm.") },
        { key: "fm", name: qsTr("FM (fréquence)"), tip: qsTr("La fréquence instantanée vaut fc + Δf·message : avec un sinus de fréquence fm, des raies tous les fm autour de fc (fonctions de Bessel de β = Δf/fm), sur la bande de Carson 2(Δf + fm).") },
        { key: "pm", name: qsTr("PM (phase)"), tip: qsTr("La phase vaut φ + Δφ·message : c'est une FM du message dérivé.") }
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

    // ---- Filters ----
    readonly property var filterKinds: [
        { key: "iir", name: qsTr("RII : récursif (Butterworth, Chebyshev, Bessel, elliptique)"),
          tip: qsTr("Réponse impulsionnelle infinie : des flancs raides pour peu de calcul ; phase non linéaire") },
        { key: "fir", name: qsTr("RIF : non récursif (fenêtre ou équiondulation)"),
          tip: qsTr("Réponse impulsionnelle finie : phase linéaire et toujours stable ; plus de coefficients") },
        { key: "typed", name: qsTr("Coefficients tapés (b, a)"),
          tip: qsTr("H(z) = Σ b[k] z⁻ᵏ / Σ a[k] z⁻ᵏ, comme lfilter(b, a) de SciPy : des nombres ou des formules de k, des variables ; un RIF si a = 1") }
    ]
    readonly property var filterResponses: [
        { key: "lowpass", name: qsTr("Passe-bas"), tip: qsTr("Garde ce qui est en dessous de la coupure") },
        { key: "highpass", name: qsTr("Passe-haut"), tip: qsTr("Garde ce qui est au-dessus de la coupure") },
        { key: "bandpass", name: qsTr("Passe-bande"), tip: qsTr("Garde la bande entre la fréquence basse et la fréquence haute") },
        { key: "bandstop", name: qsTr("Coupe-bande"), tip: qsTr("Retire la bande entre la fréquence basse et la fréquence haute (un secteur à 50 Hz…)") }
    ]
    readonly property var filterFamilies: [
        { key: "butterworth", name: "Butterworth", tip: qsTr("Le plus plat possible dans la bande passante, sans ondulation ; −3 dB à la coupure") },
        { key: "chebyshev1", name: "Chebyshev I", tip: qsTr("Une ondulation dans la bande passante pour un flanc plus raide ; à la coupure, −ondulation") },
        { key: "chebyshev2", name: "Chebyshev II", tip: qsTr("Plat dans la bande passante, ondulation dans la bande coupée (des zéros) ; à la fréquence donnée, l'atténuation") },
        { key: "bessel", name: "Bessel", tip: qsTr("Un retard presque constant dans la bande : les formes passent sans dépassement ; flanc plus doux") },
        { key: "elliptic", name: qsTr("Elliptique (Cauer)"), tip: qsTr("Une ondulation dans les deux bandes pour le flanc le plus raide à ordre donné ; à la coupure, −ondulation, puis au moins l'atténuation dès la fréquence d'arrêt") }
    ]
    readonly property var firWindows: [
        { key: "rectangular", name: qsTr("Rectangulaire"), tip: qsTr("La transition la plus étroite, mais des lobes à −21 dB seulement") },
        { key: "hann", name: qsTr("Hann"), tip: qsTr("Lobes vers −44 dB") },
        { key: "hamming", name: qsTr("Hamming"), tip: qsTr("Lobes vers −53 dB : le choix habituel") },
        { key: "blackman", name: qsTr("Blackman"), tip: qsTr("Lobes vers −74 dB, transition plus large") },
        { key: "blackmanHarris", name: qsTr("Blackman-Harris"), tip: qsTr("Lobes au-delà de −90 dB, la transition la plus large") },
        { key: "kaiser", name: qsTr("Kaiser (β)"), tip: qsTr("Le compromis réglé par β : 5 vers −54 dB, 8,6 vers −87 dB") }
    ]
    // How a FIR is designed: one of the windows (as firWindows), or equiripple.
    readonly property var firDesigns: [
        { key: "rectangular", name: qsTr("Rectangulaire"), tip: qsTr("La transition la plus étroite, mais des lobes à −21 dB seulement") },
        { key: "hann", name: qsTr("Hann"), tip: qsTr("Lobes vers −44 dB") },
        { key: "hamming", name: qsTr("Hamming"), tip: qsTr("Lobes vers −53 dB : le choix habituel") },
        { key: "blackman", name: qsTr("Blackman"), tip: qsTr("Lobes vers −74 dB, transition plus large") },
        { key: "blackmanHarris", name: qsTr("Blackman-Harris"), tip: qsTr("Lobes au-delà de −90 dB, la transition la plus large") },
        { key: "kaiser", name: qsTr("Kaiser (β)"), tip: qsTr("Le compromis réglé par β : 5 vers −54 dB, 8,6 vers −87 dB") },
        { key: "equiripple", name: qsTr("Équiondulation (Parks-McClellan)"),
          tip: qsTr("Les transitions données, l'erreur répartie au mieux entre les bandes (poids tirés de l'ondulation et de l'atténuation) : le RIF optimal à nombre de coefficients donné") }
    ]
    readonly property var filterApplications: [
        { key: "causal", name: qsTr("Causale (comme en temps réel)"), short: qsTr("en causal"),
          tip: qsTr("Échantillon après échantillon : le retard et la phase du filtre, ce que ferait un microcontrôleur") },
        { key: "zerophase", name: qsTr("À phase nulle (aller-retour)"), short: qsTr("à phase nulle"),
          tip: qsTr("Une fois dans chaque sens : ni retard ni déphasage, gain au carré ; sur un enregistrement seulement") },
        { key: "aligned", name: qsTr("Alignée (RIF sans son retard)"), short: qsTr("aligné, sans retard"),
          tip: qsTr("Un RIF une fois, son retard de (N − 1)/2 échantillons retiré : le résultat reste en face de l'original") }
    ]
    // How a record is prolonged beyond its ends before filtering it at zero phase (forward, then backward).
    readonly property var edgeExtensions: [
        { key: "predicted", name: qsTr("Prédits (le signal continue)"), short: qsTr("prédits"),
          tip: qsTr("Prédiction linéaire d'après les échantillons proches de chaque bord : une somme de sinusoïdes continue telle quelle, ce qui s'éteint continue de s'éteindre ; pas de bouffée aux bords") },
        { key: "odd", name: qsTr("Symétrie impaire (comme SciPy)"), short: qsTr("impairs"),
          tip: qsTr("2·x[0] − x[k], le choix de filtfilt de SciPy et MATLAB : garde une tendance ; un signal qui commence sur une crête y fait une bouffée") },
        { key: "even", name: qsTr("Miroir (symétrie paire)"), short: qsTr("en miroir"),
          tip: qsTr("x[k] : garde une crête ; un signal qui commence en pente y fait une bouffée") },
        { key: "constant", name: qsTr("Constants (premier et dernier échantillon)"), short: qsTr("constants"),
          tip: qsTr("Le premier et le dernier échantillon répétés") },
        { key: "none", name: qsTr("Non prolongés"), short: qsTr("non prolongés"),
          tip: qsTr("Le filtre part du régime établi du premier échantillon : le début et la fin gardent leur transitoire") }
    ]
}
