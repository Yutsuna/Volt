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
* **Justification de l'état actuel :** C'est un raccourci temporaire documenté (*"until ScopeResolver publishes a table"*). Il est suffisant pour le typage actuel de `source/Lib/` car la stdlib n'utilise pas de shadowing ambigu dans ses méthodes.50: 
51: ---
52: 
53: ## III. Implémentation Complétée : Passe `ScopeResolver` (Order 10)
54: 
55: La passe **`ScopeResolver`** (Order 10 dans [PassList.inl](file:///home/Yutsuna/Volt/source/Volt/Sema/Public/Volt/Sema/PassList.inl)) a été entièrement implémentée et validée (2026-07-22).
56: 
57: ### ✅ Bilan des réalisations (`ScopeResolver`) :
58: 1. **`ScopeTable.hpp` & `ScopeResolver.cpp` :** Implémentation complète de l'arène de portées lexicales (`ScopeTable`), traversal récursif par réflexion `Overloaded` + `Meta::ForEachField` sans aucun `switch` sur les node kinds.
59: 2. **Migration `TypeChecker` (Élimination du Point 5) :** `Checker::Locals` a été remplacé par `LocalTypes` indexé structurellement par `BindingSite` (`StmtId`/`ParamId`), résolvant le bug latent des collisions de symboles entre branches frères.
60: 3. **Diagnostics & Couverture de tests :** Redéclaration dans la même portée lexicale diagnostiquée ; masquage imbriqué (*shadowing*) autorisé et validé (88/88 tests verts).
61: 4. **Fix d'inclusion autonome :** `ScopeTable.hpp` inclut désormais explicitement `StringInterner.hpp` et `using Core::Symbol;`.
62: 
63: ---
64: 
65: ## IV. Dette Technique Résiduelle & Prochaines Étapes
66: 
67: ### 🟡 Dette technique résiduelle (Mineure & Documentée)
68: 1. **Identifiants créés après Order 10 (`MacroExpansion` / `CaseLowering`) :**
69:    - `ScopeResolver` s'exécute à l'Order 10. `ScopeTable` reste incrémentale (`Declare`/`BindUse` publiques) pour permettre une repasse légère si ces lowerings génèrent de nouveaux identifiants non résolus.
70: 2. **Alimentation des `DeclId` (Membres) dans `BindingSite` :**
71:    - Réservé pour l'unification complète des membres d'instance/classe dans la table de portées.
72: 
73: ### 🟢 Prochaines étapes prioritaires
74: 1. **Captures de closures / Lambdas / Blocs (`do |x| ... end`) :**
75:    - Exploitation de la hiérarchie `Scope::Parent` pour analyser les captures lexicales.
76: 2. **Génération de code (Backend / Codegen) :**
77:    - Exploitation de `ScopeTable` pour dériver les durées de vie et allocations sur la pile (*stack frames*).
78: 3. **Commit Git :**
79:    - Validation et commit des modifications actuelles sur la branche `Feat/Add-Semantic`.

