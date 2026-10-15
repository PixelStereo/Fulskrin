# Audit du moteur vidéo, du rendu et des timings — 15 octobre 2026

## Version et conclusion

Version auditée : `main` **35e73e837769cfba70d2ccfcd5ae884d430b35e9**, après fusion des PR #173–178. Machine locale : Mac Studio Apple M3 Ultra, macOS 26.7.1, Qt 6.11.2, FFmpeg 9. Les demandes d’interface ultérieures sont publiées séparément dans la [PR #179](https://github.com/PixelStereo/Pinol/pull/179).

Le moteur tient les cadences demandées dans les charges courtes mesurées. En revanche, l’audit reproduit des défauts d’arrêt d’urgence, de commandes en attente, de continuité temporelle et de transparence vidéo. **Il ne faut pas interpréter les anciens tests verts comme une validation complète du comportement en exploitation.** Les corrections de timing ci-dessous ne sont pas incluses dans la PR d’interface.

Les résultats distinguent des défauts reproduits, des comportements dont le contrat doit être clarifié, des limites connues du pipeline et des vérifications matérielles encore nécessaires.

## Périmètre et méthode

Lecture des chemins de décodage/démultiplexage/livraison/seek, files de frames et budgets, conversion vidéo et surfaces matérielles, rendu et présentation, audio et resynchronisation, séquenceur et son thread, timelines/oscillateurs, snapshots/presets/délais/transitions/typewriter, interactions, timecode et instrumentation. Les sources centrales représentent plus de 9 000 lignes, complétées par les tests et les sorties.

La suite existante contient 32 zones moteur/interface/CLI. Une première passe locale en réussit 31 ; la suite `video` échoue. Pour éliminer les médias manquants, les fichiers exacts de CI ont été récupérés depuis l’artifact `test-media` 11957609719 du run 38706314842. La réexécution vidéo conserve **six assertions en échec** : une attente de format trop restrictive, une vraie différence d’alpha et quatre assertions de budget incompatibles avec le chemin zero copy. Ce résultat n’est pas déclaré vert.

Les régressions supplémentaires utilisent les API réelles du moteur. [Le pilote manuel](../test/manual/video_timing_audit.cpp) fait volontairement apparaître les comportements observés ; il ne transforme pas ces défauts en assertions acceptées dans la CI. Chaque scénario de rendu recrée son moteur, afin de respecter la fin de vie de ses ressources.

## Mesures courtes du moteur

| Charge | Durée mesurée | Rendu observé | Échéances manquées selon le compteur | Intervalle p99 / maximum | Calcul p99 / maximum | Attente du verrou UI p99 |
|---|---:|---:|---:|---:|---:|---:|
| H.264 + ProRes + HAP + H.264/AAC, composition 1080p, 60 Hz | 12 s | 720 frames, 60 Hz | 0 | 17,67 / 17,67 ms | 4,00 / 4,38 ms | 1,66 ms |
| Même charge, 120 Hz | 12 s | 1 440 frames, 120 Hz | 0 | 9,31 / 9,31 ms | 4,00 / 4,94 ms | 1,37 ms |
| Deux H.264 4K à 25 images/s, composition 4K, 60 Hz (essai isolé) | 20 s | 1 200 frames, 60 Hz | 0 | 17,67 / 17,67 ms | 4,00 / 5,22 ms | 1,36 ms |

Les fichiers gardent leur cadence propre de 25 ou 30 images/s. Les percentiles sont des bornes issues d’histogrammes, pas des quantiles calculés sur tous les échantillons bruts. Le compteur « missed » ne compte qu’un intervalle dépassant **1,5 période** : zéro à ce compteur ne signifie pas que chaque image est arrivée exactement à sa deadline.

Ces mesures portent sur le thread de rendu, sans fenêtre de sortie physique présentée. Les métriques de décodage du benchmark incluent son préchauffage, alors que ses métriques de rendu sont remises à zéro après celui-ci. Les quelques retards et décodes longs du démarrage ne doivent donc pas être attribués automatiquement à une perte en régime établi.

