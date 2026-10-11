# Backtest : ce que fait un desk quant, et ce qu'on construit ici

| | |
|---|---|
| **Dépend de** | la lib de pricing `quantModeling` de [`quant-modeling`](https://github.com/HadrienT/quant-modeling) (pricers, grecques, calibrations, pour le backtest de couverture du §11), sa piste d'audit et son replay ([WP 18](https://github.com/HadrienT/quant-modeling/blob/main/blueprint/wp/18-observability.md)), son portefeuille et sa VaR ([WP 09](https://github.com/HadrienT/quant-modeling/blob/main/blueprint/wp/09-portfolio-risk.md)), le dépôt `~/data-ingest` (§3) |
| **Débloque** | une page `/backtest` dont chaque chiffre est défendable ; un backtest de couverture qui met la lib de pricing à l'épreuve des données |
| **Branches** | une par lot : `feat/b0-skeleton`, `feat/b2-engine`, … (§15) |
| **Lectures principales** | López de Prado, *Advances in Financial Machine Learning*, Wiley 2018 (ch. 7, 11–14) ; Bacon, *Practical Portfolio Performance Measurement and Attribution*, 2ᵉ éd., Wiley 2008 ; Bergomi, *Stochastic Volatility Modeling*, CRC 2016 (ch. 1, pour le P&L de couverture) ; Christoffersen, *Elements of Financial Risk Management*, 2ᵉ éd., Academic Press 2012 (ch. 13, backtest des modèles de risque) |

Ce document a deux usages. C'est d'abord un **compagnon de lecture** : les §1
à §13 décrivent ce qu'on appelle « backtest » sur un desk, avec les formules et
les références. C'est ensuite une **spécification** : le §2 dit ce que faisait
l'ancienne page de `quant-modeling` et où elle s'écartait de la pratique, les
§14 à §17 disent ce qu'on construit dans ce dépôt, dans quel ordre, et comment
on le teste.

Il a été écrit dans `quant-modeling` (lot 24 de son blueprint) avant que ce
dépôt existe ; il est désormais **canonique ici**.

Les seuils réglementaires du §12 (feux tricolores de Bâle, test d'attribution
du P&L de FRTB) sont donnés de mémoire. **Avant de les coder, les revérifier
contre le texte primaire** cité au §19 et citer le paragraphe dans le code.

---

## Sommaire

0. [Notation](#0-notation)
1. [Le problème en une page](#1-le-problème-en-une-page)
2. [État des lieux : l'ancienne page](#2-état-des-lieux--lancienne-page)
3. [Les données : ce qu'on savait, à la date où on le savait](#3-les-données--ce-quon-savait-à-la-date-où-on-le-savait)
4. [Le moteur de simulation](#4-le-moteur-de-simulation)
5. [Coûts de transaction et capacité](#5-coûts-de-transaction-et-capacité)
6. [Construire le portefeuille](#6-construire-le-portefeuille)
7. [Mesurer la performance](#7-mesurer-la-performance)
8. [Un Sharpe est une estimation : erreur-type et tests](#8-un-sharpe-est-une-estimation--erreur-type-et-tests)
9. [Surajustement et essais multiples](#9-surajustement-et-essais-multiples)
10. [Attribution : d'où vient la performance](#10-attribution--doù-vient-la-performance)
11. [Le backtest d'un desk de dérivés : la couverture](#11-le-backtest-dun-desk-de-dérivés--la-couverture)
12. [Le backtest des modèles de risque](#12-le-backtest-des-modèles-de-risque)
13. [Du backtest à la production](#13-du-backtest-à-la-production)
14. [Architecture](#14-architecture)
15. [Les lots](#15-les-lots)
16. [Tests de propriété](#16-tests-de-propriété)
17. [Données et limites assumées](#17-données-et-limites-assumées)
18. [Aide-mémoire](#18-aide-mémoire)
19. [Bibliographie](#19-bibliographie)
20. [Questions d'entretien](#20-questions-dentretien)

---

## 0. Notation

| Symbole | Sens |
|---|---|
| $t = 1, \dots, T$ | jours de bourse de la période testée ; $T$ est le nombre d'observations |
| $P_{i,t}$ | prix de clôture de l'actif $i$ ; $D_{i,t}$ son dividende détaché en $t$ |
| $R_{i,t} = (P_{i,t} + D_{i,t})/P_{i,t-1} - 1$ | rendement **arithmétique** total de l'actif $i$ |
| $w_{i,t}$ | poids de l'actif $i$ détenu **pendant** le jour $t$, décidé au plus tard en $t-1$ |
| $r_{f,t}$ | taux sans risque du jour $t$, celui **de l'époque** |
| $R_{p,t} = \sum_i w_{i,t} R_{i,t} - \text{coûts}_t$ | rendement net du portefeuille ; $R^e_{p,t} = R_{p,t} - r_{f,t}$ son excès |
| $\hat\mu$, $\hat\sigma$, $\hat\gamma_3$, $\hat\gamma_4$ | moyenne, écart-type, asymétrie et kurtosis (3 pour une gaussienne) de $R^e_p$ |
| $\widehat{SR} = \hat\mu / \hat\sigma$ | ratio de Sharpe **par période** ; annualisé : $\widehat{SR}\sqrt{252}$ |
| $N$ | nombre de configurations essayées avant de retenir celle qu'on montre |
| IS / OOS | *in-sample* (données ayant servi à choisir) / *out-of-sample* (données jamais vues au moment du choix) |

---

## 1. Le problème en une page

Un backtest répond à la question : « si j'avais appliqué cette règle dans le
passé, que se serait-il passé ? ». Ce n'est pas une expérience, c'est une
**simulation** : le passé n'a eu lieu qu'une fois, on ne peut pas le rejouer
avec d'autres tirages, et celui qui écrit la règle connaît déjà la suite.
Toute la discipline du métier vient de là. Un desk ne demande jamais « la
courbe monte-t-elle ? » ; il pose trois questions, dans cet ordre.

1. **Aurais-je eu cette information à cette date ?** Univers de titres, prix
   ajustés, comptes publiés, taux : chaque donnée doit être celle qui était
   connue le jour de la décision (§3).
2. **Aurais-je pu traiter à ce prix, pour cette taille ?** Délai entre le
   signal et l'exécution, écart acheteur-vendeur, impact, emprunt de titres
   (§4, §5).
3. **Combien de choses ai-je essayées avant celle-ci ?** Le meilleur de cent
   essais sur du bruit a un beau Sharpe. Sans le nombre d'essais, un Sharpe ne
   veut rien dire (§8, §9).

Le mot « backtest » recouvre quatre activités différentes selon le desk. Ce
document les couvre toutes, parce qu'un site de pricing de dérivés est concerné
par les quatre.

| Qui | Ce qui est testé | Ce qui est comparé | Section |
|---|---|---|---|
| Recherche quantitative (gestion systématique, *prop*) | une **stratégie** : une règle qui produit des positions | P&L simulé net de coûts contre un indice de référence et contre zéro | §3 à §10 |
| Desk de dérivés (*trading*, structuration) | un **modèle et sa couverture** | P&L d'une option couverte jour après jour : résidu, variance, explication par les grecques | §11 |
| Risque de marché | un **modèle de risque** (VaR, ES) | pertes réalisées contre pertes annoncées : nombre et regroupement des dépassements | §12 |
| Risque de contrepartie | un **modèle d'exposition** | valeurs réalisées contre distribution simulée | §12.4, déjà amorcé au [WP 23 de `quant-modeling`](https://github.com/HadrienT/quant-modeling/blob/main/blueprint/wp/23-xva.md) |

L'ancienne page faisait une version réduite de la première ligne. La deuxième
est celle où ce chantier a le plus à montrer : elle exige un pricer, des
grecques et une calibration, que la lib de pricing `quantModeling` possède
déjà.

---

## 2. État des lieux : l'ancienne page

> **Cette page n'est plus en ligne et son code n'est pas repris.** Elle a été
> masquée dans `quant-modeling` le 11 octobre 2026, et le moteur est réécrit
> ici en C++, de zéro. Cette section reste parce que ses quinze constats sont
> le **cahier de tests** du nouveau moteur : chacun devient un test que le lot
> correspondant doit faire passer (colonne « Constats » du §15). Les liens
> pointent sur le dernier commit de `quant-modeling` où ce code était servi.

[`api/app/routers/backtest.py`](https://github.com/HadrienT/quant-modeling/blob/6eab54c099c78d72763f9520bf38e1b1c6cc2eab/api/app/routers/backtest.py) prenait
une liste de titres, maximisait le ratio de Sharpe sur une fenêtre passée
(Markowitz, SLSQP, poids entre 0 et `max_share`), investissait le capital à la
fin de la fenêtre, suivait la valeur jusqu'à aujourd'hui et la comparait au
S&P 500. Avec `rebalance_freq > 0`, l'optimisation était refaite tous les $N$
jours. La page
([`web/src/features/backtest/`](https://github.com/HadrienT/quant-modeling/blob/6eab54c099c78d72763f9520bf38e1b1c6cc2eab/web/src/features/backtest/))
affichait la courbe, le drawdown, cinq métriques et une allocation.

Les écarts à la pratique, lus dans le code. Les sept premiers sont des erreurs
(le chiffre affiché est faux ou trompeur) ; les suivants sont des manques.

| # | Constat | Où | Conséquence |
|---|---|---|---|
| 1 | Le cache des prix est indexé par la liste de titres seule, sans la date de début | `_load_prices`, clé `",".join(all_tickers)` | Un premier appel depuis 2015 met en cache un historique qui commence en 2015 ; pendant trente minutes, un appel depuis 2005 sur les mêmes titres reçoit cet historique tronqué et optimise sur une autre fenêtre que celle affichée |
| 2 | Les prix manquants sont interpolés linéairement | `prices.interpolate(method="linear")` | Le prix d'un jour sans cotation est calculé avec le prix **suivant** : c'est une fuite du futur (§3.2), et cela lisse artificiellement la volatilité |
| 3 | Le taux sans risque est le dernier 10 ans connu (`GS10`), appliqué à tout le passé | `_fetch_risk_free_rate` | Un backtest sur 2015–2020 retranche le taux de 2026. Le taux doit être celui de l'époque, et court (3 mois), pas 10 ans (§7.2) |
| 4 | L'alpha renvoyé est **quotidien** et affiché à deux décimales | `_capm`, `coeffs[0]` ; `BacktestResults.tsx` | La tuile montre « 0.00 » quel que soit le résultat. Il faut l'annualiser et donner son erreur-type (§10.1) |
| 5 | Après retrait des poids sous `min_share`, les poids sont renormalisés sans revérifier `max_share` ; le succès de l'optimiseur n'est pas testé | `_filter_weights`, `_optimise_weights` | Un poids peut dépasser le plafond demandé. Si `n × max_share < 1` le problème n'a pas de solution et le résultat de SLSQP est pris tel quel |
| 6 | L'indice de référence `^GSPC` est un indice de **prix** ; les titres sont en clôture ajustée des dividendes | `db.prices_wide`, `_SP500_TICKER` | Le portefeuille encaisse ses dividendes, l'indice non : de l'ordre de 1,5 point par an d'avance gratuite. Il faut un indice dividendes réinvestis (§3.5) |
| 7 | Les titres proposés sont les membres **d'aujourd'hui** du S&P 500 (519 lignes, dont 361 cotées le 3 janvier 2000) | `data-ingest`, `data/tickers.csv` | Biais du survivant : on ne peut choisir que des sociétés dont on sait qu'elles ont réussi (§3.1) |
| 8 | Signal et exécution à la même clôture ; aucun coût | `_run_rebalanced_backtest` | L'optimisation utilise le rendement du jour $t$ et traite au prix de clôture de $t$. Défendable seulement avec un ordre à la clôture et un coût explicite (§4.2, §5) |
| 9 | Le rebalancement ne considère que les titres retenus à la première optimisation | `tickers=sorted_tickers` | Un titre écarté le premier jour ne revient jamais |
| 10 | Le Sharpe est calculé sur des rendements logarithmiques | `_compute_metrics` | La convention est le rendement arithmétique en excès (§7.2) ; l'écart est faible mais systématique |
| 11 | Aucune incertitude sur aucune métrique | toute la réponse | Un Sharpe de 0,8 sur trois ans a un intervalle à 95 % d'environ ±1,1 (§8.1) : il n'est pas distinguable de zéro, et rien ne le dit |
| 12 | Le « Sharpe optimal » (maximum in-sample) est renvoyé à côté du Sharpe réalisé | `optimal_sharpe` | C'est le maximum d'une fonction sur les données qui servent à la mesurer. Sa seule utilité est de montrer l'écart avec le réalisé (§6.1) |
| 13 | Aucun point de comparaison autre que l'indice | — | Sans le portefeuille équipondéré sur les mêmes titres, on ne sait pas si l'optimisation apporte quoi que ce soit (§6.3) |
| 14 | Une seule fenêtre d'optimisation, choisie par l'utilisateur | `opt_start`, `opt_end` | Le résultat dépend de deux dates. La pratique est la marche en avant sur fenêtre glissante (§9.2) |
| 15 | Le run n'est ni enregistré ni rejouable | `logger.info("backtest_run")` | Contraire au [WP 18](https://github.com/HadrienT/quant-modeling/blob/main/blueprint/wp/18-observability.md) : pas d'événement d'audit, pas de version des données |

À vérifier dans `data-ingest`, non contrôlé ici sur les données : la source
`sp500-prices` télécharge avec `yfinance` 1.7, dont le réglage par défaut
renvoie des clôtures **ajustées à la date du téléchargement**, et n'actualise
que les cinq derniers jours. Si c'est bien le cas, un dividende détaché après
le chargement initial n'ajuste pas les lignes déjà stockées : la série mélange
des prix ajustés à des dates différentes, et le rendement du jour de
détachement est faux. C'est la raison du §3.3 : stocker les prix bruts et les
événements, calculer l'ajustement à la lecture.

---

## 3. Les données : ce qu'on savait, à la date où on le savait

Sur un desk, la qualité d'un backtest est d'abord celle de sa base de données.
Le principe porte un nom : données ***point-in-time***. Chaque valeur a deux
dates — celle qu'elle décrit et celle à laquelle elle a été connue — et le
moteur ne lit, à la date $t$, que ce qui était connu en $t$.

### 3.1 Biais du survivant

Tester sur les membres actuels d'un indice, c'est exclure les faillites, les
sociétés rachetées et celles qui sont sorties de l'indice pour mauvaise
performance, et inclure dès 2005 des sociétés qui n'y sont entrées qu'en 2015
parce qu'elles avaient beaucoup monté (Brown, Goetzmann, Ibbotson & Ross
1992). Il faut deux choses.

- **L'historique de composition** : pour chaque date, la liste des membres ce
  jour-là. L'univers investissable en $t$ est la composition en $t$.
- **Les titres disparus**, avec leur dernier cours et leur **rendement de
  radiation** : ce que l'actionnaire a touché à la sortie de cote (prix de
  l'offre en cas de rachat, souvent presque rien en cas de faillite). Shumway
  (1997) montre que les bases qui omettent ce dernier rendement surestiment
  nettement les performances ; pour les radiations liées à la performance, il
  est de l'ordre de −30 %.

Les desks achètent ces données (CRSP, Compustat, fournisseurs d'indices).
Notre situation est décrite au §17.

### 3.2 Regarder dans le futur

Le *look-ahead* est toute utilisation, à la date $t$, d'une information
postérieure. Les formes courantes :

| Forme | Exemple | Parade |
|---|---|---|
| Décalage oublié | le signal calculé avec la clôture de $t$ est traité à la clôture de $t$ | décaler les poids d'un jour, ou modéliser explicitement un ordre à la clôture (§4.2) |
| Remplissage de trous | interpolation entre le prix d'avant et celui d'après | reporter la dernière valeur connue, jamais interpoler ; marquer le jour comme non traitable |
| Statistiques sur tout l'échantillon | centrer et réduire un signal avec la moyenne de toute la période ; choisir un seuil sur toute la période | toute statistique est calculée sur une fenêtre qui se termine en $t$ |
| Comptes publiés | utiliser le bénéfice du 4ᵉ trimestre au 31 décembre, alors que le 10-K sort en février | dater chaque fait par sa **date de dépôt**, pas par la fin de période |
| Données révisées | PIB, inflation, comptes retraités : la valeur lue aujourd'hui n'est pas celle publiée à l'époque | garder les millésimes (*vintages*) |
| Univers | choisir « les valeurs liquides » avec la liquidité d'aujourd'hui | critères de sélection évalués en $t$ |
| Paramètres | un taux, une volatilité ou une notation d'aujourd'hui appliqués au passé | séries historiques, lues à la date |

### 3.3 Opérations sur titres et prix ajustés

Une division du nominal (*split*) ou un dividende crée une discontinuité dans
le prix sans que l'actionnaire gagne ou perde quoi que ce soit. Deux façons de
la traiter.

- **Série ajustée** : tous les prix passés sont multipliés par un facteur pour
  que la série soit continue. Commode pour calculer des rendements. Deux
  défauts : la série **change à chaque nouvel événement** (un backtest relancé
  six mois plus tard ne lit plus les mêmes nombres), et les niveaux de prix
  passés sont fictifs (un titre « à 0,30 \$ » en 1995 qui cotait 40 \$), ce qui
  fausse tout ce qui dépend du niveau : nombre de titres entiers, coût par
  action, filtres sur le prix.
- **Prix bruts et table d'événements** : on stocke le prix réellement coté,
  et à part les dividendes (date de détachement, montant) et les divisions
  (date, ratio). Le moteur tient une position en **nombre de titres**, crédite
  le dividende en espèces à la date de détachement et multiplie la position à
  la division. C'est ce que fait un moteur de desk, et c'est reproductible.

Le rendement total $R_{i,t}$ de la notation est calculé à partir des prix
bruts et des événements, à la lecture.

### 3.4 Calendrier, horaires, cours figés

- Le calendrier est celui de la place de cotation (jours fériés, demi-séances).
  Un jour sans cotation n'est pas un rendement nul à interpoler.
- Des marchés qui ferment à des heures différentes ne sont pas synchrones :
  corréler la clôture de Tokyo et celle de New York du même jour civil mélange
  deux instants séparés de quatorze heures. On décale, ou on passe en
  rendements hebdomadaires.
- Un titre peu traité garde le même cours plusieurs jours : sa volatilité et
  ses corrélations sont sous-estimées, et il paraît diversifiant. On filtre
  sur le volume et sur la proportion de rendements nuls.
- Plusieurs devises : convertir avec le taux de change **du jour**, à la même
  heure de fixing que le prix.

### 3.5 L'indice de référence

On compare ce qui est comparable : un portefeuille qui encaisse ses dividendes
se compare à un indice **dividendes réinvestis** (*total return*), ou à un
fonds indiciel coté dont la clôture ajustée inclut les dividendes. Un bon
indice de référence est investissable, connu à l'avance, et de même univers que
la stratégie.

### 3.6 Contrôles de qualité avant tout calcul

Rendements au-delà de quelques dizaines d'écarts-types (souvent une division
non ajustée), prix nuls ou négatifs, volumes nuls, séries qui s'arrêtent,
doublons, dates hors calendrier. Un desk fait tourner ces contrôles à
l'ingestion et refuse de lancer un backtest sur une série qui échoue. Ici, le
rapport de qualité accompagne le résultat (§15, lots B1 et B2).

---

## 4. Le moteur de simulation

### 4.1 Deux architectures

| | Vectorisé | Événementiel |
|---|---|---|
| Principe | matrice de poids × matrice de rendements, décalage d'un jour | boucle sur le temps : à chaque date, lire les données connues, décider, passer des ordres, les exécuter, tenir la comptabilité |
| Force | rapide, quelques lignes, idéal pour explorer | ne peut pas regarder dans le futur par construction ; gère les ordres, les exécutions partielles, les opérations sur titres ; **le même code de stratégie tourne en production** |
| Faiblesse | une erreur de décalage passe inaperçue ; coûts et contraintes approximatifs | plus lent, plus de code |
| Usage sur un desk | recherche exploratoire | validation avant mise en production |

La pratique : explorer en vectorisé, confirmer en événementiel, et exiger que
les deux donnent le même P&L sur une stratégie simple (c'est un test, §16).

### 4.2 La chronologie d'une journée

C'est l'endroit où se glissent la plupart des erreurs. Le moteur doit rendre
explicite, pour chaque décision, **quelles données elle a vues** et **à quel
prix elle est exécutée**.

```
clôture t-1 ──► calcul du signal ──► poids cibles ──► ordres ──► exécution ──► position tenue pendant t
   (données ≤ t-1)                                          (ouverture t, ou clôture t,
                                                              ou VWAP de t, au choix déclaré)
```

Trois conventions acceptées, à déclarer dans le résultat :

- signal à la clôture de $t-1$, exécution à l'**ouverture** de $t$ ;
- signal à la clôture de $t-1$, exécution à la **clôture** de $t$ (prudent :
  un jour entier de retard) ;
- signal à la clôture de $t$, exécution à la **même clôture**, par ordre à la
  clôture — admissible seulement si le signal n'utilise que des données
  disponibles quelques minutes avant, et avec un coût d'exécution.

Un contrôle classique : relancer avec un jour de retard supplémentaire. Si la
performance s'effondre, la stratégie dépendait de l'instant d'exécution, donc
d'une hypothèse qu'on ne tiendra pas en réel.

### 4.3 La comptabilité

Le moteur tient un livre, pas une série de poids :

- **positions en nombre de titres** et **solde d'espèces** ; la valeur du
  portefeuille est $V_t = \text{espèces}_t + \sum_i n_{i,t} P_{i,t}$ ;
- les espèces portent intérêt au taux court de l'époque ; un solde négatif
  (levier) coûte ce taux plus une marge ;
- une position vendeuse reçoit le produit de la vente, paie les dividendes
  détachés et un **coût d'emprunt** du titre ;
- les dividendes sont crédités à la date de détachement (ou de paiement si on
  veut être exact) ; les divisions multiplient $n_i$ ;
- un titre radié est soldé à son rendement de radiation ;
- chaque ordre laisse une ligne : date, titre, quantité, prix, coût. Le
  journal des ordres fait partie du résultat.

Entre deux rebalancements, **les poids dérivent** avec les prix : ce sont les
quantités qui sont fixes, pas les poids. Le rendement d'un portefeuille
« 60/40 » rebalancé chaque mois n'est pas $0{,}6 R_1 + 0{,}4 R_2$ chaque jour.

### 4.4 Rebalancement

- **Calendaire** : fin de mois, de trimestre. Simple, mais sensible à la date
  choisie ; on vérifie en décalant le jour de rebalancement.
- **Par bandes** : on ne traite que si un poids s'écarte de sa cible de plus
  d'un seuil. Réduit la rotation à performance comparable.
- **Rotation** (*turnover*) d'un rebalancement : $\tfrac12 \sum_i |w_i^{\text{cible}} - w_i^{\text{avant}}|$.
  Annualisée, c'est le premier chiffre qu'on regarde après le Sharpe : une
  rotation de 10 par an avec 10 points de base par transaction coûte un point
  de rendement par an.

### 4.5 Contraintes

Bornes par titre, par secteur, levier brut et net, neutralité au marché
($\sum_i w_i \beta_i = 0$), limite de rotation, taille en fraction du volume
quotidien, lots entiers. Elles s'appliquent **dans** l'optimisation. Les
appliquer après coup en renormalisant, comme le constat 5 du §2, casse les
autres contraintes.

---

## 5. Coûts de transaction et capacité

Un backtest sans coûts mesure une autre stratégie que celle qu'on traiterait.
Novy-Marx & Velikov (2016) montrent que la plupart des anomalies publiées à
forte rotation ne survivent pas à leurs coûts.

### 5.1 Les composantes

| Composante | Ordre de grandeur (grandes capitalisations américaines) | Modèle |
|---|---|---|
| Courtage et frais | fraction de point de base à quelques points de base | proportionnel au montant, ou fixe par action |
| Demi-écart acheteur-vendeur | 1 à 5 points de base ; beaucoup plus sur les petites valeurs | $\tfrac12 s_{i,t} \times$ montant traité |
| Impact de marché | nul pour un particulier, dominant pour un fonds | loi en racine carrée, ci-dessous |
| Emprunt de titres (positions vendeuses) | 0,25 à 0,5 % par an pour un titre facile, plusieurs dizaines de % pour un titre difficile | taux annuel × valeur empruntée |
| Financement du levier | taux court plus une marge | sur le solde d'espèces négatif |
| Fiscalité, taxes sur transactions | selon la place | proportionnel |

**Impact en racine carrée.** Pour un ordre de $Q$ titres sur un titre de
volume quotidien $V$ et de volatilité quotidienne $\sigma_d$, le déplacement
moyen du prix est

$$\frac{\Delta P}{P} \approx Y \,\sigma_d \sqrt{\frac{Q}{V}}, \qquad Y \text{ de l'ordre de } 1$$

(Almgren, Thum, Hauptmann & Li 2005 ; Tóth et al. 2011). Le coût croît donc
comme la puissance 3/2 de la taille : c'est ce qui limite la **capacité** d'une
stratégie, c'est-à-dire l'encours au-delà duquel les coûts mangent le
rendement. Almgren & Chriss (2000) donnent le cadre pour étaler un ordre dans
le temps.

**Sans carnet d'ordres**, l'écart acheteur-vendeur s'estime à partir des plus
hauts, plus bas et clôtures quotidiens : Corwin & Schultz (2012), Abdi &
Ranaldo (2017). Notre base contient ces champs.

> **Limite mesurée au lot B2.** Ces deux estimateurs sont faits pour des écarts
> larges. Quand l'écart est petit devant la volatilité quotidienne, l'estimation
> n'est que du bruit, de l'ordre de 0,4 fois cette volatilité pour Corwin &
> Schultz et de 0,2 fois pour Abdi & Ranaldo sur une fenêtre d'un mois. Sur une
> grande capitalisation simulée (écart vrai de 3 points de base, volatilité de
> 1,5 % par jour), ils rendent environ 60 et 30 points de base : dix à vingt
> fois trop. Ils retrouvent l'écart à 5 % près quand il vaut trois fois la
> volatilité quotidienne. Sur les titres du S&P 500 l'estimation est donc une
> **borne supérieure**, pas un coût : le coût par défaut de la page doit être
> un coût proportionnel déclaré, et l'écart estimé réservé aux titres peu
> liquides ou à l'analyse de sensibilité du §5.2. Le test
> `SpreadEstimators.OverstateANarrowSpread` garde ce chiffre sous les yeux.

### 5.2 Ce qu'on rapporte

- Performance **brute et nette**, côte à côte.
- Rotation annualisée et coût total en points de base par an.
- **Coût d'équilibre** : le coût par transaction qui ramène le rendement en
  excès à zéro. S'il est de 8 points de base et que le coût réaliste est de 5,
  la stratégie ne tient qu'à un fil.
- Sensibilité : le Sharpe net en fonction du coût supposé (une courbe, pas un
  point).
- Part du volume quotidien traitée, en maximum et en médiane.

---

## 6. Construire le portefeuille

### 6.1 Pourquoi le Sharpe maximal in-sample est fragile

L'optimisation moyenne-variance prend en entrée des rendements espérés
$\hat\mu$ et une matrice de covariance $\hat\Sigma$ estimés sur le passé.

- **La moyenne ne s'estime pas.** L'erreur-type d'un rendement annuel moyen
  est $\sigma/\sqrt{\text{années}}$, quelle que soit la fréquence d'observation
  (Merton 1980). Un titre de volatilité 25 % observé cinq ans : ±11 points par
  an. Les différences de $\hat\mu$ entre titres sont du bruit.
- **L'optimiseur maximise l'erreur.** Il surpondère les titres dont le
  rendement est surestimé et la variance sous-estimée (Michaud 1989). Le
  résultat se concentre sur deux ou trois lignes, d'où le besoin d'un plafond
  comme `max_share`, qui devient alors le vrai paramètre de la stratégie.
- **Le Sharpe in-sample est biaisé vers le haut** : c'est un maximum pris sur
  les données mêmes qui le mesurent. L'écart entre Sharpe in-sample et Sharpe
  réalisé est une mesure du surajustement, pas une déception.
- DeMiguel, Garlappi & Uppal (2009) : sur quatorze modèles d'optimisation et
  sept jeux de données, aucun ne bat régulièrement le portefeuille
  équipondéré hors échantillon ; il faudrait de l'ordre de 3 000 mois
  d'historique pour que l'optimisation l'emporte avec 25 actifs.

### 6.2 Ce qu'on utilise à la place

| Méthode | Idée | Ce qu'elle évite |
|---|---|---|
| Équipondéré ($1/N$) | aucun paramètre | toute erreur d'estimation |
| Variance minimale | n'utilise que $\hat\Sigma$ | l'estimation de $\hat\mu$ |
| Covariance contractée (Ledoit & Wolf 2004) | $\hat\Sigma$ tirée vers une cible structurée, intensité optimale en formule fermée | une matrice mal conditionnée quand $N$ approche $T$ |
| Parité de risque (Maillard, Roncalli & Teïletche 2010) | chaque titre contribue autant à la variance | la concentration |
| Cible de volatilité (Moreira & Muir 2017) | levier inversement proportionnel à la volatilité récente | les régimes de forte volatilité |
| Black-Litterman | rendements d'équilibre du marché, corrigés par des vues | des $\hat\mu$ historiques |
| Rendements espérés issus d'un **signal** | $\hat\mu_i$ proportionnel à un score (valeur, momentum, qualité…) | de confondre « estimer » et « prévoir » |

La dernière ligne est le cas général d'un fonds systématique : la recherche
porte sur le signal, mesuré par son **coefficient d'information** (corrélation
de rang entre le score en $t$ et le rendement de $t+1$), et la loi fondamentale
de Grinold relie le tout : $IR \approx IC \times \sqrt{\text{nombre de paris indépendants}}$.

### 6.3 Les points de comparaison obligatoires

Toute stratégie est affichée avec, sur les mêmes titres, les mêmes dates et
les mêmes coûts : le portefeuille équipondéré rebalancé à la même fréquence,
l'achat-conservation des poids initiaux, et l'indice de référence dividendes
réinvestis. Une optimisation qui ne bat pas $1/N$ n'a rien montré.

---

## 7. Mesurer la performance

### 7.1 Les métriques

| Métrique | Définition | Remarque |
|---|---|---|
| Rendement annualisé (CAGR) | $(V_T/V_0)^{252/T} - 1$ | géométrique |
| Volatilité annualisée | $\hat\sigma(R_p)\sqrt{252}$ | sur rendements arithmétiques |
| Ratio de Sharpe | $\hat\mu(R^e_p)/\hat\sigma(R^e_p) \times \sqrt{252}$ | Sharpe 1966, 1994 |
| Ratio de Sortino | $\hat\mu(R^e_p) / \sqrt{\tfrac1T\sum_t \min(R^e_{p,t},0)^2}$, annualisé | ne pénalise que la baisse (Sortino & Price 1994) |
| Drawdown maximal | $\min_t \left(V_t / \max_{s\le t} V_s - 1\right)$ | dépend de la longueur de la période |
| Durée sous l'eau | plus longue période sans nouveau plus haut | souvent plus parlante que la profondeur |
| Ratio de Calmar | CAGR / \|drawdown maximal\| | |
| Asymétrie, kurtosis | $\hat\gamma_3$, $\hat\gamma_4$ | une stratégie vendeuse de volatilité a un beau Sharpe et une asymétrie négative |
| VaR et ES historiques | quantile à 1 % ou 5 % des rendements, et moyenne au-delà | |
| Bêta, alpha | régression de $R^e_p$ sur $R^e_m$ ; alpha annualisé = intercept × 252 | avec erreur-type (§10.1) |
| Erreur de suivi (*tracking error*) | $\hat\sigma(R_p - R_b)\sqrt{252}$ | |
| Ratio d'information | $\hat\mu(R_p - R_b)/\hat\sigma(R_p - R_b)\times\sqrt{252}$ | le Sharpe du pari actif |
| Rotation, coût annuel | §4.4, §5.2 | |
| Taux de réussite, gain moyen / perte moyenne | par jour ou par position | à lire ensemble |

On ajoute les rendements par année civile et par sous-période : une stratégie
dont tout le gain vient de 2020 n'est pas la même que celle qui gagne chaque
année.

### 7.2 Conventions à écrire sous chaque chiffre

- **Rendements arithmétiques** pour moyennes, volatilités et Sharpe (ils
  s'agrègent entre titres : le rendement d'un portefeuille est la moyenne
  pondérée des rendements arithmétiques, pas des rendements logarithmiques) ;
  géométrique pour le CAGR.
- **Excès de rendement jour par jour**, avec le taux court de l'époque (bon du
  Trésor à 3 mois), pas un taux unique ni un taux long.
- **Annualisation en $\sqrt{252}$** : exacte seulement si les rendements ne
  sont pas autocorrélés. Sinon le facteur est
  $q/\sqrt{q + 2\sum_{k=1}^{q-1}(q-k)\rho_k}$ avec $q = 252$ (Lo 2002) ; une
  autocorrélation positive, typique des actifs peu liquides ou lissés, gonfle
  le Sharpe annualisé naïf.
- Fréquence des observations et nombre d'observations $T$ affichés.

---

## 8. Un Sharpe est une estimation : erreur-type et tests

### 8.1 Erreur-type

Pour des rendements indépendants, l'erreur-type du Sharpe par période est
(Lo 2002, corrigé de la non-normalité par Mertens 2002) :

$$\text{SE}(\widehat{SR}) \approx \sqrt{\frac{1 - \hat\gamma_3\,\widehat{SR} + \frac{\hat\gamma_4 - 1}{4}\,\widehat{SR}^2}{T}}$$

Sous normalité cela se réduit à $\sqrt{(1 + \widehat{SR}^2/2)/T}$. En unités
annuelles, l'erreur-type vaut à peu près $1/\sqrt{\text{années}}$ : **±0,58 sur
trois ans, ±0,32 sur dix ans**. Un Sharpe annualisé de 0,8 mesuré sur trois ans
a un intervalle à 95 % d'environ [−0,3 ; 1,9].

De là, la statistique $t$ de la performance : $t \approx \widehat{SR}_{\text{annuel}} \times \sqrt{\text{années}}$.
Un Sharpe de 1 demande quatre ans pour atteindre $t = 2$, neuf ans pour
$t = 3$.

### 8.2 Probabilistic Sharpe Ratio

La probabilité que le vrai Sharpe dépasse un seuil $SR^*$ (Bailey & López de
Prado 2012) :

$$PSR(SR^*) = \Phi\!\left(\frac{(\widehat{SR} - SR^*)\sqrt{T-1}}{\sqrt{1 - \hat\gamma_3\,\widehat{SR} + \frac{\hat\gamma_4 - 1}{4}\,\widehat{SR}^2}}\right)$$

et la longueur d'historique minimale pour l'affirmer au niveau $\alpha$ :

$$MinTRL = 1 + \left[1 - \hat\gamma_3\,\widehat{SR} + \tfrac{\hat\gamma_4 - 1}{4}\,\widehat{SR}^2\right]\left(\frac{z_\alpha}{\widehat{SR} - SR^*}\right)^2$$

Une asymétrie négative et des queues épaisses allongent l'historique requis :
c'est la traduction chiffrée de « cette stratégie ramasse des pièces devant un
rouleau compresseur ».

### 8.3 Bootstrap

Quand les rendements sont autocorrélés ou que la métrique n'a pas de formule
(drawdown maximal, Calmar), on rééchantillonne **par blocs** pour conserver la
dépendance temporelle : bootstrap stationnaire de Politis & Romano (1994), à
longueur de bloc aléatoire. On en tire un intervalle de confiance pour chaque
métrique et la distribution du drawdown maximal — ce qui répond à la question
qu'un gérant pose vraiment : « le drawdown que je vis est-il compatible avec
le backtest ? ».

### 8.4 Comparer deux stratégies

Deux Sharpe mesurés sur la même période sont corrélés ; on ne compare pas
leurs intervalles séparément. Le test est celui de Jobson & Korkie (1981),
dans sa version robuste aux queues épaisses et à l'autocorrélation de Ledoit &
Wolf (2008), par bootstrap de la **différence**. C'est le test à faire entre
l'optimisé et l'équipondéré.

---

## 9. Surajustement et essais multiples

### 9.1 Le mécanisme

Parmi $N$ stratégies sans aucune valeur, la meilleure a un Sharpe estimé dont
l'espérance croît avec $N$. Pour des essais indépendants de vrai Sharpe nul et
d'écart-type $\sigma_{SR}$ entre essais (Bailey & López de Prado 2014) :

$$\mathbb{E}\big[\max_n \widehat{SR}_n\big] \approx \sigma_{SR}\left[(1-\gamma)\,\Phi^{-1}\!\left(1 - \tfrac1N\right) + \gamma\,\Phi^{-1}\!\left(1 - \tfrac{1}{Ne}\right)\right]$$

avec $\gamma \approx 0{,}5772$ la constante d'Euler-Mascheroni. Application :
avec cinq ans de données ($\sigma_{SR} \approx 1/\sqrt5$ en annuel), **45
essais indépendants suffisent** pour qu'on s'attende à trouver un Sharpe
annualisé de 1 dans du bruit pur (Bailey, Borwein, López de Prado & Zhu 2014).
Changer une fenêtre, un seuil, une liste de titres, une fréquence de
rebalancement : chacun est un essai.

### 9.2 Séparer ce qui choisit de ce qui mesure

| Méthode | Principe | Limite |
|---|---|---|
| Une coupure IS / OOS | choisir sur le début, mesurer sur la fin | un seul chemin ; on finit par regarder l'OOS plusieurs fois, et il cesse d'en être un |
| **Marche en avant** (*walk-forward*) | à chaque date de rebalancement, estimer sur la fenêtre qui se termine là (glissante ou croissante), appliquer jusqu'à la suivante ; concaténer les périodes d'application | un seul chemin historique ; les premières années ne sont jamais testées |
| Validation croisée **purgée** avec **embargo** | $k$ blocs ; on retire de l'apprentissage les observations dont la fenêtre chevauche le bloc de test, plus une marge après lui | nécessaire dès que les étiquettes couvrent plusieurs jours (López de Prado 2018, ch. 7) |
| Validation croisée **combinatoire** purgée (CPCV) | toutes les combinaisons de $k$ blocs de test parmi $n$ ; reconstitue plusieurs chemins hors échantillon | donne une **distribution** de Sharpe hors échantillon au lieu d'un nombre (ch. 12) |
| Réserve finale | une période mise de côté, regardée une fois, à la fin | la seule vraie mesure ; elle ne sert qu'une fois |

La marche en avant est le minimum : c'est la seule lecture honnête d'un
« rebalancement avec ré-optimisation ». La fenêtre d'estimation et la fréquence
sont des paramètres, donc des essais.

### 9.3 Corriger le Sharpe du nombre d'essais

- **Deflated Sharpe Ratio** (Bailey & López de Prado 2014) : le $PSR$ du §8.2
  évalué au seuil $SR^* = \mathbb{E}[\max_n \widehat{SR}_n]$ du §9.1, avec le
  nombre d'essais $N$ et la variance des Sharpe entre essais. C'est la
  probabilité que le Sharpe retenu soit positif **sachant qu'on en a essayé
  $N$**.
- **Seuil de $t$ relevé** : Harvey, Liu & Zhu (2016) recensent plusieurs
  centaines de facteurs publiés et concluent qu'un nouveau facteur devrait
  passer $t > 3$, pas $t > 2$. Harvey & Liu (2015) en tirent une décote du
  Sharpe.
- **Reality Check** de White (2000) et test **SPA** de Hansen (2005) : par
  bootstrap, la probabilité que la meilleure de $N$ stratégies batte la
  référence par chance.
- **Probabilité de surajustement** (PBO, Bailey, Borwein, López de Prado &
  Zhu 2017), par validation croisée combinatoire symétrique : on découpe la
  matrice (dates × configurations) en $S$ blocs ; pour chaque partage en deux
  moitiés, on prend la meilleure configuration sur une moitié et on regarde
  son rang sur l'autre. La PBO est la fréquence à laquelle le champion
  in-sample finit sous la médiane hors échantillon.

### 9.4 Le protocole de recherche

Ce qui distingue le plus un desk d'un amateur n'est pas une formule, c'est une
procédure (Arnott, Harvey & Markowitz 2019) :

1. **L'hypothèse avant les données** : une raison économique pour que l'effet
   existe (une prime de risque, une contrainte d'autres acteurs, un biais
   documenté), écrite avant le premier calcul.
2. **Un journal des essais** : chaque configuration lancée est enregistrée,
   avec ses paramètres et son résultat, y compris celles qu'on abandonne. Le
   $N$ du §9.3 se lit dans ce journal.
3. **Robustesse** : le résultat tient-il si on change légèrement chaque
   paramètre, la période, l'univers, le jour de rebalancement ? Une surface de
   performance en plateau rassure ; un pic isolé inquiète.
4. **Sous-périodes et régimes** : crises, hausses de taux, forte et faible
   volatilité.
5. **Données simulées** : faire tourner la chaîne sur des rendements sans
   aucun signal. Si elle « trouve » une stratégie, elle fuit.
6. **Relecture par un tiers** qui cherche l'erreur.

Et une attente réaliste : McLean & Pontiff (2016) mesurent qu'un effet publié
rend en moyenne 26 % de moins hors échantillon et 58 % de moins après
publication. Un desk applique une décote au Sharpe du backtest avant de
dimensionner une position.

---

## 10. Attribution : d'où vient la performance

### 10.1 Régression factorielle

$$R^e_{p,t} = \alpha + \sum_k \beta_k F_{k,t} + \varepsilon_t$$

avec pour facteurs le marché seul (CAPM), les trois ou cinq facteurs de Fama &
French (1993, 2015), le momentum (Carhart 1997). L'**alpha** est ce que les
facteurs n'expliquent pas ; il s'affiche annualisé, avec sa statistique $t$
calculée par l'estimateur de Newey & West (1987), robuste à l'autocorrélation
et à l'hétéroscédasticité. Un Sharpe élevé avec un alpha nul veut dire que la
stratégie est une exposition à des primes connues, achetable pour quelques
points de base dans un fonds indiciel factoriel.

On regarde aussi les bêtas **glissants** : une stratégie dont le bêta de
marché passe de 0,3 à 1,2 selon l'année n'a pas le profil que sa moyenne
suggère.

### 10.2 Attribution de Brinson

Pour un portefeuille géré contre un indice, par secteur $s$ (Brinson, Hood &
Beebower 1986 ; Brinson & Fachler 1985) :

- **allocation** : $(w^p_s - w^b_s)(R^b_s - R^b)$ — avoir surpondéré les bons
  secteurs ;
- **sélection** : $w^b_s (R^p_s - R^b_s)$ — avoir choisi les bons titres dans
  chaque secteur ;
- **interaction** : $(w^p_s - w^b_s)(R^p_s - R^b_s)$.

### 10.3 Décomposition du risque

Contribution de chaque position à la volatilité, $w_i (\Sigma w)_i / \sqrt{w^\top \Sigma w}$,
dont la somme redonne la volatilité totale ; part du risque expliquée par les
facteurs, part spécifique. La même décomposition sert pour la VaR.

---

## 11. Le backtest d'un desk de dérivés : la couverture

Sur un desk d'options, la question n'est pas « ce modèle redonne-t-il les prix
d'aujourd'hui ? » — tous les modèles calibrés le font — mais « **si je couvre
avec ce modèle, que reste-t-il dans mon P&L ?** ». Le backtest de couverture y
répond, et c'est le test qui départage les modèles de volatilité.

### 11.1 La formule fondamentale

On achète une option au prix de volatilité implicite $\sigma_i$ et on la couvre
en delta, en continu, avec le delta de Black-Scholes à cette même volatilité.
Si le sous-jacent réalise une volatilité instantanée $\sigma_t$, le P&L final
est (El Karoui, Jeanblanc & Shreve 1998 ; Carr & Madan 1998 ; Bergomi 2016,
ch. 1) :

$$\text{P\&L}_T = \int_0^T e^{r(T-t)}\,\tfrac12\,\Gamma_i(t, S_t)\,S_t^2\,\big(\sigma_t^2 - \sigma_i^2\big)\,dt$$

Tout le métier est dans cette ligne.

- Le P&L est l'écart entre variance réalisée et variance implicite, **pondéré
  par le gamma en dollars** $\Gamma S^2$ le long du chemin. Deux chemins de même
  volatilité réalisée donnent des P&L différents selon l'endroit où le gamma
  était fort (près du strike, près de l'échéance).
- Couvrir à la volatilité implicite donne un P&L qui a un signe sûr si
  $\sigma_t > \sigma_i$ partout, mais un montant qui dépend du chemin. Couvrir
  à la vraie volatilité (si on la connaissait) donne un montant final certain
  et une valeur de marché qui fluctue en route (Ahmad & Wilmott 2005).
- Un mauvais modèle ne fait pas perdre de l'argent en moyenne à lui seul : il
  **ajoute de la variance** au P&L et le rend dépendant de facteurs qu'on
  croyait couverts.

### 11.2 Ce que le temps discret ajoute

Avec $N$ réajustements au lieu d'une couverture continue, même si la
volatilité est exactement celle du modèle, le P&L a un écart-type (Derman &
Kamal 1999) :

$$\sigma_{\text{P\&L}} \approx \sqrt{\frac{\pi}{4}}\;\frac{\partial C}{\partial \sigma}\;\frac{\sigma}{\sqrt N}$$

soit, pour une option à la monnaie, environ $0{,}89/\sqrt N$ du prix : 19 % du
prix pour un mois couvert chaque jour ($N = 21$), et encore 5,6 % pour un an
couvert chaque jour ($N = 252$). C'est le **bruit de fond** de tout backtest de couverture :
une différence entre deux modèles n'a de sens que rapportée à lui, et sa
décroissance en $1/\sqrt N$ est une propriété testable (§16).

Les coûts de transaction s'y ajoutent. Leland (1985) : couvrir tous les
$\Delta t$ avec un coût proportionnel aller-retour $k$ revient à valoriser
avec une volatilité modifiée

$$\hat\sigma^2 = \sigma^2\left(1 \pm \sqrt{\frac{2}{\pi}}\,\frac{k}{\sigma\sqrt{\Delta t}}\right)$$

(plus pour une position vendeuse d'option, moins pour une position acheteuse).
Réajuster plus souvent réduit le bruit et augmente les coûts : la fréquence de
couverture est un arbitrage, et le backtest en trace la frontière.

### 11.3 Le protocole

Pour chaque jour $t$ de l'historique, pour chaque option du panier suivi :

1. **Calibrer** le modèle sur la surface de volatilité du jour $t$ (données
   de $t$ seulement).
2. **Valoriser** l'option et calculer ses grecques avec ce modèle.
3. **Tenir la couverture** décidée en $t$ jusqu'en $t+1$ : sous-jacent pour le
   delta, éventuellement une ou deux options liquides pour le vega et le gamma.
4. En $t+1$, **constater** : variation du prix de marché de l'option,
   variation de la couverture, portage et financement. La différence est
   l'**erreur de couverture** du jour.
5. Recalibrer, recommencer.

Ce qu'on rapporte, par modèle et par tranche de moneyness et de maturité :
moyenne de l'erreur (un biais est une prime de risque encaissée ou payée,
§11.5), **écart-type** (le critère principal), queues, autocorrélation, et
l'erreur rapportée au bruit de discrétisation du §11.2.

Les deux articles fondateurs suivent ce protocole. Bakshi, Cao & Chen (1997)
comparent Black-Scholes, volatilité stochastique, sauts et taux stochastiques
sur trois critères : cohérence des paramètres, erreur de prix hors échantillon
(calibrer en $t$, valoriser en $t+1$), erreur de couverture. Dumas, Fleming &
Whaley (1998) montrent qu'une volatilité locale recalibrée chaque semaine
couvre **moins bien** qu'un Black-Scholes à volatilité implicite lissée : un
modèle qui colle parfaitement à la surface du jour peut se tromper sur sa
dynamique.

### 11.4 Quel delta ?

Le delta dépend d'une hypothèse sur ce que fait la surface quand le
sous-jacent bouge (Derman 1999) : volatilité collée au strike (*sticky
strike*, le delta de Black-Scholes), collée à la moneyness (*sticky delta*),
ou volatilité locale (la volatilité à la monnaie baisse quand le spot monte,
plus vite que le skew ne le dit). Sur les indices actions, où spot et
volatilité sont négativement corrélés, le delta de Black-Scholes d'un call est
trop élevé. Hull & White (2017) estiment le **delta de variance minimale**
directement sur les données :

$$\delta_{MV} = \delta_{BS} + \frac{\nu_{BS}}{S\sqrt T}\,\big(a + b\,\delta_{BS} + c\,\delta_{BS}^2\big)$$

et mesurent le gain en réduction de variance de l'erreur de couverture par
rapport au delta de Black-Scholes. C'est le point de comparaison à battre pour
un modèle de volatilité stochastique ou locale.

### 11.5 Variante en monde simulé

Hull & Suo (2002) séparent le risque de modèle du bruit des données : on
**simule** le marché avec un modèle riche (par exemple Heston ou rough
Bergomi calibré), on valorise et on couvre avec un modèle plus simple
recalibré chaque jour sur les prix simulés, et on mesure l'erreur. Deux
intérêts ici : cela ne demande **aucun historique d'options**, et la vérité
est connue, ce qui en fait un test de la chaîne elle-même.

### 11.6 Backtest de stratégies de volatilité

Vendre une option et la couvrir en delta, c'est encaisser $\sigma_i^2 - \sigma_t^2$
pondéré par le gamma : la **prime de risque de variance**. Elle est positive
en moyenne sur les indices actions (Bakshi & Kapadia 2003 ; Carr & Wu 2009) —
la volatilité implicite dépasse en moyenne la réalisée — avec une asymétrie
très négative. Tester ce type de stratégie demande tout ce qui précède, plus :
des prix d'options **traitables** (acheter à l'offre, vendre à la demande ;
les écarts sont larges et dominent le résultat), le roulement des échéances,
la marge exigée, et la lecture du §8.2 sur l'asymétrie. Broadie, Chernov &
Johannes (2009) montrent que les rendements spectaculaires des ventes de puts
ne sont pas anormaux une fois rapportés à leur levier et à la taille des
échantillons.

### 11.7 Explication du P&L

Chaque jour, un desk décompose le P&L de son livre :

$$\Delta V \approx \underbrace{\Delta\,\delta S}_{\text{delta}} + \underbrace{\tfrac12\Gamma\,\delta S^2}_{\text{gamma}} + \underbrace{\Theta\,\delta t}_{\text{theta}} + \underbrace{\mathcal V\,\delta\sigma}_{\text{vega}} + \underbrace{\rho\,\delta r}_{\text{taux}} + \text{termes croisés (vanna, volga)} + \text{nouvelles opérations} + \textbf{inexpliqué}$$

L'**inexpliqué** est le P&L réel moins le P&L prédit par les grecques de la
veille. C'est le backtest quotidien du modèle et des grecques : un inexpliqué
qui grossit signale un facteur de risque manquant, une grecque fausse ou une
donnée de marché douteuse. Le contrôle des produits vérifie cette décomposition
tous les matins ; FRTB en a fait un test réglementaire (§12.3).

### 11.8 Stabilité de la calibration

Série quotidienne des paramètres calibrés : un modèle dont les paramètres
sautent d'un jour à l'autre, ou restent collés à leurs bornes, se recalibre
pour absorber ce qu'il ne décrit pas, et ses grecques ne valent rien entre
deux calibrations (c'est le symptôme de l'issue
[quant-modeling#146](https://github.com/HadrienT/quant-modeling/issues/146)). On suit l'erreur de
calibration, l'erreur de prix du lendemain à paramètres figés, et la
volatilité des paramètres.

---

## 12. Le backtest des modèles de risque

### 12.1 VaR : compter les dépassements

Un modèle de VaR à 99 % sur un jour annonce chaque soir une perte qui ne
devrait être dépassée qu'un jour sur cent. Soit $I_t = 1$ si la perte du jour
$t$ dépasse la VaR annoncée la veille, $x = \sum_t I_t$ sur $T$ jours, et
$p = 1\,\%$.

- **Couverture inconditionnelle** (Kupiec 1995) : le nombre de dépassements
  est-il compatible avec $p$ ?

  $$LR_{uc} = -2\ln\!\left[\frac{(1-p)^{T-x}\,p^{x}}{(1-x/T)^{T-x}\,(x/T)^{x}}\right] \sim \chi^2(1)$$

- **Indépendance** (Christoffersen 1998) : les dépassements arrivent-ils en
  grappes ? Avec $n_{ij}$ le nombre de jours dans l'état $j$ précédés de l'état
  $i$, $\pi_{01} = n_{01}/(n_{00}+n_{01})$, $\pi_{11} = n_{11}/(n_{10}+n_{11})$
  et $\pi = (n_{01}+n_{11})/T$ :

  $$LR_{ind} = -2\ln\!\left[\frac{(1-\pi)^{n_{00}+n_{10}}\,\pi^{n_{01}+n_{11}}}{(1-\pi_{01})^{n_{00}}\,\pi_{01}^{n_{01}}\,(1-\pi_{11})^{n_{10}}\,\pi_{11}^{n_{11}}}\right] \sim \chi^2(1)$$

- **Couverture conditionnelle** : $LR_{cc} = LR_{uc} + LR_{ind} \sim \chi^2(2)$.

Un modèle peut avoir le bon nombre de dépassements et être mauvais : s'ils
tombent tous la même quinzaine, il ne réagit pas aux changements de régime.
Avec 250 jours et $p = 1\,\%$ on attend 2,5 dépassements : ces tests ont peu de
puissance, d'où l'intérêt de tester aussi à 95 % et de regarder toute la
distribution (§12.4).

**Deux P&L.** Le P&L **hypothétique** est celui du portefeuille de la veille,
figé, revalorisé aux prix du jour ; le P&L **réel** inclut les opérations de
la journée et les commissions. Le modèle se juge sur le premier : c'est lui
que la VaR prétend prévoir.

### 12.2 Les feux tricolores de Bâle

Le superviseur compte les dépassements de la VaR à 99 % un jour sur les 250
derniers jours ouvrés (Comité de Bâle 1996) et majore le capital en
conséquence. *Chiffres à revérifier avant de coder.*

| Zone | Dépassements | Majoration du multiplicateur (Bâle 2,5 : base 3) | Multiplicateur FRTB (base 1,5) |
|---|---|---|---|
| Verte | 0 à 4 | 0 | 1,50 |
| Orange | 5 | +0,40 | 1,70 |
| | 6 | +0,50 | 1,76 |
| | 7 | +0,65 | 1,83 |
| | 8 | +0,75 | 1,88 |
| | 9 | +0,85 | 1,92 |
| Rouge | 10 et plus | +1,00 | 2,00 |

Sous FRTB, le backtest se fait aussi **par desk** : un desk perd l'usage du
modèle interne au-delà de 12 dépassements à 99 % ou de 30 à 97,5 % sur douze
mois.

### 12.3 Le test d'attribution du P&L (FRTB)

Par desk, on compare chaque jour le P&L hypothétique (calculé par les
systèmes du front office, avec tous les facteurs de risque) au P&L
**théorique du risque** (calculé par le modèle de risque, avec ses facteurs
simplifiés), sur 250 jours, par deux statistiques : la corrélation de rang de
Spearman et la distance de Kolmogorov-Smirnov entre les deux distributions.
*Seuils à revérifier.*

| Zone | Corrélation de Spearman | Distance de Kolmogorov-Smirnov |
|---|---|---|
| Verte | > 0,80 | < 0,09 |
| Orange | entre les deux | entre les deux |
| Rouge | < 0,70 | > 0,12 |

C'est la version réglementaire de l'inexpliqué du §11.7 : le modèle de risque
voit-il le même P&L que le desk ?

### 12.4 Expected shortfall et densités

FRTB remplace la VaR par l'ES à 97,5 %. L'ES n'est pas *élicitable* : il
n'existe pas de fonction de score dont il soit le minimiseur (Gneiting 2011),
ce qui a fait croire qu'il ne se backtestait pas. Il se backteste, conjointement
avec la VaR (Fissler & Ziegel 2016). Acerbi & Szekely (2014) proposent, avec
$X_t$ le P&L, $I_t$ l'indicatrice de dépassement de la VaR et $\alpha = 2{,}5\,\%$ :

$$Z_2 = \sum_{t=1}^{T} \frac{X_t\,I_t}{T\,\alpha\,ES_t} + 1$$

d'espérance nulle si le modèle est juste, négative s'il sous-estime le risque ;
les seuils critiques s'obtiennent par simulation sous le modèle.

Plus général : tester **toute la distribution prédite**. Si $F_t$ est la
fonction de répartition annoncée, $u_t = F_t(X_t)$ doit être uniforme sur
[0, 1] et indépendant d'un jour à l'autre (Diebold, Gunther & Tay 1998) ;
Berkowitz (2001) teste $\Phi^{-1}(u_t)$ contre une gaussienne centrée réduite
sans autocorrélation, avec bien plus de puissance qu'un comptage de
dépassements. C'est aussi la méthode du backtest des modèles d'**exposition**
du risque de contrepartie (Comité de Bâle 2010 ; Anfuso, Karyampas & Nawroth),
à plusieurs horizons, avec la difficulté que des horizons qui se chevauchent
ne sont pas indépendants. Le [WP 23](https://github.com/HadrienT/quant-modeling/blob/main/blueprint/wp/23-xva.md) applique déjà l'idée à la
marge initiale dynamique.

---

## 13. Du backtest à la production

- **Même code** en recherche et en production : la stratégie est une fonction
  des données connues en $t$ ; seul le fournisseur de données et le passeur
  d'ordres changent. C'est l'argument décisif pour le moteur événementiel.
- **Reproductibilité** : un résultat est accompagné de la version du code, de
  la version (ou de l'empreinte) des données, des paramètres et de la graine.
  Relancer doit redonner les mêmes chiffres, au bit près. C'est le rôle de la
  piste d'audit de `quant-modeling` ([WP 18](https://github.com/HadrienT/quant-modeling/blob/main/blueprint/wp/18-observability.md)),
  à laquelle ce dépôt fournit sa version et l'empreinte des données reçues
  ([`contract.md`](contract.md) §4).
- **Marche à blanc** (*paper trading*) : la stratégie tourne en réel sans
  capital pendant quelques mois. C'est le seul hors-échantillon que personne
  n'a pu regarder.
- **Suivi réel contre backtest** : on compare en continu le P&L réalisé à la
  distribution annoncée (§8.3), et on fixe à l'avance le drawdown ou l'écart
  à partir duquel on coupe.
- **Gouvernance** : sous SR 11-7, un modèle qui engage du capital est validé
  par une équipe indépendante de celle qui l'a écrit, qui refait le backtest
  avec ses propres outils et documente hypothèses et limites.

---

## 14. Architecture

Le moteur est une **lib C++20 autonome**, dans ce dépôt, exposée à Python par
un wheel pybind11 `quantbacktest`. Le site reste dans `quant-modeling` : sa
page `/backtest` et un routeur FastAPI mince, qui lit les données en base,
appelle le wheel et émet l'événement d'audit. Les raisons de ce découpage et
les alternatives écartées sont dans
[`blueprint/decisions.md`](../blueprint/decisions.md) ; la frontière exacte
entre les deux dépôts est dans [`contract.md`](contract.md).

Pourquoi du C++ : un run isolé sur des données quotidiennes est petit, mais la
validation le répète des centaines ou des milliers de fois (marche en avant
avec ré-optimisation sur une grille de paramètres, CPCV, PBO, bootstrap), et le
backtest de couverture en monde simulé enchaîne chemins × dates ×
recalibrations autour d'une lib de pricing qui est déjà en C++. Mesuré au lot
B2 sur 500 titres et 26 ans : 81 ms pour un run rebalancé chaque mois, 0,97 s
rebalancé chaque jour, 4,5 ms pour le calcul vectorisé équivalent ; le détail
et ce qui n'est pas mesuré (la comparaison avec Python) sont dans l'ADR-002.

```
include/quantBacktest/
  core/          types, calendrier, séries, résultats, progression et annulation
  data/          vues sur les tableaux reçus : prix bruts, événements, univers à date, taux à date
  quality/       contrôles du §3.6 ; un rapport, et un refus si la série est inutilisable
  engine/        moteur comptable du §4.3 : positions, espèces, ordres, journal
  costs/         modèles de coût du §5
  construction/  1/N, variance minimale, Sharpe maximal, parité de risque, contraction
  metrics/       §7 : une fonction par métrique, convention dans le commentaire
  inference/     §8 : erreurs-types, PSR, bootstrap stationnaire, test de différence
  validation/    §9 : marche en avant, CPCV, DSR, PBO
  attribution/   §10
  hedging/       §11 : boucle calibrer → valoriser → couvrir → constater
  risk_models/   §12 : Kupiec, Christoffersen, feux tricolores
src/             implémentations, même découpage
bindings/python/ pybind11 → wheel `quantbacktest`
tests/           GoogleTest, un fichier par module, tests de propriété du §16
benchmarks/      google-benchmark
```

Règles d'architecture, sans exception :

- **Le moteur ne lit aucune base et aucun fichier.** Il reçoit des tableaux
  (prix bruts, événements, univers à date, taux à date) et rend des tableaux.
  Le SQL est dans `quant-modeling`, l'ingestion dans `data-ingest`. Une donnée
  absente ou trop ancienne est une erreur explicite, jamais un repli ni une
  interpolation.
- **Une seule direction de dépendance** : ce dépôt connaît `quantModeling` ;
  `quantModeling` ne connaît pas ce dépôt. Seul `hedging/` inclut ses en-têtes.
  Tous les autres modules se construisent et se testent sans lui.
- **La stratégie est séparée du moteur** : une stratégie est une fonction des
  données connues en $t$ qui rend des poids cibles ou des ordres. Le moteur ne
  branche sur aucune stratégie, comme un engine de `quantModeling` ne branche
  sur aucun produit.
- **L'interface Python ne porte que des données simples** : tableaux numpy,
  nombres, chaînes, dictionnaires. Aucun objet C++ de `quantmodeling` ne
  traverse vers `quantbacktest` : les deux wheels lient chacun leur copie de
  la lib, leurs types ne sont pas interchangeables.
- **Déterminisme** : mêmes entrées et même graine, mêmes sorties au bit près,
  quel que soit le nombre de threads. C'est la condition du replay.
- **Les appels longs rendent la main** : ils relâchent le GIL, rapportent leur
  progression et s'arrêtent sur demande. La gestion de la tâche (file,
  identifiant, annulation par l'utilisateur) est dans l'API.
- **Chiffres affichés** : chaque métrique rendue porte sa convention et son
  incertitude. Une métrique qu'on ne sait pas encadrer n'est pas rendue.

---

## 15. Les lots

Dans l'ordre. Chaque lot a son issue GitHub, dans le dépôt indiqué. La colonne
« Constats » renvoie aux numéros du §2 : le lot livre un test par constat.

| Lot | Dépôt | Contenu | Constats | Critère d'acceptation |
|---|---|---|---|---|
| **B0 — Socle** | ici | CMake, presets, vcpkg, GoogleTest, clang-format 18, pre-commit, CI, squelette du wheel pybind11 ; `quantModeling` tiré à un commit épinglé (ADR-003), après avoir vérifié qu'il se laisse consommer comme sous-projet | — | `scripts/make.sh` construit et passe un test ; le wheel s'importe ; un test valorise un call avec `quantModeling` ; la CI est verte |
| **B1 — Données** | `data-ingest` | Prix bruts + table des dividendes et divisions ; indice dividendes réinvestis ; facteurs de Fama-French (bibliothèque de données de K. French) ; historique de composition du S&P 500 ; contrôles de qualité à l'ingestion. Vérifier d'abord l'hypothèse sur l'ajustement des clôtures (fin du §2) | 6, 7 | Le rendement total reconstruit à partir des prix bruts et des événements égale celui d'une série ajustée fraîchement téléchargée, à la précision d'arrondi près. L'univers à une date passée ne contient aucun titre entré dans l'indice après cette date |
| **B2 — Moteur comptable et coûts** | ici | `core/`, `data/`, `quality/`, `engine/`, `costs/` : positions en titres, espèces, dividendes et divisions, chronologie déclarée du §4.2, journal des ordres, coûts proportionnels et écart estimé sur plus haut / plus bas. Premier benchmark | 2, 8 | Tests de propriété du §16 (conservation, pas de futur, coûts nuls ⇒ égalité avec le calcul vectorisé, achat-conservation ⇒ zéro ordre après le premier jour, invariance aux divisions) |
| **B3 — Métriques et inférence** | ici | `metrics/`, `inference/`, régression factorielle de `attribution/` (alpha annualisé, erreur-type de Newey-West) ; chaque métrique rendue avec son intervalle | 3, 4, 10, 11 | Sur des rendements gaussiens simulés de Sharpe connu, l'intervalle à 95 % contient la vraie valeur dans 95 % ± 2 % des tirages |
| **B4 — Construction et points de comparaison** | ici | $1/N$, variance minimale, Sharpe maximal, contraction de Ledoit-Wolf, parité de risque, contraintes tenues **dans** l'optimiseur et échec explicite si le problème est infaisable ; équipondéré et achat-conservation rendus d'office ; test de différence de Sharpe. Le solveur est à choisir par un ADR (il n'y a pas de SLSQP en C++ dans nos dépendances) | 5, 9, 12, 13 | Parité de risque : contributions égales à 1e-6 près. Variance minimale : variance inférieure ou égale à celle de tout autre portefeuille admissible tiré au hasard. Aucun poids hors de ses bornes |
| **B5 — Validation** | ici | Marche en avant à fenêtre glissante ou croissante ; CPCV ; Sharpe déflaté ; PBO sur la grille de paramètres explorée | 14 | Sur des rendements sans signal, la PBO est proche de 0,5 et le Sharpe déflaté du meilleur essai est sous 0,95 dans au moins 95 % des tirages |
| **B6 — Backtest de couverture** | ici | `hedging/`, d'abord en monde simulé (§11.5 : Heston ou rough Bergomi comme vérité, Black-Scholes, volatilité locale et le modèle vrai comme couvertures), puis sur l'historique de surfaces dès qu'il est assez long (§17) ; delta de Black-Scholes contre delta de variance minimale ; décomposition du P&L du §11.7 | — | Monde Black-Scholes : écart-type de l'erreur en $1/\sqrt N$, pente mesurée en log-log à ±0,1 ; moyenne du P&L égale à l'intégrale du §11.1 quand implicite ≠ réalisée. La couverture par le modèle vrai a l'écart-type le plus faible |
| **B7 — Backtest des modèles de risque** | ici | `risk_models/` : Kupiec, Christoffersen, zone du feu tricolore, sur des séries de P&L et de VaR fournies par l'appelant | — | Sur une série simulée dont la VaR est exacte, taux de rejet de Kupiec à 5 % égal à 5 % ± 2 %. Sur une VaR sous-estimée de moitié, rejet quasi systématique |
| **B8 — Branchement et page** | `quant-modeling` | Le wheel dans l'image et la CI ; lecture point-in-time en base ; routeur mince ; tâche avec progression et annulation ; événement d'audit `backtest.run` et son replay ; série de P&L hypothétique et de VaR du [WP 09](https://github.com/HadrienT/quant-modeling/blob/main/blueprint/wp/09-portfolio-risk.md) pour B7 ; page `/backtest` refaite et remise en ligne : onglets stratégie / couverture / modèle de risque, brut et net, intervalles sur chaque tuile, points de comparaison, rapport de qualité des données, marche en avant visible sur la courbe. Peut commencer dès B3, un onglet à la fois | 1, 15 | Deux requêtes successives avec des dates de début différentes ne partagent pas leurs prix. Un run rejoué redonne les mêmes chiffres. Aucun chiffre sans convention ni incertitude |

Le **journal des essais** du §9.4 est la piste d'audit de `quant-modeling` : le
$N$ du Sharpe déflaté se compte dans les événements `backtest.run`. Ce dépôt
ne tient aucun journal ; il calcule le Sharpe déflaté pour le $N$ et la
variance entre essais qu'on lui donne.

---

## 16. Tests de propriété

- **Conservation** : chaque jour, valeur = espèces + positions ; variation de
  valeur = P&L des positions + dividendes + intérêts − coûts, à 1e-9 près.
- **Pas de futur** : modifier les données postérieures à $t$ ne change aucune
  décision ni aucune valeur jusqu'à $t$ (on tronque ou on bruite la fin de la
  série et on compare le début).
- **Coûts nuls** : le moteur événementiel et le calcul vectorisé donnent le
  même P&L.
- **Monotonie des coûts** : la valeur finale décroît quand le coût unitaire
  croît.
- **Invariance aux divisions** : diviser le nominal d'un titre par deux à une
  date quelconque ne change pas la courbe de valeur.
- **Achat-conservation** : aucun ordre après le premier jour ; valeur finale
  égale à la somme des quantités initiales valorisées, dividendes compris.
- **Actif sans risque seul** : Sharpe nul (et non infini), drawdown nul.
- **Invariance d'échelle** : doubler le capital double la valeur et ne change
  aucun ratio ; doubler le levier (financé au taux sans risque) ne change pas
  le Sharpe.
- **Couverture** : pente en $-1/2$ de l'erreur en fonction du nombre de
  réajustements ; P&L moyen nul quand la volatilité de couverture est la
  vraie ; signe du P&L donné par le signe de $\sigma_t^2 - \sigma_i^2$.
- **Tests statistiques** : taille empirique des tests (Kupiec, différence de
  Sharpe) conforme à leur niveau sur données simulées sous l'hypothèse nulle.

---

## 17. Données et limites assumées

État de la base au 9 octobre 2026.

| Besoin | Ce qu'on a | Ce qui manque | Décision |
|---|---|---|---|
| Prix quotidiens | `prices.sp500_daily`, plus haut / plus bas / clôture / volume depuis 2000, 519 titres | prix bruts et événements séparés (§3.3) | lot B1 |
| Univers historique | la liste d'aujourd'hui | la composition à chaque date, et surtout les cours des titres radiés, qui ne sont pas disponibles gratuitement | Ingérer l'historique des entrées et sorties ; **le biais du survivant ne peut pas être entièrement retiré sans base payante**. La page le dit, et propose par défaut un univers qui n'en souffre pas : fonds indiciels cotés larges et sectoriels |
| Taux sans risque historique | `DGS3MO` depuis 1981, `DTB3` depuis 1954, `SOFR` depuis 2018 ; `macro.fred_series` est versionnée, donc déjà point-in-time | rien | lu par l'API et passé au moteur (lot B8) |
| Indice dividendes réinvestis | `^GSPC` (prix seulement) | l'indice *total return*, ou la clôture ajustée d'un fonds indiciel | lot B1 |
| Facteurs | rien | Fama-French, momentum | lot B1 |
| Comptes des sociétés | faits XBRL des 10-K / 10-Q, avec la date de dépôt | rien pour un signal fondamental point-in-time | disponible si un lot futur en a besoin |
| Surfaces de volatilité historiques | `options.chain_snapshot` : 12 sous-jacents, **14 jours** (depuis le 12 septembre 2026). Yahoo ne donne pas de chaînes passées | de la profondeur ; elle ne s'achète pas gratuitement, elle s'accumule | B6 commence en monde simulé ; la version sur données réelles attend quelques mois d'historique. Ne pas interrompre l'ingestion quotidienne |
| Volatilité implicite longue | `VIXCLS` depuis 1990 | — | permet dès maintenant une étude de la prime de variance sur l'indice (VIX² contre variance réalisée à 30 jours), en disant que le VIX n'est pas un prix traitable |
| Écarts acheteur-vendeur | rien sur les actions ; offre et demande dans les chaînes d'options | carnet d'ordres | estimateurs sur plus haut / plus bas (§5.1) |
| Emprunt de titres | rien | taux d'emprunt par titre | positions vendeuses hors périmètre, ou coût forfaitaire déclaré |

Hors périmètre : données intrajournalières, modélisation d'un carnet d'ordres,
exécution réelle. Et la règle de la roadmap de `quant-modeling` tient : ce
chantier n'ajoute aucun produit à son catalogue.

---

## 18. Aide-mémoire

| À savoir | Valeur |
|---|---|
| Erreur-type d'un Sharpe annualisé | $\approx 1/\sqrt{\text{années}}$ : 0,58 sur 3 ans, 0,32 sur 10 ans |
| Statistique $t$ d'une performance | Sharpe annualisé × $\sqrt{\text{années}}$ |
| Seuil de $t$ pour un nouveau facteur | 3 (Harvey, Liu & Zhu) |
| Essais suffisants pour un Sharpe de 1 dans du bruit, 5 ans de données | 45 |
| Dégradation moyenne d'un effet publié | −26 % hors échantillon, −58 % après publication |
| Erreur-type d'un rendement annuel moyen | $\sigma/\sqrt{\text{années}}$ : 9 points pour 20 % de volatilité sur 5 ans |
| Impact de marché | $\propto \sigma\sqrt{Q/V}$ ; coût total en puissance 3/2 de la taille |
| Bruit de couverture discrète, option à la monnaie | $\approx 0{,}89/\sqrt N$ du prix |
| P&L d'une option couverte en delta | $\int \tfrac12 \Gamma S^2(\sigma_t^2 - \sigma_i^2)\,dt$ |
| Feux tricolores, VaR 99 % sur 250 jours | vert jusqu'à 4 dépassements, rouge à partir de 10 |
| Test d'attribution du P&L, zone verte | Spearman > 0,80 et KS < 0,09 |
| ES de FRTB | 97,5 % |

---

## 19. Bibliographie

Les références marquées † sont celles dont un détail (revue, année, seuil) est
cité de mémoire et doit être revérifié avant d'être repris dans le code ou sur
la page.

**Méthode et surajustement**

| Référence | Section |
|---|---|
| López de Prado, *Advances in Financial Machine Learning*, Wiley, 2018 | §9 |
| Arnott, Harvey & Markowitz, « A Backtesting Protocol in the Era of Machine Learning », *Journal of Financial Data Science* 1(1), 2019 | §9.4 |
| Bailey, Borwein, López de Prado & Zhu, « Pseudo-Mathematics and Financial Charlatanism: The Effects of Backtest Overfitting on Out-of-Sample Performance », *Notices of the AMS* 61(5), 2014 | §9.1 |
| Bailey, Borwein, López de Prado & Zhu, « The Probability of Backtest Overfitting », *Journal of Computational Finance* 20(4), 2017 | §9.3 |
| Bailey & López de Prado, « The Sharpe Ratio Efficient Frontier », *Journal of Risk* 15(2), 2012 | §8.2 |
| Bailey & López de Prado, « The Deflated Sharpe Ratio: Correcting for Selection Bias, Backtest Overfitting and Non-Normality », *Journal of Portfolio Management* 40(5), 2014 | §9.1, §9.3 |
| Harvey, Liu & Zhu, « … and the Cross-Section of Expected Returns », *Review of Financial Studies* 29(1), 2016 | §9.3 |
| Harvey & Liu, « Backtesting », *Journal of Portfolio Management* 42(1), 2015 | §9.3 |
| White, « A Reality Check for Data Snooping », *Econometrica* 68(5), 2000 | §9.3 |
| Hansen, « A Test for Superior Predictive Ability », *Journal of Business & Economic Statistics* 23(4), 2005 | §9.3 |
| McLean & Pontiff, « Does Academic Research Destroy Stock Return Predictability? », *Journal of Finance* 71(1), 2016 | §9.4 |

**Performance et inférence**

| Référence | Section |
|---|---|
| Bacon, *Practical Portfolio Performance Measurement and Attribution*, 2ᵉ éd., Wiley, 2008 | §7, §10 |
| Sharpe, « Mutual Fund Performance », *Journal of Business* 39(1), 1966 ; « The Sharpe Ratio », *Journal of Portfolio Management* 21(1), 1994 | §7 |
| Sortino & Price, « Performance Measurement in a Downside Risk Framework », *Journal of Investing* 3(3), 1994 | §7 |
| Lo, « The Statistics of Sharpe Ratios », *Financial Analysts Journal* 58(4), 2002 | §7.2, §8.1 |
| Mertens, « Comments on Variance of the IID Estimator in Lo (2002) », note de travail, 2002 † | §8.1 |
| Jobson & Korkie, « Performance Hypothesis Testing with the Sharpe and Treynor Measures », *Journal of Finance* 36(4), 1981 | §8.4 |
| Ledoit & Wolf, « Robust Performance Hypothesis Testing with the Sharpe Ratio », *Journal of Empirical Finance* 15(5), 2008 | §8.4 |
| Politis & Romano, « The Stationary Bootstrap », *Journal of the American Statistical Association* 89(428), 1994 | §8.3 |
| Newey & West, « A Simple, Positive Semi-Definite, Heteroskedasticity and Autocorrelation Consistent Covariance Matrix », *Econometrica* 55(3), 1987 | §10.1 |
| Fama & French, « Common Risk Factors in the Returns on Stocks and Bonds », *Journal of Financial Economics* 33(1), 1993 ; « A Five-Factor Asset Pricing Model », *JFE* 116(1), 2015 | §10.1 |
| Carhart, « On Persistence in Mutual Fund Performance », *Journal of Finance* 52(1), 1997 | §10.1 |
| Brinson, Hood & Beebower, « Determinants of Portfolio Performance », *Financial Analysts Journal* 42(4), 1986 ; Brinson & Fachler, « Measuring Non-US Equity Portfolio Performance », *Journal of Portfolio Management* 11(3), 1985 | §10.2 |

**Données, coûts, construction**

| Référence | Section |
|---|---|
| Brown, Goetzmann, Ibbotson & Ross, « Survivorship Bias in Performance Studies », *Review of Financial Studies* 5(4), 1992 | §3.1 |
| Shumway, « The Delisting Bias in CRSP Data », *Journal of Finance* 52(1), 1997 | §3.1 |
| Almgren & Chriss, « Optimal Execution of Portfolio Transactions », *Journal of Risk* 3(2), 2000 | §5.1 |
| Almgren, Thum, Hauptmann & Li, « Direct Estimation of Equity Market Impact », *Risk*, 2005 | §5.1 |
| Tóth et al., « Anomalous Price Impact and the Critical Nature of Liquidity in Financial Markets », *Physical Review X* 1, 2011 | §5.1 |
| Corwin & Schultz, « A Simple Way to Estimate Bid-Ask Spreads from Daily High and Low Prices », *Journal of Finance* 67(2), 2012 | §5.1 |
| Abdi & Ranaldo, « A Simple Estimation of Bid-Ask Spreads from Daily Close, High, and Low Prices », *Review of Financial Studies* 30(12), 2017 | §5.1 |
| Novy-Marx & Velikov, « A Taxonomy of Anomalies and Their Trading Costs », *Review of Financial Studies* 29(1), 2016 | §5 |
| Markowitz, « Portfolio Selection », *Journal of Finance* 7(1), 1952 | §6 |
| Merton, « On Estimating the Expected Return on the Market », *Journal of Financial Economics* 8(4), 1980 | §6.1 |
| Michaud, « The Markowitz Optimization Enigma: Is "Optimized" Optimal? », *Financial Analysts Journal* 45(1), 1989 | §6.1 |
| DeMiguel, Garlappi & Uppal, « Optimal Versus Naive Diversification: How Inefficient Is the 1/N Portfolio Strategy? », *Review of Financial Studies* 22(5), 2009 | §6.1 |
| Ledoit & Wolf, « A Well-Conditioned Estimator for Large-Dimensional Covariance Matrices », *Journal of Multivariate Analysis* 88(2), 2004 | §6.2 |
| Maillard, Roncalli & Teïletche, « The Properties of Equally Weighted Risk Contribution Portfolios », *Journal of Portfolio Management* 36(4), 2010 | §6.2 |
| Moreira & Muir, « Volatility-Managed Portfolios », *Journal of Finance* 72(4), 2017 | §6.2 |
| Grinold & Kahn, *Active Portfolio Management*, 2ᵉ éd., McGraw-Hill, 1999 | §6.2 |

**Couverture et dérivés**

| Référence | Section |
|---|---|
| Bergomi, *Stochastic Volatility Modeling*, Chapman & Hall / CRC, 2016, ch. 1 | §11.1 |
| El Karoui, Jeanblanc-Picqué & Shreve, « Robustness of the Black and Scholes Formula », *Mathematical Finance* 8(2), 1998 | §11.1 |
| Carr & Madan, « Towards a Theory of Volatility Trading », dans *Volatility*, Risk Books, 1998 | §11.1 |
| Ahmad & Wilmott, « Which Free Lunch Would You Like Today, Sir? Delta Hedging, Volatility Arbitrage and Optimal Portfolios », *Wilmott Magazine*, 2005 | §11.1 |
| Derman & Kamal, « When You Cannot Hedge Continuously: The Corrections of Black-Scholes », Goldman Sachs Quantitative Strategies Research Notes, 1999 | §11.2 |
| Leland, « Option Pricing and Replication with Transactions Costs », *Journal of Finance* 40(5), 1985 | §11.2 |
| Bakshi, Cao & Chen, « Empirical Performance of Alternative Option Pricing Models », *Journal of Finance* 52(5), 1997 | §11.3 |
| Dumas, Fleming & Whaley, « Implied Volatility Functions: Empirical Tests », *Journal of Finance* 53(6), 1998 | §11.3 |
| Derman, « Regimes of Volatility », *Risk*, 1999 | §11.4 |
| Hull & White, « Optimal Delta Hedging for Options », *Journal of Banking & Finance* 82, 2017 | §11.4 |
| Hull & Suo, « A Methodology for Assessing Model Risk and Its Application to the Implied Volatility Function Model », *Journal of Financial and Quantitative Analysis* 37(2), 2002 | §11.5 |
| Bakshi & Kapadia, « Delta-Hedged Gains and the Negative Market Volatility Risk Premium », *Review of Financial Studies* 16(2), 2003 | §11.6 |
| Carr & Wu, « Variance Risk Premiums », *Review of Financial Studies* 22(3), 2009 | §11.6 |
| Broadie, Chernov & Johannes, « Understanding Index Option Returns », *Review of Financial Studies* 22(11), 2009 | §11.6 |

**Modèles de risque**

| Référence | Section |
|---|---|
| Christoffersen, *Elements of Financial Risk Management*, 2ᵉ éd., Academic Press, 2012 | §12 |
| Kupiec, « Techniques for Verifying the Accuracy of Risk Measurement Models », *Journal of Derivatives* 3(2), 1995 | §12.1 |
| Christoffersen, « Evaluating Interval Forecasts », *International Economic Review* 39(4), 1998 | §12.1 |
| Comité de Bâle, *Supervisory Framework for the Use of "Backtesting" in Conjunction with the Internal Models Approach to Market Risk Capital Requirements*, 1996 † | §12.2 |
| Cadre de Bâle, MAR32 (backtest et attribution du P&L par desk) et MAR33 (multiplicateur) † | §12.2, §12.3 |
| Acerbi & Szekely, « Backtesting Expected Shortfall », *Risk*, 2014 | §12.4 |
| Gneiting, « Making and Evaluating Point Forecasts », *Journal of the American Statistical Association* 106(494), 2011 | §12.4 |
| Fissler & Ziegel, « Higher Order Elicitability and Osband's Principle », *Annals of Statistics* 44(4), 2016 | §12.4 |
| Diebold, Gunther & Tay, « Evaluating Density Forecasts with Applications to Financial Risk Management », *International Economic Review* 39(4), 1998 | §12.4 |
| Berkowitz, « Testing Density Forecasts, with Applications to Risk Management », *Journal of Business & Economic Statistics* 19(4), 2001 | §12.4 |
| Comité de Bâle, *Sound Practices for Backtesting Counterparty Credit Risk Models* (BCBS 185), 2010 | §12.4 |
| Anfuso, Karyampas & Nawroth, « A Sound Basel III Compliant Framework for Backtesting Credit Exposure Models », 2014 † | §12.4 |
| Fed / OCC, *SR 11-7 — Supervisory Guidance on Model Risk Management*, 2011 | §13 |

---

## 20. Questions d'entretien

1. Votre backtest affiche un Sharpe de 1,5 sur quatre ans. Que demandez-vous
   avant d'y croire ? *(Nombre d'essais, coûts, univers à date, décalage
   signal-exécution, erreur-type ≈ 0,5, asymétrie.)*
2. Pourquoi le Sharpe réalisé d'un portefeuille optimisé est-il presque
   toujours sous son Sharpe in-sample ?
3. Quelle différence entre une validation croisée ordinaire et une validation
   purgée avec embargo, et pourquoi la première fuit-elle sur des séries
   financières ?
4. Vous couvrez en delta une option achetée à 20 % de volatilité implicite ; le
   sous-jacent réalise 25 %. Gagnez-vous de l'argent à coup sûr ? De combien ?
5. Deux modèles reproduisent la même surface de volatilité. Comment
   choisissez-vous ?
6. Un modèle de VaR à 99 % a eu trois dépassements en 250 jours, tous la même
   semaine. Est-il bon ?
7. Pourquoi dit-on que l'ES ne se backteste pas, et pourquoi est-ce faux ?
8. Qu'est-ce que l'inexpliqué d'un P&L, et que faites-vous quand il grossit ?
