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
| `total_returns(dates, close, *, dividends=None, splits=None)` | B8 (aide) | `dates` : `datetime64` croissantes ; `close` : clôtures **brutes**, une ligne par date, une colonne par titre ; `dividends`, `splits` : triplets comme pour `run_allocation` | tableau de même forme : rendement arithmétique total `s (P[t] + D[t]) / P[t-1] - 1`. Première ligne à `NaN`, ainsi que tout rendement dont une des deux clôtures manque |
| `period_rates(dates, annual_rate, *, basis)` | B8 (aide) | `annual_rate` : taux simple annualisé **en décimal** (0,05 pour 5 %), un par date ; `basis` : jours de l'année monétaire (360 en USD), sans valeur par défaut | tableau : taux simple de chaque période, `annual_rate[t-1] × jours entre t-1 et t / basis`, zéro en première position. C'est la forme attendue par `cash_rate` |
| `performance_report(returns, cash_rate, benchmark=None, *, bootstrap_replications, mean_block_length, seed, periods_per_year=252, confidence=0.95, tail=0.05, threads=1, progress=None, cancel=None)` | B3 | `returns` : rendements arithmétiques par période, nets de coûts ; `cash_rate` : taux sans risque simple de chaque période, aligné ; `benchmark` : rendements d'un indice dividendes réinvestis, aligné. Les trois arguments du bootstrap n'ont **pas de valeur par défaut** ; `bootstrap_replications=0` ne rend que ce qui a une erreur-type analytique | dictionnaire : `observations`, `periods_per_year`, `years` ; `metrics` (nom → `value`, `standard_error`, `lower`, `upper`, `confidence`, `method`, `convention`) ; `omitted` (nom → raison) ; `t_statistic`, `probabilistic_sharpe_ratio` (seuil zéro), `minimum_track_record_years`. Ratios annualisés en √`periods_per_year` ; `max_drawdown` négatif ou nul ; `value_at_risk` et `expected_shortfall` par période, en **pertes positives**. Une métrique qu'on ne sait pas encadrer est dans `omitted`, pas dans `metrics` |
| `factor_regression(excess, factors, *, newey_west_lags, periods_per_year=252, confidence=0.95)` | B3 | `excess` : rendements en excès par période ; `factors` : tableau à deux dimensions, une ligne par période, une colonne par facteur. `newey_west_lags` n'a **pas de valeur par défaut** (0 = covariance de White) | dictionnaire : `alpha` (**annualisé** : intercept × `periods_per_year`, même forme qu'une métrique), `betas` (liste, une entrée par facteur), `alpha_t_statistic`, `beta_t_statistics`, `r_squared`, `residual_volatility` (annualisée), `observations`, `newey_west_lags` |
| `optimal_block_length(series)` | B3 (règle) | une série, par exemple les rendements | nombre : longueur moyenne de bloc du bootstrap stationnaire selon Politis & White (2004), corrigée par Patton, Politis & White (2009). Optimale pour la **moyenne** de la série ; un point de départ pour les autres statistiques. À passer en `mean_block_length` |
| `newey_west_lag(observations)` | B3 (règle) | nombre d'observations | entier : `floor(4 (T/100)^(2/9))`, la règle fixe que les logiciels statistiques prennent par défaut. Une convention, pas une sélection sur les données. À passer en `newey_west_lags` |
| `var_backtest(pnl, value_at_risk, *, tail_probability)` | B7 | `pnl` : P&L signé de chaque jour, le P&L **hypothétique** ; `value_at_risk` : VaR annoncée pour ce jour, en **perte positive**, alignée ; `tail_probability` : 0,01 pour une VaR à 99 %, sans valeur par défaut | dictionnaire : `observations`, `exceptions`, `expected_exceptions`, `exception_days` (tableau de 0 et 1) ; `unconditional_coverage` (Kupiec), `independence` et `conditional_coverage` (Christoffersen), chacun avec `statistic`, `p_value`, `degrees_of_freedom`, à `NaN` quand l'échantillon ne permet pas le test ; `transitions` ((n00, n01), (n10, n11)) ; `traffic_light` : `zone` (`green`, `amber`, `red`), `cumulative_probability`, et, pour 250 jours à 99 % seulement (sinon `None`), `basel_plus_factor` et `frtb_multiplier` |
| `sharpe_difference(first, second, *, bootstrap_replications, mean_block_length, seed, periods_per_year=252, confidence=0.95, threads=1, progress=None, cancel=None)` | B4 | deux séries de rendements en excès sur les mêmes périodes | dictionnaire : `analytical` (différence des Sharpe annualisés, première moins seconde, erreur-type de Jobson-Korkie corrigée par Memmel, même forme qu'une métrique), `z_statistic`, `p_value` (bilatérale), `correlation` ; `bootstrap` (intervalle par centiles, même forme) et `bootstrap_p_value`, à `None` si `bootstrap_replications=0` |
| `run_allocation(dates, close, *, method, lookback, rebalance, initial_capital, proportional_cost, open=None, high=None, low=None, volume=None, universe=None, cash_rate=None, dividends=None, splits=None, covariance="ledoit_wolf", expanding=False, min_weight=0, max_weight=1, timing="next_close", extra_lag_days=0, start_index=0, allow_borrowing=False, allow_short=False, borrow_rate=None, whole_shares=False, spread_estimator=None, spread_window=21, spread_min_pairs=10, impact_coefficient=None, impact_window=21, periods_per_year=252, progress=None, cancel=None)` | B4 | `dates` : `datetime64`, croissantes ; `close`, `open`, `high`, `low`, `volume` : tables **brutes**, une ligne par date, une colonne par titre, `NaN` = pas de cotation ; `universe` : table booléenne d'appartenance ; `cash_rate` : taux simple de chaque période ; `dividends` : triplet (indices de titre, dates de détachement, montants par action) ; `splits` : triplet (indices, dates, ratios = nouvelles actions par ancienne). `method` : `equal_weight`, `minimum_variance`, `maximum_sharpe`, `risk_parity` ; `rebalance` : nombre de jours ou `"monthly"` ; `proportional_cost` : fraction du montant traité (0,0005 = 5 points de base) ; `timing` : `next_close`, `next_open`, `same_close` ; `spread_estimator` : `None`, `corwin_schultz`, `abdi_ranaldo` ; `expanding` : fenêtre d'estimation croissante au lieu de glissante ; `impact_coefficient` : `None`, ou le coefficient Y de l'impact en racine carrée (exige `volume`) ; `allow_short` : autorise les poids négatifs (`min_weight` < 0) et exige `borrow_rate`, le taux simple de chaque période payé sur les titres empruntés ; `whole_shares` : ordres arrondis vers zéro à un nombre entier de titres. Les cinq premiers mots-clés n'ont **pas de valeur par défaut** | dictionnaire : `strategy`, `equal_weight`, `buy_and_hold` et `gross` (la stratégie sans aucun coût, `None` si elle n'en avait pas), chacun avec `dates`, `value`, `cash`, `position_pnl`, `dividends`, `interest`, `costs`, `borrow_cost`, `traded_notional`, `turnover`, `stale_positions`, `shares` (jours × titres) et `journal` (`decision_date`, `execution_date`, `asset`, `requested`, `quantity`, `price`, `cost`, `status`) ; `allocations` (`dates`, `weights`, `eligible`, `in_sample_sharpe`, `shrinkage`) ; `cost_summary` : `annualized_turnover` (aller simple), `annual_cost_bps`, `break_even_cost` (une métrique : le coût proportionnel qui annule le rendement en excès, ou `None` sans transaction), `participation` (`executions`, `maximum`, `median` : part du montant quotidien traité prise par les exécutions, ou `None` sans `volume`) ; `conventions` (chaînes) ; `quality` (liste de `severity`, `asset`, `count`, `message`). Chaque jour : `value` = `cash` + titres × dernière clôture connue, et sa variation = `position_pnl` + `dividends` + `interest` − `costs` − `borrow_cost`. `in_sample_sharpe` est mesuré sur la fenêtre d'estimation : un maximum in-sample, pas une prévision |
| `run_allocation_grid(dates, close, *, grid, initial_capital, proportional_cost, ..., threads=1, progress=None, cancel=None)` | B5 (lanceur) | mêmes données, mêmes coûts et mêmes règles d'exécution que `run_allocation` ; `grid` : liste de dictionnaires, chacun avec `method`, `lookback`, `rebalance` et, en option, `covariance`, `expanding`, `min_weight`, `max_weight`. **Toute configuration essayée y figure** : le nombre de colonnes est le nombre d'essais | dictionnaire : `dates` ; `returns` (une ligne par date, une colonne par configuration, rendements nets quotidiens : c'est la table qu'attendent `walk_forward`, `combinatorial_purged_cv`, `deflated_sharpe` et `backtest_overfitting`) ; `descriptions` ; `final_values`. Une configuration qui échoue fait échouer la grille, et le message donne son rang |
| `hedging_backtest(world, *, strike, maturity, hedge, rebalances, paths, seed, is_call=True, hedge_volatility=None, coefficients=None, cost_rate=0, confidence=0.95, threads=1, progress=None, cancel=None)` | B6 | `world` : dictionnaire qui **nomme** le modèle suivi par le marché simulé et donne ses paramètres : `{"model": "black_scholes", "spot", "rate", "dividend_yield", "volatility"}` ou `{"model": "heston", "spot", "rate", "dividend_yield", "v0", "kappa", "theta", "xi", "rho"}` ; taux continus, par an ; `maturity` en années ; `hedge` : `black_scholes_fixed` (exige `hedge_volatility`), `black_scholes_implied`, `skew_adjusted`, `minimum_variance` (exige `coefficients`), `true_model` ; `rebalances` : nombre de réajustements, également espacés, sur la vie de l'option | dictionnaire, pour **une option achetée et couverte en delta**, P&L à l'échéance, en devise : `premium` ; `mean` et `standard_deviation` (même forme qu'une métrique, erreur-type de Monte-Carlo) ; `skewness`, `kurtosis`, `quantiles` ; `step_error_autocorrelation` ; `discretisation_noise` (plancher de Derman-Kamal) ; `gamma_integral` (pour `black_scholes_fixed`, sinon `None`) ; `explanation` (`delta`, `gamma`, `theta`, `vega`, `unexplained`, chacun une métrique) ; `pnl` (une valeur par chemin) ; `uncalibrated_steps` ; `convention` |
| `fit_minimum_variance_delta(world, *, strike, maturity, rebalances, paths, seed, is_call=True, threads=1, progress=None, cancel=None)` | B6 | mêmes arguments que ci-dessus | dictionnaire `a`, `b`, `c` : coefficients du delta de variance minimale de Hull & White (2017), à passer à `hedging_backtest(hedge="minimum_variance", coefficients=...)`. Estimer sur une graine, couvrir sur une autre |
| `walk_forward(returns, *, train, test, expanding=False, periods_per_year=252)` | B5 | `returns` : table à deux dimensions, une ligne par période, **une colonne par configuration essayée** (toutes, y compris celles qu'on a abandonnées) ; `train`, `test` : longueurs de la fenêtre qui choisit et de la période d'application, sans valeur par défaut | dictionnaire : `folds` (liste de `train_begin`, `train_end`, `test_begin`, `test_end`, `chosen`, `in_sample_sharpe`, `out_of_sample_sharpe`, Sharpe annualisés) ; `first_row` ; `returns` (rendements hors échantillon mis bout à bout, à partir de `first_row`) ; `chosen` (configuration tenue à chaque période) |
| `combinatorial_purged_cv(returns, *, groups, test_groups, purge, embargo, periods_per_year=252)` | B5 | même table ; `groups` blocs contigus dont `test_groups` de test par partage ; `purge` et `embargo` en périodes, sans valeur par défaut | dictionnaire : `splits`, `paths`, `path_sharpes` (Sharpe annualisé de chaque chemin hors échantillon reconstitué), `path_returns` (chemins × périodes), `in_sample_sharpes`, `out_of_sample_sharpes`, `chosen` (un par partage). Les chemins partagent leurs données : ce ne sont pas des tirages indépendants |
| `deflated_sharpe(returns, *, periods_per_year=252)` | B5 | même table ; chaque colonne compte pour un essai | dictionnaire : `best`, `sharpe`, `expected_maximum`, `sharpe_deviation` (annualisés), `trials`, `deflated_sharpe_ratio` (probabilité que le Sharpe retenu soit positif sachant le nombre d'essais) |
| `backtest_overfitting(returns, *, blocks, periods_per_year=252)` | B5 | même table ; `blocks` pair, au plus 20 | dictionnaire : `probability` (PBO), `splits`, `logits` (un par partage), `mean_in_sample_sharpe`, `mean_out_of_sample_sharpe` (annualisés). La PBO est une fréquence sur des partages qui se recouvrent : elle est rendue **sans erreur-type**, avec la distribution des logits |

Erreurs : une entrée inutilisable lève `quantbacktest.Error`, sous-classe de
`ValueError` ; un run arrêté par `cancel` lève `quantbacktest.Cancelled`,
sous-classe de `Error`. `progress` reçoit la fraction accomplie ; `cancel` est
appelée après chaque rapport de progression et arrête le run si elle rend vrai.

## 6. Compatibilité

Tant que le wheel est en version `0.x`, une signature peut changer d'une
version mineure à l'autre ; `quant-modeling` épingle donc une version exacte.
Un changement de signature est annoncé dans l'issue de la PR, avec l'issue
jumelle côté `quant-modeling`.
