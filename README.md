# QTacquisitionApp 2 — refonte

Outil pédagogique et utilitaire de développement : acquisition (carte à microcontrôleur en série,
instrument SCPI, flux réseau, carte son) et traitement du signal.
Ce dépôt est la réécriture de QtAcquisitionApp (« Architecture milestone 57 »), menée selon l'audit du 20/11/2026.

**Statut : phase 0 (socle) en cours.** Le cœur de calcul et ses tests sont en place ; l'application Qt Quick
arrive en phase 3. L'ancienne application reste utilisable d'ici là.

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
tests/                       qacq_tests : un exécutable, un groupe CTest par étiquette
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
Préréglages `ci` (avertissements traités comme des erreurs) et `sanitize` (AddressSanitizer + UBSan).

Pour régénérer les références : `python tools/reference/make_scipy_reference.py` (numpy et scipy).

## Ce que corrige le cœur, par rapport à l'ancienne application

| Défaut de l'audit | Avant | Maintenant (test) |
| --- | --- | --- |
| 1. THD sur un long enregistrement (1 001,37 Hz, 10 % H3, 5 % H5, 48 kHz) | 1,13 % lu sur 10 s, fondamentale 0,40 au lieu de 1 sur 20,8 s | 11,180340 % sur 4,2 s, 10 s et 20,8 s ; f0 à 1e-11 près (`[audit]`) |
| 2. IIR en forme directe b/a | 37 passe-bas faux ou divergents sur 160, passe-bande d'ordre 6 faux jusqu'à 1 kHz | sections du second ordre : les 196 cas justes ; ordre jusqu'à 32 (Bessel 12) |
| 3. Portions du signal ignorées | spectre limité aux 65 536 derniers points ; I/Q tronqué à la puissance de 2 inférieure | toujours tout l'enregistrement, toute longueur (FFT de Bluestein) |
| 4. Plafond de 80 M opérations | FIR 127 coefficients refusé sur 21 s à 48 kHz | 60 s à 48 kHz filtrées en 0,3 s (FFT par recouvrement-addition) |
| Duplication | 4 FFT, 18 définitions de pi, 3 chemins FIR | une FFT, une constante, un moteur de filtrage |

Comparaison à SciPy 1.18 (19 filtres IIR, dont les cinq cas qui faisaient défaut) : réponse en fréquence à
moins de 4e-9 dB, sortie de `sosfilt` et de `sosfiltfilt` à moins de 3e-12 en valeur relative ;
coefficients `firwin` à 1e-13 ; Welch à 1e-9.

## Méthode de l'analyse de tonalité

1. f0 approchée : pic d'un spectre de Hann (2^20 points au plus), interpolation gaussienne.
2. Affinage : ajustement aux moindres carrés de la fondamentale et de ses harmoniques sur les deux moitiés
   d'une portion croissante de l'enregistrement ; la dérive de phase entre les moitiés donne l'erreur de fréquence.
3. Amplitudes : un seul ajustement de la composante continue et de toutes les harmoniques sur tout
   l'enregistrement (matrice normale en forme fermée, coût O(N K)).

Limite connue : une composante qui n'est pas une harmonique (50 Hz du secteur, par exemple) n'est pas dans le
modèle ; elle fuit très peu dans l'ajustement (quelques 1e-8 en valeur relative dans l'essai à 50 Hz + 3,3 kHz).

## Feuille de route

0. **Socle** : dépôt, CMake, CI, cœur sans Qt et banc de références. Fait : FFT, fenêtres, spectres,
   tonalité, filtres. Reste : générateur (dont formes limitées en bande), modulations et canal (BER),
   rééchantillonnage, décodeurs d'acquisition (ASCII linéaire, trames binaires avec resynchronisation).
1. Modèle et session : signal typé, dépôt avec annuler/rétablir, fichier de session, réglages mémorisés.
2. Acquisition sur un thread dédié : sources série, TCP, UDP, SCPI, carte son ; tampon sans verrou ;
   enregistrement continu ; liste des ports ; déclenchement.
3. Coquille Qt Quick reprise de QTsignalApp V2 et moteur de tracé unique.
4. Modules : oscilloscope, analyse, générateur, filtres, modulations.
5. Ensuite : rééchantillonnage, corrélation, transformée en Z, OFDM et codage, théorie, outils.
