# PLAN — Bilan du chantier Sema et Feuilles de Route (`Feat/Add-Semantic`)

**État de référence (2026-07-22) :** Build vert, 77/77 tests passés au vert (`volt-build format test`), typage de la stdlib 100 % valide (`volt check source/Lib/` : 13/13 fichiers sans aucune erreur), graphe `graphify` à jour.

Ce document recense le bilan complet des travaux d'analyse sémantique réalisés dans `Sema` (`TypeStore`, `TypeBinder`, `TypeChecker`), la dette technique résiduelle et la stratégie pour la prochaine étape prioritaire.

---

## I. Bilan des Tâches Réalisées & Détails d'Implémentation

### ✅ Point 0 : Inférence tardive & paresseuse des littéraux entiers
* **Problème résolu :** `IntLiteral` était typé immédiatement en `Int32`, provoquant un faux positif dans `Array.vl:29-30` (`new_cap = @capacity == 0 ? 8 : @capacity * 2` puis `Pointer<T>.malloc(new_cap)` où `malloc` attend `UInt64`).
* **Implémentation dans [TypeChecker.cpp](file:///home/Yutsuna/Volt/source/Volt/Sema/Private/Passes/TypeChecker.cpp) :**
  - **Marquage non contraint :** Un littéral entier nu (`IntLiteral`) est enregistré dans `UnconstrainedLiterals` avec le type `Int32` comme simple fallback.
  - **Suivi des indirections :** Les variables locales initialisées par une expression non contrainte sont enregistrées dans `UnconstrainedVarInitializers["new_cap"] = InitializerExprId`.
  - **Propagation des contraintes (`ConstrainExprType`) :** Lors d'un appel (`CheckCallArgs`), d'un `Return`, d'un `LocalDecl` ou d'un `Assign` typé, la contrainte cible (ex: `UInt64`) est répercutée de façon récursive descendante sur les identifiants locaux (`new_cap`), les expressions conditionnelles (`Ternary`) et les opérations binaires (`Binary`).
  - **Timing binaire :** Dans `@capacity * 2`, la contrainte du premier opérande (`UInt64`) est appliquée sur l'opérande non contraint (`2`) **avant** la recherche de la méthode d'opérateur `*`.
  - **Scoping per-method (`EnterMethod`) :** Sauvegarde et restauration de `UnconstrainedLiterals`, `UnconstrainedVarInitializers`, `Locals` et `CurrentMethodReturnType` à l'entrée et à la sortie de chaque corps de méthode.
  - **Sûreté stdlib :** 0 modification dans la stdlib (`source/Lib`).

### ✅ Point 1 : Vérification des arguments d'appel (`CheckCallArgs`)
* **Implémentation :** Implémentation de `CheckCallArgs(Loc, Resolution, Args)` dans `TypeChecker.cpp`.
* Supporte les appels classiques (`Call` via `CalleeResolution`) et les appels qualifiés (`DotCall`).
* Vérifie l'arité (nombre d'arguments requis vs fournis) et la correspondance des types paramètres/arguments avec rapports de diagnostics explicites.

### ✅ Point 2 : `DotCall` câblé sur la résolution de membres
* **Implémentation :** Intégration complète dans `TypeChecker.cpp:579-589`.
* Routé via `LookupOn( SelfValue, Ctx.Ast.Text( Expr.Method ) )` + `CheckCallArgs`.

### ✅ Point 3 : Validation du contexte `bSelf` (Statique vs Instance)
* **Implémentation :**
  - `NakedTypeExprs` (ensemble de `ExprId`) suit les expressions représentant des références de types nues (`Pointer`, `Pointer<T>`, ou `self` dans un contexte statique).
  - `bStaticContext` conserve le contexte statique vs instance lors du parcours (`EnterMethod`).
  - `CheckMemberSelf` et `CheckDotCallSelf` empêchent l'accès aux membres d'instance depuis un contexte statique et inversement.

