# QTacquisitionApp 2 — refonte

Outil pédagogique et utilitaire de développement : acquisition (carte à microcontrôleur en série,
instrument SCPI, flux réseau, carte son) et traitement du signal.
Ce dépôt est la réécriture de QtAcquisitionApp (« Architecture milestone 57 »), menée selon l'audit du 20/11/2026.

**Statut : phase 2 (acquisition) en cours.** Le cœur de calcul, le décodage des acquisitions, le modèle
de session et leurs tests sont en place ; l'application Qt Quick arrive en phase 3. L'ancienne application reste utilisable d'ici là.

## Décisions (20/11/2026)

- Interface en Qt Quick, en reprenant la coquille de QTsignalApp V2 (ruban, panneaux repliables, thème).
- Deux applications séparées, qui partagent la coquille et les composants.
- Première version : acquisition, analyse, générateur, filtres et modulations. Les labos OFDM, codage,
  HARQ et adaptation de lien viendront ensuite.
- Interface en français, toutes les chaînes prêtes pour la traduction (`qsTr`).
- Matériel visé : carte à microcontrôleur en série, instrument SCPI, flux réseau, carte son.

## Structure

```
dsp/                         qacq_dsp : cœur de traitement du signal C++20, sans aucune dépendance Qt
  include/qacq/dsp/constants.h     la seule définition de pi du projet
  include/qacq/dsp/fft.h           la seule FFT : toute longueur (radix 2, Bluestein), plans réutilisables
  include/qacq/dsp/window.h        fenêtres périodiques (analyse) et symétriques (conception de FIR)
  include/qacq/dsp/spectrum.h      spectre d'amplitude et DSP, spectre I/Q centré, Welch, spectrogramme
  include/qacq/dsp/tone.h          fondamentale, harmoniques, THD, SINAD, SNR, SFDR, ENOB
  include/qacq/dsp/filter_design.h IIR (Butterworth, Chebyshev I et II, Bessel) en sections, FIR fenêtrés
  include/qacq/dsp/filtering.h     filtrage en sections, aller-retour sans déphasage, FIR par FFT
  include/qacq/dsp/random.h        aléatoire reproductible (xoshiro256**, même suite sur tout compilateur)
  include/qacq/dsp/generator.h     générateur : formes échantillonnées ou limitées en bande (série de Fourier)
  include/qacq/dsp/modulation.h    AM, FM, PM, ASK, FSK, BPSK, QPSK ; porteuse réelle ou bande de base I/Q
  include/qacq/dsp/channel.h       bruit blanc gaussien réglé en SNR ou en Eb/N0
  include/qacq/dsp/receiver.h      récepteurs (corrélation, filtre adapté RRC, FSK non cohérente), TEB
io/                          qacq_io : décodage des données acquises, sans aucune dépendance Qt
  include/qacq/io/decoder.h        interface commune : octets reçus découpés n'importe où -> trames
  include/qacq/io/text_decoder.h   lignes de nombres (CSV, virgule décimale, « nom:valeur », en-tête)
  include/qacq/io/binary_decoder.h flux brut ; trames à synchronisation, compteur et somme de contrôle
  include/qacq/io/crc.h            sommes, XOR, CRC-8, CRC-16 (CCITT, XMODEM, MODBUS), CRC-32
  include/qacq/io/scpi.h           réponses d'instruments : nombres ASCII, blocs IEEE 488.2, SYST:ERR?
  include/qacq/io/spsc_ring.h      tampon circulaire sans verrou entre le thread d'acquisition et l'interface
model/                       qacq_model : la session, sans aucune dépendance Qt
  include/qacq/model/signal.h       signal à voies, fréquence, unités, origine ; échantillons partagés
  include/qacq/model/document.h     liste des signaux avec annuler/rétablir (historique borné en mémoire)
  include/qacq/model/session_file.h fichier de session .qacq : en-tête JSON lisible + échantillons binaires
  include/qacq/model/csv.h          import et export CSV (virgule décimale, colonne de temps, en-tête)
  include/qacq/model/wav.h          import et export WAV (PCM 8 à 32 bits, flottant, multivoie)
  include/qacq/model/json.h         JSON sans Qt (ordre des membres gardé, nombres exacts)
acquisition/                 qacq_acquisition : logique d'acquisition, sans aucune dépendance Qt
  include/qacq/acquisition/settings.h        réglages des sources (série, TCP, UDP, SCPI, carte son, simulateur), JSON
  include/qacq/acquisition/recorder.h        tuyau sans verrou vers l'interface, mémoire de l'acquisition en cours
  include/qacq/acquisition/rate_estimator.h  fréquence d'échantillonnage mesurée sur le flux
  include/qacq/acquisition/trigger.h         déclenchement logiciel (seuil, front, hystérésis, pré-déclenchement)
  include/qacq/acquisition/simulator.h       appareil simulé : texte, binaire, trames, instrument SCPI, défauts
  include/qacq/acquisition/firmware_example.h exemple de programme pour la carte (Arduino ou C portable)
  include/qacq/acquisition/stream_writer.h   enregistrement continu sur disque (CSV, WAV)
app/                         qacq_app : couche applicative sur QtCore (sans fenêtre, testable)
  include/qacq/app/signal_list_model.h liste des signaux pour QML, annuler/rétablir
  include/qacq/app/session.h        nouveau, ouvrir, enregistrer, importer, exporter, réglages des modules
  include/qacq/app/app_settings.h   réglages mémorisés : fichiers récents, dernier dossier, préréglages
  include/qacq/app/acquisition_source.h  les sources, chacune sur son thread : série, TCP, UDP, SCPI, carte son, simulateur
  include/qacq/app/acquisition_controller.h  démarrer, arrêter, statistiques, déclenchement, enregistrement continu
  include/qacq/app/serial_port_list.h  ports série présents, carte reconnue (Arduino, STM32, ESP32…), branchement détecté
tests/                       qacq_tests (cœurs) et qacq_app_tests (couche Qt) : un groupe CTest par étiquette
  fixtures/scipy_reference.inc     résultats SciPy (générés, ne pas éditer)
  signals.h                        signaux d'essai déterministes, identiques en C++ et en Python
tools/reference/             make_scipy_reference.py : régénère les références SciPy
scripts/                     build.cmd et run.cmd : compiler, tester et lancer sous Windows
cmake/                       options de compilation communes (avertissements stricts, pas de FMA)
```

