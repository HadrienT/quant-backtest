# Contrat entre quant-backtest et quant-modeling

Ce document dit **qui possède quoi** et **sous quelle forme les deux dépôts se
parlent**. Il est canonique ici ; le modifier est un ADR dans
[`../blueprint/decisions.md`](../blueprint/decisions.md), et se fait par deux
issues coordonnées (une par dépôt).

Les §1 à §4 fixent les règles que suit l'interface Python ; le §5 liste ses
fonctions et se remplit lot par lot.

## 1. Qui possède quoi

| Sujet | Propriétaire |
|---|---|
| Moteur comptable, coûts, construction, métriques, inférence, validation, attribution, couverture, tests des modèles de risque | **quant-backtest** |
| Contrôles de qualité sur les séries reçues | **quant-backtest** |
| Pricers, grecques, calibrations, modèles | **quant-modeling** (lib `quantModeling`) |
| Lecture en base (SQL sur le Postgres de `data-ingest`) | **quant-modeling** (`api/app/`) |
| Route HTTP, schéma de requête et de réponse, contrat OpenAPI | **quant-modeling** |
| Authentification, gestion des tâches longues (file, identifiant, annulation par l'utilisateur) | **quant-modeling** |
| Événement d'audit `backtest.run`, replay, journal des essais | **quant-modeling** (`api/app/audit/`) |
| Page `/backtest`, sa documentation et ses sources | **quant-modeling** (`web/`) |
| Ingestion des données manquantes (lot B1) | **data-ingest** |

Une question qui ne rentre dans aucune ligne se tranche par la règle : **tout
calcul ici, toute entrée-sortie là-bas**.

## 2. Les deux liens

```
                 lib C++ quantModeling, à un commit épinglé (FetchContent)
quant-modeling ─────────────────────────────────────────────────────────► quant-backtest
   (pricing)                                                                 (hedging/)

                 wheel quantbacktest, à une version épinglée
quant-backtest ─────────────────────────────────────────────────────────► quant-modeling
   (moteur)                                                                  (api/app/)
```

Ce n'est pas un cycle de code : aucun fichier de `quant-modeling/include` ou
`src` ne connaît ce dépôt. Mais ce sont **deux épinglages**, à monter dans cet
ordre quand le pricing change :

1. `quant-modeling` fusionne son changement dans `main` ;
2. ici, une PR monte le SHA de `quantModeling`, la CI rejoue les tests de
   `hedging/` ;
3. `quant-modeling` monte la version du wheel `quantbacktest` dans son image.

Tant que l'étape 2 n'est pas faite, le backtest de couverture valorise avec
l'ancien pricing : c'est voulu (reproductibilité), et c'est visible, parce que
le wheel expose le SHA contre lequel il a été construit (§4).

## 3. Forme de l'interface Python

- **Données simples seulement** : tableaux numpy, nombres, chaînes,
  dictionnaires, listes. Aucun objet du wheel `quantmodeling` n'est accepté ni
  rendu (ADR-005). Un modèle est désigné par son nom et ses paramètres.
- **Les dates** sont des tableaux `datetime64[D]`, triés, sans doublon. Le
  calendrier est celui des données reçues ; le moteur n'en invente aucun jour.
- **Les prix sont bruts** (non ajustés), accompagnés des tables de dividendes
  et de divisions. Une série ajustée n'est pas une entrée valide du moteur
  comptable.
- **Une valeur manquante est un `NaN`**, qui veut dire « pas de cotation ce
  jour-là ». Le moteur ne la remplace jamais par une interpolation.
- **Tout ce qui dépend du temps est daté** : un taux sans risque, un univers,
  un facteur arrivent comme des séries, jamais comme un nombre unique appliqué
  à tout le passé.
- **Chaque métrique rendue** porte sa valeur, sa convention et son incertitude
  (erreur-type ou intervalle, et la méthode qui l'a produite).
- **Les erreurs sont explicites** : une entrée inutilisable lève une exception
  dont le message, en anglais, peut être montré tel quel à l'utilisateur.
- **Les appels longs** relâchent le GIL, acceptent une fonction de progression
  et un signal d'annulation, et s'arrêtent proprement quand il est levé.
- **Le nombre de threads** est un paramètre de l'appel, borné par l'appelant.
  Le résultat n'en dépend pas.

## 4. Reproductibilité

Un run doit pouvoir être rejoué par `quant-modeling` et redonner les mêmes
chiffres au bit près. Pour cela le wheel expose :

| Élément | Rôle |
|---|---|
| sa version et le SHA du commit de ce dépôt | version du code du moteur |
| le SHA de `quantModeling` contre lequel il a été construit | version du pricing utilisé par `hedging/` |
| une fonction d'empreinte des tableaux d'entrée | « empreinte des données lues » de l'événement d'audit |
| la graine de tout tirage aléatoire, en paramètre explicite | bootstrap, mondes simulés |

`quant-modeling` enregistre ces éléments, avec la requête entière, dans
l'événement `backtest.run`. Ce dépôt n'émet aucun événement et ne tient aucun
journal.

## 5. Fonctions exposées

Chaque lot qui ajoute une fonction au wheel ajoute ici sa ligne : nom, entrées,
sorties, unités, convention.

| Fonction | Lot | Entrées | Sorties |
|---|---|---|---|
| `build_info()` | B0 | aucune | dictionnaire de chaînes : `version` (`majeur.mineur.correctif`, aussi dans `__version__`), `git_sha` (40 chiffres hexadécimaux, suivis de `-dirty` si l'arbre avait des modifications non commitées, ou `unknown` si le build n'avait pas d'information git), `quantmodeling_sha` (40 chiffres hexadécimaux) |
| `fingerprint(*arrays)` | B3 | un ou plusieurs tableaux numpy, de tout type sauf objet | chaîne de 64 chiffres hexadécimaux : SHA-256 du type, de la forme et des octets de chaque tableau, dans l'ordre. Les mêmes valeurs dans un autre type, une autre forme ou un autre ordre donnent une autre empreinte |
| `performance_report(returns, cash_rate, benchmark=None, *, bootstrap_replications, mean_block_length, seed, periods_per_year=252, confidence=0.95, tail=0.05, threads=1, progress=None, cancel=None)` | B3 | `returns` : rendements arithmétiques par période, nets de coûts ; `cash_rate` : taux sans risque simple de chaque période, aligné ; `benchmark` : rendements d'un indice dividendes réinvestis, aligné. Les trois arguments du bootstrap n'ont **pas de valeur par défaut** ; `bootstrap_replications=0` ne rend que ce qui a une erreur-type analytique | dictionnaire : `observations`, `periods_per_year`, `years` ; `metrics` (nom → `value`, `standard_error`, `lower`, `upper`, `confidence`, `method`, `convention`) ; `omitted` (nom → raison) ; `t_statistic`, `probabilistic_sharpe_ratio` (seuil zéro), `minimum_track_record_years`. Ratios annualisés en √`periods_per_year` ; `max_drawdown` négatif ou nul ; `value_at_risk` et `expected_shortfall` par période, en **pertes positives**. Une métrique qu'on ne sait pas encadrer est dans `omitted`, pas dans `metrics` |
| `factor_regression(excess, factors, *, newey_west_lags, periods_per_year=252, confidence=0.95)` | B3 | `excess` : rendements en excès par période ; `factors` : tableau à deux dimensions, une ligne par période, une colonne par facteur. `newey_west_lags` n'a **pas de valeur par défaut** (0 = covariance de White) | dictionnaire : `alpha` (**annualisé** : intercept × `periods_per_year`, même forme qu'une métrique), `betas` (liste, une entrée par facteur), `alpha_t_statistic`, `beta_t_statistics`, `r_squared`, `residual_volatility` (annualisée), `observations`, `newey_west_lags` |
| `var_backtest(pnl, value_at_risk, *, tail_probability)` | B7 | `pnl` : P&L signé de chaque jour, le P&L **hypothétique** ; `value_at_risk` : VaR annoncée pour ce jour, en **perte positive**, alignée ; `tail_probability` : 0,01 pour une VaR à 99 %, sans valeur par défaut | dictionnaire : `observations`, `exceptions`, `expected_exceptions`, `exception_days` (tableau de 0 et 1) ; `unconditional_coverage` (Kupiec), `independence` et `conditional_coverage` (Christoffersen), chacun avec `statistic`, `p_value`, `degrees_of_freedom`, à `NaN` quand l'échantillon ne permet pas le test ; `transitions` ((n00, n01), (n10, n11)) ; `traffic_light` : `zone` (`green`, `amber`, `red`), `cumulative_probability`, et, pour 250 jours à 99 % seulement (sinon `None`), `basel_plus_factor` et `frtb_multiplier` |

Erreurs : une entrée inutilisable lève `quantbacktest.Error`, sous-classe de
`ValueError` ; un run arrêté par `cancel` lève `quantbacktest.Cancelled`,
sous-classe de `Error`. `progress` reçoit la fraction accomplie ; `cancel` est
appelée après chaque rapport de progression et arrête le run si elle rend vrai.

## 6. Compatibilité

Tant que le wheel est en version `0.x`, une signature peut changer d'une
version mineure à l'autre ; `quant-modeling` épingle donc une version exacte.
Un changement de signature est annoncé dans l'issue de la PR, avec l'issue
jumelle côté `quant-modeling`.
