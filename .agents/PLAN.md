# PLAN — Bilan du chantier Sema et Feuilles de Route (`Feat/Add-Semantic`)

**État de référence (2026-07-23) :** Build vert, 93/93 tests passés au vert (`volt-build format test`), typage de la stdlib 100 % valide (`volt check source/Lib/` : 13/13 fichiers sans aucune erreur), graphe `graphify` à jour.

Ce document recense le bilan complet des travaux d'analyse sémantique réalisés dans `Sema` (`TypeStore`, `TypeBinder`, `TypeChecker`), la dette technique résiduelle et la stratégie pour la prochaine étape prioritaire.

---

## I. Bilan des Tâches Réalisées & Détails d'Implémentation

### Point 0 : Inférence tardive & paresseuse des littéraux entiers
* **Problème résolu :** `IntLiteral` était typé immédiatement en `Int32`, provoquant un faux positif dans `Array.vl:29-30` (`new_cap = @capacity == 0 ? 8 : @capacity * 2` puis `Pointer<T>.malloc(new_cap)` où `malloc` attend `UInt64`).
* **Implémentation dans TypeChecker.cpp :**
  - **Marquage non contraint :** Un littéral entier nu (`IntLiteral`) est enregistré dans `UnconstrainedLiterals` avec le type `Int32` comme simple fallback.
  - **Suivi des indirections :** Les variables locales initialisées par une expression non contrainte sont enregistrées dans `UnconstrainedVarInitializers["new_cap"] = InitializerExprId`.
  - **Propagation des contraintes (`ConstrainExprType`) :** Lors d'un appel (`CheckCallArgs`), d'un `Return`, d'un `LocalDecl` ou d'un `Assign` typé, la contrainte cible (ex: `UInt64`) est répercutée de façon récursive descendante sur les identifiants locaux (`new_cap`), les expressions conditionnelles (`Ternary`) et les opérations binaires (`Binary`).
  - **Timing binaire :** Dans `@capacity * 2`, la contrainte du premier opérande (`UInt64`) est appliquée sur l'opérande non contraint (`2`) **avant** la recherche de la méthode d'opérateur `*`.
  - **Scoping per-method (`EnterMethod`) :** Sauvegarde et restauration de `UnconstrainedLiterals`, `UnconstrainedVarInitializers`, `Locals` et `CurrentMethodReturnType` à l'entrée et à la sortie de chaque corps de méthode.
  - **Sûreté stdlib :** 0 modification dans la stdlib (`source/Lib`).

### Point 1 : Vérification des arguments d'appel (`CheckCallArgs`)
* **Implémentation :** Implémentation de `CheckCallArgs(Loc, Resolution, Args)` dans `TypeChecker.cpp`.
* Supporte les appels classiques (`Call` via `CalleeResolution`) et les appels qualifiés (`DotCall`).
* Vérifie l'arité (nombre d'arguments requis vs fournis) et la correspondance des types paramètres/arguments avec rapports de diagnostics explicites.

### Point 2 : `DotCall` câblé sur la résolution de membres
* **Implémentation :** Intégration complète dans `TypeChecker.cpp:579-589`.
* Routé via `LookupOn( SelfValue, Ctx.Ast.Text( Expr.Method ) )` + `CheckCallArgs`.

### Point 3 : Validation du contexte `bSelf` (Statique vs Instance)
* **Implémentation :**
  - `NakedTypeExprs` (ensemble de `ExprId`) suit les expressions représentant des références de types nues (`Pointer`, `Pointer<T>`, ou `self` dans un contexte statique).
  - `bStaticContext` conserve le contexte statique vs instance lors du parcours (`EnterMethod`).
  - `CheckMemberSelf` et `CheckDotCallSelf` empêchent l'accès aux membres d'instance depuis un contexte statique et inversement.

### Point 4 : Discrimination des membres par unité (`MemberByDecl`)
* **Implémentation :** `MemberByDecl` dans `TypeStore.hpp` prend désormais `std::uint32_t Unit` en paramètre pour filtrer à la fois sur `Entry.Unit == Unit` et `Entry.Decl == Decl`, évitant les collisions d'AST inter-fichiers.

### Point 5 : Élimination de `Checker::Locals` plate (Portées lexicales)
* **Implémentation :** `Checker::Locals` a été remplacé par `LocalTypes` indexé structurellement par `BindingSite` (`StmtId`/`ParamId`), résolvant les collisions de symboles entre branches frères.

