# PLAN — Bilan du chantier Sema et Feuilles de Route (`Feat/Add-Semantic`)

**État de référence (2026-07-24) :** Build vert, 122/122 tests passés au vert (`volt-build format test`), typage de la stdlib 100 % valide (`volt check source/Lib/` : 0 erreur), graphe `graphify` à jour. Voir `ON_GOING.md` pour le détail des phases 6 à 8 (mixins, génériques de méthode, convention d'appel `&`, diagnostic de bloc non inféré).

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

## V. Strictification & Diagnostic des Membres (Passé au Vert - 2026-07-23)

### Point 7 : Strictification du diagnostic d'accès aux membres inconnus & Support des opérateurs de bits
* **Problème résolu :** Dans `TypeChecker.cpp`, `LookupOn` et `MemberType` faisaient un `return` silencieux sans émettre de rapport sémantique (`Report`) lorsqu'une méthode ou un membre n'existait pas (`Found.Decl == nullptr`). De plus, le parseur (`IsOperatorMethodStart` dans `ParseDecl.cpp`) ne reconnaissait pas les opérateurs de bits `&`, `|`, `^`, `~` après `def`.
* **Implémentation dans TypeChecker.cpp & ParseDecl.cpp :**
  - **Diagnostic strict :** Ajout de la vérification `Ctx.Values.Has(Receiver)` et émission du rapport `Report(Loc, "type " + NameOfValue(Receiver) + " has no member '" + std::string(Name) + "'")` pour tout membre nommé non déclaré sur une instance typée.
  - **Filtre des opérateurs natifs :** `IsBuiltinPrimitiveOp` autorise les opérateurs natifs bas niveau sur les types de layout primitifs/pointeurs sans lever de faux diagnostic.
  - **Support des opérateurs de bits après `def` :** Ajout des jetons `Amp`, `Pipe`, `Caret`, `Tilde` dans `IsOperatorMethodStart` ([ParseDecl.cpp](file:///home/Yutsuna/Projects/VoltLang/Volt/source/Volt/Frontend/Private/Parser/ParseDecl.cpp)).
  - **Sûreté de la stdlib & Mixin Arithmetic :** Ajout de `include Comparable` dans [source/Lib/Mixins/Arithmetic.vl](file:///home/Yutsuna/Projects/VoltLang/Volt/source/Lib/Mixins/Arithmetic.vl), passage au vert de la stdlib et de tous les tests (97/97 tests CTest verts).

---

## VI. Dette Technique Résiduelle du MiddleEnd & Feuille de Route

Le MiddleEnd (`Sema`) fait passer 100 % de la suite de tests (97/97 tests verts) et rejette désormais strictement les accès aux membres inconnus. Les dettes résiduelles du MiddleEnd s'articulent ainsi :

### Dette technique résiduelle (Identifiée & Structurée)

1. **[FAIT - 2026-07-23] Diagnostic d'absence de membre (`LookupOn` & `MemberType` strict) :**
   - **Statut :** RÉALISÉ & VALIDÉ. Les appels à des méthodes inconnues (`numbers.map`, `s.trim`, `w.invalid_method`) sont désormais strictly rejetés avec des diagnostics clairs.

2. **[FAIT - 2026-07-23] Support des Mixins & Méthodes d'ordre supérieur (`Mixin` / `Include` / `Enumerable`) :**
   - **Statut :** RÉALISÉ & VALIDÉ. `include Enumerable<T>` conserve ses arguments génériques (`Super`/`Includes` sont des `SigTypeId`), `LookupMemberOn` compose les substitutions le long de la chaîne avec `self` figé sur le receveur d'origine, et `mixin Enumerable<T>` fournit `.map` / `.filter` / `.count` à `Array<T>`. La conformité des `abstract def` est contrôlée (`CheckAbstractConformance`), avec exemption des opérateurs fournis par le backend sur un layout primitif (`IsBuiltinOpOn`).

3. **Identifiants créés après Order 10 (`MacroExpansion` / `CaseLowering`) :**
   - `ScopeResolver` s'exécute à l'Order 10. `ScopeTable` reste incrémentale (`Declare`/`BindUse` publiques) pour permettre une repasse légère si ces lowerings génèrent de nouveaux identifiants non résolus.

4. **Inférence des littéraux non entiers & collections (`UnconstrainedLiterals`) :**
   - `ConstrainExprType` et `UnconstrainedLiterals` ne gèrent actuellement que les `IntLiteral`. Les littéraux flottants (`FloatLiteral`) et les collections nues (`ArrayLit`, `HashLit`) nécessitent la même propagation descendante de contraintes sémantiques.

5. **[FAIT - 2026-07-23] Inférence automatique des méthodes génériques (`GenericInst`) :**
   - **Statut :** RÉALISÉ & VALIDÉ. `Frontend::Method` porte ses propres `Generics`, l'espace de paramètres est concaténé type ++ méthode, et `UnifySig` / `UnifyArgs` / `UnifyBlock` referment les trous depuis les arguments puis depuis le type réel du bloc : `arr.map do | i | i > 1 end` vaut `Array<Bool>` sans annotation explicite.
   - **Convention d'appel (tranchée) :** un argument `&callable`, en dernière position et sans nom, remplit le slot `&block` — décidé au parse-time par `Parser::PromoteCapturedBlock`, Sema inchangé. `&` accepte `(` en plus d'`identifier`/`Constant`, d'où `numbers.map( &transform )` et `numbers.map( &( ( &.+ 10 ) >> ( &.* 2 ) ) )`. Appeler une valeur (`f( x )`) résout le membre annoté `@[Apply]` via `LookupApplyOn` : `FunctionalSpec.vl` passe de 11 à 6 erreurs.
   - **[FAIT - 2026-07-24] Diagnostic strict sur `&block` non inféré :** `numbers.map( &transform )` avec `transform` non annoté rendait un `Array<?>` **silencieux** — pire que le diagnostic d'arité qu'il remplaçait. `IsBlockResultInferred` (`ExprInferencer.cpp`) rejette maintenant ce cas explicitement (`cannot infer block parameter types for '…'`), sans faux positif sur les corps génériques de la stdlib (`Enumerable<T>` lui-même) ni sur les blocs `do … end` littéraux. Couverture fermée : `samples/Sema/CallableArgs.vl` (promotion `&`, appel direct sur un local via `@[Apply]`) et l'ajout d'un bloc à deux paramètres dans `samples/Sema/BlockParamTypes.vl` (`each_with_index`). Détail dans `ON_GOING.md` §Phase 8.
   - **Reste ouvert :** une lambda à paramètres non annotés et sans type attendu (`add10 = ( &.+ 10 )` puis `add10( 5 )`) n'est toujours pas typée — le diagnostic ci-dessus la rejette proprement dans le cas `&block`, mais ne la résout pas. Il faut une inférence depuis le site d'usage — chantier séparé, solveur bidirectionnel.

6. **[FAIT - 2026-07-24] Contrôle d'exhaustivité du Pattern Matching (`CaseExpr`) :**
   - **Statut :** RÉALISÉ & VALIDÉ. Voir Étape C (§VII.2) pour l'implémentation complète (liaison des `enum` dans `TypeStore`, `EMemberKind::EnumCase`, diagnostic `"non-exhaustive case: ..."`).

7. **Distinction des Closures Évasives vs Non-évasives (*Escaping Closures*) :**
   - `ClosureFrame` dimensionne les environnements de pile pour les captures, mais le `TypeChecker` doit différencier les closures à évasion de celles immédiatement inlinées (`non-escaping`).

8. **Alimentation des `DeclId` (Membres) dans `BindingSite` :**
   - Réservé pour l'unification complète des membres d'instance/classe dans la table de portées (`ScopeTable`).

9. **Égalité des `Enum` (`===` / `Comparable`) non câblée :**
   - Découvert en implémentant l'exhaustivité (§VII.2, Étape C) : un motif `when Enum::Case` (receveur explicite, par opposition au sucre `.Case`) est désugaré par `CaseLowering` en `pattern === target`, mais aucun `enum` n'inclut aujourd'hui `Comparable`/n'implémente `===` — `check` rejette actuellement ce motif avec `"type X has no member '==='"`. Il faut soit doter les `enum` d'un `===` structurel par défaut (comparaison de discriminant), soit documenter que `include Comparable` est requis. La reconnaissance de ce motif dans `EnumCaseNameOf` (exhaustivité) est déjà prête et n'attend que cette câblage.

---

## VII. Feuille de Route Prioritaire (100 % Frontend & Middle-End)

1. **[FAIT - 2026-07-23] Câblage des Mixins & Méthodes d'ordre supérieur (`Enumerable` / `Array.vl`) :**
   - Résolution et injection des mixins génériques, typage descendant des blocs, inférence des génériques de méthode, conformité des `abstract def`. Couvert par `samples/Sema/MixinGenerics.vl`, `BlockParamTypes.vl`, `AbstractConformance.vl`.

2. **[FEUILLE DE ROUTE FRONTEND / MIDDLE-END] :**
   - **[FAIT - 2026-07-24] Étape E : Promotion des arguments de bloc et sigil `&` (`Parser`) :** `PromoteCapturedBlock` (`ParseExpr.cpp`) promeut les arguments positionnels trailing non nommés de nature fonctionnelle (`Section` `Operator`/`InstanceMethod`/`StaticCapture`, `Composition`, `Lambda`) en `BlockArg`. `numbers.map( &.+ 10 )` et `numbers.map( &.to_s )` fonctionnent désormais directement.
   - **[FAIT - 2026-07-24] Étape A : Table des Fonctions Libres (`TypeStore` / `TypeBinder` / `TypeChecker`) :** Un `def` déclaré directement dans un module (jamais dans un `Class`/`Struct`/`Mixin`) est modélisé comme un `Member` (Kind == Method) sans propriétaire : `TypeStore::DeclareFunction` / `FunctionByDecl` / `LookupFunction` (nouveau `std::vector<Member> Functions` + index par nom, `Sema/Layout/TypeStore.hpp`). Déclaré et résolu dans le seam sériel existant (`TypeBinder.cpp` : `ForEachFreeFunction` recense les `Method` top-level, `Binder::BindFunction` / `SignatureResolver::ResolveFunction` les peuplent — mêmes deux phases que `BindType`/`Resolve`, aucune nouvelle passe dans `PassList.inl`). Résolu dans `ExprInferencer.cpp` : l'`Identifier` qui n'est ni un local, ni un type, ni un membre de `self` retente désormais `LookupFreeFunction` (`MemberResolver.cpp`, réutilise `Resolution`/`Reinstantiate`/`CheckCallArgs` sans receveur ni générique de type). `CallType` diagnostique désormais un appel `foo( ... )` dont l'identifiant nu ne résout jamais nulle part (`"unknown function '...'"`), le pendant du diagnostic de membre inconnu pour les appels non qualifiés.
     - **Effet de bord découvert et corrigé :** activer la vérification d'arguments sur les appels de fonctions libres a exposé que `Pointer<Void>` (dans `memcpy`/`memcmp`, `source/Lib/Primitives/String.vl`) ne matchait plus `Pointer<UInt8>` — `Void` n'est déclaré nulle part dans la stdlib, donc son argument générique reste une `SigTypeId` invalide. Correction générique (pas spécifique à `Void`, zero-hardcode) dans `CheckCallArgs`/`ArgTypeMatches` (`MemberResolver.cpp`) : deux types partageant le même nominal et la même arité de générique matchent quand un slot du côté paramètre est un trou non résolu (`ParamVal.Args[i]` invalide) — même logique qu'un générique de méthode non instancié.
     - **Couverture :** `samples/Sema/FreeFunctions.vl` (appel valide, arité + types + retour propagé, ordre de déclaration indifférent), `FreeFunctionArityMismatch.vl` / `FreeFunctionTypeMismatch.vl` / `UnknownFreeFunction.vl` (échecs attendus, `VOLT_CHECK_EXPECT_FAIL` dans `cmake/VoltTests.cmake`). 122/122 tests verts, `volt check source/Lib/` toujours 0 erreur, run TSAN (`volt-build debug tsan`) sur un circuit multi-fichiers sans avertissement.
     - **Nettoyage post-implémentation :** deux signalements `clang-tidy` restants dans `TypeStore.hpp` corrigés à la main (include direct de `Node.hpp` au lieu de `Decl.hpp` transitif pour `DeclId`/`NodeName` ; initialiseur désigné pour `Primitive{ .Spelling, .Bits }`). Le golden `samples/Circuits/DiamandDeps/Project.vl` a aussi été régénéré (`golden-update`) après un reformatage incident de ce fichier échantillon par `volt-build format` (indentation de `modules(...)`), sans rapport avec les fonctions libres elles-mêmes.
   - **[FAIT - 2026-07-24] Étape B : Inférence des littéraux flottants & collections (`UnconstrainedLiterals`) :** `LiteralType` (`LiteralInferencer.hpp`) marque désormais aussi bien `IntLiteral` que `FloatLiteral` comme non contraints (même arène `UnconstrainedLiterals`, même fallback `Float64`). `ConstrainExprType` (`TypeCheckerContext.cpp`) gagne deux nouveaux cas de propagation structurelle, au même niveau que `Ternary`/`Binary` : un `ArrayLit` répercute `Target.Args[0]` sur chaque élément (`fixed : Array<UInt64> = [ 1, 2, 3 ]`, ou en argument d'appel `sum_all( [ 4, 5, 6 ] )` via `CheckCallArgs`), un `HashLit` répercute `Target.Args[0]`/`Args[1]` sur `Keys`/`Values` respectivement. Aucun nouveau nœud AST, aucune nouvelle passe — extension du seam existant. Couverture : `samples/Sema/UnconstrainedLiterals.vl` (littéral flottant nu narrowé par un paramètre `Float32`, tableau littéral nu narrowé par un local typé et par un argument d'appel, hash littéral narrowé par un local typé). 125/125 tests verts, `volt check source/Lib/` toujours 0 erreur.
   - **[FAIT - 2026-07-24] Étape C : Exhaustivité du Pattern Matching (`CaseExpr`) :** Prérequis découvert en cours de route : les `enum` n'étaient liés nulle part dans `TypeStore` — `TypeBinder::ForEachTypeDecl` ne visitait que `Struct`/`Class`/`Mixin`, et `DeclStmtWalker::WalkDecl` n'avait pas de branche `Frontend::Enum` (donc `Context.SelfValue` ne valait jamais rien à l'intérieur d'un corps de méthode d'enum). Les deux sont désormais câblés comme `Struct`/`Class` (même forme de `TypeDecl`, `EnterType` avec `bConcrete = true`). Un cas d'énumération (`EnumCase`) est modélisé comme un nouveau `EMemberKind::EnumCase` — un `Member` dont le `Result` est le type auto-instancié de l'énumération (`TypeBinder::SelfSigOf`) — si bien que `.Todo` / `TaskStatus::InProgress` / `Optional::Some( x )` passent par le même `LookupOn`/`CheckCallArgs` qu'un appel de méthode ordinaire, sans nouveau code d'inférence. `CheckMemberSelf`/`CheckDotCallSelf` (`MemberResolver.cpp`) exemptent ce `Kind` du contrôle statique/instance : un cas d'énumération est légitimement accédé des deux façons (`Enum::Case` en construction, `.Case` en comparaison dans un `case self when`). Le contrôle d'exhaustivité lui-même vit dans une nouvelle branche `CaseExpr` de `ComputeExpr` (`ExprInferencer.cpp`) : le type du scrutin (`Target` ou `self`) donne le `NominalId` ; si celui-ci déclare au moins un `EnumCase` (sinon aucun contrôle — un `case` ordinaire sur une valeur quelconque n'est jamais concerné), chaque motif de chaque `when` est reconnu via `EnumCaseNameOf`, qui reconnaît récursivement les deux formes que produit `CaseLowering` (Order 22) : `Call(Member(...))` pour le sucre `.Name`, et `Binary(TripleEq)` pour un motif à receveur explicite (`Enum::Name`) une fois enveloppé dans la comparaison au `target`. Sans `else`, tout `EnumCase` non couvert déclenche `"non-exhaustive case: missing variant(s) '...' for type ..."` à l'emplacement du `case`. Couverture : `samples/Sema/EnumExhaustiveness.vl` (couverture complète par sucre `.Name`, `else` couvrant un `when` partiel), `samples/Sema/NonExhaustiveCase.vl` (`.Blue` manquant, aucun `else` — échec attendu, `VOLT_CHECK_EXPECT_FAIL`). 131/131 tests verts, `volt check source/Lib/` toujours 0 erreur. Reste hors périmètre : le motif à receveur explicite (`Enum::Name`) n'est fonctionnel dans un `when` qu'une fois l'énumération dotée d'un opérateur `===` (`Comparable`) — absent de la stdlib aujourd'hui ; la reconnaissance `Binary(TripleEq)` d'`EnumCaseNameOf` est prête pour ce jour-là mais n'est pas encore exercée par un échantillon.
   - **Étape D : Closures Évasives vs Non-évasives (*Escaping Closures*) :** Analyse sémantique de l'évasion des closures (`escaping` vs `non-escaping`).
   - **Étape F : Complétion de la stdlib & Résolution des expressions point-free composées (`FunctionalSpec.vl`).**

3. **[ULTÉRIEUR] Génération de code (Backend / Codegen & WASM - `volt run` / `volt build`) :**
   - Mise en œuvre de la codegen LLVM AOT, l'interpréteur/JIT et WASM une fois le Frontend et le Middle-End terminés et validés à 100 %.


