# RAII cascade en Volt — réécriture modulaire du middle-end

## Context

Volt n'a pas de GC : le middle-end injecte les `finalize()` à la sortie de portée.
La mécanique existe (`InsertFinalizeCalls`, sixième post-walk sweep dans
`TypeChecker`) mais elle a grossi en un monolithe de **1654 lignes**
(`Sema/Private/Passes/TypeChecker/FinalizeLowering.cpp`), doublé d'une seconde
implantation de la même logique dans `Sema/Private/Layout/TypeBinder.cpp`
(`SynthesizeFinalizeStubs`, ~200 lignes). Elle ne modélise nulle part
l'**ownership** : chaque cas (alias-copy, move-out en tail, escape via
constructeur, variable de `rescue`, cascade de champ) est un garde ad-hoc ajouté
après coup, et les chemins de sortie ont trois mécanismes parallèles
(wrap `BeginExpr`, splice avant `return`, splice avant `break`/`next`).

Mesuré sur le build courant (`python3 scripts/valgrind_check.py`) : **50/57
PASSED, 5 fuites, 2 échecs de build**. Quatre causes racines distinctes :

| # | Cause | Preuve |
|---|---|---|
| 1 | **Temporaires rvalue jamais finalisés.** `CollectCandidates` ne parcourt que `Scope.Order`/`Bindings` : un résultat de `String#+`, `.trim`, `Array#map/filter`, ou un `ArrayLit` abaissé n'a aucun binding. | `FullOOP` (`String#+` dans `Button#render`), `Mixins` (`full_id`), `Composition` (`map`/`filter`/`trim`), `ForLoop`, `BreakNext` (`for x in [10,20,30]`) |
| 2 | **Les types génériques n'obtiennent aucun `finalize`.** `SynthesizeFinalizeStubs` et `CollectCascadeFields` sont gardés par `Generics.IsEmpty()`. `Hash<K,V>` n'a pas de `finalize`, donc son champ `@entries : Array<HashEntry<K,V>>` n'est jamais libéré. | `ForLoop.vl` / `for_dictionary` : `Array#initialize<HashEntry<String,Int32>>` fuit |
| 3 | **La cascade d'éléments se déclenche sur n'importe quel Aggregate à ≥1 argument générique.** `GetElementFinalizeCandidateType` lit `SemaType.Args[0]` à l'aveugle : `Proc<String>` reçoit une boucle `.size`/`[]` synthétisée. | **Régression** de `393032b` (ajout de `finalize` à `Proc`) : `Curry.vl` et `PointFree.vl` ne compilent plus — `type Proc has no member 'size'` |
| 4 | **Point aveugle des sorties en position d'expression.** `ContainsUnstructuredExit` fait sauter la **méthode entière** — aucun finalize nulle part — dès qu'un `return`/`break`/`next` se cache dans un `if`/`case`/`begin` en position d'expression. | `core-ast.md` §"la seule forme que cette récursion structurelle ne peut atteindre" |

Objectif : ownership explicite, temporaires à durée de **full-expression**,
**une seule** frontière de cleanup par région, **un seul** mécanisme d'unwind,
et 0 fuite sur 57 samples — sans figer une architecture à réécrire ensuite.
Tout reste 100 % middle-end : le backend ne gagne aucun nœud.

---

## Le modèle (verrouillé)

```
Ownership
    ↓
Owned / Borrowed / Moved
    ↓
CleanupRegion
    ↓
full-expression pour les temporaires · scope pour les locales
    ↓
un seul mécanisme d'unwind
```

Une seule règle, dont tout le reste découle :

> **Toute valeur possédée vit jusqu'à la fin de sa région, sauf si son ownership
> est transféré avant. Tout chemin qui quitte une région traverse sa frontière
> de cleanup.**

### `CleanupRegion` est la primitive — `BeginExpr` n'en est que l'abaissement

`CleanupRegion` **définit** le lifetime. `BeginExpr { Body, EnsureBody }` est la
représentation de la frontière dans le middle-end *actuel*, pas la définition.

Conséquence structurelle, à tenir : **`Frontend::BeginExpr` n'est nommé que dans
`CleanupRegion.cpp`**, derrière `EmitBoundary( CleanupRegion, StmtList ) ->
StmtList`. Ni `Temporaries`, ni `ScopeCleanup`, ni `ExitPaths` ne construisent
un `BeginExpr` : ils déclarent des régions et des valeurs possédées. Changer la
représentation du boundary (nœud dédié, table de cleanup par frame, landing pads)
ne doit toucher que ce fichier. C'est vérifiable mécaniquement — cf. §Vérification.

Deux sortes de régions, **une seule implémentation** :

- **Région de portée** — un `StmtList` (corps de méthode, corps de `if`/`while`/
  clause `when`/`begin`, `TopStmts`). Possède les locales nommées.
- **Région de full-expression** — un statement qui a matérialisé des
  temporaires. Possède ces temporaires.

