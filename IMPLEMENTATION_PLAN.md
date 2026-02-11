# Plan d'implémentation — CoD1 SoloQ S&D Stats Tracker

## Vue d'ensemble

Le payload cible (fpschallenge.eu) nécessite 3 sources de données combinées :

1. **Config du match** — vient de la plateforme : `match_id`, `team1_id`, `team2_id`, noms, UUIDs joueurs, format
2. **Événements de manche** — vient de PAM/GSC : round end, scores, bomb state
3. **Stats par joueur** — vient de PAM/mémoire : kills, deaths, assists, damage, grenades, plants, defuses

Ces 3 sources sont sur des systèmes différents et doivent être fusionnées avant l'envoi HTTP.

---

## Architecture cible

```
fpschallenge.eu
      │ (match config payload au démarrage)
      ▼
  Backend Node.js (port 3005)
      │  stocke la config en RAM/fichier
      │
      │◄── HTTP POST (stats par round) ── LD_PRELOAD (cod1plus.so)
                                                │
                                         lit les événements PAM
                                         depuis stdout CoD1
                                                │
                                         CoD1 + PAM (sd.gsc modifié)
                                         print("[STATS_EVENT] {...}")
                                         à chaque fin de manche
```

---

## Composant 1 — Config du match (réception)

### Problème
Le payload cible contient `match_id`, `team1_id`, `team2_id`, les noms d'équipes, et surtout les **UUIDs joueurs** — données qui n'existent pas dans CoD1.

### Solution
Le backend reçoit la config du match **avant le lancement** (via un POST de la plateforme ou manuellement). Elle est stockée et consultable par le LD_PRELOAD.

### Endpoints backend à ajouter
- `POST /api/match/config` — reçoit la config complète de la plateforme (match_id, teams, players+UUIDs)
- `GET /api/match/current` — retourne la config active (consommée par cod1plus.so)

### Mapping joueur
Les UUIDs plateforme doivent être associés aux joueurs CoD1. Deux méthodes possibles :
- **Par nom** (`forceNickNames=true` dans le payload → les joueurs sont forcés à jouer sous leur vrai nom)
- **Par GUID** (`cl_guid` dans `client_t.userinfo`, déjà lisible — ex: `243D3280F01F0DC9F49E5B22BC124428`)

Le plus fiable : matching par **nom** (avec `forceNickNames`) + fallback GUID.

---

## Composant 2 — Événements PAM (GSC → stdout)

### Principe
PAM GSC ne peut pas faire de requêtes HTTP. La solution : faire `print()` dans `sd.gsc` à chaque fin de manche avec une ligne JSON taguée. Le LD_PRELOAD capture stdout de CoD1 et parse ces lignes.

### Où hooker dans sd.gsc
PAM a déjà un système d'événements (`events.gsc`). Le point d'injection est dans `sd.gsc` au moment où le round est résolu :
- Quand toute l'équipe adverse est morte
- Quand la bombe explose
- Quand la bombe est désamorcée
- Quand le temps expire

Ces 4 cas convergent dans la logique de fin de round de `sd.gsc`.

### Format de la ligne à printer

```
[STATS_EVENT] {"round":1,"allies_score":0,"axis_score":1,"bomb_planted":false,"bomb_exploded":true,"bomb_defused":false,"players":[{"name":"wsx","team":"axis","kills":1,"deaths":0,"assists":0,"damage":100,"grenades":0,"plants":0,"defuses":0,"score":1.0},...]}
```

### Variables PAM disponibles dans sd.gsc
| Variable | Contenu |
|----------|---------|
| `game["roundsplayed"]` | Numéro de manche |
| `game["allies_score"]` | Score équipe allies |
| `game["axis_score"]` | Score équipe axis |
| `game["bombplanted"]` | Timestamp plant (ou undefined) |
| `level.bombplanted` | Booléen bombe plantée |
| `game["playerstats"][i].kills` | Kills joueur i |
| `game["playerstats"][i].deaths` | Deaths joueur i |
| `game["playerstats"][i].assists` | Assists joueur i |
| `game["playerstats"][i].damage` | Damage joueur i |
| `game["playerstats"][i].grenades` | Grenades joueur i |
| `game["playerstats"][i].plants` | Plants joueur i |
| `game["playerstats"][i].defuses` | Defuses joueur i |
| `game["playerstats"][i].score` | Score joueur i |
| `game["playerstats"][i].team` | Équipe joueur i |
| `game["playerstats"][i].name` | Nom joueur i |

**Important** : PAM track déjà les stats depuis le début du match, pas seulement par round. Pour avoir les stats **par round**, il faut soit :
- **Option A** : Lire les stats cumulées + calculer le delta depuis la manche précédente (côté backend)
- **Option B** : Ajouter un reset des compteurs dans la GSC à chaque début de round (plus propre, plus simple)

→ **Option A recommandée** : moins de modifications GSC, le backend fait le diff.