## Compiler et tester

Sous Windows, avec Qt 6.11 et son compilateur MinGW :

```
scripts\build.cmd        configure, compile et lance tous les tests (journaux dans build\logs\)
```

En phase 0, `scripts\run.cmd` indique seulement que l'application n'existe pas encore.

Ou directement, avec les préréglages de `CMakeUserPresets.json` (propres à ce PC) :

```
cmake --preset mingw-release
cmake --build --preset mingw-release
ctest --preset mingw-all
```

Partout ailleurs (Linux, CI) : `cmake --preset release && cmake --build --preset release && ctest --preset all`.
Préréglages `ci` (avertissements traités comme des erreurs), `sanitize` (AddressSanitizer + UBSan) et `tsan`
(ThreadSanitizer, pour le tampon sans verrou).

Pour régénérer les références : `python tools/reference/make_scipy_reference.py` (numpy et scipy).

## Ce que corrige le cœur, par rapport à l'ancienne application

| Défaut de l'audit | Avant | Maintenant (test) |
| --- | --- | --- |
| 1. THD sur un long enregistrement (1 001,37 Hz, 10 % H3, 5 % H5, 48 kHz) | 1,13 % lu sur 10 s, fondamentale 0,40 au lieu de 1 sur 20,8 s | 11,180340 % sur 4,2 s, 10 s et 20,8 s ; f0 à 1e-11 près (`[audit]`) |
| 2. IIR en forme directe b/a | 37 passe-bas faux ou divergents sur 160, passe-bande d'ordre 6 faux jusqu'à 1 kHz | sections du second ordre : les 196 cas justes ; ordre jusqu'à 32 (Bessel 12) |
| 3. Portions du signal ignorées | spectre limité aux 65 536 derniers points ; I/Q tronqué à la puissance de 2 inférieure | toujours tout l'enregistrement, toute longueur (FFT de Bluestein) |
| 4. Plafond de 80 M opérations | FIR 127 coefficients refusé sur 21 s à 48 kHz | 60 s à 48 kHz filtrées en 0,3 s (FFT par recouvrement-addition) |
| Duplication | 4 FFT, 18 définitions de pi, 3 chemins FIR | une FFT, une constante, un moteur de filtrage |
| Carré à 1,3 kHz échantillonné à 48 kHz | repliement à −16,4 dB des harmoniques (raie la plus forte −25,6 dB) | mode « limité en bande » : aucune raie parasite (< 1e-10), dépassement de Gibbs 8,95 % (`[generator]`) |
| Bruit du générateur | `std::normal_distribution` : suite différente selon le compilateur | même suite partout, vérifiée sur les vecteurs publiés de xoshiro256** et splitmix64 |
| Décodeur ASCII | quadratique : 57 à 650 ms/Mo selon la taille des blocs, ligne en attente sans limite | 8 ms/Mo quelle que soit la taille des blocs ; lignes bornées (4 096 octets par défaut) (`[text]`) |
| Binaire | sans trame : un octet perdu décale toutes les valeurs suivantes | trames à synchronisation, compteur et CRC : reprise après corruption, trames perdues comptées et marquées (`[binary]`) |
| SCPI | réponses ASCII seulement | blocs binaires IEEE 488.2 (formes d'onde d'oscilloscope), valeurs spéciales SCPI-99 (`[scpi]`) |
| Tampon d'acquisition | verrou lecture-écriture, 65 536 échantillons par voie | tampon sans verrou, un producteur et un consommateur, vérifié sous ThreadSanitizer (`[ring]`) |