Chaque région émet **exactement une** frontière, quel que soit le nombre de
valeurs possédées — jamais un `begin/ensure` par opérateur.

```
x = a + b + c

# une région de full-expression, un seul boundary, deux temporaires :
begin
  __t0 = a + b        # Owned
  __t1 = __t0 + c     # Owned
  x    = __t1         # move : __t1 -> Moved, retiré de la cleanup list
ensure
  __t0.finalize()     # seul le non-moved reste
end
```

### L'invariant central

```
Every OwnedValue
    -> moved exactly once
    OR finalized exactly once
    OR explicitly escaped/transferred
```

Vérifiable par une identité comptable, pas par valgrind :

```
RaiiOwnedCreated == RaiiMoves + RaiiFinalizes + RaiiExplicitEscapes
RaiiOwnedWithoutCleanup == 0
```

`OwnershipTable` porte ces compteurs ; `RaiiPass` vérifie l'identité en fin de
sweep et incrémente `RaiiOwnedWithoutCleanup` pour toute `OwnedValue` sortant de
sa région sans move ni finalize. Ces compteurs remontent à
`volt check --metrics` par `Meta::ForEachField` (voir §Instrumentation) —
**valgrind devient une confirmation, plus l'oracle.**

---

## Ownership : `Owned` doit être *prouvé*, jamais présumé

Le piège de la Phase 3 : `Call` + type finalisable **≠** `Owned`.
`String#dup`/`#+`/`#trim` rendent un buffer neuf ; `Array<T>#[]` rend une vue sur
un élément ; `String.from_c_string` rend une `String` non-possédante
(`@owns_buffer = false`). Les traiter pareil, c'est un double-free.

**Classification (`Raii/Ownership`)** :

| Forme | État | Justification |
|---|---|---|
| `Identifier` / `InstanceVar` / `Member` nu | `Borrowed` | lecture d'un emplacement existant — c'est déjà la règle du garde alias-init actuel (`FinalizeLowering.cpp:503`) |
| `Call` avec `CalleeResolution.bConstructs` | `Owned` | une construction produit une valeur neuve. Certain, aucune inférence |
| `Call` vers un callee dont `Member::bReturnsOwned` est vrai | `Owned` | fait **dérivé**, cf. ci-dessous |
| tout le reste | `Borrowed` | **défaut sûr** |

Le défaut est `Borrowed`, pas `Owned` : une fuite est un gap déjà assumé par le
dépôt, une corruption ne l'est pas (`FinalizeLowering.cpp:354-361` pose
explicitement cet arbitrage). Une classification manquée coûte une fuite
mesurable via `RaiiOwnedWithoutCleanup`, jamais un double-free.

**`bReturnsOwned` est dérivé, pas annoté.** C'est un fait que le compilateur
calcule à partir du corps, enregistré sur la résolution — exactement la forme
que `rules/zero-hardcode.md` sanctionne (« take the decision in the resolver and
record it on the resolution », comme `bConstructs`/`bIndirect`), et **surtout pas
une quatrième annotation** (la liste `@[Primitive] @[External] @[Literal]` est
close).

- Nouveau `Raii/OwnershipInference.{hpp,cpp}`, exécuté au **même seam Driver
  sérial** que `SynthesizeFinalizeStubs` (`Driver.cpp:458`, après
  `ResolveStructLayouts`, avant `ResolveUnitSignatures`) — ce seam parcourt déjà
  tous les AST d'unités et mute le `TypeStore`, le précédent existe.
- Point fixe sur le graphe d'appel : une méthode `ReturnsOwned` ssi son
  expression de retour est une construction, ou un appel à une méthode déjà
  `ReturnsOwned`. Initialisation à `false`, itération jusqu'à stabilité —
  monotone, donc terminaison garantie, et un cycle non prouvé reste `false`
  (donc `Borrowed`, donc sûr).
- Le bit atterrit sur `Sema::Member` (cross-unit, gelé, sérialisé) : la Phase 3
  le lit sans avoir besoin de l'`AstContext` d'une autre unité — c'est
  précisément la limitation que `EscapesAsConstructorArgument` documente
  aujourd'hui et qu'on cesse ainsi de payer.
- **À valider en début de Phase 3** : que ce seam voit bien toutes les unités
  avant les runs `TypeChecker` (parallèles). Si non, le point fixe se replie sur
  un ordre topologique des unités au même endroit ; l'analyse elle-même ne change
  pas.

### Moves conditionnels : CFG d'abord, flags runtime en dernier recours

Pour `x = if c then t0 else t1 end`, la résolution **par branche** est
préférée — le CFG donne la réponse :

```
branch A:  move t0 -> x ; finalize t1
branch B:  finalize t0  ; move t1 -> x
```

`Temporaries` résout donc l'ownership **par arête sortante** de la région, pas
globalement. Un `Bool` local synthétisé (`__t0.finalize() if __t0_owned`) reste
le **fallback documenté**, réservé aux cas où le transfert n'est réellement pas
résoluble statiquement (transfert traversant une boucle, ou dépendant d'une
valeur runtime). Il est compté (`RaiiRuntimeOwnershipFlags`) : si le compteur
monte sur du code ordinaire, c'est que l'analyse statique recule.

