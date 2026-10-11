# Décisions d'architecture

Chaque décision : ce qui est retenu, pourquoi, et **ce qui est écarté**. Une
décision qui change devient une nouvelle entrée qui la remplace ; on ne réécrit
pas l'historique.

---

## ADR-001 — Un dépôt séparé de `quant-modeling`

**Décision.** Le moteur de backtest vit dans ce dépôt. `quant-modeling` ne
garde que ce qui est du site : la page `/backtest`, un routeur FastAPI mince,
la lecture en base, l'événement d'audit.

**Pourquoi.** Demande du mainteneur (11 octobre 2026) : ne pas charger le dépôt
de pricing avec un moteur comptable et des statistiques de performance. Le
moteur ne change pas avec un pricer et n'a pas à être livré avec lui. Onze des
douze modules ne dépendent pas de la lib de pricing du tout.

**Écarté.**
- *Un dossier `backtest/` dans `quant-modeling`* : plus simple (une CI, un
  build, pas d'épinglage), mais c'est exactement ce que le mainteneur ne veut
  pas, et cela mélange deux cycles de vie.
- *Sortir aussi le site de `quant-modeling`* : c'est le site, plus que le
  backtest, qui lie les choses ; mais c'est un autre chantier, bien plus gros,
  sans rapport avec celui-ci.

**Coût assumé.** Une seconde CI, un second jeu de presets, et une montée de
version à faire à la main quand `quantModeling` change (ADR-003).

---

## ADR-002 — Le moteur est en C++, pas dans l'API en Python

**Décision.** Tout le calcul est en C++20. Python ne fait que passer des
tableaux.

**Pourquoi.** Un run isolé sur des données quotidiennes (500 titres, 26 ans)
est petit. Ce qui coûte, c'est la répétition : marche en avant avec
ré-optimisation sur une grille de paramètres, CPCV, PBO, bootstrap ; et la
couverture en monde simulé, où une boucle Python autour d'appels au wheel de
pricing serait le goulot. **C'est un ordre de grandeur, pas une mesure** : le
lot B2 livre un benchmark, et cette entrée sera complétée par ses chiffres.

**Écarté.**
- *Python vectorisé (numpy) dans l'API*, comme le prévoyait d'abord le lot 24
  de `quant-modeling` : suffisant pour un run, et beaucoup moins de code ;
  mais le moteur événementiel (une boucle sur le temps, par construction) et
  la validation y sont lents, et la boucle de couverture y traverse pybind11 à
  chaque pas.
- *Python accéléré (numba)* : règle la boucle, pas l'appel à la lib de pricing,
  et ajoute une dépendance lourde à l'image.

---

## ADR-003 — `quantModeling` est une dépendance C++ épinglée sur un commit

**Décision.** Le module `hedging/` consomme la lib `quantModeling` par
`FetchContent`, avec un `GIT_TAG` qui est un **SHA de commit**, jamais une
branche. Les autres modules ne l'incluent pas.

**Pourquoi.** Un build doit être reproductible : le même commit d'ici doit
toujours construire contre le même pricing. `quantModeling` laisse déjà ses
tests, ses bindings, ses benchmarks et son binaire derrière des options
éteintes par défaut (`QM_BUILD_TESTS`, `QM_BUILD_PYTHON`, …), ce qui le rend
en principe consommable comme sous-projet. **À vérifier au lot B0** : qu'il se
configure sans être le projet racine, et qu'il expose une cible utilisable. Si
ce n'est pas le cas, la correction se fait par une issue et une PR dans
`quant-modeling`.

**Vérifié au lot B0** (11 octobre 2026, commit `fdc1a10` de `quant-modeling`).
Il se configure comme sous-projet sans modification : ses chemins partent de
`PROJECT_SOURCE_DIR`, ses options sont éteintes par défaut, et la cible
`quantModeling` porte ses en-têtes et Eigen. Aucune issue à ouvrir là-bas. Deux
conséquences : Eigen entre dans `vcpkg.json` d'ici, parce que la lib le
cherche ; et `FetchContent` clone tout le dépôt (site compris), un SHA ne
pouvant pas être tiré en clone superficiel.

**Écarté.**
- *Sous-module git* : même épinglage, mais une manipulation de plus à chaque
  clone et à chaque montée de version, pour un mainteneur qui veut un clone
  qui se construit sans étape manuelle.
- *Port vcpkg en overlay* : la solution la plus propre à terme (une ligne dans
  `vcpkg.json`), mais elle demande à `quantModeling` des règles d'installation
  et d'export qu'il n'a pas. À reconsidérer si un troisième projet veut
  consommer la lib.
- *Chemin local vers `~/quant-modeling`* : ne se construit ni en CI ni sur un
  autre clone, et dépend de la branche ouverte dans le dossier voisin.

---

## ADR-004 — Un wheel importé par l'API, pas un service

**Décision.** Le moteur est livré comme un wheel pybind11 `quantbacktest`, que
l'API de `quant-modeling` importe dans son propre process.

**Pourquoi.** L'API possède déjà l'authentification, la piste d'audit, la
télémétrie, le contrat OpenAPI et le déploiement. Un wheel n'en duplique
aucun.

**Écarté.**
- *Un service à part* (son conteneur, nginx qui route `/api/backtest`) :
  isole davantage et se redéploie seul, mais oblige à refaire ici l'auth JWT,
  l'émission d'audit, la télémétrie et un second contrat OpenAPI, et ajoute un
  conteneur à exploiter. À reconsidérer seulement si les runs longs gênent
  l'API au point de devoir tourner sur un autre process.

---

## ADR-005 — L'interface Python ne porte que des données simples

**Décision.** Les fonctions du wheel prennent et rendent des tableaux numpy,
des nombres, des chaînes et des dictionnaires. Un modèle de pricing est décrit
par son nom et ses paramètres ; le moteur construit lui-même l'objet C++.

**Pourquoi.** Les wheels `quantmodeling` et `quantbacktest` lient chacun leur
propre copie de la lib `quantModeling`. Faire passer un objet C++ de l'un à
l'autre dépendrait de l'ABI, de la version de pybind11 et de l'égalité exacte
des deux commits : fragile, et faux en silence quand cela casse. Des données
simples rendent aussi le run **rejouable** : la requête entière se sérialise
dans l'événement d'audit.

**Écarté.**
- *Partager les types pybind11 entre les deux modules* : possible, mais lie
  les deux wheels à la même version au commit près.

---

## ADR-006 — Repartir de zéro, sans reprendre l'ancien code

**Décision.** Rien de l'ancien routeur Python de `quant-modeling`
(`api/app/routers/backtest.py`) ni de l'ancienne page n'est repris. La page a
été masquée le 11 octobre 2026 en attendant.

**Pourquoi.** Les écarts de l'ancien code tiennent à sa conception (un Sharpe
maximisé sur une seule fenêtre, des poids au lieu d'un livre), pas à des
détails ; aucune ligne de pandas ne survit au passage en C++ ; et corriger
d'abord l'ancien code aurait été du travail jeté. Ce qu'on garde, ce sont ses
**quinze constats** (§2 du compagnon), qui deviennent des tests.

**Écarté.**
- *Corriger d'abord l'ancien code* (l'ancien « lot B0 ») pour que la page reste
  en ligne avec des chiffres justes : plusieurs jours sur un code condamné.
- *Laisser la page en ligne avec un bandeau* : des chiffres faux restent
  affichés.

---

## ADR-007 — Le moteur ne lit aucune donnée lui-même

**Décision.** Aucun accès à Postgres, à un fichier ou au réseau dans ce dépôt.
L'appelant fournit des tableaux ; le moteur les contrôle (`quality/`) et
refuse ceux qui sont inutilisables.

**Pourquoi.** C'est la règle de `quantModeling` (« aucun instrument ne lit de
donnée de marché ») appliquée ici. Elle rend le moteur testable sans base,
déterministe, et laisse à un seul endroit (`quant-modeling`, `api/app/db.py`)
la connaissance du schéma de `data-ingest`.

**Écarté.**
- *Un paquet Python de lecture dans ce dépôt* : placerait la logique
  point-in-time à côté du moteur, mais ferait dépendre ce dépôt du schéma de
  `data-ingest` et d'identifiants de base. Les ajustements qui relèvent du
  métier (créditer un dividende, appliquer une division) sont faits par le
  moteur, à partir des prix bruts et des événements reçus ; il ne reste à
  l'appelant que du SQL.

---

## ADR-008 — `hedging/` est une cible CMake à part

**Décision.** Deux cibles : `quantBacktest` (tous les modules sauf la
couverture), qui ne lie pas `quantModeling`, et `quantBacktest_hedging`, qui le
lie. Deux exécutables de test suivent le même découpage. L'option
`QB_BUILD_HEDGING=OFF` (preset `no-hedging`) retire la seconde, et la CI
construit les deux configurations.

**Pourquoi.** La règle « seul `hedging/` inclut `quantModeling` » est alors
tenue par le build et non par la relecture : un fichier de `quantBacktest` qui
inclurait un en-tête de pricing ne compile pas, faute de chemin d'inclusion.
Le preset `no-hedging` donne aussi une boucle de travail courte pour les lots
B2 à B5 et B7, qui n'ont pas à recompiler la lib de pricing.

**Écarté.**
- *Une seule lib, la règle vérifiée par un script sur les `#include`* : moins
  de cibles, mais une règle de plus à maintenir hors du build.

**Dans le même lot.** pybind11 vient de l'environnement de build Python
(`pyproject.toml`) et non de vcpkg, dont le port tire son propre Python ; et
`vcpkg.json` fixe un `builtin-baseline`, pour que les versions d'Eigen et de
GoogleTest ne dépendent pas de la date du clone de vcpkg.