Comparaison à SciPy 1.18 (19 filtres IIR, dont les cinq cas qui faisaient défaut) : réponse en fréquence à
moins de 4e-9 dB, sortie de `sosfilt` et de `sosfiltfilt` à moins de 3e-12 en valeur relative ;
coefficients `firwin` à 1e-13 ; Welch à 1e-9.

## Modulations, canal et taux d'erreur

Conventions de l'ancienne application conservées : BPSK 0 → +1 ; QPSK de Gray 00 → (+,+), 01 → (−,+),
11 → (−,−), 10 → (+,−) ; ASK tout ou rien ; FSK à phase continue ; trame UART 8N1 (départ 0, 8 bits poids
faible en tête, arrêt 1). Bruit réel de variance N0·fs/2, complexe N0·fs/2 par voie, Eb = P/Rb.

Taux d'erreur mesurés sur 200 000 bits (`[ber]`), tous dans 5 écarts-types de la théorie :

| Liaison | Eb/N0 | Mesuré | Théorie |
| --- | --- | --- | --- |
| BPSK, porteuse réelle | 6 dB | 2,45e-3 | 2,39e-3 |
| QPSK, porteuse réelle | 6 dB | 2,39e-3 | 2,39e-3 |
| ASK tout ou rien, porteuse réelle | 9 dB | 2,45e-3 | 2,41e-3 |
| FSK non cohérente, porteuse réelle | 10 dB | 3,57e-3 | 3,37e-3 |
| BPSK en cosinus surélevé (0,35), bande de base | 5 dB | 5,87e-3 | 5,95e-3 |

Le récepteur à corrélation est adapté à l'impulsion réellement émise, image à 2 fc comprise. Quand 2 fc/Rs
n'est pas entier, l'énergie de chaque symbole dépend de la phase de la porteuse à son début : un léger excès
d'erreurs (≈ +8 % pour fc = 3,7 Rs), qui disparaît quand fc ≫ Rs. L'essai le documente.

## Décodage des données acquises

