# Pinol — audit d’architecture et de débogage, 16 octobre 2026

## Périmètre et niveau de preuve

Révision examinée : `7308b2744c66b693e144bc8f52fd4f1f7de86706` (main).
Le correctif CI #188 est indépendant des changements de production de cette PR.
Lecture ciblée des flux critiques : Engine/Render, paramètres/interactions,
AudioOutput/AudioStream/AudioAnalysis, décodage vidéo/HAP, publication,
chargement/sauvegarde et séquenceur. Ce document n’est pas une certification
exhaustive de toutes les fonctionnalités ou de tous les pilotes matériels.

Aucun incident de spectacle, média de reproduction ou profil GPU réel n’a été
fourni. Les échecs GitHub Actions sont observés ; les autres constats indiquent
explicitement s’ils sont reproduits ou seulement établis par lecture de code.
Un risque architectural n’est pas présenté comme la cause prouvée d’un crash.

## Architecture et flux de bout en bout

| Sous-système | Responsabilités et échanges | Sources principales |
|---|---|---|
| Application / UI Qt | Démarrage, édition, undo, prévisualisations et fenêtres de sortie ; commandes vers le moteur | `src/main.cpp`, `src/ui`, `Engine.cpp` |
| Modèle de spectacle | Layers, viewports, paramètres, animations, snapshots, presets et interactions ; verrou récursif partagé | `Engine.h`, `Layer.h`, `Layers.cpp`, `Animations.cpp`, `Snapshots.cpp` |
| Horloge de cues | Thread Qt dédié, échéances et rappels, métriques de retard ; partage le verrou du moteur | `Sequencer.cpp` |
| Rendu | Thread propriétaire du contexte GL, tâches GL, horloges, sources, passes, composition et publication | `Engine::renderLoop`, `Engine::frame`, `Render.cpp` |
| Vidéo | Un thread de décodage par instance ; démux FFmpeg, codec ou HAP, seeks, file horodatée et recyclage | `VideoDecoder.cpp`, `VideoDecoderDemux.cpp`, `VideoDecoderSeek.cpp`, `VideoDecoderDelivery.cpp`, `VideoDecoderHap.cpp` |
| Upload et formats | Plans YUV/RGB ou blocs compressés ; PBO et textures ; surface matérielle directe quand disponible | `VideoFrame.h`, `VideoTexture.cpp`, `VideoSurfaceMac.cpp` |
| Audio | Thread de décodage par stream, ring buffer vers callback miniaudio ; transport extrapolé par horloge monotone | `AudioStream.cpp`, `AudioOutput.cpp` |
| Analyse audio | Enveloppes et FFT stéréo, publication atomique ; lecture par les interactions et le widget de spectre | `AudioAnalysis.h`, `InteractionsEngine.cpp`, `AudioSpectrum.cpp` |
| Sorties | Viewports, sorties locales, Syphon/Spout et readback CPU pour NDI/OMT | `Render.cpp`, `Publishing.cpp`, `Publish.cpp` |
| Persistance | Prévalidation JSON, reconstruction des sources, chemins/relink ; sauvegarde via QSaveFile | `Project.cpp`, `ProjectCollect.cpp` |
| Instrumentation | Histogrammes de durée, deadlines, latence OSC/cues et métriques de décodage | `FrameTiming.*`, `VideoDecoderMetrics.cpp` |

### Parcours d’une frame vidéo

1. L’UI ou le chargement de projet appelle les opérations de source du moteur.
2. Le décodeur ouvre le média et produit des frames portant un temps de timeline.
3. Le chemin normal conserve les plans natifs ; HAP conserve les blocs compressés
   quand le GPU accepte leur format. Snappy/démux restent du travail CPU.
4. Le rendu récupère la frame adaptée au temps demandé, puis la transmet à
   `VideoTexture`. Sur macOS, les surfaces compatibles peuvent être utilisées
   sans copie ; ce n’est pas garanti pour tous les formats ni toutes les plateformes.
5. `frame()` traite les rappels/animations, puis les interactions, actualise les
   sources, rend les passes et compose les sorties.
