# Volt C++23 Compiler — Architecture « béton » 2026

## Context

L'ancien Volt (Crystal, dossier `../Volt2`) est devenu impossible à faire évoluer : inférence
globale de types (lenteur codegen façon Crystal) et surtout **types hardcodés dans le compilateur**.
On repart de zéro sur `BREAKING/Refactor-Volt-C++`. Objectif de cette phase : poser les **fondations
méta-programmées** (Support/Meta + Lexer + AST + Parser + JSX natif) de façon à ce qu'**ajouter une
feature = ~10 lignes**, jamais 500. Le compilateur C++ ne connaît aucun type Volt : il raisonne en
*Memory Layouts*. Les passes sémantiques et le codegen viendront ensuite, mais l'architecture doit
déjà les accueillir sans réécriture.

Contraintes fermes (rappel) :
- **Signatures explicitement typées**, inférence *locale* uniquement → compilation par fichier
  parallélisable.
- **Zéro hardcode de type** : `Int`, `String`, `Array` sont du Volt pur ; le C++ ne voit que
  `Primitive / Pointer / Aggregate` + annotations (`@[Primitive]`, `@[Intrinsic]`, `@[External]`).
- **JSX natif** dans la grammaire, abaissé en appels `Volt::JSX.create_element(...)`.
- Style **Unreal PascalCase**, C++23, `Public/Private`, module CMake `VoltModule`, `-Werror`.
- **Pas de pointeurs intelligents** dans l'AST : stockage valeur (arena + index typés).

## État existant (à réutiliser, ne pas casser)

- `cmake/VoltModule.cmake` : chaque module = `VoltModule(NAME .. TYPE .. SOURCES "Private/*.cpp"
  PUBLIC_INCLUDES "Public/" DEPS ..)`. Layout `volt/Source/<Module>/{Public,Private}`.
- `cmake/VoltBuild.cmake` : `VoltAddModules(<base> Core Volt ...)` — on y ajoutera les nouveaux modules.
- `cmake/VoltCompiler.cmake` : warnings stricts + `-Werror`, sanitizers optionnels. Le code doit passer.
- `cmake/VoltLLVM.cmake` : `VOLT_PKG_LLVM_*` déjà enregistré (pour le codegen AOT plus tard).
- Modules actuels : `Core` (shared lib, quasi vide : `Test.hpp/.cpp`) et `Volt` (exe `main`).
- Surface du langage confirmée par les samples : `samples/**` + `../Volt2/Lib/**`.

## Architecture cible (arborescence)

Nouveaux modules sous `volt/Source/` (ajoutés à `VoltAddModules`) :

```
Core/        Public/Volt/Core/
  Support/       Arena.hpp, Id.hpp (index typés), StringInterner.hpp, SmallVec.hpp, Result.hpp, Span.hpp
  Meta/          Overloaded.hpp, Reflect.hpp (réflexion de champs), Manifest.hpp (X-macro helpers), TypeList.hpp
  Diagnostics/   SourceManager.hpp, SourceLocation.hpp, Diagnostic.hpp, DiagEngine.hpp (thread-safe)
Frontend/    Public/Volt/Frontend/            (nouveau : shared lib, DEPS Core)
  Lexer/         TokenKind.inl (manifeste), Token.hpp, Lexer.hpp
  AST/           Nodes.inl (manifeste unique), Node.hpp, Expr.hpp, Stmt.hpp, Decl.hpp, Type.hpp,
                 Jsx.hpp, AstContext.hpp (arenas), AstPrinter.hpp (auto via Reflect)
  Parser/        Parser.hpp, Pratt.hpp (table binding-power), + Private/Parse{Expr,Stmt,Decl,Type,Jsx}.cpp
Sema/        Public/Volt/Sema/                (nouveau, squelette seulement cette phase)
  Layout/        MemoryLayout.hpp (Primitive|Pointer|Aggregate), TypeStore.hpp
  Pass.hpp (registre de passes), PassList.inl (manifeste ordonné)
Driver/      Public/Volt/Driver/              (nouveau : graphe circuit/module + pool parallèle)
Volt/        Private/Main.cpp                 (existant : devient le front CLI qui appelle Driver)
```

