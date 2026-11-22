# QTacquisitionApp 2 — refonte

Outil pédagogique et utilitaire de développement : acquisition (carte à microcontrôleur en série,
instrument SCPI, flux réseau, carte son) et traitement du signal.
Ce dépôt est la réécriture de QtAcquisitionApp (« Architecture milestone 57 »), menée selon l'audit du 20/11/2026.

**Statut : phase 4 (les modules) en cours.** Le cœur de calcul, le décodage des acquisitions, le modèle de
session, l'acquisition sur ses threads et leurs tests sont en place. La fenêtre Qt Quick existe, sur la coquille
de QTsignalApp V2 : modules Signaux (visualiser, mesurer, ajouter des courbes et des spectres),
Acquisition, Générateur et Filtres ; Modulations arrive ensuite. L'ancienne application reste utilisable
d'ici là.

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
  include/qacq/dsp/filter_design.h IIR (Butterworth, Chebyshev I et II, Bessel, elliptique) en sections, FIR
                                    fenêtrés ou équiondulation (Parks-McClellan, `src/remez.cpp`), ordre minimal
                                    d'un gabarit (comme buttord, cheb1ord, cheb2ord, ellipord)
  include/qacq/dsp/filtering.h     filtrage en sections, aller-retour sans déphasage (bords prédits ou comme
                                    SciPy), prédiction linéaire, FIR par FFT
  include/qacq/dsp/random.h        aléatoire reproductible (xoshiro256**, même suite sur tout compilateur)
  include/qacq/dsp/generator.h     générateur : formes échantillonnées ou limitées en bande (série de Fourier)
  include/qacq/dsp/modulation.h    AM, FM, PM, ASK, FSK, BPSK, QPSK ; porteuse réelle ou bande de base I/Q ;
                                    AnalogModulator, partagé avec la porteuse modulée du générateur
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
  include/qacq/app/analysis_controller.h  analyse (module Signaux) : calcul sur son thread, mesures mises en forme, export
  include/qacq/app/curves_controller.h    courbes ajoutées : liste (session), courbes du temps sur un thread
  include/qacq/app/generator_controller.h module Générateur : recette, aperçu sur son thread (session à part), ajout
  include/qacq/app/filter_controller.h    module Filtres : recette, conception, aperçu avant/après sur son thread, ajout
  include/qacq/app/audio_player.h  écoute d'un signal sur la carte son (Qt Multimedia, préparée sur un thread)
plot/                        qacq_plot : le moteur de tracé de toutes les vues, sans aucune dépendance Qt
  include/qacq/plot/axis.h          graduations 1-2-5 et log, un préfixe par axe, quantités affichées et saisies
  include/qacq/plot/trace.h         tracé à tout zoom : extrêmes par colonne de pixels, pyramide min/max, ruptures
analysis/                    qacq_analysis : l'analyse sans Qt (spectre d'une zone, mesures, courbes, CSV)
  include/qacq/analysis/analysis.h  spectre d'amplitude ou densité (Welch), réel ou I/Q, niveaux en dB, mesures
  include/qacq/analysis/measures.h  statistiques A-B, mesures d'oscilloscope, pics, bande à −3 dB
  include/qacq/analysis/calculus.h  intégrale, dérivée, valeurs par période, courbe calculée (nouveau signal)
  include/qacq/analysis/curves.h    courbes ajoutées : sortes, noms, unités, liste en JSON, calcul, « Garder »
  include/qacq/analysis/generation.h  recettes du générateur : voies, formes d'onde additionnées, séries de Fourier,
                                    motifs dessinés (points, tracés, série exacte), récurrences, opérations entre
                                    voies, porteuses modulées, ordre des voies, JSON, exemples
  include/qacq/analysis/expression.h  formules tapées (coefficients de Fourier…) : opérateurs, fonctions, variables
  include/qacq/analysis/playback.h    un signal prêt pour la carte son : voies, rééchantillonnage, niveau, format
  include/qacq/analysis/filters.h     module Filtres : recette (RII, RIF fenêtré ou équiondulation, coefficients
                                    tapés ; ordre choisi ou calculé d'après un gabarit), conception, réponses (gain,
                                    phase dépliée, retard, impulsion, indice), pôles et zéros, ce qu'un gabarit
                                    tient, application à un signal (causale, à phase nulle, alignée), JSON, exemples
  include/qacq/analysis/filter_code.h le filtre pour microcontrôleur : flottant ou virgule fixe (Q15, Q31), mise à
                                    l'échelle, arithmétique simulée au bit près, ce que l'arrondi fait, le C
ui/                          QTacquisitionApp : la fenêtre Qt Quick (coquille de QTsignalApp V2)
  signal_plot.h                    le tracé (QQuickPaintedItem) : signal de la session, défilement, captures
  spectrum_plot.h                  le spectre : fréquences linéaires ou log, niveaux en dB ou linéaires, harmoniques
  motif_editor.h                   l'éditeur d'un motif dessiné du générateur : points, tracés à main levée, grille
  response_plot.h                  les réponses d'un filtre : gain, phase, retard, impulsion, indice, plan z
  qml/Main.qml                     fenêtre, barre des modules, ruban, liste des signaux, propriétés
  qml/AcquisitionView.qml          module Acquisition : tracé en direct, ruban, état et chiffres de la liaison
  qml/AcquisitionSettings.qml      réglages de la source, du format, de la cadence, des voies ; préréglages
  qml/SignalsView.qml              module Signaux : le signal choisi, les courbes du temps et les spectres ajoutés
  qml/AnalysisPanel.qml            onglet Mesures : mesures de la zone, pics des spectres, réglages des calculs
  qml/GeneratorView.qml            module Générateur : aperçu du signal et de son spectre, ruban, ajout à la session
  qml/GeneratorPanel.qml           onglet Composition : signal, voies, formes d'onde (WaveCard.qml), préréglages
  qml/FiltersView.qml              module Filtres : réponses du filtre (Concevoir), avant et après (Appliquer)
  qml/FilterPanel.qml              onglet Filtre : la recette en détail, ce qu'il fait, ses coefficients
  qml/FilterCodeDialog.qml         le filtre en C pour microcontrôleur : format, mise à l'échelle, nom, .h et .c
  qml/ViewGroup.qml                groupe « Vue » du ruban, le même dans chaque module