6. Fences et buffers de sortie permettent le partage avec les previews et sorties.
   Les sorties réseau CPU passent par readback et thread d’envoi.

### Parcours audio vers image

1. `AudioStream::run()` décode et rééchantillonne vers une file producteur/consommateur.
2. `AudioOutput::render()` mélange les streams dans le callback miniaudio.
3. `AudioAnalysis::process()` analyse ce mélange avant le volume maître et le blackout.
4. Le callback applique ensuite gain/fade et limite la sortie.
5. `Engine::stepInteractions()` lit le spectre une fois pour les interactions Band,
   sélectionne les bandes, applique plage/filtre/courbe/master/takeover et écrit les paramètres.
6. La frame composée reflète ces paramètres. Les interactions passent après les animations.

Conséquence à préserver : baisser le volume maître ne coupe pas l’analyse du mix
pré-fader. Changer cela modifierait la conduite du spectacle.

## Échecs CI : causes retracées

Run initial : https://github.com/PixelStereo/Pinol/actions/runs/38809817891
Correctif : https://github.com/PixelStereo/Pinol/pull/188

### Windows : exports Unicode (observé)

Les générateurs d’exemples utilisent `ensure_ascii=False` et `Path.write_text`
sans encodage. Les noms d’interaction contiennent `→`, absent de cp1252.
Les logs montrent `UnicodeEncodeError` pour les deux shows ; les fixtures
échouent, donc leurs rendus dépendants ne sont pas exécutés.

Correction #188 : UTF-8 explicite pour projets, cuebooks et métadonnées FM.
Vérification locale : les quatre tests Python passent en simulant cp1252 comme
encodage implicite de `write_text`.

### Audio : test asynchrone mal synchronisé (observé + chemin confirmé)

`engine_test.cpp` appelle `e.start()` avant l’aire `sound`. Le rendu est donc
asynchrone. `advanceFades()` ne traite que les rappels de show et
`renderFrame()` retourne immédiatement lorsque `m_threaded` est vrai.
L’assertion immédiatement après `setInteraction()` peut lire l’ancienne valeur.
Le premier seuil `opacity > .28` accepte même l’opacité initiale de 1 : faux positif.

Le premier commit de #188 remplaçait `advanceFades` par `renderFrame`, ce qui
était insuffisant. Les logs du run 38849295585 le démontrent. Le commit
`371801d3009247544dc284f75b0c2bc2e85c785f` corrige ce point :
- initialiser l’opacité à .05, hors des deux plages attendues ;
- attendre le résultat observable du thread de rendu avec le helper existant
  borné à deux secondes ;
- vérifier les plages [.28,.8] et [.2,.22], avec les inégalités strictes du test ;
- contrôler le succès de `setInteraction`.

Les tolérances spectrales ne sont pas relâchées et aucun changement du moteur
n’est nécessaire pour ce défaut de test. La validation multiplateforme du
nouveau commit appartient à la CI, pas aux tests locaux autonomes.

### macOS : timeout du rendu logiciel (observé, validation du correctif en CI)

`champ_render` est interrompu à 30.04 s ; le journal annonce Apple Software
Renderer. `matieres_render` termine à 28.31 s. Les deux rendent trois frames
procédurales pleine résolution. #188 porte la borne à 120 s, en conservant
`--strict`, les dimensions et le nombre de frames. Cela adapte un smoke test ;
ce n’est ni une preuve de performance GPU ni une preuve qu’aucun autre blocage
n’existe. Le succès du nouveau run reste nécessaire.

## Corrections de production de cette PR

### Parcours spectral inutile — mesuré et corrigé

Avant : `Spectrum::band()` parcourt systématiquement les 2 048 bins non-DC,
même pour une bande de quelques dizaines de Hz. Avec I interactions et B bins,
le coût est O(I × B) par frame, sous le verrou du moteur.

