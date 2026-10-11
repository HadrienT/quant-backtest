# quant-backtest — notes pour Claude Code

Moteur de backtest en **C++20**, exposé à Python par un wheel pybind11
`quantbacktest`. Il couvre les quatre sens du mot sur un desk : backtest d'une
stratégie, d'un modèle et de sa couverture, d'un modèle de risque de marché,
d'un modèle d'exposition.

**Ce dépôt ne contient ni site web, ni accès à une base, ni code de pricing.**
Il calcule : il reçoit des tableaux et rend des tableaux.

## État du dépôt

**Spécification seule, aucun code.** Le dépôt a été créé le 11 octobre 2026 à
partir du lot 24 du blueprint de `quant-modeling`, quand le mainteneur a décidé
d'écrire le moteur en C++ et de le sortir du dépôt de pricing. L'ancienne page
`/backtest` de `quant-modeling` a été masquée le même jour ; son code Python
n'est **pas** repris, le moteur est réécrit de zéro. Le premier lot à faire est
**B0** (socle de build). Mettre cette section à jour à chaque lot livré.

## Par où commencer

| Fichier | Rôle |
|---|---|
| [`docs/companion.md`](docs/companion.md) | Compagnon de lecture **et** spécification : la pratique d'un desk (§1 à §13), les quinze constats sur l'ancienne page (§2), l'architecture (§14), les lots (§15), les tests de propriété (§16), l'état des données (§17), la bibliographie (§19) |
| [`docs/contract.md`](docs/contract.md) | Frontière avec `quant-modeling` : qui possède quoi, forme de l'interface, épinglage des versions |
| [`blueprint/README.md`](blueprint/README.md) | Les lots et leur graphe de dépendances : le consulter avant de démarrer un lot |
| [`blueprint/decisions.md`](blueprint/decisions.md) | Décisions d'architecture, avec les alternatives écartées |

## Relation avec les autres dépôts

| Dépôt | Rôle vis-à-vis d'ici |
|---|---|
| `~/quant-modeling` | **Dépendance C++** : la lib `quantModeling` (pricers, grecques, calibrations), tirée à un commit épinglé, pour le seul module `hedging/`. **Consommateur** : son API importe le wheel `quantbacktest`. Il porte le routeur FastAPI, la lecture en base, l'événement d'audit, le replay et la page `/backtest` |
| `~/data-ingest` | Ingestion des données de marché dans Postgres. Le lot B1 (prix bruts, événements, composition historique, facteurs) se fait là-bas |
| `~/quant-platform` | Kafka, base d'audit, observabilité. Ce dépôt n'y émet rien : c'est l'API de `quant-modeling` qui émet |

Règles de frontière :

- **Ne jamais éditer un dépôt voisin depuis une session ouverte ici.** Un
  changement qui touche deux dépôts = une issue dans chacun, référencées l'une
  par l'autre, et deux PR coordonnées.
- **La dépendance ne va que dans un sens.** Ce dépôt connaît `quantModeling` ;
  rien dans `quant-modeling/include` ou `src` ne doit connaître ce dépôt.
- **Le contrat** est canonique ici ([`docs/contract.md`](docs/contract.md)) ;
  le modifier est un ADR.

## Principes d'architecture non négociables

1. **Le moteur ne lit aucune base, aucun fichier, aucun réseau.** Le SQL est
   dans `quant-modeling`, l'ingestion dans `data-ingest`. Pas de Yahoo Finance,
   nulle part.
2. **Point-in-time par construction** : à la date $t$, le moteur ne donne à la
   stratégie que ce qui était connu en $t$. Le test « pas de futur » du §16
   s'applique à tout nouveau chemin de données.
3. **Jamais d'interpolation ni de repli.** Une donnée absente est un jour non
   traitable ou une erreur explicite.
4. **Le livre est en titres et en espèces**, pas en poids. Chaque ordre laisse
   une ligne dans le journal.
5. **La stratégie est séparée du moteur** : le moteur ne branche sur aucune
   stratégie, comme un engine de `quantModeling` ne branche sur aucun produit.
6. **Seul `hedging/` inclut `quantModeling`.** Les autres modules se
   construisent et se testent sans lui.
7. **L'interface Python ne porte que des données simples** (tableaux numpy,
   nombres, chaînes, dictionnaires). Aucun objet du wheel `quantmodeling` ne
   traverse.
8. **Déterminisme** : mêmes entrées et même graine, mêmes sorties au bit près,
   quel que soit le nombre de threads.
9. **Chaque chiffre rendu porte sa convention et son incertitude.** Une
   métrique qu'on ne sait pas encadrer n'est pas rendue.
10. **Rester près des sources** : noms, formules et conventions de signe sont
    ceux des références du §19 ; la référence est citée dans le commentaire de
    la fonction. Les seuils réglementaires du §12 sont revérifiés contre le
    texte primaire avant d'être codés.

## Commandes

Aucune pour l'instant : les fichiers de build arrivent avec le lot B0, qui
remplira ce tableau. La cible est la même chaîne que `quant-modeling` :
`cmake --preset default && cmake --build build`, `ctest --test-dir build`,
un preset `asan`, `scripts/make.sh`, `scripts/build_wheel.sh`.

## Suivi du travail — GitHub Issues

`gh` est authentifié (compte `HadrienT`).

- **Au démarrage d'une session** : `gh issue list --state open`.
- **Issue traitée** → `gh issue close <n> --comment "fait dans <sha>"`.
- **Jamais** de fichier markdown de passation entre sessions. Une tâche qui
  survit à la session est une issue.
- La conception vit dans `docs/` et `blueprint/` ; l'issue y renvoie, elle ne
  la remplace pas.

## Conventions

- **Conversation avec le mainteneur : en français.** Code, identifiants,
  commentaires, messages de commit, `README.md` : en anglais. `docs/` et
  `blueprint/` sont en français.
- **Commits : passer par une branche, jamais directement sur `main`** (le
  commit initial de documentation est la seule exception). Terminer les
  messages par la ligne `Co-Authored-By` de la session.
- **C++** : `clang-format --style=file`, **version 18** ; le `.clang-format`
  est celui de `quant-modeling`. **Python** : `black`. Le hook pre-commit est à
  installer sur chaque clone : `pre-commit install -t pre-commit`.
- **Propriété mémoire** : préférer `unique_ptr` et des références non
  propriétaires.
- **Tout module arrive avec son test**, et de préférence un test de
  *propriété* (conservation, pas de futur, invariance aux divisions, taille
  empirique d'un test statistique) plutôt qu'une égalité numérique. La liste
  est au §16 du compagnon.
- **Gestion des dépendances déclarative** : tout passe par `vcpkg.json` et les
  presets CMake ; un clone neuf se construit sans étape manuelle.
- **Pas de produit nouveau.** Ce chantier n'ajoute aucun payoff au catalogue de
  `quant-modeling`.