Chaque décodeur reçoit les octets tels qu'ils arrivent (coupés n'importe où, même au milieu d'un nombre ou
d'une trame), produit des trames d'échantillons simultanés (une valeur par voie), compte ce qu'il rejette et
marque les ruptures (données perdues, nouvel enregistrement) pour que l'affichage coupe la courbe à cet endroit.

- **Texte** (`Serial.println`, `printf`) : une ligne par trame, colonnes séparées par virgule, point-virgule,
  tabulation ou espaces ; virgule décimale en option ; paires « nom:valeur » à la manière du traceur série
  d'Arduino ; une ligne d'en-tête sans nombre nomme les voies ; `#` pour un commentaire.
- **Trames binaires**, le choix robuste pour une carte à microcontrôleur :
  `[synchro 1 à 8 octets][compteur 0, 1 ou 2 octets][échantillons][somme de contrôle]`, types int8 à float64,
  petit ou grand boutisme, échelle et décalage (points de convertisseur vers volts). Après une corruption, le
  décodeur retrouve la trame suivante ; le compteur révèle les trames perdues. Sans somme de contrôle, une
  trame n'est acceptée que si la suivante commence aussi par la synchro.
- **SCPI** : nombres ASCII ou blocs `#<n><longueur><données>` ; avec plusieurs voies, les valeurs sont
  entrelacées. Une réponse de deux trames ou plus (une forme d'onde) forme un enregistrement à part.

## Session

Un signal est une valeur partagée et non modifiable : renommer un signal de dix millions d'échantillons ne
copie que quelques chaînes, et annuler cinquante étapes ne coûte que les échantillons qui ont vraiment
changé. L'historique garde des états complets (annulation exacte quelle que soit l'opération) et oublie les
plus anciens au-delà de 1 Gio d'échantillons conservés pour lui seul ou de 200 étapes. Les réglages qu'on fait
glisser (curseur) se fondent en une seule étape.

Le fichier de session `.qacq` commence par `QACQSESS`, puis un en-tête JSON lisible dans un éditeur de texte
(signaux, voies, unités, ruptures, provenance, réglages des modules), puis les échantillons en petit boutisme,
voie après voie. Une voie dont toutes les valeurs sont exactement des float32 (points de convertisseur) est
stockée sur 4 octets, sans perte. Chaque voie a son CRC-32 : un fichier abîmé est signalé, jamais relu avec
de fausses valeurs. L'enregistrement passe par un fichier temporaire : un échec ne détruit pas la session
précédente. 24 Mo s'écrivent et se relisent en moins de 0,1 s.

## Acquisition

**Les sources** (couche Qt) tournent chacune dans son propre thread, pour qu'un appareil lent ou un réseau
bloqué ne fige jamais l'interface :
- **Port série** : vitesse, bits, parité, contrôle de flux, DTR ; une carte débranchée arrête l'acquisition
  proprement en gardant ce qui a été reçu. La liste des ports reconnaît la carte (Arduino, STM32 ST-Link,
  ESP32, Pico, Teensy, CH340, CP210x, FTDI…) et signale un port qui apparaît.
- **TCP** (client) et **UDP** (écoute sur toutes les interfaces ou seulement ce PC, multidiffusion).
- **Instrument SCPI sur le réseau** (port 5025) : `*IDN?`, commandes de préparation, lecture de `SYST:ERR?`,
  puis la requête répétée ; la suivante part une fois la réponse entière reçue, une réponse en retard est
  redemandée. Une forme d'onde (`:WAV:DATA?`) devient un enregistrement marqué par une rupture. L'USB (VISA)
  viendra ensuite.
- **Carte son** : entrée choisie par son nom, format vérifié avant de démarrer.
- **Simulateur** : une carte imaginaire, au rythme réel.

**Le contrôleur** démarre et arrête, publie les statistiques (octets, trames, rejets, erreurs de CRC, trames
perdues, fréquence mesurée, mémoire), déclenche, enregistre en continu ; à l'arrêt, ce qui a été reçu devient un
signal de la session (annulable), avec sa provenance : réglages, statistiques, identité de l'instrument.
Ses essais n'ont besoin d'aucun matériel : le programme de test lance lui-même un serveur TCP, un émetteur UDP
et un faux instrument SCPI.