Après : calcul d’un intervalle conservateur borné, puis même accumulation dans
le même ordre et même pondération aux bords. Une marge d’un bin protège les
frontières arrondies. Les indices sont clampés avant conversion en entier,
même pour des fréquences finies extrêmes. Les taux non positifs sont rejetés.
Le coût devient proportionnel au nombre de bins effectivement concernés,
avec une petite marge constante. Pas d’allocation, cache mutable ou nouvelle
synchronisation. Une bande couvrant tout le spectre reste O(B).

Tests différentiels : 12 000 intervalles pseudo-aléatoires, 36 864 cas aux bords
FFT et cas extrêmes, sur six taux de 1 à 192 000 Hz. Les résultats sont égaux
exactement au parcours original sur les mêmes spectres de puissance finis.

Microbenchmark autonome, g++ -O2, environnement Linux de travail : 10 000
itérations de 15 bandes (cinq bandes répétées sur trois layers), trois essais :

| Essai | Parcours complet | Parcours borné | Rapport |
|---|---:|---:|---:|
| 1 | 614.594 ms | 292.502 ms | 2.10× |
| 2 | 586.636 ms | 278.563 ms | 2.11× |
| 3 | 612.114 ms | 271.513 ms | 2.25× |

Ce gain concerne uniquement les appels de sélection de bande. Aucun gain de
FPS, de latence globale ou de débit de décodage n’est revendiqué.

### Calibration Nyquist — reproduite et corrigée

Un signal stéréo alternant +0.1 et -0.1 a un RMS de 0.1. La normalisation du
moteur doit donner `1-exp(-4*0.1)`, soit 0.329679966. Avant correction,
une bande couvrant le signal donne 0.403334022.

Cause : `publishSpectrum()` traite le dernier bin comme les bins intérieurs.
Ceux-ci ont une fréquence négative distincte ; le bin Nyquist est son propre
conjugué. La combinaison des deux canaux doublait sa contribution.
Correction : facteur 0.5 uniquement sur ce bin dans chaque canal. Les autres
bins, la fenêtre Hann, l’analyse stéréo et les enveloppes restent identiques.
Tests : 32 et 48 kHz, canaux en phase et opposition de phase.

Portée : la sortie actuelle travaille à 48 kHz et l’UI limite normalement les
bandes à 20 kHz, donc cette erreur ne justifie pas les échecs CI audio observés.
Elle concerne notamment l’API spectrale sur une bande atteignant Nyquist et les
taux plus bas. Ce changement corrige une valeur erronée, pas une fonctionnalité.

## Zones critiques restantes : preuves, risques et stratégie