## Défauts reproduits prioritaires

### A1 — P1 : un GO déjà en file peut redémarrer après Escape

`goToSequence()` poste un callback Qt depuis un autre thread. `stopAll()` efface les runs mais n’invalide pas ces callbacks. Scénario : envoyer GO depuis un thread, appeler `stopShowPlayback()` avant de traiter la file UI, puis traiter les événements. Résultat : **timeline Playing, position de séquence 0**, alors que le show vient d’être arrêté.

Source : [Sequencer.cpp, mise en file et arrêt](https://github.com/PixelStereo/Pinol/blob/35e73e837769cfba70d2ccfcd5ae884d430b35e9/src/engine/Sequencer.cpp#L284).

Correction recommandée : génération d’annulation des transports, vérifiée au moment d’exécuter chaque commande. Un nouveau GO envoyé volontairement après l’arrêt doit rester possible. Tests : commande avant/après Escape, STOP de séquence, changement de projet et suppression de séquence.

### A2 — P1 : Escape n’annule pas les autres actions d’un lot déjà préparé

`advance()` prépare une liste `fire`, libère le verrou puis exécute toutes ses actions. Avec deux séquences atteignant leur pre-wait au même tick, si le rappel de la première déclenche l’arrêt, **la seconde est néanmoins rappelée** : deux rappels observés, un seul attendu.

Source : [Sequencer.cpp, exécution hors verrou](https://github.com/PixelStereo/Pinol/blob/35e73e837769cfba70d2ccfcd5ae884d430b35e9/src/engine/Sequencer.cpp#L503).

Correction recommandée : chaque action doit porter l’identité de son run et sa génération ; revalider avant l’application, sans conserver un verrou lourd pendant le chargement des sources.

### A3 — P1 : l’Auto-follow peut dépasser une timeline qui vient seulement de démarrer

Scénario déterministe : pre-wait 300 ms, timeline 200 ms, post-wait 100 ms ; un appel `advance(1.0)` arrive tard. Le séquenceur atteint **le step suivant alors que la première timeline est encore Playing à clock 0**. Il utilise le dépassement du pre-wait comme si l’action avait réellement joué pendant ce temps.

Le report d’un dépassement est correct pour des waits purs ; il n’est pas équivalent à l’exécution rétroactive d’un transport. Un verrou occupé ou un rappel lourd peut produire un tick tardif dans le logiciel réel.

Source : [Sequencer.cpp, calcul de continuation avant application des actions](https://github.com/PixelStereo/Pinol/blob/35e73e837769cfba70d2ccfcd5ae884d430b35e9/src/engine/Sequencer.cpp#L516).

Correction recommandée : Auto-follow suit la fin effective de l’action lancée, avec distinction entre instant prévu et instant appliqué. Tester retard court/long, plusieurs steps, snapshot et timeline, à 60/120 Hz.

### A4 — P1 : les animations ISF continuent après Escape

Le stop arrête les timelines, médias et fondus ; il ne fige pas `TIME` des générateurs et effets ISF. Avec un shader affichant `TIME / 20` dans son canal rouge, la valeur passe de **0,003922 à 0,035294** dans les 600 ms suivant Escape, soit environ **627 ms d’animation supplémentaire** après quantification de la capture.

Sources : [arrêt des transports](https://github.com/PixelStereo/Pinol/blob/35e73e837769cfba70d2ccfcd5ae884d430b35e9/src/engine/Snapshots.cpp#L1025), [horloge ISF](https://github.com/PixelStereo/Pinol/blob/35e73e837769cfba70d2ccfcd5ae884d430b35e9/src/engine/Isf.cpp#L825).

Correction recommandée : définir le gel/reprise des horloges ISF et des buffers persistants. Mettre seulement la vitesse à zéro ne garantit pas le gel d’un shader à feedback ou dépendant de FRAMEINDEX. Vérifier aussi les générateurs dépendant d’une entrée externe.

### A5 — P1 : perte réelle de transparence ProRes 4444 avec VideoToolbox

Sur cette machine, le fichier de référence `fmt_yuva444p10.mov` produit `ayuv64le` avec décodage matériel et `yuva444p12le` en logiciel. Les chemins matériel **et matériel avec copie forcée** donnent un alpha opaque là où le logiciel conserve la transparence : pixel central logiciel **(202, 255, 82, 139)** contre matériel **(201, 255, 83, 255)**.

Différences absolues matériel vs logiciel : moyennes RGBA **0,852 / 0,301 / 1,358 / 115,807** ; maxima **4 / 2 / 5 / 215**. Les couleurs sont proches ; la perte d’alpha est déterminante. Il ne s’agit donc pas seulement du nom d’un format inattendu dans un test. Désactiver uniquement le zero copy ne corrige pas ce cas.

Sources : [sélection du décodage matériel](https://github.com/PixelStereo/Pinol/blob/35e73e837769cfba70d2ccfcd5ae884d430b35e9/src/engine/VideoDecoder.cpp), [description des plans](https://github.com/PixelStereo/Pinol/blob/35e73e837769cfba70d2ccfcd5ae884d430b35e9/src/engine/VideoDecoderLayout.cpp).

Correction recommandée : garantir la conservation d’alpha ou choisir le décodage logiciel pour les profils/combinaisons où elle ne l’est pas. Ajouter un test matériel-vers-logiciel sur la transparence, pas seulement sur RGB. Le composant responsable exact — décodeur matériel, transfert ou description de format — nécessite une isolation supplémentaire.

## Autres anomalies de timing

### A6 — P2 : deux GO en attente peuvent viser deux fois le même step

Deux appels GO depuis un autre thread calculent tous deux `nextOfLocked()` avant leur exécution. Après traitement de la file : **position 0, deux runs**, au lieu de position 1. Calculer le prochain step dans la commande effectivement exécutée, tout en conservant l’identité de sa séquence d’origine.

Source : [goSequence et goToSequence](https://github.com/PixelStereo/Pinol/blob/35e73e837769cfba70d2ccfcd5ae884d430b35e9/src/engine/Sequencer.cpp#L337).

### A7 — P2 : saut de phase d’oscillateur lors d’un changement de mode de boucle

Durée d’une passe 1 s, période sinus 0,7 s, temps joué 1,2 s. Passage Loop → PingPong : valeur **0,110029 → 0,889971**, saut immédiat **0,779942**. `setLoop()` remappe le clock utilisé également comme phase d’oscillateur.

La continuité des Repeat sans changement de mode est bien couverte et passe. Le changement du mode pendant la lecture doit avoir son propre test. Séparer le temps accumulé de modulation du clock servant aux passes, au seek et à la direction.

Source : [Animations.cpp](https://github.com/PixelStereo/Pinol/blob/35e73e837769cfba70d2ccfcd5ae884d430b35e9/src/engine/Animations.cpp#L150).

### A8 — P2 : shader et timeline divergent après un blocage modéré du rendu

Blocage injecté de 650 ms : la timeline avance de **0,7023 s**, le shader d’environ **0,3137 s**. `nextDt()` limite le dt des shaders à 250 ms, tandis que les timelines et médias utilisent `m_realDt`, limité à 2 s. Une divergence de phase demeure après la reprise.

Sources : [nextDt et contexte ISF](https://github.com/PixelStereo/Pinol/blob/35e73e837769cfba70d2ccfcd5ae884d430b35e9/src/engine/Render.cpp#L868), [IsfInstance::render](https://github.com/PixelStereo/Pinol/blob/35e73e837769cfba70d2ccfcd5ae884d430b35e9/src/engine/Isf.cpp#L825).

Correction recommandée : temps absolu partagé pour les playheads et le TIME ; sous-pas bornés pour les simulations/filtres qui en ont besoin. Un simple dt non borné peut déstabiliser des shaders à feedback.

### A9 — P2 : au-delà de 2 s de blocage, le show perd du temps réel

Blocage injecté de 3,1 s : temps réel **3,1350 s**, timeline **2,0177 s**, soit environ **1,1173 s perdue**. Le cue clock et le timecode ne suivent pas cette même limite. L’audio extrapole également son transport entre deux frames, ce qui peut nécessiter une resynchronisation à la reprise ; l’effet audible n’a pas été mesuré en boucle matérielle.

Correction recommandée : politique explicite de rattrapage, gel ou resynchronisation de tout le show. Garder un plafond de dt local ne suffit pas à préserver une synchronisation entre horloges.

### A10 — Contrat à clarifier : pause globale et fondus de composition

À vitesse globale 0, le fondu de layer reste à **0**, mais un fondu de composition déclenché par snapshot atteint environ **0,23686** pendant l’essai de 650 ms. Le chemin de composition reçoit du temps réel afin que le blackout réponde pendant une pause ; cette exception entraîne aussi les fondus d’opacité/volume de snapshots.

Source : [stepShowRecalls](https://github.com/PixelStereo/Pinol/blob/35e73e837769cfba70d2ccfcd5ae884d430b35e9/src/engine/Snapshots.cpp#L1113).

Il faut distinguer le blackout manuel de sécurité des automations du show, ou documenter et afficher clairement cette exception. Ce comportement est reproduit ; son statut fonctionnel dépend du contrat de pause retenu.

## Presets et fiabilité des outils

### A11 — P2 : des modifications de preset étaient ignorées par l’interface

`samePresets()` comparait les valeurs, temps et easing mais pas les **délais, animations et interactions**. Un changement portant uniquement sur ces champs pouvait être éliminé comme une absence de modification. La nouvelle PR d’interface #179 corrige ce comparateur et ajoute des tests de modification de délai seul, capture d’interaction seule et annulation.

La même PR renforce l’undo des presets : annuler un rappel différé supprime son exécution future et restaure les géométries/définitions concernées. Il ne faut pas déduire de cette correction UI que A1–A10 sont corrigés.

### A12 — P2 : la télémétrie peut masquer une corrélation ou un partage sauté

Le logger CSV lit `shares_skipped` alors que `frameTimingJson()` publie `gpu_shares_skipped`. La colonne peut donc rester à zéro malgré des partages sautés. L’analyseur recherche l’événement `snapshot` alors que le moteur journalise `snapshot_recall`.

Preuve : un événement snapshot_recall et une frame tardive 50 ms après donnent **un événement tardif, zéro événement près d’une cue/timeline** dans l’analyseur.

Sources : [EnduranceLogger.h](https://github.com/PixelStereo/Pinol/blob/35e73e837769cfba70d2ccfcd5ae884d430b35e9/src/EnduranceLogger.h#L134), [analyseur](https://github.com/PixelStereo/Pinol/blob/35e73e837769cfba70d2ccfcd5ae884d430b35e9/scripts/analyze_timeline_endurance.py).

### A13 — P2 : la suite vidéo contient des attentes dépendantes du chemin matériel

Le test H.264 exige `yuv420p` alors que `nv12` matériel donne ici une image conforme. Les tests de budget exigent des octets comptés et des mises en attente, alors que les surfaces zero copy sont explicitement hors de ce budget. Ces attentes doivent être testées sur un décodeur logiciel forcé, avec une validation distincte des surfaces matérielles.

**Conserver le test d’alpha en échec** tant que A5 n’est pas corrigé. Élargir les attentes ne doit pas supprimer une vraie régression d’image.

Source : [video_test.cpp](https://github.com/PixelStereo/Pinol/blob/35e73e837769cfba70d2ccfcd5ae884d430b35e9/test/video_test.cpp#L440).

### A14 — Limite de mesure : le benchmark s’adapte à la cadence de rendu obtenue

Le seuil de réussite utilise `min(cadence fichier, cadence de rendu obtenue)`. Une cadence de rendu inférieure à celle demandée réduit ainsi l’objectif de chaque vidéo. Le message final et le code de retour ne constituent pas à eux seuls un critère d’acceptation de la cadence demandée. Comparer séparément cadence demandée/obtenue, deadline misses et PTS effectivement montrés.

Source : [bench.cpp](https://github.com/PixelStereo/Pinol/blob/35e73e837769cfba70d2ccfcd5ae884d430b35e9/test/bench.cpp#L194).

## Ce que la vérification confirme et ses limites

- Les suites existantes passent pour Repeat/Auto-follow standard à 60/120 Hz, délais individuels, snapshots, interactions, undo/structure, rendu, GPU targets, OSC, publication et contrôles UI ciblés. Cela couvre les scénarios écrits, pas tous les entrelacements entre threads.
- Le décodeur borne ses files, traite les seeks par génération, protège les boucles sans frame et possède des reprises d’erreurs. La lecture inverse utilise des fenêtres de décodage ; elle peut coûter davantage avec un GOP long. Le budget de frames est un plafond souple, avec minimum par flux, et ne représente pas toute la RAM/GPU.
- L’audio extrapole le transport, corrige une petite dérive et resynchronise les grands écarts avec un court fondu. Les tests synthétiques ne prouvent pas la latence sortie/retour d’une carte réelle.
- Le rendu et les copies multivues utilisent des échanges GPU. Les essais sans receiver ne prouvent pas l’absence de tearing, de perte ou de délai dans Syphon/Spout/NDI/DeckLink.
- Le pipeline couleur reste principalement SDR. Les limites déjà documentées demeurent : images fixes converties en RGBA8 à l’import, certains fallbacks CPU en 8 bits, clipping HDR et absence de gestion colorimétrique complète. Voir [l’audit couleur existant](COLOR_PIPELINE_AUDIT.md). RGBA16 interne ne garantit pas une chaîne de sortie physique 10 bits.
- Les modes de timecode interne/externe et leurs tests existent ; aucune entrée/sortie LTC/MTC/ATC réelle n’a été connectée pour cet audit.

## Ordre de correction recommandé

1. Invalidation des commandes en attente et lots d’actions après STOP/Escape ; puis Auto-follow sur fin effective de transport.
2. Préservation de l’alpha matériel, avec choix de repli fiable.
3. Politique de gel/reprise ISF, puis unification des horloges et continuité des oscillateurs lors des changements de mode.
4. Correction des compteurs/analyseurs et des attentes de test dépendantes du matériel.
5. Validation sous charge représentative, plusieurs sorties et médias 4K/8K, avec durée de plusieurs heures, suivi mémoire et appareils réels.

Une revue de l’implémentation et des tests a été effectuée par l’agent auteur. Aucune approbation indépendante de mainteneur ni validation d’endurance physique n’est prétendue.

## Reproduire les sondes manuelles

Depuis une copie de cette version, ajouter temporairement à la configuration CMake :

```cmake
add_executable(pinol_timing_audit test/manual/video_timing_audit.cpp)
target_link_libraries(pinol_timing_audit PRIVATE pinol_engine Qt6::Widgets)
```

Construire ce target puis exécuter le binaire avec un contexte graphique valide. Il imprime des lignes `PROBE` et injecte volontairement des pauses de 650 ms et 3,1 s dans le rendu. Il ne se lance jamais dans l’application utilisateur ni dans la CI normale. Sur macOS, les services graphiques doivent être accessibles. Les captures de TIME sont quantifiées en 8 bits et donnent une estimation de phase ; le clock de timeline est lu directement.