tests/                       qacq_tests (cœurs) et qacq_app_tests (couche Qt) : un groupe CTest par étiquette
  fixtures/scipy_reference.inc     résultats SciPy (générés, ne pas éditer)
  codegen/generate.cpp             écrit le C de 52 filtres que le test codegen compile et dont il lance l'autotest
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

Comparaison à SciPy 1.18 (26 filtres IIR, dont les cinq cas qui faisaient défaut et sept elliptiques) :
réponse en fréquence à moins de 4e-9 dB, sortie de `sosfilt` et de `sosfiltfilt` à moins de 3e-12 en valeur
relative ; `sosfiltfilt` et `filtfilt` avec chacun des `padtype` (odd, even, constant, None) à 1e-14 ; coefficients `firwin` à 1e-13 ; Parks-McClellan (`remez`) : le programme d'origine de McClellan,
Parks et Rabiner (1973), celui que SciPy exécute, mêmes coefficients à 1e-16 sur les 7 cas de référence et
sur 680 cas tirés au hasard, mêmes échecs aux mêmes itérations ; là où SciPy échoue sur un filtre long
(« Failure to converge at iteration 2 » : le départ régulier égalise l'erreur à 1e-16, au niveau de
l'arrondi), l'échange repart des fréquences extrêmes d'un filtre deux fois plus court et converge ; robuste à
des cosinus faux de quelques unités du dernier chiffre (une autre bibliothèque mathématique) ; ordres minimaux
d'un gabarit (`buttord`, `cheb1ord`, `cheb2ord`, `ellipord`) : 18 cas de référence et 12 000 tirés au hasard,
mêmes ordres, mêmes fréquences à 1e-12, sauf pour des coupe-bande : environ un sur quarante a ici un ordre de
moins (la minimisation bornée de SciPy, arrêtée à 1e-5 près sur tan(πf/fs), s'arrête loin du bord à basse
fréquence quand le mieux est le bord lui-même : il est alors laissé où il était ; le gabarit tenu, vérifié sur
chacun), et un sur mille a ses fréquences à 1e-4 près (la même minimisation, arrêtée ailleurs selon le dernier
chiffre de la bibliothèque mathématique) ; Welch à 1e-9.

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

**Import** (CSV, texte, WAV ; bouton Importer, Ctrl+I ou fichiers déposés sur la fenêtre, plusieurs à la
suite) : la lecture tourne sur un fil à part, la fenêtre reste vivante. Au-delà d'un tiers de seconde, une
fenêtre « Import en cours » montre le fichier, l'avancement (octets lus, échantillons convertis pour un
WAV), le temps restant estimé et **Annuler** : la lecture s'arrête au bloc suivant (1 Mo), rien n'est
ajouté, et les fichiers qui attendaient leur tour sont abandonnés aussi. Une nouvelle session ou une session
ouverte pendant un import l'annule.

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
- **Carte son** : entrée choisie par son nom, format vérifié avant de démarrer. Sa fréquence est celle de
  l'enregistrement, quelle que soit la fréquence « annoncée » restée des autres sources (jusqu'au 22/11/2026,
  une fréquence annoncée de 1 MHz pour le simulateur passait avant : 7 s de voix à 48 kHz duraient 337 ms ;
  l'onglet Signal propose alors « Remettre 48 kHz »). Les signaux enregistrés s'appellent « Enregistrement »
  (le nom d'une autre source, « Essai du simulateur », « Acquisition », devient celui de la source choisie ; un
  nom à soi reste).
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
  molette ou molette sur une échelle pour les valeurs, glisser pour se déplacer (temps, valeurs ou les deux),
  clic droit glissé pour zoomer sur un intervalle, double clic pour tout voir ; la lecture suit le pointeur
  (instant et valeur de chaque voie à l'échantillon le plus proche).
- **En direct** : défilement des dernières secondes, ou dernière capture du déclenchement (t = 0 à l'instant du
  déclenchement, niveau marqué), « Figer » pour lire pendant que les données continuent d'arriver.

## Le module Signaux

Un seul module pour regarder, mesurer et analyser un signal de la session (Signaux et Analyse fusionnés le
21/11/2026, selon l'audit). En haut le signal choisi ; dessous, **seulement les courbes qu'on ajoute**
(onglet Courbes ▸ Ajouter…, ou clic droit sur le tracé). Sans courbe, le signal prend toute la hauteur.
- **Ruban** : deux onglets bien visibles (pastilles avec icône, l'onglet ouvert en couleur), et dans les
  deux le groupe Fichier (Importer) et le groupe Écouter (le signal sur la carte son : voir le Générateur).
  Dans une fenêtre trop étroite, les groupes de tous les rubans passent à la ligne (deux étages, ou plus) au
  lieu de défiler : aucun bouton n'est caché. **Onglet Visualiser** : Vue (Tout voir, Zoom ±, Précédente, sur le
  dernier tracé survolé ; le même groupe que dans Acquisition), Voies (Superposées), Mesurer (Curseurs ; Pic
  et Suivant avec un spectre), Image. **Onglet Courbes** : Courbes (Ajouter…, Superposées, Garder, Tout
  retirer, Exporter ▾ : les courbes du temps ou les spectres en CSV), Zone analysée, Spectres (amplitude ou
  densité, dB ou linéaire, fréquences log, harmoniques).
- **Panneau de droite** : onglet Mesures (mesures de chaque voie sur la zone analysée, pics des spectres,
  réglages des calculs) et onglet Signal (nom, échantillonnage, voies, notes).
- **Fréquence d'échantillonnage** (onglet Signal) : c'est l'échelle des temps du signal. La changer ne
  touche pas aux échantillons ; la durée, l'axe du temps, les fréquences mesurées, l'axe des spectres et les
  intégrales la suivent (ce n'est pas un rééchantillonnage). Un changement d'un facteur 10 ou plus est
  d'abord confirmé, avec l'ancienne et la nouvelle durée (1 MHz → 1 Hz : 1,8 s deviendrait 500 h).
- **Peu de doublons** : importer (aussi dans le ruban), exporter le signal, ouvrir, enregistrer sont dans la
  barre du haut (sessions récentes sous « Ouvrir ▾ », chemin de la session en infobulle de son nom) ;
  renommer, exporter, supprimer dans la liste (clic droit, F2, Suppr) ; le bouton « Importer un fichier… » de
  la liste n'apparaît que quand elle est vide, et un fichier déposé sur la fenêtre est importé.
- **Graphe du temps** : la voie elle-même (pour superposer), son intégrale, sa dérivée, sa moyenne, sa valeur
  efficace ou sa fréquence par période ; sur tout le signal ou entre A et B. Il suit le zoom et les curseurs
  du signal. Superposées, deux unités ont chacune leur échelle (gauche et droite) ; au-delà, une bande par
  unité.
- **Graphe des fréquences** : le spectre (amplitude ou densité) des voies choisies, seules calculées.
- **Spectrogrammes** (Ajouter… ▸ Spectrogramme) : le spectre au fil du temps, dans leur propre graphe sous le
  signal, sur le même axe du temps (il suit le zoom et les curseurs, et l'inverse) ; une bande par voie, les
  fréquences en hauteur, le niveau en couleurs (dB de l'unité de la voie, échelle des couleurs à droite de la
  bande : du plus haut niveau à 80 dB en dessous). Sur tout le signal ou entre A et B. Segments de 256 à
  16 384 échantillons, recouvrement de 0 à 87,5 % (onglet Mesures ▸ Réglages des calculs, ou clic droit sur
  les spectrogrammes, avec Δf et la durée d'un segment ; même fenêtre et même moyenne retirée que les
  spectres). Sur un long signal, une colonne moyenne plusieurs segments
  voisins (3 000 colonnes au plus, moins avec de très longs segments : 48 Mo au plus par voie) ; le dernier
  segment finit avec le signal (plus près du précédent si le pas ne tombe pas juste) : aucun échantillon
  n'est laissé de côté, la fin comprise. Le calcul tourne sur son propre thread ; « Calcul du spectrogramme :
  40 % », avec une barre, dit où il en est, et l'ancien résultat reste affiché pendant qu'il recommence
  (autres segments). La lecture sous le pointeur donne t, f et le niveau.
- Les voies du signal, en haut, se superposent aussi (onglet Visualiser, groupe « Voies »).
- Les courbes sont des vues du signal choisi, recalculées quand il change, enregistrées dans la session ;
  « Garder » fait un signal de la session des courbes du temps affichées. Une étiquette par courbe : un clic
  la masque, la croix la retire. Au bout des étiquettes des spectres et des spectrogrammes, un « ? » dit au
  survol comment ils sont calculés (fenêtre, points, Δf, remarques, zone analysée ; segments) : rien ne
  s'affiche en travers des graphes, sauf la progression d'un spectrogramme pendant son calcul et la zone
  analysée sur le signal quand elle n'est pas « Tout ».
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
- **Export** en CSV pour un tableur (virgule décimale, « ; » entre les colonnes, UTF-8 avec sa marque) : les
  spectres (fréquence, niveau en dB et en valeur) et les courbes du temps affichées (le temps, puis une
  colonne par courbe, « nan » là où une courbe n'a pas de valeur) ; aussi par clic droit sur leur graphe.
- **Mesures temporelles** de chaque voie (comme un oscilloscope) : niveaux haut et bas (deux modes de
  l'histogramme séparés par un creux ; sinon, un bruit par exemple, les extrêmes), période et fréquence par
  les fronts (hystérésis de 10 % ; des fronts trop irréguliers — bruit, balayage, salves — ne font pas de
  période, et sans deux niveaux pas de temps de montée non plus), rapport cyclique,
  montée et descente 10-90 % ou 20-80 % (réglage), dépassements, moyenne et efficace sur les périodes entières
  (sans le biais d'une période coupée au bord de la zone), temps d'établissement à ±1 % et ±0,1 % après le
  dernier front (vers un niveau plat seulement : carré, échelon). **Pics du spectre** : les 8 plus hauts, un
  par lobe, et la bande à −3 dB autour du maximum (passe-bas, passe-haut ou bande : coupures, largeur, centre).
- **Calculs des courbes du temps** : intégrale cumulée (trapèzes, moyenne retirée au choix), dérivée
  (différences centrées), moyenne, efficace et fréquence par période (une valeur par période, d'un front
  montant au suivant, vide hors des périodes entières).

## Le module Générateur

- **Une recette, voie par voie** : un signal de 1 à 16 voies, chacune avec son nom, son unité, son décalage
  et jusqu'à 32 formes d'onde qui s'additionnent : sinus, carré, triangle, dent de scie, impulsions (rapport
  cyclique), continu, échelon et Dirac (à l'instant choisi), bruit blanc (gaussien : écart type ; uniforme :
  crête), balayage (de f1 à f2, linéaire ou logarithmique, sur toute la durée). Un sinus et un bruit font un
  sinus bruité, trois sinus un multiton. Une forme d'onde se décoche pour l'écarter sans la perdre, se monte
  ou se descend ; en changer de sorte garde ce que les deux partagent (amplitude, fréquence, phase…).
- **Série de Fourier par ses coefficients** (forme d'onde « Série de Fourier ») : fondamentale f₀, N
  harmoniques (0 : tous ceux sous fs/2), coefficients en formules de n : x(t) = a₀ + Σ aₙ cos(nω₀t) +
  bₙ sin(nω₀t), ou les Cₙ complexes d'un signal réel (Re Cₙ et Im Cₙ, ou |Cₙ| et arg Cₙ en degrés ; C₀ = a₀,
  Cₙ = (aₙ − j bₙ)/2, C₋ₙ = Cₙ*). Changer de forme réécrit les formules pour la même série, en restant
  court : le nombre en tête est multiplié (bₙ = 4*E/(pi*n)*odd(n) → Im Cₙ = -2*E/(pi*n)*odd(n)), un terme de
  signe constant prend une phase constante (|Cₙ| = 2*E/(pi*n)*odd(n), arg Cₙ = -90), et revenir à la forme
  d'avant redonne les formules d'avant. Les formules, longues ou non, se lisent en entier (retour à la
  ligne) ; Entrée les applique, Échap les rend. Les formules
  connaissent n (aussi N ou k), Nmax, f0, w0, T0, fs, pi (π), e, les variables de la composante
  (« E = 1 ; D = 0,25 ») ; + − * / ^, produits implicites (2n, 2pi, pi n, 2(n+1)), comparaisons (1 ou 0), sin, cos,
  tan, asin, acos, atan, atan2, sinh, cosh, tanh, sqrt, abs, exp, ln, log (décimal), log2, floor, ceil, round,
  sign, min, max, mod, pow, sinc, odd, even, sind, cosd, tand, deg, rad et if(condition; alors; sinon) (seule la
  branche choisie est calculée : if(n == 1; 0; 1/(n−1))) ; « ; » entre les arguments, 0,5 ou 0.5 ; « bₙ = … »
  ou « = … » devant une formule sont acceptés. Les erreurs disent quel terme, quoi et où (« bₙ : Nom inconnu
  « D » (caractère 3) », « aₙ n'est pas un nombre fini pour n = 1 »). **Modèles** : carré, triangle, dent de
  scie, impulsions, sinus redressé double et simple alternance (avec le E et le D des variables) ; un carré,
  un triangle, une dent de scie ou des impulsions changés en série de Fourier gardent leur forme (mêmes
  échantillons, à 1e-15 près). Le tableau des 12 premiers coefficients (aₙ, bₙ, Aₙ et φₙ de
  Aₙ sin(nω₀t + φₙ)) suit chaque modification. Harmoniques à fs/2 ou au-delà laissés de côté (avertissement)
  ou repliés au choix ; somme par le schéma de Horner (4·10⁸ harmoniques × échantillons au plus).
- **Motif dessiné** (forme d'onde « Motif dessiné ») : une période dessinée à la souris dans le panneau
  Composition, x de 0 à T, y de −A à +A. Un clic pose un point, un point se glisse entre ses voisins, un
  double-clic ou un clic droit le retire, un tracé à main levée redessine le motif là où il passe (simplifié,
  256 points au plus par tracé ; un trait vertical fait un saut) ; « Grille » (ou Maj) aimante les points au
  1/32 de la période et au 1/8 de la hauteur ; Suppr et les flèches agissent sur le point choisi. Les points
  se joignent par **paliers**, **segments** ou une **courbe lisse** (cubique monotone de Fritsch–Carlson :
  jamais au-delà des points, plate à un sommet) ; deux points au même x font un saut. Le motif se répète à sa
  fréquence (phase comprise), ou s'étale **une fois sur toute la durée**. Ses points se lisent et se tapent
  aussi en texte (« 0 -1 ; 0,1 1 ; 0,5 1 ; 0,6 -1 », 1 024 points au plus). **Modèles** : rampe, escalier,
  trapèze, charge et décharge RC (τ = T/10), cardiogramme ; un carré, des impulsions, un triangle ou une dent
  de scie changés en motif gardent leurs échantillons (un sinus : 16 points, courbe lisse, à 0,6 % près).
  Sans repliement, le motif est sa série de Fourier exacte sous fs/2 (chaque morceau est un polynôme : les
  coefficients viennent des sauts de la valeur et de ses dérivées aux jonctions).
- **Récurrence** (forme d'onde « Récurrence ») : y[n] tapé comme une formule des valeurs d'avant, y[n-1] à
  y[n-16] et x[n] à x[n-16] (aussi « y(n-1) », « y_{n-1} », ou y1, x1…), de n, t (n/fs), fs, Te (1/fs), N et
  de variables (« a = 0,9 ; w = 2*pi*f/fs »), à partir de valeurs initiales (« y[-1] = 1 ; y[-2] = 0 », les
  autres à 0), avec les fonctions des séries de Fourier. **Entrée** x[n] : aucune, Dirac (réponse
  impulsionnelle), échelon (réponse indicielle), bruit blanc gaussien, ou **les formes d'onde au-dessus**, que
  y[n] remplace alors : la récurrence les filtre (celles d'en dessous s'ajoutent encore). Les erreurs se
  disent à la frappe (« y[n] : Nom inconnu « b » », « y[n] ne dépend que des valeurs d'avant »), et une
  valeur qui n'est plus un nombre fini arrête le calcul (« la récurrence diverge », avec n). Les 12 premières
  valeurs (n, x[n], y[n]) suivent chaque calcul. **Modèles** : oscillateur (2 cos(w) y[n-1] − y[n-2] : un
  sinus exact ; un sinus changé en récurrence devient le sien, phase comprise), passe-bas du premier ordre,
  résonateur, moyenne glissante, marche aléatoire, suite logistique (chaos).
- **Opération entre voies** : y tapé comme une formule des voies v1 à v16 (telles qu'elles sont faites,
  décalage compris ; aussi V1…), de x (les formes d'onde au-dessus dans sa voie, qu'elle remplace alors), de
  t, n, fs, Te, N et de variables : « v1 + v2 », « v1*v2 », « v1/v2 », « sqrt(v1^2 + v2^2) »,
  « (1 + 0,5*v2)*v1 », ou toute fonction du temps (« sin(2*pi*50*t)^3 »). Chaque voie est faite après celles
  qu'elle lit, quel que soit leur ordre ; une voie ne peut pas se lire elle-même, même par d'autres
  (« Voies 1 → 2 → 1 »). Une valeur qui n'est pas un nombre (division par zéro) vaut 0, et c'est dit. Une
  nouvelle opération fait la somme des deux premières autres voies ; **Modèles ▾** : somme, différence,
  produit, quotient, moyenne, module, valeur absolue, carré, redressement. Les voies lues sont nommées sous la
  formule (« v1 : « Tension » »). Une voie lue par une autre ne peut pas être retirée (le message dit
  laquelle) ; retirer une autre voie renumérote les formules (v3 devient v2).
- **Porteuse modulée** : A cos(2π fc t + φ) modulée en **amplitude** A(1 + m·message), en **fréquence**
  fc + Δf·message ou en **phase** φ + Δφ·message, par un message tapé comme une opération : un sinus
  (« sin(2*pi*fm*t) », fm en variable), une autre voie (« v1 »), les formes d'onde au-dessus (« x »,
  remplacées), ou toute formule (**Message ▾**). Les formules de modulation sont celles du module Modulations
  (`dsp::AnalogModulator`) : les mêmes échantillons. Une surmodulation (m × crête du message > 1), une
  fréquence instantanée hors de 0…fs/2 ou une porteuse au-delà de fs/2 sont dites.
- **Repliement ou pas** : un carré, un triangle, une dent de scie ou des impulsions sont échantillonnés tels
  quels (leurs harmoniques au-delà de fs/2 se replient, comme sur un CAN sans filtre), ou « sans repliement »
  par leur série de Fourier jusqu'à fs/2, éventuellement limitée à quelques harmoniques (Gibbs bien visible).
  Au-delà d'environ 4·10⁸ harmoniques × échantillons (une seconde de calcul), la série est raccourcie et un
  avertissement le dit.
- **Aperçu** : le signal est calculé sur son propre thread un instant après chaque modification (le calcul en
  cours s'arrête si la recette change), avec une roue et le pourcentage s'il dure ; il s'affiche avec le
  spectre de chaque voie, et ses mesures (statistiques, tonalité, THD…) sont dans l'onglet Mesures du
  panneau de droite. Les avertissements (fréquence au-delà de fs/2, Dirac hors de l'enregistrement) disent
  la voie et la forme d'onde en cause. Cinquante millions d'échantillons au plus, toutes voies comprises
  (400 Mo) ; au-delà, la recette est refusée avant tout calcul. L'aperçu n'est calculé que module ouvert.
- **Ajouter à la session** : l'aperçu devient un signal de la liste (ses échantillons sont partagés, pas
  copiés), en une étape d'annulation ; ses notes disent ce qu'il contient et sa provenance garde la recette.
  Dans le panneau Signal, « Reprendre dans le générateur » remet cette recette dans le module.
- **Écouter** (ruban, groupe Écouter, aussi dans le module Signaux pour le signal choisi) : l'aperçu joué
  sur la carte son, une voie en mono ou les voies 1 et 2 en stéréo (gauche, droite). Il est préparé sur
  son propre thread au format de la sortie : rééchantillonné à sa fréquence (sinc fenêtré de Kaiser, coupé
  à 0,45 de la plus basse des deux fréquences : rien ne se replie quand on descend), **normalisé** (crête à
  −1 dBFS) ou **tel quel** (±1 dans l'unité du signal = pleine échelle, au-delà écrêté et dit), une fois ou
  **en boucle** (le rééchantillonnage raccorde alors la fin au début). **Son ▾** : voies, niveau, boucle,
  volume (changé pendant l'écoute), sortie audio ; ces réglages sont retenus. Pendant l'écoute, chaque
  nouvel aperçu remplace le précédent (en boucle surtout) : ce qu'on change dans la recette s'entend. Une
  seule écoute à la fois dans l'application : tant qu'un son joue, le bouton est « Arrêter » dans les deux
  modules (son info-bulle dit quoi, où et depuis combien de temps). Dix minutes au plus ; ce qui
  dépasse ce que la carte rend (au-delà de 0,45 × sa fréquence) est dit.
- **Exemples** (ruban) : sinus 1 kHz, sinus et bruit, multiton, carré avec et sans repliement, balayage
  20 Hz → 20 kHz, échelon et bruit, Dirac, réseau 50 Hz (tension et courant déphasé avec un harmonique 3),
  série de Fourier d'un carré (15 harmoniques, Gibbs), redressement double alternance, cardiogramme dessiné
  avec le 50 Hz du secteur, carré filtré par une récurrence (passe-bas RC), puissance instantanée v1 × v2
  (P = U·I·cos φ en moyenne), modulations d'amplitude et de fréquence d'un message de 500 Hz (voie 1) sur une
  porteuse de 10 kHz, bruit blanc. La recette en cours est retenue d'une fois sur l'autre ; les préréglages en
  gardent d'autres.
- **Bruit reproductible** : une graine pour tout le signal ; chaque bruit en tire une suite à lui (graine +
  1000 × voie + forme d'onde), la même sur tout compilateur.

## Le module Filtres

Un filtre conçu, regardé, puis essayé sur un signal de la session. Deux onglets au ruban :

- **Concevoir** : le type (**RII**, récursif : Butterworth, Chebyshev I, Chebyshev II, Bessel, elliptique ;
  **RIF**, non récursif, par la méthode de la fenêtre : rectangulaire, Hann, Hamming, Blackman,
  Blackman-Harris, Kaiser de β réglable, ou en **équiondulation** par l'algorithme de Parks-McClellan ; ou des
  **coefficients tapés**, voir plus bas), la
  réponse (passe-bas, passe-haut, passe-bande, coupe-bande), la coupure ou les deux bords de la bande, l'ordre
  (1 à 32 ; une bande en a deux fois plus) ou le nombre de coefficients (3 à 65 535 ; 4 095 en équiondulation),
  l'ondulation (Chebyshev I, elliptique, équiondulation), l'atténuation (Chebyshev II, elliptique,
  équiondulation) et la largeur des transitions (équiondulation). L'**elliptique** (Cauer) ondule dans les deux
  bandes pour le flanc le plus raide à ordre donné : le panneau dit la fréquence d'arrêt calculée, à partir de
  laquelle l'atténuation est tenue. L'**équiondulation** place les bandes à une demi-transition de part et
  d'autre de chaque fréquence, pèse l'erreur de chacune d'après l'ondulation et l'atténuation demandées, et dit
  ce qu'elle atteint (mesuré sur la réponse) et combien de coefficients le gabarit demande (estimation de
  Kaiser) ; un gabarit qu'elle ne tient pas est signalé, avec ce qu'il faudrait. Des coefficients qui
  mèneraient bien au-delà de 170 dB (au-delà de la précision des calculs, où l'échange ne converge plus) :
  le filtre est calculé sur ceux du milieu, les autres valent 0, et c'est dit. **Sortie ▸ Gain** (dans les deux
  onglets) : le signal filtré multiplié par 10^(gain/20), une fois quelle que soit l'application (à phase
  nulle aussi, où le reste du filtre compte deux fois), de −120 à +120 dB ; les réponses le montrent, les
  coefficients restent ceux du filtre à 0 dB, avec g en tête. Il est conçu pour la fréquence d'échantillonnage
  du signal choisi dans la liste (**fs du signal**), ou pour celle qu'on tape. Le RII est réalisé en sections
  du second ordre (la forme qui reste stable en pratique) ; un pôle à moins de 1e-5 du cercle unité est
  signalé. **Afficher** : le gain (dB ou linéaire), la phase (dépliée à partir des racines de chaque section,
  la même à tout zoom ; un saut de 180° là où un zéro est sur le cercle ; repliée sur demande), le temps de
  propagation de groupe (en échantillons), les réponses impulsionnelle et indicielle, les pôles (×) et les
  zéros (○) dans le plan z avec le cercle unité (ceux d'un RIF de plus de 512 coefficients ne sont pas
  calculés ; 0,3 s pour 511, chaque racine laissée dès qu'elle est trouvée). Le filtre tel qu'il sera appliqué s'y ajoute en pointillé quand il diffère (à phase nulle : le
  gain au carré, ni phase ni retard ; RIF aligné : sans son retard). Sur le tracé, la molette zoome, un
  glissement déplace, un glissement du bouton droit zoome sur ce qu'il couvre ; les valeurs sous le pointeur
  s'affichent en haut à droite (dans le plan z : le pôle ou le zéro le plus proche, son module, sa
  fréquence). Les fréquences suivent le filtre, ici et dans les spectres d'Appliquer : logarithmiques quand son
  bord le plus haut passe sous 3 % de fs/2 (20 Hz à 10 kHz, une bande autour de 50 Hz), linéaires quand il
  repasse au-dessus ; entre deux, le bouton **Log** décide. **Exemples** : passe-bas Butterworth 1 kHz,
  coupe-bande 50 Hz du secteur et passe-haut 0,5 Hz contre la dérive (tous deux à phase nulle), bande
  téléphone 300 Hz à 3,4 kHz (Chebyshev I), elliptique 1 kHz d'ordre 5, Bessel 1 kHz, RIF passe-bas de
  Hamming, RIF équiondulation 2 kHz, RIF passe-bande de Kaiser ; et, en coefficients tapés, moyenne glissante
  sur 8 points, lissage exponentiel, encoche à 50 Hz (biquad), dérivée.
- **Gabarit** (Concevoir ▸ Réglage) : l'ordre (un RIF : le nombre de coefficients) n'est plus choisi mais
  calculé, le plus petit qui tient un gabarit : jusqu'où le filtre passe avec au plus **Amax** d'atténuation
  (un RIF : d'ondulation, d'un extrême à l'autre), d'où il coupe avec au moins **Amin** (fp et fa ; fa₁, fp₁,
  fp₂, fa₂ pour une bande, dans leur ordre le long des fréquences). RII : comme `buttord`, `cheb1ord`,
  `cheb2ord` et `ellipord` de SciPy (mêmes ordres, mêmes fréquences ; un coupe-bande rapproche d'abord ses bords
  de passage de sa bande coupée quand cela fait baisser l'ordre, par la même minimisation bornée que SciPy et
  MATLAB, un bord laissé où il était quand cette recherche coûterait un ordre) : −Amax tombe juste sur les bords
  de la bande passante, l'excédent va à la bande coupée ; Bessel, qui
  n'a pas de formule, par sa réponse résolue (jusqu'à l'ordre 12). RIF : par une fenêtre, les coupures au milieu
  des transitions (Kaiser : β tiré de l'écart demandé, comme `kaiserord`), ou en équiondulation sur les bandes
  du gabarit (poids tirés de Amax et Amin) ; le plus petit nombre qui le tient est cherché à partir de
  l'estimation de Kaiser, puis retenu (un long RIF en équiondulation demande quelques conceptions la première
  fois : une demi-seconde à 2 000 coefficients). Le gabarit est tracé sur le gain (où le gain ne doit pas aller,
  ombré) ; le panneau dit ce qui est tenu (mesuré), l'ordre réel que demande le gabarit, et ce qu'il faudrait
  avec les autres familles et en RIF (« Butterworth d'ordre 8, Chebyshev d'ordre 5, elliptique d'ordre 4… »). Un
  ordre au-delà de ce qui se conçoit (32, Bessel 12), une fenêtre qui ne descend pas assez (Hamming : guère plus
  de 60 dB), plus de coefficients que le maximum : refusés, avec ce qui le tiendrait. Passer au gabarit part du
  filtre tel qu'il est : le gabarit qu'il tient, lu sur son gain (où il sort de la bande passante, où il entre
  dans la bande coupée, arrondi à 3 chiffres vers un gabarit plus facile ; un RIF par fenêtre : ses transitions
  rendues symétriques autour de ses coupures), d'où un ordre au plus égal ; ou le gabarit qu'il avait, si le
  filtre le tient encore. Revenir à l'ordre choisi part du filtre calculé (son ordre, ses fréquences, ses
  coefficients). Exemples : passe-bas elliptique par
  gabarit (1 kHz, 60 dB dès 1,5 kHz : ordre 6), RIF passe-bande en équiondulation (1 kHz à 3 kHz).
- **Coefficients tapés** (type **b, a**) : H(z) = Σ b[k] z⁻ᵏ / Σ a[k] z⁻ᵏ, soit y[n] = b[0] x[n] + b[1] x[n − 1]
  + … − a[1] y[n − 1] − …, comme `lfilter(b, a)` de SciPy ; un RIF si a = 1. Dans le panneau Filtre : b et a
  en listes de nombres ou de formules séparés par « ; » (ou « , » suivie d'une espace, ou un par ligne ; une
  liste de SciPy, de NumPy, `array([…])` compris, ou de MATLAB, `[1 -2 1]`, se colle telle quelle ; « 0,5 »
  est un nombre, « 1,2,3 » trois), ou une seule formule de k avec le nombre de coefficients (b = 1/N, N fois :
  une moyenne glissante) ; des variables (« N = 8 ; w0 = 2 pi 50/fs ») ; les formules connaissent k, fs,
  Te = 1/fs, pi, e et les fonctions du générateur. Ce que donne chaque liste s'affiche sous elle. Tout est
  divisé par a[0] (nul : refusé) ; les zéros en tête de b sont un retard. Un RII tapé est réalisé en sections
  du second ordre à partir de ses racines (un biquad tel quel) : les racines en z = ±1 du numérateur, souvent
  multiples (le (1 + z⁻¹)^N d'un Butterworth), sont mises à part exactement, les autres affinées en long
  double ; un Butterworth d'ordre 6 tapé depuis SciPy redonne le filtre à 1e-12, un d'ordre 8 à 1 kHz sur
  48 kHz à 7e-8. Des pôles que b et a placent mal (une coupure très basse, un ordre élevé : chaque chiffre
  compte) sont signalés. Un pôle hors du cercle unité : instable, signalé, ses réponses montrées (la réponse
  impulsionnelle tant qu'elle grandit d'un facteur un million), mais il n'est pas appliqué ; sur le cercle (un
  intégrateur, un oscillateur) : appliqué en causal seulement. Au plus 257 b et 257 a pour un RII (ordre
  256), 65 535 b pour un RIF.
- **Appliquer** : le signal choisi dans la liste, une de ses voies, **avant et après** (superposés, ou une
  bande chacun) et leurs spectres ; l'aperçu porte sur les 4 194 304 premiers échantillons et se calcule sur
  son propre thread, le plus récent l'emportant. **Application** : **causale** (échantillon après échantillon,
  ce que ferait un microcontrôleur : avec le retard et la phase du filtre), **à phase nulle** (aller puis
  retour, comme `filtfilt` de SciPy : ni retard ni déphasage, l'atténuation doublée) ou **alignée** (un RIF
  une fois, son retard de (N − 1)/2 retiré). À phase nulle, **Bords** : comment l'enregistrement est prolongé
  au-delà de ses bouts pour que le filtre y démarre comme si le signal continuait. **Prédits** (par défaut) :
  par prédiction linéaire d'ordre 96 sur les 4 096 échantillons de chaque bord, en moindres carrés (avant et
  arrière pour un signal régulier, avant seulement pour un signal qui s'éteint ou qui grandit), aussi loin que
  le filtre sonne (sa réponse retombée à 1e-6) ; une somme de sinusoïdes continue telle quelle, ce qui s'éteint
  continue de s'éteindre jusqu'à zéro, un enregistrement au repos reste au repos. **Symétrie impaire**
  (2·x[0] − x[k], le choix de SciPy et de MATLAB : garde une tendance, mais un signal qui commence sur une
  crête y fait une bouffée), **miroir**, **constants** ou **non prolongés** (`padtype` odd, even, constant et
  None de SciPy : mêmes résultats à 1e-14). Mesuré sur des enregistrements de quelques secondes comparés au
  même signal filtré au milieu d'un enregistrement bien plus long (écart au pire près des bords, rapporté au
  signal filtré) : l'AM d'une porteuse qui commence sur une crête, 1e-6 prédit contre 4 en symétrie impaire ;
  trois sinus à travers un passe-bande étroit, 0,002 contre 1 ; une dérive lente sous un passe-haut à 0,5 Hz,
  de 0,05 à 0,4 selon fs contre 0,4 à 0,7 ; un carré, du bruit : jamais plus mal que la symétrie impaire. Un
  filtre qui sonne sur plus d'un dixième de l'enregistrement est signalé, avec sa durée même quand elle dépasse
  l'enregistrement : à moins de cette durée des bords, le résultat dépend de ce que faisait le signal avant et
  après, que l'enregistrement ne dit pas ; et quand les bords sont prolongés bien moins longtemps (comme SciPy,
  ou pas du tout), c'est la mise en route du filtre qu'on voit. Ainsi 40 Hz de bande à 10,5 kHz (ordre 10) sur
  20 ms d'AM : le filtre sonne 234 ms ; prédits, seule la bande latérale sort, 0,25 V partout ; prolongés de
  63 échantillons comme SciPy, ou pas du tout, le filtre n'a pas le temps de démarrer (0,2 µV au plus, comme
  `sosfiltfilt`).
  **Écouter** l'aperçu, avant ou après. Un signal échantillonné autrement que le filtre est refusé
  (le filtre ne ferait pas ce que montrent ses réponses) ; un signal à trous est filtré d'un seul tenant, et
  c'est dit.
- **Code C** (Concevoir ▸ Microcontrôleur) : le filtre tel qu'une carte l'exécute, en causal, échantillon
  après échantillon, en C99 que prend tout compilateur pour microcontrôleur (déclarations en tête de bloc,
  pas de débordement d'un `int` de 16 bits) : un `nom.h` et un `nom.c` (`nom_init`, `nom_echantillon`,
  `nom_bloc`, `nom_autotest`), à copier ou à enregistrer dans un dossier. **Flottant** `float` ou `double` :
  sections en forme directe II transposée, RIF sur une ligne à retard doublée (les N derniers échantillons
  toujours à la suite), coefficients symétriques repliés (moitié moins de multiplications). **Virgule fixe**
  Q15 (échantillons et coefficients `int16_t`, accumulateur `int32_t`, ou `int64_t` quand 32 bits coûteraient
  de la précision, comme CMSIS-DSP) ou Q31 (`int32_t`, accumulateur `int64_t`) : sections en forme directe I,
  chacune dans son format Qd choisi pour que l'accumulateur ne puisse pas déborder quels que soient les
  échantillons, sortie arrondie et saturée ; b2 = b0 gardé à l'arrondi (un coupe-bande garde ses zéros sur le
  cercle) ; mise à l'échelle **somme** (par défaut : la somme des |h| de l'entrée à chaque section au plus
  0,99, rien ne peut déborder, aucune oscillation de débordement) ou **crête** (le gain au plus 0,99 à toute
  fréquence : un sinus établi pleine échelle ne déborde pas, quelques dB de bruit en moins, mais un
  transitoire peut déborder, et c'est dit) ; le gain que la dernière section ne peut prendre sans déborder va
  dans un étage de sortie. Ce que l'arrondi fait est dit : réponse des coefficients arrondis (près du maximum,
  au pire, plancher de la bande coupée), pôle le plus proche du cercle, bruit de calcul (rapport signal sur
  bruit sur un bruit blanc contre le calcul en double), saturations d'un sinus pleine échelle, gain crête ; ce
  qui ne suffit pas est signalé (« prenez Q31 ou du flottant »), ce qui serait instable une fois arrondi est
  refusé. `nom_autotest()` passe 32 échantillons et compare aux sorties calculées ici par la même
  arithmétique, puis une impulsion suivie de zéros aussi longtemps que le filtre répond, dont il compare la
  somme des (n + 1) y[n] (tous les coefficients y comptent) : au bit près en virgule fixe, à l'arrondi près en
  flottant (une multiplication-addition fusionnée, un autre ordre des calculs : 1e-4 près pour un RII en float).
  Vérifié par le test `codegen` : 52 filtres dans les quatre formats et les deux mises à l'échelle, compilés
  en C99 par le compilateur C du build avec ses avertissements stricts, leurs autotests sans écart (et ici
  aussi en C89, avec Clang `-Weverything`, en C++ et avec FMA).
- **Ajouter** (dans les deux onglets) : le signal choisi, toutes ses voies filtrées, devient un signal de la
  session (« Signal · passe-bas 1 kHz », « AM · passe-bande 10 kHz à 12 kHz, +6 dB »), en une étape
  d'annulation ; ses notes disent le filtre et ce qu'il fait, sa provenance garde la recette.

Le panneau de droite (**Filtre**) reprend la recette champ par champ (gain en sortie compris), dit ce que fait
le filtre (sections, stabilité, retards à quelques fréquences, ce qu'il atteint) et montre ses coefficients, un par ligne (« b0 = … » à « a2 = … »
pour chaque section d'un RII, a0 = 1 ; « h[k] = … » pour un RIF, les 600 premiers ; des coefficients tapés :
b[k] et a[k] divisés par a[0], puis les sections) ; **Copier les
coefficients** les copie tous, en pleine précision, avec ce qu'est le filtre en tête. **Mesures** donne celles
de l'aperçu, avant et après. La recette est retenue d'une fois sur l'autre.

## Outils des tracés

- **Curseurs A et B** (Signaux, Acquisition figée ou déclenchée) : on les glisse, ou Ctrl + clic
  place A et Maj + clic place B. Dans Signaux, ce sont les mêmes sur le signal, les courbes du temps et les
  spectrogrammes : la croix d'un relevé les retire partout, comme le bouton Curseurs, et la zone « Entre A
  et B » revient alors à « Tout ». Un liseré sombre borde leurs lignes, visibles même sur un tracé dense de
  leur couleur ; leurs étiquettes restent visibles avec une bande par voie. Sur le temps : Δt, 1/Δt, valeur
  de chaque voie sous A et sous B, et entre A et B min, max, moyenne, efficace, crête à crête et intégrale
  (V·s), avec assez de chiffres pour voir leur écart (une fréquence de 440 Hz ± 4 mHz se lit
  « 440,0021 Hz » ; de même dans l'onglet Mesures). Sur le spectre : Δf, niveaux et écart en dB, valeur
  efficace de la bande A-B (somme de la densité sur la bande).
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
- **Spectrogrammes** : les mêmes gestes pour le temps et les fréquences (molette sur l'échelle des
  fréquences ou Ctrl + molette, glisser, ↑ ↓) ; la molette ou un glissement vertical sur l'échelle des
  couleurs change l'étendue des niveaux montrés (10 à 200 dB), un double clic la remet à 80 dB.
- **Bornes d'une échelle** : un clic droit sur une échelle (valeurs d'une bande ou d'un côté, niveaux et
  fréquences d'un spectre, fréquences et couleurs d'un spectrogramme) ou sur l'axe du temps ouvre son menu,
  dans tous les tracés (signal, courbes du temps, spectrogrammes, spectres, acquisition) : minimum et
  maximum tapés avec leur unité ou un préfixe (2 ms, 1,5 kHz, −20 dBV), appliqués à la validation ;
  Automatique (sur le temps : Tout le signal ; sur les fréquences : Toutes les fréquences) ; pour les
  spectres, fréquences logarithmiques et niveaux en décibels. Le menu suit les gestes faits pendant qu'il
  est ouvert. En acquisition qui défile, l'axe du temps se règle avec le champ Fenêtre. Un spectrogramme
  garde des couleurs réglées à la main par bande (la molette sur son échelle change alors son minimum).
- **Clic droit dans le tracé** (sans glisser ; glisser zoome toujours) : le menu du tracé, avec une case par
  voie ou par courbe à afficher, « Superposées », Tout afficher, Échelle verticale automatique, Tout voir, et
  pour les courbes : Ajouter, Garder, Retirer les courbes masquées. Superposées, deux unités ont chacune leur
  échelle (dans la couleur de leur voie quand elle est seule de son côté). Le bouton « Superposées » du
  ruban fait de même (une bande par voie quand il est éteint). Pour ce qui est calculé, le menu a en plus
  une colonne « Calcul » avec les mêmes réglages que l'onglet Mesures, appliqués aussitôt : spectres (zone
  analysée, amplitude ou densité, fenêtre et β de Kaiser, zéros ajoutés ou longueur et recouvrement des
  segments, moyenne retirée, harmoniques mesurées) ; spectrogrammes (longueur et recouvrement des segments
  avec Δf et durée, fenêtre et moyenne retirée, communes avec les spectres).
- **Marqueur de pic** (spectres) : sur le plus haut pic, puis le suivant (clic droit : le précédent).
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
4. **Modules** (en cours) : analyse (spectre, densité, mesures, courbes ajoutées), fusion de Signaux et
   Analyse en un module (audit du 21/11), export des courbes, garde-fou de la fréquence d'échantillonnage,
   spectrogramme en courbe ajoutée, générateur (voies, formes d'onde additionnées, aperçu, séries de Fourier
   par leurs coefficients, motif dessiné, récurrence, écoute sur la carte son, opérations entre voies,
   porteuses AM, FM, PM), filtres (RII Butterworth, Chebyshev I et II, Bessel, elliptique ; RIF fenêtré ou en
   équiondulation de Parks-McClellan ; coefficients tapés ; gain en sortie ; réponses, plan z, application
   causale, à phase nulle aux bords prédits ou alignée ; code C pour microcontrôleur, flottant ou virgule fixe ;
   conception par gabarit, l'ordre calculé) : faits ; puis modulations numériques, démodulation et taux d'erreur (module Modulations, sur
   `dsp/modulation` et `dsp/receiver`) ; VISA pour les instruments en USB. À étudier : réponse en fréquence
   entre deux voies (gain, phase, marges), distorsion pondérée A ; opérations sur un signal de la session
   (×k, +c, entre voies) dans Signaux.
5. Ensuite : rééchantillonnage, corrélation, transformée en Z, OFDM et codage, théorie, outils.