| Priorité | Constat établi dans le code | Risque / limite | Refactoring proposé et garde-fou |
|---|---|---|---|
| P1 | `Render.cpp::frame` tient `m_mutex` de la mise à jour jusqu’aux previews ; rendu et préparation de sorties inclus | Les contrôles et cues partageant le mutex peuvent attendre une frame lente ; durée réelle de contention non mesurée ici | Mesurer attente/possession du verrou, puis séparer état éditable et snapshot de rendu avec propriétaires de ressources explicites. Tester ordre animation → interaction et suppression de layer pendant publication |
| P1 | `VideoDecoder.cpp::softwareThreads` laisse les anciens décodeurs inchangés et accorde encore au moins deux threads aux nouveaux ; comptage après ouverture | Le budget annoncé ne borne pas strictement les threads. Avec budget 8 et allocation initiale 4, la suite peut être 4+4+2. S’ajoutent threads par décodeur, audio, pool HAP et codecs matériels | Renommer/documenter la politique souple, mesurer tout le processus ; pour une vraie limite, réserver atomiquement et définir une admission/scheduling des nouvelles sources. Ne pas imposer une nouvelle limite sans politique de lecture explicite |
| P1 | Le budget de frames exclut upload/GPU ; `kMinQueue=2` demeure même au-delà du seuil, staging par stream | La mémoire totale n’est pas bornée à 512 MiB ; N sources conservent au moins leurs buffers minimaux | Comptabilité RAM, PBO et GPU séparée ; stress 1/8/32 flux avec seeks et marche arrière. Une limite dure exige une politique d’admission, pas seulement un compteur |
| P1 | `VideoDecoderDelivery.cpp::deliver` utilise le résultat d’`av_frame_alloc()` sans test de null | Chemin non protégé en cas d’échec d’allocation ; aucun crash OOM reproduit dans cet audit | Injection d’échec d’allocation, propagation d’erreur et conservation/recyclage correct des buffers ; auditer aussi allocations à l’ouverture avant de déclarer le pipeline robuste à OOM |
| P1 | `VideoDecoderSeek.cpp::doSeek` et `AudioStream::doSeek` ignorent l’échec du second `av_seek_frame` puis réinitialisent le codec | Un fichier non seekable ou endommagé peut laisser le transport et le démux en désaccord ; scénario réel non reproduit ici | Résultat de seek explicite, métrique et politique commune de récupération. Tests avec IO contrôlée, échec des deux seeks, seek pendant EOF et changement de direction |
| P2 | `AudioAnalysis::spectrum` essaie trois lectures puis retourne le dernier résultat sans garantie que sa génération soit validée | Lecture potentiellement partielle ou zéro lors d’une collision ; atomique ne veut pas dire snapshot cohérent. Pas de data race démontrée | Publication de snapshots avec propriété des buffers, ou résultat valide/stale explicite. Conserver callback sans attente ; tester plusieurs lecteurs, reset et changement de taux |
| P2 | FFT exécutée dans le callback même sans consommateur spectral ; 4 096 points et hop 1 024 | Coût fixe sur l’échéance audio ; à 48 kHz première fenêtre complète à ~85.3 ms, publication ensuite ~21.3 ms. Ce n’est pas la latence totale mesurée | Instrumenter le pire temps callback. Étudier FFT sur worker avec file bornée ; garder les enveloppes rapides et mesurer le retard ajouté avant migration |
| P2 | `AudioOutput::publish` attend en yield la fin d’un callback actif | Le callback ne bloque pas sur ce verrou mais l’éditeur de la liste peut attendre, sans borne temporelle formelle | Réclamation différée des anciens mixes hors callback ; valider add/remove/stop concurrents et ownership avant remplacement |
| P2 | `findParameter`/`indexOfId` recherchent linéairement ; `Layer::parameters` recalcule une signature incluant inputs/FX | Multiplication du coût par interactions, tracks, layers et paramètres | Index id→layer et path→param avec invalidation par génération ; tests suppression, undo, reload ISF et remplacement de source. Ne pas cacher des pointeurs sans gestion de durée de vie |
| P2 | `feedInteraction` borne la file à 8 192 puis ignore les nouveaux événements ; `stepInteractions` parcourt les événements pour chaque interaction | Travail O(interactions × événements) au pic ; perte possible du dernier état de contrôleur sous flood | Compteurs de pertes et index de routage ; coalescer seulement les contrôleurs continus, conserver l’ordre des triggers/press-release. Tests flood et relâchement de touche |
| P2 | `Publishing::readback` peut attendre 2 ms par sortie, et partage différé jusqu’à 4 ms ; partage immédiat appelle glFinish | Des bornes par sortie ne forment pas un budget global de frame ; pas de mesure GPU matérielle ici | Deadline commune pour readbacks, politique de saut observable et validation des récepteurs. Conserver les modes de partage et leur sémantique |
| P2 | Fichiers VideoDecoder séparés mais même objet privé et dépendances communes ; Engine distribué sur de nombreux fichiers | Découpage physique déjà utile, isolation et tests unitaires encore limités | Extraire progressivement DemuxSession, SeekState et FrameDelivery avec résultats typés ; tests différentiels sur codecs/frames et génération de seek |

Les priorités sont des priorités d’investigation et de réduction de risque,
pas des affirmations que chacun de ces points a provoqué un incident.

## Duplication et maintenabilité

- `VideoDecoder::doSeek` et `AudioStream::doSeek` répètent conversion de temps,
  seek de secours et flush. Le reset du resampler est propre à l’audio : ne pas
  fusionner aveuglément les deux méthodes. Partager d’abord un résultat de seek
  et la conversion de timestamps, garder les resets spécifiques.
