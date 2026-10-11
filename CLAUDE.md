# quant-backtest — notes pour Claude Code

Moteur de backtest en **C++20**, exposé à Python par un wheel pybind11
`quantbacktest`. Il couvre les quatre sens du mot sur un desk : backtest d'une
stratégie, d'un modèle et de sa couverture, d'un modèle de risque de marché,
d'un modèle d'exposition.

**Ce dépôt ne contient ni site web, ni accès à une base, ni code de pricing.**
Il calcule : il reçoit des tableaux et rend des tableaux.

## État du dépôt

**Tous les lots de ce dépôt sont livrés : B0, B2, B3, B4, B5, B6 et B7.** Il
reste B1 dans `data-ingest` et B8 dans `quant-modeling`. Modules présents :
`core/`, `data/`, `quality/`, `costs/`, `engine/`, `metrics/`, `inference/`,
`construction/`, `validation/`, `risk_models/`, `hedging/`, et la régression
factorielle de `attribution/`. Le wheel `quantbacktest` expose :

- `build_info()`, `fingerprint()`, `total_returns()`, `period_rates()` ;
- `run_allocation()`, le backtest d'une stratégie avec ses points de
  comparaison, son run brut et son bilan de coûts ;
- `run_allocation_grid()`, un backtest par configuration, qui rend la table
  attendue par la validation ;
- `performance_report()`, `factor_regression()`, `sharpe_difference()` ;
- `walk_forward()`, `combinatorial_purged_cv()`, `deflated_sharpe()`,
  `backtest_overfitting()`, sur une table de rendements à une colonne par
  configuration essayée ;
- `hedging_backtest()`, `fit_minimum_variance_delta()` ;
- `var_backtest()`.

Leur forme est au §5 de [`docs/contract.md`](docs/contract.md). Une stratégie
est du C++ : le wheel n'accepte pas de poids calculés côté Python, qui
contourneraient le point-in-time.

Contraintes d'optimisation disponibles : bornes par titre et budget, rien
d'autre (ADR-010).

Le backtest de couverture tourne en **monde simulé** seulement (Black-Scholes
ou Heston) ; la version sur surfaces historiques attend des données (issue
#15). Il contourne un défaut du pricer Heston de `quantModeling` loin du
strike (ADR-011) : à retirer quand `quant-modeling#168` est corrigée.

Le moteur comptable sait vendre à découvert (`allow_short`, avec un taux
d'emprunt fourni), arrondir aux lots entiers (`whole_shares`) et facturer un
impact de marché en racine carrée. Ce qu'il **ne fait pas** encore (issue #9) :
le rendement de radiation d'un titre qui cesse de coter (la position reste à
son dernier cours, signalée dans le rapport de qualité), faute de données.

À savoir avant d'utiliser le coût d'écart estimé : sur des titres liquides les
estimateurs de Corwin-Schultz et d'Abdi-Ranaldo surestiment l'écart d'un ordre
de grandeur (§5.1 du compagnon).

Le dépôt a été créé le 11 octobre 2026 à partir du lot 24 du blueprint de
`quant-modeling`, quand le mainteneur a décidé d'écrire le moteur en C++ et de
le sortir du dépôt de pricing. L'ancienne page `/backtest` de `quant-modeling`
a été masquée le même jour ; son code Python n'est **pas** repris, le moteur
est réécrit de zéro. Les restes non bloquants des lots sont dans les issues
ouvertes. Le lot B1 est suivi dans `data-ingest` (issue #14
de ce dépôt-là). Mettre cette section à jour à chaque lot livré.

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

`VCPKG_ROOT` doit pointer sur un clone de vcpkg ; les dépendances sont
installées par le manifeste [`vcpkg.json`](vcpkg.json) à la configuration.

| Commande | Effet |
|---|---|
| `scripts/make.sh` | Configure (preset `default`), construit dans `build/`, lance tous les tests |
| `scripts/make_asan.sh` | Idem avec ASan et UBSan, dans `build-asan/` |
| `cmake --preset default && cmake --build build` | Construire sans lancer les tests |
| `ctest --test-dir build --output-on-failure` | Tous les tests |
| `ctest --test-dir build -LE hedging` | Les tests qui n'ont pas besoin de `quantModeling` |
| `ctest --test-dir build -R BuildInfo` | Les tests dont le nom contient un motif |
| `cmake --preset no-hedging && cmake --build build-no-hedging` | Construire sans `quantModeling` (principe 6) : la boucle courte pour tout sauf `hedging/` |
| `scripts/bench.sh` | Construit (preset `bench`, dans `build-bench/`) et lance le benchmark ; les arguments vont à google-benchmark, par exemple `--benchmark_filter=Monthly` |
| `scripts/build_wheel.sh` | Wheel `quantbacktest` dans `dist/` |
| `pytest` | Tests Python, sur le wheel installé dans l'environnement courant (`pip install dist/*.whl pytest`) |
| `pre-commit run --all-files` | `clang-format` 18 et `black` |

Cibles CMake : `quantBacktest` (tout sauf la couverture, ne lie pas
`quantModeling`), `quantBacktest_hedging` (la seule qui le lie),
`quantBacktest_tests`, `quantBacktest_hedging_tests` et `quantBacktest_bench`.
Un fichier source ou un test nouveau s'ajoute à la main dans la liste de sa
cible. Les tests construisent leurs données avec
[`tests/support/synthetic_market.hpp`](tests/support/synthetic_market.hpp).
Tout tirage aléatoire de la lib passe par `core/random.hpp` (ADR-009) ; les
tests, eux, peuvent utiliser `<random>`.

**Monter la version de `quantModeling`** : changer `QB_QUANTMODELING_SHA` dans
`cmake/QuantModeling.cmake` (un SHA de commit, jamais une branche), dans une PR
à part.

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