Sans Qt, déjà là et testé :
- **Réglages** de chaque source et du décodage, en JSON (préréglages, provenance du signal acquis), avec
  vérification et messages en français. La carte son donne toujours des échantillons bruts, un instrument
  toujours des réponses SCPI.
- **Tuyau sans verrou** du thread d'acquisition vers l'interface : si l'interface prend du retard, des trames
  entières sont abandonnées, comptées et marquées comme rupture, jamais coupées en deux.
- **Mémoire de l'acquisition** par blocs de 64 Ki échantillons : arrêt à la limite (512 Mio par défaut, ou une
  durée) ou fenêtre glissante qui garde les dernières minutes.
- **Fréquence mesurée** par régression sur 5 s d'arrivées : une horloge de carte décalée de 0,1 % est mesurée
  à 0,02 % près malgré des lectures par paquets toutes les 8 à 24 ms.
- **Déclenchement** comme un oscilloscope : seuil, front montant ou descendant, hystérésis, pré-déclenchement,
  modes automatique, normal et unique ; instant interpolé au millième d'échantillon.
- **Simulateur** : les mêmes octets qu'une carte (texte, binaire, trames avec compteur et CRC) ou qu'un
  instrument SCPI (`*IDN?`, `MEAS?`, `:WAV:DATA?`, `SYST:ERR?`), avec octets abîmés, trames perdues et horloge
  décalée à volonté : toute la chaîne se teste sans matériel.
- **Exemple de programme pour la carte** : croquis Arduino ou C portable (STM32, ESP32, PIC) qui envoie
  exactement le format réglé, CRC compris. Vérifié ici : le C généré compile sans avertissement
  (`-std=c99 -Wall -Wextra -Wconversion -pedantic`) pour 18 combinaisons de format, et ses trames sont décodées
  sans une erreur.
- **Enregistrement continu** sur disque en CSV ou WAV ; un WAV interrompu (plantage) se relit quand même.

## Méthode de l'analyse de tonalité

1. f0 approchée : pic d'un spectre de Hann (2^20 points au plus), interpolation gaussienne.
2. Affinage : ajustement aux moindres carrés de la fondamentale et de ses harmoniques sur les deux moitiés
   d'une portion croissante de l'enregistrement ; la dérive de phase entre les moitiés donne l'erreur de fréquence.
3. Amplitudes : un seul ajustement de la composante continue et de toutes les harmoniques sur tout
   l'enregistrement (matrice normale en forme fermée, coût O(N K)).

Limite connue : une composante qui n'est pas une harmonique (50 Hz du secteur, par exemple) n'est pas dans le
modèle ; elle fuit très peu dans l'ajustement (quelques 1e-8 en valeur relative dans l'essai à 50 Hz + 3,3 kHz).

## Feuille de route

0. **Socle** (fait) : dépôt, CMake, CI, cœurs sans Qt et banc de références : FFT, fenêtres, spectres,
   tonalité, filtres, générateur, modulations, canal et taux d'erreur, décodeurs d'acquisition (texte,
   trames binaires, SCPI), tampon sans verrou.
1. **Modèle et session** (fait) : sans Qt : signal, liste avec annuler/rétablir, fichier de session, CSV, WAV ;
   couche Qt : modèle de liste pour QML, session (ouvrir, enregistrer, importer, exporter), réglages mémorisés.
2. **Acquisition** (en cours) : fait : réglages, tuyau, mémoire, fréquence mesurée, déclenchement, simulateur,
   exemples de programme, enregistrement continu ; sources Qt sur leur thread (série, TCP, UDP, SCPI sur le
   réseau, carte son, simulateur), liste des ports, contrôleur d'acquisition. Reste : VISA pour l'USB.
3. Coquille Qt Quick reprise de QTsignalApp V2 et moteur de tracé unique.
4. Modules : oscilloscope, analyse, générateur, filtres, modulations.
5. Ensuite : rééchantillonnage, corrélation, transformée en Z, OFDM et codage, théorie, outils.