---

## Architecture cible

`FinalizeLowering.cpp` (1654 l.) et la moitié RAII de `TypeBinder.cpp` (~200 l.)
disparaissent au profit de deux groupes. **Séparation verrouillée : le
type-level ne connaît ni CFG ni lifetime d'expression ; le body-level ne
re-dérive aucun fait de type.**

### `source/Volt/Sema/Private/Raii/` — niveau **type**

| Fichier | Rôle |
|---|---|
| `Ownership.{hpp,cpp}` | `EOwnership { Owned, Borrowed, Moved }`, `OwnedValue { Site, Type, State }`, `OwnershipTable` (+ les compteurs de l'identité comptable). `IsFinalizeCandidateType` (extrait de `FinalizeLowering.cpp:316`). `ClassifyExpr` — la table de classification ci-dessus. `ContainerElementType` : **remplace** `GetElementFinalizeCandidateType`, gate sur la revendication de node-kind (`TypeStore::LookupNodeKind( "ArrayLit" )` + `IsSubclassOf`), plus sur `Args[0]`. Corrige la cause **3**. |
| `OwnershipInference.{hpp,cpp}` | Le point fixe `bReturnsOwned` décrit ci-dessus. |
| `FinalizeSynthesis.{hpp,cpp}` | L'actuel `SynthesizeFinalizeStubs` (`TypeBinder.cpp:1384`, + `IsFinalizeCandidateNominal`, `EnsureFinalizeStub`, `MaxFinalizeDepth`) déplacé ici. Point d'appel inchangé. Le garde `Generics.IsEmpty()` tombe (cause **2**). |
| `FieldCascade.{hpp,cpp}` | `CollectCascadeFields`, `FindOwnFinalizeMethod`, le scan `@field.finalize` déjà écrit à la main (garde anti-double-free), `AppendFieldCascade`, `BuildFieldFinalizeCall`. Étendu aux génériques. |

La constante `"finalize"` dupliquée (`MemberResolver.hpp:21` +
`TypeBinder.cpp:1197`) est résorbée : une seule définition, dans `Ownership.hpp`.

### `source/Volt/Sema/Private/Passes/TypeChecker/Raii/` — niveau **corps**

| Fichier | Rôle |
|---|---|
| `CleanupRegion.{hpp,cpp}` | **La primitive.** `CleanupRegion { Owned[], Parent, Kind }` + `EmitBoundary`. **Seul fichier autorisé à nommer `Frontend::BeginExpr`.** Reprend l'étape 5 de `ProcessBlock` (`:1126-1155`), recopie de `TailType` comprise. |
| `ExitPaths.{hpp,cpp}` | **Le mécanisme unique de sortie.** `RegionsUnwoundBy( Exit, RegionStack ) -> OwnedValue[]` pour n'importe quel `Return`/`Break`/`Next`/`RaiseExpr`, à n'importe quelle profondeur, statement **ou expression**. Un seul splice, appelé pour les quatre. Remplace `ReturnAmbient`/`LoopAmbient` et **supprime `ContainsUnstructuredExit`** — pas de nouveau bail-out. |
| `Temporaries.{hpp,cpp}` | Matérialisation full-expression + propagation des moves par arête. Voir Phase 3. |
| `ScopeCleanup.{hpp,cpp}` | L'ex-`ProcessBlock`, réécrit sur `CleanupRegion` + `ExitPaths` : locales, récursion structurelle, exemption move-out, candidat implicite de `RescueClause`. |
| `FinalizeCallBuilder.{hpp,cpp}` | `BuildFinalizeCallOnReceiver` (`:560`), `BuildFinalizeCall` (`:711`) — synthèse d'arbre d'appel, boucle de cascade d'éléments incluse. |
| `RaiiPass.cpp` | `InsertFinalizeCalls` — orchestrateur (~80 l.) + vérification de l'identité comptable. |

Wiring : ajouter les fichiers à `source/Volt/Sema/meson.build`. Tout est
`Private/`, intra-module ⇒ **aucun `SEMA_EXPORT`** (`rules/shared-lib-exports.md`).

### Instrumentation (`PassStats`)

`Sema/Public/Volt/Sema/Pass.hpp` : `PassStats` est parcouru par
`Meta::ForEachField` ⇒ **un champ = une ligne**, et il remonte à
`volt check --metrics` sans autre édition (`rules/meta-first.md`) :

```
RaiiLocals · RaiiTemporaries · RaiiOwnedCreated · RaiiMoves · RaiiFinalizes
RaiiExplicitEscapes · RaiiOwnedWithoutCleanup · RaiiCleanupPaths
RaiiNestedExpressionExits · RaiiUnsupportedExits · RaiiRuntimeOwnershipFlags
```

---

## Phases

> **Contrainte absolue : jamais deux `ninja`/`meson` en parallèle**
> (`rules/build-performance.md`). Un seul build à la fois, séquentiel — y compris
> si des sous-agents sont employés : **ne déléguer aucune étape qui builde.**

### Phase 0 — Filet de sécurité *(aucun changement de comportement)* (DONE)

Baseline valgrind en JSON (`--json-report`), puis générer les `.lowered.golden`
manquants des 12 samples `samples/Tests/RAII/` (`ninja -C build golden-update`).
La suite `golden-lowered` (`tests/meson.build:68-84`) les ramasse par simple
existence de fichier — **aucune édition meson**. C'est l'angle mort actuel :
les finalize synthétisés/splicés ne sont capturés par *aucun* test (les `.golden`
existants sont des dumps `parse`, en amont du lowering).

Ajouter les 11 `.expected` manquants (`exit=0`, cf. `FieldCascade.vl.expected`)
pour que la suite `samples` exécute les 12 de bout en bout.

### Phase 1 — Régression `Proc` *(cause 3, isolée, ~20 lignes)* (DONE)

`ContainerElementType` gate sur la revendication de node-kind `ArrayLit`
(mécanisme sanctionné par `rules/zero-hardcode.md`, identique à
`NilLiteral`/`PointerType`/`FuncType`/`HashLit` — **aucun nom de type Volt en
C++**). Débloque `Curry.vl` et `PointFree.vl`, qui servent ensuite de fixtures.

### Phase 2 — Extraction modulaire *(aucun changement de comportement)* (DONE)

Découpage selon l'arborescence ci-dessus, à comportement **strictement
identique** : `.lowered.golden` et rapport valgrind bit-à-bit inchangés. Le
monolithe tombe *avant* l'ajout de sémantique, sinon les phases 3–5 ne sont pas
revuables. `EOwnership` est introduit ici comme vocabulaire, en modélisant
l'existant : les gardes actuels (alias-init, escape constructeur, move-out tail)
deviennent des transitions d'état nommées, sans changer un seul résultat.

### Phase 3 — Temporaires à durée de full-expression *(cause 1 — le gros morceau)*

`Temporaries.cpp`, exécuté par `RaiiPass` **avant** `ScopeCleanup` et après
`LowerClosureLits`/`LowerArrayLits` — donc `Proc.new( FuncAddr, env )` et les
littéraux abaissés sont déjà des `Call` ordinaires : la fuite `Proc.env`
(`CASCADE_FINALIZE.md` item 4) tombe pour la même raison, **sans code spécifique**.

Par statement :

1. **Classifier** chaque sous-expression via `Ownership::ClassifyExpr` — jamais
   « `Call` finalisable ⇒ `Owned` ». Seules les `Owned` sont matérialisées.
2. **Matérialiser** en `__tN = <expr>` avant le statement, avec un `Binding` de
   site `BindingSite{ExprId}` (la forme « locale implicite » que le `__env` de
   `ClosureLifting` utilise déjà) et `Values.SetExprType`.
3. **Propager les moves par arête sortante** (binding à un nom, `return`,
   argument de constructeur) : `Owned -> Moved`, sortie de la cleanup list.
   Branches résolues indépendamment (cf. §Moves conditionnels).
4. **Une seule frontière** : `CleanupRegion::EmitBoundary` sur
   `[__t0=…, __t1=…, stmt]`.

Cas à traiter explicitement : la condition d'un `While` est réévaluée à chaque
tour — sa région de full-expression est la condition elle-même, pas le statement
englobant, sinon le temporaire fuit par itération.

Attendu : `FullOOP`, `Mixins`, `Composition`, `BreakNext` propres ; `ForLoop`
réduit à la seule fuite `Hash`.

---

## Phase 3 — TERMINÉE (les 8 régressions sont corrigées)

**Statut : `meson test` = 272 OK / 2 FAIL** — exactement la baseline d'avant
Phase 3 (les 2 restants sont `golden`/`golden-lowered` `Sema/MixinGenerics.vl`,
préexistants et sans rapport). `scripts/valgrind_check.py` : **0 fuite au sens
du script, 0 échec de build** (contre 5 fuites + 2 build failures avant), et
**0 erreur valgrind sur les 57 samples** — plus aucun use-after-free ni double
free. Restent 7 samples avec des octets `definitely lost` (voir « précision
restante » plus bas) : des fuites comptées, jamais une corruption.

### Les cinq causes des régressions, et leur correction

Toutes dans `Lifetime/Temporaries.cpp` sauf la première, qui est un problème
d'ordonnancement dans `FinalizeLowering.cpp`.

1. **`ScopeCleanup` doit tourner AVANT `Temporaries`, pas après.** Une région
   de temporaires est un `StmtList` comme un autre : un `ScopeCleanup` passant
   après collectait les locales déclarées *dans* cette région comme si la
   région était leur portée, et les libérait à la fin de la région au lieu de
   la fin du corps — `tidy = users.map( f )` finalisait `tidy` avant que le
   statement suivant ne le lise. L'ordre inverse donne à chaque locale la
   portée où elle a réellement été écrite ; `Temporaries` ne réécrit ensuite
   que des slots d'expression, jamais un `StmtList`, donc il ne peut pas
   déranger le cleanup déjà posé.
2. **Un `Call::Callee` n'est jamais matérialisé.** Un callee est une
   *désignation*, pas une valeur possédée — et le backend indexe la résolution
   sur l'id du `Callee` (`ExprCallEmitter.cpp`), donc détacher ce slot lui
   retirait sa résolution sous les pieds (« call at expression N carries no
   callee resolution »). La descente continue quand même, pour qu'un receveur
   caché dans le callee (`( a + b ).trim`) reste collecté pour son compte.
3. **`raise e` est un move.** L'exception est confiée au mécanisme d'unwind,
   qui la porte jusqu'au `rescue` qui l'attrape ; c'est cette frame-là qui la
   relâche. La finaliser à la fin de la full-expression libérait l'exception
   en vol sous son propre handler (les 4 samples `Exceptions` + `RaiseUnwind`).
4. **La valeur d'un `Assign` est un move, à n'importe quelle profondeur** — pas
   seulement quand l'`Assign` est la racine du statement. Une affectation
   confie sa valeur à l'emplacement qu'elle écrit : une locale, un champ, ou la
   mémoire derrière un `Deref` (c'est exactement comme ça que l'env d'une
   closure est rempli).
5. **Un temporaire de type *callable* n'est pas matérialisé.** L'env d'une
   closure capturante contient les locales de la frame englobante
   (`ClosureLifting` réécrit chaque usage d'une variable capturée, des deux
   côtés, en un load à travers cet env) : le relâcher à la fin du statement qui
   a construit la closure libère la mémoire que la portée englobante continue
   de lire (`arr.each do |i| total = total + i end`). Identifié par le type qui
   revendique le node-kind `FuncType` (`IsCallableType`), jamais par un nom de
   type Volt.

### Précision restante (à reprendre en Phase 4/5, pas des bugs)

Deux règles délibérément conservatrices, chacune coûtant une fuite comptée et
jamais une corruption :

- **Les arguments d'un `Call` dont la résolution ne porte aucune `Decl`** sont
  traités comme `Moved` : une invocation synthétisée (la forme que prend
  l'argument lié d'une section abaissée, en route vers un env de closure) ne
  peut pas être prouvée emprunteuse. C'est ce qui restait derrière
  `PointFree.vl`.
- **L'env d'une closure n'a pas encore sa vraie durée de vie** (la portée de ce
  qu'elle capture, pas la full-expression). D'où les 8 octets perdus par
  closure capturante dans `Mixins`/`Curry`/`ForLoop`/`BreakNext`.

L'outil `VOLT_RAII_TRACE` a servi à diagnostiquer tout ce qui précède puis a
été **supprimé** — il ne reste aucun bloc de debug dans `Temporaries.cpp`.

### Historique (état à l'arrêt précédent, conservé pour mémoire)

### Ce qui est écrit et compile

| Fichier | État |
|---|---|
| `Sema/Public/Volt/Sema/Layout/TypeStore.hpp` | `Member::bReturnsOwned` ajouté (+ doc). Accesseurs mutables `MutableMembers( NominalId )` / `MutableFreeFunctions()` ajoutés à côté de `MemberByDecl`. |
| `Sema/Public/Volt/Sema/Raii/OwnershipInference.hpp` | Nouveau. Déclare `SEMA_EXPORT void InferReturnOwnership( span<const AstContext* const>, TypeStore& )`. |
| `Sema/Private/Raii/OwnershipInference.cpp` | Nouveau (~430 l.). Point fixe monotone. Voir détail ci-dessous. |
| `Sema/Public/Volt/Sema/Pass.hpp` | Les 11 compteurs `Raii*` ajoutés à `PassStats` (remontent déjà à `check --metrics`, vérifié). |
| `.../TypeChecker/Lifetime/CleanupRegion.{hpp,cpp}` | `EmitBoundaryInto( …, ExprId Slot, … )` ajouté : écrit la frontière *dans* un slot existant. L'invariant « `Frontend::BeginExpr` seulement ici » tient toujours. |
| `.../TypeChecker/Lifetime/Temporaries.{hpp,cpp}` | Nouveau (~470 l.). Le cœur de la phase. |
| `.../TypeChecker/FinalizeLowering.cpp` | Appelle `RunTemporaries` **avant** `RunScopeCleanup`, sur `TopStmts` et sur chaque `Method::Body`. |
| `Driver/Private/Driver.cpp` | `Sema::Raii::InferReturnOwnership( UnitAsts, Types )` appelé **après** `ResolveUnitSignatures`, avant `Diagnostics.Merge` et avant la phase sema parallèle. Magic du cache frontend bumpé `VOLTFE04` → `VOLTFE05` (obligatoire : `Member` est un agrégat réfléchi, un champ de plus décale tout le dump). |

### Décisions de conception prises (et pourquoi)

1. **Le seam validé n'est PAS celui du plan.** Le plan disait « même seam que
   `SynthesizeFinalizeStubs` (`Driver.cpp:458`) ». C'est **faux** : à cet
   endroit `ResolveUnitSignatures` n'a pas encore tourné, donc les membres ne
   sont pas résolvables. `InferReturnOwnership` est donc appelé **juste après
   la boucle `ResolveUnitSignatures`**, toujours sérial, toujours avant
   `ForEachUnitParallel( &Driver::RunSemaOne )`. Vérifié : le seam voit bien
   toutes les unités.
2. **`CalleeResolution` n'existe pas au seam** (elle est construite dans
   `TypeChecker`, en parallèle, par unité). Le point fixe travaille donc
   **syntaxiquement** sur les AST bruts :
   - `Call( Member( Obj, "new" ), … )` ⇒ owned (construction) ;
   - receveur *statique* résoluble (`String.owned( … )`, `self.helper( … )`) ⇒
     `Store.LookupMember( nominal, name )`, précis ;
   - sinon **index par nom, tout-ou-rien** : owned ssi *tous* les membres du
     store portant cette orthographe sont `bReturnsOwned`. C'est **sound**
     (le vrai callee est dans l'ensemble) mais imprécis — un seul `to_string`
     empruntant démote tous les `to_string`. Perte = fuite comptée, jamais
     double free.
   - un nom local lié à une valeur owned propage l'ownership (indispensable :
     `Enumerable#map` finit par `result`, pas par une construction).
