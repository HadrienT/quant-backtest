# Contrat entre quant-backtest et quant-modeling

Ce document dit **qui possède quoi** et **sous quelle forme les deux dépôts se
parlent**. Il est canonique ici ; le modifier est un ADR dans
[`../blueprint/decisions.md`](../blueprint/decisions.md), et se fait par deux
issues coordonnées (une par dépôt).

L'interface Python ne porte encore que l'identité du build : les §1 à §4 fixent
les règles qu'elle suit, le §5 est rempli lot par lot, à mesure que les
fonctions arrivent.

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

## 6. Compatibilité

Tant que le wheel est en version `0.x`, une signature peut changer d'une
version mineure à l'autre ; `quant-modeling` épingle donc une version exacte.
Un changement de signature est annoncé dans l'issue de la PR, avec l'issue
jumelle côté `quant-modeling`.
