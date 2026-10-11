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
pricing serait le goulot. C'était un ordre de grandeur ; les chiffres du lot B2 sont ci-dessous.

**Mesuré au lot B2** (11 octobre 2026, `scripts/bench.sh`, un cœur d'un Xeon
E5-2680 v4, données simulées de 500 titres sur 6 552 jours, soit 26 ans).

| Calcul | Temps |
|---|---|
| Un run, équipondéré rebalancé chaque mois, coût proportionnel | 81 ms |
| Le même avec un coût d'écart estimé (table déjà calculée) | 70 ms |
| Un run rebalancé chaque jour (3,3 millions d'ordres au journal) | 0,97 s |
| Calcul vectorisé du rebalancement quotidien, sans livre ni coûts | 4,5 ms |
| Contrôles de qualité des données, une fois par jeu de données | 236 ms |
| Table des écarts estimés (Corwin-Schultz), une fois par jeu de données | 300 ms |

Mille runs mensuels d'une grille de validation tiennent donc en moins d'une
minute et demie sur un cœur. **Ce qui n'est pas mesuré** : l'équivalent en
Python. L'ancien code n'a pas été repris et aucune boucle Python comparable
n'a été écrite ; l'écart avec Python reste un ordre de grandeur supposé. Deux
choses apprises en mesurant : les contrôles de qualité coûtent trois fois un
run mensuel, d'où `data_already_checked` pour ne les faire qu'une fois ; et
estimer l'écart à chaque ordre multipliait le temps par vingt, d'où une table
calculée une fois (`estimated_spreads`).

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

---

## ADR-009 — Un générateur aléatoire à nous, un flux par réplication

**Décision.** Tout tirage passe par `core/random.hpp` (SplitMix64), jamais par
les distributions de `<random>`. Chaque réplication d'un bootstrap tire dans
son propre flux, dérivé de la graine et du numéro de la réplication. Les
premiers tirages du générateur sont épinglés par un test.

**Pourquoi.** Le contrat demande les mêmes chiffres au bit près pour les mêmes
entrées et la même graine, quel que soit le nombre de threads, et sur toute
machine. Les distributions de la bibliothèque standard ne garantissent pas la
même suite d'une implémentation à l'autre ; et un générateur partagé entre
threads donnerait un résultat qui dépend de l'ordonnancement.

**Écarté.**
- *`std::mt19937_64` et `std::uniform_real_distribution`* : le moteur est
  spécifié, pas la distribution.
- *Un générateur par thread* : déterministe pour un nombre de threads donné,
  mais le résultat change quand ce nombre change.

**Dans le même lot (B3).** Eigen entre dans la lib principale (régression,
puis optimisation au lot B4), en dépendance privée : aucun en-tête public ne
l'expose. Le nombre de retards de Newey-West et la longueur de bloc du
bootstrap n'ont pas de valeur par défaut : ce sont des choix que l'appelant
déclare, et que le résultat répète.

---

## ADR-010 — Le solveur d'optimisation est écrit ici

**Décision.** Les portefeuilles optimisés sont calculés par deux algorithmes
écrits dans `construction/`, sur Eigen :

- un **ensemble actif primal** (Nocedal & Wright 2006, §16.5) pour le problème
  quadratique à budget et à bornes, $\min \tfrac12 w^\top \Sigma w - c^\top w$
  sous $\sum_i w_i = 1$ et $l \le w \le u$. Il sert à la variance minimale et,
  par recherche de racine le long de la frontière efficiente, au Sharpe
  maximal ;
- une **itération de Newton** sur la formulation convexe de Spinu (2013) pour
  la parité de risque.

**Pourquoi.** Le lot B4 demande que les contraintes soient tenues **dans**
l'optimiseur et qu'un problème sans solution échoue explicitement (constat 5).
Un ensemble actif rend un poids à sa borne *égal* à sa borne, pas à 1e-6 près,
et l'infaisabilité se décide avant de résoudre, par une somme. Les problèmes
sont petits et denses (de quelques titres à quelques centaines), et un
portefeuille long à bornes n'a que peu de poids strictement intérieurs : le
système à résoudre à chaque itération est de la taille de ces poids-là.

**Écarté.**
- *OSQP* (port vcpkg) : robuste et rapide, mais c'est une méthode ADMM, dont la
  solution respecte les bornes à une tolérance près. Il faudrait rogner et
  renormaliser après coup, ce qui est exactement le défaut du constat 5.
- *Un solveur quadratique général* (Goldfarb-Idnani, `eigen-quadprog`) : exact
  lui aussi, mais il ajoute une dépendance pour une classe de contraintes
  (inégalités linéaires quelconques) dont on n'a pas l'usage aujourd'hui.
- *Reprendre SLSQP* par NLopt : une dépendance lourde, et un solveur local
  dont le succès doit être testé à chaque appel.

**Limites assumées.** Seules les bornes par titre et le budget sont des
contraintes : ni secteur, ni neutralité au marché, ni limite de rotation
(§4.5). Les ajouter demandera des inégalités générales, donc de revenir sur
cette décision. La covariance doit être définie positive : celle de
l'échantillon ne l'est pas avec moins de périodes que de titres, et elle est
alors refusée avec un message qui renvoie à l'estimateur contracté.

---

## ADR-011 — Dans la couverture, `quantModeling` valorise ; le monde simulé est écrit ici

**Décision.** Le backtest de couverture en monde simulé demande à
`quantModeling` les prix et les grecques (Black-Scholes par le registre,
Heston par la méthode COS, volatilité implicite), et simule lui-même les
trajectoires du marché, avec le générateur de l'ADR-009 : un pas exact en
Black-Scholes, un schéma d'Euler à troncature complète en Heston.

**Pourquoi.** Une couverture a besoin, à chaque date, de l'état complet du
monde (le spot **et** la variance) ; le cadre de simulation de `quantModeling`
est fait pour valoriser un produit, et n'expose que ce que le produit lit.
Simuler ici garde aussi le déterminisme sous notre contrôle : un flux par
trajectoire, donc le même résultat quel que soit le nombre de threads.

**Écarté.**
- *Passer par `ISimulationModel` de `quantModeling`* : il faudrait y déclarer
  une ligne de temps et un produit factice pour lire les spots, et la variance
  n'en sortirait pas.
- *Écrire aussi les pricers ici* : c'est précisément ce que ce dépôt ne fait
  pas. Le seul calcul de pricing ajouté est un garde-fou, celui qui suit.

**Garde-fou assumé.** `heston_cos_price` rend des prix faux quand le strike
sort de son intervalle d'intégration (issue `quant-modeling#168`). Au-delà de
huit écarts-types du strike, `hedging/` n'appelle pas le pricer et prend la
valeur intrinsèque, qui est la valeur exacte à la précision machine. À retirer
quand le défaut est corrigé et le commit épinglé monté.