3. **La classification au *site d'appel*, elle, est précise** : dans
   `Temporaries`, `ResolutionOf` lit `Context.CalleeResolution` — clé
   `Callee.Value` pour un `Call`, clé propre pour `Binary`/`Unary`.
4. **La région est posée sur l'*expression racine*, pas sur le statement.**
   `EmitBoundaryInto` réécrit le slot en place ⇒ le parent n'est jamais
   reconstruit (`rules/ast-rewrite.md`), et ça marche uniformément pour
   `LocalDecl::Init`, `Return::Value`, `While::Cond`, `If::Cond`, un
   `ExprStmt` nu. C'est ce qui rend le cas `While::Cond` gratuit.
5. **Le scrutinee d'un `CaseExpr` est délibérément exclu** : après
   `CaseLowering`, chaque pattern de clause relit le *même* `Scrutinee` id ;
   l'envelopper rejouerait la région (et son cleanup) une fois par clause.

### Bug déjà trouvé et corrigé (garder la leçon)

> **La dernière expression d'un corps EST la valeur de ce corps.**

Traiter le tail d'un `StmtList` comme `Discarded` faisait matérialiser puis
finaliser la valeur de retour : `String#+` se termine sur
`String.owned( buf, total )`, donc tout appelant d'un opérateur string
recevait un buffer déjà libéré (`s = "aa" + "bb"` ⇒ *double free detected in
tcache*). Corrigé : dans `ProcessStmtList`, `bTail = ( Pos + 1 == Body.Size() )`
⇒ `ERootUse::Moved`. Vaut à tous les niveaux (tail d'une branche `If`/`when`/
`begin` en position d'expression aussi).

### Les 10 échecs restants — diagnostic en cours

```
golden-lowered/Sema/MixinGenerics.vl   ← PRÉ-EXISTANT, sans rapport (segfault parse)
samples/Tests/Exceptions/Exceptions.vl
samples/Tests/Exceptions/UncaughtRaise.vl
samples/Tests/Exceptions/BeginRescue.vl
samples/Tests/Exceptions/ExceptionMessage.vl
samples/Tests/Functional/Composition.vl
samples/Tests/Functional/PointFree.vl
samples/Tests/RAII/RaiseUnwind.vl
samples/Tests/ControlFlow/ForLoop.vl
samples/Tests/ControlFlow/BreakNext.vl
```

Deux familles distinctes :

**(A) Échec de *build* — `Composition`, `PointFree`.**

```
✗ Finalize failed: llvm: call at expression 6 carries no callee resolution
  — TypeChecker records one for every call it accepts (while emitting '_V_init_22')
```

Repro minimal (2 lignes), dans le scratchpad sous `d3.vl` :

```volt
raw_name = "  VoLt "
name = raw_name |> (&.trim)
```

Trace (`VOLT_RAII_TRACE=1 volt build -i d3.vl -o /dev/null`) :

```
[raii] d3.vl:2 root=7 kind=17 ownroot=false
[raii]    site=5 kind=18 callee-res=true
```

Lecture : le `Pipeline` d'origine occupait l'expr **6**, réécrit *en place* en
`Call` par `PipelineLowering`. Le site matérialisé est **5** (la `Section`
`(&.trim)`, devenue `Proc.new( FuncAddr, env )` par `ClosureLifting`) — et ce
site **porte une entrée `CalleeResolution` sur son propre id** (`callee-res=true`),
ce que `DetachSlot` déplace vers le nouveau slot puis **efface à l'ancien id**.
Le backend, lui, résout un `Call` via `Callees->Get( Node.Callee )`
(`ExprCallEmitter.cpp:23`) — donc **clé = l'id du *callee*, pas celui du `Call`**.

⇒ **Hypothèse de travail à vérifier en premier** : le slot matérialisé sert
*aussi* de `Callee` à un `Call` parent (ici le `Call` id 6 issu du pipeline :
son `Callee` est un `Member( <site 5>, "call" )`, ou directement le site).
Quand `DetachSlot` efface `CalleeResolution[5]` et met un `Identifier` en 5,
le parent perd sa résolution. **Un slot en position de callee ne doit jamais
être matérialisé**, ou bien la résolution doit être *dupliquée* et non
*déplacée*. La piste la plus simple et la plus sûre : dans
`CollectOwnedSubExprs`, ne jamais collecter un enfant atteint par le champ
`Call::Callee` (un callee n'est pas une valeur qu'on possède, c'est une
désignation) — à ajouter au même mécanisme que `MovedChildren`.

**(B) Échec à l'*exécution* — les 4 `Exceptions` + `RaiseUnwind` + `ForLoop` +
`BreakNext`.**

```
$ volt build -i samples/Tests/Exceptions/BeginRescue.vl -o br && ./br
free(): invalid pointer     (exit 134)
```

Non diagnostiqué. Pistes, par ordre de probabilité :
1. Un temporaire matérialisé dans un corps `begin/rescue` : `EmitBegin`
   ré-propage l'unwind, et la région imbriquée que `Temporaries` pose *à
   l'intérieur* d'un `BeginExpr` existant peut finaliser deux fois sur le
   chemin `rescue.dispatch` → `begin.ensure`.
2. `RaiseExpr` : `raise "msg"` est désucré en construction d'`Exception` —
   c'est un `Owned` **transféré au mécanisme d'unwind**, donc un move, pas un
   temporaire à finaliser. Rien ne l'exclut aujourd'hui ⇒ très probable cause
   du `free(): invalid pointer`. **À exclure explicitement** (même mécanisme
   que `MovedChildren`).
3. `ForLoop`/`BreakNext` : la région posée sur `While::Cond` interagit avec le
   splice `break`/`next` de `ScopeCleanup` (qui court-circuite `ensure`).

### Outil de debug laissé en place (À RETIRER en Phase 6)

`Temporaries.cpp` contient un bloc traçant chaque région créée, activé par la
variable d'environnement `VOLT_RAII_TRACE` :

```sh
VOLT_RAII_TRACE=1 ./build/source/Volt/Volt/volt build -i FICHIER.vl -o /dev/null
# [raii] <path>:<line> root=<exprid> kind=<variant index> ownroot=<bool>
# [raii]    site=<exprid> kind=<variant index> callee-res=<bool>
```

Il a servi à isoler (A) ; il faut le supprimer avant la fin de l'épopée (avec
les `#include <print>` / `<cstdlib>` / `SourceManager.hpp` correspondants).