## Les 6 mécanismes méta-programmés (le cœur anti-500-lignes)

### 1. Manifeste unique de nœuds — `AST/Nodes.inl` (X-macro)
Une seule liste pilote **tout** :
```cpp
//        Catégorie   Nom
VOLT_EXPR(IntLiteral) VOLT_EXPR(FloatLiteral) VOLT_EXPR(StringLiteral) VOLT_EXPR(CharLiteral)
VOLT_EXPR(BoolLiteral) VOLT_EXPR(NilLiteral) VOLT_EXPR(SymbolLiteral) VOLT_EXPR(ArrayLit)
VOLT_EXPR(HashLit) VOLT_EXPR(Identifier) VOLT_EXPR(InstanceVar) VOLT_EXPR(SelfExpr)
VOLT_EXPR(Binary) VOLT_EXPR(Unary) VOLT_EXPR(Ternary) VOLT_EXPR(Assign) VOLT_EXPR(Call)
VOLT_EXPR(Index) VOLT_EXPR(Member) VOLT_EXPR(SizeOf) VOLT_EXPR(Deref) VOLT_EXPR(Interp)
VOLT_EXPR(JsxElement) VOLT_EXPR(JsxFragment)
VOLT_STMT(ExprStmt) VOLT_STMT(If) VOLT_STMT(While) VOLT_STMT(Return) VOLT_STMT(LocalDecl)
VOLT_DECL(Module) VOLT_DECL(Class) VOLT_DECL(Struct) VOLT_DECL(Mixin) VOLT_DECL(Method)
VOLT_DECL(Field) VOLT_DECL(Include) VOLT_DECL(Component) VOLT_DECL(Circuit) VOLT_DECL(Annotation)
```
Ré-inclus avec des définitions différentes de `VOLT_EXPR/STMT/DECL`, il génère : l'`enum class *Kind`,
le `std::variant`, les tables de dispatch, le nom-de-nœud pour les diagnostics. **Ajouter un nœud =
1 ligne ici + le struct.**

### 2. Réflexion de champs — `Meta/Reflect.hpp`
Chaque struct-nœud déclare ses champs une fois ; tout parcours générique (printer, clone, egalité,
free-vars, walk) est dérivé — **aucun visiteur par nœud** pour les passes qui ne s'intéressent pas au nœud :
```cpp
struct Binary {
  using Self = Binary;
  SourceRange Loc; TokenKind Op; ExprId Lhs, Rhs;
  VOLT_FIELDS(Op, Lhs, Rhs)            // -> tuple de member-ptrs, itérable à la compilation
};
```
`ForEachField(Node, Fn)` + `AstPrinter` générique tombent « gratuits ».

### 3. AST valeur, arena + index typés — `Support/Arena.hpp`, `Support/Id.hpp`
Pas de `unique_ptr`. `AstContext` détient `Arena<ExprNode>`, `Arena<StmtNode>`, `Arena<DeclNode>`
(chaque `*Node` = le `std::variant` de sa catégorie). Références = `ExprId`/`StmtId`/`DeclId`
(u32 fort). **Un `AstContext` par fichier** → parsing/sema local 100 % parallèle, cache-friendly,
sérialisable pour le hot-reload.

### 4. Dispatch de passes — `Meta/Overloaded.hpp`
```cpp
std::visit(Overloaded{
  [&](Binary& B){ /* règle ciblée */ },
  [&](auto& N){ WalkFields(N); }         // défaut générique via Reflect
}, Ctx.Expr(Id));
```
Une passe (`ScopeResolver`, `TypeChecker`, `JsxLowering`) = une fonction pure + un `Overloaded`.

