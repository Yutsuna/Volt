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