### Autres notes utiles pour la reprise

- **`volt parse --lowered` ne montre RIEN de ce travail** : `--lowered`
  n'exécute que les passes `EPassKind::Lowering`, or `InsertFinalizeCalls` est
  un post-walk *dans* `TypeChecker` (`Analysis`). Les `.lowered.golden` des
  samples RAII sont donc inchangés par la Phase 3 — ils ne sont **pas** un
  filet pour cette phase. Le seul vrai oracle reste `volt build --emit ir` +
  l'exécution + valgrind. (À reconsidérer en Phase 6 : le filet promis par la
  Phase 0 ne couvre pas ce qu'on croyait.)
- **Inspecter l'IR** : `volt build -i F.vl --emit ir -o F.ll`, puis chercher
  `@_V6String8finalize` / `begin.ensure` dans la fonction concernée. C'est ce
  qui a confirmé que la forme émise pour `s = "aa".dup` est correcte.
- **Piège de test** : `volt build … && ./bin; echo $?` renvoie le code du
  *build* si le build échoue. Plusieurs faux « exit 134 » ont été lus ainsi
  sur des binaires périmés. Toujours `rm -f *.bin` et tester le build et le
  run séparément.
- `volt build --stdin` existe et évite de créer des fichiers temporaires.
- Métriques déjà exploitables : `volt check --metrics -i F.vl` imprime
  `RaiiTemporaries`, `RaiiOwnedCreated`, `RaiiMoves`, `RaiiFinalizes`,
  `RaiiCleanupPaths`. L'identité comptable de §L'invariant central n'est
  **pas encore** vérifiée par le pass (`RaiiOwnedWithoutCleanup` /
  `RaiiExplicitEscapes` / `RaiiUnsupportedExits` restent à 0 sans être
  calculés).