- Les deux `examples/*/build_show.py` répètent l’export JSON et la structure de
  show. Le défaut Unicode a touché les deux. Une petite fonction d’export UTF-8
  commune serait appropriée ; conserver les tests d’ownership propres aux shows.
- `LayerParams`, paramètres de composition et paramètres d’interactions répètent
  les bindings et la validation. Une description typée commune peut réduire les
  divergences ; les plages UI, limites absolues et règles de sérialisation doivent
  rester distinctes.
- Ne pas confondre partage utile et couplage : les callbacks audio et le rendu GL
  ont des contraintes de blocage et de durée de vie différentes.

## Cas limites et critères de non-régression

1. Modèle manuel versus rendu démarré : aucune attente de rendu manuel en mode threadé.
2. Mapping absent : une valeur initiale hors plage doit faire échouer le test.
3. Silence, bandes inversées, NaN/inf de fréquence, intervalles hors spectre,
   fréquence extrême finie, taux nul/négatif, limites fractionnaires de bins.
4. Canaux opposés : sommer les puissances, ne pas annuler les canaux avant FFT.
5. Nyquist : compter une seule fois son énergie ; ne pas modifier les bins intérieurs.
6. Retard de callback, seek et changement de rate : ne pas promettre un snapshot cohérent
   simplement parce que chaque champ est atomique.
7. Charge multi-stream : mesurer threads totaux et mémoire totale, pas seulement FFmpeg
   et les buffers comptés dans le budget de frames.
8. Sauvegarde/chargement : garder QSaveFile et le preflight avant clearProject ; préserver
   les médias manquants comme warnings. La reconstruction après preflight n’est pas un
   chargement complètement transactionnel des ressources.
9. Arrêt sur IO lente : `close()` joint le thread de décodage ; le chemin de lecture
   doit pouvoir revenir. Aucun timeout général d’IO n’a été établi dans le flux inspecté.
10. Rendu logiciel CI : un timeout ne prouve pas un shader invalide ; garder --strict et
    tester la performance séparément sur matériel représentatif.

## Validation et limites

Exécuté localement :

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Isrc/engine test/audio_analysis.cpp -o /tmp/audio-analysis
/tmp/audio-analysis
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc/engine test/audio_analysis.cpp -o /tmp/audio-analysis-sanitized
ASAN_OPTIONS=detect_leaks=0 /tmp/audio-analysis-sanitized
g++ -std=c++17 -O2 -Wall -Wextra -Isrc/engine test/manual/audio_band_bench.cpp -o /tmp/audio-band-bench
/tmp/audio-band-bench
git diff --check
```

Les nouveaux tests exécutés contre le header de main échouent sur les quatre cas
Nyquist et le taux négatif (cinq échecs). Avec le correctif, les tests audio
autonomes passent normalement et sous ASan/UBSan. LeakSanitizer
ne fonctionne pas dans cet environnement (/proc inaccessible), donc sa vérification
est explicitement désactivée ; aucune validation des fuites n’est revendiquée.
Les dépendances Qt/FFmpeg de compilation ne sont pas disponibles localement.
La compilation native complète, TSan et les trois plateformes doivent être validés
par GitHub Actions. Aucun test Apple Silicon physique, NDI/Spout/Syphon externe,
charge vidéo lourde ou endurance prolongée n’a été effectué ici.

## Séquence de refactoring conseillée

1. Terminer la validation de #188 ; conserver les checks qui détectent les pannes.
2. Livrer les corrections spectrales isolées et leurs tests ; pas de changement de
   formats de projet ou d’API publique dans cette PR.
3. Ajouter mesures contention/callback/événements perdus ; choisir ensuite les budgets.
4. Introduire index de paramètres et buffers réutilisés avec tests d’invalidation.
5. Extraire démux/seek/livraison par contrats de résultat, avec injection d’erreurs.
6. Préparer snapshots de rendu et réclamation audio différée dans des PR séparées,
   assorties de tests concurrence, undo, destruction et ordre des événements.

Une réécriture globale du moteur sans ces garde-fous ne permettrait pas d’affirmer
la préservation de toutes les fonctionnalités. Les changements livrés ici sont
volontairement localisés et vérifiables.
