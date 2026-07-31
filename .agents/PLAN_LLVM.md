# Plan d'attaque — Finalisation du LLVM Tier 1

## Context

La suite de tests LLVM (configuration `All CTest`, filtrée sur `^Llvm`, lancée depuis
l'IDE) est aujourd'hui à **43/68** (25 échecs sur 68 tests, soit 13 samples
sur 34 dans `samples/Tests/**`). Chaque sample est enregistré deux fois par
`cmake/VoltTests.cmake:88-110` : `LlvmIr.*` (émission + `verifyModule`) et `LlvmRun.*`
(binaire lié, exécuté, comparé à `.expected`).

Mesuré sur `build/debug-testing-llvm` (binaire à jour, commit `fccf910`) :

```
63% tests passed, 25 tests failed out of 68
```

Le diagnostic complet montre que les échecs ne sont **pas** concentrés dans le backend :
ils se répartissent sur le lexer, le parser, ScopeResolver, TypeChecker, la stdlib et
l'émetteur LLVM. Deux d'entre eux sont des défauts silencieux graves — **un segfault du
compilateur** (`Composition.vl`, exit 139) et **une boucle `until` post-test compilée en
pré-test** (`UntilLoop.vl`, code faux sans aucun diagnostic).

Objectif : la suite de tests LLVM verte (configuration `All CTest`, filtre `^Llvm`), sans
hardcode de type Volt, sans lowering dans le
backend, sans code non modulaire — et avec la dette restante **écrite** plutôt que
contournée.

---

## Cartographie : 13 samples → 9 chantiers

| Sample en échec | Cause(s) | Phase |
|---|---|---|
| `OOP/Inheritance.vl` | `}#` au lieu de `#}` (typo) | 0 |
| `ControlFlow/WhileLoop.vl` | `.length`, `-> Int32` narrowing, `.handled?`, `def` sans `-> T` + ArrayLit | 0, 5 |
| `Functional/Lambda.vl` | `( &.+ 5 )` — inférence bidirectionnelle refusée | 0 (dette) |
| `Functional/PointFree.vl` | idem + `.map( transform )` + `1..5` | 0 (dette), 7 |
| `Functional/Composition.vl` | **SIGSEGV compilateur** + ArrayLit | 1, 5 |
| `Conditional/CaseWhen.vl` | `CaseLowering` ne vide pas `Target` | 2 |
| `Conditional/IfInline.vl` | local déclaré dans une `Branch` scope | 3a |
| `Exceptions/BeginRescue.vl` | variable de fichier invisible dans un `def` | 3b |
| `Conditional/IfElsifElse.vl` | `then` non consommé + `if` non-expression | 4a, 4d |
| `Conditional/UnlessElse.vl` | `unless` bloc absent + `unless` non-expression | 4b, 4d |
| `ControlFlow/UntilLoop.vl` | `begin…end until` compilé en pré-test | 4c |
| `ControlFlow/BreakNext.vl` | ArrayLit + `break` non-local hors d'un bloc | 5, 6 |
| `ControlFlow/ForLoop.vl` | ArrayLit + `Hash#each` + `Range` + destructuring hétérogène | 0 (dette), 5, 7 |

---

## Phase 0 — Corrections de samples + dette écrite

Aucun changement compilateur. Ce sont soit des typos, soit des non-goals **déjà
documentés** dans `rules/core-ast.md`. Décision utilisateur : on annote pour que ça
compile, on **garde la forme originale en commentaire** juste au-dessus, et on inscrit la
dette dans les règles.

- `samples/Tests/OOP/Inheritance.vl:5` — `}#` → `#}`. Le lexer n'accepte que `#{ … #}`
  (`Frontend/Private/Lexer/Lexer.cpp:98-129`), et 8 autres emplacements du corpus
  utilisent `#}`. **Vérifié : ce seul caractère fait passer le sample (`exit=15`).**
- `samples/Tests/ControlFlow/WhileLoop.vl` — `.length` → `.size` (lignes 70, 82, 102 ;
  `length` n'existe nulle part dans `source/Lib/`, `size` est la convention partout) ;
  `strip_leading_spaces -> Int32` → `-> UInt64` (retourner un `UInt64` d'une méthode
  `Int32` est un *narrowing*, refusé par `IsWideningScalar` — comportement délibéré,
  cf. `rules/zero-hardcode.md`) ; `.handled?` → `.handled` ; annoter
  `def email/active?/attempts/connected?/mark_handled` d'un `-> T` (non-goal documenté :
  « A method with no `-> T` has no return type »).
- `samples/Tests/Functional/Lambda.vl` — `add_five = ( &.+ 5 )` → `add_five = ( x : Int32 ) => x + 5`,
  forme point-free conservée en commentaire. **`double( 4 )` fonctionne déjà** : le
  protocole `@[Apply]` est complet (`MemberResolver.cpp:296-320`, `LookupApplyOn`), seul
  le lambda **non annoté** échoue.
- `samples/Tests/Functional/PointFree.vl` — mêmes annotations ; `.map( transform )` →
  `.map( &transform )` (`PromoteCapturedBlock`, `ParseExpr.cpp:574-606`, ne promeut au
  slot `&block` que `Section`/`Composition`/`Lambda` — un `Identifier` nu reste
  positionnel, par choix).
- `samples/Tests/ControlFlow/ForLoop.vl` — `for_array` réécrit en forme homogène
  (un `struct User` + `for user in users`) ; la forme hétérogène
  `[[ "Alice", "a@b.c", 42 ], …]` conservée en commentaire.
- `samples/Tests/ControlFlow/BreakNext.vl` — `test_break_with_value` réécrit en
  accumulateur :
  ```volt
  found = 0
  for x in [ 10, 20, 30 ]
    found = x
    break if x == 20
  end
  found
  ```
  L'assertion actuelle est **arithmétiquement fausse** en sémantique Ruby : `break x`
  fait valoir `20` à l'appel `each`, qui est un `ExprStmt` jeté, et la queue `0` est ce
  que la fonction renvoie. La forme accumulateur teste exactement la sortie non-locale
  (phase 6) sans dépendre de `break <valeur>`.

**Dette à écrire dans `.agents/rules/core-ast.md` § "Known non-goals, refused loudly"** —
mettre à jour l'entrée existante et en ajouter trois :

1. La fixture citée `samples/Functional/FunctionalSpec.vl` **n'existe plus** ; le cas vit
   dans `samples/Tests/Functional/{Lambda,PointFree}.vl`. Corriger le chemin et préciser
   que la forme point-free y est en commentaire, pas supprimée.
2. **Littéraux hétérogènes / tuples** : `[ "Alice", "a@b.c", 42 ]` demande un type somme
   ou un `Tuple<…>` — même mur que `T?`.
3. **`break <valeur>` hors d'un bloc** : demande que le type du résultat de l'appel
   porteur du bloc soit la jointure des valeurs de `break` du corps ; `Frontend::Break`
   est traité en feuille par `DeclStmtWalker.cpp:250` et rien ne joint quoi que ce soit.
   Refusé nommément par le backend (phase 6c).

---

## Phase 1 — SIGSEGV du compilateur (3 lignes, priorité absolue)

`ClosureEmitter.cpp:158-166` construit la frame imbriquée d'un corps de closure et copie
`Unit`/`Owner`/`OwnerArgs` mais **jamais `Values`/`Callees`**, qui restent `nullptr`
(`LlvmState.hpp:115-116`). Le premier `Call` dans le corps fait
`Frame.Callees->Get( … )` (`ExprEmitter.cpp:1234`) sur `this == 0x0`.

Tous les autres sites les posent : `LlvmEmitter.cpp:367-368` (`DefineMember`),
`:589-590` (`EmitUnitInit`), `MonoEmitter.cpp:116-117` (overlay),
`ExprEmitter.cpp:1221-1222`. `ClosureEmitter` est le seul oubli.

- `ClosureEmitter.cpp` ~163 — ajouter `Frame.Values = Enclosing.Values;` et
  `Frame.Callees = Enclosing.Callees;`. Copier depuis **`Enclosing`**, pas depuis
  `Unit->Values` : c'est ce qui fait qu'une closure dans un corps *monomorphisé* lit
  l'overlay, l'invariant que `llvm.md` énonce (« every other emitter function already
  reads … through `Frame.Values`/`Frame.Callees` »).
- `ClosureEmitter.cpp:105` — `const Sema::UnitTypes &Values = *Unit.Values;` doit devenir
  `*Frame.Values`. Sinon les layouts des paramètres et le type de résultat de la closure
  (lignes 110, 140) sont lus dans les `UnitTypes` de l'unité *déclarante* — faux pour
  `each do | item |` à l'intérieur de `Enumerable#map`, c'est-à-dire tout corps
  Enumerable monomorphisé. Ce bug mord dès que la phase 5 fait réellement émettre
  `users.map( … )`.

Sample de régression : `samples/Tests/Functional/ClosureCall.vl` — deux lignes, un lambda
dont le corps est un appel, invoqué via `@[Apply]`.

---

## Phase 2 — `CaseLowering` : le scrutin quitte `Target`

`CaseLowering.cpp:133` réécrit le nœud avec `Case.Target` intact alors que les lignes
66-131 l'ont **déjà plié** dans chaque motif (`pattern === Target`). Le garde-fou backend
`ExprEmitter.cpp:1614` refuse alors, à juste titre. Les quatre méthodes de `CaseWhen.vl`
sont touchées ; `eval_grade` (forme sans cible) survit uniquement parce que `Target` y est
déjà invalide.

Le piège : `Target` est **encore lu après l'ordre 22** par `CaseType`
(`TypeChecker/ExprInferencer.cpp:175-178`) pour l'exhaustivité des enums. Le vider sans
plus désactive silencieusement `samples/Sema/EnumExplicitReceiver.vl` — les autres
fixtures utilisent `case self` et retomberaient sur `Context.SelfValue` sans rien dire.

**Design retenu — un champ explicite plutôt que de l'archéologie de motifs :**

- `Frontend/AST/Expr.hpp` — ajouter `ExprId Scrutinee;` à `CaseExpr`, dans son
  `VOLT_FIELDS(...)`. Meta-first : le printer, `ForEachField` et le walk le suivent
  gratuitement.
- `CaseLowering.cpp:133` — `Case.Scrutinee = TargetId; Case.Target = Frontend::ExprId{};`
  avant l'écriture. Le sous-arbre du scrutin reste atteignable, donc ScopeResolver et
  TypeChecker continuent de le résoudre.
- `ExprInferencer.cpp:175` — `CaseType` lit `Scrutinee` s'il est valide, sinon `Target`,
  sinon `Context.SelfValue`. Aucune reconstruction depuis les motifs pliés.

Le contrat backend (« après lowering, `Target` est invalide ») devient enfin vrai, et le
garde-fou `ExprEmitter.cpp:1614` reste tel quel.

---

## Phase 3 — Sema : portées

### 3a. Un local implicite déclaré dans une branche

`inline_res = "success" if true` produit `If { cond, then: [ ExprStmt( Assign ) ] }`
(`ApplyModifiers`, `ParseStmt.cpp:95-101` — aucun `LocalDecl` n'est créé pour un `x = v`
non annoté). `ScopeResolver.cpp:371-394` déclare le nom dans `Current`, qui est ici la
`EScopeKind::Branch` poussée par le handler `If` (`:188-201`). L'usage suivant ne résout
rien et le backend refuse (`ExprEmitter.cpp:390-398`).

**Le TypeChecker implémente déjà le modèle plat façon Ruby** : `Locals`/`LocalTypes`/
`LocalSites` sont plats par *méthode* et ne sont échangés qu'à `EnterMethod`
(`DeclStmtWalker.cpp:122-194`). C'est pourquoi `inline_res` type correctement en `String`
et que seul codegen se plaint. ScopeResolver est le seul composant en désaccord.

- `ScopeResolver.cpp:371-394`, bras `Assign` — déclarer dans la plus proche portée
  englobante **non-`Branch`** (remonter `Parent` tant que `Kind == Branch`, s'arrêter à
  `Method`/`Block`/`Type`/`Unit`). S'arrêter à `Block` est obligatoire : un corps de
  closure doit garder ses affectations locales (`RecordCapture`, `:237`).
- Même bras — appeler `Context.Scopes.SetScopeOfExpr( Node.Target, DeclScope )`.
  `SlotFor` (`StmtEmitter.cpp:92-132`) décide « binding de portée Unit → global de
  module » via `ScopeOfExpr( Site )`, et un global est créé avec
  `llvm::Constant::getNullValue` (`:120`). Un `String` zéroé vaut `{ null, 0 }`, donc
  `inline_res_false.empty?` est `true` — exactement ce qu'assert le sample. Sans ça, le
  binding tombe sur `MakeTemp` (`:39-51`), une `alloca` **non zéroée**, et le sample lit
  de la mémoire indéterminée.
- `x : T = v if c` (`LocalDecl`, `:227`) garde son comportement actuel — c'est ce qui
  préserve `samples/Sema/BranchLocals.vl` et `ShadowNestedIf.vl`.

### 3b. Variables de fichier visibles dans un `def`

Décision de langage prise : **un fichier est un module, ses locals top-level sont ses
globals**. Le backend est déjà construit pour ça — `ExprEmitter.cpp:405-419` route un
binding de portée `Unit` vers `SlotFor`, et `StmtEmitter.cpp:99-123` émet
`_V_global_<ordinal>_<name>`, linkage interne, null-initialisé. Sema ne produit jamais
cette forme. Trois verrous :

- `ScopeResolver.cpp:45-56` — marche `TopDecls` **puis** `TopStmts`. Tout corps de `def`
  est donc résolu avant que `calculate_ensure_count = 0` n'ait déclaré quoi que ce soit.
  Inverser : `TopStmts` d'abord.
- `TypeChecker.cpp:44-49` — même ordre, même inversion.
- `TypeCheckerContext::FindLocal` (`TypeCheckerContext.cpp:25-50`) — `EnterMethod` vide
  les maps de locals, donc même avec un binding correct la recherche échoue. Ajouter un
  repli sur `Ctx.Values.SiteType( Site )` pour un binding *possédé par une portée Unit* :
  cette map n'est jamais échangée, est déjà écrite à chaque déclaration
  (`WriteLocal`, `:83-99`) et c'est **exactement** ce que le backend lit lui-même
  (`ExprEmitter.cpp:409`).
- Corollaire à traiter dans le même geste — l'asymétrie silencieuse : dans un `def`,
  `uncaught_ensure_executed = true` sur un nom de portée fichier **déclare un nouveau
  local** (`:379`) et masque la globale sans le moindre diagnostic, alors que `+=` échoue
  bruyamment (`:375` saute la branche déclarante pour un `Op` composé). Une fois les
  globales visibles, `=` doit résoudre sur la globale comme `+=`.

---

## Phase 4 — Frontend : parser

Fait structurant : **il n'y a pas de table de parselets préfixes**. `Pratt.inl` ne contient
que `VOLT_INFIX`/`VOLT_ASSIGN`/`VOLT_PREFIX`, et toutes les lignes `VOLT_PREFIX`
construisent un `Unary` (`ParseExpr.cpp:233-242`). Toute forme préfixe structurelle
(`begin`, `case`, `(`, `[`, `{`, JSX) est un `case` du `switch` de `ParsePrimary`
(`ParseExpr.cpp:286-449`). « Ajouter un parselet préfixe » = « ajouter un `case` ».

### 4a. `then` (2 lignes)
`ParseIf` (`ParseStmt.cpp:130-156`) et `ParseElsif` (`:165`) ne consomment jamais `KwThen`
(`TokenKind.inl:94`), qui n'est accepté qu'en un seul endroit :
`Accept( TokenKind::KwThen )`, `ParseExpr.cpp:710` (`ParseCaseExpr`). Copier cet appel
après la condition dans les deux fonctions. Débloque `if true then "oui" else "non" end`.
Aucun conflit avec la forme modificatrice : `InfixBinding( KwIf )` vaut 0, donc la boucle
`ParseExpr` casse sur un `if` traînant (`:75-80`) et `ApplyModifiers` le récupère.

### 4b. `unless` bloc (~25 lignes)
`KwUnless` existe (`TokenKind.inl:116`) et le langage le déclare **déjà** ouvreur de bloc
fermé par `end` (`IsBlockOpener`, `ParseDecl.cpp:23`, pour la capture brute des macros) —
seule la clause de `ParseStatement` manque (`ParseStmt.cpp:42-60`).

Le gabarit exact existe : `ParseUntil` (`:201-218`) est `ParseWhile` + condition niée.
`ParseUnless` est `ParseIf` + la même négation. Un `case TokenKind::KwUnless:` dans
`ParseStatement`, une déclaration à côté de `ParseIf` (`Parser.hpp:199`). `else` après
`unless` sort gratuitement : `ParseStatementBlock` (`:10-12`) termine déjà sur `KwElse`.

**Uniformiser la négation** : `ApplyModifiers` utilise `TokenKind::Bang` (`:107`) alors
que `ParseUntil` utilise `TokenKind::KwNot` (`:208`). Choisir `KwNot` — c'est la
déclaration de `Bool#not` dans `source/Lib/Primitives/Bool.vl`.

### 4c. `begin … end until` post-test + modificateur `while`

`ApplyModifiers` (`ParseStmt.cpp:115-125`) désucre `stmt until cond` en
`While{ Cond: not cond, Body: [stmt] }` — boucle pré-test à zéro tour. Correct pour un
statement simple (`i += 1 until i >= 20` avec `i = 10` finit bien à 20), **faux** pour
`begin … end until cond`, qui doit tourner au moins une fois. Le dump `volt parse` le
confirme, et le sample échoue à l'assertion `iterations == 1` de `test_begin_end_until`
(`UntilLoop.vl:83`) — vérifié au débogueur.

- `Frontend/AST/Stmt.hpp:33-39` — `While` ne porte que `Loc/Cond/Body`. Ajouter
  `bool bPostTest = false;` dans son `VOLT_FIELDS`.
- `ParseStmt.cpp:115-125` — quand le statement interne est un `ExprStmt` enveloppant un
  `BeginExpr`, poser `bPostTest = true`. Détection par
  `std::get_if<ExprStmt>` / `std::get_if<BeginExpr>`, la forme qu'utilisent déjà
  `PromoteCapturedBlock` (`ParseExpr.cpp:574-607`) et `AttachTrailingBlock` (`:609-629`).
- `StmtEmitter.cpp:210-237` — une branche : émettre le bloc `Body` en premier puis
  `br Test`. `LoopFrame{ .Latch = Test, .Merge = Merge }` est inchangé, donc `next` saute
  bien au test et non au corps. Le flag plutôt qu'un désucrage parser précisément pour ça.
- Même fonction : ajouter le **modificateur `while`**, absent aujourd'hui
  (`ApplyModifiers` n'accepte que `if`/`unless`/`until`), donc `x += 1 while cond` ne
  parse pas.

### 4d. `if` / `unless` en position d'expression

Nécessaire pour `val = if … end` (`IfElsifElse.vl:20`) et `status = unless … end`
(`UnlessElse.vl:15`). **Option retenue : déplacer `If` de `VOLT_STMT` vers `VOLT_EXPR`**,
exactement comme `CaseExpr`/`BeginExpr`, qui ne sont *que* des expressions et atteignent
la position statement par le bras `default:` de `ParseStatement` → `ExprStmt`.

Ce qui rend l'option viable : `Frontend::If` n'a que **deux consommateurs** dans tout le
dépôt — `ScopeResolver.cpp:188-201` et `StmtEmitter.cpp:174-208`, deux lambdas
autonomes qui migrent vers les visiteurs d'expression.

- `Nodes.inl:74` — une ligne : `VOLT_STMT( If )` → `VOLT_EXPR( If )`. `Then`/`Else`
  restent des `StmtList` ; `elsif` reste un `If` imbriqué dans `Else`.
- `ParseStmt.cpp:44-45` — supprimer l'early-out `case KwIf: return ParseIf();` ;
  `ParseIf`/`ParseUnless` renvoient un `ExprId` et deviennent deux `case` de
  `ParsePrimary`.
- `ScopeResolver.cpp:338-353` (bras `BeginExpr`) est le gabarit du scoping :
  `PushScope( Current, EScopeKind::Branch )`.
- `ExprInferencer.cpp` — un bras `IfType`, copie de la queue de `CaseType`
  (`:169-215`) : `TrailingType` + `Context.UnifyBranchTypes`.
- `StmtEmitter.cpp:174-208` → `ExprEmitter.cpp` : `EmitIf` converge par **slot**
  (`MakeTemp` + `StoreTailValue`), **jamais par `CreatePHI`** — même raison que
  `EmitCase` (`ExprEmitter.cpp:1607-1700`) : une branche est une liste de statements, donc
  le nombre de blocs entrants n'est connu qu'une fois l'échelle construite. `StoreTailValue`
  saute déjà un opérande `void`, ce qui couvre le cas nouveau « la branche se termine par
  un `break`/`raise` et n'a pas de valeur ».
- La **règle de queue** (`EmitStmts( List, bTail )`) a exactement deux lecteurs et `If` en
  est un. Après le déplacement, un `ExprStmt` traînant émet le `ret` de la valeur du slot,
  ce qui *simplifie* la propagation au lieu de la dupliquer.

**Conséquence attendue** : tous les fichiers `tests/golden/**` changent (le dump reflète
la catégorie du nœud). Régénération par le target `golden-update` — c'est mécanique et
attendu, pas un signal.

---

## Phase 5 — Protocole de construction des littéraux agrégés

> **Ce chantier a été entièrement repensé — voir `.agents/PLAN_LITERAL_LOWERING.md`
> et `.agents/rules/backend-machine-only.md`.** §5c (`@[LiteralAppend]`) est
> refusé : la liste fermée d'annotations est `@[Primitive]`/`@[External]`/
> `@[Literal]`, point final (voir la section "refused example, kept on
> purpose" de `rules/zero-hardcode.md`). Plus fondamentalement, §5a-§5f
> gardaient `ArrayLit`/`HashLit` comme des nœuds que **le backend** dispatche
> et construit — même « en lecture seule » via `EmitResolvedCall`, c'est
> encore le backend qui connaît l'existence de ces nœuds, ce qui est refusé
> sans exception (le backend ne connaît que `i8..i64`/`u8..u64`/`f32`/`f64`/
> pointeurs/(dé)référencement — rien d'autre, jamais). Le design correct
> lowered `ArrayLit`/`HashLit` en AST core ordinaire (`Begin`/`Assign`/
> `Binary`/`Call`) **à l'intérieur de `TypeChecker`**, une fois le type du
> littéral stabilisé — le reste de cette section (§5a-§5f) est conservé
> ci-dessous pour mémoire historique mais ne doit plus être implémenté tel
> quel.
>
> **`ArrayLit` : fait.** `HashLit` : pas commencé. Voir le statut détaillé en
> tête de `.agents/PLAN_LITERAL_LOWERING.md`.

`ExprEmitter.cpp:643-644` refuse **inconditionnellement** tout `ArrayLit`/`HashLit`
(`FailAggregateLiteral`, `:776-788`). Vérifié : un simple `xs = [ 1, 2, 3 ]` suffit.
Bloque `Composition.vl`, `WhileLoop.vl`, `ForLoop.vl`, `BreakNext.vl`.

**Un pass `LiteralLowering` est exclu**, et c'est `rules/core-ast.md` qui l'exclut :
« Lowering them to `Array.new` + `push` needs the generic argument, which is only known
*after* `TypeChecker`, so it needs a pass that hand-annotates the types of what it
creates — the one thing the structural invariant forbids. »

**Design retenu : le précédent `Binary`/`Unary` → `CalleeResolution`** — « realised with
no pass and no node ». Sema résout, le backend émet.

### 5a. `EmitResolvedCall` accepte une valeur de receveur (refactor pur)
`EmitResolvedCall` prend **déjà** `std::span<const Frontend::ExprId> Args` — c'est
`EmitCall` qui les détache du nœud `Call` (`ExprEmitter.cpp:1256`), et `EmitBinary` lui
passe déjà un span synthétique d'un élément. Côté arguments, **rien à faire** : les
éléments d'un `ArrayLit` sont de vrais `ExprId` déjà typés (`AstInvariant` le garantit).

Le receveur, lui, est un `ExprId` (`Self = EmitExpr( Receiver )`), alors que le receveur
d'un `push` est la `llvm::Value*` que l'appel d'init vient de renvoyer. Ajouter un
paramètre final défaillant `llvm::Value *ReceiverValue = nullptr`
(`LlvmState.hpp:485`) et une branche en tête de l'échelle du receveur. Les quatre sites
d'appel existants compilent sans modification.

### 5b. Élargissement de la clé de `UnitCallees` (plomberie pure)
Il faut **deux** entrées par littéral. Une map parallèle dupliquerait
`SerializeCache`/`DeserializeCache`/`FixupDecls` — ce que `rules/meta-first.md` proscrit.

Dans `Sema/Public/Volt/Sema/Layout/CalleeMap.hpp` :
```cpp
enum class ECalleeSlot : std::uint8_t { Callee = 0, LiteralInit = 1, LiteralAppend = 2 };
struct CalleeKey { Frontend::ExprId Expr; ECalleeSlot Slot = ECalleeSlot::Callee; };
void Set ( Frontend::ExprId, CalleeEntry, ECalleeSlot = ECalleeSlot::Callee );
const CalleeEntry *Get ( Frontend::ExprId, ECalleeSlot = ECalleeSlot::Callee ) const;
```
Tous les appelants existants (`Frame.Callees->Get( Id )`, `ExprEmitter.cpp:657/671/964/1021`)
sont inchangés grâce au défaut. `Sema/Private/Layout/CalleeMapSerialize.cpp` gagne un
`Meta::Serialize( W, Key.Slot )` et le type de clé de `PendingDecls` s'élargit — ~15 lignes.

### 5c. La stdlib nomme les membres (zéro hardcode)
`Array` porte déjà `@[Literal( ArrayLit )]` et `Hash` `@[Literal( HashLit )]`.

- `source/Lib/Primitives/Array.vl` — `@[LiteralAppend]` sur `def push`.
- `source/Lib/Primitives/Hash.vl` — `@[LiteralAppend]` sur `def []=` ; **et** passer le
  claim à `@[Literal( HashLit, Keys, Values )]`.
- `TypeStore.hpp` ~114 — `bool bLiteralAppend` sur `Member`, à côté de `bApply`/`bUnhandled`.
- `Sema/Private/Layout/TypeBinder.cpp:449-450 / 474-475 / 494-495` — la boucle
  `PendingAnnotation` qui pose déjà `bApply` et `bUnhandled`. Aucun mécanisme nouveau.

**L'ordre des arguments d'`append` vient de `LiteralSlots`, pas du C++.** Décider en dur
que `Keys` alimente le paramètre 0 et `Values` le paramètre 1 serait *inventer une
convention d'ordre de champs* — précisément ce que le commentaire de
`FailAggregateLiteral` refuse. `@[Literal( Kind, Champ, … )]` sait déjà nommer quel champ
AST alimente quel slot (`TypeBinder::ReadLiteralSlots` → `NominalType::LiteralSlots`,
lu par `SlotTypes` avec repli sur les champs réfléchis). On réutilise le même ordre : le
helper de l'émetteur est un `Meta::ForEachField` qui collecte les `ExprList` nommées dans
l'ordre des slots, et un futur littéral à trois listes coûte zéro ligne de C++.

**`@[LiteralInit]` n'est pas nécessaire** : `LookupOn( Context, LiteralType, ConstructorCall )`
donne l'entrée avec `bConstructs` déjà vrai, sans annotation (`MemberResolver.hpp:12-16`
autorise explicitement les *syntaxes* `new`/`initialize` en C++ — ce ne sont pas des noms
de type). Ne l'ajouter que si l'on veut un jour qu'un littéral nomme un initialiseur
différent de `new`.

### 5d. Sema résout — **au moment du snapshot, pas de l'inférence**
Le `ArrayLit` prend son type par le bras attrape-tout `ExprInferencer.cpp:794` →
`LiteralType` (`LiteralInferencer.hpp:107`) : `LookupNodeKind( "ArrayLit" )` → `Array`,
puis `SlotTypes`, puis `MakeType( Base, Args )`.

Mais `ConstrainNode( ArrayLit )` (`TypeCheckerConstraint.cpp:110`) **re-type le littéral
après coup** : `xs : Array<Int64> = [ 1, 2, 3 ]` infère d'abord `Array<Int32>` puis se
déplace. Résoudre à l'inférence figerait `Receiver`/`Bindings` sur `Array<Int32>`, le
backend aplatirait ces `Bindings` en `FlatArgs`, manglerait `Array<Int32>#push`, et
stockerait un `i32` dans un slot `Array<Int64>`. C'est la classe de bug que `llvm.md`
enregistre sous « a use's `ExprType` can lag behind its binding's `SiteType` », en version
non bénigne.

- Pendant l'inférence : enregistrer seulement l'`ExprId` —
  `Context.LiteralSites.insert( Id.Value )`, un `unordered_set` à côté de
  `UnconstrainedLiterals` dans `TypeCheckerContext`.
- **`TypeChecker.cpp`, juste avant la boucle de snapshot (`:57`)** : parcourir
  `LiteralSites`, lire l'`ExprType` **stabilisé**, appeler les lookups, `Set` les deux
  entrées.
- **`TypeChecker/Reinstantiate.cpp:191`**, même insertion avant sa propre boucle de
  snapshot — sans quoi un `ArrayLit` dans un corps générique n'a aucun protocole sous
  monomorphisation.

Les lookups : **`LookupOn`, pas `MemberType`**. `MemberType` (`MemberResolver.hpp:78`)
écrit inconditionnellement `Context.CalleeResolution[Id.Value]` (`:369`) — il écraserait
le slot du littéral — et diagnostique « type X has no member 'Y' » en nommant un membre
stdlib que l'utilisateur n'a jamais écrit. Le gabarit est `LookupApplyOn`
(`MemberResolver.cpp:295-318`) : balayer `Store.Type( Base ).Members` pour le drapeau,
puis re-résoudre **par le nom interné du membre lui-même**, de sorte que le chemin
d'instanciation soit l'ordinaire. Aucun nom Volt n'entre en C++ — le nom sort du store,
précédent posé par `@[Apply]`.

Sur l'entrée d'init obtenue par `ConstructorCall`, `Result` est le type du littéral et
`bConstructs` est vrai — c'est ce qui déclenche la branche `Constructed` de
`EmitResolvedCall` et rend le stockage.

### 5e. Le backend émet
Remplacer `FailAggregateLiteral` par `EmitArrayLit` / `EmitHashLit` :
1. `MakeTemp` un slot du layout du littéral (`Values.Get( Id )`).
2. `EmitResolvedCall` de l'entrée `LiteralInit`, **sans argument explicite** — la
   machinerie d'arguments par défaut de Sema fournit `initial_capacity = 8`, et `push`
   fait croître. Uniforme, aucun reniflage d'arité.
3. Pour chaque élément, `EmitResolvedCall` de l'entrée `LiteralAppend` avec
   `ReceiverValue` = le slot et un span synthétique construit **dans l'ordre des slots
   déclarés**.
4. Le résultat est le `ptr` du slot (convention « un agrégat est une adresse »).

`MonoRequest`, `FunctionFor`, `CoerceWidth`, le spill d'agrégat et `EmitUnwindCheck`
viennent gratuitement puisque tout passe par `EmitResolvedCall`.

**À savoir** — `Hash#[]=` calcule `key.hash % @entries.capacity` mais **n'utilise jamais
`idx`** : il `push` inconditionnellement, et `Hash#[]` fait un balayage linéaire. Correct,
mais O(n). Et il exige `K#hash` : rien ne le vérifie (les bornes `T : Hashable` sont un
non-goal), l'erreur remonte à la *monomorphisation* en pointant dans `Hash.vl`. Pour les
samples visés (`K = String`) c'est satisfait. Enfin `HashLit` déclenche une instanciation
générique **à deux niveaux** (`Array<HashEntry<K,V>>` puis `HashEntry<K,V>.new`) : si le
`Monomorphizer` ne draine pas ça d'un coup, livrer `ArrayLit` d'abord et `HashLit` ensuite.

### 5f. Contingence — `@[Allocator]`
`ClosureEmitter.cpp:250-263` refuse un environnement de closure *échappante avec captures*
faute de point d'entrée d'allocation marqué. La phase 5 n'en dépend pas
(`Array#initialize` atteint `malloc` par du Volt ordinaire via `Pointer<T>.malloc`). Si un
sample déclenche le refus : un claim **global** calqué sur `@[ExceptionRoot]` —
`TypeStore::SetAllocator/GetAllocator` en miroir de `SetExceptionRoot/GetExceptionRoot`
(`TypeStore.hpp:511-527`), lu dans la même boucle `PendingAnnotation`, posé sur
`libc_malloc` (`Pointer.vl:5-6`, déjà `@[External( "libc", "malloc" )]`,
`( size : UInt64 ) -> Pointer<Void>`).

---

## Phase 6 — `break` non-local hors d'un bloc, sur le transport Tier 1

`for x in c … break … end` désucre (parser) en `c.each do |x| … end`. `break` dans une
closure est refusé aujourd'hui (`StmtEmitter.cpp:264-286`) ; `next` fonctionne déjà
(émet `ret`).

Le transport Tier 1 existe : globals thread-local `volt.exc.tag`/`.value`/`.storage`, pile
`FunctionFrame::Rescues`, `EmitExceptionCheck` après chaque appel ordinaire, et le
**chemin empoisonné** (`EmitPoisonedPath`, `ExceptionEmitter.cpp:~228`).

### 6a. Une paire de globals distincte, et un seul check
`volt.brk.flag` (`i1`), **pas** un sentinelle réservée dans `volt.exc.tag` :
- `EmitEntryPoint` (`LlvmEmitter.cpp:542-564`) teste `Tag != InvalidValue` et **appelle le
  hook `@[Unhandled]`** avec `volt.exc.storage` comme receveur : un `break` échappé
  imprimerait un rapport d'exception sur un buffer non initialisé et changerait le code de
  sortie.
- `EmitAncestorTest` (`ExceptionEmitter.cpp:~215`) indexe `volt.exc.ancestry` par la
  valeur du tag ; un second sentinelle sortirait de la table.
- Avec des globals séparés, les deux sémantiques voulues sortent **par construction** :
  non rattrapable par un `rescue` (l'échelle de clauses ne teste que le tag d'exception),
  et `ensure` s'exécute quand même (le chemin empoisonné est partagé).

**Fusionner les vérifications** : `EmitExceptionCheck` devient `EmitUnwindCheck()`, qui
charge les deux, `or` les prédicats et fait **un seul** `CondBr` vers `EmitPoisonedPath`.
Deux post-checks séparés doubleraient le branchement sur le chemin chaud pour rien.

### 6b. Deux corrections dans le chemin de dépliage existant
- **`ExceptionEmitter.cpp:~488-500`** — la fin du bloc `Ensure` re-teste **uniquement** le
  tag d'exception (`StillTag != InvalidValue`). Avec un `break` en vol c'est faux, donc on
  branche sur `Merge` et l'exécution **reprend normalement après le `begin`** : le `break`
  est silencieusement avalé par n'importe quel `begin`/`ensure` englobant. Ce re-test doit
  devenir le même prédicat `exc || brk`. Un `or`, mais porteur : sans lui,
  `Enumerable#any?` dans un `begin/ensure` boucle ou répond faux sans diagnostic.
- **Le `break` lui-même ne doit pas émettre un `ret` brut.** `set flag` puis
  `EmitPoisonedPath()` — sinon un `ensure` *dans le corps de la closure* est sauté :
  ```volt
  arr.each do |x|
    begin
      break
    ensure
      cleanup()
    end
  end
  ```
  `EmitPoisonedPath` signifie déjà « le `begin` le plus interne que cette fonction possède,
  sinon retour anticipé », ce qui est exactement la bonne règle. **`EmitBlockNext`
  (`ClosureEmitter.cpp:308`) a le même défaut latent aujourd'hui** — le router de la même
  manière dans le même changement.

### 6c. Consommation au site d'appel — `EmitResolvedCall`, et là seulement
Trace de `arr.any? do |x| … end` :

| frame | site | comportement |
|---|---|---|
| `do…end` utilisateur | `break` | flag + `EmitPoisonedPath` |
| `Array#each` | `block.call( item )` → `EmitApplyCall` | post-check → chemin empoisonné → `ret` hors du `while` **et** hors de `each` |
| `Enumerable#any?` | `each do \| item \| … end` → `EmitResolvedCall` avec `Block` valide | **consomme** : efface le flag |

`EmitApplyCall` (`ClosureEmitter.cpp:334`) **propage toujours**, ne consomme jamais :
`block.call( item )` ne passe aucun bloc, et c'est la frame qui doit continuer à se
déplier. Son `EmitExceptionCheck` final devient `EmitUnwindCheck` et c'est tout.
`EmitCall` est la mauvaise couche (adaptateur mince que `Binary`/`Unary`/`Member` nu
contournent) ; `EmitResolvedCall` a déjà `Block` en main (`ExprEmitter.cpp:1263`), connaît
`bExternal`, et est le seul endroit où le post-check se déclenche.

`each` revient **normalement** (son chemin empoisonné est un `ret`), donc le contrôle
revient avec le flag posé. Utiliser le motif « convergence par slot » de `EmitBegin`
(`MakeTemp` dans le bloc d'entrée), pas un `phi`. Pour le cas `for`, l'appel est `-> Void`
et tout s'effondre en « effacer le flag ».

### 6d. Ce qui reste refusé, nommément
- **`break <valeur>` hors d'un bloc.** `Frontend::Break{ Loc, Value }` porte bien un
  `ExprId`, mais `DeclStmtWalker.cpp:250` traite `Break` en feuille et rien ne joint quoi
  que ce soit : il n'existe aucun `SemaTypeId` pour la valeur, donc rien pour dimensionner
  un buffer thread-local ni pour typer le slot de résultat du consommateur, et Sema ne
  vérifie pas que deux `break v` d'un même bloc s'accordent. Refus par message nommant le
  trou (style maison), + entrée dans `rules/core-ast.md` (phase 0).
- **`break <valeur>` dans un `while` de la même frame** reste refusé
  (`StmtEmitter.cpp:276`). `While` est un `VOLT_STMT` sans slot de résultat ni type, et la
  règle de queue ne le traverse pas. Garder **deux messages distincts** : le cas bloc est
  un problème de *transport*, le cas `while` un problème de *type*.

### 6e. Le shim d'entrée
`EmitEntryPoint` (`LlvmEmitter.cpp:515-526`) appelle chaque `_V_init_<n>` **sans aucune
vérification entre les appels**, et `EmitUnitInit` pose `bReturnsValue = false`. Un `raise`
dans le top-level de l'unité 0 fait donc `ret void` puis exécute l'init de l'unité 1. La
phase 5 rend les statements top-level *capables de lever* pour la première fois
(`Pointer<T>.malloc` dans `Array#initialize`) : ajouter un `EmitUnwindCheck()` entre les
appels d'init.

---

## Phase 7 — stdlib : `Hash#each` et `Range`

### 7a. `Hash#each`
`for k, v in h` désucre (`ParseStmt.cpp:249-256`) en `h.each do | k, v | … end`. `Hash` ne
déclare pas `each` et n'inclut pas `Enumerable` — et `include Enumerable<V>` **ne suffira
pas** : le contrat d'`Enumerable` est mono-élément.

`source/Lib/Primitives/Hash.vl` — `def each( &block : ( K, V ) -> Void ) -> Void`, boucle
`while` sur `@entries` yieldant `entry.key, entry.value` pour `entry.used`. ~10 lignes de
Volt, aucun C++.

### 7b. `Range<T>` et l'opérateur `..`
`1..3` ne produit **aucun diagnostic** aujourd'hui : `Int32` a un layout `Primitive`, donc
`IsBuiltinPrimitiveOp` (`MemberResolver.cpp:332-357`, via `IsOperatorName` `:20-29`, qui
accepte tout nom ne commençant ni par une lettre ni par `_`) exempte `..` comme opérateur
machine ; `MemberType` renvoie un `Found.Result` invalide et se tait. L'échec surgit deux
lignes plus loin en « Identifier in value position was never given a type ». C'est le vrai
item de travail, pas le type lui-même.

- `MemberResolver.cpp` — exclure `..` et `...` de l'exemption primitive, sans quoi
  `Instructions.inl` se verra demander une ligne qu'il n'a pas.
- `ParseDecl.cpp` `IsOperatorMethodStart` **et** `IsOperatorName` doivent accepter le même
  jeu — ce sont deux moitiés d'un seul contrat (`rules/zero-hardcode.md`, « Declaring the
  operator is not optional »).
- `source/Lib/Mixins/Comparable.vl` — `def ..( other : self ) -> Range<self>` avec corps
  (`Range<self>.new( self, other )`). Comparable et non Arithmetic : un intervalle demande
  un ordre.
- `source/Lib/Primitives/Range.vl` — `struct Range<T>`, `getter first/last`,
  `include Enumerable<T>`, `def each` (boucle `while` yieldant).

---

## Phase 8 — Finition

- Configuration `format` (formatage parallèle et caché par fichier), à lancer une seule
  fois en fin de phase, puis un build et les tests à travers l'IDE (`-Werror`).
- Régénérer les goldens : configuration `golden-update` (target CMake, lancée depuis
  l'IDE). Obligatoire après la phase 4d, et après la phase 2 si `Scrutinee` s'imprime.
- Configuration `tidy` **une seule fois, à la toute fin de l'epic** (toutes les phases
  terminées) — jamais deux en parallèle, jamais en cours de phase : c'est coûteux, et
  parfois peu pertinent en C++26 ; privilégier les diagnostics de l'IDE pendant
  l'itération.
- `graphify update .` (AST-only, sans coût API) : les phases 4d, 5 et 6 ajoutent des
  fonctions top-level et une entrée de manifeste.
- Mettre à jour la doc dans le même geste :
  - `.agents/backend/llvm.md` — retirer les gaps « ArrayLit / HashLit have no recorded
    construction protocol » et « break … refused », documenter `EmitUnwindCheck`, le
    protocole `@[LiteralAppend]` et `ECalleeSlot`.
  - `.agents/rules/core-ast.md` — les trois entrées de dette de la phase 0 ; et si `If`
    passe en `VOLT_EXPR`, **le décompte « 36 `VOLT_EXPR` — 27 core, 9 sugar » change** :
    mettre à jour le tableau des 27 nœuds et la ligne d'en-tête.
  - `.agents/frontend/contracts.md` — dérive constatée : il annonce « ScopeResolver
    (Order 30) et TypeChecker (Order 35) » alors que `PassList.inl` dit 10 / 30 / 35 / 40.

---

## Ordre d'exécution recommandé

L'ordre n'est pas cosmétique : plusieurs phases s'invalident mutuellement.

1. **Phase 1** (segfault) — 3 lignes, débloque toute mesure sur `Composition.vl`.
2. **Phase 0** (samples + dette) — met le corpus au niveau du langage réel.
3. **Phase 2** (`CaseLowering`) — isolé.
4. **Phase 3a puis 3b** (portées) — **avant** la phase 6 : `ClosureEnvField::Site` est un
   `BindingSite` et `SlotFor` clé ses globals sur `UnitGlobalKey{ Ordinal, Site }` sans
   vider `ModuleGlobals` par fonction ; un site qui bouge alors qu'un global périmé existe
   sous l'ancienne clé est la seule façon de désynchroniser les deux.
5. **Phase 4a/4b/4c** (parser, petits) puis **4d** (`If` → `VOLT_EXPR`) **seule** : c'est
   la plus grosse édition de l'émetteur (règle de queue) et elle est orthogonale aux
   phases 5 et 6. Ne jamais la mener en parallèle de l'une d'elles.
6. **Phase 5** dans son ordre interne : 5a (refactor) → 5b (plomberie) → 5c (annotations)
   → 5d (résolution) → 5e (émission). Livrer `ArrayLit` avant `HashLit`.
7. **Phase 6** : 6a/6b (transport partagé) → 6c (consommation) → 6d (refus nommés) → 6e.
8. **Phase 7** (stdlib) — indépendante, peut se faire en parallèle de 5/6.
9. **Phase 8**.

---

## Vérification

À chaque phase, avant de la déclarer close :

Un build et les tests à travers l'IDE (jamais un « run » sur une cible module —
`Core`/`Frontend`/`Sema`/… sont des bibliothèques, pas des exécutables ; lancer une
configuration de test comme `All CTest`, qui construit ses dépendances). La
configuration `format` ne se lance qu'en fin de phase, pas à chaque itération.

Ciblage pendant l'itération (le binaire est `build/bin/volt_d`) :

```sh
# le sample de la phase, IR puis exécution
./build/bin/volt_d build samples/Tests/<Dir>/<Sample>.vl --emit ir -o /tmp/x.ll
./build/bin/volt_d build samples/Tests/<Dir>/<Sample>.vl -o /tmp/x && /tmp/x ; echo "exit=$?"

# la suite LLVM seule
cd build/debug-testing-llvm && ctest -R '^Llvm' --output-on-failure -j8
```

Contrôles spécifiques :

- **Phase 1** — `./build/bin/volt_d build samples/Tests/Functional/Composition.vl -o /tmp/c`
  ne doit plus sortir 139. Le crash actuel est vérifié au débogueur :
  `UnitCallees::Get` (`CalleeMap.hpp:91`) avec `this = 0x0`, appelé depuis
  `EmitCall` → `EmitClosureBody` (`ClosureEmitter.cpp:201`).
- **Phase 2** — census à zéro sur le nœud lowered, sur deux fichiers ne différant que par
  du remplissage (`rules/ast-rewrite.md`) :
  ```sh
  ./build/bin/volt_d parse --lowered --no-color --no-location F | grep -cE '─ CaseExpr\b'
  ```
  et `samples/Sema/EnumExplicitReceiver.vl` doit **toujours** échouer à `check` sur
  l'exhaustivité (c'est la fixture qui garde ce comportement).
- **Phase 3b** — `Check.samples/Sema/*` inchangés ; en particulier aucune régression sur
  `RedeclareSameScope.vl` et `BranchLocals.vl`.
- **Phase 4d** — après `golden-update`, relire le diff des goldens : seul le
  *rattachement* de `If` doit changer, jamais la structure des branches.
- **Phase 5/6** — un build ASan (Debug, `VOLT_ENABLE_ASAN=ON`, via l'IDE) sur
  `Composition.vl`, `BreakNext.vl` et
  `ForLoop.vl` : aucun rapport (`rules/ast-rewrite.md`, checklist).
- **Global** — `AstInvariant`, `ZeroHardcode`, `Corpus.*`, `Golden.*` et les trois
  `*SerializeTest` doivent rester verts ; la phase 5b touche la sérialisation du cache
  (issue #61), donc `SemaSerializeTest` est le garde-fou de cette phase.

**Critère de sortie : 68/68 sur la configuration `All CTest` filtrée `^Llvm`, et un
build + tests verts en entier à travers l'IDE, formatage inclus.**