### ✅ Point 4 : Discrimination des membres par unité (`MemberByDecl`)
* **Implémentation :** `MemberByDecl` dans `TypeStore.hpp` prend désormais `std::uint32_t Unit` en paramètre pour filtrer à la fois sur `Entry.Unit == Unit` et `Entry.Decl == Decl`, évitant les collisions d'AST inter-fichiers.

### ✅ Point 6 : Ménage, Formattage et Graphe de Connaissances
* **Implémentation :** Validation par `volt-build format test` (77/77 tests verts), vérification globale `volt check source/Lib/`, et régénération du graphe `graphify update .`.

---

## II. La Dette Technique Actuelle

### Point 5 : `Checker::Locals` — Table plate par corps de méthode
* **Description :** Actuellement, `Checker::Locals` dans `TypeChecker.cpp` est une simple `std::unordered_map<Symbol, SemaTypeId>` à plat par corps de méthode.
* **Impact :** Elle ne gère pas l'empilement de portées lexicales (*scope stack*), le masquage de variables (*shadowing*) dans des blocs imbriqués (`if`, `while`, `block`), ni les captures de closures.
* **Justification de l'état actuel :** C'est un raccourci temporaire documenté (*"until ScopeResolver publishes a table"*). Il est suffisant pour le typage actuel de `source/Lib/` car la stdlib n'utilise pas de shadowing ambigu dans ses méthodes.

---

## III. Prochaine Étape Prioritaire : Option 1 — `ScopeResolver` (Passe Order 10)

La prochaine étape prioritaire absolue de développement est l'implémentation complète de la passe **`ScopeResolver`** (Order 10 dans [PassList.inl](file:///home/Yutsuna/Volt/source/Volt/Sema/Public/Volt/Sema/PassList.inl)).

### Pourquoi faire `ScopeResolver` D'ABORD (avant le Backend / Codegen) ?

1. **La génération de code (Backend) a un besoin vital des portées :**
   Que l'on génère du LLVM IR, du C ou du Bytecode, le backend doit savoir à chaque instruction de quelle variable stack/frame il s'agit (gestion fine des durées de vie, allocations sur la pile, portée des temporaires, réutilisation d'emplacements et masquage). Construire un backend sur une table d'allocations plates incomplète rendrait la génération de code extrêmement fragile dès qu'il y a des boucles ou des blocs `if`/`else` imbriqués.

2. **Clôture définitive du chantier Sema :**
   En implémentant `ScopeResolver` (Passe Order 10), on élimine la dernière dette technique documentée (`Checker::Locals` - Point 5). La phase d'analyse sémantique (`Sema`) devient alors 100 % complète, propre et rigoureuse.

3. **Préparation du support des closures / Lambdas / Blocs :**
   Le style fonctionnel de Volt (closures, lambdas, blocs `do/end` passés via `&block`) repose intégralement sur la capacité du compilateur à analyser la capture de portées lexicales (*lexical closure captures*). C'est précisément la responsabilité architecturale de `ScopeResolver`.

---

## IV. Plan d'Action pour `ScopeResolver`

1. **Conception du Scope Tree dans `PassContext` :**
   - Définir la structure des portées lexicales (`Scope`, `ScopeId`, liens parent-enfant) dans `Sema`.
2. **Implémentation de `ScopeResolver` ([Passes.cpp](file:///home/Yutsuna/Volt/source/Volt/Sema/Private/Passes/Passes.cpp)) :**
   - Parcourir l'AST et enregistrer la déclaration et l'utilisation de chaque identifiant dans son nœud de portée exact.
3. **Migration de `TypeChecker.cpp` :**
   - Remplacer l'usage de la table plate `Checker::Locals` par l'interrogation de la table de portées publiée par `ScopeResolver`.
4. **Validation :**
   - Exécution de `volt-build format test` et `volt check source/Lib/`.
