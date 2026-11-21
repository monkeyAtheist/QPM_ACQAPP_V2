# QTacquisitionApp 2 — refonte

Outil pédagogique et utilitaire de développement : acquisition (carte à microcontrôleur en série,
instrument SCPI, flux réseau, carte son) et traitement du signal.
Ce dépôt est la réécriture de QtAcquisitionApp (« Architecture milestone 57 »), menée selon l'audit du 20/11/2026.

**Statut : phase 4 (les modules) en cours.** Le cœur de calcul, le décodage des acquisitions, le modèle de
session, l'acquisition sur ses threads et leurs tests sont en place. La fenêtre Qt Quick existe, sur la coquille
de QTsignalApp V2 : modules Signaux (visualiser), Acquisition et Analyse (spectre et mesures) ; Générateur,
Filtres et Modulations arrivent ensuite. L'ancienne application reste utilisable d'ici là.

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
  include/qacq/app/units.h          quantités pour les champs de QML : « 48 kHz » affiché, « 44,1 kHz » saisi
  include/qacq/app/analysis_controller.h  module Analyse : calcul sur son thread, mesures mises en forme, export
  include/qacq/app/curves_controller.h    courbes ajoutées : liste (session), courbes du temps sur un thread
plot/                        qacq_plot : le moteur de tracé de toutes les vues, sans aucune dépendance Qt
  include/qacq/plot/axis.h          graduations 1-2-5 et log, un préfixe par axe, quantités affichées et saisies
  include/qacq/plot/trace.h         tracé à tout zoom : extrêmes par colonne de pixels, pyramide min/max, ruptures