### Capture stdout dans LD_PRELOAD
CoD1 utilise `printf()` / `write()` pour sa console. Le LD_PRELOAD peut hooker `write(fd=1, ...)` pour intercepter stdout et parser les lignes `[STATS_EVENT]`.

---

## Composant 3 — Construction et envoi du payload

### Logique dans cod1plus.c (LD_PRELOAD)
1. Au démarrage : charger la config du match depuis `GET /api/match/current`
2. Surveiller stdout pour les lignes `[STATS_EVENT]`
3. À chaque événement reçu :
   - Fusionner avec la config du match (noms équipes, IDs, format)
   - Mapper `player.name` → `player.uuid` via la config
   - Calculer `team1_score` / `team2_score` (en tenant compte du changement de côté à mi-match)
   - Construire le JSON complet au format fpschallenge.eu
   - POST vers l'endpoint cible

### Champs du payload et leur source

| Champ payload | Source |
|---------------|--------|
| `type` | Constante `"data"` |
| `start_time` | Timestamp au démarrage du match (stocké dans config) |
| `match_id` | Config match |
| `team1_id` / `team2_id` | Config match |
| `team1_name` / `team2_name` | Config match |
| `format` | Config match (`"BO1"`, `"BO3"`, etc.) |
| `forceNickNames` | Config match |
| `playersCount` | Nombre de joueurs dans config |
| `team1_score` / `team2_score` | Événement GSC (allies/axis score + mapping équipe↔team1/team2) |
| `map` | Lire depuis svs ou cvar `sv_mapname` en mémoire |
| `round` | `"Round X | MR12"` — construit depuis `game["roundsplayed"]` + format |
| `state` | `"playing"` / `"finished"` selon score et limite |
| `debug` | `"sd endround"` (fixe) |
| `players[].uuid` | Mapping nom→uuid depuis config |
| `players[].name` | Événement GSC |
| `players[].team` | Événement GSC (allies/axis) + mapping vers team1/team2 |
| `players[].kills` | Événement GSC |
| `players[].deaths` | Événement GSC |
| `players[].assists` | Événement GSC |
| `players[].damage` | Événement GSC |
| `players[].grenades` | Événement GSC |
| `players[].plants` | Événement GSC |
| `players[].defuses` | Événement GSC |
| `players[].score` | Événement GSC |

### Point délicat : allies/axis vs team1/team2
À mi-match, les équipes changent de côté (halftime dans PAM). team1 peut être allies au départ puis axis ensuite. Il faut tracker quel côté correspond à quelle team_id dans la config.

---

## Étapes de développement (dans l'ordre)

### Étape 1 — Backend : réception config match
- Ajouter `POST /api/match/config` qui stocke la config en mémoire
- Ajouter `GET /api/match/current` qui la retourne
- Prévoir un champ `sides` pour tracker le mapping allies/axis ↔ team1/team2 avec halftime

### Étape 2 — GSC : hook round end dans sd.gsc
- Trouver le point exact de fin de round dans `sd.gsc`
- Ajouter une fonction `PrintStatsEvent()` qui formate et print le JSON taguée
- Appeler cette fonction juste avant le `wait` post-round ou la préparation de la prochaine manche
- Tester sur serveur et vérifier la ligne dans stdout

### Étape 3 — LD_PRELOAD : capture stdout
- Hooker `write(fd=1, ...)` via LD_PRELOAD
- Parser les lignes contenant `[STATS_EVENT]`
- Extraire le JSON embarqué
- Logger pour validation

### Étape 4 — LD_PRELOAD : chargement config + construction payload
- Au démarrage du thread : GET `/api/match/current`
- Stocker la config en mémoire dans cod1plus.c
- À chaque événement capturé : fusionner GSC data + config match → payload complet

### Étape 5 — LD_PRELOAD : envoi HTTP vers cible
- POST le payload vers l'URL de la plateforme (ou vers backend local qui forward)
- Gérer les erreurs réseau (retry ou log)

### Étape 6 — Mapping halftime
- Détecter le halftime dans les événements GSC (`game["is_halftime"]`)
- Swapper le mapping allies/axis ↔ team1/team2 dans la config en RAM

---

## Questions ouvertes à résoudre avant de coder

1. **Comment la config du match arrive-t-elle ?** — Est-ce que fpschallenge.eu peut envoyer un POST au backend au moment où le match est créé ? Ou est-ce manuel (fichier config) ?

2. **forceNickNames = true est garanti ?** — Les joueurs jouent-ils forcément sous leur nom de plateforme ? Sinon il faut le matching par GUID.

3. **Le payload est envoyé où exactement ?** — Directement vers `fpschallenge.eu/api/v2/...` ou vers votre backend local qui relaie ?

4. **Stats cumulées ou par round ?** — Le payload montré semble contenir les stats depuis le début du match (kills cumulés), pas uniquement du round en cours. À confirmer.

5. **PAM `_player_stat.gsc` est-il actif par défaut en S&D comp ?** — Vérifier que `game["playerstats"]` est bien populé en jeu compétitif.