### 5. Registre de passes / règles — manifeste ordonné `Sema/PassList.inl`
Choix de fiabilité : **manifeste X-macro** plutôt que l'auto-enregistrement `inline constexpr`
(qui souffre du static-init-order et du dead-strip des TUs dans une lib statique). On garde la
propriété « 1 ligne pour ajouter » sans le footgun :
```cpp
VOLT_PASS(ScopeResolver, 10)
VOLT_PASS(JsxLowering,   20)
VOLT_PASS(TypeChecker,   30)
```
Idem pour la table Pratt : un manifeste `Parser/Pratt.inl` (token → binding power, parselet).

### 6. Lexer piloté par manifeste — `Lexer/TokenKind.inl`
Une liste `VOLT_TOKEN(Name, "spelling")` génère l'enum, la table de spelling, le lookup mots-clés.
Gère les points durs Volt : commentaires doc `#{ … #}` vs interpolation `#{expr}` dans les strings,
annotations `@[ … ]`, suffixes entiers typés `16_u64`, char `'C'`, `->`, `<=>`, `::`, `**`.

## Grammaire à couvrir cette phase (issue des samples)

- **Décls** : `module/class/struct/mixin … end`, génériques `Foo[ T ]`, héritage `class A < B`,
  `def name(args) -> Ret … end`, `def self.name`, méthodes `[]`, `[]=`, `<=>`, `?`/`!` suffixes,
  `abstract def`, `include X`, `getter`/`property`/`ivar : Type`, `@[Link(..)]`/`@[External(..)]`.
- **Types** : `Int32`, `UInt8*`, `Pointer[T]`, `Array[T]`, `Void`, `T?` nilable, `Core::AppConfig`
  qualifié, `-> Ret`, tableau fixe `UInt8[ 20 ]`.
- **Exprs** (Pratt) : littéraux (+ interpolation `"#{..}"`, `[..] of T`, `{ k => v }`), binaires/unaires,
  ternaire `?:`, appels avec/sans parens (`raise "x"`, `divisible_by? 2`), index, `.member`, `@ivar`,
  `self`, `sizeof T`, deref `*p` et `*p = v`, `&`, affectations `= += -= ^= <<`.
- **Stmts** : `if/elsif/else/end`, `while … end`, `return`, modifieurs `x if c` / `x unless c`,
  déclaration locale typée `name : Type`.