---

### Phase 4 — `finalize` sur les types génériques *(cause 2)*

Lever `Generics.IsEmpty()` dans `FinalizeSynthesis` et `FieldCascade`. Un champ
dont le type **déclare** `finalize` structurellement est cascadable même dans un
corps générique : `@entries : Array<HashEntry<K,V>>` est typé `Array<T>`, qui
déclare `finalize` quel que soit `T`. `@entries.finalize` est bien typé sous la
convention de typage différé que tout corps générique utilise déjà
(`core-ast.md` §"Generic definition bodies") — **aucun bound requis**, c'est
l'enseignement de `CASCADE_FINALIZE.md` item 2 appliqué un cran plus haut.

Mur restant : un champ dont le type est un **paramètre générique nu**
(`HashEntry<K,V>::key : K`). Cascade par instanciation via `Reinstantiate.cpp`,
où `K`/`V` sont concrets. À isoler, en dernier ; si le coût explose, le
documenter comme seul gap restant plutôt que de forcer un bound
`T : Finalizable` (qui obligerait `Hash<Int32,Int32>` à en déclarer un — refusé
par `rules/zero-hardcode.md`).

Attendu : `ForLoop` propre. **0 fuite sur 57.**

### Phase 5 — Sorties en position d'expression *(cause 4)*