### Point 6 : Ménage, Formatage et Graphe de Connaissances
* **Implémentation :** Validation par `volt-build format test` (93/93 tests verts), vérification globale `volt check source/Lib/`, et régénération du graphe `graphify update .`.

---

## II. Passe `ScopeResolver` (Order 10)

La passe **`ScopeResolver`** (Order 10 dans `PassList.inl`) a été entièrement implémentée et validée.

### Bilan des réalisations (`ScopeResolver`) :
1. **`ScopeTable.hpp` & `ScopeResolver.cpp` :** Implémentation complète de l'arène de portées lexicales (`ScopeTable`), traversal récursif par réflexion `Overloaded` + `Meta::ForEachField` sans aucun `switch` sur les node kinds.
2. **Migration `TypeChecker` :** Intégration de la résolution par site de déclaration.
3. **Diagnostics & Couverture de tests :** Redéclaration dans la même portée lexicale diagnostiquée ; masquage imbriqué (*shadowing*) autorisé et validé (93/93 tests verts).
4. **Fix d'inclusion autonome :** `ScopeTable.hpp` inclut désormais explicitement `StringInterner.hpp` et `using Core::Symbol;`.

---

## III. Captures de Closures (`Feat/Add-scopes`)

La détection et la publication des **captures de closures** ont été entièrement implémentées et validées sur la branche `Feat/Add-scopes` (PR #49).

### Bilan des réalisations (`Feat/Add-scopes`) :
1. **Modèle de données de Capture (`ScopeTable.hpp`) :**
   - Ajout du type `Capture` (`Site`, `Name`, `DeclaringScope`, `ClosureScope`).
   - Enregistrement O(1) et publication via `CapturesOf(ScopeId)`, `CapturesOfExpr(ExprId)` et `SetScopeOfExpr(ExprId, ScopeId)`.
2. **Détection & Propagation (`ScopeResolver.cpp`) :**
   - Implémentation de `CheckAndRecordCaptures` analysant le franchissement de portées de type `EScopeKind::Block`.
   - Propagation automatique pour les closures imbriquées (*nested closures*).
3. **Validation & Couverture (`ClosureCaptures.vl`) :**
   - Couverture des lambdas `( params ) => expr` et des blocs `do |params| ... end`.

---

## IV. Typage des Closures & Frames (`Feat/Closure-Frame` - PR #50)

Le typage sémantique des closures et le calcul de leurs structures de pile (`ClosureFrame`) ont été entièrement implémentés et fusionnés dans `main` (PR #50).

### Bilan des réalisations (`Feat/Closure-Frame`) :
1. **Typage dans `TypeChecker.cpp` (Order 30) :**
   - Inspection des captures via `CapturesOfExpr` / `CapturesOf`.
   - Closures sans captures (`not bHasCaptures`) : Typées en pointeurs de fonctions natifs / `Lambda`.
   - Closures avec captures (`bHasCaptures`) : Typées avec leur structure d'environnement `ClosureFrame` / `Block`.
2. **Calcul de Frame (`ClosureFrame.cpp` & `ClosureFrame.hpp`) :**
   - Construction et dimensionnement des structures d'environnement de capture pour la pile.
   - Validation complète du build `-Werror` et des tests (`93/93 tests passés au vert`).

---

## V. Dette Technique Résiduelle & Prochaines Étapes

### Dette technique résiduelle (Mineure & Documentée)
1. **Identifiants créés après Order 10 (`MacroExpansion` / `CaseLowering`) :**
   - `ScopeResolver` s'exécute à l'Order 10. `ScopeTable` reste incrémentale (`Declare`/`BindUse` publiques) pour permettre une repasse légère si ces lowerings génèrent de nouveaux identifiants non résolus.
2. **Alimentation des `DeclId` (Membres) dans `BindingSite` :**
   - Réservé pour l'unification complète des membres d'instance/classe dans la table de portées.

### Prochaines étapes prioritaires
1. **Génération de code (Backend / Codegen - `volt run` & `volt build`) :**
   - Exploitation des `ClosureFrame` et des tableaux de capture dans l'interpréteur/JIT et le backend LLVM AOT pour émettre la gestion des environnements sur la pile.
2. **Support des Closures dans les Cibles WebAssembly / WASM (`volt build --target wasm`) :**
   - Abaissement des environnements de closure vers les tables et la mémoire WASM.