- **JSX (.vlx)** : `component Name(args) … end`, éléments `<div class="x">`, attrs statiques /
  dynamiques `{expr}` / namespacés `on:click`, texte + interpolation `{count}`, auto-fermants.
  Parsé en nœuds `JsxElement`/`JsxFragment`, **abaissé en appels** par la passe `JsxLowering`
  (garde l'info structurée pour le tooling/hot-reload, finit en `Volt::JSX.create_element`).

## Modèle de types — zéro hardcode (`Sema/Layout`)

`MemoryLayout = Primitive{ kind, bits } | Pointer{ pointee } | Aggregate{ fields[] }`.
Les noms (`Int`, `String`, `Array[T]`) sont résolus vers un layout **via le Volt de la stdlib** +
annotations : `@[Primitive("i32")]`, `@[Intrinsic("llvm.add.i32")]`, `@[External("libc","calloc")]`.
Le C++ ne mentionne jamais « String ». (Squelette de types posé cette phase ; résolution complète
avec le TypeChecker à la phase suivante.)

## Diagnostics thread-safe (`Core/Diagnostics`)

`SourceManager` (id fichier → texte + index de lignes), `Diagnostic{ Severity, SourceRange, Message,
Notes[] }`, `DiagEngine` avec sink protégé par `std::mutex` et buffer par thread fusionné en fin de
passe (faible contention pendant le parse parallèle).

## Ordre de construction (incréments)

1. **Core/Support + Meta + Diagnostics** : `Arena`, `Id`, `StringInterner`, `Result`, `Overloaded`,
   `Reflect`, `Manifest`, `SourceManager`, `DiagEngine`. (Fondation, testable en isolation.)
2. **Lexer** : `TokenKind.inl`, `Token`, `Lexer` — tokenise tous les samples sans erreur.
3. **AST** : `Nodes.inl`, structs `Expr/Stmt/Decl/Type/Jsx`, `AstContext`, `AstPrinter` réflexif.
4. **Parser** : Pratt exprs + descente récursive décls/stmts/types ; round-trip print sur les samples.
5. **ParseJsx** : `.vlx` → nœuds JSX ; passe `JsxLowering` → appels `create_element`.
6. **Driver** : manifeste `circuit` + graphe `@[Link]`, pool `std::jthread`, front CLI dans `Volt/Main.cpp`.
7. **Sema squelette** : `MemoryLayout`, `TypeStore`, registre `PassList.inl` (impl. complète = phase suivante).

Nouveaux modules déclarés dans `cmake/VoltBuild.cmake` :
`VoltAddModules(<base> Core Frontend Sema Driver Volt)` ; chaque `CMakeLists.txt` via `VoltModule`
(`Frontend`/`Sema`/`Driver` = shared libs, `DEPS Core` en cascade ; `Volt` `DEPS Driver`).

## Vérification (end-to-end)

- **Build** : `nix develop` puis configure/ninja (ou `scripts/build.rb`) — doit passer `-Werror`.
- **Golden lexer/AST** : petit exe/onglet de test qui lit chaque fichier de `samples/**` et
  `../Volt2/Lib/**`, imprime tokens puis AST ré-imprimé (`AstPrinter`) ; comparer à des goldens
  archivés → détecte les régressions de grammaire sans écrire un test par nœud.
- **JSX** : sur `samples/JSX/Components/*.vlx`, vérifier que `JsxLowering` produit l'arbre d'appels
  `Volt::JSX.create_element(...)` attendu (comparer au commentaire « IDEA » de `Counter.vlx`).
- **Parallélisme** : compiler `samples/Circuits/DiamandDeps` (multi-fichiers) via le Driver et
  vérifier l'absence de data race sous `VOLT_ENABLE_TSAN=ON`.
- **Zéro-hardcode (garde-fou)** : `grep -R` interdisant les identifiants de types Volt
  (`\bString\b`, `\bArray\b`, `\bInt32\b`) dans `Frontend`/`Sema` (hors commentaires/tests).

## `.agents/` — agents, skills & rules du projet

Nouveau dossier `.agents/` à la racine, pour cadrer toute contribution (humaine ou IA) sur cette
architecture. Chaque fichier **impose deux réflexes transverses** : (a) lancer
`clang-format -i` (config repo `.clang-format` déjà présente : LLVM/Allman, `SpacesInParens`, colonne
170) + respecter `.clang-tidy` avant de terminer ; (b) après un changement d'archi significatif,
lancer **`/graphify volt/Source`** pour rafraîchir le graphe de connaissance du compilateur.

```
.agents/
  README.md                      # comment utiliser agents/skills/rules ; pointe vers .clang-format & /graphify
  rules/
    cpp-style.md                 # Unreal PascalCase, C++23, Public/Private, -Werror ; « clang-format avant commit »
    meta-first.md                # feature = éditer un manifeste (Nodes/TokenKind/PassList.inl), viser ~10 lignes
    zero-hardcode.md             # aucun nom de type Volt dans Frontend/Sema ; Memory Layouts only
    ast-value.md                 # arena + Id typés, jamais de smart pointers dans l'AST
    graphify.md                  # après une évolution d'archi : /graphify volt/Source (garder la carte à jour)
  agents/
    volt-ast-architect.md        # conçoit/étend nœuds AST + manifestes ; applique clang-format & graphify
    volt-parser-engineer.md      # règles Pratt / descente récursive + JSX
    volt-sema-pass.md            # passes via Overloaded + Reflect, enregistrées dans PassList.inl
    volt-build-nix.md            # plomberie VoltModule / CMake / flake Nix
    volt-reviewer.md             # relit : zéro-hardcode, ~10 lignes/feature, clang-format, clang-tidy
  skills/
    add-ast-node/SKILL.md        # recette : 1 ligne Nodes.inl + struct + VOLT_FIELDS → fini
    add-parse-rule/SKILL.md      # ajouter un parselet / une règle de descente
    add-sema-pass/SKILL.md       # nouvelle passe + entrée PassList.inl
    format-and-check/SKILL.md    # clang-format -i + clang-tidy + build ninja + (option) /graphify
```

Note d'intégration : Claude Code lit les subagents dans `.claude/agents/`. On garde la source de
vérité dans `.agents/` (portable, versionnée) et on la câble au harness via un lien/copie
`.claude/agents → .agents/agents` (et skills de même) au moment de l'implémentation.

## Surface CLI cible (`volt <command>`)

Le front `Volt/Main.cpp` devient une **table de commandes** (meta-first : une
commande = une entrée { nom, résumé, options, fonction }) ; le contrat complet
des options par commande est dans `.agents/rules/cli-surface.md`.

```
volt build | check | circuit | format | help | parse | repl | run | version
```

Toutes seront faites ; priorité actuelle : `run`, `repl`, `parse`, `check`,
`version`, `help`, `circuit` (puis `build`, `format`). Correspondances :
`parse` → Driver + AstPrinter (formats json|dot|text = back-ends du printer) ;
`check` → passes Sema (`--type` sélectionne les passes) ; `run`/`repl` → phase
JIT/interpréteur sur la même pipeline Driver ; `version` → `VERSION.md`.

## Observations de revue (2026-07-17) — chantiers d'archi

Revue de modularité : les manifestes tiennent leur promesse (~10 lignes) ; les
sept points ci-dessous sont les endroits où elle casse. État : **tous traités**
(cette session), sauf mention contraire.

1. **Canal de sortie des passes.** `PassContext` n'exposait que `Ast/Types/Diags` ;
   toute passe voulant remonter un résultat au Driver devait toucher 3 modules.
   → `PassStats` (Sema) embarqué dans `CompileUnit`, rempli par les passes via
   `PassContext.Stats`. (Corrige au passage `Result.JsxLowered` toujours à 0.)
2. **Hardcode du Driver.** `"Link"`, `"entrypoint"`, `"modules"`, `".vl"`/`".vlx"`
   éparpillés + cascades de `std::get_if` de 5 niveaux pour lire le manifeste
   circuit. → constantes centralisées (`Driver/WellKnown.hpp`) + helpers de
   requête AST (`Frontend/AST/AstQuery.hpp`) ; ajouter une clé manifeste ≈ 5 lignes.
3. **Asymétrie Pratt.** L'infixe était en table mais préfixes et affectations
   restaient des switch manuscrits dans `ParseExpr.cpp`. → `VOLT_PREFIX` /
   `VOLT_ASSIGN` dans `Pratt.inl` ; nouvel opérateur préfixe = 1 ligne.
4. **`JsxLowering` à cheval Frontend/Sema.** Seule passe non conforme à la
   convention « fonction pure sur PassContext ». → implémentation rapatriée en
   privé côté Sema ; le header public Frontend disparaît.
5. **`AstPrinter` et const.** Le printer est const mais `Driver::PrintUnits`
   gardait un `const_cast` vestigial. → supprimé.
6. **Couture cross-unité (le vrai risque phase suivante).** Tout est par-fichier ;
   le TypeChecker réel devra voir les décls des autres fichiers. → phase sérielle
   « publication d'interfaces » entre parse parallèle et sema parallèle :
   `Sema/Link/InterfaceRegistry` (nom qualifié → unité + DeclId, clés `std::string`
   car interners locaux), exposé en lecture seule aux passes via `PassContext`.
7. **Garde-fous non branchés.** Golden sweep et grep zéro-hardcode promis mais
   inexistants. → `scripts/golden.rb` (sweep `samples/**` via `volt --print`,
   goldens sous `tests/golden/`) + `scripts/check_hardcode.sh`, câblés en ctest.

## Hors périmètre (phases suivantes)

TypeChecker complet, résolution de layouts, JIT interpréteur (dev/hot-reload), codegen LLVM AOT,
runtime `Volt::JSX`/DOM virtuel (écrits en Volt). L'architecture ci-dessus les accueille sans réécriture.