`ExitPaths` parcourt expressions **et** statements en portant la pile de
régions : un `Return` dans `x = if c then return 1 else 2 end` est trouvé, et son
préfixe de cleanup splicé dans le `StmtList` de la branche (`If::Then`/`Else`
*sont* des `StmtList` — l'insertion est la même opération qu'ailleurs, seule la
*découverte* manquait). `ContainsUnstructuredExit` et son bail-out par méthode
disparaissent.

**Aucun traitement spécial par nœud** : `If`/`CaseExpr`/`BeginExpr` en position
d'expression arrivent par le même chemin, `Return`/`Break`/`Next`/`RaiseExpr`
arrivent par le même chemin. `RaiiNestedExpressionExits` compte les cas traités ;
`RaiiUnsupportedExits` doit rester à 0.

### Phase 6 — Régressions, nettoyage, docs

Nouveaux samples, chacun avec `.expected` **et** `.lowered.golden`, couvrant les
sept formes verrouillées :

1. rvalue chaînée (`a + b + c`) ;
2. rvalue passée en argument (dont un `Proc` — non-régression cause 3) ;
3. move simple (`x = a + b`, `return` par nom nu) ;
4. move conditionnel (`x = if c then t0 else t1 end`) ;
5. `return` en position d'expression ;
6. `break`/`next` en position d'expression ;
7. **exception levée pendant la construction de temporaires** (`f( a + b, g() )`
   où `g` raise — les temporaires déjà construits doivent traverser la frontière).

