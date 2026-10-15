# Matières instables — Calculated Rupture

Une pièce audiovisuelle de 1 min 56 s. La matière commence en suspension, se charge, se fend, devient courant puis se consume. Les références visuelles guident le vocabulaire : poussière blanche sur fond noir, membranes cyan/violet, ramifications cuivre et vert pâle, grands courants bleus. Le son reste la cause du mouvement.

Le projet demande la version de PINOL contenant l’analyse audio et les interactions internes `audio/level`, `audio/bass`, `audio/high` et `audio/onset`.

## Jouer

1. Ouvrir `Matieres_instables.pinol`. Choisir la sortie audio et son niveau d’écoute.
2. Sélectionner la séquence « Calculated Rupture · Matières instables » et déclencher GO une seule fois. Les mouvements sont armés puis la bande et les cues partent automatiquement.
3. Suivre les 18 timelines dans le rack LIVE du séquenceur. Double-cliquer une ligne ouvre son éditeur. ACTIVE conserve aussi les timelines en pause ; ALL affiche les timelines arrêtées.
4. À la fin, l’image et le son s’éteignent, puis les mouvements s’arrêtent. Pour rejouer, déclencher le premier step depuis son numéro.

Le STOP des séquences arrête leurs attentes et leurs enchaînements ; il ne coupe pas la bande ou une timeline déjà lancée. Utiliser le blackout si une extinction immédiate de la sortie est nécessaire.

## Partition

| Temps | Cue | Image et geste |
|---|---|---|
| 0:00–0:16 | Poussière conductrice | Un volume presque blanc émerge du noir. Les attaques détachent des grains, les aigus révèlent des points fins. Échelle intime, peu de couleur. |
| 0:16–0:32 | La masse prend corps | L’énergie grave mesurée augmente nettement : le nuage se comprime, s’épaissit et laisse apparaître une structure bleue. Les mouvements lents continuent entre les événements. |
| 0:32–0:48 | Fissures et membranes | Ouverture cyan/violet autour du changement de brillance. Les voiles s’écartent et les filaments traversent les vides. Le retour de masse vers 0:36 reste porté par les graves. |
| 0:48–1:04 | Friction blanche | Davantage de strates et de lignes fines. Les aigus modifient leur netteté ; les impacts poussent la matière sans faire clignoter tout l’écran. |
| 1:04–1:20 | Courants profonds | La scène s’élargit. Les veines bleues et les couches fluides deviennent dominantes, avec une respiration de densité plus lente que les attaques. |
| 1:20–1:25.8 | Orage intérieur | Les ramifications cuivre prennent le relais. Le noyau gagne de la pression ; la périphérie continue à dériver. |
| 1:25.8–1:28 | Rupture / noyau blanc | Plus forte attaque spectrale repérée dans le morceau. Une condensation blanche traverse le réseau ; ses filaments restent visibles dans la lumière. |
| 1:28–1:38.3 | Retombée magnétique | Repli violet, grains qui subsistent, masses encore chargées par le bas du spectre. Donner une sensation de mémoire et de traction. |
| 1:38.3–1:42.8 | Seconde faille | Nouvelle montée d’attaques : le bleu revient sous le cuivre, les courants et les membranes se chevauchent. |
| 1:42.8–1:48 | Décharge finale | La matière atteint sa plus forte superposition. Le noyau reste détaillé grâce à une compression douce des hautes lumières. |
| 1:48–1:56.088 | Cendres / extinction | Le niveau mesuré baisse fortement. Les trois matières et la bande s’effacent ensemble ; on laisse le dernier vide se former. |

Les entrées à 16, 32, 48, 64, 80, 85.8, 88, 98.3, 102.8 et 108 secondes viennent de l’analyse du niveau et des variations spectrales du fichier fourni. Ce sont des choix de mise en scène basés sur ces repères, pas une prétendue transcription de chaque événement sonore.

## Réactions audio

- Niveau RMS → énergie de la matière, avec une attaque courte et une détente plus lente.
- Graves → amplitude des torsions et pression du noyau.
- Aigus → finesse et visibilité des fibres et des points.
- Attaques → poussées brèves et condensation lumineuse, suivies d’une détente.

L’analyse porte sur le mix des fichiers réellement lus par PINOL, avant le fader général. Modifier le niveau d’écoute ne change donc pas les images. Le silence relâche les enveloppes. Les deux canaux sont mesurés séparément pour ne pas perdre un son en opposition de phase. Il s’agit de bandes d’énergie par filtres, pas d’une FFT affichée comme telle.

Les 18 timelines donnent six mouvements indépendants à chacune des trois matières : densité, turbulence, dérive, échelle, torsion et phase. Les cues possèdent les opacités, la teinte et la progression dramatique ; l’audio possède quatre entrées distinctes. Ils ne se disputent pas les mêmes paramètres.

## Régénérer / version à une timeline

```sh
python3 build_show.py --audio "/chemin/Calculated_Rupture.mp3" --out "/chemin/nouveau-show"
python3 build_show.py --audio "/chemin/Calculated_Rupture.mp3" --out "/chemin/show-1-timeline" --timelines 1
```

Le script nécessite Python 3 et `ffprobe`. La seconde variante regroupe les 18 pistes dans une seule timeline. Il copie le son dans le dossier de destination. Dans le dépôt source, le shader est pris dans `isf/generators`; dans le pack distribué, il est déjà dans `shaders/`.

La composition préparée est en 1280 × 720 à 60 fps, avec trois générateurs procéduraux et aucun média vidéo lourd. Passer en 1920 × 1080 après validation sur la machine de diffusion. Le son fourni reste dans le pack local ; il n’est pas publié dans le dépôt GitHub.