analysis/                    qacq_analysis : le module Analyse sans Qt (spectre d'une zone, mesures, CSV)
  include/qacq/analysis/analysis.h  spectre d'amplitude ou densité (Welch), réel ou I/Q, niveaux en dB, mesures
  include/qacq/analysis/measures.h  statistiques A-B, mesures d'oscilloscope, pics, bande à −3 dB
  include/qacq/analysis/calculus.h  intégrale, dérivée, valeurs par période, courbe calculée (nouveau signal)
  include/qacq/analysis/curves.h    courbes ajoutées : sortes, noms, unités, liste en JSON, calcul, « Garder »
ui/                          QTacquisitionApp : la fenêtre Qt Quick (coquille de QTsignalApp V2)
  signal_plot.h                    le tracé (QQuickPaintedItem) : signal de la session, défilement, captures
  spectrum_plot.h                  le spectre : fréquences linéaires ou log, niveaux en dB ou linéaires, harmoniques
  qml/Main.qml                     fenêtre, barre des modules, ruban, liste des signaux, propriétés
  qml/AcquisitionView.qml          module Acquisition : tracé en direct, ruban, état et chiffres de la liaison
  qml/AcquisitionSettings.qml      réglages de la source, du format, de la cadence, des voies ; préréglages
  qml/SignalsView.qml              module Signaux : le signal choisi à tout zoom, avec la lecture au pointeur
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

```
scripts\run.cmd          compile ce qui a changé (l'application seule) puis la lance ; messages dans build\logs\app.txt
```

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

## Le tracé

Un seul moteur pour toutes les vues (`plot/`, sans Qt, testé) ; l'élément Qt Quick `SignalPlot` ne fait que
dessiner ce qu'il calcule.
- **À tout zoom** : la durée visible est découpée en colonnes de pixels. Avec plus de deux échantillons par
  colonne, chacune garde ses extrêmes, son premier et son dernier échantillon : aucun pic n'est jamais perdu,
  quel que soit le zoom. Plus près, les échantillons eux-mêmes, avec un point sur chacun quand on les distingue.
- **Vite** : une pyramide d'extrêmes par blocs (16, 64, 256… échantillons) donne les extrêmes de n'importe quel
  intervalle en O(log n) : dix millions d'échantillons se tracent aussi vite que mille.
- **Ruptures** (échantillons perdus, enregistrement repris) : le trait est coupé et marqué en pointillés.
- **Axes** : graduations 1-2-5, un seul préfixe par axe (« Temps (ms) » : 0 ; 0,5 ; 1…), virgule décimale ; une
  bande par voie à son échelle, ou toutes sur un même axe ; échelle automatique stable en direct (elle suit
  aussitôt ce qui en sort, ne rétrécit que quand le signal n'occupe plus que 40 % de la hauteur).
- **Souris** : molette pour le zoom du temps autour du pointeur (en direct : la largeur de la fenêtre), Ctrl +
  molette pour les valeurs, glisser pour se déplacer, clic droit glissé pour zoomer sur un intervalle, double clic
  pour tout voir ; la lecture suit le pointeur (instant et valeur de chaque voie à l'échantillon le plus proche).
- **En direct** : défilement des dernières secondes, ou dernière capture du déclenchement (t = 0 à l'instant du
  déclenchement, niveau marqué), « Figer » pour lire pendant que les données continuent d'arriver.

## L'analyse

Le module Analyse montre en haut le signal choisi ; dessous, **seulement les courbes qu'on ajoute**
(Courbes ▸ Ajouter…). Sans courbe, le signal prend toute la hauteur.
- **Graphe du temps** : la voie elle-même (pour superposer), son intégrale, sa dérivée, sa moyenne, sa valeur
  efficace ou sa fréquence par période ; sur tout le signal ou entre A et B. Il suit le zoom et les curseurs
  du signal. Superposées, deux unités ont chacune leur échelle (gauche et droite) ; au-delà, une bande par
  unité.
- **Graphe des fréquences** : le spectre (amplitude ou densité) des voies choisies, seules calculées.
- Les voies du signal, en haut, se superposent aussi (groupe « Voies du signal »).
- Les courbes sont des vues du signal choisi, recalculées quand il change, enregistrées dans la session ;
  « Garder » fait un signal de la session des courbes du temps affichées. Une étiquette par courbe : un clic
  la masque, la croix la retire.
- **Zone analysée** (spectres et mesures) : tout le signal (par défaut : le zoom ne change rien), la partie
  visible, ou entre les curseurs A et B. Un signal dont la fréquence change (balayage) a bien un spectre
  différent selon la partie regardée.
- **Spectre d'amplitude** : une FFT de toute la zone ; une sinusoïde se lit à son amplitude crête (0 dBV pour
  1 V). Fenêtres de Hann, à sommet plat (amplitude exacte), Blackman-Harris (−92 dB), Blackman, Hamming,
  Kaiser (β réglable), rectangulaire ; zéros ajoutés × 2, 4 ou 8. Au-delà de 65 536 échantillons, la FFT
  passe à la puissance de 2 suivante (zéros ajoutés, tous les échantillons utilisés, c'est indiqué) ; limite
  2^24 points, au-delà le module propose la densité ou un zoom.
- **Densité spectrale** (Welch) : moyenne de segments qui se recouvrent (256 à 65 536 échantillons, 0 à 75 %),
  en unité/√Hz ou en dB ; une paire I/Q a aussi sa densité (moyenne des spectres complexes).
- **Paire I/Q** : spectre de −fs/2 à +fs/2 (fréquences négatives).
- **Composante continue** : retirée par la moyenne pondérée par la fenêtre (Σ w x / Σ w), segment par segment
  en densité. La moyenne simple d'une zone qui ne tient pas un nombre entier de périodes n'est pas la
  composante continue : la retirer laissait un faux palier à 0 Hz et sur la première raie (−42 dB sur
  l'essai à 440 Hz), qui changeait avec le zoom ; il ne reste plus que la fuite propre de la fenêtre.
- **Mesures de chaque voie** sur la même zone : moyenne, efficace, efficace sans la moyenne, minimum, maximum,
  crête à crête, facteur de crête, première et dernière valeur, intégrale (aire, en V·s) ; tonalité : fréquence, amplitude, THD, SINAD, rapport signal/bruit, SFDR,
  bits effectifs, tableau des harmoniques. Une harmonique est repérée (H2, H3…) sur le spectre quand elle
  dépasse d'environ 10 dB la médiane des raies de part et d'autre d'elle (hors de son lobe) ; les autres
  restent dans le tableau, en gris.
- **Tracé** : fréquences linéaires ou logarithmiques, niveaux en dB (dBV, dB re 1 A…) ou linéaires ; mêmes
  gestes que le tracé du temps ; la lecture donne le plus haut point sous le pointeur (fréquence, niveau, et
  la valeur en dB). Le calcul tourne sur son propre thread : zoomer pendant un calcul le relance avec la
  dernière zone ; changer dB/linéaire ou l'échelle des fréquences ne recalcule rien.
- **Export** du spectre en CSV (fréquence, niveau en dB et en valeur, virgule décimale).
- **Mesures temporelles** de chaque voie (comme un oscilloscope) : niveaux haut et bas (modes de
  l'histogramme, ou extrêmes), période et fréquence par les fronts (hystérésis de 10 %), rapport cyclique,
  montée et descente 10-90 % ou 20-80 % (réglage), dépassements, moyenne et efficace sur les périodes entières
  (sans le biais d'une période coupée au bord de la zone), temps d'établissement à ±1 % et ±0,1 % après le
  dernier front (vers un niveau plat seulement : carré, échelon). **Pics du spectre** : les 8 plus hauts, un
  par lobe, et la bande à −3 dB autour du maximum (passe-bas, passe-haut ou bande : coupures, largeur, centre).
- **Calculs des courbes du temps** : intégrale cumulée (trapèzes, moyenne retirée au choix), dérivée
  (différences centrées), moyenne, efficace et fréquence par période (une valeur par période, d'un front
  montant au suivant, vide hors des périodes entières).

## Outils des tracés

- **Curseurs A et B** (Signaux, Acquisition figée ou déclenchée, Analyse) : on les glisse, ou Ctrl + clic
  place A et Maj + clic place B. Sur le temps : Δt, 1/Δt, valeur de chaque voie sous A et sous B, et entre A
  et B min, max, moyenne, efficace, crête à crête et intégrale (V·s). Sur le spectre : Δf, niveaux et écart en
  dB, valeur efficace de la bande A-B (somme de la densité sur la bande).
- **Se déplacer** : glisser le tracé avec le bouton gauche déplace ce que le geste suit surtout : à
  l'horizontale le temps (ou les fréquences), à la verticale les valeurs, en biais les deux (le sens se décide
  après 6 pixels, pour qu'un défilement dans le temps un peu de travers ne touche pas à l'échelle). Glisser une
  échelle ne déplace que ses valeurs (deux unités superposées : l'une sans l'autre), glisser l'axe du temps
  que le temps. Aussi : Maj + molette, défilement horizontal (pavé tactile, molette inclinable), flèches ← →
  (Maj : une demi-vue), ↑ ↓ pour les valeurs de la voie sous le pointeur, Début / Fin, + / − pour zoomer
  (après un clic sur le tracé), et une barre de défilement sous le tracé dès qu'il est zoomé. En acquisition
  qui défile, seules les valeurs se déplacent.
- **Échelles des valeurs** : la molette sur une échelle (ou Ctrl + molette sur le tracé) la zoome autour du
  pointeur ; un double clic sur une échelle la rend automatique, « Échelle verticale automatique » (clic
  droit) les rend toutes automatiques sans toucher au temps, un double clic sur le tracé remet tout.
- **Clic droit** (sans glisser ; glisser zoome toujours) : le menu du tracé, avec une case par voie ou par
  courbe à afficher, « Superposées », Tout afficher, Échelle verticale automatique, Tout voir, et pour les
  courbes : Ajouter, Garder, Retirer les courbes masquées. Superposées, deux unités ont chacune leur échelle
  (dans la couleur de leur voie quand elle est seule de son côté). Le bouton « Superposées » du ruban fait de
  même (une bande par voie quand il est éteint).
- **Marqueur de pic** (Analyse) : sur le plus haut pic, puis le suivant (clic droit : le précédent).
- **Vue précédente** : chaque zoom, déplacement ou « tout voir » du temps (ou des fréquences et des niveaux du
  spectre) se défait (50 pas) ; les échelles des valeurs du temps reviennent par le double clic.
- **Image** : le graphique copié dans le presse-papiers ou enregistré en PNG, tel qu'à l'écran.

## Méthode de l'analyse de tonalité

1. f0 approchée : pic d'un spectre de Hann (2^20 points au plus), interpolation gaussienne. Un enregistrement
   plus long est aussi regardé en entier, moyenné par blocs : une onde lente que ses 2^20 premiers points ne
   séparent pas de la composante continue (0,1 Hz sur 10 millions de points à 48 kHz) y est trouvée.
2. Affinage : ajustement aux moindres carrés de la fondamentale et de ses harmoniques sur les deux moitiés
   d'une portion croissante de l'enregistrement ; la dérive de phase entre les moitiés donne l'erreur de fréquence.
   Un enregistrement de moins de quatre périodes (pas de dérive mesurable) prend la fréquence qui laisse le
   plus petit résidu, cherchée à moins d'une raie (section dorée) : 40 points d'une sinusoïde, 2,5 périodes,
   donnent 6,25 Hz à 2×10⁻⁹ près au lieu de 6,2545 Hz (et une distorsion fictive de −63 dB).
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
2. **Acquisition** (fait, sauf VISA) : réglages, tuyau, mémoire, fréquence mesurée, déclenchement, simulateur,
   exemples de programme, enregistrement continu ; sources Qt sur leur thread (série, TCP, UDP, SCPI sur le
   réseau, carte son, simulateur), liste des ports, contrôleur d'acquisition. Reste : VISA pour l'USB.
3. **L'application** (faite) : moteur de tracé unique (sans Qt, testé), coquille Qt Quick reprise de
   QTsignalApp V2 (barre des modules, ruban, liste des signaux, propriétés, thème clair ou sombre), modules
   Signaux et Acquisition ; glisser-déposer de fichiers, préréglages, réglages retenus d'une fois sur l'autre ;
   import d'un fichier texte avec aperçu et reconnaissance de sa forme (virgule décimale, titres, colonne de
   temps en s, ms ou µs).
4. **Modules** (en cours) : analyse (spectre, densité, mesures, courbes ajoutées : fait) ; ensuite la fusion
   de Signaux et Analyse en un module (audit du 21/11), spectrogramme, générateur, filtres, modulations ; VISA
   pour les instruments en USB. À étudier : réponse en fréquence entre deux voies (gain, phase, marges),
   distorsion pondérée A.
5. Ensuite : rééchantillonnage, corrélation, transformée en Z, OFDM et codage, théorie, outils.