Plus : `Hash<String,String>` générique. Nettoyer `source/Lib/` des
`@field.finalize` manuels devenus redondants. Réécrire
`.agents/CASCADE_FINALIZE.md` autour du modèle d'ownership ; mettre à jour
`.agents/rules/core-ast.md` § `InsertFinalizeCalls`.
`python3 scripts/graphify/update_graphify.py`. Configuration `format` **une fois
en fin de phase**, `tidy` **une fois en fin d'épopée** (`rules/cpp-style.md`).

---

## Vérification

Séquentiellement après chaque phase, jamais en parallèle :

```sh
ninja -C build
ninja -C build golden-update            # phases qui changent l'AST abaissé
ninja -C build tests | grep -iEn "FAIL|ASAN"
python3 scripts/valgrind_check.py | grep -iEn "FAIL|LEAK|MEMORY|BUILD"
```

Critères de sortie de l'épopée :

1. `valgrind_check.py` : **57 PASSED, 0 LEAKED, 0 MEMORY_ERROR, 0 BUILD_FAILED.**
2. `meson test` vert, `-Werror` propre, `golden` + `golden-lowered` verts.
3. `volt check --metrics` sur `source/Lib/**` et `samples/**` :
   `RaiiUnsupportedExits == 0`, `RaiiOwnedWithoutCleanup == 0`, et l'identité
   `RaiiOwnedCreated == RaiiMoves + RaiiFinalizes + RaiiExplicitEscapes`.
4. **`BeginExpr` n'apparaît que dans `CleanupRegion.cpp`** — le boundary reste un
   détail d'abaissement :
   ```sh
   grep -rn "BeginExpr" source/Volt/Sema/Private/Passes/TypeChecker/Raii \
                        source/Volt/Sema/Private/Raii    # → CleanupRegion.cpp seulement
   ```
5. Garde-fous `rules/zero-hardcode.md` inchangés :
   ```sh
   grep -RnE '\b(String|Array|Int32|Int64|UInt8|Float64|Proc|Exception)\b' \
     source/Volt/Frontend source/Volt/Sema --include='*.hpp' --include='*.cpp'
   grep -RhoE '@\[[A-Za-z]+' source/Lib | sort -u   # @[External @[Literal @[Primitive
   ```
6. Aucun fichier `source/Volt/Backend*/` modifié (`git diff --stat`).
7. Build ASAN (`ninja -C build-asan`) propre sur les samples RAII — long, une
   seule fois, après que tout le reste est vert.
