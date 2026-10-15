# Champ de rupture — FM / 3 minutes

Un set audiovisuel original : 180 secondes, une bande FM stéréo composée pour la pièce, trois matières procédurales, 18 timelines et 15 interactions Audio. Poussières dans le noir, peaux lumineuses, faisceaux qui se fragmentent. Une dramaturgie d'apparition, de pression et de disparition, inspirée des environnements audiovisuels d'Alex Guevara, notamment [Transient](https://www.alex-guevara.com/transient) et [Phantoms of Existence](https://www.alex-guevara.com/poe).

La musique et le shader sont créés pour ce projet. Aucun enregistrement, sample ou visuel de l'artiste n'est repris.

## Jouer

Avec la version de PINOL contenant l'onglet **Audio** :

1. Ouvrir `Champ_de_rupture.pinol`. Conserver les dossiers `audio` et `shaders` à côté.
2. Choisir la sortie audio et régler le niveau d'écoute.
3. Dans le séquenceur, sélectionner **Champ de rupture · FM / 3 minutes**, puis déclencher **GO une seule fois**. Les 18 mouvements sont armés, puis la bande et les cues s'enchaînent.
4. Dans Interactions → Audio, sélectionner une des routes pour voir sa bande du spectre et son mapping.
5. Pour rejouer, déclencher le premier step depuis son numéro. L'extinction finale ramène image et son au silence.

STOP arrête les enchaînements ; les sources déjà lancées continuent. Le blackout reste la commande de coupure immédiate.

## Partition

| Temps | Cue | Matière sonore et visuelle |
|---|---|---|
| 0:00 | Émergence / chambre noire | Grains de verre FM isolés. Un nuage à peine éclairé émerge. |
| 0:12 | Objets suspendus | Premier corps métallique ; la poussière prend du volume. |
| 0:28 | La poussière se souvient | Objets plus rapprochés, champs lentement désaccordés, mémoire bleue. |
| 0:44 | Peaux de résonance | Frictions et suraigus déploient les nappes d'interférences. |
| 1:00 | Première chambre / vide | Montée FM, impact grave puis très bref creux. Le champ devient faisceau. |
| 1:16 | Compression | Résonances graves et densité croissante ; le nuage se replie. |
| 1:31 | Grêle métallique | Grains irréguliers serrés ; les hautes fréquences révèlent les fibres. |
| 1:44 | Fracture / silence | Deuxième montée, frappe inharmonique et vide, trajectoires coupées. |
| 1:56 | Mémoire oblique | Résidus violets, désaccord lent, retour des nappes. |
| 2:08 | Masse critique | Densité maximale, peaux et poussières se superposent. |
| 2:24 | Le champ cède | Troisième fracture : forte superposition lumineuse, puis détente. |
| 2:40 | Débris lents | Les résonances s'éloignent ; le grain et la dérive subsistent. |
| 2:52–3:00 | Retour à la chambre noire | Extinction progressive de toutes les matières et de la bande. |

## Le son

Synthèse FM avec rapports inharmoniques, modulation imbriquée et indices décroissants, corps métalliques à partiels non entiers, excitations de friction et champs FM désaccordés. La distribution des événements est irrégulière et reproductible (graine fixe). Diffusion stéréo à retards croisés. Aucun service de génération ni bibliothèque de samples n'est nécessaire.

Le master local est produit en WAV 48 kHz / 16 bits, puis en MP3 256 kbit/s pour le pack. Le contrôle de cette version mesure environ −17,1 LUFS, 11,7 LU de plage dynamique et −1 dBFS de crête vraie.

## Le lien son–image

Chaque matière reçoit cinq routes Audio :

- Spectre complet → énergie.
- 28–120 Hz → pression et torsion grave.
- 700–3200 Hz → structure et finesse métallique.
- 6000–16000 Hz → densité du grain et des stries.
- Attaques globales → poussées lumineuses courtes.

La FFT analyse le mix lu par PINOL avant le fader général : le niveau d'écoute ne change pas l'image. Les paramètres de mouvement appartiennent aux timelines ; les paramètres audio appartiennent aux interactions ; les cues changent opacité et teinte. Aucun des trois ne réécrit les valeurs pilotées par les deux autres.

## Régénérer

Python 3, NumPy et ffmpeg/ffprobe pour composer ; Python 3 et ffprobe pour fabriquer le projet :

```sh
python3 compose.py --out ./master
python3 build_show.py --audio ./master/Champ_de_rupture.mp3 --out ./nouveau-set
python3 build_show.py --audio ./master/Champ_de_rupture.mp3 --out ./set-compact --timelines 1
```

La variante compacte contient les 18 pistes dans une timeline. La composition est préparée en 1280×720 à 60 fps ; valider la cadence sur la machine de diffusion avant de monter la résolution.
